#pragma once

#include <jbc/core/attribute.hpp>

#include <libca/core/datatype.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace jhasm::util {

///@brief typeRef 文本面打包：targetType << 24 | first << 8 | second。
///
///first/second 按 target 家族取该形态的自然载荷（type_parameter_index /
///supertype_index / formal_parameter_index / throws_index → first；
///bound_index / catch_index → second）。只覆盖类/方法级 target 形态——
///带 pc offset 的 Code 级形态不进 Jasm 文本面（与旧栈打印行为一致）。
inline ca::i32 packTypeRef(ca::u8 targetType, ca::u32 first, ca::u32 second) noexcept
{
    return (static_cast<ca::i32>(targetType) << 24) | (static_cast<ca::i32>(first & 0xFFFF) << 8)
         | static_cast<ca::i32>(second & 0xFF);
}

///@brief packTypeRef 的逆。
inline void unpackTypeRef(ca::i32 ref, ca::u8& targetType, ca::u32& first,
                          ca::u32& second) noexcept
{
    targetType = static_cast<ca::u8>((static_cast<ca::u32>(ref) >> 24) & 0xFF);
    first      = (static_cast<ca::u32>(ref) >> 8) & 0xFFFF;
    second     = static_cast<ca::u32>(ref) & 0xFF;
}

///@brief type_path 的文本编码：每段一个小写字母——a=深入数组、b=深入嵌套、
///       c=通配符界、d=类型实参（后跟十进制下标），段间直接连写（贪心可逆）。
///
///字母 + 数字的连写是 tokenizer 的安全 Identifier 形态（数字带下划线才会被
///归类为 Number，字母开头恒为 Identifier）。空路径编码为空串。
inline std::string typePathToString(
    const std::vector<jbc::core::TypeAnnotationsAttr::TypePathEntry>& path)
{
    static constexpr char kinds[] = {'a', 'b', 'c', 'd'};
    std::string result;
    for (const auto& entry : path) {
        if (entry.kind > 3) continue;
        result.push_back(kinds[entry.kind]);
        if (entry.kind == 3) result += std::to_string(entry.typeArgIndex);
    }
    return result;
}

///@brief typePathToString 的逆；非法文本抛 std::invalid_argument。
inline std::vector<jbc::core::TypeAnnotationsAttr::TypePathEntry>
parseTypePath(const std::string& text)
{
    using Entry = jbc::core::TypeAnnotationsAttr::TypePathEntry;
    std::vector<Entry> result;
    ca::usize index = 0;
    while (index < text.size()) {
        Entry entry;
        switch (text[index]) {
        case 'a': entry.kind = 0; break;
        case 'b': entry.kind = 1; break;
        case 'c': entry.kind = 2; break;
        case 'd':
        {
            entry.kind         = 3;
            ca::usize digits   = index + 1;
            ca::usize end      = digits;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9')
                ++end;
            if (end == digits) throw std::invalid_argument("Type path argument index missing");
            entry.typeArgIndex = static_cast<ca::u8>(std::stoul(text.substr(digits, end - digits)));
            index              = end;
            result.push_back(entry);
            continue;
        }
        default: throw std::invalid_argument("Invalid type path entry");
        }
        ++index;
        result.push_back(entry);
    }
    return result;
}

}   //namespace jhasm::util
