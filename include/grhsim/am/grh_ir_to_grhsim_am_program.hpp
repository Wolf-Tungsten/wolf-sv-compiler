#ifndef WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_PROGRAM_HPP
#define WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_PROGRAM_HPP

#include "core/diagnostics.hpp"
#include "core/grh.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_program_validate.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    ValidationResult validate(const ProgramInterface &interface,
                              ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const SchedulingFacts &facts,
                              ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const LinearProgramArtifact &artifact,
                              const ValidationOptions &options = {});
    ValidationResult validate(const AmGraph &graph,
                              const ValidationOptions &options = {});
    ValidationResult validate(const ExecutableModel &model,
                              const ValidationOptions &options = {});

    struct ActivityScheduleOptions
    {
        // Aligns with the legacy maxOpInComputeSupernode limit.
        std::size_t maxInstructionsPerBlock = 128;
        // Commit blocks may carry a longer ordered state-write chain than a
        // compute block, but never split an indivisible effect atom.
        // Aligns with the legacy maxOpInCommitSupernode limit.
        std::size_t maxCommitInstructionsPerBlock = 4096;
        // Aligns with the legacy enableCoarsen/enableChainMerge switches; the
        // out1/in1 and sibling merge stages are both built in.
        bool enableCoarsening = true;
        bool collectStats = false;
        // DP fixed cost per segment boundary; legacy hard-codes +1 per segment.
        double dpSegmentPenalty = 1.0;
        // Merge host member instruction limit for the out1/in1/sibling merge
        // sweeps. 0 selects the default 256: larger values cut fewer
        // cross-block values but grow an oversized-block tail that re-executes
        // wholesale on activation (XiangShan: 7000 -> +6% host time, 256 ->
        // -3% host time vs the legacy partitioner; see compute-partition
        // NO0004).
        std::size_t dpCoarsenBudget = 0;
        // Post-DP local-move refinement rounds for the compute partition
        // (0 = off). Deterministic, monotone cost decrease; see
        // grhsim_am_compute_graph_partition.cpp.
        std::size_t dpRefinementRounds = 10;
    };

    struct AmOptimizeOptions
    {
        bool dce = true;
        bool constFold = true;
        bool cse = true;
        // Bypass single-operand Assign instructions (alias the result to the
        // operand). Assigns reading a state variable are kept by default:
        // commit-side instructions must observe the pre-commit value through
        // the snapshot (lowering preCommitValue). See stateReadAlias for the
        // refined rule.
        bool assignAlias = true;
        // Refined form of assignAlias: an Assign reading a state variable
        // may still be bypassed when no commit-side instruction (state
        // write, change detector, host effect; transitively through
        // single-operand Assign chains) references its result. Only the
        // preCommit snapshots that actually feed commit operands are kept.
        bool stateReadAlias = true;
        // Rewrite 1-bit LogicAnd/LogicOr/LogicNot to the bitwise And/Or/Not
        // (identical semantics on 1-bit operands) so CSE can merge the two
        // forms instead of keeping parallel guard-logic copies.
        bool logicUnify = true;
        // Absorb a Not/LogicNot that feeds only Mux selects (single-use or
        // shared): mux(!c, a, b) == mux(c, b, a) with the arms swapped, and
        // the Not instruction is removed. Restricted to 1-bit inverted
        // operands because the IR requires a 1-bit Mux select.
        bool muxNotAbsorb = true;
        // Fuse SliceStatic chains (slice(slice(x, l1), l2) -> slice(x,
        // l1+l2)) and bypass identity slices (lsb 0, identical type).
        bool sliceFuse = true;
        // Canonicalize 1-bit Not to Eq(x, 0): matches the reference flow's
        // negation form and lets CSE merge it with an explicit x == 0.
        // Measured on XiangShan (compute-partition NO0005): pure bucket
        // shift, no ge2/sum_outdeg gain (CSE merged 6 pairs) — default off.
        bool notUnify = false;
        // Fold MemoryRead instructions with a constant address on memories
        // that are never written. Emitted/interpreted storage is
        // zero-initialized, so Undef/Zero init reads as zero and Constant
        // init carries the packed lane literal; both are compile-time known.
        bool constMemFold = true;
        // Allow fold/CSE/alias to eliminate results that are referenced by
        // the ProgramInterface (output ports, declared observables): the
        // interface entries are re-pointed to the alias representative and
        // the visibility roles move with them. false restores the legacy
        // policy (any roled variable is untouchable).
        bool interfaceAlias = true;
    };

    struct GrhSimAmCppOptions
    {
        std::filesystem::path outputDirectory;
        std::string modelName;
        uint64_t maxOutputFileBytes = UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);
        std::map<std::string, std::string, std::less<>> attributes;
    };

    struct GrhSimAmCppResult
    {
        bool success = true;
        std::vector<std::string> artifacts;
    };

    // The pipeline currency is the AmGraph: lowering builds the graph
    // natively from GRH IR, graph passes rewrite it in place, and the linear
    // AM Program is materialized from the graph only at schedule finalize.
    class GrhIRToGrhSimAMGraphLoweringStage
    {
    public:
        virtual ~GrhIRToGrhSimAMGraphLoweringStage() = default;

        virtual std::optional<AmGraph>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics) = 0;
    };

    class GrhSimAmCppEmitStage
    {
    public:
        virtual ~GrhSimAmCppEmitStage() = default;

        virtual GrhSimAmCppResult
        emit(const ExecutableModel &model,
             const GrhSimAmCppOptions &options,
             wolvrix::lib::diag::Diagnostics &diagnostics) = 0;
    };

    struct GrhIRToGrhSimAMProgramResult
    {
        bool success = false;
        std::vector<std::string> artifacts;
        std::optional<ExecutableModel> model;
    };

    class GrhIRToGrhSimAMProgram
    {
    public:
        GrhIRToGrhSimAMProgram(GrhIRToGrhSimAMGraphLoweringStage &lowering,
                             GrhSimAmCppEmitStage &emitter);

        // Configures the AM optimization stage that runs between lowering and
        // scheduling. Defaults to all optimizations enabled.
        void setAmOptimizeOptions(AmOptimizeOptions options);

        // GRHSIM AM Graph -> GRHSIM AM Program：split-am-graph → opt-am-compute-graph →
        // partition-am-compute-graph → partition-am-commit-graph → materialize。
        static std::optional<ExecutableModel>
        graphToProgram(AmGraph &&graph,
                       const ActivityScheduleOptions &options,
                       wolvrix::lib::diag::Diagnostics &diagnostics);

        std::optional<AmGraph>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics);
        GrhIRToGrhSimAMProgramResult run(AmGraph &&graph,
                                       const ActivityScheduleOptions &scheduleOptions,
                                       const GrhSimAmCppOptions &emitOptions,
                                       wolvrix::lib::diag::Diagnostics &diagnostics);
        GrhIRToGrhSimAMProgramResult run(const wolvrix::lib::grh::Graph &graph,
                                       const ActivityScheduleOptions &scheduleOptions,
                                       const GrhSimAmCppOptions &emitOptions,
                                       wolvrix::lib::diag::Diagnostics &diagnostics);

    private:
        GrhIRToGrhSimAMGraphLoweringStage &lowering_;
        GrhSimAmCppEmitStage &emitter_;
        AmOptimizeOptions optimizeOptions_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_PROGRAM_HPP
