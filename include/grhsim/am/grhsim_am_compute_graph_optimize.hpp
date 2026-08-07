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

    // opt-am-compute-graph 的 mux-merge atom pass：把 compute 侧 select 相同
    // 的 mux 组（成员都是单指令 atom）连同其独占使用的生产者锥合并为不可
    // 分割的调度 atom，重写 context 的 atom 表并重建 compute/commit 两张诱
    // 导子图。组内 mux 与吸收锥的总指令数超过 muxAtomMax 时整组放弃（emit
    // 端块内同 select 融合兜底）。返回 false 表示内部错误（diagnostics 已
    // 填）；合并组在 atom DAG 上成环时自动解除合并，不算错误。
    bool mergeMuxSelectAtoms(AmGraph &graph, AmGraphSplitContext &context,
                             std::size_t muxAtomMax,
                             wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP
