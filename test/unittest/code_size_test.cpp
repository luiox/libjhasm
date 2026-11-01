/// @file        code_size_test.cpp
/// @brief       方法体 64KB 上限契约：超限诊断带实际字节数与 65535 上限；
///              远距 goto 依赖 libjbc 导出期自动加宽（goto→goto_w），编译成功。
#include <gtest/gtest.h>

#include "jhasm/jhasm.hpp"

#include <libca/str/utf16_string.hpp>
#include <libca/str/utf8_string.hpp>

#include <string>

namespace {

ca::str::Utf16String u16(const std::string& ascii)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(ascii.data()), ascii.size()));
}

std::string messageOf(const jhasm::error::Error& error)
{
    return error.message().to_utf8_string().to_std_string();
}

}   // namespace

TEST(assemble_code_size, oversized_method_reports_size_and_limit)
{
    // 66000 条 nop + return = 66001 字节 code：超过 65535，报错带实际
    // 大小，且经 diagScope 携带方法上下文。
    std::string source =
        ".class public super demo/T {\n"          //行 1
        "\n"
        "    .method public static big ()V {\n"   //行 3
        "        code: {\n";
    for (int i = 0; i < 66000; ++i) source += "            nop\n";
    source +=
        "            return\n"
        "        }\n"
        "    }\n"
        "}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    const std::string msg = messageOf(result.errors().front());
    EXPECT_NE(msg.find("66001"), std::string::npos) << msg;
    EXPECT_NE(msg.find("65535"), std::string::npos) << msg;
    EXPECT_NE(msg.find("big"), std::string::npos) << msg;
}

TEST(assemble_code_size, far_goto_compiles_via_auto_widening)
{
    // goto 与目标相距 33000+ 字节：libjbc 导出期把 goto 加宽为 goto_w
    // （fixpoint，模型不动）。此前该形态在 lower 报 branch overflow。
    std::string source =
        ".class public super demo/T {\n"
        "\n"
        "    .method public static far ()V {\n"
        "        code: {\n"
        "            goto L0\n";
    for (int i = 0; i < 33000; ++i) source += "            nop\n";
    source +=
        "            L0:\n"
        "            return\n"
        "        }\n"
        "    }\n"
        "}";
    const auto result = jhasm::assemble(u16("t.jasm"), u16(source), {});
    ASSERT_TRUE(result.has_value());
    for (const auto& e : result.errors()) {
        ADD_FAILURE() << messageOf(e);
    }
    ASSERT_FALSE(result.get()->bytes.empty());
}
