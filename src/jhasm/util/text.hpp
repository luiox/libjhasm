#pragma once

// UTF-16 文本面对比与转换（libjhasm 单源）：jasm 语法关键字/助记符/修饰符
// 都是 ASCII 字面，与 UTF-16 源文本的相等判断、错误诊断的 UTF-16 化在此收敛，
// 不再各编译单元自持拷贝。

#include <libca/str/utf16_string.hpp>
#include <libca/str/utf8_string.hpp>


#include <string>

namespace jhasm::util {

///@brief UTF-16 文本与 ASCII 字面严格相等（长度一致逐单元比对）。
inline bool equals(ca::str::Utf16StringRef value, const char* ascii) noexcept
{
    ca::usize index = 0;
    for (; ascii[index] != '\0'; ++index) {
        if (index >= value.length() ||
            value.char_at(index).value != static_cast<ca::u16>(ascii[index]))
            return false;
    }
    return index == value.length();
}

///@brief UTF-8 窄串 → UTF-16（错误消息/诊断文本出水面用）。
inline ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

}   //namespace jhasm::util
