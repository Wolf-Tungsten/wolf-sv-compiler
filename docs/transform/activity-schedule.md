# activity-schedule

## 功能概述

`activity-schedule` pass 为单个 graph 构建 GrhSIM 用的 activity supernode 调度结果，并把结果写入 session。

它不会把 graph 改写成新的 wrapper/module 结构，但会：

- 为缺失 symbol 的可分区 op 补内部 symbol
- 冻结 graph
- 显式分类 `source` / `sink` / `compute` / `declaration`
- 将 `kConstant` / `kRegisterReadPort` / `kLatchReadPort` 的 compute 侧 use 前置 clone
- 重新冻结并重建 op topo 数据
- 通过 `buildComputeNodeRewrite(...)` 构造 compute node / commit node 中间模型
- 在 compute-node cluster DAG 上做 topo ordering / coarsen / 连续分段，生成最终 `computeSupernode`
- 展开并导出最终 `computeSupernode` / `commitSupernode` 调度模型
- 导出 `supernode_to_ops` / `op_to_supernode` / `dag` / `supernode_kind` / `compute_nodes_by_supernode` / `value_fanout` / `topo_order` / `state_read_supernodes` / `summary_stats`

这个 pass 目前主要服务于 `grhsim-cpp` emit。

## 路径语义

`-path` 的解析规则和其他 path-based pass 一致。

- 单段路径：直接按 graph 名选中目标 graph
- 多段路径：`<root>.<inst>...`

多段路径从 root graph 开始，逐层按实例 `instanceName` 查找，并通过实例的 `moduleName` 进入下一级 graph。

## 主要选项

`ActivityScheduleOptions` 定义在 [activity_schedule.hpp](../../include/transform/activity_schedule.hpp)。这里按 `testcase/xs-components` GrhSIM flow 显式传入的参数列文档入口；C++ 里仍保留的兼容/实验参数不作为当前推荐接口。

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `-path` | 无 | 目标 graph / 实例路径，必填 |
| `-max-op-in-compute-supernode` | `128` | 连续 DP 分段时的目标 op 数上限；它不是 coarsen merge 上限，也不是最终 `supernode_to_ops[i].size()` 的硬上限 |
| `-max-op-in-commit-supernode` | `4096` | 单个 `commitSupernode` 最多包含的 sink op 数 |

## Session 输出

对 `path=<target>`，pass 会写入以下 key：

- `<target>.activity_schedule.supernode_to_ops`
- `<target>.activity_schedule.op_to_supernode`
- `<target>.activity_schedule.dag`
- `<target>.activity_schedule.supernode_kind`
- `<target>.activity_schedule.compute_nodes_by_supernode`
- `<target>.activity_schedule.value_fanout`
- `<target>.activity_schedule.topo_order`
- `<target>.activity_schedule.state_read_supernodes`
- `<target>.activity_schedule.summary_stats`

这些 key 的写入位置见 [activity_schedule.cpp](../../lib/transform/activity_schedule.cpp)。

## 当前 compute node / commit node 语义

截至 `2026-05-23`，`activity-schedule` 主路径保留 `source` / `compute` / `sink` / `declaration` 语义分类，并显式构造 `ComputeRewriteBuild` 中间模型：

1. 初次 `buildActivityOpData(...)` 只收集可分区 op，并按 operand def 建 op topo DAG。
2. `cloneSourceUsesForCompute(...)` 会把 source op 到 compute op 的每个 use 前置 clone，并把 compute operand 改接到 clone value；source -> commit 仍保留原始 source value。
3. 如果发生 clone，pass 会重新 `freeze()` 并重建 `ActivityOpData` / `opClasses`。
4. `buildComputeNodeRewrite(...)` 先按 sink event key 构造 `commitNodes`，再为仍有调度用途的 source op 和每个 compute op 构造 `computeNodes`。
5. 当前默认 `ComputeNode` 通常只含一个 op；其 `boundaryInputs` 是由其他 compute node 产生、并被本 node 读取的 operand value。
6. `buildComputeDag(...)` 基于 `boundaryInputs` 建 compute-node DAG。
7. `materializeComputeNodeSchedule(...)` 在 compute-node DAG 上做 cluster topo / coarsen / DP 分段，然后展开成最终 `computeSupernode`；随后追加 `commitSupernode`。

`sink op` 包括：

- 只包含本地 state write：
  - `kRegisterWritePort`
  - `kLatchWritePort`
  - `kMemoryWritePort`
- `kMemoryFillPort`

`source op` 只包括：

- `kConstant`
- `kRegisterReadPort`
- `kLatchReadPort`

`kMemoryReadPort` 是 compute op，不作为 source clone 处理。`kSystemTask` / `kDpicCall` 也按普通 compute op 处理。

关键不变量：

- source op 到 compute op 的 use 会在调度初始化前 clone；source op 不能克隆到 `commitSupernode`
- direct source -> commit 依赖通过原始 source value 的 `value_fanout` 保留
- sink op 只出现在 `commitSupernode`
- `commitSupernode` 不作为 value producer 产生出边
- `supernode_kind` 显式标记最终 supernode 是 `compute` 还是 `commit`
- `compute_nodes_by_supernode` 记录最终 compute supernode 由哪些 compute node 展开得到；commit supernode 对应空列表

## 当前 computeSupernode 调度算法

截至 `2026-05-23`，主路径的 `computeSupernode` 调度逻辑对应 source 前置 clone + `buildComputeNodeRewrite(...)` + `materializeComputeNodeSchedule(...)`，行为如下：

1. 对 source -> compute use 前置 clone；source -> commit 仍保留 direct source dependency。
2. 对每个仍有调度用途的 source op 和每个 compute op 初始化一个 compute node，并按 boundary input 建 compute-node DAG。
3. 初始 cluster 是“每个 compute node 各自一个 cluster”，按 compute-node topo order 初始化，再通过 `orderNodeClustersTopologically(...)` 重新排序；topo ready tie-break 会优先选择与前一个 cluster 有更多 activation 连接的候选。
4. 如果启用 coarsen，按小流水线反复执行策略：
  - `out1`：当前 cluster 只有一个后继时，按 activation gain 从高到低尝试与该后继合并。
  - `in1`：当前 cluster 只有一个前驱时，按 activation gain 从高到低尝试与该前驱合并。
  - `boundary-gain`：按跨 cluster boundary activation edge weight 从高到低尝试合并 producer/consumer cluster。
  - 当前代码把 coarsen merge 上限设为 `std::numeric_limits<std::size_t>::max()`，所以 coarsen 本身不受 `maxOpInComputeSupernode` 约束。
