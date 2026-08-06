# GRHSIM-AM 指令集

> 状态：讨论稿 v0。本稿定义简单组合指令、变化检测、状态单元、外部调用和 Block 激活指令。

Program、Variable、Block 和理想存储见 [GRHSIM Abstract Machine 基础设计](grhsim-am.md)，
system/DPI 的宿主接口见 [HostEnvironment 参考定义](grhsim-host-environment.md)。

## 1. 范围

本文定义 `kConstant` 至 `kSliceArray` 对应 40 种 GRH 纯组合 Operation 的 Logic
形态：`kConstant` 直接成为 constant Variable，其余 39 种在 operand/result 均为
Logic 时产生本稿指令；Real/String constant 遵循同一规则。本文还定义
`changed.any/changed.pos/changed.neg`、`reg.write`、`mem.read/mem.write/mem.fill` 和
`latch.write`、数组视图指令 `mem.read_all/mem.write_lanes` 与纯组合
`array.mux/array.reduce_*/array.broadcast/array.onehot`（`kArrayLaneConst` 与
`kConstant` 一样直接成为 constant Variable）、`system.function/system.task`、
`dpi.call` 和 `act.f/act.b`。层次结构和 XMR
不是 GRHSIM-AM 指令，必须在 lower 前消解。Real/String 专用组合指令仍待后续定义；
constant、`changed.any`、系统调用和 DPI 已按本文定义覆盖 Real/String。

本稿指令同时是 `AmGraph` 工作图形式的 op 载荷：图形式只改变指令的承载与连接
（Variable 声明语义、状态操作数边的 PreCommit/Live 分类显式化），不改变任何指令
语义，见[流水线文档](grhsim-am-pipeline.md) 2.4 节。

## 2. 公共语义

### 2.1 Instruction schema

指令的逻辑结构为：

```text
Instruction = (Opcode, Results, Operands, Attributes)
```

规范文本形式为：

```text
%result0, ... = opcode %operand0, ... {attributeName = attributeValue, ...}
```

没有 Result 时省略左侧和 `=`；没有 Operand 时省略 operand 列表；没有 Attribute 时
省略 `{...}`。逗号只分隔同一列表中的元素，opcode、operand 列表和 Attribute 块之间
不加逗号。

Results 和 Operands 都是 VarId，但职责不同：

- Result 写在 `=` 左侧，只表示指令产生的新值；一条指令的 Result 按 opcode schema
  定序；
- 只读 Operand 写在 opcode 后，按 opcode schema 的语义顺序排列；
- 同时被读取和写回的状态对象，例如 `reg.write` 的 `%target` 和 `changed` 的 `%old`，
  是 read-write Operand，不是 Result；
- Result 可以在类型允许时与 Operand 使用相同 VarId；执行仍先读完全部 Operand，再
  写 Result 和 read-write Operand，因此文本不是 SSA。

Operand 排布遵循以下约定：

| 类别 | 排布 |
| --- | --- |
| 纯计算 | 数据源按表达式顺序排列。二元指令固定为 lhs、rhs。 |
| 选择/寻址 | `mux` 固定为 cond、true、false；slice/memory read 固定为 base/target 在前，随后是 address/index。 |
| 状态写 | `mem.write` 固定为 `%cond, %addr, %mask, %data, %target`；`mem.write_lanes` 的 laneMask 在最前；`reg.write/latch.write/mem.fill` 为已合并写入条件与掩码的 payload 在前、`%target` 随后；variadic event 始终位于末尾（`latch.write` 无 event）。 |
| 外部调用 | `system.function` 的 Operand 全是参数；有条件的 `system.task/dpi.call` 把 `%cond` 放在最前。输入参数按调用 schema 排列，variadic event 始终位于末尾；返回值和 output/inout 参数写在 Result 列表。 |
| Block 激活 | `%event` 是唯一 Operand，目标 Block 集合放在 `targets` Attribute。 |

`reg.write/latch.write/mem.fill` 不再有 `%cond/%mask` 操作数：写入条件与掩码在 lowering
时合并进 payload（见第 12 节）；`mem.write` 保留 `%cond/%mask` 操作数，写使能与逐 bit
掩码由指令自身判定。当前状态写的规范排列为：

```text
reg.write       %nextValue, %target, %event0, ...
mem.write       %cond, %addr, %mask, %data, %target, %event0, ...
mem.fill        %data, %target, %event0, ...
latch.write     %nextValue, %target
mem.write_lanes %laneMask, %data, %target, %event0, ...
```

所有运行时 RTL 数据都通过 Variable 表达，包括 constant Variable。Attribute 只保存
Program 构造时确定、执行期间不变、且不属于 RTL Value 的静态配置。Attribute 名称使用
`lower_snake_case`，在一条指令内唯一；未知、重复或缺少必需 Attribute 均非法。Attribute
排列顺序不影响语义，规范文本按名称字典序排列；list 的元素顺序是否有意义由对应
Attribute 定义。

当前 Attribute Value 使用数学整数、Bool、String、Enum 或这些类型的有限 list/set，
不使用 VarId 表示运行时数据。本稿不定义通用 immediate operand。Variable Init 仍是
Variable 自身的结构化 metadata，不属于 Instruction Attribute。

当前 opcode 的 Attribute schema 为：

| Opcode | Attribute | Type | 必需 | 语义 |
| --- | --- | --- | --- | --- |
| `slice_static` | `lsb` | Nat | 是 | 静态切片最低 bit 下标。 |
| `system.function` | `has_side_effects` | Bool | 是 | 调用是否产生 Result 之外的可观察效果。 |
| `system.function` | `name` | non-empty String | 是 | 不带 `$` 的系统函数名。 |
| `system.function` | `schedule` | `normal \| once \| final` | 是 | 普通、至多一次或 finalize 阶段调用。 |
| `system.task` | `event_count` | Nat | 是 | Operand 尾部 event 的数量。 |
| `system.task` | `event_mode` | `immediate \| pending` | 是 | event 仅限当前执行，或可在同一次 `eval()` 内保留。 |
| `system.task` | `name` | non-empty String | 是 | 不带 `$` 的系统任务名。 |
| `system.task` | `schedule` | `normal \| once \| final` | 是 | 普通、至多一次或 finalize 阶段调用。 |
| `dpi.call` | `event_count` | Nat | 是 | Operand 尾部 event 的数量。 |
| `dpi.call` | `event_mode` | `immediate \| pending` | 是 | event 仅限当前执行，或可在同一次 `eval()` 内保留。 |
| `dpi.call` | `import` | non-empty String | 是 | Program 中唯一 `DpiImport.Symbol`。 |
| `act.f/act.b` | `targets` | `non-empty set<BlockId>` | 是 | 激活目标；文本按 BlockId 升序排列。 |

除上表外，本稿 opcode 均不接受 Attribute。

当前 opcode signature 汇总如下；`rw` 表示 read-write Operand，`*` 表示 variadic
尾部列表：

| Opcode family | Results | Operands | Attributes |
| --- | --- | --- | --- |
| `assign` | `%d` | `%s` | - |
| `add/sub/mul/div/mod` | `%d` | `%a, %b` | - |
| `and/or/xor/xnor` | `%d` | `%a, %b` | - |
| `not` | `%d` | `%a` | - |
| `eq/ne/lt/le/gt/ge` | `%d` | `%a, %b` | - |
| `logic_and/logic_or` | `%d` | `%a, %b` | - |
| `logic_not/reduce_*` | `%d` | `%a` | - |
| `shl/lshr/ashr` | `%d` | `%value, %amount` | - |
| `mux` | `%d` | `%cond, %true, %false` | - |
| `concat` | `%d` | `%src0, ..., %srcN-1` | - |
| `replicate` | `%d` | `%src` | - |
| `slice_static` | `%d` | `%base` | `lsb: Nat` |
| `slice_dynamic` | `%d` | `%base, %start` | - |
| `slice_array` | `%d` | `%base, %index` | - |
| `changed.any/pos/neg` | `%res` | `%new, %old (rw)` | - |
| `reg.write` | - | `%nextValue, %target (rw), %event0, ..., %eventE-1` | - |
| `mem.read` | `%res` | `%target, %addr` | - |
| `mem.write` | - | `%cond, %addr, %mask, %data, %target (rw), %event0, ..., %eventE-1` | - |
| `mem.fill` | - | `%data, %target (rw), %event0, ..., %eventE-1` | - |
| `latch.write` | - | `%nextValue, %target (rw)` | - |
| `system.function` | `%res` | `%arg0, ..., %argA-1` | `has_side_effects: Bool, name: String, schedule: Enum` |
| `system.task` | - | `%cond, %arg0, ..., %argA-1, %event0, ..., %eventE-1` | `event_count: Nat, event_mode: Enum, name: String, schedule: Enum` |
| `dpi.call` | `%return?, %out0, ..., %outO-1, %inout0, ..., %inoutQ-1` | `%cond, %in0, ..., %inI-1, %inout0, ..., %inoutQ-1, %event0, ..., %eventE-1` | `event_count: Nat, event_mode: Enum, import: String` |
| `act.f/act.b` | - | `%event` | `targets: set<BlockId>` |

### 2.2 执行与类型公共规则

`%n` 表示 VariableArea 中的 VarId `n`，即 `n in [0, VariableCount)`。ControlArea
和 ActiveArea 没有 VarId，不能写成指令的 `%n` Operand 或 Result。第 3 至 10 节组合
指令的所有 Result 和 Operand 都必须是 `BV<W, Sign>`；Array 不参与这些指令。

