#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP

#include "grhsim/am/grhsim_am_graph_partition.hpp"

namespace wolvrix::lib::grhsim::am
{

    // opt-am-compute-graph：compute 图上的图级优化阶段。当前为空（预留阶段
    // 边界），未来的 compute 图优化（合并消除、活动度感知改写等）落在这里。
    void optAmComputeGraph(AmComputeGraph &computeGraph,
                           const AmGraphPartitionInput &input);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_OPTIMIZE_HPP