5. coarsen 之后，按 topo 顺序做连续分段 DP；DP 用 `maxOpInComputeSupernode` 限制一个 segment 内的 op 数，但不会拆开已经 coarsen 到一起的单个 cluster。
6. DP segment 被 flatten 成 compute node 列表，再展开为最终 `computeSupernode` op 列表；每个 supernode 内会再做一次 local op topo sort。
7. 最终追加 commit supernode，并从实际 supernode 内容重新构造 `op_to_supernode` / `dag` / `value_fanout` / `state_read_supernodes` / `topo_order`。

当前主路径里：

- 分段输入只包含 compute node cluster，不混入 `commitSupernode`
- 不再使用旧 `WorkingPartition` 路径里的 `fixedBoundary`
- 当前 DP 只优化 topo 连续段，不做任意 DAG partition
- `maxOpInComputeSupernode` 是 DP 目标约束，不是 coarsen 硬约束；如果 coarsen 产生 oversized cluster，DP 会把该 cluster 保留为单独 segment

### Boundary activation 权重

当前 compute 侧会为 cluster DAG 额外统计 `ClusterValueEdges`：

- `valueFanouts` 记录每个 compute-side boundary value 的 producer cluster 和 distinct target clusters。
- `(fromCluster, toCluster)` weight 是 `fromCluster -> toCluster` 上不同 boundary value 形成的 activation edge 数。
- 同一个 value 在同一个 target cluster 内有多个 use，只算一条 activation edge；同一个 value 扇出到多个 target cluster，会按 target cluster 数累计。

这个权重同时服务三处：

- topo ready tie-break
- `boundary-gain` coarsen
- 连续分段 DP 的 activation cost

它只统计 compute node cluster 之间的 value 依赖；commit 输入依赖仍在最终 `value_fanout` materialize 阶段处理。

当前优化目标以 `boundary_activation_edges` 为主，因为它同时覆盖：

- `boundary_values`：多少唯一 value 跨 supernode。
- fanout：每个跨 supernode value 激活多少个目标 supernode。

因此一个 merge 的收益不只看是否消掉某个唯一 value，也看它能否减少该 value 的跨 supernode 目标数。

### Topo-aware ordering

初始和 coarsen 后的 cluster 排序都必须保持 DAG topo 合法。当前使用 Kahn-style ready set：

1. ready set 默认按 cluster id 保持确定性。
2. 如果上一个输出 cluster 对某个 ready candidate 有 activation 边，则优先选择 activation weight 最大的 candidate。
3. 若 weight 相同，回退到较小 cluster id。

这个规则只改变同一 topo frontier 内的 tie-break，不改变依赖方向，因此不会引入新 cycle。实现上只检查上一个 cluster 的 outgoing value edge，避免在百万级 ready set 上全扫描。

### Coarsen pipeline

coarsen 阶段是一个小流水线，每轮按固定顺序执行策略，直到整轮没有变化：

1. `out1`
   - 当前 cluster 只有一个后继时，尝试合并到该后继。
   - 候选要求 producer -> consumer activation weight 非零，并按该 weight 降序处理。
   - 使用 DSU 批量合并候选，之后统一 canonicalize，并用 topo check 验证结果。
   - 适合吞掉线性 producer -> consumer 链。
2. `in1`
   - 当前 cluster 只有一个前驱时，尝试合并到该前驱。
   - 候选要求 producer -> consumer activation weight 非零，并按该 weight 降序处理。
   - 同样使用 DSU 批量合并，最后统一做 topo check。
   - 适合吞掉线性 consumer tail。
3. `boundary-gain`
   - 按 cluster pair 的 boundary activation edge weight 从高到低尝试合并。
   - 目标是优先消掉 activation 连接最紧的 supernode 边。
   - 先尝试一轮 batch matching：同一 batch 内每个 cluster 最多参与一次 merge。
   - 如果 batch merge 后无法得到合法 topo order，则退回到按候选逐个尝试，接受第一个 topo 合法的 merge。

所有策略共享的合法性约束：

- 合并后必须能重新得到合法 topo order。
- 当前代码没有对 coarsen 后 cluster 大小设置 `maxOpInComputeSupernode` 上限。

后续新增策略应作为新的 pipeline stage 插入，而不是绕开统一约束。每个 stage 应只负责产生候选 merge；合法性仍由 topo check 兜底。如果需要把 `maxOpInComputeSupernode` 变成 coarsen 硬约束，应先修改 `materializeComputeNodeSchedule(...)` 里传给 `tryMergeNode*` 的 `coarsenMaxNodes`。

### Boundary-aware DP

coarsen 后，DP 只在 topo-ordered cluster 序列上做连续分段，不做任意 DAG partition。

约束：

- 单个 segment 的 compute-node op 总数不超过 `maxOpInComputeSupernode`。
- 如果单个 cluster 已经超过上限，保留为单独 segment。

目标函数：

```text
cost(segment) = incoming_boundary_activation_edges + 1
```

其中 `incoming_boundary_activation_edges` 是该 segment 消费、但 producer 不在同一 segment 内的 compute-side boundary value distinct source-target segment 数。若同一个 value 在同一 segment 内被多个 cluster 使用，只算一次；若同一个 value 扇出到多个 segment，则每个 target segment 各算一次。`+1` 是轻量 segment 数惩罚，用于在 activation cost 相同时偏向更少 segment。

DP 只决定最终 computeSupernode 的连续分段边界。最终 `value_fanout`、`dag`、`state_read_supernodes` 仍在 materialize 阶段从实际 supernode 内容重新计算。

## 旧路径遗留说明

下文部分记录文件中仍保留的旧 `WorkingPartition` / `fixedBoundary` / DP 代码路径，主要用于历史排查和性能对比；它们不是当前主路径。

## 旧路径 `coarsen` 的 change-coupled 规则

