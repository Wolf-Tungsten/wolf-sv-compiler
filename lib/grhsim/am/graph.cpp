#include "grhsim/am/graph.hpp"

#include "grhsim/am/opcode_traits.hpp"
#include "program_internal.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

        std::optional<VariableId> stateWriteTargetOf(ProgramView program,
                                                     InstructionId instruction)
        {
            const OpcodeTraits traits = opcodeTraits(program.opcode(instruction));
            if (traits.effect != OpcodeEffect::StateReadWrite ||
                traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand)
            {
                return std::nullopt;
            }
            const auto operands = program.operands(instruction);
            if (traits.stateTargetOperand >= operands.size())
            {
                return std::nullopt;
            }
            return operands[traits.stateTargetOperand];
        }

        AmStateKind stateKindOf(Opcode opcode)
        {
            switch (opcode)
            {
            case Opcode::LatchWrite:
                return AmStateKind::Latch;
            case Opcode::MemoryWrite:
            case Opcode::MemoryFill:
            case Opcode::MemoryWriteLanes:
                return AmStateKind::Memory;
            default:
                return AmStateKind::Register;
            }
        }

        uint64_t stateAccessKey(InstructionId instruction, std::size_t position)
        {
            return (static_cast<uint64_t>(instruction.value) << 32U) |
                   static_cast<uint64_t>(position);
        }

        template <typename Record, typename Attr>
        void upsertAttribute(std::vector<Record> &records, InstructionId instruction,
                             const Attr &attributes)
        {
            const auto it = std::lower_bound(
                records.begin(), records.end(), instruction,
                [](const Record &record, InstructionId needle) {
                    return record.instruction < needle;
                });
            if (it != records.end() && it->instruction == instruction)
            {
                it->attributes = attributes;
                return;
            }
            records.insert(it, Record{.instruction = instruction, .attributes = attributes});
        }
    } // namespace

    struct AmGraph::Impl
    {
        detail::ProgramStorage storage;
        ProgramInterface interface;
        std::vector<ValueFacts> valueFacts;
        std::vector<InstructionEffect> effects;
        std::vector<uint8_t> removed;
        std::vector<OrderedEffect> orderedEffects;
        std::unordered_map<uint64_t, AmStateAccess> stateAccesses;
    };

    AmGraph::AmGraph() : impl_(std::make_unique<Impl>()) {}
    AmGraph::~AmGraph() = default;
    AmGraph::AmGraph(AmGraph &&) noexcept = default;
    AmGraph &AmGraph::operator=(AmGraph &&) noexcept = default;

    AmGraph AmGraph::fromLinearProgram(const LinearProgramArtifact &artifact)
    {
        AmGraph graph;
        Impl &impl = *graph.impl_;
        if (artifact.program.valid())
        {
            impl.storage = *artifact.program.storage_;
        }
        impl.interface = artifact.interface;

        const ProgramView program(&impl.storage);
        const std::size_t variableCount = program.variableCount();
        const auto &roles = artifact.schedulingFacts.variableRoles;
        impl.valueFacts.assign(variableCount, ValueFacts{});
        for (uint32_t variable = 0; variable < variableCount; ++variable)
        {
            ValueFacts facts;
            if (variable < roles.size())
            {
                facts.roles = roles[variable];
            }
            const VariableRecord &record = program.variable(VariableId{variable});
            if (hasRole(facts.roles, VariableRole::State))
            {
                facts.kind = AmValueKind::State;
            }
            else if (hasRole(facts.roles, VariableRole::ExternalInput))
            {
                facts.kind = AmValueKind::Input;
            }
            else if (record.init.valid() &&
                     program.init(record.init).kind == InitKind::Constant)
            {
                facts.kind = AmValueKind::Constant;
            }
            if (facts.kind == AmValueKind::State &&
                program.type(record.type).kind == TypeKind::Array)
            {
                facts.stateKind = AmStateKind::Memory;
            }
            impl.valueFacts[variable] = facts;
        }
        // Refine the state kind from the writes that target each state value.
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            const std::optional<VariableId> target = stateWriteTargetOf(program, instruction);
            if (target && target->valid() && target->value < variableCount)
            {
                impl.valueFacts[target->value].stateKind = stateKindOf(program.opcode(instruction));
            }
        }

        if (artifact.schedulingFacts.instructionEffects.size() == program.instructionCount())
        {
            impl.effects = artifact.schedulingFacts.instructionEffects;
        }
        else
        {
            impl.effects.assign(program.instructionCount(), InstructionEffect::Pure);
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const Opcode opcode = program.opcode(InstructionId{index});
                switch (opcodeTraits(opcode).effect)
                {
                case OpcodeEffect::Pure:
                    impl.effects[index] = InstructionEffect::Pure;
                    break;
                case OpcodeEffect::ChangeDetector:
                case OpcodeEffect::StateReadWrite:
                    impl.effects[index] = InstructionEffect::StateReadWrite;
                    break;
                case OpcodeEffect::StateRead:
                    impl.effects[index] = InstructionEffect::StateRead;
                    break;
                case OpcodeEffect::HostRead:
                    impl.effects[index] = InstructionEffect::HostRead;
                    break;
                case OpcodeEffect::HostEffect:
                case OpcodeEffect::Activation:
                    impl.effects[index] = InstructionEffect::HostEffect;
                    break;
                }
            }
        }
        impl.removed.assign(program.instructionCount(), uint8_t{0});
        impl.orderedEffects = artifact.schedulingFacts.orderedEffects;
        return graph;
    }

    LinearProgramArtifact AmGraph::toLinearProgram() const
    {
        const Impl &impl = *impl_;
        LinearProgramArtifact artifact;
        artifact.interface = impl.interface;

        const uint32_t oldCount = static_cast<uint32_t>(impl.storage.opcodes.size());
        const bool hasTombstones =
            std::any_of(impl.removed.begin(), impl.removed.end(),
                        [](uint8_t flag) { return flag != 0; });
        std::vector<uint32_t> remap;
        if (hasTombstones)
        {
            remap.assign(oldCount, kInvalidIndex);
            uint32_t liveCount = 0;
            for (uint32_t index = 0; index < oldCount; ++index)
            {
                if (!impl.removed[index])
                {
                    remap[index] = liveCount++;
                }
            }
        }

        auto storage = std::make_unique<detail::ProgramStorage>();
        *storage = impl.storage;
        if (hasTombstones)
        {
            // Compact away tombstoned instructions and remap every
            // instruction-indexed side table to the dense new ids.
            detail::ProgramStorage compacted;
            compacted.types = std::move(storage->types);
            compacted.stringOffsets = std::move(storage->stringOffsets);
            compacted.stringBytes = std::move(storage->stringBytes);
            compacted.initDescriptors = std::move(storage->initDescriptors);
            compacted.initActions = std::move(storage->initActions);
            compacted.literals = std::move(storage->literals);
            compacted.literalWords = std::move(storage->literalWords);
            compacted.literalBytes = std::move(storage->literalBytes);
            compacted.variables = std::move(storage->variables);
            compacted.variableLabels = std::move(storage->variableLabels);
            compacted.dpiImports = std::move(storage->dpiImports);
            compacted.dpiParameters = std::move(storage->dpiParameters);
            for (uint32_t index = 0; index < oldCount; ++index)
            {
                if (impl.removed[index])
                {
                    continue;
                }
                compacted.opcodes.push_back(impl.storage.opcodes[index]);
                for (uint32_t operand = impl.storage.operandOffsets[index];
                     operand < impl.storage.operandOffsets[index + 1]; ++operand)
                {
                    compacted.operands.push_back(impl.storage.operands[operand]);
                }
                compacted.operandOffsets.push_back(
                    static_cast<uint32_t>(compacted.operands.size()));
                for (uint32_t result = impl.storage.resultOffsets[index];
                     result < impl.storage.resultOffsets[index + 1]; ++result)
                {
                    compacted.results.push_back(impl.storage.results[result]);
                }
                compacted.resultOffsets.push_back(
                    static_cast<uint32_t>(compacted.results.size()));
            }
            const auto remapRecords = [&](auto &records) {
                using Record = std::decay_t<decltype(*records.begin())>;
                std::vector<Record> kept;
                for (const Record &record : records)
                {
                    if (record.instruction.value < oldCount &&
                        remap[record.instruction.value] != kInvalidIndex)
                    {
                        Record copy = record;
                        copy.instruction = InstructionId{remap[record.instruction.value]};
                        kept.push_back(copy);
                    }
                }
                records = std::move(kept);
            };
            remapRecords(compacted.sliceStaticAttributes);
            remapRecords(compacted.systemFunctionAttributes);
            remapRecords(compacted.systemTaskAttributes);
            remapRecords(compacted.dpiCallAttributes);
            remapRecords(compacted.activationAttributes);
            *storage = std::move(compacted);
        }

        artifact.program = LinearProgram(std::move(storage));
        artifact.schedulingFacts.variableRoles.reserve(impl.valueFacts.size());
        for (const ValueFacts &facts : impl.valueFacts)
        {
            artifact.schedulingFacts.variableRoles.push_back(facts.roles);
        }
        if (hasTombstones)
        {
            artifact.schedulingFacts.instructionEffects.reserve(impl.effects.size());
            for (uint32_t index = 0; index < impl.effects.size(); ++index)
            {
                if (!impl.removed[index])
                {
                    artifact.schedulingFacts.instructionEffects.push_back(impl.effects[index]);
                }
            }
            for (const OrderedEffect &effect : impl.orderedEffects)
            {
                if (effect.instruction.valid() && effect.instruction.value < oldCount &&
                    remap[effect.instruction.value] != kInvalidIndex)
                {
                    artifact.schedulingFacts.orderedEffects.push_back(OrderedEffect{
                        .instruction = InstructionId{remap[effect.instruction.value]},
                        .group = effect.group,
                        .ordinal = effect.ordinal,
                    });
                }
            }
        }
        else
        {
            artifact.schedulingFacts.instructionEffects = impl.effects;
            artifact.schedulingFacts.orderedEffects = impl.orderedEffects;
        }
        return artifact;
    }

    ProgramView AmGraph::program() const noexcept
    {
        return ProgramView(&impl_->storage);
    }

    const ProgramInterface &AmGraph::interface() const noexcept
    {
        return impl_->interface;
    }

    ProgramInterface &AmGraph::mutableInterface() noexcept
    {
        return impl_->interface;
    }

    std::vector<OrderedEffect> &AmGraph::orderedEffects() noexcept
    {
        return impl_->orderedEffects;
    }

    const std::vector<OrderedEffect> &AmGraph::orderedEffects() const noexcept
    {
        return impl_->orderedEffects;
    }

    std::size_t AmGraph::variableCount() const noexcept
    {
        return impl_->storage.variables.size();
    }

    std::size_t AmGraph::instructionCount() const noexcept
    {
        return impl_->storage.opcodes.size();
    }

    const AmGraph::ValueFacts &AmGraph::valueFacts(VariableId variable) const
    {
        static const ValueFacts fallback{};
        if (!variable.valid() || variable.value >= impl_->valueFacts.size())
        {
            return fallback;
        }
        return impl_->valueFacts[variable.value];
    }

    void AmGraph::setValueFacts(VariableId variable, ValueFacts facts)
    {
        if (variable.valid() && variable.value < impl_->valueFacts.size())
        {
            impl_->valueFacts[variable.value] = facts;
        }
    }

    bool AmGraph::instructionRemoved(InstructionId instruction) const
    {
        return instruction.valid() && instruction.value < impl_->removed.size() &&
               impl_->removed[instruction.value] != 0;
    }

    VariableId AmGraph::addVariable(TypeId type, InitId init, std::optional<StringId> label,
                                    ValueFacts facts)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const VariableId id{static_cast<uint32_t>(storage.variables.size())};
        storage.variables.push_back(VariableRecord{.type = type, .init = init});
        if (label && label->valid())
        {
            storage.variableLabels.push_back(VariableLabel{.variable = id, .label = *label});
        }
        impl_->valueFacts.push_back(facts);
        return id;
    }

    InstructionId AmGraph::addInstruction(Opcode opcode, std::span<const VariableId> results,
                                          std::span<const VariableId> operands)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const InstructionId id{static_cast<uint32_t>(storage.opcodes.size())};
        storage.opcodes.push_back(opcode);
        storage.operands.insert(storage.operands.end(), operands.begin(), operands.end());
        storage.operandOffsets.push_back(static_cast<uint32_t>(storage.operands.size()));
        storage.results.insert(storage.results.end(), results.begin(), results.end());
        storage.resultOffsets.push_back(static_cast<uint32_t>(storage.results.size()));
        impl_->removed.push_back(uint8_t{0});
        switch (opcodeTraits(opcode).effect)
        {
        case OpcodeEffect::Pure:
            impl_->effects.push_back(InstructionEffect::Pure);
            break;
        case OpcodeEffect::ChangeDetector:
        case OpcodeEffect::StateReadWrite:
            impl_->effects.push_back(InstructionEffect::StateReadWrite);
            break;
        case OpcodeEffect::StateRead:
            impl_->effects.push_back(InstructionEffect::StateRead);
            break;
        case OpcodeEffect::HostRead:
            impl_->effects.push_back(InstructionEffect::HostRead);
            break;
        case OpcodeEffect::HostEffect:
        case OpcodeEffect::Activation:
            impl_->effects.push_back(InstructionEffect::HostEffect);
            break;
        }
        return id;
    }

    void AmGraph::setInstructionOperand(InstructionId instruction, std::size_t position,
                                        VariableId operand)
    {
        detail::ProgramStorage &storage = impl_->storage;
        if (!instruction.valid() || instruction.value >= storage.opcodes.size())
        {
            return;
        }
        const uint32_t begin = storage.operandOffsets[instruction.value];
        const uint32_t end = storage.operandOffsets[instruction.value + 1];
        if (begin + position < end)
        {
            storage.operands[begin + position] = operand;
        }
    }

    void AmGraph::removeInstruction(InstructionId instruction)
    {
        if (instruction.valid() && instruction.value < impl_->removed.size())
        {
            impl_->removed[instruction.value] = 1;
        }
    }

    void AmGraph::setSliceStaticAttributes(InstructionId instruction, uint32_t lsb)
    {
        upsertAttribute(impl_->storage.sliceStaticAttributes, instruction,
                        SliceStaticAttributes{.lsb = lsb});
    }

    void AmGraph::setSystemFunctionAttributes(InstructionId instruction,
                                              const SystemFunctionAttributes &attributes)
    {
        upsertAttribute(impl_->storage.systemFunctionAttributes, instruction, attributes);
    }

    void AmGraph::setSystemTaskAttributes(InstructionId instruction,
                                          const SystemTaskAttributes &attributes)
    {
        upsertAttribute(impl_->storage.systemTaskAttributes, instruction, attributes);
    }

    void AmGraph::setDpiCallAttributes(InstructionId instruction,
                                       const DpiCallAttributes &attributes)
    {
        upsertAttribute(impl_->storage.dpiCallAttributes, instruction, attributes);
    }

    TypeId AmGraph::addType(const Type &type)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const TypeId id{static_cast<uint32_t>(storage.types.size())};
        storage.types.push_back(type);
        return id;
    }

    StringId AmGraph::addString(std::string_view text)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const StringId id{static_cast<uint32_t>(storage.stringOffsets.size() - 1)};
        storage.stringBytes.insert(storage.stringBytes.end(), text.begin(), text.end());
        storage.stringOffsets.push_back(static_cast<uint32_t>(storage.stringBytes.size()));
        return id;
    }

    LiteralId AmGraph::addBitLiteral(TypeId type, std::span<const uint64_t> words)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const LiteralId id{static_cast<uint32_t>(storage.literals.size())};
        const uint32_t begin = static_cast<uint32_t>(storage.literalWords.size());
        storage.literalWords.insert(storage.literalWords.end(), words.begin(), words.end());
        storage.literals.push_back(detail::LiteralRecord{
            .type = type,
            .words = Range32{.offset = begin,
                             .count = static_cast<uint32_t>(storage.literalWords.size() - begin)},
            .bytes = Range32{},
        });
        return id;
    }

    InitId AmGraph::addConstantInit(LiteralId literal)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const InitId id{static_cast<uint32_t>(storage.initDescriptors.size())};
        storage.initDescriptors.push_back(InitDescriptor{
            .kind = InitKind::Constant,
            .payload = literal.value,
            .count = 0,
        });
        return id;
    }

    DpiImportId AmGraph::addDpiImport(StringId symbol,
                                      std::span<const DpiParameter> parameters,
                                      DpiReturn returnValue)
    {
        detail::ProgramStorage &storage = impl_->storage;
        const DpiImportId id{static_cast<uint32_t>(storage.dpiImports.size())};
        const uint32_t begin = static_cast<uint32_t>(storage.dpiParameters.size());
        storage.dpiParameters.insert(storage.dpiParameters.end(), parameters.begin(),
                                     parameters.end());
        storage.dpiImports.push_back(detail::DpiImportRecord{
            .symbol = symbol,
            .parameters =
                Range32{.offset = begin,
                        .count =
                            static_cast<uint32_t>(storage.dpiParameters.size() - begin)},
            .returnValue = returnValue,
        });
        return id;
    }

    AmStateAccess AmGraph::stateAccess(InstructionId instruction,
                                       std::size_t operandPosition) const
    {
        const auto it = impl_->stateAccesses.find(stateAccessKey(instruction, operandPosition));
        return it == impl_->stateAccesses.end() ? AmStateAccess::PreCommit : it->second;
    }

    void AmGraph::setStateAccess(InstructionId instruction, std::size_t operandPosition,
                                 AmStateAccess access)
    {
        const uint64_t key = stateAccessKey(instruction, operandPosition);
        if (access == AmStateAccess::PreCommit)
        {
            impl_->stateAccesses.erase(key);
            return;
        }
        impl_->stateAccesses[key] = access;
    }

    InstructionEffect AmGraph::instructionEffect(InstructionId instruction) const
    {
        if (!instruction.valid() || instruction.value >= impl_->effects.size())
        {
            return InstructionEffect::Pure;
        }
        return impl_->effects[instruction.value];
    }

    void AmGraph::setInstructionEffect(InstructionId instruction, InstructionEffect effect)
    {
        if (instruction.valid() && instruction.value < impl_->effects.size())
        {
            impl_->effects[instruction.value] = effect;
        }
    }

    const std::vector<InstructionEffect> &AmGraph::instructionEffects() const noexcept
    {
        return impl_->effects;
    }

    std::vector<VariableRole> AmGraph::variableRoles() const
    {
        std::vector<VariableRole> roles;
        roles.reserve(impl_->valueFacts.size());
        for (const ValueFacts &facts : impl_->valueFacts)
        {
            roles.push_back(facts.roles);
        }
        return roles;
    }

} // namespace wolvrix::lib::grhsim::am
