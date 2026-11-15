.. _syntax:

libjhasm JAsm 语法规范
======================

状态：对齐 0.3.x 实现。本文以本仓源码为唯一权威，逐条规则注明依据的文件；标注（实测）的结论有运行佐证（test/smoke 或专项探针），标注（源码推断）的结论只经源码核对、未经运行复验。上游 jumanji144/Jasm（revision ``aacebfa``，C++ 重写的对照基线）只在第 9 节出现，冲突时以本文为准。

本文覆盖 HotSpot/JVM 组合面。Dalvik 语法与 CLI 形态不在本库范围。

.. _syntax-lexical:

1. 词法
-------

.. _syntax-lexical-charset:

1.1 字符集、空白与行
^^^^^^^^^^^^^^^^^^^^

源文本按 UTF-16 处理。诊断位置为 1 基行号加 1 基列号，列以 UTF-16 码元计（``util/location.hpp``）。

空白分隔 token（``tokenizer.cpp`` 的 ``is_java_whitespace``）：U+0009 到 U+000D、U+001C 到 U+0020（含四个文件分隔符控制符，比 Java 白名单宽）、U+1680、U+2000 到 U+200A（U+2007 除外）、U+2028、U+2029、U+205F、U+3000。CR 是普通空白；只有 LF 推进行号。

.. _syntax-lexical-comment:

1.2 注释
^^^^^^^^

两种形态，解析前丢弃，可出现在任何空白可出现的位置：

.. code:: text

    // 行注释，到行尾
    /* 块注释，可跨行 */

块注释在词法层处理转义：反斜杠转义会吞掉其后一个字符，所以 ``\*/`` 不终止注释，注释在下一次裸露的 ``*/`` 才结束。未闭合的块注释与未闭合的字符串一样是错误（``Unterminated multiline comment``）。

.. _syntax-lexical-operator:

1.3 操作符
^^^^^^^^^^

``{`` ``}`` ``:`` ``,`` 四个字符恒为单字符 token（``Tokenizer::is_operator``），即使紧贴其他文本。除这四个字符外没有任何操作符级字符：``;`` ``.`` ``(`` ``)`` ``[`` ``]`` 等都是标识符的普通组成部分。

.. _syntax-lexical-identifier:

1.4 标识符
^^^^^^^^^^

标识符是不属于其他类别的任意字符序列：非空白、非四个操作符、不以引号或注释定界符开头（词法中途遇到引号会终止当前 token 并进入字符串态）。点前缀标识符（``.class`` 等）是声明关键字；修饰符、助记符、标签名、描述符、成员路径、``true``、``false``、``NaN``、``Infinity`` 家族都是标识符。

两条硬规则：

1. 读侧标识符不反转义（``ast/primitive.hpp`` 的 ``Identifier::literal`` 直接返回原文）。源码里 ``\uXXXX`` 进名字的就是 6 个字面字符：``new \u0041`` 生成的类名是 6 字符文本 ``\u0041``，不是 ``A``（实测）。要真字符直接写 UTF-8 原文。
2. 反斜杠是普通字符，标识符内不做任何转义处理。

大小写敏感。``Foo`` 与 ``foo`` 是两个名字。

.. _syntax-lexical-string:

1.5 字符串与字符
^^^^^^^^^^^^^^^^

字符串用双引号，字符用单引号。内容按 Java 转义族解码（``tokenizer.cpp`` 的 ``process_escape``）：

.. list-table::
   :header-rows: 1

   * - 源写法
     - 落地内容
   * - ``\n \r \t \b \f``
     - 对应控制字符
   * - ``\" \' \\``
     - 对应字符本身
   * - ``\uXXXX``
     - 该码元；十六进制位大小写不敏感
   * - ``\`` 加其他字符
     - 保留反斜杠本身，吞掉被转义字符：``\q`` 落成 ``\``

错误行为：``\u`` 后不足 4 位报 ``Truncated unicode escape``，4 位中出现非十六进制字符报 ``Invalid unicode escape``，两种情况都把 ``\u`` 原样保留进内容并继续解析；字符串内出现行尾或文末报 ``Unterminated string`` 并就地截断。

注意解析器与 ``util/escape_util.cpp`` 的 ``EscapeUtil::unescape`` 是两套行为：后者对未知转义保留反斜杠与字符两个字符。汇编输入走的是前者（tokenizer），以本文为准；``unescape`` 是独立工具函数，汇编流水线不调用它。

.. _syntax-lexical-number:

1.6 数字
^^^^^^^^

数字 token 由正则整体判定（``tokenizer.cpp`` 的 ``is_number_text``），负号属于数字的一部分。语义展开：

- 十进制整数：``0``、``123``、``1_000``（下划线可在数字之间）。
- 十六进制整数：``0x1A``、``0X1a``。
- 二进制整数：``0b101``（词法接受，语义见下）。
- 浮点：小数点形 ``1.5`` ``.5`` ``5.``，指数形 ``1e5`` ``1E-3``，十六进制浮点 ``0x1.8p3``（必须有 ``p`` 指数）。
- 后缀：整数分支可带 ``L l F f D d``，浮点分支可带 ``F f D d``。``5F`` 是 float，``5D`` 是 double，``5L`` 是 long，``1e5`` 无后缀但含指数，是 double。
- 单独的 ``-`` 是标识符不是数字。

