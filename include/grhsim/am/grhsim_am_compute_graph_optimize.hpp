#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP

#include "core/diagnostics.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_graph_partition.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"

#include <cstddef>

namespace wolvrix::lib::grhsim::am
{

    // opt-am-compute-graph：compute 图上的图级优化阶段。当前为空（预留阶段
    // 边界），未来的 compute 图优化（合并消除、活动度感知改写等）落在这里。
    void optAmComputeGraph(AmComputeGraph &computeGraph,
                           const AmGraphPartitionInput &input);

    // opt-am-compute-graph 的 tree-atom 形成 pass（NO0008）：compute 侧
    // 单用纯生产者折叠进其唯一消费者的 atom，使每个 atom 成为单输出表达式
    // 树（对齐 gsim "node = 信号 + assignTree" 的图基元语义；mux 根树即
    // when 树）。屏障：commit 侧 atom、comb-loop SCC atom、非纯副作用、
    // pinned（interface/observable/external）结果。折叠关系是指令 DAG 上
    // 的静态森林（汇合、确定性、不保序外的成环可能），折叠后重建 atom 表
    // 与 compute/commit 两张诱导子图。mux 根 atom 的 signature 记 select
    // 变量 id，其余 compute atom 记 kInvalidAtomSignature。返回 false 表示
    // 内部错误（diagnostics 已填）。
    bool foldSingleOutputTreeAtoms(AmGraph &graph, AmGraphSplitContext &context,
                                   wolvrix::lib::diag::Diagnostics &diagnostics);

    // opt-am-compute-graph 的 fanout 吸收 pass（NO0015，tree-atom fold 之后
    // 运行）：把消费方 atom 数 ≥2 的小指令数 compute atom 吸收进全部消费方
    // atom —— 与"atom 建立前按消费方复制指令锥 + fold 吸收"语义等价，消费
    // 方 atom 变胖，不产生新的分区单元（lab 建模口径：修正图锚点档 cross
    // 394,306 → 180,213 ≈ 1.011x gsim @ cap2/预算×1.0）。屏障：非纯成员、
    // 多结果根、ordered-effect 触及、comb-loop/commit/Activation/Host 消费
    // 方；pinned（interface/observable/declared）根保留孤本 atom。反 topo
    // 单遍向上游级联到读口（mem.read 等 StateRead 根本身不复制）。预算按
    // 复制指令数计（budgetMultiplier × compute 指令总数），耗尽即停。
    // maxAtomInstructions = 0 时禁用。返回 false 表示内部错误
    // （diagnostics 已填）。
    bool absorbFanoutAtoms(AmGraph &graph, AmGraphSplitContext &context,
                           std::size_t maxAtomInstructions, double budgetMultiplier,
                           std::size_t maxConsumers,
                           wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP
