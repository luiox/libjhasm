#pragma once

// jvm 编译面共享原语（模块内内部头，不出公开面）：旗语/句柄/atype 常量、
// Result→异常桥、规格驻留仓、文本与描述符工具。jvm_compiler.cpp 只留类级
// 装配与对外入口；常量/注解/方法体三面分见 jvm_constants、jvm_annotations、
// code_compiler。

#include "jvm_compiler.hpp"

#include "jhasm/util/access_flags.hpp"
#include "jhasm/util/text.hpp"

#include <jbc/jbc.hpp>

#include <libca/str/utf8_string.hpp>

#include <deque>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace jhasm::compile {

namespace C  = jbc::core;
namespace IR = jbc::ir;

//旗语常量与文本对比单源在 jhasm::util（编译/打印/解析共用），此处按编译面
//习惯重导出。
using jhasm::util::equals;
using util::ACC_ANNOTATION;
using util::ACC_ABSTRACT;
using util::ACC_BRIDGE;
using util::ACC_ENUM;
using util::ACC_FINAL;
using util::ACC_INTERFACE;
using util::ACC_MODULE;
using util::ACC_NATIVE;
using util::ACC_PRIVATE;
using util::ACC_PROTECTED;
using util::ACC_PUBLIC;
using util::ACC_RECORD;
using util::ACC_STATIC;
using util::ACC_STRICT;
using util::ACC_SUPER;
using util::ACC_SYNCHRONIZED;
using util::ACC_SYNTHETIC;
using util::ACC_TRANSIENT;
using util::ACC_VARARGS;
using util::ACC_VOLATILE;

// 方法句柄 reference_kind（JVMS 4.4.8）。
constexpr ca::u32 H_GETFIELD = 1;
constexpr ca::u32 H_GETSTATIC = 2;
constexpr ca::u32 H_PUTFIELD = 3;
constexpr ca::u32 H_PUTSTATIC = 4;
constexpr ca::u32 H_INVOKEVIRTUAL = 5;
constexpr ca::u32 H_INVOKESTATIC = 6;
constexpr ca::u32 H_INVOKESPECIAL = 7;
constexpr ca::u32 H_NEWINVOKESPECIAL = 8;
constexpr ca::u32 H_INVOKEINTERFACE = 9;

// newarray 的 atype 操作数（JVMS 6.5 newarray 表）。
constexpr ca::i32 T_BOOLEAN = 4;
constexpr ca::i32 T_CHAR = 5;
constexpr ca::i32 T_FLOAT = 6;
constexpr ca::i32 T_DOUBLE = 7;
constexpr ca::i32 T_BYTE = 8;
constexpr ca::i32 T_SHORT = 9;
constexpr ca::i32 T_INT = 10;
constexpr ca::i32 T_LONG = 11;

[[noreturn]] inline void fail(const std::string& message)
{
    throw std::invalid_argument(message);
}

//错误类型的 to_string 探测（ADL）：没有诊断文本的错误退化成空串。
template <typename E, typename = void>
struct HasToString : std::false_type
{
};
template <typename E>
struct HasToString<E, std::void_t<decltype(to_string(std::declval<E>()))>> : std::true_type
{
};

template <typename T, typename E>
T require(ca::core::Result<T, E> result, const char* stage)
{
    if (result.is_err()) {
        std::string detail;
        if constexpr (HasToString<E>::value) detail = to_string(result.unwrap_err());
        fail(std::string(stage) + " failed" + (detail.empty() ? "" : ": " + detail));
    }
    if constexpr (std::is_void_v<T>) {
        return;
    }
    else {
        return std::move(result).unwrap();
    }
}

///@brief 规格字符串驻留仓：libjbc 的 MethodHandleSpec/SemAnnotation/BsmArg
///      一律持 string_view（借用型规格），而 Jasm 文本面的名字/描述符都是
///      即时临时串。构建侧把全部借用串落此仓，仓的生命周期覆盖到同步
///      intern/attach 调用返回为止。
class SpecArena final
{
public:
    std::string_view put(std::string value)
    {
        owned_.push_back(std::move(value));
        return owned_.back();
    }

private:
    std::deque<std::string> owned_;
};

