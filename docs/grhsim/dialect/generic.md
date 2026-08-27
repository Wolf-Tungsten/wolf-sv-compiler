# GRHSIM IR generic 方言

> 状态：新架构文档。本文定义 GRHSIM IR 的 `generic.*` 方言：从 GRH IR 映射
> 得到的后端无关操作集合，是各方言的公共基底（最小集合性质，见
> [GRHSIM IR](../grhsim-ir.md) 第 5 节）。配合
> [GRHSIM 仿真模型](../grhsim-simulation-model.md) 与 GRHSIM IR 文档阅读；
> 引用 GRH IR 操作语义时见 [GRH IR 白皮书](../../grh/grh-ir.md)。

## 1. 定位

`generic.*` 是 GRH IR 映射到 GRHSIM IR 时使用的初始方言。设计目标：

- **与 GRH IR 对齐**：操作尽可能与 GRH IR 一一对应，保留语义信息（寄存器
  与锁存器、单行写与 lanes 写等都保持区分）；映射以更名与搬移为主，只插
  入保持 GRH 上下文定宽、条件真值与写冲突语义所需的 bridge op（3.5 节）；
- **语义完整**：仿真模型要求的状态访问、边缘检测、host 调用都有显式操作
  （第 4～6 节），不依赖任何隐式通道；
- **后端无关**：不涉及值编码与存储布局，这些属于存储层（GRHSIM IR 第 3
  节）与后端方言。

与模型的对应关系（仿真模型第 5 节）：读类 op 一律读本轮快照；写类 op 的
写出汇集到本轮末态 `S'`；带事件的 op 在求值时检测边缘并同步 `S_edge`
（第 5 节）；`generic.host_call` 经 `H` 发起调用。

## 2. 类型系统

generic 方言的类型系统是 GRHSIM IR 两轨类型系统中的语义轨（见
[GRHSIM IR](../grhsim-ir.md) 第 3 节）：所有 op 的语义只定义在本节的类
型上，后端类型只是这些类型的表示特化。

### 2.1 类型文法

```text
Type ::= logic<W, s>          -- 逻辑向量，W ≥ 1
       | array<N, Elem>       -- 数组，N ≥ 1，Elem 必须是逻辑向量
       | real                 -- IEEE-754 binary64
       | string               -- 有限 byte 序列

s ∈ {signed, unsigned}
```

类型相等为结构相等：类别与全部参数相同才相等。

### 2.2 各类型

**`logic<W, s>`**：`W` 位四态逻辑向量（0/1/X/Z）。signedness 不改变存储
的位，只影响扩展、比较与算术的解释；逐操作的四态与位宽规则与 GRH IR
一致。编码（两态/四态、打包方式）属存储层，不改变操作语义；后端选择两
态编码时，要求设计运行中不产生 X/Z，否则行为超出本文定义。

**`array<N, logic<W, s>>`**：`N` 个逻辑向量的数组，下标 `0..N-1`。只作
为存储器状态的类型出现，不作为数据流值；整数组读出时给 packed 视图
`logic<N*W, s>`，位布局 `packed[i*W +: W] = a[i]`（4.6 节）。地址操作数
是无符号解释的逻辑向量；越界读得全 X，越界写忽略（SV 惯例）。

**`real`**：IEEE-754 binary64。仅用于 host 调用的参数与返回值。

**`string`**：有限 byte 序列。仅用于 host 调用的参数与返回值。

### 2.3 类型的使用位置

| 位置 | 允许的类型 |
| --- | --- |
| 数据流边（op 操作数/结果） | `logic` / `real` / `string` |
| `S_in`、`S_out`（端口） | `logic`（`real`/`string` 端口暂不支持，需要时由后续修订补充） |
| `S_state`（状态单元） | `logic`（寄存器、锁存器）/ `array`（存储器） |

### 2.4 值相等

eval 的不动点判定（`S' == S`，仿真模型 5.4 节）与边缘检测以值相等为基
础，按类型定义：

- `logic`：逐位四态精确比较（同 `generic.case_eq`）；
- `array`：逐元素相等；
- `real`：按完整 64-bit 位模式比较（不同位模式的 NaN 不相等）；
- `string`：逐 byte 比较。

### 2.5 常量

`generic.const` 可产生 `logic`、`real` 或 `string`，但不能产生 `array`。
`value` 属性保存 GRH IR `kConstant.constValue` 的文本表示；logic 常量使用
Verilog 字面量（如 `"8'hEF"`、`"16'sd-5"`）并支持 `x`/`z`。当前 real 与
string 值只允许作为 host 调用参数，普通 generic 计算操作仍只接受 logic。

