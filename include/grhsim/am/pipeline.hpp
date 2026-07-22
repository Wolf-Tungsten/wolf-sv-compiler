#ifndef WOLVRIX_GRHSIM_AM_PIPELINE_HPP
#define WOLVRIX_GRHSIM_AM_PIPELINE_HPP

#include "core/diagnostics.hpp"
#include "core/grh.hpp"
#include "grhsim/am/program.hpp"
#include "grhsim/am/validate.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    enum class PortDirection : uint8_t
    {
        Input = 0,
        Output = 1,
        Inout = 2,
    };

    struct PortBinding
    {
        StringId name;
        PortDirection direction = PortDirection::Input;
        VariableId input;
        VariableId output;
        VariableId outputEnable;
    };

    struct ProgramInterface
    {
        std::vector<PortBinding> ports;
        std::vector<VariableLabel> declaredVariables;
    };

    enum class VariableRole : uint8_t
    {
        None = 0,
        ExternalInput = 1U << 0U,
        ExternalOutput = 1U << 1U,
        State = 1U << 2U,
        Observable = 1U << 3U,
    };

    constexpr VariableRole operator|(VariableRole lhs, VariableRole rhs) noexcept
    {
        return static_cast<VariableRole>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    }

    constexpr bool hasRole(VariableRole value, VariableRole role) noexcept
    {
        return (static_cast<uint8_t>(value) & static_cast<uint8_t>(role)) != 0;
    }

    enum class InstructionEffect : uint8_t
    {
        Pure = 0,
        StateRead = 1,
        StateWrite = 2,
        StateReadWrite = 3,
        HostRead = 4,
        HostEffect = 5,
    };

    struct OrderedEffect
    {
        InstructionId instruction;
        uint32_t group = 0;
        uint32_t ordinal = 0;
    };

    // These arrays are indexed by dense AM IDs and are consumed by scheduling.
    // They are analysis input, not part of the executable Program contract.
    struct SchedulingFacts
    {
        std::vector<VariableRole> variableRoles;
        std::vector<InstructionEffect> instructionEffects;
        std::vector<OrderedEffect> orderedEffects;

        void clearAndRelease();
    };

    struct LinearProgramArtifact
    {
        LinearProgram program;
        ProgramInterface interface;
        SchedulingFacts schedulingFacts;
    };

    struct ExecutableModel
    {
        ScheduledProgram program;
        ProgramInterface interface;
    };

    ValidationResult validate(const ProgramInterface &interface,
                              ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const SchedulingFacts &facts,
                              ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const LinearProgramArtifact &artifact,
                              const ValidationOptions &options = {});
    ValidationResult validate(const ExecutableModel &model,
                              const ValidationOptions &options = {});

    struct ActivityScheduleOptions
    {
        std::size_t maxInstructionsPerBlock = 128;
        // Commit blocks may carry a longer ordered state-write chain than a
        // compute block, but never split an indivisible effect atom.
        std::size_t maxCommitInstructionsPerBlock = 4096;
        std::size_t maxStateWritesPerBlock = 4096;
        bool enableCoarsening = true;
        bool collectStats = false;
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

    class GrhToAmLoweringStage
    {
    public:
        virtual ~GrhToAmLoweringStage() = default;

        virtual std::optional<LinearProgramArtifact>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics) = 0;
    };

    class AmActivityScheduleStage
    {
    public:
        virtual ~AmActivityScheduleStage() = default;

        virtual std::optional<ExecutableModel>
        schedule(LinearProgramArtifact &&linear,
                 const ActivityScheduleOptions &options,
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

    struct GrhSimAmPipelineResult
    {
        bool success = false;
        std::vector<std::string> artifacts;
        std::optional<ExecutableModel> model;
    };

    class GrhSimAmPipeline
    {
    public:
        GrhSimAmPipeline(GrhToAmLoweringStage &lowering,
                         AmActivityScheduleStage &scheduler,
                         GrhSimAmCppEmitStage &emitter);

        std::optional<LinearProgramArtifact>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics);
        GrhSimAmPipelineResult run(LinearProgramArtifact &&linear,
                                   const ActivityScheduleOptions &scheduleOptions,
                                   const GrhSimAmCppOptions &emitOptions,
                                   wolvrix::lib::diag::Diagnostics &diagnostics);
        GrhSimAmPipelineResult run(const wolvrix::lib::grh::Graph &graph,
                                   const ActivityScheduleOptions &scheduleOptions,
                                   const GrhSimAmCppOptions &emitOptions,
                                   wolvrix::lib::diag::Diagnostics &diagnostics);

    private:
        GrhToAmLoweringStage &lowering_;
        AmActivityScheduleStage &scheduler_;
        GrhSimAmCppEmitStage &emitter_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_PIPELINE_HPP