数字字面量的类别由后缀与形态判定（``ast/primitive.cpp``）：含 ``.`` ``e`` ``E`` 或带 ``F f D d`` 后缀为浮点；带 ``L l D d`` 后缀或（浮点且无 F 后缀）为宽。

三个语义事实（实测，细节见第 9 节）：``0b`` 前缀的词法能过，但整型语义解析按十进制前缀截断，``ldc 0b101`` 落 0，是数字文法里唯一坏形；负十六进制正常（``ldc -0x10`` 落 -16），但 ``bipush``/``sipush``/``line``/``iinc`` 增量这些整数操作数位只认十进制，``bipush -0x10`` 被拒而 ``bipush -16`` 可；下划线分隔在整数操作数位直接拒绝，在 ``ldc`` 位截断（``ldc 1_000`` 落 1）。

.. _syntax-values:

2. 值文法
---------

所有声明体、注解值、指令操作数共用一套值文法（``declaration_parser.cpp``）：

.. list-table::
   :header-rows: 1

   * - 形态
     - 含义
   * - 裸词
     - 标识符；``true``/``false`` 先判为布尔
   * - 数字
     - 数字字面量
   * - ``"..."``
     - 字符串
   * - ``'...'``
     - 字符
   * - ``{ }``
     - 空值（Empty）
   * - ``{ v1, v2, ... }``
     - 数组；尾逗号允许
   * - ``{ k1: v1, k2: v2, ... }``
     - 对象；键是任意非操作符 token（数字可作键，``lookupswitch`` 用），尾逗号允许
   * - ``{ .decl ..., .decl ... }``
     - 声明块（首元素为点声明时）

对象值为键 ``code`` 的特殊形态：值按指令块解析（第 4 节）。指令操作数必须与助记符同行，这也是唯一按行切割值的地方。布尔元素（``true``/``false``）只在注解值与字段常量值里落地，``ldc`` 指令位不接受（5.2）。尾逗号实测合法（数组与对象均收）。

.. _syntax-decl:

3. 声明
-------

.. _syntax-decl-general:

3.1 通用形态与挂载
^^^^^^^^^^^^^^^^^^

声明以点前缀关键字起头，后跟元素序列，元素终止于 ``}``、``,`` 或下一个点声明。未知关键字报 ``Unknown declaration``（``ast_processor.cpp``）。

多数类级与成员级声明是待挂属性：按文本顺序落到下一个兼容目标上。挂载关系全表：

.. list-table::
   :header-rows: 1

   * - 声明
     - 形态
     - 挂载目标
   * - ``.class``
     - ``.class <修饰符> <类名> { 成员 }``
     - 自身（编译单元根）
   * - ``.field``
     - ``.field <修饰符> <名> <字段描述符> [{ value: <常量> }]``
     - 自身
   * - ``.method``
     - ``.method <修饰符> <名> <方法描述符> { 体 }``
     - 自身
   * - ``.super``
     - ``.super <内部名>``
     - 下一个 ``.class``
   * - ``.implements``
     - ``.implements <内部名>``（可多次）
     - 下一个 ``.class``
   * - ``.signature``
     - ``.signature "<泛化签名>"``
     - 下一个 ``.class`` / ``.field`` / ``.method`` / ``.record-component``
   * - ``.sourcefile``
     - ``.sourcefile "<源文件名>"``
     - 下一个 ``.class``
   * - ``.inner``
     - ``.inner <修饰符> { name: <名>, outer: <内部名>, inner: <内部名> }``
     - 下一个 ``.class``；``name`` 与 ``outer`` 可省，``inner`` 必填
   * - ``.outer-class``
     - ``.outer-class <内部名>``
     - 下一个 ``.class``
   * - ``.outer-method``
     - ``.outer-method <名> <方法描述符>``
     - 下一个 ``.class``（配合 ``.outer-class`` 落 EnclosingMethod）
   * - ``.nest-host``
     - ``.nest-host <内部名>``
     - 下一个 ``.class``
   * - ``.nest-member``
     - ``.nest-member <内部名>``（可多次）
     - 下一个 ``.class``
   * - ``.permitted-subclass``
     - ``.permitted-subclass <内部名>``（可多次）
     - 下一个 ``.class``
   * - ``.record-component``
     - ``.record-component <名> <字段描述符>``
     - 下一个 ``.class`` 的 Record 组件；可前置 ``.signature`` 与注解
   * - ``.visible-annotation``（别名 ``.annotation``）
     - 见 3.6
     - 下一个目标；注解值内部出现时作嵌套注解值
   * - ``.invisible-annotation``
     - 同上
     - 同上
   * - ``.type-visible-annotation``（别名 ``.type-annotation``）、``.type-invisible-annotation``
     - 见 3.7
     - 同上
   * - ``.enum``
     - ``.enum <类型> <常量名>``
     - 仅注解值内合法，外部出现报错

类体内只接受 ``.field`` 与 ``.method``（其他报 ``Expected field or method``）；类体内成员级待挂属性（注解、签名）必须被成员消费，否则报 ``Unconsumed class member attributes``。

.. _syntax-decl-modifiers:

