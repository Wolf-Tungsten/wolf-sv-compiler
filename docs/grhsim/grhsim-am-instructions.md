# GRHSIM-AM 指令集

> 状态：讨论稿 v0。本稿定义简单组合指令和 Block 激活指令。

Program、Variable、Block 和理想存储见 [GRHSIM Abstract Machine 基础设计](grhsim-am.md)。

## 1. 范围

本文定义 `kConstant` 至 `kSliceArray` 对应 40 种 GRH 纯组合 Operation 的 Logic
形态：`kConstant` 直接成为 constant Variable，其余 39 种在 operand/result 均为
Logic 时产生本稿指令；Real/String constant 遵循同一规则。本文还定义 `actf/actb`。
状态单元和层次指令待定义。Real/String 专用组合指令、SystemFunction、SystemTask 和
DPI 均属于 GRHSIM-AM，但其具体指令留待后续定义；constant 和通用 act 已按本文
定义覆盖 Real/String。

## 2. 公共语义

普通指令形式为：

```text
opcode %dst, %src... [, immediate...]
```

`%n` 表示 `[0, VariableCount)` 中的 VarId `n`。控制区位于 Variable 之后，没有 VarId，
不能写成普通指令的 `%n` 操作数。第 3 至 10 节组合指令的所有 `%dst` 和 `%src` 都必须
是 `BV<W, S>`；Array 不参与这些指令。每条组合指令先读取全部源值和立即数，再计算并
一次写回目标，因此目标可以与任一源使用相同 VarId，但目标不能是 constant Variable。

记：

```text
U_W(x)   = x 的 W-bit 无符号整数值
S_W(x)   = x 的 W-bit 二进制补码整数值
low_W(n) = 整数 n 模 2^W 后的 W-bit 位模式
truth(x) = (U_W(x) != 0)
```

无下标的 `U(x)` 表示按 `width(x)` 求无符号整数值；`trunc0` 表示向 0 取整。

```text
commonS(a, b) = signed    if type(a).S = signed and type(b).S = signed
                unsigned  otherwise
```

`resize(x, W, S)` 产生 W-bit 位模式：缩小时保留最低 W bit；扩大时，`S=signed`
复制源最高位，`S=unsigned` 补 0。

所有宽度和下标计算使用数学整数，不允许发生宿主整数溢出。第 3 至 10 节的每个组合
opcode 都是全函数，边界行为不得继承宿主语言的未定义行为。

### 2.1 GRH lower 规则

每个 GRH Logic Value 都映射为同宽、同 Signedness 的 AM `BV<W, S>` Variable。指令
根据 operand Type 自行完成下表规定的共同类型转换，不需要为 Signedness 选择不同
opcode。

| GRH kind | 计算宽度 W | 计算 signedness C | 结果 Type |
| --- | --- | --- | --- |
| `kAdd/kSub/kAnd/kOr/kXor/kXnor` | `max(width(L), width(R))` | `commonS(L, R)` | `BV<W, C>` |
| `kMul` | `width(L) + width(R)` | `commonS(L, R)` | `BV<W, C>` |
| `kDiv/kMod` | `width(L)` | `commonS(L, R)` | `BV<W, C>` |
| equality 与关系比较 | `max(width(L), width(R))` | `commonS(L, R)` | `BV<1, unsigned>` |
| `kMux` | result 宽度 | `commonS(True, False)` | `BV<W, C>` |

表中二元 operand 都先执行 `resize(operand, W, C)`。此外：

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
- 第 3 至 10 节的 opcode 不接受 Real 或 String operand/result；这些类型的组合形态、
  系统调用和 DPI 由对应的待定义指令承接。

`kConstant` 必须在 Block 构造前完成上述 lower；移除它后为空的候选 Block 不生成，
随后再稠密分配 BlockId 并确定 act 的 Targets。消费者直接以 constant Variable 的
VarId 作为源。

## 3. 赋值

| Opcode | 对应 GRH | 形式 | 语义与约束 |
| --- | --- | --- | --- |
| `assign` | `kAssign` | `assign %d, %s` | 调整位宽后写入；目标 Signedness 不改变位模式。 |

设源宽 `Ws`、目标宽 `Wd`：

- `Wd = Ws`：原样复制；
- `Wd < Ws`：保留源的最低 Wd bit；
- `Wd > Ws`：源 Type 为 signed 时复制源 bit `Ws-1`，否则补 0。

## 4. 算术运算

以下指令形式均为 `opcode %d, %a, %b`。令计算宽度和 signedness 为 2.1 节定义的
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

`and/or/xor/xnor` 形式为 `opcode %d, %a, %b`。二元 operand 按 2.1 节得到 `W`、
`C`，令 `a' = resize(a, W, C)`、`b' = resize(b, W, C)`，并对 `a'`、`b'` 逐位
运算；目标必须是 `BV<W, C>`。`not %d, %a` 要求目标与源 Type 相同。