截至 `2026-04-27`，旧 `WorkingPartition` 路径里的 `enable-forward-merge` 已不再只表示“forwarder merge”。

这条旧路径会优先尝试把 singleton cluster 合并到某个前驱 cluster，但前提是：这个 op 的输出对该前驱输入是“输入一变，输出一定变”的关系。

目前已经显式纳入的规则是：

- `kNot`
- `kAssign`，但只在结果位宽不小于输入位宽时成立，也就是不能靠截断吞掉输入变化
- `kConcat`
- `kReplicate` 的 data operand
- `kAdd` / `kSub` / `kXor` / `kXnor`，但只在“唯一动态前驱 + 其余 operand 都是常量”的情况下成立

当前明确不纳入这类 guaranteed-change merge 的 op 包括：

- `kSliceStatic` / `kSliceDynamic`：输入变化可能落在未被观察的 slice 之外
- `kMux`：变化可能发生在未选中的数据支路
- `kEq` / `kNe` / 比较类 / reduction 类：多个不同输入可能映射到同一个输出
- `kAnd` / `kOr` / `kMul` / shift 类：输入变化可能被 mask、折叠或截断掉

如果 guaranteed-change 规则不命中，旧路径仍会保留一层 legacy alias/forwarder fallback，主要覆盖：

- `kAssign`
- `kConcat`
- `kSliceStatic`
- `kSliceDynamic`

这意味着旧路径已经开始把 `coarsen` 往“同步变动驱动”的方向收敛，但还没有完全删掉旧的 forwarder 兼容路径。

## 旧 `WorkingPartition` 路径里 `max-op-in-compute-supernode` 的含义

下面这些说明只适用于旧 `WorkingPartition` / DP 路径，不适用于当前 `computeNode` 主路径。

在旧路径里，这个参数不是：

- 最终 supernode 数量目标
- 单个 supernode 的 bit 数上限
- 单个 supernode 的输出行数上限
- emit 后文件大小上限

它的实际语义是：

- 在 coarsen 合并时，限制 merged cluster 的 member 数
- 在 DP 分段时，限制 segment 的 member 总数
- refine 也沿用同一个 member 上限

也就是说，它限制的是 activity-schedule 内部 partition member count，而不是“最终想要多少个 supernode”。

补充说明：

- 它不限制 `sink-supernode`
- 因此最终 `supernode_to_ops[i].size()` 可能明显大于 `max-op-in-compute-supernode`
- 这仍然不等价于 emit 后文件大小上限；如果某些 op 自身的 C++ 展开非常大，仍可能生成很大的 `sched_*.cpp`

## 旧路径中的 `fixedBoundary` 语义

截至 `2026-05-08`，当前主路径的 `computeNode` 调度不再使用 `fixedBoundary`。

文件里仍保留的旧 `WorkingPartition` 路径中，`isSideEffectBoundaryKind(...)` 恒为 `false`，原因是：

- GrhSIM 的时序和 side effect 由 event edge / commit 语义控制
- `activity-schedule` 不再额外插入“单次执行保护”
- `kSystemTask` / `kDpicCall` / `kLatchWritePort` 等 op 不再因为 pass 内部边界规则被强制拆开

此外，旧路径里还会把 `sink-only cluster` 标成 `fixedBoundary`，用于阻止旧的 `WorkingPartition` merge / DP 跨过 sink cluster；这部分逻辑同样不在当前主路径里生效。

## 历史：replication 后置拆分

这段只记录旧实现背景；截至 `2026-05-23`，当前 `activity_schedule.cpp` 主路径不再包含 replication / `splitOversizedSymbolClusters(...)` 阶段。

旧实现曾在 replication 之后、最终 materialize 之前增加 oversized symbol cluster 拆分：

- 先基于 replication 之后的 graph 重新建立 topo
- 对每个 symbol cluster 收集 live op symbol，并按 topo 顺序排序
- 如果某个 cluster 的 live op 数超过 `max-op-in-compute-supernode`，就按连续 topo chunk 重新切开

这样做的目的不是改变 DP 的 cost model，而是修复 replication 把单个 supernode 膨胀到远超上限、从而导致极端大 `sched_*.cpp` 的问题。当前主路径改为 `materializeComputeNodeSchedule(...)` 中的 compute-node coarsen / DP / optional oversize compute-node split。

## XiangShan 实测记录

下面的数字都来自真实运行，不是估算。

测试环境：

- 目标：`SimTop`
- 流程：`make xs_wolf_grhsim_emit`
- 输入：`build/xs/grhsim/wolvrix_xs_post_stats.json`
- 相关脚本：[`scripts/wolvrix_xs_grhsim.py`](/workspace/gaoruihao-dev-gpu/wolvrix-playground/scripts/wolvrix_xs_grhsim.py)

### 2026-05-13：GSIM 对齐默认值

GSIM XiangShan 当前使用：

```bash
--supernode-max-size=15 --cpp-max-size-KB=8192
```

对应已有 GSIM stats：

- `emitted_supernode_count=84714`
- `emitted_supernode_edge_count=645829`

activity-schedule 的 `max-op-in-compute-supernode` 统计的是 GRH op 级调度原子，不是 GSIM 的 FIRRTL node，因此不能直接设为 `15`。当前 XiangShan GrhSIM flow 默认值为：

```bash
XS_GSIM_SUPERNODE_MAX_SIZE=15
XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE=108
```

实测命令：

```bash
make xs_wolf_grhsim_emit
```

日志：

- [xs_wolf_grhsim_build_20260513_225303.log](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/logs/xs/xs_wolf_grhsim_build_20260513_225303.log)
- [activity_schedule_supernode_stats.json](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/activity_schedule_supernode_stats.json)

结果：

- `supernodes=94383`
- `compute_supernodes=62823`
- `commit_supernodes=31560`
- `dag_edges=1261846`
- `boundary_values=1573742`
- `boundary_activation_edges=2835577`
- `compute_compute_value_pairs=2366673`
- `compute_commit_value_pairs=468904`
- `ops_per_supernode.mean=76.279`
- `ops_per_supernode.median=107`
- `ops_per_supernode.p90=128`
- `ops_per_supernode.p99=128`
- `ops_per_supernode.max=768`
- `activity-schedule` 耗时 `379598 ms`
- `write_grhsim_cpp` 耗时 `45771 ms`
- 总耗时 `447271 ms`

对比本轮重构后误用小粒度默认值 `max-op-in-compute-supernode=16` 的结果：

- `supernodes: 547969 -> 94383`
- `compute_supernodes: 516409 -> 62823`
- `boundary_values: 2296075 -> 1573742`
- `boundary_activation_edges: 4449749 -> 2835577`

因此当前 XiangShan 默认值不再使用小图实验里的 `16`。在 activation-aware coarsen/DP 之后，默认使用 `108` 让总 supernode 数量更接近 GSIM `--supernode-max-size=15` 的 `84,714`。该默认值只是经验对齐点，仍可通过 `XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE` 做 sweep。

### `max-op-in-compute-supernode=128`

命令：

```bash
make xs_wolf_grhsim_emit XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE=128
```

日志：

- [xs_wolf_grhsim_build_20260412_000231.log](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/logs/xs/xs_wolf_grhsim_build_20260412_000231.log)

结果：

- `supernodes=802073`
- `seed_supernodes=5883646`
- `coarse_supernodes=1315150`
- `dp_supernodes=1315119`
- `replication_cloned=6034308`
- `replication_erased=2114916`
- `activity-schedule` 耗时 `233854 ms`
- `write_grhsim_cpp` 耗时 `39649 ms`
- 总耗时 `310251 ms`
- emit 目录总大小 `4340807690` bytes，约 `4.34 GB`
- emit 文件数 `17341`
- 其中 `sched_*.cpp` `17267` 个
- `state_init_*.cpp` `37` 个
- `state_commit_shadow_*.cpp` `30` 个
- `state_commit_write_*.cpp` `1` 个

### `max-op-in-compute-supernode=75`（tie-break 修复前基线）

命令：

```bash
make xs_wolf_grhsim_emit XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE=75
```

日志：

- [xs_wolf_grhsim_build_20260412_014220.log](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/logs/xs/xs_wolf_grhsim_build_20260412_014220.log)
- 当时的 `activity_schedule_supernode_stats.json` 已被后续重跑覆盖，分布数据见下文摘录

结果：

- `supernodes=815101`
- `seed_supernodes=5883646`
- `coarse_supernodes=1389863`
- `dp_supernodes=1389863`
- `replication_cloned=6168817`
- `replication_erased=2242731`
- `activity-schedule` 耗时 `197259 ms`
- `write_grhsim_cpp` 耗时 `39350 ms`
- 总耗时 `272782 ms`

最终 `ops / supernode` 分布：

- 平均值 `12.035`
- 中位数 `2`
- `p90=16`
- `p99=137`
- 最大值 `92141`

这组数据说明：

- `max-op-in-compute-supernode=75` 已经正确传入
- 但它只是 partition member 上限，不是目标装箱大小
- 它也不是最终 `supernode_to_ops[i].size()` 的硬上限
- 在这次运行里，`dp_supernodes` 与 `coarse_supernodes` 完全相同，说明 DP 阶段基本没有继续把 coarse cluster 合并大
- 最终 `ops / supernode` 的 `max=92141` 也进一步说明，replication 和 final materialize 之后，最终 `op` 数已经不再受这个值直接约束
- 因此最终 `ops / supernode` 的平均值仍然远低于 `75`
- 额外静态排查发现，当 `buildDpSegments()` 遇到等成本候选时，旧实现会优先选择 `begin` 更大的方案，也就是偏向更短的末段；这会进一步压低 segment fill ratio
- 该 tie-break 已在 `2026-04-12` 改为“同成本优先更长 segment”，后续 sweep 需要以新结果为准

### `max-op-in-compute-supernode=75`（tie-break 修复后重跑）

命令：

```bash
make xs_wolf_grhsim_emit XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE=75
```

日志：

- [xs_wolf_grhsim_build_20260412_020705.log](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/logs/xs/xs_wolf_grhsim_build_20260412_020705.log)
- [activity_schedule_supernode_stats.json](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/activity_schedule_supernode_stats.json)

结果：

- `supernodes=64616`
- `seed_supernodes=5883646`
- `coarse_supernodes=1389863`
- `dp_supernodes=88477`
- `replication_cloned=5663308`
- `replication_erased=2323791`
- `activity-schedule` 耗时 `194666 ms`
- `write_grhsim_cpp` 耗时 `38500 ms`
- 总耗时 `269426 ms`
- emit 目录总大小 `3739475879` bytes，约 `3.74 GB`
- emit 文件数 `16041`

最终 `ops / supernode` 分布：

- 平均值 `142.738`
- 中位数 `75.0`
- `p90=218`
- `p99=1179`
- 最大值 `96887`

和修复前的同参数基线相比：

- `supernodes: 815101 -> 64616`
- `dp_supernodes: 1389863 -> 88477`
- `ops_mean: 12.035 -> 142.738`
- `ops_median: 2 -> 75.0`

这次结果说明：

- 问题核心确实在 `buildDpSegments()` 的 tie-break，而不是 `max-op-in-compute-supernode` 根本没有传进去
- `coarse_supernodes` 基本不变，说明改动没有改变 coarsen 形状，收益主要来自 DP 真正开始把 coarse cluster 装进更长 segment
- 修复后，`max-op-in-compute-supernode=75` 已经足以把最终 `supernodes` 压到 `64k` 量级
- 因此此前关于“单调调大 `max-op-in-compute-supernode` 也难以下到 `9-10 万`”的判断，只适用于 tie-break 修复前的旧实现

### `max-op-in-compute-supernode=1024`

命令：

```bash
make xs_wolf_grhsim_emit XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE=1024
```

日志：

- [xs_wolf_grhsim_build_20260412_001132.log](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/logs/xs/xs_wolf_grhsim_build_20260412_001132.log)

结果：

- `supernodes=703585`
- `seed_supernodes=5883646`
- `coarse_supernodes=1049777`
- `dp_supernodes=1002797`
- `replication_cloned=5593152`
- `replication_erased=1690069`
- `activity-schedule` 耗时 `221216 ms`
- `write_grhsim_cpp` 耗时 `38799 ms`
- 总耗时 `296424 ms`
- emit 目录总大小 `4291554856` bytes，约 `4.29 GB`
- emit 文件数 `17170`

