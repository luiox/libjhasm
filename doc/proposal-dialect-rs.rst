jasm r/s 双方言提案
===================

.. _proposal-dialect-motivation:

1. 动机
-------

现状从文本到字节是一条自动选形的路，共五处，都有源码定位：

1. ``ldc`` / ``ldc_w`` / ``ldc2_w`` 三拼写同语义，落地由常量类别与池索引宽度决定（``jvm_opcodes.cpp`` 别名表加 libjbc 写面）
2. 槽位 0 到 3 的装载存储无条件升格短格式，``iload 0`` 落 ``iload_0``\ （``code_compiler.cpp``）
3. ``bipush`` / ``sipush`` 编译期并入 ``ldc``，见 :ref:`syntax-insn-push`
4. ``lookupswitch`` 键编译期排序（``code_compiler.cpp``）
5. ``goto`` / ``jsr`` 偏移放不下时导出期自动加宽 ``_W``\ （libjbc ``code_dom.cpp``）

这套归一让文本好写、canonical 输出稳定，归一用途已经闭环。代价是文本不携带指令形态：写哪个助记符不决定落哪个 opcode。需要形态保真的场景现在没有通道：

- 语料复现：libjsim demo 语料（y.jasm）带着真实 classfile 的 ``bipush`` / ``sipush`` 原生形态，过汇编器会被并成 ``ldc``，产物与原始 dump 语义等价、形态不同
- 去混淆重建与字节级对拍：改写产物的指令序列希望与原文逐条对应
- 最小复现与测试：需要精确控制落地字节形态

方言分成 r（raw）与 s（simplified）两版。r 保留原始指令形态，s 延续现有的汇编归一规则。

.. _proposal-dialect-s:

2. s 方言（现状正式化）
-----------------------

s 沿用现有的汇编规则：

- ldc 族三拼写都收，语义由常量类别决定；long/double 恒走 cat2 形态，池索引超 255 自动升格
- ``bipush`` / ``sipush`` 合法，编译期并入 ``ldc``
- 槽位短格式自动升格，``lookupswitch`` 键排序
- 帧分析 Strict 策略照跑，SMT 按代次生成

s 反汇编把外部 classfile 中的原生 ``bipush`` / ``sipush`` 打印成 ``ldc``，操作数为指令表示的有符号整数。回读这段 s 文本再反汇编，仍打印同样的 ``ldc`` 文本。

现有语料、smoke、单元测试全部落在 s。首行无方言标记且 API 未指定方言时按 s 解释，见 :ref:`proposal-dialect-declare`。

.. _proposal-dialect-r:

3. r 方言（原始形态）
---------------------

r 按显式助记符选择 opcode。下表对照两种方言的编码规则。

.. list-table::
   :header-rows: 1

   * - 归一点
     - s 行为
     - r 行为
   * - ``ldc`` / ``ldc_w`` / ``ldc2_w``
     - 同语义，落地自动选形
     - 三种拼写各自对应一个 opcode。重建后 ``ldc`` 的常量池索引若超过 255，两档 verify 均报错，须显式改写为 ``ldc_w``。``ldc_w`` 和 ``ldc2_w`` 使用 u2 索引。
   * - ``bipush`` / ``sipush``
     - 并入 ``ldc``
     - 按写的落地
   * - 槽位装载存储
     - 槽 0 到 3 升格短格式
     - ``iload 0`` 落长格式，``iload_0`` 落短格式，不互换
   * - ``lookupswitch`` 键序
     - 编译期排序
     - 按原键序编码。verify 开时要求键严格升序，否则报错。verify 关时接受任意键序，可生成带乱序键的非法 classfile。
   * - ``tableswitch`` 范围
     - ``low = min``，``high = min + cases 数 - 1``
     - 同 s，按 ``cases`` 中的目标顺序编码。
   * - ``goto`` / ``jsr``
     - 溢出自动加宽
     - 显式 ``goto_w`` / ``jsr_w`` 保形。``goto`` / ``jsr`` 仅在偏移超出短指令编码范围时加宽。

r 反汇编按 classfile 中的 opcode 和操作数打印，保留 ``bipush`` / ``sipush``、``ldc_w`` / ``ldc2_w``、槽位装载存储的长短格式以及 ``lookupswitch`` 的原始键序。

verify 分两档：

