#pragma once

#include "jhasm/ast/specific.hpp"
#include "jhasm/analysis/inheritance_checker.hpp"
#include "jhasm/error/result.hpp"

#include <libca/core/datatype.hpp>

#include <vector>

namespace jhasm::compile {

struct JvmCompilerOptions final
{
    ///@brief 目标 classfile 主版本号（如 61 = Java 17）。
    ca::u16 majorVersion = 61;
    ///@brief 目标 classfile 次版本号（preview 特性使用 0xFFFF）。
    ca::u16 minorVersion = 0;
    ///@brief 非拥有指针；非空时用于 StackMap frame 的 common-super 计算。
    const analysis::InheritanceChecker* inheritance = nullptr;
};

struct JvmCompileResult final
{
    ///@brief 编译产物：完整 classfile 字节。
    ///
    ///本结构不再携带类模型视图（旧栈经 libjblw 泄漏 ClassFileViewPtr 的形态已
    ///退役）。需要模型的下游用后端库自行从字节重建（libjbc：readClass）。
    std::vector<ca::u8> bytes;
};

///@brief 将处理后的 JVM AST 编译为 classfile bytes。
class JvmCompiler final
{
public:
    explicit JvmCompiler(JvmCompilerOptions options = {})
        : options_(options)
    {}
    error::Result<JvmCompileResult> compile(const std::vector<ast::ElementPtr>& input) const;
    error::Result<JvmCompileResult> compile(const ast::Class& clazz) const;

private:
    JvmCompilerOptions options_;
};

}   //namespace jhasm::compile