| Opcode | 对应 GRH | 结果 |
| --- | --- | --- |
| `and` | `kAnd` | 逐位 `a' & b'` |
| `or` | `kOr` | 逐位 `a' \| b'` |
| `xor` | `kXor` | 逐位 `a' ^ b'` |
| `xnor` | `kXnor` | 逐位 `~(a' ^ b')`，只保留 W bit |
| `not` | `kNot` | 逐位 `~a`，只保留 W bit |

## 6. 比较运算

形式均为 `opcode %d, %a, %b`。两个 operand 按 2.1 节规整为 `a'`、`b'`，计算宽度
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

逻辑指令的目标均为 `BV<1, unsigned>`；源可以具有任意宽度和 Signedness：

| Opcode | 对应 GRH | 形式 | 结果 |
| --- | --- | --- | --- |
| `logic_and` | `kLogicAnd` | `logic_and %d, %a, %b` | `truth(a) && truth(b)` |
| `logic_or` | `kLogicOr` | `logic_or %d, %a, %b` | `truth(a) \|\| truth(b)` |
| `logic_not` | `kLogicNot` | `logic_not %d, %a` | `!truth(a)` |

规约指令形式为 `opcode %d, %a`，目标为 `BV<1, unsigned>`，源为任意
`BV<W, S>`：

| Opcode | 对应 GRH | 结果 |
| --- | --- | --- |
| `reduce_and` | `kReduceAnd` | 所有位均为 1 |
| `reduce_nand` | `kReduceNand` | `reduce_and` 的反值 |
| `reduce_or` | `kReduceOr` | 至少一位为 1 |
| `reduce_nor` | `kReduceNor` | `reduce_or` 的反值 |
| `reduce_xor` | `kReduceXor` | 1 bit 的个数为奇数 |
| `reduce_xnor` | `kReduceXnor` | `reduce_xor` 的反值 |

## 8. 移位

形式均为 `opcode %d, %value, %amount`。`%d` 与 `%value` 的 Type 必须完全相同；
`%amount` 可为任意 `BV<A, S>`，始终按 `U(amount)` 解释，其 Signedness 被忽略。

| Opcode | 对应 GRH | `U(amount) < W` | `U(amount) >= W` |
| --- | --- | --- | --- |
| `shl` | `kShl` | 左移并保留最低 W bit | 0 |
| `lshr` | `kLShr` | 逻辑右移，高位补 0 | 0 |
| `ashr` | `kAShr` | value 为 signed 时复制 bit `W-1`，否则补 0 | value 为 signed 时复制 bit `W-1`，否则为 0 |

## 9. 选择与数据重组

| Opcode | 对应 GRH | 形式 | 语义与约束 |
| --- | --- | --- | --- |
| `mux` | `kMux` | `mux %d, %cond, %t, %f` | `%cond` 为任意 Signedness 的 `BV<1, S>`；分支按 result 宽度和 `commonS(t,f)` resize，目标 Type 与结果一致；条件为 1 取 `%t`，否则取 `%f`。 |
| `concat` | `kConcat` | `concat %d, %s0, ..., %sN-1` | `N >= 1`；目标为 `BV<sum(width(si)), unsigned>`；`%s0` 位于最高位。 |
| `replicate` | `kReplicate` | `replicate %d, %s` | 目标为 unsigned BV，且其宽度是 `width(s)` 的正整数倍；按该倍数重复拼接 `%s`。 |

lower `kReplicate` 时必须验证 `rep = width(d) / width(s)`。

## 10. 切片

| Opcode | 对应 GRH | 形式 | 约束 |
| --- | --- | --- | --- |
| `slice_static` | `kSliceStatic` | `slice_static %d, %base, Start` | `%d` 必须为 unsigned；`Start >= 0` 且 `Start + width(d) <= width(base)`。 |
| `slice_dynamic` | `kSliceDynamic` | `slice_dynamic %d, %base, %start` | `%d` 必须为 unsigned；`%start` 忽略 Signedness 并按 `U(start)` 解释。 |
| `slice_array` | `kSliceArray` | `slice_array %d, %base, %index` | `%d` 必须为 unsigned 且 `width(d)` 整除 `width(base)`；`%index` 忽略 Signedness 并按 `U(index)` 解释。 |

`slice_static` 返回 `base[Start + width(d) - 1 : Start]`。另外两条指令对目标 bit `j`
分别读取：

```text
slice_dynamic: base[U(start) + j]
slice_array:   base[U(index) * width(d) + j]
```

源下标超出 `base` 时该目标 bit 为 0。乘加下标使用数学整数，不得因宿主溢出回绕。
`slice_array` 操作 packed BV，不是对 AM `Array` 的访存。

lower 时，`kSliceStatic.sliceStart` 成为 `Start`，并验证
`sliceEnd = Start + width(d) - 1`；`kSliceDynamic/kSliceArray.sliceWidth` 必须等于
`width(d)`。