- verify 开时做 JVMS 合法性检查。``ldc`` / ``ldc_w`` 操作数必须是 cat1 可装载常量，``ldc2_w`` 操作数必须是 long 或 double。槽位、分支目标和 switch 结构继续检查，帧分析照跑，SMT 照生成。显式 ``maxs`` 不足时按 :ref:`proposal-dialect-maxs` 的警告规则处理。
- verify 关时只以语法、结构和编码检查作为输出门槛。允许可编码的非法形态，例如 ``ldc_w`` 装载 cat2 常量、乱序 ``lookupswitch`` 键和空方法体。跳过帧分析和 SMT 生成。

.. _proposal-dialect-declare:

4. 声明与默认
-------------

方言标记采用首行注释 ``//! dialect r``。上游 Jasm 与 Recaf 按注释跳过。本仓解析器只识别首行标记，其余位置按普通注释处理。s printer 输出无标记文本，r printer 在首行输出标记。

汇编 API 的 ``dialect`` 字段可留空。首行有标记时，文本声明方言，API 显式指定不同方言时报错。首行无标记时，使用 API 指定的方言，字段留空则采用 s。``verify`` 由汇编选项独立设置。使用缺省选项的调用方回读 r printer 的输出时，按 r 解析。

.. _proposal-dialect-maxs:

5. r 模式的伴生形态：maxs
-------------------------

方法的 ``Code`` 属性携带 ``max_stack`` 和 ``max_locals``，现由帧分析算出。r 在方法体对象中用 ``maxs`` 指定这两项。

.. code-block:: text

   maxs: { stack: 3, locals: 7 },

r printer 对每个带 ``Code`` 属性的方法输出一行 ``maxs``，值取自该属性。显式值须在 u2 范围内，汇编器将其原样写入。verify 开时运行帧分析，省略 ``maxs`` 则写入分析结果。verify 关时必须显式给出 ``maxs``，缺失时报错。

显式 ``stack`` 或 ``locals`` 小于所需值时，汇编器给出警告，仍使用文本值生成 classfile。verify 开时以帧分析结果对比。verify 关时按可确定的下界告警，例如已知栈效果的执行路径达到的栈深度、参数和局部变量指令所需的槽位。警告检查跳过无法确定的部分，始终允许按给定值输出。

.. _proposal-dialect-contract:

6. 契约边界
-----------

r 一期保留可编码的指令形态，包括 opcode、槽位、分支形式与 switch 键序。``goto`` / ``jsr`` 的偏移超出短指令编码范围时才加宽，printer 如实打印宽指令。

``ldc`` 只能编码 u1 常量池索引。重建常量池后，索引超过 255 时两档 verify 均报错，用户须显式改写成 ``ldc_w``。``ldc_w`` 与 ``ldc2_w`` 的索引字段均为 u2。

r 一期不承诺整文件字节全等，两条原因：

1. 常量池由重建分配索引，原始池顺序不保留，ldc 操作数里的池索引可能不同
2. StackMapTable 原样携带依赖池保序，不在一期范围

整文件字节保真由 libjbc 会话保真写回提供。

.. _proposal-dialect-plan:

7. 实施面
---------

改动集中在 libjhasm，一期预期零 libjbc 改动：

- 读侧识别首行标记，在 ``jvm_opcodes.cpp`` 按方言解析 ldc 族，r 模式的三个拼写各自映射到对应 opcode。
- 编译侧在 ``code_compiler.cpp`` 按方言选择短格式升格、push 并入和 switch 排序，落实 r 的编码检查和 ``maxs`` 警告。``JvmCompilerOptions`` 增加 ``dialect`` 和 ``verify``。
- printer 在 r 模式保留第 3 节列出的指令形态，输出首行方言标记和各方法的 ``maxs``。s printer 将外部原生 ``bipush`` / ``sipush`` 打印成 ``ldc``。
- 核对清单：libjbc 导出面确认只加宽不缩窄，显式 ``goto_w`` 不被改写；r 模式 LDC_W、BIPUSH 等 op 能原样写出

落地后 syntax.rst 加方言一节，第 5、6 节的归一规则标注方言归属。

.. _proposal-dialect-compat:

8. 兼容性
---------

缺省 s，现有汇编语料继续按 s 解释。首行带 ``//! dialect r`` 的文本按 r 解释，无标记文本可由 API 选用 r。两方言的往返测试各自进 smoke，覆盖外部原生 push 指令在 s 下的文本稳定性及在 r 下的形态保真。
