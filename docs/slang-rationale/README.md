# Slang 编译链路概览

## 目标受众与阅读提示

本文面向日常编写 SystemVerilog 的工程师，目的是把 `external/slang` 在本项目中从“接收源文件”到“输出结果（诊断信息 / 分析数据）”的完整流水线拆解出来。为了便于对照代码，文中在合适的位置注明了关键实现所在的源文件路径（相对仓库根目录）。如果对编译器术语不熟悉，可以先浏览“全局流程速览”章节，再按需深入后续小节。

---

## 全局流程速览

| 阶段 | 主要职责 | 关键类型 / 文件 |
| ---- | -------- | ---------------- |
| 1. 命令行驱动 | 解析 CLI 参数、构造选项、触发后续阶段 | `src/main.cpp`、`external/slang/source/driver/Driver.cpp` |
| 2. 源文件收集 | 展开文件模式、处理 library map、按逻辑单元组织文件 | `external/slang/source/driver/SourceLoader.cpp` |
| 3. 预处理与词法 | 处理宏、`include`、条件编译并产生记号流 | `external/slang/source/parsing/Preprocessor.cpp`、`.../Lexer.cpp` |
| 4. 语法分析 | 把记号解析为 `SyntaxTree`（语法树）并记录依赖元数据 | `external/slang/source/parsing/Parser.cpp`、`external/slang/source/syntax/SyntaxTree.cpp` |
| 5. 构建语义世界 | 将语法树灌入 `ast::Compilation`，绑定声明、建立符号表 | `external/slang/source/ast/Compilation.cpp` 及 `.../symbols` |
| 6. Elaborate & 常量折叠 | 实例化层次、求解参数、展开 `generate` / `defparam` | `external/slang/source/ast/Compilation.cpp`（`getRoot` / `elaborate`） |
| 7. 诊断汇总 | 收集解析和语义阶段产生的错误与警告 | 同上 |
| 8. 补充静态分析 | 数据流 / 驱动一致性 / 未使用对象检查 | `external/slang/source/analysis/AnalysisManager.cpp` |
| 9. 输出 | 打印顶层实例、输出诊断文本或 JSON | `external/slang/source/driver/Driver.cpp` |

---

## 1. 命令行驱动

- 可执行入口位于 `src/main.cpp`，它创建 `slang::driver::Driver`，依次调用：
  1. `Driver::addStandardArgs` 配置所有 CLI 选项；
  2. `Driver::parseCommandLine` / `processOptions` 读取参数；
  3. `Driver::parseAllSources` 触发源文件收集与解析；
  4. `Driver::runFullCompilation` 完成 elaboration、分析与报告。
- `Driver` 负责维护 `DiagnosticEngine` 和 `SourceManager`（用于集中处理文件与诊断），并把所有阶段打包成一次流水执行（`external/slang/source/driver/Driver.cpp`）。

理解 Driver 的作用就像熟悉一条综合脚本：它把命令行的 include、define、library 配置流向后续的处理组件。

---

## 2. 源文件收集与编译单元划分

### 2.1 SourceLoader 核心职责

- `SourceLoader`（`external/slang/source/driver/SourceLoader.cpp`）是所有输入文件的调度中心，针对命令行的 `-f`/`+libext`/`-C` 等指令搭建编译单元：
  - `addFilesInternal` 展开通配符路径、去重，并记录每个文件归属的库 (`SourceLibrary`) 及是否为 library 单元。
  - `addLibraryMaps` 支持解析 IEEE library map 文件，递归处理 `library` / `include` 声明。
  - `addSeparateUnit` 允许把一组文件强制打包成独立的 `SyntaxTree`，常用于 `-C` 列表场景。
- `loadAndParseSources` 是收集阶段的主干函数：
  1. 根据 `Driver` 填充的 `Bag`（选项包）创建预处理 / 解析配置；
  2. 多线程（可选）读取文件到 `SourceBuffer`；
  3. 针对不同模式（单编译单元、library 继承宏等）安排何时解析；
  4. 在找不到被引用模块/包时，利用 `searchDirectories` + `searchExtensions` 进行二次搜索补齐。

### 2.2 SourceManager 的角色

- `SourceManager`（`external/slang/source/text/SourceManager.cpp`）提供统一的文件 ID、行列号换算、`include` 路径查找及缓存管理。`Driver::addStandardArgs` 把 `-I` / `--isystem` / `--disable-local-includes` 等选项全部注入它。
- 所有后续阶段都通过 `SourceManager` 查号定位，确保诊断信息能精确还原到原始文件。

---

