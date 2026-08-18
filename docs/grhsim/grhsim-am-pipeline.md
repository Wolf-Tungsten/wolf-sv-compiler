# GRHSIM-AM lowering、调度与 C++ emit 流水线

本文定义 GRHSIM-AM 新流水线的代码框架和分阶段落地边界。它是实现设计，不是
[GRHSIM-AM 规范](grhsim-am.md)的语义修订。规范中的 `Program` 始终表示可以交给
`Machine` 执行、已经具有 `B0`/`B1+`、`changed`/`act` 和完整 `eval()` 语义的最终程序。

目标流水线是（阶段术语以 NO0004 框架为准）：

```text
normalized GRH
    -- lowering-to-am-graph        --> am::AmGraph（GRHSIM AM Graph，一等工作 IR，原生建图）
    -- opt-am-graph                --> optimizeAmGraph（fold/assign-alias/memfold/cse 不动点 + DCE）
    -- split-am-graph              --> am::AmComputeGraph + am::AmCommitGraph（atom 级诱导子图）
    -- opt-am-compute-graph        --> mux-merge atom pass（NO0006：同 select mux 组 + 独占锥 atom 化）
    -- partition-am-compute-graph  --> am::AmComputeActivityGraph（活动度划分）
    -- partition-am-commit-graph   --> am::AmCommitEventGraph（事件聚类）
    -- materialize                 --> am::ScheduledProgram（GRHSIM AM Program）
    -- emit                        --> C++ 模型
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

> 实现进展（2026-08-13，emit-cost NO0011）：lowering 常量折叠 + 常量写合并。
> lowering-to-am-graph 新增构建期常量折叠 peephole——纯 op 的操作数全为字面
> 常量时直接求值并 intern 为常量变量（求值器与 opt-am-graph 共用
> `grhsim_am_const_eval.hpp` 的 `evaluatePure`，与解释器逐位一致），
> `coerceToType` 常量直通；仅当结果值的全部消费者都在生产者之后
> （`firstUseOrdinal_` 守卫，乱序消费安全）且不带接口角色时折叠。
> `emitScalarWrites` 新增同 cond、常量 mask+data 的连续写合并（顺序 blend
> 折叠为单条常量映像写）。针对 gsim 导出器把向量寄存器逐元素写物化为全宽
> slice/concat 重建链的形态（NO0010 #2-#12 巨 atom），香山全图 folded_ops
> ≈49.97 万、merged_state_writes ≈3.34 万，coremark 50k difftest 逐位一致，
> host 692s→486s。escape hatch：
> `WOLVRIX_GRHSIM_AM_DISABLE_LOWERING_CONST_FOLD` /
> `WOLVRIX_GRHSIM_AM_DISABLE_WRITE_MERGE`。

> 实现进展（2026-08-08，NO0015）：`absorbFanoutAtoms` 落入 opt-am-compute-graph
> （tree-atom fold 之后、导出与分区之前）——消费方 atom ≥2 且成员指令数 ≤
> `fanoutAbsorbMaxInstructions`（**默认 0=禁用**：指标收益与指令复制量近似
> 线性地转化为 host 成本，cap48/×1.0 实测 +164%，待孤本开销治理后再评估
> 默认）的 compute atom 吸收进全部消费方 atom（等价于 atom 建立前按消费方
> 复制指令锥 + fold 吸收；消费方变胖，不产生新分区单元）。屏障：非纯成员、
> 多结果根、ordered-effect 触及、comb-loop/commit/Activation/Host 消费方；
> pinned 或带 commit 消费方的根保留孤本 atom。预算 `fanoutAbsorbBudgetMult`
> （×compute 指令总数）耗尽即停，单 atom 消费方上限
> `fanoutAbsorbMaxConsumers`（默认 256）。rewire 采用延迟回放（规则记录→
> 重建期对最终成员表统一回放）。香山生产锚点档：cross_values_compute_network
> 396,008→192,054（2.22x→**1.078x**，过 1.10x 达标线）@ cap2/预算×2.0。
> CLI/Makefile：`--fanout-absorb-max-instructions` /
> `--fanout-absorb-budget-mult` / `--fanout-absorb-max-consumers` 与
> `XS_WOLF_GRHSIM_AM_FANOUT_ABSORB_*`。
>
> 实现进展（2026-08-12，supernode-align NO0018）：分区映射校准落成。
> atom→block 与 gsim node→supernode 的贴合度以 pair-F1 度量（join 口径见
> NO0009/NO0018）：校准点 cap15 + coarsen 预算 7000 atoms + mergeWhen 关停
> （flatten 图上 gsim mergeWhenNodes 实质死亡，remove 仅 1,183）达
> pair-F1 0.9255（基线 0.4260），已锁定为 grhsim-am-lower-json 默认。
> 融合锚点改为 DP 后按块内同 select 局部计算（`atomFusionAnchor`），与
> mergeWhen 彻底解耦——mergeWhen 关停不再损失 emitter 块级 mux-run 融合。
> state-anchor 扫描（value 图寄存器边的虚拟锚点重建：读锚点按状态变量
> 归组/写锚点按 commit atom 归组/有效度数守卫）以
> `--dp-state-anchor-mode`（0 关 / 1 仅读归组 / 2 全量）保留为实验装置，
> 两种模式均被 pair-F1 否决（0.8935 / 0.8268），默认关。新增
> `WOLVRIX_GRHSIM_AM_CLUSTER_ASSIGN_JSONL`（atom→coarsen cluster 导出）。
>
> 实现进展（2026-08-08，NO0008）：atom 单输出树化 + mergeWhen 降为 coarsen，
> 取代 NO0006 的 mux-merge atom 路线（该路线经 supernode-align NO0014 诊断为
> 层级错误：多输出 atom 破坏链合并并制造跨块扇出）。**P1**：
> `foldSingleOutputTreeAtoms` 取代 `mergeMuxSelectAtoms`——compute 侧单用纯
> 生产者折叠进唯一消费者的 atom，每个 atom 成为单输出表达式树（mux 根树即
> when 树，对齐 gsim node=信号+assignTree）；屏障为 commit 侧、comb-loop SCC、
> 非纯副作用与 pinned 结果；`AmAtomKind::MuxMerge` 退役，新增 `Tree`；mux 根
> compute atom（含 Singleton）的 signature 记 select 变量 id，其余记
> `kInvalidAtomSignature`。`muxAtomMax` 参数链整体移除。**P2**：mergeWhen 作为
> coarsen 首个 sweep 落入 partition-am-compute-graph（gsim mergeWhenNodes 移
> 植：按 select 生产者锚点归组、拓扑波前就绪门控、组尺寸 >
> `mergeWhenMinGroup`（默认 5，<2 关闭）才合并；只动 DSU 归属，不改 atom）；
> 组内成员共享融合锚点，materialize 的块内 Kahn 以锚点为序键使同组 mux 根
> atom 相邻。**P3**：emitter 改块级 mux-run 融合（`planMuxFusionRuns`）：相邻
> 同 select 的 mux 根 atom 成 run，锥成员按序前置、根 mux 合并为单 if/else；
> run 在 select 变化、非合格 atom 或锥引用前序 run 根结果（防 use-before-def）
> 处断开；select 从根指令推导，不依赖 signature。CLI/Makefile 旋钮：
> `grhsim-am-lower-json --merge-when-min-group` /
> `XS_WOLF_GRHSIM_AM_MERGE_WHEN_MIN_GROUP` / wolvrix_xs_grhsim_am.py 同名透传。

> 实现进展（2026-08-07，NO0007 P3）：分区经济模型换 atom 计权——compute 分区
> `member[rid]=1`（atom 即尺寸单位，mergeLimit/DP maxNodes/refinement 块尺寸随之
> 全部按 atom 计），commit 桶合并主限改 `maxCommitAtomsPerBlock`（atom 计）并保留
> `kMaxGuardEventMergeOps=4096` 指令数二级护栏（emitter guard/event merge op 语义）。
> 全链路命名诚实化：`maxAtomsPerBlock`/`maxCommitAtomsPerBlock`/`dpCoarsenAtomBudget`
> （默认值 128/4096/256 不变），CLI 对应 `--max-atoms-per-block`/`--dp-coarsen-atom-budget`，
> Makefile `XS_WOLF_GRHSIM_AM_MAX_ATOMS_PER_BLOCK`/`XS_WOLF_GRHSIM_AM_DP_COARSEN_ATOM_BUDGET`。
> 指令图 JSONL 导出上移到编排器（mux-merge pass 之后，`atom`/`comb_loop_atom` 为
> 后 merge 口径）；块归属导出 assign 记录加 `atom` 字段、block 记录加 `atoms` 字段。
>
> 实现进展（2026-08-07，NO0007 P1+P2）：atom 升为一等公民。P1：
> `ScheduledProgram` 新增 atom 层（block→atom→instruction 三层视图 +
> kind/signature 元数据；`AtomId`、`AmAtomKind`、四个 atom arena），builder
> `beginAtom/endAtom` + 隐式 Singleton 兜底（现存调用点零改动），split 阶段
> 标注 Singleton/CombLoopScc/CommitEvent，`mergeMuxSelectAtoms` 重建携带
> kind/signature（MuxMerge+select id），materialize 按 atom 写入，
> validate 新增 atom 不变量（tiling/kind 形状/范围放置）；发射产物字节级
> 不变（T512 diff 全同）。P2：emitter 直消费 atom 层——MuxMerge atom 两相
> 发射（cone 成员按成员序普通发射，臂成员归入单 if-else），删除
> `planMuxRuns` 块内模式识别与兜底（cap 跳过/未归组的相邻同 select mux
> 不再融合，行为变化已钉测试）；统计口径更名 `mux_atom_fused`。
>
> 实现进展（2026-08-07，NO0006）：opt-am-compute-graph 迎来首位住户——mux-merge
> atom pass（`mergeMuxSelectAtoms`）：compute 侧同 select 的 mux 组（成员均为单
> 指令 atom）连同其独占使用的生产者锥合并为不可分割的调度 atom，吸收锥按拓扑
> 序在前、组内 mux 连续在后；cap（`ActivityScheduleOptions.muxAtomMax`，默认
> 512，0 关闭）限制 atom 总尺寸、超限整组放弃（激活粒度保护，非分区可行性）；
> comb-loop atom 不可拆、select 的锥不吸收；合并后重建 atom 表/atom DAG 并做
> SCC 解合并兜底。C++ emitter 新增块内同 select 连续 mux 段的 if-else 融合
> （select 只求值一次，宽/窄两臂都支持），cap 跳过或被分区拆散的组自然退化为
> 逐条三元。CLI `grhsim-am-lower-json --mux-atom-max` 与 Makefile
> `XS_WOLF_GRHSIM_AM_MUX_ATOM_MAX` 透传。
>
> 实现进展（2026-08-07，NO0005）：状态写指令改为 cond/mask 变体族，勘误 NO0001 的
> 「reg.write/latch.write cond/mask 数据化」决策。`reg.write/latch.write/mem.write`
> 各派生 `.c/.m/.cm` 三个显式 opcode 变体（共 12 个），操作数布局统一为
> `[cond?, addr?（仅 mem）, mask?, data, target, events...]`；lowering 按 cond/mask
> 常量性逐写口选型（常量 0 整条消除、常量 1/全 1 不占操作数位），`targetSnapshot`
> 快照 assign、read-old blend 与自 mux 三件套整体删除——「条件不触发 → 保持旧值」由
> 不写保证，mask 混合是 commit 局部的读旧值位混合。常量 mask 在 C++ emitter 端折叠为
> 立即数。纯 latch commit Block 的 gate 改为 watch 每条 latch 写的
> cond?/mask?/data 操作数。AM 套件 12/12。
>
> 实现进展（2026-08-06，NO0004）：流程框架与术语统一。阶段命名以
> `pdocs/grh-notepad/am-graph/NO0004` 为准：lowering-to-am-graph → opt-am-graph →
> split-am-graph → opt-am-compute-graph（空阶段预留）→ partition-am-compute-graph /
> partition-am-commit-graph → materialize → emit；产物类型对应为 `AmGraph` /
> `AmComputeGraph`+`AmCommitGraph` / `AmComputeActivityGraph` / `AmCommitEventGraph` /
> `ScheduledProgram`（GRHSIM AM Program）。
>
> 实现进展（2026-08-06）：转换方向修正落地（NO0003）——AmGraph 升为阶段间一等 IR。
> lowering 原生建图（不再先产 LinearProgram 再转换）；optimize 移植到图上；
> scheduler 直接消费图，线性 AM Program 只在 finalize 由 `toLinearProgram()` 物化。
> `fromLinearProgram`/`toLinearProgram` 保留用于测试与物化。香山指令图导出、
> block assignment、C++ 发射三份产物与改造前逐字节一致，AM 套件 10/10。
>
> 实现进展（2026-08-05）：AM 执行模型五条升级与转换路径重构已落地（香山 difftest
> 73,580/49,996 通过，host 324-326s vs opt1 基线 317.6s）。lower 校验后的
> `LinearProgramArtifact` 先经 `AmGraph` 无损往返 + 语义校验再进 optimize；
> production scheduler 入口即把产物摄入 `AmGraph`（见 2.4），全部分析只读图存储。
> `reg.write/latch.write` 的 cond/mask 合并进 nextValue；`mem.write` 保留
> cond/mask 操作数（禁用写整体抑制，无 read-old 链）；commit 块按事件签名聚合、
> 首部 gate detector 合并为一次 if 门控、按激活位过滤执行；commit 锥打包短寿后即
> 移除。完整经过见 `pdocs/grh-notepad/am-graph/NO0001`。
>
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
> 实现进展（2026-07-29）：三项对齐落地。① 运行时 dispatch 整体替换为 legacy
> batch 形态：`eval_scan_*()` 静态调用 + 8 Block/byte chunk 活动字节快照 + 升序直线
> 位测试 + 内联 body + 同 byte 前向 act 局部接力，commit 段 `eval_commit_*()` 无条件
> 内联，废弃 `execute_block` 三级动态分派与位图扫描主循环。② coarsen budget 自动
> 公式从 32×cap 改为 1.5×cap（=192）：32× 在单指令 atom DAG 上跑满收敛产出
> (128,4096] DP 不可拆 oversized singleton（XS 9,415 compute 块、均值 ~470 指令），
> 1.5× 恢复 legacy 粒度（33,738 vs 31,534）；扫参矩阵与机制分析见
> `grhsim-am-vs-legacy-analysis-20260727.md` §8.2。③ 持久化窄值改为独立类成员
> `v<VariableId>`（XS 6.64M 个，gsim 形态）；跨块 changed result 单列密集
> `changedResults_`（唯一运行时下标访问）；生成 Makefile 带 clang PCH 与
> `ifeq ($(origin CXX),default)` 兜底。期间发现 clang 22.1.2 对数万以上 `{}` 初始化
> 成员的隐式默认 ctor 静默截断初始化（XS 上 stringValues_ 未构造 → init() SIGSEGV），
> 修复为成员不带初始化式 + init() 单条 memset 清零连续成员区。
> Gate 状态：ctest AM 8/8、全量 54/57（3 个既有无关失败）；xs-components
> 053/044/100 各 20,000 向量与 legacy bit-exact；XS difftest 2k/20k 双配置通过、
> 50k 通过（退休指令数 73,580、IPC 1.471718 与 legacy 一致）。host time（干净机）：
> 最终 b192 = 2k 81,760 / 20k 905,050 / 50k 1,982,820 ms，对比 07-28 基线
> 50k −26.1%；对 legacy 同窗口 50k 为 11.7x（07-28 为 15.8x）。静态 dispatch 在同
> 块数下仍净回退（+19~22%，扫描胶取指开销，与 P4/P4.5 同机制），由块细化抵消并
> 反超；成员存储在 20k 再贡献 −1.4~1.8%。剩余差距方向：commit operand capture、
> detector 密度、commit 段每轮无条件全扫（分析文档 §6/§7.5）。
>
> 实现进展（2026-07-29，ST00010 detector 分组折叠）：分析文档 §6 的 P2' 方向
> 落地为 emitter 侧 peephole（IR/scheduler/validator 不动）。对每 Block 尾部的
> (changed.*, act.f/act.b) watch-group run，按激活目标签名（方向 + 排序 target
> 集合）在发射期重新分组：组内 detector 的比较无分支地累加进块局部标志
> `detGrp_N`（old 基线仍逐 detector 私有、原位置更新），每组一次合并写掩码；
> 纯同 byte relay 的 forward 组进一步退化为无分支形式
> `byteFlags |= -detGrp & mask`（legacy deferred-group 惯用法）。被折叠 event
> 变量不再有成员（XS 规模下约省 2M × 8B 静态状态）。折叠条件：窄标量、非跨块
> changed result、event 全部 use 都是同 run 的 act、操作数不定义在 run 内。
> Gate 状态：ctest AM 8/8、全量 54/57（3 个既有无关失败）；xs-components
> 053/044/100 各 20,000 向量与 legacy bit-exact。折叠覆盖：053 案 1,594/1,636
> detector 折为 308 组（58 组无分支 relay）、044 案 1,793/1,843 折为 327 组。
> XS difftest 2k/20k 与 host time 对照（b192 基线 81,760 / 905,050 ms）进行中，
> 结果补记于 `pdocs/grh_notepad`。
>
> 实现进展（2026-07-30，ST00011 数组写点激活）：根因（gprofng @ AM emu 2k）：
> commit 块尾部 1,667 个全数组 changed.any 每轮对每个被 watch 的 memory 做一次
> memcmp + 一次 memcpy 基线回写（libc 热点 ~23%）。emitter peephole（IR/
> scheduler/validator 不动）：mem.write 在写元素时顺带判变
> （masked_write_words_detect），mem.fill 在填充循环内逐元素判变
> （assign_words_detect / slice_words_detect，保持"重填相同值不再激活"的
> 收敛语义），尾部 detector 改读块局部标志 arrChg_N，memcmp/memcpy/基线
> 存储全消。等价性：每个写数组的 commit 块都自带 (块, target) detector 且
> act.b 覆盖全部 reader，只检测本块自己的写仍完备；跨块基线漂移触发本来就
> 是冗余重复（激活幂等）。XS 规模 std::equal 1,667 → 0（detect 调用点
> 6,951）。Gate：ctest AM 8/8；xs-components 053/044/100 各 20,000 向量与
> legacy bit-exact；XS difftest 2k/20k 通过（instrCnt 与历史一致），干净机
> host ms 2k 25,366 / 20k 569,934 / 50k 1,045,557（对 ST00010 2k 2.07x、
> 20k 1.04x；主要消掉 reset/首评尖峰）。
>
> 实现进展（2026-07-30，ST00012 commit 事件批次门控）：每个 commit 块的全部
> 写/判变/act 包进一条批次事件检查（事件操作数去重后的 OR，legacy
> commit-batch 惯用法）：被 watch 事件全静默的轮次（如负沿 eval）整块一次
> 分支跳过，替代逐语句事件加载。块内无事件写、latch 写或事件在块内产生的块
> 保持原样。emitter peephole（IR/scheduler/validator 不动）。Gate：ctest AM
> 8/8；xs-components 053/044/100 各 20,000 向量与 legacy bit-exact。
>
> 实现进展（2026-07-30，ST00013 标量写点判变融合，分析文档 P5 的 emitter
> 落地）：RegisterWrite 写点比较 next != target，真变才写并置 wrChg_N
> （窄路径写抑制、宽路径 masked_write_words_detect），尾部 detector 改读标志
> （ST00010 组累加或 event 变量），old 基线死亡；多写 target 跨写点 OR 累加
> （多余激活为规范允许），写回相同值不再激活（收敛性不变）。XS 尾部标量比较
> 277,431 → 67,394（−75.7%）。Gate：ctest AM 8/8；xs-components 053/044/100
> 各 20,000 向量与 legacy bit-exact。
>
> 组合（ST00011+12+13）XiangShan 干净机 host ms：2k 22,431 / 20k 279,039 /
> 50k 757,952，difftest 全过且 instrCnt/IPC 与 legacy 完全一致；对干净重测的
> ST00010（52,431 / 592,416）2.34x / 2.12x（07-29 的 905,050/1,982,820 经
> 干净重测证伪为并发构建污染）；对 legacy 20k 6.15x、50k 4.47x。剩余差距
> 主体转向 compute 侧（detector 密度 ~2.0M 站点、跨块值判变、宽值 helper）。
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
模型 ABI。当前 AM emitter 不支持 waveform，AM target 会显式拒绝该选项，
而不是回退到 legacy emitter。

AM emitter 的可观测性/实验属性（`GrhSimAmCppOptions.attributes`，CLI/脚本/Makefile 已接线）：

- `runtimeProfile`（`--runtime-profile` / `XS_WOLF_GRHSIM_AM_RUNTIME_PROFILE=1`）：
  编译进逐块 exec 计数、compute/commit 相计时与激活/变更计数器；运行期
  `EMU_RUNTIME_PROFILE=1` 启用，`dump_runtime_profile()` 在仿真结束时输出汇总与
  top-32 块；设置 `EMU_AM_BLOCK_EXECS=<path>` 时额外导出全量逐块 exec 文件
  （"block kind execs" 行，block 0 为 entry 块，其 execs 即 eval() 调用次数）。
- `fullEvaluation`（`--full-evaluation` / `XS_WOLF_GRHSIM_AM_FULL_EVALUATION=1`）：
  扫描/commit 的 byte chunk 以 ownedMask 代替活动位快照，所有块无条件执行；
  块体的激活簿记保留，用于测量活动过滤本身的价值（NO0017 oracle）。
- `branchlessActivation`（`--branchless-activation`，默认 off）：条件激活合并
  （act.f/act.b 与折叠 detector-group 合并共用 `emitActivationMerge`）去分支化——
  条件一次求值后符号扩展成全 1/全 0 字掩码，各目标活动字无条件
  `activeWords_[w] |= mask & actMask`，act.b 的 `backwardFired_` 改逻辑或累积。
  以恒写活动字为代价消除静态 ~455K 站点 / 动态 ~4.8G 次的数据相关分支
  （NO0017 块级 profile）；`runtimeProfile` 开启时自动回退分支形式以保持
  激活计数口径。off 时输出与既往逐字节一致。
- `changedTrace`（`--changed-trace` / `XS_WOLF_GRHSIM_AM_CHANGED_TRACE=1`）：
  运行期设置 `EMU_AM_CHANGED_TRACE=<path>` 后，按 (eval, round) 流式写出
  二进制 changed 变量记录（3×uint64 头 + count×uint32 id），窗口由
  `EMU_AM_TRACE_BEGIN_EVAL`/`EMU_AM_TRACE_END_EVAL` 限定。注意其数据源是
  跨块 changed-results 的 mark 列表，不含已被 detector-group folding 合并的
  检测器，不能作为全量变更源。
- `resizeElision`（`--resize-elision`，默认 off）：同宽无符号 `resize_value`
  胶消除。开启后 `resizedExpr` 对 `sourceWidth == targetWidth && !signExtend`
  的操作数直接发存储引用（跳过 `resize_value(x, N, false, N)`，其语义仅为
  `x & mask(N)`），覆盖全部调用点（二元操作数、窄 `assign`、比较、Mux 两臂、
  mux-run 融合臂）；有符号同宽与异宽站点保留原样。安全性来自写侧掩码不变量：
  每个已存储窄标量（块内局部 `local*_N`、跨块 `changedResults_[k]`、持久成员
  `v<K>`）的所有写入点都按声明宽截断——`resultAssign`/`assignVariableStatement`/
  Mux 臂/wideSliceAssign 显式 `& mask(bitWidth)`，mem.read 窄结果、reg.write 窄
  路径、DPI integral commit、init（常量/随机）同样掩码，`changedResults_` 只存
  0/1 事件标志；外部输入端口在 `eval()` 入口以 `v<K> = uint64(port) & mask`
  截断，TB 写入的高位垃圾不会进入模型；`valueExpr` 只会产生上述三种存储引用，
  无表达式形式，故同宽 resize 恒为 identity。`sourceWidth == targetWidth == 64`
  被同一规则自然覆盖（掩码恒等，无需特判）。off 时输出与既往逐字节一致。
- `inlineScalarHelpers`（`--inline-scalar-helpers`，默认 off）：把窄标量的
  `slice_value`、逻辑/算术移位和 `signed_value` 从 runtime TU 的外联定义改为
  生成头文件内的 `constexpr` 定义，使各 Block TU 可做跨调用点常量传播与内联。
  不改变 helper 语义；除法、取模以及宽值/数组 helper 仍保持 outlined，避免把
  大循环复制进海量调用点。当前 CoreMark 模型静态覆盖 424,064 个 slice、
  107,130 个逻辑移位及少量 signed/算术移位站点。off 时仍发原 runtime 定义。
- `inlineScalarConstants`（`--inline-scalar-constants`，默认 off）：把
  `InitKind::Constant` 且宽度不超过 64 bit 的不可写 BitVector 在读取点直接发射为
  按声明宽度掩码后的 `UINT64_C(...)` 字面量，让各 Block TU 可继续常量传播并消除
  模型对象 load。AM validator 禁止 instruction 写常量变量；需要可寻址 word 数据的
  helper 仍使用常量变量的真实存储，`init()` 与输入端口写入同样明确绕过字面量路径，
  因而不会生成 `&UINT64_C(...)` 或向字面量赋值。宽常量和其他 init kind 保持原样，
  off 时输出与既往一致。
- `inlineScalarConstantStorageElision`（`--inline-scalar-constant-storage-elision`，默认
  off）：与 `inlineScalarConstants` 同时开启时，进一步删除没有全局/提前读取、没有
  地址需求且最多只跨 Block 按值读取的窄常量的 `v<K>` 成员和 `init()` store。状态写、
  端口、host/DPI、memory/array helper 以及其他需地址的路径会被 escape/pin 分析排除；
  读取仍由上项发射字面量，因此该开关不单独改变未满足安全条件的常量。

> 历史基线（2026-07-22）：早期 single-TU full emit 为 5,080,563 条 linear 指令、
> 9,574,478 条 scheduled 指令、1,021,857 个 Block 和 2,040,184 个 detector，生成
> 1,679,120,625-byte C++ TU。这些数字仅保留为旧 emitter/scheduler 证据，不是当前结果。

## 1. 为什么单线性块不能叫 Program

规范中的 `Program` 不是“若干 AM 指令的容器”。它至少已经满足以下可执行契约：

- `B0` 是每次 `eval()` 无条件执行的 EntryBlock；
- 普通 Block 分为 compute 段和构成连续后缀的 commit 段：两类 Block 都只在 active
  时执行（首次求值激活全部 Block），commit Block 的激活只来自其首部 gate detector
  watch 的变量；
- `changed` 有独占的 `old` Variable，并在比较后更新基线；
- `act.f` 严格前向（目标 BlockId 更大，compute 或 commit Block 皆可），并在同一趟
  升序扫描内被消费；`act.b` 指向非 EntryBlock 的任意 Block（scheduler 只在 commit
  Block 尾部产生，目标不大于源块），其激发是要求下一 round 的唯一信号；
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

## 2. 长期数据契约与工作图形式

### 2.1 `am::LinearProgram`

`LinearProgram` 是 AM 指令的线性载体。自 2026-08-06 起，阶段间的流通货币是
`AmGraph`（2.4）：lowering 原生建图、optimize 在图上改写、scheduler 全程读图；
`LinearProgram`（连同 `ProgramInterface` 与 facts）是图在 finalize 时刻的物化形式，
`LinearProgramArtifact` 类型保留给物化产物与测试构造：

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
Block。state write 只允许位于 commit Block；commit Block 与 compute Block 一样按
激活位过滤执行（首次 `eval()` 激活全部 Block），其激活只来自首部 gate detector
watch 的变量。

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

### 2.4 `am::AmGraph`（一等工作图 IR）

`AmGraph`（`include/grhsim/am/grhsim_am_graph.hpp`）是 lowering 与 emission 之间的一等工作
IR，也是阶段间的流通货币：instruction 为 op、Variable 为 value。**lowering 原生建图**
（`GrhIRToGrhSimAMGraphLowering::lower` 直接返回 `AmGraph`，构建路径上不存在线性中间体）；
optimize 在图上就地改写；scheduler 全程读图，只在 finalize 时刻经
`toLinearProgram()` 物化出线性 `LinearProgram` 再构造 `ScheduledProgram`。
`fromLinearProgram`/`toLinearProgram` 是无损转换（node/variable id 一一保留，未经
改写的图往返字节级一致），保留给测试构造与 finalize 物化。图的特性：

- Variable 原生携带声明语义：`AmValueKind` 区分 Comb/Constant/Input/State，State 再以
  `AmStateKind` 细分 Register/Latch/Memory；init、role 不再只存于调度侧表；
- 引用 State 的操作数边带 `AmStateAccess` 分类（`PreCommit` 读本轮 commit 前快照，
  `Live` 读在飞值、只在属主 commit 锥内合法）——破环点在 IR 上显式可见，不再是
  隐式调度约定；未标记的边一律视为 PreCommit；
- 支持增删指令（删除打墓碑、id 稳定）、操作数重连、墓碑压缩与 per-instruction
  effect/role 视图；ordered-effect 组作为图级事实随行，供合并/复制类 pass 改写；
- 构建面与 `LinearProgramBuilder` 平价（reserve/init 系列/literal/DPI/Attribute），
  lowering 与 optimize 的 compact 重建都直接用它。

校验路径与线性时代一致：`validate(const AmGraph&)` 在图存储上跑同一套 linear 级
语义检查（`validate(ProgramView)`）加 interface/facts 对齐检查。

## 3. AM activity scheduler

### 3.1 输入为何更适合访存分析

在 LinearProgram 上，访存已经呈现为明确的 AM 对象：

- 一个 Array Variable 表示 memory storage，元素 Type 和固定 depth 已知；
- `mem.read` 显式给出 memory 与 address；
- `mem.write` 显式给出 cond、address、mask、data 和 event；`mem.fill` 显式给出已合并
  写入条件的整片 data 和 event；
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
   `Init = undef` 的 old Variable 和一个 event Variable，并放在所在 Block 的 act
   之前；commit Block 的 gate detector 放在块首部、全部状态写之前。
6. state write 只进入 commit Block。commit Block 的结构固定为 [首部 gate detector] +
   [状态写] + [尾部 watch + act]：首部为块内每个事件签名元素 (kind, raw) 一条克隆
   `changed.*`（写指令的 event operand 重指向这些块内 detector 的结果；纯 latch 块
   每条 latch 写的 cond?/mask?/data 操作数各一条 `ChangedAny`），门控为全部首部
   detector 结果的 OR，整块只判一次，状态写与尾部 watch/act 都在门内。同一
   register/memory/latch target
   存在多个候选 write 时，由块内文本顺序和 commit 段静态 BlockId 顺序兑现
   priority/effect order；每个 commit Block 对自身写入的每个 state target 在块尾物化
   一个共享 `changed.any target,targetOld`，实际变化经 `act.b` 激活 reader compute
   Block。reader 一律在下一 round 才执行、只看到本轮最终值，因此不存在轮内瞬态暴露。
7. 对外部可写 port 的 watch 在 `B0` 物化；B0 只含 `changed`、派生 event 所需的组合指令
   和 `act.f`。内部 watch 放在其 producer 所在 compute Block 或 writer 所在 commit
   Block。外部输入同时是 commit Block 的激活来源之一：commit gate detector watch 的
   输入变量（无定义指令）经 B0 的 `ChangedAny` watch 前向激活对应 commit Block。
8. 指向更大 BlockId 的激活依赖生成 `act.f`，指向不大于源块的依赖生成 `act.b`：B0 与
   compute 段内的 watch/def-use 依赖生成 `act.f`；commit Block 的尾部判变 watch 指向
   reader compute Block 生成 `act.b`。commit Block 不按 def-use 数据边激活：compute→
   commit 的 `act.f` 只携带被目标块首部 gate detector watch 的变量；commit→commit
   经 writer 块的共享尾部 watch，按块序前向（同轮）或后向（下轮）。同一 event/target
   去重，但不合并语义不同的 old 基线。
9. 按 BlockId 顺序写出 ScheduledProgram，丢弃 derived facts，运行最终 validator。

第一次 `eval()` 激活所有 Block（compute 与 commit）是 Machine 语义，不需要额外伪指令。Scheduler
也不能把 `changed.old = current` 的初始化偷偷加入 Program；规范要求 old 使用
`undef`，首次 event 及其影响可能是 AM 层未定义行为。

当前 production scheduler 只采用 compute 和 commit 两类 Block。scheduler 入口直接
消费 `AmGraph`（2.4， lowering/optimize 的产物就是图），下文所述 def-use、ordered effect、
SCC、coarsen/segment DP、门控与激活分析全部读图存储；Scheduling atom 严格等于
instruction dependency graph 的一个 SCC；singleton SCC 就是一个 atom。纯计算、state read、
raw `changed`、DPI/system call 和 `SystemFunction` 都属于 compute；带 state target 的
reg/latch/memory write 属于 commit。DPI/system/effect 顺序和同 target 多写 priority 只形成
有向边，不会把有序序列或 writers 收缩为一个 atom。

分块实现全仓唯一（`lib/grhsim/am/grhsim_am_graph_split.cpp` 与
`lib/grhsim/am/grhsim_am_{compute,commit}_graph_partition.cpp`）。AM 图
摄入并完成 atom（SCC）分类后，先做一次 **compute/commit 分图**
（**split-am-graph**，`splitAmGraph`）：atom DAG 拆成两张诱导子图（局部 id 保持全局相对
次序，跨类边不属于任一子图），commit→compute 的依赖在此判非法；之后两个分区 pass
各自独立处理一张子图：

- **compute 子图按连通性划分**（**partition-am-compute-graph**，`partitionAmComputeGraph`，
  自 2026-08-06 起为 gsim 风格连通性算法，前身是 out1/in1/sibling 迭代 coarsen +
  activation-cost segment DP）：三个阶段——①coarsen：`mergeOut1` 逆序单遍（出度 1 并入
  最早后继）、`mergeIn1` 正序单遍（入度 1 并入最晚前驱）、`mergeSublings`（前驱集完全相等
  分组合并、宿主上限 30），宿主成员上限取 `dpCoarsenAtomBudget`（0=默认 256，**atom 计**，
  上限越大跨边越少但超大块运行时尾巴越重，见 compute-partition NO0004 §8），合并后确定性
  拓扑重排；②Kernighan 边割 DP：cluster 拓扑序列上以每边界 `dpSegmentPenalty`（默认 1.0）
  罚项最小化边割切分，块容量受 `maxAtomsPerBlock`（默认 128，**atom 计**，NO0007 P3 起
  atom 是分区大小单位）限制，单超 cap 的
  cluster 自成一块；③**局部移动精化**（`refineClusterBlocks`，`dpRefinementRounds` 默认
  10、0 关闭）：按精确块级 incoming-copy 成本增量把 cluster 移向邻接块，单调降本、
  全确定性，候选须保持跨块 def-before-use（拓扑硬约束）；
- **commit 子图按事件聚类**（**partition-am-commit-graph**，`partitionAmCommitGraph`）：commit atom 只按事件
  签名聚合——每个 event 经其定义 `changed` 规范化为（边沿种类, 被观测 Variable），
  排序去重后作为分块键，同键 atom 在 `maxCommitAtomsPerBlock`（默认 4096，**atom 计**）
  且指令数不超 `kMaxGuardEventMergeOps`（4096，指令口径护栏）的限制
  内合并；事件签名只含 event operand——`reg.write/latch.write/mem.write` 各变体的
  cond/mask 是写指令自身的门控操作数，不参与分块。

两路结果随后在 materialize 阶段合并回全局 atom 编号（commit Block 序接在 compute 段
之后，中间留 input sink 位）。真正的 SCC atom 超限时保留为一个 oversized Block 并报
告诊断。commit 跨 target 合并只改变活动粒度，不改变 Block 内拓扑/effect 次序。
合并逻辑全仓唯一（materialize 内），不设组合包装入口。

commit Block 不再常扫描：每轮 compute 阶段结束后，commit 阶段按 BlockId 升序、以与
compute 相同的激活位图过滤执行 commit Block（首次 `eval()` 激活全部 Block）。块首部
gate detector 先执行并更新基线；门控（全部首部 detector 结果的 OR）为假时整块跳过，
为真时写指令按文本顺序执行——写自身不再判定 event；`reg.write/latch.write/mem.write`
的 `.c/.cm` 变体自带 cond 门控（cond 不命中则不写也不做判变比较），`.m/.cm` 变体在
commit 局部做逐 bit mask 混合（读旧值 `v=(v&~mask)|(data&mask)`），mem 各变体保留
地址越界抑制（无 mask 变体整元素覆盖、不读旧值），`mem.write_lanes` 保留
`any(laneMask)` 早退。使 visible state 实际变化且存在 reader 时，尾部判变 event 激发
`act.b`，激活该 state 的 reader compute Block 进入下一 round。没有 operand 快照、
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

### 3.2.4 指令图研究导出（JSONL，2026-07-30）

`GrhIRToGrhSimAMProgram::graphToProgram` 支持把**调度前**的指令图导出为 JSONL，
供 topo-partition-proj 的离线分区研究（harness/打分/搜索/训练）使用。设置环境变量即触发，
不影响正常调度流程；导出失败（路径不可写等）会使调度报错退出，不会静默跳过：

```bash
WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL=/path/to/graph.jsonl \
    grhsim-am-lower-json design.json SimTop --schedule
