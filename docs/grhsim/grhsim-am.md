# GRHSIM 抽象机

> GRHSIM-AM（Abstract Machine）是 GRH 与具体仿真后端之间的统一执行模型。
> 本文只介绍核心概念和执行流程；具体 opcode 见
> [GRHSIM-AM 指令集](grhsim-am-instructions.md)，宿主服务边界见
> [HostEnvironment 参考定义](grhsim-host-environment.md)。
> 从 normalized GRH 经不可执行的 `am::LinearProgram`、AM activity scheduler 到最终
> Program 和 C++ emitter 的代码框架与分阶段迁移计划见
> [GRHSIM-AM lowering、调度与 C++ emit 流水线](grhsim-am-pipeline.md)。该流水线文档
> 只定义构建阶段和工程边界；最终 Program 的执行语义仍以本文为准。

## 1. 它解决什么问题

GRH 描述 RTL 的结构和行为，但解释器、JIT 和 C++ 代码生成器需要一套共同的运行时
语义。GRHSIM-AM 将 GRH 静态 lower 成后端无关的 Program；除本文明确留给实现选择或
定义为 AM 层未定义行为的部分外，各后端必须保证相同的可观察行为：

```text
SystemVerilog -> GRH -> GRHSIM-AM Program -> Interpreter / JIT / C++
```

抽象机记为：

```text
Machine M = (Program P, State S, HostEnvironment H)
```

- `P` 是静态程序，创建后不再改变；
- `S` 是运行状态，`eval(M)` 更新 `S` 并可通过 `H` 产生外部效果；
- `H` 提供时间、I/O、随机初始化、system call 和 DPI binding 等宿主服务。

H 不属于 Program 或 VariableArea；它可以产生外部可观察效果并向调用返回值。所有后端
必须保持 AM 规定的调用顺序和 Type 语义，但不要求使用相同物理接口。参考能力和生命周期
见 [HostEnvironment 文档](grhsim-host-environment.md)。

调用方与 Machine 的基本交互是：

```text
写入输入状态 -> eval(M) -> 读取输出状态
```

`eval()` 不自动翻转时钟，也不推进仿真时间。调用方需要自行修改时钟值，并在每个需要
求值的电平调用 `eval()`。

## 2. Program：变量、Block 和 DPI 声明

一个 Program 包含 Variable、Block 和可选的 DPI import 声明：

```text
Program = (Variables, Blocks, DpiImports)
Variable = (VarId, Type, Label, Init)
Block = (BlockId, Instructions)
```

`DpiImports` 是 Program 构造时确定的外部函数签名表，不属于运行状态；不使用 DPI 时为空。
Program 是单一、扁平的行为程序，不保存 module/instance 层次或 XMR 路径。`kInstance`
必须在构造 Variables 和 Blocks 前展开，XMR 必须预先 resolve；`kBlackbox` 不允许进入
GRHSIM-AM，也不能作为外部模型边界保留。

### 2.1 Variable

Variable 是有固定类型的逻辑存储对象。`VarId` 从 0 开始连续编号，既是变量标识，
也是它在状态区中的逻辑地址。`Label` 仅用于阅读和诊断，不要求唯一，也不参与执行。

当前类型包括：

```text
BV<W, signed | unsigned>
Real
String
Array<N, BV<W, signed | unsigned>>
```

- `BV` 是只含 0 和 1 的固定位宽位向量；GRH Logic 中仍含 X/Z 的值不能 lower 为 BV；
- signedness 不改变存储的 bit，只影响扩展、比较和算术解释；
- `Real` 保存 IEEE-754 binary64 的完整位模式；
- `String` 是有限 byte 序列；
- Array 长度固定，整个 Array 只占一个 VarId。

`sameValue(x, y)` 用来判断两个同类型值的表示是否完全相同：BV 逐 bit 比较，Real
比较完整 64-bit 位模式，String 逐 byte 比较，Array 逐元素比较。因此两个不同编码的
NaN 也不视为相同值。

每个 Variable 创建时按 `Init` 初始化：

- `constant(value)`：标量常量，运行期间不可写；
- `undef`：初始值不受 AM 约束；
- `[]`：以类型零值初始化；
- `set(expr)`：初始化标量；
- `fill(start, count, expr)`：初始化 Array 的一段区间；
- `load(path, hex | bin, range)`：从文件初始化 Array。

