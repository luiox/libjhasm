#pragma once

// 访问旗语常量（JVMS 4.1 Table 4.1-B）——libjhasm 单源。编译面（jasm 修饰符
// 文本 → 旗语）与打印面（旗语 → 修饰符文本）共用；旧栈 jbytecode::core::Opcodes
// 的旗语面收编于此。

#include <libca/core/datatype.hpp>

namespace jhasm::util {

constexpr ca::i32 ACC_PUBLIC = 0x0001;
constexpr ca::i32 ACC_PRIVATE = 0x0002;
constexpr ca::i32 ACC_PROTECTED = 0x0004;
constexpr ca::i32 ACC_STATIC = 0x0008;
constexpr ca::i32 ACC_FINAL = 0x0010;
constexpr ca::i32 ACC_SUPER = 0x0020;
constexpr ca::i32 ACC_SYNCHRONIZED = 0x0020;
constexpr ca::i32 ACC_VOLATILE = 0x0040;
constexpr ca::i32 ACC_BRIDGE = 0x0040;
constexpr ca::i32 ACC_TRANSIENT = 0x0080;
constexpr ca::i32 ACC_VARARGS = 0x0080;
constexpr ca::i32 ACC_NATIVE = 0x0100;
constexpr ca::i32 ACC_INTERFACE = 0x0200;
constexpr ca::i32 ACC_ABSTRACT = 0x0400;
constexpr ca::i32 ACC_STRICT = 0x0800;
constexpr ca::i32 ACC_SYNTHETIC = 0x1000;
constexpr ca::i32 ACC_ANNOTATION = 0x2000;
constexpr ca::i32 ACC_ENUM = 0x4000;
constexpr ca::i32 ACC_MODULE = 0x8000;
constexpr ca::i32 ACC_RECORD = 0x10000;

}   //namespace jhasm::util
