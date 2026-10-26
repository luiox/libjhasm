#include "jvm_analyzer.hpp"

#include <jbc/jbc.hpp>

#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace jhasm::analysis {
namespace {

namespace C  = jbc::core;
namespace IR = jbc::ir;

ca::str::Utf16String diagnosticText(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::str::Utf8String owning(std::string_view value)
{
    return ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                          value.size());
}

///@brief InheritanceChecker → libjbc Strict 帧引擎的引用合并策略。
///
///同名引用合并为自身；跨类引用经 InheritanceChecker 求公共父类——与旧栈
///HierarchyVerifier::merge 的可观测语义一致（空 checker 回落 Object）。
///合并产物的名字由本对象自持（帧域对 resolver 返回的 VType.name 只借用，
///寿命约定与 libjbc 测试栈的 StubResolver 同构：resolver 活过 compute 与
///快照期）。
class InheritanceResolver final : public jbc::analysis::ITypeResolver
{
public:
    explicit InheritanceResolver(const InheritanceChecker& inheritance) noexcept
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
    const InheritanceChecker* inheritance_;
    std::deque<std::string>   owned_;
};

///@brief VType → jhasm 拥有型 Value（描述符串）。
///
///uninitNewOffset 指到产生方 new 指令的类名（帧域 VType 不携带该名，从
///方法体按 pc 反查）；thisName 用于 UninitThis 槽。
Value snapshot(const jbc::analysis::VType& value,
               const std::unordered_map<ca::u32, std::string_view>& newClasses,
               std::string_view thisName)
{
    using jbc::analysis::VKind;
    switch (value.k) {
    case VKind::Int: return Value(owning("I"));
    case VKind::Float: return Value(owning("F"));
    case VKind::Long: return Value(owning("J"));
    case VKind::Double: return Value(owning("D"));
    case VKind::Ref:
        if (!value.name.empty()) {
            if (value.name.front() == '[') return Value(owning(value.name));
            return Value(owning("L" + std::string(value.name) + ";"));
        }
        return Value{};
    case VKind::Null: return Value(owning("Lnull;"));
    case VKind::UninitThis:
        if (!thisName.empty()) return Value(owning("L" + std::string(thisName) + ";"));
        return Value{};
    case VKind::Uninitialized:
    {
        const auto found = newClasses.find(value.uninitOffset());
        if (found != newClasses.end()) return Value(owning("L" + std::string(found->second) + ";"));
        return Value{};
    }
    case VKind::Top:
    case VKind::ReturnAddress:
    case VKind::Bottom:
    default: return Value{};
    }
}

Frame snapshot(const jbc::analysis::FrameAt& source,
               const std::unordered_map<ca::u32, std::string_view>& newClasses,
               std::string_view thisName)
{
    Frame result;
    result.locals.reserve(source.locals.size());
    result.stack.reserve(source.stack.size());
    for (const auto& local : source.locals)
        result.locals.push_back(snapshot(local, newClasses, thisName));
    for (const auto& stack : source.stack)
        result.stack.push_back(snapshot(stack, newClasses, thisName));
    return result;
}

// ── 操作数类别一致性扫描（JVMS 4.10.1 操作数类别面）────────────────────
//
// 帧引擎的赋值点只做形状/宽度检查（域格语义），int 上 arraylength、
// ()I 方法体 return 这类类别错配无感直通。此处对可达帧快照做逐指令类别
// 复核：快照 Value 携带描述符形态类别（"I"/"J"/"L…;"/"Lnull;"/空=未知或
// 垫底），已知类别不匹配即产出 error（Strict 域的值带精确类别与名字，不
// 误报；空槽一律放行——保守面）。

/// @brief 引用形态描述符判定（L…; / [ 开头；"Lnull;" 是 null 形态）。
bool isRefShape(std::string_view d)
{
    return d.size() >= 2 && (d.front() == 'L' || d.front() == '[');
}

/// @brief 从栈顶数第 fromTop 个值行（跳过垫底/未知空行；不足为 nullptr）。
const Value* valueAt(const Frame& frame, ca::usize fromTop)
{
    ca::usize seen = 0;
    for (ca::usize i = frame.stack.size(); i-- > 0;) {
        if (frame.stack[i].isUninitialized()) continue;
        if (seen == fromTop) return &frame.stack[i];
        ++seen;
    }
    return nullptr;
}

/// @brief 值行描述符（缺槽/未知行返回空 view）。
std::string_view descAt(const Frame& frame, ca::usize fromTop)
{
    const Value* value = valueAt(frame, fromTop);
    if (value == nullptr) return std::string_view();
    return std::string_view(reinterpret_cast<const char*>(value->descriptor().data()),
                            value->descriptor().byte_length());
}

/// @brief Memberref → 成员描述符（非成员引用失败）。
bool memberDescOf(const C::ClassFile& cf, ca::u32 cpIndex, std::string_view& desc)
{
    if (!cf.cp.valid(cpIndex)) return false;
    const C::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    if (entry.tag != C::CpTag::Fieldref && entry.tag != C::CpTag::Methodref &&
        entry.tag != C::CpTag::InterfaceMethodref)
        return false;
    const C::ConstantPool::Entry& nat = cf.cp.at(entry.b);
    if (nat.tag != C::CpTag::NameAndType) return false;
    desc = cf.utf8(nat.b);
    return true;
}

/// @brief 引用名互赋（JVM 赋值兼容语义）：同名 / null 底 / 数组同维协变 /
///        checker 子类判定；名字未知一律放行（保守面）。
bool refCompatible(std::string_view value, std::string_view target,
                   const InheritanceChecker& inheritance)
{
    if (value.empty() || value == "Lnull;") return true;
    if (value == target) return true;
    if (value.front() == '[' && target.front() == '[') {
        if (value.substr(1) == target.substr(1)) return true;
        // 同维才谈元素赋值（维度不同恒不兼容）；基座按引用赋值，或目标基座
        // 为 Object（数组协变的上界）。
        ca::usize vd = 0;
        while (vd < value.size() && value[vd] == '[') ++vd;
        ca::usize td = 0;
        while (td < target.size() && target[td] == '[') ++td;
        if (vd != td) return false;
        std::string_view ve = value.substr(vd);
        std::string_view te = target.substr(td);
        if (te == "Ljava/lang/Object;") return true;
        return refCompatible(ve, te, inheritance);
    }
    if (value.front() != 'L' || target.front() != 'L') return false;
    return inheritance.isSubclassOf(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()) + 1,
                                          value.size() - 2),
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(target.data()) + 1,
                                          target.size() - 2));
}