```

导出点位于 def-use 索引、ordered effect 边和 SCC 都建好之后、block 形成之前，因此导出
内容就是调度器自己看到的那张图（环收缩直接复用生产 SCC 结果）。格式为
`wolvrix.am-instruction-graph.v1`，每行一条 JSON 记录：

- 首行 header：`{"record":"header","format":...,"instructions":N,"variables":M,"atoms":A,`
  `"comb_loop_atoms":C,"def_use_edges":E1,"external_reads":E2,"order_edges":E3}`；
- 节点：`{"record":"node","id":I,"op":<Opcode 数值>,"opcode":"<名称>","width":W,`
  `"state_write":B,"atom":A,"comb_loop_atom":B}`。`width` 为该指令全部 result 的位宽之和
  （Array 类型按 元素位宽×深度 计，Real 计 64）；`state_write` 等价于"属于 commit 类指令"
  （RegisterWrite/MemoryWrite/MemoryFill/LatchWrite）；`atom` 为 SCC 分量编号，
  `comb_loop_atom` 表示所属 SCC 含 ≥2 条指令（纯组合环打包，真实设计上恒为 false）；
- 数据依赖边：`{"record":"edge","kind":"def_use","src":S,"dst":D,"var":V,"width":W}`，
  按 (src, dst, var) 去重；`width` 为变量位宽，可直接折算 `ceil(W/64)` 拷贝数；
- 外部读：`{"record":"edge","kind":"external_read","dst":D,"var":V,"width":W}`，表示
  无定义指令的变量（state target、接口输入等）被指令 D 读取——生产段 DP 把这类变量当作
  永久边界计入 incoming 成本，打分器需要它们对齐口径；state target 操作数本身按依赖
  规则不算 use，不会出现在这里；
- 顺序约束边：`{"record":"edge","kind":"order","src":S,"dst":D}`，来自 ordered effect
  组链与隐式 host 序链，不带位宽。

注意 use 不去除 commit 指令内的读（段 DP 口径会跳过 commit 内 use），需要与生产 DP
完全同口径时由消费方按 `state_write` 标志自行过滤。全香山（466 万指令）导出约
10.4M 条 def_use 边，文件为 GB 级，消费方应按行流式读取。

同一 schedule 运行还支持导出**生产 block assignment（plain 基线解）**，供 harness
对账（scorer 重算 vs 生产统计）。设置环境变量即触发，与指令图导出相互独立、可同次
运行同时开启：

```bash
WOLVRIX_GRHSIM_AM_BLOCK_ASSIGNMENT_JSONL=/path/to/block_assignment.jsonl \
    grhsim-am-lower-json design.json SimTop --schedule