## 3. 与 GRH IR 的操作映射

### 3.1 逐操作直译（语义不变）

以下操作与 GRH IR 同名操作一一对应，语义（含四态与位宽规则）完全相同，
本文不重复定义。lowering 为满足 generic 的显式类型契约可能插入 3.5 节所
述 bridge op，因此最终数据流不保证逐边原样复制：

| GRH IR | generic | 说明 |
| --- | --- | --- |
| `kConstant` | `generic.const` | 常量 |
| `kAdd/kSub/kMul/kDiv/kMod` | `generic.add/sub/mul/div/mod` | 算术 |
| `kAnd/kOr/kXor/kXnor` | `generic.and/or/xor/xnor` | 二元位运算 |
| `kNot` | `generic.not` | 一元位运算 |
| `kLt/kLe/kGt/kGe` | `generic.lt/le/gt/ge` | 关系比较 |
| `kEq/kNe` | `generic.eq/ne` | 逻辑相等比较 |
| `kCaseEq/kCaseNe` | `generic.case_eq/case_ne` | 精确比较 |
| `kWildcardEq/kWildcardNe` | `generic.wild_eq/wild_ne` | 通配比较 |
| `kLogicAnd/kLogicOr/kLogicNot` | `generic.logic_and/logic_or/logic_not` | 逻辑运算 |
| `kReduceAnd/kReduceNand/kReduceOr/kReduceNor/kReduceXor/kReduceXnor` | `generic.reduce_and/reduce_nand/reduce_or/reduce_nor/reduce_xor/reduce_xnor` | 规约 |
| `kShl/kLShr/kAShr` | `generic.shl/lshr/ashr` | 移位 |
| `kMux` | `generic.mux` | 数据选择 |
| `kSliceStatic` | `generic.slice_static` | 静态切片 |
| `kSliceDynamic` | `generic.slice_dynamic` | 动态切片 |
| `kSliceArray` | `generic.slice_array` | 数组元素切片 |
| `kAssign` | `generic.assign` | 赋值 |
| `kConcat` | `generic.concat` | 位拼接 |
| `kReplicate` | `generic.replicate` | 位复制 |
| `kArrayLaneConst` | `generic.array_lane_const` | 数组视图 |
| `kArrayMux` | `generic.array_mux` | 数组视图 |
| `kArrayOnehot` | `generic.array_onehot` | 数组视图 |
| `kArrayReduceOr/kArrayReduceAnd/kArrayReduceXor` | `generic.array_reduce_or/and/xor` | 数组视图 |
| `kArrayReduceLanesOr/kArrayReduceLanesAnd/kArrayReduceLanesXor` | `generic.array_reduce_lanes_or/and/xor` | 数组视图 |
| `kArrayBroadcast` | `generic.array_broadcast` | 数组视图 |

### 3.2 消除的操作

- **层次结构**：`kInstance`、`kBlackbox`、`kXMRRead`、`kXMRWrite` 无对应操
  作——层次在映射时全部展开，XMR 在映射时解析（GRHSIM IR 第 1 节）。
- **声明类**：`kRegister`、`kLatch`、`kMemory`、`kDpicImport` 不再是图上的
  op——寄存器/锁存器/存储器声明移入 StateDecl，DPI 声明移入 HostTable
  （GRHSIM IR 第 2 节）。声明的属性（`width`、`row`、`isSigned`、`initValue`
  等）平移到对应声明条目，语义不变。

### 3.3 更名与改造的操作

端口读写、状态端口与调用类操作按下表对应；除注明外，操作数、结果、属性
与语义同 GRH IR：

