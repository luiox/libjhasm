.. _design-dialects:

Jasm 方言设计
=============

libjhasm 用同一套 Jasm 声明语法支持两种指令表示。s（simplified）是缺省方言，沿用自动选择指令编码的汇编规则，适合编写和阅读以语义为主的文本。r（raw）保留可编码的指令形态，用于复现已有字节码、对拍指令编码和构造验证器测试数据。工程结构见 :doc:`design`，调用方式见 :doc:`manual`，操作数和声明语法见 :doc:`syntax`。

.. _design-dialects-selection:

方言选择
--------

文本入口 ``jhasm::assemble`` 在词法分析前检查输入。只有从第一个字符开始的首行注释 ``//! dialect r`` 能选择 r，标记后可跟空格和制表符。标记与显式的 ``JvmCompilerOptions::dialect`` 冲突时，错误定位到首行。有标记且无冲突时使用 r，无标记时使用 API 指定的方言，选项留空时使用 s。直接编译 AST 的入口从选项取方言。

标记采用注释形态，其他位置照普通注释处理，避免把新的顶层声明引入共享语法。整类反汇编的 r 输出带首行标记，s 输出无标记。单方法反汇编产生的是方法片段，选择方言由承载它的类文本或汇编选项负责。标记只传递方言，重新汇编未通过验证的 r 产物时仍需由调用方显式关闭 ``verify``。

.. _design-dialects-encoding:

指令编码与输出
--------------

``jvm_opcodes.cpp`` 以 libjbc 的助记符表为基础映射 opcode。``code_compiler.cpp`` 按有效方言构建 libjbc 的 ``CodeDom``，由 libjbc 计算布局和写出指令。``jvm_printer.cpp`` 从已解码的 IR 取操作数，按下列规则打印。

.. list-table::
   :header-rows: 1
   :widths: 23 34 43

   * - 指令
     - s
     - r
   * - ``ldc`` / ``ldc_w`` / ``ldc2_w``
     - 汇编按常量类别和池索引选择编码，反汇编将 ``ldc_w`` 打印为 ``ldc``。
     - 汇编和反汇编保留三个助记符各自对应的 opcode。
   * - ``bipush`` / ``sipush``
     - 汇编将立即数转为 Integer 常量并按 ``ldc`` 族编码。外部 classfile 的原生 push 反汇编为 ``ldc`` 加同值有符号整数。
     - 汇编分别写入有符号 i8、i16 立即数，反汇编保留原始助记符及立即数值。
   * - 局部变量指令
     - 汇编将适用的槽位 0 到 3 装载或存储指令编码为短格式。槽号或 ``iinc`` 增量的编码宽度由操作数决定。
     - ``iload 0`` 和 ``iload_0`` 各自保形。支持用 ``wide iload 0``、``wide iinc 0 1`` 等显式保留前缀。普通形式在操作数超出窄格式范围时仍自动使用 ``wide``。
   * - ``lookupswitch``
     - 汇编按键排序。
     - 汇编和反汇编保留键序。开启验证时键必须严格递增，关闭验证时允许乱序或重复键。
   * - ``tableswitch``
     - 以 ``min`` 为 low，以 ``min + cases 数量 - 1`` 为 high，保留目标表的顺序。
     - 与 s 相同。
   * - ``goto`` / ``jsr``
     - libjbc 在短偏移装不下时加宽，显式宽分支保持原样。
     - 与 s 相同。反汇编打印实际编码的分支助记符。

常量池重建按新类分配索引。``ldc`` 的索引为 u1，r 中重建所得索引大于 255 时，两档验证均报错。操作数为 category-1 常量时，可显式改用采用 u2 索引的 ``ldc_w``。这个限制也适用于从已有 classfile 打印出来的 r 文本。

r 保留的是文本可表达的指令形态。分支的实际偏移、switch 填充和常量池索引由新布局决定，``StackMapTable`` 由所选验证路径决定。对原 classfile 做字节保真编辑属于 libjbc 会话的职责。

.. _design-dialects-verification:

验证与方法体上限
----------------

s 始终运行 libjbc 的 Strict 帧分析。``verify`` 只控制 r 的方法体验证。r 开启验证时，在编码约束之外检查 ``ldc`` 族常量类别、``lookupswitch`` 键序等语义规则，并使用同一 Strict 帧分析。栈映射表在目标主版本号大于等于 50 且构建器判定需要时生成。r 关闭验证时跳过帧分析和栈映射表生成，语法、结构和操作数编码范围仍需满足。两条路径保留非空方法的 65535 字节上限。

当前 Strict 帧引擎将动态常量装载近似为 Int。r 开启验证时，``ldc2_w`` 接受 CONSTANT_Long 和 CONSTANT_Double，描述符为 ``J`` 或 ``D`` 的动态常量在 r 中须用关闭验证的路径编码。由于该引擎的近似模型，其他动态常量的帧分析仍受此限制。

r 反汇编从 ``Code`` 属性输出 ``maxs``。汇编带 ``Code`` 的方法时，显式 ``maxs`` 的 ``stack`` 和 ``locals`` 均为 u2 值。关闭验证时必填。开启验证时可省略，汇编器采用计算结果。显式值偏小时按字段发出警告，仍将该值写入产物。

- 开启验证时，``stack`` 与 Strict 计算值和异常处理器入口所需的一槽下界比较。``locals`` 与 Strict 计算值、参数槽需求及指令引用到的所有槽位需求比较。
- 关闭验证时，``locals`` 的下界来自方法参数、接收者槽和所有局部变量指令，计入 long 和 double 的双槽宽度。
- 关闭验证时，``stack`` 从方法入口沿直线前缀计数。已知效果包括 ``nop``、``iinc``、常量入栈、局部变量装载和存储、``pop``、``pop2``、``dup`` 族和 ``swap``，以及数值加减乘除和取余。对象和数组相关指令中覆盖 ``new``、``newarray``、``anewarray``、``arraylength``、``checkcast`` 和 ``instanceof``。遇到分支、其他未知效果或栈下溢时停止。有异常处理器时下界至少为一槽。

.. _design-dialects-empty-code:

零长度 Code
------------

libjbc 的常规 ``Code`` 编解码拒绝零长度指令区。r 关闭验证且方法没有指令时，``finishBody`` 检查没有异常表项，随后以不透明属性写出 ``Code``。属性体包含文本给出的两个上限字段和三个零值计数，分别表示指令字节数、异常表项数和嵌套属性数。``JvmPrinter`` 在 r 下遇到 ``Code`` 解码失败时，仅对这个 12 字节形态回退打印 ``maxs`` 和空 ``code``。其他方法体仍通过 libjbc 的结构化编解码路径。

.. _design-dialects-tests:

回归覆盖
--------

``test/unittest/dialect_test.cpp`` 检查实际 opcode 和操作数，覆盖首行标记冲突、外部原生 push 的 s 输出稳定性、r 的常量装载与槽位形态往返、``ldc`` 索引溢出、两种 switch 规则、两档验证的显式上限告警和编码错误，以及无栈映射表与零长度方法体。``test/unittest/code_size_test.cpp`` 覆盖方法尺寸上限和分支加宽。``test/smoke/roundtrip_smoke.cpp`` 补充两种方言的端到端反汇编和重汇编覆盖。