每条指令先读取全部 Operand 和 Attribute，基于这些快照计算，再写回全部 Result 和
read-write Operand。除非具体 opcode 另有说明，这些写回在语义上同时发生。因此 Result
可以与任一 Operand 使用相同 VarId；同一 VarId 不能被一条指令以两个不同值写回。
constant Variable 只能作为只读 Operand。

记：

```text
U_W(x)   = x 的 W-bit 无符号整数值
S_W(x)   = x 的 W-bit 二进制补码整数值
low_W(n) = 整数 n 模 2^W 后的 W-bit 位模式
truth(x) = (U_W(x) != 0)
```

无下标的 `U(x)` 表示按 `width(x)` 求无符号整数值；`trunc0` 表示向 0 取整。

```text
commonS(a, b) = signed    if type(a).Sign = signed and type(b).Sign = signed
                unsigned  otherwise
```

`resize(x, W, Sign)` 产生 W-bit 位模式：缩小时保留最低 W bit；扩大时，
`Sign=signed` 复制源最高位，`Sign=unsigned` 补 0。

所有宽度和下标计算使用数学整数，不允许发生宿主整数溢出。第 3 至 10 节的每个组合
opcode 都是全函数，边界行为不得继承宿主语言的未定义行为。

### 2.3 GRH lower 规则

每个 GRH Logic Value 都映射为同宽、同 Signedness 的 AM `BV<W, Sign>` Variable。指令
根据 operand Type 自行完成下表规定的共同类型转换，不需要为 Signedness 选择不同
opcode。

| GRH kind | 计算宽度 W | 计算 signedness C | 结果 Type |
| --- | --- | --- | --- |
| `kAdd/kSub/kAnd/kOr/kXor/kXnor` | `max(width(L), width(R))` | `commonS(L, R)` | `BV<W, C>` |
| `kMul` | `width(L) + width(R)` | `commonS(L, R)` | `BV<W, C>` |
| `kDiv/kMod` | `width(L)` | `commonS(L, R)` | `BV<W, C>` |
| equality 与关系比较 | `max(width(L), width(R))` | `commonS(L, R)` | `BV<1, unsigned>` |
| `kShl/kLShr/kAShr` | result 宽度 | `signedness(L)` | `BV<W, C>` |
| `kMux` | result 宽度 | `commonS(True, False)` | `BV<W, C>` |

除 shift 外，表中二元 operand 都先执行 `resize(operand, W, C)`。Shift 只对
lhs 执行该 resize；amount 保留原 Type，并按 `U(amount)` 解释。此外：

- `kConstant` 的结果 Value 映射为 `Init = constant(value)` 的 Variable，不生成指令、
  Block 或 Block 激活；该规则同样适用于 Real 和 String constant；
- 若 GRH result Type 与表中的原生结果 Type 不同，先将指令结果写入原生 Type 的临时
  Variable，再用 `assign` 写入映射该 GRH result 的 Variable；
- `kAssign` 扩大时使用源 Type 的 signedness；目标 Signedness 只影响后续解释；
- `kAShr` 保留 opcode，由左操作数 Type 决定高位复制符号位还是补 0；
- GRHSIM-AM 的二态 BV 将普通、case 和 wildcard 相等比较分别规范化为同一个 `eq`
  或 `ne`；
- GRH Logic literal 必须解析为与结果 Variable 完全匹配的 typed value；含 X/Z 的值
  无法 lower 到 GRHSIM-AM BV；
- 第 3 至 10 节的 opcode 不接受 Real 或 String operand/result；这些类型的组合形态
  留待后续定义，SystemFunction/SystemTask 和 DPI 分别由第 14、15 节承接。

`kConstant` 必须在 Block 构造前完成上述 lower；移除它后为空的候选 Block 不生成，
随后再稠密分配 BlockId 并确定 act 的 `targets` Attribute。消费者直接以 constant
Variable 的 VarId 作为源。

## 3. 赋值

| Opcode | 对应 GRH | 形式 | 语义与约束 |
| --- | --- | --- | --- |
| `assign` | `kAssign` | `%d = assign %s` | 调整位宽后写入；目标 Signedness 不改变位模式。 |

设源宽 `Ws`、目标宽 `Wd`：

- `Wd = Ws`：原样复制；
- `Wd < Ws`：保留源的最低 Wd bit；
- `Wd > Ws`：源 Type 为 signed 时复制源 bit `Ws-1`，否则补 0。

## 4. 算术运算

以下指令形式均为 `%d = opcode %a, %b`。令计算宽度和 signedness 为 2.3 节定义的
`W`、`C`，并令 `a' = resize(a, W, C)`、`b' = resize(b, W, C)`；`%d` 必须为
`BV<W, C>`。

| Opcode | 对应 GRH | 结果 |
| --- | --- | --- |
| `add` | `kAdd` | `low_W(U_W(a') + U_W(b'))` |
| `sub` | `kSub` | `low_W(U_W(a') - U_W(b'))` |
| `mul` | `kMul` | `low_W(U_W(a') * U_W(b'))` |
| `div` | `kDiv` | `C=unsigned` 时为 `low_W(U_W(a') / U_W(b'))`；`C=signed` 时为 `low_W(trunc0(S_W(a') / S_W(b')))`。 |
| `mod` | `kMod` | `C=unsigned` 时为 `low_W(U_W(a') mod U_W(b'))`；`C=signed` 时为 `low_W(S_W(a') - trunc0(S_W(a') / S_W(b')) * S_W(b'))`。 |

`div/mod` 的除数为 0 时结果为 0。signed 最小负数除以 -1 按 `low_W` 回绕；signed
余数的符号跟随被除数。所有结果编码为 W-bit 位模式，不得触发宿主溢出。除零返回 0
是 GRHSIM-AM 的确定规则。

## 5. 位运算

`and/or/xor/xnor` 形式为 `%d = opcode %a, %b`。二元 Operand 按 2.3 节得到 `W`、
`C`，令 `a' = resize(a, W, C)`、`b' = resize(b, W, C)`，并对 `a'`、`b'` 逐位
运算；Result 必须是 `BV<W, C>`。`%d = not %a` 要求 Result 与 Operand Type 相同。

| Opcode | 对应 GRH | 结果 |
| --- | --- | --- |
| `and` | `kAnd` | 逐位 `a' & b'` |
| `or` | `kOr` | 逐位 `a' \| b'` |
| `xor` | `kXor` | 逐位 `a' ^ b'` |
| `xnor` | `kXnor` | 逐位 `~(a' ^ b')`，只保留 W bit |
| `not` | `kNot` | 逐位 `~a`，只保留 W bit |

## 6. 比较运算

形式均为 `%d = opcode %a, %b`。两个 Operand 按 2.3 节规整为 `a'`、`b'`，计算宽度
为 W、共同 signedness 为 C；`%d` 必须为 `BV<1, unsigned>`。

| Opcode | 对应 GRH | 结果为 1 的条件 |
| --- | --- | --- |
| `eq` | `kEq/kCaseEq/kWildcardEq` | `a'`、`b'` 位模式相同 |
| `ne` | `kNe/kCaseNe/kWildcardNe` | `a'`、`b'` 位模式不同 |
| `lt` | `kLt` | `C=unsigned` 时比较 `U_W(a') < U_W(b')`，否则比较 `S_W(a') < S_W(b')`。 |
| `le` | `kLe` | 同上；满足 `<=`。 |
| `gt` | `kGt` | 同上；满足 `>`。 |
| `ge` | `kGe` | 同上；满足 `>=`。 |

条件不成立时结果为 0。

## 7. 逻辑与规约

逻辑指令的 Result 均为 `BV<1, unsigned>`；Operand 可以具有任意宽度和 Signedness：

| Opcode | 对应 GRH | 形式 | 结果 |
| --- | --- | --- | --- |
| `logic_and` | `kLogicAnd` | `%d = logic_and %a, %b` | `truth(a) && truth(b)` |
| `logic_or` | `kLogicOr` | `%d = logic_or %a, %b` | `truth(a) \|\| truth(b)` |
| `logic_not` | `kLogicNot` | `%d = logic_not %a` | `!truth(a)` |

规约指令形式为 `%d = opcode %a`，Result 为 `BV<1, unsigned>`，Operand 为任意
`BV<W, Sign>`：

| Opcode | 对应 GRH | 结果 |
| --- | --- | --- |
| `reduce_and` | `kReduceAnd` | 所有位均为 1 |
| `reduce_nand` | `kReduceNand` | `reduce_and` 的反值 |
| `reduce_or` | `kReduceOr` | 至少一位为 1 |
| `reduce_nor` | `kReduceNor` | `reduce_or` 的反值 |
| `reduce_xor` | `kReduceXor` | 1 bit 的个数为奇数 |
| `reduce_xnor` | `kReduceXnor` | `reduce_xor` 的反值 |

## 8. 移位

形式均为 `%d = opcode %value, %amount`。`%d` 与 `%value` 的 Type 必须完全相同；
`%amount` 可为任意 `BV<A, Sign>`，始终按 `U(amount)` 解释，其 Signedness 被忽略。

| Opcode | 对应 GRH | `U(amount) < W` | `U(amount) >= W` |
| --- | --- | --- | --- |
| `shl` | `kShl` | 左移并保留最低 W bit | 0 |
| `lshr` | `kLShr` | 逻辑右移，高位补 0 | 0 |
| `ashr` | `kAShr` | value 为 signed 时复制 bit `W-1`，否则补 0 | value 为 signed 时复制 bit `W-1`，否则为 0 |

## 9. 选择与数据重组

