/// @file        dialect_test.cpp
/// @brief       R/S 方言的指令编码、往返文本与诊断契约。
#include <gtest/gtest.h>

#include "jhasm/jhasm.hpp"

#include <jbc/core/reader.hpp>
#include <libca/str/utf16_string.hpp>
#include <libca/str/utf8_string.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using jhasm::JvmDialect;
using jhasm::compile::JvmCompilerOptions;

constexpr std::string_view rawMarker = "//! dialect r\n";

ca::str::Utf16String u16(std::string_view text)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(text.data()), text.size()));
}

std::string classWithCode(std::string_view code, std::string_view maxs = {},
                          std::string_view descriptor = "()V")
{
    return ".super java/lang/Object\n.class public super DialectTest {\n"
           "    .method public static test " +
           std::string(descriptor) + " {\n" + std::string(maxs) + "        code: {\n" +
           std::string(code) + "        }\n    }\n}\n";
}

auto assemble(std::string_view source, JvmCompilerOptions options = {})
{
    return jhasm::assemble(u16("dialect.jasm").ref(), u16(source).ref(), options);
}

struct DecodedCode
{
    std::vector<ca::u8> bytes;
    ca::u16 stack;
    ca::u16 locals;
    bool hasStackMap;
};

std::optional<DecodedCode> readCode(const std::vector<ca::u8>& bytes)
{
    auto parsed = jbc::core::readClass({bytes.data(), bytes.size()});
    if (parsed.is_err()) return std::nullopt;
    auto owner = std::move(parsed).unwrap();
    auto& file = *owner.file;
    if (file.methods.size() != 1) return std::nullopt;
    auto decoded = file.decodeCode(file.methods.front());
    if (decoded.is_err()) return std::nullopt;
    auto* code = decoded.unwrap();
    if (!code) return std::nullopt;
    const bool hasStackMap = std::any_of(code->attrs.begin(), code->attrs.end(),
        [&](const auto& attr) { return file.utf8(attr.nameCp) == "StackMapTable"; });
    return DecodedCode{std::move(code->code), code->maxStack, code->maxLocals, hasStackMap};
}

template<typename T>
std::string diagnostics(const jhasm::error::Result<T>& result)
{
    std::string text;
    for (const auto& error : result.errors())
        text += error.to_string().to_utf8_string().to_std_string() + "\n";
    return text;
}

