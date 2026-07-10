# activity-schedule

## 功能概述

`activity-schedule` pass 为单个 graph 构建 GrhSIM 使用的静态 activity
schedule，并把结果写入 session。当前只保留 plain 调度路径；其他实验调度路径和
相关诊断导出都不再作为实现或接口存在。

这个 pass 不会生成新的 wrapper/module，但会：

- 为缺失 symbol 的可分区 op 补内部 symbol
- 冻结 graph 并建立 def-use 信息
- 按 `ActivityOpClass::{Source,Sink,Compute,Declaration}` 分类 op
- 将 `ActivityOpClass::Source` 到 compute op 的 use 前置 clone
- 构造初始 compute supernode / commit node 中间模型
- 在 compute-supernode DAG 上执行 plain coarsen 和连续分段
- 展开最终 `computeSupernode` / `commitSupernode` 调度模型
- 将 schedule 写入 session，供 `grhsim-cpp` emit 使用

完整运行时术语和静态到运行时映射见
[GrhSIM Scheduling](../emit/grhsim-scheduling.md)。

## 路径语义

`-path` 的解析规则和其他 path-based pass 一致。

- 单段路径：直接按 graph 名选中目标 graph
- 多段路径：`<root>.<inst>...`

多段路径从 root graph 开始，逐层按实例 `instanceName` 查找，并通过实例的
`moduleName` 进入下一级 graph。

## 选项

`ActivityScheduleOptions` 定义在
[activity_schedule.hpp](../../include/transform/activity_schedule.hpp)。

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `-path` | 无 | 目标 graph / 实例路径，必填 |
| `-max-op-in-compute-supernode` | `128` | coarsen 和连续分段的 op 数上限；不会拆开单个初始 MFFC compute supernode |
| `-max-op-in-commit-supernode` | `4096` | 单个 commit supernode 最多包含的 sink op 数 |
| `-local-shared-compute-max-fanout` | `4` | local shared compute clone 的 fanout 上限 |
| `-local-shared-compute-max-width` | `256` | local shared compute clone 的 value 宽度上限 |
| `-split-oversize-compute-supernode-max-ops` | `0` | 超大初始 compute supernode split 的 chunk 上限；为 0 时使用 `max-op-in-compute-supernode` |
| `-disable-coarsen` | `false` | 关闭 plain coarsen |
| `-disable-chain-merge` | `false` | 关闭 plain `out1` / `in1` chain merge；`siblings` 仍可执行 |
| `-enable-local-shared-compute` | `false` | 允许克隆局部共享 compute producer |
| `-disable-commit-guard-event-buckets` | `false` | 关闭 commit guard/event bucket 分组 |
| `-split-oversize-compute-supernodes` | `false` | materialize 阶段拆分超过上限的单个初始 compute supernode |
| `-declared-value-compute-supernode-boundary` | `false` | 把带 declared symbol 的 value 作为初始 compute-supernode 截断边界 |
| `-export-compute-dag` | 无 | 导出 `wolvrix.compute-op-dag.v1` compute op DAG JSON |

兼容说明：旧参数 `-max-op-in-compute-node` 仍会被 parser 接受但已不影响调度；
旧的 `compute-node` split / declared-boundary 参数名会映射到对应的
`compute-supernode` 参数。

## Plain 调度路径

当前主路径如下：

1. `buildActivityOpData(...)` 收集可调度 op，并按 operand def-use 建 op-level DAG。
2. `cloneSourceUsesForCompute(...)` 复制 source-class op 到 compute 用户侧；source-class
   value 到 commit 的 use 保留原始 value。
3. 如果发生 clone，pass 重新 `freeze()` 并重建 `ActivityOpData` / `opClasses`。
4. `buildInitialComputeSupernodes(...)` 先按 sink event key / guard key 构造
   `commitNodes`，再从 commit input、output/inout 根和无 result compute op 出发
   构造初始 MFFC compute supernode。初始 MFFC 不按 op 数截断。
5. `buildComputeSupernodeDag(...)` 基于初始 compute supernode 的 `boundaryInputs`
   建 compute-supernode DAG；如果产生 cycle，会把相关多 op compute supernode 拆回
   singleton 后重建。
6. `materializeComputeSupernodeSchedule(...)` 从每个初始 compute supernode 的
   singleton cluster 开始，在 cluster DAG 上执行 plain coarsen。
7. 对 coarsen 后的 cluster topo 序列做连续分段，分段上限为
   `max-op-in-compute-supernode`。
