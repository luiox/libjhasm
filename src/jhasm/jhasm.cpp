#include "jhasm.hpp"

#include <string_view>

namespace jhasm {
namespace {

constexpr std::string_view rawMarker = "//! dialect r";

bool hasRawMarker(ca::str::Utf16StringRef source) noexcept
{
    if (source.length() < rawMarker.size()) return false;
    for (ca::usize index = 0; index < rawMarker.size(); ++index) {
        if (source.char_at(index).unit() != rawMarker[index]) return false;
    }
    for (ca::usize index = rawMarker.size(); index < source.length(); ++index) {
        const auto unit = source.char_at(index).unit();
        if (unit == '\r' || unit == '\n') return true;
        if (unit != ' ' && unit != '\t') return false;
    }
    return true;
}

}   //namespace

const char* get_lib_version() noexcept
{
    return "0.3.0";
}

const char* get_upstream_revision() noexcept
{
    return "aacebfa8b06a5c478adf13d9ce8e0e9f896ba840";
}

error::Result<compile::JvmCompileResult> assemble(ca::str::Utf16StringRef     sourceName,
                                                  ca::str::Utf16StringRef     source,
                                                  compile::JvmCompilerOptions options)
{
    if (hasRawMarker(source)) {
        if (options.dialect && options.dialect != JvmDialect::R) {
            const auto message = ca::str::Utf16String::value_of(
                "Dialect marker conflicts with the explicit assembler dialect");
            const util::Location location(1, 1, static_cast<ca::i32>(rawMarker.size()), sourceName);
            return error::Result<compile::JvmCompileResult>::err(
                {error::Error(message.ref(), location)});
        }
        options.dialect = JvmDialect::R;
    }
    auto tokens = parser::Tokenizer{}.tokenize(sourceName, source);
    if (!tokens.is_ok())
        return error::Result<compile::JvmCompileResult>(
            std::nullopt, tokens.errors(), tokens.warnings());
    auto declarations = parser::DeclarationParser{}.parseAny(*tokens.get());
    if (declarations.has_err())
        return error::Result<compile::JvmCompileResult>(std::nullopt, declarations.errors(), {});
    auto processed = parser::ASTProcessor{}.process(declarations.get());
    if (!processed.is_ok())
        return error::Result<compile::JvmCompileResult>(
            std::nullopt, processed.errors(), processed.warnings());
    return compile::JvmCompiler(options).compile(*processed.get());
}

error::Result<ca::str::Utf16String> disassemble(const std::vector<ca::u8>& classBytes)
{
    return disassemble(classBytes, JvmDialect::S);
}

error::Result<ca::str::Utf16String> disassemble(const std::vector<ca::u8>& classBytes,
                                                JvmDialect dialect)
{
    return printer::JvmPrinter{}.print(classBytes, dialect);
}

error::Result<ca::str::Utf16String> disassembleMethod(const std::vector<ca::u8>&    classBytes,
                                                       ca::str::Utf8StringRef methodName,
                                                       ca::str::Utf8StringRef descriptor)
{
    return disassembleMethod(classBytes, methodName, descriptor, JvmDialect::S);
}

error::Result<ca::str::Utf16String> disassembleMethod(const std::vector<ca::u8>& classBytes,
                                                     ca::str::Utf8StringRef methodName,
                                                     ca::str::Utf8StringRef descriptor,
                                                     JvmDialect dialect)
{
    return printer::JvmPrinter{}.printMethod(classBytes, methodName, descriptor, dialect);
}

error::Result<analysis::JvmAnalysisResult> analyze(const std::vector<ca::u8>&          classBytes,
                                                   const analysis::InheritanceChecker& inheritance)
{
    return analysis::JvmAnalyzer(inheritance).analyze(classBytes);
}

}   //namespace jhasm