| GRH IR | generic | 改造要点 |
| --- | --- | --- |
| 端口 Value | `generic.in_read` / `generic.out_write` | 端口访问显式化（4.1 节） |
| `kRegisterReadPort` | `generic.reg_read` | 读快照（4.2 节） |
| `kRegisterWritePort` | `generic.reg_write` | events 操作数改属性；求值时同步 `S_edge`（4.3、第 5 节） |
| `kLatchReadPort` | `generic.latch_read` | 读快照（4.4 节） |
| `kLatchWritePort` | `generic.latch_write` | 电平门控，无事件（4.5 节） |
| `kMemoryReadPort` | `generic.mem_read` | 读快照（4.6 节） |
| `kMemoryReadAllPort` | `generic.mem_read_all` | 读快照（4.6 节） |
| `kMemoryWritePort` | `generic.mem_write` | 改造同 `generic.reg_write`（4.7 节） |
| `kMemoryWriteLanesPort` | `generic.mem_write_lanes` | 改造同 `generic.reg_write`（4.8 节） |
| `kMemoryFillPort` | `generic.mem_fill` | 整数组条件写，事件改为属性（4.9 节） |
| `kSystemFunction` | `generic.host_call`（查询形式） | 第 6 节 |
| `kSystemTask` | `generic.host_call`（调用形式） | 第 6 节 |
| `kDpicCall` | `generic.host_call` | 按条目类别取查询或调用形式（第 6 节） |

### 3.4 仿真导向操作（迁移兼容）

`generic.mem_read_all`、`generic.mem_write_lanes`（4.6、4.8 节）与
`generic.array_*` 数组视图操作（3.1 节）是一类特殊操作：它们面向仿真性
能优化（整数组 / packed 视图上的批量操作），与 SystemVerilog 语义很难直
接对应——这类操作正是设立 GRHSIM IR 的核心动机之一，本应只存在于
GRHSIM IR。

当前它们因历史原因存在于 GRH IR，并由 GRH IR 上的 pass 产生；generic 方
言保留它们作为迁移兼容。目标形态：产生这些操作的 pass 迁移到 GRHSIM IR
上（属于子图替换，GRHSIM IR 4.1 节），届时从 GRH IR 中删除这些操作，
GRH IR 只保留能对应 SV 语义的操作集合。

### 3.5 lowering bridge

GRH Value 的声明类型同时承担 self-determined 与 context-determined 语义；
generic op 则要求每个 native 计算和状态访问都有显式、可验证的 Type。为保
持语义，`lower_grhsim` 在以下边界插入 bridge：

- 被使用但没有 source op 或端口绑定的 GRH Value，按自身 Type 物化为零常
  量：`logic<W>` 为 `W'b0`，`real` 为 `0.0`，`string` 为空串。
- effect `host_call`、寄存器/锁存器写和单地址/整数组存储器写的条件若宽于
  1 bit，先生成同 Type 零常量，再以 `generic.ne(value, zero)` 得到 1-bit
  真值；这等价于 SystemVerilog 条件的“非零为真”。
- `and/or/xor/xnor` 的 native 结果宽度取两个操作数宽度的最大值；若 GRH
  结果处在更窄或不同 signedness 的上下文，再以 `generic.assign` 转成声明
  Type。mux/array_mux 的数据分支以及状态写的 data/mask 同样在需要时插入
  `generic.assign`，使 native op 与 StateDecl 契约精确匹配。
- `kMemoryFillPort` 若提供一个 element-width 值而非完整 packed 图像，先以
  `generic.array_broadcast {rows = N}` 复制成 `logic<N*W>`；完整 packed 输
  入直接使用。
- 同一 StateDecl 有多个存储器 writer 时，lowering 按 4.10 节把源端顺序与
  显式 priority group 规范化为一个连续的 generic priority group。

例如 `logic<8> a` 与 `logic<32> b` 在 16-bit 上下文执行 `a & b`，lowering
生成 32-bit `generic.and`，随后生成 16-bit `generic.assign`。对于
`array<4, logic<8>> mem` 的 8-bit fill 数据 `%x`，lowering 生成：

```text
%image = generic.array_broadcast {rows = 4} (%x)  -- logic<32>
         generic.mem_fill {state = "mem", ...} (%cond, %image)
```

## 4. 状态与端口访问 op

以下 op 的读写目标引用 StateDecl 中的标识符（`S_in`、`S_out`、`S_state` 的
成员）。所有读取自本轮快照，所有写入汇集到本轮末态（仿真模型 5.1、5.2
节）。

### 4.1 generic.in_read / generic.out_write

**generic.in_read**（输入端口读）

- **operands**：无
- **results**：`res[0]`：端口值
- **attrs**：`port` (string)：StateDecl 中的输入端口标识
- **语义**：`res[0] = S_in[port]`。`S_in` 在 eval 期间固定，故一次 eval 内
  各轮读出相同。

**generic.out_write**（输出端口写）

- **operands**：`oper[0]` (value)：写出的值（位宽同端口）
- **results**：无
- **attrs**：
  - `port` (string)：StateDecl 中的输出端口标识
  - `eventState` (bool，可选，默认 `false`)：该 sink 是 lowering 为内部事件
    提升出的历史状态，不是用户可见输出
