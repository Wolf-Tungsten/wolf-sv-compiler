# GRHSIM-AM 指令集

> 状态：讨论稿 v0。本稿定义简单组合指令、变化检测、状态单元、外部调用和 Block 激活指令。

Program、Variable、Block 和理想存储见 [GRHSIM Abstract Machine 基础设计](grhsim-am.md)，
system/DPI 的宿主接口见 [HostEnvironment 参考定义](grhsim-host-environment.md)。

## 1. 范围

本文定义 `kConstant` 至 `kSliceArray` 对应 40 种 GRH 纯组合 Operation 的 Logic
形态：`kConstant` 直接成为 constant Variable，其余 39 种在 operand/result 均为
Logic 时产生本稿指令；Real/String constant 遵循同一规则。本文还定义
`changed.any/changed.pos/changed.neg`、`reg.write`、`mem.read/mem.write/mem.fill` 和
`latch.write`、`system.function/system.task`、`dpi.call` 和 `act.f/act.b`。层次结构和 XMR
不是 GRHSIM-AM 指令，必须在 lower 前消解。Real/String 专用组合指令仍待后续定义；
constant、`changed.any`、系统调用和 DPI 已按本文定义覆盖 Real/String。

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
| 状态写 | `%cond` 在最前；address 如有则紧随其后；然后是 `%mask`、payload、`%target`；variadic event 始终位于末尾。 |
| 外部调用 | `system.function` 的 Operand 全是参数；有条件的 `system.task/dpi.call` 把 `%cond` 放在最前。输入参数按调用 schema 排列，variadic event 始终位于末尾；返回值和 output/inout 参数写在 Result 列表。 |
| Block 激活 | `%event` 是唯一 Operand，目标 Block 集合放在 `targets` Attribute。 |

因此当前状态写的规范排列为：

```text
reg.write   %cond, %mask, %nextValue, %target, %event0, ...
mem.write   %cond, %addr, %mask, %nextValue, %target, %event0, ...
mem.fill    %cond, %data, %target, %event0, ...
latch.write %cond, %mask, %nextValue, %target
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
| `reg.write` | - | `%cond, %mask, %nextValue, %target (rw), %event0, ..., %eventE-1` | - |
| `mem.read` | `%res` | `%target, %addr` | - |
| `mem.write` | - | `%cond, %addr, %mask, %nextValue, %target (rw), %event0, ..., %eventE-1` | - |
| `mem.fill` | - | `%cond, %data, %target (rw), %event0, ..., %eventE-1` | - |
| `latch.write` | - | `%cond, %mask, %nextValue, %target (rw)` | - |
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

`%res` 表示当前 epoch 观察到的事件。Machine 在每次 `eval()` 开始、执行 EntryBlock
前，以及进入每个后续 epoch 前，将所有 `changed` 的 `%res` 清零；执行 `changed` 后，
结果保持到当前 epoch 结束，因此可以作为同一 epoch 后续 Block 中 `reg.write`、
`mem.write` 或 `mem.fill` 的 event。
由普通组合指令派生的 event 没有这种隐式清零语义，遵循普通 Variable 的读写规则。
`act.f/act.b` 必须与产生其 event 的 `changed` 或组合指令位于同一 Block。后端可以
融合相邻的 `changed` 和 act，且不物化 `%res`。

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
reg.write %cond, %mask, %nextValue, %target, %event0, ..., %eventE-1
```

各 operand 定义如下：

下表中的 `Sign` 和 `SignM` 分别取 `signed` 或 `unsigned`，两者不要求相同。

| Operand | Type | 含义 |
| --- | --- | --- |
| `%cond` | `BV<1, Sign>` | 写使能；Signedness 被忽略，为 1 时允许更新。 |
| `%mask` | `BV<W, SignM>` | 逐 bit 写掩码；Signedness 被忽略，bit `i` 为 1 时更新 target bit `i`。 |
| `%nextValue` | 与 `%target` 完全相同 | 被选中的 bit 使用的新值。 |
| `%target` | `BV<W, Sign>` | 对应 `kRegister` 的可写 Variable，同时是本指令的读写目标。 |
| `%event0...E-1` | `BV<1, unsigned>` | 已完成边沿检测的触发事件；`E >= 1`。 |

