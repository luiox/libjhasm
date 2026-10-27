#include "jvm_printer.hpp"

#include "jhasm/util/escape_util.hpp"
#include "jhasm/util/type_annotation_codec.hpp"

#include <jbc/jbc.hpp>
#include "libca/core/tag_cast.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace jhasm::printer {
namespace {

namespace C  = jbc::core;
namespace IR = jbc::ir;

// 访问旗语（JVMS 4.1 Table 4.1-B；旧栈 jbytecode::core::Opcodes 的旗语面在此内联）。
constexpr ca::i32 ACC_PUBLIC = 0x0001;
constexpr ca::i32 ACC_PRIVATE = 0x0002;
constexpr ca::i32 ACC_PROTECTED = 0x0004;
constexpr ca::i32 ACC_STATIC = 0x0008;
constexpr ca::i32 ACC_FINAL = 0x0010;
constexpr ca::i32 ACC_SUPER = 0x0020;
constexpr ca::i32 ACC_SYNCHRONIZED = 0x0020;
constexpr ca::i32 ACC_VOLATILE = 0x0040;
constexpr ca::i32 ACC_BRIDGE = 0x0040;
constexpr ca::i32 ACC_TRANSIENT = 0x0080;
constexpr ca::i32 ACC_VARARGS = 0x0080;
constexpr ca::i32 ACC_NATIVE = 0x0100;
constexpr ca::i32 ACC_INTERFACE = 0x0200;
constexpr ca::i32 ACC_ABSTRACT = 0x0400;
constexpr ca::i32 ACC_STRICT = 0x0800;
constexpr ca::i32 ACC_SYNTHETIC = 0x1000;
constexpr ca::i32 ACC_ANNOTATION = 0x2000;
constexpr ca::i32 ACC_ENUM = 0x4000;
constexpr ca::i32 ACC_MODULE = 0x8000;
constexpr ca::i32 ACC_RECORD = 0x10000;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

///@brief JVM modified UTF-8 → UTF-16（JVMS 4.4.7）：0xC0 0x80 解码为 NUL，
///      代理对以两个三字节单元（CESU-8 形态）顺序落两个 char16。字节序一致
///      往返由编码侧 EscapeUtil 负责，此处只管解码容错（畸形字节落 '?'）。
ca::str::Utf16String modifiedUtf16(std::string_view value)
{
    ca::str::Utf16StringBuilder builder;
    builder.reserve(value.size());
    ca::usize index = 0;
    while (index < value.size()) {
        const ca::u8 lead = static_cast<ca::u8>(value[index]);
        ca::u16 unit = 0;
        if ((lead & 0x80) == 0) {
            unit = lead;
            index += 1;
        }
        else if ((lead & 0xE0) == 0xC0) {
            unit = static_cast<ca::u16>((lead & 0x1F) << 6);
            if (index + 1 < value.size())
                unit |= static_cast<ca::u16>(static_cast<ca::u8>(value[index + 1]) & 0x3F);
            index += 2;
        }
        else if ((lead & 0xF0) == 0xE0) {
            unit = static_cast<ca::u16>((lead & 0x0F) << 12);
            if (index + 1 < value.size())
                unit |= static_cast<ca::u16>((static_cast<ca::u8>(value[index + 1]) & 0x3F) << 6);
            if (index + 2 < value.size())
                unit |= static_cast<ca::u16>(static_cast<ca::u8>(value[index + 2]) & 0x3F);
            index += 3;
        }
        else {
            unit = '?';
            index += 1;
        }
        builder.append(ca::str::Char16(unit));
    }
    return builder.build();
}

std::string escaped(ca::str::Utf8StringRef value, bool string)
{
    //CP 内容是 modified UTF-8（可能含 CESU-8 代理对/0xC0 0x80），不走严格 UTF-8 校验。
    auto converted = modifiedUtf16(std::string_view(
        reinterpret_cast<const char*>(value.data()), value.byte_length()));
    auto result    = string ? util::EscapeUtil::escape_string(converted.ref())
                            : util::EscapeUtil::escape_literal(converted.ref());
    return result.to_utf8_string().to_std_string();
}

std::string literal(std::string_view value)
{
    return escaped(ca::str::Utf8StringRef::from_data(
                       reinterpret_cast<const ca::u8*>(value.data()), value.size()),
                   false);
}

std::string quoted(std::string_view value)
{
    return "\"" + escaped(ca::str::Utf8StringRef::from_data(
                              reinterpret_cast<const ca::u8*>(value.data()), value.size()),
                          true)
           + "\"";
}

std::string floating(ca::f64 value, bool wide)
{
    if (std::isnan(value)) return wide ? "NaND" : "NaNF";
    if (std::isinf(value)) {
        std::string result = value < 0 ? "-Infinity" : "Infinity";
        return result + (wide ? "D" : "F");
    }
    std::ostringstream stream;
    stream << std::setprecision(wide ? 17 : 9) << value;
    std::string result   = stream.str();
    const auto  exponent = result.find_first_of("eE");
    if (exponent != std::string::npos && exponent + 1 < result.size() &&
        result[exponent + 1] == '+')
        result.erase(exponent + 1, 1);
    if (result.find('.') == std::string::npos && result.find_first_of("eE") == std::string::npos)
        result += ".0";
    return result + (wide ? "D" : "F");
}

std::string lowerOpcode(C::Op op)
{
    const char* text = C::mnemonicOf(op);
    if (text == nullptr || *text == '\0')
        throw std::invalid_argument("Unsupported JVM opcode");
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

std::string modifiers(ca::i32 access, bool clazz, bool method)
{
    std::string result;
    auto        add = [&](ca::i32 flag, const char* name) {
        if ((access & flag) != 0) result += std::string(name) + " ";
    };
    add(ACC_PUBLIC, "public");
    add(ACC_PRIVATE, "private");
    add(ACC_PROTECTED, "protected");
    add(ACC_STATIC, "static");
    add(ACC_FINAL, "final");
    if (clazz) add(ACC_SUPER, "super");
    if (method) add(ACC_SYNCHRONIZED, "synchronized");
    if (!clazz && !method) {
        add(ACC_VOLATILE, "volatile");
        add(ACC_TRANSIENT, "transient");
    }
    if (method) {
        add(ACC_BRIDGE, "bridge");
        add(ACC_VARARGS, "varargs");
        add(ACC_NATIVE, "native");
    }
    add(ACC_INTERFACE, "interface");
    add(ACC_ABSTRACT, "abstract");
    add(ACC_STRICT, "strictfp");
    add(ACC_SYNTHETIC, "synthetic");
    add(ACC_ANNOTATION, "annotation");
    add(ACC_ENUM, "enum");
    add(ACC_MODULE, "module");
    add(ACC_RECORD, "record");
    return result;
}

// —— 常量池载荷打印（ldc / 字段初值 / BSM 静态参数共用）——

std::string constant(const C::ClassFile& cf, const C::BootstrapMethodsAttr* bsm, ca::u32 index);

const char* handleKind(ca::u32 kind)
{
    static const char* kinds[] = {nullptr,
                                  "getfield",
                                  "getstatic",
                                  "putfield",
                                  "putstatic",
                                  "invokevirtual",
                                  "invokestatic",
                                  "invokespecial",
                                  "newinvokespecial",
                                  "invokeinterface"};
    if (kind < 1 || kind > 9) throw std::invalid_argument("Unsupported method handle kind");
    return kinds[kind];
}

std::string handle(const C::ClassFile& cf, ca::u32 index)
{
    const auto& entry = cf.cp.at(index);
    if (entry.tag != C::CpTag::MethodHandle)
        throw std::invalid_argument("Constant is not a method handle");
    const auto& reference = cf.cp.at(entry.b);
    const auto& nat       = cf.cp.at(reference.b);
    return std::string("{ ") + handleKind(entry.a) + ", " +
           literal(cf.className(reference.a)) + "." + literal(cf.cp.utf8Of(nat.a)) + ", " +
           literal(cf.cp.utf8Of(nat.b)) + " }";
}

std::string constants(const C::ClassFile& cf, const C::BootstrapMethodsAttr* bsm,
                      const std::vector<ca::u32>& values)
{
    std::string result = "{ ";
    for (ca::usize index = 0; index < values.size(); ++index) {
        if (index) result += ", ";
        result += constant(cf, bsm, values[index]);
    }
    return result + " }";
}

std::string constant(const C::ClassFile& cf, const C::BootstrapMethodsAttr* bsm, ca::u32 index)
{
    if (!cf.cp.valid(index)) throw std::invalid_argument("Invalid constant pool index");
    const auto& entry = cf.cp.at(index);
    switch (entry.tag) {
    case C::CpTag::Integer: return std::to_string(entry.ival);
    case C::CpTag::Long: return std::to_string(entry.lval) + "L";
    case C::CpTag::Float:
    {
        ca::f32 value = 0.0f;
        const auto bits = static_cast<ca::u32>(entry.ival);
        std::memcpy(&value, &bits, sizeof(value));
        return floating(value, false);
    }
    case C::CpTag::Double:
    {
        ca::f64 value = 0.0;
        const auto bits = static_cast<ca::u64>(entry.lval);
        std::memcpy(&value, &bits, sizeof(value));
        return floating(value, true);
    }
    case C::CpTag::String: return quoted(cf.cp.utf8Of(entry.a));
    case C::CpTag::Class:
    {
        //类字面量按字段描述符形态打印（与编译侧 constantValue 的 L/[ 前缀
        //识别闭环）；数组类目的 CP 名本身就是描述符。
        const auto name = cf.cp.utf8Of(entry.a);
        if (!name.empty() && name.front() == '[') return literal(name);
        return literal("L" + std::string(name) + ";");
    }
    case C::CpTag::MethodType: return literal(cf.cp.utf8Of(entry.a));
    case C::CpTag::MethodHandle: return handle(cf, index);
    case C::CpTag::Dynamic:
    {
        if (bsm == nullptr || entry.a >= bsm->entries.size())
            throw std::invalid_argument("Dynamic constant references missing bootstrap method");
        const auto& nat = cf.cp.at(entry.b);
        const auto& bsmEntry = bsm->entries[entry.a];
        return "{ " + literal(cf.cp.utf8Of(nat.a)) + ", " + literal(cf.cp.utf8Of(nat.b)) + ", " +
               handle(cf, bsmEntry.methodRef) + ", " + constants(cf, bsm, bsmEntry.args) + " }";
    }
    default: throw std::invalid_argument("Unsupported JVM constant");
    }
}

// —— 属性槽定位（按名字查找 + 惰性解码；会话持有非 const 模型）——

template <typename T>
T* findAttr(C::ClassFile& cf, std::vector<C::AttrSlot>& slots, std::string_view name)
{
    for (auto& slot : slots) {
        if (!cf.cp.utf8Equals(slot.nameCp, name)) continue;
        const auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err())
            throw std::invalid_argument(std::string("Attribute decode failed: ") + name.data());
        //只读取用可变体：peek() 是 const AttrBody*，tag_cast 的 const 链会一路
        //传染到 Record 组件的嵌套槽表；槽的 body 是公开成员，直接取不置脏。
        auto* body = ca::core::tag_cast::cast<T>(slot.body.get());
        if (body == nullptr)
            throw std::invalid_argument(std::string("Unexpected attribute body: ") + name.data());
        return body;
    }
    return nullptr;
}

// —— 注解打印（AnnotationsAttr 的 typed 模型 → Jasm 文本）——

std::string annotationBody(const C::ClassFile& cf, const C::AnnotationsAttr::Annotation& annotation);

std::string annotationElement(const C::ClassFile& cf, const C::AnnotationsAttr::ElemValue& value)
{
    switch (value.tag) {
    case 's': return quoted(cf.cp.utf8Of(value.cp));
    case 'B':
    case 'S':
    case 'I': return std::to_string(cf.cp.at(value.cp).ival);
    case 'C':
    {
        ca::str::Utf16StringBuilder builder;
        builder.append(ca::str::Char16(static_cast<ca::u16>(cf.cp.at(value.cp).ival)));
        auto text         = builder.build();
        auto escapedValue = util::EscapeUtil::escape_string(text.ref());
        return "'" + escapedValue.to_utf8_string().to_std_string() + "'";
    }
    case 'Z': return cf.cp.at(value.cp).ival != 0 ? "true" : "false";
    case 'J': return std::to_string(cf.cp.at(value.cp).lval) + "L";
    case 'F':
    {
        ca::f32 number = 0.0f;
        const auto bits = static_cast<ca::u32>(cf.cp.at(value.cp).ival);
        std::memcpy(&number, &bits, sizeof(number));
        return floating(number, false);
    }
    case 'D':
    {
        ca::f64 number = 0.0;
        const auto bits = static_cast<ca::u64>(cf.cp.at(value.cp).lval);
        std::memcpy(&number, &bits, sizeof(number));
        return floating(number, true);
    }
    case 'c': return literal(cf.cp.utf8Of(value.cp));
    case 'e':
        return ".enum " + literal(cf.cp.utf8Of(value.typeNameCp)) + " " +
               literal(cf.cp.utf8Of(value.constNameCp));
    case '@':
        return value.nested ? ".annotation " + literal(cf.cp.utf8Of(value.nested->typeCp)) + " " +
                                  annotationBody(cf, *value.nested)
                            : ".annotation java/lang/Object { }";
    case '[':
    {
        std::string result = "{ ";
        for (ca::usize index = 0; index < value.items.size(); ++index) {
            if (index) result += ", ";
            result += annotationElement(cf, value.items[index]);
        }
        return result + " }";
    }
    default: throw std::invalid_argument("Unsupported annotation value");
    }
}

std::string annotationBody(const C::ClassFile& cf, const C::AnnotationsAttr::Annotation& annotation)
{
    std::string result = "{ ";
    for (ca::usize index = 0; index < annotation.values.size(); ++index) {
        if (index) result += ", ";
        const auto& value = annotation.values[index];
        const auto  name  = cf.cp.utf8Of(value.first);
        if (!name.empty()) result += literal(name) + ": ";
        result += annotationElement(cf, value.second);
    }
    return result + " }";
}

void annotations(std::ostringstream& output, const C::ClassFile& cf,
                 const std::vector<C::AnnotationsAttr::Annotation>& values, const char* keyword,
                 const std::string& indent = {})
{
    for (const auto& value : values)
        output << indent << keyword << ' ' << literal(cf.cp.utf8Of(value.typeCp)) << ' '
               << annotationBody(cf, value) << '\n';
}

void typeAnnotations(std::ostringstream& output, const C::ClassFile& cf,
                     const std::vector<C::TypeAnnotationsAttr::Annotation>& values,
                     const char* keyword, const std::string& indent = {})
{
    for (const auto& value : values) {
        ca::u8 targetType = 0;
        ca::u32 first = 0, second = 0;
        switch (value.targetType) {
        case C::TypeAnnotationsAttr::kClassTypeParameter:
        case C::TypeAnnotationsAttr::kMethodTypeParameter:
        case C::TypeAnnotationsAttr::kSupertype:
        case C::TypeAnnotationsAttr::kClassTypeParameterBound:
        case C::TypeAnnotationsAttr::kMethodTypeParameterBound:
        case C::TypeAnnotationsAttr::kFormalsParameter:
        case C::TypeAnnotationsAttr::kThrows:
            first  = value.targetType == C::TypeAnnotationsAttr::kClassTypeParameter ||
                             value.targetType == C::TypeAnnotationsAttr::kMethodTypeParameter
                         ? value.typeParameterIndex
                     : value.targetType == C::TypeAnnotationsAttr::kSupertype
                         ? value.supertypeIndex
                     : value.targetType ==
                               C::TypeAnnotationsAttr::kClassTypeParameterBound ||
                               value.targetType ==
                                   C::TypeAnnotationsAttr::kMethodTypeParameterBound
                         ? value.typeParameterIndex
                     : value.targetType == C::TypeAnnotationsAttr::kFormalsParameter
                         ? value.formalParameterIndex
                         : value.throwsIndex;
            second = value.targetType == C::TypeAnnotationsAttr::kClassTypeParameterBound ||
                             value.targetType ==
                                 C::TypeAnnotationsAttr::kMethodTypeParameterBound
                         ? value.boundIndex
                         : 0;
            break;
        default:
            //Code 级（带 pc offset）形态不进文本面；原样按 offset 打包进高位，
            //编译侧原样回写，不做结构化解释。
            first  = value.offset;
            second = value.typeArgumentIndex;
            break;
        }
        output << indent << keyword << ' '
               << util::packTypeRef(value.targetType, first, second) << ' '
               << literal(util::typePathToString(value.typePath)) << ' '
               << literal(cf.cp.utf8Of(value.typeIndexCp)) << ' '
               << "{ ";
        for (ca::usize index = 0; index < value.values.size(); ++index) {
            if (index) output << ", ";
            const auto& entry = value.values[index];
            const auto  name  = cf.cp.utf8Of(entry.first);
            if (!name.empty()) output << literal(name) << ": ";
            output << annotationElement(cf, entry.second);
        }
        output << " }" << '\n';
    }
}

// —— 方法体打印（promote 后的 CodeUnit：LabelId 边界 + 扁平 Insn）——

struct LabelHash final
{
    ca::usize operator()(const IR::LabelId& id) const noexcept
    {
        return static_cast<ca::usize>((id.owner * 1315423911u) ^ (id.slot * 2654435761u) ^
                                      (id.gen * 97u));
    }
};

using LabelNames = std::unordered_map<IR::LabelId, std::string, LabelHash>;

class MethodLabels final
{
public:
    explicit MethodLabels(const IR::CodeUnit& unit)
    {
        //按指令顺序给锚定标签命名（旧栈 LABEL 节点顺序语义）；再给仍被引用、
        //但锚定在方法末尾（End 边界）的标签续编——它们打印在方法体末尾。
        for (const auto id : unit.dom.order()) {
            for (const auto& label : unit.dom.labelsAt(id)) {
                if (names_.count(label) == 0)
                    names_.emplace(label, "L" + std::to_string(next_++));
            }
        }
        for (const auto id : unit.dom.order()) {
            const auto* insn = unit.dom.peek(id);
            if (insn == nullptr) continue;
            switch (C::shapeOf(insn->op)) {
            case C::Shape::Branch2:
            case C::Shape::Branch4: ensure(insn->target); break;
            case C::Shape::TableSwitch:
            case C::Shape::LookupSwitch:
                ensure(insn->target);
                for (const auto& target : insn->targets) ensure(target);
                break;
            default: break;
            }
        }
        for (const auto& entry : unit.exc) {
            ensure(entry.start);
            ensure(entry.end);
            ensure(entry.handler);
        }
    }

