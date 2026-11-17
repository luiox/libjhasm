.. _development:

开发指南
========

构建环境
--------

在仓库根目录运行以下命令。准备 Git、xmake 3.0.9 和支持 C++17 的编译器。
Windows 使用带 C++ 工具集的 Visual Studio 或 Build Tools，Linux 使用 GCC 或 Clang。
持续集成在 Windows 和 Linux 上使用相同的 xmake 版本。

首次在本地仓库构建时，注册 ``luiox-repo`` 获取 ``libca``。
``xmake.lua`` 固定 ``libca`` 为 0.0.16，模块为 ``core,str,crypto,zip``。
``libjbc`` 0.4.24 由仓内 ``xmake/packages/libjbc.lua`` 定义，固定到提交
``82ce8bf7951c0bfd18ad12260fea7ba51ecbaae8``，与本库共用同一套 ``libca`` 配置。
``libca`` 是公开依赖，``libjbc`` 是静态库的私有实现依赖。

Windows 静态库
^^^^^^^^^^^^^^

以下命令在 PowerShell 中执行。

.. code-block:: powershell

   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
   xmake f -p windows -m release -y
   xmake build -y libjhasm

Linux 静态库
^^^^^^^^^^^^

.. code-block:: bash

   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
   xmake f -p linux -m release -y
   xmake build -y libjhasm

本地测试与调试
--------------

``--with_tests=y`` 开启 GoogleTest 单测目标 ``libjhasm_unittest``，需要显式构建。
``--with_smoke=y`` 开启独立目标 ``libjhasm_smoke``。配置时用 ``-m debug`` 生成调试版本，
然后用 ``xmake run -d libjhasm_unittest`` 通过本机调试器运行。

Smoke 覆盖汇编和分析、两种方言的文本往返，以及仓内 ``test/smoke/corpus/test_class.class``
语料的重编译幂等。传入该文件的绝对路径，供从目标输出目录启动的程序读取。
准备 Java 17 或更新版本的 JDK，并确认 ``java -version`` 能在当前终端运行。
Smoke 会调用 ``java -Xverify:all``，分别加载 s 和 r 汇编的测试类。
语料文件缺失或 ``java`` 无法运行时，程序会打印相应的 ``[skip]``，排查时留意这两项。

Windows 调试与测试
^^^^^^^^^^^^^^^^^^

.. code-block:: powershell

   xmake f -p windows -m debug --with_tests=y --with_smoke=y -y
   xmake build -y libjhasm_unittest
   xmake run libjhasm_unittest
   xmake build -y libjhasm_smoke
   java -version
   xmake run libjhasm_smoke (Join-Path (Get-Location) 'test/smoke/corpus/test_class.class')

Linux 调试与测试
^^^^^^^^^^^^^^^^

.. code-block:: bash

   xmake f -p linux -m debug --with_tests=y --with_smoke=y -y
   xmake build -y libjhasm_unittest
   xmake run libjhasm_unittest
   xmake build -y libjhasm_smoke
   java -version
   xmake run libjhasm_smoke "$(pwd)/test/smoke/corpus/test_class.class"

``.github/workflows/ci.yml`` 目前在推送到 ``main`` 和目标分支为 ``main`` 的拉取请求上运行。
Windows 和 Linux 构建任务均运行静态库构建与 smoke，单测按上面的命令手动运行。
文档任务运行在 Linux。目标分支为其他分支的拉取请求可用这里的本地命令复现检查。

文档构建与引用检查
------------------

文档任务使用 Python 3.12、Sphinx 9.1.1 和 Doxygen 1.18.0。
将 Python 3.12 环境和 Doxygen 1.18.0 加入 ``PATH``，在仓库根目录执行构建命令。

.. code-block:: console

   python -m pip install "sphinx==9.1.1"
   python doc/check_refs.py .
   sphinx-build -b html -E -W --keep-going doc doc/_build
   doxygen --version
   doxygen doc/Doxyfile

``-E`` 每次重建 Sphinx 环境并重新读取所有源文档，``-W`` 将警告视为失败。
站点入口为 ``doc/_build/index.html``。
Doxygen 递归读取 ``src/jhasm`` 的 ``*.hpp``，生成的 API 站点在 ``doc/_apidoc/html``。
``WARN_AS_ERROR`` 会拦住无效命令、引用或参数名等文档错误，未写注释的符号仍可提取。

文档写作约定
------------

- 使用方法和示例写入 :doc:`manual`，跨模块设计写入 :doc:`design`，语法约定写入 :doc:`syntax`。
  新增 RST 页面时，把页面加入 ``doc/index.rst`` 的 ``toctree``，表格使用 ``list-table``。
- RST 中用 ``:doc:`` 引用页面、用 ``:ref:`` 引用小节。小节锚点必须唯一。
  代码注释用 ``doc:<label>`` 指向 ``.. _<label>:``，代码提及的仓内 RST 路径必须存在。
- 单个 API 的参数、返回值和失败约定写在头文件的 ``///`` 注释里，模块定位可写入 ``@defgroup``。
  RST 用“头注”指向对应的头文件名或组名，引用门禁会检查目标。
- Markdown 用于流程文档。仓内设计文档引用使用 RST，提及仓外 Markdown 时在同一行标明来源仓库。