```

格式为 `wolvrix.am-block-assignment.v1`，每行一条记录：

- 首行 header：指令/变量数、`blocks`（normal block 数，不含 entry block 0）、
  `compute_blocks`、`commit_blocks`、`input_sink_block`（无则为 0），以及生产侧
  算出的三个对账指标（见下）；
- block 记录：`{"record":"block","id":B,"kind":"compute|commit","size":S}`，
  覆盖 1..blocks（input sink 为 size 0 的 compute 块）；entry block 0 不含线性
  指令，不出现在记录中；
- assign 记录：`{"record":"assign","instr":I,"block":B}`，指令 id 与指令图导出
  的节点 id 一致。

header 中的三个对账指标在生产内部按如下口径计算（harness  scorer 应独立复算到
相同数值；`exp/tools/reconcile_baseline.py` 即为参考实现）：

- `dag_edges`：def_use 边跨 block 时按 (producer block, consumer block) 去重的
  块间依赖边数（order 边不计入）；
- `compute_compute_value_pairs`：按 (value, 消费它的 compute block) 去重的跨块
  传值对数；value 不在该 block 定义才计入，state target / 接口输入等无定义变量
  是永久边界、对每个消费 compute 块都计一次，commit 块内的读不计——与段 DP 的
  incoming activation 成本同口径；
- `incoming_copy_cost`：上述每个对按 `max(1, ceil(width/64))` 折算的拷贝总数，
  即 topo-partition-proj 04 文档第一阶段优化目标。

### 3.2.4.1 split 图导出（compute/commit 子图，2026-08-06）

split-am-graph 阶段的产物本身也可以导出（成员资格、诱导边与 atom 注解全部取自
split 上下文，与分区 pass 所见一致，不是全图导出的离线过滤）：

```bash
WOLVRIX_GRHSIM_AM_SPLIT_GRAPH_JSONL=/path/to/prefix \
    grhsim-am-lower-json design.json SimTop --schedule