### 对比结论

从修复前的 `128` 提高到修复前的 `1024` 后：

- `supernodes` 从 `802073` 降到 `703585`
- 只减少了 `98488`
- 降幅约 `12.3%`

这说明在 tie-break 修复前的旧实现里，当前 XiangShan 规模下：

- `max-op-in-compute-supernode` 会影响结果
- 但影响不是线性的
- 最终 supernode 数量并不主要受这个参数单独控制
- 最终 `ops / supernode` 的平均值也不会自动逼近这个上限

尤其值得注意的是，修复前旧实现中：

- `75` 时 `coarse_supernodes=1389863`，`dp_supernodes=1389863`
- `128` 时 `dp_supernodes=1315119`
- `1024` 时 `dp_supernodes=1002797`
- 但最终 `supernodes` 仍然有 `703585`

以及：

- `75` 时最终 `ops / supernode` 平均值只有 `12.035`
- 中位数只有 `2`

这说明在修复前旧实现中，真正的瓶颈已经不只是 DP 分段上限，还包括：

- coarsen 的图结构约束
- symbol partition
- replication 之后的最终 materialize 形状

## 对 “90k-100k supernode” 目标的判断

基于 `2026-04-12` 的 XiangShan 实测，可以明确记录：

- 在 tie-break 修复前，不能指望仅通过调大 `max-op-in-compute-supernode`，把 `supernodes` 从 `70-80 万` 直接压到 `9-10 万`
- 在 tie-break 修复后，`max-op-in-compute-supernode=75` 已经可以把 `supernodes` 压到 `64616`

因此，后续优化方向应区分两件事：

- 如果目标只是把数量降到 `90k-100k` 量级，当前 tie-break 修复已经足够，甚至已经低于该目标
- 如果目标是继续理解为什么结果会直接降到 `64k`，则应继续排查 replication 和 final materialize 是否又进一步合并了大量活跃区块

如果接下来还要继续做结构优化，仍然应优先排查：

- coarsen 规则是否仍然过于保守
- replication 策略是否产生了过多活跃分裂
- final materialize / symbol partition 是否保留了过多切口

而不是继续单独放大 `max-op-in-compute-supernode`。

## 相关实现与脚本变更记录

这轮实验还伴随了几项和 GrhSIM 流程相关的实现调整：

- `scripts/wolvrix_xs_grhsim.py` 在 `activity-schedule` 之后不再 `store_json`
- `Makefile` / `scripts/wolvrix_xs_grhsim.py` 已支持通过 `XS_WOLF_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE` / `WOLVRIX_XS_GRHSIM_MAX_OP_IN_COMPUTE_SUPERNODE` 传参
- `grhsim-cpp` emit 已支持将超大的 state/init/commit 文件拆分为多个 `.cpp`
- emitter 会清理同前缀旧产物，避免重复 emit 到同一路径时残留过期 `sched_*.cpp`

如果后续继续做 sweep，建议至少记录：

- `max-op-in-compute-supernode`
- `supernodes`
- `coarse_supernodes`
- `dp_supernodes`
- `replication_cloned`
- `replication_erased`
- `activity-schedule` 耗时
- emit 总大小

## 2026-04-12：grhsim-cpp 编译可用性修复

在默认 `max-op-in-compute-supernode=72`、tie-break 修复已经落地的基础上，这一轮继续处理的是 `grhsim-cpp` 的 C++ 编译体量问题，而不是 supernode 数量本身。

核心修改在 [grhsim_cpp.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/wolvrix/lib/emit/grhsim_cpp.cpp)：

- `ValueStorageClass` 重新整理为四类：`LocalScratch`、`BatchScratch`、`EvalScratch`、`ObjectState`
- 新增 `EvalScratchFrame`，把大量“只需在一次 `eval()` 内存活”的临时值移出 model 对象成员
- `eval_batch_*()` 改为显式接收 `EvalScratchFrame &`
- `refresh_outputs()` 改为显式接收 `const EvalScratchFrame &`
- 宽位逻辑值也不再默认落到对象成员，而是按 word count 放入 `eval_frame.logic_words_N[...]`
- 带 `valueFanout` 的 logic 中间值不再一律物化为 `val__...` 成员，而是尽量放到 `EvalScratch`
- `systemTaskArgExpr()` 改为统一走 `valueRef(...)`，避免 system task 代码路径继续直接引用已迁移走的 `val__...` 字段

这轮修改背后的判断是：

- 编译时间的主瓶颈不是单个 `sched_*.cpp` 太大
- 更大的问题是每个 TU 都要重复解析一个包含海量 `val__...` 成员声明的超大头文件
- 因此正确方向是减少对象成员物化数量，而不是继续机械地把 `.cpp` 切得更碎

### 头文件体量变化

以 XiangShan `SimTop` 为例，这轮修改前后，[grhsim_SimTop.hpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop.hpp) 的体量变化为：

- 早期问题状态：`474556002` bytes，`7453831` 行，`val__` 出现 `5663649` 次
- 仅把部分 scalar logic 迁到 eval scratch 后：`294045391` bytes，`3642140` 行，`val__` 出现 `1976459` 次
- 本轮完成 scalar + wide logic 迁移后：`146340646` bytes，`1232537` 行，`val__` 出现 `13654` 次

也就是说，相比最初问题状态：

- 头文件大小降到约 `30.8%`
- 行数降到约 `16.5%`
- `val__` 成员/引用数量降到约 `0.24%`

后续在继续定位 `grhsim-cpp` 运行语义问题时，这套 `ValueStorageClass` / `EvalScratchFrame` 分层又被整体撤回了。当前实现重新回到更直接的模型：

- 所有非输入 value 统一分配到持久 `value_*_slots_`
- `eval_batch_*()` / `refresh_outputs()` 不再传 `eval_frame`
- 先消除 emitter 自身的分类分叉，再单独排查 `grhsim-cpp` 的调度与执行语义

### 完整 `emu` 编译结果

这轮在 XiangShan difftest 目录下完成了完整 `emu` 构建：