- **语义**：普通形式每轮把 `value` 写入 `S'_out[port]`。每个输出端口必须
  恰好有一个 `generic.out_write`（单一驱动）。

`eventState = true` 只用于内部一位事件值。`lower_grhsim` 为这类 StateDecl
强制使用 `$event` 后缀，并让引用它的 `events` / `eventEdge` 消费者与 writer
建立调度依赖。单线程后端在 writer 执行时比较本轮旧值与新值，产生仅在当
前 fixed-point round 有效的 posedge/negedge pulse；同轮排在 writer 后的消费
者读取 pulse，round 结束后 pulse 清零。该 StateDecl 仍参与 `S_edge` 历史跟
踪，但不导出为生成 C++ 类的公开输出端口。

### 4.2 generic.reg_read

- **operands**：无
- **results**：`res[0]`：寄存器值（位宽 `W`）
- **attrs**：`state` (string)：StateDecl 中的寄存器标识
- **语义**：`res[0] = 快照 S_state[state]`。

### 4.3 generic.reg_write

对应 GRH IR `kRegisterWritePort`，多事件触发、边缘门控的寄存器写。

- **operands**：
  - `oper[0]` (updateCond)：更新使能（1-bit）
  - `oper[1]` (nextValue)：更新值（位宽 `W`）
  - `oper[2]` (mask)：逐位写掩码（位宽 `W`）
- **results**：无
- **attrs**：
  - `state` (string)：目标寄存器标识
  - `events` (string[])：事件信号的 StateDecl 标识（见第 5 节）
  - `eventEdge` (string[])：与 `events` 等长，`"posedge"` / `"negedge"`
- **语义**：任一事件检测到边缘（触发）且 `updateCond` 为 1 的轮次，按
  mask 把 `nextValue` 合并入末态 `S'_state[state]`；否则不写入。reset/
  enable 优先级、异步复位等同 GRH IR，由上游 `generic.mux` 等表达式编码
  （编码案例见 GRH IR `kRegisterWritePort`）。每个 `generic.reg_write` 每
  轮求值时同步其事件信号的 `S_edge`（第 5 节）。

### 4.4 generic.latch_read

- **operands**：无
- **results**：`res[0]`：锁存器值（位宽 `W`）
- **attrs**：`state` (string)：StateDecl 中的锁存器标识
- **语义**：`res[0] = 快照 S_state[state]`。

### 4.5 generic.latch_write

对应 GRH IR `kLatchWritePort`，电平门控，无事件。

- **operands**：
  - `oper[0]` (updateCond)：更新使能（1-bit）
  - `oper[1]` (nextValue)：更新值（位宽 `W`）
  - `oper[2]` (mask)：逐位写掩码（位宽 `W`）
- **results**：无
- **attrs**：`state` (string)：目标锁存器标识
- **语义**：`updateCond` 为 1 的轮次，按 mask 把 `nextValue` 合并入末态；
  为 0 时不写入（锁存器保持）。

### 4.6 generic.mem_read / generic.mem_read_all

**generic.mem_read**（单地址异步读）

- **operands**：`oper[0]` (addr)：读地址（无符号解释）
- **results**：`res[0]` (data)：读出的行（位宽 `W`）
- **attrs**：`state` (string)：目标存储器标识
- **语义**：`res[0] = 快照 S_state[state][addr]`。同步读由边缘门控的
  `generic.reg_write` 捕获读结果实现（同 GRH IR 的约定）。

**generic.mem_read_all**（整数组读）

- **operands**：无
- **results**：`res[0]`：packed 视图，位宽 `row × W`，位布局
  `res[0][i*W +: W] = mem[i]`
- **attrs**：`state` (string)：目标存储器标识
- **语义**：`res[0] = 快照 S_state[state]` 的 packed 视图。

### 4.7 generic.mem_write

对应 GRH IR `kMemoryWritePort`，事件触发的单地址写。

- **operands**：
  - `oper[0]` (updateCond)：写入条件（1-bit）
  - `oper[1]` (addr)：写地址
  - `oper[2]` (data)：写数据（位宽 `W`）
  - `oper[3]` (mask)：逐位写掩码（位宽 `W`）
