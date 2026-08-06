#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_PARTITION_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_PARTITION_HPP

#include "grhsim/am/grhsim_am_graph_partition.hpp"

namespace wolvrix::lib::grhsim::am
{

    // partition-am-compute-graph（活动度划分）：compute 子图上做
    // out1/in1/sibling 迭代 coarsen、确定性拓扑、segment DP（最小化跨段
    // incoming 激活成本 + 每段 segmentPenalty）。def-use 成本仍读全局变量
    // 空间，commit atom 内的 use 经分图表跳过。
    std::optional<AmComputeActivityGraph>
    partitionAmComputeGraph(const AmGraphPartitionInput &input,
                            const AmGraphSplit &split, std::string &error);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMPUTE_GRAPH_PARTITION_HPP