TEST(jvm_dialect, raw_instruction_forms_and_wide_prefix_survive_round_trip)
{
    const std::string source = std::string(rawMarker) + classWithCode(
        "            iload 0\n"
        "            pop\n"
        "            iload_0\n"
        "            pop\n"
        "            wide iload 0\n"
        "            pop\n"
        "            wide iinc 0 1\n"
        "            bipush -128\n"
        "            pop\n"
        "            sipush -32768\n"
        "            pop\n"
        "            ldc 37\n"
        "            pop\n"
        "            ldc_w 38\n"
        "            pop\n"
        "            ldc2_w 39L\n"
        "            pop2\n"
        "            goto_w Done\n"
        "        Done:\n"
        "            return\n", {}, "(I)V");
    const auto assembled = assemble(source);
    ASSERT_TRUE(assembled.is_ok()) << diagnostics(assembled);
    const auto code = readCode(assembled.get()->bytes);
    ASSERT_TRUE(code.has_value());
    ASSERT_EQ(code->bytes.size(), 40u);
    ASSERT_TRUE(code->hasStackMap);
    auto actual = code->bytes;
    // Pool indices are assigned during assembly. Keep the instruction bytes and mask only indices.
    for (std::size_t i : {24u, 27u, 28u, 31u, 32u}) actual[i] = 0;
    EXPECT_EQ(actual, (std::vector<ca::u8>{
        0x15, 0x00, 0x57, 0x1a, 0x57,           // iload / iload_0
        0xc4, 0x15, 0x00, 0x00, 0x57,           // wide iload
        0xc4, 0x84, 0x00, 0x00, 0x00, 0x01,     // wide iinc
        0x10, 0x80, 0x57, 0x11, 0x80, 0x00, 0x57, // bipush / sipush
        0x12, 0x00, 0x57, 0x13, 0x00, 0x00, 0x57, 0x14, 0x00, 0x00, 0x58,
        0xc8, 0x00, 0x00, 0x00, 0x05, 0xb1}));   // goto_w / return

    const auto printed = jhasm::disassemble(assembled.get()->bytes, JvmDialect::R);
    ASSERT_TRUE(printed.is_ok()) << diagnostics(printed);
    const auto text = printed.get()->to_utf8_string().to_std_string();
    EXPECT_EQ(text.find(rawMarker), 0u);
    EXPECT_NE(text.find("wide iload 0\n"), std::string::npos) << text;
    EXPECT_NE(text.find("wide iinc 0 1\n"), std::string::npos) << text;
    EXPECT_NE(text.find("goto_w "), std::string::npos) << text;
    const auto method = jhasm::disassembleMethod(assembled.get()->bytes,
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>("test"), 4),
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>("(I)V"), 4),
        JvmDialect::R);
    ASSERT_TRUE(method.is_ok()) << diagnostics(method);
    const auto methodText = method.get()->to_utf8_string().to_std_string();
    EXPECT_EQ(methodText.find(rawMarker), std::string::npos) << methodText;
    EXPECT_NE(methodText.find("maxs: { stack: 2, locals: 1 }"), std::string::npos) << methodText;
    EXPECT_NE(methodText.find("bipush -128\n"), std::string::npos) << methodText;
    EXPECT_NE(methodText.find("sipush -32768\n"), std::string::npos) << methodText;
    const auto rebuilt = assemble(text);
    ASSERT_TRUE(rebuilt.is_ok()) << diagnostics(rebuilt);
    const auto printedAgain = jhasm::disassemble(rebuilt.get()->bytes, JvmDialect::R);
    ASSERT_TRUE(printedAgain.is_ok()) << diagnostics(printedAgain);
    EXPECT_EQ(printedAgain.get()->to_utf8_string().to_std_string(), text);
}

TEST(jvm_dialect, simplified_printer_normalizes_native_push_instructions)
{
    const auto source = classWithCode(
        "            bipush -128\n"
        "            pop\n"
        "            sipush -32768\n"
        "            pop\n"
        "            return\n");
    JvmCompilerOptions raw;
    raw.dialect = JvmDialect::R;
    const auto native = assemble(source, raw);
    ASSERT_TRUE(native.is_ok()) << diagnostics(native);
    const auto printed = jhasm::disassemble(native.get()->bytes);
    ASSERT_TRUE(printed.is_ok()) << diagnostics(printed);
    const auto text = printed.get()->to_utf8_string().to_std_string();
    EXPECT_EQ(text.find("bipush"), std::string::npos) << text;
    EXPECT_EQ(text.find("sipush"), std::string::npos) << text;
    EXPECT_NE(text.find("ldc -128\n"), std::string::npos) << text;
    EXPECT_NE(text.find("ldc -32768\n"), std::string::npos) << text;

    // The marker is recognized only on the first line. Default S still normalizes assembly.
    const auto simplified = assemble("// ordinary comment\n" + std::string(rawMarker) + source);
    ASSERT_TRUE(simplified.is_ok()) << diagnostics(simplified);
    const auto code = readCode(simplified.get()->bytes);
    ASSERT_TRUE(code.has_value());
    ASSERT_EQ(code->bytes.size(), 7u);
    EXPECT_EQ(code->bytes[0], 0x12);
    EXPECT_EQ(code->bytes[3], 0x12);
    const auto rebuilt = assemble(text);
    ASSERT_TRUE(rebuilt.is_ok()) << diagnostics(rebuilt);
    const auto printedAgain = jhasm::disassemble(rebuilt.get()->bytes);
    ASSERT_TRUE(printedAgain.is_ok()) << diagnostics(printedAgain);
    EXPECT_EQ(printedAgain.get()->to_utf8_string().to_std_string(), text);
}