- **results**：无
- **attrs**：`state` (string)；`events` (string[])；`eventEdge` (string[])；
  `memoryWrite.priorityGroup` / `memoryWrite.priority`（可选，语义同 GRH
  IR：同组同存储器、priority 唯一连续，同轮多写使能时按 priority 从大到
  小依次施加，`0` 最后写入）
- **语义**：触发且 `updateCond` 为 1 的轮次，按 mask 把 `data` 合并入末
  态 `S'_state[state][addr]`。每轮求值时同步其事件信号的 `S_edge`（第 5
  节）。

### 4.8 generic.mem_write_lanes

对应 GRH IR `kMemoryWriteLanesPort`，事件触发的 lanes 写（lane 内整写，
无逐位 mask）。

- **operands**：
  - `oper[0]` (laneMask)：守卫向量（位宽 `row`），每 lane 1 bit 写使能
  - `oper[1]` (data)：写数据（packed 视图，位宽 `row × W`）
- **results**：无
- **attrs**：同 4.7 节（含可选的 `memoryWrite.priority*`）
- **语义**：触发的轮次，对 `laneMask[i] = 1` 的 lane 写入
  `data[i*W +: W]`，其余 lane 保持。每轮求值时同步其事件信号的 `S_edge`
  （第 5 节）。

### 4.9 generic.mem_fill

对应 GRH IR `kMemoryFillPort`，事件触发地替换完整存储器图像。

- **operands**：
  - `oper[0]` (updateCond)：写入条件（1-bit）
  - `oper[1]` (data)：完整 packed 图像（位宽 `row × W`）
- **results**：无
- **attrs**：同 4.7 节（含可选的 `memoryWrite.priority*`）
- **语义**：触发且 `updateCond` 为 1 的轮次，对每个
  `i in [0, row)` 写入 `S'_state[state][i] = data[i*W +: W]`。未触发或
  条件为 0 时保持原值；每轮同步事件信号的 `S_edge`。

例如 `state` 是 `array<4, logic<8, unsigned>>`、`data = 32'h04030201`，
执行后 row 0 到 row 3 依次为 `8'h01`、`8'h02`、`8'h03`、`8'h04`。
若 GRH 输入是单个 8-bit element，lowering 按 3.5 节先广播；例如
`data = 8'hA5` 产生 packed 图像 `32'hA5A5A5A5`。

### 4.10 写冲突

同一存储器 StateDecl 有多个 writer（`mem_write`、`mem_write_lanes`、
`mem_fill` 任意组合）时，全部 writer 必须属于同一个 priority group，且
priority 是从 `0` 到 `N-1` 的连续唯一整数。执行按 priority 从大到小施加，
因此 priority `0` 最后写入并在重叠位上获胜。

`lower_grhsim` 在每个 StateDecl 边界统一建立这个契约：未带 priority 的
writer 保持 GRH source order；显式 group 内按原 priority 从大到小保持既有
优先顺序，并在该 group 第一次出现的位置作为一个连续片段。得到总序后，第
`i` 个 writer 被赋值 `priority = N-1-i`，所有 writer 使用规范组名
`$lower_grhsim$<state>`。因此完全无属性、部分有属性和多个显式组的输入都
会变成一个连续目标组，且 priority `0` 仍对应源语义中最后获胜的 writer。

寄存器/锁存器的多个写 op 同轮使能仍属未定义行为，实现应诊断；本节的规
范化只适用于存储器 writer。

## 5. 写操作上的边缘检测与 S_edge 更新

带事件的 op（`generic.reg_write`、`generic.mem_write`、
`generic.mem_write_lanes`、`generic.mem_fill` 与调用形式的
`generic.host_call`）持有事件属性 `events` / `eventEdge`。与 GRH IR 的
差别：事件信号是 StateDecl 标识（属性），不经数据流边传递——op 直接从
本轮快照 `S` 与 `S_edge` 读取。
事件信号必须是 `S` 的成员（仿真模型第 3 节）。

此类 op 每轮求值时执行两件事：

1. **边缘检测**：对每个事件信号比较 `cur = S[signal]`（快照）与
   `old = S_edge[signal]`：`posedge` ⟺ `old == 0` 且 `cur == 1`；
   `negedge` ⟺ `old == 1` 且 `cur == 0`。任一事件检测到边缘，op 被触发
   （是否生效再看 `updateCond`/`callCond`）。含 X/Z 时不构成边缘；SV
   LRM 的四态迁移分类（如 `0→x`）如有需要，由后续修订补充。