指令先同时读取全部 operand 的原值，再令：

```text
fire = (U(cond) = 1) && exists i in [0, E): U(event_i) = 1
```

若 `fire = false`，`%target` 保持不变；若 `fire = true`，对每个 `i in [0, W)`
一次写回：

```text
target'[i] = mask[i] ? nextValue[i] : target[i]
```

`%target` 必须与所有 event 使用不同 VarId；它可以在 Type 允许时与
`%cond/%mask/%nextValue` 使用相同 VarId，上述“先读后写”规则保证别名时仍使用指令
开始时读到的值。`reg.write` 不产生 result，目标 Signedness 不参与 mask 选择。
`reg.write` 也不隐式激活读取 `%target` 的 Block；lower 必须在本次最终写回之后使用
`changed.any %target, %targetOld` 检测实际状态变化，再通过 `act.f/act.b` 传播。

GRH `eventEdge[i]` 与 `events[i]` 在 lower 时先转换成 event：`posedge` 使用
`changed.pos`，`negedge` 使用 `changed.neg`。多个 event 采用 OR 触发，即任一 event
为 1 即满足 `fire` 的事件部分。event 列表只决定是否触发，不表示事件优先级，也不根据
触发来源选择 nextValue；同步/异步 reset、enable 等优先级必须由 `%cond/%nextValue`
上游的 mux 显式编码。例如：

```text
reg.write %writeCond, %allMask, %selectedNext, %q, %clkpos, %rstneg
```

`%clkpos` 或 `%rstneg` 任一个为 1 都满足事件条件，最终是否写以及写入 data 仍分别由
`%writeCond` 和 `%selectedNext` 决定。同一原始信号同时需要 posedge 和 negedge 时，
可以按第 11 节示例只执行一次 `changed.any`，再在 `reg.write` 所在 Block 中根据当前
单 bit 值派生 `%event`。

例如：

```text
B0:
    %clkchanged = changed.any %clk, %oldclk
    act.f %clkchanged {targets = [4]}

B4:
    %clkpos = logic_and %clkchanged, %clk
    reg.write %en, %wmask, %d, %q, %clkpos
    %qchanged = changed.any %q, %qold
    act.f %qchanged {targets = [5]}
```

这等价于在 `%clk` 上升沿且 `%en = 1` 时执行逐 bit masked write；下降沿虽然会激活 B4，
但 `%clkpos = 0`，因此不会写 `%q`。只有 `%q` 的最终位模式实际变化时才激活 B5。

同一 `%target` 的多条 `reg.write` 按 `(EpochCounter, BlockId, 指令文本位置)` 确定的实际
执行顺序生效。若多条指令在同一次 `eval()` 中触发，后执行者读取前一条写后的 target；
重叠 mask bit 由后执行者覆盖。需要与源设计一致的优先级时，lower 必须通过 Block
顺序、互斥 cond 或预先合并 nextValue/mask 显式编码，不得依赖 GRH Operation 的原始
遍历顺序。同一 target 的候选写应排在一次共享的 `changed.any %target, %targetOld`
之前，由这一个 detector 在最终写回后统一激活 register readers，不能为每条写分别创建
target old。

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
mem.write %cond, %addr, %mask, %nextValue, %target, %event0, ..., %eventE-1
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%cond` | `BV<1, SignC>` | 写使能；Signedness 被忽略。 |
| `%addr` | `BV<A, SignA>` | 写地址；`A >= 1`，Signedness 被忽略。 |
| `%mask` | `BV<W, SignM>` | 逐 bit 写掩码；Signedness 被忽略。 |
| `%nextValue` | `BV<W, Sign>` | 写数据，与 memory 元素 Type 完全相同。 |
| `%target` | `Array<N, BV<W, Sign>>` | 被写入的 memory Variable。 |
| `%event0...E-1` | `BV<1, unsigned>` | 已完成边沿检测的触发事件；`E >= 1`。 |

`Sign`、`SignC`、`SignA` 和 `SignM` 分别取 `signed` 或 `unsigned`。指令先同时读取全部
operand 的原值，再令：

```text
fire = (U(cond) = 1)
       && (exists i in [0, E): U(event_i) = 1)
       && (U(addr) < N)
