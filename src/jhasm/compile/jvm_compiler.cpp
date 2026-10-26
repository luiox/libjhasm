#include "jvm_compiler.hpp"

#include "jhasm/instructions/jvm_instructions.hpp"
#include "jhasm/util/type_annotation_codec.hpp"

#include <jbc/jbc.hpp>

#include <libca/str/utf8_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jhasm::compile {
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

// 方法句柄 reference_kind（JVMS 4.4.8）。
constexpr ca::u32 H_GETFIELD = 1;
constexpr ca::u32 H_GETSTATIC = 2;
constexpr ca::u32 H_PUTFIELD = 3;
constexpr ca::u32 H_PUTSTATIC = 4;
constexpr ca::u32 H_INVOKEVIRTUAL = 5;
constexpr ca::u32 H_INVOKESTATIC = 6;
constexpr ca::u32 H_INVOKESPECIAL = 7;
constexpr ca::u32 H_NEWINVOKESPECIAL = 8;
constexpr ca::u32 H_INVOKEINTERFACE = 9;

// newarray 的 atype 操作数（JVMS 6.5 newarray 表）。
constexpr ca::i32 T_BOOLEAN = 4;
constexpr ca::i32 T_CHAR = 5;
constexpr ca::i32 T_FLOAT = 6;
constexpr ca::i32 T_DOUBLE = 7;
constexpr ca::i32 T_BYTE = 8;
constexpr ca::i32 T_SHORT = 9;
constexpr ca::i32 T_INT = 10;
constexpr ca::i32 T_LONG = 11;

[[noreturn]] void fail(const std::string& message) { throw std::invalid_argument(message); }

//错误类型的 to_string 探测（ADL）：没有诊断文本的错误退化成空串。
template <typename E, typename = void>
struct HasToString : std::false_type
{
};
template <typename E>
struct HasToString<E, std::void_t<decltype(to_string(std::declval<E>()))>> : std::true_type
{
};

template <typename T, typename E>
T require(ca::core::Result<T, E> result, const char* stage)
{
    if (result.is_err()) {
        std::string detail;
        if constexpr (HasToString<E>::value) detail = to_string(result.unwrap_err());
        fail(std::string(stage) + " failed" + (detail.empty() ? "" : ": " + detail));
    }
    if constexpr (std::is_void_v<T>) {
        return;
    }
    else {
        return std::move(result).unwrap();
    }
}

///@brief 规格字符串驻留仓：libjbc 的 MethodHandleSpec/SemAnnotation/BsmArg
///      一律持 string_view（借用型规格），而 Jasm 文本面的名字/描述符都是
///      即时临时串。构建侧把全部借用串落此仓，仓的生命周期覆盖到同步
///      intern/attach 调用返回为止。
class SpecArena final
{
public:
    std::string_view put(std::string value)
    {
        owned_.push_back(std::move(value));
        return owned_.back();
    }

private:
    std::deque<std::string> owned_;
};

bool equals(ca::str::Utf16StringRef value, const char* ascii) noexcept
{
    ca::usize index = 0;
    for (; ascii[index] != '\0'; ++index) {
        if (index >= value.length() ||
            value.char_at(index).value != static_cast<ca::u16>(ascii[index]))
            return false;
    }
    return index == value.length();
}

ca::str::Utf8String utf8(ca::str::Utf16StringRef value)
{
    return value.to_utf8_string();
}

ca::str::Utf8String utf8(const ast::Element& value)
{
    return utf8(value.content());
}

