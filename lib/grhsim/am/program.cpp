#include "grhsim/am/program.hpp"

#include "program_internal.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        template <typename Id, typename Container>
        std::size_t checkedIndex(Id id, const Container &container, std::string_view what)
        {
            if (!id.valid() || static_cast<std::size_t>(id.value) >= container.size())
            {
                throw std::out_of_range(std::string("invalid ") + std::string(what));
            }
            return static_cast<std::size_t>(id.value);
        }

        template <typename Record>
        const Record *findInstructionRecord(const std::vector<Record> &records,
                                            InstructionId instruction) noexcept
        {
            if (!instruction.valid())
            {
                return nullptr;
            }
            const auto it = std::lower_bound(
                records.begin(), records.end(), instruction.value,
                [](const Record &record, uint32_t value) {
                    return record.instruction.value < value;
                });
            return it != records.end() && it->instruction == instruction ? &*it : nullptr;
        }

        template <typename T>
        uint64_t vectorBytes(const std::vector<T> &values) noexcept
        {
            return static_cast<uint64_t>(values.size()) * static_cast<uint64_t>(sizeof(T));
        }

    } // namespace

    namespace detail
    {
        ProgramStorage::ProgramStorage()
        {
            stringOffsets.push_back(0);
            operandOffsets.push_back(0);
            resultOffsets.push_back(0);
            initDescriptors.push_back(InitDescriptor{.kind = InitKind::Undef});
            initDescriptors.push_back(InitDescriptor{.kind = InitKind::Zero});
        }
    } // namespace detail

    Type Type::bitVector(uint32_t width, Signedness sign)
    {
        if (width == 0)
        {
            throw std::invalid_argument("AM bit-vector width must be positive");
        }
        return Type{
            .kind = TypeKind::BitVector,
            .signedness = sign,
            .bitWidth = width,
            .elementCount = 0,
        };
    }

    Type Type::real()
    {
        return Type{
            .kind = TypeKind::Real,
            .signedness = Signedness::Unsigned,
            .bitWidth = 0,
            .elementCount = 0,
        };
    }

    Type Type::string()
    {
        return Type{
            .kind = TypeKind::String,
            .signedness = Signedness::Unsigned,
            .bitWidth = 0,
            .elementCount = 0,
        };
    }

    Type Type::array(uint32_t elements, uint32_t elementWidth, Signedness sign)
    {
        if (elements == 0 || elementWidth == 0)
        {
            throw std::invalid_argument("AM array length and element width must be positive");
        }
        return Type{
            .kind = TypeKind::Array,
            .signedness = sign,
            .bitWidth = elementWidth,
            .elementCount = elements,
        };
    }

    std::string_view toString(Opcode opcode) noexcept
    {
        static constexpr std::array<std::string_view, 59> names = {
            "assign",
            "add",
            "sub",
            "mul",
            "div",
            "mod",
            "and",
            "or",
            "xor",
            "xnor",
            "not",
            "eq",
            "ne",
            "lt",
            "le",
            "gt",
            "ge",
            "logic_and",
            "logic_or",
            "logic_not",
            "reduce_and",
            "reduce_nand",
            "reduce_or",
            "reduce_nor",
            "reduce_xor",
            "reduce_xnor",
            "shl",
            "lshr",
            "ashr",
            "mux",
            "concat",
            "replicate",
            "slice_static",
            "slice_dynamic",
            "slice_array",
            "changed.any",
            "changed.pos",
            "changed.neg",
            "reg.write",
            "mem.read",
            "mem.write",
            "mem.fill",
            "latch.write",
            "system.function",
            "system.task",
            "dpi.call",
            "act.f",
            "act.b",
            "mem.read_all",
            "mem.write_lanes",
            "array.mux",
            "array.reduce_or",
            "array.reduce_and",
            "array.reduce_xor",
            "array.broadcast",
            "array.onehot",
            "array.reduce_lanes_or",
            "array.reduce_lanes_and",
            "array.reduce_lanes_xor",
        };
        const std::size_t index = static_cast<std::size_t>(opcode);
        return index < names.size() ? names[index] : std::string_view("unknown");
    }

    std::size_t ProgramView::typeCount() const noexcept
    {
        return storage_ ? storage_->types.size() : 0;
    }

    std::size_t ProgramView::stringCount() const noexcept
    {
        return storage_ && !storage_->stringOffsets.empty() ? storage_->stringOffsets.size() - 1 : 0;
    }

    std::size_t ProgramView::initCount() const noexcept
    {
        return storage_ ? storage_->initDescriptors.size() : 0;
    }

    std::size_t ProgramView::literalCount() const noexcept
    {
        return storage_ ? storage_->literals.size() : 0;
    }

    std::size_t ProgramView::variableCount() const noexcept
    {
        return storage_ ? storage_->variables.size() : 0;
    }

    std::size_t ProgramView::instructionCount() const noexcept
    {
        return storage_ ? storage_->opcodes.size() : 0;
    }

    std::size_t ProgramView::dpiImportCount() const noexcept
    {
        return storage_ ? storage_->dpiImports.size() : 0;
    }

    const Type &ProgramView::type(TypeId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        return storage_->types[checkedIndex(id, storage_->types, "AM TypeId")];
    }

    std::string_view ProgramView::string(StringId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        const std::size_t index = checkedIndex(id, storage_->stringOffsets, "AM StringId");
        if (index + 1 >= storage_->stringOffsets.size())
        {
            throw std::out_of_range("invalid AM StringId");
        }
        const uint32_t begin = storage_->stringOffsets[index];
        const uint32_t end = storage_->stringOffsets[index + 1];
        if (begin == end)
        {
            return {};
        }
        return std::string_view(storage_->stringBytes.data() + begin, end - begin);
    }

    const InitDescriptor &ProgramView::init(InitId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        return storage_->initDescriptors[checkedIndex(id, storage_->initDescriptors, "AM InitId")];
    }

    std::span<const InitAction> ProgramView::initActions(InitId id) const
    {
        const InitDescriptor &descriptor = init(id);
        if (descriptor.kind != InitKind::Actions)
        {
            return {};
        }
        const uint64_t end = static_cast<uint64_t>(descriptor.payload) + descriptor.count;
        if (end > storage_->initActions.size())
        {
            throw std::logic_error("AM init action range is invalid");
        }
        if (descriptor.count == 0)
        {
            return {};
        }
        return std::span<const InitAction>(storage_->initActions.data() + descriptor.payload,
                                           descriptor.count);
    }

    LiteralView ProgramView::literal(LiteralId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        const auto &record = storage_->literals[checkedIndex(id, storage_->literals, "AM LiteralId")];
        const uint64_t wordEnd = static_cast<uint64_t>(record.words.offset) + record.words.count;
        const uint64_t byteEnd = static_cast<uint64_t>(record.bytes.offset) + record.bytes.count;
        if (wordEnd > storage_->literalWords.size() || byteEnd > storage_->literalBytes.size())
        {
            throw std::logic_error("AM literal payload range is invalid");
        }
        LiteralView view{.type = record.type};
        if (record.words.count != 0)
        {
            view.words = std::span<const uint64_t>(storage_->literalWords.data() + record.words.offset,
                                                   record.words.count);
        }
        if (record.bytes.count != 0)
        {
            view.bytes = std::string_view(storage_->literalBytes.data() + record.bytes.offset,
                                          record.bytes.count);
        }
        return view;
    }

    const VariableRecord &ProgramView::variable(VariableId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        return storage_->variables[checkedIndex(id, storage_->variables, "AM VariableId")];
    }

    std::optional<StringId> ProgramView::variableLabel(VariableId id) const noexcept
    {
        if (!storage_ || !id.valid())
        {
            return std::nullopt;
        }
        const auto it = std::lower_bound(
            storage_->variableLabels.begin(), storage_->variableLabels.end(), id.value,
            [](const VariableLabel &label, uint32_t value) {
                return label.variable.value < value;
            });
        if (it == storage_->variableLabels.end() || it->variable != id)
        {
            return std::nullopt;
        }
        return it->label;
    }

    std::span<const VariableLabel> ProgramView::variableLabels() const noexcept
    {
        return storage_ ? std::span<const VariableLabel>(storage_->variableLabels) : std::span<const VariableLabel>();
    }

    Opcode ProgramView::opcode(InstructionId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        return storage_->opcodes[checkedIndex(id, storage_->opcodes, "AM InstructionId")];
    }

    std::span<const VariableId> ProgramView::operands(InstructionId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        const std::size_t index = checkedIndex(id, storage_->opcodes, "AM InstructionId");
        const uint32_t begin = storage_->operandOffsets[index];
        const uint32_t end = storage_->operandOffsets[index + 1];
        if (begin == end)
        {
            return {};
        }
        return std::span<const VariableId>(storage_->operands.data() + begin, end - begin);
    }

    std::span<const VariableId> ProgramView::results(InstructionId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        const std::size_t index = checkedIndex(id, storage_->opcodes, "AM InstructionId");
        const uint32_t begin = storage_->resultOffsets[index];
        const uint32_t end = storage_->resultOffsets[index + 1];
        if (begin == end)
        {
            return {};
        }
        return std::span<const VariableId>(storage_->results.data() + begin, end - begin);
    }

    std::optional<SliceStaticAttributes>
    ProgramView::sliceStaticAttributes(InstructionId id) const noexcept
    {
        if (!storage_)
        {
            return std::nullopt;
        }
        const auto *record = findInstructionRecord(storage_->sliceStaticAttributes, id);
        return record ? std::optional<SliceStaticAttributes>(record->attributes) : std::nullopt;
    }

    std::optional<SystemFunctionAttributes>
    ProgramView::systemFunctionAttributes(InstructionId id) const noexcept
    {
        if (!storage_)
        {
            return std::nullopt;
        }
        const auto *record = findInstructionRecord(storage_->systemFunctionAttributes, id);
        return record ? std::optional<SystemFunctionAttributes>(record->attributes) : std::nullopt;
    }

    std::optional<SystemTaskAttributes>
    ProgramView::systemTaskAttributes(InstructionId id) const noexcept
    {
        if (!storage_)
        {
            return std::nullopt;
        }
        const auto *record = findInstructionRecord(storage_->systemTaskAttributes, id);
        return record ? std::optional<SystemTaskAttributes>(record->attributes) : std::nullopt;
    }

    std::optional<DpiCallAttributes>
    ProgramView::dpiCallAttributes(InstructionId id) const noexcept
    {
        if (!storage_)
        {
            return std::nullopt;
        }
        const auto *record = findInstructionRecord(storage_->dpiCallAttributes, id);
        return record ? std::optional<DpiCallAttributes>(record->attributes) : std::nullopt;
    }

    std::optional<ActivationAttributesView>
    ProgramView::activationAttributes(InstructionId id) const noexcept
    {
        if (!storage_)
        {
            return std::nullopt;
        }
        const auto *record = findInstructionRecord(storage_->activationAttributes, id);
        if (!record)
        {
            return std::nullopt;
        }
        const uint64_t end = static_cast<uint64_t>(record->targets.offset) + record->targets.count;
        if (end > storage_->activationTargets.size())
        {
            return std::nullopt;
        }
        if (record->targets.count == 0)
        {
            return ActivationAttributesView{};
        }
        return ActivationAttributesView{
            .targets = std::span<const BlockId>(storage_->activationTargets.data() + record->targets.offset,
                                                record->targets.count),
        };
    }

    DpiImportView ProgramView::dpiImport(DpiImportId id) const
    {
        if (!storage_)
        {
            throw std::logic_error("AM ProgramView is empty");
        }
        const auto &record = storage_->dpiImports[checkedIndex(id, storage_->dpiImports, "AM DpiImportId")];
        const uint64_t end = static_cast<uint64_t>(record.parameters.offset) + record.parameters.count;
        if (end > storage_->dpiParameters.size())
        {
            throw std::logic_error("AM DPI parameter range is invalid");
        }
        DpiImportView view{
            .symbol = record.symbol,
            .returnValue = record.returnValue,
        };
        if (record.parameters.count != 0)
        {
            view.parameters = std::span<const DpiParameter>(
                storage_->dpiParameters.data() + record.parameters.offset,
                record.parameters.count);
        }
        return view;
    }

    ProgramStorageStats ProgramView::storageStats() const noexcept
    {
        ProgramStorageStats stats;
        if (!storage_)
        {
            return stats;
        }
        stats.types = storage_->types.size();
        stats.strings = stringCount();
        stats.stringBytes = storage_->stringBytes.size();
        stats.variables = storage_->variables.size();
        stats.instructions = storage_->opcodes.size();
        stats.operands = storage_->operands.size();
        stats.results = storage_->results.size();
        stats.blocks = storage_->blockOffsets.empty() ? 0 : storage_->blockOffsets.size() - 1;
        stats.blockInstructionIds = storage_->blockInstructions.size();
        const auto setArena = [&](ProgramArena kind, const auto &arena) {
            ArenaStorageStats &arenaStats = stats.arenas[static_cast<std::size_t>(kind)];
            using Element = typename std::decay_t<decltype(arena)>::value_type;
            arenaStats.elements = arena.size();
            arenaStats.capacity = arena.capacity();
            arenaStats.elementBytes = sizeof(Element);
        };
        setArena(ProgramArena::Types, storage_->types);
        setArena(ProgramArena::StringOffsets, storage_->stringOffsets);
        setArena(ProgramArena::StringBytes, storage_->stringBytes);
        setArena(ProgramArena::InitDescriptors, storage_->initDescriptors);
        setArena(ProgramArena::InitActions, storage_->initActions);
        setArena(ProgramArena::Literals, storage_->literals);
        setArena(ProgramArena::LiteralWords, storage_->literalWords);
        setArena(ProgramArena::LiteralBytes, storage_->literalBytes);
        setArena(ProgramArena::Variables, storage_->variables);
        setArena(ProgramArena::VariableLabels, storage_->variableLabels);
        setArena(ProgramArena::Opcodes, storage_->opcodes);
        setArena(ProgramArena::OperandOffsets, storage_->operandOffsets);
        setArena(ProgramArena::Operands, storage_->operands);
        setArena(ProgramArena::ResultOffsets, storage_->resultOffsets);
        setArena(ProgramArena::Results, storage_->results);
        setArena(ProgramArena::SliceStaticAttributes, storage_->sliceStaticAttributes);
        setArena(ProgramArena::SystemFunctionAttributes, storage_->systemFunctionAttributes);
        setArena(ProgramArena::SystemTaskAttributes, storage_->systemTaskAttributes);
        setArena(ProgramArena::DpiCallAttributes, storage_->dpiCallAttributes);
        setArena(ProgramArena::ActivationAttributes, storage_->activationAttributes);
        setArena(ProgramArena::ActivationTargets, storage_->activationTargets);
        setArena(ProgramArena::DpiImports, storage_->dpiImports);
        setArena(ProgramArena::DpiParameters, storage_->dpiParameters);
        setArena(ProgramArena::BlockOffsets, storage_->blockOffsets);
        setArena(ProgramArena::BlockInstructions, storage_->blockInstructions);
        stats.instructionBytes =
            vectorBytes(storage_->opcodes) + vectorBytes(storage_->operandOffsets) +
            vectorBytes(storage_->operands) + vectorBytes(storage_->resultOffsets) +
            vectorBytes(storage_->results);
        stats.variableBytes = vectorBytes(storage_->types) + vectorBytes(storage_->variables);
        stats.initAndLiteralBytes =
            vectorBytes(storage_->initDescriptors) + vectorBytes(storage_->initActions) +
            vectorBytes(storage_->literals) + vectorBytes(storage_->literalWords) +
            vectorBytes(storage_->literalBytes);
        stats.attributeBytes =
            vectorBytes(storage_->sliceStaticAttributes) +
            vectorBytes(storage_->systemFunctionAttributes) +
            vectorBytes(storage_->systemTaskAttributes) + vectorBytes(storage_->dpiCallAttributes) +
            vectorBytes(storage_->activationAttributes) + vectorBytes(storage_->activationTargets) +
            vectorBytes(storage_->dpiImports) + vectorBytes(storage_->dpiParameters);
        stats.stringAndLabelBytes = vectorBytes(storage_->stringOffsets) +
                                    vectorBytes(storage_->stringBytes) +
                                    vectorBytes(storage_->variableLabels);
        stats.blockBytes = vectorBytes(storage_->blockOffsets) + vectorBytes(storage_->blockInstructions);
        stats.estimatedBytes = stats.instructionBytes + stats.variableBytes +
                               stats.initAndLiteralBytes + stats.attributeBytes +
                               stats.stringAndLabelBytes + stats.blockBytes;
        for (const ArenaStorageStats &arena : stats.arenas)
        {
            stats.reservedBytes += arena.capacityBytes();
        }
        return stats;
    }

    LinearProgram::LinearProgram() = default;
    LinearProgram::~LinearProgram() = default;
    LinearProgram::LinearProgram(LinearProgram &&) noexcept = default;
    LinearProgram &LinearProgram::operator=(LinearProgram &&) noexcept = default;

    LinearProgram::LinearProgram(std::unique_ptr<detail::ProgramStorage> storage)
        : storage_(std::move(storage))
    {
    }

    ScheduledProgram::ScheduledProgram() = default;
    ScheduledProgram::~ScheduledProgram() = default;
    ScheduledProgram::ScheduledProgram(ScheduledProgram &&) noexcept = default;
    ScheduledProgram &ScheduledProgram::operator=(ScheduledProgram &&) noexcept = default;

    ScheduledProgram::ScheduledProgram(std::unique_ptr<detail::ProgramStorage> storage)
        : storage_(std::move(storage))
    {
    }

    std::size_t ScheduledProgram::blockCount() const noexcept
    {
        return storage_ && !storage_->blockOffsets.empty() ? storage_->blockOffsets.size() - 1 : 0;
    }

    std::size_t ScheduledProgram::blockSize(BlockId block) const
    {
        if (!storage_ || !block.valid() || static_cast<std::size_t>(block.value) >= blockCount())
        {
            throw std::out_of_range("invalid AM BlockId");
        }
        const std::size_t index = block.value;
        return static_cast<std::size_t>(storage_->blockOffsets[index + 1] - storage_->blockOffsets[index]);
    }

    InstructionId ScheduledProgram::blockInstruction(BlockId block, std::size_t index) const
    {
        const std::size_t count = blockSize(block);
        if (index >= count)
        {
            throw std::out_of_range("invalid AM block instruction index");
        }
        const uint32_t flatIndex = storage_->blockOffsets[block.value] + static_cast<uint32_t>(index);
        if (storage_->blockInstructions.empty())
        {
            return InstructionId{flatIndex};
        }
        return storage_->blockInstructions[flatIndex];
    }

} // namespace wolvrix::lib::grhsim::am
