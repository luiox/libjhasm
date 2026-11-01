#pragma once

// 方法体编译面（编译面内部头）：CodeCompiler 线性发射 + 延迟标签绑定，以及
// 发射完成后的 finishBody（Strict 帧分析 → SMT 生成 → maxes → lower → 挂回）。

#include "jvm_compile_detail.hpp"

#include <unordered_map>
#include <utility>

namespace jhasm::compile {

/// @brief 方法体编译器：逐条发射指令到 CodeUnit 的 CodeDom，标签与行号延迟
///       绑定到下一条落码指令；行号行（insn 句柄 + 行号）由 lineRows 带出，
///       lower 后由 finishBody 换算成 pc 落 LNT。
class CodeCompiler final
{
public:
    CodeCompiler(IR::ClassSession& session, IR::CodeUnitRef unit, const ast::Method& method,
                 const std::string& descriptor);

    void compile();

    /// @brief 行号行（insn 句柄 + 行号）；lower 后由 finishBody 换算成 pc 落 LNT。
    const std::vector<std::pair<IR::InsnId, ca::i32>>& lineRows() const noexcept
    {
        return lineRows_;
    }

private:
    IR::LabelId label(const ast::Identifier& value);
    ca::i32 variable(const ast::ElementPtr& value, bool wide);
    void emit(const ast::InstructionPtr& item);
    void append(IR::Insn in);
    IR::Insn insnFor(ca::str::Utf16StringRef name, const std::vector<ast::ElementPtr>& args);
    static bool isVarOpcode(C::Op op) noexcept;
    static bool isJumpOpcode(C::Op op) noexcept;
    std::string methodPath(const std::vector<ast::ElementPtr>& args) const;

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

/// @brief Strict 帧计算 → SMT 生成 → maxes 采用 → lower → SMT/LNT 挂回。
///
/// 全新方法体没有现成 pc：先 lower 一轮让 DOM 拿到真实 srcPc（帧引擎的
/// Uninitialized 载荷与 SMT 的目标/偏移都以 pc 计），再计算帧与生成 SMT，
/// 最后第二轮 lower 只为提交 maxes（字节不变 → 走 maxesChanged 快路径）。
void finishBody(IR::ClassSession& session, IR::MethodId method, IR::CodeUnit& unit,
                const std::vector<std::pair<IR::InsnId, ca::i32>>& lineRows,
                const JvmCompilerOptions& options, const std::string& name,
                const std::string& descriptor, bool isStatic);

}   //namespace jhasm::compile
