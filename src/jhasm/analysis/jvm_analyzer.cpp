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
                for (ca::usize position = 0; position < frames.size(); ++position) {
                    if (frames[position].reachable)
                        methodResult.frames[position] =
                            snapshot(frames[position], newClasses, cf.thisName());
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
