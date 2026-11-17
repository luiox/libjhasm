.. _manual:

libjhasm 使用手册
=================

libjhasm 是处理 JVM classfile 和 JAsm 文本的 C++17 库。使用
``jhasm/jhasm.hpp`` 调用汇编、反汇编和帧分析接口。库的构建与依赖配置见
:doc:`development`，完整文本语法见 :doc:`syntax`。

汇编并查看生成的类
------------------

下面的程序把内嵌的 JAsm 汇编为 ``Demo.class``，然后将反汇编文本打印到标准输出。
将它保存为 ``demo.cpp``，编译时链接 ``libjhasm``。

.. code-block:: cpp

   #include "jhasm/jhasm.hpp"

   #include <libca/str/utf16_string.hpp>

   #include <fstream>
   #include <iostream>

   template <typename T>
   void printDiagnostics(const jhasm::error::Result<T>& result)
   {
       for (const auto& warning : result.warnings()) {
           std::cerr << "warning: "
                     << warning.to_string().to_utf8_string().to_std_string() << '\n';
       }
       for (const auto& error : result.errors()) {
           std::cerr << "error: "
                     << error.to_string().to_utf8_string().to_std_string() << '\n';
       }
   }

   int main()
   {
       using ca::str::Utf16String;
       const auto name = Utf16String::value_of("Demo.jasm");
       const auto source = Utf16String::value_of(R"jasm(.super java/lang/Object
   .class public Demo {
       .method public static answer ()I {
           code: {
               bipush -42
               ireturn
           }
       }
   }
   )jasm");

       const auto assembled = jhasm::assemble(name.ref(), source.ref());
       printDiagnostics(assembled);
       if (!assembled.is_ok()) return 1;

       const auto& bytes = assembled.get()->bytes;
       std::ofstream file("Demo.class", std::ios::binary);
       file.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
       file.close();
       if (!file) {
           std::cerr << "Could not write Demo.class\n";
           return 1;
       }

       const auto printed = jhasm::disassemble(bytes);
       printDiagnostics(printed);
       if (!printed.is_ok()) return 1;
       std::cout << printed.get()->to_utf8_string().to_std_string();
   }

按 :doc:`development` 完成依赖注册和配置后，可以把 ``demo.cpp`` 放到仓库根目录，
在 ``xmake.lua`` 末尾添加以下目标，再运行 ``xmake build libjhasm_demo`` 和
``xmake run libjhasm_demo``。

.. code-block:: lua

   target("libjhasm_demo")
       set_kind("binary")
       add_files("demo.cpp")
       add_deps("libjhasm")
       set_rundir("$(projectdir)")
       if is_plat("windows") then
           add_cxflags("/utf-8", {tools = "cl"})
       end
   target_end()

``Utf16String::value_of`` 将以 NUL 结尾的 UTF-8 文本转换为拥有型 UTF-16 字符串。
``assemble`` 接收源名称和源码的 UTF-16 视图，源名称用于定位诊断。程序使用缺省的
s 方言，示例中的 ``bipush -42`` 会反汇编为 ``ldc -42``。更改目标 classfile 版本
可设置 ``JvmCompilerOptions::majorVersion`` 和 ``minorVersion``，缺省为 61.0
（Java 17）。

读取整个 classfile 可将文件内容按二进制读入 ``std::vector<ca::u8>``，交给
``jhasm::disassemble``。只需展示某个方法时，调用
``jhasm::disassembleMethod(bytes, methodName, descriptor)``，用 UTF-8 视图传入方法名
和描述符，例如 ``answer`` 与 ``()I``。返回文本可以嵌入类声明。

选择 s 或 r 方言
----------------

- **s** 适合手写 JAsm 或查看规范化文本。无标记且未指定方言的源码默认按 s 汇编，
  无方言参数的反汇编也默认输出 s。
- **r** 适合保留指令形态或构造字节码测试样本。在源码第一行写 ``//! dialect r``，
  或者在无标记时将 ``JvmCompilerOptions::dialect`` 设为 ``jhasm::JvmDialect::R``。
  API 设定的方言与首行标记冲突时，汇编返回诊断。

``jhasm::disassemble(bytes, jhasm::JvmDialect::R)`` 输出带首行标记的完整类文本。
回读此输出时可以直接把 UTF-16 文本视图交给 ``assemble``。单方法反汇编也接受 r
方言参数。将 r 方法片段放入完整类后，应使用首行标记或 API 选项按 r 汇编整个类。

r 方言的 ``maxs`` 声明
----------------------

r 方言反汇编带 ``Code`` 属性的方法时会输出 ``maxs``。手写 r 文本时可省略
``maxs``，由缺省的 ``verify = true`` 路径运行帧分析并计算栈深和局部变量槽数。
显式给出 ``maxs`` 时，数值会写入 classfile。能够判定数值不足时，汇编结果携带警告。

如需构造含不合法字节码形态的测试样本，在 r 方言的汇编选项中设置 ``verify = false``，
并为每个含 ``code`` 的方法显式提供 ``maxs``。这一模式跳过帧分析和 StackMapTable
生成，仍检查结构和编码范围。以下是一个故意将操作数栈上限写成 0 的完整输入。
``bipush`` 需要一个栈槽，因此汇编会产生警告并保留给定值。

.. code-block:: text

   //! dialect r
   .super java/lang/Object
   .class public BadStack {
       .method public static answer ()I {
           maxs: { stack: 0, locals: 0 },
           code: {
               bipush -42
               ireturn
           }
       }
   }

s 方言始终运行帧分析。r 的指令保留范围、常量池索引限制和警告规则见
:doc:`design-dialects`。

处理诊断与分析帧
----------------

通过 ``errors()`` 和 ``warnings()`` 读取诊断。``is_ok()`` 表示有值且没有错误，
警告可以与有效结果同时出现。``location()`` 在有位置时返回源位置，
行、列从 1 开始，列按 UTF-16 码元计。``to_string()`` 可输出已格式化的诊断。

需要检查指令执行前的局部变量和操作数栈时，将 classfile 字节传给
``jhasm::analyze(bytes)``。结果的 ``methods`` 按方法保存名称、描述符和
``frames``；帧按指令顺序排列，不可达的指令对应空项。即使报告了方法分析错误，
结果中仍可能有其他方法的帧。读取这些部分结果时检查 ``get()``，并同时保留
``errors()`` 中的诊断。
