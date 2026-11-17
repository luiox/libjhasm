#include "code_compiler.hpp"

#include "jvm_constants.hpp"
#include "jvm_opcodes.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace jhasm::compile {

namespace {

/// @brief 局部变量装载/存储的 _N 短格式（各族 _0.._3 连续；RET 无短格式）。
C::Op varShortForm(C::Op op, ca::i32 slot)
{
    C::Op base;
    switch (op) {
    case C::Op::ILOAD:  base = C::Op::ILOAD_0;  break;
    case C::Op::LLOAD:  base = C::Op::LLOAD_0;  break;
    case C::Op::FLOAD:  base = C::Op::FLOAD_0;  break;
    case C::Op::DLOAD:  base = C::Op::DLOAD_0;  break;
    case C::Op::ALOAD:  base = C::Op::ALOAD_0;  break;
    case C::Op::ISTORE: base = C::Op::ISTORE_0; break;
    case C::Op::LSTORE: base = C::Op::LSTORE_0; break;
    case C::Op::FSTORE: base = C::Op::FSTORE_0; break;
    case C::Op::DSTORE: base = C::Op::DSTORE_0; break;
    default:            base = C::Op::ASTORE_0; break;
    }
    return static_cast<C::Op>(static_cast<ca::i32>(base) + slot);
}

ca::i64 checkedInteger(const ast::Element& value, ca::i64 min, ca::i64 max, const char* role)
{
    auto text = narrow(value);
    if (!text.empty() && (text.back() == 'l' || text.back() == 'L')) text.pop_back();
    const auto prefix = !text.empty() && (text.front() == '-' || text.front() == '+') ? 1u : 0u;
    const bool hex = text.size() > prefix + 2 && text[prefix] == '0' &&
                     (text[prefix + 1] == 'x' || text[prefix + 1] == 'X');
    if (hex) text.erase(prefix, 2);
    const char* begin = text.data();
    if (!text.empty() && text.front() == '+') ++begin;
    const char* end = text.data() + text.size();
    ca::i64 result = 0;
    const auto parsed = std::from_chars(begin, end, result, hex ? 16 : 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result < min || result > max)
        fail(std::string(role) + " must be an integer in [" + std::to_string(min) + ", " +
             std::to_string(max) + "]");
    return result;
}

IR::Insn rawConstantLoad(IR::ClassSession& session, C::Op op, const ConstSpec& value, bool verify)
{
    if (verify) {
        const bool wideValue = value.kind == ConstSpec::Kind::Long ||
                               value.kind == ConstSpec::Kind::Double;
        const bool wideDynamic = value.kind == ConstSpec::Kind::Dynamic &&
                                 (value.dynamicDesc == "J" || value.dynamicDesc == "D");
        if (op == C::Op::LDC2_W && !wideValue)
            fail("ldc2_w requires a long or double constant");
        if (op != C::Op::LDC2_W && (wideValue || wideDynamic))
            fail(std::string(C::mnemonicOf(op)) + " requires a category-1 constant");
    }
    IR::Insn in;
    in.op = op;
    in.cp = internConstant(session, value);
    if (op == C::Op::LDC && in.cp > 255)
        fail("ldc constant pool index " + std::to_string(in.cp) + " exceeds 255; use ldc_w");
    return in;
}

struct LocalUse final
{
    ca::u32 index;
    ca::u32 width;
};

std::optional<LocalUse> localUse(const IR::Insn& in)
{
    using C::Op;
    const auto op = in.op;
    const auto shape = C::shapeOf(op);
    if (shape == C::Shape::Local1 || shape == C::Shape::Iinc1) {
        const auto width = op == Op::LLOAD || op == Op::DLOAD ||
                           op == Op::LSTORE || op == Op::DSTORE;
        return LocalUse{static_cast<ca::u32>(in.a), width ? 2u : 1u};
    }
    if ((op >= Op::ILOAD_0 && op <= Op::ALOAD_3) ||
        (op >= Op::ISTORE_0 && op <= Op::ASTORE_3)) {
        const auto base = op <= Op::ALOAD_3 ? Op::ILOAD_0 : Op::ISTORE_0;
        const auto offset = static_cast<ca::u32>(op) - static_cast<ca::u32>(base);
        const auto group = offset / 4;
        return LocalUse{offset % 4, group == 1 || group == 3 ? 2u : 1u};
    }
    return std::nullopt;
}

ca::u32 requiredLocals(const IR::CodeUnit& unit, const std::string& descriptor, bool isStatic)
{
    auto required = static_cast<ca::u32>(paramSlotTotal(descriptor, isStatic));
    for (const auto id : unit.dom.order()) {
        if (const auto use = localUse(*unit.dom.peek(id)))
            required = std::max(required, use->index + use->width);
    }
    return required;
}

struct StackEffect final
{
    ca::u32 consumed;
    ca::u32 produced;
};

std::optional<StackEffect> knownStackEffect(const IR::Insn& in)
{
    using C::Op;
    const auto op = in.op;
    if (op == Op::NOP || op == Op::IINC) return StackEffect{0, 0};
    if (op >= Op::ACONST_NULL && op <= Op::LDC2_W) {
        const bool wide = (op >= Op::LCONST_0 && op <= Op::LCONST_1) ||
                          (op >= Op::DCONST_0 && op <= Op::DCONST_1) || op == Op::LDC2_W;
        return StackEffect{0, wide ? 2u : 1u};
    }
    if (op >= Op::ILOAD && op <= Op::ALOAD_3)
        return StackEffect{0, localUse(in)->width};
    if (op >= Op::ISTORE && op <= Op::ASTORE_3)
        return StackEffect{localUse(in)->width, 0};
    if (op >= Op::IADD && op <= Op::DREM) {
        // 加、减、乘、除、余数的 opcode 都按 int、long、float、double 排列。
        const ca::u32 width = (static_cast<ca::u8>(op) - static_cast<ca::u8>(Op::IADD)) % 2 + 1;
        return StackEffect{2 * width, width};
    }
    switch (op) {
    case Op::POP: return StackEffect{1, 0};
    case Op::POP2: return StackEffect{2, 0};
    case Op::DUP: return StackEffect{1, 2};
    case Op::DUP_X1: return StackEffect{2, 3};
    case Op::DUP_X2: return StackEffect{3, 4};
    case Op::DUP2: return StackEffect{2, 4};
    case Op::DUP2_X1: return StackEffect{3, 5};
    case Op::DUP2_X2: return StackEffect{4, 6};
    case Op::SWAP: return StackEffect{2, 2};
    case Op::NEW: return StackEffect{0, 1};
    case Op::NEWARRAY:
    case Op::ANEWARRAY:
    case Op::ARRAYLENGTH:
    case Op::CHECKCAST:
    case Op::INSTANCEOF: return StackEffect{1, 1};
    default: return std::nullopt;
    }
}

ca::u32 stackLowerBound(const IR::CodeUnit& unit)
{
    // 从入口沿确定的前缀计数。遇到分支、未知栈效果或下溢就停止，不作帧推断。
    ca::u32 depth = 0;
    ca::u32 bound = unit.exc.empty() ? 0 : 1;
    for (const auto id : unit.dom.order()) {
        const auto effect = knownStackEffect(*unit.dom.peek(id));
        if (!effect || depth < effect->consumed) break;
        depth = depth - effect->consumed + effect->produced;
        bound = std::max(bound, depth);
    }
    return bound;
}

void warnMaxs(std::vector<error::Warn>& warnings, const char* name, ca::u16 supplied, ca::u32 required)
{
    if (supplied >= required) return;
    const auto& scope = diagScope();
    std::string message = std::string("maxs ") + name + " " + std::to_string(supplied) +
                          " is below the required minimum " + std::to_string(required);
    if (!scope.member.empty()) message += " (in " + scope.member + ")";
    const auto text = ca::str::Utf16String::value_of(message.c_str());
    if (scope.location) warnings.emplace_back(text.ref(), *scope.location);
    else warnings.emplace_back(text.ref());
}

void adoptRawMaxs(IR::CodeUnit& unit, const RawMaxs& maxs, ca::u32 stack, ca::u32 locals,
                  std::vector<error::Warn>& warnings)
{
    warnMaxs(warnings, "stack", maxs.stack, stack);
    warnMaxs(warnings, "locals", maxs.locals, locals);
    unit.maxStack = maxs.stack;
    unit.maxLocals = maxs.locals;
}

C::AttrSlot& codeSlot(IR::ClassSession& session, IR::MethodId method)
{
    const auto* entry = session.unit().method(method);
    auto& cf = session.backing();
    if (!entry || entry->backingIndex >= cf.methods.size()) fail("Method lost during lowering");
    for (auto& slot : cf.methods[entry->backingIndex].attrs) {
        const auto decoded = cf.decodeAttr(slot);
        if (decoded.is_ok() && slot.peek() && slot.peek()->kind() == C::AttrKind::Code)
            return slot;
    }
    fail("Code attribute lost during lowering");
}

void writeEmptyCode(IR::ClassSession& session, IR::MethodId method, const IR::CodeUnit& unit)
{
    if (!unit.exc.empty()) fail("Empty code cannot have exception targets");
    require(session.commitMethodMetadata(method), "commitMethodMetadata");
    auto& slot = codeSlot(session, method);
    // libjbc 的 Code 编码器拒绝零长度。仅在 R 关闭 verify 时直接写该属性。
    C::ByteSink bytes;
    bytes.u16(unit.maxStack);
    bytes.u16(unit.maxLocals);
    bytes.u32(0); // code_length
    bytes.u16(0); // exception_table_length
    bytes.u16(0); // attributes_count
    slot = C::AttrSlot::makeNew(slot.nameCp, std::make_unique<C::RawAttr>(std::move(bytes.bytes())));
}

/// @brief InheritanceChecker → Strict 帧引擎（编译侧 SMT 生成用；与 analyzer 同构）。
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

}   //namespace

