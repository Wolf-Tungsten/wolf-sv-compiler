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
        // When true, the segment DP charges each incoming variable by
        // ceil(bitWidth/64) copies (runtime copy count) instead of a unit
        // cost per variable. The reported scoreboard cost already uses the
        // width-folded formula either way.
        bool dpWidthWeightedCopyCost = false;
        // 0 = automatic (1.5 * maxInstructionsPerBlock). The legacy 32x factor
        // lets AM's single-instruction-atom coarsen converge into
        // DP-indivisible oversized clusters far above the segment cap; 1.5x
        // keeps the legacy block granularity (XiangShan ~33.7k vs 31.5k).
        std::size_t dpCoarsenBudget = 0;
    };

    struct AmOptimizeOptions
    {
        bool dce = true;
        bool constFold = true;
        bool cse = true;
        // Bypass single-operand Assign instructions (alias the result to the
        // operand). Assigns reading a state variable are commit read-old
        // snapshots (lowering preCommitValue) and are never bypassed.
        bool assignAlias = true;
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