```bash
NOOP_HOME=/workspace/gaoruihao-dev-gpu/wolvrix-playground/testcase/xiangshan \
make -C testcase/xiangshan/difftest emu \
  BUILD_DIR=/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim \
  GEN_CSRC_DIR=/workspace/gaoruihao-dev-gpu/wolvrix-playground/testcase/xiangshan/build/generated-src \
  NUM_CORES=1 WITH_CHISELDB=0 WITH_CONSTANTIN=0 \
  GRHSIM=1 \
  GRHSIM_MODEL_DIR=/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit
```

结果：

- `7008` 个 `sched_*.cpp` 全部编译通过
- 后续静态库归档与最终链接也成功
- 产物为 [build/xs/grhsim/grhsim-compile/emu](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim-compile/emu)
- `emu` 大小 `660178984` bytes，约 `629.6 MiB`

按产物时间戳估算：

- 首个对象文件时间：`2026-04-12 12:07:51`
- `emu` 完成时间：`2026-04-12 12:23:49`
- 整轮编译耗时约 `15 分 59 秒`

这说明当前这套：

- `max-op-in-compute-supernode=72`
- DP tie-break 修复
- replication 后置拆分
- `EvalScratchFrame` 值存储分层

已经足以把 XiangShan 的 `grhsim` emit 推进到“可以完整编译出 `emu`”的状态。

## 2026-04-12：emit 端去掉“非必要模板代码”

在继续分析 `grhsim-cpp` 编译时间时，发现除了超大公共头之外，生成代码里还存在大量“非必要模板调用”：

- 窄位 scalar concat / bit-pack helper 被生成为 header-only 模板
- 宽位 `slice/cast/scalar-concat` helper 也通过 `std::array<N>` 模板实例化
- `7008` 个 `sched_*.cpp` 会重复解析并实例化这批 helper

针对这个问题，在 [grhsim_cpp.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/wolvrix/lib/emit/grhsim_cpp.cpp) 中做了两轮 emit 侧收缩：

- 第一轮：把以下 helper 从模板改为普通函数
  - `grhsim_cat`
  - `grhsim_cat_prefix`
  - `grhsim_cat_rhs`
  - `grhsim_pack_bits_u64`
  - `grhsim_concat_scalars_u64`
  - `grhsim_concat_uniform_scalars_u64`
- 第二轮：把以下宽位 helper 的调用路径改为“普通 helper + 局部 `std::array` 包装返回值”
  - `grhsim_cast_words`
  - `grhsim_slice_words`
  - `grhsim_concat_scalars_words`
  - `grhsim_concat_uniform_scalars_words`

静态统计结果：

- 初始状态：`sched` 中显式 `grhsim_foo<...>` 调用共 `466169` 次
- 去掉窄位非必要模板后：降到 `170851`
- 再去掉宽位 `slice/cast/scalar-concat` 显式模板调用后：降到 `0`

对应最热点的显式模板调用依次被清掉：

- `grhsim_slice_words`
- `grhsim_cast_words`
- `grhsim_cat_rhs`
- `grhsim_cat`
- `grhsim_concat_uniform_scalars_u64`
- `grhsim_pack_bits_u64`

需要注意的是：

- 截至这次修改，[grhsim_SimTop_runtime.hpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_runtime.hpp) 中仍然还有 `69` 个模板定义
- 这些剩余模板主要是宽位算术/比较/移位等真正按 `N` 专用化的 helper
- 也就是说，这次改动完成的是“emit 不再生成非必要模板调用”，而不是“runtime.hpp 完全无模板”

这个结论支持了一个更明确的工程方向：

- 非必要 helper 尽量改成普通函数
- 只有确实依赖 `N` 专用化收益的宽位算法，才保留模板实现

## 2026-04-12：`value` / `evt_edge` 改为 typed slot array

在继续分析 `grhsim-cpp` 的头文件体积时，确认当前主要瓶颈不是单一 helper，而是生成类把海量对象按“一个值/一个状态/一个 shadow 一个字段”直接展开到了头文件里。

因此先做了第一轮更激进的存储收敛：

- 所有 `value` 不再生成独立字段
- 改为按数据类型落到少量连续 slot 数组
  - `value_bool_slots_`
  - `value_u8_slots_`
  - `value_u16_slots_`
  - `value_u32_slots_`
  - `value_u64_slots_`
  - `value_words_<N>_slots_`
  - `value_string_slots_`
- `evt_edge` 也不再生成独立字段
  - 改为 `event_edge_slots_[idx]`
- `valueRef(...)` / 事件表达式引用统一改成“数组名 + 常量索引”
- `init()` 中的 `kValues` / `kEventEdges` 也改为整池初始化，不再逐 value / 逐 edge 展开

这一轮没有继续保留 `local/batch/eval/object` 的值存储细分。对于非输入 `value`，emit 侧统一视为可索引 object slot。

对 XiangShan (`xs-default`, `max-op-in-compute-supernode=72`) 的实测结果：

- [grhsim_SimTop.hpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop.hpp)
  - 修改前：`273,227,744` bytes
  - 修改后：`145,602,254` bytes
- 头文件中的 `val_*` 成员声明：从海量展开降为 `0`
- 头文件中的 `evt_edge_*` 成员声明：降为 `0`
- 只保留每类 slot array 的少量声明，各 `sched_*.cpp` 通过常量索引访问

这个结果说明：

- “全 value 数组化” 对头文件体积的收益是立刻可见的
- 这一步已经消掉了当前头文件中最大的一块膨胀来源
- 剩余的大头主要转移为
  - `state_reg_* / state_latch_* / state_mem_*`
  - `state_shadow_*`
  - `memory write shadow`

功能回归方面，至少以下 spot-check 已重新通过：

- `make run_hdlbits_grhsim DUT=071 SKIP_PY_INSTALL=1`

后续若继续压缩头文件，下一优先级应是：

- `state shadow` 数组化
- `memory write shadow` 数组化
- `register/latch/memory` 本体按类型建池

## 2026-04-12：`state shadow` / `memory write shadow` 继续池化

在完成 `value` / `evt_edge` 数组化后，头文件里剩下最突出的“非 state 本体”膨胀来源是：

- `state shadow_*`
- `memory write shadow_*`

