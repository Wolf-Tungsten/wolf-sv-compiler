#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMMIT_GRAPH_PARTITION_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMMIT_GRAPH_PARTITION_HPP

#include "grhsim/am/grhsim_am_graph_partition.hpp"

namespace wolvrix::lib::grhsim::am
{

    // partition-am-commit-graph（事件聚类）：commit 子图上按 (event rank,
    // min instruction) 优先级 Kahn，同事件签名桶内限量合并。
    std::optional<AmCommitEventGraph>
    partitionAmCommitGraph(const AmGraphPartitionInput &input,
                           const AmGraphSplit &split, std::string &error);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMMIT_GRAPH_PARTITION_HPP
