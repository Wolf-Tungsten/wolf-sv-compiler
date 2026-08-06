#include "grhsim/am/pipeline.hpp"

#include "grhsim/am/graph.hpp"
#include "grhsim/am/opcode_traits.hpp"
#include "grhsim/am/optimize.hpp"

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
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
            const auto operands = program.operands(instruction);
            std::size_t target = 0;
            switch (program.opcode(instruction))
            {
            case Opcode::RegisterWrite:
            case Opcode::LatchWrite:
                target = 1;
                break;
            case Opcode::MemoryRead:
                target = 0;
                break;
            case Opcode::MemoryReadAll:
                target = 0;
                break;
            case Opcode::MemoryWriteLanes:
                target = 2;
                break;
            case Opcode::MemoryWrite:
                target = 4;
                break;
            case Opcode::MemoryFill:
                target = 1;
                break;
            default:
                return std::nullopt;
            }
            return target < operands.size() ? std::optional<VariableId>(operands[target])
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
                return opcode == Opcode::RegisterWrite || opcode == Opcode::LatchWrite ||
                       opcode == Opcode::MemoryWrite || opcode == Opcode::MemoryFill ||
                       opcode == Opcode::MemoryWriteLanes;
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

    GrhSimAmPipeline::GrhSimAmPipeline(GrhToAmLoweringStage &lowering,
                                       AmActivityScheduleStage &scheduler,
                                       GrhSimAmCppEmitStage &emitter)
        : lowering_(lowering), scheduler_(scheduler), emitter_(emitter)
    {
    }

    void GrhSimAmPipeline::setAmOptimizeOptions(AmOptimizeOptions options)
    {
        optimizeOptions_ = options;
    }

    std::optional<LinearProgramArtifact>
    GrhSimAmPipeline::lower(const wolvrix::lib::grh::Graph &graph,
                            wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        if (diagnostics.hasError())
        {
            return std::nullopt;
        }
        std::optional<LinearProgramArtifact> linear = lowering_.lower(graph, diagnostics);
        if (!linear || diagnostics.hasError())
        {
            return std::nullopt;
        }
        return linear;
    }

    GrhSimAmPipelineResult
    GrhSimAmPipeline::run(LinearProgramArtifact &&linear,
                          const ActivityScheduleOptions &scheduleOptions,
                          const GrhSimAmCppOptions &emitOptions,
                          wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        GrhSimAmPipelineResult result;
        if (diagnostics.hasError())
        {
            return result;
        }
        if (!reportValidation(validate(linear,
                                       ValidationOptions{.level = ValidationLevel::Semantic}),
                              "grhsim-am-lower",
                              diagnostics))
        {
            return result;
        }

        // The graph form is the working IR: everything between lowering and
        // emission runs on the AmGraph. For now this is a lossless
        // round-trip; graph passes land here one by one.
        {
            AmGraph graph = AmGraph::fromLinearProgram(linear);
            linear = graph.toLinearProgram();
            if (!reportValidation(validate(linear,
                                           ValidationOptions{.level = ValidationLevel::Semantic}),
                                  "grhsim-am-graph",
                                  diagnostics))
            {
                return result;
            }
        }

        if (!optimizeLinearProgram(linear, optimizeOptions_, diagnostics) ||
            diagnostics.hasError())
        {
            return result;
        }

        std::optional<ExecutableModel> model =
            scheduler_.schedule(std::move(linear), scheduleOptions, diagnostics);
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

    GrhSimAmPipelineResult
    GrhSimAmPipeline::run(const wolvrix::lib::grh::Graph &graph,
                          const ActivityScheduleOptions &scheduleOptions,
                          const GrhSimAmCppOptions &emitOptions,
                          wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        std::optional<LinearProgramArtifact> linear = lower(graph, diagnostics);
        if (!linear)
        {
            return {};
        }
        return run(std::move(*linear), scheduleOptions, emitOptions, diagnostics);
    }

} // namespace wolvrix::lib::grhsim::am