2. **S_edge 更新**：无论是否触发，把其事件信号在 `S_edge` 中的值同步为
   `cur`（写入 `S'_edge`）。模型 5.3 节的逐轮同步即由带事件的 op 执行；
   同一信号被多个 op 跟踪时，各 op 同步出相同的值，无冲突。

因此同一边缘在一次 eval 内恰好可见一次：检测发生的下一轮，`S_edge` 已
同步为当前值，不再重复触发。

被跟踪信号集 TrackSet 由图上各 op 的 `events` 与区域激活条件引用的信号
汇总得到。区域激活条件（GRHSIM IR 4.2 节）的形式与事件属性相同：
（信号，`"posedge"` / `"negedge"`），或恒真（每轮必做）。

映射时，`events` 中的信号若不是 `S` 的成员（内部线网），映射负责先把它
提升为状态单元。

## 6. host 调用 op：generic.host_call

所有宿主动作统一为 HostTable 条目 + `generic.host_call`。条目记录：

- `entry` (string)：条目标识；
- `kind`：`"query"`（查询类）或 `"effect"`（副作用类），分类见仿真模型
  4.3 节；
- 参数签名（类型、方向）与绑定（宿主符号）。

GRH IR 的系统函数/系统任务（`name` 去掉 `$`）与 DPI import 都注册为条
目；`hasSideEffects`、`procKind`、`hasTiming` 等属性随条目平移保留。

**generic.host_call** 按条目类别有两种形式：

- **查询形式**（`"query"` 条目，对应 `kSystemFunction`）：
  - **operands**：`oper[0]`.. (args)：调用参数，按条目签名排列
  - **results**：返回值，按条目签名
  - **语义**：每轮经 `H` 求值并返回宿主当前结果。查询本身不改变 `S`，但
    `H` 允许是有状态的（仿真模型 5.5 节），其返回可随宿主状态变化——
    `$random`、`$urandom` 等随机源即是如此：每次调用推进宿主随机状态，
    返回可随轮次变化。若因此使迭代不收敛，由振荡诊断报告（仿真模型 5.6
    节）。
- **调用形式**（`"effect"` 条目，对应 `kSystemTask`、`kDpicCall`）：
  - **operands**：`oper[0]` (callCond)：调用条件（1-bit）；`oper[1]`..
    (args)：调用参数
  - **results**：按条目签名（DPI 的返回值与 out/inout 参数输出侧）
  - **attrs**：`entry` (string)；`events` (string[])；`eventEdge`
    (string[])（可为空）
  - **语义**：`callCond` 为 1 且被事件触发（`events` 为空时视为每轮触发）
    的轮次，经 `H` 发起调用。每轮求值时同步其事件信号的 `S_edge`（第 5
    节）。

本方言不对副作用调用做跨迭代去重：调用在条件成立的每一轮都发生。门控
是否恰当（边缘门控还是电平门控）是设计者的责任——门控不当的自然后果
就是重复触发。

例（`always @(posedge clk) $display("debug: %h", data);`）：

```text
%d = ...                                        -- data 的驱动表达式
   = generic.host_call {entry = "display", events = ["clk"], eventEdge = ["posedge"]}
       (1'b1, "debug: %h", %d)
```

例（上升沿寄存器，`always @(posedge clk) q <= d;`）：

```text
%d = ...                                        -- d 的驱动表达式
   = generic.reg_write {state = "q", events = ["clk"], eventEdge = ["posedge"]}
       (1'b1, %d, 全1)
```

## 7. 初始化

StateDecl 条目携带初始化属性（寄存器的 `initValue`，存储器的
`initKind/initFile/initValue/initStart/initLen`），属性形式与语义沿用
GRH IR 对应声明：初始化在 Sim 构建时执行一次，多项初始化按顺序执行、后
者覆盖前者。`$random` 初始化的具体规则（PRNG 与可复现性）由后续文档定
义。

`S_edge` 在 Sim 构建时初始化为 TrackSet 中各信号的初始值，使初始时不存
在可见边缘（仿真模型 5.3 节）。

## 8. 本文不定义的内容

- 逻辑值的编码与存储布局（存储层与后端方言）。
- 图划分、调度的具体形式与 pass 组织（[Pass 系统与流水线](../grhsim-ir-pipeline.md)）。
- `$random` 等随机源的 PRNG 与可复现性规则。
- 四态 X/Z 迁移的精细边沿分类（第 5 节已注明）。
- eval 的调用时机与时间推进（同仿真模型文档）。