这一轮继续把这两类对象从“逐字段展开”改成按类型分桶的 slot pool：

- `state shadow`
  - `state_shadow_touched_slots_`
  - `state_shadow_{bool,u8,u16,u32,u64}_slots_`
  - `state_shadow_words_<N>_slots_`
- `memory write shadow`
  - `memory_write_touched_slots_`
  - `memory_write_addr_slots_`
  - `memory_write_data_*`
  - `memory_write_mask_*`

同时：

- `StateShadowDecl` / `WriteDecl` 不再保存字段名字符串
- 改为保存 slot index / scalar kind / word count
- `sched` 写 shadow、`init()` 清零 shadow、`commit_state_updates()` 读回 shadow 都统一走 helper 生成的索引表达式

对 XiangShan (`xs-default`, `max-op-in-compute-supernode=72`) 的继续实测结果：

- [grhsim_SimTop.hpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop.hpp)
  - `value` 池化后：`145,602,254` bytes
  - `state shadow` / `memory write shadow` 也池化后：`47,111,248` bytes

也就是说，相比最初的字段展开版本：

- 初始：`273,227,744` bytes
- 现在：`47,111,248` bytes

头文件体积已经下降到原来的约 `17.2%`。

静态观察上：

- `val_*` 成员声明：`0`
- `evt_edge_*` 成员声明：`0`
- `state shadow` / `memory write shadow` 也不再按对象逐字段展开
- 头文件中剩余的 `state_shadow_` / `memory_write_` 命中主要是
  - 少量 pool 声明
  - `commit_state_shadow_chunk_*` / `commit_write_chunk_*` 方法声明

回归点验：

- `make run_hdlbits_grhsim DUT=071 SKIP_PY_INSTALL=1` 通过
- `make run_hdlbits_grhsim DUT=117 SKIP_PY_INSTALL=1` 通过

做到这一步以后，`grhsim-cpp` 头文件剩余的大头已经非常明确：

- `state_reg_*`
- `state_latch_*`
- `state_mem_*`

如果继续压缩头文件，下一步最值得做的就是把 state 本体也按类型建池。

## 2026-04-12：`state` 本体也改为 typed pool

在前两轮之后，头文件剩余的主要膨胀来源已经只剩：

- `state_reg_*`
- `state_latch_*`
- `state_mem_*`

这一轮继续把 state 本体也从“逐字段对象”改成了 typed pool：

- `register` / `latch`
  - `state_logic_{bool,u8,u16,u32,u64}_slots_`
  - `state_logic_words_<N>_slots_`
- `memory`
  - `state_mem_{u8,u16,u32,u64}_slots_`
  - `state_mem_words_<N>_slots_`

实现上：

- `StateDecl` 不再保存 `fieldName`
- 改为保存
  - `slotIndex`
  - `scalarKind`
  - `wordCount`
- 统一通过 `stateRef(state)` 生成访问表达式
- `register/latch` 直接引用 pool 元素
- `memory` 引用 pool 中的 `vector<elem>` / `vector<array<...>>`
- `init()` 新增独立的 `kStateStorage` 阶段，先分配 state pool，再做逐 state 初始化

对 XiangShan (`xs-default`, `max-op-in-compute-supernode=72`) 的实测结果继续下降：

- 初始字段展开版本：`273,227,744` bytes
- `value + evt_edge` 池化后：`145,602,254` bytes
- `state shadow + memory write shadow` 池化后：`47,111,248` bytes
- `state` 本体也池化后：
  - [grhsim_SimTop.hpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop.hpp)
  - `411,948` bytes

静态上可见的变化：

- `state_reg_*`：`0`
- `state_latch_*`：`0`
- `state_mem_*` 的命中只剩少量 pool 声明
- 头文件里 state 相关主体已经收敛为几十行 pool 声明，而不再是几十万行成员展开

这意味着头文件体积相比最初版本已经下降到约 `0.15%`。

回归点验：

- `make run_hdlbits_grhsim DUT=071 SKIP_PY_INSTALL=1` 通过
- `make run_hdlbits_grhsim DUT=117 SKIP_PY_INSTALL=1` 通过

做到这一步后，`grhsim-cpp` 的“超大头文件”问题基本已经被结构性解决。

## 2026-04-13：单线程 `grhsim` / XiangShan coremark 早期失败排查

本节只记录单线程 `grhsim` 的排查结果，与 `repcut` 无关。

### 当前稳定复现点

直接运行当前增量链接后的 `emu`：

```bash
cd /workspace/gaoruihao-dev-gpu/wolvrix-playground
./build/xs/grhsim/grhsim-compile/emu \
  -i ./testcase/xiangshan/ready-to-run/coremark-2-iteration.bin \
  --diff ./testcase/xiangshan/ready-to-run/riscv64-nemu-interpreter-so \
  -b 0 -e 0
```

稳定复现结果：

- `instrCnt = 2`
- `cycleCnt = 662`
- 首先命中的断言包括：
  - [MEFreeList.sv](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/rtl/rtl/MEFreeList.sv:2026)
  - [StdFreeList.sv](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/rtl/rtl/StdFreeList.sv:1381)
  - [StdFreeList_1.sv](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/rtl/rtl/StdFreeList_1.sv:652)

这三个断言里，当前最先拿到有效运行时证据的是 `MEFreeList.sv:2026`。

### 参考波形对拍结论

参考波形文件：

- [tmp/xs_wolf_20260413_165458.fst](/workspace/gaoruihao-dev-gpu/wolvrix-playground/tmp/xs_wolf_20260413_165458.fst)

波形工具验证可用：

- [fst_tree.py](/workspace/gaoruihao-dev-gpu/wolvrix-playground/tools/fst_tools/fst_tree.py)
- [fst_cycle_trace.py](/workspace/gaoruihao-dev-gpu/wolvrix-playground/tools/fst_tools/fst_cycle_trace.py)

在参考波形中，`intFreeList` 就是 RTL 中 `MEFreeList intFreeList (...)` 的实例。抽取 `cycle 656..662` 后，得到：