8. 展开 compute segment 为 `computeSupernode`，追加 commit node 形成
   `commitSupernode`，然后重建最终 `dag`、`value_fanout`、`topo_order` 和
   `state_read_supernodes`。

plain coarsen 按以下顺序只执行一轮合并，然后直接进入 DP：

- `out1`：producer 只有一个 compute successor
- `in1`：consumer 只有一个 compute predecessor
- `siblings`：拥有相同 predecessor 集合的 sibling clusters

`out1` / `in1` 合并上限为 `16 * max-op-in-compute-supernode`；`siblings` 合并上限为
`16 * max-op-in-compute-supernode`。`max-op-in-compute-supernode=0` 时这些 coarsen
上限视为无限制。每个 stage 批量合并后都会重新做 topo check。
`out1` / `in1` 受 `enableChainMerge` 控制；`siblings` 属于 plain coarsen 基础路径。

连续分段只决定 topo 序列中相邻 cluster 如何合并成最终 compute supernode，不做任意 DAG
partition。除单个 coarsen cluster 已经超过上限的情况外，分段上限仍为
`max-op-in-compute-supernode`；DP 不会拆开单个 oversize cluster。分段成本是：

```text
incoming_boundary_activation_edges + 1
```

同成本时偏向更长 segment。

## 结构统计口径

pass 会在日志中输出 `activity-schedule compute-supernode out-degree detail`。
同一份数据也会写入 Session key
`<target>.activity_schedule.initial_compute_supernode_out_degree`。

该统计基于 coarsen 前的初始 compute-supernode DAG，口径为 `value_target_dedup`：

- 节点是 `buildInitialComputeSupernodes(...)` 生成并完成 cycle split 后的初始
  compute supernode。
- 若 compute supernode `sn1` 产生的一个或多个 value 被 compute supernode `sn2` 作为
  boundary input 读取，则计一条 `sn1 -> sn2` 出边。
- 同一对 `(sn1, sn2)` 即使由多个 value 连接，也只计一条出边。
- 不统计 `sn -> sn` 自环；compute DAG 构造时会跳过同 supernode dependency。
- 不统计 compute -> commit 边；该分布只描述初始 compute-supernode 之间的 value target。

输出字段：

- `nodes`：初始 compute supernode 数。
- `edges`：去重后的初始 compute-supernode DAG 边数，等于所有 node 出度之和。
- `mean_milli`：平均出度乘以 1000 的整数值。
- `p50` / `p90` / `p99` / `max`：出度分位数和最大值。
- `buckets`：按出度 bucket 统计 node 数，bucket 为
  `0,1,2,3-4,5-8,9-16,17-32,33-64,65-128,129-256,257-512,513-1024,>1024`。

Session 中的类型为 `ActivityScheduleComputeSupernodeOutDegreeStats`；字段含义同日志，
其中 `meanMilli` 对应日志字段 `mean_milli`。

## Session 输出

对 `path=<target>`，pass 写入以下 key：

- `<target>.activity_schedule.supernode_to_ops`
- `<target>.activity_schedule.op_to_supernode`
- `<target>.activity_schedule.dag`
- `<target>.activity_schedule.supernode_kind`
- `<target>.activity_schedule.value_fanout`
- `<target>.activity_schedule.topo_order`
- `<target>.activity_schedule.state_read_supernodes`
- `<target>.activity_schedule.initial_compute_supernode_out_degree`

## Compute DAG 导出

`-export-compute-dag=<path>` 在 `buildInitialComputeSupernodes(...)` 之后、最终 materialize
之前导出 compute 侧 DAG。导出文件是 `topo-graph-partition-harness` 的输入协议，
不暴露任何旧策略的内部权重模型。

导出语义：

- JSON `format` 固定为 `wolvrix.compute-op-dag.v1`
- `options.node_granularity` 固定为 `op`
- node 对应一个 compute op
- `op_id` 记录原始 GRH operation id
- `topo_pos` 是导出时重新排序后的 op-level topo 位置
- edge 对应从 `src` op result value 到 `dst` op operand 的依赖
- `options.edge_weight` 固定为 `value_bitwidth_words`
- `edges[].values[]` 记录该 op pair 上的 distinct value，每个 value 只带 `id` 和 `width`
- `edges[].weight = ceil(sum(edges[].values[].width) / 64)`，最小为 1
- 不导出 commit node / sink op