# 产出 <prefix>.compute.jsonl 与 <prefix>.commit.jsonl
```

格式为 `wolvrix.am-split-graph.v1`，record 形状沿用指令图导出惯例：header 增加
`side`（compute|commit）；node 记录含 `atom`（子图局部 atom id）、`min_instruction`，
commit 侧另含 `event_rank`；commit 侧的 `external_read` 是其全部边界输入（含
`src_side="compute"` 的 compute 产出值与真正无定义的 state/接口读）。compute 图与
gsim 打平图（`--flatten-nodes`）语义同构，是 compute 分区算法研究的切入点
（量化基线见 pdocs/grh-notepad/compute-partition/NO0001）。

### 3.2.5 AM 指令流优化（DCE / const-fold / CSE / assign 别名 / ROM 折叠 / 逻辑统一 / mux 取反吸收 / slice 融合，2026-07-31 初版，2026-08-06 扩展）

`grhsim/am/grhsim_am_graph_optimize.{hpp,cpp}` 在 lowering 与 schedule 之间提供可选的指令流优化：
`optimizeAmGraph(AmGraph&, AmOptimizeOptions{dce, constFold, cse, assignAlias, stateReadAlias, logicUnify, muxNotAbsorb, sliceFuse, notUnify(默认关), constMemFold, interfaceAlias},
Diagnostics&)`（自 2026-08-06 起在图上就地重建，前身为线性版的 `optimizeLinearProgram`）。`grhsim-am-lower-json` 经 `--am-optimize=dce,fold,cse,alias,memfold,ifacealias`（默认全开）/
`--no-am-optimize` 控制；实验路径与生产路径均默认开启——生产路径由
`GrhIRToGrhSimAMProgram::run` 在 lower 校验后、graphToProgram 前调用，可用
`setAmOptimizeOptions` 改配或全关。
动机与两级（GRH 层 + AM 层）实验设计见 topo-partition-proj `docs/20`；2026-08-04
扩展的动机与实测见 pdocs/grh-notepad/supernode-align NO0011；2026-08-06 图形收敛
pass 的归因与实测见 pdocs/grh-notepad/compute-partition NO0003/NO0005。

pass 内部以"别名表 + 指令重写表"双机制工作：fold/CSE/别名类 pass 把结果变量别名到
代表变量；形态重写类 pass（logicUnify/muxNotAbsorb/sliceFuse）记录
(opcode, operands[, slice lsb]) 重写项，后续 pass 与 compact 一律经 effective
访问器读取指令形态，定点迭代至不动点后一次稠密重建。

- **DCE**：根集合 = effect ∈ {StateWrite, StateReadWrite, HostRead, HostEffect} 的指令
  ∪ `orderedEffects` 涉及指令 ∪ 产出 ExternalOutput/Observable 角色的指令（跟随别名
  解析到代表变量），沿 def-use 反图（重写后的有效操作数）标记活指令。StateRead
  （MemoryRead）结果无引用且不在 `orderedEffects` 中时可删。
- **const-fold**：操作数全为常量的纯 op 求值进 literal 池（逐位镜像 interpreter
  语义），与 CSE、assign 别名迭代至不动点。
- **CSE**：纯 op hash-cons，key = (opcode, result type, 规范化操作数[可交换 op
  排序], slice/system 属性)；Memory/DPI/System/Changed 一律不参与。重复指令的结果
  变量别名为首次出现的变量。
- **assign 别名**（assignAlias/stateReadAlias）：单操作数且结果/操作数类型完全一致
  的 Assign 直接别名其操作数。操作数为 State 角色时按 stateReadAlias 细化判定：
  仅当结果（沿单操作数 Assign 链闭包）可达 commit 侧指令（状态写/变化检测/宿主
  效应）操作数时才保留——那是 commit read-old 快照（lowering preCommitValue）；
  只喂 compute 逻辑的状态读 Assign（GRH kAssign 线网）一律旁路，消费者直读状态
  变量（值恒等，仅缩短激活链）。
- **逻辑统一**（logicUnify）：结果与全部操作数均为 1-bit 的 LogicAnd/LogicOr/
  LogicNot 重写为按位 And/Or/Not（1-bit 下 truth(x)=x，两形式语义恒等），使 CSE
  能合并 Chisel `&`/`&&` 双形式产生的平行副本。
- **mux 取反吸收**（muxNotAbsorb）：`mux(not(c), a, b)` → `mux(c, b, a)`，要求 Not
  单用且结果无接口可见性；LogicNot 对任意宽度成立（select 只取真值），按位 Not
  仅 1-bit。Not 指令随删除。
- **slice 融合**（sliceFuse）：`slice(slice(x, l1), l2)` → `slice(x, l1+l2)`（两级
  均要求在界内以保零填充语义逐位一致）；lsb=0 且类型完全一致的恒等 slice 直接
  别名——与状态读 Assign 同样查 commit 闭包，喂 commit 的状态恒等 slice 保留。
  中间 slice 由 DCE 回收。
- **not 归一**（notUnify，默认关）：1-bit Not → Eq(x, 0)，对齐参照系的否定形式。
  香山实测纯桶间搬移（CSE 仅 +6 合并），不出力故默认关闭（NO0005 §5）。
- **ROM 折叠**（constMemFold）：目标 memory 无任何写/填充且地址为编译期常量的
  MemoryRead 折叠为常量——存储零初始化语义下 Undef/Zero init 读出恒 0（与
  interpreter/emitter 逐位一致），越界地址按 interpreter 语义折 0；Actions init
  （$readmemh）不参与。香山设计上无从未写入的 memory，该 pass 实测折叠 0 条。
- **接口重指向**（interfaceAlias）：被消除变量若被 ProgramInterface 引用（输出
  端口、declaredVariables 可观测变量），compact 时把接口项重指向别名代表，并把
  ExternalOutput/Observable 角色位转移给代表（角色-接口一致性校验要求集合精确
  匹配）；State/ExternalInput 角色变量永不别名。关闭则恢复旧策略（带角色变量
  一律不可别名，香山 84.4 万 declared 变量会使 CSE 几乎失效）。
- **compaction**：删除采用稠密 id 重写（不用 NOP 标记，避免死指令占用分块容量
  预算）；`instructionEffects` 由 `opcodeTraits(opcode).effect` 机械重算，
  `variableRoles`、`orderedEffects`、`interface.ports/declaredVariables` 与
  slice/system/dpi 属性记录全部随 id 重映射。完成后 `validate(artifact)` 自检，
  失败则保持原 artifact 不变并返回 false。

全香山实测（L1 清理后的 3,946,245 指令输入）：CSE 净删 16,359 条重复纯 op、
fold 2 条、DCE 0 条（死锥已被 GRH 层收编），耗时 ~6 s；E2（L2-only，未过 L1 的
脏图）结果见 topo-partition-proj `docs/20` 的实验矩阵。2026-08-04 扩展后实测
（3,387,378 指令输入）：CSE 162,488 + assign 别名 185,426，指令 -9.9%，香山
CoreMark 50k difftest 通过且仿真 host time -15.5%（supernode-align NO0011 §6）。
2026-08-06 图形收敛 pass 后实测（3,630,947 指令输入）：CSE 163,131 + assign 别名
354,984 + 统一 205,278 + 吸收 72 + slice 融合 4,837，指令 3,630,947 → 3,111,412；
split 后 compute 图出度≥2 节点 599,947 → 574,724（compute-partition NO0005）。
单测 `tests/grhsim/am/test_optimize.cpp` 覆盖死锥删除、可交换 CSE、fold 级联、
接口重指向与角色转移、状态快照保留与纯计算状态读旁路、逻辑统一、mux 取反吸收、
slice 融合、ROM 折叠与根集合安全性。

### 3.2.6 gsim node 对齐 atom 化（NO0006，2026-08-11）

当输入 GRH JSON 携带 gsim node 溯源（`gsim.node_id` int attr，由 gsim 侧
`--export-executable-grh` 在 flatten 图上对每条 node 所属 op 戳记；`kConstant`
豁免、保持全局常量），AM 流水线默认切换到 **node 对齐模式**，把"atom 是启发式
产物"改为"atom 由 gsim node 结构直接构造"：

- **lowering**：逐指令侧表 `AmGraph::gsimNodeId`（-1 = 无归属）在
  `addInstruction` 单点戳记； lowering 合成胶（coerce/result-bridge/dpi-bridge
  Assign）按"触发者归属"继承当前 op 的 node id；preCommit 快照与 changed
  检测器显式无归属（AM 时钟域附加层，gsim 无对应 node）；commit 指令经
  `PendingStateWrite` 继承写 op 的 node id。任一 op 带溯源即置
  `hasGsimNodeProvenance`。
- **模式门控**：`ActivityScheduleOptions::gsimNodeAligned`
  （Auto/On/Off，默认 Auto=有溯源即启用），env 覆盖
  `WOLVRIX_GRHSIM_AM_NODE_ALIGNED=0|1`。
- **optimize/fold 策略**：该模式下默认**跳过** `optimizeAmGraph`（assignAlias
  会抹掉 anchor、CSE 会跨 node 合并——都破坏 1:1 映射；gsim flatten 图本身已经
  gsim 优化过），escape hatch `WOLVRIX_GRHSIM_AM_NODE_ALIGNED_OPTIMIZE=1`
  供 A/B；split 后的 tree-atom fold 与 fanout absorb 同样跳过（atom 已定型）。
- **split 的 atom 构造**：commit 指令保持 singleton-per-instruction；compute
  指令按 node_id 分组（一个 node 一个 atom）；无归属 compute 指令 singleton；
  SCC 仍跑用于环安全——跨组多指令 SCC union 兜底并标 CombLoopScc（香山实测
  0 次）；组内 anchor（唯一外定义汇）旋正末位，多汇组（EXT 多输出）合法并计
  multi_sink。类型：commit→CommitEvent（签名=event rank）、多指令组→Tree、
  单指令→Singleton（mux 根签名约定不变）。
- **审计**：`WOLVRIX_GRHSIM_AM_NODE_ATOM_AUDIT_JSONL=<path>` 逐 atom JSONL
  （node_id/kind/instructions/multi_sink）+ stderr 汇总（atom 计数、node 双射
  校验、无归属 opcode 直方图、SCC union 计数）。

香山 flatten 图实测：3,071,711 个 node 映射 compute atom 与 gsim node 一一对应
（零重复），严格 compute 口径 L2 = 0.9995x，atom-DAG 边 recall 99.2%；逐类型
残差与边残差全部列账（见 pdocs/grh-notepad/emit-cost NO0006 §11）。单测
`tests/grhsim/am/test_node_aligned.cpp` 覆盖戳记、Tree atom 成形与 anchor 序、
commit singleton、无归属检测器、Off 模式回退旧路径。

### 3.2.7 oversized Block 分块与 chunk 函数 `__restrict__`（2026-08-11/12）

超大 Block（指令数超 `blockChunkInstructions`，默认 3000，不可拆 atom 可超过）
的语句体拆成 `block_<id>_chunk_<k>()` 成员函数顺序调用；Block 局部值/watch
标志以父作用域数组（`localblk_<id>[k]` 等）经指针参数共享给 chunk 函数。

**关键陷阱（2026-08-12 实锤）**：chunk 函数的共享数组指针参数必须带
`__restrict__`。缺失时，chunk 体内每条经裸指针的 store 对 LLVM 而言
may-alias 全部成员变量（XiangShan 模型 142 万个 `uint64_t` 成员），GVN 的
非局部 clobber 查询（`MemoryDependenceResults::getNonLocalPointerDepFromBB`
→ TBAA）随语句数超线性爆炸：62k 行 TU 单任务 -O3 40 分钟编不完（gdb 栈采样
实锤卡在 GVN）。加 `__restrict__` 后同一 TU **10.31s**（>230x）；合法性依据是
这些指针恒指向调用方栈数组，绝不指向成员存储。gsim 无此问题：其临时量是
函数内真 C++ 局部变量（纯 SSA）。全量 emu（351 TU）-O3 零降级构建 7m44s。
详见 pdocs/grh-notepad/emit-cost NO0007 §12。

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
- scheduler 接受 `AmGraph&&` 并消费所有权，finalize 时物化出 `LinearProgram` 交给
  `ScheduledProgramBuilder`；能复用的 Variables、Type、Attribute、Init 和 instruction
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

生成 runtime 把全部 Block 的激活状态表示为每 64 个 Block 一个 word 的单一
active 位图（不分 current/next，没有 summary 层；compute 与 commit Block 共用）。
`eval()` 主循环是两阶段 round（2026-07-29 起为静态 dispatch 形态）：
compute 阶段按 (source, part) 升序直接调用每个覆盖 compute 段的 `eval_scan_*()` 成员
函数，函数内按 8 个 Block 一个 byte chunk 消费位图——把活动字节快照进局部
`byteFlags`、清掉全局字节中本 chunk 拥有的位，然后对拥有的位做升序直线
`if ((byteFlags & bit) != 0) { ... }` 测试并内联执行 Block 体（legacy/GSIM 的 batch
形态）；`act.f` 的目标恒为更大的 Block，同 byte 且属本 chunk 的目标改写
在位 `byteFlags`（局部接力），其余经编译期常量掩码写全局位图，两者都因严格前向而
在同一趟升序遍历内被消费。commit 阶段按 (source, part) 升序直接调用
`eval_commit_*()`，以与 compute 阶段相同的 byte-chunk 激活扫描执行被激活的 commit
Block（同 byte 前向 act 局部接力同样成立）；每个 commit Block 的首部
`changed.*` gate detector 结果 OR 成单一门控表达式，状态写与尾部 watch/act 都在
门内，整块只判一次。`act.b` 置位 reader compute Block 并置 `backwardFired_`，作为
“需要下一轮”的唯一信号。一轮完整遍历没有任何 `act.b` 激发即收敛；round 计数超过
上限（1,000,000）报 did not converge。首次 `eval()` 激活所有 Block（commit Block
借此同步 gate detector 基线并评估每个状态写一次）。跨块消费的 `changed` 结果使用
`set_changed_result` 将实际为真的结果加入 dirty list，每轮结束只清理这些结果，而不是
生成每个 detector 的静态 clear store；同块消费的 result 每次执行都被重写，不进
dirty list。

生成模型的持久化存储（2026-07-29 起）：窄标量值（BitVector ≤ 64 bit）凡持久化的一律
是独立类成员 `v<VariableId>`（gsim 式成员变量形态，让编译器把每个值当独立变量做
静态排布与寄存器分配；块内局部值仍物化为 C++ 局部变量，不占成员）；跨块消费的
`changed` 结果是唯一被运行时下标访问的值（dirty list 清零），单列密集数组
`changedResults_[denseId]`；宽值（> 64 bit 与 Array）仍在 `wideValues_` 数组。
生成的 Makefile 用 clang `-x c++-header` + 每 TU `-include-pch` 预编译模型头
（成员声明达数百万行量级，PCH 是编译耗时摊销的关键），并用
`ifeq ($(origin CXX),default)` 兜底 GNU make 内建的 g++ 默认值（g++ 不支持
`-include-pch`）。

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

`GrhIRToGrhSimAMGraphLoweringStage::lower()` 返回的 artifact 不能保存 Graph pointer/reference。100M
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
| AM dense ID、SoA storage、Linear/ScheduledProgram、ProgramView、move-only builders、reserve API、ProgramInterface/SchedulingFacts/LinearProgramArtifact/ExecutableModel | `include/grhsim/am/grhsim_am_program.hpp`、`lib/grhsim/am/grhsim_am_program.cpp` |
| linear/scheduled validator | `include/grhsim/am/grhsim_am_program_validate.hpp`、`lib/grhsim/am/grhsim_am_program_validate.cpp` |
| AmGraph 一等工作 IR | `include/grhsim/am/grhsim_am_graph.hpp`、`lib/grhsim/am/grhsim_am_graph.cpp` |
| 总流程 GrhIRToGrhSimAMProgram（含 static `graphToProgram()` 编排：split → opt → 两路 partition → materialize）、stage 接口与 validate 组合 | `include/grhsim/am/grh_ir_to_grhsim_am_program.hpp`、`lib/grhsim/am/grh_ir_to_grhsim_am_program.cpp` |
| opcode 分类 helper | `include/grhsim/am/grhsim_am_opcode_traits.hpp` |
| lowering-to-am-graph | `include/grhsim/am/grh_ir_to_grhsim_am_graph.hpp`、`lib/grhsim/am/grh_ir_to_grhsim_am_graph.cpp` |
| opt-am-graph | `include/grhsim/am/grhsim_am_graph_optimize.hpp`、`lib/grhsim/am/grhsim_am_graph_optimize.cpp` |
| split-am-graph 阶段（含指令图导出） | `include/grhsim/am/grhsim_am_graph_split.hpp`、`lib/grhsim/am/grhsim_am_graph_split.cpp` |
| 分区家族共享类型与低层 splitAmGraph | `include/grhsim/am/grhsim_am_graph_partition.hpp`、`lib/grhsim/am/grhsim_am_graph_partition.cpp` |
| opt-am-compute-graph（空阶段预留） | `include/grhsim/am/grhsim_am_compute_graph_optimize.hpp`、`lib/grhsim/am/grhsim_am_compute_graph_optimize.cpp` |
| partition-am-compute-graph（coarsen + segment DP） | `include/grhsim/am/grhsim_am_compute_graph_partition.hpp`、`lib/grhsim/am/grhsim_am_compute_graph_partition.cpp` |
| partition-am-commit-graph（事件聚类） | `include/grhsim/am/grhsim_am_commit_graph_partition.hpp`、`lib/grhsim/am/grhsim_am_commit_graph_partition.cpp` |
| materialize 阶段（含 block assignment 导出与 finalize） | `include/grhsim/am/grhsim_am_graph_to_program.hpp`、`lib/grhsim/am/grhsim_am_graph_to_program.cpp` |
| AM Program 参考解释器 | `include/grhsim/am/grhsim_am_program_interpreter.hpp`、`lib/grhsim/am/grhsim_am_program_interpreter.cpp` |
| AM Program C++ 发射器 | `include/grhsim/am/grhsim_am_program_cpp_emitter.hpp`、`lib/grhsim/am/grhsim_am_program_cpp_emitter.cpp` |
| 调度阶段共享内部助手 | `lib/grhsim/am/grhsim_am_common.hpp`（lib 内部） |
| 单元与 contract tests | `tests/grhsim/am/` |

不要把 IR 定义塞进旧 `transform/activity_schedule.cpp` 或 `emit/grhsim_cpp.cpp`。`program`
与 builder 不依赖 GRH transform 或 C++ emitter；总流程 API 声明单向的
`GRH -> AmGraph -> ExecutableModel -> C++` stage；emitter 只读取 core AM
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
| 首次求值 | 旧路径 seed compute/activity；AM 先执行 B0，首次求值激活全部 Block（compute 与 commit），old 为 `undef` | 不把旧 baseline 强加给 AM；reset 后行为与首次未定义边界分开测 |
| round 结构 | 旧路径按 batch 调 compute、每 round 扫 commit；AM 对齐同一两阶段 round：compute 与 commit 都按 active 过滤升序执行（commit 相位在后），任一 `act.b` 激发即要求下一轮 | feedback 收敛、写回可见时点和执行次数符合 AM |
| 激活边 | 旧代码依赖 topo active id 和 batch 内局部传播；AM `act.f` 指向严格更大的 BlockId（compute 或 commit），`act.b` 指向非 EntryBlock 的任意 Block（scheduler 只在 commit Block 尾部产生）；指向 commit Block 的激活边只携带其 gate detector watch 的变量 | 每条激活边的放置与 target 范围经 validator 证明 |
| event 生命周期 | 旧 event edge slot 通常按 fixed-point round 清零；AM 跨块消费的 changed result 在 round 末清零（同块消费不清），B0 每次 eval 执行 | pos/neg、同轮多消费者和跨轮 event 不丢失或重复 |
| state write | 旧 commit supernode 每 round 扫描；AM reg/latch/mem write 位于按激活位执行的 commit Block，写按 cond/mask 常量性选型为 12 个显式变体（`.c/.m/.cm`），cond 门控与 commit 局部 mask 混合由指令自身判定，mem 变体保留地址越界抑制；写自身无 event 判定，由块首部 gate detector 的 OR 门控整块写入 | 多写 priority、mask/fill、read-during-write 和 reader reactivation 一致 |
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
