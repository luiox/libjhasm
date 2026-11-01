#include "code_compiler.hpp"

#include "jvm_constants.hpp"
#include "jvm_opcodes.hpp"

#include <algorithm>
#include <deque>
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
                           const ast::Method& method, const std::string& descriptor)
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
        pendingLines_.push_back(static_cast<ca::i32>(integerValue(*item->arguments()[0])));
        return;
    }
    append(insnFor(name, item->arguments()));
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

IR::Insn CodeCompiler::insnFor(ca::str::Utf16StringRef name, const std::vector<ast::ElementPtr>& args)
{
    const auto op  = opcode(name);
    const auto low = [name](const char* text) { return equals(name, text); };
    IR::Insn    in;
    in.op = op;
    if (low("bipush") || low("sipush")) {
        ConstSpec spec;
        spec.kind = ConstSpec::Kind::Int;
        spec.num  = static_cast<ca::i64>(integerValue(*args[0]));
        in        = constantLoad(session_, spec);
    }
    else if (low("ldc") || low("ldc_w") || low("ldc2_w")) {
        in = constantLoad(session_, constantValue(args[0], arena_));
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
        in.wide = in.a > 255;
        if (in.a <= 3 && op != C::Op::RET) {
            //槽 0..3 一律落 _N 短格式（jblw 写出口径）。
            in.op = varShortForm(op, static_cast<ca::i32>(in.a));
        }
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
                const std::string& descriptor, bool isStatic)
{
    C::ClassFile& cf = session.backing();

    //64KB 预检：lower 落盘前给出带实际字节数的诊断。libjbc 的
    //LowerError::CodeTooLarge 仍是兜底，但预检消息自带数字，排查更快。
    const auto codeSize = unit.dom.encodedSize();
    if (codeSize.is_ok() && codeSize.unwrap() > 0xFFFF)
        fail("Method body is " + std::to_string(codeSize.unwrap()) +
             " bytes of code, exceeding the 65535-byte limit (JVMS code_length)");

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
        fail(std::string("Stack frame analysis failed: ") + jbc::analysis::to_string(summary.fail));
    if (!jbc::analysis::applyComputedMaxes(unit, summary, sig))
        fail("Stack frame maxes adoption failed");

    auto built = jbc::analysis::SmtBuildResult{};
    if (cf.major >= 50) {
        auto generated = jbc::analysis::buildStackMapTable(session, unit, frames);
        if (generated.is_err())
            fail(std::string("StackMapTable generation failed: ") +
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

}   //namespace jhasm::compile