///@brief UTF-16 → JVM modified UTF-8（JVMS 4.4.7）：NUL 走 0xC0 0x80，补充字符
///      按代理对拆成两个三字节单元（CESU-8 形态）。CP Utf8 载体的唯一编码口径，
///      与 printer 侧 modifiedUtf16 解码器 1:1 往返；ASCII 输入字节不变。
std::string modifiedUtf8(ca::str::Utf16StringRef value)
{
    std::string out;
    out.reserve(value.length() * 3);
    for (ca::usize index = 0; index < value.length(); ++index) {
        const ca::u16 unit = value.char_at(index).unit();
        if (unit == 0) {
            out += "\xC0\x80";
        }
        else if (unit < 0x80) {
            out += static_cast<char>(unit);
        }
        else if (unit < 0x800) {
            out += static_cast<char>(0xC0 | (unit >> 6));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
        else {
            out += static_cast<char>(0xE0 | (unit >> 12));
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
    }
    return out;
}

std::string narrow(const ast::Element& value)
{
    return modifiedUtf8(value.content());
}

ca::str::Utf16String diagnosticText(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::u32 access(const ast::Modifiers& modifiers)
{
    ca::u32 result = 0;
    for (const auto& value : modifiers.values()) {
        if (!value) continue;
        const auto name = value->content();
        if (equals(name, "public"))
            result |= ACC_PUBLIC;
        else if (equals(name, "private"))
            result |= ACC_PRIVATE;
        else if (equals(name, "protected"))
            result |= ACC_PROTECTED;
        else if (equals(name, "static"))
            result |= ACC_STATIC;
        else if (equals(name, "final"))
            result |= ACC_FINAL;
        else if (equals(name, "super"))
            result |= ACC_SUPER;
        else if (equals(name, "synchronized"))
            result |= ACC_SYNCHRONIZED;
        else if (equals(name, "volatile"))
            result |= ACC_VOLATILE;
        else if (equals(name, "bridge"))
            result |= ACC_BRIDGE;
        else if (equals(name, "transient"))
            result |= ACC_TRANSIENT;
        else if (equals(name, "varargs"))
            result |= ACC_VARARGS;
        else if (equals(name, "native"))
            result |= ACC_NATIVE;
        else if (equals(name, "interface"))
            result |= ACC_INTERFACE;
        else if (equals(name, "abstract"))
            result |= ACC_ABSTRACT;
        else if (equals(name, "strictfp"))
            result |= ACC_STRICT;
        else if (equals(name, "synthetic"))
            result |= ACC_SYNTHETIC;
        else if (equals(name, "annotation"))
            result |= ACC_ANNOTATION;
        else if (equals(name, "enum"))
            result |= ACC_ENUM;
        else if (equals(name, "module"))
            result |= ACC_MODULE;
        else if (equals(name, "record"))
            result |= ACC_RECORD;
    }
    //access_flags 是 u16 域：ACC_RECORD（0x10000）是 JDK 内部位，classfile 出口
    //按旧栈口径静默截断（Record 属性的在场与否才是记录类的规范形态）。
    return result & 0xFFFF;
}

///@brief 标识符 → 类内部名：L..; 描述符形态剥壳；数组描述符（[ 开头）与
///       普通内部名原样保留。
std::string internalNameOf(const ast::Identifier& value)
{
    //CP 内部名同样走 modified UTF-8：与 javac 一致，增补字符以 CESU-8 代理对落池。
    auto name = narrow(value);
    if (name.size() > 1 && name.front() == 'L' && name.back() == ';')
        return name.substr(1, name.size() - 2);
    return name;
}

///@brief 类型的字段描述符形态（注解 'c' 元素与注解类型落 CP 用）。
std::string descriptorOf(const ast::Identifier& value)
{
    const auto name = internalNameOf(value);
    if (!name.empty() && name.front() == '[') return name;
    return "L" + name + ";";
}

ca::i64 integerValue(const ast::Element& value)
{
    std::string text = narrow(value);
    if (!text.empty() && (text.back() == 'l' || text.back() == 'L')) text.pop_back();
    return std::stoll(text, nullptr, 0);
}

ca::f64 floatingValue(const ast::Element& value)
{
    std::string text = narrow(value);
    if (!text.empty() &&
        (text.back() == 'f' || text.back() == 'F' || text.back() == 'd' || text.back() == 'D'))
        text.pop_back();
    return std::stod(text);
}

C::Op opcode(ca::str::Utf16StringRef name)
{
#define JHASM_OPCODE(Name) \
    if (equals(name, #Name)) return C::Op::Name
    JHASM_OPCODE(NOP);
    JHASM_OPCODE(ACONST_NULL);
    JHASM_OPCODE(ICONST_M1);
    JHASM_OPCODE(ICONST_0);
    JHASM_OPCODE(ICONST_1);
    JHASM_OPCODE(ICONST_2);
    JHASM_OPCODE(ICONST_3);
    JHASM_OPCODE(ICONST_4);
    JHASM_OPCODE(ICONST_5);
    JHASM_OPCODE(LCONST_0);
    JHASM_OPCODE(LCONST_1);
    JHASM_OPCODE(FCONST_0);
    JHASM_OPCODE(FCONST_1);
    JHASM_OPCODE(FCONST_2);
    JHASM_OPCODE(DCONST_0);
    JHASM_OPCODE(DCONST_1);
    JHASM_OPCODE(BIPUSH);
    JHASM_OPCODE(SIPUSH);
    JHASM_OPCODE(LDC);
    JHASM_OPCODE(ILOAD);
    JHASM_OPCODE(ILOAD_0);
    JHASM_OPCODE(ILOAD_1);
    JHASM_OPCODE(ILOAD_2);
    JHASM_OPCODE(ILOAD_3);
    JHASM_OPCODE(LLOAD);
    JHASM_OPCODE(LLOAD_0);
    JHASM_OPCODE(LLOAD_1);
    JHASM_OPCODE(LLOAD_2);
    JHASM_OPCODE(LLOAD_3);
    JHASM_OPCODE(FLOAD);
    JHASM_OPCODE(FLOAD_0);
    JHASM_OPCODE(FLOAD_1);
    JHASM_OPCODE(FLOAD_2);
    JHASM_OPCODE(FLOAD_3);
    JHASM_OPCODE(DLOAD);
    JHASM_OPCODE(DLOAD_0);
    JHASM_OPCODE(DLOAD_1);
    JHASM_OPCODE(DLOAD_2);
    JHASM_OPCODE(DLOAD_3);
    JHASM_OPCODE(ALOAD);
    JHASM_OPCODE(ALOAD_0);
    JHASM_OPCODE(ALOAD_1);
    JHASM_OPCODE(ALOAD_2);
    JHASM_OPCODE(ALOAD_3);
    JHASM_OPCODE(IALOAD);
    JHASM_OPCODE(LALOAD);
    JHASM_OPCODE(FALOAD);
    JHASM_OPCODE(DALOAD);
    JHASM_OPCODE(AALOAD);
    JHASM_OPCODE(BALOAD);
    JHASM_OPCODE(CALOAD);
    JHASM_OPCODE(SALOAD);
    JHASM_OPCODE(ISTORE);
    JHASM_OPCODE(ISTORE_0);
    JHASM_OPCODE(ISTORE_1);
    JHASM_OPCODE(ISTORE_2);
    JHASM_OPCODE(ISTORE_3);
    JHASM_OPCODE(LSTORE);
    JHASM_OPCODE(LSTORE_0);
    JHASM_OPCODE(LSTORE_1);
    JHASM_OPCODE(LSTORE_2);
    JHASM_OPCODE(LSTORE_3);
    JHASM_OPCODE(FSTORE);
    JHASM_OPCODE(FSTORE_0);
    JHASM_OPCODE(FSTORE_1);
    JHASM_OPCODE(FSTORE_2);
    JHASM_OPCODE(FSTORE_3);
    JHASM_OPCODE(DSTORE);
    JHASM_OPCODE(DSTORE_0);
    JHASM_OPCODE(DSTORE_1);
    JHASM_OPCODE(DSTORE_2);
    JHASM_OPCODE(DSTORE_3);
    JHASM_OPCODE(ASTORE);
    JHASM_OPCODE(ASTORE_0);
    JHASM_OPCODE(ASTORE_1);
    JHASM_OPCODE(ASTORE_2);
    JHASM_OPCODE(ASTORE_3);
    JHASM_OPCODE(IASTORE);
    JHASM_OPCODE(LASTORE);
    JHASM_OPCODE(FASTORE);
    JHASM_OPCODE(DASTORE);
    JHASM_OPCODE(AASTORE);
    JHASM_OPCODE(BASTORE);
    JHASM_OPCODE(CASTORE);
    JHASM_OPCODE(SASTORE);
    JHASM_OPCODE(POP);
    JHASM_OPCODE(POP2);
    JHASM_OPCODE(DUP);
    JHASM_OPCODE(DUP_X1);
    JHASM_OPCODE(DUP_X2);
    JHASM_OPCODE(DUP2);
    JHASM_OPCODE(DUP2_X1);
    JHASM_OPCODE(DUP2_X2);
    JHASM_OPCODE(SWAP);
    JHASM_OPCODE(IADD);
    JHASM_OPCODE(LADD);
    JHASM_OPCODE(FADD);
    JHASM_OPCODE(DADD);
    JHASM_OPCODE(ISUB);
    JHASM_OPCODE(LSUB);
    JHASM_OPCODE(FSUB);
    JHASM_OPCODE(DSUB);
    JHASM_OPCODE(IMUL);
    JHASM_OPCODE(LMUL);
    JHASM_OPCODE(FMUL);
    JHASM_OPCODE(DMUL);
    JHASM_OPCODE(IDIV);
    JHASM_OPCODE(LDIV);
    JHASM_OPCODE(FDIV);
    JHASM_OPCODE(DDIV);
    JHASM_OPCODE(IREM);
    JHASM_OPCODE(LREM);
    JHASM_OPCODE(FREM);
    JHASM_OPCODE(DREM);
    JHASM_OPCODE(INEG);
    JHASM_OPCODE(LNEG);
    JHASM_OPCODE(FNEG);
    JHASM_OPCODE(DNEG);
    JHASM_OPCODE(ISHL);
    JHASM_OPCODE(LSHL);
    JHASM_OPCODE(ISHR);
    JHASM_OPCODE(LSHR);
    JHASM_OPCODE(IUSHR);
    JHASM_OPCODE(LUSHR);
    JHASM_OPCODE(IAND);
    JHASM_OPCODE(LAND);
    JHASM_OPCODE(IOR);
    JHASM_OPCODE(LOR);
    JHASM_OPCODE(IXOR);
    JHASM_OPCODE(LXOR);
    JHASM_OPCODE(IINC);
    JHASM_OPCODE(I2L);
    JHASM_OPCODE(I2F);
    JHASM_OPCODE(I2D);
    JHASM_OPCODE(L2I);
    JHASM_OPCODE(L2F);
    JHASM_OPCODE(L2D);
    JHASM_OPCODE(F2I);
    JHASM_OPCODE(F2L);
    JHASM_OPCODE(F2D);
    JHASM_OPCODE(D2I);
    JHASM_OPCODE(D2L);
    JHASM_OPCODE(D2F);
    JHASM_OPCODE(I2B);
    JHASM_OPCODE(I2C);
    JHASM_OPCODE(I2S);
    JHASM_OPCODE(LCMP);
    JHASM_OPCODE(FCMPL);
    JHASM_OPCODE(FCMPG);
    JHASM_OPCODE(DCMPL);
    JHASM_OPCODE(DCMPG);
    JHASM_OPCODE(IFEQ);
    JHASM_OPCODE(IFNE);
    JHASM_OPCODE(IFLT);
    JHASM_OPCODE(IFGE);
    JHASM_OPCODE(IFGT);
    JHASM_OPCODE(IFLE);
    JHASM_OPCODE(IF_ICMPEQ);
    JHASM_OPCODE(IF_ICMPNE);
    JHASM_OPCODE(IF_ICMPLT);
    JHASM_OPCODE(IF_ICMPGE);
    JHASM_OPCODE(IF_ICMPGT);
    JHASM_OPCODE(IF_ICMPLE);
    JHASM_OPCODE(IF_ACMPEQ);
    JHASM_OPCODE(IF_ACMPNE);
    JHASM_OPCODE(GOTO);
    JHASM_OPCODE(JSR);
    JHASM_OPCODE(RET);
    JHASM_OPCODE(TABLESWITCH);
    JHASM_OPCODE(LOOKUPSWITCH);
    JHASM_OPCODE(IRETURN);
    JHASM_OPCODE(LRETURN);
    JHASM_OPCODE(FRETURN);
    JHASM_OPCODE(DRETURN);
    JHASM_OPCODE(ARETURN);
    JHASM_OPCODE(RETURN);
    JHASM_OPCODE(GETSTATIC);
    JHASM_OPCODE(PUTSTATIC);
    JHASM_OPCODE(GETFIELD);
    JHASM_OPCODE(PUTFIELD);
    JHASM_OPCODE(INVOKEVIRTUAL);
    JHASM_OPCODE(INVOKESPECIAL);
    JHASM_OPCODE(INVOKESTATIC);
    JHASM_OPCODE(INVOKEINTERFACE);
    JHASM_OPCODE(INVOKEDYNAMIC);
    JHASM_OPCODE(NEW);
    JHASM_OPCODE(NEWARRAY);
    JHASM_OPCODE(ANEWARRAY);
    JHASM_OPCODE(ARRAYLENGTH);
    JHASM_OPCODE(ATHROW);
    JHASM_OPCODE(CHECKCAST);
    JHASM_OPCODE(INSTANCEOF);
    JHASM_OPCODE(MONITORENTER);
    JHASM_OPCODE(MONITOREXIT);
    JHASM_OPCODE(MULTIANEWARRAY);
    JHASM_OPCODE(IFNULL);
    JHASM_OPCODE(IFNONNULL);
    JHASM_OPCODE(GOTO_W);
    JHASM_OPCODE(JSR_W);
#undef JHASM_OPCODE
    if (equals(name, "LDC_W") || equals(name, "LDC2_W")) return C::Op::LDC;
    if (equals(name, "INVOKEVIRTUALINTERFACE")) return C::Op::INVOKEINTERFACE;
    if (equals(name, "INVOKESTATICINTERFACE")) return C::Op::INVOKESTATIC;
    if (equals(name, "INVOKESPECIALINTERFACE")) return C::Op::INVOKESPECIAL;
    throw std::invalid_argument("Unknown JVM opcode");
}

// —— 常量规格（Jasm 常量字面 → CP 落位前的事实描述；Dynamic 族递归）——

struct ConstSpec final
{
    enum class Kind : ca::u8 { Int, Long, Float, Double, String, Class, MethodHandle, Dynamic };
    Kind        kind = Kind::Int;
    ca::i64     num  = 0;
    ca::u32     fbits = 0;
    ca::u64     dbits = 0;
    //字符串族一律借用 SpecArena（下游 BsmArg/MethodHandleSpec 也是借用型）。
    std::string_view str;

    IR::MethodHandleSpec handle;

    std::string_view          dynamicName;
    std::string_view          dynamicDesc;
    IR::MethodHandleSpec      dynamicHandle;
    std::vector<ConstSpec>    dynamicArgs;
};

ca::u32 floatBits(ca::f32 value)
{
    ca::u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::pair<ca::u64, ca::f64> doubleWords(ca::f64 value)
{
    ca::u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return {bits, value};
}

IR::MethodHandleSpec methodHandle(const ast::ElementPtr& value, SpecArena& arena);
ConstSpec            constantValue(const ast::ElementPtr& value, SpecArena& arena);

IR::MethodHandleSpec makeHandle(ca::u32 kind, const std::string& path,
                                const std::string& descriptor, SpecArena& arena)
{
    const auto separator = path.rfind('.');
    if (separator == std::string::npos) fail("Handle path has no member name");
    IR::MethodHandleSpec spec;
    spec.referenceKind = kind;
    const auto stored  = arena.put(path);
    spec.owner         = stored.substr(0, separator);
    spec.name          = stored.substr(separator + 1);
    spec.desc          = arena.put(descriptor);
    if (kind <= H_PUTSTATIC) {
        if (jbc::type::parseFieldType(spec.desc).is_err()) fail("Invalid field descriptor");
    }
    else if (jbc::type::parseMethodDescriptor(spec.desc).is_err()) {
        fail("Invalid method descriptor");
    }
    return spec;
}

IR::MethodHandleSpec shortcutHandle(ca::str::Utf16StringRef name, SpecArena& arena)
{
    if (equals(name, "LambdaMetafactory.metafactory")) {
        return makeHandle(H_INVOKESTATIC,
                          "java/lang/invoke/LambdaMetafactory.metafactory",
                          "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/"
                          "invoke/MethodType;"
                          "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/"
                          "invoke/MethodType;)"
                          "Ljava/lang/invoke/CallSite;",
                          arena);
    }
    fail("Unknown method handle shortcut");
}

IR::MethodHandleSpec methodHandle(const ast::ElementPtr& value, SpecArena& arena)
{
    if (!value) fail("Missing method handle");
    if (value->getType() == ast::ElementKind::IDENTIFIER)
        return shortcutHandle(value->content(), arena);
    if (value->getType() != ast::ElementKind::ARRAY) fail("Invalid method handle");
    const auto& parts = std::static_pointer_cast<ast::Array>(value)->values();
    if (parts.size() != 3) fail("Invalid method handle arity");
    ca::u32     kind     = 0;
    const auto  kindName = parts[0]->content();
    if (equals(kindName, "getfield"))
        kind = H_GETFIELD;
    else if (equals(kindName, "getstatic"))
        kind = H_GETSTATIC;
    else if (equals(kindName, "putfield"))
        kind = H_PUTFIELD;
    else if (equals(kindName, "putstatic"))
        kind = H_PUTSTATIC;
    else if (equals(kindName, "invokevirtual"))
        kind = H_INVOKEVIRTUAL;
    else if (equals(kindName, "invokestatic"))
        kind = H_INVOKESTATIC;
    else if (equals(kindName, "invokespecial"))
        kind = H_INVOKESPECIAL;
    else if (equals(kindName, "newinvokespecial"))
        kind = H_NEWINVOKESPECIAL;
    else if (equals(kindName, "invokeinterface"))
        kind = H_INVOKEINTERFACE;
    else
        fail("Unknown method handle kind");
    return makeHandle(kind, narrow(*parts[1]), narrow(*parts[2]), arena);
}

ConstSpec numberConstant(const ast::Number& number)
{
    ConstSpec result;
    if (number.isFloatingPoint()) {
        if (number.isWide()) {
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(floatingValue(number)).first;
        }
        else {
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(static_cast<ca::f32>(floatingValue(number)));
        }
    }
    else if (number.isWide()) {
        result.kind = ConstSpec::Kind::Long;
        result.num  = integerValue(number);
    }
    else {
        result.kind = ConstSpec::Kind::Int;
        result.num  = integerValue(number);
    }
    return result;
}

ConstSpec constantValue(const ast::ElementPtr& value, SpecArena& arena)
{
    if (!value) fail("Missing constant");
    switch (value->getType()) {
    case ast::ElementKind::NUMBER: return numberConstant(*std::static_pointer_cast<ast::Number>(value));
    case ast::ElementKind::STRING:
    {
        ConstSpec result;
        result.kind = ConstSpec::Kind::String;
        result.str  = arena.put(narrow(*value));
        return result;
    }
    case ast::ElementKind::CHARACTER:
    {
        ConstSpec result;
        result.kind = ConstSpec::Kind::Int;
        result.num  = value->content().empty() ? 0 : value->content().char_at(0).value;
        return result;
    }
    case ast::ElementKind::BOOL:
    {
        ConstSpec result;
        result.kind = ConstSpec::Kind::Int;
        result.num  = std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0;
        return result;
    }
    case ast::ElementKind::IDENTIFIER:
    {
        auto text = value->content();
        if (equals(text, "true") || equals(text, "false")) {
            ConstSpec result;
            result.kind = ConstSpec::Kind::Int;
            result.num  = equals(text, "true") ? 1 : 0;
            return result;
        }
        if (equals(text, "NaN") || equals(text, "NaND")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(std::numeric_limits<ca::f64>::quiet_NaN()).first;
            return result;
        }
        if (equals(text, "NaNF")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(std::numeric_limits<ca::f32>::quiet_NaN());
            return result;
        }
        if (equals(text, "Infinity") || equals(text, "+Infinity") || equals(text, "InfinityD") ||
            equals(text, "+InfinityD")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(std::numeric_limits<ca::f64>::infinity()).first;
            return result;
        }
        if (equals(text, "-Infinity") || equals(text, "-InfinityD")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(-std::numeric_limits<ca::f64>::infinity()).first;
            return result;
        }
        if (equals(text, "InfinityF") || equals(text, "+InfinityF")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(std::numeric_limits<ca::f32>::infinity());
            return result;
        }
        if (equals(text, "-InfinityF")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(-std::numeric_limits<ca::f32>::infinity());
            return result;
        }
        const auto descriptor = narrow(*value);
        if (!descriptor.empty() &&
            (descriptor.front() == 'L' || descriptor.front() == '[' || descriptor.front() == '(')) {
            if (jbc::type::parseFieldType(descriptor).is_err()) fail("Invalid type descriptor");
            ConstSpec result;
            result.kind = ConstSpec::Kind::Class;
            result.str  = arena.put(descriptor.front() == '['
                                        ? descriptor
                                        : descriptor.substr(1, descriptor.size() - 2));
            return result;
        }
        {
            ConstSpec result;
            result.kind   = ConstSpec::Kind::MethodHandle;
            result.handle = shortcutHandle(text, arena);
            return result;
        }
    }
    case ast::ElementKind::ARRAY:
    {
        const auto& parts = std::static_pointer_cast<ast::Array>(value)->values();
        if (parts.size() == 3) {
            ConstSpec result;
            result.kind   = ConstSpec::Kind::MethodHandle;
            result.handle = methodHandle(value, arena);
            return result;
        }
        if (parts.size() != 4) fail("Invalid dynamic constant");
        const auto descriptor = narrow(*parts[1]);
        const auto parsed     = jbc::type::parseFieldType(descriptor);
        if (parsed.is_err()) fail("Invalid dynamic constant descriptor");
        ConstSpec result;
        result.kind          = ConstSpec::Kind::Dynamic;
        result.dynamicName   = arena.put(narrow(*parts[0]));
        result.dynamicDesc   = arena.put(descriptor);
        result.dynamicHandle = methodHandle(parts[2], arena);
        if (parts[3]->getType() == ast::ElementKind::ARRAY) {
            for (const auto& item : std::static_pointer_cast<ast::Array>(parts[3])->values())
                result.dynamicArgs.push_back(constantValue(item, arena));
        }
        return result;
    }
    default: fail("Unsupported constant kind");
    }
}

IR::BsmArg bsmArgOf(IR::ClassSession& session, const ConstSpec& value);

///@brief 常量规格 → CP 索引（语义 intern：命中既有池不膨胀）。
ca::u32 internConstant(IR::ClassSession& session, const ConstSpec& value)
{
    switch (value.kind) {
    case ConstSpec::Kind::Int: return session.internInteger(static_cast<ca::i32>(value.num));
    case ConstSpec::Kind::Long: return session.internLong(value.num);
    case ConstSpec::Kind::Float: return session.internFloatBits(value.fbits);
    case ConstSpec::Kind::Double: return session.internDoubleBits(value.dbits);
    case ConstSpec::Kind::String: return session.internString(value.str);
    case ConstSpec::Kind::Class: return session.internClass(value.str);
    case ConstSpec::Kind::MethodHandle:
        return require(session.internMethodHandle(value.handle), "internMethodHandle");
    case ConstSpec::Kind::Dynamic:
    {
        std::vector<IR::BsmArg> args;
        args.reserve(value.dynamicArgs.size());
        for (const auto& item : value.dynamicArgs) args.push_back(bsmArgOf(session, item));
        const auto bsm = require(
            session.appendBootstrapMethod(value.dynamicHandle, args), "appendBootstrapMethod");
        return require(session.internDynamic(bsm, value.dynamicName, value.dynamicDesc),
                       "internDynamic");
    }
    }
    fail("Unreachable constant kind");
}

IR::BsmArg bsmArgOf(IR::ClassSession& session, const ConstSpec& value)
{
    IR::BsmArg result;
    switch (value.kind) {
    case ConstSpec::Kind::Int:
    case ConstSpec::Kind::Long:
        result.kind = IR::BsmArg::Kind::Int;
        result.kind = value.kind == ConstSpec::Kind::Long ? IR::BsmArg::Kind::Long
                                                          : IR::BsmArg::Kind::Int;
        result.num  = value.num;
        return result;
    case ConstSpec::Kind::Float:
        result.kind  = IR::BsmArg::Kind::Float;
        result.fbits = value.fbits;
        return result;
    case ConstSpec::Kind::Double:
        result.kind  = IR::BsmArg::Kind::Double;
        result.dbits = value.dbits;
        return result;
    case ConstSpec::Kind::String:
        result.kind = IR::BsmArg::Kind::String;
        result.str  = value.str;
        return result;
    case ConstSpec::Kind::Class:
        result.kind = IR::BsmArg::Kind::Class;
        result.str  = value.str;
        return result;
    case ConstSpec::Kind::MethodHandle:
        result.kind   = IR::BsmArg::Kind::MethodHandle;
        result.handle = value.handle;
        return result;
    case ConstSpec::Kind::Dynamic:
    {
        std::vector<IR::BsmArg> nested;
        nested.reserve(value.dynamicArgs.size());
        for (const auto& item : value.dynamicArgs) nested.push_back(bsmArgOf(session, item));
        const auto bsm = require(
            session.appendBootstrapMethod(value.dynamicHandle, nested), "appendBootstrapMethod");
        result.kind = IR::BsmArg::Kind::Dynamic;
        result.bsm  = bsm;
        result.name = value.dynamicName;
        result.desc = value.dynamicDesc;
        return result;
    }
    }
    fail("Unreachable constant kind");
}

// —— 注解：ast → libjbc 合成规格（简单注解/默认值）与手写 typed 体（类型注解）——

IR::SemElemValue semElement(const ast::ElementPtr& value, SpecArena& arena);
IR::SemAnnotation semAnnotation(const ast::Annotation& value, SpecArena& arena);

IR::SemElemValue numberElement(const ast::Number& number)
{
    IR::SemElemValue result;
    if (number.isFloatingPoint()) {
        if (number.isWide()) {
            result.form  = IR::SemElemValue::Form::Double;
            result.dbits = doubleWords(floatingValue(number)).first;
        }
        else {
            result.form  = IR::SemElemValue::Form::Float;
            result.fbits = floatBits(static_cast<ca::f32>(floatingValue(number)));
        }
    }
    else if (number.isWide()) {
        result.form = IR::SemElemValue::Form::Long;
        result.num  = integerValue(number);
    }
    else {
        result.form = IR::SemElemValue::Form::Int;
        result.num  = integerValue(number);
    }
    return result;
}

IR::SemElemValue semElement(const ast::ElementPtr& value, SpecArena& arena)
{
    if (!value) fail("Missing annotation element");
    IR::SemElemValue result;
    switch (value->getType()) {
    case ast::ElementKind::BOOL:
        result.form = IR::SemElemValue::Form::Bool;
        result.num  = std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0;
        return result;
    case ast::ElementKind::CHARACTER:
        result.form = IR::SemElemValue::Form::Char;
        result.num  = value->content().empty() ? 0 : value->content().char_at(0).value;
        return result;
    case ast::ElementKind::STRING:
        result.form = IR::SemElemValue::Form::String;
        result.str  = arena.put(narrow(*value));
        return result;
    case ast::ElementKind::NUMBER: return numberElement(*std::static_pointer_cast<ast::Number>(value));
    case ast::ElementKind::IDENTIFIER:
        if (equals(value->content(), "true")) {
            result.form = IR::SemElemValue::Form::Bool;
            result.num  = 1;
            return result;
        }
        if (equals(value->content(), "false")) {
            result.form = IR::SemElemValue::Form::Bool;
            result.num  = 0;
            return result;
        }
        result.form = IR::SemElemValue::Form::Class;
        result.str  = arena.put(descriptorOf(*std::static_pointer_cast<ast::Identifier>(value)));
        return result;
    case ast::ElementKind::ENUM_VALUE:
    {
        const auto item = std::static_pointer_cast<ast::EnumValue>(value);
        result.form     = IR::SemElemValue::Form::Enum;
        result.enumType = arena.put(descriptorOf(*item->enumType()));
        result.str      = arena.put(narrow(*item->enumValue()));
        return result;
    }
    case ast::ElementKind::ANNOTATION:
        result.form   = IR::SemElemValue::Form::Nested;
        result.nested = std::make_shared<IR::SemAnnotation>(
            semAnnotation(*std::static_pointer_cast<ast::Annotation>(value), arena));
        return result;
    case ast::ElementKind::ARRAY:
        for (const auto& item : std::static_pointer_cast<ast::Array>(value)->values())
            result.array.push_back(semElement(item, arena));
        result.form = IR::SemElemValue::Form::Array;
        return result;
    default: fail("Unsupported annotation element");
    }
}

IR::SemAnnotation semAnnotation(const ast::Annotation& value, SpecArena& arena)
{
    IR::SemAnnotation result;
    result.typeDesc = arena.put(descriptorOf(*value.classType()));
    for (const auto& entry : value.values())
        result.values.emplace_back(arena.put(narrow(*entry.first)),
                                   semElement(entry.second, arena));
    return result;
}

C::AnnotationsAttr::ElemValue rawElement(IR::ClassSession& session, const ast::ElementPtr& value);
C::AnnotationsAttr::Annotation rawAnnotation(IR::ClassSession& session,
                                             const ast::Annotation& value);

C::AnnotationsAttr::ElemValue rawElement(IR::ClassSession& session, const ast::ElementPtr& value)
{
    if (!value) fail("Missing annotation element");
    C::AnnotationsAttr::ElemValue result;
    switch (value->getType()) {
    case ast::ElementKind::BOOL:
        result.tag = 'Z';
        result.cp  = session.internInteger(
            std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0);
        return result;
    case ast::ElementKind::CHARACTER:
        result.tag = 'C';
        result.cp  = session.internInteger(
            value->content().empty() ? 0 : value->content().char_at(0).value);
        return result;
    case ast::ElementKind::STRING:
        result.tag = 's';
        result.cp  = session.internString(narrow(*value));
        return result;
    case ast::ElementKind::NUMBER:
    {
        const auto number = std::static_pointer_cast<ast::Number>(value);
        if (number->isFloatingPoint()) {
            if (number->isWide()) {
                result.tag  = 'D';
                result.cp   = session.internDoubleBits(doubleWords(floatingValue(*number)).first);
            }
            else {
                result.tag = 'F';
                result.cp  = session.internFloatBits(
                    floatBits(static_cast<ca::f32>(floatingValue(*number))));
            }
        }
        else if (number->isWide()) {
            result.tag = 'J';
            result.cp  = session.internLong(integerValue(*number));
        }
        else {
            result.tag = 'I';
            result.cp  = session.internInteger(static_cast<ca::i32>(integerValue(*number)));
        }
        return result;
    }
    case ast::ElementKind::IDENTIFIER:
        if (equals(value->content(), "true") || equals(value->content(), "false")) {
            result.tag = 'Z';
            result.cp  = session.internInteger(equals(value->content(), "true") ? 1 : 0);
            return result;
        }
        result.tag = 'c';
        result.cp  = session.internUtf8(
            descriptorOf(*std::static_pointer_cast<ast::Identifier>(value)));
        return result;
    case ast::ElementKind::ENUM_VALUE:
    {
        const auto item   = std::static_pointer_cast<ast::EnumValue>(value);
        result.tag        = 'e';
        result.typeNameCp = session.internUtf8(descriptorOf(*item->enumType()));
        result.constNameCp = session.internUtf8(narrow(*item->enumValue()));
        return result;
    }
    case ast::ElementKind::ANNOTATION:
        result.tag    = '@';
        result.nested = std::make_unique<C::AnnotationsAttr::Annotation>(
            rawAnnotation(session, *std::static_pointer_cast<ast::Annotation>(value)));
        return result;
    case ast::ElementKind::ARRAY:
        result.tag = '[';
        for (const auto& item : std::static_pointer_cast<ast::Array>(value)->values())
            result.items.push_back(rawElement(session, item));
        return result;
    default: fail("Unsupported annotation element");
    }
}

C::AnnotationsAttr::Annotation rawAnnotation(IR::ClassSession& session,
                                             const ast::Annotation& value)
{
    C::AnnotationsAttr::Annotation result;
    result.typeCp = session.internUtf8(descriptorOf(*value.classType()));
    for (const auto& entry : value.values())
        result.values.emplace_back(session.internUtf8(narrow(*entry.first)),
                                   rawElement(session, entry.second));
    return result;
}

C::TypeAnnotationsAttr::Annotation typeAnnotation(IR::ClassSession& session,
                                                  const ast::Annotation& value)
{
    using TA = C::TypeAnnotationsAttr;
    C::TypeAnnotationsAttr::Annotation result;
    ca::u8 targetType = 0;
    ca::u32 first = 0, second = 0;
    util::unpackTypeRef(value.typeRef() ? static_cast<ca::i32>(integerValue(*value.typeRef())) : 0,
                        targetType, first, second);
    result.targetType = targetType;
    switch (targetType) {
    case TA::kClassTypeParameter:
    case TA::kMethodTypeParameter: result.typeParameterIndex = static_cast<ca::u8>(first); break;
    case TA::kSupertype: result.supertypeIndex = first; break;
    case TA::kClassTypeParameterBound:
    case TA::kMethodTypeParameterBound:
        result.typeParameterIndex = static_cast<ca::u8>(first);
        result.boundIndex         = static_cast<ca::u8>(second);
        break;
    case TA::kField:
    case TA::kMethodReturn:
    case TA::kMethodReceiver: break;
    case TA::kFormalsParameter: result.formalParameterIndex = static_cast<ca::u8>(first); break;
    case TA::kThrows: result.throwsIndex = first; break;
    default: fail("Unsupported type annotation target");
    }
    if (value.typePath()) result.typePath = util::parseTypePath(narrow(*value.typePath()));
    result.typeIndexCp = session.internUtf8(descriptorOf(*value.classType()));
    for (const auto& entry : value.values())
        result.values.emplace_back(session.internUtf8(narrow(*entry.first)),
                                   rawElement(session, entry.second));
    return result;
}

std::unique_ptr<C::TypeAnnotationsAttr> typeAnnotationsAttr(
    IR::ClassSession& session, const std::vector<ast::AnnotationPtr>& values)
{
    auto body  = std::make_unique<C::TypeAnnotationsAttr>();
    body->annotations.reserve(values.size());
    for (const auto& value : values) body->annotations.push_back(typeAnnotation(session, *value));
    return body;
}

template <typename AttachSimple>
void attachSimpleAnnotations(IR::ClassSession& session, bool visible,
                             const std::vector<ast::AnnotationPtr>& values,
                             AttachSimple&& attach)
{
    if (values.empty()) return;
    //arena 活到 attach 返回：SemAnnotation 的借用串都在这里。
    SpecArena             arena;
    std::vector<IR::SemAnnotation> annos;
    annos.reserve(values.size());
    for (const auto& value : values) annos.push_back(semAnnotation(*value, arena));
    require(attach(visible, annos), "attachAnnotations");
}

template <typename AttachType>
void attachTypeAnnotations(IR::ClassSession& session, bool visible,
                           const std::vector<ast::AnnotationPtr>& values, AttachType&& attach)
{
    if (values.empty()) return;
    require(attach(visible ? "RuntimeVisibleTypeAnnotations" : "RuntimeInvisibleTypeAnnotations",
                   typeAnnotationsAttr(session, values)),
            "attachTypeAnnotations");
}

///@brief 简单注解 + 类型注解的成套挂载（类/字段/方法共用形状）。
template <typename AttachSimple, typename AttachType>
void installAnnotations(IR::ClassSession& session, const ast::Attributes& attributes,
                        AttachSimple&& attachSimple, AttachType&& attachType)
{
    attachSimpleAnnotations(session, true, attributes.visibleAnnotations, attachSimple);
    attachSimpleAnnotations(session, false, attributes.invisibleAnnotations, attachSimple);
    attachTypeAnnotations(session, true, attributes.visibleTypeAnnotations, attachType);
    attachTypeAnnotations(session, false, attributes.invisibleTypeAnnotations, attachType);
}

std::unique_ptr<C::SignatureAttr> signatureAttr(IR::ClassSession& session,
                                                const ast::Attributes& attributes)
{
    if (!attributes.signature) return nullptr;
    auto body        = std::make_unique<C::SignatureAttr>();
    body->signatureCp = session.internUtf8(narrow(*attributes.signature));
    return body;
}

///@brief Record 组件嵌套属性表直挂（挂载点在 Record 属性体内，不经会话 attach 面）。
void mountComponentAttrs(IR::ClassSession& session, std::vector<C::AttrSlot>& table,
                         const ast::Attributes& attributes)
{
    if (auto signature = signatureAttr(session, attributes))
        table.push_back(C::AttrSlot::makeNew(session.internUtf8("Signature"), std::move(signature)));
    if (!attributes.visibleAnnotations.empty()) {
        auto body = std::make_unique<C::AnnotationsAttr>();
        for (const auto& value : attributes.visibleAnnotations)
            body->annos.push_back(rawAnnotation(session, *value));
        table.push_back(
            C::AttrSlot::makeNew(session.internUtf8("RuntimeVisibleAnnotations"), std::move(body)));
    }
    if (!attributes.invisibleAnnotations.empty()) {
        auto body = std::make_unique<C::AnnotationsAttr>();
        for (const auto& value : attributes.invisibleAnnotations)
            body->annos.push_back(rawAnnotation(session, *value));
        table.push_back(
            C::AttrSlot::makeNew(session.internUtf8("RuntimeInvisibleAnnotations"), std::move(body)));
    }
    if (!attributes.visibleTypeAnnotations.empty())
        table.push_back(C::AttrSlot::makeNew(
            session.internUtf8("RuntimeVisibleTypeAnnotations"),
            typeAnnotationsAttr(session, attributes.visibleTypeAnnotations)));
    if (!attributes.invisibleTypeAnnotations.empty())
        table.push_back(C::AttrSlot::makeNew(
            session.internUtf8("RuntimeInvisibleTypeAnnotations"),
            typeAnnotationsAttr(session, attributes.invisibleTypeAnnotations)));
}

// —— 方法体编译：线性发射 + 延迟标签绑定 —— //

ca::i32 paramSlotTotal(const std::string& descriptor, bool isStatic)
{
    const auto parsed = jbc::type::parseMethodDescriptor(descriptor);
    if (parsed.is_err()) fail("Invalid method descriptor");
    const auto method = std::move(parsed).unwrap();
    return (isStatic ? 0 : 1) + static_cast<ca::i32>(jbc::type::paramSlotCount(method));
}

class CodeCompiler final
{
public:
    CodeCompiler(IR::ClassSession& session, IR::CodeUnitRef unit, const ast::Method& method,
                 const std::string& descriptor)
        : session_(session)
        , unit_(std::move(unit))
        , method_(method)
    {
        const auto parsed = require(jbc::type::parseMethodDescriptor(descriptor),
                                    "parseMethodDescriptor");
        ca::i32   slot      = 0;
        ca::usize nameIndex = 0;
        const bool isStatic = (access(method.modifiers()) & ACC_STATIC) != 0;
        if (!isStatic) {
            if (!method.parameters().empty())
                variables_[narrow(*method.parameters()[nameIndex++])] = slot;
            ++slot;
        }
        for (const auto& parameter : parsed.params) {
            if (nameIndex < method.parameters().size())
                variables_[narrow(*method.parameters()[nameIndex++])] = slot;
            slot += static_cast<ca::i32>(jbc::type::slotWidth(parameter));
        }
        nextVariable_ = slot;
    }

    void compile()
    {
        unit_->dom.beginDeferredTargets();
        if (method_.code()) {
            for (const auto& item : method_.code()->instructions()) emit(item);
        }
        for (const auto& value : method_.exceptions()) {
            IR::CodeUnit::Exc entry;
            entry.start   = label(*value->start());
            entry.end     = label(*value->end());
            entry.handler = label(*value->handler());
            entry.catchCp = value->exceptionType() && !equals(value->exceptionType()->content(), "*")
                                ? session_.internClass(internalNameOf(*value->exceptionType()))
                                : 0;
            unit_->exc.push_back(entry);
        }
        require(unit_->dom.endDeferredTargets(), "endDeferredTargets");
    }

    ///@brief 行号行（insn 句柄 + 行号）；lower 后由 finisher 换算成 pc 落 LNT。
    const std::vector<std::pair<IR::InsnId, ca::i32>>& lineRows() const noexcept
    {
        return lineRows_;
    }

private:
    IR::LabelId label(const ast::Identifier& value)
    {
        const std::string name  = narrow(value);
        auto              found = labels_.find(name);
        if (found != labels_.end()) return found->second;
        auto result = require(unit_->dom.createLabel(), "createLabel");
        labels_.emplace(name, result);
        return result;
    }

    ca::i32 variable(const ast::ElementPtr& value, bool wide)
    {
        if (value->getType() == ast::ElementKind::NUMBER)
            return static_cast<ca::i32>(integerValue(*value));
        const std::string name  = narrow(*value);
        auto              found = variables_.find(name);
        if (found != variables_.end()) return found->second;
        const ca::i32 result = nextVariable_;
        variables_.emplace(name, result);
        nextVariable_ += wide ? 2 : 1;
        return result;
    }

    void emit(const ast::InstructionPtr& item)
    {
        if (item->getType() == ast::ElementKind::LABEL) {
            pendingLabels_.push_back(label(*item->identifier()));
            return;
        }
        const auto name = item->identifier()->content();
        if (equals(name, "line")) {
            pendingLines_.push_back(static_cast<ca::i32>(integerValue(*item->arguments()[0])));
            return;
        }
        append(insnFor(name, item->arguments()));
    }

    void append(IR::Insn in)
    {
        const auto id = require(unit_->dom.append(std::move(in)), "append");
        for (const auto& label : pendingLabels_)
            require(unit_->dom.bindLabel(label, id), "bindLabel");
        pendingLabels_.clear();
        for (const auto& line : pendingLines_) lineRows_.emplace_back(id, line);
        pendingLines_.clear();
    }

    IR::Insn insnFor(ca::str::Utf16StringRef name, const std::vector<ast::ElementPtr>& args)
    {
        const auto op  = opcodeUpper(name);
        const auto low = [name](const char* text) { return equals(name, text); };
        IR::Insn    in;
        in.op = op;
        if (low("bipush") || low("sipush")) {
            in.a = static_cast<ca::i32>(integerValue(*args[0]));
        }
        else if (low("ldc") || low("ldc_w") || low("ldc2_w")) {
            const ConstSpec constant = constantValue(args[0], arena_);
            const ca::u32   index    = internConstant(session_, constant);
            //宽常量（J/D）恒 LDC2_W；索引超 u1 域自动升格 LDC_W（loadConst 口径）。
            in.op  = constant.kind == ConstSpec::Kind::Long ||
                            constant.kind == ConstSpec::Kind::Double
                        ? C::Op::LDC2_W
                        : (index <= 0xFF ? C::Op::LDC : C::Op::LDC_W);
            in.cp  = index;
        }
        else if (low("newarray")) {
            const auto type = args[0]->content();
            if (equals(type, "boolean"))
                in.a = T_BOOLEAN;
            else if (equals(type, "char"))
                in.a = T_CHAR;
            else if (equals(type, "float"))
                in.a = T_FLOAT;
            else if (equals(type, "double"))
                in.a = T_DOUBLE;
            else if (equals(type, "byte"))
                in.a = T_BYTE;
            else if (equals(type, "short"))
                in.a = T_SHORT;
            else if (equals(type, "int"))
                in.a = T_INT;
            else if (equals(type, "long"))
                in.a = T_LONG;
            else
                fail("Unsupported NEWARRAY operand");
        }
        else if (low("new") || low("anewarray") || low("checkcast") || low("instanceof")) {
            const auto typeName = internalNameOf(
                *std::static_pointer_cast<ast::Identifier>(args[0]));
            if (low("new") || low("checkcast") || low("instanceof")) {
                in.cp = session_.internClass(typeName);
            }
            else {
                const auto component =
                    typeName.front() == '[' ? typeName : "L" + typeName + ";";
                in.cp = session_.internClass("[" + component);
            }
        }
        else if (isVarOpcode(op)) {
            const bool wide = op == C::Op::LLOAD || op == C::Op::DLOAD || op == C::Op::LSTORE ||
                              op == C::Op::DSTORE;
            in.a    = variable(args[0], wide);
            in.wide = in.a > 255;
        }
        else if (low("iinc")) {
            const ca::i32 index    = variable(args[0], false);
            const ca::i32 constant = static_cast<ca::i32>(integerValue(*args[1]));
            in.a    = index;
            in.b    = constant;
            in.wide = index > 255 || constant < -128 || constant > 127;
        }
        else if (isJumpOpcode(op)) {
            in.target = label(*std::static_pointer_cast<ast::Identifier>(args[0]));
        }
        else if (low("tableswitch")) {
            const auto& object = *std::static_pointer_cast<ast::Object>(args[0]);
            auto minKey     = ca::str::Utf16String::value_of("min");
            auto defaultKey = ca::str::Utf16String::value_of("default");
            auto casesKey   = ca::str::Utf16String::value_of("cases");
            in.op      = C::Op::TABLESWITCH;
            in.a       = static_cast<ca::i32>(integerValue(*object.value(minKey.ref())));
            in.target  = label(*std::static_pointer_cast<ast::Identifier>(object.value(defaultKey.ref())));
            if (const auto& cases = object.value(casesKey.ref());
                cases->getType() == ast::ElementKind::ARRAY) {
                for (const auto& value : std::static_pointer_cast<ast::Array>(cases)->values())
                    in.targets.push_back(
                        label(*std::static_pointer_cast<ast::Identifier>(value)));
            }
            //CodeDom 不变量：b = max，目标数必须恰为 b - a + 1。
            in.b = in.a + static_cast<ca::i32>(in.targets.size()) - 1;
        }
        else if (low("lookupswitch")) {
            const auto& object = *std::static_pointer_cast<ast::Object>(args[0]);
            auto defaultKey = ca::str::Utf16String::value_of("default");
            std::vector<std::pair<ca::i32, IR::LabelId>> pairs;
            for (const auto& entry : object.values()) {
                if (equals(entry.first->content(), "default")) continue;
                pairs.emplace_back(static_cast<ca::i32>(integerValue(*entry.first)),
                                   label(*std::static_pointer_cast<ast::Identifier>(entry.second)));
            }
            std::sort(pairs.begin(), pairs.end(),
                      [](const auto& left, const auto& right) { return left.first < right.first; });
            in.op     = C::Op::LOOKUPSWITCH;
            in.target = label(
                *std::static_pointer_cast<ast::Identifier>(object.value(defaultKey.ref())));
            for (auto& pair : pairs) {
                in.keys.push_back(pair.first);
                in.targets.push_back(pair.second);
            }
        }
        else if (op >= C::Op::GETSTATIC && op <= C::Op::PUTFIELD) {
            const std::string path      = narrow(*args[0]);
            const auto        separator = path.rfind('.');
            if (separator == std::string::npos) fail("Field path has no name");
            const auto descriptor = narrow(*args[1]);
            if (jbc::type::parseFieldType(descriptor).is_err()) fail("Invalid field descriptor");
            in.cp = session_.internFieldRef(path.substr(0, separator),
                                            path.substr(separator + 1), descriptor);
        }
        else if (op >= C::Op::INVOKEVIRTUAL && op <= C::Op::INVOKEINTERFACE) {
            const std::string path      = methodPath(args);
            const auto        separator = path.rfind('.');
            if (separator == std::string::npos) fail("Method path has no name");
            const auto descriptor = narrow(*args[1]);
            if (jbc::type::parseMethodDescriptor(descriptor).is_err())
                fail("Invalid method descriptor");
            const bool isInterface =
                low("invokeinterface") || low("invokevirtualinterface") ||
                low("invokestaticinterface") || low("invokespecialinterface");
            const std::string owner = path.substr(0, separator);
            const std::string name2 = path.substr(separator + 1);
            in.cp = isInterface ? session_.internInterfaceMethodRef(owner, name2, descriptor)
                                : session_.internMethodRef(owner, name2, descriptor);
        }
        else if (low("invokedynamic")) {
            std::vector<IR::BsmArg> bootstrapArgs;
            if (args[3]->getType() == ast::ElementKind::ARRAY) {
                for (const auto& value : std::static_pointer_cast<ast::Array>(args[3])->values())
                    bootstrapArgs.push_back(bsmArgOf(session_, constantValue(value, arena_)));
            }
            const auto descriptor = narrow(*args[1]);
            if (jbc::type::parseMethodDescriptor(descriptor).is_err())
                fail("Invalid invokedynamic descriptor");
            const auto bsm = require(session_.appendBootstrapMethod(methodHandle(args[2], arena_),
                                                                    bootstrapArgs),
                                     "appendBootstrapMethod");
            const auto nat = session_.backing().cp.addNameAndType(
                session_.internUtf8(narrow(*args[0])), session_.internUtf8(descriptor));
            in.op = C::Op::INVOKEDYNAMIC;
            in.cp = session_.backing().cp.addInvokeDynamic(bsm, nat);
        }
        else if (low("multianewarray")) {
            const auto descriptor = narrow(*args[0]);
            if (jbc::type::parseFieldType(descriptor).is_err())
                fail("Invalid array descriptor");
            in.a  = static_cast<ca::i32>(integerValue(*args[1]));
            in.cp = session_.internClass(descriptor);
        }
        return in;
    }

    static C::Op opcodeUpper(ca::str::Utf16StringRef name)
    {
        std::string text = name.to_utf8_string().to_std_string();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        auto upper = ca::str::Utf16String::value_of(text.c_str());
        return opcode(upper.ref());
    }

    static bool isVarOpcode(C::Op op) noexcept
    {
        return (op >= C::Op::ILOAD && op <= C::Op::ALOAD) ||
               (op >= C::Op::ISTORE && op <= C::Op::ASTORE) || op == C::Op::RET;
    }

    static bool isJumpOpcode(C::Op op) noexcept
    {
        return (op >= C::Op::IFEQ && op <= C::Op::JSR) || op == C::Op::IFNULL ||
               op == C::Op::IFNONNULL || op == C::Op::GOTO_W || op == C::Op::JSR_W;
    }

    std::string methodPath(const std::vector<ast::ElementPtr>& args) const
    {
        return narrow(*args[0]);
    }

    IR::ClassSession& session_;
    IR::CodeUnitRef   unit_;
    const ast::Method& method_;
    SpecArena          arena_;
    std::unordered_map<std::string, IR::LabelId> labels_;
    std::unordered_map<std::string, ca::i32>      variables_;
    std::vector<IR::LabelId>                      pendingLabels_;
    std::vector<ca::i32>                          pendingLines_;
    std::vector<std::pair<IR::InsnId, ca::i32>>   lineRows_;
    ca::i32                                       nextVariable_ = 0;
};

///@brief InheritanceChecker → Strict 帧引擎（编译侧 SMT 生成用；与 analyzer 同构）。
class InheritanceResolver final : public jbc::analysis::ITypeResolver
{
public:
    explicit InheritanceResolver(const analysis::InheritanceChecker& inheritance) noexcept
        : inheritance_(&inheritance)
    {}

    ca::core::Result<jbc::analysis::VType, jbc::analysis::TypeMergeError> merge(
        jbc::analysis::VType first, jbc::analysis::VType second) override
    {
        using jbc::analysis::TypeMergeError;
        using jbc::analysis::VType;
        if (!first.name.empty() && first.name == second.name)
            return ca::core::Ok(VType::ref(first.name));
        if (first.name.empty() || second.name.empty())
            return ca::core::Err(TypeMergeError::Unresolvable);
        const auto common = inheritance_->commonSuperclass(
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(first.name.data()),
                                              first.name.size()),
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(second.name.data()),
                                              second.name.size()));
        owned_.emplace_back(common.to_std_string());
        return ca::core::Ok(VType::ref(std::string_view(owned_.back())));
    }

private:
    const analysis::InheritanceChecker* inheritance_;
    std::deque<std::string>             owned_;
};

///@brief Strict 帧计算 → SMT 生成 → maxes 采用 → lower → SMT/LNT 挂回。
///
///全新方法体没有现成 pc：先 lower 一轮让 DOM 拿到真实 srcPc（帧引擎的
///Uninitialized 载荷与 SMT 的目标/偏移都以 pc 计），再计算帧与生成 SMT，
///最后第二轮 lower 只为提交 maxes（字节不变 → 走 maxesChanged 快路径）。
void finishBody(IR::ClassSession& session, IR::MethodId method, IR::CodeUnit& unit,
                const std::vector<std::pair<IR::InsnId, ca::i32>>& lineRows,
                const JvmCompilerOptions& options, const std::string& name,
                const std::string& descriptor, bool isStatic)
{
    C::ClassFile& cf = session.backing();

    //Return 族指令与描述符返回类别一致性（JVMS §4.10.1.4 returnTypeMatching；
    //jasmin 同款装配期拒绝）：()I 方法体写 `return`、`()V` 写 `ireturn` 这类
    //源级错配在帧分析引擎无感（引擎只对栈做形状/宽度检查），在此按描述符
    //返回类型逐一比对方法体内全部 xRETURN 指令。
    {
        const auto parsed = jbc::type::parseMethodDescriptor(descriptor);
        char        expect = 'V';
        if (parsed.is_ok()) {
            const auto& md = parsed.unwrap();
            if (md.ret_void) {
                expect = 'V';
            }
            else if (md.ret.kind == jbc::type::TypeDesc::Kind::Class) {
                expect = 'A';   // 引用（含数组：class_name + array_dims）
            }
            else {
                switch (md.ret.primitive) {
                case jbc::type::BaseType::Long: expect = 'J'; break;
                case jbc::type::BaseType::Float: expect = 'F'; break;
                case jbc::type::BaseType::Double: expect = 'D'; break;
                default: expect = 'I'; break;   // B/C/S/Z/I 同 IRETURN 装载类别
                }
            }
        }
        const auto returns = [&](C::Op op) {
            switch (op) {
            case C::Op::IRETURN: return 'I';
            case C::Op::LRETURN: return 'J';
            case C::Op::FRETURN: return 'F';
            case C::Op::DRETURN: return 'D';
            case C::Op::ARETURN: return 'A';
            case C::Op::RETURN: return 'V';
            default: return '\0';
            }
        };
        for (const auto id : unit.dom.order()) {
            const auto* insn = unit.dom.peek(id);
            if (insn == nullptr) continue;
            const char got = returns(insn->op);
            if (got != '\0' && got != expect)
                fail("Return opcode does not match method descriptor return type: " + name +
                     descriptor);
        }
    }

    require(IR::lowerMethod(session, method), "lowerMethod");

    jbc::analysis::MethodSig sig;
    sig.isStatic  = isStatic;
    sig.desc      = descriptor;
    sig.name      = name;
    sig.thisClass = cf.thisName();

    jbc::analysis::ShapeResolver shape;
    std::optional<InheritanceResolver> hierarchy;
    if (options.inheritance != nullptr) hierarchy.emplace(*options.inheritance);
    jbc::analysis::ITypeResolver& resolver =
        hierarchy.has_value() ? static_cast<jbc::analysis::ITypeResolver&>(*hierarchy)
                              : static_cast<jbc::analysis::ITypeResolver&>(shape);
    jbc::analysis::FrameEngine          engine(cf.cp, resolver, jbc::analysis::FramePolicy::Strict);
    std::vector<jbc::analysis::FrameAt> frames;
    const auto summary = engine.compute(unit, sig, frames);
    if (!summary.ok)
        fail("Stack frame analysis failed for method " + name + ": " +
             jbc::analysis::to_string(summary.fail));
    if (!jbc::analysis::applyComputedMaxes(unit, summary, sig))
        fail("Stack frame maxes adoption failed for method " + name);

    auto built = jbc::analysis::SmtBuildResult{};
    if (cf.major >= 50) {
        auto generated = jbc::analysis::buildStackMapTable(session, unit, frames);
        if (generated.is_err())
            fail("StackMapTable generation failed for method " + name + ": " +
                 jbc::analysis::to_string(generated.unwrap_err()));
        built = std::move(generated).unwrap();
    }

    require(IR::lowerMethod(session, method), "lowerMethod");

    //挂回（lower 后重新定位 Code 槽；与 libjbc J7 管线同序）。
    const auto index = session.unit().indexOf(method);
    if (index == IR::kNoMethod) fail("Method lost after lower");
    C::MemberInfo& m = cf.methods[session.unit().methods[index].backingIndex];
    C::CodeAttr*  code = nullptr;
    for (auto& slot : m.attrs) {
        const auto decoded = cf.decodeAttr(slot);
        if (decoded.is_ok() && slot.peek() != nullptr &&
            slot.peek()->kind() == C::AttrKind::Code) {
            code = static_cast<C::CodeAttr*>(slot.mutate());
            break;
        }
    }
    if (code == nullptr) fail("Code attribute lost after lower");

    if (built.needed) {
        auto nameCp = cf.cp.findUtf8("StackMapTable");
        if (nameCp == 0) nameCp = cf.cp.addUtf8("StackMapTable");
        code->attrs.push_back(C::AttrSlot::makeNew(nameCp, std::move(built.attr)));
    }
    if (!lineRows.empty()) {
        auto lnt   = std::make_unique<C::LineNumberTableAttr>();
        lnt->rows.reserve(lineRows.size());
        for (const auto& [id, line] : lineRows) {
            const auto* insn = unit.dom.peek(id);
            if (insn == nullptr) continue;
            lnt->rows.push_back(
                C::LineNumberTableAttr::Row{insn->srcPc, static_cast<ca::u32>(line)});
        }
        if (!lnt->rows.empty()) {
            auto nameCp = cf.cp.findUtf8("LineNumberTable");
            if (nameCp == 0) nameCp = cf.cp.addUtf8("LineNumberTable");
            code->attrs.push_back(C::AttrSlot::makeNew(nameCp, std::move(lnt)));
        }
    }
}

std::vector<ca::u8> compileClass(const ast::Class& clazz, const JvmCompilerOptions& options)
{
    const std::string thisName = internalNameOf(*clazz.name());
    const std::string superName =
        clazz.superName() ? internalNameOf(*clazz.superName()) : std::string{};
    std::vector<std::string> interfaceNames;
    for (const auto& value : clazz.interfaces())
        interfaceNames.push_back(internalNameOf(*value));

    IR::NewClassSeed seed;
    seed.version = (static_cast<ca::u32>(options.majorVersion) << 16) | options.minorVersion;
    seed.access  = access(clazz.modifiers());
    seed.thisName = thisName;
    seed.superName = superName;
    seed.interfaces = std::vector<std::string_view>{interfaceNames.begin(), interfaceNames.end()};
    auto session = require(IR::makeSession(seed), "makeSession");

    //类级属性（挂载顺序与旧栈打印面的出现顺序对齐，方便文本对照）。
    if (auto signature = signatureAttr(*session, clazz.attributes()))
        require(session->attachClassAttr("Signature", std::move(signature)), "attachClassAttr");
    if (clazz.sourceFile()) {
        auto source   = std::make_unique<C::SourceFileAttr>();
        source->sourceFileCp = session->internUtf8(narrow(*clazz.sourceFile()));
        require(session->attachClassAttr("SourceFile", std::move(source)), "attachClassAttr");
    }
    if (clazz.outerClass()) {
        auto enclosing = std::make_unique<C::EnclosingMethodAttr>();
        enclosing->classCp = session->internClass(internalNameOf(*clazz.outerClass()));
        if (clazz.outerMethod()) {
            enclosing->methodNatCp = session->backing().cp.addNameAndType(
                session->internUtf8(narrow(*clazz.outerMethod()->name())),
                session->internUtf8(narrow(*clazz.outerMethod()->descriptor())));
        }
        require(session->attachClassAttr("EnclosingMethod", std::move(enclosing)),
                "attachClassAttr");
    }
    if (clazz.nestHost()) {
        auto nest      = std::make_unique<C::NestHostAttr>();
        nest->hostClassCp = session->internClass(internalNameOf(*clazz.nestHost()));
        require(session->attachClassAttr("NestHost", std::move(nest)), "attachClassAttr");
    }
    if (!clazz.nestMembers().empty()) {
        auto nest = std::make_unique<C::NestMembersAttr>();
        for (const auto& value : clazz.nestMembers())
            nest->memberCps.push_back(session->internClass(internalNameOf(*value)));
        require(session->attachClassAttr("NestMembers", std::move(nest)), "attachClassAttr");
    }
    if (!clazz.permittedSubclasses().empty()) {
        auto permitted = std::make_unique<C::PermittedSubclassesAttr>();
        for (const auto& value : clazz.permittedSubclasses())
            permitted->permittedCps.push_back(session->internClass(internalNameOf(*value)));
        require(session->attachClassAttr("PermittedSubclasses", std::move(permitted)),
                "attachClassAttr");
    }
    if (!clazz.innerClasses().empty()) {
        auto inner = std::make_unique<C::InnerClassesAttr>();
        for (const auto& value : clazz.innerClasses()) {
            C::InnerClassesAttr::Row row;
            row.innerClassInfoCp = session->internClass(internalNameOf(*value->innerClass()));
            row.outerClassInfoCp =
                value->outerClass() ? session->internClass(internalNameOf(*value->outerClass())) : 0;
            row.innerNameCp = value->name() ? session->internUtf8(narrow(*value->name())) : 0;
            row.accessFlags = static_cast<ca::u16>(access(value->modifiers()));
            inner->rows.push_back(row);
        }
        require(session->attachClassAttr("InnerClasses", std::move(inner)), "attachClassAttr");
    }
    if (!clazz.recordComponents().empty()) {
        auto record = std::make_unique<C::RecordAttr>();
        for (const auto& value : clazz.recordComponents()) {
            C::RecordAttr::Component component;
            component.nameCp        = session->internUtf8(narrow(*value->name()));
            component.descriptorCp  = session->internUtf8(narrow(*value->descriptor()));
            mountComponentAttrs(*session, component.attrs, value->attributes());
            record->components.push_back(std::move(component));
        }
        require(session->attachClassAttr("Record", std::move(record)), "attachClassAttr");
    }
    installAnnotations(
        *session, clazz.attributes(),
        [s = session.get()](bool visible, const std::vector<IR::SemAnnotation>& annos) {
            return s->attachClassAnnotations(visible, annos);
        },
        [s = session.get()](std::string_view name, std::unique_ptr<C::AttrBody> body) {
            return s->attachClassAttr(name, std::move(body));
        });

    for (const auto& item : clazz.contents()) {
        if (item->getType() == ast::ElementKind::FIELD) {
            const auto field = std::static_pointer_cast<ast::Field>(item);
            //Seed 名字/描述符是 string_view：先落命名串再挂，视图必须活过 addField。
            const std::string fieldName = narrow(*field->name());
            const std::string fieldDesc = narrow(*field->descriptor());
            IR::FieldSeed fieldSeed;
            fieldSeed.name   = fieldName;
            fieldSeed.desc   = fieldDesc;
            fieldSeed.access = access(field->modifiers());
            if (jbc::type::parseFieldType(fieldSeed.desc).is_err())
                fail("Invalid field descriptor");
            const auto id = require(session->addField(fieldSeed), "addField");
            if (auto signature = signatureAttr(*session, field->attributes()))
                require(session->attachAttr(id, "Signature", std::move(signature)),
                        "attachAttr");
            if (field->fieldValue()) {
                SpecArena arena;
                const auto constant = constantValue(field->fieldValue(), arena);
                IR::ConstValueSpec value;
                switch (constant.kind) {
                case ConstSpec::Kind::Int:
                    value.kind = IR::ConstValueSpec::Kind::Int;
                    value.num  = constant.num;
                    break;
                case ConstSpec::Kind::Long:
                    value.kind = IR::ConstValueSpec::Kind::Long;
                    value.num  = constant.num;
                    break;
                case ConstSpec::Kind::Float:
                    value.kind  = IR::ConstValueSpec::Kind::Float;
                    value.fbits = constant.fbits;
                    break;
                case ConstSpec::Kind::Double:
                    value.kind  = IR::ConstValueSpec::Kind::Double;
                    value.dbits = constant.dbits;
                    break;
                case ConstSpec::Kind::String:
                    value.kind = IR::ConstValueSpec::Kind::String;
                    value.str  = constant.str;
                    break;
                default: fail("Unsupported field constant kind");
                }
                require(session->attachConstantValue(id, value), "attachConstantValue");
            }
            installAnnotations(
                *session, field->attributes(),
                [s = session.get(), id](bool visible, const std::vector<IR::SemAnnotation>& annos) {
                    return s->attachAnnotations(id, visible, annos);
                },
                [s = session.get(), id](std::string_view name, std::unique_ptr<C::AttrBody> body) {
                    return s->attachAttr(id, name, std::move(body));
                });
        }
        else if (item->getType() == ast::ElementKind::METHOD) {
            const auto method = std::static_pointer_cast<ast::Method>(item);
            std::string name = narrow(*method->name());
            std::string descriptor = narrow(*method->descriptor());
            if (jbc::type::parseMethodDescriptor(descriptor).is_err())
                fail("Invalid method descriptor");
            const bool isStatic = (access(method->modifiers()) & ACC_STATIC) != 0;
            const bool withCode = method->code() != nullptr;

            IR::MethodSeed methodSeed;
            methodSeed.name   = name;
            methodSeed.desc   = descriptor;
            methodSeed.access = access(method->modifiers());
            methodSeed.withCode = withCode;
            methodSeed.maxLocals = static_cast<ca::u16>(
                paramSlotTotal(descriptor, isStatic));
            const auto id = require(session->addMethod(methodSeed), "addMethod");

            if (auto signature = signatureAttr(*session, method->attributes()))
                require(session->attachAttr(id, "Signature", std::move(signature)), "attachAttr");
            installAnnotations(
                *session, method->attributes(),
                [s = session.get(), id](bool visible, const std::vector<IR::SemAnnotation>& annos) {
                    return s->attachAnnotations(id, visible, annos);
                },
                [s = session.get(), id](std::string_view name, std::unique_ptr<C::AttrBody> body) {
                    return s->attachAttr(id, name, std::move(body));
                });
            if (!method->parameters().empty()) {
                std::vector<IR::ParamNameRow> rows;
                rows.reserve(method->parameters().size());
                std::vector<std::string> parameterNames;
                parameterNames.reserve(method->parameters().size());
                for (const auto& parameter : method->parameters()) {
                    //ParamNameRow.name 是 string_view：先落驻留串再挂行。
                    parameterNames.push_back(narrow(*parameter));
                    rows.push_back(IR::ParamNameRow{parameterNames.back(), 0});
                }
                require(session->attachMethodParameters(id, rows), "attachMethodParameters");
            }
            if (method->defaultValue()) {
                SpecArena arena;
                require(session->attachAnnotationDefault(id, semElement(method->defaultValue(),
                                                                        arena)),
                        "attachAnnotationDefault");
            }

            if (withCode) {
                auto promoted = IR::promoteMethod(*session, id);
                if (promoted.is_err()) fail("Method body promote failed");
                IR::CodeUnitRef unit = std::move(promoted).unwrap();
                CodeCompiler compiler(*session, unit, *method, descriptor);
                compiler.compile();
                finishBody(*session, id, *unit, compiler.lineRows(), options, name, descriptor,
                           isStatic);
            }
        }
    }

    auto bytes = require(C::writeClass(session->backing()), "writeClass");
    return bytes;
}
}   //namespace

error::Result<JvmCompileResult> JvmCompiler::compile(
    const std::vector<ast::ElementPtr>& input) const
{
    const ast::Class* clazz = nullptr;
    for (const auto& item : input) {
        if (item && item->getType() == ast::ElementKind::CLASS) {
            if (clazz) {
                auto message =
                    ca::str::Utf16String::value_of("Expected exactly one class declaration");
                return error::Result<JvmCompileResult>::err({error::Error(message.ref())});
            }
            clazz = static_cast<const ast::Class*>(item.get());
        }
    }
    if (!clazz) {
        auto message = ca::str::Utf16String::value_of("Expected a class declaration");
        return error::Result<JvmCompileResult>::err({error::Error(message.ref())});
    }
    return compile(*clazz);
}

error::Result<JvmCompileResult> JvmCompiler::compile(const ast::Class& clazz) const
{
    try {
        auto bytes = compileClass(clazz, options_);
        return error::Result<JvmCompileResult>::ok({std::move(bytes)});
    }
    catch (const std::exception& exception) {
        auto                      message  = diagnosticText(exception.what());
        const util::Location*     location = clazz.location();
        std::vector<error::Error> errors;
        if (location)
            errors.emplace_back(message.ref(), *location);
        else
            errors.emplace_back(message.ref());
        return error::Result<JvmCompileResult>::err(std::move(errors));
    }
}

}   //namespace jhasm::compile
