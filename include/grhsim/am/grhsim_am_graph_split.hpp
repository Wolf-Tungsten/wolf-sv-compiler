#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_SPLIT_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_SPLIT_HPP

#include "grhsim/am/grhsim_am_graph_partition.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    // Split-stage working types shared by the scheduling stages (the helper
    // algorithms over them live in lib/grhsim/am/grhsim_am_common.hpp).
    struct DefUseIndex
    {
        std::vector<uint32_t> definitions;
        std::vector<uint32_t> useOffsets;
        std::vector<uint32_t> uses;
    };

    struct OrderEdge
    {
        uint32_t source = 0;
        uint32_t target = 0;
    };

    struct CsrGraph
    {
        std::vector<uint32_t> offsets;
        std::vector<uint32_t> targets;
    };

    // Owning working set produced by the split-am-graph stage driver. The
    // downstream partition and materialize stages read the atom tables and
    // the split graphs from here. The spans inside the AmGraphPartitionInput
    // assembled by partitionInput() address this context's storage, so the
    // caller must keep the context alive while the input is in use.
    struct AmGraphSplitContext
    {
        uint32_t instructionCount = 0;
        uint32_t variableCount = 0;
        uint32_t atomCount = 0;
        DefUseIndex defUse;
        std::vector<OrderEdge> orderedEdges;
        CsrGraph atomGraph; // atom DAG CSR (SCC condensation)
        std::vector<uint32_t> instructionAtom; // instruction -> atom
        std::vector<uint32_t> atomMemberOffsets; // atom -> member instructions CSR
        std::vector<uint32_t> atomMembers;
        std::vector<uint32_t> atomInstructions; // per-atom instruction count
        std::vector<uint32_t> atomStateWrites;
        std::vector<uint8_t> atomIsCommit;
        std::vector<uint32_t> atomMinInstruction;
        std::vector<uint32_t> commitEventRank;
        // Atom 分类学（NO0007 P1）：Singleton/CombLoopScc/CommitEvent 由
        // split 阶段标注（CommitEvent 的 signature 为 commitEventRank），
        // MuxMerge 由 mux-merge atom pass 在重建时标注（signature 为组内
        // 共享的 select 变量 id）。
        std::vector<uint8_t> atomKinds;
        std::vector<uint32_t> atomSignatures;
        std::size_t oversizedAtomCount = 0;
        std::size_t maxAtomInstructions = 0;
        std::size_t maxAtomStateWrites = 0;
        // Scheduling limits folded into the assembled partition input.
        std::size_t maxAtomsPerBlock = 128;
        std::size_t maxCommitAtomsPerBlock = 4096;
        bool enableCoarsening = true;
        std::size_t coarsenAtomBudget = 256;
        double segmentPenalty = 1.0;
        std::size_t refinementRounds = 10;
        AmGraphSplit split;

        // Assembles the span POD consumed by the partition passes; the spans
        // address this context's storage (see the lifetime note above).
        AmGraphPartitionInput partitionInput() const;
    };

    // split-am-graph stage driver: builds the def-use index and the ordered
    // edges, validates the state-writer groups, packs instructions into
    // scheduling atoms (SCC + intra-atom ordering), classifies the atom
    // costs, ranks the commit events, and splits the atom DAG into the
    // compute and commit induced subgraphs.
    std::optional<AmGraphSplitContext>
    splitAmGraphStage(AmGraph &graph,
                      const ActivityScheduleOptions &options,
                      wolvrix::lib::diag::Diagnostics &diagnostics);

    // Research export of the pre-scheduling instruction graph (def-use +
    // ordered-effect edges plus the atom packing) as JSONL. Atom fields come
    // from the split context, so the export reflects the mux-merge atom pass
    // when the orchestrator ran it (post-merge 口径).
    bool exportInstructionGraphJsonl(ProgramView program,
                                     const AmGraphSplitContext &context,
                                     const std::filesystem::path &path,
                                     wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_SPLIT_HPP