```

若 `fire = false`，整个 `%target` 保持不变。若 `fire = true`，只更新地址
`r = U(addr)` 对应的 row：

```text
target'[r][j] = mask[j] ? nextValue[j] : target[r][j]  for j in [0, W)
target'[k]    = target[k]                               for k != r
```

因此越界写不更新任何 row。`mem.write` 不产生 result，也不隐式激活读取 `%target` 的
Block；需要传播实际 memory 变化时，lower 使用 `changed.any` 检测最终 Array Value，
再通过 `act.f/act.b` 激活相关 reader。event 的 OR 语义、eventEdge lowering 以及上游
cond/nextValue 优先级编码与 `reg.write` 相同。

例如：

```text
mem.write %wen, %waddr, %wmask, %wdata, %mem, %clkpos
```

仅当 `%wen = 1`、`%clkpos = 1` 且 `U(waddr) < N` 时更新选中 row；mask 为 0 的 bit
保持原值。

同一 memory 的多条 `mem.write` 按实际指令执行顺序生效。地址不同的写分别保留；地址
相同且 mask 重叠时，后执行者覆盖重叠 bit。带 GRH `memoryWrite.priorityGroup/priority`
的写端口必须保持在同一顺序域，并按 priority 从大到小 lower，使 priority 0 最后执行。
没有 priority 属性的写端口不能从 GRH Operation 遍历顺序推导源级碰撞优先级。
同一 target 的 `mem.read` 和 `mem.write` 也按实际执行顺序观察状态：read 在 write 前
执行时读取旧 row，在 write 后执行时读取更新后的 row。同一 target 的全部候选写之后
只需一次共享的 `changed.any` 检测最终 Array Value 并传播变化。

#### 12.2.3 `mem.fill`

GRH `kMemoryFillPort` lower 为：

```text
mem.fill %cond, %data, %target, %event0, ..., %eventE-1
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%cond` | `BV<1, SignC>` | Fill 使能；Signedness 被忽略。 |
| `%data` | `BV<W, SignD>` 或 `BV<N*W, SignD>` | 广播 row 值或 LSB-row-first packed memory 值；Signedness 被忽略。 |
| `%target` | `Array<N, BV<W, Sign>>` | 被整体写入的 memory Variable。 |
| `%event0...E-1` | `BV<1, unsigned>` | 已完成边沿检测的触发事件；`E >= 1`。 |

`Sign`、`SignC` 和 `SignD` 分别取 `signed` 或 `unsigned`。`N*W` 使用数学整数计算，
不得发生宿主整数溢出。指令先同时读取全部 operand，再令：

```text
fire = (U(cond) = 1) && exists i in [0, E): U(event_i) = 1
```

若 `fire = false`，`%target` 保持不变。若 `fire = true`：

- `width(data) = W` 时，把 `%data` 的 W-bit 位模式广播到所有 row；
- `width(data) = N*W` 时，row `r` 读取
  `data[r * W + W - 1 : r * W]`，即 row 0 位于 packed data 的最低 W bit。

形式化地，对每个 `r in [0, N)`、`j in [0, W)`：

```text
target'[r][j] = width(data) = W ? data[j] : data[r * W + j]
```

当 `N = 1` 时两种 data 宽度相同，两种解释产生相同结果。`mem.fill` 不产生 result，
也不隐式激活 memory reader；实际 Array Value 的变化仍由最终写回后的共享
`changed.any` 检测。event 的 OR 语义和 eventEdge lowering 与 `reg.write/mem.write`
相同。

例如，`%mem` 为 `Array<4, BV<8, unsigned>>`：

```text
mem.fill %en, %byte,   %mem, %rstneg  // 4 个 row 都写入 byte
mem.fill %en, %packed, %mem, %rstneg  // row r 写入 packed[8*r +: 8]
```

`mem.fill` 与同一 target 的 `mem.write/mem.read` 按实际指令执行顺序观察状态。fill 后的
write 可以覆盖选中 row 的部分 bit；write 后的 fill 会覆盖整个 memory。需要确定的
源级优先级必须由 lower 后的指令顺序显式表达。

### 12.3 Latch

每个 GRH `kLatch` lower 为一个可写的 `BV<W, Sign>` Variable，记作 `%target`。Variable
的宽度和 Signedness 来自 `kLatch.width/isSigned`，并使用 `Init = undef`。
`kLatchReadPort` 直接映射到该 VarId，对 stored latch value 的读取不生成指令。

GRH `kLatchWritePort` lower 为：

```text
latch.write %cond, %mask, %nextValue, %target
```

| Operand | Type | 含义 |
| --- | --- | --- |
| `%cond` | `BV<1, SignC>` | Latch 透明/写使能条件；Signedness 被忽略。 |
| `%mask` | `BV<W, SignM>` | 逐 bit 写掩码；Signedness 被忽略。 |
| `%nextValue` | 与 `%target` 完全相同 | 被选中 bit 的新值。 |
| `%target` | `BV<W, Sign>` | 对应 `kLatch` 的可写 Variable，同时是读写目标。 |

`Sign`、`SignC` 和 `SignM` 分别取 `signed` 或 `unsigned`。指令先同时读取全部 operand。
若 `U(cond) = 0`，`%target` 保持不变；若 `U(cond) = 1`，则对每个
`i in [0, W)` 一次写回：

```text
target'[i] = mask[i] ? nextValue[i] : target[i]
```

`%target` 可以在 Type 允许时与 `%cond/%mask/%nextValue` 使用相同 VarId，结果仍基于
指令开始时读取的值。`latch.write` 不产生 result，也不隐式激活读取 `%target` 的
Block；最终 stored value 的实际变化由共享的 `changed.any %target, %targetOld` 检测并
传播。与带 event 的状态写不同，`latch.write` 是 level-sensitive：任何一次 Block
执行都会按当时的 `%cond` 判断是否更新。

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
latch.write %gate, %allMask, %d, %q
%qchanged = changed.any %q, %qold
act.f %qchanged {targets = [5]}
```

