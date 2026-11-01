/// @file        error_path_test.cpp
/// @brief       错误路径契约：assemble 失败面报错可归属（成员上下文 + jasm
///              位置）、disassemble 对垃圾字节显式失败，绝无静默成功。
#include <gtest/gtest.h>

#include "jhasm/jhasm.hpp"

#include <libca/str/utf16_string.hpp>
#include <libca/str/utf8_string.hpp>

#include <string>
#include <vector>

namespace {

ca::str::Utf16String u16(const char* ascii)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(ascii), std::string(ascii).size()));
}

std::string messageOf(const jhasm::error::Error& error)
{
    return error.message().to_utf8_string().to_std_string();
}

bool hasLine(const jhasm::error::Error& error, int line)
{
    return error.location() != nullptr && error.location()->line() == line;
}

TEST(assemble_error, unknown_instruction_reports_source_line)
{
    constexpr const char* source =
        ".class public super demo/T {\n"     //行 1
        "\n"
        "    .method public static foo (I)V {\n"  //行 3
        "        code: {\n"
        "            iload 0\n"
        "            badop_xyz\n"            //行 6
        "            return\n"
        "        }\n"
        "    }\n"
        "}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    EXPECT_EQ(messageOf(result.errors().front()), "Unknown JVM instruction");
    EXPECT_TRUE(hasLine(result.errors().front(), 6));
}

TEST(assemble_error, bad_field_descriptor_carries_member_context)
{
    constexpr const char* source =
        ".class public super demo/T {\n"     //行 1
        "\n"
        "    .field private count ZZZ\n"     //行 3
        "}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    EXPECT_NE(messageOf(result.errors().front()).find("in field count ZZZ"),
              std::string::npos);
    EXPECT_TRUE(hasLine(result.errors().front(), 3));
}

TEST(assemble_error, bad_method_descriptor_carries_member_context)
{
    constexpr const char* source =
        ".class public super demo/T {\n"     //行 1
        "\n"
        "    .method public static foo (V)V {\n"   //行 3：V 不是合法参数类型
        "        code: { return }\n"
        "    }\n"
        "}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    EXPECT_NE(messageOf(result.errors().front()).find("Invalid method descriptor"),
              std::string::npos);
    EXPECT_NE(messageOf(result.errors().front()).find("in method foo"), std::string::npos);
}

TEST(assemble_error, unbalanced_stack_fails_frame_analysis)
{
    constexpr const char* source =
        ".class public super demo/T {\n"     //行 1
        "\n"
        "    .method public static foo ()V {\n"   //行 3
        "        code: {\n"
        "            pop\n"                  //空栈弹栈：帧分析必须拒绝
        "            return\n"
        "        }\n"
        "    }\n"
        "}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    EXPECT_NE(messageOf(result.errors().front()).find("Stack frame analysis failed"),
              std::string::npos);
    EXPECT_NE(messageOf(result.errors().front()).find("in method foo"), std::string::npos);
}

TEST(assemble_error, unexpected_operand_count_is_rejected)
{
    constexpr const char* source =
        ".class public super demo/T {\n"
        "\n"
        "    .method public static foo ()V {\n"
        "        code: {\n"
        "            bipush\n"               //缺操作数
        "            return\n"
        "        }\n"
        "    }\n"
        "}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    EXPECT_EQ(messageOf(result.errors().front()), "Unexpected JVM instruction operand count");
}

TEST(assemble_error, multiple_class_declarations_rejected)
{
    constexpr const char* source =
        ".class public super demo/A {\n}\n"
        ".class public super demo/B {\n}";
    const auto result = jhasm::assemble(u16("t.jasm").ref(), u16(source).ref(), {});
    ASSERT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    EXPECT_NE(messageOf(result.errors().front()).find("exactly one class"),
              std::string::npos);
}

TEST(disassemble_error, garbage_bytes_fail_explicitly)
{
    const std::vector<ca::u8> garbage = {0x11, 0x22, 0x33, 0x44};
    const auto                result  = jhasm::disassemble(garbage);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.errors().empty());
}

}   //namespace
