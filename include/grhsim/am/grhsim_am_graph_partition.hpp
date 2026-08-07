#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_PARTITION_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_PARTITION_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    // POD view of the scheduled atom DAG so the coarsen+DP block formation does
    // not depend on types local to GrhIRToGrhSimAMProgram::graphToProgram().
    struct AmGraphPartitionInput
    {
        uint32_t atomCount = 0;
        std::span<const uint32_t> atomOffsets; // atom DAG CSR, size atomCount + 1
        std::span<const uint32_t> atomTargets;
        std::span<const uint32_t> atomInstructions; // per-atom instruction count
        std::span<const uint32_t> atomStateWrites;
        std::span<const uint8_t> atomIsCommit;
        std::span<const uint32_t> atomMinInstruction;
        std::span<const uint32_t> commitEventRank;
        uint32_t variableCount = 0;
        std::span<const uint32_t> definitions; // variable -> defining instruction (kInvalid if none)
        std::span<const uint32_t> useOffsets;  // variable -> using instructions CSR
        std::span<const uint32_t> uses;
        std::span<const uint32_t> instructionAtom; // instruction -> atom
        std::size_t maxInstructionsPerBlock = 128;
        std::size_t maxCommitInstructionsPerBlock = 4096;
        bool enableCoarsening = true;   // out1/in1/sibling merge sweeps
        std::size_t coarsenBudget = 256; // merge host member instruction limit
        double segmentPenalty = 1.0;    // DP fixed cost per segment boundary
        // Post-DP local-move refinement rounds (0 = off). Each round scans
        // clusters in topo order and moves a cluster to a neighbor block when
        // the move strictly reduces the exact block-level incoming-copy cost.
        std::size_t refinementRounds = 10;
    };

    // ------------------------------------------------------------------
    // split-am-graph 及其两张子图（框架术语见
    // pdocs/grh-notepad/am-graph/NO0004）：atom DAG 按 compute/commit 拆成
    // 两张诱导子图，之后每张图由各自的分区 pass 独立处理。诱导子图使用保持
    // 全局 atom 相对次序的稠密局部 id，映射表负责来回翻译。
    // ------------------------------------------------------------------

    // GRHSIM AM Compute Graph（atom 级诱导子图）。
    struct AmComputeGraph
    {
        uint32_t atomCount = 0;
        std::vector<uint32_t> globalOfAtom; // compute local -> global atom
        std::vector<uint32_t> localOfAtom;  // global atom -> compute local (invalid for commit)
        std::vector<uint32_t> offsets;      // induced compute DAG CSR (local ids)
        std::vector<uint32_t> targets;
    };

    // GRHSIM AM Commit Graph（atom 级诱导子图）。
    struct AmCommitGraph
    {
        uint32_t atomCount = 0;
        std::vector<uint32_t> globalOfAtom; // commit local -> global atom
        std::vector<uint32_t> localOfAtom;  // global atom -> commit local (invalid for compute)
        std::vector<uint32_t> offsets;      // induced commit DAG CSR (local ids)
        std::vector<uint32_t> targets;
    };

    struct AmGraphSplit
    {
        AmComputeGraph computeGraph;
        AmCommitGraph commitGraph;
    };

    // split-am-graph：校验输入并把 atom DAG 拆成 compute/commit 两张诱导子图。
    // commit atom 指向 compute atom 的依赖在此判非法（state commit 不得喂养
    // pre-commit 工作）。
    std::optional<AmGraphSplit>
    splitAmGraph(const AmGraphPartitionInput &input, std::string &error);

    // opt-am-compute-graph：compute 图上的图级优化阶段。当前为空（预留阶段
    // 边界），未来的 compute 图优化（合并消除、活动度感知改写等）落在这里。
    void optAmComputeGraph(AmComputeGraph &computeGraph,
                           const AmGraphPartitionInput &input);

    // GRHSIM AM Compute Activity Graph：compute 图的活动度划分结果
    // （atomBlock/atomTopo 以 compute 局部 id 索引）。
    struct AmComputeActivityGraph
    {
        std::vector<uint32_t> atomBlock; // per compute-local atom, 1..blockCount
        std::vector<uint32_t> atomTopo;  // compute-local atoms, block-grouped topological
        uint32_t blockCount = 0;
        std::size_t clustersAfterCoarsen = 0;
        std::size_t dpSegments = 0;
        uint64_t coarsenMs = 0;
        uint64_t dpMs = 0;
        std::size_t coarsenRounds = 0;
        std::size_t coarsenOut1Merges = 0;
        std::size_t coarsenIn1Merges = 0;
        std::size_t coarsenSiblingMerges = 0;
        std::string initialDegreeHistogram;
        // Post-DP local-move refinement stats (0 when disabled).
        std::size_t refinementRounds = 0;
        std::size_t refinementMoves = 0;
        uint64_t refinementMs = 0;
        double refinementCostBefore = 0.0;
        double refinementCostAfter = 0.0;
    };

    // partition-am-compute-graph（活动度划分）：compute 子图上做 gsim 风格
    // 单遍 out1/in1/sibling 合并、确定性 LIFO Kahn 重排、Kernighan DP
    // （最小化边界出边割数 + 每边界 segmentPenalty），DP 后接局部移动精化。
    std::optional<AmComputeActivityGraph>
    partitionAmComputeGraph(const AmGraphPartitionInput &input,
                            const AmGraphSplit &split, std::string &error);

    // GRHSIM AM Commit Event Graph：commit 图的事件聚类划分结果
    // （atomBlock/atomTopo 以 commit 局部 id 索引）。
    struct AmCommitEventGraph
    {
        std::vector<uint32_t> atomBlock; // per commit-local atom, 1..blockCount
        std::vector<uint32_t> atomTopo;  // commit-local atoms, bucket emission order
        uint32_t blockCount = 0;
    };

    // partition-am-commit-graph（事件聚类）：commit 子图上按 (event rank,
    // min instruction) 优先级 Kahn，同事件签名桶内限量合并。
    std::optional<AmCommitEventGraph>
    partitionAmCommitGraph(const AmGraphPartitionInput &input,
                           const AmGraphSplit &split, std::string &error);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_PARTITION_HPP