每次执行时，`%gate = 1` 使 `%q` 更新为 `%d`，`%gate = 0` 使 `%q` 保持。只有 stored
`%q` 的位模式实际变化时才激活 B5。

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
difftest observer 等必须与当前 event 和当前参数快照绑定的调用使用这一模式；后续 epoch
即使 `%cond` 变为 true，也不能重放已经消失的 event。

`event_mode = pending` 且 `E > 0` 时，该指令独占一个初始化为 false 的内部
`pending_event` bit，并改为：

```text
pending_event = pending_event || raw_event_hit
fire_pending = truth(cond) && pending_event
```

多个 event 仍是 OR 关系；同一次执行中命中一个或多个 event 都只把同一个 bit 置为 true。
`pending_event` 可以跨同一次顶层 `eval()` 内的 AM epoch 保留，raw event 在后续 epoch
变为 0 不会清除它。每次顶层 `eval()` 开始前，Machine 清零所有此类 bit，因此 pending
event 不能跨两个顶层 `eval()` 调用传播。`E = 0` 时两种 mode 等价，均不分配或读取
`pending_event`。

`pending_event` 只保存“至少发生过一个事件”，不保存事件发生时的 `%cond` 或参数。
指令按 mode 选择 `fire = fire_immediate` 或 `fire = fire_pending`。`fire = false` 时不调用
binding；pending mode 在后续 epoch 消费 event 时，重新读取该次
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
  后续 epoch 的 task 参数重放副作用。

