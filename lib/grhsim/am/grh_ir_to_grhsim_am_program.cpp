#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include "grhsim/am/grhsim_am_commit_graph_partition.hpp"
#include "grhsim/am/grhsim_am_compute_graph_optimize.hpp"
#include "grhsim/am/grhsim_am_compute_graph_partition.hpp"
#include "grhsim/am/grhsim_am_graph_optimize.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"
#include "grhsim/am/grhsim_am_graph_to_program.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        bool reportValidation(const ValidationResult &validation,
                              std::string_view stage,
                              wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            for (const std::string &error : validation.errors)
            {
                diagnostics.error(error, std::string(stage));
            }
            return validation.success();
        }

        void addError(ValidationResult &result,
                      const ValidationOptions &options,
                      std::string message)
        {
            const std::size_t limit = std::max<std::size_t>(options.maxErrors, 1);
            if (result.errors.size() < limit)
            {
                result.errors.push_back(std::move(message));
            }
        }

        void appendErrors(ValidationResult &result,
                          ValidationResult source,
                          const ValidationOptions &options)
        {
            for (std::string &error : source.errors)
            {
                if (result.errors.size() >= std::max<std::size_t>(options.maxErrors, 1))
                {
                    break;
                }
                result.errors.push_back(std::move(error));
            }
        }

        std::vector<uint32_t> interfaceInputVariables(const ProgramInterface &interface)
        {
            std::vector<uint32_t> variables;
            variables.reserve(interface.ports.size());
            for (const PortBinding &port : interface.ports)
            {
                if ((port.direction == PortDirection::Input || port.direction == PortDirection::Inout) &&
                    port.input.valid())
                {
                    variables.push_back(port.input.value);
                }
            }
            std::sort(variables.begin(), variables.end());
            variables.erase(std::unique(variables.begin(), variables.end()), variables.end());
            return variables;
        }

        std::vector<uint32_t> interfaceOutputVariables(const ProgramInterface &interface)
        {
            std::vector<uint32_t> variables;
            variables.reserve(interface.ports.size() * 2);
            for (const PortBinding &port : interface.ports)
            {
                if ((port.direction == PortDirection::Output ||
                     port.direction == PortDirection::Inout) &&
                    port.output.valid())
                {
                    variables.push_back(port.output.value);
                }
                if (port.direction == PortDirection::Inout && port.outputEnable.valid())
                {
                    variables.push_back(port.outputEnable.value);
                }
            }
            std::sort(variables.begin(), variables.end());
            variables.erase(std::unique(variables.begin(), variables.end()), variables.end());
            return variables;
        }

        bool isChangedOpcode(Opcode opcode) noexcept
        {
            return opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                   opcode == Opcode::ChangedNeg;
        }

        std::vector<uint64_t> changedPrivateVariableMask(ProgramView program)
        {
            std::vector<uint64_t> variables;
            if (!program.valid())
            {
                return variables;
            }
            variables.assign(program.variableCount() / 64 +
                                 (program.variableCount() % 64 != 0 ? 1 : 0),
                             0);
            const auto mark = [&](VariableId variable) {
                if (variable.valid() && variable.value < program.variableCount())
                {
                    variables[variable.value / 64] |= UINT64_C(1) << (variable.value % 64);
                }
            };
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const InstructionId instruction{index};
                if (!isChangedOpcode(program.opcode(instruction)))
                {
                    continue;
                }
                const auto results = program.results(instruction);
                const auto operands = program.operands(instruction);
                if (results.size() == 1)
                {
                    mark(results.front());
                }
                if (operands.size() == 2)
                {
                    mark(operands[1]);
                }
            }
            return variables;
        }

        bool containsVariable(const std::vector<uint64_t> &variables, VariableId variable)
        {
            return variable.valid() && variable.value / 64 < variables.size() &&
                   (variables[variable.value / 64] &
                    (UINT64_C(1) << (variable.value % 64))) != 0;
        }

        std::optional<VariableId> stateTarget(ProgramView program, InstructionId instruction)
        {
            const OpcodeTraits traits = opcodeTraits(program.opcode(instruction));
            if ((traits.effect != OpcodeEffect::StateRead &&
                 traits.effect != OpcodeEffect::StateReadWrite) ||
                traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand)
            {
                return std::nullopt;
            }
            const auto operands = program.operands(instruction);
            return traits.stateTargetOperand < operands.size()
                       ? std::optional<VariableId>(operands[traits.stateTargetOperand])
                       : std::nullopt;
        }

        bool writesInterfaceInput(ProgramView program,
                                  InstructionId instruction,
                                  const std::vector<uint32_t> &interfaceInputs)
        {
            const auto isInput = [&](VariableId variable) {
                return variable.valid() &&
                       std::binary_search(interfaceInputs.begin(), interfaceInputs.end(),
                                          variable.value);
            };
            const auto results = program.results(instruction);
            if (std::any_of(results.begin(), results.end(), isInput))
            {
                return true;
            }
            const OpcodeTraits traits = opcodeTraits(program.opcode(instruction));
            if (traits.effect != OpcodeEffect::StateReadWrite ||
                traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand)
            {
                return false;
            }
            const auto operands = program.operands(instruction);
            return traits.stateTargetOperand < operands.size() &&
                   isInput(operands[traits.stateTargetOperand]);
        }

        InstructionEffect expectedInstructionEffect(ProgramView program,
                                                     InstructionId instruction)
        {
            const Opcode opcode = program.opcode(instruction);
            if (opcode == Opcode::SystemFunction)
            {
                const auto attributes = program.systemFunctionAttributes(instruction);
                return attributes && attributes->hasSideEffects ? InstructionEffect::HostEffect
                                                                : InstructionEffect::HostRead;
            }
            switch (opcodeTraits(opcode).effect)
            {
            case OpcodeEffect::Pure:
                return InstructionEffect::Pure;
            case OpcodeEffect::ChangeDetector:
            case OpcodeEffect::StateReadWrite:
                return InstructionEffect::StateReadWrite;
            case OpcodeEffect::StateRead:
                return InstructionEffect::StateRead;
            case OpcodeEffect::HostRead:
                return InstructionEffect::HostRead;
            case OpcodeEffect::HostEffect:
                return InstructionEffect::HostEffect;
            case OpcodeEffect::Activation:
                return InstructionEffect::HostEffect;
            }
            return InstructionEffect::HostEffect;
        }

        const Type *variableType(ProgramView program, VariableId variable)
        {
            if (!variable.valid() || variable.value >= program.variableCount())
            {
                return nullptr;
            }
            const TypeId type = program.variable(variable).type;
            return type.valid() && type.value < program.typeCount() ? &program.type(type) : nullptr;
        }

        bool sameBitWidth(ProgramView program, VariableId lhs, VariableId rhs)
        {
            const Type *lhsType = variableType(program, lhs);
            const Type *rhsType = variableType(program, rhs);
            return lhsType && rhsType && lhsType->kind == TypeKind::BitVector &&
                   rhsType->kind == TypeKind::BitVector &&
                   lhsType->bitWidth == rhsType->bitWidth;
        }

        struct EntryDefinition
        {
            uint32_t variable = 0;
            std::size_t position = 0;
            InstructionId instruction;
        };

        struct EntryUse
        {
            VariableId variable;
            std::size_t beforePosition = 0;
        };

        std::vector<uint32_t>
        entryWatchedInputs(const ScheduledProgram &scheduled,
                           const std::vector<uint32_t> &interfaceInputs)
        {
            std::vector<EntryDefinition> definitions;
            std::vector<EntryUse> worklist;
            const ProgramView program = scheduled.view();
            const BlockId entry{0};
            for (std::size_t position = 0; position < scheduled.blockSize(entry); ++position)
            {
                const InstructionId instruction = scheduled.blockInstruction(entry, position);
                if (!instruction.valid() || instruction.value >= program.instructionCount())
                {
                    continue;
                }
                for (VariableId result : program.results(instruction))
                {
                    if (result.valid() && result.value < program.variableCount())
                    {
                        definitions.push_back(EntryDefinition{
                            .variable = result.value,
                            .position = position,
                            .instruction = instruction,
                        });
                    }
                }
                if (program.opcode(instruction) == Opcode::ActForward)
                {
                    const auto operands = program.operands(instruction);
                    if (operands.size() == 1)
                    {
                        worklist.push_back(EntryUse{
                            .variable = operands.front(),
                            .beforePosition = position,
                        });
                    }
                }
            }

            std::sort(definitions.begin(), definitions.end(),
                      [](const EntryDefinition &lhs, const EntryDefinition &rhs) {
                          return lhs.variable < rhs.variable ||
                                 (lhs.variable == rhs.variable && lhs.position < rhs.position);
                      });
            const auto latestDefinition = [&](const EntryUse &use) -> std::optional<std::size_t> {
                if (!use.variable.valid())
                {
                    return std::nullopt;
                }
                const auto it = std::lower_bound(
                    definitions.begin(), definitions.end(), use,
                    [](const EntryDefinition &definition, const EntryUse &key) {
                        return definition.variable < key.variable.value ||
                               (definition.variable == key.variable.value &&
                                definition.position < key.beforePosition);
                    });
                if (it == definitions.begin())
                {
                    return std::nullopt;
                }
                const auto previous = std::prev(it);
                if (previous->variable != use.variable.value)
                {
                    return std::nullopt;
                }
                return static_cast<std::size_t>(previous - definitions.begin());
            };

            std::vector<uint8_t> visited(definitions.size(), 0);
            std::vector<uint32_t> watchedInputs;
            while (!worklist.empty())
            {
                const EntryUse use = worklist.back();
                worklist.pop_back();
                const std::optional<std::size_t> definitionIndex = latestDefinition(use);
                if (!definitionIndex || visited[*definitionIndex])
                {
                    continue;
                }
                visited[*definitionIndex] = 1;
                const EntryDefinition &definition = definitions[*definitionIndex];
                const Opcode opcode = program.opcode(definition.instruction);
                const auto results = program.results(definition.instruction);
                const auto operands = program.operands(definition.instruction);
                if (results.size() != 1 || results.front().value != definition.variable)
                {
                    continue;
                }

                if (opcode == Opcode::ChangedAny)
                {
                    if (operands.size() == 2 && operands.front().valid() &&
                        std::binary_search(interfaceInputs.begin(), interfaceInputs.end(),
                                           operands.front().value) &&
                        !latestDefinition(EntryUse{
                            .variable = operands.front(),
                            .beforePosition = definition.position,
                        }))
                    {
                        watchedInputs.push_back(operands.front().value);
                    }
                    continue;
                }

                bool preservesTruth = false;
                if (opcode == Opcode::Assign && operands.size() == 1)
                {
                    preservesTruth = sameBitWidth(program, results.front(), operands.front());
                }
                else if ((opcode == Opcode::Or || opcode == Opcode::LogicOr) &&
                         operands.size() == 2)
                {
                    preservesTruth = true;
                }
                else if (opcode == Opcode::Concat && !operands.empty())
                {
                    preservesTruth = true;
                }
                else if ((opcode == Opcode::Replicate || opcode == Opcode::ReduceOr) &&
                         operands.size() == 1)
                {
                    preservesTruth = true;
                }
                if (!preservesTruth)
                {
                    continue;
                }
                for (VariableId operand : operands)
                {
                    worklist.push_back(EntryUse{
                        .variable = operand,
                        .beforePosition = definition.position,
                    });
                }
            }

            std::sort(watchedInputs.begin(), watchedInputs.end());
            watchedInputs.erase(std::unique(watchedInputs.begin(), watchedInputs.end()),
                                watchedInputs.end());
            return watchedInputs;
        }
    } // namespace

    bool gsimNodeAlignedScheduling(const AmGraph &graph,
                                   const ActivityScheduleOptions &options)
    {
        if (const char *env = std::getenv("WOLVRIX_GRHSIM_AM_NODE_ALIGNED"))
        {
            const std::string_view value(env);
            if (value == "0")
            {
                return false;
            }
            if (value == "1")
            {
                return true;
            }
        }
        switch (options.gsimNodeAligned)
        {
        case GsimNodeAlignedMode::On:
            return true;
        case GsimNodeAlignedMode::Off:
            return false;
        case GsimNodeAlignedMode::Auto:
            break;
        }
        return graph.hasGsimNodeProvenance();
    }

    bool gsimNodeAlignedOptimizeForced()
    {
        const char *env = std::getenv("WOLVRIX_GRHSIM_AM_NODE_ALIGNED_OPTIMIZE");
        return env != nullptr && std::string_view(env) == "1";
    }

    void SchedulingFacts::clearAndRelease()
    {
        std::vector<VariableRole>().swap(variableRoles);
        std::vector<InstructionEffect>().swap(instructionEffects);
        std::vector<OrderedEffect>().swap(orderedEffects);
    }

    ValidationResult validate(const ProgramInterface &interface,
                              ProgramView program,
                              const ValidationOptions &options)
    {
        ValidationResult result;
        if (!program.valid())
        {
            addError(result, options, "ProgramInterface is paired with an empty AM program");
            return result;
        }

        auto validVariable = [&](VariableId variable) {
            return variable.valid() && variable.value < program.variableCount();
        };
        auto validName = [&](StringId name) {
            return name.valid() && name.value < program.stringCount() && !program.string(name).empty();
        };
        auto externallyWritable = [&](VariableId variable) {
            return validVariable(variable) &&
                   program.init(program.variable(variable).init).kind != InitKind::Constant;
        };
        const std::vector<uint64_t> privateChanged =
            interface.ports.empty() && interface.declaredVariables.empty()
                ? std::vector<uint64_t>{}
                : changedPrivateVariableMask(program);

        std::vector<std::string_view> portNames;
        portNames.reserve(interface.ports.size());
        for (std::size_t index = 0; index < interface.ports.size(); ++index)
        {
            const PortBinding &port = interface.ports[index];
            bool valid = validName(port.name);
            const bool exposesChanged = containsVariable(privateChanged, port.input) ||
                                        containsVariable(privateChanged, port.output) ||
                                        containsVariable(privateChanged, port.outputEnable);
            if (exposesChanged)
            {
                addError(result,
                         options,
                         "AM ProgramInterface exposes a private changed old/result variable: index=" +
                             std::to_string(index));
            }
            if (valid)
            {
                portNames.push_back(program.string(port.name));
            }
            switch (port.direction)
            {
            case PortDirection::Input:
                valid = valid && externallyWritable(port.input) && !port.output.valid() &&
                        !port.outputEnable.valid();
                break;
            case PortDirection::Output:
                valid = valid && validVariable(port.output) && !port.input.valid() &&
                        !port.outputEnable.valid();
                break;
            case PortDirection::Inout:
                valid = valid && externallyWritable(port.input) && validVariable(port.output) &&
                        validVariable(port.outputEnable);
                if (valid)
                {
                    const Type *inputType = variableType(program, port.input);
                    const Type *outputType = variableType(program, port.output);
                    const Type *enableType = variableType(program, port.outputEnable);
                    valid = inputType && outputType && *inputType == *outputType && enableType &&
                            enableType->kind == TypeKind::BitVector && enableType->bitWidth == 1;
                }
                break;
            default:
                valid = false;
                break;
            }
            if (!valid)
            {
                addError(result,
                         options,
                         "invalid or duplicate AM ProgramInterface port binding: index=" +
                             std::to_string(index));
            }
        }
        std::sort(portNames.begin(), portNames.end());
        if (std::adjacent_find(portNames.begin(), portNames.end()) != portNames.end())
        {
            addError(result, options, "AM ProgramInterface port names must be unique");
        }

        for (const VariableLabel &declared : interface.declaredVariables)
        {
            if (!validVariable(declared.variable) || !validName(declared.label))
            {
                addError(result, options, "invalid AM declared-variable binding");
            }
            if (containsVariable(privateChanged, declared.variable))
            {
                addError(result,
                         options,
                         "AM declared-variable binding exposes a private changed old/result variable");
            }
        }
        return result;
    }

    ValidationResult validate(const SchedulingFacts &facts,
                              ProgramView program,
                              const ValidationOptions &options)
    {
        ValidationResult result;
        if (!program.valid())
        {
            addError(result, options, "SchedulingFacts are paired with an empty AM program");
            return result;
        }
        if (facts.variableRoles.size() != program.variableCount())
        {
            addError(result,
                     options,
                     "AM scheduling facts must contain one variable role per VarId");
        }
        if (facts.instructionEffects.size() != program.instructionCount())
        {
            addError(result,
                     options,
                     "AM scheduling facts must contain one effect class per InstructionId");
        }
        for (std::size_t index = 0; index < facts.variableRoles.size(); ++index)
        {
            if ((static_cast<uint8_t>(facts.variableRoles[index]) & UINT8_C(0xf0)) != 0)
            {
                addError(result,
                         options,
                         "AM scheduling fact has unknown variable-role bits: variable=" +
                             std::to_string(index));
            }
        }
        for (std::size_t index = 0; index < facts.instructionEffects.size(); ++index)
        {
            if (facts.instructionEffects[index] > InstructionEffect::HostEffect)
            {
                addError(result,
                         options,
                         "AM scheduling fact has an invalid instruction effect: instruction=" +
                             std::to_string(index));
            }
            else if (index < program.instructionCount() &&
                     facts.instructionEffects[index] !=
                         expectedInstructionEffect(program, InstructionId{static_cast<uint32_t>(index)}))
            {
                addError(result,
                         options,
                         "AM scheduling effect does not match opcode traits: instruction=" +
                             std::to_string(index));
            }
        }

        uint32_t previousGroup = 0;
        uint32_t previousOrdinal = 0;
        bool havePrevious = false;
        for (const OrderedEffect &effect : facts.orderedEffects)
        {
            if (!effect.instruction.valid() || effect.instruction.value >= program.instructionCount())
            {
                addError(result,
                         options,
                         "AM ordered-effect fact refers to an invalid InstructionId");
                continue;
            }
            if (havePrevious &&
                (effect.group < previousGroup ||
                 (effect.group == previousGroup && effect.ordinal <= previousOrdinal)))
            {
                addError(result,
                         options,
                         "AM ordered-effect facts must be sorted and unique by group and ordinal");
            }
            previousGroup = effect.group;
            previousOrdinal = effect.ordinal;
            havePrevious = true;
        }
        return result;
    }

    ValidationResult validate(const LinearProgramArtifact &artifact,
                              const ValidationOptions &options)
    {
        ValidationResult result = validate(artifact.program, options);
        appendErrors(result, validate(artifact.interface, artifact.program.view(), options), options);
        appendErrors(result, validate(artifact.schedulingFacts, artifact.program.view(), options), options);
        const ProgramView program = artifact.program.view();
        if (program.valid())
        {
            const std::vector<uint32_t> interfaceInputs = interfaceInputVariables(artifact.interface);
            if (!interfaceInputs.empty())
            {
                for (uint32_t index = 0; index < program.instructionCount(); ++index)
                {
                    if (writesInterfaceInput(program, InstructionId{index}, interfaceInputs))
                    {
                        addError(result,
                                 options,
                                 "AM instruction writes a ProgramInterface input: instruction=" +
                                     std::to_string(index));
                    }
                }
            }
            if (artifact.schedulingFacts.variableRoles.size() != program.variableCount())
            {
                return result;
            }
            const std::vector<uint32_t> interfaceOutputs = interfaceOutputVariables(artifact.interface);
            std::vector<uint32_t> roleInputs;
            std::vector<uint32_t> roleOutputs;
            roleInputs.reserve(interfaceInputs.size());
            roleOutputs.reserve(interfaceOutputs.size());
            for (uint32_t index = 0; index < artifact.schedulingFacts.variableRoles.size(); ++index)
            {
                if (hasRole(artifact.schedulingFacts.variableRoles[index], VariableRole::ExternalInput))
                {
                    roleInputs.push_back(index);
                }
                if (hasRole(artifact.schedulingFacts.variableRoles[index], VariableRole::ExternalOutput))
                {
                    roleOutputs.push_back(index);
                }
            }
            if (roleInputs != interfaceInputs)
            {
                addError(result,
                         options,
                         "AM SchedulingFacts external-input roles do not match ProgramInterface");
            }
            if (roleOutputs != interfaceOutputs)
            {
                addError(result,
                         options,
                         "AM SchedulingFacts external-output roles do not match ProgramInterface");
            }
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const std::optional<VariableId> target = stateTarget(program, InstructionId{index});
                if (target && target->valid() && target->value < program.variableCount() &&
                    !hasRole(artifact.schedulingFacts.variableRoles[target->value],
                             VariableRole::State))
                {
                    addError(result,
                             options,
                             "AM state/memory target is missing the State variable role: instruction=" +
                                 std::to_string(index));
                }
            }
        }
        return result;
    }

    ValidationResult validate(const AmGraph &graph,
                              const ValidationOptions &options)
    {
        const ProgramView program = graph.program();
        SchedulingFacts facts;
        facts.variableRoles = graph.variableRoles();
        facts.instructionEffects = graph.instructionEffects();
        facts.orderedEffects = graph.orderedEffects();

        ValidationResult result = validate(program, options);
        appendErrors(result, validate(graph.interface(), program, options), options);
        appendErrors(result, validate(facts, program, options), options);
        if (program.valid())
        {
            const std::vector<uint32_t> interfaceInputs = interfaceInputVariables(graph.interface());
            if (!interfaceInputs.empty())
            {
                for (uint32_t index = 0; index < program.instructionCount(); ++index)
                {
                    if (writesInterfaceInput(program, InstructionId{index}, interfaceInputs))
                    {
                        addError(result,
                                 options,
                                 "AM instruction writes a ProgramInterface input: instruction=" +
                                     std::to_string(index));
                    }
                }
            }
            if (facts.variableRoles.size() != program.variableCount())
            {
                return result;
            }
            const std::vector<uint32_t> interfaceOutputs = interfaceOutputVariables(graph.interface());
            std::vector<uint32_t> roleInputs;
            std::vector<uint32_t> roleOutputs;
            roleInputs.reserve(interfaceInputs.size());
            roleOutputs.reserve(interfaceOutputs.size());
            for (uint32_t index = 0; index < facts.variableRoles.size(); ++index)
            {
                if (hasRole(facts.variableRoles[index], VariableRole::ExternalInput))
                {
                    roleInputs.push_back(index);
                }
                if (hasRole(facts.variableRoles[index], VariableRole::ExternalOutput))
                {
                    roleOutputs.push_back(index);
                }
            }
            if (roleInputs != interfaceInputs)
            {
                addError(result,
                         options,
                         "AM SchedulingFacts external-input roles do not match ProgramInterface");
            }
            if (roleOutputs != interfaceOutputs)
            {
                addError(result,
                         options,
                         "AM SchedulingFacts external-output roles do not match ProgramInterface");
            }
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const std::optional<VariableId> target = stateTarget(program, InstructionId{index});
                if (target && target->valid() && target->value < program.variableCount() &&
                    !hasRole(facts.variableRoles[target->value], VariableRole::State))
                {
                    addError(result,
                             options,
                             "AM state/memory target is missing the State variable role: instruction=" +
                                 std::to_string(index));
                }
            }
        }
        return result;
    }

    ValidationResult validate(const ExecutableModel &model,
                              const ValidationOptions &options)
    {
        ValidationResult result = validate(model.program, options);
        appendErrors(result, validate(model.interface, model.program.view(), options), options);
        if (!model.program.valid() || model.program.blockCount() == 0)
        {
            return result;
        }

        const ProgramView program = model.program.view();
        const std::size_t blockCount = model.program.blockCount();

        // Commit Blocks, when present, form one contiguous suffix of the Block
        // space; without them every Block is a compute Block.
        std::size_t commitBegin = blockCount;
        bool validCommitRange = true;
        if (model.commitBlockBegin == 0)
        {
            validCommitRange = model.commitBlockEnd == 0;
        }
        else if (model.commitBlockBegin < model.commitBlockEnd &&
                 model.commitBlockEnd == blockCount)
        {
            commitBegin = model.commitBlockBegin;
        }
        else
        {
            validCommitRange = false;
        }
        if (!validCommitRange)
        {
            addError(result, options,
                     "AM ExecutableModel commit Blocks must form a contiguous suffix of the Block space");
        }

        if (validCommitRange)
        {
            const auto isStateWrite = [](Opcode opcode) {
                return isStateWriteOpcode(opcode);
            };
            std::vector<uint32_t> changedBlocks(program.variableCount(), UINT32_MAX);
            for (uint32_t block = 0; block < blockCount; ++block)
            {
                const BlockId blockId{block};
                const bool isCommitBlock = block >= commitBegin;
                bool seenGateDetector = false;
                bool seenStateWrite = false;
                for (std::size_t position = 0;
                     position < model.program.blockSize(blockId); ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(blockId, position);
                    if (!instruction.valid() || instruction.value >= program.instructionCount())
                    {
                        continue;
                    }
                    const Opcode opcode = program.opcode(instruction);
                    if (isStateWrite(opcode))
                    {
                        if (!isCommitBlock)
                        {
                            addError(result, options,
                                     "AM state write instruction outside a commit Block: instruction=" +
                                         std::to_string(instruction.value) +
                                         " block=" + std::to_string(block));
                        }
                        seenStateWrite = true;
                        if (isCommitBlock && !seenGateDetector)
                        {
                            addError(result, options,
                                     "AM commit Block state write is not preceded by a gate detector: instruction=" +
                                         std::to_string(instruction.value) +
                                         " block=" + std::to_string(block));
                        }
                    }
                    if (isCommitBlock && isChangedOpcode(opcode) && !seenStateWrite)
                    {
                        seenGateDetector = true;
                    }
                    if (opcode == Opcode::ActForward)
                    {
                        if (const auto attributes = program.activationAttributes(instruction))
                        {
                            for (const BlockId target : attributes->targets)
                            {
                                if (!target.valid() || target.value <= block ||
                                    target.value >= blockCount)
                                {
                                    addError(result, options,
                                             "AM ActForward target is not a later Block: instruction=" +
                                                 std::to_string(instruction.value) +
                                                 " target=" + std::to_string(target.value));
                                }
                            }
                        }
                    }
                    if (opcode == Opcode::ActBackward)
                    {
                        if (const auto attributes = program.activationAttributes(instruction))
                        {
                            for (const BlockId target : attributes->targets)
                            {
                                if (!target.valid() || target.value < 1 ||
                                    target.value >= blockCount)
                                {
                                    addError(result, options,
                                             "AM ActBackward target is not a non-entry Block: instruction=" +
                                                 std::to_string(instruction.value) +
                                                 " target=" + std::to_string(target.value));
                                }
                            }
                        }
                    }
                    if (isChangedOpcode(opcode))
                    {
                        const auto results = program.results(instruction);
                        if (results.size() == 1 && results.front().valid() &&
                            results.front().value < changedBlocks.size())
                        {
                            changedBlocks[results.front().value] = block;
                        }
                    }
                }
            }

            // Atom-kind placement: CommitEvent atoms only inside the commit
            // Block range; Tree/CombLoopScc only outside it.
            for (uint32_t block = 0; block < blockCount; ++block)
            {
                const BlockId blockId{block};
                const bool isCommitBlock = commitBegin != blockCount && block >= commitBegin;
                for (std::size_t atomIndex = 0;
                     atomIndex < model.program.blockAtomCount(blockId); ++atomIndex)
                {
                    const AtomId atom = model.program.blockAtom(blockId, atomIndex);
                    const AmAtomKind kind = model.program.atomKind(atom);
                    if (kind == AmAtomKind::CommitEvent && !isCommitBlock)
                    {
                        addError(result, options,
                                 "AM CommitEvent atom is outside the commit Block range: atom=" +
                                     std::to_string(atom.value) +
                                     " block=" + std::to_string(block));
                    }
                    if ((kind == AmAtomKind::Tree || kind == AmAtomKind::CombLoopScc) &&
                        isCommitBlock)
                    {
                        addError(result, options,
                                 "AM compute atom kind is inside the commit Block range: atom=" +
                                     std::to_string(atom.value) +
                                     " block=" + std::to_string(block));
                    }
                }
            }

            // A Changed result consumed across Blocks must flow strictly forward.
            for (uint32_t block = 0; block < blockCount; ++block)
            {
                const BlockId blockId{block};
                for (std::size_t position = 0;
                     position < model.program.blockSize(blockId); ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(blockId, position);
                    if (!instruction.valid() || instruction.value >= program.instructionCount())
                    {
                        continue;
                    }
                    for (const VariableId operand : program.operands(instruction))
                    {
                        if (!operand.valid() || operand.value >= changedBlocks.size())
                        {
                            continue;
                        }
                        const uint32_t producerBlock = changedBlocks[operand.value];
                        if (producerBlock == UINT32_MAX || producerBlock <= block)
                        {
                            continue;
                        }
                        addError(result, options,
                                 "AM Changed result is consumed by an earlier Block: variable=" +
                                     std::to_string(operand.value) +
                                     " producerBlock=" + std::to_string(producerBlock) +
                                     " consumerBlock=" + std::to_string(block));
                    }
                }
            }
        }

        const std::vector<uint32_t> interfaceInputs = interfaceInputVariables(model.interface);
        if (interfaceInputs.empty())
        {
            return result;
        }
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            if (writesInterfaceInput(program, InstructionId{index}, interfaceInputs))
            {
                addError(result,
                         options,
                         "AM instruction writes a ProgramInterface input: instruction=" +
                             std::to_string(index));
            }
        }
        const std::vector<uint32_t> watchedInputs =
            entryWatchedInputs(model.program, interfaceInputs);
        // Commit Blocks are activation-driven, but only some input reads need
        // an EntryBlock watch: reads inside compute Blocks (they recompute)
        // and the watched operand of commit-Block changed.* gate detectors
        // (that is exactly the activation path, e.g. the clock). Write value
        // operands in commit Blocks (addr/data/laneMask/...) are read live
        // when the Block's own event fires and need no activation.
        std::vector<uint32_t> activationInputs;
        for (uint32_t block = 0; block < blockCount; ++block)
        {
            const BlockId blockId{block};
            const bool isCommitBlock = block >= commitBegin;
            for (std::size_t position = 0;
                 position < model.program.blockSize(blockId); ++position)
            {
                const InstructionId instruction =
                    model.program.blockInstruction(blockId, position);
                if (!instruction.valid() || instruction.value >= program.instructionCount())
                {
                    continue;
                }
                const Opcode opcode = program.opcode(instruction);
                const auto operands = program.operands(instruction);
                for (std::size_t operandPosition = 0;
                     operandPosition < operands.size(); ++operandPosition)
                {
                    const VariableId operand = operands[operandPosition];
                    const bool needsWatch =
                        !isCommitBlock ||
                        (isChangedOpcode(opcode) && operandPosition == 0);
                    if (needsWatch && operand.valid() &&
                        std::binary_search(interfaceInputs.begin(), interfaceInputs.end(),
                                           operand.value))
                    {
                        activationInputs.push_back(operand.value);
                    }
                }
            }
        }
        std::sort(activationInputs.begin(), activationInputs.end());
        activationInputs.erase(
            std::unique(activationInputs.begin(), activationInputs.end()),
            activationInputs.end());
        if (!std::includes(watchedInputs.begin(), watchedInputs.end(),
                           activationInputs.begin(), activationInputs.end()))
        {
            addError(result,
                     options,
                     "ScheduledProgram EntryBlock does not activate from every ProgramInterface input");
        }
        return result;
    }

    GrhIRToGrhSimAMProgram::GrhIRToGrhSimAMProgram(GrhIRToGrhSimAMGraphLoweringStage &lowering,
                                       GrhSimAmCppEmitStage &emitter)
        : lowering_(lowering), emitter_(emitter)
    {
    }

    void GrhIRToGrhSimAMProgram::setAmOptimizeOptions(AmOptimizeOptions options)
    {
        optimizeOptions_ = options;
    }

    std::optional<AmGraph>
    GrhIRToGrhSimAMProgram::lower(const wolvrix::lib::grh::Graph &graph,
                            wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        if (diagnostics.hasError())
        {
            return std::nullopt;
        }
        std::optional<AmGraph> lowered = lowering_.lower(graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            return std::nullopt;
        }
        return lowered;
    }

    std::optional<ExecutableModel>
    GrhIRToGrhSimAMProgram::graphToProgram(AmGraph &&graph,
                                   const ActivityScheduleOptions &options,
                                   wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        if (diagnostics.hasError()) {
            return std::nullopt;
        }
        if (options.maxAtomsPerBlock == 0 || options.maxCommitAtomsPerBlock == 0) {
            diagnostics.error("AM activity scheduling limits must be non-zero",
                              std::string(detail::kDiagnosticContext));
            return std::nullopt;
        }
        if (!detail::reportValidation(
                validate(graph, ValidationOptions{.level = ValidationLevel::Semantic}),
                diagnostics)) {
            return std::nullopt;
        }

        // ---- stage: split-am-graph --------------------------------------
        std::optional<AmGraphSplitContext> context =
            splitAmGraphStage(graph, options, diagnostics);
        if (!context) {
            return std::nullopt;
        }

        // ---- stage: opt-am-compute-graph --------------------------------
        // tree-atom formation (NO0008): single-use pure producers fold into
        // their unique consumer's atom, so every compute atom is a
        // single-output expression tree (mux-rooted trees are the when-tree
        // analogue). Rewrites the split context's atom tables and rebuilds
        // the induced subgraphs on the folded atom DAG. Runs before
        // partitionInput() assembly because the partition input spans
        // address the context storage.
        // Debug escape hatch (NO0017): WOLVRIX_GRHSIM_AM_DISABLE_TREE_ATOM_FOLD
        // skips the fold entirely for import-compat bisection.
        // NO0006: node-aligned scheduling builds final atoms directly from
        // the gsim node grouping in split-am-graph, so the fold (and the
        // fanout absorb below) are skipped.
        const bool nodeAligned = gsimNodeAlignedScheduling(graph, options);
        const bool disableTreeAtomFold =
            nodeAligned || std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_TREE_ATOM_FOLD") != nullptr;
        // NO0002 L2 alignment knob: options.treeAtomFoldMaxInstr caps the
        // fold-set size (connected sub-tree split); the
        // WOLVRIX_GRHSIM_AM_TREE_ATOM_FOLD_MAX_INSTR env var applies when the
        // option is 0.
        std::size_t treeAtomFoldMaxInstr = options.treeAtomFoldMaxInstr;
        if (treeAtomFoldMaxInstr == 0) {
            if (const char *text = std::getenv("WOLVRIX_GRHSIM_AM_TREE_ATOM_FOLD_MAX_INSTR"))
            {
                const std::string_view view(text);
                const auto [end, convError] =
                    std::from_chars(view.data(), view.data() + view.size(),
                                    treeAtomFoldMaxInstr);
                if (convError != std::errc{} || end != view.data() + view.size())
                {
                    diagnostics.error("WOLVRIX_GRHSIM_AM_TREE_ATOM_FOLD_MAX_INSTR must be a "
                                      "non-negative integer",
                                      std::string(detail::kDiagnosticContext));
                    return std::nullopt;
                }
            }
        }
        if (!disableTreeAtomFold &&
            !foldSingleOutputTreeAtoms(graph, *context, diagnostics,
                                       treeAtomFoldMaxInstr)) {
            return std::nullopt;
        }

        // Fanout absorption (NO0015): replicate small fanout>=2 compute
        // atoms into every consumer atom up to the read-port boundary, so
        // the partition graph sheds the shared-fanout structure gsim never
        // had. Runs after the tree-atom fold (consumers are atoms by then)
        // and rebuilds the same tables. Off under node-aligned scheduling
        // (NO0006): the node-grouped atoms are already final.
        if (!nodeAligned && options.fanoutAbsorbMaxInstructions > 0 &&
            !absorbFanoutAtoms(graph, *context,
                               options.fanoutAbsorbMaxInstructions,
                               options.fanoutAbsorbBudgetMult,
                               options.fanoutAbsorbMaxConsumers, diagnostics)) {
            return std::nullopt;
        }

        // Research export of the instruction graph: after the tree-atom fold
        // so the atom fields are the post-fold split-context values.
        if (const char *exportPath = std::getenv("WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL")) {
            if (exportPath[0] == '\0' ||
                !exportInstructionGraphJsonl(graph.program(), *context,
                                             std::filesystem::path(exportPath), diagnostics)) {
                diagnostics.error("AM instruction graph export failed",
                                  std::string(detail::kDiagnosticContext));
                return std::nullopt;
            }
        }

        const AmGraphPartitionInput blockInput = context->partitionInput();

        // Reserved compute-graph optimization stage boundary (currently a no-op).
        optAmComputeGraph(context->split.computeGraph, blockInput);

        // ---- stage: partition-am-compute-graph --------------------------
        // Activity-driven partitioning of the compute graph, producing the
        // GRHSIM AM Compute Activity Graph.
        std::string blockError;
        const auto computeActivity =
            partitionAmComputeGraph(blockInput, context->split, blockError);
        if (!computeActivity) {
            diagnostics.error(std::move(blockError), std::string(detail::kDiagnosticContext));
            return std::nullopt;
        }

        // ---- stage: partition-am-commit-graph ---------------------------
        // Event-clustering partitioning of the commit graph, producing the
        // GRHSIM AM Commit Event Graph.
        const auto commitEvent = partitionAmCommitGraph(blockInput, context->split, blockError);
        if (!commitEvent) {
            diagnostics.error(std::move(blockError), std::string(detail::kDiagnosticContext));
            return std::nullopt;
        }

        // ---- stage: materialize -----------------------------------------
        return materializeAmProgram(graph, *context, *computeActivity, *commitEvent, options,
                                    diagnostics);
    }

    GrhIRToGrhSimAMProgramResult
    GrhIRToGrhSimAMProgram::run(AmGraph &&graph,
                          const ActivityScheduleOptions &scheduleOptions,
                          const GrhSimAmCppOptions &emitOptions,
                          wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        GrhIRToGrhSimAMProgramResult result;
        if (diagnostics.hasError())
        {
            return result;
        }
        if (!reportValidation(validate(graph,
                                       ValidationOptions{.level = ValidationLevel::Semantic}),
                              "grhsim-am-lower",
                              diagnostics))
        {
            return result;
        }

        // NO0006: with gsim node-aligned scheduling the gsim flatten graph
        // is already optimized, and the AM opt passes (assignAlias/cse/fold)
        // would erase node anchors and break the 1:1 node mapping — skip
        // them unless the escape hatch forces a run for A/B.
        const bool nodeAligned = gsimNodeAlignedScheduling(graph, scheduleOptions);
        if (nodeAligned && !gsimNodeAlignedOptimizeForced())
        {
            diagnostics.info("am.optimize skipped: gsim node-aligned scheduling is active "
                             "(set WOLVRIX_GRHSIM_AM_NODE_ALIGNED_OPTIMIZE=1 to force)",
                             "grhsim-am-lower");
        }
        else if (!optimizeAmGraph(graph, optimizeOptions_, diagnostics) ||
                 diagnostics.hasError())
        {
            return result;
        }

        std::optional<ExecutableModel> model =
            graphToProgram(std::move(graph), scheduleOptions, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return result;
        }
        if (!reportValidation(validate(*model,
                                       ValidationOptions{.level = ValidationLevel::Semantic}),
                              "grhsim-am-activity-schedule",
                              diagnostics))
        {
            return result;
        }

        result.model.emplace(std::move(*model));
        GrhSimAmCppResult emitResult = emitter_.emit(*result.model, emitOptions, diagnostics);
        result.artifacts = std::move(emitResult.artifacts);
        result.success = emitResult.success && !diagnostics.hasError();
        return result;
    }

    GrhIRToGrhSimAMProgramResult
    GrhIRToGrhSimAMProgram::run(const wolvrix::lib::grh::Graph &graph,
                          const ActivityScheduleOptions &scheduleOptions,
                          const GrhSimAmCppOptions &emitOptions,
                          wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        std::optional<AmGraph> lowered = lower(graph, diagnostics);
        if (!lowered)
        {
            return {};
        }
        return run(std::move(*lowered), scheduleOptions, emitOptions, diagnostics);
    }

} // namespace wolvrix::lib::grhsim::am
