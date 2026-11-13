libjhasm 设计规格
=================================

状态：机制先行。三层文档体系的门禁已就位（Sphinx ``-W`` + ``check_refs`` 引用
门禁 + Doxygen ``WARN_AS_ERROR``），设计规格随内容落位本目录。

三层按**权威来源**分工，扩展名即角色：代码 ``///`` Doxygen 注释是微观契约
（单符号参数/返回/失败模式与单模块背景定位，``@defgroup`` 组注承载）；
``doc/*.rst`` 是宏观规格（跨模块设计、模块思想与重要取舍，随内容落位）；
markdown 只承载流程文档（README 等）。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；``check_refs.py`` 校验
这些指向不会腐烂。RST 里写 ``X`` 头注 指向微观层的 ``@defgroup`` 组名或头
文件名。硬规则全文见 ``check_refs.py`` 模块 docstring。

.. toctree::
   :maxdepth: 2
   :caption: 设计与契约
