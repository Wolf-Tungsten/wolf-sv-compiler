# SyntaxTree 与 AST 的区别

本文基于 `external/slang` 目录下的源码与文档，梳理 Slang 编译流程中 `SyntaxTree` 与 AST 的定位差异。

## 二者定位
- **SyntaxTree**：封装词法分析、预处理与语法解析的结果，生成由 `SyntaxNode` 与 Token 组成的具体语法树（Concrete Syntax Tree）。它持有解析所需的内存资源，用于直接操作、遍历或重写源代码结构，保留所有格式与注释信息。参见 `docs/parsing.dox:123` 与 `include/slang/syntax/SyntaxTree.h:37`。
- **AST（Slang AST）**：位于 `slang::ast` 命名空间，由 `Compilation` 在接收多个 `SyntaxTree` 后进行符号绑定、类型解析与层次展开得到的抽象语法树。主要由 `Symbol`、`Statement`、`Expression` 等语义节点构成，用于表达设计的逻辑含义。参见 `include/slang/ast/Compilation.h:253` 及 `include/slang/ast/Symbol.h:36`。

## 工作流程
1. 通过 `SyntaxTree::fromFile` / `fromText` 等接口解析源文件，得到语法树并占有解析产生的内存。
2. 调用 `Compilation::addSyntaxTree` 将语法树交给编译阶段。
3. 调用 `Compilation::getRoot()` 完成语义分析与实例化，生成完整的 AST 层次结构。

## 使用场景建议
- 需要源代码级别操作（格式保持、重写、快速语法检查、处理不完整代码）时，使用 `SyntaxTree` 与其 `SyntaxVisitor`/`SyntaxRewriter` 辅助类。
- 需要语义信息（模块实例、类型推导、层次遍历、生成 JSON AST）时，使用 `Compilation` 生成的 AST。

## 互相关联
- AST 节点通常会保留指向原始 `SyntaxNode` 的引用，便于诊断与位置映射，但两者生命周期独立：`SyntaxTree` 负责具体语法结构，AST 承载语义模型。
