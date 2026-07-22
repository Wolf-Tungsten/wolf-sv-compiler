# GRHSIM-AM lowering、调度与 C++ emit 流水线

本文定义 GRHSIM-AM 新流水线的代码框架和分阶段落地边界。它是实现设计，不是
[GRHSIM-AM 规范](grhsim-am.md)的语义修订。规范中的 `Program` 始终表示可以交给
`Machine` 执行、已经具有 `B0`/`B1+`、`changed`/`act` 和完整 `eval()` 语义的最终程序。

目标流水线是：

```text
normalized GRH
    -> am::LinearProgram          // 单一线性构建区，不可执行
    -> AM activity scheduler
    -> am::ScheduledProgram       // 对应规范中的最终 Program
    -> AM C++ emitter
```

设计目标有三个：

1. activity 划分面对的是已经 lower 的 AM 访存和计算语义，而不是 GRH operation 的
   粗粒度分类；
2. 调度结果本身就是可验证、可解释、可被多个后端消费的 AM Program，不再是一组只有
   旧 C++ emitter 理解的 session side table；
3. 100M+ instruction 规模仍使用线性、紧凑、可流式处理的数据结构，避免对象图和多份
   全量副本。

本文所说的 `am::LinearProgram`、`am::ScheduledProgram` 和 `ProgramInterface` 是本框架
采用的 C++ 实现类型名。规范层名称仍然只有 `Program`；当 `ScheduledProgram` 通过最终验证后，
它在语义上就是规范中的 `Program`。

