#include "grhsim/am/pipeline.hpp"

#include "grhsim/am/opcode_traits.hpp"

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
                target = 3;
                break;
            case Opcode::MemoryRead:
                target = 0;
                break;
            case Opcode::MemoryWrite:
                target = 4;
                break;
            case Opcode::MemoryFill:
                target = 2;
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

        void validatePreCommitSnapshots(ValidationResult &result,
                                        ProgramView program,
                                        std::span<const PreCommitSnapshot> snapshots,
                                        const ValidationOptions &options)
        {
            std::vector<uint32_t> targets;
            targets.reserve(snapshots.size());
            for (const PreCommitSnapshot &snapshot : snapshots)
            {
                const Type *sourceType = variableType(program, snapshot.source);
                const Type *targetType = variableType(program, snapshot.target);
                if (!sourceType || !targetType ||
                    sourceType->kind != TypeKind::BitVector ||
                    snapshot.source == snapshot.target ||
                    *sourceType != *targetType)
                {
                    addError(result, options,
                             "AM pre-commit snapshot has invalid or mismatched variables");
                    continue;
                }
                if (program.init(program.variable(snapshot.target).init).kind ==
                    InitKind::Constant)
                {
                    addError(result, options,
                             "AM pre-commit snapshot target must be writable");
                }
                targets.push_back(snapshot.target.value);
            }
            std::sort(targets.begin(), targets.end());
            if (std::adjacent_find(targets.begin(), targets.end()) != targets.end())
            {
                addError(result, options,
                         "AM pre-commit snapshot targets must be unique");
            }
        }

        void validateCommitOperandCaptures(
            ValidationResult &result, ProgramView program,
            std::span<const CommitOperandCapture> captures,
            const ValidationOptions &options)
        {
            std::vector<uint32_t> sources;
            std::vector<uint32_t> targets;
            sources.reserve(captures.size());
            targets.reserve(captures.size());
            for (const CommitOperandCapture &capture : captures)
            {
                const Type *sourceType = variableType(program, capture.source);
                const Type *targetType = variableType(program, capture.target);
                if (!sourceType || !targetType ||
                    sourceType->kind != TypeKind::BitVector ||
                    capture.source == capture.target || *sourceType != *targetType)
                {
                    addError(result, options,
                             "AM commit operand capture has invalid or mismatched variables");
                    continue;
                }
                if (program.init(program.variable(capture.target).init).kind ==
                    InitKind::Constant)
                {
                    addError(result, options,
                             "AM commit operand capture target must be writable");
                }
                sources.push_back(capture.source.value);
                targets.push_back(capture.target.value);
            }
            std::sort(sources.begin(), sources.end());
            std::sort(targets.begin(), targets.end());
            if (std::adjacent_find(targets.begin(), targets.end()) != targets.end())
            {
                addError(result, options,
                         "AM commit operand capture targets must be unique");
            }
            if (std::any_of(targets.begin(), targets.end(),
                            [&](uint32_t target) {
                                return std::binary_search(sources.begin(), sources.end(),
                                                          target);
                            }))
            {
                addError(result, options,
                         "AM commit operand capture sources and targets must not alias");
            }
            bool targetDefinedByInstruction = false;
            for (uint32_t instruction = 0;
                 instruction < program.instructionCount() &&
                 !targetDefinedByInstruction;
                 ++instruction)
            {
                for (VariableId result : program.results(InstructionId{instruction}))
                {
                    if (std::binary_search(targets.begin(), targets.end(),
                                           result.value))
                    {
                        targetDefinedByInstruction = true;
                        break;
                    }
                }
            }
            if (targetDefinedByInstruction)
            {
                addError(result, options,
                         "AM commit operand capture target must not have an instruction definition");
            }
        }

        void validateCommitEventOwnership(ValidationResult &result,
                                          const ExecutableModel &model,
                                          const ValidationOptions &options)
        {
            const ProgramView program = model.program.view();
            std::vector<bool> changedVariables(program.variableCount(), false);
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const InstructionId instruction{index};
                if (!isChangedOpcode(program.opcode(instruction)))
                {
                    continue;
                }
                const auto results = program.results(instruction);
                if (results.size() == 1 && results.front().valid() &&
                    results.front().value < changedVariables.size())
                {
                    changedVariables[results.front().value] = true;
                }
            }

            std::vector<bool> commitEvents(program.variableCount(), false);
            for (uint32_t blockIndex = model.commitBlockBegin;
                 blockIndex < model.commitBlockEnd; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0;
                     position < model.program.blockSize(block); ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = program.opcode(instruction);
                    const auto operands = program.operands(instruction);
                    std::size_t eventBegin = operands.size();
                    if (opcode == Opcode::RegisterWrite)
                    {
                        eventBegin = 4;
                    }
                    else if (opcode == Opcode::MemoryWrite)
                    {
                        eventBegin = 5;
                    }
                    else if (opcode == Opcode::MemoryFill)
                    {
                        eventBegin = 3;
                    }
                    for (std::size_t index = eventBegin; index < operands.size(); ++index)
                    {
                        const VariableId event = operands[index];
                        if (event.valid() && event.value < changedVariables.size() &&
                            changedVariables[event.value])
                        {
                            commitEvents[event.value] = true;
                        }
                    }
                }
            }
            const auto isCommitEvent = [&](VariableId variable) {
                return variable.valid() && variable.value < commitEvents.size() &&
                       commitEvents[variable.value];
            };

            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const InstructionId instruction{index};
                const Opcode opcode = program.opcode(instruction);
                if (!isChangedOpcode(opcode))
                {
                    for (VariableId variable : program.results(instruction))
                    {
                        if (isCommitEvent(variable))
                        {
                            addError(
                                result, options,
                                "AM commit event has a non-Changed instruction result writer: variable=" +
                                    std::to_string(variable.value) + " instruction=" +
                                    std::to_string(index));
                        }
                    }
                }

                const bool writesState =
                    opcode == Opcode::RegisterWrite || opcode == Opcode::LatchWrite ||
                    opcode == Opcode::MemoryWrite || opcode == Opcode::MemoryFill;
                if (writesState)
                {
                    const std::optional<VariableId> target =
                        stateTarget(program, instruction);
                    if (target && isCommitEvent(*target))
                    {
                        addError(result, options,
                                 "AM commit event is a state write target: variable=" +
                                     std::to_string(target->value) + " instruction=" +
                                     std::to_string(index));
                    }
                }

                if (isChangedOpcode(opcode))
                {
                    const auto operands = program.operands(instruction);
                    if (operands.size() == 2 && isCommitEvent(operands[1]))
                    {
                        addError(result, options,
                                 "AM commit event is a Changed old operand: variable=" +
                                     std::to_string(operands[1].value) + " instruction=" +
                                     std::to_string(index));
                    }
                }
            }

            for (const PreCommitSnapshot &snapshot : model.preCommitSnapshots)
            {
                if (isCommitEvent(snapshot.target))
                {
                    addError(result, options,
                             "AM commit event is a pre-commit snapshot target: variable=" +
                                 std::to_string(snapshot.target.value));
                }
            }
            for (const CommitOperandCapture &capture : model.commitOperandCaptures)
            {
                if (isCommitEvent(capture.target))
                {
                    addError(result, options,
                             "AM commit event is a commit operand capture target: variable=" +
                                 std::to_string(capture.target.value));
                }
            }
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
            validatePreCommitSnapshots(result, program, artifact.preCommitSnapshots,
                                       options);
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

        validatePreCommitSnapshots(result, model.program.view(),
                                   model.preCommitSnapshots, options);
        validateCommitOperandCaptures(result, model.program.view(),
                                      model.commitOperandCaptures, options);

        const bool hasCommitRange = model.commitBlockBegin != 0 || model.commitBlockEnd != 0;
        if (hasCommitRange &&
            (model.commitBlockBegin == 0 || model.commitBlockBegin >= model.commitBlockEnd ||
             model.commitBlockEnd > model.program.blockCount()))
        {
            addError(result, options,
                     "ExecutableModel has an invalid commit Block range");
        }
        if (hasCommitRange && model.commitBlockBegin < model.commitBlockEnd)
        {
            const std::size_t commitCount = model.commitBlockEnd - model.commitBlockBegin;
            bool validPlan = model.commitBlockOrder.size() == commitCount &&
                             model.commitGroupOffsets.size() >= 2 &&
                             model.commitGroupOffsets.front() == 0 &&
                             model.commitGroupOffsets.back() == commitCount;
            const bool hasCapturePlan =
                !model.commitOperandCaptures.empty() ||
                !model.commitOperandCaptureOffsets.empty();
            bool validCaptures =
                !hasCapturePlan ||
                (model.commitOperandCaptureOffsets.size() == commitCount + 1 &&
                 model.commitOperandCaptureOffsets.front() == 0 &&
                 model.commitOperandCaptureOffsets.back() ==
                     model.commitOperandCaptures.size());
            std::vector<bool> seen(commitCount, false);
            for (std::size_t index = 1; index < model.commitGroupOffsets.size(); ++index)
            {
                validPlan = validPlan &&
                            model.commitGroupOffsets[index] >
                                model.commitGroupOffsets[index - 1] &&
                            model.commitGroupOffsets[index] <= commitCount;
            }
            for (BlockId block : model.commitBlockOrder)
            {
                if (!block.valid() || block.value < model.commitBlockBegin ||
                    block.value >= model.commitBlockEnd)
                {
                    validPlan = false;
                    continue;
                }
                const std::size_t index = block.value - model.commitBlockBegin;
                validPlan = validPlan && !seen[index];
                seen[index] = true;
            }
            uint32_t previousCaptureOffset = 0;
            for (uint32_t offset : model.commitOperandCaptureOffsets)
            {
                validCaptures = validCaptures &&
                                offset >= previousCaptureOffset &&
                                offset <= model.commitOperandCaptures.size();
                previousCaptureOffset = offset;
            }
            if (!validPlan)
            {
                addError(result, options,
                         "ExecutableModel has an invalid commit Block execution plan");
            }
            if (!validCaptures)
            {
                addError(result, options,
                         "ExecutableModel has an invalid commit operand capture plan");
            }
            if (model.commitBlockBegin != 0 &&
                model.commitBlockEnd <= model.program.blockCount())
            {
                validateCommitEventOwnership(result, model, options);
            }
        }
        else if (!model.commitBlockOrder.empty() || !model.commitGroupOffsets.empty() ||
                 !model.commitOperandCaptures.empty() ||
                 !model.commitOperandCaptureOffsets.empty())
        {
            addError(result, options,
                     "ExecutableModel has a commit execution plan without a commit Block range");
        }

        const std::vector<uint32_t> interfaceInputs = interfaceInputVariables(model.interface);
        if (interfaceInputs.empty())
        {
            return result;
        }
        const ProgramView program = model.program.view();
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
        if (!std::includes(watchedInputs.begin(), watchedInputs.end(),
                           interfaceInputs.begin(), interfaceInputs.end()))
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
