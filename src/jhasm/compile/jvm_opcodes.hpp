#pragma once

// jasm 助记符 → libjbc Op 映射（编译面唯一入口）。反查表单源 = libjbc
// mnemonicOf 全量码表（小写）；表中另有 jasm 的 invoke*interface 扩展。
// S 将 ldc_w/ldc2_w 归一为 ldc。大小写不敏感。

#include "jhasm/dialect.hpp"

#include <jbc/jbc.hpp>

#include <libca/str/utf16_string.hpp>

namespace jhasm::compile {

namespace C = jbc::core;

/// @brief jasm 助记符 → Op；未知助记符抛 std::invalid_argument（带助记符文本）。
C::Op opcode(ca::str::Utf16StringRef name, JvmDialect dialect = JvmDialect::S);

}   //namespace jhasm::compile
