#pragma once

#include "jhasm/dialect.hpp"
#include "jhasm/error/result.hpp"

#include <libca/core/datatype.hpp>
#include <libca/str/utf8_string.hpp>

#include <vector>

namespace jhasm::printer {

/// @brief 将 JVM classfile bytes 打印为可重新汇编的 Jasm。
class JvmPrinter final
{
public:
    /// @brief 打印完整 classfile 为含 `.class` 声明的完整 Jasm 文本。
    ///
    /// @param classBytes 完整 classfile 字节。
    /// @param dialect S 输出规范化文本；R 保留指令形态并输出首行方言标记。
    /// @return 失败返回单条无位置诊断：classfile 读取失败带
    ///         ``classfile read failed`` 前缀，成员级失败带 ``in <member>``
    ///         前缀归属到具体字段/方法。
    error::Result<ca::str::Utf16String> print(const std::vector<ca::u8>& classBytes,
                                             JvmDialect dialect = JvmDialect::S) const;

    /// @brief 将指定方法打印为可嵌入 class Jasm 的声明。
    ///
    /// @param classBytes 已验证或可由 ClassReader 读取的完整 classfile 字节。
    /// @param name 方法名；构造器使用 `\<init\>`，类初始化器使用 `\<clinit\>`。
    /// @param descriptor JVM 方法描述符，用于消除重载歧义。
    /// @param dialect 指令打印方言，方言标记由外围 class 文本决定。
    /// @return 找到时返回单个 `.method` 声明；方法不存在或 class 无法读取时返回诊断。
    error::Result<ca::str::Utf16String> printMethod(const std::vector<ca::u8>&    classBytes,
                                                     ca::str::Utf8StringRef name,
                                                     ca::str::Utf8StringRef descriptor,
                                                     JvmDialect dialect = JvmDialect::S) const;
};

}   //namespace jhasm::printer
