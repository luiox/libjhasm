# libjhasm

JVM 汇编文本（JAsm）的 C++ 工具库，命名空间 `jhasm`：文本解析（tokenizer /
声明解析）、语义分析（帧分析、继承检查）、编译到 class 文件、以及反向打印。

## 构建

依赖 libjbc（字节码 core/ir/analysis/type 面，项目内联 git 直连包，见
xmake/packages/libjbc.lua）与 libca（luiox-repo git 直连包，public 传递）。
libjbc 为实现层私有依赖，公开头零 jbc 类型。

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f
xmake build libjhasm
```

## 文档

- 用户手册演示如何从 C++ 汇编、反汇编和分析 classfile。[用户手册](doc/manual.rst)
- 语法规范提供声明、指令和操作数参考。[Jasm 语法规范](doc/syntax.rst)
- 设计文档记录模块分工、校验与编码规则。[架构设计](doc/design.rst) [r/s 方言设计](doc/design-dialects.rst)
- 开发指南提供构建、测试和文档检查命令。[开发指南](doc/development.rst)

## License

MIT，见 [LICENSE](LICENSE)。
