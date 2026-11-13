/// @file        opcode_table_test.cpp
/// @brief       助记符三表漂移断言。
///
/// jasm 助记符面有三张手工维护的表：libjbc kMnemonic（打印面源）、编译面反查
/// 表（jvm_opcodes）、jasm 语法签名表（jvm_instructions）。编译面反查表已单源
/// libjbc 码表，本文件锁定三表硬一致性：
///   1. 每个 Op 的 mnemonicOf 必须反查回自身（ldc 族别名与 wide 除外）；
///   2. 语法签名表里每个名字必须能被编译面映射（伪指令 line 除外）；
///   3. libjbc 码表里每个助记符必须在语法签名表里（wide 除外），上游新增
///      指令若只改 libjbc，此处即刻报警。
#include <gtest/gtest.h>

#include "jhasm/compile/jvm_opcodes.hpp"
#include "jhasm/instructions/jvm_instructions.hpp"

#include <jbc/jbc.hpp>

#include <libca/str/utf16_string.hpp>

#include <stdexcept>
#include <string>

namespace {

using jbc::core::Op;

ca::str::Utf16String u16(const std::string& ascii)
{
    return ca::str::Utf16String::value_of(ascii.c_str());
}

TEST(opcode_table, every_standard_mnemonic_reverse_maps)
{
    for (ca::u32 b = 0; b < 0xCA; ++b) {
        const auto      op       = static_cast<Op>(b);
        const char*     mnemonic = jbc::core::mnemonicOf(op);
        ASSERT_NE(mnemonic, nullptr);
        const std::string name(mnemonic);
        if (b == 0x13 || b == 0x14) {
            // ldc_w/ldc2_w 与 ldc 同一 CP 语义（装载面按索引宽度升格落码）。
            EXPECT_EQ(jhasm::compile::opcode(u16(name).ref()), Op::LDC) << name;
            continue;
        }
        if (b == 0xC4) {
            // wide 是修饰前缀不是独立指令：jasm 无显式 wide，编译面拒绝。
            EXPECT_THROW(jhasm::compile::opcode(u16(name).ref()), std::invalid_argument) << name;
            continue;
        }
        EXPECT_EQ(jhasm::compile::opcode(u16(name).ref()), op) << name;
    }
}

TEST(opcode_table, lookup_is_case_insensitive)
{
    EXPECT_EQ(jhasm::compile::opcode(u16("NoP").ref()), Op::NOP);
    EXPECT_EQ(jhasm::compile::opcode(u16("GOTO_W").ref()), Op::GOTO_W);
}

TEST(opcode_table, jasm_extensions_resolve)
{
    EXPECT_EQ(jhasm::compile::opcode(u16("invokevirtualinterface").ref()), Op::INVOKEINTERFACE);
    EXPECT_EQ(jhasm::compile::opcode(u16("invokestaticinterface").ref()), Op::INVOKESTATIC);
    EXPECT_EQ(jhasm::compile::opcode(u16("invokespecialinterface").ref()), Op::INVOKESPECIAL);
}

TEST(opcode_table, unknown_mnemonic_throws_with_text)
{
    try {
        jhasm::compile::opcode(u16("nosuchop").ref());
        FAIL() << "expected std::invalid_argument";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("nosuchop"), std::string::npos);
    }
}

TEST(opcode_table, syntax_table_names_all_compilable)
{
    for (const auto& spec : jhasm::instructions::JvmInstructions::instance().all()) {
        if (std::string(spec.name) == "line") continue;   //伪指令：行号声明，非 opcode
        EXPECT_NO_THROW(jhasm::compile::opcode(u16(spec.name).ref())) << spec.name;
    }
}

TEST(opcode_table, every_libjbc_mnemonic_present_in_syntax_table)
{
    for (ca::u32 b = 0; b < 0xCA; ++b) {
        if (b == 0xC4) continue;   //wide 前缀：jasm 面无显式形态
        const std::string name(jbc::core::mnemonicOf(static_cast<Op>(b)));
        const auto*       spec =
            jhasm::instructions::JvmInstructions::instance().find(u16(name).ref());
        ASSERT_TRUE(spec != nullptr) << name;
    }
}

}   //namespace
