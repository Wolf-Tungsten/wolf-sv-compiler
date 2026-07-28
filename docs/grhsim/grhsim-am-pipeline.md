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

> 状态：本文描述目标框架和迁移 gates。项目层保留文末链接的 legacy Graph + session
> 路径，同时以独立 Make target 暴露 AM 路径；现有 legacy target 没有被静默切换。
> 当前 `Structural`/`Semantic` validator 也只是部分 scaffold validator；即使
> `ValidationLevel::Semantic` 通过，也不等于已完整验证
> [指令集第 17 节](grhsim-am-instructions.md#17-合法性检查)的 AM 语义。当前已检查
> opcode/BV signature、state/memory target、activation event、`changed` old 独占性、
> interface input 写隔离、外部 input/output role 对齐、B0 的保守 net-change provenance、
> slice、system/DPI signature、非法 enum、Linear-only normal form，以及 ExecutableModel
> 级的 commit Block 连续后缀、state write/`act.f`/`act.b` 放置与 target 范围、跨块
> `changed` result 的严格前向流。尚待最终 gate 覆盖的包括 host binding 唯一性、
> final-call 数据依赖顺序、`changed` result 的完整独占/round 生命周期契约、
> B0 activation target 到实际 reader 的完整性、边沿分支的联合完备性和
> ordered-effect 完整性证明。

> 实现进展（2026-07-28）：运行时调度模型已从“epoch + `act.f`/`act.b` 双缓冲 +
> commit 双通道”整体替换为与 legacy 对齐的两阶段 round 模型：compute Block 按
> BlockId 升序、以单一 active 位图过滤执行；commit Block 构成连续后缀，每轮总是
> 扫描；任一 `act.b` 激发即要求下一轮，一趟完整遍历无激发即收敛。
> `BaselineActivityScheduleStage`、`AmBlockFormation::Greedy`、
> `maxStateWritesPerBlock`、commit group 执行计划、commit operand capture、
> consume-on-event、writer-frontier 和 `preCommitSnapshots`（eval 首快照绑定）已全部
> 删除，全仓只剩一份 activity schedule 实现（legacy 移植的 coarsen + segment DP）。
> 改造计划见 `pdocs/draft/grhsim-am-legacy-round-model-plan-20260728.md`。
>
> 同日的 XS difftest 裁决出一处语义修正：commit 写指令的操作数若**直接引用寄存器
> state**，就地读取会把 commit 段内/段间的先写后读变成 read-new，破坏启动路径
> （XiangShan BPU 预测 PC 管线 16954→17116，首取指地址多推进一个 64B 块，NEMU
> 重放崩溃于 cycle ~568）。legacy 的正确语义是 read-old（sink 数据来自 compute
> 已收敛值）。修复在 lowering：这类操作数改经快照变量 + 一条普通 compute
> `assign`（在 compute 阶段求值、随 state 变化经 `act.b` 重激活）读取，恒等于本轮
> commit 前的值；不恢复任何运行时捕获/快照机制，RMW target 旧值与 event 操作数
> 保持就地读取（与 legacy/旧模型一致）。Gate 状态：AM 单测 8/8、xs-components
> 053/044/100 与 legacy 20,000 向量 bit-exact；XiangShan CoreMark/NEMU 已严格按
> 2k -> 20k -> 50k 升档，**三档全部通过**（50k 时双方退休指令数 73,580、IPC 1.471718
> 完全一致）。50k host time：AM 2,682,743 ms，同窗口 legacy 169,387 ms（15.8x）；
> 对比旧 AM 模型的 4,178,703 ms（历史记录）提升 1.56x，但仍未回到 legacy 基线
> （355,000 ms 历史记录）附近的同一数量级，性能阈值需另行评审。性能差距的代码层
> 来源仍是逐 Block 动态 dispatch、detector 密度与 commit 段常扫描（见
> `grhsim-am-vs-legacy-analysis-20260727.md` §3.4/§3.5）。
>
> 历史记录（2026-07-25，对应上述重构前的旧模型）：production scheduler 已删除
> `Isolated` class，commit write 曾采用 consume-on-event（该机制已删除），wide-result
> shift 已在执行前按 result 宽度扩展 lhs。
> fresh XiangShan `SimTop` v8 产品包含 4,950,236 条 linear 指令、37,461 个 normal
> Block 加 B0、8,992,117 条 scheduled 指令和 1,875,970 个 detector。CoreMark/NEMU
> 已严格按 2k -> 20k -> 50k 运行，三档全部通过；50k 结果为
> `instrCnt=73580, cycleCnt=49996, guestCycles=50001`。功能 gate 已关闭，但 host time
> 为 4,178,703 ms，尚未达到旧基线的 355,000 ms 性能目标。
> 当前权威进度和剩余 gates 记录在
> `pdocs/grh_notepad/notes/00/000-099/NO00030_grhsim_am_pipeline_framework_20260722.md`。

项目层两条 XiangShan 路径显式分离：

```text
make xs_wolf_grhsim_emu
make run_xs_wolf_grhsim_emu
    -> build/xs/grhsim           -> legacy activity-schedule + legacy C++ emitter

make xs_wolf_grhsim_am_emu
make run_xs_wolf_grhsim_am_emu
    -> build/xs/grhsim-am        -> post-stats JSON -> AM lower/schedule/C++ emitter
```

AM 路径由 `scripts/wolvrix_xs_grhsim_am.py` 编排。前处理子进程只生成 normalized
post-stats JSON 并退出，释放 GRH 内存后再启动 `grhsim-am-lower-json`。两条路径使用独立的
normalize、emit、model build 和运行日志名称；difftest 侧继续复用相同的 `GRHSIM=1`
模型 ABI。当前 AM emitter 不支持 waveform/runtime-profile，AM target 会显式拒绝对应选项，
而不是回退到 legacy emitter。

> 历史基线（2026-07-22）：早期 single-TU full emit 为 5,080,563 条 linear 指令、
> 9,574,478 条 scheduled 指令、1,021,857 个 Block 和 2,040,184 个 detector，生成
> 1,679,120,625-byte C++ TU。这些数字仅保留为旧 emitter/scheduler 证据，不是当前结果。

## 1. 为什么单线性块不能叫 Program

规范中的 `Program` 不是“若干 AM 指令的容器”。它至少已经满足以下可执行契约：

- `B0` 是每次 `eval()` 无条件执行的 EntryBlock；
- 普通 Block 分为 compute 段和构成连续后缀的 commit 段：compute Block 只在 active
  时执行，首次求值额外激活全部 compute Block，commit Block 每轮（round）总是扫描；
- `changed` 有独占的 `old` Variable，并在比较后更新基线；
- `act.f` 只出现在 B0 和 compute Block，只指向更大的 compute BlockId，并在同一趟
  compute 扫描内被消费；`act.b` 只出现在 commit Block，target 为 compute Block，
  其激发是要求下一 round 的唯一信号；
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

移位必须在 lowering 边界完成原生 Type 规整。对 `kShl`、`kLShr` 和 `kAShr`，
原生 AM shift Type 为 `BV<width(GRH result), signedness(lhs)>`；先把 lhs resize/coerce 到
该 Type，再执行 shift，使 AM result 与 lhs Type 完全一致。若映射后的 GRH
result Signedness 不同，shift 先写入原生 temporary，再由 `assign` 写入 result。
禁止先按 lhs 原宽执行 shift 再扩宽，因为扩宽无法恢复已经截掉的高位；
`kAShr` 也不能采用 result Signedness，否则 signed lhs/unsigned result 会把算术右移
退化成逻辑右移。

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
ExecutableModel       = (ScheduledProgram, ProgramInterface, commitBlockBegin, commitBlockEnd)
```

`commitBlockBegin`/`commitBlockEnd` 是一个半开 Block 区间：commit Block 构成 Block
空间的连续后缀，区间终点恒为 Program 的 Block 总数；两者均为 0 表示没有 commit
Block。state write 只允许位于 commit Block；commit Block 每轮总是被扫描，不占用
激活状态。

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
2. 将 event consumer、memory intent、DPI/system/effect 顺序和多写 priority 表示为有向
   依赖/顺序边。它们是必须保持的约束，但不因而要求两端属于同一个 scheduling atom；合法时
   可以跨 atom 和 Block。
3. 对该依赖图做 SCC/环检查并构造 condensation DAG。condensation DAG 的顶点才是
   indivisible scheduling atom；不能被 AM round 语义合法表达的组合环必须诊断，不能靠原始
   instruction 顺序碰运气。SCC 之后允许以确定性、可解释的合并规则收缩 atom（当前为
   legacy 移植的 out1/in1/sibling coarsen，见本节末的分块实现）；未来若提出新的
   typed contraction rule，必须另行定义和验证。
4. 按 cost、局部性和硬上限把 atom 划为 block：compute Block 先形成稳定拓扑序并连续
   编号，commit Block 在 Block 空间中构成连续后缀。
5. 对跨 block 的可观察 value 变化创建或复用 watch。同一 (Block, VarId, change kind)
   的 activation edge 组共用一个 detector。每个 materialized `changed` 创建独占、
   `Init = undef` 的 old Variable 和一个 event Variable，并放在所在 Block 尾部、act 之前。
6. state write 只进入 commit Block。同一 register/memory/latch target 存在多个候选
   write 时，由块内文本顺序和 commit 段静态 BlockId 顺序兑现 priority/effect order；
   每个 commit Block 对自身写入的每个 state target 在块尾物化一个共享
   `changed.any target,targetOld`，实际变化经 `act.b` 激活 reader compute Block。
   reader 一律在下一 round 才执行、只看到本轮最终值，因此不存在轮内瞬态暴露。
7. 对外部可写 port 的 watch 在 `B0` 物化；B0 只含 `changed`、派生 event 所需的组合指令
   和 `act.f`。内部 watch 放在其 producer 所在 compute Block 或 writer 所在 commit
   Block。
8. B0/compute Block 内指向更大 compute BlockId 的依赖生成 `act.f`；commit Block 的判变
   event 指向 reader compute Block 生成 `act.b`。commit Block 每轮常扫描，不接收任何
   激活边。同一 event/target 去重，但不合并语义不同的 old 基线。
9. 按 BlockId 顺序写出 ScheduledProgram，丢弃 derived facts，运行最终 validator。

第一次 `eval()` 激活所有 compute Block 是 Machine 语义，不需要额外伪指令。Scheduler
也不能把 `changed.old = current` 的初始化偷偷加入 Program；规范要求 old 使用
`undef`，首次 event 及其影响可能是 AM 层未定义行为。

当前 production scheduler 只采用 compute 和 commit 两类 Block。Scheduling atom 严格等于
instruction dependency graph 的一个 SCC；singleton SCC 就是一个 atom。纯计算、state read、
raw `changed`、DPI/system call 和 `SystemFunction` 都属于 compute；带 state target 的
reg/latch/memory write 属于 commit。DPI/system/effect 顺序和同 target 多写 priority 只形成
有向边，不会把有序序列或 writers 收缩为一个 atom。

分块实现全仓唯一（`lib/grhsim/am/activity_schedule.cpp`，从 legacy 逐行移植）：先对
atom DAG 做 out1/in1/sibling 三路迭代 coarsen（`enableCoarsening` 控制，cluster 指令上限
为 `dpCoarsenBudget`，0 表示自动取 32 × `maxInstructionsPerBlock`），再在确定性拓扑
序列上做 segment DP，以“跨段 incoming 激活 value 数加每段 `dpSegmentPenalty`（默认
1.0）”为代价切成 compute Block。compute Block 受 `maxInstructionsPerBlock`（默认 128）
限制；commit 侧按 normalized event + update guard 聚合分块（对齐 legacy
`commitGuardEventBuckets` 的默认行为），受 `maxCommitInstructionsPerBlock`（默认 4096）
限制；真正的 SCC atom 超限时保留为一个 oversized Block 并报告诊断。commit 跨 target
合并只改变活动粒度，不改变 Block 内拓扑/effect 次序。

commit Block 没有独立的运行时激活通道：每轮 compute 阶段结束后，全部 commit Block 按
BlockId 升序执行一次。写指令按文本顺序读取执行点可见的 operand 和 event，由块内
guard/event 决定是否真正写入；使 visible state 实际变化且存在 reader 时，同块判变 event
激发 `act.b`，激活该 state 的 reader compute Block 进入下一 round。没有 operand 快照、
没有 pending event、没有跨轮保留。

> 历史记录（旧模型，机制已删除）：旧模型曾对 XiangShan commit 路径定位确认，block
> 36995 的 guard/address operands 在每个新 activation batch 都会重新 capture，不存在旧
> operand capture 复用；真正跨批保留的是 pending event，把该旧 event 恢复后再与新
> operands 组合判断，正是 consume-on-event 所禁止的重放。上述 capture/pending event
> 机制已随 2026-07-28 重构整体删除。

### 3.2.1 已实施（2026-07-23）：移除 `isolated` class

`isolated` 是旧实现的保守策略，不是 AM 语义。当前 production scheduler 已删除
`BlockClass::Isolated`、独立 ready queue、`isolated_blocks` 统计以及禁止合块分支。所有非
commit host instruction，包括 `SystemFunction`，进入 compute phase，并可在正常 cap 下与其他
compute instruction 共用 Block。

host instruction 保留其指令内的 firing predicate 和生命周期：`event_mode = immediate`
的 `system.task`/`dpi.call` 按 `condition && ((E = 0) || OR(本次 events))` 判断，
`event_mode = pending` 则把命中的 event 保留到同一次 `eval()` 的后续 round，直到调用
成功；`system.task` 另按 Normal/Once/Final 生命周期执行。GRH lower 后的 system task 和
无 Result observer DPI 使用 Immediate，产生 Result 的 DPI 使用 Pending。host 的执行机会
现在继承合并后 Block 的联合 activation domain，不再继承一个私有 Block 边界。带 event 的
task/DPI 可按对应 mode 过滤无关激活；eventless task/DPI 和 `SystemFunction` 则可能因同
Block 其他成员的 activation 而增加调用次数。这正是当前
“host 作为 compute 合块”策略的明确运行语义：组合活动 Block 被重新激活时可重复
执行 `display`、function 或其他 host instruction，不因此恢复 `isolated`。可观察顺序由
DPI/system/effect 有向边保持；与 lowering 前调用次数是否等价，仍须通过逐次
call-trace differential gate 验证，不能仅由 ScheduledProgram 内部语义推导。

production scheduler tests 已覆盖 implicit/explicit host sequence 的跨 Block 顺序与 cap 内合块、
posedge host 与其他 compute instruction 共 Block，以及 `SystemFunction` 与其普通 producer
合块后随输入变化重新调用。

> 历史记录（旧模型，机制已删除）：同 target writers 回归曾覆盖跨 commit Block 但只有一个
> final watcher、signed one-bit writer guard 的 unsigned event 规范化，以及 `ActBackward`
> 下一 epoch 唤醒 earlier writer 后的同 epoch frontier 传播。另一个 interpreter regression
> 曾构造 `A -> B -> C` runtime frontier chain，其中 B 同时是一个 target 的 final writer 和
> 另一个 target 的 earlier writer，确认逐层 `act.f` 可在同 epoch 到 C，且 scheduler 不物化
> `A -> C` 静态传递闭包。writer-frontier 机制与这些回归已随 2026-07-28 重构删除；新模型下
> 同一 target 多写仅由 commit 段静态顺序表达。

### 3.2.2 2026-07-23 post-`Isolated` XiangShan 实测

> 本节为 2026-07-28 运行时模型重构前的旧模型实测，仅作历史证据保留；其中
> “writer-frontier activations”等统计项对应的机制已删除，不代表当前 scheduler 输出。

完整 `SimTop` 在上述 scheduler 上重新完成 emit、model build 和 difftest emu link：

```text
linear AM instructions                   5,080,563
SCC atoms                                5,080,563
oversized atoms                                  0

compute Blocks                              37,423
commit Blocks                                  515
input sink Block                                  1
normal Blocks                               37,939
normal Blocks plus B0                       37,940

changed detectors                        2,022,159
activation targets                       3,556,634
writer-frontier activations                      0
scheduled instructions                   9,532,818
emitted artifacts                              411
```

本输入的 `writer-frontier activations=0` 不表示该机制不存在；上节所述 focused regressions 已
直接覆盖 split same-target frontier。Full emit 用时 47.22 秒，peak RSS 28,458,200 KiB；model
build 用时 15:20.43，archive 为 843 MiB，model directory 为 3.1 GiB；emu link 用时 1.68 秒，
peak RSS 971,808 KiB，生成的 emu 文件为 503 MiB。

CoreMark/NEMU 的新模型边界为：

```text
-C 100      PASS
-C 571      PASS
-C 572      FAIL, SIGSEGV
-C 2000     FAIL, SIGSEGV
first bad model tick approximately cycleCnt 568
-C 20000    NOT RUN
-C 50000    NOT RUN
```

因此 2k gate 明确失败，并与更小的 `-C 572` 失败边界一致；之后没有继续运行 20k/50k。

### 3.2.3 2026-07-24/25 wide-result shift 修复后的 XiangShan 实测

> 本节为 2026-07-28 运行时模型重构前的旧模型实测，仅作历史证据保留；其中
> “commit groups”、“commit operand captures”等统计项对应的机制已删除，三档
> CoreMark 结果与 host time 属于旧模型功能/性能基线，新模型的 XS gate 重跑后再更新。

consume-on-event v7 产品已通过 2k，但在必须的 20k gate 中于
`cycleCnt=8250` 失败；观测到的五条 RefillBuffer cache line 全为 0，因此当时
没有越级运行 50k。失败的 GRH 链为 1-bit `SliceStatic` -> result 为 514-bit 的
`Shl` -> 514-bit `MemoryWritePort`。旧 AM lowering 先在 1-bit lhs 宽度上执行
shift，再扩宽到 514 bit，因而不可逆地丢失 bit 1..513。该 checkpoint 共有 5,400 个
`kShl`，其中 3,376 个 result 比 lhs 宽，并包含 2,048 个 1 -> 514 形态。

lowering 现在对 `kShl` / `kLShr` / `kAShr` 选择
`BV<result width, lhs signedness>` 作为原生 Type，先 coerce lhs 再 shift；若映射后的
GRH result Signedness 不同，则保留现有的后续 `assign`。常量折叠也采用相同的
先 resize 后 shift 次序。fresh XiangShan JSON 中的 3,376 个 wide `kShl` 没有任何
直接或递归常量 lhs，`kLShr` / `kAShr` 也没有 wide 形态，因此该相邻修复不改变
本次 v8 产品内容。focused regression 覆盖 wide `Shl`、signed wide `LShr`、signed wide
`AShr` 和同形状常量折叠；全部 8 个 `grhsim-am-*` CTest 和新注册的
`transform-const-fold` 均通过。

fresh v8 从 SHA-256
`a2f50b37834dbf97be15f336a6e05ccc59f87a499187f2d15edd78dc1fd727ea` 的 post-stats JSON
重新 lower/schedule/emit，并使用全新 model archive 和 emu：

```text
linear AM instructions                   4,950,236
compute Blocks                              36,963
commit Blocks                                  497
input sink Block                                  1
normal Blocks                               37,461
normal Blocks plus B0                       37,462
changed detectors                        1,875,970
activation targets                       3,218,269
commit groups                                    1
commit operand captures                    256,085
scheduled instructions                   8,992,117
emitted artifacts                              426
```

保存的 lower 日志记录 lower/schedule/emit 用时 40.26 秒，peak RSS 28,027,228 KiB；
当次 `/usr/bin/time -v` 终端记录的 model + emu build 用时为 6:19.79，peak RSS
6,126,112 KiB（未另存 build time 日志）。fresh emu 的 SHA-256 为
`addf9dccfdae7cd2c21620782b99faa2d817d5b1749ea5bd5f3e10f11957d212`。

CoreMark/NEMU 严格在前一档通过后才启动下一档：

```text
-C 2000     PASS, instrCnt=3,     cycleCnt=1996,  host=140574 ms
-C 20000    PASS, instrCnt=14121, cycleCnt=19996, host=1542760 ms
-C 50000    PASS, instrCnt=73580, cycleCnt=49996, host=4178703 ms
             guestCycles=50001, IPC=1.471718, exit=0
```

三档都保持 difftest 开启，没有 mismatch、refill failure、assertion 或 crash。50k 的
instruction/cycle 计数与旧功能基线完全一致；但 host time 为 4,178,703 ms，约为
355,000 ms 旧性能目标的 11.77 倍，因而本轮关闭的是功能 gate，不是性能 gate。

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

生成 runtime 把 compute Block 的激活状态表示为每 64 个 Block 一个 word 的单一
active 位图（不分 current/next，没有 summary 层；commit Block 每轮总是执行，不占用
激活状态）。`eval()` 主循环是两阶段 round：compute 阶段按 BlockId 升序扫描位图，
取位即清并执行对应 Block，`act.f` 置位更大的 compute Block，因严格前向而在同一趟
扫描内被消费；commit 阶段按 BlockId 升序总是执行全部 commit Block，`act.b` 置位
reader compute Block 并置 `backwardFired_`，作为“需要下一轮”的唯一信号。一轮完整
遍历没有任何 `act.b` 激发即收敛；round 计数超过上限（1,000,000）报 did not converge。
首次 `eval()` 仍激活所有 compute Block。跨块消费的 `changed` 结果使用
`set_changed_result` 将实际为真的结果加入 dirty list，每轮结束只清理这些结果，而不是
生成每个 detector 的静态 clear store；同块消费的 result 每次执行都被重写，不进
dirty list。

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
const ExecutableModel&   // (am::ScheduledProgram, ProgramInterface, commitBlockBegin, commitBlockEnd)
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
ScheduledProgram 作为长期实现：旧 session key 只是 emitter side table，不带 AM Program
的 validator 契约与所有权边界。

## 6. 分阶段实现和验收 gates

建议源码按所有权边界摆放：

| 责任 | 位置 |
| --- | --- |
| AM dense ID、SoA storage、Linear/ScheduledProgram 与只读 view | `include/grhsim/am/program.hpp`、`lib/grhsim/am/program.cpp` |
| move-only builders 与 reserve API | `include/grhsim/am/builder.hpp`、`lib/grhsim/am/builder.cpp` |
| linear/scheduled validator | `include/grhsim/am/validate.hpp`、`lib/grhsim/am/validate.cpp` |
| ProgramInterface、SchedulingFacts、stage/pipeline API | `include/grhsim/am/pipeline.hpp`、`lib/grhsim/am/pipeline.cpp` |
| opcode 分类 helper | `include/grhsim/am/opcode_traits.hpp` |
| coarsen + segment DP 分块实现（全仓唯一） | `include/grhsim/am/activity_schedule.hpp`、`lib/grhsim/am/activity_schedule.cpp` |
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
- 以 `ProductionActivityScheduleStage` 打通
  `LinearProgramArtifact -> ExecutableModel`；
- 增加 artifact/interface validator，并补全 Program validator；Program validator 分别执行
  linear-only normal form 与最终规范规则，不能混用；
- 用 scheduled-growth reserve 和 per-arena size/capacity telemetry 验证 synthetic
  changed/act/Variable、activation target 和 block CSR 都可预留、可计量；
- 记录当前 HDLBits、C910/XS 代表用例的行为、emit/compile/run 时间和峰值 RSS；
- 建立覆盖 first eval、edge、反馈、多个 state writer、memory、DPI/system effect 的小型
  differential corpus。

Gate：术语中不存在可执行“single-block Program”；所有预算字段可测；每个已知风险
至少有一个预定 test oracle；notepad 包含 Phase 1 待办。

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
前释放，且无 per-op heap allocation。

### Phase 2：AM activity scheduler

- 实现紧凑 facts、block partition、B0、watch、`changed` 和 `act.f/act.b` materialize；
- 实现 ScheduledProgram validator 和确定性摘要 hash；
- 在小设计上用规范解释器或直接参考执行器比较全量执行与 activity 执行。
- 正式 scheduler 必须产生多 Block reader 精确激活并通过
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
| 首次求值 | 旧路径 seed compute/activity；AM 先执行 B0，首次求值额外激活全部 compute Block（commit Block 每轮常扫），old 为 `undef` | 不把旧 baseline 强加给 AM；reset 后行为与首次未定义边界分开测 |
| round 结构 | 旧路径按 batch 调 compute、每 round 扫 commit；AM 对齐同一两阶段 round：compute 按 active 过滤升序执行，commit 常扫描，任一 `act.b` 激发即要求下一轮 | feedback 收敛、写回可见时点和执行次数符合 AM |
| 激活边 | 旧代码依赖 topo active id 和 batch 内局部传播；AM `act.f` 只指向更大的 compute BlockId 且不出现在 commit Block，`act.b` 只在 commit Block、target 为 compute Block | 每条激活边的放置与 target 范围经 validator 证明 |
| event 生命周期 | 旧 event edge slot 通常按 fixed-point round 清零；AM 跨块消费的 changed result 在 round 末清零（同块消费不清），B0 每次 eval 执行 | pos/neg、同轮多消费者和跨轮 event 不丢失或重复 |
| state write | 旧 commit supernode 每 round 扫描；AM reg/latch/mem write 同样位于每轮常扫的 commit Block，读取执行点 operand，由块内 guard/event 决定写入 | 多写 priority、mask/fill、read-during-write 和 reader reactivation 一致 |
| memory 划分 | 旧 `kMemoryReadPort` 是 source-class 且可能 clone；新层看到显式 `mem.read` 和 Array | address 依赖保留；有副作用/可能 alias 的访问不被非法复制或重排 |
| 外部输入 | 两条路径都应只观察两次 eval 间的最终值，但 seed 机制不同 | 0->1->0 后再 eval 不产生虚假变化 |
| DPI/system | 旧 supernode/batch 和 full-pass fast path 可能改变调用次数；AM 要保持 Block、schedule、once/final 顺序 | 逐次调用 trace、参数/返回 ABI 和 finalize 顺序一致 |
| value equality | 旧 emitter 有宽值/packed-array 专用优化；AM `sameValue` 对 BV、Real bit pattern、String、Array 有统一定义 | NaN bit pattern、宽值最高字 mask、Array 更新的 changed 判断一致 |
| 非收敛 | 旧路径无收敛上限；AM 保留 round 上限作为实现保护（解释器 `maxRounds`、生成代码固定 1,000,000） | 不把未收敛静默当成功；诊断能定位 Block/round |
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
