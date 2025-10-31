# Slang AST 访问方法细化

本文汇总 `external/slang/include/slang/ast` 与 `docs/reference/slang-netlist` 中介绍的 AST 访问接口，说明它们的职责、联动方式，并给出可直接编译的示例代码。重点关注语义 AST（`slang::ast` 命名空间）所支持的访问方法。

## 根节点与作用域遍历

- `Compilation::getRoot()`：在添加完语法树后调用即可获得 `RootSymbol`，这是遍历 AST 的统一入口（`external/slang/include/slang/ast/Compilation.h:292`）。
- `Scope::members()` / `membersOfType<T>()`：返回当前作用域内成员的迭代器区间，访问顶层定义或按类型过滤非常方便（`external/slang/include/slang/ast/Scope.h:203` 与 `external/slang/include/slang/ast/Scope.h:209`）。
- `Symbol::as<T>()` / `as_if<T>()`：对具体派生类型进行断言式或条件式转换，结合 `Scope::members()` 即可快速定位目标节点（`external/slang/include/slang/ast/Symbol.h:210`）。

示例：遍历根作用域中的所有模块与接口定义，并打印其层次路径。

```cpp
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/DefinitionSymbols.h"

void dumpTopDefinitions(ast::Compilation& compilation) {
    const ast::RootSymbol& root = compilation.getRoot();
    for (const ast::DefinitionSymbol& def : root.membersOfType<ast::DefinitionSymbol>()) {
        fmt::print("definition {} @ {}\n", def.name, def.getHierarchicalPath());
    }
}
```

## 分派式访问方法

语义节点都提供 `visit` 风格的分派函数，会根据枚举类型将控制权转发给访问器，实现统一的双重分派。主要接口如下表所示：

| 调用入口 | 处理的枚举 | 典型用途 | 定义位置 |
| --- | --- | --- | --- |
| `Symbol::visit(visitor)` | `SymbolKind` | 对符号节点（模块、端口、变量、类型等）进行访问 | `external/slang/include/slang/ast/ASTVisitor.h:168` |
| `Statement::visit(visitor)` | `StatementKind` | 访问程序块内部的语句树 | `external/slang/include/slang/ast/ASTVisitor.h:282` |
| `Expression::visit(visitor)` | `ExpressionKind` | 访问所有表达式节点 | `external/slang/include/slang/ast/ASTVisitor.h:345` |
| `TimingControl::visit(visitor)` | `TimingControlKind` | 访问时序控制，如 `@(...)`、`#delay` | `external/slang/include/slang/ast/ASTVisitor.h:324` |
| `Constraint::visit(visitor)` | `ConstraintKind` | 访问随机约束块 | `external/slang/include/slang/ast/ASTVisitor.h:448` |
| `AssertionExpr::visit(visitor)` | `AssertionExprKind` | 访问断言表达式 | `external/slang/include/slang/ast/ASTVisitor.h:468` |
| `BinsSelectExpr::visit(visitor)` | `BinsSelectExprKind` | 访问覆盖率 bin 选择 | `external/slang/include/slang/ast/ASTVisitor.h:492` |
| `Pattern::visit(visitor)` | `PatternKind` | 访问 `case` / `inside` 模式 | `external/slang/include/slang/ast/ASTVisitor.h:510` |
| `RandSeqProductionSymbol::ProdBase::visit(visitor)` | `ProdKind` | 访问随机序列产生式 | `external/slang/include/slang/ast/ASTVisitor.h:528` |

这些接口与 `ASTVisitor` 模板协同工作：访问器只需实现 `visit` 或 `handle` 方法，即可自动获得对所有子节点的递归访问。

## ASTVisitor 模板参数

`slang::ast::ASTVisitor<TDerived, VisitStatements, VisitExpressions, VisitBad, VisitCanonical>` 提供五个模板参数控制遍历策略（`external/slang/include/slang/ast/ASTVisitor.h:68`）：

- `VisitStatements`：为 `true` 时，在 `visitDefault` 中自动调用节点的 `visitStmts`。
- `VisitExpressions`：为 `true` 时，会访问声明时的初始化表达式，及各类 `visitExprs`。
- `VisitBad`：为 `true` 时不会跳过 `bad()` 节点，适合错误分析。
- `VisitCanonical`：为 `true` 时，模块实例等节点会展开规范化后的 `body`。
- `TDerived`：访问器自身类型，需继承自模板实例（CRTP）。

若只需快速写一个访问器，可以使用 `ast::makeVisitor` 将若干 lambda 合成为一个具备默认遍历行为的临时访问器（`external/slang/include/slang/ast/ASTVisitor.h:139`）。

## 节点自带的辅助访问

多数符号会通过 `visitExprs` / `visitStmts` 弥补语句与表达式树的链接。例如：

- `InstanceSymbol::visitExprs` 会遍历端口连接表达式（`external/slang/include/slang/ast/ASTVisitor.h:411`）。
- `SubroutineSymbol::visitStmts` 会递归任务 / 函数体语句（`external/slang/include/slang/ast/ASTVisitor.h:443`）。

结合 `VisitExpressions=true` 与 `VisitStatements=false` 等配置，可以灵活控制访问范围。

## 示例：自定义端口收集器

下面的访问器演示如何通过 `ASTVisitor` 收集所有端口符号，并顺便解析端口连接表达式。

```cpp
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/PortSymbols.h"

struct PortCollector
    : ast::ASTVisitor<PortCollector,
                      /*VisitStatements=*/false,
                      /*VisitExpressions=*/true,
                      /*VisitBad=*/false,
                      /*VisitCanonical=*/true> {
    std::vector<const ast::PortSymbol*> ports;

    void handle(const ast::PortSymbol& port) {
        ports.push_back(&port);
        visitDefault(port); // 访问内部连接表达式（如存在）
    }
};

void collectPorts(ast::Compilation& compilation) {
    PortCollector visitor;
    compilation.getRoot().visit(visitor);
    fmt::print("port count = {}\n", visitor.ports.size());
}
```

## 示例：借鉴 slang-netlist 的遍历方式

`docs/reference/slang-netlist` 中的 `NetlistVisitor` 以 `VisitExpressions=true`、`VisitStatements=false`、`VisitCanonical=true` 的配置遍历实例体，核心逻辑如下（参考 `docs/reference/slang-netlist/include/netlist/NetlistVisitor.hpp:16` 与 `docs/reference/slang-netlist/source/NetlistVisitor.cpp:45` 之后的实现）：

```cpp
struct NetlistVisitor
    : ast::ASTVisitor<NetlistVisitor, false, true, false, true> {
    void handle(const ast::InstanceSymbol& symbol) {
        if (!symbol.body.flags.has(ast::InstanceFlags::Uninstantiated)) {
            symbol.body.visit(*this); // 访问实例体成员
            for (auto portConn : symbol.getPortConnections())
                handlePortConnection(symbol, *portConn);
        }
    }

    void handlePortConnection(const ast::Symbol& owner,
                              const ast::PortConnection& conn) {
        // 解析端口连接表达式，构建依赖信息
    }
};
```

通过这种组合方式，可以在访问器中集中处理关心的节点类型，同时依赖 `visitDefault` 自动向下遍历其他成员。

## 小结

slang AST 的访问方法以 `visit` 分派 + `ASTVisitor` 模板为核心，配合作用域遍历与类型转换工具，实现了类型安全、可定制的遍历框架。选择合适的模板参数与辅助访问函数，即可快速构建分析、转换或报告生成工具。