例如 `%0` 为值 `4'b1111` 的 `BV<4, signed>`，`%1` 为 `BV<8, unsigned>`；`%2`
为相同位模式的 `BV<4, unsigned>`，`%3` 为 `BV<8, signed>`：

```text
assign %1, %0  -> %1 = 8'hFF  // 源为 signed，符号扩展
assign %3, %2  -> %3 = 8'h0F  // 源为 unsigned，零扩展
```

## 11. 状态单元

待定义：`kLatch/kLatchReadPort/kLatchWritePort`、
`kRegister/kRegisterReadPort/kRegisterWritePort`、
`kMemory/kMemoryReadPort/kMemoryWritePort/kMemoryFillPort`。

## 12. 层次与跨层引用

待定义：`kInstance/kBlackbox`、`kXMRRead/kXMRWrite`。

## 13. 系统调用

属于 GRHSIM-AM，待定义：`kSystemFunction/kSystemTask`。操作数和结果可使用 BV、Real
或 String。

## 14. DPI

属于 GRHSIM-AM，待定义：`kDpicImport/kDpicCall`。

## 15. Block 激活

```text
actf %new, %old, {TargetBlockId...}
actb %new, %old, {TargetBlockId...}
```

`{TargetBlockId...}` 是静态确定的非空 BlockId 集合，记作 `Targets`；集合没有顺序和
重复元素。`Active(b)` 和 `NextEpochActive(b)` 是基础设计定义的 M 控制槽别名；对它们
的写入是 opcode 的隐式效果，不是 `%` 操作数。

两条指令同时读取 `%new` 和 `%old`，要求二者 Type 完全相同，并使用基础设计中定义的
`sameValue` 比较完整 Value：

- Value 不同时，`actf` 对每个 `b in Targets` 设置 `Active(b) = true`，`actb` 对每个
  `b in Targets` 设置 `NextEpochActive(b) = true`；Value 相同时不产生激活；
- 完成比较和激活后，两条指令都无条件执行 `%old = %new`。源 Value 在写 old 前已经
  全部读出；old 的这次写入本身不产生额外激活；
- 设指令所在 Block 为 B：`actf` 的每个 Target 必须满足
  `B.BlockId < Target < BlockCount`；`actb` 的每个 Target 必须满足
  `1 <= Target < BlockCount`；
- act 可位于 Block 的任意位置，比较该指令执行点读到的 Value。对应 Bool 槽一旦置为
  true，不因后续 Value 再次变化而清除；其清除由 `eval()` 调度语义完成；
- 每条 act 独占一个 old Variable，且无论 Targets 包含多少 Block 都只比较并更新这一个
  old；new 和 old 必须是不同 VarId。因此 AM 语义中的 old 数量按 act 数量计算；后端
  可在保持行为时合并物理存储和比较；
- old 只记录所属 act 的变动检测基线，不得出现在其他指令中，也不得由外部写入。实例
  完成 Init 后先统一执行 `old = new`；
- 仿真边沿检测必须使用不同的 Variable，具体边沿检测 opcode 待定义。

例如，同一输入变化需要激活 Block 3 和 5 时，一条 act 即可完成：

```text
actf %0, %1, {3, 5}
```

执行后 `Active(3) = Active(5) = true`，且 `%1` 等于 `%0`。

BlockId 0 的 EntryBlock 每次 `eval()` 开始时执行一次且只包含 `actf`，由其中的 act
设置 `Active` 区中的相应 Bool 槽，形成初始激活状态。除 `M[FirstEvalId]` 控制的首次
全量激活外，`actf/actb` 是唯一的 Block 激活来源。每个 `actf`-Target 对直接派生一条
Forward 依赖，每个 `actb`-Target 对直接派生一条 Backward 依赖。

## 16. 合法性检查

加载 Program 时，对本稿指令至少验证：

- opcode、源数量、立即数数量和立即数取值合法；
- 所有 `%` 操作数均为 `[0, VariableCount)` 中的合法 VarId，不引用控制区；组合指令
  引用的 Variable 类型为其要求的 `BV`；
- 组合指令目标、源和结果的宽度与 Signedness 满足各指令约束；
- constant Variable 只能作为源，不能作为任何 opcode 的目标；
- 每个 GRH Logic Value 的 `width/isSigned` 与对应 Variable Type 完全一致；
- `actf/actb` 的 new/old 是不同 VarId 且 Type 完全相同；old 不是 constant、使用空
  Init、只属于这一条 act，且不出现于其他指令；
- Targets 是静态非空 BlockId 集合；对所在 Block B，`actf` 的每个 Target 满足
  `B.BlockId < Target < BlockCount`，`actb` 的每个 Target 满足
  `1 <= Target < BlockCount`；
- EntryBlock 只包含 `actf`，不包含 `actb` 或其他 opcode；其中 new 不是 constant，
  也不作为任何指令的目标。
