jasm r/s 双方言提案
===================

.. _proposal-dialect-motivation:

1. 动机
-------

现状从文本到字节是一条自动选形的路，共五处，都有源码定位：

1. ``ldc``/``ldc_w``/``ldc2_w`` 三拼写同语义，落地由常量类别与池索引宽度决定（``jvm_opcodes.cpp`` 别名表加 libjbc 写面）
2. 槽位 0 到 3 的装载存储无条件升格短格式，``iload 0`` 落 ``iload_0``（``code_compiler.cpp``）
3. ``bipush``/``sipush`` 编译期并入 ``ldc``（5.3）
4. ``lookupswitch`` 键编译期排序（``code_compiler.cpp``）
5. ``goto``/``jsr`` 偏移放不下时导出期自动加宽 ``_W``（libjbc ``code_dom.cpp``）

这套归一让文本好写、canonical 输出稳定，归一用途已经闭环。代价是文本不携带指令形态：写哪个助记符不决定落哪个 opcode。需要形态保真的场景现在没有通道：

- 语料复现：libjsim demo 语料（y.jasm）带着真实 classfile 的 ``bipush``/``sipush`` 原生形态，过汇编器会被并成 ``ldc``，产物与原始 dump 语义等价、形态不同
- 去混淆重建与字节级对拍：改写产物的指令序列希望与原文逐条对应
- 最小复现与测试：需要精确控制落地字节形态

提案：方言分成 r（raw）与 s（simplified）两版。r 是所见即所得的原始形态，s 是现在的归一形态。

.. _proposal-dialect-s:

2. s 方言（现状正式化）
-----------------------

s 就是现状行为正式命名，语义不动：

- ldc 族三拼写都收，语义由常量类别决定；long/double 恒走 cat2 形态，池索引超 255 自动升格
- ``bipush``/``sipush`` 合法，编译期并入 ``ldc``
- 槽位短格式自动升格，``lookupswitch`` 键排序
- 帧分析 Strict 策略照跑，SMT 按代次生成

现有语料、smoke、单元测试全部落在 s。文本没有 ``.dialect`` 声明时按 s 解释。

.. _proposal-dialect-r:

3. r 方言（原始形态）
---------------------

r 的规则一句话：助记符与 opcode 一一对应，写什么落什么。对照第 1 节的五处归一：

.. list-table::
   :header-rows: 1

   * - 归一点
     - s 行为
     - r 行为
   * - ``ldc``/``ldc_w``/``ldc2_w``
     - 同语义，落地自动选形
     - 三拼写对应三个 opcode，按写的落地
   * - ``bipush``/``sipush``
     - 并入 ``ldc``
     - 按写的落地
   * - 槽位装载存储
     - 槽 0 到 3 升格短格式
     - ``iload 0`` 落长格式，``iload_0`` 落短格式，不互换
   * - ``lookupswitch`` 键序
     - 编译期排序
     - 保持写的顺序
   * - ``goto``/``jsr``
     - 溢出自动加宽
     - 显式 ``goto_w`` 保形；只有偏移真放不下才加宽，这是 JVMS 强制而非选择

verify 分两档：

- verify 开：在 r 形态规则上做 JVMS 合法性检查。``ldc``/``ldc_w`` 操作数必须是 cat1 可装载常量，``ldc2_w`` 操作数必须是 long 或 double；槽位、分支目标、switch 结构照现有检查面。帧分析照跑，SMT 照生成
- verify 关：只做语法与结构检查，强制原样落地。接受 JVM 认为非法的形态（比如 ``ldc_w`` 装载 cat2 常量），不跑帧分析，不生成 SMT，空方法体放行

.. _proposal-dialect-declare:

4. 声明与默认
-------------

方言标记是文本首行的行注释 ``//! dialect r``，shebang 式。语法上它就是注释，上游 Jasm 与 Recaf 的解析器按注释跳过，s 产物与它们产出的文本保持互通；本仓解析器只把首行这一处当标记，其余位置是普通注释。s 文本与缺省文本不带标记，行为完全不变。汇编选项加 dialect 与 verify 两个字段做 API 覆盖，选项优先于首行标记。printer 在 r 模式把标记作为首行输出，文本自描述，往返自闭合。

也考虑过真顶层指令的方案，放弃：上游解析器遇到不认识的声明直接报错，互通面就破了。注释形态对两边都无害。缺省 s。

.. _proposal-dialect-maxs:

5. r 模式的伴生形态：maxs
-------------------------

classfile 每个方法携带 max_stack 与 max_locals，现在由帧分析算出，文本里不存在。r 的 no-verify 档不跑分析，这两项必须有显式来源：

.. code-block:: jasm

   maxs: { stack: 3, locals: 7 },

printer 在 r 模式总是输出这一行，值取自产物；assembler 见到 maxs 直接采用不重算。手写 r 文本且 no-verify 时不带 maxs 报错。verify 档不带 maxs 时照现有路径分析算出，带了则优先采用文本值。

.. _proposal-dialect-contract:

6. 契约边界
-----------

r 一期承诺指令形态所见即所得：同一助记符与操作数序列落地为同一 opcode 序列、同样的槽位、分支偏移结构与 switch 键序。

r 一期不承诺整文件字节全等，两条原因：

1. 常量池由重建分配索引，原始池顺序不保留，ldc 操作数里的池索引可能不同
2. StackMapTable 原样携带依赖池保序，不在一期范围

整文件字节保真继续走 libjbc 会话保真写回那条路，文本方言不承担这个职责。溢出强迫加宽是唯一合法的形态改写，加宽后 canonical 如实打 ``goto_w``。

.. _proposal-dialect-plan:

7. 实施面
---------

改动集中在 libjhasm，一期预期零 libjbc 改动：

- 读侧：``jvm_opcodes.cpp`` 别名表按方言分流，r 模式三拼写映射三个 op
- 编译：``code_compiler.cpp`` 三处门控（短格式升格、push 并入、switch 排序只在 s 生效）；dialect 与 verify 两字段进 ``JvmCompilerOptions``
- 打印：printer 加 r 模式，按真实 op 打 ``ldc_w``/``ldc2_w``、长短格式与键序，首行输出方言标记与 ``maxs``
- 核对清单：libjbc 导出面确认只加宽不缩窄，显式 ``goto_w`` 不被改写；r 模式 LDC_W、BIPUSH 等 op 能原样写出

落地后 syntax.rst 加方言一节，第 5、6 节的归一规则标注方言归属。

.. _proposal-dialect-compat:

8. 兼容性
---------

缺省 s，现有语料与门禁零影响。r 显式 opt-in，只有首行带 ``//! dialect r`` 的文本改变解释。两方言的往返测试各自进 smoke。