初始化动作按顺序执行，后面的动作覆盖前面的动作，并且只在创建 Machine 时执行一次。
`random` 初始化保证产生目标宽度的 BV，具体 PRNG 和跨后端可复现性由实现选择；本文同时
给出可直接采用的 SplitMix64 参考定义。完整规则见 [Variable Init 规范](#9-variable-init-规范)。

### 2.2 Block 和 Instruction

Block 是按文本顺序完整执行的指令序列。Block 内没有隐式分支或提前退出；数据选择由
`mux` 等显式指令表达。

```text
Instruction = (Opcode, Results, Operands, Attributes)
```

Results 和 Operands 引用 Variable；所有 RTL 数据包括 constant 都使用 Variable 表达。
Attributes 是 Program 构造时确定的具名静态配置，不引用运行时 Value，也不存在通用
immediate operand。Variable Init 仍属于 Variable metadata，不是 Instruction
Attribute。

```text
%2 = and %0, %1
```

这里 `%n` 表示 VarId `n`。指令先读取全部 Operands，再写 Results 和 read-write
Operands，因此在类型允许时 Result 可以与 Operand 使用相同 VarId。完整文本语法、
operand 排布、Attribute schema、类型和边界规则见
[GRHSIM-AM 指令集](grhsim-am-instructions.md)。

`BlockId` 同样从 0 开始连续编号，但与 VarId 属于不同命名空间：

- `B0` 是 EntryBlock，每次 `eval()` 开始时执行一次；
- `B1` 及之后依次是 compute Block 连续区间和 commit Block 连续后缀（允许为空）：
  compute Block 只在被激活时执行，commit Block 每个 round 总是执行。布局与指令
  放置约束见第 4.1 节。

### 2.3 DpiImports

`DpiImports` 是有限的 DpiImport 列表：

```text
DpiImport    = (Symbol, Parameters, Return)
Parameters   = [DpiParameter...]
DpiParameter = (Name, Direction, Type, AbiKind)
DpiReturn    = (Type, AbiKind)

Direction = input | output | inout
AbiKind   = integral | real64 | real32 | string
Return    = none | DpiReturn
```

- `Symbol` 是非空 String，在同一 Program 中唯一；AM 不支持按参数类型重载同名 import；
- `Parameters` 的列表顺序就是外部函数的形参顺序，参与调用语义；
- `Name` 是非空 String，在同一个 DpiImport 中唯一，只用于 lowering、诊断和调试，不参与
  运行时参数匹配；
- `Direction` 明确形参的数据流方向；
- `Type` 是第 2.1 节定义的 AM Type；DPI 参数和返回值只允许 BV、Real 或 String，不允许
  Array；
- `AbiKind` 保存仅靠 AM Type 无法恢复的调用边界语义，并必须与 Type 满足下表约束；
- `Return = none` 表示 void 函数，否则给出返回值的 Type 和 AbiKind。Return 没有 Name 或
  Direction。

| AbiKind | 允许的 Type | 调用边界语义 |
| --- | --- | --- |
| `integral` | `BV<W, Sign>`，`W >= 1` | 按完整 W-bit 位模式传递；Sign 决定宿主进行数值转换时的解释。 |
| `real64` | `Real` | 以 IEEE-754 binary64 值传递。 |
| `real32` | `Real` | 调用前舍入为 IEEE-754 binary32；调用返回 AM 时再扩展为 binary64。 |
| `string` | `String` | 以有限 byte string 传递；具体物理表示由 host binding 适配。 |

Program 文本或序列化格式必须完整保存 Parameter 顺序；DpiImport 列表本身没有语义顺序，
规范文本按 Symbol 排序。`dpi.call` 只通过 `import` Attribute 引用 Symbol，不持有参数名称、
方向或 ABI 类型的副本。

创建 Machine 时，每个被 `dpi.call` 引用的 DpiImport 都必须解析到唯一 host binding，且
binding 的参数数量、顺序、Direction、Type、AbiKind 和 Return 完全匹配。未引用的声明
可以不绑定。缺失、重复或签名不匹配均导致 Machine 创建失败；不能在第一次调用时再猜测
签名。BV、Real 和 String 的逻辑传值形式见
[HostValue 定义](grhsim-host-environment.md#1-hostvalue)。

GRH `kDpicImport` 按以下规则 lower。源类型名先转为小写；未列出的类型以及
`realtime/chandle` 当前不能 lower：

| GRH `argsType/returnType` | AM Type | AbiKind | 额外约束 |
| --- | --- | --- | --- |
| `bit/logic` | `BV<width, Sign>` | `integral` | `width >= 1`。 |
| `byte` | `BV<8, Sign>` | `integral` | GRH width 必须为 8。 |
| `shortint` | `BV<16, Sign>` | `integral` | GRH width 必须为 16。 |
| `int/integer` | `BV<32, Sign>` | `integral` | GRH width 必须为 32。 |
| `longint/time` | `BV<64, Sign>` | `integral` | GRH width 必须为 64。 |
| `real` | `Real` | `real64` | width 和 signedness 不参与 AM 声明。 |
| `shortreal` | `Real` | `real32` | width 和 signedness 不参与 AM 声明。 |
| `string` | `String` | `string` | width 和 signedness 不参与 AM 声明。 |

表中的 `width` 和 `Sign` 分别来自 `argsWidth/returnWidth` 与
`argsSigned/returnSigned`。`argsDirection/argsWidth/argsName/argsSigned/argsType` 五个参数
数组必须等长，并逐项组成 DpiParameter；direction 只能是 `input/output/inout`。
`hasReturn = false` 产生 `Return = none`；`hasReturn = true` 时必须存在完整的
`returnType/returnWidth/returnSigned`，并按同一张表产生 DpiReturn。lower 完成后，GRH
属性名和源类型别名不进入 AM Program。

例如，以下声明精确表示
`import "DPI-C" function int add(input int a, input int b);`：

```text
DpiImport(
    Symbol = "add",
    Parameters = [
        (Name = "a", Direction = input, Type = BV<32, signed>, AbiKind = integral),
        (Name = "b", Direction = input, Type = BV<32, signed>, AbiKind = integral)
    ],
    Return = (Type = BV<32, signed>, AbiKind = integral)
)
```

该声明只描述签名，不执行调用，也不占用 Variable 或 Block；实际调用由指令集中的
[`dpi.call`](grhsim-am-instructions.md#15-dpi-call) 表达。

## 3. State：值和调度状态

State 分为三个逻辑区域：

```text
State S
├── VariableArea   每个 Variable 的当前 Value
├── ControlArea    FirstEval、RoundCounter、host 调用内部状态
└── ActiveArea     Active
```

关键字段如下：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `Values[v]` | `Value(Type(v))` | VarId `v` 的当前值 |
| `FirstEval` | `Bool` | 是否尚未进行首次全量求值 |
| `RoundCounter` | `Nat` | 当前 `eval()` 已执行的 round 数，仅用于不收敛诊断 |
| `CallCompleted[c]` | `Bool` | `schedule = once` 的 system function/task 是否已经调用 |
| `PendingHostEvent[c]` | `Bool` | `event_mode = pending` 的 host 指令是否持有已命中、尚未消费的 event |
| `Finalized` | `Bool` | Machine 是否已执行 finalize |
| `Active[b]` | `Bool` | compute Block `b` 是否被激活、等待执行 |

其中每个 `schedule = once` 的 system function/task 都有一个独占的 `CallCompleted`
槽，每个携带 pending event 的 host 指令（`system.function/system.task/dpi.call`）
都有一个独占的 `PendingHostEvent` 槽。`Bool = {false, true}`，`Nat = {0, 1, 2, ...}`。
两者是 Machine 内部类型，不是 Program 可以声明的 Variable Type；`Nat` 使用数学
自然数语义，不发生固定位宽回绕。

`Active` 是单一集合，不分当前/下一 round，也没有分层 summary；commit Block 不占用
激活状态，因为它每个 round 总是执行（见第 4 节）。`changed` 的 `old` 基线和结果
event 都是 VariableArea 中的普通 Variable，其调度生命周期见第 4.3 节。

规范给出了一种连续的逻辑布局，便于统一寻址：

```text
VariableArea          [0, VariableCount)
FirstEval             VariableCount
RoundCounter          VariableCount + 1
CallCompleted         接下来的 OnceCallCount 个槽
PendingHostEvent      接下来的 PendingEventCallCount 个槽
Finalized             再下一个槽
Active                接下来的 BlockCount 个槽（commit Block 的槽恒为 false）
```

这是语义布局，不是物理实现要求。后端可以内联常量、压缩激活位或消除不需要物化的槽，
只要外部可观察行为一致。

## 4. 变化驱动调度

GRHSIM-AM 不在每次求值时无条件运行所有 Block。它以 round 为迭代单位推进：compute
Block 按激活位过滤执行，commit Block 每个 round 总是执行；变化检测只激活受影响的
compute Block，直到一次完整遍历不再产生新的激活为止。

### 4.1 Block 布局与指令放置

BlockId 从 0 开始连续编号，并按类别划分为三个区间：

```text
B0            EntryBlock
B1 .. Bc      compute Block（组合计算、state read、changed、host 指令）
Bc+1 .. Bn    commit Block（state write + 判变 + act.b；允许为空）
```

commit Block 占据连续后缀区间，这是 validator 强制的结构不变量。commit Block 按
normalized event + update guard 聚合分块；同一 event/guard 桶内的写保持静态
priority/effect order。

指令放置约束（validator 强制）：

| 指令 | 允许所在 Block | target 约束 |
| --- | --- | --- |
| `act.f` | 仅 EntryBlock 和 compute Block | 严格更大的 compute BlockId |
| `act.b` | 仅 commit Block | 任意 compute Block |
| state write（`reg.write/latch.write/mem.write/mem.fill`） | 仅 commit Block | — |
| `changed` | 任意 Block | 每条 `changed` 独占 `old`，语义见第 4.2 节 |

`act.f` 不指向 commit Block：commit Block 每个 round 总是执行，不需要激活边。
compute 段内 `act.f` 严格前向，因此单趟升序扫描就能排空当轮全部 compute 活动——
这由 Block 编号规则结构性保证，不需要单独的校验 pass。

### 4.2 `changed` 与 `act`

变化检测指令比较当前值 `new` 和该指令独占的上次观察值 `old`：

```text
%event = changed.any %new, %old
%event = changed.pos %new, %old
%event = changed.neg %new, %old
```

`changed.any` 在完整 Value 不同时产生 1；`changed.pos/changed.neg` 分别检测单 bit
BV 的 `0 -> 1` 和 `1 -> 0`。每条指令产生 `BV<1, unsigned>` event，并在比较后
无条件执行 `old = new`。

Block 激活指令只消费 event：

```text
act.f %event {targets = [TargetBlockId...]}
act.b %event {targets = [TargetBlockId...]}
```

`targets` Attribute 表示静态 BlockId 集合，以下记作 Targets。

- event 为 1 时，`act.f` 将 Targets 置位 `Active`；目标严格前向，在同一趟 compute
  扫描内被消费；
- event 为 1 时，`act.b` 将 Targets 置位 `Active`；目标留给下一 round 执行，并标记
  本轮存在 backward 激活；
- event 为 0 时不产生激活，act 不修改任何 Variable。

act 的 event 必须在同一 Block 中由更早的指令写入，确保 act 每次执行时消费的是本次
Block 执行产生的事件，而不是 VariableArea 中残留的旧值。一个 event 可以供同一 Block
中的多条 act 使用。多个来源重复激活同一 compute Block 仍只记录一个布尔值；结合
`act.f` 的前向约束，每个 compute Block 在一个 round 内最多执行一次。

`act.b` 是唯一的“需要下一 round”信号：一次完整 round 遍历中没有任何 `act.b`
激发（event == 1）即视为收敛。

每条 `changed` 必须独占自己的 `old` Variable。这个 Variable 只保存变化检测历史，
不能被其他指令或调用方使用，并以 `Init = undef` 创建。Machine 不为它建立特殊初始
基线；第一次执行 `changed` 时也按普通规则比较当时的 new 和实现选取的 undef old，
然后执行 `old = new`。因此首次 change/edge 及其触发的状态写属于 AM 层未定义行为，
不同后端无需一致。精确类型和边沿语义见
[GRHSIM-AM 指令集](grhsim-am-instructions.md#11-变化检测)。

### 4.3 changed / event 生命周期

`changed` 指令触碰两块状态，生命周期不同：

- `old` 基线是“检测状态”：执行后立刻 `old = new`，跨 round、跨 `eval()` 一直
  保持，不需要也不允许被清理；
- result（event Variable）是可能被其他 Block 在执行点读取的“通信媒介”，是否需要
  round 末清理按消费者位置分类：

| result 的消费者 | round 末清理 | 理由 |
| --- | --- | --- |
| 全部在同一块内（典型：同块 `act.f`/`act.b`） | 不需要 | 块每次执行时 `changed` 必定先于消费者重写 result；块不执行时消费者也不执行，旧值没有读者 |
| 存在跨块消费者（其他 Block 的 state write event/guard operand、host 指令或组合指令 operand） | 必须清零 | 生产块下一 round 若未被激活就不重新执行，而消费块（尤其常扫描的 commit Block）仍会读；残留的 event=1 会让写每轮重复发生、`act.b` 每轮激发，不动点判断直接失效 |

由此得到规则：

- round 末的清空集合是跨块消费的 `changed` 结果子集：结果为真才进入 dirty-list，
  round 末只清这些；
- commit Block 内的判变 `changed`（喂同块 `act.b`）属于同块消费，且 commit Block
  每个 round 必执行、result 每轮重写，不进入 dirty-list；
- compute 阶段产生的跨块 event 在同一 round 的 commit 阶段可读；下一 round 若未重新
  产生则为 0。每轮末都清理使跨 eval 衔接自然成立：最后一次循环结束时跨块 event 已
  归 0，下次 `eval()` 首轮天然干净；同块消费的 result 是普通 Variable 值，随状态
  保留到下次调用，块再次执行时先重写再消费；
- EntryBlock 的输出 event 属于跨块消费：B0 每次 `eval()` 只执行一次且不会重跑，
  这类 event 必须 round 末清，否则会在本次 `eval()` 的所有 round 保持为 1，导致
  下游写和 host 调用每轮重复触发；
- 跨块 event 只允许前向流：validator 强制跨块消费时生产块的 BlockId 小于消费块，
  保证同一 round 先产后读（B0 为 0 号块，天然满足）；`act` 消费的 event 必须同块
  先行写入。

### 4.4 state write 与 reader 重激活

commit Block 每个 round 总是执行：写指令按文本顺序执行，由块内 guard/event 决定是否
真正写入。操作数的读取分两类：

- **pre-commit（read-old）**：写指令的非 target、非 event 操作数若**直接引用寄存器
  state**（register read port 直通），lowering 会把它替换为一个快照变量，由一条普通
  compute `assign`（snapshot = state）供给。该 assign 在 compute 阶段求值（本轮
  commit 阶段开始前收敛），并在 state 变化时经 `act.b` 随 reader 集合重激活，因此
  commit 读到的恒为本轮 commit 前的值——同轮 commit 段内先写后读不会读到新值。
  这是对 legacy"sink 数据来自 compute 已收敛值"语义的精确对齐（2026-07-28 XS
  difftest 裁决：就地 read-new 会破坏启动路径上的先写后读链）。
- **就地读取**：compute 产生的值、RMW 写的 target 旧值（多写 priority 语义要求）
  和 event 操作数按执行点读取，不做任何跨轮保留。

实际变化检测与写路径融合：写使 visible state 实际变化且存在 reader 时，由同块
`changed` 探测器产生 event，供同块 `act.b` 消费，激活该 state 的 reader compute
Block。

同一 target 被多个 commit Block 写入时，由 commit 段内静态 BlockId 顺序保证
priority/effect order；reader 一律在下一 round 才执行，只看到本轮最终值，因此不
存在轮内瞬态暴露问题。写变又被写回可能导致多余一轮激活，属于允许的多执行。

### 4.5 EntryBlock

EntryBlock 是 `B0`，由 `changed`、用于派生 event 的组合指令和 `act.f` 构成。它在
每次 `eval()` 开始时无条件执行一次，将两次调用之间的外部净变化转成首个 round 的
初始 compute 激活。EntryBlock 不包含 `act.b`、state write 或其他有状态指令。

外部写入本身不会直接激活 Block。例如输入从 0 改成 1、又在调用 `eval()` 前改回 0，
EntryBlock 只看到最终值 0，因此不会观察到中间变化。

### 4.6 首次求值

第一次 `eval()` 会额外激活所有 compute Block（commit Block 本就每轮执行）。这保证
即使初始值没有触发 EntryBlock，整个 Program 仍会完成一次初始求值。`changed` 在首次
求值中正常产生 event，使 backward 依赖可以跨 round 收敛；
`reg.write/mem.write/mem.fill/latch.write` 也只按其显式 cond 和 event 判断是否写入。
由于无显式初值的状态和每条 `changed.old` 使用 `undef`，首次 event、状态写和输出
可能属于 AM 层未定义行为；调用方负责通过 reset/clock 协议建立其需要的有效状态。
首次求值正常返回时才清除 `FirstEval`；此后只执行由变化传播激活的 compute Block。

## 5. `eval()` 流程

一次求值可以概括为：

```text
1. 拷贝输入端口，清空激活位和跨块 changed 结果，RoundCounter = 0
2. 执行 EntryBlock，检测调用间的外部变化
3. 若 FirstEval = true，激活全部 compute Block，并在本次求值正常返回时清除 FirstEval
4. round 循环：compute 阶段按激活位升序执行 compute Block，
   commit 阶段升序执行全部 commit Block；轮末清跨块 changed 结果
5. 本轮没有任何 act.b 激发则收敛返回
```

更接近实现的伪代码如下：

```text
eval(S):
    require Finalized = false
    拷贝 input 端口
    initial = FirstEval
    RoundCounter = 0
    clear Active
    clear 跨块 changed 结果（dirty-list）
    execute B0

    if initial:
        activate 全部 compute Block

    loop:
        backwardFired = false
        // compute 阶段：按 BlockId 升序，active 过滤，边扫边清
        for b = 1 ... c:
            if Active[b]:
                Active[b] = false
                execute Bb          // act.f 置位更大的 compute bit，本趟内被消费
        // commit 阶段：按 BlockId 升序，总是执行
        for b = c + 1 ... n:
            execute Bb              // 块内 guard/event 决定写是否发生；
                                    // 实际写变 -> 同块 changed + act.b 置位 reader bit
                                    // 并置 backwardFired
        clear 跨块 changed 结果      // round 末，见第 4.3 节
        RoundCounter += 1；超过上限则报告不收敛错误
        if not backwardFired: break

    收尾：写回 output 端口
    if initial:
        FirstEval = false
    return
```

要点：

- 拷贝输入、写回输出是集成层契约：C++ emitter 在生成的 `eval()` 入口把
  ProgramInterface 输入端口拷入 VariableArea，返回前写回输出端口；解释器后端由
  调用方在 `eval()` 前直接写入输入状态（见第 1 节），可观察效果相同。
- `act.f` 只允许指向更大的 compute BlockId，在同一趟 compute 扫描内被消费；每个
  compute Block 每轮最多执行一次（bit 幂等，前向约束保证被消费的 bit 本趟不会被
  重新置位）。
- `act.b` 只指向已经扫过的 compute 段，置位的 bit 留给下一 round；它是唯一的“需要
  下一 round”信号。不动点判断就是循环条件：一趟完整遍历下来没有任何 `act.b` 激发
  （event == 1）即收敛，逐项对应 legacy 的
  `pending_eval_round = commit_activated_readers_`。
- 正常返回时 `Active` 全空，跨块 `changed` 结果已在最后一个 round 末清零；Variable
  的值和 `changed` 的 `old` 基线保留到下一次调用。
- 如果每个 round 都继续产生 backward 激活，`eval()` 将不收敛；实现可以设置 round
  上限并报告错误。该上限是实现保护，不属于语义。

## 6. 一个最小例子

下面的 Program 计算 `y = ~x`。为便于阅读，示例用 Variable Label 代替实际的数字
VarId：

```text
B0:                         // EntryBlock
    %x_changed = changed.any %x, %x_seen
    act.f %x_changed {targets = [1]}

B1:
    %n = not %x
    %n_changed = changed.any %n, %n_seen
    act.f %n_changed {targets = [2]}

B2:
    %y = assign %n
```

第一次 `eval()` 会执行 B1、B2，得到初始结果。之后调用方修改 `x` 再调用 `eval()`：

```text
x 变化 -> B0 激活 B1 -> B1 更新 n 并激活 B2 -> B2 更新 y
```

所有边都是 forward，因此整个传播在第一个 round 内完成。如果 B1 需要重新触发一个
已经扫描过的 compute Block，就应使用 `act.b`，由下一 round 继续处理。该例不含
state write，因此没有 commit Block；含状态写的 Program 布局见第 4.1 节。

## 7. Machine 生命周期与外部访问

创建 Machine 时：

1. 验证 Program；
2. 初始化全部 Variable；
3. 清零全部 `changed` 结果；
4. 清零全部 `CallCompleted` 和 `PendingHostEvent`，设置 `Finalized = false`；
5. 设置 `FirstEval = true`、`RoundCounter = 0`；
6. 清空全部激活位。

调用方只能在 Machine 空闲时访问 State，即创建完成后、第一次 `eval()` 前，或两次
`eval()` 之间。调用方可以读取状态，但只能修改满足以下条件的 Variable：

- 写入值与 Variable Type 完全一致；
- Variable 不是 constant；
- Variable 不是 `changed` 的 `old` 基线或结果 event。

ControlArea 和 ActiveArea 对调用方只读。`finalize()` 只能在一次正常返回的 `eval()`
之后、且此后没有外部写入 Variable 时调用。Machine 被 finalize 后不能再次 `eval()`；
重复 `finalize()` 不产生效果。正常销毁前应调用 `finalize()`，按静态顺序执行
`schedule = final` 的 system function/task 并 flush host side effect。

Program 本身不区分 input、output、state 和
temporary；端口到 VarId 的映射属于集成层契约，不能依赖不唯一的 Label。

## 8. Lowering 和合法性要点

从 GRH lower 到 GRHSIM-AM 时，需要保证：

- `kInstance` 已完全展开，`kXMRRead/kXMRWrite` 已 resolve；这些结构 Operation 不产生 AM
  指令，lower 输入中仍有残留时说明预处理不完整；
- `kBlackbox` 无条件禁止；无论是否存在 host binding 或外部模型，lower 输入中出现它都
  必须失败；
- VarId 和 BlockId 各自连续、无空洞，且至少存在 EntryBlock；
- 每个操作数类型满足 opcode 约束，constant 不作为指令目标；
- GRH Logic 映射成同宽、同 signedness 的 BV，含 X/Z 时 lower 失败；
- `kConstant` 直接成为 constant Variable，不生成计算指令；
- `kSliceStatic.sliceStart` lower 为 `slice_static.lsb` Attribute，并用 result 宽度验证
  `sliceEnd`；Block 激活目标 lower 为 `act.f/act.b.targets` Attribute；
- `kRegister` 映射为可写 BV Variable，`kRegisterReadPort` 直接映射到该 VarId，
  `kRegisterWritePort` 映射为 `reg.write`；`kMemory` 映射为固定长度的 BV Array，
  `kMemoryReadPort/kMemoryWritePort/kMemoryFillPort` 分别映射为
  `mem.read/mem.write/mem.fill`；
- `kLatch` 映射为可写 BV Variable，`kLatchReadPort` 直接映射到该 VarId，
  `kLatchWritePort` 映射为 `latch.write`；lower 前建议运行
  [`latch-transparent-read`](../transform/latch-transparent-read.md)，将透明读旁路显式
  改写为组合逻辑；
- EntryBlock 只包含 `changed`、event 派生所需的组合指令和 `act.f`，且目标都是
  compute Block；
- `act.f/act.b` 消费同一 Block 中先行计算的 `BV<1, unsigned>` event；`act.f` 只能
  出现在 EntryBlock 和 compute Block，且目标必须是更大的 compute BlockId；`act.b`
  只能出现在 commit Block，目标是 compute Block（BlockId 任意）；
- 每条 `changed` 的 `old` 独占且使用空初始化；`changed.any` 的 `new/old` 类型相同，
  `changed.pos/changed.neg` 的 `new/old` 是类型相同的单 bit BV；跨块消费的
  `changed` 结果只允许前向流，生产块 BlockId 必须小于消费块；
- commit Block 占据 BlockId 连续后缀；`reg.write/mem.write/mem.fill/latch.write`
  只能出现在 commit Block，读取执行点可见的显式 operand；状态写回后以同块
  `changed.any` 检测 target 的实际变化，经同块 `act.b` 激活读取者 compute Block；
  同一 target 的多写由 commit 段静态 BlockId 顺序保证 priority/effect order；
- `kSystemFunction/kSystemTask` 分别映射为 `system.function/system.task`；
  `hasSideEffects` 规范化为 `system.function.has_side_effects`，raw event 先变成
  `changed` event，`procKind/hasTiming` 规范化为显式 schedule 和 Block 激活；
- `kDpicImport` 按第 2.3 节映射为 Program 级 DpiImport；声明 symbol 和 parameter name
  满足唯一性，参数属性数组完整且等长，Type 与 AbiKind 匹配；`kDpicCall` 映射为
  `dpi.call`，参数按 import 声明方向和顺序规范化，raw event 先完成边沿检测，有序
  external call group 保持为同一顺序域。

普通组合 Operation 的精确映射、位宽转换和边界行为见
[GRHSIM-AM 指令集](grhsim-am-instructions.md)。`undef` 的 AM 层未定义行为和 random
初始化的实现约束分别见第 9.1、9.4 节。

## 9. Variable Init 规范

Init 是 Program 的静态组成部分，只在创建 Machine 时执行一次。后续 `eval()` 不会
重新初始化 Variable，也不会重新读取初始化文件。

### 9.1 文法

```text
Init       = constant(ScalarValue)
           | undef
           | [InitAction...]

InitAction = set(InitExpr)
           | fill(Start, Count, InitExpr)
           | load(Path, Format, Range)

InitExpr   = literal(ScalarValue)
           | random
           | random(SeedValue)

ScalarType = BV<W, Sign> | Real | String
ScalarValue: Value(ScalarType)
SeedValue  : Value(BV<64, unsigned>)
Format     = hex | bin
Range      = all | from(Start) | span(Start, Count)
```

`Start` 和 `Count` 是数学自然数；出现 `Count` 时必须满足 `Count > 0`，相关计算不发生
宿主整数回绕。`Path` 是 Program 元数据中的非空路径字符串，不是 AM 的 String Value。

`undef` 可用于任意 Type。创建 Machine 时，后端可为它选择任意类型合法的 Value；选择
方法、重复运行是否相同以及不同后端是否一致均不受 AM 约束。任何在该 Variable 被确定
写入前依赖其值的可观察行为都是 AM 层未定义行为。实现仍必须把它物化为类型合法的值，
不得通过读取未初始化宿主内存、越界访问等宿主语言未定义行为实现 `undef`。

### 9.2 零值

动作序列执行前，Variable 先取对应 Type 的零值：

| Type | `zero(Type)` |
| --- | --- |
| `BV<W, Sign>` | W 个 0 bit |
| `Real` | IEEE-754 binary64 正零的位模式 |
| `String` | 空 byte 序列 |
| `Array<N, BV<W, Sign>>` | N 个零值元素 |

### 9.3 初始化形式

| 形式 | 合法目标 | 语义 |
| --- | --- | --- |
| `constant(value)` | ScalarType | 直接使用 value，并使 Variable 在整个生命周期内不可写 |
| `undef` | 任意 Type | 后端任选类型合法初值；依赖该初值的行为不受 AM 约束 |
| `[]` | 任意 Type | 保持 `zero(Type)`，Variable 仍可写 |
| `set(expr)` | ScalarType | 设置整个标量 |
| `fill(start, count, expr)` | Array | 按索引递增设置 `[start, start + count)` |
| `load(path, format, range)` | Array | 从文本文件依次读取 Array 元素 |

`constant(value)`、`undef` 和动作序列是互斥的三种 Init。动作从左到右执行，后面的动作
覆盖前面的动作；空序列 `[]` 与显式设置零值在 Program 中仍是不同表示。

`constant` 和 `literal` 中的 Value 必须与目标 ScalarType 或 Array 元素 Type 完全
一致。标量动作序列最多包含一个 `set`；Array 动作序列不能包含 `set`。`fill` 必须满足
`start + count <= N`，并对区间内的每个元素分别求值一次 `expr`。

### 9.4 Random

`random` 和 `random(seed)` 只能用于 `BV<W, Sign>`，且 `W >= 1`；seed 必须是
`BV<64, unsigned>`。每次求值必须产生恰好 W bit 的类型合法值，Signedness 不参与随机
bit 的生成。`fill` 中每个 Array 元素分别求值一次，因此每个元素构成一次独立的 random
请求，而不是生成一个值后广播。

AM 不强制 PRNG 算法、无显式 seed 时的熵来源、各请求的消费顺序或不同后端之间逐 bit
一致。实现可以使用确定性或非确定性算法，但应记录其 seed 设置和可复现性约定；显式
`random(seed)` 的 SeedValue 必须原样传给所选算法，不能静默忽略。后端无法提供 random
服务时，包含 random InitExpr 的 Machine 创建失败。该规则只适用于 InitExpr，
不影响运行期随机 SystemFunction。

以下是推荐的确定性参考实现。Machine 从 HostEnvironment 取得一个 64-bit
`MachineRandomSeed`，并为 `random` 建立一个 state；每个语法上的 `random(seed)` InitExpr
出现位置建立一个独立 state，并以其显式 seed 初始化。参考初始化顺序为 VarId 升序、
InitAction 文本顺序、Array element index 升序；每次求值从对应 state 连续取 word：

```text
splitmix64_next(state):
    state = state + 0x9E3779B97F4A7C15 mod 2^64
    z = state
    z = (z xor (z >> 30)) * 0xBF58476D1CE4E5B9 mod 2^64
    z = (z xor (z >> 27)) * 0x94D049BB133111EB mod 2^64
    return z xor (z >> 31)
```

为一个 W-bit 结果调用 `ceil(W / 64)` 次：第 0 个 word 填充最低 64 bit，后续 word
依次填充更高位，最后将最高 word 中超出 W 的 bit 清零。例如 W = 100 时，第一个 word
成为 `[63:0]`，第二个 word 的低 36 bit 成为 `[99:64]`。这一算法、stream 划分和遍历
顺序都是参考约定而非 AM 可观察一致性要求；实现采用其他方案时仍须满足本节第一、二段
的类型、宽度、逐元素求值和显式 seed 传递规则。

### 9.5 文件加载

Machine 创建时由 HostEnvironment 提供绝对目录 `FileRoot`。绝对 Path 直接使用；相对 Path 与
`FileRoot` 拼接后对 `.` 和 `..` 做词法归一化。路径不展开 `~`、环境变量或 shell
表达式，`..` 可以越出 `FileRoot`。

Range 决定允许写入的数学地址区间：

```text
all            -> [0, +inf)
from(start)    -> [start, +inf)
span(start, n) -> [start, start + n)
```

实际写入范围还要与 Array 的 `[0, N)` 取交集。cursor 在 `all` 时从 0 开始，在
`from` 和 `span` 时从 start 开始。每读取一个数据 token，尝试写入 cursor 后将其加一；
`@address` 将 cursor 设置为指定的绝对十六进制元素地址。即使 cursor 位于有效范围外，
读取数据 token 后仍会递增。

文件词法规则如下：

- 忽略空白、`//` 行注释和 `/* ... */` 块注释；
- 数值 token 中的 `_` 被忽略；
- hex 数据只能包含十六进制数字，bin 数据只能包含 `0` 或 `1`；
- address token 是 `@` 加非空十六进制地址；
- X/Z token 非法；空文件合法。

数据短于元素位宽时高位补零，长于元素位宽时只保留最低位；Signedness 不影响补位和
截断。路径解析、文件访问、注释或 token 解析任一步失败，Machine 创建失败，不产生
可用的部分初始化 State。
