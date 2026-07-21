# GRHSIM 抽象机

> GRHSIM-AM（Abstract Machine）是 GRH 与具体仿真后端之间的统一执行模型。
> 本文只介绍核心概念和执行流程；具体 opcode 见
> [GRHSIM-AM 指令集](grhsim-am-instructions.md)。

## 1. 它解决什么问题

GRH 描述 RTL 的结构和行为，但解释器、JIT 和 C++ 代码生成器需要一套共同的运行时
语义。GRHSIM-AM 将 GRH 静态 lower 成后端无关的 Program，各后端只需保证相同的
可观察行为：

```text
SystemVerilog -> GRH -> GRHSIM-AM Program -> Interpreter / JIT / C++
```

抽象机记为：

```text
Machine M = (Program P, State S)
```

- `P` 是静态程序，创建后不再改变；
- `S` 是运行状态，`eval(M)` 只更新 `S`。

调用方与 Machine 的基本交互是：

```text
写入输入状态 -> eval(M) -> 读取输出状态
```

`eval()` 不自动翻转时钟，也不推进仿真时间。调用方需要自行修改时钟值，并在每个需要
求值的电平调用 `eval()`。

## 2. Program：变量和 Block

一个 Program 只有两部分：

```text
Program = (Variables, Blocks)
Variable = (VarId, Type, Label, Init)
Block = (BlockId, Instructions)
```

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
- `[]`：以类型零值初始化；
- `set(expr)`：初始化标量；
- `fill(start, count, expr)`：初始化 Array 的一段区间；
- `load(path, hex | bin, range)`：从文件初始化 Array。