TEST(jvm_dialect, explicit_dialect_conflict_points_to_first_line)
{
    JvmCompilerOptions options;
    options.dialect = JvmDialect::S;
    const auto result = assemble(std::string(rawMarker) + classWithCode("return\n"), options);
    EXPECT_FALSE(result.has_value());
    ASSERT_FALSE(result.errors().empty());
    const auto* location = result.errors().front().location();
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->line(), 1);
    EXPECT_EQ(location->column(), 1);
}

TEST(jvm_dialect, ldc_index_overflow_is_an_error_in_both_raw_verify_modes)
{
    std::string body;
    for (int value = 0; value < 260; ++value)
        body += "ldc_w " + std::to_string(value) + "\npop\n";
    constexpr auto maxs = "maxs: { stack: 1, locals: 0 },\n";
    for (bool verify : {true, false}) {
        SCOPED_TRACE(verify);
        JvmCompilerOptions options;
        options.dialect = JvmDialect::R;
        options.verify = verify;
        const auto tooNarrow = assemble(classWithCode(body + "ldc 90000\npop\nreturn\n", maxs),
                                        options);
        EXPECT_FALSE(tooNarrow.has_value());
        EXPECT_NE(diagnostics(tooNarrow).find("ldc_w"), std::string::npos)
            << diagnostics(tooNarrow);
        const auto wideEnough = assemble(classWithCode(body + "ldc_w 90000\npop\nreturn\n", maxs),
                                         options);
        EXPECT_TRUE(wideEnough.is_ok()) << diagnostics(wideEnough);
    }
}

TEST(jvm_dialect, lookupswitch_preserves_order_only_in_raw_mode)
{
    constexpr auto body =
        "            iconst_0\n"
        "            lookupswitch { 7: Done, -2: Done, default: Done }\n"
        "        Done:\n"
        "            return\n";
    const auto simplified = assemble(classWithCode(body));
    ASSERT_TRUE(simplified.is_ok()) << diagnostics(simplified);
    const auto sorted = readCode(simplified.get()->bytes);
    ASSERT_TRUE(sorted.has_value());
    ASSERT_EQ(sorted->bytes.size(), 29u);
    EXPECT_EQ(sorted->bytes[15], 0xfe); // -2, then 7
    EXPECT_EQ(sorted->bytes[23], 7);

    JvmCompilerOptions raw;
    raw.dialect = JvmDialect::R;
    const auto source = classWithCode(body, "maxs: { stack: 1, locals: 0 },\n");
    const auto rejected = assemble(source, raw);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_NE(diagnostics(rejected).find("lookupswitch"), std::string::npos)
        << diagnostics(rejected);

    raw.verify = false;
    const auto accepted = assemble(source, raw);
    ASSERT_TRUE(accepted.is_ok()) << diagnostics(accepted);
    const auto unsorted = readCode(accepted.get()->bytes);
    ASSERT_TRUE(unsorted.has_value());
    ASSERT_EQ(unsorted->bytes.size(), 29u);
    EXPECT_EQ(unsorted->bytes[15], 7);
    EXPECT_EQ(unsorted->bytes[23], 0xfe);
    const auto printed = jhasm::disassemble(accepted.get()->bytes, JvmDialect::R);
    ASSERT_TRUE(printed.is_ok()) << diagnostics(printed);
    const auto text = printed.get()->to_utf8_string().to_std_string();
    ASSERT_NE(text.find("-2:"), std::string::npos) << text;
    EXPECT_LT(text.find("7:"), text.find("-2:")) << text;
}