3.2 修饰符
^^^^^^^^^^

23 个拼写（``ast/specific.cpp`` 的 ``Modifiers::isValid``），大小写敏感：

``public`` ``private`` ``protected`` ``static`` ``final`` ``abstract`` ``strictfp`` ``transient`` ``volatile`` ``synchronized`` ``native`` ``varargs`` ``bridge`` ``synthetic`` ``enum`` ``annotation`` ``module`` ``super`` ``interface`` ``record`` ``sealed`` ``open`` ``non-sealed``

拼写到旗语的映射见 ``compile/jvm_compile_detail.hpp`` 的 ``access()``。同一旗语位在不同声明种类下拼写不同：0x0020 对类是 ``super``、对方法是 ``synchronized``；0x0040 对字段是 ``volatile``、对方法是 ``bridge``；0x0080 对字段是 ``transient``、对方法是 ``varargs``。打印按声明种类回显对应拼写（``printer`` 的 ``modifiers``）。

修饰符的合法组合不做校验，落到什么旗语就是什么。``record`` 映射到 0x10000，超出 classfile access 的 u16 域，出口静默截断（Record 属性的在场才是记录类的规范形态）。

.. _syntax-decl-class:

3.3 类
^^^^^^

.. code:: text

    .sourcefile "Hello.jasm"
    .super java/lang/Object
    .implements java/lang/Runnable
    .class public super demo/Hello {
        .field ...
        .method ...
    }

- 一个编译单元恰好一个类（``jvm_compiler.cpp``）。``implements`` 不是 ``.class`` 行的修饰符，是独立声明，写在 ``.class`` 行里报 ``Invalid modifier``（实测）。
- 类名接受内部名（``demo/Hello``）、``Ldemo/Hello;`` 形态（剥壳落内部名）与数组描述符（原样，``internalNameOf``）。
- ``.super`` 缺省为空：不写 ``.super`` 时产物 ``super_class`` 落 0（实测，字节级核对），这种 classfile 只有 ``java/lang/Object`` 自身可加载，手写具体类必须显式给 ``.super``。
- ``super`` 修饰符控制 ACC_SUPER 位（``invokespecial`` 语义依赖），javac 产物普遍带，手写类建议显式写。

.. _syntax-decl-field:

3.4 字段
^^^^^^^^

.. code:: text

    .field public static final COUNT I { value: 42 }
    .field private name Ljava/lang/String;

常量值对象必须只有 ``value`` 一个键。ConstantValue 支持五种常量：整型、long、float、double、字符串（``jvm_compiler.cpp``）；类、方法句柄、condy 落 ``Unsupported field constant kind``。布尔写作 ``{ value: true }``，落 Integer 1（实测）。字段描述符按 JVMS 校验。

.. _syntax-decl-method:

3.5 方法
^^^^^^^^

.. code:: text

    .method public static add (II)I {
        parameters: { left, right },
        code: {
            iload left
            iload right
            iadd
            ireturn
        }
    }

方法体是对象，四个键（键序任意，重复键取首个）：

.. list-table::
   :header-rows: 1

   * - 键
     - 值
     - 落地
   * - ``parameters``
     - 名字数组
     - 参数名按描述符序分配槽位（4.2）；同时落 MethodParameters 属性，打印不回显
   * - ``exceptions``
     - 四元组数组 ``{ start, end, handler, type }``
     - 异常表；区间左闭右开；``type`` 为内部名或 ``*``（全捕获）；表序即派发序
   * - ``code``
     - 指令块（第 4 节）；``code: { }`` 为空指令体
     - Code 属性
   * - ``default-value``
     - 注解值
     - 注解接口方法的 AnnotationDefault

abstract 或 native 方法用空体 ``{ }``：不落 Code 属性。注意 ``code: { }``（空指令体）不是空体声明的等价写法：它会落一个没有 return 出口的 Code 属性，被帧分析拒绝（实测）。非空体方法会跑完整编译管线：指令发射、帧分析（Strict 策略）、class v50 起生成 StackMapTable、64KB 方法体预检（第 7 节）。

.. _syntax-decl-annotation:

3.6 注解
^^^^^^^^

.. code:: text

    .visible-annotation Ldemo/Marker; {
        value: "public",
        count: 7,
        big: 42L,
        ratio: 1.5F,
        exact: 2.718281828459045D,
        flag: true,
        color: 'x',
        mode: .enum demo/Mode FAST,
        nested: .annotation demo/Inner { v: 1 },
        names: { "a", "b" }
    }

``.annotation`` 是 ``.visible-annotation`` 的别名；``.invisible-annotation`` 落 RuntimeInvisible。注解类型名按描述符落池：``demo/Marker`` 与 ``Ldemo/Marker;`` 等价（``descriptorOf``）。值文法到 JVMS tag 的落地表（``jvm_annotations.cpp`` 的 ``rawElement``）：

