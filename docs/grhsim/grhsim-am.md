# GRHSIM Abstract Machine 基础设计

> 状态：讨论稿 v0。本文只定义 GRHSIM-AM 的最小执行语义。

## 1. 定位

GRHSIM-AM 执行由 GRH 静态 lower 得到的仿真程序，位于 GRH 与解释器、JIT、C++ 等
后端之间。它不解释 SystemVerilog，不动态创建 Variable，也不隐式推进仿真时间。

一次 Program 执行对应一次 `eval()`：先执行 EntryBlock，再执行零个或多个 epoch，直到
没有新的反向激活。

## 2. 核心对象

```text
Program  = (Variables, Blocks)
Variable = (VarId, Type, Label, Init)
Block    = (BlockId, Instructions)
```

- `Variables` 和 `Blocks` 都是有限集合；
- Program 不存储独立的依赖对象；依赖由 act 的 Targets 完整表达；
- `M` 是 AM 的全部可变实例状态，不属于 Program，定义见 3.3 节。

本文将 `actf`、`actb` 统称为 act；其完整指令语义见
[GRHSIM-AM 指令集：Block 激活](grhsim-am-instructions.md#15-block-激活)。本文只规定
act 与 Block 依赖及执行调度的关系。

## 3. Variable 与理想存储

### 3.1 Variable

- `VarId`：Variable 在 M 中的逻辑地址。令 `VariableCount = |Variables|`，Program 的
  VarId 集合必须恰为 `[0, VariableCount)`；
- `Type`：编译期确定的值类型；
- `Label`：字符串，仅用于诊断和可读输出，不要求唯一，不参与执行语义；
- `Init`：Variable 的常量值或有序初始化动作序列。

GRHSIM-AM 的 Type 和运行时 Value 定义为：

```text
Signedness = unsigned | signed
ScalarType = BV<W, S> | Real | String
Type       = ScalarType | Array<N, BV<W, S>>

Value(BV<W, S>)           = 恰好 W 个二态 bit
Value(Real)               = IEEE-754 binary64 的 64-bit 位模式
Value(String)             = 有限 8-bit byte 序列
Value(Array<N, BV<W, S>>) = N 个 Value(BV<W, S>) 的有序序列
```

其中 `W > 0`、`N > 0`。GRHSIM-AM 的 `BV` 固定为二态；GRH Logic 中含 X/Z 的值
不能 lower。
Signedness 只属于 `BV`，不改变 W 个存储 bit；它决定 literal 赋值、宽度扩展以及类型
驱动指令的有符号或无符号解释。`Real` 保留包括正负零、无穷和 NaN payload 在内的
完整位模式；`String` 不规定文本编码。

各 Type 的零值依次为全 0 bit、binary64 正零、空 byte 序列和逐元素零。两个同 Type
Value 的 `sameValue` 定义为：`BV` 逐 bit 相同，`Real` 的 64-bit 位模式相同，`String`
长度和各 byte 相同，`Array` 逐元素相同。不同 Type 的 Value 不可比较。

Program 的 Variable 集合及每个 Array 的长度均为静态；`String` payload 的长度是
Value 的一部分，不会创建新的 Variable 或 VarId。

### 3.2 Init

`Init` 定义为：

```text
Init       = constant(ScalarValue)
           | [InitAction...]
InitAction = set(InitExpr)
           | fill(Start, Count, InitExpr)
           | load(Path, Format, Range)
InitExpr   = literal(ScalarValue)
           | random
           | random(SeedValue)
SeedValue  : Value(BV<64, unsigned>)
Path       = string
Format     = hex | bin
Range      = all | from(Start) | span(Start, Count)
```

`Start`、`Count`、load cursor 和 `@address` 均为数学非负整数，运算不发生宿主整数
回绕；使用 `Count` 时要求 `Count > 0`。

`ScalarValue` 是任意 `ScalarType` 的 Value。typed value 已在 lower 时解析为与目标
Type 完全匹配的值；`random` 和 `random(SeedValue)` 只可用于目标 `BV`。

- `constant(value)`：只用于 ScalarType；创建实例时确定值，在 epoch 0 前即可读取，
  此后跨所有 epoch 和 `eval()` 保持不变；
- `set`：设置整个 ScalarType Variable；
- `fill`：按索引递增设置 `Array` 的 `[Start, Start + Count)`，每个元素依次求值
  `InitExpr`；
- `load`：在创建 AM 实例时从文件系统读取文本，并按 `Format` 和 `Range` 初始化
  `Array`。

创建 AM 实例时，`constant(value)` Variable 直接取 value；其他 Variable 先置为其 Type
的零值，再按 Init 序列顺序执行动作，后动作覆盖先前写入。`Init = []` 表示 GRH 没有
指定初始化；其运行时初值仍为零，但它与显式零初始化在 IR 中保持可区分。
同一个 value 放入 `constant(value)` 时不可写，放入 `[set(literal(value))]` 时仍可写。

所有 Init 完成后，AM 为每条 act 执行一次基线初始化 `old = new`。act 的 old 必须使用
`Init = []`；该基线初始化不是 InitAction，并覆盖其 Type 零值。

实例初始化上下文为：

```text
InitContext = (FileRoot: Path)
```

`FileRoot` 是运行期指定的绝对目录。GRH `initFile` 原样成为 `Path`；绝对 `Path`
直接使用，相对 `Path` 与 `FileRoot` 拼接后做 `.`、`..` 词法归一化。`FileRoot` 只是
基准目录，`..` 可以越出它；不展开 `~`、环境变量或 shell 表达式。`load` 在每个 AM
实例创建时执行一次，因此实例创建前对文件的修改可见，但文件不会在后续 `eval()`
中重读。

`random` 和 `random(SeedValue)` 保留 GRH 的两种随机初始化形式；GRH `$random(seed)`
的 seed 按二进制补码取最低 64 bit，映射为 Type 为 `BV<64, unsigned>` 的 SeedValue。
当前版本不定义 PRNG、seed 作用或取位规则；执行任何求值到这两种 `InitExpr` 的 Init
动作均为 UB，AM 不约束实例状态及其后续可观察行为，也不要求实现诊断。该 UB 只
适用于 InitExpr；运行期 SystemFunction 随机调用的语义另行定义。

每个 Variable 只在创建 AM 实例时应用一次 Init，之后其值保留在 M 中。Program 不区分
input、output、state 或 temporary。调用方只应在实例创建完成后且 `eval()` 尚未开始，
或一次 `eval()` 返回后访问 M；AM 不为访问时机提供防护或检测，在实例创建或 `eval()`
执行期间访问不属于合法用法。这里的外部访问不限制 SystemFunction、SystemTask 或
DPI 指令自身定义的外部效果。

AM 不为 Variable 增加 input 标记。哪些外部变化应启动哪些 Block，由 EntryBlock 中的
`actf` 显式表达；EntryBlock 比较的是两次 `eval()` 采样之间的净变化，中间发生但在
本次调用前恢复原值的外部写不可见。外部写本身不隐式激活 Block；未被 EntryBlock
观察的写入不会设置初始 `Active` 控制槽。

#### GRH 到 Init 的规范化

| GRH | GRHSIM-AM Init |
| --- | --- |
| `kConstant.constValue` | 结果 Value 对应一个 `Init = constant(value)` 的 Variable；不生成指令、Block 或 Block 激活。 |
| `kRegister.initValue` | `[set(InitExpr)]`；普通 literal、`$random`、`$random(seed)` 分别映射为三种 `InitExpr`。 |
| 无初始化属性 | 空序列 `[]`。 |
| `kMemory` 的 `literal` | 一个 `fill`；缺省 `initValue` 按 literal 0。 |
| `kMemory` 的 `readmemh/readmemb` | 分别映射为 `load(Path, hex, Range)` 和 `load(Path, bin, Range)`。 |

GRH lowering 还遵循以下规则：

- `kConstant.constValue` 必须解析为与 result Variable ScalarType 完全匹配的 value；
  Logic constant 映射为 `BV`，含 X/Z 时 lower 失败，Real 和 String constant 分别
  映射为对应的 ScalarValue；
- `kRegister(width=W, isSigned=B)` 映射为 `BV<W, S>`，其中 B 为 true 时 S 为
  signed，否则为 unsigned；`kMemory(row=N, width=W, isSigned=B)` 同理映射为
  `Array<N, BV<W, S>>`，memory row `i` 对应 array index `i`；
- 普通 GRH Logic literal 按 GRH 的赋值规则消解为与目标 `BV<W, S>` 完全匹配的值；
  含 X/Z 的结果必须在 lower 时报告错误；
- `literal`：`start < 0` 表示全数组；否则必须 `start >= 0 && len > 0`，表示
  `[start, start + len)`；
- `readmemh/readmemb`：`start < 0` 表示省略范围，`start >= 0 && len <= 0` 表示从
  `start` 到数组末尾，`start >= 0 && len > 0` 表示 `[start, start + len)`；
- 上述三种情况分别映射为 `all`、`from(start)`、`span(start, len)`；
- `all`、`from(s)`、`span(s, n)` 分别选择地址集合 `[0, +inf)`、`[s, +inf)`、
  `[s, s + n)`；实际写入地址还须与 Array 的 `[0, N)` 取交集；
- `load` cursor 在 `all` 时从 0 开始，在 `from` 或 `span` 时从 `Start` 开始；数据
  token 写入 cursor 后令其加一，`@address` 将 cursor 设为绝对十六进制行号；
- `_` 在数值中被忽略；去掉 `_` 后，hex 数据 token 必须是非空 `[0-9a-fA-F]+`，
  bin 数据 token 必须是非空 `[01]+`，address 必须是 `@` 后跟非空十六进制数；
  忽略空白、`//` 行注释和 `/*...*/` 块注释；
- 数据 token 少于 W bit 时高位补零，多于 W bit 时只保留最低 W bit；Signedness
  不影响该补位规则，结果类型为目标 `BV<W, S>`。含 X/Z 的 token 非法。
  cursor 位于选定 Range 或数组边界外时不写入，但仍递增；
- 空文件合法且不写入任何值；
- literal 区间与数组 `[0, N)` 取交集后 lower；空区间不产生动作；
- Path 解析、文件打开或读取、注释或 token 解析任一步失败，都使 AM 实例创建失败，
  不产生可用的部分初始化实例。多个 Init 动作的顺序及重叠覆盖关系必须保留。

例如 `Array<4, BV<8, unsigned>>` 的 `mem.hex` 内容为 `0A 0B @3 FF`，先执行全范围
`readmemh`，再用 literal 初始化第 0 行，得到：

```text
[load("mem.hex", hex, all), fill(0, 1, literal(8'h7E))]
```

最后一个动作覆盖第 0 行。`kMemoryFillPort` 是运行期写入，不属于 Init。

### 3.3 分区理想存储

M 是从 MemId 到 MemValue 的有限映射，是 AM 的理想存储和全部可变实例状态。Variable
位于前缀，所有非 Variable 控制槽位于其后的连续后缀：

```text
Bool         = {false, true}
Nat          = {0, 1, ...}
MemId        = Nat
ControlValue = Bool | Nat
MemValue     = ControlValue | Value(Type)
M            = finite_map<MemId, MemValue>

VariableCount = |Program.Variables|
BlockCount    = |Program.Blocks|

FirstEvalId         = VariableCount
EpochId             = VariableCount + 1
ActiveBias          = VariableCount + 2
NextEpochActiveBias = ActiveBias + BlockCount
MemorySize          = NextEpochActiveBias + BlockCount

VariableRegion = [0, VariableCount):
  M[VarId] = Value(Type(VarId))

ControlRegion = [VariableCount, MemorySize):
  M[FirstEvalId]                  : Bool
  M[EpochId]                      : Nat
  M[ActiveBias + b]               : Bool, 0 <= b < BlockCount
  M[NextEpochActiveBias + b]      : Bool, 0 <= b < BlockCount

dom(M) = [0, MemorySize)
```

下文用 `Active(b)` 和 `NextEpochActive(b)` 分别作为 `M[ActiveBias + b]` 和
`M[NextEpochActiveBias + b]` 的可读写别名。它们不是独立于 M 的集合或状态。
后端可以将每个 Bool 区压缩为 bitset，只要保持按 BlockId 读写的语义。

`Bool` 和 `Nat` 是不同的带类型值，只用于控制区，不能与 `BV` 混用，也不是 Variable
Type。控制槽是 M 中的 typed cell，但不是 Program 的 Variable 记录。Nat 运算使用数学
整数，不发生回绕。

创建实例时，所有 Variable 按 3.2 节完成 Init 并写入变量区，随后建立 act 的 old
基线；再设置 `M[FirstEvalId] = true`、`M[EpochId] = 0`，并将两个激活区的所有 Bool
置为 false。`M[FirstEvalId]` 表示首次全量激活是否尚未发出；首次全量激活发出后立即
将其清零。

MemId/VarId 与 BlockId 是独立命名空间；BlockId 只通过上述 bias 映射到对应控制槽。
实例创建完成后，调用方与 AM 之间的设计数据和控制状态交互都通过 M 完成；`eval()`
是唯一的非访存执行命令。控制区由 AM 写入，调用方只可读取。变量区的外部写是一次
存储变动，不表示持续驱动，也不是 InitAction；写入 Value 必须与 Variable Type 完全
相同，且目标不能是 constant 或 act 的 old Variable。外部写本身不隐式激活 Block。

外部 `read(MemId)` 可读取 `[0, MemorySize)` 中任一槽；越界读取失败。外部
`write(MemId, x)` 只能写 `[0, VariableCount)` 中类型匹配且可写的 Variable；写控制区、
越界 MemId、constant 或 act 的 old Variable 均失败且不修改 M。

普通指令只能显式引用 `[0, VariableCount)` 中的 VarId。指令的 `read(v)` 和
`write(v, x)` 直接访问变量 `v` 的完整 Value；`write` 要求 `type(x) = Type(v)` 且目标
可写，否则不修改 M。constant Variable 仍保留 VarId；后端可以内联其值而不分配物理
存储。不存在字节寻址、指针运算、地址别名或运行时创建 Variable。

`Array` 通过 `(base VarId, element index)` 访问元素；index 是指令操作数，不参与
VarId 算术。后端可以拆分、合并、缓存或消除物理存储，只要保持 AM 语义。

## 4. 指令与基本块

### 4.1 指令

普通指令采用直接访存的 CISC 形式：

```text
opcode dst..., src... [attributes]
```

例如：

```text
and %2, %0, %1
```

表示读取 VarId 0 和 1，按位与后写入 VarId 2。每个 opcode 必须完整定义操作数数量、
类型约束、结果函数、截断或越界规则，以及对理想存储或外部环境的效果。核心 opcode
中的纯计算结果必须是全函数，不能继承宿主语言的未定义行为。

具体 opcode 见 [GRHSIM-AM 指令集](grhsim-am-instructions.md)。

### 4.2 Block

令 `BlockCount = |Blocks|`。Program 必须满足 `BlockCount >= 1`，BlockId 集合恰为
`[0, BlockCount)`；BlockId 0 固定为 EntryBlock，其余是普通 Block。普通 Block 未被
激活时不执行；一旦被激活，就按文本顺序执行全部 Instructions，不分支、不 trap、
不提前退出。EntryBlock 的特殊执行规则见第 5、6 节。

动态数据选择由 `mux` 等显式 opcode 表达。指令的源和目标操作数可用于派生读写集合，
但这些集合不是 Block 字段。

## 5. BlockId 与依赖

act 的 Targets 是依赖关系的唯一来源。定义：

```text
Forward = {(B.BlockId, t) |
           actf(_, _, Targets) in B.Instructions,
           t in Targets}

Backward = {(B.BlockId, t) |
            actb(_, _, Targets) in B.Instructions,
            t in Targets}
```

每个 Targets 都是静态非空 BlockId 集合。对任意 `(s, t) in Forward`，必须满足
`s < t < BlockCount`；对任意 `(s, t) in Backward`，必须满足
`1 <= t < BlockCount`，不约束 s 与 t 的大小关系，因此允许指向自身。关系使用集合
语义，不同 act 可以派生相同的依赖。

EntryBlock 满足以下约束：

- `BlockId = 0`，Program 中恰好存在一个 EntryBlock；
- Instructions 只包含 `actf`；
- 每次 `eval()` 开始时无条件执行一次，不设置 `Active(0)`，也不在 epoch 中再次执行；
- `Active(0)` 和 `NextEpochActive(0)` 是统一寻址保留的槽，始终为 false；
- EntryBlock 中每个 `actf` 的 new Variable 不是 constant，在 `eval()` 期间只读，不能
  作为任何指令的目标。

因为 Forward 中每条依赖的 target ID 都严格大于 source ID，BlockId 的整数升序就是
Forward 的拓扑序，不需要另存 topo order。`actf` 派生的 Forward 在当前 epoch 生效，
`actb` 派生的 Backward 在下一 epoch 生效。首次全量激活不属于这两个关系。

Backward 逻辑上是跨 epoch 的二分关系：

```text
source(epoch k, BlockId) -> target(epoch k + 1, BlockId)
```

同一普通 BlockId 可以同时作为 source 和 target，因此允许跨 epoch 自反馈。
实现可以缓存 Forward 和 Backward，但缓存不参与 AM 语义。

## 6. 一次 eval 的执行

`Active` 和 `NextEpochActive` 是 M 中的瞬时控制区，不是 `eval()` 的局部集合。每次
`eval()` 开始时清零，正常返回时也全部为 false，因此不会跨调用传递激活。

一次 Program 执行按以下伪代码求不动点；`inout` 表示被调用过程可修改该参数：

```text
procedure executeBlock(B, inout M):
    for I in B.Instructions in text order:
        match I:
            case actf(new, old, Targets):
                newValue <- M[new]
                oldValue <- M[old]
                if not sameValue(newValue, oldValue):
                    for b in Targets:
                        Active(b) <- true
                M[old] <- newValue

            case actb(new, old, Targets):
                newValue <- M[new]
                oldValue <- M[old]
                if not sameValue(newValue, oldValue):
                    for b in Targets:
                        NextEpochActive(b) <- true
                M[old] <- newValue

            case _:
                execute I according to its opcode semantics

procedure eval(inout M):
    M[EpochId] <- 0
    for b in [0, BlockCount):
        Active(b) <- false
        NextEpochActive(b) <- false

    executeBlock(Blocks[0], M)

    if M[FirstEvalId] = true:
        for b in [1, BlockCount):
            Active(b) <- true
        M[FirstEvalId] <- false

    while exists b in [1, BlockCount) where Active(b) = true:
        for b in [1, BlockCount) in ascending order:
            if Active(b) = true:
                Active(b) <- false
                executeBlock(Blocks[b], M)

        if not exists b in [1, BlockCount) where NextEpochActive(b) = true:
            return

        M[EpochId] <- M[EpochId] + 1
        for b in [1, BlockCount):
            Active(b) <- NextEpochActive(b)
            NextEpochActive(b) <- false

    return
```

两个 Bool 区分别是当前 epoch 和下一 epoch 激活集合的特征函数；重复置 true 没有额外
效果，同一普通 Block 在一个 epoch 中最多执行一次。执行 Block 前先清除其 `Active`
槽；Forward 满足 `source < target`，保证当前扫描尚未越过随后置 true 的目标。Backward
激活只设置 `NextEpochActive`，在下一 epoch 生效。

除首次全量激活外，`actf` 和 `actb` 是唯一的激活来源；完整语义见指令集。正常返回时
两个激活区必然全为 false。`M[EpochId]` 是从 0 开始的当前 epoch 编号；若没有执行
epoch，其值仍为 0。

调用方须在一次 `eval()` 开始前完成对 M 的外部访问，并在其返回前不再访问 M。
若每个 epoch barrier 都存在值为 true 的 `NextEpochActive` 槽，Program 不收敛；实现
可以设置 epoch 上限并报告错误，但该上限不改变 AM 语义。

## 7. 合法性检查

加载 Program 时至少验证：

- VarId 集合恰为 `[0, VariableCount)`；`BlockCount >= 1`，BlockId 集合恰为
  `[0, BlockCount)`，其中 BlockId 0 是唯一的 EntryBlock；
- Type 符合 3.1 节文法，其中 W、N 为正整数，Signedness 为 `unsigned` 或 `signed`，
  Label 是字符串；
- Init 必须且只能选择 `constant(value)` 或动作序列之一；
- `constant` 只用于 ScalarType，其 value 与 Variable Type 完全匹配；`BV` value 不含
  X/Z；
- 动作序列中的 `set` 只用于 ScalarType，`fill`、`load` 只用于 `Array`；`literal` 的
  typed value 必须与目标 ScalarType 或 Array 元素 Type 完全匹配；`random` 只用于
  `BV`，`load` 产生 Array 元素 Type；
- ScalarType 的动作序列最多包含一个 `set`，`Array` 的动作序列不包含 `set`；`Path`
  必须是非空字符串，`Format` 必须是 `hex` 或 `bin`；
- `random(SeedValue)` 的 SeedValue Type 必须为 `BV<64, unsigned>`；
- 对 `Array<N, ...>`，`fill` 必须满足 `Start >= 0`、`Count > 0`、
  `Start + Count <= N`；`from(Start)` 只要求 `Start >= 0`，
  `span(Start, Count)` 只要求 `Start >= 0`、`Count > 0`；
- 所有作为 Variable 引用的指令操作数使用 `[0, VariableCount)` 中的合法 VarId，不引用
  控制区，并满足 opcode 的类型约束；constant Variable 不得作为指令目标；
- EntryBlock 只包含 `actf`；
- `actf`、`actb` 的 new/old Variable 是不同 VarId、Type 完全相同，old 不是 constant；
  每个 old 只由一条 act 使用，不出现于任何其他指令，且 `Init = []`；
- 每条 `actf`、`actb` 的 Targets 是静态非空 BlockId 集合，且每个 Target 都位于
  `[1, BlockCount)`；`actf` 的每个 Target 还必须大于所在 Block 的 BlockId；EntryBlock
  中 actf 的 new 不作为任何指令目标且不是 constant。

本文不统一约定 Program 加载、实例创建、合法阶段的外部访问操作或执行失败的承载
方式；“失败”、“非法”和“报告错误”只规定必须被拒绝的条件，具体 API、诊断和恢复
策略由实现决定。违反 3.2 节的外部访问时机属于调用契约之外，AM 不必检测或拒绝。

## 8. 待续定义

- Reg、Mem、Latch、Real/String 运算的具体指令；
- 边沿检测指令及其首次 `eval()` 的基线语义；
- SystemFunction、SystemTask 和 DPI 的具体指令及外部效果；
- random 初始化的确定语义；在此之前按 3.2 节的 UB 处理。