/// @brief 单指令类别复核；违规产出 error（"Method name<desc>: …" 形态）。
void checkInsnKinds(const C::ClassFile& cf, const jbc::analysis::MethodSig& sig,
                    const IR::Insn& insn, const Frame& frame,
                    const InheritanceChecker& inheritance, std::vector<error::Error>& errors)
{
    const auto fail = [&](const std::string& detail) {
        errors.emplace_back(diagnosticText("Method " + std::string(sig.name) +
                                           std::string(sig.desc) + ": " + detail)
                                .ref());
    };
    const auto expectTop = [&](std::string_view got, const char* what, std::string_view want) {
        if (!got.empty() && got != want)
            fail(what + std::string(" with stack top ") + std::string(got) + " (expected " +
                 std::string(want) + ")");
    };
    const auto expectRefTop = [&](std::string_view got, const char* what) {
        if (!got.empty() && !isRefShape(got))
            fail(what + std::string(" with non-reference stack top ") + std::string(got));
    };
    // 成员描述符参数区解析（有成员引用时）。
    std::string_view memberDesc;
    const bool       hasMember = memberDescOf(cf, insn.cp, memberDesc);
    jbc::type::MethodDesc callee;
    bool                  calleeOk = false;
    if (hasMember && (insn.op == C::Op::INVOKEVIRTUAL || insn.op == C::Op::INVOKESPECIAL ||
                      insn.op == C::Op::INVOKESTATIC || insn.op == C::Op::INVOKEINTERFACE)) {
        const auto parsed = jbc::type::parseMethodDescriptor(memberDesc);
        if (parsed.is_ok()) {
            callee    = std::move(parsed).unwrap();
            calleeOk  = true;
        }
    }
    // 实参/接收者检查：按描述符逐参自栈顶向栈底对位（cat2 双槽）。kind=false
    // 的参数（prim 类别）核对精确类别；引用参数核对引用形态 + 名字互赋。
    const auto checkArgs = [&]() {
        if (!calleeOk) return;
        ca::usize fromTop = 0;
        auto      slotOf  = [&](const jbc::type::TypeDesc& t) {
            return t.kind == jbc::type::TypeDesc::Kind::Primitive &&
                           (t.primitive == jbc::type::BaseType::Long ||
                            t.primitive == jbc::type::BaseType::Double)
                       ? 2u
                       : 1u;
        };
        auto render = [&](const jbc::type::TypeDesc& t) -> std::string {
            if (t.kind == jbc::type::TypeDesc::Kind::Class) {
                return std::string(t.array_dims, '[') + "L" + std::string(t.class_name) + ";";
            }
            switch (t.primitive) {
            case jbc::type::BaseType::Byte: return std::string(t.array_dims, '[') + "B";
            case jbc::type::BaseType::Char: return std::string(t.array_dims, '[') + "C";
            case jbc::type::BaseType::Double: return std::string(t.array_dims, '[') + "D";
            case jbc::type::BaseType::Float: return std::string(t.array_dims, '[') + "F";
            case jbc::type::BaseType::Int: return std::string(t.array_dims, '[') + "I";
            case jbc::type::BaseType::Long: return std::string(t.array_dims, '[') + "J";
            case jbc::type::BaseType::Short: return std::string(t.array_dims, '[') + "S";
            case jbc::type::BaseType::Boolean: return std::string(t.array_dims, '[') + "Z";
            }
            return std::string(t.array_dims, '[') + "I";
        };
        for (ca::usize index = callee.params.size(); index-- > 0;) {
            const auto&       param = callee.params[index];
            const std::string want  = render(param);
            const std::string_view got = descAt(frame, fromTop);
            if (!got.empty()) {
                if (want.front() != 'L' && want.front() != '[') {
                    expectTop(got, "argument", want);
                }
                else if (!refCompatible(got, want, inheritance)) {
                    fail("argument " + std::string(got) + " is not assignable to " + want);
                }
            }
            fromTop += slotOf(param);
        }
        if (insn.op != C::Op::INVOKESTATIC) {
            const std::string_view receiver = descAt(frame, fromTop);
            expectRefTop(receiver, "invoke receiver");
        }
    };

    switch (insn.op) {
    case C::Op::IRETURN: expectTop(descAt(frame, 0), "ireturn", "I"); break;
    case C::Op::LRETURN: expectTop(descAt(frame, 0), "lreturn", "J"); break;
    case C::Op::FRETURN: expectTop(descAt(frame, 0), "freturn", "F"); break;
    case C::Op::DRETURN: expectTop(descAt(frame, 0), "dreturn", "D"); break;
    case C::Op::ARETURN: expectRefTop(descAt(frame, 0), "areturn"); break;
    case C::Op::RETURN:
        if (!sig.desc.empty() && sig.desc.find(")V") != sig.desc.size() - 2)
            fail("return in method with non-void return type");
        break;
    case C::Op::ARRAYLENGTH: expectRefTop(descAt(frame, 0), "arraylength"); break;
    case C::Op::IALOAD: case C::Op::LALOAD: case C::Op::FALOAD: case C::Op::DALOAD:
    case C::Op::BALOAD: case C::Op::CALOAD: case C::Op::SALOAD:
        expectTop(descAt(frame, 1), "array load", "I");   ///< index
        expectRefTop(descAt(frame, 0), "array load");     ///< arrayref
        break;
    case C::Op::AALOAD:
        expectTop(descAt(frame, 1), "array load", "I");
        expectRefTop(descAt(frame, 0), "array load");
        break;
    case C::Op::IASTORE: case C::Op::LASTORE: case C::Op::FASTORE: case C::Op::DASTORE:
    case C::Op::BASTORE: case C::Op::CASTORE: case C::Op::SASTORE:
        expectRefTop(descAt(frame, 0), "array store");    ///< value（prim 数组元素
                                                          ///< 类别由引擎宽度面承载）
        expectTop(descAt(frame, 1), "array store", "I");  ///< index
        expectRefTop(descAt(frame, 2), "array store");    ///< arrayref
        break;
    case C::Op::AASTORE:
        expectRefTop(descAt(frame, 0), "array store");
        expectTop(descAt(frame, 1), "array store", "I");
        expectRefTop(descAt(frame, 2), "array store");
        break;
    case C::Op::ATHROW:
    case C::Op::MONITORENTER:
    case C::Op::MONITOREXIT: expectRefTop(descAt(frame, 0), "instruction"); break;
    case C::Op::GETFIELD: {
        bool ok = false;
        if (hasMember) {
            const ca::usize close = memberDesc.find(')');
            if (close != std::string_view::npos) {
                const std::string_view d = memberDesc.substr(close + 1);
                ok                       = true;
                const std::string_view receiver = descAt(frame, d == "J" || d == "D" ? 2 : 1);
                expectRefTop(receiver, "field access");
            }
        }
        (void)ok;
        break;
    }
    case C::Op::PUTFIELD: {
        if (hasMember) {
            const ca::usize close = memberDesc.find(')');
            if (close != std::string_view::npos) {
                const std::string_view d     = memberDesc.substr(close + 1);
                const ca::usize        slots = (d == "J" || d == "D") ? 2u : 1u;
                const std::string_view value = descAt(frame, 0);
                if (!value.empty()) {
                    if (d.front() != 'L' && d.front() != '[') {
                        expectTop(value, "field store", d);
                    }
                    else if (!refCompatible(value, d, inheritance)) {
                        fail("field store of " + std::string(value) + " into " +
                             std::string(d));
                    }
                }
                const std::string_view receiver = descAt(frame, slots);
                expectRefTop(receiver, "field access");
            }
        }
        break;
    }
    case C::Op::PUTSTATIC: {
        if (hasMember) {
            const ca::usize close = memberDesc.find(')');
            if (close != std::string_view::npos) {
                const std::string_view d = memberDesc.substr(close + 1);
                const std::string_view value = descAt(frame, 0);
                if (!value.empty()) {
                    if (d.front() != 'L' && d.front() != '[') {
                        expectTop(value, "field store", d);
                    }
                    else if (!refCompatible(value, d, inheritance)) {
                        fail("field store of " + std::string(value) + " into " +
                             std::string(d));
                    }
                }
            }
        }
        break;
    }
    case C::Op::INVOKEVIRTUAL:
    case C::Op::INVOKESPECIAL:
    case C::Op::INVOKESTATIC:
    case C::Op::INVOKEINTERFACE: checkArgs(); break;
    case C::Op::ISTORE: case C::Op::ISTORE_0: case C::Op::ISTORE_1:
    case C::Op::ISTORE_2: case C::Op::ISTORE_3:
        expectTop(descAt(frame, 0), "istore", "I");
        break;
    case C::Op::LSTORE: case C::Op::LSTORE_0: case C::Op::LSTORE_1:
    case C::Op::LSTORE_2: case C::Op::LSTORE_3:
        expectTop(descAt(frame, 0), "lstore", "J");
        break;
    case C::Op::FSTORE: case C::Op::FSTORE_0: case C::Op::FSTORE_1:
    case C::Op::FSTORE_2: case C::Op::FSTORE_3:
        expectTop(descAt(frame, 0), "fstore", "F");
        break;
    case C::Op::DSTORE: case C::Op::DSTORE_0: case C::Op::DSTORE_1:
    case C::Op::DSTORE_2: case C::Op::DSTORE_3:
        expectTop(descAt(frame, 0), "dstore", "D");
        break;
    case C::Op::ASTORE: case C::Op::ASTORE_0: case C::Op::ASTORE_1:
    case C::Op::ASTORE_2: case C::Op::ASTORE_3:
        expectRefTop(descAt(frame, 0), "astore");
        break;
    case C::Op::TABLESWITCH:
    case C::Op::LOOKUPSWITCH: expectTop(descAt(frame, 0), "switch", "I"); break;
    case C::Op::CHECKCAST:
    case C::Op::INSTANCEOF:
    case C::Op::IFNULL:
    case C::Op::IFNONNULL: expectRefTop(descAt(frame, 0), "instruction"); break;
    default: break;
    }
}

}   //namespace