CodeCompiler::CodeCompiler(IR::ClassSession& session, IR::CodeUnitRef unit,
                           const ast::Method& method, const std::string& descriptor,
                           JvmDialect dialect, bool verify)
    : session_(session)
    , unit_(std::move(unit))
    , method_(method)
    , dialect_(dialect)
    , verify_(verify)
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

void CodeCompiler::compile()
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

IR::LabelId CodeCompiler::label(const ast::Identifier& value)
{
    const std::string name  = narrow(value);
    auto              found = labels_.find(name);
    if (found != labels_.end()) return found->second;
    auto result = require(unit_->dom.createLabel(), "createLabel");
    labels_.emplace(name, result);
    return result;
}

ca::i32 CodeCompiler::variable(const ast::ElementPtr& value, bool wide)
{
    if (value->getType() == ast::ElementKind::NUMBER) {
        const auto index = dialect_ == JvmDialect::R
                               ? checkedInteger(*value, 0, 65535, "Local variable index")
                               : integerValue(*value);
        return static_cast<ca::i32>(index);
    }
    const std::string name  = narrow(*value);
    auto              found = variables_.find(name);
    if (found != variables_.end()) return found->second;
    const ca::i32 result = nextVariable_;
    if (dialect_ == JvmDialect::R && result > 65535) fail("Local variable index exceeds 65535");
    variables_.emplace(name, result);
    nextVariable_ += wide ? 2 : 1;
    return result;
}