例如：

```text
system.task %true, %format, %data, %clkpos {
    event_count = 1, event_mode = immediate,
    name = "display", schedule = normal
}
```

仅当 `%true = 1` 且 `%clkpos = 1` 的本次执行才调用 `display`；该 event 不会在后续
epoch 重放。

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
一次顶层 `eval()` 内的 epoch 保留，并在成功调用后清零；pending bit 不保存事件发生时
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
`event_mode = immediate`，避免旧 event 使用后续 epoch 的参数重放副作用。

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
重复元素，文本形式按 BlockId 升序规范化。`Active(b)` 和 `NextEpochActive(b)` 是基础
设计定义的 ActiveArea 槽位别名；对它们的写入是 opcode 的隐式效果，不是 `%` 操作数。

`%event` 必须是 `BV<1, unsigned>`，且必须由同一 Block 中排在 act 之前的指令写入，
保证每次执行 act 前都已刷新 event。event 为 1 时，`act.f` 对每个
`b in Targets` 设置 `Active(b) = true`，`act.b` 对每个 `b in Targets` 设置
`NextEpochActive(b) = true`；event 为 0 时不产生激活。act 不写 VariableArea，也不维护
历史值。同一个 event 可以由同一 Block 中的多条 act 消费。

- 设指令所在 Block 为 B：`act.f` 的每个 Target 必须满足
  `B.BlockId < Target < BlockCount`；`act.b` 的每个 Target 必须满足
  `1 <= Target < BlockCount`；
- 对应 Bool 槽一旦置为 true，不因同一 epoch 内后续 event 为 0 而清除；其清除由
  `eval()` 调度语义完成。

例如，同一输入变化需要激活 Block 3 和 5 时，一条 act 即可完成：

```text
%2 = changed.any %0, %1
act.f %2 {targets = [3, 5]}
```

若 `%0` 与 `%1` 不同，执行后 `Active(3) = Active(5) = true`，且 `%1` 等于 `%0`。

BlockId 0 的 EntryBlock 每次 `eval()` 开始时执行一次，可以包含 `changed`、用于派生
event 的第 3 至 10 节组合指令以及 `act.f`，不能包含 `act.b` 或其他有状态指令。
EntryBlock 中 act 使用的 event 必须在本次 EntryBlock 执行中先行计算。除
`S[FirstEvalId]` 控制的首次全量激活外，`act.f/act.b` 是唯一的 Block 激活来源。每个
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
- epoch 0 开始前和每次进入后续 epoch 前，所有 `changed` 的 res 均被清零；act 使用的
  组合派生 event 在 act 所在 Block 中先行计算；
- `reg.write` 的 cond、mask、nextValue、target 和 event 数量及 Type 满足第 12.1 节；
  target 不是 constant，且与每个 event 使用不同 VarId；
- `mem.read` 的 target/addr/res Type 满足第 12.2.1 节，res 不是 constant；
- `mem.write` 的 cond、addr、mask、nextValue、target 和 event 数量及 Type 满足
  第 12.2.2 节，target 不是 constant；带 priority 的写组完整且按规定顺序 lower；
- `mem.fill` 的 cond、data、target 和 event 数量及 Type 满足第 12.2.3 节；data 宽度
  等于 W 或数学整数 `N*W`，target 不是 constant；
- `latch.write` 的 cond、mask、nextValue 和 target Type 满足第 12.3 节，target 不是
  constant；若未运行 `latch-transparent-read`，每个 latch read 的透明旁路已由其他
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
- `targets` 是静态非空 BlockId set Attribute，且 act 没有其他 Attribute；对所在 Block B，`act.f` 的每个 Target 满足
  `B.BlockId < Target < BlockCount`，`act.b` 的每个 Target 满足
  `1 <= Target < BlockCount`；
- EntryBlock 只包含 `changed`、event 派生所需的组合指令和 `act.f`；其中 `changed`
  的 new 不是 constant，也不作为任何普通 Block 指令的目标。
