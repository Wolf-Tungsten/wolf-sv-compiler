#include "grhsim/am/activity_schedule.hpp"

#include "grhsim/am/builder.hpp"
#include "grhsim/am/opcode_traits.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        bool hasDependencyOrderedInstructions(ProgramView program,
                                              wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            std::vector<uint64_t> unresolvedDefinitions(
                program.variableCount() / 64 +
                    (program.variableCount() % 64 != 0 ? 1 : 0),
                0);
            const auto unresolved = [&](VariableId variable) {
                return variable.valid() && variable.value < program.variableCount() &&
                       (unresolvedDefinitions[variable.value / 64] &
                        (UINT64_C(1) << (variable.value % 64))) != 0;
            };
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                for (VariableId result : program.results(InstructionId{index}))
                {
                    if (!result.valid() || result.value >= program.variableCount())
                    {
                        diagnostics.error("baseline AM scheduler found an invalid result VariableId",
                                          "grhsim-am-activity-schedule");
                        return false;
                    }
                    unresolvedDefinitions[result.value / 64] |=
                        UINT64_C(1) << (result.value % 64);
                }
            }
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const InstructionId instruction{index};
                for (VariableId operand : program.operands(instruction))
                {
                    if (unresolved(operand))
                    {
                        diagnostics.error(
                            "baseline AM scheduler requires every Result producer to precede its uses: instruction=" +
                                std::to_string(index) + " variable=" +
                                std::to_string(operand.value),
                            "grhsim-am-activity-schedule");
                        return false;
                    }
                }
                for (VariableId result : program.results(instruction))
                {
                    unresolvedDefinitions[result.value / 64] &=
                        ~(UINT64_C(1) << (result.value % 64));
                }
            }
            return true;
        }
    } // namespace

    std::optional<ExecutableModel>
    BaselineActivityScheduleStage::schedule(LinearProgramArtifact &&linear,
                                            const ActivityScheduleOptions &options,
                                            wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        const ProgramView linearView = linear.program.view();
        if (!linearView.valid() ||
            linear.schedulingFacts.variableRoles.size() != linearView.variableCount() ||
            linear.schedulingFacts.instructionEffects.size() != linearView.instructionCount())
        {
            diagnostics.error("baseline AM scheduler received incomplete scheduling facts",
                              "grhsim-am-activity-schedule");
            return std::nullopt;
        }
        if (!hasDependencyOrderedInstructions(linearView, diagnostics))
        {
            return std::nullopt;
        }

        std::vector<VariableId> externalInputs;
        externalInputs.reserve(linear.interface.ports.size());
        for (const PortBinding &port : linear.interface.ports)
        {
            if (port.direction == PortDirection::Input || port.direction == PortDirection::Inout)
            {
                externalInputs.push_back(port.input);
            }
        }
        std::sort(externalInputs.begin(), externalInputs.end());
        externalInputs.erase(std::unique(externalInputs.begin(), externalInputs.end()),
                             externalInputs.end());
        std::vector<VariableId> stateVariables;
        const auto appendStateVariable = [&](VariableId variable,
                                             std::optional<std::size_t> instructionIndex) {
            const auto source = [&]() {
                return instructionIndex ? "instruction=" + std::to_string(*instructionIndex)
                                        : std::string("VariableRole::State");
            };
            if (!variable.valid() || variable.value >= linearView.variableCount())
            {
                diagnostics.error("baseline AM scheduler found an invalid state target: " +
                                      source(),
                                  "grhsim-am-activity-schedule");
                return false;
            }
            if (linearView.init(linearView.variable(variable).init).kind == InitKind::Constant)
            {
                diagnostics.error("baseline AM scheduler found a constant state target: variable=" +
                                      std::to_string(variable.value) + " source=" +
                                      source(),
                                  "grhsim-am-activity-schedule");
                return false;
            }
            stateVariables.push_back(variable);
            return true;
        };
        for (uint32_t index = 0; index < linearView.variableCount(); ++index)
        {
            const VariableRole role = linear.schedulingFacts.variableRoles[index];
            if (hasRole(role, VariableRole::State))
            {
                if (!appendStateVariable(VariableId{index}, std::nullopt))
                {
                    return std::nullopt;
                }
            }
        }
        for (std::size_t index = 0; index < linear.schedulingFacts.instructionEffects.size(); ++index)
        {
            if (linear.schedulingFacts.instructionEffects[index] == InstructionEffect::HostRead ||
                linear.schedulingFacts.instructionEffects[index] == InstructionEffect::HostEffect)
            {
                diagnostics.error("baseline AM scheduler cannot conservatively schedule a host interaction: instruction=" +
                                      std::to_string(index),
                                  "grhsim-am-activity-schedule");
                return std::nullopt;
            }

            const InstructionId instruction{static_cast<uint32_t>(index)};
            const OpcodeTraits traits = opcodeTraits(linearView.opcode(instruction));
            if (traits.effect != OpcodeEffect::StateReadWrite)
            {
                continue;
            }
            const auto operands = linearView.operands(instruction);
            if (traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand ||
                traits.stateTargetOperand >= operands.size())
            {
                diagnostics.error("baseline AM scheduler found a state read-write instruction without a valid target operand: instruction=" +
                                      std::to_string(index),
                                  "grhsim-am-activity-schedule");
                return std::nullopt;
            }
            if (!appendStateVariable(operands[traits.stateTargetOperand], index))
            {
                return std::nullopt;
            }
        }
        std::sort(stateVariables.begin(), stateVariables.end());
        stateVariables.erase(std::unique(stateVariables.begin(), stateVariables.end()),
                             stateVariables.end());
        const std::size_t stateVariableCount = stateVariables.size();
        uint32_t previousGroup = 0;
        uint32_t previousInstruction = 0;
        bool havePreviousEffect = false;
        for (const OrderedEffect &effect : linear.schedulingFacts.orderedEffects)
        {
            if (havePreviousEffect && effect.group == previousGroup &&
                effect.instruction.value <= previousInstruction)
            {
                diagnostics.error("baseline AM scheduler cannot preserve an ordered-effect group whose ordinal order disagrees with the linear instruction order",
                                  "grhsim-am-activity-schedule");
                return std::nullopt;
            }
            previousGroup = effect.group;
            previousInstruction = effect.instruction.value;
            havePreviousEffect = true;
        }
        linear.schedulingFacts.clearAndRelease();

        const std::size_t semanticInstructionCount = linearView.instructionCount();
        TypeId eventType;
        for (uint32_t index = 0; index < linearView.typeCount(); ++index)
        {
            const Type &type = linearView.type(TypeId{index});
            if (type.kind == TypeKind::BitVector && type.bitWidth == 1 &&
                type.signedness == Signedness::Unsigned)
            {
                eventType = TypeId{index};
                break;
            }
        }
        const bool needsNormalBlock = semanticInstructionCount != 0 || stateVariableCount != 0 ||
                                      !externalInputs.empty();
        const std::size_t watchedInputCount = needsNormalBlock ? externalInputs.size() : 0;
        const std::size_t watchCount = watchedInputCount + stateVariableCount;
        ProgramInterface interface = std::move(linear.interface);

        ScheduledProgramBuilder builder(std::move(linear.program));
        builder.reserve(ScheduledProgramReserve{
            .additionalTypes = eventType.valid() ? 0U : 1U,
            .additionalVariables = watchCount * 2,
            .additionalInstructions = watchCount * 2,
            .additionalOperands = watchCount * 3,
            .additionalResults = watchCount,
            .blocks = needsNormalBlock ? 2U : 1U,
            .blockInstructionIds = semanticInstructionCount + watchCount * 2,
            .activationInstructions = watchCount,
            .activationTargets = watchCount,
        });
        if (!eventType.valid())
        {
            eventType = builder.addType(Type::bitVector(1));
        }

        builder.beginBlock();
        if (needsNormalBlock)
        {
            const std::array<BlockId, 1> bodyTarget = {BlockId{1}};
            for (VariableId input : externalInputs)
            {
                const TypeId type = builder.view().variable(input).type;
                const VariableId oldValue = builder.addVariable(type, builder.undefInit());
                const VariableId event = builder.addVariable(eventType, builder.zeroInit());
                const std::array<VariableId, 1> changedResults = {event};
                const std::array<VariableId, 2> changedOperands = {input, oldValue};
                const InstructionId changed =
                    builder.addInstruction(Opcode::ChangedAny, changedResults, changedOperands);
                const std::array<VariableId, 1> actOperands = {event};
                const InstructionId activate =
                    builder.addInstruction(Opcode::ActForward, {}, actOperands);
                builder.setActivationTargets(activate, bodyTarget);
                builder.appendBlockInstruction(changed);
                builder.appendBlockInstruction(activate);
            }
        }
        builder.endBlock();
        std::vector<VariableId>().swap(externalInputs);

        if (needsNormalBlock)
        {
            const std::array<BlockId, 1> bodyTarget = {BlockId{1}};
            builder.beginBlock();
            for (uint32_t index = 0; index < semanticInstructionCount; ++index)
            {
                builder.appendBlockInstruction(InstructionId{index});
            }
            for (VariableId state : stateVariables)
            {
                const TypeId type = builder.view().variable(state).type;
                const VariableId oldValue = builder.addVariable(type, builder.undefInit());
                const VariableId event = builder.addVariable(eventType, builder.zeroInit());
                const std::array<VariableId, 1> changedResults = {event};
                const std::array<VariableId, 2> changedOperands = {state, oldValue};
                const InstructionId changed =
                    builder.addInstruction(Opcode::ChangedAny, changedResults, changedOperands);
                const std::array<VariableId, 1> actOperands = {event};
                const InstructionId activate =
                    builder.addInstruction(Opcode::ActBackward, {}, actOperands);
                builder.setActivationTargets(activate, bodyTarget);
                builder.appendBlockInstruction(changed);
                builder.appendBlockInstruction(activate);
            }
            builder.endBlock();
        }
        std::vector<VariableId>().swap(stateVariables);

        ExecutableModel model{
            .program = builder.finish(),
            .interface = std::move(interface),
        };
        if (options.collectStats)
        {
            const ProgramStorageStats stats = model.program.view().storageStats();
            diagnostics.info("baseline schedule stats: linear_instructions=" +
                                 std::to_string(semanticInstructionCount) +
                                 " scheduled_instructions=" + std::to_string(stats.instructions) +
                                 " scheduled_variables=" + std::to_string(stats.variables) +
                                 " external_input_watches=" + std::to_string(watchedInputCount) +
                                 " state_watches=" + std::to_string(stateVariableCount) +
                                 " block_instruction_ids=" +
                                 std::to_string(stats.blockInstructionIds) +
                                 " storage_bytes=" + std::to_string(stats.estimatedBytes) +
                                 " reserved_bytes=" + std::to_string(stats.reservedBytes),
                             "grhsim-am-activity-schedule");
        }
        return model;
    }

} // namespace wolvrix::lib::grhsim::am
