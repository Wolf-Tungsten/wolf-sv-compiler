#ifndef WOLVRIX_GRHSIM_AM_PROGRAM_HPP
#define WOLVRIX_GRHSIM_AM_PROGRAM_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace wolvrix::lib::grhsim::am
{

    template <typename Tag>
    struct DenseId
    {
        static constexpr uint32_t kInvalidValue = std::numeric_limits<uint32_t>::max();

        uint32_t value = kInvalidValue;

        constexpr bool valid() const noexcept { return value != kInvalidValue; }
        static constexpr DenseId invalid() noexcept { return {}; }
        explicit constexpr operator bool() const noexcept { return valid(); }
        friend constexpr auto operator<=>(DenseId, DenseId) = default;
    };

    struct VariableIdTag;
    struct InstructionIdTag;
    struct BlockIdTag;
    struct TypeIdTag;
    struct StringIdTag;
    struct InitIdTag;
    struct LiteralIdTag;
    struct DpiImportIdTag;

    using VariableId = DenseId<VariableIdTag>;
    using InstructionId = DenseId<InstructionIdTag>;
    using BlockId = DenseId<BlockIdTag>;
    using TypeId = DenseId<TypeIdTag>;
    using StringId = DenseId<StringIdTag>;
    using InitId = DenseId<InitIdTag>;
    using LiteralId = DenseId<LiteralIdTag>;
    using DpiImportId = DenseId<DpiImportIdTag>;

    static_assert(sizeof(VariableId) == sizeof(uint32_t));
    static_assert(sizeof(InstructionId) == sizeof(uint32_t));
    static_assert(sizeof(BlockId) == sizeof(uint32_t));
    static_assert(sizeof(TypeId) == sizeof(uint32_t));
    static_assert(sizeof(StringId) == sizeof(uint32_t));
    static_assert(sizeof(InitId) == sizeof(uint32_t));
    static_assert(sizeof(LiteralId) == sizeof(uint32_t));
    static_assert(sizeof(DpiImportId) == sizeof(uint32_t));

    struct Range32
    {
        uint32_t offset = 0;
        uint32_t count = 0;

        friend constexpr auto operator<=>(const Range32 &, const Range32 &) = default;
    };

    static_assert(sizeof(Range32) == 2 * sizeof(uint32_t));

    enum class Signedness : uint8_t
    {
        Unsigned = 0,
        Signed = 1,
    };

    enum class TypeKind : uint8_t
    {
        BitVector = 0,
        Real = 1,
        String = 2,
        Array = 3,
    };

    struct Type
    {
        TypeKind kind = TypeKind::BitVector;
        Signedness signedness = Signedness::Unsigned;
        uint16_t reserved = 0;
        uint32_t bitWidth = 1;
        uint32_t elementCount = 0;

        static Type bitVector(uint32_t width, Signedness sign = Signedness::Unsigned);
        static Type real();
        static Type string();
        static Type array(uint32_t elements,
                          uint32_t elementWidth,
                          Signedness sign = Signedness::Unsigned);

        friend constexpr auto operator<=>(const Type &, const Type &) = default;
    };

    enum class InitKind : uint8_t
    {
        Undef = 0,
        Zero = 1,
        Constant = 2,
        Actions = 3,
    };

    enum class InitExprKind : uint8_t
    {
        Literal = 0,
        Random = 1,
        RandomSeeded = 2,
    };

    struct InitExpr
    {
        InitExprKind kind = InitExprKind::Literal;
        LiteralId literal;
        uint64_t seed = 0;
    };

    enum class InitActionKind : uint8_t
    {
        Set = 0,
        Fill = 1,
        Load = 2,
    };

    enum class LoadFormat : uint8_t
    {
        Hex = 0,
        Binary = 1,
    };

    enum class InitRangeKind : uint8_t
    {
        All = 0,
        From = 1,
        Span = 2,
    };

    struct InitAction
    {
        InitActionKind kind = InitActionKind::Set;
        LoadFormat format = LoadFormat::Hex;
        InitRangeKind rangeKind = InitRangeKind::All;
        uint8_t reserved = 0;
        InitExpr expression;
        uint64_t start = 0;
        uint64_t count = 0;
        StringId path;
    };

    struct InitDescriptor
    {
        InitKind kind = InitKind::Undef;
        uint8_t reserved0 = 0;
        uint16_t reserved1 = 0;
        uint32_t payload = 0;
        uint32_t count = 0;
    };

    struct VariableRecord
    {
        TypeId type;
        InitId init;
    };

    struct VariableLabel
    {
        VariableId variable;
        StringId label;
    };

    static_assert(sizeof(InitDescriptor) == 3 * sizeof(uint32_t));
    static_assert(sizeof(VariableRecord) == 2 * sizeof(uint32_t));
    static_assert(sizeof(VariableLabel) == 2 * sizeof(uint32_t));

    struct LiteralView
    {
        TypeId type;
        std::span<const uint64_t> words;
        std::string_view bytes;
    };

    enum class Opcode : uint8_t
    {
        Assign = 0,
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        And,
        Or,
        Xor,
        Xnor,
        Not,
        Eq,
        Ne,
        Lt,
        Le,
        Gt,
        Ge,
        LogicAnd,
        LogicOr,
        LogicNot,
        ReduceAnd,
        ReduceNand,
        ReduceOr,
        ReduceNor,
        ReduceXor,
        ReduceXnor,
        Shl,
        LogicalShr,
        ArithmeticShr,
        Mux,
        Concat,
        Replicate,
        SliceStatic,
        SliceDynamic,
        SliceArray,
        ChangedAny,
        ChangedPos,
        ChangedNeg,
        RegisterWrite,
        MemoryRead,
        MemoryWrite,
        MemoryFill,
        LatchWrite,
        SystemFunction,
        SystemTask,
        DpiCall,
        ActForward,
        ActBackward,
        MemoryReadAll,
        MemoryWriteLanes,
        ArrayMux,
        ArrayReduceOr,
        ArrayReduceAnd,
        ArrayReduceXor,
        ArrayBroadcast,
        ArrayOnehot,
        ArrayReduceLanesOr,
        ArrayReduceLanesAnd,
        ArrayReduceLanesXor,
    };

    static_assert(sizeof(Opcode) == sizeof(uint8_t));

    std::string_view toString(Opcode opcode) noexcept;

    enum class CallSchedule : uint8_t
    {
        Normal = 0,
        Once = 1,
        Final = 2,
    };

    enum class HostEventMode : uint8_t
    {
        Immediate = 0,
        Pending = 1,
    };

    struct SliceStaticAttributes
    {
        uint32_t lsb = 0;
    };

    struct SystemFunctionAttributes
    {
        StringId name;
        CallSchedule schedule = CallSchedule::Normal;
        bool hasSideEffects = false;
    };

    struct SystemTaskAttributes
    {
        StringId name;
        uint32_t eventCount = 0;
        CallSchedule schedule = CallSchedule::Normal;
        HostEventMode eventMode = HostEventMode::Immediate;
    };

    struct DpiCallAttributes
    {
        StringId importSymbol;
        uint32_t eventCount = 0;
        HostEventMode eventMode = HostEventMode::Immediate;
    };

    struct ActivationAttributesView
    {
        std::span<const BlockId> targets;
    };

    enum class DpiDirection : uint8_t
    {
        Input = 0,
        Output = 1,
        Inout = 2,
    };

    enum class DpiAbiKind : uint8_t
    {
        Integral = 0,
        Real64 = 1,
        Real32 = 2,
        String = 3,
    };

    struct DpiParameter
    {
        StringId name;
        TypeId type;
        DpiDirection direction = DpiDirection::Input;
        DpiAbiKind abi = DpiAbiKind::Integral;
        uint16_t reserved = 0;
    };

    struct DpiReturn
    {
        TypeId type;
        DpiAbiKind abi = DpiAbiKind::Integral;
        bool present = false;
        uint16_t reserved = 0;
    };

    struct DpiImportView
    {
        StringId symbol;
        std::span<const DpiParameter> parameters;
        DpiReturn returnValue;
    };

    enum class ProgramArena : uint8_t
    {
        Types = 0,
        StringOffsets,
        StringBytes,
        InitDescriptors,
        InitActions,
        Literals,
        LiteralWords,
        LiteralBytes,
        Variables,
        VariableLabels,
        Opcodes,
        OperandOffsets,
        Operands,
        ResultOffsets,
        Results,
        SliceStaticAttributes,
        SystemFunctionAttributes,
        SystemTaskAttributes,
        DpiCallAttributes,
        ActivationAttributes,
        ActivationTargets,
        DpiImports,
        DpiParameters,
        BlockOffsets,
        BlockInstructions,
        Count,
    };

    struct ArenaStorageStats
    {
        uint64_t elements = 0;
        uint64_t capacity = 0;
        uint64_t elementBytes = 0;

        uint64_t sizeBytes() const noexcept { return elements * elementBytes; }
        uint64_t capacityBytes() const noexcept { return capacity * elementBytes; }
    };

    struct ProgramStorageStats
    {
        uint64_t types = 0;
        uint64_t strings = 0;
        uint64_t stringBytes = 0;
        uint64_t variables = 0;
        uint64_t instructions = 0;
        uint64_t operands = 0;
        uint64_t results = 0;
        uint64_t blocks = 0;
        uint64_t blockInstructionIds = 0;
        uint64_t instructionBytes = 0;
        uint64_t variableBytes = 0;
        uint64_t initAndLiteralBytes = 0;
        uint64_t attributeBytes = 0;
        uint64_t stringAndLabelBytes = 0;
        uint64_t blockBytes = 0;
        uint64_t estimatedBytes = 0;
        uint64_t reservedBytes = 0;
        std::array<ArenaStorageStats, static_cast<std::size_t>(ProgramArena::Count)> arenas{};

        const ArenaStorageStats &arena(ProgramArena kind) const noexcept
        {
            return arenas[static_cast<std::size_t>(kind)];
        }
    };

    namespace detail
    {
        struct ProgramStorage;
    }

    class ProgramView
    {
    public:
        ProgramView() = default;

        std::size_t typeCount() const noexcept;
        std::size_t stringCount() const noexcept;
        std::size_t initCount() const noexcept;
        std::size_t literalCount() const noexcept;
        std::size_t variableCount() const noexcept;
        std::size_t instructionCount() const noexcept;
        std::size_t dpiImportCount() const noexcept;

        const Type &type(TypeId id) const;
        std::string_view string(StringId id) const;
        const InitDescriptor &init(InitId id) const;
        std::span<const InitAction> initActions(InitId id) const;
        LiteralView literal(LiteralId id) const;
        const VariableRecord &variable(VariableId id) const;
        std::optional<StringId> variableLabel(VariableId id) const noexcept;
        std::span<const VariableLabel> variableLabels() const noexcept;

        Opcode opcode(InstructionId id) const;
        std::span<const VariableId> operands(InstructionId id) const;
        std::span<const VariableId> results(InstructionId id) const;

        std::optional<SliceStaticAttributes> sliceStaticAttributes(InstructionId id) const noexcept;
        std::optional<SystemFunctionAttributes> systemFunctionAttributes(InstructionId id) const noexcept;
        std::optional<SystemTaskAttributes> systemTaskAttributes(InstructionId id) const noexcept;
        std::optional<DpiCallAttributes> dpiCallAttributes(InstructionId id) const noexcept;
        std::optional<ActivationAttributesView> activationAttributes(InstructionId id) const noexcept;

        DpiImportView dpiImport(DpiImportId id) const;
        ProgramStorageStats storageStats() const noexcept;
        bool valid() const noexcept { return storage_ != nullptr; }

    private:
        explicit ProgramView(const detail::ProgramStorage *storage) : storage_(storage) {}

        const detail::ProgramStorage *storage_ = nullptr;

        friend class LinearProgram;
        friend class ScheduledProgram;
        friend class LinearProgramBuilder;
        friend class ScheduledProgramBuilder;
        friend class AmGraph;
    };

    class LinearProgram
    {
    public:
        LinearProgram();
        ~LinearProgram();
        LinearProgram(LinearProgram &&) noexcept;
        LinearProgram &operator=(LinearProgram &&) noexcept;
        LinearProgram(const LinearProgram &) = delete;
        LinearProgram &operator=(const LinearProgram &) = delete;

        ProgramView view() const noexcept { return ProgramView(storage_.get()); }
        bool valid() const noexcept { return storage_ != nullptr; }

    private:
        explicit LinearProgram(std::unique_ptr<detail::ProgramStorage> storage);

        std::unique_ptr<detail::ProgramStorage> storage_;

        friend class LinearProgramBuilder;
        friend class ScheduledProgramBuilder;
        friend class AmGraph;
    };

    class ScheduledProgram
    {
    public:
        ScheduledProgram();
        ~ScheduledProgram();
        ScheduledProgram(ScheduledProgram &&) noexcept;
        ScheduledProgram &operator=(ScheduledProgram &&) noexcept;
        ScheduledProgram(const ScheduledProgram &) = delete;
        ScheduledProgram &operator=(const ScheduledProgram &) = delete;

        ProgramView view() const noexcept { return ProgramView(storage_.get()); }
        bool valid() const noexcept { return storage_ != nullptr; }
        std::size_t blockCount() const noexcept;
        std::size_t blockSize(BlockId block) const;
        InstructionId blockInstruction(BlockId block, std::size_t index) const;

    private:
        explicit ScheduledProgram(std::unique_ptr<detail::ProgramStorage> storage);

        std::unique_ptr<detail::ProgramStorage> storage_;

        friend class ScheduledProgramBuilder;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_PROGRAM_HPP
