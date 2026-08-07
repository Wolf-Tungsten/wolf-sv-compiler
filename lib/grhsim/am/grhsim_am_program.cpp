#include "grhsim/am/grhsim_am_program.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

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
        static constexpr std::array<std::string_view, 68> names = {
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
            "reg.write.c",
            "reg.write.m",
            "reg.write.cm",
            "latch.write.c",
            "latch.write.m",
            "latch.write.cm",
            "mem.write.c",
            "mem.write.m",
            "mem.write.cm",
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
        stats.atoms = storage_->atomKinds.size();
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
        setArena(ProgramArena::BlockAtomOffsets, storage_->blockAtomOffsets);
        setArena(ProgramArena::AtomInstructionOffsets, storage_->atomInstructionOffsets);
        setArena(ProgramArena::AtomKinds, storage_->atomKinds);
        setArena(ProgramArena::AtomSignatures, storage_->atomSignatures);
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
        stats.blockBytes = vectorBytes(storage_->blockOffsets) +
                           vectorBytes(storage_->blockInstructions) +
                           vectorBytes(storage_->blockAtomOffsets) +
                           vectorBytes(storage_->atomInstructionOffsets) +
                           vectorBytes(storage_->atomKinds) +
                           vectorBytes(storage_->atomSignatures);
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

    std::size_t ScheduledProgram::atomCount() const noexcept
    {
        return storage_ ? storage_->atomKinds.size() : 0;
    }

    std::size_t ScheduledProgram::blockAtomCount(BlockId block) const
    {
        if (!storage_ || !block.valid() || static_cast<std::size_t>(block.value) >= blockCount())
        {
            throw std::out_of_range("invalid AM BlockId");
        }
        const std::size_t index = block.value;
        return static_cast<std::size_t>(storage_->blockAtomOffsets[index + 1] -
                                        storage_->blockAtomOffsets[index]);
    }

    AtomId ScheduledProgram::blockAtom(BlockId block, std::size_t index) const
    {
        if (index >= blockAtomCount(block))
        {
            throw std::out_of_range("invalid AM block atom index");
        }
        return AtomId{storage_->blockAtomOffsets[block.value] +
                      static_cast<uint32_t>(index)};
    }

    std::size_t ScheduledProgram::atomInstructionCount(AtomId atom) const
    {
        if (!storage_ || !atom.valid() || static_cast<std::size_t>(atom.value) >= atomCount())
        {
            throw std::out_of_range("invalid AM AtomId");
        }
        const std::size_t index = atom.value;
        return static_cast<std::size_t>(storage_->atomInstructionOffsets[index + 1] -
                                        storage_->atomInstructionOffsets[index]);
    }

    InstructionId ScheduledProgram::atomInstruction(AtomId atom, std::size_t index) const
    {
        const std::size_t count = atomInstructionCount(atom);
        if (index >= count)
        {
            throw std::out_of_range("invalid AM atom instruction index");
        }
        const uint32_t flatIndex =
            storage_->atomInstructionOffsets[atom.value] + static_cast<uint32_t>(index);
        if (storage_->blockInstructions.empty())
        {
            return InstructionId{flatIndex};
        }
        return storage_->blockInstructions[flatIndex];
    }

    AmAtomKind ScheduledProgram::atomKind(AtomId atom) const
    {
        if (!storage_ || !atom.valid() || static_cast<std::size_t>(atom.value) >= atomCount())
        {
            throw std::out_of_range("invalid AM AtomId");
        }
        return static_cast<AmAtomKind>(storage_->atomKinds[atom.value]);
    }

    uint32_t ScheduledProgram::atomSignature(AtomId atom) const
    {
        if (!storage_ || !atom.valid() || static_cast<std::size_t>(atom.value) >= atomCount())
        {
            throw std::out_of_range("invalid AM AtomId");
        }
        return storage_->atomSignatures[atom.value];
    }

    namespace
    {
        constexpr uint64_t kMaxArenaEntries = std::numeric_limits<uint32_t>::max();
        constexpr uint64_t kMaxOffsetTableRecords = kMaxArenaEntries - 1;

        detail::ProgramStorage &requireStorage(std::unique_ptr<detail::ProgramStorage> &storage,
                                               bool finished)
        {
            if (finished || !storage)
            {
                throw std::logic_error("AM builder has already been finished or moved");
            }
            return *storage;
        }

        const detail::ProgramStorage *viewStorage(const std::unique_ptr<detail::ProgramStorage> &storage,
                                                  bool finished) noexcept
        {
            return finished ? nullptr : storage.get();
        }

        void requireRoom(std::size_t current, std::size_t additional, std::string_view arena)
        {
            if (current > kMaxArenaEntries || additional > kMaxArenaEntries - current)
            {
                throw std::overflow_error(std::string("AM ") + std::string(arena) +
                                          " exceeds the 32-bit arena limit");
            }
        }

        void requireOffsetTableRoom(std::size_t currentRecords,
                                    std::size_t additionalRecords,
                                    std::string_view arena)
        {
            if (currentRecords > kMaxOffsetTableRecords ||
                additionalRecords > kMaxOffsetTableRecords - currentRecords)
            {
                throw std::overflow_error(std::string("AM ") + std::string(arena) +
                                          " exceeds the 32-bit offset-table limit");
            }
        }

        template <typename T>
        void ensureAppendCapacity(std::vector<T> &arena, std::size_t additional)
        {
            if (additional > arena.max_size() - arena.size())
            {
                throw std::length_error("AM arena exceeds host vector capacity");
            }
            const std::size_t required = arena.size() + additional;
            if (required <= arena.capacity())
            {
                return;
            }
            const std::size_t growth = std::max<std::size_t>(arena.capacity() / 2, 1);
            const std::size_t grown = arena.capacity() <= arena.max_size() - growth
                                          ? arena.capacity() + growth
                                          : arena.max_size();
            arena.reserve(std::max(required, grown));
        }

        template <typename T>
        bool aliasesArena(std::span<const T> values, const std::vector<T> &arena) noexcept
        {
            if (values.empty() || arena.empty())
            {
                return false;
            }
            const T *valueBegin = values.data();
            const T *valueEnd = valueBegin + values.size();
            const T *arenaBegin = arena.data();
            const T *arenaEnd = arenaBegin + arena.size();
            const std::less<const T *> less;
            return less(valueBegin, arenaEnd) && less(arenaBegin, valueEnd);
        }

        void requireValidType(const Type &type)
        {
            switch (type.kind)
            {
            case TypeKind::BitVector:
                if (type.bitWidth == 0 || type.elementCount != 0)
                {
                    throw std::invalid_argument("invalid AM bit-vector type");
                }
                break;
            case TypeKind::Array:
                if (type.bitWidth == 0 || type.elementCount == 0)
                {
                    throw std::invalid_argument("invalid AM array type");
                }
                break;
            case TypeKind::Real:
            case TypeKind::String:
                if (type.bitWidth != 0 || type.elementCount != 0 ||
                    type.signedness != Signedness::Unsigned)
                {
                    throw std::invalid_argument("invalid AM real or string type");
                }
                break;
            }
        }

        std::string_view storedString(const detail::ProgramStorage &storage, StringId id)
        {
            if (!id.valid() || static_cast<std::size_t>(id.value) + 1 >= storage.stringOffsets.size())
            {
                return {};
            }
            const uint32_t begin = storage.stringOffsets[id.value];
            const uint32_t end = storage.stringOffsets[id.value + 1];
            return std::string_view(storage.stringBytes.data() + begin, end - begin);
        }

        bool dpiAbiCompatible(const Type &type, DpiAbiKind abi) noexcept
        {
            switch (abi)
            {
            case DpiAbiKind::Integral:
                return type.kind == TypeKind::BitVector;
            case DpiAbiKind::Real64:
            case DpiAbiKind::Real32:
                return type.kind == TypeKind::Real;
            case DpiAbiKind::String:
                return type.kind == TypeKind::String;
            }
            return false;
        }

        template <typename Id, typename Container>
        void requireId(Id id, const Container &container, std::string_view what)
        {
            if (!id.valid() || static_cast<std::size_t>(id.value) >= container.size())
            {
                throw std::invalid_argument(std::string("invalid ") + std::string(what));
            }
        }

        TypeId appendType(detail::ProgramStorage &storage, const Type &type)
        {
            requireValidType(type);
            requireRoom(storage.types.size(), 1, "type table");
            ensureAppendCapacity(storage.types, 1);
            const TypeId id{static_cast<uint32_t>(storage.types.size())};
            storage.types.push_back(type);
            return id;
        }

        StringId appendString(detail::ProgramStorage &storage, std::string_view text)
        {
            std::string stableText;
            if (aliasesArena(std::span<const char>(text.data(), text.size()), storage.stringBytes))
            {
                stableText.assign(text);
                text = stableText;
            }
            requireOffsetTableRoom(storage.stringOffsets.size() - 1, 1, "string table");
            requireRoom(storage.stringBytes.size(), text.size(), "string byte arena");
            ensureAppendCapacity(storage.stringOffsets, 1);
            ensureAppendCapacity(storage.stringBytes, text.size());
            const StringId id{static_cast<uint32_t>(storage.stringOffsets.size() - 1)};
            storage.stringBytes.insert(storage.stringBytes.end(), text.begin(), text.end());
            storage.stringOffsets.push_back(static_cast<uint32_t>(storage.stringBytes.size()));
            return id;
        }

        VariableId appendVariable(detail::ProgramStorage &storage,
                                  TypeId type,
                                  InitId init,
                                  std::optional<StringId> label)
        {
            requireId(type, storage.types, "AM TypeId");
            requireId(init, storage.initDescriptors, "AM InitId");
            if (label)
            {
                if (!label->valid() || static_cast<std::size_t>(label->value) + 1 >= storage.stringOffsets.size())
                {
                    throw std::invalid_argument("invalid AM label StringId");
                }
                requireRoom(storage.variableLabels.size(), 1, "variable label table");
            }
            requireRoom(storage.variables.size(), 1, "variable table");
            ensureAppendCapacity(storage.variables, 1);
            if (label)
            {
                ensureAppendCapacity(storage.variableLabels, 1);
            }
            const VariableId id{static_cast<uint32_t>(storage.variables.size())};
            storage.variables.push_back(VariableRecord{.type = type, .init = init});
            if (label)
            {
                storage.variableLabels.push_back(VariableLabel{.variable = id, .label = *label});
            }
            return id;
        }

        InstructionId appendInstruction(detail::ProgramStorage &storage,
                                        Opcode opcode,
                                        std::span<const VariableId> results,
                                        std::span<const VariableId> operands)
        {
            std::vector<VariableId> stableResults;
            std::vector<VariableId> stableOperands;
            if (aliasesArena(results, storage.results) ||
                aliasesArena(results, storage.operands))
            {
                stableResults.assign(results.begin(), results.end());
                results = stableResults;
            }
            if (aliasesArena(operands, storage.operands) ||
                aliasesArena(operands, storage.results))
            {
                stableOperands.assign(operands.begin(), operands.end());
                operands = stableOperands;
            }
            for (VariableId variable : results)
            {
                requireId(variable, storage.variables, "AM instruction result VariableId");
            }
            for (VariableId variable : operands)
            {
                requireId(variable, storage.variables, "AM instruction operand VariableId");
            }
            requireOffsetTableRoom(storage.opcodes.size(), 1, "instruction table");
            requireRoom(storage.results.size(), results.size(), "instruction result arena");
            requireRoom(storage.operands.size(), operands.size(), "instruction operand arena");
            ensureAppendCapacity(storage.opcodes, 1);
            ensureAppendCapacity(storage.resultOffsets, 1);
            ensureAppendCapacity(storage.results, results.size());
            ensureAppendCapacity(storage.operandOffsets, 1);
            ensureAppendCapacity(storage.operands, operands.size());

            const InstructionId id{static_cast<uint32_t>(storage.opcodes.size())};
            storage.opcodes.push_back(opcode);
            storage.results.insert(storage.results.end(), results.begin(), results.end());
            storage.resultOffsets.push_back(static_cast<uint32_t>(storage.results.size()));
            storage.operands.insert(storage.operands.end(), operands.begin(), operands.end());
            storage.operandOffsets.push_back(static_cast<uint32_t>(storage.operands.size()));
            return id;
        }

        Opcode requireInstructionOpcode(const detail::ProgramStorage &storage,
                                        InstructionId instruction,
                                        Opcode expected)
        {
            requireId(instruction, storage.opcodes, "AM InstructionId");
            const Opcode actual = storage.opcodes[instruction.value];
            if (actual != expected)
            {
                throw std::invalid_argument("AM attribute does not match instruction opcode");
            }
            return actual;
        }

        template <typename Record>
        void appendAttributeRecord(std::vector<Record> &records,
                                   Record record,
                                   std::string_view attributeName)
        {
            if (!records.empty() && records.back().instruction.value >= record.instruction.value)
            {
                throw std::invalid_argument(std::string("AM ") + std::string(attributeName) +
                                            " attributes must be attached once in InstructionId order");
            }
            requireRoom(records.size(), 1, attributeName);
            ensureAppendCapacity(records, 1);
            records.push_back(std::move(record));
        }

        void appendSliceStaticAttributes(detail::ProgramStorage &storage,
                                         InstructionId instruction,
                                         uint32_t lsb)
        {
            requireInstructionOpcode(storage, instruction, Opcode::SliceStatic);
            appendAttributeRecord(storage.sliceStaticAttributes,
                                  detail::SliceStaticAttributeRecord{
                                      .instruction = instruction,
                                      .attributes = SliceStaticAttributes{.lsb = lsb},
                                  },
                                  "slice_static");
        }

        void appendSystemFunctionAttributes(detail::ProgramStorage &storage,
                                            InstructionId instruction,
                                            const SystemFunctionAttributes &attributes)
        {
            requireInstructionOpcode(storage, instruction, Opcode::SystemFunction);
            if (!attributes.name.valid() ||
                static_cast<std::size_t>(attributes.name.value) + 1 >= storage.stringOffsets.size() ||
                storage.stringOffsets[attributes.name.value] == storage.stringOffsets[attributes.name.value + 1])
            {
                throw std::invalid_argument("AM system function name must be a valid non-empty string");
            }
            appendAttributeRecord(storage.systemFunctionAttributes,
                                  detail::SystemFunctionAttributeRecord{
                                      .instruction = instruction,
                                      .attributes = attributes,
                                  },
                                  "system.function");
        }

        void appendSystemTaskAttributes(detail::ProgramStorage &storage,
                                        InstructionId instruction,
                                        const SystemTaskAttributes &attributes)
        {
            requireInstructionOpcode(storage, instruction, Opcode::SystemTask);
            if (!attributes.name.valid() ||
                static_cast<std::size_t>(attributes.name.value) + 1 >= storage.stringOffsets.size() ||
                storage.stringOffsets[attributes.name.value] == storage.stringOffsets[attributes.name.value + 1])
            {
                throw std::invalid_argument("AM system task name must be a valid non-empty string");
            }
            appendAttributeRecord(storage.systemTaskAttributes,
                                  detail::SystemTaskAttributeRecord{
                                      .instruction = instruction,
                                      .attributes = attributes,
                                  },
                                  "system.task");
        }

        void appendDpiCallAttributes(detail::ProgramStorage &storage,
                                     InstructionId instruction,
                                     const DpiCallAttributes &attributes)
        {
            requireInstructionOpcode(storage, instruction, Opcode::DpiCall);
            if (!attributes.importSymbol.valid() ||
                static_cast<std::size_t>(attributes.importSymbol.value) + 1 >= storage.stringOffsets.size() ||
                storage.stringOffsets[attributes.importSymbol.value] ==
                    storage.stringOffsets[attributes.importSymbol.value + 1])
            {
                throw std::invalid_argument("AM DPI import symbol must be a valid non-empty string");
            }
            appendAttributeRecord(storage.dpiCallAttributes,
                                  detail::DpiCallAttributeRecord{
                                      .instruction = instruction,
                                      .attributes = attributes,
                                  },
                                  "dpi.call");
        }

        void requireAllRequiredAttributes(const detail::ProgramStorage &storage)
        {
            std::size_t sliceStaticIndex = 0;
            std::size_t systemFunctionIndex = 0;
            std::size_t systemTaskIndex = 0;
            std::size_t dpiCallIndex = 0;
            std::size_t activationIndex = 0;
            const auto consumeRecord = [](const auto &records,
                                          std::size_t &recordIndex,
                                          InstructionId instruction) {
                if (recordIndex >= records.size() ||
                    records[recordIndex].instruction != instruction)
                {
                    return false;
                }
                ++recordIndex;
                return true;
            };
            for (uint32_t index = 0; index < storage.opcodes.size(); ++index)
            {
                const InstructionId instruction{index};
                bool present = true;
                switch (storage.opcodes[index])
                {
                case Opcode::SliceStatic:
                    present = consumeRecord(storage.sliceStaticAttributes,
                                            sliceStaticIndex,
                                            instruction);
                    break;
                case Opcode::SystemFunction:
                    present = consumeRecord(storage.systemFunctionAttributes,
                                            systemFunctionIndex,
                                            instruction);
                    break;
                case Opcode::SystemTask:
                    present = consumeRecord(storage.systemTaskAttributes,
                                            systemTaskIndex,
                                            instruction);
                    break;
                case Opcode::DpiCall:
                    present = consumeRecord(storage.dpiCallAttributes,
                                            dpiCallIndex,
                                            instruction);
                    break;
                case Opcode::ActForward:
                case Opcode::ActBackward:
                    present = consumeRecord(storage.activationAttributes,
                                            activationIndex,
                                            instruction);
                    break;
                default:
                    break;
                }
                if (!present)
                {
                    throw std::logic_error("AM instruction is missing required typed attributes: instruction=" +
                                           std::to_string(index) + " opcode=" +
                                           std::string(toString(storage.opcodes[index])));
                }
            }
        }
    } // namespace

    LinearProgramBuilder::LinearProgramBuilder()
        : storage_(std::make_unique<detail::ProgramStorage>())
    {
    }

    LinearProgramBuilder::~LinearProgramBuilder() = default;
    LinearProgramBuilder::LinearProgramBuilder(LinearProgramBuilder &&) noexcept = default;
    LinearProgramBuilder &LinearProgramBuilder::operator=(LinearProgramBuilder &&) noexcept = default;

    void LinearProgramBuilder::reserve(const ProgramReserve &reserve)
    {
        auto &storage = requireStorage(storage_, finished_);
        requireRoom(0, reserve.types, "type table reserve");
        requireOffsetTableRoom(0, reserve.strings, "string table reserve");
        requireRoom(0, reserve.stringBytes, "string byte arena reserve");
        requireRoom(0, reserve.initDescriptors, "init descriptor table reserve");
        requireRoom(0, reserve.initActions, "init action arena reserve");
        requireRoom(0, reserve.literals, "literal table reserve");
        requireRoom(0, reserve.literalWords, "literal word arena reserve");
        requireRoom(0, reserve.literalBytes, "literal byte arena reserve");
        requireRoom(0, reserve.variables, "variable table reserve");
        requireRoom(0, reserve.variableLabels, "variable label table reserve");
        requireOffsetTableRoom(0, reserve.instructions, "instruction table reserve");
        requireRoom(0, reserve.operands, "instruction operand arena reserve");
        requireRoom(0, reserve.results, "instruction result arena reserve");
        requireRoom(0, reserve.sliceStaticAttributes, "slice attribute table reserve");
        requireRoom(0, reserve.systemFunctionAttributes, "system function attribute table reserve");
        requireRoom(0, reserve.systemTaskAttributes, "system task attribute table reserve");
        requireRoom(0, reserve.dpiCallAttributes, "DPI call attribute table reserve");
        requireRoom(0, reserve.dpiImports, "DPI import table reserve");
        requireRoom(0, reserve.dpiParameters, "DPI parameter arena reserve");
        storage.types.reserve(reserve.types);
        storage.stringOffsets.reserve(reserve.strings + 1);
        storage.stringBytes.reserve(reserve.stringBytes);
        storage.initDescriptors.reserve(std::max<std::size_t>(reserve.initDescriptors, 2));
        storage.initActions.reserve(reserve.initActions);
        storage.literals.reserve(reserve.literals);
        storage.literalWords.reserve(reserve.literalWords);
        storage.literalBytes.reserve(reserve.literalBytes);
        storage.variables.reserve(reserve.variables);
        storage.variableLabels.reserve(reserve.variableLabels);
        storage.opcodes.reserve(reserve.instructions);
        storage.operandOffsets.reserve(reserve.instructions + 1);
        storage.operands.reserve(reserve.operands);
        storage.resultOffsets.reserve(reserve.instructions + 1);
        storage.results.reserve(reserve.results);
        storage.sliceStaticAttributes.reserve(reserve.sliceStaticAttributes);
        storage.systemFunctionAttributes.reserve(reserve.systemFunctionAttributes);
        storage.systemTaskAttributes.reserve(reserve.systemTaskAttributes);
        storage.dpiCallAttributes.reserve(reserve.dpiCallAttributes);
        storage.dpiImports.reserve(reserve.dpiImports);
        storage.dpiParameters.reserve(reserve.dpiParameters);
    }

    TypeId LinearProgramBuilder::addType(const Type &type)
    {
        return appendType(requireStorage(storage_, finished_), type);
    }

    StringId LinearProgramBuilder::addString(std::string_view text)
    {
        return appendString(requireStorage(storage_, finished_), text);
    }

    InitId LinearProgramBuilder::undefInit() const noexcept
    {
        return InitId{0};
    }

    InitId LinearProgramBuilder::zeroInit() const noexcept
    {
        return InitId{1};
    }

    LiteralId LinearProgramBuilder::addBitLiteral(TypeId type, std::span<const uint64_t> words)
    {
        auto &storage = requireStorage(storage_, finished_);
        std::vector<uint64_t> stableWords;
        if (aliasesArena(words, storage.literalWords))
        {
            stableWords.assign(words.begin(), words.end());
            words = stableWords;
        }
        requireId(type, storage.types, "AM literal TypeId");
        const Type &literalType = storage.types[type.value];
        uint64_t expectedWords = 0;
        if (literalType.kind == TypeKind::BitVector)
        {
            expectedWords = (static_cast<uint64_t>(literalType.bitWidth) + 63) / 64;
        }
        else if (literalType.kind == TypeKind::Real)
        {
            expectedWords = 1;
        }
        else
        {
            throw std::invalid_argument("AM word literal requires a bit-vector or real type");
        }
        if (words.size() != expectedWords)
        {
            throw std::invalid_argument("AM word literal payload has the wrong size");
        }
        if (literalType.kind == TypeKind::BitVector && literalType.bitWidth % 64 != 0 && !words.empty())
        {
            const uint32_t usedBits = literalType.bitWidth % 64;
            if ((words.back() >> usedBits) != 0)
            {
                throw std::invalid_argument("AM bit-vector literal has nonzero bits above its width");
            }
        }
        requireRoom(storage.literals.size(), 1, "literal table");
        requireRoom(storage.literalWords.size(), words.size(), "literal word arena");
        ensureAppendCapacity(storage.literals, 1);
        ensureAppendCapacity(storage.literalWords, words.size());
        const LiteralId id{static_cast<uint32_t>(storage.literals.size())};
        const Range32 wordRange{
            .offset = static_cast<uint32_t>(storage.literalWords.size()),
            .count = static_cast<uint32_t>(words.size()),
        };
        storage.literalWords.insert(storage.literalWords.end(), words.begin(), words.end());
        storage.literals.push_back(detail::LiteralRecord{
            .type = type,
            .words = wordRange,
            .bytes = {},
        });
        return id;
    }

    LiteralId LinearProgramBuilder::addStringLiteral(TypeId type, std::string_view bytes)
    {
        auto &storage = requireStorage(storage_, finished_);
        std::string stableBytes;
        if (aliasesArena(std::span<const char>(bytes.data(), bytes.size()), storage.literalBytes))
        {
            stableBytes.assign(bytes);
            bytes = stableBytes;
        }
        requireId(type, storage.types, "AM string literal TypeId");
        if (storage.types[type.value].kind != TypeKind::String)
        {
            throw std::invalid_argument("AM string literal requires a string type");
        }
        requireRoom(storage.literals.size(), 1, "literal table");
        requireRoom(storage.literalBytes.size(), bytes.size(), "literal byte arena");
        ensureAppendCapacity(storage.literals, 1);
        ensureAppendCapacity(storage.literalBytes, bytes.size());
        const LiteralId id{static_cast<uint32_t>(storage.literals.size())};
        const Range32 byteRange{
            .offset = static_cast<uint32_t>(storage.literalBytes.size()),
            .count = static_cast<uint32_t>(bytes.size()),
        };
        storage.literalBytes.insert(storage.literalBytes.end(), bytes.begin(), bytes.end());
        storage.literals.push_back(detail::LiteralRecord{
            .type = type,
            .words = {},
            .bytes = byteRange,
        });
        return id;
    }

    InitId LinearProgramBuilder::addConstantInit(LiteralId literal)
    {
        auto &storage = requireStorage(storage_, finished_);
        requireId(literal, storage.literals, "AM LiteralId");
        requireRoom(storage.initDescriptors.size(), 1, "init descriptor table");
        ensureAppendCapacity(storage.initDescriptors, 1);
        const InitId id{static_cast<uint32_t>(storage.initDescriptors.size())};
        storage.initDescriptors.push_back(InitDescriptor{
            .kind = InitKind::Constant,
            .payload = literal.value,
            .count = 0,
        });
        return id;
    }

    InitId LinearProgramBuilder::addActionsInit(std::span<const InitAction> actions)
    {
        auto &storage = requireStorage(storage_, finished_);
        std::vector<InitAction> stableActions;
        if (aliasesArena(actions, storage.initActions))
        {
            stableActions.assign(actions.begin(), actions.end());
            actions = stableActions;
        }
        for (const InitAction &action : actions)
        {
            if (action.kind == InitActionKind::Load)
            {
                if (!action.path.valid() ||
                    static_cast<std::size_t>(action.path.value) + 1 >= storage.stringOffsets.size() ||
                    storage.stringOffsets[action.path.value] == storage.stringOffsets[action.path.value + 1])
                {
                    throw std::invalid_argument("AM load init action requires a valid non-empty path");
                }
                if (action.rangeKind == InitRangeKind::Span && action.count == 0)
                {
                    throw std::invalid_argument("AM load span count must be positive");
                }
            }
            else
            {
                if (action.kind == InitActionKind::Fill && action.count == 0)
                {
                    throw std::invalid_argument("AM fill init action count must be positive");
                }
                if (action.expression.kind == InitExprKind::Literal)
                {
                    requireId(action.expression.literal, storage.literals, "AM init literal");
                }
            }
        }
        requireRoom(storage.initDescriptors.size(), 1, "init descriptor table");
        requireRoom(storage.initActions.size(), actions.size(), "init action arena");
        ensureAppendCapacity(storage.initDescriptors, 1);
        ensureAppendCapacity(storage.initActions, actions.size());
        const InitId id{static_cast<uint32_t>(storage.initDescriptors.size())};
        const uint32_t offset = static_cast<uint32_t>(storage.initActions.size());
        storage.initActions.insert(storage.initActions.end(), actions.begin(), actions.end());
        storage.initDescriptors.push_back(InitDescriptor{
            .kind = InitKind::Actions,
            .payload = offset,
            .count = static_cast<uint32_t>(actions.size()),
        });
        return id;
    }

    VariableId LinearProgramBuilder::addVariable(TypeId type,
                                                 InitId init,
                                                 std::optional<StringId> label)
    {
        return appendVariable(requireStorage(storage_, finished_), type, init, label);
    }

    InstructionId LinearProgramBuilder::addInstruction(Opcode opcode,
                                                       std::span<const VariableId> results,
                                                       std::span<const VariableId> operands)
    {
        if (opcode == Opcode::ActForward || opcode == Opcode::ActBackward)
        {
            throw std::invalid_argument("LinearProgram cannot contain block activation instructions");
        }
        return appendInstruction(requireStorage(storage_, finished_), opcode, results, operands);
    }

    void LinearProgramBuilder::setSliceStaticAttributes(InstructionId instruction, uint32_t lsb)
    {
        appendSliceStaticAttributes(requireStorage(storage_, finished_), instruction, lsb);
    }

    void LinearProgramBuilder::setSystemFunctionAttributes(
        InstructionId instruction,
        const SystemFunctionAttributes &attributes)
    {
        appendSystemFunctionAttributes(requireStorage(storage_, finished_), instruction, attributes);
    }

    void LinearProgramBuilder::setSystemTaskAttributes(
        InstructionId instruction,
        const SystemTaskAttributes &attributes)
    {
        appendSystemTaskAttributes(requireStorage(storage_, finished_), instruction, attributes);
    }

    void LinearProgramBuilder::setDpiCallAttributes(
        InstructionId instruction,
        const DpiCallAttributes &attributes)
    {
        appendDpiCallAttributes(requireStorage(storage_, finished_), instruction, attributes);
    }

    DpiImportId LinearProgramBuilder::addDpiImport(StringId symbol,
                                                   std::span<const DpiParameter> parameters,
                                                   DpiReturn returnValue)
    {
        auto &storage = requireStorage(storage_, finished_);
        std::vector<DpiParameter> stableParameters;
        if (aliasesArena(parameters, storage.dpiParameters))
        {
            stableParameters.assign(parameters.begin(), parameters.end());
            parameters = stableParameters;
        }
        if (!symbol.valid() || static_cast<std::size_t>(symbol.value) + 1 >= storage.stringOffsets.size() ||
            storage.stringOffsets[symbol.value] == storage.stringOffsets[symbol.value + 1])
        {
            throw std::invalid_argument("AM DPI import requires a valid non-empty symbol");
        }
        for (const auto &record : storage.dpiImports)
        {
            if (storedString(storage, record.symbol) == storedString(storage, symbol))
            {
                throw std::invalid_argument("AM DPI import symbol must be unique");
            }
        }
        std::unordered_set<std::string_view> parameterNames;
        parameterNames.reserve(parameters.size());
        for (const DpiParameter &parameter : parameters)
        {
            requireId(parameter.name, storage.stringOffsets, "AM DPI parameter name");
            if (static_cast<std::size_t>(parameter.name.value) + 1 >= storage.stringOffsets.size() ||
                storage.stringOffsets[parameter.name.value] == storage.stringOffsets[parameter.name.value + 1])
            {
                throw std::invalid_argument("AM DPI parameter name must be non-empty");
            }
            requireId(parameter.type, storage.types, "AM DPI parameter type");
            if (!parameterNames.insert(storedString(storage, parameter.name)).second)
            {
                throw std::invalid_argument("AM DPI parameter names must be unique");
            }
            if (!dpiAbiCompatible(storage.types[parameter.type.value], parameter.abi))
            {
                throw std::invalid_argument("AM DPI parameter Type and AbiKind are incompatible");
            }
        }
        if (returnValue.present)
        {
            requireId(returnValue.type, storage.types, "AM DPI return type");
            if (!dpiAbiCompatible(storage.types[returnValue.type.value], returnValue.abi))
            {
                throw std::invalid_argument("AM DPI return Type and AbiKind are incompatible");
            }
        }
        requireRoom(storage.dpiImports.size(), 1, "DPI import table");
        requireRoom(storage.dpiParameters.size(), parameters.size(), "DPI parameter arena");
        ensureAppendCapacity(storage.dpiImports, 1);
        ensureAppendCapacity(storage.dpiParameters, parameters.size());
        const DpiImportId id{static_cast<uint32_t>(storage.dpiImports.size())};
        const Range32 range{
            .offset = static_cast<uint32_t>(storage.dpiParameters.size()),
            .count = static_cast<uint32_t>(parameters.size()),
        };
        storage.dpiParameters.insert(storage.dpiParameters.end(), parameters.begin(), parameters.end());
        storage.dpiImports.push_back(detail::DpiImportRecord{
            .symbol = symbol,
            .parameters = range,
            .returnValue = returnValue,
        });
        return id;
    }

    ProgramView LinearProgramBuilder::view() const noexcept
    {
        return ProgramView(viewStorage(storage_, finished_));
    }

    LinearProgram LinearProgramBuilder::finish()
    {
        auto &storage = requireStorage(storage_, finished_);
        requireAllRequiredAttributes(storage);
        if (!storage.blockOffsets.empty() || !storage.blockInstructions.empty())
        {
            throw std::logic_error("LinearProgram cannot contain a block layout");
        }
        finished_ = true;
        return LinearProgram(std::move(storage_));
    }

    ScheduledProgramBuilder::ScheduledProgramBuilder(LinearProgram &&linearProgram)
        : storage_(std::move(linearProgram.storage_))
    {
        if (!storage_)
        {
            throw std::invalid_argument("ScheduledProgramBuilder requires a valid LinearProgram");
        }
        storage_->blockOffsets.clear();
        storage_->blockInstructions.clear();
        storage_->blockAtomOffsets.clear();
        storage_->atomInstructionOffsets.clear();
        storage_->atomKinds.clear();
        storage_->atomSignatures.clear();
        storage_->blockOffsets.push_back(0);
        storage_->blockAtomOffsets.push_back(0);
        storage_->atomInstructionOffsets.push_back(0);
    }

    ScheduledProgramBuilder::~ScheduledProgramBuilder() = default;
    ScheduledProgramBuilder::ScheduledProgramBuilder(ScheduledProgramBuilder &&) noexcept = default;
    ScheduledProgramBuilder &ScheduledProgramBuilder::operator=(ScheduledProgramBuilder &&) noexcept = default;

    void ScheduledProgramBuilder::reserve(const ScheduledProgramReserve &reserve)
    {
        auto &storage = requireStorage(storage_, finished_);
        requireRoom(storage.types.size(), reserve.additionalTypes, "type table");
        requireOffsetTableRoom(storage.stringOffsets.size() - 1,
                               reserve.additionalStrings,
                               "string table");
        requireRoom(storage.stringBytes.size(), reserve.additionalStringBytes, "string byte arena");
        requireRoom(storage.variables.size(), reserve.additionalVariables, "variable table");
        requireRoom(storage.variableLabels.size(), reserve.additionalVariableLabels, "variable label table");
        requireOffsetTableRoom(storage.opcodes.size(),
                               reserve.additionalInstructions,
                               "instruction table");
        requireRoom(storage.operands.size(), reserve.additionalOperands, "instruction operand arena");
        requireRoom(storage.results.size(), reserve.additionalResults, "instruction result arena");
        requireRoom(storage.sliceStaticAttributes.size(), reserve.additionalSliceStaticAttributes,
                    "slice attribute table");
        requireRoom(storage.systemFunctionAttributes.size(), reserve.additionalSystemFunctionAttributes,
                    "system function attribute table");
        requireRoom(storage.systemTaskAttributes.size(), reserve.additionalSystemTaskAttributes,
                    "system task attribute table");
        requireRoom(storage.dpiCallAttributes.size(), reserve.additionalDpiCallAttributes,
                    "DPI call attribute table");
        requireOffsetTableRoom(storage.blockOffsets.size() - 1, reserve.blocks, "block table");
        requireRoom(layoutInstructionCount_, reserve.blockInstructionIds, "block instruction arena");
        requireRoom(storage.activationAttributes.size(), reserve.activationInstructions,
                    "activation attribute table");
        requireRoom(storage.activationTargets.size(), reserve.activationTargets,
                    "activation target arena");

        storage.types.reserve(storage.types.size() + reserve.additionalTypes);
        storage.stringOffsets.reserve(storage.stringOffsets.size() + reserve.additionalStrings);
        storage.stringBytes.reserve(storage.stringBytes.size() + reserve.additionalStringBytes);
        storage.variables.reserve(storage.variables.size() + reserve.additionalVariables);
        storage.variableLabels.reserve(storage.variableLabels.size() + reserve.additionalVariableLabels);
        storage.opcodes.reserve(storage.opcodes.size() + reserve.additionalInstructions);
        storage.operandOffsets.reserve(storage.operandOffsets.size() + reserve.additionalInstructions);
        storage.operands.reserve(storage.operands.size() + reserve.additionalOperands);
        storage.resultOffsets.reserve(storage.resultOffsets.size() + reserve.additionalInstructions);
        storage.results.reserve(storage.results.size() + reserve.additionalResults);
        storage.sliceStaticAttributes.reserve(storage.sliceStaticAttributes.size() +
                                              reserve.additionalSliceStaticAttributes);
        storage.systemFunctionAttributes.reserve(storage.systemFunctionAttributes.size() +
                                                 reserve.additionalSystemFunctionAttributes);
        storage.systemTaskAttributes.reserve(storage.systemTaskAttributes.size() +
                                             reserve.additionalSystemTaskAttributes);
        storage.dpiCallAttributes.reserve(storage.dpiCallAttributes.size() +
                                          reserve.additionalDpiCallAttributes);
        storage.blockOffsets.reserve(storage.blockOffsets.size() + reserve.blocks);
        storage.blockAtomOffsets.reserve(storage.blockAtomOffsets.size() + reserve.blocks);
        storage.atomInstructionOffsets.reserve(storage.atomInstructionOffsets.size() +
                                               reserve.blockInstructionIds);
        storage.atomKinds.reserve(storage.atomKinds.size() + reserve.blockInstructionIds);
        storage.atomSignatures.reserve(storage.atomSignatures.size() +
                                       reserve.blockInstructionIds);
        blockInstructionReserve_ =
            std::max(blockInstructionReserve_,
                     static_cast<std::size_t>(layoutInstructionCount_) +
                         reserve.blockInstructionIds);
        if (!blockLayoutIdentity_)
        {
            storage.blockInstructions.reserve(blockInstructionReserve_);
        }
        storage.activationAttributes.reserve(storage.activationAttributes.size() +
                                             reserve.activationInstructions);
        storage.activationTargets.reserve(storage.activationTargets.size() + reserve.activationTargets);
    }

    TypeId ScheduledProgramBuilder::addType(const Type &type)
    {
        return appendType(requireStorage(storage_, finished_), type);
    }

    StringId ScheduledProgramBuilder::addString(std::string_view text)
    {
        return appendString(requireStorage(storage_, finished_), text);
    }

    InitId ScheduledProgramBuilder::undefInit() const noexcept
    {
        return InitId{0};
    }

    InitId ScheduledProgramBuilder::zeroInit() const noexcept
    {
        return InitId{1};
    }

    VariableId ScheduledProgramBuilder::addVariable(TypeId type,
                                                    InitId init,
                                                    std::optional<StringId> label)
    {
        return appendVariable(requireStorage(storage_, finished_), type, init, label);
    }

    InstructionId ScheduledProgramBuilder::addInstruction(Opcode opcode,
                                                          std::span<const VariableId> results,
                                                          std::span<const VariableId> operands)
    {
        return appendInstruction(requireStorage(storage_, finished_), opcode, results, operands);
    }

    void ScheduledProgramBuilder::setInstructionOperand(InstructionId instruction,
                                                        std::size_t position,
                                                        VariableId operand)
    {
        auto &storage = requireStorage(storage_, finished_);
        requireId(instruction, storage.opcodes, "AM InstructionId");
        requireId(operand, storage.variables, "AM VariableId");
        const std::size_t instructionIndex = instruction.value;
        const uint32_t begin = storage.operandOffsets[instructionIndex];
        const uint32_t end = storage.operandOffsets[instructionIndex + 1];
        if (position >= end - begin)
        {
            throw std::out_of_range("AM instruction operand position is out of range");
        }
        storage.operands[begin + position] = operand;
    }

    void ScheduledProgramBuilder::setSliceStaticAttributes(InstructionId instruction, uint32_t lsb)
    {
        appendSliceStaticAttributes(requireStorage(storage_, finished_), instruction, lsb);
    }

    void ScheduledProgramBuilder::setSystemFunctionAttributes(
        InstructionId instruction,
        const SystemFunctionAttributes &attributes)
    {
        appendSystemFunctionAttributes(requireStorage(storage_, finished_), instruction, attributes);
    }

    void ScheduledProgramBuilder::setSystemTaskAttributes(
        InstructionId instruction,
        const SystemTaskAttributes &attributes)
    {
        appendSystemTaskAttributes(requireStorage(storage_, finished_), instruction, attributes);
    }

    void ScheduledProgramBuilder::setDpiCallAttributes(
        InstructionId instruction,
        const DpiCallAttributes &attributes)
    {
        appendDpiCallAttributes(requireStorage(storage_, finished_), instruction, attributes);
    }

    void ScheduledProgramBuilder::setActivationTargets(InstructionId instruction,
                                                       std::span<const BlockId> targets)
    {
        auto &storage = requireStorage(storage_, finished_);
        std::vector<BlockId> stableTargets;
        if (aliasesArena(targets, storage.activationTargets))
        {
            stableTargets.assign(targets.begin(), targets.end());
            targets = stableTargets;
        }
        requireId(instruction, storage.opcodes, "AM activation InstructionId");
        const Opcode opcode = storage.opcodes[instruction.value];
        if (opcode != Opcode::ActForward && opcode != Opcode::ActBackward)
        {
            throw std::invalid_argument("AM activation targets require act.f or act.b");
        }
        if (targets.empty())
        {
            throw std::invalid_argument("AM activation targets must not be empty");
        }
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            if (!targets[index].valid() ||
                (index != 0 && targets[index - 1].value >= targets[index].value))
            {
                throw std::invalid_argument("AM activation targets must be valid, unique, and sorted");
            }
        }
        if (!storage.activationAttributes.empty() &&
            storage.activationAttributes.back().instruction.value >= instruction.value)
        {
            throw std::invalid_argument(
                "AM activation attributes must be attached once in InstructionId order");
        }
        requireRoom(storage.activationAttributes.size(), 1, "activation attribute table");
        requireRoom(storage.activationTargets.size(), targets.size(), "activation target arena");
        ensureAppendCapacity(storage.activationAttributes, 1);
        ensureAppendCapacity(storage.activationTargets, targets.size());
        const Range32 range{
            .offset = static_cast<uint32_t>(storage.activationTargets.size()),
            .count = static_cast<uint32_t>(targets.size()),
        };
        storage.activationTargets.insert(storage.activationTargets.end(), targets.begin(), targets.end());
        appendAttributeRecord(storage.activationAttributes,
                              detail::ActivationAttributeRecord{
                                  .instruction = instruction,
                                  .targets = range,
                              },
                              "activation");
    }

    void ScheduledProgramBuilder::beginBlock()
    {
        (void)requireStorage(storage_, finished_);
        if (blockOpen_)
        {
            throw std::logic_error("AM block construction is already open");
        }
        blockOpen_ = true;
    }

    void ScheduledProgramBuilder::appendBlockInstruction(InstructionId instruction)
    {
        auto &storage = requireStorage(storage_, finished_);
        if (!blockOpen_)
        {
            throw std::logic_error("AM block instruction appended without beginBlock");
        }
        requireId(instruction, storage.opcodes, "AM block InstructionId");
        requireRoom(layoutInstructionCount_, 1, "block instruction arena");

        // Implicit Singleton atom: instruction appended outside any explicit
        // beginAtom/endAtom pair.
        const bool implicitAtom = !atomOpen_;
        if (implicitAtom)
        {
            beginAtom(AmAtomKind::Singleton, 0);
        }
        if (blockLayoutIdentity_ && instruction.value != layoutInstructionCount_)
        {
            const std::size_t reserveCount =
                std::max<std::size_t>(blockInstructionReserve_,
                                      static_cast<std::size_t>(layoutInstructionCount_) + 1);
            storage.blockInstructions.reserve(reserveCount);
            for (uint32_t index = 0; index < layoutInstructionCount_; ++index)
            {
                storage.blockInstructions.push_back(InstructionId{index});
            }
            blockLayoutIdentity_ = false;
        }
        if (!blockLayoutIdentity_)
        {
            ensureAppendCapacity(storage.blockInstructions, 1);
            storage.blockInstructions.push_back(instruction);
        }
        ++layoutInstructionCount_;
        if (implicitAtom)
        {
            endAtom();
        }
    }

    void ScheduledProgramBuilder::endBlock()
    {
        auto &storage = requireStorage(storage_, finished_);
        if (!blockOpen_)
        {
            throw std::logic_error("AM block construction is not open");
        }
        if (atomOpen_)
        {
            throw std::logic_error("AM block closed with an unfinished atom");
        }
        requireOffsetTableRoom(storage.blockOffsets.size() - 1, 1, "block table");
        ensureAppendCapacity(storage.blockOffsets, 1);
        storage.blockOffsets.push_back(layoutInstructionCount_);
        requireOffsetTableRoom(storage.blockAtomOffsets.size() - 1, 1, "block atom table");
        ensureAppendCapacity(storage.blockAtomOffsets, 1);
        storage.blockAtomOffsets.push_back(static_cast<uint32_t>(storage.atomKinds.size()));
        blockOpen_ = false;
    }

    void ScheduledProgramBuilder::beginAtom(AmAtomKind kind, uint32_t signature)
    {
        auto &storage = requireStorage(storage_, finished_);
        if (!blockOpen_)
        {
            throw std::logic_error("AM atom construction requires an open block");
        }
        if (atomOpen_)
        {
            throw std::logic_error("AM atom construction is already open");
        }
        if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(AmAtomKind::CommitEvent))
        {
            throw std::invalid_argument("invalid AM atom kind");
        }
        ensureAppendCapacity(storage.atomKinds, 1);
        ensureAppendCapacity(storage.atomSignatures, 1);
        storage.atomKinds.push_back(static_cast<uint8_t>(kind));
        storage.atomSignatures.push_back(signature);
        atomStart_ = layoutInstructionCount_;
        atomOpen_ = true;
    }

    void ScheduledProgramBuilder::endAtom()
    {
        auto &storage = requireStorage(storage_, finished_);
        if (!atomOpen_)
        {
            throw std::logic_error("AM atom construction is not open");
        }
        if (layoutInstructionCount_ == atomStart_)
        {
            throw std::logic_error("AM atom must contain at least one instruction");
        }
        requireOffsetTableRoom(storage.atomInstructionOffsets.size() - 1, 1,
                               "atom instruction table");
        ensureAppendCapacity(storage.atomInstructionOffsets, 1);
        storage.atomInstructionOffsets.push_back(layoutInstructionCount_);
        atomOpen_ = false;
    }

    void ScheduledProgramBuilder::addBlock(std::span<const InstructionId> instructions)
    {
        auto &storage = requireStorage(storage_, finished_);
        if (blockOpen_)
        {
            throw std::logic_error("AM block construction is already open");
        }
        for (InstructionId instruction : instructions)
        {
            requireId(instruction, storage.opcodes, "AM block InstructionId");
        }

        const bool originalIdentity = blockLayoutIdentity_;
        const uint32_t originalInstructionCount = layoutInstructionCount_;
        const std::size_t originalExplicitSize = storage.blockInstructions.size();
        const std::size_t originalAtomCount = storage.atomKinds.size();
        const std::size_t originalAtomOffsetSize = storage.atomInstructionOffsets.size();
        const std::size_t originalBlockAtomOffsetSize = storage.blockAtomOffsets.size();
        beginBlock();
        try
        {
            for (InstructionId instruction : instructions)
            {
                appendBlockInstruction(instruction);
            }
            endBlock();
        }
        catch (...)
        {
            storage.blockInstructions.resize(originalExplicitSize);
            storage.atomKinds.resize(originalAtomCount);
            storage.atomSignatures.resize(originalAtomCount);
            storage.atomInstructionOffsets.resize(originalAtomOffsetSize);
            storage.blockAtomOffsets.resize(originalBlockAtomOffsetSize);
            blockLayoutIdentity_ = originalIdentity;
            layoutInstructionCount_ = originalInstructionCount;
            blockOpen_ = false;
            atomOpen_ = false;
            throw;
        }
    }

    ProgramView ScheduledProgramBuilder::view() const noexcept
    {
        return ProgramView(viewStorage(storage_, finished_));
    }

    std::size_t ScheduledProgramBuilder::pendingBlockCount() const noexcept
    {
        return storage_ && !finished_ && !storage_->blockOffsets.empty() ? storage_->blockOffsets.size() - 1 : 0;
    }

    ScheduledProgram ScheduledProgramBuilder::finish()
    {
        auto &storage = requireStorage(storage_, finished_);
        requireAllRequiredAttributes(storage);
        if (blockOpen_)
        {
            throw std::logic_error("ScheduledProgram has an unfinished Block");
        }
        if (atomOpen_)
        {
            throw std::logic_error("ScheduledProgram has an unfinished atom");
        }
        if (storage.blockOffsets.size() < 2)
        {
            throw std::logic_error("ScheduledProgram requires an EntryBlock");
        }
        if (layoutInstructionCount_ != storage.opcodes.size())
        {
            throw std::logic_error("ScheduledProgram block layout must contain every instruction exactly once");
        }

        if (!blockLayoutIdentity_)
        {
            if (storage.blockInstructions.size() != storage.opcodes.size())
            {
                throw std::logic_error("ScheduledProgram explicit block layout has the wrong size");
            }
            std::vector<uint64_t> seen((storage.opcodes.size() + 63) / 64, 0);
            for (InstructionId instruction : storage.blockInstructions)
            {
                requireId(instruction, storage.opcodes, "AM block InstructionId");
                const uint64_t mask = UINT64_C(1) << (instruction.value % 64);
                uint64_t &word = seen[instruction.value / 64];
                if ((word & mask) != 0)
                {
                    throw std::logic_error("ScheduledProgram block layout contains a duplicate instruction");
                }
                word |= mask;
            }
        }
        finished_ = true;
        return ScheduledProgram(std::move(storage_));
    }

} // namespace wolvrix::lib::grhsim::am