- `headPtrOH[223:0]` 在 `cycle 657..662` 一直是 bit `2`
- `headPtr_flag = 0`
- `headPtr_value = 2`
- `debugArchHeadPtr_flag = 0`
- `debugArchHeadPtr_value = 1`
- `debugArchHeadPtr_REG_flag = 0`
- `debugArchHeadPtr_REG_value = 1`
- `freeRegCntReg = 0xdd`
- `io_doAllocate = 1`
- `io_allocateReq_0..7 = 0,0,0,0,0,0,0,0`

也就是说，参考侧在失败窗口内的行为非常稳定：

- `headPtrOH` 明确是 bit `2`
- `headPtr_value` 也明确是 `2`

### `grhsim-cpp` 插桩结果

为了避免 `make xs_wolf_grhsim_emu` 重跑 emit 覆盖手工修改，当前排查采用：

- 直接修改生成文件：
  - [grhsim_SimTop_sched_1470.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_1470.cpp)
  - [grhsim_SimTop_sched_4336.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_4336.cpp)
  - [grhsim_SimTop_sched_4475.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_4475.cpp)
- 只增量重编命中的 `sched_*.o`
- 直接运行现成的 [emu](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim-compile/emu)

在 `MEFreeList.sv:2026` 断言前拿到的第一条有效插桩是：

```text
[grhsim-assert] eval=1431 file=build/xs/rtl/rtl/MEFreeList.sv line=2026 clock=1 reset=0 cond=1 headptr_neq=1 lhs_words=[0x4,0x0,0x0,0x0] rhs_words=[0x400000000,0x0,0x0,0x0] stop_cond=1
```

它对应的事实是：

- `lhs_words = 0x4`，也就是 bit `2`
- `rhs_words = 0x400000000`，也就是 bit `34`

### 当前有效结论

把参考波形和 `grhsim` 插桩对在一起后，可以得到当前最强结论：

- `grhsim` 在 `MEFreeList.sv:2026` 比较的左侧 one-hot，和参考波形是一致的
  - 都是 bit `2`
- `grhsim` 在同一比较的右侧 one-hot，已经偏成 bit `34`
  - 这与参考波形明显不一致

因此当前问题已经明确收敛为：

- 不是“参考值不确定”
- 不是“最后断言自己算错了”
- 而是 `MEFreeList.sv:2026` 比较参与项中的一侧 one-hot cone，在 `grhsim-cpp` 运行时已经偏离到 bit `34`

### 说明

这一节已经移除此前的推测性判断，只保留当前由运行时对拍直接支持的结论。原因很简单：

- `sv -> sv` 路径已证明正常
- `post_stats` 和 `xs_wolf.json` 中同类编码可以成功 `read_json -> emit_sv`
- 当前最强证据来自运行时对拍：
  - 参考波形右值对应 bit `2`
  - `grhsim` 断言现场右值已经偏到 bit `34`

### 后续排查方向

当前最值得继续追的是：

- `MEFreeList.sv:2026` 比较右侧 `rhs_words=[0x400000000,...]` 的生成链
- 为什么这条 one-hot cone 在 `grhsim-cpp` 运行时会跑到 bit `34`
- 同时继续利用参考波形做逐周期比对，并沿比较右侧 one-hot 的生成链继续收敛

### 2026-04-13 追加：`rhs` 上游同拍瞬时错写链

继续沿 `rhs` 上游做生成文件插桩后，已经把这次单线程 `grhsim` 早期失败收敛到一条明确的“同拍瞬时错写”链上。

这轮新增插桩主要加在：

- [grhsim_SimTop_sched_2425.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_2425.cpp)
- [grhsim_SimTop_sched_1571.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_1571.cpp)
- [grhsim_SimTop_sched_226.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_226.cpp)
- [grhsim_SimTop_sched_5835.cpp](/workspace/gaoruihao-dev-gpu/wolvrix-playground/build/xs/grhsim/grhsim_emit/grhsim_SimTop_sched_5835.cpp)

关键事实：

- `redirectGen$s1_redirect_valid_reg_last_REG` 的本地 mux 链在坏窗口里并不是“稳定算成 1”
- 相反，`sched_2425` 在同一个 `eval` 内会被重复触发
- 在 `eval=1423`，同一拍里先后出现了两次不同结果：
  - 第一轮：`src5198672=1`，于是 `_op_4801683` 算出 `next1693210=1`
  - 第一轮同时触发 `_op_4801686`，在 `posedge` 上把这个瞬时 `1` 写进 `state_shadow_bool_slots_[35953]`
  - 第二轮：`src5198672` 已经回到 `0`，`_op_4801683` 也回到 `0`
- 但第二轮已经不能再覆盖第一轮写入，因为对应 `event_edge` 只在第一次命中 `posedge`

这解释了为什么：

- `eval=1424/1425` 时
  - `value_bool_slots_[1693210]` 已经回到 `0`
  - 但 `value_bool_slots_[1693161] / state_logic_bool_slots_[36960]` 仍然变成了 `1`

沿 `rhs` 继续往上追，当前已经确认这条瞬时 `1` 的传播链为：

1. `state_logic_bool_slots_[42939]`
2. `value_bool_slots_[626213]`
3. `value_bool_slots_[6879360]`
4. `value_bool_slots_[5403883]`
5. `value_bool_slots_[5198672]`
6. `value_bool_slots_[5022462]`
7. `value_bool_slots_[1693210]`
8. `state_logic_bool_slots_[36960]`

其中本轮已经排除的分支：

- `value_bool_slots_[5536218] -> value_bool_slots_[5403876]`
  - 在坏窗口里始终是 `0`
- `value_bool_slots_[5687895]`
  - 在坏窗口里始终是 `1`

因此当前新的最强收敛点是：

- 真正造成瞬时 `1` 的不是 `5403876` 支路
- 而是 `6879360` 在同拍内先出现 `1`、随后又回到 `0`
- `6879360` 又只是 `value_bool_slots_[626213]` 的边界复制
- `626213` 则来自 `state_logic_bool_slots_[42939]`

也就是说，当前最值得继续追的源头已经不再是 `redirectGen` 本地组合 mux 本身，而是：

- 谁在更上游先把 `state_logic_bool_slots_[42939]` 错写成了 `1`
- 以及为什么这次错写会在同一个 `eval` 内先传播到 `6879360`，再被后续计算改回，但已经来不及撤销 `posedge` 写入
