#include "jvm_opcodes.hpp"

#include <libca/str/utf8_string.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace jhasm::compile {

namespace {

/// @brief 助记符反查表，一次构建：键 = 小写助记符。
///
/// 单源 = libjbc kMnemonic（mnemonicOf 覆盖 0x00–0xC9 全量 202 项，本身小写），
/// jasm 的 invoke*interface 扩展也进入该表，接口分派语义由 intern 面区分。
const std::unordered_map<std::string, C::Op>& opcodeTable()
{
    static const std::unordered_map<std::string, C::Op> table = [] {
        std::unordered_map<std::string, C::Op> t;
        t.reserve(0xCA + 3);
        for (ca::u32 b = 0; b < 0xCA; ++b) {
            //0xC4 wide 是修饰前缀不是独立指令：jasm 面无显式 wide（宽索引由
            //装载/变量面自动升格），保留旧栈口径，源文本显式 wide 拒绝。
            if (b == 0xC4) continue;
            t.emplace(C::mnemonicOf(static_cast<C::Op>(b)), static_cast<C::Op>(b));
        }
        t["invokevirtualinterface"] = C::Op::INVOKEINTERFACE;
        t["invokestaticinterface"]  = C::Op::INVOKESTATIC;
        t["invokespecialinterface"] = C::Op::INVOKESPECIAL;
        return t;
    }();
    return table;
}

}   //namespace

C::Op opcode(ca::str::Utf16StringRef name, JvmDialect dialect)
{
    //jasm 助记符纯 ASCII：UTF-8 化后逐字节归一小写再查表（源面大小写不敏感）。
    std::string text = name.to_utf8_string().to_std_string();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto found = opcodeTable().find(text);
    if (found == opcodeTable().end())
        throw std::invalid_argument("Unknown JVM opcode: " + text);
    const auto op = found->second;
    if (dialect == JvmDialect::S && (op == C::Op::LDC_W || op == C::Op::LDC2_W))
        return C::Op::LDC;
    return op;
}

}   //namespace jhasm::compile