初始化动作按顺序执行，后面的动作覆盖前面的动作，并且只在创建 Machine 时执行一次。
`random` 初始化目前保留语法但尚未定义确定语义。完整规则见
[Variable Init 规范](#9-variable-init-规范)。

### 2.2 Block 和 Instruction

Block 是按文本顺序完整执行的指令序列。Block 内没有隐式分支或提前退出；数据选择由
`mux` 等显式指令表达。

```text
and %2, %0, %1
```

这里 `%n` 表示 VarId `n`。普通组合指令先读取全部源，再计算并写回，因此在类型允许
时目标可以与源相同。各指令的类型和边界规则见
[GRHSIM-AM 指令集](grhsim-am-instructions.md)。

`BlockId` 同样从 0 开始连续编号，但与 VarId 属于不同命名空间：

- `B0` 是 EntryBlock，每次 `eval()` 开始时执行一次；
- `B1` 及之后是普通 Block，只在被激活时执行。

## 3. State：值和调度状态

State 分为三个逻辑区域：

```text
State S
├── VariableArea   每个 Variable 的当前 Value
├── ControlArea    FirstEval、EpochCounter
└── ActiveArea     Active、NextEpochActive
```

关键字段如下：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `Values[v]` | `Value(Type(v))` | VarId `v` 的当前值 |
| `FirstEval` | `Bool` | 是否尚未进行首次全量求值 |
| `EpochCounter` | `Nat` | 当前 epoch 编号，从 0 开始 |
| `Active[b]` | `Bool` | Block `b` 是否应在当前 epoch 执行 |
| `NextEpochActive[b]` | `Bool` | Block `b` 是否应在下一 epoch 执行 |

其中 `Bool = {false, true}`，`Nat = {0, 1, 2, ...}`。两者是 Machine 内部类型，
不是 Program 可以声明的 Variable Type；`Nat` 使用数学自然数语义，不发生固定位宽回绕。

规范给出了一种连续的逻辑布局，便于统一寻址：

```text
VariableArea          [0, VariableCount)
FirstEval             VariableCount
EpochCounter          VariableCount + 1
Active                接下来的 BlockCount 个槽
NextEpochActive       再接下来的 BlockCount 个槽
```

这是语义布局，不是物理实现要求。后端可以内联常量、压缩激活位或消除不需要物化的槽，
只要外部可观察行为一致。

## 4. 变化驱动调度

GRHSIM-AM 不在每次求值时无条件运行所有 Block。它用变化检测只激活受影响的 Block，
并按 epoch 传播变化。

### 4.1 `actf` 与 `actb`

两种激活指令都比较一个当前值 `new` 和该指令独占的上次观察值 `old`：

```text
actf %new, %old, {Targets...}
actb %new, %old, {Targets...}
```

如果两者不同：

- `actf` 将 Targets 加入当前 epoch 的 `Active`；
- `actb` 将 Targets 加入下一 epoch 的 `NextEpochActive`。

比较后总会执行 `old = new`。多个来源重复激活同一 Block 仍只记录一个布尔值，因此
同一 Block 在一个 epoch 内最多执行一次。

每条 act 必须独占自己的 `old` Variable。这个 Variable 只保存变化检测基线，不能被
其他指令或调用方使用。Machine 初始化完成后会先执行一次 `old = new`，避免把初始值
误判成外部变化。

### 4.2 Forward 和 Backward

一个 epoch 按 BlockId 从小到大扫描普通 Block。

- `actf` 只能指向更大的 BlockId，因此目标尚未被扫描，可以在当前 epoch 执行；
- `actb` 总是把目标放入下一 epoch，目标可以在源 Block 的前面、后面或就是它自己。

这里的 backward 表示“跨到下一 epoch”，不要求 BlockId 数值向后。

```text
epoch k:      B1 ----actf----> B3
                              |
                             actb
                              v
epoch k + 1: B2
```

### 4.3 EntryBlock

EntryBlock 是 `B0`，只包含 `actf`。它在每次 `eval()` 开始时无条件执行，将两次调用
之间的外部净变化转成 epoch 0 的初始激活。

外部写入本身不会直接激活 Block。例如输入从 0 改成 1、又在调用 `eval()` 前改回 0，
EntryBlock 只看到最终值 0，因此不会观察到中间变化。

### 4.4 首次求值

第一次 `eval()` 会额外激活所有普通 Block。这保证即使初始值没有触发 EntryBlock，
整个 Program 仍会完成一次初始求值。此后只执行由变化传播激活的 Block。

## 5. `eval()` 流程

一次求值可以概括为：

```text
1. 清空激活状态，EpochCounter = 0
2. 执行 EntryBlock，检测调用间的外部变化
3. 若 FirstEval = true，激活全部普通 Block，并清除 FirstEval
4. 按 BlockId 升序执行当前 epoch 的激活 Block
5. 若 NextEpochActive 非空，将其移入 Active，EpochCounter += 1，回到步骤 4
6. 没有待执行 Block 时返回
```

更接近实现的伪代码如下：

```text
eval(S):
    EpochCounter = 0
    clear Active and NextEpochActive
    execute B0

    if FirstEval:
        activate B1 ... Bn
        FirstEval = false

    while Active is not empty:
        for b = 1 ... n:
            if Active[b]:
                Active[b] = false
                execute Bb

        if NextEpochActive is empty:
            return

        Active = NextEpochActive
        clear NextEpochActive
        EpochCounter += 1
```

正常返回时两组激活位都为空，Variable 的值和 act 基线则保留到下一次调用。如果每个
epoch 都继续产生下一 epoch 的激活，`eval()` 将不收敛；实现可以设置 epoch 上限并
报告错误。

## 6. 一个最小例子

下面的 Program 计算 `y = ~x`。为便于阅读，示例用 Variable Label 代替实际的数字
VarId：

```text
B0:                         // EntryBlock
    actf %x, %x_seen, {1}

B1:
    not  %n, %x
    actf %n, %n_seen, {2}

B2:
    assign %y, %n
```

第一次 `eval()` 会执行 B1、B2，得到初始结果。之后调用方修改 `x` 再调用 `eval()`：

```text
x 变化 -> B0 激活 B1 -> B1 更新 n 并激活 B2 -> B2 更新 y
```

所有边都是 forward，因此整个传播在 epoch 0 内完成。如果 B1 需要重新触发一个已经
扫描过的 Block，就应使用 `actb`，由下一 epoch 继续处理。

## 7. Machine 生命周期与外部访问

创建 Machine 时：

1. 验证 Program；
2. 初始化全部 Variable；
3. 为每条 act 建立 `old = new` 基线；
4. 设置 `FirstEval = true`、`EpochCounter = 0`；
5. 清空全部激活位。

调用方只能在 Machine 空闲时访问 State，即创建完成后、第一次 `eval()` 前，或两次
`eval()` 之间。调用方可以读取状态，但只能修改满足以下条件的 Variable：

- 写入值与 Variable Type 完全一致；
- Variable 不是 constant；
- Variable 不是 act 的 `old` 基线。

ControlArea 和 ActiveArea 对调用方只读。Program 本身不区分 input、output、state 和
temporary；端口到 VarId 的映射属于集成层契约，不能依赖不唯一的 Label。

## 8. Lowering 和合法性要点

从 GRH lower 到 GRHSIM-AM 时，需要保证：

- VarId 和 BlockId 各自连续、无空洞，且至少存在 EntryBlock；
- 每个操作数类型满足 opcode 约束，constant 不作为指令目标；
- GRH Logic 映射成同宽、同 signedness 的 BV，含 X/Z 时 lower 失败；
- `kConstant` 直接成为 constant Variable，不生成计算指令；
- `kRegister` 映射为 BV，`kMemory` 映射为固定长度的 BV Array；
- EntryBlock 只包含 `actf`，且目标都是普通 Block；
- `actf` 的目标 BlockId 大于源 BlockId，`actb` 不得指向 EntryBlock；
- 每条 act 的 `new` 和 `old` 类型相同，`old` 独占且使用空初始化。

普通组合 Operation 的精确映射、位宽转换和边界行为见
[GRHSIM-AM 指令集](grhsim-am-instructions.md)。状态单元、层次引用、系统调用、DPI、
边沿检测以及确定性的 random 初始化仍待后续定义。

## 9. Variable Init 规范

Init 是 Program 的静态组成部分，只在创建 Machine 时执行一次。后续 `eval()` 不会
重新初始化 Variable，也不会重新读取初始化文件。

### 9.1 文法

```text
Init       = constant(ScalarValue)
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
| `[]` | 任意 Type | 保持 `zero(Type)`，Variable 仍可写 |
| `set(expr)` | ScalarType | 设置整个标量 |
| `fill(start, count, expr)` | Array | 按索引递增设置 `[start, start + count)` |
| `load(path, format, range)` | Array | 从文本文件依次读取 Array 元素 |

`constant(value)` 和动作序列是互斥的两种 Init。动作从左到右执行，后面的动作覆盖前面
的动作；空序列 `[]` 与显式设置零值在 Program 中仍是不同表示。

`constant` 和 `literal` 中的 Value 必须与目标 ScalarType 或 Array 元素 Type 完全
一致。标量动作序列最多包含一个 `set`；Array 动作序列不能包含 `set`。`fill` 必须满足
`start + count <= N`，并对区间内的每个元素分别求值一次 `expr`。

### 9.4 Random

`random` 和 `random(seed)` 只能用于 BV；seed 必须是 `BV<64, unsigned>`。当前版本尚未
定义 PRNG、seed 作用和取位规则，因此任何实际求值到 random 的初始化均为未定义行为；
实现不需要产生诊断。该规则仅适用于 InitExpr，不影响运行期随机系统函数。

### 9.5 文件加载

Machine 创建时由运行环境提供绝对目录 `FileRoot`。绝对 Path 直接使用；相对 Path 与
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
