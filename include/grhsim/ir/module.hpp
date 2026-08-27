#ifndef WOLVRIX_GRHSIM_IR_MODULE_HPP
#define WOLVRIX_GRHSIM_IR_MODULE_HPP

#include "core/diagnostics.hpp"

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    template <typename Tag>
    struct DenseId
    {
        static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

        uint32_t raw = kInvalid;

        constexpr bool valid() const noexcept { return raw != kInvalid; }
        static constexpr DenseId invalid() noexcept { return {}; }
        explicit constexpr operator bool() const noexcept { return valid(); }
        friend constexpr auto operator<=>(DenseId, DenseId) = default;
    };

    struct OpIdTag;
    struct EdgeIdTag;
    struct StateIdTag;
    struct HostIdTag;
    struct RegionIdTag;
    struct TypeIdTag;
    struct SymbolIdTag;

    using OpId = DenseId<OpIdTag>;
    using EdgeId = DenseId<EdgeIdTag>;
    using StateId = DenseId<StateIdTag>;
    using HostId = DenseId<HostIdTag>;
    using RegionId = DenseId<RegionIdTag>;
    using TypeId = DenseId<TypeIdTag>;
    using SymbolId = DenseId<SymbolIdTag>;

    static_assert(sizeof(OpId) == sizeof(uint32_t));
    static_assert(sizeof(EdgeId) == sizeof(uint32_t));
    static_assert(sizeof(StateId) == sizeof(uint32_t));
    static_assert(sizeof(HostId) == sizeof(uint32_t));
    static_assert(sizeof(RegionId) == sizeof(uint32_t));
    static_assert(sizeof(TypeId) == sizeof(uint32_t));
    static_assert(sizeof(SymbolId) == sizeof(uint32_t));

    struct OpKind
    {
        static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

        uint32_t raw = kInvalid;

        static constexpr OpKind get(uint16_t dialect, uint16_t opcode) noexcept
        {
            return OpKind{(static_cast<uint32_t>(dialect) << 16U) | opcode};
        }
        static constexpr OpKind invalid() noexcept { return {}; }
        constexpr bool valid() const noexcept { return raw != kInvalid; }
        constexpr uint16_t dialect() const noexcept { return static_cast<uint16_t>(raw >> 16U); }
        constexpr uint16_t opcode() const noexcept { return static_cast<uint16_t>(raw); }
        friend constexpr auto operator<=>(OpKind, OpKind) = default;
    };

    enum class TypeTrack : uint8_t
    {
        Generic = 0,
        Backend = 1,
    };

    enum class GenericTypeKind : uint8_t
    {
        Logic = 0,
        Array = 1,
        Real = 2,
        String = 3,
    };

    struct PoolRange
    {
        uint32_t offset = 0;
        uint32_t count = 0;
        friend constexpr auto operator<=>(PoolRange, PoolRange) = default;
    };

    struct TypeRec
    {
        TypeTrack track = TypeTrack::Generic;
        uint8_t kind = 0;
        uint16_t dialect = 0;
        uint32_t width = 0;
        uint32_t rows = 0;
        bool isSigned = false;
        TypeId elementType = TypeId::invalid();
        TypeId refines = TypeId::invalid();
        PoolRange parameters;
        friend constexpr auto operator<=>(const TypeRec &, const TypeRec &) = default;
    };

    class SymbolTable
    {
    public:
        SymbolId intern(std::string_view text);
        SymbolId lookup(std::string_view text) const noexcept;
        std::string_view text(SymbolId id) const noexcept;
        bool valid(SymbolId id) const noexcept;
        std::size_t size() const noexcept { return strings_.size(); }

    private:
        struct TransparentHash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view value) const noexcept;
            std::size_t operator()(const std::string &value) const noexcept;
        };

        struct TransparentEq
        {
            using is_transparent = void;
            bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
            bool operator()(const std::string &lhs, const std::string &rhs) const noexcept;
            bool operator()(const std::string &lhs, std::string_view rhs) const noexcept;
            bool operator()(std::string_view lhs, const std::string &rhs) const noexcept;
        };

        std::vector<std::string> strings_;
        std::unordered_map<std::string, SymbolId, TransparentHash, TransparentEq> byText_;
    };

    using AttrValue = std::variant<
        bool,
        int64_t,
        double,
        SymbolId,
        std::vector<bool>,
        std::vector<int64_t>,
        std::vector<double>,
        std::vector<SymbolId>>;

    struct AttrKV
    {
        SymbolId key;
        AttrValue value;
        friend bool operator==(const AttrKV &, const AttrKV &) = default;
    };

    struct Use
    {
        OpId user;
        uint32_t operandIndex = 0;
        friend constexpr auto operator<=>(Use, Use) = default;
    };

    enum class StateKind : uint8_t
    {
        Input = 0,
        Output = 1,
        State = 2,
    };

    struct StateEntry
    {
        SymbolId name;
        StateKind kind = StateKind::State;
        TypeId genType;
        TypeId backendType = TypeId::invalid();
        PoolRange initAttrs;
        friend constexpr auto operator<=>(const StateEntry &, const StateEntry &) = default;
    };

    enum class HostKind : uint8_t
    {
        Query = 0,
        Effect = 1,
    };

    enum class HostParamDirection : uint8_t
    {
        Input = 0,
        Output = 1,
        InOut = 2,
        Return = 3,
    };

    struct HostParam
    {
        SymbolId name = SymbolId::invalid();
        TypeId type;
        HostParamDirection direction = HostParamDirection::Input;
        friend constexpr auto operator<=>(const HostParam &, const HostParam &) = default;
    };

    struct HostEntry
    {
        SymbolId entry;
        HostKind kind = HostKind::Effect;
        PoolRange signature;
        SymbolId binding;
        PoolRange attrs;
        friend constexpr auto operator<=>(const HostEntry &, const HostEntry &) = default;
    };

    enum class ActivationKind : uint8_t
    {
        Always = 0,
        Posedge = 1,
        Negedge = 2,
    };

    struct Activation
    {
        ActivationKind kind = ActivationKind::Always;
        StateId state = StateId::invalid();
        friend constexpr auto operator<=>(Activation, Activation) = default;
    };

    struct RegionRec
    {
        PoolRange ops;
        Activation activation;
        PoolRange deps;
        friend constexpr auto operator<=>(const RegionRec &, const RegionRec &) = default;
    };

    class Module
    {
    public:
        explicit Module(std::string name = {});

        const std::string &name() const noexcept { return name_; }
        void setName(std::string name)
        {
            thaw();
            name_ = std::move(name);
        }

        SymbolTable &symbols() noexcept
        {
            thaw();
            return symbols_;
        }
        const SymbolTable &symbols() const noexcept { return symbols_; }
        SymbolId intern(std::string_view text)
        {
            const SymbolId existing = symbols_.lookup(text);
            if (existing.valid())
            {
                return existing;
            }
            thaw();
            return symbols_.intern(text);
        }
        std::string_view symbol(SymbolId id) const noexcept { return symbols_.text(id); }

        TypeId internLogicType(uint32_t width, bool isSigned);
        TypeId internArrayType(uint32_t rows, TypeId elementType);
        TypeId internRealType();
        TypeId internStringType();
        TypeId internBackendType(uint16_t dialect, uint8_t kind,
                                 std::span<const uint32_t> parameters,
                                 TypeId refines);
        std::span<const TypeRec> types() const noexcept { return types_; }
        const TypeRec *type(TypeId id) const noexcept;
        std::span<const uint32_t> typeParameters(TypeId id) const noexcept;

        OpId createOp(OpKind kind, SymbolId symbol = SymbolId::invalid());
        bool setOperands(OpId op, std::span<const EdgeId> operands);
        EdgeId addResult(OpId op, TypeId type, SymbolId symbol = SymbolId::invalid());
        bool replaceAllUses(EdgeId oldEdge, EdgeId newEdge);
        bool eraseOp(OpId op);
        bool setAttr(OpId op, std::string_view key, AttrValue value);
        bool eraseAttr(OpId op, std::string_view key);

        std::span<const OpId> ops() const;
        std::span<const EdgeId> edges() const;
        std::size_t opCount() const;
        std::size_t edgeCount() const;
        bool valid(OpId op) const noexcept;
        bool valid(EdgeId edge) const noexcept;
        OpKind kind(OpId op) const noexcept;
        SymbolId opSymbol(OpId op) const noexcept;
        SymbolId edgeSymbol(EdgeId edge) const noexcept;
        TypeId edgeType(EdgeId edge) const noexcept;
        OpId def(EdgeId edge) const noexcept;
        std::span<const EdgeId> operands(OpId op) const noexcept;
        std::span<const EdgeId> results(OpId op) const noexcept;
        std::span<const AttrKV> attrs(OpId op) const noexcept;
        const AttrValue *attr(OpId op, std::string_view key) const noexcept;
        std::span<const Use> users(EdgeId edge) const;
        void rebuildUses() const;

        StateId addState(std::string_view name, StateKind kind, TypeId genType,
                         std::span<const AttrKV> initAttrs = {});
        StateId findState(std::string_view name) const noexcept;
        std::span<const StateEntry> states() const noexcept { return states_; }
        const StateEntry *state(StateId id) const noexcept;
        std::span<const AttrKV> stateInitAttrs(StateId id) const noexcept;
        bool setBackendType(StateId id, TypeId type);

        HostId addHost(std::string_view entry, HostKind kind,
                       std::span<const HostParam> signature,
                       std::string_view binding,
                       std::span<const AttrKV> attrs = {});
        HostId findHost(std::string_view entry) const noexcept;
        std::span<const HostEntry> hosts() const noexcept { return hosts_; }
        const HostEntry *host(HostId id) const noexcept;
        std::span<const HostParam> hostSignature(HostId id) const noexcept;
        std::span<const AttrKV> hostAttrs(HostId id) const noexcept;

        RegionId createRegion(Activation activation = {});
        bool setRegion(OpId op, RegionId region);
        bool setRegion(std::span<const OpId> ops, RegionId region);
        bool mergeRegions(RegionId destination, RegionId source);
        bool setRegionOrder(RegionId region, std::span<const OpId> order);
        bool addRegionDep(RegionId source, RegionId destination);
        RegionId regionOf(OpId op) const noexcept;
        std::span<const RegionId> regions() const;
        const RegionRec *region(RegionId id) const noexcept;
        std::span<const OpId> regionOps(RegionId id) const noexcept;
        std::span<const RegionId> regionDeps(RegionId id) const noexcept;
        std::vector<OpId> linearize() const;
        bool hasSchedule() const noexcept;
        void clearSchedule();

        std::vector<StateId> deriveTrackSet() const;
        bool validate(wolvrix::lib::diag::Diagnostics &diagnostics) const;
        bool validate() const;

        bool frozen() const noexcept { return frozen_; }
        bool hasTombstones() const noexcept { return hasTombstones_; }
        void freeze();
        void compact();

    private:
        void thaw() noexcept { frozen_ = false; }
        void invalidateEntityCaches() const noexcept;
        PoolRange appendEdges(std::span<const EdgeId> values);
        PoolRange appendAttrs(std::span<const AttrKV> values);
        PoolRange appendHostParams(std::span<const HostParam> values);
        PoolRange appendOps(std::span<const OpId> values);
        PoolRange appendRegions(std::span<const RegionId> values);
        std::span<const AttrKV> attrRange(PoolRange range) const noexcept;
        bool valid(TypeId id) const noexcept;
        bool valid(StateId id) const noexcept;
        bool valid(HostId id) const noexcept;
        bool valid(RegionId id) const noexcept;

        std::string name_;
        SymbolTable symbols_;

        std::vector<TypeRec> types_;
        std::vector<uint32_t> typeParameterPool_;

        std::vector<OpKind> opKinds_;
        std::vector<SymbolId> opSymbols_;
        std::vector<PoolRange> opOperandRanges_;
        std::vector<PoolRange> opResultRanges_;
        std::vector<PoolRange> opAttrRanges_;
        std::vector<RegionId> opRegions_;
        std::vector<uint8_t> opAlive_;

        std::vector<TypeId> edgeTypes_;
        std::vector<OpId> edgeDefs_;
        std::vector<SymbolId> edgeSymbols_;
        mutable std::vector<PoolRange> edgeUseRanges_;
        std::vector<uint8_t> edgeAlive_;

        std::vector<EdgeId> operandPool_;
        std::vector<EdgeId> resultPool_;
        std::vector<AttrKV> attrPool_;
        mutable std::vector<Use> usePool_;

        std::vector<StateEntry> states_;
        std::unordered_map<uint32_t, StateId> statesByName_;

        std::vector<HostEntry> hosts_;
        std::vector<HostParam> hostParamPool_;
        std::unordered_map<uint32_t, HostId> hostsByName_;

        std::vector<RegionRec> regions_;
        std::vector<uint8_t> regionAlive_;
        std::vector<OpId> regionOpPool_;
        std::vector<RegionId> regionDepPool_;

        mutable std::vector<OpId> liveOpsCache_;
        mutable std::vector<EdgeId> liveEdgesCache_;
        mutable std::vector<RegionId> liveRegionsCache_;
        mutable bool entityCachesDirty_ = true;
        mutable bool usesDirty_ = true;
        bool frozen_ = false;
        bool hasTombstones_ = false;
    };

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_MODULE_HPP