error::Result<JvmAnalysisResult> JvmAnalyzer::analyze(const std::vector<ca::u8>& classBytes) const
{
    JvmAnalysisResult         result;
    std::vector<error::Error> errors;
    try {
        auto opened = IR::makeSessionFromBytes(C::ByteView{classBytes.data(), classBytes.size()});
        if (opened.is_err()) {
            const auto& failure = opened.unwrap_err();
            auto message = failure.stage == IR::SessionError::Stage::Read
                               ? std::string("classfile read failed: ") +
                                     C::to_string(failure.read)
                               : std::string("classfile session failed: ") +
                                     IR::to_string(failure.session);
            errors.emplace_back(diagnosticText(message).ref());
            return error::Result<JvmAnalysisResult>(std::move(result), std::move(errors), {});
        }
        //unwrap() && 按值移出 shared_ptr：先落命名变量持有会话，再取引用
        //（直接 *临时指针 会在语句末析构会话，留下悬空引用）。
        auto               owned   = std::move(opened).unwrap();
        IR::ClassSession& session = *owned;
        C::ClassFile&     cf      = session.backing();
        const std::string thisName{cf.thisName()};

        for (ca::usize index = 0; index < cf.methods.size(); ++index) {
            C::MemberInfo& m       = cf.methods[index];
            MethodAnalysis methodResult{owning(cf.utf8(m.nameCp)), owning(cf.utf8(m.descCp)), {}};
            if (!IR::hasCode(session, index)) {
                result.methods.push_back(std::move(methodResult));
                continue;
            }
            auto promoted = IR::promoteMethod(session, index);
            if (promoted.is_err()) {
                std::string message = "Method " + std::string{cf.utf8(m.nameCp)} +
                                      std::string{cf.utf8(m.descCp)} + ": body promote failed: " +
                                      IR::to_string(promoted.unwrap_err());
                errors.emplace_back(diagnosticText(message).ref());
                result.methods.push_back(std::move(methodResult));
                continue;
            }
            IR::CodeUnitRef unit = std::move(promoted).unwrap();
            methodResult.frames.resize(unit->dom.size());

            //帧域 VType 不携带 uninit new 的类名，按 pc 反查一份 new 指令表。
            std::unordered_map<ca::u32, std::string_view> newClasses;
            for (const auto id : unit->dom.order()) {
                const auto* insn = unit->dom.peek(id);
                if (insn != nullptr && insn->op == C::Op::NEW)
                    newClasses.emplace(insn->srcPc, cf.cp.utf8Of(cf.cp.at(insn->cp).a));
            }

            jbc::analysis::MethodSig sig;
            sig.isStatic  = (m.access & 0x0008) != 0;
            sig.desc      = cf.utf8(m.descCp);
            sig.name      = cf.utf8(m.nameCp);
            sig.thisClass = cf.thisName();

            InheritanceResolver              resolver(*inheritance_);
            jbc::analysis::FrameEngine       engine(cf.cp, resolver,
                                                    jbc::analysis::FramePolicy::Strict);
            std::vector<jbc::analysis::FrameAt> frames;
            const auto summary = engine.compute(*unit, sig, frames);
            if (!summary.ok) {
                std::string message = "Method " + std::string{cf.utf8(m.nameCp)} +
                                      std::string{cf.utf8(m.descCp)} + ": analysis failed: " +
                                      jbc::analysis::to_string(summary.fail);
                errors.emplace_back(diagnosticText(message).ref());
            }
            else {
                ca::usize position = 0;
                for (const auto id : unit->dom.order()) {
                    if (position >= frames.size()) break;
                    const auto* insn = unit->dom.peek(id);
                    if (insn != nullptr && frames[position].reachable) {
                        methodResult.frames[position] =
                            snapshot(frames[position], newClasses, cf.thisName());
                        checkInsnKinds(cf, sig, *insn, *methodResult.frames[position],
                                       *inheritance_, errors);
                    }
                    ++position;
                }
            }
            result.methods.push_back(std::move(methodResult));
        }
    }
    catch (const std::exception& exception) {
        errors.emplace_back(diagnosticText(exception.what()).ref());
    }
    return error::Result<JvmAnalysisResult>(std::move(result), std::move(errors), {});
}

}   //namespace jhasm::analysis
