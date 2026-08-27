#include "grhsim/ir/module.hpp"

#include "grhsim/ir/generic.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        template <typename T>
        std::span<const T> poolSpan(const std::vector<T> &pool, PoolRange range) noexcept
        {
            const std::size_t offset = range.offset;
            const std::size_t count = range.count;
            if (offset > pool.size() || count > pool.size() - offset)
            {
                return {};
            }
            return std::span<const T>(pool.data() + offset, count);
        }

        template <typename T>
        PoolRange appendPool(std::vector<T> &pool, std::span<const T> values)
        {
            if (pool.size() > std::numeric_limits<uint32_t>::max() ||
                values.size() > std::numeric_limits<uint32_t>::max() - pool.size())
            {
                throw std::length_error("GRHSIM IR pool exceeds 32-bit addressing");
            }
            const PoolRange result{
                .offset = static_cast<uint32_t>(pool.size()),
                .count = static_cast<uint32_t>(values.size()),
            };
            pool.insert(pool.end(), values.begin(), values.end());
            return result;
        }

        bool attrValueReferencesValidSymbols(const SymbolTable &symbols, const AttrValue &value)
        {
            if (const auto *symbol = std::get_if<SymbolId>(&value))
            {
                return symbols.valid(*symbol);
            }
            if (const auto *symbolList = std::get_if<std::vector<SymbolId>>(&value))
            {
                return std::all_of(symbolList->begin(), symbolList->end(),
                                   [&](SymbolId symbol) { return symbols.valid(symbol); });
            }
            return true;
        }

        bool attrValueIsFinite(const AttrValue &value)
        {
            if (const auto *number = std::get_if<double>(&value))
            {
                return std::isfinite(*number);
            }
            if (const auto *numbers = std::get_if<std::vector<double>>(&value))
            {
                return std::all_of(numbers->begin(), numbers->end(),
                                   [](double number) { return std::isfinite(number); });
            }
            return true;
        }

        std::string idContext(std::string_view entity, uint32_t raw)
        {
            return std::string(entity) + "=" + std::to_string(raw);
        }
    } // namespace

    std::size_t SymbolTable::TransparentHash::operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t SymbolTable::TransparentHash::operator()(const std::string &value) const noexcept
    {
        return operator()(std::string_view(value));
    }

    bool SymbolTable::TransparentEq::operator()(std::string_view lhs,
                                                std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool SymbolTable::TransparentEq::operator()(const std::string &lhs,
                                                const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool SymbolTable::TransparentEq::operator()(const std::string &lhs,
                                                std::string_view rhs) const noexcept
    {
        return std::string_view(lhs) == rhs;
    }

    bool SymbolTable::TransparentEq::operator()(std::string_view lhs,
                                                const std::string &rhs) const noexcept
    {
        return lhs == std::string_view(rhs);
    }

    SymbolId SymbolTable::intern(std::string_view text)
    {
        if (const auto found = byText_.find(text); found != byText_.end())
        {
            return found->second;
        }
        if (strings_.size() >= SymbolId::kInvalid)
        {
            throw std::length_error("GRHSIM IR symbol table exceeds 32-bit addressing");
        }
        const SymbolId id{static_cast<uint32_t>(strings_.size())};
        strings_.emplace_back(text);
        byText_.emplace(strings_.back(), id);
        return id;
    }

    SymbolId SymbolTable::lookup(std::string_view text) const noexcept
    {
        const auto found = byText_.find(text);
        return found == byText_.end() ? SymbolId::invalid() : found->second;
    }

    std::string_view SymbolTable::text(SymbolId id) const noexcept
    {
        if (!valid(id))
        {
            return {};
        }
        return strings_[id.raw];
    }

    bool SymbolTable::valid(SymbolId id) const noexcept
    {
        return id.valid() && static_cast<std::size_t>(id.raw) < strings_.size();
    }

    Module::Module(std::string name) : name_(std::move(name)) {}

    TypeId Module::internLogicType(uint32_t width, bool isSigned)
    {
        if (width == 0)
        {
            return TypeId::invalid();
        }
        for (uint32_t index = 0; index < types_.size(); ++index)
        {
            const TypeRec &type = types_[index];
            if (type.track == TypeTrack::Generic &&
                type.kind == static_cast<uint8_t>(GenericTypeKind::Logic) &&
                type.width == width && type.isSigned == isSigned)
            {
                return TypeId{index};
            }
        }
        if (types_.size() >= TypeId::kInvalid)
        {
            throw std::length_error("GRHSIM IR type table exceeds 32-bit addressing");
        }
        thaw();
        const TypeId id{static_cast<uint32_t>(types_.size())};
        types_.push_back(TypeRec{
            .track = TypeTrack::Generic,
            .kind = static_cast<uint8_t>(GenericTypeKind::Logic),
            .dialect = kGenericDialect,
            .width = width,
            .rows = 0,
            .isSigned = isSigned,
            .elementType = TypeId::invalid(),
            .refines = TypeId::invalid(),
            .parameters = {},
        });
        return id;
    }

    TypeId Module::internArrayType(uint32_t rows, TypeId elementType)
    {
        const TypeRec *element = type(elementType);
        if (rows == 0 || !element || element->track != TypeTrack::Generic ||
            element->kind != static_cast<uint8_t>(GenericTypeKind::Logic))
        {
            return TypeId::invalid();
        }
        for (uint32_t index = 0; index < types_.size(); ++index)
        {
            const TypeRec &candidate = types_[index];
            if (candidate.track == TypeTrack::Generic &&
                candidate.kind == static_cast<uint8_t>(GenericTypeKind::Array) &&
                candidate.rows == rows && candidate.elementType == elementType)
            {
                return TypeId{index};
            }
        }
        if (types_.size() >= TypeId::kInvalid)
        {
            throw std::length_error("GRHSIM IR type table exceeds 32-bit addressing");
        }
        thaw();
        const TypeId id{static_cast<uint32_t>(types_.size())};
        types_.push_back(TypeRec{
            .track = TypeTrack::Generic,
            .kind = static_cast<uint8_t>(GenericTypeKind::Array),
            .dialect = kGenericDialect,
            .width = 0,
            .rows = rows,
            .isSigned = false,
            .elementType = elementType,
            .refines = TypeId::invalid(),
            .parameters = {},
        });
        return id;
    }

    TypeId Module::internRealType()
    {
        for (uint32_t index = 0; index < types_.size(); ++index)
        {
            const TypeRec &candidate = types_[index];
            if (candidate.track == TypeTrack::Generic &&
                candidate.kind == static_cast<uint8_t>(GenericTypeKind::Real))
            {
                return TypeId{index};
            }
        }
        if (types_.size() >= TypeId::kInvalid)
        {
            throw std::length_error("GRHSIM IR type table exceeds 32-bit addressing");
        }
        thaw();
        const TypeId id{static_cast<uint32_t>(types_.size())};
        types_.push_back(TypeRec{
            .track = TypeTrack::Generic,
            .kind = static_cast<uint8_t>(GenericTypeKind::Real),
            .dialect = kGenericDialect,
        });
        return id;
    }

    TypeId Module::internStringType()
    {
        for (uint32_t index = 0; index < types_.size(); ++index)
        {
            const TypeRec &candidate = types_[index];
            if (candidate.track == TypeTrack::Generic &&
                candidate.kind == static_cast<uint8_t>(GenericTypeKind::String))
            {
                return TypeId{index};
            }
        }
        if (types_.size() >= TypeId::kInvalid)
        {
            throw std::length_error("GRHSIM IR type table exceeds 32-bit addressing");
        }
        thaw();
        const TypeId id{static_cast<uint32_t>(types_.size())};
        types_.push_back(TypeRec{
            .track = TypeTrack::Generic,
            .kind = static_cast<uint8_t>(GenericTypeKind::String),
            .dialect = kGenericDialect,
        });
        return id;
    }

    TypeId Module::internBackendType(uint16_t dialect, uint8_t kind,
                                     std::span<const uint32_t> parameters,
                                     TypeId refines)
    {
        const TypeRec *refinedType = type(refines);
        if (dialect == kGenericDialect || !refinedType ||
            refinedType->track != TypeTrack::Generic)
        {
            return TypeId::invalid();
        }
        if (types_.size() >= TypeId::kInvalid)
        {
            throw std::length_error("GRHSIM IR type table exceeds 32-bit addressing");
        }
        for (uint32_t index = 0; index < types_.size(); ++index)
        {
            const TypeRec &candidate = types_[index];
            if (candidate.track != TypeTrack::Backend || candidate.dialect != dialect ||
                candidate.kind != kind || candidate.refines != refines)
            {
                continue;
            }
            const auto existingParameters = typeParameters(TypeId{index});
            if (existingParameters.size() == parameters.size() &&
                std::equal(existingParameters.begin(), existingParameters.end(),
                           parameters.begin()))
            {
                return TypeId{index};
            }
        }
        thaw();
        const PoolRange range = appendPool(typeParameterPool_, parameters);
        const TypeId id{static_cast<uint32_t>(types_.size())};
        types_.push_back(TypeRec{
            .track = TypeTrack::Backend,
            .kind = kind,
            .dialect = dialect,
            .refines = refines,
            .parameters = range,
        });
        return id;
    }

    const TypeRec *Module::type(TypeId id) const noexcept
    {
        return valid(id) ? &types_[id.raw] : nullptr;
    }

    std::span<const uint32_t> Module::typeParameters(TypeId id) const noexcept
    {
        const TypeRec *record = type(id);
        return record ? poolSpan(typeParameterPool_, record->parameters)
                      : std::span<const uint32_t>{};
    }

    OpId Module::createOp(OpKind kindValue, SymbolId symbolValue)
    {
        if (!kindValue.valid() || (symbolValue.valid() && !symbols_.valid(symbolValue)) ||
            opKinds_.size() >= OpId::kInvalid)
        {
            return OpId::invalid();
        }
        thaw();
        const OpId id{static_cast<uint32_t>(opKinds_.size())};
        opKinds_.push_back(kindValue);
        opSymbols_.push_back(symbolValue);
        opOperandRanges_.push_back({});
        opResultRanges_.push_back({});
        opAttrRanges_.push_back({});
        opRegions_.push_back(RegionId::invalid());
        opAlive_.push_back(1);
        invalidateEntityCaches();
        return id;
    }

    bool Module::setOperands(OpId op, std::span<const EdgeId> operandValues)
    {
        if (!valid(op) || std::any_of(operandValues.begin(), operandValues.end(),
                                     [&](EdgeId edge) { return !valid(edge); }))
        {
            return false;
        }
        thaw();
        opOperandRanges_[op.raw] = appendEdges(operandValues);
        usesDirty_ = true;
        return true;
    }

    EdgeId Module::addResult(OpId op, TypeId typeValue, SymbolId symbolValue)
    {
        if (!valid(op) || !valid(typeValue) ||
            (symbolValue.valid() && !symbols_.valid(symbolValue)) ||
            edgeTypes_.size() >= EdgeId::kInvalid)
        {
            return EdgeId::invalid();
        }
        thaw();
        const EdgeId edge{static_cast<uint32_t>(edgeTypes_.size())};
        edgeTypes_.push_back(typeValue);
        edgeDefs_.push_back(op);
        edgeSymbols_.push_back(symbolValue);
        edgeUseRanges_.push_back({});
        edgeAlive_.push_back(1);

        std::vector<EdgeId> resultValues(results(op).begin(), results(op).end());
        resultValues.push_back(edge);
        opResultRanges_[op.raw] = appendPool(resultPool_, std::span<const EdgeId>(resultValues));
        usesDirty_ = true;
        invalidateEntityCaches();
        return edge;
    }

    bool Module::replaceAllUses(EdgeId oldEdge, EdgeId newEdge)
    {
        if (!valid(oldEdge) || !valid(newEdge))
        {
            return false;
        }
        if (oldEdge == newEdge)
        {
            return true;
        }
        const std::vector<Use> oldUses(users(oldEdge).begin(), users(oldEdge).end());
        std::unordered_set<uint32_t> changedUsers;
        for (const Use use : oldUses)
        {
            changedUsers.insert(use.user.raw);
        }
        for (uint32_t raw : changedUsers)
        {
            const OpId user{raw};
            std::vector<EdgeId> next(operands(user).begin(), operands(user).end());
            std::replace(next.begin(), next.end(), oldEdge, newEdge);
            if (!setOperands(user, next))
            {
                return false;
            }
        }
        return true;
    }

    bool Module::eraseOp(OpId op)
    {
        if (!valid(op))
        {
            return false;
        }
        for (EdgeId edge : results(op))
        {
            if (!users(edge).empty())
            {
                return false;
            }
        }
        thaw();
        opAlive_[op.raw] = 0;
        opKinds_[op.raw] = OpKind::invalid();
        for (EdgeId edge : results(op))
        {
            edgeAlive_[edge.raw] = 0;
        }
        hasTombstones_ = true;
        usesDirty_ = true;
        invalidateEntityCaches();
        return true;
    }

    bool Module::setAttr(OpId op, std::string_view key, AttrValue value)
    {
        if (!valid(op) || key.empty() || !attrValueReferencesValidSymbols(symbols_, value))
        {
            return false;
        }
        thaw();
        const SymbolId keySymbol = intern(key);
        std::vector<AttrKV> values(attrs(op).begin(), attrs(op).end());
        const auto found = std::find_if(values.begin(), values.end(),
                                        [&](const AttrKV &attr) { return attr.key == keySymbol; });
        if (found == values.end())
        {
            values.push_back(AttrKV{keySymbol, std::move(value)});
        }
        else
        {
            found->value = std::move(value);
        }
        std::sort(values.begin(), values.end(), [&](const AttrKV &lhs, const AttrKV &rhs) {
            return symbol(lhs.key) < symbol(rhs.key);
        });
        opAttrRanges_[op.raw] = appendAttrs(values);
        return true;
    }

    bool Module::eraseAttr(OpId op, std::string_view key)
    {
        if (!valid(op))
        {
            return false;
        }
        const SymbolId keySymbol = symbols_.lookup(key);
        if (!keySymbol.valid())
        {
            return false;
        }
        std::vector<AttrKV> values(attrs(op).begin(), attrs(op).end());
        const auto next = std::remove_if(values.begin(), values.end(),
                                         [&](const AttrKV &attr) { return attr.key == keySymbol; });
        if (next == values.end())
        {
            return false;
        }
        thaw();
        values.erase(next, values.end());
        opAttrRanges_[op.raw] = appendAttrs(values);
        return true;
    }

    std::span<const OpId> Module::ops() const
    {
        if (entityCachesDirty_)
        {
            liveOpsCache_.clear();
            liveEdgesCache_.clear();
            liveRegionsCache_.clear();
            for (uint32_t index = 0; index < opAlive_.size(); ++index)
            {
                if (opAlive_[index])
                {
                    liveOpsCache_.push_back(OpId{index});
                }
            }
            for (uint32_t index = 0; index < edgeAlive_.size(); ++index)
            {
                if (edgeAlive_[index])
                {
                    liveEdgesCache_.push_back(EdgeId{index});
                }
            }
            for (uint32_t index = 0; index < regionAlive_.size(); ++index)
            {
                if (regionAlive_[index])
                {
                    liveRegionsCache_.push_back(RegionId{index});
                }
            }
            entityCachesDirty_ = false;
        }
        return liveOpsCache_;
    }

    std::span<const EdgeId> Module::edges() const
    {
        (void)ops();
        return liveEdgesCache_;
    }

    std::size_t Module::opCount() const { return ops().size(); }
    std::size_t Module::edgeCount() const { return edges().size(); }

    bool Module::valid(OpId op) const noexcept
    {
        return op.valid() && static_cast<std::size_t>(op.raw) < opAlive_.size() &&
               opAlive_[op.raw] != 0;
    }

    bool Module::valid(EdgeId edge) const noexcept
    {
        return edge.valid() && static_cast<std::size_t>(edge.raw) < edgeAlive_.size() &&
               edgeAlive_[edge.raw] != 0;
    }

    bool Module::valid(TypeId id) const noexcept
    {
        return id.valid() && static_cast<std::size_t>(id.raw) < types_.size();
    }

    bool Module::valid(StateId id) const noexcept
    {
        return id.valid() && static_cast<std::size_t>(id.raw) < states_.size();
    }

    bool Module::valid(HostId id) const noexcept
    {
        return id.valid() && static_cast<std::size_t>(id.raw) < hosts_.size();
    }

    bool Module::valid(RegionId id) const noexcept
    {
        return id.valid() && static_cast<std::size_t>(id.raw) < regionAlive_.size() &&
               regionAlive_[id.raw] != 0;
    }

    OpKind Module::kind(OpId op) const noexcept
    {
        return valid(op) ? opKinds_[op.raw] : OpKind::invalid();
    }

    SymbolId Module::opSymbol(OpId op) const noexcept
    {
        return valid(op) ? opSymbols_[op.raw] : SymbolId::invalid();
    }

    SymbolId Module::edgeSymbol(EdgeId edge) const noexcept
    {
        return valid(edge) ? edgeSymbols_[edge.raw] : SymbolId::invalid();
    }

    TypeId Module::edgeType(EdgeId edge) const noexcept
    {
        return valid(edge) ? edgeTypes_[edge.raw] : TypeId::invalid();
    }

    OpId Module::def(EdgeId edge) const noexcept
    {
        return valid(edge) ? edgeDefs_[edge.raw] : OpId::invalid();
    }

    std::span<const EdgeId> Module::operands(OpId op) const noexcept
    {
        return valid(op) ? poolSpan(operandPool_, opOperandRanges_[op.raw])
                         : std::span<const EdgeId>{};
    }

    std::span<const EdgeId> Module::results(OpId op) const noexcept
    {
        return valid(op) ? poolSpan(resultPool_, opResultRanges_[op.raw])
                         : std::span<const EdgeId>{};
    }

    std::span<const AttrKV> Module::attrs(OpId op) const noexcept
    {
        return valid(op) ? attrRange(opAttrRanges_[op.raw]) : std::span<const AttrKV>{};
    }

    const AttrValue *Module::attr(OpId op, std::string_view key) const noexcept
    {
        if (!valid(op))
        {
            return nullptr;
        }
        const SymbolId keySymbol = symbols_.lookup(key);
        if (!keySymbol.valid())
        {
            return nullptr;
        }
        for (const AttrKV &item : attrs(op))
        {
            if (item.key == keySymbol)
            {
                return &item.value;
            }
        }
        return nullptr;
    }

    std::span<const Use> Module::users(EdgeId edge) const
    {
        if (!valid(edge))
        {
            return {};
        }
        if (usesDirty_)
        {
            rebuildUses();
        }
        return poolSpan(usePool_, edgeUseRanges_[edge.raw]);
    }

    void Module::rebuildUses() const
    {
        std::vector<std::vector<Use>> uses(edgeTypes_.size());
        for (OpId op : ops())
        {
            const auto opOperands = operands(op);
            for (uint32_t index = 0; index < opOperands.size(); ++index)
            {
                const EdgeId edge = opOperands[index];
                if (valid(edge))
                {
                    uses[edge.raw].push_back(Use{op, index});
                }
            }
        }
        usePool_.clear();
        edgeUseRanges_.assign(edgeTypes_.size(), PoolRange{});
        for (uint32_t index = 0; index < uses.size(); ++index)
        {
            if (edgeAlive_[index])
            {
                edgeUseRanges_[index] = appendPool(usePool_, std::span<const Use>(uses[index]));
            }
        }
        usesDirty_ = false;
    }

    StateId Module::addState(std::string_view name, StateKind stateKind, TypeId genType,
                             std::span<const AttrKV> initAttrs)
    {
        if (name.empty() || !valid(genType) || states_.size() >= StateId::kInvalid)
        {
            return StateId::invalid();
        }
        const SymbolId nameSymbol = intern(name);
        if (statesByName_.contains(nameSymbol.raw))
        {
            return StateId::invalid();
        }
        for (const AttrKV &attr : initAttrs)
        {
            if (!symbols_.valid(attr.key) || !attrValueReferencesValidSymbols(symbols_, attr.value))
            {
                return StateId::invalid();
            }
        }
        thaw();
        const StateId id{static_cast<uint32_t>(states_.size())};
        states_.push_back(StateEntry{
            .name = nameSymbol,
            .kind = stateKind,
            .genType = genType,
            .backendType = TypeId::invalid(),
            .initAttrs = appendAttrs(initAttrs),
        });
        statesByName_.emplace(nameSymbol.raw, id);
        return id;
    }

    StateId Module::findState(std::string_view name) const noexcept
    {
        const SymbolId symbolValue = symbols_.lookup(name);
        if (!symbolValue.valid())
        {
            return StateId::invalid();
        }
        const auto found = statesByName_.find(symbolValue.raw);
        return found == statesByName_.end() ? StateId::invalid() : found->second;
    }

    const StateEntry *Module::state(StateId id) const noexcept
    {
        return valid(id) ? &states_[id.raw] : nullptr;
    }

    std::span<const AttrKV> Module::stateInitAttrs(StateId id) const noexcept
    {
        const StateEntry *entry = state(id);
        return entry ? attrRange(entry->initAttrs) : std::span<const AttrKV>{};
    }

    bool Module::setBackendType(StateId id, TypeId typeValue)
    {
        if (!valid(id) || (typeValue.valid() && !valid(typeValue)))
        {
            return false;
        }
        if (typeValue.valid())
        {
            const TypeRec &record = types_[typeValue.raw];
            if (record.track != TypeTrack::Backend || record.refines != states_[id.raw].genType)
            {
                return false;
            }
        }
        thaw();
        states_[id.raw].backendType = typeValue;
        return true;
    }

    HostId Module::addHost(std::string_view entry, HostKind hostKind,
                           std::span<const HostParam> signature,
                           std::string_view binding,
                           std::span<const AttrKV> attrs)
    {
        if (entry.empty() || binding.empty() || hosts_.size() >= HostId::kInvalid)
        {
            return HostId::invalid();
        }
        const SymbolId entrySymbol = intern(entry);
        if (hostsByName_.contains(entrySymbol.raw))
        {
            return HostId::invalid();
        }
        for (const HostParam &parameter : signature)
        {
            if (!valid(parameter.type) ||
                (parameter.name.valid() && !symbols_.valid(parameter.name)))
            {
                return HostId::invalid();
            }
        }
        for (const AttrKV &attr : attrs)
        {
            if (!symbols_.valid(attr.key) || !attrValueReferencesValidSymbols(symbols_, attr.value))
            {
                return HostId::invalid();
            }
        }
        thaw();
        const HostId id{static_cast<uint32_t>(hosts_.size())};
        hosts_.push_back(HostEntry{
            .entry = entrySymbol,
            .kind = hostKind,
            .signature = appendHostParams(signature),
            .binding = intern(binding),
            .attrs = appendAttrs(attrs),
        });
        hostsByName_.emplace(entrySymbol.raw, id);
        return id;
    }

    HostId Module::findHost(std::string_view entry) const noexcept
    {
        const SymbolId symbolValue = symbols_.lookup(entry);
        if (!symbolValue.valid())
        {
            return HostId::invalid();
        }
        const auto found = hostsByName_.find(symbolValue.raw);
        return found == hostsByName_.end() ? HostId::invalid() : found->second;
    }

    const HostEntry *Module::host(HostId id) const noexcept
    {
        return valid(id) ? &hosts_[id.raw] : nullptr;
    }

    std::span<const HostParam> Module::hostSignature(HostId id) const noexcept
    {
        const HostEntry *entry = host(id);
        return entry ? poolSpan(hostParamPool_, entry->signature)
                     : std::span<const HostParam>{};
    }

    std::span<const AttrKV> Module::hostAttrs(HostId id) const noexcept
    {
        const HostEntry *entry = host(id);
        return entry ? attrRange(entry->attrs) : std::span<const AttrKV>{};
    }

    RegionId Module::createRegion(Activation activation)
    {
        const bool validKind = activation.kind == ActivationKind::Always ||
                               activation.kind == ActivationKind::Posedge ||
                               activation.kind == ActivationKind::Negedge;
        const StateEntry *activationState = state(activation.state);
        const TypeRec *activationType = activationState ? type(activationState->genType) : nullptr;
        if (!validKind ||
            (activation.kind == ActivationKind::Always && activation.state.valid()) ||
            (activation.kind != ActivationKind::Always &&
             (!activationState || !activationType ||
              activationType->track != TypeTrack::Generic ||
              activationType->kind != static_cast<uint8_t>(GenericTypeKind::Logic) ||
              activationType->width != 1)) ||
            regions_.size() >= RegionId::kInvalid)
        {
            return RegionId::invalid();
        }
        thaw();
        const RegionId id{static_cast<uint32_t>(regions_.size())};
        regions_.push_back(RegionRec{.activation = activation});
        regionAlive_.push_back(1);
        invalidateEntityCaches();
        return id;
    }

    bool Module::setRegion(OpId op, RegionId regionValue)
    {
        if (!valid(op) || !valid(regionValue))
        {
            return false;
        }
        thaw();
        opRegions_[op.raw] = regionValue;
        return true;
    }

    bool Module::setRegion(std::span<const OpId> opValues, RegionId regionValue)
    {
        if (!valid(regionValue) ||
            std::any_of(opValues.begin(), opValues.end(), [&](OpId op) { return !valid(op); }))
        {
            return false;
        }
        for (OpId op : opValues)
        {
            opRegions_[op.raw] = regionValue;
        }
        thaw();
        return true;
    }

    bool Module::mergeRegions(RegionId destination, RegionId source)
    {
        if (!valid(destination) || !valid(source) || destination == source ||
            regions_[destination.raw].activation != regions_[source.raw].activation)
        {
            return false;
        }
        std::vector<OpId> merged(regionOps(destination).begin(), regionOps(destination).end());
        merged.insert(merged.end(), regionOps(source).begin(), regionOps(source).end());
        for (OpId op : ops())
        {
            if (regionOf(op) == source)
            {
                opRegions_[op.raw] = destination;
                if (std::find(merged.begin(), merged.end(), op) == merged.end())
                {
                    merged.push_back(op);
                }
            }
        }
        regions_[destination.raw].ops = appendOps(merged);

        std::vector<RegionId> mergedDeps(regionDeps(destination).begin(), regionDeps(destination).end());
        mergedDeps.insert(mergedDeps.end(), regionDeps(source).begin(), regionDeps(source).end());
        mergedDeps.erase(std::remove(mergedDeps.begin(), mergedDeps.end(), destination), mergedDeps.end());
        mergedDeps.erase(std::remove(mergedDeps.begin(), mergedDeps.end(), source), mergedDeps.end());
        std::sort(mergedDeps.begin(), mergedDeps.end());
        mergedDeps.erase(std::unique(mergedDeps.begin(), mergedDeps.end()), mergedDeps.end());
        regions_[destination.raw].deps = appendRegions(mergedDeps);

        for (RegionId regionValue : regions())
        {
            if (regionValue == source || regionValue == destination)
            {
                continue;
            }
            std::vector<RegionId> deps(regionDeps(regionValue).begin(), regionDeps(regionValue).end());
            bool changed = false;
            for (RegionId &dep : deps)
            {
                if (dep == source)
                {
                    dep = destination;
                    changed = true;
                }
            }
            if (changed)
            {
                std::sort(deps.begin(), deps.end());
                deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
                deps.erase(std::remove(deps.begin(), deps.end(), regionValue), deps.end());
                regions_[regionValue.raw].deps = appendRegions(deps);
            }
        }
        regionAlive_[source.raw] = 0;
        hasTombstones_ = true;
        thaw();
        invalidateEntityCaches();
        return true;
    }

    bool Module::setRegionOrder(RegionId regionValue, std::span<const OpId> order)
    {
        if (!valid(regionValue) ||
            std::any_of(order.begin(), order.end(), [&](OpId op) {
                return !valid(op) || regionOf(op) != regionValue;
            }))
        {
            return false;
        }
        std::unordered_set<uint32_t> unique;
        for (OpId op : order)
        {
            if (!unique.insert(op.raw).second)
            {
                return false;
            }
        }
        thaw();
        regions_[regionValue.raw].ops = appendOps(order);
        return true;
    }

    bool Module::addRegionDep(RegionId source, RegionId destination)
    {
        if (!valid(source) || !valid(destination) || source == destination)
        {
            return false;
        }
        std::vector<RegionId> deps(regionDeps(source).begin(), regionDeps(source).end());
        if (std::find(deps.begin(), deps.end(), destination) != deps.end())
        {
            return true;
        }
        deps.push_back(destination);
        std::sort(deps.begin(), deps.end());
        thaw();
        regions_[source.raw].deps = appendRegions(deps);
        return true;
    }

    RegionId Module::regionOf(OpId op) const noexcept
    {
        return valid(op) ? opRegions_[op.raw] : RegionId::invalid();
    }

    std::span<const RegionId> Module::regions() const
    {
        (void)ops();
        return liveRegionsCache_;
    }

    const RegionRec *Module::region(RegionId id) const noexcept
    {
        return valid(id) ? &regions_[id.raw] : nullptr;
    }

    std::span<const OpId> Module::regionOps(RegionId id) const noexcept
    {
        const RegionRec *record = region(id);
        return record ? poolSpan(regionOpPool_, record->ops) : std::span<const OpId>{};
    }

    std::span<const RegionId> Module::regionDeps(RegionId id) const noexcept
    {
        const RegionRec *record = region(id);
        return record ? poolSpan(regionDepPool_, record->deps) : std::span<const RegionId>{};
    }

    std::vector<OpId> Module::linearize() const
    {
        if (!hasSchedule())
        {
            return std::vector<OpId>(ops().begin(), ops().end());
        }
        const auto liveRegions = regions();
        std::vector<uint32_t> indegree(regions_.size(), 0);
        for (RegionId source : liveRegions)
        {
            for (RegionId destination : regionDeps(source))
            {
                if (valid(destination))
                {
                    ++indegree[destination.raw];
                }
            }
        }
        std::deque<RegionId> ready;
        for (RegionId regionValue : liveRegions)
        {
            if (indegree[regionValue.raw] == 0)
            {
                ready.push_back(regionValue);
            }
        }
        std::vector<OpId> order;
        std::size_t visitedRegions = 0;
        while (!ready.empty())
        {
            const RegionId current = ready.front();
            ready.pop_front();
            ++visitedRegions;
            const auto currentOps = regionOps(current);
            order.insert(order.end(), currentOps.begin(), currentOps.end());
            for (RegionId destination : regionDeps(current))
            {
                if (valid(destination) && --indegree[destination.raw] == 0)
                {
                    ready.push_back(destination);
                }
            }
        }
        return visitedRegions == liveRegions.size() ? order : std::vector<OpId>{};
    }

    bool Module::hasSchedule() const noexcept
    {
        return std::any_of(regionAlive_.begin(), regionAlive_.end(), [](uint8_t alive) {
            return alive != 0;
        });
    }

    void Module::clearSchedule()
    {
        thaw();
        std::fill(opRegions_.begin(), opRegions_.end(), RegionId::invalid());
        regions_.clear();
        regionAlive_.clear();
        regionOpPool_.clear();
        regionDepPool_.clear();
        invalidateEntityCaches();
    }

    std::vector<StateId> Module::deriveTrackSet() const
    {
        std::vector<StateId> result;
        std::unordered_set<uint32_t> seen;
        const auto add = [&](StateId state) {
            if (valid(state) && seen.insert(state.raw).second)
            {
                result.push_back(state);
            }
        };
        for (OpId op : ops())
        {
            const AttrValue *events = attr(op, "events");
            const auto *symbols = events ? std::get_if<std::vector<SymbolId>>(events) : nullptr;
            if (!symbols)
            {
                continue;
            }
            for (SymbolId event : *symbols)
            {
                add(findState(symbol(event)));
            }
        }
        for (RegionId regionValue : regions())
        {
            const RegionRec *record = region(regionValue);
            if (record && record->activation.kind != ActivationKind::Always)
            {
                add(record->activation.state);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    bool Module::validate(wolvrix::lib::diag::Diagnostics &diagnostics) const
    {
        const auto report = [&](std::string message, std::string context = {}) {
            diagnostics.error(std::move(message), std::move(context));
        };

        for (uint32_t index = 0; index < types_.size(); ++index)
        {
            const TypeRec &record = types_[index];
            if (record.track == TypeTrack::Generic)
            {
                if (record.dialect != kGenericDialect)
                {
                    report("generic Type has a non-generic dialect", idContext("type", index));
                }
                if (record.kind == static_cast<uint8_t>(GenericTypeKind::Logic) && record.width == 0)
                {
                    report("logic Type width must be positive", idContext("type", index));
                }
                else if (record.kind == static_cast<uint8_t>(GenericTypeKind::Array))
                {
                    const TypeRec *element = type(record.elementType);
                    if (record.rows == 0 || !element || element->track != TypeTrack::Generic ||
                        element->kind != static_cast<uint8_t>(GenericTypeKind::Logic))
                    {
                        report("array Type must have positive rows and a logic element Type",
                               idContext("type", index));
                    }
                }
                else if (record.kind > static_cast<uint8_t>(GenericTypeKind::String))
                {
                    report("unknown generic Type kind", idContext("type", index));
                }
            }
            else
            {
                const TypeRec *refined = type(record.refines);
                if (!refined || refined->track != TypeTrack::Generic ||
                    record.dialect == kGenericDialect)
                {
                    report("backend Type must refine a valid generic Type and use a backend dialect",
                           idContext("type", index));
                }
            }
            if (record.parameters.offset > typeParameterPool_.size() ||
                record.parameters.count > typeParameterPool_.size() - record.parameters.offset)
            {
                report("Type parameter range is invalid", idContext("type", index));
            }
        }

        std::unordered_set<uint32_t> stateNames;
        for (uint32_t index = 0; index < states_.size(); ++index)
        {
            const StateEntry &entry = states_[index];
            const TypeRec *genericType = type(entry.genType);
            if (!symbols_.valid(entry.name) || !stateNames.insert(entry.name.raw).second)
            {
                report("StateDecl name is invalid or duplicated", idContext("state", index));
            }
            if (!genericType || genericType->track != TypeTrack::Generic)
            {
                report("StateDecl genType must be generic", idContext("state", index));
            }
            else if ((entry.kind == StateKind::Input || entry.kind == StateKind::Output) &&
                     genericType->kind != static_cast<uint8_t>(GenericTypeKind::Logic))
            {
                report("input/output StateDecl must use a logic Type", idContext("state", index));
            }
            else if (entry.kind == StateKind::State &&
                     genericType->kind != static_cast<uint8_t>(GenericTypeKind::Logic) &&
                     genericType->kind != static_cast<uint8_t>(GenericTypeKind::Array))
            {
                report("state StateDecl must use a logic or array Type", idContext("state", index));
            }
            if (entry.backendType.valid())
            {
                const TypeRec *backend = type(entry.backendType);
                if (!backend || backend->track != TypeTrack::Backend || backend->refines != entry.genType)
                {
                    report("StateDecl backendType does not refine genType", idContext("state", index));
                }
            }
            for (const AttrKV &attr : stateInitAttrs(StateId{index}))
            {
                if (!symbols_.valid(attr.key) ||
                    !attrValueReferencesValidSymbols(symbols_, attr.value))
                {
                    report("StateDecl init attribute is invalid", idContext("state", index));
                }
                if (!attrValueIsFinite(attr.value))
                {
                    report("StateDecl init attribute contains a non-finite floating-point value",
                           idContext("state", index));
                }
            }
        }

        std::unordered_set<uint32_t> hostNames;
        for (uint32_t index = 0; index < hosts_.size(); ++index)
        {
            const HostEntry &entry = hosts_[index];
            if (!symbols_.valid(entry.entry) || !symbols_.valid(entry.binding) ||
                !hostNames.insert(entry.entry.raw).second)
            {
                report("HostTable entry is invalid or duplicated", idContext("host", index));
            }
            for (const HostParam &parameter : hostSignature(HostId{index}))
            {
                if (!valid(parameter.type) ||
                    (parameter.name.valid() && !symbols_.valid(parameter.name)))
                {
                    report("HostTable signature contains an invalid parameter",
                           idContext("host", index));
                }
                else if (const TypeRec *parameterType = type(parameter.type);
                         parameterType->track != TypeTrack::Generic ||
                         parameterType->kind == static_cast<uint8_t>(GenericTypeKind::Array))
                {
                    report("HostTable signature must use a non-array generic Type",
                           idContext("host", index));
                }
            }
            for (const AttrKV &attr : hostAttrs(HostId{index}))
            {
                if (!symbols_.valid(attr.key) ||
                    !attrValueReferencesValidSymbols(symbols_, attr.value))
                {
                    report("HostTable attribute is invalid", idContext("host", index));
                }
                if (!attrValueIsFinite(attr.value))
                {
                    report("HostTable attribute contains a non-finite floating-point value",
                           idContext("host", index));
                }
            }
        }

        std::vector<uint32_t> resultOwnerCount(edgeTypes_.size(), 0);
        std::vector<uint32_t> outputWriterCount(states_.size(), 0);
        std::vector<std::vector<OpId>> scalarWriters(states_.size());
        std::vector<std::vector<OpId>> memoryWriters(states_.size());
        for (OpId op : ops())
        {
            if (!kind(op).valid() ||
                (opSymbol(op).valid() && !symbols_.valid(opSymbol(op))))
            {
                report("operation kind or symbol is invalid", idContext("op", op.raw));
                continue;
            }
            for (EdgeId operand : operands(op))
            {
                if (!valid(operand))
                {
                    report("operation has an invalid operand", idContext("op", op.raw));
                }
            }
            for (EdgeId result : results(op))
            {
                if (!valid(result) || def(result) != op)
                {
                    report("operation result has an invalid defining operation",
                           idContext("op", op.raw));
                }
                else
                {
                    ++resultOwnerCount[result.raw];
                }
            }
            for (const AttrKV &item : attrs(op))
            {
                if (!symbols_.valid(item.key) ||
                    !attrValueReferencesValidSymbols(symbols_, item.value))
                {
                    report("operation has an invalid attribute", idContext("op", op.raw));
                }
                if (!attrValueIsFinite(item.value))
                {
                    report("operation attribute contains a non-finite floating-point value",
                           idContext("op", op.raw));
                }
            }
            if (kind(op) == genericOp(GenericOpcode::OutWrite))
            {
                const AttrValue *portAttr = attr(op, "port");
                const auto *portName = portAttr ? std::get_if<SymbolId>(portAttr) : nullptr;
                const StateId port = portName ? findState(symbol(*portName)) : StateId::invalid();
                if (valid(port) && states_[port.raw].kind == StateKind::Output)
                {
                    ++outputWriterCount[port.raw];
                }
            }
            const OpKind opKind = kind(op);
            const bool scalarWrite = opKind == genericOp(GenericOpcode::RegWrite) ||
                                     opKind == genericOp(GenericOpcode::LatchWrite);
            const bool memoryWrite = opKind == genericOp(GenericOpcode::MemWrite) ||
                                     opKind == genericOp(GenericOpcode::MemWriteLanes) ||
                                     opKind == genericOp(GenericOpcode::MemFill);
            if (scalarWrite || memoryWrite)
            {
                const AttrValue *stateAttr = attr(op, "state");
                const auto *stateName = stateAttr ? std::get_if<SymbolId>(stateAttr) : nullptr;
                const StateId target = stateName ? findState(symbol(*stateName))
                                                 : StateId::invalid();
                if (valid(target))
                {
                    (scalarWrite ? scalarWriters : memoryWriters)[target.raw].push_back(op);
                }
            }
            dialectRegistry().validateOp(*this, op, diagnostics);
        }

        for (uint32_t index = 0; index < states_.size(); ++index)
        {
            if (states_[index].kind == StateKind::Output && outputWriterCount[index] != 1)
            {
                report("output StateDecl must have exactly one out_write",
                       idContext("state", index));
            }
            const auto mutuallyExclusiveRegisterWrites = [&](OpId lhs, OpId rhs) {
                if (kind(lhs) != genericOp(GenericOpcode::RegWrite) ||
                    kind(rhs) != genericOp(GenericOpcode::RegWrite))
                {
                    return false;
                }
                const auto eventList = [&](OpId op, std::string_view key) {
                    const AttrValue *value = attr(op, key);
                    return value ? std::get_if<std::vector<SymbolId>>(value) : nullptr;
                };
                const auto *lhsEvents = eventList(lhs, "events");
                const auto *lhsEdges = eventList(lhs, "eventEdge");
                const auto *rhsEvents = eventList(rhs, "events");
                const auto *rhsEdges = eventList(rhs, "eventEdge");
                if (!lhsEvents || !lhsEdges || !rhsEvents || !rhsEdges ||
                    lhsEvents->empty() || rhsEvents->empty() ||
                    lhsEvents->size() != lhsEdges->size() ||
                    rhsEvents->size() != rhsEdges->size())
                {
                    return false;
                }
                for (std::size_t lhsIndex = 0; lhsIndex < lhsEvents->size(); ++lhsIndex)
                {
                    for (std::size_t rhsIndex = 0; rhsIndex < rhsEvents->size(); ++rhsIndex)
                    {
                        const std::string_view lhsEdge = symbol((*lhsEdges)[lhsIndex]);
                        const std::string_view rhsEdge = symbol((*rhsEdges)[rhsIndex]);
                        const bool oppositeEdges =
                            (lhsEdge == "posedge" && rhsEdge == "negedge") ||
                            (lhsEdge == "negedge" && rhsEdge == "posedge");
                        if ((*lhsEvents)[lhsIndex] != (*rhsEvents)[rhsIndex] ||
                            !oppositeEdges)
                        {
                            return false;
                        }
                    }
                }
                return true;
            };
            bool conflictingScalarWriters = false;
            for (std::size_t lhs = 0;
                 lhs < scalarWriters[index].size() && !conflictingScalarWriters; ++lhs)
            {
                for (std::size_t rhs = lhs + 1; rhs < scalarWriters[index].size(); ++rhs)
                {
                    if (!mutuallyExclusiveRegisterWrites(scalarWriters[index][lhs],
                                                         scalarWriters[index][rhs]))
                    {
                        conflictingScalarWriters = true;
                        break;
                    }
                }
            }
            if (conflictingScalarWriters)
            {
                report("logic StateDecl has multiple register/latch writers",
                       idContext("state", index));
            }

            const auto &writers = memoryWriters[index];
            if (writers.empty())
            {
                continue;
            }
            std::optional<SymbolId> priorityGroup;
            std::vector<int64_t> priorities;
            bool completePriorityGroup = true;
            bool samePriorityGroup = true;
            for (OpId writer : writers)
            {
                const AttrValue *groupAttr = attr(writer, "memoryWrite.priorityGroup");
                const AttrValue *priorityAttr = attr(writer, "memoryWrite.priority");
                const auto *group = groupAttr ? std::get_if<SymbolId>(groupAttr) : nullptr;
                const auto *priority = priorityAttr ? std::get_if<int64_t>(priorityAttr) : nullptr;
                if (!group || !priority)
                {
                    completePriorityGroup = false;
                    continue;
                }
                if (!priorityGroup)
                {
                    priorityGroup = *group;
                }
                else if (*priorityGroup != *group)
                {
                    samePriorityGroup = false;
                }
                priorities.push_back(*priority);
            }
            if (writers.size() > 1 && !completePriorityGroup)
            {
                report("multiple memory writers require one explicit priority group",
                       idContext("state", index));
                continue;
            }
            if (!completePriorityGroup)
            {
                continue;
            }
            if (!samePriorityGroup)
            {
                report("memory writers for one StateDecl must use the same priority group",
                       idContext("state", index));
                continue;
            }
            std::sort(priorities.begin(), priorities.end());
            for (std::size_t priority = 0; priority < priorities.size(); ++priority)
            {
                if (priorities[priority] != static_cast<int64_t>(priority))
                {
                    report("memory write priorities must be unique and contiguous from zero",
                           idContext("state", index));
                    break;
                }
            }
        }

        for (EdgeId edge : edges())
        {
            if (!valid(edgeType(edge)) || !valid(def(edge)) || resultOwnerCount[edge.raw] != 1 ||
                (edgeSymbol(edge).valid() && !symbols_.valid(edgeSymbol(edge))))
            {
                report("edge has an invalid Type, definition, symbol, or result ownership",
                       idContext("edge", edge.raw));
            }
            else if (const TypeRec *edgeTypeRecord = type(edgeType(edge));
                     edgeTypeRecord->track == TypeTrack::Generic &&
                     edgeTypeRecord->kind == static_cast<uint8_t>(GenericTypeKind::Array))
            {
                report("data-flow edge cannot use an array Type", idContext("edge", edge.raw));
            }
            for (const Use use : users(edge))
            {
                if (!valid(use.user) || use.operandIndex >= operands(use.user).size() ||
                    operands(use.user)[use.operandIndex] != edge)
                {
                    report("edge use list is inconsistent", idContext("edge", edge.raw));
                }
            }
        }

        if (hasSchedule())
        {
            std::vector<uint32_t> scheduledCount(opKinds_.size(), 0);
            std::vector<uint32_t> indegree(regions_.size(), 0);
            for (RegionId regionValue : regions())
            {
                const RegionRec *record = region(regionValue);
                if (!record)
                {
                    continue;
                }
                if (record->activation.kind != ActivationKind::Always &&
                    !valid(record->activation.state))
                {
                    report("region activation references an invalid StateDecl",
                           idContext("region", regionValue.raw));
                }
                else if (record->activation.kind == ActivationKind::Always &&
                         record->activation.state.valid())
                {
                    report("always-active region must not reference a StateDecl",
                           idContext("region", regionValue.raw));
                }
                else if (record->activation.kind != ActivationKind::Always)
                {
                    const StateEntry *activationState = state(record->activation.state);
                    const TypeRec *activationType =
                        activationState ? type(activationState->genType) : nullptr;
                    if (!activationType || activationType->track != TypeTrack::Generic ||
                        activationType->kind != static_cast<uint8_t>(GenericTypeKind::Logic) ||
                        activationType->width != 1)
                    {
                        report("region activation must reference a one-bit StateDecl",
                               idContext("region", regionValue.raw));
                    }
                }
                for (OpId op : regionOps(regionValue))
                {
                    if (!valid(op) || regionOf(op) != regionValue)
                    {
                        report("region order contains an invalid or foreign operation",
                               idContext("region", regionValue.raw));
                    }
                    else
                    {
                        ++scheduledCount[op.raw];
                    }
                }
                std::unordered_set<uint32_t> deps;
                for (RegionId destination : regionDeps(regionValue))
                {
                    if (!valid(destination) || destination == regionValue ||
                        !deps.insert(destination.raw).second)
                    {
                        report("region dependency is invalid", idContext("region", regionValue.raw));
                    }
                    else
                    {
                        ++indegree[destination.raw];
                    }
                }
            }
            for (OpId op : ops())
            {
                if (!valid(regionOf(op)) || scheduledCount[op.raw] != 1)
                {
                    report("scheduled operation must belong to exactly one region order",
                           idContext("op", op.raw));
                }
            }

            const auto liveOps = ops();
            const std::size_t nodeCount = liveOps.empty()
                                              ? 0
                                              : static_cast<std::size_t>(liveOps.back().raw) + 1U;
            struct DfsFrame
            {
                OpId op;
                std::size_t resultIndex = 0;
                std::size_t useIndex = 0;
            };
            std::vector<uint8_t> visited(nodeCount, 0);
            std::vector<OpId> postorder;
            postorder.reserve(liveOps.size());
            std::vector<DfsFrame> dfs;
            for (OpId start : liveOps)
            {
                if (visited[start.raw])
                {
                    continue;
                }
                visited[start.raw] = 1;
                dfs.push_back(DfsFrame{.op = start});
                while (!dfs.empty())
                {
                    DfsFrame &frame = dfs.back();
                    const auto opResults = results(frame.op);
                    bool descended = false;
                    while (frame.resultIndex < opResults.size())
                    {
                        const auto edgeUsers = users(opResults[frame.resultIndex]);
                        if (frame.useIndex >= edgeUsers.size())
                        {
                            ++frame.resultIndex;
                            frame.useIndex = 0;
                            continue;
                        }
                        const OpId next = edgeUsers[frame.useIndex++].user;
                        if (valid(next) && !visited[next.raw])
                        {
                            visited[next.raw] = 1;
                            dfs.push_back(DfsFrame{.op = next});
                            descended = true;
                            break;
                        }
                    }
                    if (!descended)
                    {
                        postorder.push_back(frame.op);
                        dfs.pop_back();
                    }
                }
            }

            std::vector<uint8_t> assigned(nodeCount, 0);
            std::vector<uint32_t> componentIds(nodeCount, OpId::kInvalid);
            std::vector<OpId> component;
            uint32_t nextComponent = 0;
            for (auto iterator = postorder.rbegin(); iterator != postorder.rend(); ++iterator)
            {
                const OpId start = *iterator;
                if (assigned[start.raw])
                {
                    continue;
                }
                assigned[start.raw] = 1;
                componentIds[start.raw] = nextComponent;
                component.push_back(start);
                RegionId componentRegion = RegionId::invalid();
                bool split = false;
                while (!component.empty())
                {
                    const OpId current = component.back();
                    component.pop_back();
                    const RegionId currentRegion = regionOf(current);
                    if (!componentRegion.valid())
                    {
                        componentRegion = currentRegion;
                    }
                    else if (currentRegion != componentRegion)
                    {
                        split = true;
                    }
                    for (EdgeId operand : operands(current))
                    {
                        const OpId producer = def(operand);
                        if (valid(producer) && !assigned[producer.raw])
                        {
                            assigned[producer.raw] = 1;
                            componentIds[producer.raw] = nextComponent;
                            component.push_back(producer);
                        }
                    }
                }
                if (split)
                {
                    report("strongly connected component is split across Schedule regions",
                           idContext("op", start.raw));
                }
                ++nextComponent;
            }

            std::vector<uint32_t> regionPosition(nodeCount, OpId::kInvalid);
            for (RegionId regionValue : regions())
            {
                const auto orderedOps = regionOps(regionValue);
                for (uint32_t index = 0; index < orderedOps.size(); ++index)
                {
                    if (valid(orderedOps[index]))
                    {
                        regionPosition[orderedOps[index].raw] = index;
                    }
                }
            }
            for (OpId user : liveOps)
            {
                for (EdgeId edge : operands(user))
                {
                    const OpId producer = def(edge);
                    if (valid(producer) && regionOf(producer) == regionOf(user) &&
                        componentIds[producer.raw] != componentIds[user.raw] &&
                        regionPosition[producer.raw] >= regionPosition[user.raw])
                    {
                        report("region operation order violates a data dependency",
                               idContext("op", user.raw));
                    }
                }
            }

            if (linearize().size() != opCount())
            {
                report("Schedule region dependency graph is cyclic or incomplete", "schedule");
            }
            for (OpId user : ops())
            {
                for (EdgeId edge : operands(user))
                {
                    const OpId producer = def(edge);
                    const RegionId source = regionOf(producer);
                    const RegionId destination = regionOf(user);
                    if (valid(source) && valid(destination) && source != destination)
                    {
                        const auto deps = regionDeps(source);
                        if (std::find(deps.begin(), deps.end(), destination) == deps.end())
                        {
                            report("cross-region data dependency is missing from Schedule",
                                   idContext("op", user.raw));
                        }
                    }
                }
            }
        }

        return !diagnostics.hasError();
    }

    bool Module::validate() const
    {
        wolvrix::lib::diag::Diagnostics diagnostics;
        return validate(diagnostics);
    }

    void Module::freeze()
    {
        compact();
        rebuildUses();
        frozen_ = true;
    }

    void Module::compact()
    {
        thaw();

        std::vector<OpId> opMap(opKinds_.size(), OpId::invalid());
        std::vector<EdgeId> edgeMap(edgeTypes_.size(), EdgeId::invalid());
        std::vector<RegionId> regionMap(regions_.size(), RegionId::invalid());
        std::vector<OpId> compactOpOrder(ops().begin(), ops().end());
        if (hasSchedule())
        {
            std::vector<OpId> scheduledOrder = linearize();
            std::unordered_set<uint32_t> seen;
            const bool complete = scheduledOrder.size() == compactOpOrder.size() &&
                std::all_of(scheduledOrder.begin(), scheduledOrder.end(), [&](OpId op) {
                    return valid(op) && seen.insert(op.raw).second;
                });
            if (complete)
            {
                compactOpOrder = std::move(scheduledOrder);
            }
        }
        uint32_t nextOp = 0;
        for (OpId op : compactOpOrder)
        {
            opMap[op.raw] = OpId{nextOp++};
        }

        std::vector<EdgeId> compactEdgeOrder;
        compactEdgeOrder.reserve(edgeCount());
        const auto addEdge = [&](EdgeId edge) {
            if (valid(edge) && valid(edgeDefs_[edge.raw]) && !edgeMap[edge.raw].valid())
            {
                edgeMap[edge.raw] = EdgeId{static_cast<uint32_t>(compactEdgeOrder.size())};
                compactEdgeOrder.push_back(edge);
            }
        };
        for (OpId op : compactOpOrder)
        {
            for (EdgeId edge : results(op))
            {
                addEdge(edge);
            }
        }
        for (EdgeId edge : edges())
        {
            addEdge(edge);
        }
        const uint32_t nextEdge = static_cast<uint32_t>(compactEdgeOrder.size());
        uint32_t nextRegion = 0;
        for (uint32_t index = 0; index < regionAlive_.size(); ++index)
        {
            if (regionAlive_[index])
            {
                regionMap[index] = RegionId{nextRegion++};
            }
        }

        std::vector<AttrKV> newAttrPool;
        for (StateEntry &stateEntry : states_)
        {
            stateEntry.initAttrs = appendPool(newAttrPool, stateInitAttrs(
                StateId{static_cast<uint32_t>(&stateEntry - states_.data())}));
        }
        for (HostEntry &hostEntry : hosts_)
        {
            hostEntry.attrs = appendPool(newAttrPool, hostAttrs(
                HostId{static_cast<uint32_t>(&hostEntry - hosts_.data())}));
        }

        std::vector<OpKind> newOpKinds;
        std::vector<SymbolId> newOpSymbols;
        std::vector<PoolRange> newOpOperandRanges;
        std::vector<PoolRange> newOpResultRanges;
        std::vector<PoolRange> newOpAttrRanges;
        std::vector<RegionId> newOpRegions;
        std::vector<EdgeId> newOperandPool;
        std::vector<EdgeId> newResultPool;
        newOpKinds.reserve(nextOp);
        newOpSymbols.reserve(nextOp);
        newOpOperandRanges.reserve(nextOp);
        newOpResultRanges.reserve(nextOp);
        newOpAttrRanges.reserve(nextOp);
        newOpRegions.reserve(nextOp);

        for (OpId oldOp : compactOpOrder)
        {
            std::vector<EdgeId> mappedOperands;
            for (EdgeId edge : operands(oldOp))
            {
                if (edge.valid() && edge.raw < edgeMap.size() && edgeMap[edge.raw].valid())
                {
                    mappedOperands.push_back(edgeMap[edge.raw]);
                }
            }
            std::vector<EdgeId> mappedResults;
            for (EdgeId edge : results(oldOp))
            {
                if (edge.valid() && edge.raw < edgeMap.size() && edgeMap[edge.raw].valid())
                {
                    mappedResults.push_back(edgeMap[edge.raw]);
                }
            }
            newOpKinds.push_back(opKinds_[oldOp.raw]);
            newOpSymbols.push_back(opSymbols_[oldOp.raw]);
            newOpOperandRanges.push_back(appendPool(newOperandPool,
                                                    std::span<const EdgeId>(mappedOperands)));
            newOpResultRanges.push_back(appendPool(newResultPool,
                                                   std::span<const EdgeId>(mappedResults)));
            newOpAttrRanges.push_back(appendPool(newAttrPool, attrs(oldOp)));
            const RegionId oldRegion = opRegions_[oldOp.raw];
            newOpRegions.push_back(oldRegion.valid() && oldRegion.raw < regionMap.size()
                                       ? regionMap[oldRegion.raw]
                                       : RegionId::invalid());
        }

        std::vector<TypeId> newEdgeTypes;
        std::vector<OpId> newEdgeDefs;
        std::vector<SymbolId> newEdgeSymbols;
        newEdgeTypes.reserve(nextEdge);
        newEdgeDefs.reserve(nextEdge);
        newEdgeSymbols.reserve(nextEdge);
        for (EdgeId oldEdge : compactEdgeOrder)
        {
            newEdgeTypes.push_back(edgeTypes_[oldEdge.raw]);
            newEdgeDefs.push_back(opMap[edgeDefs_[oldEdge.raw].raw]);
            newEdgeSymbols.push_back(edgeSymbols_[oldEdge.raw]);
        }

        std::vector<RegionRec> newRegions;
        std::vector<OpId> newRegionOpPool;
        std::vector<RegionId> newRegionDepPool;
        newRegions.reserve(nextRegion);
        for (uint32_t old = 0; old < regions_.size(); ++old)
        {
            if (!regionMap[old].valid())
            {
                continue;
            }
            std::vector<OpId> mappedOps;
            for (OpId op : regionOps(RegionId{old}))
            {
                if (op.valid() && op.raw < opMap.size() && opMap[op.raw].valid())
                {
                    mappedOps.push_back(opMap[op.raw]);
                }
            }
            std::vector<RegionId> mappedDeps;
            for (RegionId dep : regionDeps(RegionId{old}))
            {
                if (dep.valid() && dep.raw < regionMap.size() && regionMap[dep.raw].valid())
                {
                    mappedDeps.push_back(regionMap[dep.raw]);
                }
            }
            std::sort(mappedDeps.begin(), mappedDeps.end());
            mappedDeps.erase(std::unique(mappedDeps.begin(), mappedDeps.end()), mappedDeps.end());
            newRegions.push_back(RegionRec{
                .ops = appendPool(newRegionOpPool, std::span<const OpId>(mappedOps)),
                .activation = regions_[old].activation,
                .deps = appendPool(newRegionDepPool, std::span<const RegionId>(mappedDeps)),
            });
        }

        opKinds_ = std::move(newOpKinds);
        opSymbols_ = std::move(newOpSymbols);
        opOperandRanges_ = std::move(newOpOperandRanges);
        opResultRanges_ = std::move(newOpResultRanges);
        opAttrRanges_ = std::move(newOpAttrRanges);
        opRegions_ = std::move(newOpRegions);
        opAlive_.assign(opKinds_.size(), 1);
        edgeTypes_ = std::move(newEdgeTypes);
        edgeDefs_ = std::move(newEdgeDefs);
        edgeSymbols_ = std::move(newEdgeSymbols);
        edgeAlive_.assign(edgeTypes_.size(), 1);
        edgeUseRanges_.assign(edgeTypes_.size(), {});
        operandPool_ = std::move(newOperandPool);
        resultPool_ = std::move(newResultPool);
        attrPool_ = std::move(newAttrPool);
        usePool_.clear();
        regions_ = std::move(newRegions);
        regionAlive_.assign(regions_.size(), 1);
        regionOpPool_ = std::move(newRegionOpPool);
        regionDepPool_ = std::move(newRegionDepPool);
        usesDirty_ = true;
        hasTombstones_ = false;
        invalidateEntityCaches();
    }

    void Module::invalidateEntityCaches() const noexcept
    {
        entityCachesDirty_ = true;
    }

    PoolRange Module::appendEdges(std::span<const EdgeId> values)
    {
        return appendPool(operandPool_, values);
    }

    PoolRange Module::appendAttrs(std::span<const AttrKV> values)
    {
        return appendPool(attrPool_, values);
    }

    PoolRange Module::appendHostParams(std::span<const HostParam> values)
    {
        return appendPool(hostParamPool_, values);
    }

    PoolRange Module::appendOps(std::span<const OpId> values)
    {
        return appendPool(regionOpPool_, values);
    }

    PoolRange Module::appendRegions(std::span<const RegionId> values)
    {
        return appendPool(regionDepPool_, values);
    }

    std::span<const AttrKV> Module::attrRange(PoolRange range) const noexcept
    {
        return poolSpan(attrPool_, range);
    }

} // namespace wolvrix::lib::grhsim