    const std::string& name(const IR::LabelId& label) const
    {
        const auto found = names_.find(label);
        if (found == names_.end())
            throw std::invalid_argument("Instruction references unknown label");
        return found->second;
    }

    std::vector<IR::LabelId> endLabels(const IR::CodeUnit& unit) const
    {
        std::vector<IR::LabelId> result;
        for (const auto& [label, text] : names_) {
            const auto resolved = unit.dom.resolveLabel(label);
            if (resolved.is_ok() && resolved.unwrap().isEnd()) result.push_back(label);
        }
        std::sort(result.begin(), result.end(), [&](const auto& left, const auto& right) {
            return name(left) < name(right);
        });
        return result;
    }

private:
    void ensure(const IR::LabelId& label)
    {
        if (names_.count(label) == 0 && label != IR::kNoLabel)
            names_.emplace(label, "L" + std::to_string(next_++));
    }

    LabelNames names_;
    ca::usize  next_ = 0;
};

const char* newArrayType(ca::i32 operand)
{
    switch (operand) {
    case 4: return "boolean";
    case 5: return "char";
    case 6: return "float";
    case 7: return "double";
    case 8: return "byte";
    case 9: return "short";
    case 10: return "int";
    case 11: return "long";
    default: throw std::invalid_argument("Unsupported NEWARRAY operand");
    }
}


std::string instruction(const C::ClassFile& cf, const C::BootstrapMethodsAttr* bsm,
                        const IR::Insn& in, const MethodLabels& labels)
{
    const std::string opcode = lowerOpcode(in.op);
    switch (C::shapeOf(in.op)) {
    case C::Shape::None: return opcode;
    case C::Shape::Imm1:
    case C::Shape::Imm2: return opcode + " " + std::to_string(in.a);
    case C::Shape::Local1: return opcode + " " + std::to_string(in.a);
    case C::Shape::Iinc1: return "iinc " + std::to_string(in.a) + " " + std::to_string(in.b);
    case C::Shape::Atype1: return opcode + " " + newArrayType(in.a);
    case C::Shape::CP1:
    case C::Shape::CP2:
    case C::Shape::CP2Count:
    case C::Shape::CP2Zero2:
    case C::Shape::CP2Dims:
        switch (in.op) {
        case C::Op::LDC:
        case C::Op::LDC_W:
        case C::Op::LDC2_W:
            return (in.op == C::Op::LDC2_W ? "ldc2_w " : "ldc ") + constant(cf, bsm, in.cp);
        case C::Op::GETSTATIC:
        case C::Op::PUTSTATIC:
        case C::Op::GETFIELD:
        case C::Op::PUTFIELD:
        {
            const auto& entry = cf.cp.at(in.cp);
            const auto& nat   = cf.cp.at(entry.b);
            return opcode + " " + literal(cf.className(entry.a)) + "." +
                   literal(cf.cp.utf8Of(nat.a)) + " " + literal(cf.cp.utf8Of(nat.b));
        }
        case C::Op::INVOKEVIRTUAL:
        case C::Op::INVOKESPECIAL:
        case C::Op::INVOKESTATIC:
        case C::Op::INVOKEINTERFACE:
        {
            const auto& entry      = cf.cp.at(in.cp);
            const auto& nat        = cf.cp.at(entry.b);
            std::string mnemonic   = opcode;
            if (entry.tag == C::CpTag::InterfaceMethodref && in.op != C::Op::INVOKEINTERFACE)
                mnemonic += "interface";
            return mnemonic + " " + literal(cf.className(entry.a)) + "." +
                   literal(cf.cp.utf8Of(nat.a)) + " " + literal(cf.cp.utf8Of(nat.b));
        }
        case C::Op::INVOKEDYNAMIC:
        {
            if (bsm == nullptr)
                throw std::invalid_argument("invokedynamic without BootstrapMethods");
            const auto& entry = cf.cp.at(in.cp);
            if (entry.a >= bsm->entries.size())
                throw std::invalid_argument("invokedynamic bootstrap index out of range");
            const auto& nat       = cf.cp.at(entry.b);
            const auto& bsmEntry  = bsm->entries[entry.a];
            return "invokedynamic " + literal(cf.cp.utf8Of(nat.a)) + " " +
                   literal(cf.cp.utf8Of(nat.b)) + " " + handle(cf, bsmEntry.methodRef) + " " +
                   constants(cf, bsm, bsmEntry.args);
        }
        case C::Op::NEW:
        case C::Op::CHECKCAST:
        case C::Op::INSTANCEOF:
        case C::Op::ANEWARRAY:
            // anewarray 的 CP 名按原样打印：JVMS 口径是组件类型；旧栈自产
            // 字节是数组描述符——剥 '[' 会把组件形态误判为非法，装配侧两种
            // 形态都接受（描述符名进 anewarray 语义即组件为该数组类）。
            return opcode + " " + literal(cf.cp.utf8Of(cf.cp.at(in.cp).a));
        case C::Op::MULTIANEWARRAY:
            return opcode + " " + literal(cf.cp.utf8Of(cf.cp.at(in.cp).a)) + " " +
                   std::to_string(in.a);
        default: throw std::invalid_argument("Unsupported CP instruction");
        }
    case C::Shape::Branch2:
    case C::Shape::Branch4: return opcode + " " + labels.name(in.target);
    case C::Shape::TableSwitch:
    {
        std::string result = "tableswitch { min: " + std::to_string(in.a) + ", max: " +
                             std::to_string(in.a + static_cast<ca::i32>(in.targets.size()) - 1) +
                             ", cases: { ";
        for (ca::usize index = 0; index < in.targets.size(); ++index) {
            if (index) result += ", ";
            result += labels.name(in.targets[index]);
        }
        return result + " }, default: " + labels.name(in.target) + " }";
    }
    case C::Shape::LookupSwitch:
    {
        std::string result = "lookupswitch { ";
        for (ca::usize index = 0; index < in.keys.size(); ++index)
            result += std::to_string(in.keys[index]) + ": " + labels.name(in.targets[index]) +
                      ", ";
        return result + "default: " + labels.name(in.target) + " }";
    }
    default: throw std::invalid_argument("Unsupported JVM instruction");
    }
}

void printMethodDeclaration(std::ostringstream& output, IR::ClassSession& session,
                            ca::usize methodIdx, const C::BootstrapMethodsAttr* bsm)
{
    C::ClassFile& cf = session.backing();
    C::MemberInfo& m = cf.methods[methodIdx];
    const auto name  = cf.utf8(m.nameCp);
    const auto desc  = cf.utf8(m.descCp);

    if (const auto* visible = findAttr<C::AnnotationsAttr>(cf, m.attrs, "RuntimeVisibleAnnotations"))
        annotations(output, cf, visible->annos, ".visible-annotation", "    ");
    if (const auto* invisible =
            findAttr<C::AnnotationsAttr>(cf, m.attrs, "RuntimeInvisibleAnnotations"))
        annotations(output, cf, invisible->annos, ".invisible-annotation", "    ");
    if (const auto* visible =
            findAttr<C::TypeAnnotationsAttr>(cf, m.attrs, "RuntimeVisibleTypeAnnotations"))
        typeAnnotations(output, cf, visible->annotations, ".type-visible-annotation", "    ");
    if (const auto* invisible =
            findAttr<C::TypeAnnotationsAttr>(cf, m.attrs, "RuntimeInvisibleTypeAnnotations"))
        typeAnnotations(output, cf, invisible->annotations, ".type-invisible-annotation", "    ");
    if (const auto* signature = findAttr<C::SignatureAttr>(cf, m.attrs, "Signature"))
        output << "    .signature " << quoted(cf.cp.utf8Of(signature->signatureCp)) << '\n';
    output << "    .method " << modifiers(m.access, false, true) << literal(name) << ' '
           << literal(desc);

    if (!IR::hasCode(session, methodIdx)) {
        output << " { }\n";
        return;
    }
    auto promoted = IR::promoteMethod(session, methodIdx);
    if (promoted.is_err())
        throw std::invalid_argument("Method body promote failed: " +
                                    std::string(IR::to_string(promoted.unwrap_err())));
    IR::CodeUnitRef unit = std::move(promoted).unwrap();

    MethodLabels labels(*unit);
    //LineNumberTable：行号挂在指令 pc 上，按旧栈「跟在标签行之后」的版式打印。
    std::unordered_map<ca::u32, std::vector<ca::i32>> lines;
    auto decodedCode = cf.decodeCode(m);
    if (C::CodeAttr* code = decodedCode.is_ok() ? decodedCode.unwrap() : nullptr) {
        if (const auto* lnt =
                findAttr<C::LineNumberTableAttr>(cf, code->attrs, "LineNumberTable")) {
            for (const auto& row : lnt->rows) lines[row.startPc].push_back(
                static_cast<ca::i32>(row.line));
        }
    }

    output << " {\n";
    if (!unit->exc.empty()) {
        output << "        exceptions: {\n";
        for (ca::usize index = 0; index < unit->exc.size(); ++index) {
            const auto& block = unit->exc[index];
            output << "            { " << labels.name(block.start) << ", "
                   << labels.name(block.end) << ", " << labels.name(block.handler) << ", "
                   << (block.catchCp == 0 ? "*" : literal(cf.cp.utf8Of(cf.cp.at(block.catchCp).a)))
                   << " }";
            output << (index + 1 == unit->exc.size() ? "\n" : ",\n");
        }
        output << "        },\n";
    }
    output << "        code: {\n";
    for (const auto id : unit->dom.order()) {
        const auto* insn = unit->dom.peek(id);
        if (insn == nullptr) continue;
        for (const auto& label : unit->dom.labelsAt(id))
            output << "        " << labels.name(label) << ":\n";
        const auto found = lines.find(insn->srcPc);
        if (found != lines.end())
            for (const auto line : found->second) output << "            line " << line << '\n';
        output << "            " << instruction(cf, bsm, *insn, labels) << '\n';
    }
    for (const auto& label : labels.endLabels(*unit))
        output << "        " << labels.name(label) << ":\n";
    output << "        }\n    }\n";
}

}   //namespace

