# Slang AST 访问指南

本文概述如何在 Slang 中访问和遍历语义 AST，包括入口位置、访问工具与类型判断技巧。内容参考 `external/slang/include/slang/ast` 目录下的头文件及 `ASTVisitor` 相关实现。

## 获取 AST 根节点
1. 通过 `slang::ast::Compilation::addSyntaxTree` 注册解析后的 `SyntaxTree`。
2. 调用 `Compilation::getRoot()` 得到 `RootSymbol`（`include/slang/ast/Compilation.h:253`）。
3. `RootSymbol` 同时继承自 `Symbol` 与 `Scope`，可使用 `members()` 遍历顶层定义（`include/slang/ast/symbols/CompilationUnitSymbols.h:77` 与 `include/slang/ast/Scope.h:190`）。

示例：
```cpp
Compilation compilation;
compilation.addSyntaxTree(tree);
const RootSymbol& root = compilation.getRoot();
for (const Symbol& member : root.members()) {
    // 逐个访问顶层模块 / 接口 / 包等
}
```

## 使用 ASTVisitor 遍历
- `slang::ast::ASTVisitor` 提供递归遍历框架，可按需开启语句与表达式访问（`include/slang/ast/ASTVisitor.h:70`）。
- 自定义访问时实现 `handle(const NodeType&)`，在需要继续向下时调用 `visitDefault(node)`。
- 也可通过 `makeVisitor` 快速创建基于 lambda 的访问器（`include/slang/ast/ASTVisitor.h:139`）。

示例：统计模块实例数量，并访问内部语句与表达式。
```cpp
auto visitor = ast::makeVisitor(
    [&](auto& self, const ast::InstanceSymbol& inst) {
        ++instanceCount;
        self.visitDefault(inst); // 继续访问下一级
    }
);
root.visit(visitor);
```

## Symbol / Statement / Expression 的访问入口
- 所有 `Symbol` 派生类型支持 `visit(visitor)`，会根据 `SymbolKind` 分派（`include/slang/ast/ASTVisitor.h:166`）。
- `Statement` 与 `Expression` 也提供同名 `visit` 方法，可结合语句或表达式访问标志通过 `ASTVisitor` 递归子节点（`include/slang/ast/Statement.h:232`，`include/slang/ast/Expression.h:347`）。
- 访问语句树时，许多符号类（如 `ProceduralBlockSymbol`）会通过 `visitStmts` / `visitExprs` 把语句、表达式挂接给访问器。

## 判断与转换节点类型
- 每个节点都带有 `kind` 枚举（如 `SymbolKind`、`StatementKind`、`ExpressionKind`），可直接比较判断类型。
- 常用辅助接口：
  - `node.as<ConcreteType>()`：断言并返回对应派生类型（`include/slang/ast/Symbol.h:188` 等）。
  - `node.as_if<ConcreteType>()`：类型不匹配时返回 `nullptr`，用于条件处理。
  - `ConcreteType::isKind(kind)`：静态判断 `kind` 是否属于该类型。
- `Scope::membersOfType<T>()` 可直接遍历某类成员，例如：
```cpp
for (const auto& def : root.membersOfType<DefinitionSymbol>()) {
    // def.as<DefinitionSymbol>() 已经成立
}
```

## 实践提示
- `Symbol` 派生类常提供额外的遍历接口（如 `InstanceSymbol::body`），在 `ASTVisitor` 的 `visitDefault` 中已经帮助展开。
- 通过 `getSyntax()` 可回溯到产生该 AST 节点的语法节点，便于定位源代码（`include/slang/ast/Symbol.h:128`）。
- 遇到 `bad()` 节点（语义无效）时，默认 `ASTVisitor` 会跳过，若需分析错误分支可将模板参数 `VisitBad` 设为 `true`。

依托上述接口，即可在 Slang 的 AST 中实现自定义遍历、分析或转换逻辑。
