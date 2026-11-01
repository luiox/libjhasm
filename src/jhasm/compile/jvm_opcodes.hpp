#pragma once

// jasm 助记符 → libjbc Op 映射（编译面唯一入口）。反查表单源 = libjbc
// mnemonicOf 全量码表（小写）；jasm 扩展助记符（invoke*interface 族）与
// ldc_w/ldc2_w 旧栈别名在表构建时并入。大小写不敏感。

#include <jbc/jbc.hpp>

#include <libca/str/utf16_string.hpp>

namespace jhasm::compile {

namespace C = jbc::core;

///@brief jasm 助记符 → Op；未知助记符抛 std::invalid_argument（带助记符文本）。
C::Op opcode(ca::str::Utf16StringRef name);

}   //namespace jhasm::compile