## 3. 预处理：宏、条件编译与 include

- 每个编译单元在 `SyntaxTree::create`（`external/slang/source/syntax/SyntaxTree.cpp`）内部构造 `parsing::Preprocessor`，该对象会：
  - 继承命令行与 `SourceLoader` 传递下来的宏表；
  - 根据 `PreprocessorOptions` 控制递归 `include` 深度、宏忽略列表、语言版本等；
  - 利用 `Lexer`（`external/slang/source/parsing/Lexer.cpp`）逐字符生成 `Token`，并处理 `translate_off/on`、`pragma protect` 等注释指令（详见 `Preprocessor_pragmas.cpp`）。
- 预处理阶段产出的不仅是纯净的 token 流，还包含：宏展开痕迹、`include` 元数据、条件编译中被禁用的区域信息。这些信息最终记录在 `ParserMetadata` 中，便于后续生成依赖文件或执行 lint。

对 SystemVerilog 工程师而言，这是熟悉的“仿真器前端”功能，但注意 slang 的实现把它与后续解析紧密耦合，所有诊断（如未定义宏）也在这里实时发出。

---

## 4. 语法分析：构造 SyntaxTree

- `Parser`（`external/slang/source/parsing/Parser.cpp`）读取预处理后的记号流，生成 `syntax::SyntaxNode` 树：
  - `parseCompilationUnit` 负责顶层结构；
  - `Parser_*` 辅助文件把表达式、语句、成员等语法分块实现；
  - 同时收集 `ParserMetadata`，包含默认 nettype、timescale、宏定义、`bind` / `defparam` 标记等。
- `SyntaxTree` 对象（`external/slang/source/syntax/SyntaxTree.cpp`）持有：
  - 语法树根节点与 bump allocator；
  - 诊断缓存（词法、语法错误）；
  - 继承宏列表与 `include` 指令记录；
  - 源文件 ID 列表（支撑依赖输出与 IDE 集成）。

语法树阶段仍停留在“文本结构”层面，还没有建立模块实例之间的引用关系。但它已经足够描述语言层面的层级、参数、端口等信息。

---

## 5. 将语法树灌入语义世界：Compilation

- `ast::Compilation`（`external/slang/source/ast/Compilation.cpp`）是语义阶段的核心容器。`Driver::createCompilation` 会把全部语法树（含 library map）交给 `Compilation::addSyntaxTree`：
  - 为每个 `SyntaxTree` 创建对应的 `CompilationUnitSymbol` 并挂到 `RootSymbol`；
  - 从 `ParserMetadata` 中抽取默认 nettype、timescale、`celldefine` 等配置，存入 `syntaxMetadata`；
  - 将顶层定义（module/program/interface/package 等）注册到 `definitionMap` 供后续查找；
  - 统计显式的顶层实例与全局引用（`globalInstantiations`），用于 top 寻找与 library 解析。
- `Compilation` 构造函数会初始化所有内建类型、net type、系统任务函数，并准备一个默认库（命名为 `work`，除非命令行覆盖）。对于 Verilog 用户，这相当于构建一个“仿真工作库”。

此阶段仍未真正“跑”出实例化结果，但所有语法层面的对象已经被组织到符号表结构里，为 elaboration 做准备。

---

## 6. 顶层选择与 elaboration

### 6.1 `Compilation::getRoot`

- 在第一次访问 `Compilation::getRoot()` 时触发：
  1. 解析 CLI 指定的 `top` / `defparam` / `bind` 覆盖（`parseParamOverrides`）；
  2. 将 library map 中的库与默认库顺序解析成 `defaultLiblist`；
  3. 遍历所有定义，找出可作为顶层的 module / program（无未赋默认值的参数、未在别处被实例化、属于默认库）；
  4. 如果启用了 `--top`，则解析 CLI 提供的层次路径并标记为已实例化。
- 在此过程中，`RootSymbol` 会把 `DefinitionSymbol` 与 `InstanceSymbol` 之间的关系串接起来，为 elaboration 的深度优先展开打下基础。

### 6.2 `Compilation::elaborate`

- `Compilation::getAllDiagnostics()` 会调用 `elaborate()`（`external/slang/source/ast/Compilation.cpp:1475+`），核心步骤：
  - 创建 `DiagnosticVisitor`（见 `external/slang/source/ast/ElabVisitors.h`）深度遍历所有符号、语句与表达式；
  - 每当访问到模块定义时，通过 `InstanceSymbol` 实例化并求值参数，必要时触发 `generate` 展开；
  - 依赖 `EvalContext` 和常量表达式求值器（`external/slang/source/ast/EvalContext.cpp`）折叠参数、`if` 条件、连续赋值的 RHS 等；
  - 处理 `bind`、`defparam`、`config` 等 SystemVerilog 特性，保证应用顺序与 IEEE 规则一致；
  - 记录 DPI / interface / modport 等跨模块约束，待遍历结束后统一校验。