void CodeCompiler::emit(const ast::InstructionPtr& item)
{
    //指令级诊断位置：成员上下文之上细化到当前指令（fail/顶层 catch 读取）。
    if (const auto* location = item->location()) diagScope().location = location;
    if (item->getType() == ast::ElementKind::LABEL) {
        pendingLabels_.push_back(label(*item->identifier()));
        return;
    }
    const auto name = item->identifier()->content();
    if (equals(name, "line")) {
        const auto& value = *item->arguments()[0];
        const auto line = dialect_ == JvmDialect::R ? checkedInteger(value, 0, 65535, "line number")
                                                    : integerValue(value);
        pendingLines_.push_back(static_cast<ca::i32>(line));
        return;
    }
    const auto& args = item->arguments();
    if (equals(name, "wide")) {
        if (dialect_ != JvmDialect::R) fail("Explicit wide requires the r dialect");
        if (args.empty()) fail("wide requires a local variable instruction or iinc");
        const auto modified = opcode(args[0]->content(), dialect_);
        if (!C::isWideLocalOp(modified))
            fail("wide requires a local variable instruction or iinc");
        if (args.size() != (modified == C::Op::IINC ? 3u : 2u))
            fail("Unexpected wide instruction operand count");
        const std::vector<ast::ElementPtr> operands(args.begin() + 1, args.end());
        append(insnFor(args[0]->content(), operands, true));
    }
    else {
        append(insnFor(name, args));
    }
}

void CodeCompiler::append(IR::Insn in)
{
    const auto id = require(unit_->dom.append(std::move(in)), "append");
    for (const auto& label : pendingLabels_)
        require(unit_->dom.bindLabel(label, id), "bindLabel");
    pendingLabels_.clear();
    for (const auto& line : pendingLines_) lineRows_.emplace_back(id, line);
    pendingLines_.clear();
}