error::Result<ca::str::Utf16String> JvmPrinter::print(const std::vector<ca::u8>& classBytes) const
{
    try {
        auto opened = IR::makeSessionFromBytes(C::ByteView{classBytes.data(), classBytes.size()});
        if (opened.is_err()) {
            const auto& failure = opened.unwrap_err();
            auto message = failure.stage == IR::SessionError::Stage::Read
                               ? std::string("classfile read failed: ") +
                                     C::to_string(failure.read)
                               : std::string("classfile session failed: ") +
                                     IR::to_string(failure.session);
            return error::Result<ca::str::Utf16String>::err({error::Error(utf16(message).ref())});
        }
        //unwrap() && 按值移出 shared_ptr：先落命名变量持有会话，再取引用。
        auto            owned   = std::move(opened).unwrap();
        IR::ClassSession& session = *owned;
        C::ClassFile&              cf      = session.backing();
        const C::BootstrapMethodsAttr* bsm =
            findAttr<C::BootstrapMethodsAttr>(cf, cf.attrs, "BootstrapMethods");

        std::ostringstream output;
        if (const auto* inner = findAttr<C::InnerClassesAttr>(cf, cf.attrs, "InnerClasses")) {
            for (const auto& row : inner->rows) {
                output << ".inner " << modifiers(static_cast<ca::i32>(row.accessFlags), true, false)
                       << "{ ";
                bool comma = false;
                if (row.innerNameCp != 0) {
                    output << "name: " << literal(cf.cp.utf8Of(row.innerNameCp));
                    comma = true;
                }
                if (comma) output << ", ";
                output << "inner: " << literal(cf.cp.utf8Of(cf.cp.at(row.innerClassInfoCp).a));
                if (row.outerClassInfoCp != 0)
                    output << ", outer: " << literal(cf.cp.utf8Of(cf.cp.at(row.outerClassInfoCp).a));
                output << " }\n";
            }
        }
        if (const auto* source = findAttr<C::SourceFileAttr>(cf, cf.attrs, "SourceFile"))
            output << ".sourcefile " << quoted(cf.cp.utf8Of(source->sourceFileCp)) << '\n';
        if (const auto* enclosing = findAttr<C::EnclosingMethodAttr>(cf, cf.attrs, "EnclosingMethod")) {
            const auto outer = cf.cp.utf8Of(cf.cp.at(enclosing->classCp).a);
            output << ".outer-class " << literal(outer) << '\n';
            if (enclosing->methodNatCp != 0) {
                const auto& nat = cf.cp.at(enclosing->methodNatCp);
                output << ".outer-method " << literal(cf.cp.utf8Of(nat.a)) << ' '
                       << literal(cf.cp.utf8Of(nat.b)) << '\n';
            }
        }
        if (const auto* nestHost = findAttr<C::NestHostAttr>(cf, cf.attrs, "NestHost"))
            output << ".nest-host " << literal(cf.cp.utf8Of(cf.cp.at(nestHost->hostClassCp).a))
                   << '\n';
        if (const auto* members = findAttr<C::NestMembersAttr>(cf, cf.attrs, "NestMembers"))
            for (const auto cp : members->memberCps)
                output << ".nest-member " << literal(cf.cp.utf8Of(cf.cp.at(cp).a)) << '\n';
        if (const auto* permitted =
                findAttr<C::PermittedSubclassesAttr>(cf, cf.attrs, "PermittedSubclasses"))
            for (const auto cp : permitted->permittedCps)
                output << ".permitted-subclass " << literal(cf.cp.utf8Of(cf.cp.at(cp).a)) << '\n';
        if (auto* record = findAttr<C::RecordAttr>(cf, cf.attrs, "Record")) {
            for (auto& component : record->components) {
                if (auto* visible = findAttr<C::AnnotationsAttr>(
                        cf, component.attrs, "RuntimeVisibleAnnotations"))
                    annotations(output, cf, visible->annos, ".visible-annotation");
                if (auto* invisible = findAttr<C::AnnotationsAttr>(
                        cf, component.attrs, "RuntimeInvisibleAnnotations"))
                    annotations(output, cf, invisible->annos, ".invisible-annotation");
                if (auto* typeVisible = findAttr<C::TypeAnnotationsAttr>(
                        cf, component.attrs, "RuntimeVisibleTypeAnnotations"))
                    typeAnnotations(output, cf, typeVisible->annotations,
                                    ".type-visible-annotation");
                if (auto* typeInvisible = findAttr<C::TypeAnnotationsAttr>(
                        cf, component.attrs, "RuntimeInvisibleTypeAnnotations"))
                    typeAnnotations(output, cf, typeInvisible->annotations,
                                    ".type-invisible-annotation");
                if (auto* signature =
                        findAttr<C::SignatureAttr>(cf, component.attrs, "Signature"))
                    output << ".signature " << quoted(cf.cp.utf8Of(signature->signatureCp))
                           << '\n';
                output << ".record-component " << literal(cf.cp.utf8Of(component.nameCp)) << ' '
                       << literal(cf.cp.utf8Of(component.descriptorCp)) << '\n';
            }
        }
        if (const auto* visible =
                findAttr<C::AnnotationsAttr>(cf, cf.attrs, "RuntimeVisibleAnnotations"))
            annotations(output, cf, visible->annos, ".visible-annotation");
        if (const auto* invisible =
                findAttr<C::AnnotationsAttr>(cf, cf.attrs, "RuntimeInvisibleAnnotations"))
            annotations(output, cf, invisible->annos, ".invisible-annotation");
        if (const auto* visible =
                findAttr<C::TypeAnnotationsAttr>(cf, cf.attrs, "RuntimeVisibleTypeAnnotations"))
            typeAnnotations(output, cf, visible->annotations, ".type-visible-annotation");
        if (const auto* invisible =
                findAttr<C::TypeAnnotationsAttr>(cf, cf.attrs, "RuntimeInvisibleTypeAnnotations"))
            typeAnnotations(output, cf, invisible->annotations, ".type-invisible-annotation");
        if (const auto* signature = findAttr<C::SignatureAttr>(cf, cf.attrs, "Signature"))
            output << ".signature " << quoted(cf.cp.utf8Of(signature->signatureCp)) << '\n';
        const auto superName = cf.superName();
        if (!superName.empty()) output << ".super " << literal(superName) << '\n';
        for (const auto cp : cf.interfaces)
            output << ".implements " << literal(cf.cp.utf8Of(cf.cp.at(cp).a)) << '\n';
        output << ".class " << modifiers(cf.access, true, false) << literal(cf.thisName()) << " {\n";
        for (auto& field : cf.fields) {
            if (auto* visible =
                    findAttr<C::AnnotationsAttr>(cf, field.attrs, "RuntimeVisibleAnnotations"))
                annotations(output, cf, visible->annos, ".visible-annotation", "    ");
            if (auto* invisible =
                    findAttr<C::AnnotationsAttr>(cf, field.attrs, "RuntimeInvisibleAnnotations"))
                annotations(output, cf, invisible->annos, ".invisible-annotation", "    ");
            if (auto* typeVisible =
                    findAttr<C::TypeAnnotationsAttr>(cf, field.attrs, "RuntimeVisibleTypeAnnotations"))
                typeAnnotations(output, cf, typeVisible->annotations, ".type-visible-annotation",
                                "    ");
            if (auto* typeInvisible = findAttr<C::TypeAnnotationsAttr>(
                    cf, field.attrs, "RuntimeInvisibleTypeAnnotations"))
                typeAnnotations(output, cf, typeInvisible->annotations,
                                ".type-invisible-annotation", "    ");
            if (auto* signature = findAttr<C::SignatureAttr>(cf, field.attrs, "Signature"))
                output << "    .signature " << quoted(cf.cp.utf8Of(signature->signatureCp))
                       << '\n';
            output << "    .field " << modifiers(field.access, false, false)
                   << literal(cf.utf8(field.nameCp)) << ' ' << literal(cf.utf8(field.descCp));
            if (const auto* value = findAttr<C::ConstantValueAttr>(cf, field.attrs, "ConstantValue"))
                output << " { value: " << constant(cf, bsm, value->constantValueCp) << " }";
            output << '\n';
        }
        if (!cf.fields.empty() && !cf.methods.empty()) output << '\n';
        for (ca::usize index = 0; index < cf.methods.size(); ++index) {
            printMethodDeclaration(output, session, index, bsm);
            if (index + 1 != cf.methods.size()) output << '\n';
        }
        output << "}\n";
        return error::Result<ca::str::Utf16String>::ok(utf16(output.str()));
    }
    catch (const std::exception& exception) {
        auto message = utf16(exception.what());
        return error::Result<ca::str::Utf16String>::err({error::Error(message.ref())});
    }
}

