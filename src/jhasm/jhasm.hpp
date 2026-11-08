#pragma once

#include "jhasm/analysis/jvm_analyzer.hpp"
#include "jhasm/compile/jvm_compiler.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"
#include "jhasm/printer/jvm_printer.hpp"

namespace jhasm {

/// @brief 返回当前 libjhasm 重写基线版本。
/// @return 进程生命周期内有效的 UTF-8 静态字符串。
const char* get_lib_version() noexcept;

/// @brief 返回本次重写所固定的 Jasm revision。
/// @return 进程生命周期内有效的 Git revision 静态字符串。
const char* get_upstream_revision() noexcept;

/// @brief 执行完整 Jasm JVM 汇编流水线并返回 classfile bytes。
/// @return 失败时返回错误诊断：AST 归属的语义/装配错误保留 jasm 源位置，
///         流水线内部错误无位置；前置阶段失败时 warnings 一并透出。
error::Result<compile::JvmCompileResult> assemble(ca::str::Utf16StringRef     sourceName,
                                                  ca::str::Utf16StringRef     source,
                                                  compile::JvmCompilerOptions options = {});

/// @brief 将 classfile bytes 反汇编为可重新汇编的规范化 Jasm。
/// @return 失败返回单条无位置诊断：classfile 读取失败，或打印途中遇到无法
///         解释的结构（成员级失败带 ``in <member>`` 前缀）。
error::Result<ca::str::Utf16String> disassemble(const std::vector<ca::u8>& classBytes);

/// @brief 将 classfile 中指定的方法反汇编为规范化 Jasm 声明。
///
/// 方法名与 descriptor 必须同时给出以消除重载歧义。返回的文本不包含外围 `.class` 声明，
/// 适合 API 展示或嵌入由调用方构造的完整 class Jasm。
error::Result<ca::str::Utf16String> disassembleMethod(const std::vector<ca::u8>&    classBytes,
                                                       ca::str::Utf8StringRef methodName,
                                                       ca::str::Utf8StringRef descriptor);

/// @brief 分析 classfile 中全部非 abstract/native 方法并返回 owning frame 快照。
/// @return 字节不可读时返回无值错误；分析途中单个方法失败记入 errors，
///         其余方法的快照仍在 value 中（部分成功语义）。
error::Result<analysis::JvmAnalysisResult> analyze(
    const std::vector<ca::u8>& classBytes, const analysis::InheritanceChecker& inheritance =
                                               analysis::EmptyInheritanceChecker::instance());

}   //namespace jhasm
