#include "jvm_opcodes.hpp"

#include <libca/str/utf8_string.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace jhasm::compile {

namespace {

///@brief 助记符反查表，一次构建：键 = 小写助记符。
///
///单源 = libjbc kMnemonic（mnemonicOf 覆盖 0x00–0xC9 全量 202 项，本身小写），
///新增指令只进 libjbc 码表，此处零维护；一致性由 unittest 的漂移断言锁定
///（每个 Op 的 mnemonicOf 必须反查回自身，ldc 族别名除外）。
///
///表构建时并入两组非标准条目：
///1. ldc_w/ldc2_w → LDC：与 ldc 同一 CP 语义，装载面按索引宽度决定是否升格
///   ldc_w（旧栈 jblw 写出口径），压过 LDC_W/LDC2_W 自身条目；
///2. invoke*interface 族：jasm 对接口方法调用的显式拼写（itable 分派语义由
///   intern 面区分），libjbc 码表无此名。
const std::unordered_map<std::string, C::Op>& opcodeTable()
{
    static const std::unordered_map<std::string, C::Op> table = [] {
        std::unordered_map<std::string, C::Op> t;
        t.reserve(0xCA + 5);
        for (ca::u32 b = 0; b < 0xCA; ++b) {
            //0xC4 wide 是修饰前缀不是独立指令：jasm 面无显式 wide（宽索引由
            //装载/变量面自动升格），保留旧栈口径——源文本显式 wide 拒绝。
            if (b == 0xC4) continue;
            t.emplace(C::mnemonicOf(static_cast<C::Op>(b)), static_cast<C::Op>(b));
        }
        t["ldc_w"]                  = C::Op::LDC;
        t["ldc2_w"]                 = C::Op::LDC;
        t["invokevirtualinterface"] = C::Op::INVOKEINTERFACE;
        t["invokestaticinterface"]  = C::Op::INVOKESTATIC;
        t["invokespecialinterface"] = C::Op::INVOKESPECIAL;
        return t;
    }();
    return table;
}

}   //namespace

C::Op opcode(ca::str::Utf16StringRef name)
{
    //jasm 助记符纯 ASCII：UTF-8 化后逐字节归一小写再查表（源面大小写不敏感）。
    std::string text = name.to_utf8_string().to_std_string();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto found = opcodeTable().find(text);
    if (found == opcodeTable().end())
        throw std::invalid_argument("Unknown JVM opcode: " + text);
    return found->second;
}

}   //namespace jhasm::compile