TEST(jvm_dialect, tableswitch_uses_case_count_for_range_in_both_dialects)
{
    const auto source = classWithCode(
        "            iconst_0\n"
        "            tableswitch { min: -2, max: 100, cases: { Done, Done }, default: Done }\n"
        "        Done:\n"
        "            return\n");
    const auto simplified = assemble(source);
    ASSERT_TRUE(simplified.is_ok()) << diagnostics(simplified);
    const auto simplifiedCode = readCode(simplified.get()->bytes);
    ASSERT_TRUE(simplifiedCode.has_value());

    JvmCompilerOptions options;
    options.dialect = JvmDialect::R;
    const auto raw = assemble(source, options);
    ASSERT_TRUE(raw.is_ok()) << diagnostics(raw);
    const auto rawCode = readCode(raw.get()->bytes);
    ASSERT_TRUE(rawCode.has_value());
    EXPECT_EQ(rawCode->bytes, simplifiedCode->bytes);
    const auto printed = jhasm::disassemble(raw.get()->bytes, JvmDialect::R);
    ASSERT_TRUE(printed.is_ok()) << diagnostics(printed);
    const auto text = printed.get()->to_utf8_string().to_std_string();
    EXPECT_NE(text.find("min: -2, max: -1"), std::string::npos) << text;
}

TEST(jvm_dialect, explicit_insufficient_maxs_warns_and_preserves_declared_values)
{
    const std::string source = std::string(rawMarker) + classWithCode("iload_0\npop\nreturn\n",
        "maxs: { stack: 0, locals: 0 },\n", "(I)V");
    for (bool verify : {true, false}) {
        SCOPED_TRACE(verify);
        JvmCompilerOptions options;
        options.verify = verify;
        const auto assembled = assemble(source, options);
        ASSERT_TRUE(assembled.is_ok()) << diagnostics(assembled);
        std::string warnings;
        for (const auto& warning : assembled.warnings())
            warnings += warning.message().to_utf8_string().to_std_string() + "\n";
        EXPECT_NE(warnings.find("maxs stack"), std::string::npos) << warnings;
        EXPECT_NE(warnings.find("maxs locals"), std::string::npos) << warnings;
        const auto code = readCode(assembled.get()->bytes);
        ASSERT_TRUE(code.has_value());
        EXPECT_EQ(code->stack, 0);
        EXPECT_EQ(code->locals, 0);
    }
}

TEST(jvm_dialect, unchecked_raw_requires_maxs_and_checks_encoding_range)
{
    JvmCompilerOptions raw;
    raw.dialect = JvmDialect::R;
    raw.verify = false;
    const auto missing = assemble(classWithCode("return\n"), raw);
    EXPECT_FALSE(missing.has_value());
    EXPECT_NE(diagnostics(missing).find("maxs"), std::string::npos) << diagnostics(missing);
    for (bool verify : {true, false}) {
        SCOPED_TRACE(verify);
        raw.verify = verify;
        for (const auto* maxs : {"maxs: { stack: 65536, locals: 0 },\n",
                                 "maxs: { stack: 0, locals: -1 },\n"}) {
            SCOPED_TRACE(maxs);
            const auto invalid = assemble(classWithCode("return\n", maxs), raw);
            EXPECT_FALSE(invalid.has_value());
            EXPECT_NE(diagnostics(invalid).find("maxs"), std::string::npos) << diagnostics(invalid);
        }
    }
}

