.. _design:

libjhasm 架构设计
=================

.. _design-library-boundary:

库接口与依赖边界
----------------

调用方通过 ``jhasm/jhasm.hpp`` 使用汇编、反汇编和帧分析。文本接口使用 libca
的 UTF-16 字符串，classfile 接口使用字节向量。分析结果由 libjhasm 的帧和值类型
持有，可在内部字节码会话销毁后使用。

libca 是公开依赖。libjbc 在实现层负责 classfile 读写、常量池、字节码中间表示、
布局和帧计算。把这些工作集中在同一后端，使汇编使用的操作数宽度、分支布局和
StackMapTable 偏移共享同一套字节码语义。公开接口以 Jasm 层的选项、结果和诊断
表达契约，调用者不需要管理 libjbc 会话。

.. _design-assembly:

文本到 classfile
----------------

.. code-block:: text

   UTF-16 Jasm
       -> 方言选择
       -> Tokenizer -> DeclarationParser -> ASTProcessor
       -> JvmCompiler -> CodeCompiler
       -> libjbc 布局、属性与 classfile 写出

``Tokenizer`` 从 UTF-16 源文本提取 token 并记录位置。``DeclarationParser`` 解析
声明、值和指令块。``ASTProcessor`` 将通用声明转为 JVM 类、成员和属性，并根据
指令签名检查操作数。这个阶段保留标签、操作数文本和方法体结构，供编译阶段解析
槽位、常量、标签绑定和方言特有的编码选择。

``JvmCompiler`` 要求编译单元恰好包含一个类，并开启新的 libjbc 类会话。它写入类、
字段、方法签名及相应属性。``CodeCompiler`` 逐条发射方法指令，使用可延迟绑定的
标签句柄处理前向跳转。常量规格由编译侧解释并在会话的常量池中驻留。类名和
方法名等 classfile 字符串按 modified UTF-8 写入，Jasm 输入和输出仍使用 UTF-16。

通过后端布局取得实际指令 PC 后，校验路径执行 Strict 帧分析，并在需要时为
主版本号至少为 50 的方法生成 StackMapTable。行号表也按布局后的 PC 写入。
目标版本和继承关系查询由汇编选项提供。方言如何选择指令编码和校验路径见
:doc:`design-dialects`。

.. _design-read-paths:

反汇编与分析
------------

``JvmPrinter`` 和 ``JvmAnalyzer`` 各自从输入字节打开 classfile 会话。它们按需
解码属性，把方法的 Code 提升为带标签的指令模型，随后走各自的输出路径。

- 打印器将指令、标签、已支持的属性和常量渲染为 Jasm。完整类和单个方法使用
  同一套方法打印逻辑。完整类按约定的顺序输出顶层声明，单个方法通过名称和描述符
  定位重载。文本可重建的结构与边界见 :ref:`syntax-canonical-gap`。
- 分析器运行 Strict 帧计算，随后对可达指令的类型进行补充检查。``Frame`` 表示
  指令执行前的局部变量和操作数栈。按指令顺序排列的快照表允许缺项，比如不可达
  指令。无 Code 的声明会保留方法条目，帧列表为空。

继承查询由 ``InheritanceChecker`` 抽象。依赖类路径的调用方可以注入关系查询，
缺省实现提供适合无类路径分析的保守行为。独立分析不回写 classfile。

.. _design-diagnostics:

诊断与结果所有权
----------------

``error::Result`` 同时持有可选结果、错误和警告。使用分阶段接口时，结果可带有
部分值。汇编入口遇到错误即终止，成功产物也可附带警告。反汇编失败会给出错误
诊断。分析逐个方法累积诊断，已经取得的结果可随错误一同返回。

源诊断以 1 起算的行列和 UTF-16 码元长度定位。编译阶段在类、成员和指令之间
切换诊断上下文，返回的诊断持有自己的源名称、位置和消息。二进制输入错误通常
没有 Jasm 源位置。调用方式和处理示例见 :doc:`manual`。

.. _design-verification:

验证入口
--------

单测覆盖操作码表一致性、诊断、方法大小边界和方言的指令字节。冒烟测试用仓库
语料验证文本往返，使用可用的 Java 运行时加载 s 与 r 的汇编产物。构建两类测试
及文档检查的命令见 :doc:`development`。