///@brief UTF-16 → JVM modified UTF-8（JVMS 4.4.7）：NUL 走 0xC0 0x80，补充字符
///      按代理对拆成两个三字节单元（CESU-8 形态）。CP Utf8 载体的唯一编码口径，
///      与 printer 侧 modifiedUtf16 解码器 1:1 往返；ASCII 输入字节不变。
inline std::string modifiedUtf8(ca::str::Utf16StringRef value)
{
    std::string out;
    out.reserve(value.length() * 3);
    for (ca::usize index = 0; index < value.length(); ++index) {
        const ca::u16 unit = value.char_at(index).unit();
        if (unit == 0) {
            out += "\xC0\x80";
        }
        else if (unit < 0x80) {
            out += static_cast<char>(unit);
        }
        else if (unit < 0x800) {
            out += static_cast<char>(0xC0 | (unit >> 6));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
        else {
            out += static_cast<char>(0xE0 | (unit >> 12));
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
    }
    return out;
}

inline std::string narrow(const ast::Element& value)
{
    return modifiedUtf8(value.content());
}

inline ca::u32 access(const ast::Modifiers& modifiers)
{
    ca::u32 result = 0;
    for (const auto& value : modifiers.values()) {
        if (!value) continue;
        const auto name = value->content();
        if (equals(name, "public"))
            result |= ACC_PUBLIC;
        else if (equals(name, "private"))
            result |= ACC_PRIVATE;
        else if (equals(name, "protected"))
            result |= ACC_PROTECTED;
        else if (equals(name, "static"))
            result |= ACC_STATIC;
        else if (equals(name, "final"))
            result |= ACC_FINAL;
        else if (equals(name, "super"))
            result |= ACC_SUPER;
        else if (equals(name, "synchronized"))
            result |= ACC_SYNCHRONIZED;
        else if (equals(name, "volatile"))
            result |= ACC_VOLATILE;
        else if (equals(name, "bridge"))
            result |= ACC_BRIDGE;
        else if (equals(name, "transient"))
            result |= ACC_TRANSIENT;
        else if (equals(name, "varargs"))
            result |= ACC_VARARGS;
        else if (equals(name, "native"))
            result |= ACC_NATIVE;
        else if (equals(name, "interface"))
            result |= ACC_INTERFACE;
        else if (equals(name, "abstract"))
            result |= ACC_ABSTRACT;
        else if (equals(name, "strictfp"))
            result |= ACC_STRICT;
        else if (equals(name, "synthetic"))
            result |= ACC_SYNTHETIC;
        else if (equals(name, "annotation"))
            result |= ACC_ANNOTATION;
        else if (equals(name, "enum"))
            result |= ACC_ENUM;
        else if (equals(name, "module"))
            result |= ACC_MODULE;
        else if (equals(name, "record"))
            result |= ACC_RECORD;
    }
    //access_flags 是 u16 域：ACC_RECORD（0x10000）是 JDK 内部位，classfile 出口
    //按旧栈口径静默截断（Record 属性的在场与否才是记录类的规范形态）。
    return result & 0xFFFF;
}

///@brief 标识符 → 类内部名：L..; 描述符形态剥壳；数组描述符（[ 开头）与
///       普通内部名原样保留。
inline std::string internalNameOf(const ast::Identifier& value)
{
    //CP 内部名同样走 modified UTF-8：与 javac 一致，增补字符以 CESU-8 代理对落池。
    auto name = narrow(value);
    if (name.size() > 1 && name.front() == 'L' && name.back() == ';')
        return name.substr(1, name.size() - 2);
    return name;
}

///@brief 类型的字段描述符形态（注解 'c' 元素与注解类型落 CP 用）。
inline std::string descriptorOf(const ast::Identifier& value)
{
    const auto name = internalNameOf(value);
    if (!name.empty() && name.front() == '[') return name;
    return "L" + name + ";";
}

inline ca::i64 integerValue(const ast::Element& value)
{
    std::string text = narrow(value);
    if (!text.empty() && (text.back() == 'l' || text.back() == 'L')) text.pop_back();
    return std::stoll(text, nullptr, 0);
}

inline ca::f64 floatingValue(const ast::Element& value)
{
    std::string text = narrow(value);
    if (!text.empty() &&
        (text.back() == 'f' || text.back() == 'F' || text.back() == 'd' || text.back() == 'D'))
        text.pop_back();
    return std::stod(text);
}

inline ca::u32 floatBits(ca::f32 value)
{
    ca::u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline std::pair<ca::u64, ca::f64> doubleWords(ca::f64 value)
{
    ca::u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return {bits, value};
}

///@brief 方法参数槽位总数（this 行 + 各参数槽宽）；maxLocals 初始化与
///       方法体编译器共用同一口径。
inline ca::i32 paramSlotTotal(const std::string& descriptor, bool isStatic)
{
    const auto parsed = jbc::type::parseMethodDescriptor(descriptor);
    if (parsed.is_err()) fail("Invalid method descriptor");
    const auto method = std::move(parsed).unwrap();
    return (isStatic ? 0 : 1) + static_cast<ca::i32>(jbc::type::paramSlotCount(method));
}

}   //namespace jhasm::compile
