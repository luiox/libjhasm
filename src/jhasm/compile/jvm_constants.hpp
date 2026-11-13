#pragma once

// jasm 常量字面 → CP 落位面（编译面内部头）：Jasm 常量规格（ConstSpec，含
// Dynamic 族递归）与方法句柄规格的解析、CP intern、常量装载规范化。

#include "jvm_compile_detail.hpp"

namespace jhasm::compile {

// 常量规格（Jasm 常量字面 → CP 落位前的事实描述；Dynamic 族递归）

struct ConstSpec final
{
    enum class Kind : ca::u8 { Int, Long, Float, Double, String, Class, MethodType, MethodHandle, Dynamic };
    Kind        kind = Kind::Int;
    ca::i64     num  = 0;
    ca::u32     fbits = 0;
    ca::u64     dbits = 0;
    //字符串族一律借用 SpecArena（下游 BsmArg/MethodHandleSpec 也是借用型）。
    std::string_view str;

    IR::MethodHandleSpec handle;

    std::string_view          dynamicName;
    std::string_view          dynamicDesc;
    IR::MethodHandleSpec      dynamicHandle;
    std::vector<ConstSpec>    dynamicArgs;
};

/// @brief AST 节点 → 方法句柄规格（快捷名或 [kind, path, descriptor] 三元数组）。
IR::MethodHandleSpec methodHandle(const ast::ElementPtr& value, SpecArena& arena);

/// @brief AST 节点 → 常量规格（数字/字符串/字符/布尔/NaN/Infinity/类型字面/
///       方法句柄/indy 动态常量）。
ConstSpec constantValue(const ast::ElementPtr& value, SpecArena& arena);

/// @brief 常量规格 → CP 索引（语义 intern：命中既有池不膨胀）。
ca::u32 internConstant(IR::ClassSession& session, const ConstSpec& value);

/// @brief 常量装载规范化落码（沿旧栈 jblw 写出口径）：int/float 一律落 ldc
/// （索引超 u1 域升格 ldc_w），long/double 一律落 ldc2_w（cat2 常量 JVM 只认
/// ldc2_w）。bipush/sipush/ldc 族文本并入同一语义常量后不再保形。
IR::Insn constantLoad(IR::ClassSession& session, const ConstSpec& value);

/// @brief 常量规格 → BSM 参数（Dynamic 族递归落嵌套 BSM）。
IR::BsmArg bsmArgOf(IR::ClassSession& session, const ConstSpec& value);

}   //namespace jhasm::compile
