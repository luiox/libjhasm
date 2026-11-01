#pragma once

// jasm 助记符 → libjbc Op 映射（编译面唯一入口）。jasm 小写助记符先经
// opcodeUpper 归一大写再查表；jasm 扩展助记符（LDC_W/INVOKEVIRTUALINTERFACE
// 等旧栈别名）在表外特判。

#include <jbc/jbc.hpp>

#include <libca/str/utf16_string.hpp>

namespace jhasm::compile {

namespace C = jbc::core;

///@brief 大写助记符 → Op；未知助记符抛 std::invalid_argument。
C::Op opcode(ca::str::Utf16StringRef name);

///@brief 助记符大小写归一后查表（jasm 文本面一律小写，libjbc 名称面一律大写）。
C::Op opcodeUpper(ca::str::Utf16StringRef name);

}   //namespace jhasm::compile