- Elaboration 结束后，`Compilation` 会把未被引用的定义、未使用的 `config rule` 等信息缓存下来，供后续分析或诊断使用。

---

## 7. 诊断体系

- 解析阶段的错误在 `SyntaxTree::diagnostics()` 中缓存，通过 `Compilation::getParseDiagnostics()` 汇总。
- 语义阶段的错误通过 `Compilation::addDiag` 纳入 `ASTDiagMap`，`DiagnosticVisitor` 遍历时即时记录（例如类型不匹配、非法层次引用）。
- `Driver::reportCompilation` 会在构造 `Compilation` 后先输出顶层实例列表，再遍历 `getAllDiagnostics()`，借助 `DiagnosticEngine` 格式化为文本或 JSON：
  - 支持彩色输出（`fmt::color`）；
  - 支持 `--diag-json` 将诊断写入文件或标准输出；
  - 诊断条目会根据 `SourceManager` 的换算打印准确的文件名、行列号。

如果启用了 lint-only 模式或错误上限（`--max-constexpr-steps` 等），对应的逻辑也都在 `Driver::reportDiagnostics` 中处理。

---

## 8. 补充静态分析（AnalysisManager）

- `Driver::runAnalysis` 在成功 elaboration 后调用 `analysis::AnalysisManager::analyze`（`external/slang/source/analysis/AnalysisManager.cpp`），执行更高层的语义检查：
  - 以 `AnalyzedScope` 包装实例 / 包，遍历语句获取驱动、敏感度等信息；
  - `DriverTracker` 检查信号驱动冲突与缺失；
  - `CheckUnused`（默认开启）会对未使用的局部变量、未引用的定义发出告警；
  - 可通过 CLI 打开更多 flag（例如多驱动局部变量或重复赋值的例外）。
- 分析阶段不能修改 `Compilation`，因此 `Driver::runAnalysis` 在调用前会 `compilation.freeze()`，并在结束后解冻。这样可以保证线程安全，也避免分析过程意外生成新的语义节点。

分析诊断通过 `AnalysisManager::getDiagnostics` 统一收集，再交给 `DiagnosticEngine` 输出。

---

## 9. 输出与结束

- `Driver::reportDiagnostics`（`external/slang/source/driver/Driver.cpp`）负责最终的展示：
  - 若用户请求 JSON (`--diag-json`)，则优先输出结构化诊断；
  - 标准文本诊断会按照错误 / 警告数量打印摘要；
  - `Driver::reportCompilation` 还会在成功 elaboration 时打印顶层实例的层次名，方便确认自动识别的设计入口。
- `runFullCompilation` 的返回值最终决定 CLI 退出码：有错误则返回 `false`。

---

## 10. 常见问题排查思路

1. **文件未被解析？** 检查 `SourceLoader::fileEntries` 里是否存在（可通过 `--all-depfile` 产出）。注意 library 模式下同名文件的优先级由 `library map` 的 rank 决定。
2. **宏或 include 设置无效？** 追踪 `Driver::addParseOptions` 是否把选项写入 `PreprocessorOptions`，然后确认 `Preprocessor` 的 `resetAllDirectives` 是否被后续覆盖。
3. **顶层没有被实例化？** 查看 `Compilation::getRoot` 的筛选条件（参数默认值、library、全局实例）。必要时可通过 `--top` 指定。
4. **分析阶段漏报 / 误报？** 检查对应 flag 是否开启（`Driver::options.analysisFlags`），同时确认编译未因早期诊断终止 elaboration（`Compilation::hasFatalErrors()`）。

---

## 11. 总结

Slang 在本项目中承担了完整的 SystemVerilog 前端职责：从命令行驱动、预处理、语法解析，到 elaboration 与语义分析，并最终生成诊断结果。对于 Verilog 工程师，理解这条链路有助于：

- 调适复杂编译脚本与库映射；
- 判断某个诊断是在词法/语法还是 elaboration 阶段产生；
- 利用 `AnalysisManager` 的检查扩展 lint 能力；
- 在需要时插入自定义阶段或读取 elaboration 结果（例如顶层实例树、已求值的参数）。

掌握上述阶段后，就能更高效地定位编译问题，并根据需求扩展当前工具链。