.. list-table::
   :header-rows: 1

   * - 源形态
     - tag
     - 说明
   * - 整型数字（无宽后缀）
     - ``I``
     - 注解整型没有 byte/short 形态
   * - 带 ``L l`` 后缀的整数
     - ``J``
   * - 带 ``F f`` 后缀的浮点
     - ``F``
   * - 浮点或带 ``D d`` 后缀
     - ``D``
   * - ``"text"``
     - ``s``
   * - ``'c'``
     - ``C``
     - 取首个码元
   * - ``true`` / ``false``
     - ``Z``
   * - 其他裸词
     - ``c``
     - 类值，按描述符落池
   * - ``.enum T V``
     - ``e``
   * - ``.annotation T { ... }``
     - ``@``
     - 嵌套注解，可任意深度
   * - ``{ ... }``
     - ``[``
     - 数组，元素同为上表形态；``{ }`` 落空数组

.. _syntax-decl-type-annotation:

3.7 类型注解
^^^^^^^^^^^^

读侧形态：

.. code:: text

    .type-visible-annotation Ldemo/T; {
        location: { ref: 318767104, path: "[." },
        values: { value: "sample" }
    }

``ref`` 是 JVMS type reference 的打包整数（``targetType << 24 | first << 8 | second``，``util/type_annotation_codec.hpp``）。``path`` 是本方言自己的字母段编码，不是 ASM 原文：每段一个小写字母，``a`` 深入数组、``b`` 深入嵌套、``c`` 通配符界、``d`` 类型实参（后跟十进制下标，如 ``d0``），段间连写（``ada`` 即数组里嵌套类型的第 0 个实参）。空路径编码为空串，而空串不是标识符，读侧无法表达。

已知不对称（0.3.x 未闭合面，实测）：打印面输出的是位置参数形态 ``.type-visible-annotation <ref> <path> <类型> { 值 }``，与读侧的对象形态不一致，canonical 文本回读报 ``Expected annotation type and values``。类型注解的文本构造与回读目前都不闭合，建议走库 API。

.. _syntax-code:

4. 方法体
---------

.. _syntax-code-block:

4.1 指令块结构
^^^^^^^^^^^^^^

``code`` 值是指令块（``declaration_parser.cpp`` 的 ``parseCode``）：逐行读，每行一个标签或一条指令。

- 标签：``名字:``。可前向引用；可与指令同行（``A: ldc 1`` 合法，标签绑定到同行指令；canonical 会拆回两行）。
- 指令：助记符加零到四个操作数，操作数必须与助记符同行，跨行的操作数不归属该指令。
- ``line N``：行号声明，附着到其后第一条指令，落 LineNumberTable。
- 尾部标签（后面没有指令）合法，落在方法体边界（``endDeferredTargets``）。
- 未被任何分支或异常表引用的标签不进产物：lower 成字节码后没有痕迹，canonical 也不回显（实测）。要保留位置只能被 ``goto`` 或异常表引用。

.. _syntax-code-slot:

4.2 局部变量槽位
^^^^^^^^^^^^^^^^

装载、存储、``iinc`` 首操作数、``ret`` 操作数接受符号名或槽号（``LITERAL`` 形态：标识符或数字）。

分配规则（``code_compiler.cpp`` 的 ``CodeCompiler`` 构造与 ``variable()``）：

1. 静态方法从槽 0 开始；实例方法槽 0 固定为 ``this``，占用 ``parameters`` 的第一个名字。
2. 其余名字按方法描述符的参数顺序分配；long/double 参数占两个槽。
3. ``parameters`` 里没给全的名字在指令里首次出现时按出现顺序分配新槽：普通指令占一槽，``lload``/``dload``/``lstore``/``dstore`` 占两槽。同一个名字先在哪类指令出现就按哪类定宽。
4. 数字操作数直接就是槽号，不经分配。

写侧形态：槽 0 到 3 一律落 ``_N`` 短格式（``iload_0`` 等；``ret`` 无短格式）；槽号超 255 落 wide 形。``iinc`` 的增量超出 -128 到 127 同样落 wide 形。

.. _syntax-code-exception:

4.3 异常表
^^^^^^^^^^

``exceptions`` 是四元组数组，挂在 ``code`` 之前：

.. code:: text

    exceptions: {
        { TryStart, TryEnd, Handler, java/lang/Exception },
        { CleanStart, CleanEnd, Clean, * }
    },

``*`` 为全捕获（catch_type 落 0）。标签解析与方法体共用一套（前向可）。

.. _syntax-insn:

5. 指令与操作数
---------------

.. _syntax-insn-table:

5.1 助记符总表
^^^^^^^^^^^^^^

指令名与操作数形态在 ``instructions/jvm_instructions.cpp`` 注册，解析期即校验名字与操作数个数及形状。无 ``wide`` 助记符：wide 形由编译器按 4.2 的阈值自动落。``goto_w``/``jsr_w`` 是独立助记符。

.. list-table::
   :header-rows: 1

   * - 操作数形态
     - 助记符
   * - 无操作数
     - ``nop`` ``aconst_null`` ``iconst_m1`` ``iconst_0`` 到 ``iconst_5`` ``lconst_0`` ``lconst_1`` ``fconst_0`` 到 ``fconst_2`` ``dconst_0`` ``dconst_1``；数组装载 ``iaload`` 到 ``saload``；数组存储 ``iastore`` 到 ``sastore``；栈操作 ``pop`` ``pop2`` ``dup`` ``dup_x1`` ``dup_x2`` ``dup2`` ``dup2_x1`` ``dup2_x2`` ``swap``；算术 ``iadd`` 到 ``lxor`` 全族；转换 ``i2l`` 到 ``i2s`` ``l2i`` 到 ``d2f`` ``f2i`` 到 ``d2f``；比较 ``lcmp`` ``fcmpl`` ``fcmpg`` ``dcmpl`` ``dcmpg``；返回 ``ireturn`` 到 ``return``；``arraylength`` ``athrow`` ``monitorenter`` ``monitorexit``
   * - 整数
     - ``bipush`` ``sipush`` ``line``
   * - 类型（内部名）
     - ``new`` ``anewarray`` ``checkcast`` ``instanceof``
   * - 基本类型词
     - ``newarray``（``boolean`` ``byte`` ``char`` ``short`` ``int`` ``float`` ``long`` ``double``）
   * - 常量
     - ``ldc`` ``ldc_w`` ``ldc2_w``（6.2）
   * - 槽位
     - ``iload`` ``lload`` ``fload`` ``dload`` ``aload`` ``istore`` ``lstore`` ``fstore`` ``dstore`` ``astore`` ``ret``（符号名或槽号）
   * - 槽位短格式
     - 上述五装载五存储的 ``_0`` 到 ``_3`` 共 40 个
   * - 槽位加增量
     - ``iinc <槽> <增量>``
   * - 标签
     - ``ifeq`` ``ifne`` ``iflt`` ``ifge`` ``ifgt`` ``ifle`` ``if_icmpeq`` ``if_icmpne`` ``if_icmplt`` ``if_icmpge`` ``if_icmpgt`` ``if_icmple`` ``if_acmpeq`` ``if_acmpne`` ``goto`` ``jsr`` ``ifnull`` ``ifnonnull`` ``jsr_w`` ``goto_w``
   * - 表 switch
     - ``tableswitch``（6.4）
   * - 键值 switch
     - ``lookupswitch``（6.4）
   * - 字段
     - ``getstatic`` ``putstatic`` ``getfield`` ``putfield``（6.5）
   * - 方法
     - ``invokevirtual`` ``invokespecial`` ``invokestatic`` ``invokeinterface`` ``invokevirtualinterface`` ``invokestaticinterface`` ``invokespecialinterface``（6.5）
   * - 动态调用点
     - ``invokedynamic``（6.6）
   * - 数组描述符加维数
     - ``multianewarray``

.. _syntax-insn-const:

5.2 常量加载
^^^^^^^^^^^^

``ldc`` 承载全部非常量池简单形态（``compile/jvm_constants.cpp`` 的 ``constantValue``）：

.. list-table::
   :header-rows: 1

   * - 源形态
     - 落地常量池
     - 落地指令
   * - ``42`` ``-7``
     - Integer
     - ``ldc``
   * - ``42L``
     - Long
     - ``ldc2_w``
   * - ``3.14F``
     - Float
     - ``ldc``
   * - ``3.14`` ``1e5`` ``2.71D``
     - Double
     - ``ldc2_w``
   * - ``NaN`` ``NaND``
     - Double NaN
     - ``ldc2_w``
   * - ``NaNF``
     - Float NaN
     - ``ldc``
   * - ``Infinity`` ``+Infinity`` ``InfinityD`` ``+InfinityD`` ``-Infinity`` ``-InfinityD``
     - Double 无穷
     - ``ldc2_w``
   * - ``InfinityF`` ``+InfinityF`` ``-InfinityF``
     - Float 无穷
     - ``ldc``
   * - ``"text"``
     - String
     - ``ldc``
   * - ``'e'``
     - Integer（首码元，``'e'`` 落 101）
     - ``ldc``
   * - ``Ljava/lang/String;`` ``[I``
     - Class（``L..;`` 剥壳，数组描述符原样）
     - ``ldc``
   * - ``(II)V`` 等方法描述符
     - MethodType
     - ``ldc``
   * - ``{ invokestatic, a/B.c, <desc> }``
     - MethodHandle（6.6）
     - ``ldc``
   * - ``LambdaMetafactory.metafactory``
     - MethodHandle（已知展开）
     - ``ldc``
   * - ``{ <名>, <字段描述符>, <句柄>, <参数数组> }``
     - Dynamic（condy）
     - ``ldc``

常量池索引超 255 时 ``ldc`` 自动升格 ``ldc_w``；long/double 恒走 ``ldc2_w``。三条助记符读侧全收，语义由常量类别决定，不由助记符决定。布尔字面量（``true``/``false``）不在 ``ldc`` 接受范围：指令操作数的常量校验（``isConstant``）不收布尔元素，写了报操作数错误（实测）；布尔只在注解值（落 Z）与字段常量值（落 Integer 1/0）两处生效。

.. _syntax-insn-push:

5.3 bipush 与 sipush
^^^^^^^^^^^^^^^^^^^^

两条助记符接受整数操作数，但编译为与 ``ldc`` 整数完全相同的形态（常量池 Integer 加 ``ldc``/``ldc_w``，``code_compiler.cpp`` 的 ``insnFor``）。本仓汇编产物里不存在 bipush/sipush 指令；canonical 输出见到 bipush/sipush 只可能来自外部 classfile 的原生指令。

.. _syntax-insn-branch:

5.4 分支与 switch
^^^^^^^^^^^^^^^^^

分支操作数是标签名。两种 switch：

.. code:: text

    tableswitch { min: 0, max: 2, cases: { L1, L2, L3 }, default: L4 }
    lookupswitch { -10: L1, 5: L2, default: L3 }

- ``tableswitch``：``min``、``max`` 必须是整数，``cases`` 是标签数组（可为空），``default`` 是标签。``max`` 只做语法校验，落地边界由 ``min`` 加 cases 个数推导（``min + cases 数 - 1``），两者不一致不报错。
- ``lookupswitch``：键值对形态，键是数字（对象键位置），值必须是标签；键编译期按数值排序后落地。
- ``cases`` 为空的 ``tableswitch`` 合法（无分支目标，边界按推导收缩）。

.. _syntax-insn-member:

5.5 字段与方法引用
^^^^^^^^^^^^^^^^^^

.. code:: text

    getstatic java/lang/System.out Ljava/io/PrintStream;
    putfield demo/Hello.name Ljava/lang/String;
    invokevirtual java/io/PrintStream.println (Ljava/lang/String;)V
    invokestatic demo/Calc.add (II)I
    invokeinterface java/util/Collection.isEmpty ()Z

第一操作数 ``Owner.name`` 用最后一个 ``.`` 分隔出属主与成员名：属主是内部名，成员名可含 ``$`` 等；名字本身含 ``.`` 的成员无法从文本构造（打印侧会把它转义成 ``\u002E``，读回即改名，见第 10 节）。第二操作数是描述符，按 JVMS 校验。

接口形态由助记符决定常量池条目与 opcode 的组合：``invokeinterface`` 落 INVOKEINTERFACE 加 InterfaceMethodref；三个别名 ``invokevirtualinterface`` ``invokestaticinterface`` ``invokespecialinterface`` 落对应 invoke opcode 加 InterfaceMethodref（用于字节码里 opcode 与常量池条目交叉的产物）。打印面遇 InterfaceMethodref 而 opcode 不是 INVOKEINTERFACE 时补 ``interface`` 后缀，往返闭合。

.. _syntax-insn-indy:

5.6 invokedynamic 与方法句柄
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code:: text

    invokedynamic apply (Ljava/lang/Object;)Ljava/lang/Runnable; { invokestatic, java/lang/invoke/LambdaMetafactory.metafactory, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }

四个操作数：调用点名、调用点方法描述符、引导句柄、静态参数数组（空为 ``{ }``）。句柄两种写法：三元素数组 ``{ <kind>, <Owner.name>, <描述符> }``，或短形式 ``LambdaMetafactory.metafactory``（展开为固定全限定句柄）。

句柄 kind 九个：``getfield`` ``getstatic`` ``putfield`` ``putstatic`` ``invokevirtual`` ``invokestatic`` ``invokespecial`` ``newinvokespecial`` ``invokeinterface``（``jvm_compile_detail.hpp``）。字段类句柄的第三元素按字段描述符校验，其余按方法描述符校验。

引导方法追加到类级 BootstrapMethods 属性，相同句柄与参数复用既有条目。静态参数是常量十族：整型、long、float、double、String、Class、MethodType、MethodHandle、condy（嵌套 Dynamic，``bsmArgOf``）。

.. _syntax-insn-type:

5.7 类型指令与数组
^^^^^^^^^^^^^^^^^^

- ``new`` ``checkcast`` ``instanceof``：操作数经内部名归一（``L..;`` 剥壳，数组描述符原样）落 Class 常量。
- ``anewarray``：操作数按 JVMS 6.5 是组件类型（``anewarray java/lang/String`` 落组件内部名）。写数组描述符也接受，语义是组件为该数组类（升一维）。
- ``newarray``：八个基本类型词之一，落 atype 4 到 11。
- ``multianewarray``：数组描述符加维数。

.. _syntax-canonical:

6. 规范化反汇编
---------------

.. _syntax-canonical-contract:

6.1 契约
^^^^^^^^

``jhasm::disassemble`` 的产物是可重新汇编的规范化 JAsm（``printer/jvm_printer.cpp``）：同一 classfile 字节恒产生同一文本；行尾纯 LF；注释不保留；重新汇编得到结构等价的 classfile，不保证字节逐一同。

.. _syntax-canonical-layout:

6.2 版式
^^^^^^^^

顶层顺序：``.inner``（每条一行）→ ``.sourcefile`` → ``.outer-class`` → ``.outer-method`` → ``.nest-host`` → ``.nest-member`` → ``.permitted-subclass`` → record 组件（注解、签名、``.record-component``）→ 类注解四形 → ``.signature`` → ``.super``（非空才打印）→ ``.implements`` → ``.class`` 行 → 字段 → 空行 → 方法 → ``}``。

字段各自的前置注解与签名缩进 4 空格打在 ``.field`` 行之前；方法同理。方法间空一行，字段间不空。方法声明体内先 ``exceptions``（有才打，段尾带逗号）后 ``code``。``code`` 内标签缩进 8 空格（``L0:``），指令与 ``line`` 缩进 12 空格；锚定在方法体末尾的标签按名字排序打在最后。

abstract/native 方法打 ``{ }`` 空体。

.. _syntax-canonical-behavior:

6.3 规范化行为清单
^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - 输入形态
     - canonical 输出
   * - 任意标签名
     - 按锚定顺序重编号为 ``L0`` 起步；未被分支或异常表引用的标签直接消失（4.1）
   * - ``parameters`` 名字
     - 不回显，一切局部变量按槽位号打印；槽 0 到 3 打 ``_N`` 短格式（``iload_0``），其余打长格式（``iload 4``）
   * - ``bipush``/``sipush``（本仓汇编产生）
     - 已在编译期并成 ``ldc``
   * - ``ldc_w`` / ``ldc2_w`` 输入拼写
     - 按常量类别归一：cat1 恒打 ``ldc``，cat2 恒打 ``ldc2_w``；输入拼写与池索引宽度不回显
   * - ``ldc 'e'``
     - ``ldc 101``（Integer 十进制）
   * - float/double 字面量
     - ``%.9g`` / ``%.17g`` 可往返表示，剥指数正号，无小数点与指数时补 ``.0``，带 ``F``/``D`` 后缀；实测 ``ldc 3.14F`` 打 ``ldc 3.1400001F``、``ldc 0.1`` 打 ``ldc2_w 0.10000000000000001D``；NaN 与无穷打 ``NaNF``/``NaND``/``InfinityF``/``InfinityD`` 形态（无穷恒带后缀）
   * - long 字面量
     - 十进制加 ``L``
   * - Class 常量
     - ``L名;`` 形态；数组类原样数组描述符
   * - 字符串
     - 双引号；换行打 ``\u000A`` 不用 ``\n``
   * - 含空白、逗号、点、冒号、花括号、引号、反斜杠或控制区段的名字
     - 标识符转义（``escape_literal``）：上述字符与 13 个控制或不可见区段转 ``\uXXXX`` 大写十六进制
   * - ``invokevirtual`` 等指向 InterfaceMethodref
     - 助记符补 ``interface`` 后缀
   * - ``tableswitch``
     - 单行对象 ``{ min: .., max: .., cases: { .. }, default: .. }``；``max`` 按 min 加目标数推导回显
   * - ``lookupswitch``
     - 单行键值对，键按落地（已排序）顺序回显
   * - ``invokedynamic``
     - 长形式四段；BootstrapMethods 内联进调用点，不单独成节
   * - ``.inner``
     - 键序固定 ``name``（有则打）、``inner``、``outer``（有则打）
   * - MethodParameters、StackMapTable、Deprecated、SourceDebugExtension
     - 不打印（MethodParameters 的参数名信息在 canonical 中不可恢复）

.. _syntax-canonical-gap:

6.4 往返保证的边界
^^^^^^^^^^^^^^^^^^

文本级往返（编译、打印、再编译、再打印，两次文本相同）由 ``test/smoke`` 锁定。三条已知不闭合：

1. 名字里真含空白、逗号、点等字符的成员（只能来自外部 classfile）：打印成 ``\uXXXX``，读回是字面文本，第二次汇编得到的是改名后的产物（mjt 侧同结论）。
2. 类型注解：canonical 的位置参数形态回读报错（3.7，实测）。
3. 未被引用的标签：源码合法，lower 后消失，canonical 不回显（4.1，实测）。

.. _syntax-compile:

7. 编译语义与诊断
-----------------

- 目标代次由汇编选项决定（``JvmCompilerOptions.majorVersion``，默认 61；``minorVersion`` 默认 0）。文本里没有版本声明，``.version`` 不是本方言的关键字。
- ``maxLocals`` 起点是参数槽总数，帧分析后采纳计算值。
- 帧分析为 Strict 策略；class 主版本 50 起生成 StackMapTable。未知引用类型的合并缺省落到 ``java/lang/Object``，需要精确合并时注入 ``InheritanceChecker``。
- 方法体超过 65535 字节在落盘前报错，诊断带实际字节数。
- 诊断位置 1 基行列（UTF-16 码元），编译期错误归属到成员并细化到指令；``assemble`` 任一阶段失败返回错误列表。
- ``jhasm::analyze`` 对全部非 abstract/native 方法做类型数据流分析，返回逐指令帧快照，单个方法失败不影响其他方法的快照（部分成功语义）。

.. _syntax-upstream:

8. 与上游 jumanji144/Jasm 的差异
--------------------------------

只列经本仓源码核对的条目；上游行为以 ``D:\WorkSpace\Jasm`` checkout（revision ``aacebfa``）为参照。

1. 标识符反转义语义相反：上游把标识符里的 ``\uXXXX`` 求值为对应字符；本仓原样保留（1.4）。
2. 字符串换行的 canonical 形态：上游打 ``\n`` 短形式；本仓打 ``\u000A``（``escape_string``）。
3. 常量装载的归一方向相反。读侧两个方言一致，都注册 ``ldc``/``ldc_w``/``ldc2_w`` 三个拼写并进同一常量装载语义（上游三拼写都进 ``visitLdcInsn`` 单口）。canonical 打印侧相反：上游把小值常量折叠成 ``iconst_N``/``bipush``/``sipush``/``lconst_N``/``fconst_N``/``dconst_N``，其余恒打 ``ldc``，cat2 也不打 ``ldc2_w``；本仓把 push 类编译期并入 ``ldc``，cat2 打 ``ldc2_w``，``ldc_w`` 永不回显。
4. 标签重编号、槽位索引化、``parameters`` 不回显等规范化行为以本仓 printer 为准（6.3）。

词法骨架（token 分类、数字后缀、容器值、声明结构）与上游一致。

.. _syntax-pitfalls:

9. 生成注意事项
---------------

面向自动生成 JAsm 文本（AI 或脚本）的陷阱清单，每条都对应一节：

.. list-table::
   :header-rows: 1

   * - 陷阱
     - 事实
   * - 标识符位置写 ``\uXXXX``
     - 就是字面文本，不反转义（1.4）
   * - 真名含空白、逗号、点、冒号、花括号
     - 无法从源码构造；打印侧会转义成 ``\uXXXX``，读回即改名（6.4）
   * - ``0b`` 二进制字面量
     - 词法能过，整型语义按十进制前缀截断，``ldc 0b101`` 落 0；要写二进制就换算成十进制或十六进制（1.6）
   * - 负整数操作数
     - 十进制可写（``bipush -16`` 落 ``ldc -16``）；十六进制负形 ``bipush -0x10`` 直接拒绝，先换算成十进制（1.6）
   * - 数字下划线分隔
     - ``bipush 1_0`` 等整数操作数拒绝；``ldc 1_000`` 截断成 1（1.6）
   * - ``ldc true`` / ``ldc false``
     - 指令操作数位不收布尔，直接拒绝；布尔只能出现在字段常量与注解值里（2、5.2）
   * - 指令跨行
     - 操作数必须与助记符同行，跨行操作数不归属该指令（4.1）
   * - 大小写
     - 助记符、修饰符、常量拼写（``NaN``/``Infinity``）全部大小写敏感；不存在小写 ``infinity``
   * - ``.version``
     - 不是关键字；代次由汇编选项决定（第 7 节）
   * - 与 canonical 文本逐行 diff
     - 标签重编号、槽位化、``bipush`` 并入 ``ldc``、常量形态归一都是规范化行为，逐行比较必然不同（6.3）
   * - 注解整型
     - 没有 byte/short 形态，外部产物的 B/S 元素打印为十进制数字，回读落 I（3.6）
   * - 类型注解文本回读
     - canonical 形态回读报错，见 3.7 的已知不对称
   * - ``tableswitch`` 的 ``max``
     - 只做语法校验，落地边界由 ``min`` 加 cases 数推导，写错 ``max`` 不报错但分支目标数会变（5.4）
   * - 块注释 ``\*/``
     - 不终止注释，反斜杠转义吞掉星号（1.2）
   * - ``.implements`` 写进 ``.class`` 行
     - 不是类行修饰符，报 Invalid modifier；必须另起一行（3.3）
   * - 未被引用的标签
     - 源码合法，落地时消失，canonical 不回显；不要用标签当注释占位（4.1）
   * - ``code: {}`` 空方法体
     - 非 abstract/native 方法空体被帧分析拒绝（没有 return 出口）；抽象与本地方法用 ``{ }`` 空体（3.5）

.. _syntax-evidence:

10. 证据口径
------------

本文结论依据的实现文件：

- 词法：``src/jhasm/parser/tokenizer.cpp``、``token_type.hpp``
- 值文法与声明：``src/jhasm/parser/declaration_parser.cpp``、``ast_processor.cpp``、``src/jhasm/ast/*``
- 指令与操作数校验：``src/jhasm/instructions/jvm_instructions.cpp``
- 编译语义：``src/jhasm/compile/jvm_compiler.cpp``、``code_compiler.cpp``、``jvm_constants.cpp``、``jvm_annotations.cpp``、``jvm_compile_detail.hpp``
- 打印：``src/jhasm/printer/jvm_printer.cpp``、``src/jhasm/util/escape_util.cpp``
- 公开 API：``src/jhasm/jhasm.hpp``、``compile/jvm_compiler.hpp``

实测依据分两层。一层是 ``test/smoke/roundtrip_smoke.cpp``（汇编、分析、规范化稳定与真实语料往返）。另一层是写作本文时用 0.3.3 现场构建的探针逐条运行复核的约 30 项行为断言：``bipush`` 并入 ``ldc``、``0b`` 截断落 0、负十六进制在 ``ldc`` 位正常落值而在整数操作数位拒绝、下划线截断、``ldc true`` 拒绝、字段布尔常量落 ``1``、标识符不反转义、未知字符串转义吞字符、换行 canonical 为 ``\u000A``、``_N`` 短格式回显、未引用标签消失、``lookupswitch`` 键排序、``tableswitch`` max 推导、无 ``.super`` 时 super_class 落 0（字节级核对）、空 code 体拒绝、尾逗号接受、标签与指令同行可解析、``ldc_w`` 打 ``ldc``、float 打 ``%.9g`` 形态、无后缀浮点按 double 打 ``%.17g``、Class 与 MethodType 常量往返、接口助记符往返、异常表 ``*`` 往返、类型注解读合法写不对称、``parameters`` 不回显。标注（源码推断）的条目：无。若后续实现与本文冲突，以实现为准并回改本文。