TEST(jvm_dialect, raw_verification_never_relaxes_operand_encoding_checks)
{
    constexpr std::pair<std::string_view, std::string_view> cases[] = {
        {"bipush 128", "bipush must be an integer"},
        {"iinc 0 32768", "iinc increment must be an integer"},
        {"multianewarray [[I 256", "multianewarray dimensions must be an integer"},
        {"line 65536", "line number must be an integer"},
        {"tableswitch { min: 0, max: 1, cases: { 3 }, default: Done }",
         "Invalid JVM instruction operand"},
        {"tableswitch { min: 2147483647, max: 1, cases: { Done, Done }, default: Done }",
         "tableswitch high is outside the signed 32-bit range"},
    };
    for (bool verify : {true, false}) {
        SCOPED_TRACE(verify);
        JvmCompilerOptions raw;
        raw.dialect = JvmDialect::R;
        raw.verify = verify;
        for (const auto& [instruction, expected] : cases) {
            SCOPED_TRACE(instruction);
            const auto source = classWithCode(std::string(instruction) + "\nDone:\nreturn\n",
                                              "maxs: { stack: 1, locals: 1 },\n");
            const auto result = assemble(source, raw);
            EXPECT_FALSE(result.has_value());
            EXPECT_NE(diagnostics(result).find(expected), std::string::npos) << diagnostics(result);
        }
    }
}

TEST(jvm_dialect, unchecked_raw_can_emit_invalid_code_without_stack_map)
{
    JvmCompilerOptions options;
    options.dialect = JvmDialect::R;
    constexpr auto maxs = "maxs: { stack: 2, locals: 0 },\n";
    for (const auto* body : {"pop\nreturn\n", "ldc_w 9L\npop2\nreturn\n"}) {
        SCOPED_TRACE(body);
        options.verify = true;
        const auto verified = assemble(classWithCode(body, maxs), options);
        EXPECT_FALSE(verified.has_value());
        options.verify = false;
        const auto unchecked = assemble(classWithCode(body, maxs), options);
        ASSERT_TRUE(unchecked.is_ok()) << diagnostics(unchecked);
        const auto code = readCode(unchecked.get()->bytes);
        ASSERT_TRUE(code.has_value());
        EXPECT_FALSE(code->hasStackMap);
        ASSERT_FALSE(code->bytes.empty());
        EXPECT_EQ(code->bytes.front(), *body == 'l' ? 0x13 : 0x57);
    }
}

TEST(jvm_dialect, simplified_mode_always_verifies_stack_frames)
{
    JvmCompilerOptions options;
    options.verify = false;
    const auto result = assemble(classWithCode("pop\nreturn\n"), options);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(diagnostics(result).find("Stack frame analysis failed"), std::string::npos)
        << diagnostics(result);
}

TEST(jvm_dialect, unchecked_raw_round_trips_zero_length_code)
{
    JvmCompilerOptions options;
    options.dialect = JvmDialect::R;
    constexpr auto maxs = "maxs: { stack: 3, locals: 7 },\n";
    const auto source = classWithCode("", maxs);
    EXPECT_FALSE(assemble(source, options).has_value());
    options.verify = false;
    const auto assembled = assemble(source, options);
    ASSERT_TRUE(assembled.is_ok()) << diagnostics(assembled);
    const auto& bytes = assembled.get()->bytes;
    auto parsed = jbc::core::readClass({bytes.data(), bytes.size()});
    ASSERT_FALSE(parsed.is_err());
    auto owner = std::move(parsed).unwrap();
    ASSERT_EQ(owner.file->methods.size(), 1u);
    const auto& attrs = owner.file->methods.front().attrs;
    const auto it = std::find_if(attrs.begin(), attrs.end(),
        [&](const auto& attr) { return owner.file->utf8(attr.nameCp) == "Code"; });
    ASSERT_NE(it, attrs.end());
    const auto code = owner.file->source().spanOrEmpty(it->data);
    const std::vector<ca::u8> empty = {0, 3, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(code.equals(jbc::core::ByteView{empty}));

    const auto printed = jhasm::disassemble(bytes, JvmDialect::R);
    ASSERT_TRUE(printed.is_ok()) << diagnostics(printed);
    const auto text = printed.get()->to_utf8_string().to_std_string();
    EXPECT_NE(text.find(maxs), std::string::npos) << text;
    const auto rebuilt = assemble(text, options);
    ASSERT_TRUE(rebuilt.is_ok()) << diagnostics(rebuilt);
    EXPECT_EQ(rebuilt.get()->bytes, bytes);
}

}   //namespace