error::Result<ca::str::Utf16String> JvmPrinter::printMethod(
    const std::vector<ca::u8>& classBytes, ca::str::Utf8StringRef name,
    ca::str::Utf8StringRef descriptor) const
{
    try {
        auto opened = IR::makeSessionFromBytes(C::ByteView{classBytes.data(), classBytes.size()});
        if (opened.is_err()) {
            const auto& failure = opened.unwrap_err();
            auto message = failure.stage == IR::SessionError::Stage::Read
                               ? std::string("classfile read failed: ") +
                                     C::to_string(failure.read)
                               : std::string("classfile session failed: ") +
                                     IR::to_string(failure.session);
            return error::Result<ca::str::Utf16String>::err({error::Error(utf16(message).ref())});
        }
        //unwrap() && 按值移出 shared_ptr：先落命名变量持有会话，再取引用。
        auto          owned   = std::move(opened).unwrap();
        IR::ClassSession& session = *owned;
        C::ClassFile&     cf      = session.backing();
        const C::BootstrapMethodsAttr* bsm =
            findAttr<C::BootstrapMethodsAttr>(cf, cf.attrs, "BootstrapMethods");
        for (ca::usize index = 0; index < cf.methods.size(); ++index) {
            const auto& method = cf.methods[index];
            if (cf.utf8(method.nameCp) == std::string_view(name) &&
                cf.utf8(method.descCp) == std::string_view(descriptor)) {
                std::ostringstream output;
                printMethodDeclaration(output, session, index, bsm);
                return error::Result<ca::str::Utf16String>::ok(utf16(output.str()));
            }
        }
        return error::Result<ca::str::Utf16String>::err(
            {error::Error(utf16("Method not found").ref())});
    }
    catch (const std::exception& exception) {
        auto message = utf16(exception.what());
        return error::Result<ca::str::Utf16String>::err({error::Error(message.ref())});
    }
}

}   //namespace jhasm::printer
