#ifndef WOLVRIX_GRHSIM_AM_ACTIVITY_SCHEDULE_HPP
#define WOLVRIX_GRHSIM_AM_ACTIVITY_SCHEDULE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    // POD view of the scheduled atom DAG so the coarsen+DP block formation does
    // not depend on types local to ProductionActivityScheduleStage::schedule().
    struct GrhSimAmActivityScheduleInput
    {
        uint32_t atomCount = 0;
        std::span<const uint32_t> atomOffsets; // atom DAG CSR, size atomCount + 1
        std::span<const uint32_t> atomTargets;
        std::span<const uint32_t> atomInstructions; // per-atom instruction count
        std::span<const uint32_t> atomStateWrites;
        std::span<const uint8_t> atomIsCommit;
        std::span<const uint32_t> atomMinInstruction;
        std::span<const uint32_t> commitEventRank;
        std::span<const uint32_t> commitGuardRank;
        uint32_t variableCount = 0;
        std::span<const uint32_t> definitions; // variable -> defining instruction (kInvalid if none)
        std::span<const uint32_t> useOffsets;  // variable -> using instructions CSR
        std::span<const uint32_t> uses;
        std::span<const uint32_t> instructionAtom; // instruction -> atom
        std::size_t maxInstructionsPerBlock = 128;
        std::size_t maxCommitInstructionsPerBlock = 4096;
        bool enableCoarsening = true;   // out1/in1/sibling coarsen stages
        std::size_t coarsenBudget = 64; // coarsen cluster instruction cap
        double segmentPenalty = 1.0;    // DP fixed cost per segment boundary
    };

    struct GrhSimAmActivityScheduleResult
    {
        std::vector<uint32_t> atomBlock; // 1..normalBlockCount
        std::vector<uint32_t> atomTopo;
        uint32_t normalBlockCount = 0;
        uint32_t computeBlockCount = 0;
        uint32_t commitBlockCount = 0;
        std::size_t clustersAfterCoarsen = 0;
        std::size_t dpSegments = 0;
        uint64_t coarsenMs = 0;
        uint64_t dpMs = 0;
        std::size_t coarsenRounds = 0;
        std::size_t coarsenOut1Merges = 0;
        std::size_t coarsenIn1Merges = 0;
        std::size_t coarsenSiblingMerges = 0;
        std::string initialDegreeHistogram;
    };

    // Coarsen+DP block formation: iterative out1/in1/sibling atom merging,
    // deterministic cluster topo, then a DP that cuts the cluster sequence into
    // compute blocks minimizing incoming activation edges plus a per-segment
    // penalty. Commit atoms are blocked separately afterwards.
    std::optional<GrhSimAmActivityScheduleResult>
    scheduleGrhSimAmActivityBlocks(const GrhSimAmActivityScheduleInput &input,
                                   std::string &error);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_ACTIVITY_SCHEDULE_HPP
