libjhasm 文档
=============

libjhasm 是一个 C++17 库，用于把 Jasm 文本汇编成 JVM classfile、将 classfile
反汇编为 Jasm，以及分析方法的操作数栈和局部变量。:doc:`manual` 从一个完整示例
开始，:doc:`syntax` 提供完整的语法参考。

.. toctree::
   :maxdepth: 2
   :caption: 使用与语法

   manual
   syntax

.. toctree::
   :maxdepth: 2
   :caption: 设计与开发

   design
   design-dialects
   development

.. _documentation-conventions:

文档规范
--------

RST 记录使用方法、语法规格和跨模块设计。公开头文件的 Doxygen 注释记录符号级的
入参、返回值、失败模式和所有权。Markdown 用于 README 等流程入口。

正文通过 ``:doc:`` 和 ``:ref:`` 引用其他页面或小节。代码注释可通过
``doc:<label>`` 指向小节。正文使用 ``X`` 头注 时，``X`` 应是确实存在的
公开头文件名或 Doxygen 组名。构建和检查命令见 :doc:`development`。