| Opcode | 对应 GRH | 形式 | 语义与约束 |
| --- | --- | --- | --- |
| `mux` | `kMux` | `%d = mux %cond, %t, %f` | `%cond` 为任意 Signedness 的 `BV<1, Sign>`；分支按 Result 宽度和 `commonS(t,f)` resize，Result Type 与结果一致；条件为 1 取 `%t`，否则取 `%f`。 |
| `concat` | `kConcat` | `%d = concat %s0, ..., %sN-1` | `N >= 1`；Result 为 `BV<sum(width(si)), unsigned>`；`%s0` 位于最高位。 |
| `replicate` | `kReplicate` | `%d = replicate %s` | Result 为 unsigned BV，且其宽度是 `width(s)` 的正整数倍；按该倍数重复拼接 `%s`。 |

lower `kReplicate` 时必须验证 `rep = width(d) / width(s)`。

## 10. 切片

| Opcode | 对应 GRH | 形式 | 约束 |
| --- | --- | --- | --- |
| `slice_static` | `kSliceStatic` | `%d = slice_static %base {lsb = Lsb}` | `%d` 必须为 unsigned；`Lsb >= 0` 且 `Lsb + width(d) <= width(base)`。 |
| `slice_dynamic` | `kSliceDynamic` | `%d = slice_dynamic %base, %start` | `%d` 必须为 unsigned；`%start` 忽略 Signedness 并按 `U(start)` 解释。 |
| `slice_array` | `kSliceArray` | `%d = slice_array %base, %index` | `%d` 必须为 unsigned 且 `width(d)` 整除 `width(base)`；`%index` 忽略 Signedness 并按 `U(index)` 解释。 |

`slice_static` 返回 `base[Lsb + width(d) - 1 : Lsb]`。`lsb` 是数学自然数 Attribute；
最高下标由 `%d` 的宽度唯一推导，不另存 `msb`。另外两条指令对目标 bit `j`
分别读取：

```text
slice_dynamic: base[U(start) + j]
slice_array:   base[U(index) * width(d) + j]
```

源下标超出 `base` 时该目标 bit 为 0。乘加下标使用数学整数，不得因宿主溢出回绕。
`slice_array` 操作 packed BV，不是对 AM `Array` 的访存。

lower 时，`kSliceStatic.sliceStart` 成为 `lsb`，并验证
`sliceEnd = lsb + width(d) - 1`；`kSliceDynamic/kSliceArray.sliceWidth` 必须等于
`width(d)`。

例如 `%0` 为值 `4'b1111` 的 `BV<4, signed>`，`%1` 为 `BV<8, unsigned>`；`%2`
为相同位模式的 `BV<4, unsigned>`，`%3` 为 `BV<8, signed>`：

```text
%1 = assign %0  -> %1 = 8'hFF  // 源为 signed，符号扩展
%3 = assign %2  -> %3 = 8'h0F  // 源为 unsigned，零扩展
```

## 11. 变化检测

```text
%res = changed.any %new, %old
%res = changed.pos %new, %old
%res = changed.neg %new, %old
```

`changed.any` 的 `%new` 和 `%old` 可以是 BV、Real、String 或 Array，但二者 Type
必须完全相同。它使用基础设计定义的 `sameValue` 比较完整 Value：Value 不同时结果为
1，相同时结果为 0。

`changed.pos/changed.neg` 的 `%new` 和 `%old` 必须是 Type 完全相同的单 bit BV；
Signedness 不参与边沿判断。三条指令的 `%res` 都必须是 `BV<1, unsigned>`。三个 VarId
必须两两不同，且 `%res` 和 `%old` 均不能是 constant Variable。

每条指令先同时读取 `%new` 和 `%old` 的原值，计算 `%res`，再无条件执行
`%old = %new`。因此 `%res` 的计算只能使用更新前的 `%old`，而 `%old` 在指令完成后
立即保存本次观察到的 `%new`。单 bit BV 的三种检测结果如下：

| `U(old)` | `U(new)` | `changed.any` | `changed.pos` | `changed.neg` |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | 0 |
| 0 | 1 | 1 | 1 | 0 |
| 1 | 0 | 1 | 0 | 1 |
| 1 | 1 | 0 | 0 | 0 |

每条 `changed` 独占一个 `Init = undef` 的 `%old` Variable。它只保存所属指令的上次
观察值，不得出现在其他指令中，也不得由调用方修改。Machine 不执行首次 baseline
同步；第一次执行也直接按表中规则比较 `%new/%old`，产生 `%res` 并执行 `%old = %new`。
因此首次 change/edge、由其产生的 Block 激活和状态写都是 AM 层未定义行为，不要求不同
后端或同一后端的不同运行一致。实现必须为 `%old` 选择类型合法的 Value，不能把 AM 层
UB 实现成宿主语言 UB。`%old` 的内部更新本身不激活 Block。

`%res` 表示当前轮（round）观察到的事件。`%res` 的生命周期按消费者位置分类：

- 消费者全部在同一 Block 内（典型：同块 `act.f`/`act.b`）时不需要清理：Block 每次
  执行时 `changed` 必定先于消费者重写 `%res`，Block 不执行时消费者也不执行，旧值
  没有读者；
- 存在跨 Block 消费者（其他 Block 中状态写的 event operand、host 指令的 event
  operand 或普通指令的数据 operand）时，`%res` 是 round-local 状态：Machine 在每轮
  结束（以及每次 `eval()` 开始）将这些 result 清零，实现只需把为真的 result 记入
  dirty-list 并稀疏清理。跨块 event 必须同轮先产后读，生产 Block 的 BlockId 必须
  小于消费 Block；下一轮若未重新产生则读到 0。

因此 compute 阶段产生的跨块 event（如 clock posedge）在同一轮的 commit 阶段可读；
commit Block 内首部 gate detector 与尾部判变 detector 的 `%res` 都属同块消费
（分别喂同块写指令的 event operand/块级门控和同块 `act.b`）：块被激活执行时
detector 必定先于消费者重写 `%res`，不进 dirty-list。`%old` 基线不受上述清理影响：
检测后立刻 `%old = %new`，跨轮、跨 `eval()` 一直保持。由普通组合指令派生的 event
没有这种隐式清零语义，遵循普通 Variable 的读写规则。`act.f/act.b` 必须与产生其
event 的 `changed` 或组合指令位于同一 Block。后端可以融合相邻的 `changed` 和 act，
且不物化 `%res`。

例如：

```text
%clkpos = changed.pos %clk, %oldclk
act.f %clkpos {targets = [3, 5]}
```

若执行前 `%oldclk = 0`、`%clk = 1`，则执行后 `%clkpos = 1`，并且
`%oldclk = 1`。若下一次执行时 `%clk` 仍为 1，则 `%clkpos = 0`，`%oldclk` 仍为 1。

同一时钟同时存在上升沿和下降沿消费者时，只执行一次 `changed.any`，再根据变化后的
单 bit 当前值派生方向，因此仍只需要一个 `%oldclk`：

```text
%clkchanged = changed.any %clk, %oldclk
%clkpos = logic_and %clkchanged, %clk
%clknot = logic_not %clk
%clkneg = logic_and %clkchanged, %clknot
act.f %clkpos {targets = [3]}
act.f %clkneg {targets = [4]}
```

## 12. 状态单元

### 12.1 Register