> 状态：本文描述目标框架和迁移 gates。在 Phase 3/4 通过前，生产实现仍是文末链接的
> legacy Graph + session 路径；存在框架类型和 smoke stage 不表示已经完成语义切换。
> 当前 `Structural`/`Semantic` validator 也只是部分 scaffold validator；即使
> `ValidationLevel::Semantic` 通过，也不等于已完整验证
> [指令集第 17 节](grhsim-am-instructions.md#17-合法性检查)的 AM 语义。当前已检查
> opcode/BV signature、state/memory target、activation event、`changed` old 独占性、
> interface input 写隔离、外部 input/output role 对齐、B0 的保守 net-change provenance、
> slice、system/DPI signature、非法 enum 和 Linear-only normal form。尚待最终 gate
> 覆盖的包括 host binding 唯一性、final-call 数据依赖顺序、`changed` result 的完整
> 独占/epoch 契约、B0 activation target 到实际 reader 的完整性、边沿分支的联合完备性和
> ordered-effect 完整性证明。

> 实现进展（2026-07-22）：完整 XiangShan `SimTop` 已通过 concrete lowering、production
> scheduling 和一次历史 AM full emit。该历史测量的规模为 5,080,563 条 linear AM 指令、
> 9,574,478 条 scheduled 指令、1,021,857 个 Block 和 2,040,184 个 changed detector；当时
> 生成的是 1,679,120,625-byte 的单一 C++ TU。此后 production scheduler 已采用 typed AM
> compute/commit/isolated atom 策略，C++ emitter 已改为 staged multi-TU shard 输出，生成
> runtime 已改为有序 packed activity bitset 和 dirty changed-result 清理。尚未用这版代码
> 重新 full-emit SimTop，也尚未编译全部 shard、链接 AM XiangShan emu 或运行
> 100/2k/20k/50k difftest。当前权威进度和剩余 gates 记录在
> `pdocs/grh_notepad/notes/00/000-099/NO00030_grhsim_am_pipeline_framework_20260722.md`。

当前 scaffold 已包含 `BaselineActivityScheduleStage`，用于尽早贯通强类型阶段 API 和
ScheduledProgram validator。它采用受限的 smoke 桥接布局：

```text
B0: changed.any(external input) -> act.f B1
B1: 原 LinearProgram 语义 instruction 流
    changed.any(each state, shared final value) -> act.b B1
```

这使外部输入变化进入一个普通 Block，state 实际变化时再进入下一 epoch。由于它不做
topological scheduling，只接受每个 Result producer 已经位于全部 use 之前的 LinearProgram
子集；forward def-use、`HostRead`、`HostEffect`，以及 ordinal 顺序与线性 instruction
顺序冲突的 ordered-effect group 都会被拒绝。它不做 def-use partition、memory-aware cost、
reader 精确激活或 coarsening，既不是生产 scheduler，也尚不是 differential oracle；`B1`
的全块重跑只是一条可删除的迁移桥。存在该 stage 不表示 Phase 1 的完整 GRH lowering 或
Phase 2 的 AM activity scheduler 已完成。

## 1. 为什么单线性块不能叫 Program

规范中的 `Program` 不是“若干 AM 指令的容器”。它至少已经满足以下可执行契约：

- `B0` 是每次 `eval()` 无条件执行的 EntryBlock；
- `B1` 及之后只在 active 时执行，首次求值额外激活全部普通 Block；
- `changed` 有独占的 `old` Variable，并在比较后更新基线；
- `act.f` 只指向更大的 BlockId，`act.b` 明确跨到下一 epoch；
- Block 顺序、event 清零点和 state write 的可见时点共同决定 `eval()` 行为。

GRH 刚完成 opcode lowering 时还没有这些信息。把所有指令临时塞进一个“B0”或“B1”
不会形成保守但低效的 Program，而会形成语义不同的 Program：放进 B0 会导致有状态指令
每次无条件执行，放进 B1 则没有合法 EntryBlock 来观察外部变化，也没有传播 activity 的
`changed`/`act`。因此该产物必须叫 `LinearProgram`，其中的“单线性块”只表示一个构建期
instruction region：

- 没有 BlockId，也不是规范中的 Block；
- 线性次序用于确定性构建、诊断和必须保持的 effect order，不自动成为运行时执行顺序；
- 不能创建 Machine，不能交给 interpreter/JIT/C++ emitter；
- 只有 AM activity scheduler 可以把它完成为 `ScheduledProgram`。

禁止提供“把 LinearProgram 当作单 Block 运行”的 fallback。它会让测试在小设计上偶然
通过，却掩盖 B0、event、state visibility 和 side effect 顺序尚未完成的事实。

## 2. 三个长期数据契约

### 2.1 `am::LinearProgram`

`LinearProgram` 是 normalized GRH 与 AM scheduler 之间的唯一 instruction 载体。它与
只供调度使用的 facts 一起组成 lowering 产物：

```text
LinearProgramArtifact
├── LinearProgram
│   ├── Variables          类型、Init 和可选诊断 label
│   ├── DpiImports         已规范化的 import 签名
│   └── Instructions       已类型检查的 AM 计算、访存和 effect 指令
├── ProgramInterface
└── SchedulingFacts
    ├── variableRoles
    ├── instructionEffects
    └── orderedEffects
```

普通 computation、`mem.read`、`mem.write`、`mem.fill`、`reg.write`、`latch.write`、
`dpi.call` 和 system instruction 在这一层已经使用 AM opcode、VarId、Type 和 Attribute
表达。不得保留 GRH `Operation*`、`Value*`、symbol 字符串查找或“到 emitter 再解释”的
GRH attribute。

LinearProgram 中可以已有表达 GRH raw event 的 `changed.any/pos/neg`；它们必须已经拥有
类型正确且独占的 old/event Variable。`act.f/act.b` 因为引用尚不存在的 BlockId，在这一
阶段禁止出现。Scheduler 会保留 raw-event changed，并另外加入 activity boundary 所需的
changed 和全部 act。

AM 规范允许 Result 与 Operand 共用 VarId，文本也不是 SSA。为了让 100M+ 调度使用一张
紧凑的 `definition[VariableId]` 而不是通用 reaching-definition 图，GRH lowering 对
LinearProgram 采用额外的 single-result-writer normal form：

- 每个作为 instruction Result 的非 constant Variable 最多只有一个静态 defining
  instruction；需要覆盖时创建 fresh Variable，并用显式后续计算连接；
- `reg.write`、`mem.write/fill`、`latch.write` 的 target 和 `changed` 的 old 仍是规范定义的
  read-write Operand，不被误算成 Result definition；
- lowering 不生成 Result/Operand alias；DPI output/inout 发生冲突时也先写 fresh temporary，
  再按有序 effect 显式连接。

这是 LinearProgram 的构建期 normal form，不是最终 Program 的新语义限制。
`validate(LinearProgram)` 可以检查它以保护 scheduler 假设；`validate(ScheduledProgram)`
仍必须接受 AM 规范允许的非 SSA Program，不能复用这条额外限制。如果未来某个 lowering
不能产生该 normal form，scheduler 必须显式改用 definitions/reaching-def CSR，而不是仍把
单数 producer 当成事实。

`SchedulingFacts` 只保存不能从普通 instruction def-use 唯一恢复、或在 100M+ 规模上不应
反复推导的 lowering 分类：

- Variable 是 external input/output、state 或 observable 中的哪几类；
- instruction 是 pure、state read/write、host read/effect 中的哪一类；
- DPI、system call、多写口 priority 等必须保持的 group 和 ordinal。

它不是可执行指令，也不是最终 Program metadata。raw event 的 any/pos/neg 语义必须在
LinearProgram 中成为类型合法的 `changed` instruction，或者由 lowering 提供不丢信息的
内部 watch record 后在 scheduler 中物化；不能只用 `InstructionEffect` 猜测边沿。
Scheduler 还必须为 activity boundary 创建合法的 `old`/event Variables 和 `changed`，
把依赖物化为 `act.f/act.b`，并通过最终 instruction/block 顺序兑现 ordered effect。
成功后调用 `SchedulingFacts::clearAndRelease()`，不再保存这些 lowering-only 记录。

外部输入的 net-change 观察由 `ProgramInterface` 推导，不用复制成每条 instruction 的
字符串属性。能够从 opcode 和 operand 恢复的 state/memory reader-writer 关系也必须由
scheduler 直接推导，避免另一份易失真的事实表。

### 2.2 `ProgramInterface`

AM 规范明确不在 Program 中区分 input、output、state 和 temporary；Label 也不唯一。
但 lowering、B0 构造和生成 C++ 公共端口都需要稳定的集成映射。因此流水线让一个非语义
的 `ProgramInterface` 伴随 IR：

```text
ProgramInterface
├── ports[]
│   ├── name            intern 后的 StringId
│   ├── direction       input | output | inout
│   ├── input           外部写入模型的 VariableId
│   ├── output          模型写给外部的 VariableId
│   └── outputEnable    inout 必需的驱动使能 VariableId
└── declaredVariables[] 调试/集成所需的 (VariableId, StringId)
```

它有以下边界：

- 它不是规范 `Program` 的组成部分，不改变 `eval()`、Machine State 或 HostEnvironment；
- 它只决定调用方能看到哪些 VarId、C++ 端口名和哪些外部可写 VarId 需要在 B0 被观察；
- port 次序、方向和名称是集成 ABI，不能通过非唯一 Label 反推；direction 不使用的 VarId
  字段必须为 invalid；所有有效 VarId 的 Type 从 Program Variables 取得，不能维护易失真的
  Type 副本；
- scheduler 只能在原有 Variables 尾部追加 synthetic old/event Variable，不能重编号已有
  VarId；如果未来必须重编号，必须返回显式 remap 并同步更新 interface；
- 外部 input/inout input Variable 只由调用方写入，不能作为任何 AM instruction Result 或
  state-write target；
- 所有 `changed` old/result Variable 都是私有状态，不能出现在 port 或
  `declaredVariables` 中。

`declaredVariables` 只保存确有调试或集成需求的声明映射，不能默认为所有 temporary 再复制
一份 label；否则它会成为另一张 100M 项冷表。

API 以组合产物传递所有权，避免 interface 与错误版本的 Program 配对：

```text
LinearProgramArtifact = (LinearProgram, ProgramInterface, SchedulingFacts)
ExecutableModel       = (ScheduledProgram, ProgramInterface)
```

两种 artifact 都要校验 port name 唯一、方向对应的 VarId 有效、input/output 逻辑 Type
兼容、input/inout 的可写性和 input 写隔离。`LinearProgramArtifact` 还要求
`ExternalInput`/`ExternalOutput` role 与 interface 精确一致，所有 state/memory target 都有
`State` role。`ExecutableModel` 的当前 scaffold validator 会从 B0 的 latest definition
反向追踪 `changed.any`，只允许不会丢失真值的 assign/OR/concat/replicate/reduce-or cone，
并确认每个外部 input 的 net-change 至少到达一个 `act.f`。

因此公开验证边界不能只有 `validate(LinearProgram)` 和 `validate(ScheduledProgram)`；还
需要 `validate(LinearProgramArtifact)` / `validate(ExecutableModel)`，或等价的
`validateInterface(ProgramView, ...)`。B0 coverage 可以由 validator 从 interface 与 B0
instruction 重建，不应为了证明它而把已释放的 SchedulingFacts 塞回 ExecutableModel。
当前证明有意保持保守：它既不把被覆盖或提前重写的 event/input 误算为 coverage，也接受
多个 `changed.any` 经 OR 汇聚；但它尚不证明 act target 覆盖所有实际 reader，也不接受
所有逻辑等价的边沿拆分。这两项必须由正式 scheduler 的 reader/activation graph 和最终
validator 关闭，不能把当前 scaffold 的通过结果当作完整语义证明。

### 2.3 `am::ScheduledProgram`

`ScheduledProgram` 的逻辑字段就是规范的：

```text
ScheduledProgram = (Variables, Blocks, DpiImports)
Block             = (BlockId, Instructions)
```

实现类型名中的 `Scheduled` 只是区分 C++ 构建阶段，不引入第二套执行语义。最终 validator
必须逐项执行 `grhsim-am.md` 第 2、4、5、8 节的 Program 合法性检查；通过后，interpreter、
JIT 和 C++ emitter 都只能按同一规范观察它。

Scheduler 可以选择不同的 Block 粒度或合法拓扑序。只要 `changed`/`act`、effect order、
状态可见性和最终可观察行为一致，这种差异不是新的 Program 语义。

## 3. AM activity scheduler

### 3.1 输入为何更适合访存分析

在 LinearProgram 上，访存已经呈现为明确的 AM 对象：

- 一个 Array Variable 表示 memory storage，元素 Type 和固定 depth 已知；
- `mem.read` 显式给出 memory 与 address；
- `mem.write`/`mem.fill` 显式给出 address、data、mask/range、condition 和 event；
- address/data/mask producer 都是普通 VarId def-use；
- register、latch、DPI 和 system effect 使用不同 opcode，不再依赖 GRH op class 猜测。

因此划分成本可以同时考虑 instruction 数、宽值字数、memory access 数、可能的 alias 域和
状态写回扇出。`maxOpInComputeNode` 一类“只数 operation”的启发式可以逐步替换为可解释的
block cost model。第一版仍可采取保守划分，但不得复制 memory read 来迎合旧 source-class
模型；可能 alias 的写也不能在没有证明时交换次序。

### 3.2 建议算法阶段

Scheduler 按以下阶段工作，每一阶段只保留下一阶段需要的事实：

1. 校验并 freeze LinearProgram；在上述 single-result-writer normal form 上建立
   definition、uses、state/memory access、effect order 和 interface input reader 索引。
2. 将必须一起执行的 instruction 收缩为 scheduling atom。event consumer、不可拆分的
   memory intent、ordered DPI/system call 和多写 priority 在这里形成硬约束。
3. 对依赖做 SCC/环检查，构造 condensation DAG；不能被 AM epoch 语义合法表达的组合环
   必须诊断，不能靠原始 instruction 顺序碰运气。
4. 按 cost、局部性和硬上限把 atom 划为普通 block；先形成稳定拓扑序，再连续编号为
   `B1...Bn`。
5. 对跨 block 的可观察 value 变化创建或复用 watch。pure value 只有在 source Block、
   VarId、change kind 和执行 frontier 全部相同时才能共用 detector。每个 materialized
   `changed` 创建独占、`Init = undef` 的 old Variable 和一个 event Variable，并放在 value
   更新后、act 之前。
6. 同一 register/memory/latch target 存在多个候选 write 时，先按 priority/effect order
   完成所有实际写回，再在最终 write frontier 放一个共享 `changed.any target,targetOld`；
   不能按 writer 或 activation edge 各放 detector，否则会把中间瞬态错误地暴露给 reader。
7. 对外部可写 port 的 watch 在 `B0` 物化；B0 只含 `changed`、派生 event 所需的组合指令
   和 `act.f`。内部 watch 放在其 producer/writer 所在普通 block。
8. target BlockId 大于 source 时生成 `act.f`；其余依赖，包括自激活和必须跨 epoch 的
   feedback，生成 `act.b`。同一 event/target 去重，但不合并语义不同的 old 基线。
9. 按 BlockId 顺序写出 ScheduledProgram，丢弃 derived facts，运行最终 validator。

第一次 `eval()` 激活所有 `B1...Bn` 是 Machine 语义，不需要额外伪指令。Scheduler 也不能
把 `changed.old = current` 的初始化偷偷加入 Program；规范要求 old 使用 `undef`，首次
event 及其影响可能是 AM 层未定义行为。

当前 production scheduler 的第一阶段采用保守的两类装桶策略。它把 AM atom 分为 compute、
commit 和 isolated：纯计算与 state read 是 compute；带 state target 的 reg/latch/memory
write 是 commit；host call 和 raw `changed` 暂为 isolated。Kahn ready 集合只在同类 atom
之间连续装桶，因此不会违反已经建立的 def-use 或 ordered-effect 边。compute 使用
`maxInstructionsPerBlock`，commit 使用独立的 `maxCommitInstructionsPerBlock`（默认 4096），
并同时受 `maxStateWritesPerBlock` 限制；不可拆 atom 超限时保留为一个 oversized Block 并报告
诊断。commit 的跨 target 合并只改变活动粒度，不改变 block 内的拓扑/effect 次序；同一 target
的 watcher 仍只在 final write frontier 物化。这个阶段刻意不复制 legacy source，也不按
Graph symbol/string 推断 guard。memory alias、宽值成本和更精细的 cone/chain coarsen 只能在
AM typed facts 足够并经 differential gate 验证后加入。

### 3.3 临时 scheduling facts

以下内容只属于 scheduler workspace，不进入 ScheduledProgram 或 session 的长期公共契约：

| Fact | 用途 | 最晚释放点 |
| --- | --- | --- |
| opcode class/cost | 划分 compute、memory、effect 成本 | block 划分后 |
| definition 与 use CSR | def-use、boundary 和 input reader | watch/act 完成后 |
| state/memory access table | reader 激活、alias 和 write order | block/edge 完成后 |
| effect/order groups | DPI、system、多写 priority 顺序 | block 内顺序确定后 |
| SCC、indegree、topo worklist | 合法排序和 feedback 判定 | BlockId 确定后 |
| instruction-to-atom/block | edge 聚合和最终 materialize | block 表写出后 |
| activation edge CSR | changed/act target 集合 | act 指令写出后 |
| origin map | 诊断；默认关闭，按需保留 | 诊断结束后 |

不得再次导出与当前 `activity_schedule.supernode_to_ops`、`value_fanout` 等同构的一组
session key，再让 emitter 拼回执行模型。那会保留两份事实来源，违背这次改造的目的。
需要统计时，从 workspace 在释放前生成聚合计数；需要调试时显式请求独立 artifact，不能
默认保留 100M 项 origin/debug 表。

## 4. 100M+ instruction 的物理表示与预算

### 4.1 强制表示原则

100M 规模禁止用 `vector<unique_ptr<Instruction>>`、每 instruction 一个 `std::vector`、
每边一个对象、per-op `std::string` 或 `unordered_map<Id, ...>` 作为主表示。框架采用：

- 默认 32-bit `VariableId`、`InstructionId`、`BlockId`、`TypeId`、`StringId` 等 dense ID；
  构建时检查总量，
  `0xffffffff` 保留为 invalid，超界明确失败或切换经独立验证的 large-index build；
- 带末尾 sentinel 的 offset table 最多对应 `UINT32_MAX - 1` 个逻辑 record，使其
  `recordCount + 1` 个物理 offset 仍不超过单 arena 的 32-bit 数量上限；
- instruction 使用 SoA：`Opcode[]`、`operandOffsets[]`、`resultOffsets[]`，operand/result
  放入连续 VarId arena；
- Block 使用 offset 和可选的扁平 InstructionId permutation，不使用
  `vector<vector<...>>`；identity instruction 顺序只保存 offset，不物化每条 4-byte ID；
- def-use、activation edge 和 reader set 使用 CSR：`offsets[] + ids[]`；
- lowering 用映射保证 Type 和字符串 intern，storage 只保存 dense table；Attribute 按
  opcode 放入以 InstructionId 索引的 sparse typed table，Label 和 origin 放在可选冷表；
- builder 完成时用单调 cursor 线性核对这些已排序 typed attribute table，不对每条
  instruction 做二分查找；interface 的 changed-private 检查使用 bit-packed VarId mask，
  不物化每个 detector 的 old/result ID 列表；
- builder 提供 `ProgramReserve`，优先通过预扫描精确 reserve，freeze 后只读；不能因
  `vector` 扩容同时保留数 GiB 新旧 buffer。无法预估的超大输入才采用 chunk staging，
  freeze 时仍只形成一份最终 dense arena；
- scheduler 接受 `LinearProgramArtifact&&` 并消费所有权，`ScheduledProgramBuilder` 再
  接管其中的 `LinearProgram&&`；能复用的 Variables、Type、Attribute、Init 和 instruction
  arena 直接转入 ScheduledProgram，生产模式不在 session 同时保留两份完整 IR。

当前 `ProgramReserve` 已覆盖 linear 主表和现有 typed attribute arena，
`ScheduledProgramReserve` 也可预留新增 Variable/instruction/operand/result、typed attribute、
activation target、Block offset 和 blockInstructionIds。这些 API 补齐了预留面，但尚未
关闭 large-scale gate：如果 LinearProgram 只按 linear 最终 size 紧密 reserve，scheduler 在接管
后再调用 `ScheduledProgramBuilder::reserve()` 仍可能搬移 GiB 级前缀 arena。生产 lowering
必须把可预测的 synthetic tail 合并进初始 reserve，或者将相应 storage 改为分段表示。
在完成 100M synthetic RSS/no-copy 实测前，不能仅凭 reserve API 声称该 gate 通过。

instruction hot table 的逻辑布局是：

```text
opcodes[I]             : u8
operandOffsets[I + 1]  : u32
operands[ROperand]     : VariableId(u32)
resultOffsets[I + 1]   : u32
results[RResult]       : VariableId(u32)
```

arity 由相邻 offset 相减得到，不需要给每条 instruction 保存 count。Block 使用
`blockOffsets[B + 1]` 和可选的 `blockInstructionIds[]` permutation；identity 顺序时可由
offset 直接恢复 InstructionId。物理 indirection 不改变规范中“Block 内按文本顺序执行”
的逻辑。

### 4.2 可审计预算

Scheduler 会追加 activity instruction 和 Variable，因此必须区分输入与输出规模。令：

```text
IL, RL, VL = LinearProgram 的 instruction、operand/result 引用、Variable 数
IS, RS, VS = ScheduledProgram 的 instruction、operand/result 引用、Variable 数
ED          = scheduler 临时 dependency edge 数
EA          = act.f/act.b 的 target BlockId 引用总数
B           = Block 数
IB          = 物理 blockInstructionIds permutation 中的引用数（identity 时为 0）
A           = intern 后 Type/Init/Attribute/string/label 等冷表 byte 数
```

其中：

```text
IS = IL + IChanged + IAct + IEventDerive
VS = VL + 2 * IChanged + VEventDerive
```

这里的 `IChanged` 只计 scheduler 新增 detector；LinearProgram 已有的 raw-event changed 已在
`IL/VL` 中。每条新增 changed 至少需要独占 old 和 result event 两个 Variable。实现可以因
event 派生复用而减少 `VEventDerive`，但预算不能预设为零。

首版必须逐项报告下列 arena；它们是已列主表的预算，不是遗漏 payload/capacity 后仍声称的
总 RSS 上界：

| 部分 | 已列 arena 预算，不含 Value/Init payload |
| --- | --- |
| Linear instruction hot table | 约 `9 * IL + 4 * RL` bytes |
| Scheduled instruction hot table | 约 `9 * IS + 4 * RS` bytes；实现通过 move 复用 linear 前缀 |
| Scheduled Variable metadata | `8 * VS` bytes，label 在 sparse 冷表 |
| Block CSR | `4 * (B + 1) + 4 * IB` bytes；identity 顺序时物理 `IB = 0` |
| activation target arena | `4 * EA` bytes，不能含混并入 Attribute 估算 |
| intern/sparse typed attribute/cold arena | `A` bytes，不按 use 复制 |
| scheduler 峰值 scratch | 目标不高于 `12 * IL + 8 * VL + 4 * ED` bytes，另计正在追加的 scheduled delta |

例如最终 `IS = 100M`、平均每条 scheduled instruction 三个 operand/result 引用时，
instruction 主表约 `0.9 GB + 1.2 GB = 2.1 GB`；若每条 instruction 在 block 表中出现
一次且顺序不是 identity，再增加约 `0.4 GB`；identity 顺序可省略这个 permutation
arena。如果给定的是 `IL = 100M`，必须先测出上述 synthetic delta，不能仍套用
2.1 GB。
若同一最终压力用例还有 `VS = 100M`、`ED = 300M`，则 Variable metadata 再占约 `0.8 GB`，
主 IR 加常规 block/冷表约为 3.2 至 3.6 GB；按 scratch 目标计算，调度峰值应落在约
6 至 8 GB，再另计超宽 literal、Array Init payload 和输出 buffer。验收使用实测字段代入，
不能把这个示例当成所有设计的固定配额。
这组数字的目的不是承诺固定 RSS，而是让每个新 side table 都能换算为真实 GiB：一个
额外的 `u32[100M]` 就是约 400 MB，一个 `u64[100M]` 就是约 800 MB。

每次 large-scale gate 必须报告 `IL/RL/VL/IS/RS/VS/ED/EA/B/IB/A`、各 arena 的 size 与
capacity bytes、峰值 RSS 和阶段释放点。
`ProgramStorageStats` 已通过 `ProgramArena`/`ArenaStorageStats` 报告逐 arena 的
size/capacity bytes，不只是合计 `estimatedBytes`；scheduler workspace 的 phase
high-water/release telemetry 仍未实现。`ED/EA/A` 不能从最终 instruction 数间接猜测。
Array 初始化内容、超宽常量和生成 C++ 文本可能主导总内存，需单列，不能藏进“每 op
预算”。如果 scratch 超过上述目标，应先改为 bit-packed flag、CSR、buffer 复用或分阶段
流式算法，而不是提高默认机器内存假装问题消失。

### 4.3 Emitter 的有界工作集

AM C++ emitter 按 Block span 和输出 shard 流式生成，工作集应由“当前 block/batch +
有界输出 buffer”决定。禁止为所有 instruction 构造 C++ AST/string，也禁止重新建立一份
GRH 风格 def-use 图。跨 shard 需要的静态信息应来自一次紧凑索引或直接来自最终
`changed`/`act`/VarId，不从 source text 反向解析。

当前实现将公开模型 ABI 拆为 `<prefix>.hpp`、`<prefix>_support.hpp`、
`<prefix>_runtime.cpp` 和连续编号的 `<prefix>_blocks_N.cpp`。默认每个 block source
承载 2,048 个 Block；Makefile 显式列出有序 `SRCS`，不使用 wildcard。Instruction C++ 文本
只在当前 shard 写入，生成在 staging directory 完成后才发布，因此 rejected emit 不会留下
半套 artifact。

生成 runtime 将 current/next activity 表示为每 64 个 Block 一个 word，并为 non-empty
activity word 再维护一层 summary bitset。`act.f` 设置 current 位图，`act.b` 设置 next
位图；消费时按递增 BlockId 取位，因此符合合法 Program 的严格 forward target 规则。首次
`eval()` 仍激活所有普通 Block。`changed` 使用 `set_changed_result` 将实际为真的结果加入
dirty list，epoch/eval 边界只清理这些结果，而不是生成每个 detector 的静态 clear store。

## 5. 与旧 Graph + session emitter 的并轨边界

当前路径是：

```text
normalized GRH
    -> activity-schedule
    -> Session 中多组 Graph OperationId/supernode/value_fanout 数据
    -> grhsim-cpp 同时读取 Graph 和这些 session key
```

新路径完成后，AM C++ emitter 的完整输入只能是：

```text
const ExecutableModel&   // (am::ScheduledProgram, ProgramInterface)
```

这是唯一并轨边界。可以复用旧 emitter 在边界之后的成熟基础设施：

- C++ value storage、宽值 helper 和 runtime ABI；
- 公开 model class、`init/eval/finalize` 外壳；
- 源文件 batch/shard、文件大小保护、Makefile 和并行写文件；
- 已证明对相应 AM opcode 等价的 codegen combine。

不能越过边界继续读取 Graph operation、OperationId、symbol lookup 或
`activity_schedule.*` session key。旧 combine 若仍以 GRH 图形状匹配，必须先改为 AM
opcode/VarId pattern，并证明不改变 `sameValue`、changed 或 activation 行为，才能复用。

Session 仍可作为 orchestration 和所有权容器，但不再充当语义拼接协议。建议过渡期使用
一个 typed `ExecutableModel` key，emit API 取出同一 artifact 中的 Program 与 Interface；
large design 默认 move/consume 前一阶段产物。为了 A/B 对比而同时保留旧 Graph schedule
与新 Program 只允许在小型测试或显式 profiling 模式中进行。

`GrhToAmLoweringStage::lower()` 返回的 artifact 不能保存 Graph pointer/reference。100M
生产入口必须分阶段调用：lower 返回 owned artifact 后先从 Session/Design 移除 GRH，再进入
schedule 和 emit。贯穿一个 `run(const Graph&)` 调用的 convenience pipeline 不能让调用方在
中途释放 Graph，只适合小设计；若保留该入口，其 Graph RSS 必须计入峰值，不能作为
no-double-copy gate 的证据。

迁移期间可以让现有 `emit_grhsim_cpp(...)` 外部 API 保持不变，并在内部调用新 lowering、
scheduler 和 AM emitter；这只是 API 兼容。禁止把旧 session schedule 翻译成“看起来像”
ScheduledProgram 作为长期实现，因为旧 compute/commit round 与 AM epoch 并不天然同构。

## 6. 分阶段实现和验收 gates

建议源码按所有权边界摆放：

| 责任 | 位置 |
| --- | --- |
| AM dense ID、SoA storage、Linear/ScheduledProgram 与只读 view | `include/grhsim/am/program.hpp`、`lib/grhsim/am/program.cpp` |
| move-only builders 与 reserve API | `include/grhsim/am/builder.hpp`、`lib/grhsim/am/builder.cpp` |
| linear/scheduled validator | `include/grhsim/am/validate.hpp`、`lib/grhsim/am/validate.cpp` |
| ProgramInterface、SchedulingFacts、stage/pipeline API | `include/grhsim/am/pipeline.hpp`、`lib/grhsim/am/pipeline.cpp` |
| opcode 分类 helper | `include/grhsim/am/opcode_traits.hpp` |
| 受限的 baseline smoke scheduler | `include/grhsim/am/activity_schedule.hpp`、`lib/grhsim/am/activity_schedule.cpp` |
| concrete lowering/scheduler/emitter 私有实现 | `lib/grhsim/am/` 下按职责拆分的 `.cpp` |
| 单元与 contract tests | `tests/grhsim/am/` |

不要把 IR 定义塞进旧 `transform/activity_schedule.cpp` 或 `emit/grhsim_cpp.cpp`。`program`
与 builder 不依赖 GRH transform 或 C++ emitter；pipeline API 声明单向的
`GRH -> LinearProgramArtifact -> ExecutableModel -> C++` stage；emitter 只读取 core AM
artifact。具体实现增长后可拆私有 `.cpp`，但公共所有权方向保持不变。

每个 Phase 开始、关键决策改变和 Gate 关闭时，都要在 `pdocs/grh_notepad` 更新同一推进
记录，至少写入：本阶段事实、选择及替代方案、测量数据、未关闭风险、执行过的命令和下
阶段入口。notepad 是进度证据，不替代本文的长期契约，也不进入 AM artifact。

### Phase 0：冻结契约与测量基线

- 把本文的数据契约转成 header skeleton、validator 接口和 size `static_assert`；
- 以 `BaselineActivityScheduleStage` 打通
  `LinearProgramArtifact -> ExecutableModel`，只把它作为 API/validator smoke bridge；
- 增加 artifact/interface validator，并补全 Program validator；Program validator 分别执行
  linear-only normal form 与最终规范规则，不能混用；
- 用 scheduled-growth reserve 和 per-arena size/capacity telemetry 验证 synthetic
  changed/act/Variable、activation target 和 block CSR 都可预留、可计量；
- 记录当前 HDLBits、C910/XS 代表用例的行为、emit/compile/run 时间和峰值 RSS；
- 建立覆盖 first eval、edge、反馈、多个 state writer、memory、DPI/system effect 的小型
  differential corpus。

Gate：术语中不存在可执行“single-block Program”；baseline 的限制有测试且未被当成
memory-aware scheduler；所有预算字段可测；每个已知风险至少有一个预定 test oracle；
notepad 包含 baseline 证据和 Phase 1 待办。

### Phase 1：compact LinearProgram 与 lowering

- 实现 arena、intern table、ProgramInterface、SchedulingFacts 和 linear validator；
- normalized GRH 完整 lower 为 AM Type/opcode/Attribute，不再把 GRH 节点交给后续阶段；
- 强制并验证 LinearProgram single-result-writer normal form，报告插入的 fresh temporary；
- 生产 orchestration 在 `lower()` 返回 owned `LinearProgramArtifact` 后立即释放/移除 GRH；
  只接受 `const Graph&` 并贯穿整个 convenience `run()` 的入口必须把 Graph RSS 计入峰值，
  不能用于证明 100M no-double-copy gate；
- 提供只用于测试的小规模 text dump，生产路径不默认生成巨型文本。

Gate：opcode 覆盖与指令集文档逐项对齐；非法 hierarchy/blackbox/X/Z 输入在 lowering
失败；端口 ABI round-trip；100M synthetic case 满足主表和峰值预算、GRH 已在 schedule
前释放，且无 per-op heap allocation。Baseline 能消费该 artifact 只证明阶段接线正确，
不等于 Phase 2 完成。

### Phase 2：AM activity scheduler

- 实现紧凑 facts、block partition、B0、watch、`changed` 和 `act.f/act.b` materialize；
- 实现 ScheduledProgram validator 和确定性摘要 hash；
- 在小设计上用规范解释器或直接参考执行器比较全量执行与 activity 执行。
- 先用规范解释器和 differential corpus 证明 `BaselineActivityScheduleStage` 的适用子集，
  再决定是否将它保留为小设计 oracle；当前拒绝 host interaction 的 smoke bridge
  不得当作 differential oracle。正式 scheduler 必须产生多 Block reader 精确激活并通过
  memory-aware cost/alias tests，不能回退为永久 B1。

Gate：所有规范 Program 不变量通过；不同线程数产生相同 Program hash；feedback/event/
state/memory/side-effect corpus 可观察行为一致；large case 的 scratch 与阶段释放满足预算。

### Phase 3：AM C++ emitter 并轨

- 建立只读 ExecutableModel 的 emitter front-end；
- 在 AM 边界后逐步接回 runtime、宽值 helper、sharding 和合法 combine；
- 生成代码中可追溯 BlockId/InstructionId，但 release 模式不保留全量字符串 origin。

Gate：代码搜索和依赖检查证明 emitter 不读取 Graph 或 `activity_schedule.*`；生成 C++
编译通过；解释器与生成模型在小型 corpus 逐 eval 对比；HDLBits 全量通过。

### Phase 4：大设计 shadow 与切换

- C910/XS 使用同一 normalized GRH 分别运行旧/新路径，比较端口、DPI/system call 顺序和
  difftest 结果，不要求 schedule 形状相同；
- 报告 lowering/schedule/emit/compile/run 时间、生成源码大小、Program arena 和峰值 RSS；
- 修复差异后将默认路径切到 AM，保留短期显式 legacy 开关。

Gate：代表 workload 的功能等价；无未解释的 side-effect trace 差异；100M+ 规模不产生
双份全量 IR；性能与内存回归阈值由 Phase 0 基线明确批准，而不是口头判断。

### Phase 5：移除旧语义拼接

- 删除旧 emitter 对 `activity_schedule.*` key 和 Graph schedule shape 的依赖；
- 删除只服务旧 emitter 的 session schedule schema，更新当前调度/emit 文档；
- interpreter/JIT 若接入，直接复用同一 ScheduledProgram validator 和语义测试。

Gate：仓库没有第二套 runtime schedule 真相；所有生产入口都经过
`LinearProgram -> ScheduledProgram`；旧开关移除后完成迁移。

## 7. 已知语义差异和迁移风险

旧实现的行为不能直接当成 AM 规范。迁移时至少逐项审计以下差异：

| 风险 | 当前旧路径与 AM 契约的差异点 | 必须验证的结果 |
| --- | --- | --- |
| 首次求值 | 旧路径 seed compute/activity；AM 先执行 B0，再额外激活全部 B1+，old 为 `undef` | 不把旧 baseline 强加给 AM；reset 后行为与首次未定义边界分开测 |
| round/epoch | 旧 compute bit + 每 round 扫 commit；AM 每个普通 Block 都由 activity 驱动，`act.b` 明确进入下一 epoch | feedback 收敛、写回可见时点和执行次数符合 AM |
| forward 激活 | 旧代码依赖 topo active id 和 batch 内局部传播；AM `act.f` 只能指向更大 BlockId | 每条 forward edge 经 validator 证明，其他边必须是 `act.b` |
| event 生命周期 | 旧 event edge slot 通常按 fixed-point round 清零；AM changed result 在 epoch 边界清零，B0 每次 eval 执行 | pos/neg、同 epoch 多消费者和跨 epoch event 不丢失或重复 |
| state write | 旧 commit supernode 可能每 round 扫描；AM reg/latch/mem write 位于被激活 Block 并读取执行点 operand | 多写 priority、mask/fill、read-during-write 和 reader reactivation 一致 |
| memory 划分 | 旧 `kMemoryReadPort` 是 source-class 且可能 clone；新层看到显式 `mem.read` 和 Array | address 依赖保留；有副作用/可能 alias 的访问不被非法复制或重排 |
| 外部输入 | 两条路径都应只观察两次 eval 间的最终值，但 seed 机制不同 | 0->1->0 后再 eval 不产生虚假变化 |
| DPI/system | 旧 supernode/batch 和 full-pass fast path 可能改变调用次数；AM 要保持 Block、schedule、once/final 顺序 | 逐次调用 trace、参数/返回 ABI 和 finalize 顺序一致 |
| value equality | 旧 emitter 有宽值/packed-array 专用优化；AM `sameValue` 对 BV、Real bit pattern、String、Array 有统一定义 | NaN bit pattern、宽值最高字 mask、Array 更新的 changed 判断一致 |
| 非收敛 | 两条路径的循环单位和可选保护不同 | 不把未收敛静默当成功；诊断能定位 Block/epoch |
| 特化路径 | 旧 full-pass specialization 可绕过 generic fixed point | 只有证明等价的特化才能留在 AM emitter，不能以性能为由豁免 |

此外，hierarchy/XMR/blackbox 和含 X/Z Logic 必须按 AM lowering 规范拒绝或在更早阶段处理；
不能因为旧 emitter 曾接受某种残留 Graph 形态，就扩张最终 Program 的合法集合。

## 8. 文档边界

- 最终可执行语义：[GRHSIM-AM 规范](grhsim-am.md)
- opcode、operand 和 Attribute：[GRHSIM-AM 指令集](grhsim-am-instructions.md)
- DPI/system 宿主边界：[HostEnvironment](grhsim-host-environment.md)
- 迁移前的 Graph 调度实现：[当前 activity-schedule](../transform/activity-schedule.md)
- 迁移前的生成模型：[当前 GrhSIM C++ 模型](../emit/grhsim-model.md)
- 迁移前的 compute/commit 细节：[当前 GrhSIM 调度方法](../emit/grhsim-scheduling.md)

后三篇描述的是 legacy Graph + session 路径。在 Phase 5 之前它们仍是当前代码事实，但不能
用来覆盖本流水线最终输出必须满足的 AM Program 语义。