IR::Insn CodeCompiler::insnFor(ca::str::Utf16StringRef name,
                              const std::vector<ast::ElementPtr>& args, bool explicitWide)
{
    const auto op  = opcode(name, dialect_);
    const auto low = [name](const char* text) { return equals(name, text); };
    const bool raw = dialect_ == JvmDialect::R;
    IR::Insn    in;
    in.op = op;
    if (op == C::Op::BIPUSH || op == C::Op::SIPUSH) {
        if (raw) {
            const bool isByte = op == C::Op::BIPUSH;
            in.a = static_cast<ca::i32>(checkedInteger(*args[0], isByte ? -128 : -32768,
                                                       isByte ? 127 : 32767, C::mnemonicOf(op)));
        }
        else {
            ConstSpec spec;
            spec.kind = ConstSpec::Kind::Int;
            spec.num  = integerValue(*args[0]);
            in        = constantLoad(session_, spec);
        }
    }
    else if (op == C::Op::LDC || op == C::Op::LDC_W || op == C::Op::LDC2_W) {
        const auto value = constantValue(args[0], arena_);
        in = raw ? rawConstantLoad(session_, op, value, verify_) : constantLoad(session_, value);
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
        // JVMS 6.5 anewarray：CP Class 载组件类型（javac 语料同形，
        // `anewarray java/lang/String` 落 `java/lang/String`）。旧栈
        // TypeInsnNode.desc 口径落的是数组描述符，运行时会多出一维，
        // 本仓不再沿用；disassemble 面对两种形态都按原样打印。
        in.cp = session_.internClass(typeName);
    }
    else if (isVarOpcode(op)) {
        const bool wide = op == C::Op::LLOAD || op == C::Op::DLOAD || op == C::Op::LSTORE ||
                          op == C::Op::DSTORE;
        in.a    = variable(args[0], wide);
        in.wide = explicitWide || in.a > 255;
        if (!raw && in.a <= 3 && op != C::Op::RET) {
            //槽 0..3 一律落 _N 短格式（jblw 写出口径）。
            in.op = varShortForm(op, static_cast<ca::i32>(in.a));
        }
    }
    else if (low("iinc")) {
        const ca::i32 index    = variable(args[0], false);
        const auto constant = raw ? checkedInteger(*args[1], -32768, 32767, "iinc increment")
                                  : integerValue(*args[1]);
        in.a    = index;
        in.b    = static_cast<ca::i32>(constant);
        in.wide = explicitWide || index > 255 || in.b < -128 || in.b > 127;
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
        const auto min = integerValue(*object.value(minKey.ref()));
        if (min < std::numeric_limits<ca::i32>::min() || min > std::numeric_limits<ca::i32>::max())
            fail("tableswitch min is outside the signed 32-bit range");
        in.a = static_cast<ca::i32>(min);
        in.target  = label(*std::static_pointer_cast<ast::Identifier>(object.value(defaultKey.ref())));
        if (const auto& cases = object.value(casesKey.ref());
            cases->getType() == ast::ElementKind::ARRAY) {
            for (const auto& value : std::static_pointer_cast<ast::Array>(cases)->values()) {
                if (!value || value->getType() != ast::ElementKind::IDENTIFIER)
                    fail("tableswitch cases must be labels");
                in.targets.push_back(
                    label(*std::static_pointer_cast<ast::Identifier>(value)));
            }
        }
        //CodeDom 不变量：b = max，目标数必须恰为 b - a + 1。
        const auto max = static_cast<ca::i64>(in.a) + static_cast<ca::i64>(in.targets.size()) - 1;
        if (max < std::numeric_limits<ca::i32>::min() || max > std::numeric_limits<ca::i32>::max())
            fail("tableswitch high is outside the signed 32-bit range");
        in.b = static_cast<ca::i32>(max);
    }
    else if (low("lookupswitch")) {
        const auto& object = *std::static_pointer_cast<ast::Object>(args[0]);
        auto defaultKey = ca::str::Utf16String::value_of("default");
        std::vector<std::pair<ca::i32, IR::LabelId>> pairs;
        for (const auto& entry : object.values()) {
            if (equals(entry.first->content(), "default")) continue;
            const auto key = raw
                                 ? checkedInteger(*entry.first, std::numeric_limits<ca::i32>::min(),
                                                  std::numeric_limits<ca::i32>::max(),
                                                  "lookupswitch key")
                                 : integerValue(*entry.first);
            if (raw && verify_ && !pairs.empty() && pairs.back().first >= key)
                fail("lookupswitch keys must be strictly increasing");
            pairs.emplace_back(static_cast<ca::i32>(key),
                               label(*std::static_pointer_cast<ast::Identifier>(entry.second)));
        }
        if (!raw) {
            std::sort(pairs.begin(), pairs.end(),
                      [](const auto& left, const auto& right) { return left.first < right.first; });
        }
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
        const auto dimensions = raw ? checkedInteger(*args[1], 0, 255, "multianewarray dimensions")
                                    : integerValue(*args[1]);
        if (raw && verify_ &&
            (dimensions == 0 || static_cast<std::size_t>(dimensions) > descriptor.find_first_not_of('[')))
            fail("multianewarray dimensions must be between 1 and the array rank");
        in.a  = static_cast<ca::i32>(dimensions);
        in.cp = session_.internClass(descriptor);
    }
    return in;
}