每个 GRH `kRegister` lower 为一个可写的 `BV<W, Sign>` Variable，记作 `%target`。
Variable 的宽度和 Signedness 来自 `kRegister.width/isSigned`；`initValue` 按
[Variable Init 规范](grhsim-am.md#9-variable-init-规范) lower，没有 `initValue` 时使用
`Init = undef`。`kRegisterReadPort` 直接映射到该 VarId，对 register 的读取就是读取
`%target`，不生成指令。

`kRegisterWritePort` lower 为：

```text
reg.write %nextValue, %target, %event0, ..., %eventE-1
```

各 operand 定义如下：

| Operand | Type | 含义 |
| --- | --- | --- |
| `%nextValue` | 与 `%target` 完全相同 | 已合并全部写入条件与掩码的新值；指令把它原样写入 `%target`。 |
| `%target` | `BV<W, Sign>` | 对应 `kRegister` 的可写 Variable，同时是本指令的读写目标。 |
| `%event0...E-1` | `BV<1, unsigned>` | 声明该写所属事件签名的触发事件；`E >= 0`（当前 GRH lowering 要求每个 register 写口至少一个 event）。 |

指令先同时读取全部 operand 的原值，再一次写回：

```text
target' = nextValue
```

写指令自身没有 cond/mask/event 判定：源设计的写使能、逐 bit 掩码和多写优先级已由
lowering 折叠进 `%nextValue`（read-old 链式规则见下）；是否执行写由所在 commit
Block 的 gate 决定（块首部 gate detector 结果的 OR，整块只判一次，见
[执行模型第 4.4 节](grhsim-am.md#44-state-write-与-reader-重激活)）。`%event`
operand 声明该写所属的事件签名：scheduler 把每个 event 经其定义 `changed` 规范化为
（边沿种类, 被观测 Variable），按签名集合划分 commit Block，并把 event operand 重
指向块内克隆的 gate detector 结果；它不再逐指令参与触发判定。

`%target` 必须与所有 event 使用不同 VarId；它可以在 Type 允许时与 `%nextValue`
使用相同 VarId，上述"先读后写"规则保证别名时仍使用指令开始时读到的值。
`reg.write` 不产生 result。`reg.write` 只位于 commit Block：commit Block 与 compute
Block 一样按激活位执行，只在其 gate detector watch 的变量变化后被激活。
`reg.write` 也不隐式激活读取 `%target` 的 Block；使 `%target` 实际变化且存在
reader 时，由块尾共享判变 detector 激活 reader compute Block，因此 scheduler 在同一
commit Block 的最终写回之后物化一条共享的 `changed.any %target, %targetOld`，再通过
`act.b` 传播。

cond/mask 折叠规则：同一 `%target`、同一事件签名的写链折叠为单条 `reg.write`。令
链中第 i 条写的 GRH 条件为 cond_i、掩码为 mask_i、数据为 next_i，acc 初值为
`%target` 自身（read-old：读取本轮 commit 相位开始前的状态值），按链序：

```text
blend_i = (acc & ~mask_i) | (next_i & mask_i)
acc     = mux(cond_i, blend_i, acc)
```

链尾的 acc 即为 `%nextValue`；无掩码的写其 blend_i 退化为 `next_i`。

GRH `eventEdge[i]` 与 `events[i]` 在 lower 时先转换成 event：`posedge` 使用
`changed.pos`，`negedge` 使用 `changed.neg`。同一 (边沿种类, 被观测 Variable) 的
event 在一次 Graph lowering 内共享一条 `changed.pos/changed.neg` 指令及其独占
old/event Variable，不为每个写口分别创建 detector；各消费方读到的是同一 detector 的
观测历史。同一写口的多个 event 构成一个事件签名，任一 event 命中都打开所在块的
gate（效果同旧的逐指令 OR 触发）；event 列表不表示事件优先级，也不根据触发来源选择
nextValue；同步/异步 reset、enable 等优先级由 lowering 折叠 `%nextValue` 时按链序
嵌套 mux 显式编码。例如：

```text
reg.write %mergedNext, %q, %clkpos, %rstneg
```

`%clkpos` 或 `%rstneg` 任一个命中都打开所在 commit Block 的 gate，最终写入的 data
由 `%mergedNext` 决定（例如先折叠 reset 分支、再折叠 enable 分支）。同一原始信号同时
需要 posedge 和 negedge 时，可以按第 11 节示例只执行一次 `changed.any`，再在 B0 或
compute Block 中根据当前单 bit 值派生 `%event`。

例如：

```text
B0:
    %clkchanged = changed.any %clk, %oldclk
    %clkpos = logic_and %clkchanged, %clk
    act.f %clkchanged {targets = [3, 7]}

B3:  // compute Block：读取 %q 等，计算 %d 并按上式折叠出 %next
    ...

B7:  // commit Block
    %g = changed.pos %clk, %gclk_old   // 首部 gate detector（scheduler 克隆）
    reg.write %next, %q, %g
    %qchanged = changed.any %q, %qold  // 尾部 watch
    act.b %qchanged {targets = [3]}
```

这等价于在 `%clk` 上升沿把折叠后的 `%next` 写入 `%q`：`%clk` 是 B7 首部 gate
detector watch 的变量，B0 对它的判变经 `act.f` 激活 B7（commit Block 的激活只允许
携带 gate detector watch 的变量，def-use 数据边不激活 commit Block）。块首部
detector 在块被激活时先执行并更新基线；gate（全部首部 detector 结果的 OR）为假时
跳过块内全部写与尾部 watch——例如下降沿轮次中 `%g = 0`，B7 被激活但不写 `%q`。
只有 `%q` 的最终位模式实际变化时 `%qchanged` 才为 1，`act.b` 激发并要求下一轮重新
执行 reader compute Block B3。

lowering 把同一 `%target`、同一事件签名的写链折叠为单条 `reg.write`，因此一个
commit Block 内同一 target 至多一条写。同一 target 的多写只能来自不同事件签名的
commit Block，由 commit 段静态 BlockId 顺序表达优先级：后执行的块读取前一块写后的
target。每个 commit Block 对自身写入的每个 state target 在块尾物化共享的
`changed.any %target, %targetOld`，reader 一律在下一轮执行，只看到本轮最终值。需要与
源设计一致的优先级时，lower 必须通过写链折叠顺序和 commit 段块序显式编码，不得依赖
GRH Operation 的原始遍历顺序。

### 12.2 Memory

每个 GRH `kMemory` lower 为一个可写的 `Array<N, BV<W, Sign>>` Variable，记作
`%target`。`N` 来自 `kMemory.row`，元素宽度和 Signedness 来自
`kMemory.width/isSigned`，并要求 `N >= 1`、`W >= 1`。
`initKind/initFile/initValue/initStart/initLen` 按
[Variable Init 规范](grhsim-am.md#9-variable-init-规范) lower 为按顺序执行的
`fill/load` 动作；没有初始化属性时使用 `Init = undef`。

#### 12.2.1 `mem.read`

GRH `kMemoryReadPort` lower 为异步读：

```text
%res = mem.read %target, %addr
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%target` | `Array<N, BV<W, Sign>>` | 被读取的 memory Variable。 |
| `%addr` | `BV<A, SignA>` | 读取地址；`A >= 1`，Signedness 被忽略。 |
| `%res` | `BV<W, Sign>` | 读取结果，与 memory 元素 Type 完全相同。 |

指令先读取 `%target` 和 `%addr`。若 `U(addr) < N`，则 `%res = target[U(addr)]`；否则
`%res = zero(BV<W, Sign>)`。地址计算使用数学整数，不因宿主整数宽度截断或回绕。
`mem.read` 不修改 `%target`，`%res` 不能是 constant Variable。`%res` 可以在 Type
允许时与 `%addr` 使用相同 VarId，结果仍基于指令开始时读到的地址。同步读不增加
event operand，而是由上层 Register 捕获 `%res`。

例如，`%mem` 为 `Array<3, BV<8, unsigned>>`：

```text
%data = mem.read %mem, %addr
```

`%addr = 1` 时读取 `%mem[1]`；`%addr = 3` 或更大时 `%data = 8'h00`。

#### 12.2.2 `mem.write`

GRH `kMemoryWritePort` lower 为：

```text
mem.write %cond, %addr, %mask, %data, %target, %event0, ..., %eventE-1
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%cond` | `BV<1, SignC>` | 写使能；为 0 时整写抑制。Signedness 被忽略。 |
| `%addr` | `BV<A, SignA>` | 写地址；`A >= 1`，Signedness 被忽略。 |
| `%mask` | `BV<W, SignM>` | 逐 bit 写掩码，宽度等于元素宽度 `W`；Signedness 被忽略。 |
| `%data` | `BV<W, Sign>` | 写数据，与 memory 元素 Type 完全相同。 |
| `%target` | `Array<N, BV<W, Sign>>` | 被写入的 memory Variable。 |
| `%event0...E-1` | `BV<1, unsigned>` | 声明该写所属事件签名的触发事件；`E >= 1`（operand 总数至少 6）。 |

`Sign`、`SignC`、`SignA` 和 `SignM` 分别取 `signed` 或 `unsigned`。指令先同时读取全部
operand 的原值。仅当 `cond = 1` 且 `U(addr) < N` 时执行写，此时只更新地址
`r = U(addr)` 对应的 row，对每个 `j in [0, W)`：

```text
mask[j] = 1 → target'[r][j] = data[j]
mask[j] = 0 → target'[r][j] = target[r][j]
target'[k] = target[k]    for k != r
```

否则（`cond = 0` 或地址越界）整个 `%target` 保持不变——cond 抑制与越界抑制是
`mem.write` 自身保留的判定；被抑制的写不发生任何读旧值/混合，`%target` 完全不变。
`%mask` 全 0 的写不改变任何 bit，效果等同整写抑制。写指令自身没有 event 判定：是否
执行写由所在 commit Block 的 gate 决定；event operand 的事件签名声明与重指向规则与
`reg.write` 相同。`mem.write` 不产生 result，也不隐式激活读取 `%target` 的 Block；
`mem.write` 只位于 commit Block，需要传播实际 memory 变化时，scheduler 使用同块尾部
`changed.any` 检测最终 Array Value，再通过 `act.b` 激活相关 reader compute Block；
被抑制的写不改变 Array Value，不会触发该 detector。eventEdge lowering 与 `reg.write`
相同。

多写规则：同一 `%target`、同一事件签名内的每条元素写保留为一条 `mem.write`，lowering
不为元素写插入 read-old `mem.read`，也不做跨写合并。块内多条写按指令顺序依次生效：
每条写直接作用在执行点可见的 live memory image 上，同地址碰撞由后写覆盖先写，部分
掩码的后写也不会破坏更早写更新的其他 bit。例如：

```text
mem.write %wen, %waddr, %wmask, %wdata, %mem, %clkpos
```

仅当所在块 gate 打开、`%wen = 1` 且 `U(waddr) < N` 时，把 `%wdata` 按 `%wmask` 写入
选中 row；`%wen = 0` 或地址越界时不更新任何 row。

带 GRH `memoryWrite.priorityGroup/priority` 的写端口必须保持在同一顺序域，并按
priority 从大到小 lower，使 priority 0 最后执行。没有 priority 属性的写端口不能从
GRH Operation 遍历顺序推导源级碰撞优先级。同一 target 的 `mem.read` 和 `mem.write`
也按实际执行顺序观察状态：read 在 write 前执行时读取旧 row，在 write 后执行时读取
更新后的 row。每个 commit Block 对自身写入的每个 state target 在块尾物化共享
`changed.any` 检测最终 Array Value 并传播变化；跨 commit Block 的多写由 commit 段
静态顺序表达优先级，reader 在下一轮才观察到本轮最终值。

#### 12.2.3 `mem.fill`

GRH `kMemoryFillPort` lower 为：

```text
mem.fill %data, %target, %event0, ..., %eventE-1
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%data` | `BV<N*W, SignD>` | 整片 packed memory 图像（LSB-row-first）；Signedness 被忽略。 |
| `%target` | `Array<N, BV<W, Sign>>` | 被整体写入的 memory Variable。 |
| `%event0...E-1` | `BV<1, unsigned>` | 声明该写所属事件签名的触发事件；`E >= 0`（当前 GRH lowering 要求每个 fill 口至少一个 event）。 |

`Sign` 和 `SignD` 分别取 `signed` 或 `unsigned`。`N*W` 使用数学整数计算，不得发生
宿主整数溢出。`%data` 恒为整片 packed 图像，不再接受单行（`BV<W>`）data：GRH 的单行
fill 由 lowering 先经 `array.broadcast` 扩成整片 packed，再用
`mux(cond, packed, mem.read_all(target))` 把 fill 条件合并进 `%data`（条件不成立时
保持 read-old 的当前整片值）。

指令先同时读取全部 operand，再无判定地整片写回：对每个 `r in [0, N)`、
`j in [0, W)`：

```text
target'[r][j] = data[r * W + j]
```

即 row 0 位于 packed data 的最低 W bit。写指令自身没有 cond/event 判定，是否执行由
所在 commit Block 的 gate 决定；event operand 的事件签名声明与重指向规则与
`reg.write` 相同。`mem.fill` 不产生 result，也不隐式激活 memory reader；`mem.fill`
只位于 commit Block，实际 Array Value 的变化仍由同块尾部共享 `changed.any` 检测并经
`act.b` 传播。eventEdge lowering 与 `reg.write/mem.write` 相同。

例如，`%mem` 为 `Array<4, BV<8, unsigned>>`：

```text
mem.fill %packed, %mem, %rstneg  // row r 写入 packed[8*r +: 8]
```

源设计的单行广播 fill 由 lowering 展开为：

```text
%packedByte = array.broadcast %byte
%old = mem.read_all %mem
%merged = mux %en, %packedByte, %old
mem.fill %merged, %mem, %rstneg
```

`mem.fill` 与同一 target 的 `mem.write/mem.read` 按实际指令执行顺序观察状态。fill 后的
write 可以覆盖选中 row；write 后的 fill 会覆盖整个 memory。需要
确定的源级优先级必须由 lower 后的指令顺序显式表达。同一事件签名内元素写与 fill
混合时，各写保留各自的指令形态，不发生跨种类合并，按指令顺序生效。

### 12.3 Latch

每个 GRH `kLatch` lower 为一个可写的 `BV<W, Sign>` Variable，记作 `%target`。Variable
的宽度和 Signedness 来自 `kLatch.width/isSigned`，并使用 `Init = undef`。
`kLatchReadPort` 直接映射到该 VarId，对 stored latch value 的读取不生成指令。

GRH `kLatchWritePort` lower 为：

```text
latch.write %nextValue, %target
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%nextValue` | 与 `%target` 完全相同 | 已合并透明条件与掩码的新值。 |
| `%target` | `BV<W, Sign>` | 对应 `kLatch` 的可写 Variable，同时是读写目标。 |

`latch.write` 恰好两个 operand，没有 event。指令先同时读取全部 operand，再无判定地
一次写回：

```text
target' = nextValue
```

源设计的透明/写使能条件与逐 bit 掩码由 lowering 按与 `reg.write` 相同的 read-old
链式规则折叠进 `%nextValue`：acc 初值为 `%target`，按链序
`acc = mux(cond_i, (acc & ~mask_i) | (next_i & mask_i), acc)`（无掩码时 blend 退化为
`next_i`）。`latch.write` 是 level-sensitive 的：折叠后的 `%nextValue` 在条件不成立
时等于 `%target` 当前值，因此写回总是安全；纯 latch commit Block 的 gate 由每条
latch 写一条 `changed.any %nextValue` 构成，只有 nextValue 实际变化才激活块并执行
写回。

`%target` 可以在 Type 允许时与 `%nextValue` 使用相同 VarId，结果仍基于指令开始时
读取的值。`latch.write` 不产生 result，也不隐式激活读取 `%target` 的 Block；
`latch.write` 只位于 commit Block，最终 stored value 的实际变化由同块尾部共享的
`changed.any %target, %targetOld` 检测并经 `act.b` 传播。

建议在 lower 到 GRHSIM-AM 前运行 GRH
[`latch-transparent-read`](../transform/latch-transparent-read.md) transform。该 pass
把每个 read user 改写成显式透明读逻辑；对部分 mask，其等价形式为：

```text
%notMask = not %mask
%newBits = and %nextValue, %mask
%oldBits = and %target, %notMask
%maskedNext = or %newBits, %oldBits
%visible = mux %cond, %maskedNext, %target
```

transform 后，原 `kLatchReadPort` 仍映射到 `%target`，新增的 `not/and/or/mux` 按第 5、
9 节普通组合指令 lower。这样 `%cond = 1` 时 read user 立即看到 masked next value，
`%cond = 0` 时看到 stored value；`latch.write` 只负责维护 stored `%target`。

当前 `latch-transparent-read` 要求同一 `kLatch` 最多一个 write port；多个 write port 会
报告 multi-driven 错误。若流水线不运行该 transform，AM 仍按实际执行顺序执行
`latch.write`，但 lowering 必须自行保证透明读语义已经通过等价组合逻辑表达，不能只把
所有 read port 替换成 stored `%target`。

例如：

```text
// commit Block（纯 latch 块）
%g = changed.any %mergedNext, %gold   // 首部 gate detector：nextValue 判变
latch.write %mergedNext, %q
%qchanged = changed.any %q, %qold     // 尾部 watch
act.b %qchanged {targets = [5]}
```

其中 `%mergedNext` 由 lowering 折叠产生，例如
`%mergedNext = mux %gate, (or (and %d, %allMask), (and %q, (not %allMask))), %q`。
`%gate = 0` 时 `%mergedNext = %q`，块不被激活或 gate 关闭，`%q` 保持。只有 stored
`%q` 的位模式实际变化时 `%qchanged` 才为 1，`act.b` 激发并在下一轮激活 reader
compute Block B5。

### 12.4 数组视图（Array View）

数组视图把一个 `Array<N, BV<W, Sign>>` state 视为 `N` 个等宽 lane 的整体：packed
视图值宽 `N*W`，lane `i` 占连续位 `[i*W +: W]`，lane 0 位于最低 W bit；宽 `N` 的
BV 用作守卫/选择向量，每 lane 1 bit。数组的存储仍是第 12.2 节的 memory
Variable；注意 memory 按 word-stride 行平铺存储（每行 `ceil(W/64)` 个 u64），与
packed 视图的连续位布局不同，`mem.read_all/mem.write_lanes` 在两种布局之间逐 lane
pack/unpack，语义上只按 `[i*W +: W]` 定义。`mem.read_all/mem.write_lanes` 的 state
target 是 memory Variable，按状态访问对象归入 `mem.*` 命名家族；其余 `array.*`
opcode 均为纯组合。数组视图 opcode 的 Signedness 只来自
对应 Variable Type，不改变位级语义。

#### 12.4.1 `mem.read_all`

GRH `kMemoryReadAllPort` lower 为全数组读指令 `mem.read_all`：

```text
%res = mem.read_all %target
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%target` | `Array<N, BV<W, Sign>>` | 被读取的 memory Variable。 |
| `%res` | `BV<N*W, SignR>` | 全数组 packed 视图值。 |

指令读取 `%target`，并对每个 `i in [0, N)` 令 `res[i*W +: W] = target[i]`。
`N*W` 使用数学整数计算。`mem.read_all` 不修改 `%target`，`%res` 不能是 constant
Variable；它按实际执行顺序观察状态，与同一 target 的 `mem.read` 等价。

#### 12.4.2 `mem.write_lanes`

GRH `kMemoryWriteLanesPort` lower 为 `mem.write_lanes`：

```text
mem.write_lanes %laneMask, %data, %target, %event0, ..., %eventE-1
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%laneMask` | `BV<N, SignM>` | 逐 lane 写使能；Signedness 被忽略。 |
| `%data` | `BV<N*W, SignD>` | packed 写数据；Signedness 被忽略。 |
| `%target` | `Array<N, BV<W, Sign>>` | 被写入的 memory Variable。 |
| `%event0...E-1` | `BV<1, unsigned>` | 声明该写所属事件签名的触发事件；`E >= 0`（当前 GRH lowering 要求每个 lane 写口至少一个 event）。 |

指令先同时读取全部 operand 的原值。`%laneMask` 全 0 时整个 `%target` 保持不变
（`any(laneMask)` 早退——这是本指令自身保留的唯一判定）；否则对每个
`r in [0, N)`：

```text
laneMask[r] = 1 → target'[r] = data[r * W + W - 1 : r * W]
laneMask[r] = 0 → target'[r] = target[r]
```

lane 内整写，没有逐 bit mask；lane 内部分写由上游在生成 `%data` 时并入（例如用
`array.mux` 合并保持值）。laneMask 已合并写使能与 lane 粒度，因此本指令形态不变；
写指令自身没有 event 判定，是否执行由所在 commit Block 的 gate 决定，event operand
的事件签名声明与重指向规则与 `reg.write` 相同。`mem.write_lanes` 不产生 result，也不
隐式激活 reader；它只位于 commit Block，实际 Array Value 的变化由同块尾部共享
`changed.any` 检测并经 `act.b` 传播。eventEdge lowering、priority 写组
（GRH `memoryWrite.priorityGroup/priority`，同组按 priority 从大到小执行、`0` 最后
写入）以及多写按实际执行顺序生效的规则都与 `mem.write` 相同。

例如，`%mem` 为 `Array<4, BV<8, unsigned>>`：

```text
mem.write_lanes %lane_mask, %packed, %mem, %clkpos
```

所在块 gate 打开且 `%lane_mask = 4'b0101` 时，`%mem[0] ← packed[7:0]`、
`%mem[2] ← packed[23:16]`，其余 lane 保持。

#### 12.4.3 `array.mux`

GRH `kArrayMux` lower 为逐 lane 选择：

```text
%res = array.mux %sel, %t, %f
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%sel` | `BV<N, SignS>` | 选择向量，每 lane 1 bit；Signedness 被忽略。 |
| `%t` | `BV<N*W, SignT>` | 真值数据。 |
| `%f` | `BV<N*W, SignF>` | 假值数据。 |
| `%res` | `BV<N*W, Sign>` | 逐 lane 选择结果。 |

对每个 `i in [0, N)`：`res[i*W +: W] = sel[i] ? t[i*W +: W] : f[i*W +: W]`。
lane 选择是位级语义（与 `mux` 相同），因此 `%t/%f/%res` 只要求位宽一致且是
`%sel` 宽度的整数倍；Signedness 遵循 `mux` 的共同约定，即
`Sign = commonS(SignT, SignF)`（仅两者均为 `signed` 时结果为 `signed`）。

#### 12.4.4 `array.reduce_or / array.reduce_and / array.reduce_xor`

GRH `kArrayReduceOr/kArrayReduceAnd/kArrayReduceXor` lower 为数组归约：

```text
%res = array.reduce_or  %data
%res = array.reduce_and %data
%res = array.reduce_xor %data
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%data` | `BV<N*W, SignD>` | packed 输入数据。 |
| `%res` | `BV<1, unsigned>` | 归约结果。 |

语义为先逐 lane 内归约、再跨 lane 归约，例如
`res = OR_{i in [0, N)} (OR_{b in [0, W)} data[i*W + b])`。归约满足结合律，因此
结果与对 `%data` 全宽执行 `reduce_or/reduce_and/reduce_xor` 完全相同；lane 划分
只存在于 GRH `elemWidth` Attribute 的形状校验（`width(data) % elemWidth = 0`），不
作为 AM 指令 Attribute 保留。

#### 12.4.5 `array.broadcast`

GRH `kArrayBroadcast` lower 为标量广播：

```text
%res = array.broadcast %scalar
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%scalar` | `BV<W, SignS>` | 输入标量。 |
| `%res` | `BV<N*W, SignR>` | 广播结果。 |

对每个 `i in [0, N)`：`res[i*W +: W] = scalar`，即 `%res` 是 `%scalar` 的
`N = width(res) / width(scalar)` 次复制，与 `replicate` 的位级语义相同。结果宽度
必须是源宽度的整数倍。

#### 12.4.6 `array.onehot`

GRH `kArrayOnehot` lower 为索引译码：

```text
%res = array.onehot %x
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%x` | `BV<A, SignX>` | 索引；`A >= 1`，Signedness 被忽略。 |
| `%res` | `BV<N, SignR>` | one-hot 向量。 |

对每个 `i in [0, N)`：`res[i] = (U(x) = i)`，即 `(1 << U(x))` 截 `N` 位；
`U(x) >= N` 时 `%res` 为全 0。

#### 12.4.7 `array.reduce_lanes_or / array.reduce_lanes_and / array.reduce_lanes_xor`

GRH `kArrayReduceLanesOr/kArrayReduceLanesAnd/kArrayReduceLanesXor` lower 为逐 lane
归约：

```text
%res = array.reduce_lanes_or  %data
%res = array.reduce_lanes_and %data
%res = array.reduce_lanes_xor %data
```

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%data` | `BV<N*W, SignD>` | packed 输入数据。 |
| `%res` | `BV<N, unsigned>` | 逐 lane 归约出的守卫向量。 |

对每个 `i in [0, N)`：`res[i] = OR/AND/XOR_{b in [0, W)} data[i*W + b]`，即每个
lane 内独立归约、结果保留逐 lane 结构（与 `array.reduce_*` 的全数组 1-bit 归约
互补）。lane 宽度 `W = width(data) / width(res)` 由 Type 推出，`width(data)` 必须
是 `width(res)` 的整数倍；GRH `elemWidth` Attribute 的形状校验为
`width(res) * elemWidth = width(data)`。

#### 12.4.8 非指令：`kArrayLaneConst`

`kArrayLaneConst` 与 `kConstant` 一样不生成指令：lower 时按
`elemWidth/rows/values` Attribute 把 lane 常量表打包为 `rows*elemWidth` 位的
compile-time literal，`res[i*elemWidth +: elemWidth] = values[i]`，结果 Value 映射为
`Init = constant(packed)` 的 Variable。`values` 的长度必须等于 `rows`，每个
`values[i]` 必须非负且能用 `elemWidth` 位表示。

## 13. 非指令：层次与跨层引用

`kInstance` 描述电路实例关系，`kXMRRead/kXMRWrite` 描述通过层次路径定位的连接或状态
访问；它们都是电路结构，而不是运行时仿真行为，因此不对应任何 GRHSIM-AM opcode。
AM Program 没有 module/instance 层次树，也不在执行指令时解析路径。`kBlackbox` 同样
没有 opcode，但它不是可绑定的 AM 外部边界，而是 GRHSIM-AM 明确不支持的结构。

lower 到 AM 前必须完成：

- 完全展开 `kInstance`，把实例内的端口连接、Variable、Block 和状态行为内联到扁平
  Program；
- 运行 XMR resolve，把 `kXMRRead/kXMRWrite` 转成显式端口连接、普通赋值或对应状态单元
  的 read/write；
- 完成上述处理后重新建立依赖关系并分配 VarId/BlockId。

最终输入 AM lower 的 GRH 中残留 `kInstance` 或 `kXMRRead/kXMRWrite` 表示结构预处理未
完成，必须报告 lowering 错误；它们不能保留为未知指令，也不能在运行时通过字符串路径
补做解析。任何 `kBlackbox` 都必须无条件报告 lowering 错误，即使 HostEnvironment 或
集成层存在同名实现也不能绑定或保留它。若设计需要外部行为，上游 transform 必须先把
Blackbox 替换成明确受支持的普通逻辑/状态，或替换成 Program 级 `DpiImport` 和
`dpi.call`；无法完成显式改写时拒绝该设计。

## 14. 系统调用

### 14.1 `system.function`

GRH `kSystemFunction` lower 为：

```text
%res = system.function %arg0, ..., %argA-1 {
    has_side_effects = false | true, name = "functionName", schedule = normal | once | final
}
```

`system.function` 恰好产生一个 Result，所有 Operand 都是按源程序顺序排列的输入参数，
因此 `A >= 0` 且不需要参数计数 Attribute。

| Operand/Result | Type | 含义 |
| --- | --- | --- |
| `%arg0...A-1` | `BV<W, Sign>`、`Real` 或 `String` | 系统函数参数。 |
| `%res` | `BV<W, Sign>`、`Real` 或 `String` | 系统函数返回值。 |

参数快照按 [HostValue](grhsim-host-environment.md#1-hostvalue) 传递，不做隐式 Type 转换：
Real 保留完整 binary64 位模式，String 保留显式 byte 长度和内嵌 NUL。binding 可以接受
Real/String 参数，也可以返回 Real/String；返回 HostValue 的 Type 必须与 `%res` 完全
相同。后端不支持某个合法签名时必须在 Machine 创建期间报告 unsupported。

`name` 是不带前导 `$` 的非空 String，区分大小写。Machine 创建时必须按
`(name, argument Types, result Type)` 解析到唯一 host system-function binding；未找到、
重复或签名不匹配时 Program 非法。binding 定义具体函数的参数约束和返回值语义，例如
`clog2`、`time`、`random`、`sformatf`、`fopen` 或 `ferror`。AM 指令本身不根据名称猜测
返回 Type，也不允许未知函数静默返回零值。

执行时先读取全部参数快照，再调用：

```text
next = host_system_function(name, args, Type(res))
res  = next
```

调用正常返回后才写 `%res`；binding 报错或未返回时属于 Machine 执行错误，不能提交部分
结果。Result 可以与任一 Operand 使用相同 VarId，调用仍观察写回前的参数快照。
`system.function` 不隐式激活 Result 的消费者；需要传播实际变化时，在调用之后使用
`changed.any` 和 `act.f/act.b`。

`has_side_effects = true` 表示调用除返回 `%res` 外还可能修改 host 状态、进行 I/O 或产生
其他可观察效果。此时指令是显式调度边界，不能删除、合并、推测执行或跨越其他有副作用
指令重排。`has_side_effects = false` 只保证没有 Result 之外的可观察写效果，不保证返回值
是常量或只由参数决定；例如 host 时间或其他只读外部状态仍可能变化。除非具体 binding 另行
声明 deterministic/pure，后端不能仅凭该 Bool 做常量折叠、CSE 或跨有副作用调用移动。

`schedule` 决定调用阶段：

| `schedule` | 语义 |
| --- | --- |
| `normal` | 每次执行该指令都调用，并以新返回值写 `%res`。 |
| `once` | Machine 生命周期中第一次执行该指令时调用并设置该指令独占的 completed bit；后续执行不再调用，`%res` 保持第一次返回值。 |
| `final` | `eval()` 执行 Block 时跳过；Machine 的 `finalize()` 阶段按静态顺序调用一次并写 `%res`。 |

`once` 使用 ControlArea 中初始化为 0 的内部 completed bit，而不是读取 `FirstEval`。
`final` system function 只能被静态顺序更晚的 `final` system function/task 使用；不能把
它的 Result 提供给普通 eval 指令，因为该值在 `eval()` 期间尚未产生。final 调用按
`(BlockId, 指令文本位置)` 排序，并要求 lowering 同时满足其数据依赖顺序。

GRH lower 时，缺失的 `hasSideEffects` 按 false 处理并成为必需的
`has_side_effects` Attribute；`procKind = "initial"` 映射为 `schedule = once`，
`procKind = "final"` 映射为 `schedule = final`，其余值映射为 `schedule = normal`。
`hasTiming` 不保留；带 timing 的 initial function 所在 Block 仍由显式 event 激活，
`once` 保证它只在第一次实际执行时调用。`name` 去除且只去除一个源级前导 `$`。

例如：

```text
%count = system.function %value {
    has_side_effects = false, name = "clog2", schedule = normal
}
```

每次指令执行都会使用当时的 `%value` 调用 `clog2` binding，并把返回值写入 `%count`。

### 14.2 `system.task`

GRH `kSystemTask` lower 为：

```text
system.task %cond, %arg0, ..., %argA-1, %event0, ..., %eventE-1 {
    event_count = E, event_mode = immediate | pending,
    name = "taskName", schedule = normal | once | final
}
```

`system.task` 没有 Result。Operand 的前缀和尾部边界由必需的 `event_count = E` 唯一
确定：`%cond` 后的前 `A = operand_count - 1 - E` 个 Variable 是任务参数，最后 E 个是
已完成边沿检测的 event。要求 `A >= 0`、`E >= 0` 且 `operand_count >= 1 + E`。

| Operand | Type | 含义 |
| --- | --- | --- |
| `%cond` | `BV<1, SignC>` | 调用条件；Signedness 被忽略。 |
| `%arg0...A-1` | `BV<W, Sign>`、`Real` 或 `String` | 按源程序顺序传给系统任务的参数。 |
| `%event0...E-1` | `BV<1, unsigned>` | 已完成边沿检测的触发事件。 |

BV、Real 和 String 参数都按 [HostValue](grhsim-host-environment.md#1-hostvalue) 原样传递。
系统任务没有 Result；若任务需要产生 AM Value，必须建模为 `system.function`、DPI output
或其他显式写入指令。格式化、编码和文件输出如何解释 Real/String 由具体 task binding
定义，AM 不隐式把 String 当作 NUL 终止字符串。

`name` 是不带前导 `$` 的非空任务名，区分大小写。Machine 创建时必须为该名称解析到
唯一且接受给定参数 Type 的 host system-task binding；无法解析或签名不匹配时 Program
非法，不能把未知任务静默解释为 no-op。`system.task` 是有副作用的显式调度边界，后端
不能删除、合并、推测执行或跨越其他有副作用指令重排它。

执行该指令时先读取本次执行的全部 Operand 快照，并令：

```text
raw_event_hit = exists i in [0, E): U(event_i) = 1
fire_immediate = truth(cond) && ((E = 0) || raw_event_hit)
```

`event_mode = immediate` 直接使用 `fire_immediate`，不保存 raw event。display、assert、
difftest observer 等必须与当前 event 和当前参数快照绑定的调用使用这一模式；后续 round
即使 `%cond` 变为 true，也不能重放已经消失的 event。

`event_mode = pending` 且 `E > 0` 时，该指令独占一个初始化为 false 的内部
`pending_event` bit，并改为：

```text
pending_event = pending_event || raw_event_hit
fire_pending = truth(cond) && pending_event
```

多个 event 仍是 OR 关系；同一次执行中命中一个或多个 event 都只把同一个 bit 置为 true。
`pending_event` 可以跨同一次顶层 `eval()` 内的 AM round 保留，raw event 在后续 round
变为 0 不会清除它。每次顶层 `eval()` 开始前，Machine 清零所有此类 bit，因此 pending
event 不能跨两个顶层 `eval()` 调用传播。`E = 0` 时两种 mode 等价，均不分配或读取
`pending_event`。

`pending_event` 只保存“至少发生过一个事件”，不保存事件发生时的 `%cond` 或参数。
指令按 mode 选择 `fire = fire_immediate` 或 `fire = fire_pending`。`fire = false` 时不调用
binding；pending mode 在后续 round 消费 event 时，重新读取该次
指令执行时的 `%cond` 和参数快照。`fire = true` 时，以当前快照按顺序调用
`host_system_task(name, args)`；pending mode 调用成功后把该指令的 `pending_event` 清为
false。
binding 可以产生文本/文件 I/O、诊断、仿真结束请求等外部效果，具体任务的参数和效果由
binding 契约定义。效果按实际指令执行顺序可见；即使两次调用的名称和参数相同，也不得
合并。

`schedule` 决定调用阶段：

| `schedule` | 语义 |
| --- | --- |
| `normal` | 每次执行该指令都按上述 `fire` 判断。 |
| `once` | 在 Machine 生命周期中第一次 `fire = true` 时调用并把该指令的隐式 completed bit 置 1；completed 后不再调用。`fire = false` 不设置 completed。 |
| `final` | `eval()` 执行 Block 时跳过；Machine 的 `finalize()` 阶段忽略 event、只按当时的 `%cond` 判断一次并调用。要求 `E = 0`。 |

每条 `schedule = once` 的 `system.task` 独占一个初始化为 0 的内部 completed bit，它属于
ControlArea 而不是 Variable，没有 VarId。`finalize()` 在一个 Machine 生命周期中必须
幂等：final system function/task 按 `(BlockId, 指令文本位置)` 顺序至多执行一次。正常
销毁 Machine 前必须调用 `finalize()`；终止型 system task 若不返回，也必须先执行尚未
执行的 final 调用和必要的 host flush，再向宿主传播终止请求。

GRH lower 时：

- `eventEdge[i]` 与 raw `events[i]` 先 lower 成对应的 `changed.pos/changed.neg` 结果；
  `eventEdge` 不保留为 AM Attribute；
- `procKind = "final"` 映射为 `schedule = final`；`procKind = "initial"` 映射为
  `schedule = once`，无论 `hasTiming` 取值；其余过程种类映射为 `schedule = normal`；
- `hasTiming` 本身不保留；时序触发已由显式 event 和 `once` 的 completed 状态表达；
- `name` 去除且只去除一个源级前导 `$`，参数 Variable 保持源程序顺序；
- 当前 GRH lowering 对 `system.task` 选择 `event_mode = immediate`，避免旧 event 使用
  后续 round 的 task 参数重放副作用。

例如：

```text
system.task %true, %format, %data, %clkpos {
    event_count = 1, event_mode = immediate,
    name = "display", schedule = normal
}
```

仅当 `%true = 1` 且 `%clkpos = 1` 的本次执行才调用 `display`；该 event 不会在后续
round 重放。

## 15. DPI Call

Program 级 DpiImport 的结构、ABI 类型和 GRH lowering 由
[AM 基础设计第 2.3 节](grhsim-am.md#23-dpiimports) 定义。本节只定义可执行的
`dpi.call` 指令。

GRH `kDpicCall` lower 为：

```text
%return?, %out0, ..., %outO-1, %inoutOut0, ..., %inoutOutQ-1 =
    dpi.call %cond, %in0, ..., %inI-1, %inoutIn0, ..., %inoutInQ-1,
             %event0, ..., %eventE-1 {
                 event_count = E, event_mode = immediate | pending,
                 import = "symbol"
             }
```

若没有 Result，省略左侧和 `=`。`import` 必须解析到唯一 DpiImport。参数严格按声明顺序
过滤并分组：input Operand 在前，inout 输入侧 Operand 随后；Result 中可选 return 在
最前，output Result 随后，inout 输出侧 Result 最后。各组内部保持 DpiImport 中的形参
相对顺序。设声明分别有 I、O、Q 个 input、output、inout 参数，则 operand/result 数量
必须满足：

```text
operand_count = 1 + I + Q + E
result_count  = (return != none ? 1 : 0) + O + Q
```

`%cond` 是任意 Signedness 的单 bit BV；每个 event 必须是 `BV<1, unsigned>`。每个参数
Operand、Result 和可选 return Result 的 AM Type 必须与 DpiImport 对应项映射出的 Type
完全相同。`event_count = E` 为 Nat，且 `E >= 0`。

DPI 的 input、output、inout 和 return 都可以使用 Real 或 String；分组和写回顺序与 BV
完全相同。`AbiKind = real64/real32/string` 的转换由 HostEnvironment DPI adapter 执行：
real64 保留 binary64，real32 在调用边界舍入/扩展，String 必须保留 AM 的完整 byte
序列。无法满足某个 DpiImport ABI 的后端必须在 Machine 创建期间拒绝该 binding。

执行时使用与 `system.task` 相同的 `event_mode` 和 `fire` 计算。pending mode 同样只跨
一次顶层 `eval()` 内的 round 保留，并在成功调用后清零；pending bit 不保存事件发生时
的 cond 或参数，消费时读取当时的 cond、全部 input/inout 输入侧以及 event 快照。
`fire = false` 时不调用外部函数，也不写任何 Result；Result 保留执行前的值。
`fire = true` 时：

1. 以所有 input 快照作为只读实参，并以所有 inout 输入侧快照初始化独立的本地
   read-write 实参；output 使用按对应 AM Type 零值初始化的独立本地输出槽；
2. 按 DpiImport 的原始 parameter 顺序调用由 `symbol` 绑定的 host function；
3. 函数正常返回后，同时把 return、output 和 inout 本地值写入对应 Result。

因此 Operand 与 Result 可以复用 VarId，且同一个输入 Variable 可以传给多个形参；调用
始终读取调用前快照，写回不会影响本次调用的其他实参。Result VarId 必须两两不同。
外部函数异常、无法返回或 ABI 不匹配时属于 Machine 执行错误，不能提交部分 Result。

`dpi.call` 可能同时产生外部副作用和 Variable 结果，是显式调度边界，不得删除、合并、
推测执行或跨越其他有副作用指令重排。Result 写回本身不隐式激活消费者；需要传播实际
变化时，在调用之后使用 `changed.any` 和 `act.f/act.b`。同一 GRH
`gsim.external_instance_group` 中的 call 必须放入同一 AM 顺序域，并按
`gsim.external_call_ordinal` 升序执行；这些调度属性完成排序后不保留为 `dpi.call`
Attribute。

Lowering 使用 DpiImport 声明和 GRH `inArgName/outArgName/inoutArgName` 将各组重排为上述
规范顺序，验证 `hasReturn` 后不再保留这些 name list 或 `hasReturn`。raw event 和
`eventEdge` 按第 14.2 节规则 lower，`eventEdge` 不保留。当前 lowering 对产生 return、
output 或 inout Result 的调用选择 `event_mode = pending`，使值生产调用可以等待同一次
`eval()` 内的 guard 和参数收敛；无 Result 的 observer 调用选择
`event_mode = immediate`，避免旧 event 使用后续 round 的参数重放副作用。

例如声明 `add(input logic[31:0] a, input logic[31:0] b) -> logic[31:0]` 时：

```text
%sum = dpi.call %true, %a, %b, %clkpos {
    event_count = 1, event_mode = pending, import = "add"
}
```

声明 `swap(inout int a, inout int b) -> none` 时：

```text
%xout, %yout = dpi.call %true, %x, %y, %clkpos {
    event_count = 1, event_mode = pending, import = "swap"
}
```

无 Result 的 observer 使用 immediate mode，例如：

```text
dpi.call %valid, %pc, %clkpos {
    event_count = 1, event_mode = immediate, import = "observe_commit"
}
```

## 16. Block 激活

```text
act.f %event {targets = [TargetBlockId...]}
act.b %event {targets = [TargetBlockId...]}
```

`targets` 是静态确定的非空 BlockId set Attribute，记作 `Targets`；集合没有顺序和
重复元素，文本形式按 BlockId 升序规范化。普通 Block 分为 compute 段和 commit 段，
commit Block 占据 Block 空间的连续后缀，记其起点为 `CommitBegin`（无 commit Block
时 `CommitBegin = BlockCount`）。ActiveArea 为每个 Block 保存一个 `Active(b)` Bool
槽（单一集合，不分 current/next，也没有分层 summary；compute 与 commit Block 共用
同一激活位图，都只在被激活时执行，首次 `eval()` 激活全部 Block）；对 `Active(b)`
的写入是 opcode 的隐式效果，不是 `%` 操作数。

`%event` 必须是 `BV<1, unsigned>`，且必须由同一 Block 中排在 act 之前的指令写入，
保证每次执行 act 前都已刷新 event。event 为 1 时，`act.f`/`act.b` 都对每个
`b in Targets` 设置 `Active(b) = true`；`act.f` 的目标严格前向，置位在同一趟升序
扫描（compute 或 commit 阶段）内被消费；`act.b` 的目标按约定不大于源块、本轮已被
扫过，置位留给下一轮，因此任一 `act.b` 激发（event 为 1）即表示需要下一轮。event
为 0 时不产生激活。act 不写 VariableArea，也不维护
历史值。同一个 event 可以由同一 Block 中的多条 act 消费。

- `act.f` 可以出现在任意 Block；设指令所在 Block 为 B，其每个 Target 必须满足
  `B.BlockId < Target`，即严格前向——目标可以是更晚的 compute 或 commit Block。
  指向 commit Block 的 `act.f` 只允许携带被目标 commit Block 首部 gate detector
  watch 的变量（时钟源/派生时钟/latch nextValue），def-use 数据边不激活 commit
  Block；
- `act.b` 可以出现在任意非 EntryBlock，其每个 Target 必须满足 `1 <= Target`，即
  非 EntryBlock 的任意 Block；scheduler 只在 commit Block 尾部物化 `act.b`，且目标
  不大于源块（已扫过的 compute reader 或更早的 commit Block），置位留给下一轮；
- `Active(b)` 一旦置为 true，不因同一轮内后续 event 为 0 而清除；compute 与 commit
  阶段都按 BlockId 升序扫描、执行即清，其清除由 `eval()` 调度语义完成。

例如，同一输入变化需要激活 Block 3 和 5 时，一条 act 即可完成：

```text
%2 = changed.any %0, %1
act.f %2 {targets = [3, 5]}
```

若 `%0` 与 `%1` 不同，执行后 `Active(3) = Active(5) = true`，且 `%1` 等于 `%0`。

BlockId 0 的 EntryBlock 每次 `eval()` 开始时执行一次，可以包含 `changed`、用于派生
event 的第 3 至 10 节组合指令以及 `act.f`，不能包含 `act.b` 或其他有状态指令。
EntryBlock 中 act 使用的 event 必须在本次 EntryBlock 执行中先行计算。除
`FirstEval` 控制的首次全量激活外，`act.f/act.b` 是唯一的 Block 激活来源。每个
`act.f`-Target 对直接派生一条 Forward 依赖，每个 `act.b`-Target 对直接派生一条
Backward 依赖。

## 17. 合法性检查

加载 Program 时，对本稿指令至少验证：

- opcode、result/operand 数量和 Attribute schema 合法；Attribute 名称唯一、类型和
  取值满足对应 opcode 约束；
- 所有 `%` Result/Operand 均为 VariableArea 中的合法 VarId，不引用 ControlArea 或
  ActiveArea；组合指令引用的 Variable 类型为其要求的 `BV`；
- Result、Operand 和 read-write Operand 的宽度与 Signedness 满足各 opcode schema；
  同一 VarId 不被一条指令以两个不同值写回；
- constant Variable 只能作为只读 Operand，不能作为 Result 或 read-write Operand；
- 每个 GRH Logic Value 的 `width/isSigned` 与对应 Variable Type 完全一致；
- `changed.any` 的 new/old Type 完全相同；`changed.pos/changed.neg` 的 new/old 是
  Type 完全相同的单 bit BV；三者的 res 都是 `BV<1, unsigned>`；new、old、res 的
  VarId 两两不同；res 不是 constant 且使用空 Init，old 使用 `Init = undef`；两者分别
  只属于这一条 `changed`，old 不出现于其他指令；
- 每次 `eval()` 开始和每轮结束时，存在跨 Block 消费者的 `changed` res 均被清零；
  跨块消费的 res 其生产 Block 的 BlockId 必须小于消费 Block；act 使用的
  组合派生 event 在 act 所在 Block 中先行计算；
- `reg.write` 的 nextValue、target 和 event 数量及 Type 满足第 12.1 节；target 不是
  constant，且与每个 event 使用不同 VarId；
- `mem.read` 的 target/addr/res Type 满足第 12.2.1 节，res 不是 constant；
- `mem.write` 的 cond、addr、mask、data、target 和 event 数量及 Type 满足第 12.2.2 节，
  target 不是 constant；带 priority 的写组完整且按规定顺序 lower；
- `mem.fill` 的 data、target 和 event 数量及 Type 满足第 12.2.3 节；data 宽度恒等于
  数学整数 `N*W`（整片 packed 图像），target 不是 constant；
- `mem.read_all` 的 target/res Type 满足第 12.4.1 节，res 不是 constant；
- `mem.write_lanes` 的 laneMask、data、target 和 event 数量及 Type 满足第 12.4.2 节，
  target 不是 constant；带 priority 的写组完整且按规定顺序 lower；
- `array.mux/array.broadcast/array.onehot` 的 operand/result 宽度整除关系满足
  第 12.4 节；`array.reduce_*` 的 result 是 `BV<1, unsigned>`；
- `latch.write` 恰好两个 operand，nextValue 和 target Type 满足第 12.3 节，target
  不是 constant；若未运行 `latch-transparent-read`，每个 latch read 的透明旁路已由其他
  等价 GRH 组合逻辑显式表达；
- 输入 lowering 的 GRH 不包含 `kInstance/kXMRRead/kXMRWrite`，这些结构操作已按第 13 节
  展开或消解；输入也不包含任何 `kBlackbox`，且不得通过 HostEnvironment 或外部模型
  binding 绕过该限制；
- `system.function` 恰有一个非 constant Result，参数和 Result 都是 BV、Real 或 String，
  三个必需 Attribute、binding 签名和 schedule 满足第 14.1 节；`schedule = final` 的
  Result 只供静态顺序更晚的 final 调用使用；
- `system.task` 没有 Result，cond、参数、尾部 event、四个必需 Attribute 及
  `event_count` 分界满足第 14.2 节；`schedule = final` 时 event 数为 0；任务 binding
  唯一且签名匹配；
- `dpi.call` 的 import 可唯一解析到满足 AM 基础设计第 2.3 节的 DpiImport，参数分组、
  operand/result 数量、Type、尾部 event 和三个必需 Attribute 满足第 15 节，所有
  Result 非 constant 且 VarId 两两不同；同一有序调用组完整且按 ordinal 顺序 lower；
- `act.f/act.b` 的 event 是 `BV<1, unsigned>`，并由同一 Block 中更早的指令写入；
- `slice_static.lsb` 是满足第 10 节范围约束的 Nat，且该指令没有其他 Attribute；
- `targets` 是静态非空 BlockId set Attribute，且 act 没有其他 Attribute；commit Block
  构成 Block 空间的连续后缀 `[CommitBegin, BlockCount)`；state write
  （`reg.write/latch.write/mem.write/mem.fill/mem.write_lanes`）只位于 commit
  Block，且 commit Block 内每条 state write 之前至少有一条 `changed.*` gate
  detector；`act.f` 对所在 Block B 其每个 Target 满足 `B.BlockId < Target`（可指向
  更晚的 compute 或 commit Block）；`act.b` 只位于非 EntryBlock，其每个 Target
  满足 `1 <= Target`；
- EntryBlock 只包含 `changed`、event 派生所需的组合指令和 `act.f`；其中 `changed`
  的 new 不是 constant，也不作为任何普通 Block 指令的目标。