bool CodeCompiler::isVarOpcode(C::Op op) noexcept
{
    return (op >= C::Op::ILOAD && op <= C::Op::ALOAD) ||
           (op >= C::Op::ISTORE && op <= C::Op::ASTORE) || op == C::Op::RET;
}

bool CodeCompiler::isJumpOpcode(C::Op op) noexcept
{
    return (op >= C::Op::IFEQ && op <= C::Op::JSR) || op == C::Op::IFNULL ||
           op == C::Op::IFNONNULL || op == C::Op::GOTO_W || op == C::Op::JSR_W;
}

std::string CodeCompiler::methodPath(const std::vector<ast::ElementPtr>& args) const
{
    return narrow(*args[0]);
}

void finishBody(IR::ClassSession& session, IR::MethodId method, IR::CodeUnit& unit,
                const std::vector<std::pair<IR::InsnId, ca::i32>>& lineRows,
                const JvmCompilerOptions& options, const std::string& name,
                const std::string& descriptor, bool isStatic, JvmDialect dialect,
                const std::optional<RawMaxs>& maxs, std::vector<error::Warn>& warnings)
{
    C::ClassFile& cf = session.backing();
    const bool verify = dialect == JvmDialect::S || options.verify;

    if (!verify) {
        if (!maxs) fail("R dialect with verify disabled requires maxs");
        adoptRawMaxs(unit, *maxs, stackLowerBound(unit), requiredLocals(unit, descriptor, isStatic),
                     warnings);
        if (unit.dom.order().empty()) {
            writeEmptyCode(session, method, unit);
            return;
        }
    }

    //64KB 预检：lower 落盘前给出带实际字节数的诊断。libjbc 的
    //LowerError::CodeTooLarge 仍是兜底，但预检消息自带数字，排查更快。
    const auto codeSize = unit.dom.encodedSize();
    if (codeSize.is_ok() && codeSize.unwrap() > 0xFFFF)
        fail("Method body is " + std::to_string(codeSize.unwrap()) +
             " bytes of code, exceeding the 65535-byte limit (JVMS code_length)");

    require(IR::lowerMethod(session, method), "lowerMethod");

    auto built = jbc::analysis::SmtBuildResult{};
    if (verify) {
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
        jbc::analysis::FrameEngine engine(cf.cp, resolver, jbc::analysis::FramePolicy::Strict);
        std::vector<jbc::analysis::FrameAt> frames;
        auto summary = engine.compute(unit, sig, frames);
        if (!summary.ok)
            fail(std::string("Stack frame analysis failed: ") + jbc::analysis::to_string(summary.fail));
        if (dialect == JvmDialect::R)
            summary.maxLocals = std::max(summary.maxLocals, requiredLocals(unit, descriptor, isStatic));
        if (maxs) {
            const auto stack = std::max(summary.maxStack, unit.exc.empty() ? 0u : 1u);
            adoptRawMaxs(unit, *maxs, stack, summary.maxLocals, warnings);
        }
        else if (!jbc::analysis::applyComputedMaxes(unit, summary, sig)) {
            fail("Stack frame maxes adoption failed");
        }

        if (cf.major >= 50) {
            auto generated = jbc::analysis::buildStackMapTable(session, unit, frames);
            if (generated.is_err())
                fail(std::string("StackMapTable generation failed: ") +
                     jbc::analysis::to_string(generated.unwrap_err()));
            built = std::move(generated).unwrap();
        }

        require(IR::lowerMethod(session, method), "lowerMethod");
    }

    //挂回（lower 后重新定位 Code 槽；与 libjbc J7 管线同序）。
    auto* code = static_cast<C::CodeAttr*>(codeSlot(session, method).mutate());

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

}   //namespace jhasm::compile
