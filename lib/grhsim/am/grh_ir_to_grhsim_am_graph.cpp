#include "grhsim/am/grh_ir_to_grhsim_am_graph.hpp"

#include "grhsim_am_const_eval.hpp"

#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{
    namespace
    {
        using grh::AttributeValue;
        using grh::Graph;
        using grh::Operation;
        using grh::OperationId;
        using grh::OperationKind;
        using grh::ValueId;
        using grh::ValueType;

        constexpr std::string_view kExternalInstanceGroupAttr =
            "gsim.external_instance_group";
        constexpr std::string_view kExternalCallOrdinalAttr =
            "gsim.external_call_ordinal";
        // NO0006: gsim node provenance stamped by the executable-GRH
        // exporter; every node-owned op carries its owner node id.
        constexpr std::string_view kGsimNodeIdAttr = "gsim.node_id";

        // RAII guard for the lowering-time gsim node context: every
        // instruction created while the guard is live inherits the node id
        // (trigger-owner rule), and the previous context is restored on
        // scope exit.
        struct ScopedGsimNodeId
        {
            ScopedGsimNodeId(int64_t &slot, int64_t value)
                : slot_(slot), previous_(slot)
            {
                slot_ = value;
            }
            ~ScopedGsimNodeId() { slot_ = previous_; }

            int64_t &slot_;
            int64_t previous_;
        };

        std::string lowerAscii(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return result;
        }

        std::string trim(std::string_view text)
        {
            std::size_t begin = 0;
            while (begin < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[begin])))
            {
                ++begin;
            }
            std::size_t end = text.size();
            while (end > begin &&
                   std::isspace(static_cast<unsigned char>(text[end - 1])))
            {
                --end;
            }
            return std::string(text.substr(begin, end - begin));
        }

        std::string normalizedCallName(std::string_view name)
        {
            if (!name.empty() && name.front() == '$')
            {
                name.remove_prefix(1);
            }
            return std::string(name);
        }

        bool isStateDeclaration(OperationKind kind) noexcept
        {
            return kind == OperationKind::kRegister || kind == OperationKind::kLatch ||
                   kind == OperationKind::kMemory;
        }

        bool isStateReadPort(OperationKind kind) noexcept
        {
            return kind == OperationKind::kRegisterReadPort ||
                   kind == OperationKind::kLatchReadPort;
        }

        bool isForbiddenStructuralOperation(OperationKind kind) noexcept
        {
            return kind == OperationKind::kInstance || kind == OperationKind::kBlackbox ||
                   kind == OperationKind::kXMRRead || kind == OperationKind::kXMRWrite;
        }

        struct DpiParameterInfo
        {
            std::string name;
            Type type;
            TypeId typeId;
            DpiDirection direction = DpiDirection::Input;
            DpiAbiKind abi = DpiAbiKind::Integral;
        };

        struct DpiImportInfo
        {
            bool valid = false;
            std::string symbol;
            StringId symbolId;
            DpiImportId importId;
            std::vector<DpiParameterInfo> parameters;
            DpiReturn returnValue;
            Type returnType;
        };

        struct ExplicitOrder
        {
            uint32_t group = 0;
            uint32_t ordinal = 0;
        };

        // One collected state write operation before variant selection. The
        // update condition and write mask stay expression operands; when the
        // writes of one (target, event signature) group are materialized,
        // each write independently selects the simplest cond/mask opcode
        // variant by the constantness of its cond and mask operands.
        struct PendingStateWrite
        {
            Opcode opcode = Opcode::RegisterWrite;
            VariableId target;
            VariableId cond;     // invalid for mem.write_lanes
            VariableId mask;     // invalid for mem.write_lanes / mem.fill
            VariableId data;     // nextValue / element data / fill data / packed lanes
            VariableId addr;     // mem.write only
            std::vector<VariableId> events; // empty for latch.write
            std::optional<ExplicitOrder> explicitOrder;
            std::string context;
            // gsim node id of the source write op (NO0006); the commit
            // instructions materialized from this record inherit it.
            int64_t gsimNodeId = -1;
        };

        struct PendingOrderedGroup
        {
            bool memoryPriority = false;
            std::vector<std::pair<uint32_t, OperationId>> members;
            std::vector<ValueId> referenceEvents;
            std::vector<std::string> referenceEdges;
        };

        class LoweringContext
        {
        public:
            LoweringContext(const Graph &graph,
                            diag::Diagnostics &diagnostics,
                            const GrhIRToGrhSimAMGraphLoweringOptions &options)
                : graph_(graph), diagnostics_(diagnostics), options_(options)
            {
                // Escape hatches for A/B bisection of the lowering-time
                // constant fold and the constant state-write merge (NO0010).
                disableLoweringConstFold_ =
                    std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_LOWERING_CONST_FOLD") !=
                    nullptr;
                disableWriteMerge_ =
                    std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_WRITE_MERGE") != nullptr;
                // Escape hatch for the NO0012 wide concat/slice chain lazy
                // replay (A/B bisection of emitted-cost fixes).
                disableChainReplay_ =
                    std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_CHAIN_REPLAY") != nullptr;
                disableLaneWriteMerge_ =
                    std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_LANE_WRITE_MERGE") !=
                    nullptr;
                if (const char *debugNode =
                        std::getenv("WOLVRIX_GRHSIM_AM_CHAIN_DEBUG_NODE"))
                {
                    int64_t parsed = 0;
                    const auto [end, convError] =
                        std::from_chars(debugNode, debugNode + std::strlen(debugNode),
                                        parsed);
                    if (convError == std::errc{} && *end == '\0')
                    {
                        debugChainNode_ = parsed;
                    }
                }
            }

            std::optional<AmGraph> run()
            {
                try
                {
                    if (!preflight())
                    {
                        return std::nullopt;
                    }
                    reserve();
                    createStateVariables();
                    createDpiImports();
                    createValueVariables();
                    if (failed_)
                    {
                        return std::nullopt;
                    }
                    createInterface();
                    computeFirstUseOrdinals();
                    planChainReplay();
                    lowerInstructions();
                    if (failed_)
                    {
                        return std::nullopt;
                    }
                    materializeStateWrites();

                    std::sort(amGraph_.orderedEffects().begin(), amGraph_.orderedEffects().end(),
                              [](const OrderedEffect &lhs, const OrderedEffect &rhs) {
                                  if (lhs.group != rhs.group)
                                  {
                                      return lhs.group < rhs.group;
                                  }
                                  return lhs.ordinal < rhs.ordinal;
                              });
                    amGraph_.mutableInterface() = std::move(interface_);

                    // The graph is the lowering product itself: it was built
                    // natively, no linear intermediate ever existed.
                    const ValidationResult validation =
                        validate(amGraph_, ValidationOptions{.level = ValidationLevel::Semantic,
                                                             .maxErrors = 64});
                    if (!validation.success())
                    {
                        for (const std::string &message : validation.errors)
                        {
                            diagnostics_.error(message, "grhsim-am-lowering validation");
                        }
                        return std::nullopt;
                    }
                    if (flattenedUnknownLiterals_ != 0)
                    {
                        diagnostics_.info(
                            "flattened X/Z Logic literals for two-state AM lowering: count=" +
                                std::to_string(flattenedUnknownLiterals_),
                            "grhsim-am-lowering");
                    }
                    diagnostics_.info(
                        "am.import assign sites: from_grh=" +
                            std::to_string(assignFromGrhCount_) +
                            " pre_commit=" + std::to_string(assignPreCommitCount_) +
                            " coerce=" + std::to_string(assignCoerceCount_) +
                            " result_bridge=" + std::to_string(assignResultBridgeCount_) +
                            " dpi_bridge=" + std::to_string(assignDpiBridgeCount_),
                        "grhsim-am-lowering");
                    if (lowerConstFoldCount_ != 0 || mergedStateWriteCount_ != 0)
                    {
                        diagnostics_.info(
                            "am.import const fold: folded_ops=" +
                                std::to_string(lowerConstFoldCount_) +
                                " merged_state_writes=" +
                                std::to_string(mergedStateWriteCount_),
                            "grhsim-am-lowering");
                    }
                    if (chainReplaySkipCount_ != 0 || chainReplayRejectCount_ != 0)
                    {
                        diagnostics_.info(
                            "am.import chain replay: groups=" +
                                std::to_string(chainReplayGroups_.size()) +
                                " skipped_ops=" +
                                std::to_string(chainReplaySkipCount_) +
                                " replay_instrs=" +
                                std::to_string(chainReplayEmitCount_) +
                                " plan_folded=" +
                                std::to_string(chainReplayPlanFoldCount_) +
                                " rejected_groups=" +
                                std::to_string(chainReplayRejectCount_) +
                                " lane_write_merged=" +
                                std::to_string(laneWriteMergedCount_) +
                                " lane_fan_rejected=" +
                                std::to_string(laneWriteFanRejectCount_),
                            "grhsim-am-lowering");
                    }
                    return std::optional<AmGraph>(std::move(amGraph_));
                }
                catch (const std::exception &ex)
                {
                    error(std::string("AM lowering failed: ") + ex.what(), graph_.symbol());
                    return std::nullopt;
                }
            }

        private:
            void error(std::string message, std::string context = {})
            {
                failed_ = true;
                diagnostics_.error(std::move(message), std::move(context));
            }

            std::string opContext(const Operation &op) const
            {
                std::string result = "operation '";
                result.append(op.symbolText());
                result.append("' (");
                result.append(grh::toString(op.kind()));
                result.push_back(')');
                if (op.srcLoc())
                {
                    const grh::SrcLoc &loc = *op.srcLoc();
                    if (!loc.file.empty())
                    {
                        result.append(" at ");
                        result.append(loc.file);
                        if (loc.line != 0)
                        {
                            result.push_back(':');
                            result.append(std::to_string(loc.line));
                        }
                    }
                }
                return result;
            }

            template <typename T>
            std::optional<T> optionalAttr(const Operation &op, std::string_view name)
            {
                const std::optional<AttributeValue> value = op.attr(name);
                if (!value)
                {
                    return std::nullopt;
                }
                if (const T *typed = std::get_if<T>(&*value))
                {
                    return *typed;
                }
                error("attribute '" + std::string(name) + "' has the wrong type",
                      opContext(op));
                return std::nullopt;
            }

            template <typename T>
            std::optional<T> requiredAttr(const Operation &op, std::string_view name)
            {
                const std::optional<AttributeValue> value = op.attr(name);
                if (!value)
                {
                    error("missing required attribute '" + std::string(name) + "'",
                          opContext(op));
                    return std::nullopt;
                }
                if (const T *typed = std::get_if<T>(&*value))
                {
                    return *typed;
                }
                error("attribute '" + std::string(name) + "' has the wrong type",
                      opContext(op));
                return std::nullopt;
            }

            bool preflight()
            {
                std::size_t maxValueIndex = 0;
                for (ValueId value : graph_.values())
                {
                    maxValueIndex = std::max(maxValueIndex,
                                             static_cast<std::size_t>(value.index));
                    if (graph_.valueType(value) == ValueType::Logic &&
                        graph_.valueWidth(value) <= 0)
                    {
                        error("Logic Value has non-positive width",
                              std::string(graph_.symbolText(graph_.valueSymbol(value))));
                    }
                }
                valueMap_.assign(maxValueIndex + 1, VariableId::invalid());

                std::size_t maxOperationIndex = 0;
                for (OperationId operation : graph_.operations())
                {
                    maxOperationIndex = std::max(maxOperationIndex,
                                                 static_cast<std::size_t>(operation.index));
                    const Operation op = graph_.getOperation(operation);
                    if (isForbiddenStructuralOperation(op.kind()))
                    {
                        std::string message;
                        switch (op.kind())
                        {
                        case OperationKind::kInstance:
                            message = "kInstance remains after hierarchy flattening";
                            break;
                        case OperationKind::kBlackbox:
                            message = "kBlackbox is not a supported GRHSIM-AM boundary";
                            break;
                        case OperationKind::kXMRRead:
                        case OperationKind::kXMRWrite:
                            message = "XMR remains after XMR resolution";
                            break;
                        default:
                            break;
                        }
                        error(std::move(message), opContext(op));
                    }
                }
                stateByOperation_.assign(maxOperationIndex + 1,
                                         VariableId::invalid());
                stateTypeByOperation_.resize(maxOperationIndex + 1);
                dpiImportByOperation_.resize(maxOperationIndex + 1);
                explicitOrderByOperation_.resize(maxOperationIndex + 1);

                collectExposedValues();
                prepareExplicitOrderGroups();
                return !failed_;
            }

            void collectExposedValues()
            {
                const auto expose = [&](ValueId value) {
                    if (value.valid())
                    {
                        exposedValues_.insert(value.index);
                    }
                };
                for (const grh::Port &port : graph_.inputPorts())
                {
                    expose(port.value);
                }
                for (const grh::Port &port : graph_.outputPorts())
                {
                    expose(port.value);
                }
                for (const grh::InoutPort &port : graph_.inoutPorts())
                {
                    expose(port.in);
                    expose(port.out);
                    expose(port.oe);
                }
                for (grh::SymbolId symbol : graph_.declaredSymbols())
                {
                    const ValueId value = graph_.findValue(symbol);
                    if (value.valid())
                    {
                        exposedValues_.insert(value.index);
                        declaredValueIndices_.insert(value.index);
                    }
                    const OperationId operation = graph_.findOperation(symbol);
                    if (operation.valid() && isStateDeclaration(graph_.opKind(operation)))
                    {
                        declaredStateIndices_.insert(operation.index);
                    }
                }
            }

            void prepareExplicitOrderGroups()
            {
                std::map<std::string, PendingOrderedGroup, std::less<>> groups;
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
                    const auto memoryGroup =
                        optionalAttr<std::string>(op, grh::kMemoryWritePriorityGroupAttr);
                    const auto memoryPriority =
                        optionalAttr<int64_t>(op, grh::kMemoryWritePriorityAttr);
                    if (memoryGroup.has_value() != memoryPriority.has_value())
                    {
                        error("memory write priority attributes must appear together",
                              opContext(op));
                    }
                    if (memoryGroup || memoryPriority)
                    {
                        if (op.kind() != OperationKind::kMemoryWritePort &&
                            op.kind() != OperationKind::kMemoryWriteLanesPort)
                        {
                            error("memory write priority attributes are only valid on kMemoryWritePort/kMemoryWriteLanesPort",
                                  opContext(op));
                            continue;
                        }
                        if (!memoryGroup || memoryGroup->empty() || !memoryPriority ||
                            *memoryPriority < 0 ||
                            static_cast<uint64_t>(*memoryPriority) >=
                                std::numeric_limits<uint32_t>::max())
                        {
                            error("invalid memory write priority group or priority",
                                  opContext(op));
                            continue;
                        }
                        const auto target = requiredAttr<std::string>(op, "memSymbol");
                        const auto edges = requiredAttr<std::vector<std::string>>(op, "eventEdge");
                        if (!target || !edges)
                        {
                            continue;
                        }
                        std::string key = "memory:" + *target + ":" + *memoryGroup;
                        PendingOrderedGroup &group = groups[key];
                        group.memoryPriority = true;
                        group.members.emplace_back(static_cast<uint32_t>(*memoryPriority), operation);
                        const auto operands = op.operands();
                        const std::size_t eventStart =
                            op.kind() == OperationKind::kMemoryWriteLanesPort ? 2 : 4;
                        if (operands.size() >= eventStart)
                        {
                            std::vector<ValueId> events(operands.begin() + eventStart,
                                                        operands.end());
                            if (group.members.size() == 1)
                            {
                                group.referenceEvents = std::move(events);
                                group.referenceEdges = *edges;
                            }
                            else if (group.referenceEvents != events ||
                                     group.referenceEdges != *edges)
                            {
                                error("ordered memory writes must use identical events and edges",
                                      opContext(op));
                            }
                        }
                    }

                    const auto externalGroup =
                        optionalAttr<std::string>(op, kExternalInstanceGroupAttr);
                    const auto externalOrdinal =
                        optionalAttr<int64_t>(op, kExternalCallOrdinalAttr);
                    if (externalGroup.has_value() != externalOrdinal.has_value())
                    {
                        error("external call ordering attributes must appear together",
                              opContext(op));
                    }
                    if (externalGroup || externalOrdinal)
                    {
                        if (op.kind() != OperationKind::kDpicCall)
                        {
                            error("external call ordering attributes are only valid on kDpicCall",
                                  opContext(op));
                            continue;
                        }
                        if (!externalGroup || externalGroup->empty() || !externalOrdinal ||
                            *externalOrdinal < 0 ||
                            static_cast<uint64_t>(*externalOrdinal) >=
                                std::numeric_limits<uint32_t>::max())
                        {
                            error("invalid external call group or ordinal", opContext(op));
                            continue;
                        }
                        std::string key = "external:" + *externalGroup;
                        PendingOrderedGroup &group = groups[key];
                        group.members.emplace_back(static_cast<uint32_t>(*externalOrdinal), operation);
                    }
                }

                uint32_t groupId = 0;
                for (auto &[key, group] : groups)
                {
                    (void)key;
                    std::sort(group.members.begin(), group.members.end(),
                              [](const auto &lhs, const auto &rhs) {
                                  return lhs.first < rhs.first;
                              });
                    for (std::size_t index = 0; index < group.members.size(); ++index)
                    {
                        if (group.members[index].first != index)
                        {
                            error("ordered effect group ordinals must be unique and contiguous");
                            break;
                        }
                    }
                    for (std::size_t index = 0; index < group.members.size(); ++index)
                    {
                        uint32_t ordinal = static_cast<uint32_t>(index);
                        if (group.memoryPriority)
                        {
                            ordinal = static_cast<uint32_t>(group.members.size() - 1 - index);
                        }
                        explicitOrderByOperation_[group.members[index].second.index] =
                            ExplicitOrder{.group = groupId, .ordinal = ordinal};
                    }
                    ++groupId;
                }
                nextOrderedGroup_ = groupId;
            }

            void reserve()
            {
                std::size_t stateCount = 0;
                std::size_t eventCount = 0;
                std::size_t instructionCount = 0;
                std::size_t operandCount = 0;
                std::size_t resultCount = 0;
                // Slack for the nextValue merge logic (mux/blend/read-old)
                // materialized per collected state write.
                std::size_t mergeSlack = 0;
                std::size_t stringCount = graph_.inputPorts().size() +
                                          graph_.outputPorts().size() +
                                          graph_.inoutPorts().size() +
                                          graph_.declaredSymbols().size();
                std::size_t stringBytes = 0;
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
                    if (isStateDeclaration(op.kind()))
                    {
                        ++stateCount;
                        ++stringCount;
                        stringBytes += op.symbolText().size();
                    }
                    if (op.kind() != OperationKind::kConstant &&
                        op.kind() != OperationKind::kArrayLaneConst &&
                        op.kind() != OperationKind::kRegister &&
                        op.kind() != OperationKind::kRegisterReadPort &&
                        op.kind() != OperationKind::kLatch &&
                        op.kind() != OperationKind::kLatchReadPort &&
                        op.kind() != OperationKind::kMemory &&
                        op.kind() != OperationKind::kDpicImport)
                    {
                        ++instructionCount;
                        operandCount += op.operands().size() + 1;
                        resultCount += op.results().size();
                    }
                    if (const auto edges = op.attr("eventEdge"))
                    {
                        if (const auto *typed =
                                std::get_if<std::vector<std::string>>(&*edges))
                        {
                            eventCount += typed->size();
                        }
                    }
                    switch (op.kind())
                    {
                    case OperationKind::kRegisterWritePort:
                    case OperationKind::kLatchWritePort:
                        mergeSlack += 6;
                        break;
                    case OperationKind::kMemoryWritePort:
                        mergeSlack += 9;
                        break;
                    case OperationKind::kMemoryFillPort:
                        mergeSlack += 3;
                        break;
                    default:
                        break;
                    }
                }
                instructionCount += eventCount + mergeSlack;
                operandCount += 2 * eventCount + 3 * mergeSlack;
                resultCount += eventCount + mergeSlack;

                ProgramReserve reserve;
                reserve.types = 64;
                reserve.strings = stringCount + 64;
                reserve.stringBytes = stringBytes + 4096;
                reserve.initDescriptors = 2 + graph_.operations().size() / 16;
                reserve.initActions = graph_.operations().size() / 16;
                reserve.literals = graph_.operations().size() / 16;
                reserve.literalWords = graph_.operations().size() / 8;
                reserve.variables =
                    graph_.values().size() + stateCount + 2 * eventCount + mergeSlack;
                reserve.variableLabels = exposedValues_.size() + stateCount;
                reserve.instructions = instructionCount;
                reserve.operands = operandCount;
                reserve.results = resultCount;
                reserve.sliceStaticAttributes = instructionCount / 16;
                reserve.systemFunctionAttributes = instructionCount / 128;
                reserve.systemTaskAttributes = instructionCount / 128;
                reserve.dpiCallAttributes = instructionCount / 128;
                reserve.dpiImports = 64;
                reserve.dpiParameters = 512;
                amGraph_.reserve(reserve);
            }

            Type typeForValue(ValueId value)
            {
                switch (graph_.valueType(value))
                {
                case ValueType::Logic:
                    return Type::bitVector(
                        static_cast<uint32_t>(graph_.valueWidth(value)),
                        graph_.valueSigned(value) ? Signedness::Signed
                                                  : Signedness::Unsigned);
                case ValueType::Real:
                    return Type::real();
                case ValueType::String:
                    return Type::string();
                }
                throw std::logic_error("unknown GRH ValueType");
            }

            TypeId internType(const Type &type)
            {
                const auto found = typeIds_.find(type);
                if (found != typeIds_.end())
                {
                    return found->second;
                }
                const TypeId id = amGraph_.addType(type);
                typeIds_.emplace(type, id);
                return id;
            }

            StringId internString(std::string_view text)
            {
                const auto found = stringIds_.find(std::string(text));
                if (found != stringIds_.end())
                {
                    return found->second;
                }
                const StringId id = amGraph_.addString(text);
                stringIds_.emplace(std::string(text), id);
                return id;
            }

            AmValueFacts valueFactsFor(TypeId type, InitId init, VariableRole role)
            {
                AmValueFacts facts;
                facts.roles = role;
                if (hasRole(role, VariableRole::State))
                {
                    facts.kind = AmValueKind::State;
                    if (amGraph_.program().type(type).kind == TypeKind::Array)
                    {
                        facts.stateKind = AmStateKind::Memory;
                    }
                }
                else if (hasRole(role, VariableRole::ExternalInput))
                {
                    facts.kind = AmValueKind::Input;
                }
                else if (init.valid() &&
                         amGraph_.program().init(init).kind == InitKind::Constant)
                {
                    facts.kind = AmValueKind::Constant;
                }
                return facts;
            }

            VariableId addVariable(TypeId type, InitId init,
                                   std::optional<StringId> label = std::nullopt,
                                   VariableRole role = VariableRole::None)
            {
                return amGraph_.addVariable(type, init, label,
                                            valueFactsFor(type, init, role));
            }

            VariableId addVariable(const Type &type, InitId init,
                                   std::optional<StringId> label = std::nullopt,
                                   VariableRole role = VariableRole::None)
            {
                return addVariable(internType(type), init, label, role);
            }

            void addRole(VariableId variable, VariableRole role)
            {
                if (!variable.valid() || variable.value >= amGraph_.variableCount())
                {
                    throw std::logic_error("invalid AM variable role update");
                }
                AmValueFacts facts = amGraph_.valueFacts(variable);
                facts.roles = facts.roles | role;
                if (hasRole(facts.roles, VariableRole::State))
                {
                    facts.kind = AmValueKind::State;
                }
                else if (hasRole(facts.roles, VariableRole::ExternalInput) &&
                         facts.kind != AmValueKind::State)
                {
                    facts.kind = AmValueKind::Input;
                }
                amGraph_.setValueFacts(variable, facts);
            }

            InstructionId addInstruction(Opcode opcode,
                                         std::span<const VariableId> results,
                                         std::span<const VariableId> operands,
                                         std::optional<InstructionEffect> effect = std::nullopt)
            {
                const InstructionId id = amGraph_.addInstruction(opcode, results, operands);
                // NO0006: single stamping choke point — every instruction
                // created while a node-owned GRH op lowers inherits that
                // op's gsim node id (-1 when unowned).
                amGraph_.setGsimNodeId(id, currentGsimNodeId_);
                if (effect)
                {
                    amGraph_.setInstructionEffect(id, *effect);
                }
                return id;
            }

            std::optional<std::vector<uint64_t>> parseLogicLiteral(
                std::string_view literal, uint32_t width, bool isSigned,
                std::string_view context)
            {
                std::string compact;
                compact.reserve(literal.size());
                for (char ch : literal)
                {
                    if (ch != '_' && !std::isspace(static_cast<unsigned char>(ch)))
                    {
                        compact.push_back(ch);
                    }
                }
                if (compact.empty() || compact.front() == '$' || compact.front() == '"')
                {
                    error("unsupported Logic literal '" + std::string(literal) + "'",
                          std::string(context));
                    return std::nullopt;
                }
                bool negative = false;
                if (compact.front() == '-' || compact.front() == '+')
                {
                    negative = compact.front() == '-';
                    compact.erase(compact.begin());
                }
                if (compact.empty())
                {
                    error("empty Logic literal", std::string(context));
                    return std::nullopt;
                }
                try
                {
                    slang::SVInt parsed = slang::SVInt::fromString(compact);
                    if (negative)
                    {
                        parsed = -parsed;
                    }
                    parsed.setSigned(isSigned);
                    parsed = parsed.resize(static_cast<slang::bitwidth_t>(width));
                    parsed.setSigned(isSigned);
                    if (parsed.hasUnknown())
                    {
                        if (options_.unknownLogic == UnknownLogicPolicy::Reject)
                        {
                            error(
                                "Logic literal contains X or Z and cannot be lowered to AM BV: '" +
                                    std::string(literal) + "'",
                                std::string(context));
                            return std::nullopt;
                        }
                        parsed.flattenUnknowns();
                        ++flattenedUnknownLiterals_;
                    }
                    const std::size_t wordCount = (static_cast<std::size_t>(width) + 63) / 64;
                    std::vector<uint64_t> words(wordCount);
                    const uint64_t *raw = parsed.getRawPtr();
                    std::copy_n(raw, wordCount, words.begin());
                    if (width % 64 != 0)
                    {
                        words.back() &= (UINT64_C(1) << (width % 64)) - 1;
                    }
                    return words;
                }
                catch (const std::exception &ex)
                {
                    error("failed to parse Logic literal '" + std::string(literal) +
                              "': " + ex.what(),
                          std::string(context));
                    return std::nullopt;
                }
            }

            std::optional<uint64_t> parseRandomSeed(std::string_view text,
                                                    std::string_view context)
            {
                auto words = parseLogicLiteral(text, 64, false, context);
                if (!words)
                {
                    return std::nullopt;
                }
                return words->front();
            }

            std::optional<InitExpr> makeInitExpr(std::string_view text,
                                                 TypeId scalarType,
                                                 uint32_t width,
                                                 bool isSigned,
                                                 std::string_view context)
            {
                const std::string value = trim(text);
                if (value == "$random")
                {
                    return InitExpr{.kind = InitExprKind::Random};
                }
                constexpr std::string_view prefix = "$random(";
                if (value.starts_with(prefix) && value.ends_with(')'))
                {
                    const std::string_view seedText(value.data() + prefix.size(),
                                                    value.size() - prefix.size() - 1);
                    const auto seed = parseRandomSeed(seedText, context);
                    if (!seed)
                    {
                        return std::nullopt;
                    }
                    return InitExpr{.kind = InitExprKind::RandomSeeded, .seed = *seed};
                }
                auto words = parseLogicLiteral(value, width, isSigned, context);
                if (!words)
                {
                    return std::nullopt;
                }
                const LiteralId literal = amGraph_.addBitLiteral(scalarType, *words);
                return InitExpr{.kind = InitExprKind::Literal, .literal = literal};
            }

            std::optional<InitId> constantInit(const Operation &op, ValueId result,
                                               TypeId typeId)
            {
                const auto literal = requiredAttr<std::string>(op, "constValue");
                if (!literal)
                {
                    return std::nullopt;
                }
                const Type type = typeForValue(result);
                LiteralId literalId;
                if (type.kind == TypeKind::BitVector)
                {
                    auto words = parseLogicLiteral(*literal, type.bitWidth,
                                                   type.signedness == Signedness::Signed,
                                                   opContext(op));
                    if (!words)
                    {
                        return std::nullopt;
                    }
                    literalId = amGraph_.addBitLiteral(typeId, *words);
                }
                else if (type.kind == TypeKind::Real)
                {
                    try
                    {
                        std::size_t consumed = 0;
                        const double value = std::stod(trim(*literal), &consumed);
                        const std::string compact = trim(*literal);
                        if (consumed != compact.size())
                        {
                            throw std::invalid_argument("trailing characters");
                        }
                        const uint64_t bits = std::bit_cast<uint64_t>(value);
                        literalId = amGraph_.addBitLiteral(typeId,
                                                          std::span<const uint64_t>(&bits, 1));
                    }
                    catch (const std::exception &ex)
                    {
                        error("failed to parse Real literal '" + *literal + "': " +
                                  ex.what(),
                              opContext(op));
                        return std::nullopt;
                    }
                }
                else if (type.kind == TypeKind::String)
                {
                    literalId = amGraph_.addStringLiteral(typeId, *literal);
                }
                else
                {
                    error("GRH constant cannot have Array type", opContext(op));
                    return std::nullopt;
                }
                return amGraph_.addConstantInit(literalId);
            }

            std::optional<InitId> arrayLaneConstInit(const Operation &op, ValueId result,
                                                     TypeId typeId)
            {
                const auto elemWidth = requiredAttr<int64_t>(op, "elemWidth");
                const auto rows = requiredAttr<int64_t>(op, "rows");
                const auto values = requiredAttr<std::vector<int64_t>>(op, "values");
                if (!elemWidth || !rows || !values)
                {
                    return std::nullopt;
                }
                if (*elemWidth <= 0 || *rows <= 0 ||
                    static_cast<uint64_t>(*elemWidth) >
                        std::numeric_limits<uint32_t>::max() ||
                    static_cast<uint64_t>(*rows) >
                        std::numeric_limits<uint32_t>::max() ||
                    values->size() != static_cast<std::size_t>(*rows))
                {
                    error("kArrayLaneConst attributes are invalid", opContext(op));
                    return std::nullopt;
                }
                const uint64_t laneWidth = static_cast<uint64_t>(*elemWidth);
                const uint64_t laneCount = static_cast<uint64_t>(*rows);
                const uint64_t packedWidth = laneWidth * laneCount;
                const Type type = typeForValue(result);
                if (type.kind != TypeKind::BitVector || packedWidth == 0 ||
                    packedWidth != type.bitWidth)
                {
                    error("kArrayLaneConst result width does not equal elemWidth * rows",
                          opContext(op));
                    return std::nullopt;
                }
                std::vector<uint64_t> words(
                    static_cast<std::size_t>((packedWidth + 63U) / 64U), 0);
                for (uint64_t lane = 0; lane < laneCount; ++lane)
                {
                    const int64_t laneValue = (*values)[static_cast<std::size_t>(lane)];
                    if (laneValue < 0 ||
                        (laneWidth < 64 &&
                         (static_cast<uint64_t>(laneValue) >> laneWidth) != 0))
                    {
                        error("kArrayLaneConst lane value does not fit elemWidth",
                              opContext(op));
                        return std::nullopt;
                    }
                    const uint64_t bits = static_cast<uint64_t>(laneValue);
                    for (uint64_t bit = 0; bit < laneWidth; ++bit)
                    {
                        if (((bits >> bit) & 1U) != 0)
                        {
                            const uint64_t position = lane * laneWidth + bit;
                            words[static_cast<std::size_t>(position / 64U)] |=
                                UINT64_C(1) << (position % 64U);
                        }
                    }
                }
                const LiteralId literalId = amGraph_.addBitLiteral(typeId, words);
                return amGraph_.addConstantInit(literalId);
            }

            std::optional<InitId> registerInit(const Operation &op, TypeId type,
                                               uint32_t width, bool isSigned)
            {
                const auto value = optionalAttr<std::string>(op, "initValue");
                if (!value)
                {
                    return amGraph_.undefInit();
                }
                const auto expression = makeInitExpr(*value, type, width, isSigned,
                                                     opContext(op));
                if (!expression)
                {
                    return std::nullopt;
                }
                const InitAction action{
                    .kind = InitActionKind::Set,
                    .expression = *expression,
                };
                return amGraph_.addActionsInit(std::span<const InitAction>(&action, 1));
            }

            std::optional<InitId> memoryInit(const Operation &op, TypeId elementType,
                                             uint32_t width, bool isSigned,
                                             uint32_t rows)
            {
                const auto kinds = optionalAttr<std::vector<std::string>>(op, "initKind");
                const auto files = optionalAttr<std::vector<std::string>>(op, "initFile");
                const auto values = optionalAttr<std::vector<std::string>>(op, "initValue");
                const auto starts = optionalAttr<std::vector<int64_t>>(op, "initStart");
                const auto lengths = optionalAttr<std::vector<int64_t>>(op, "initLen");
                const bool any = kinds || files || values || starts || lengths;
                if (!any || (kinds && kinds->empty()))
                {
                    return amGraph_.undefInit();
                }
                if (!kinds || !files || !starts || !lengths ||
                    files->size() != kinds->size() || starts->size() != kinds->size() ||
                    lengths->size() != kinds->size())
                {
                    error("memory initialization attribute arrays are incomplete or have different lengths",
                          opContext(op));
                    return std::nullopt;
                }

                std::vector<InitAction> actions;
                actions.reserve(kinds->size());
                for (std::size_t index = 0; index < kinds->size(); ++index)
                {
                    const std::string kind = lowerAscii((*kinds)[index]);
                    const int64_t start = (*starts)[index];
                    const int64_t length = (*lengths)[index];
                    if (kind == "readmemh" || kind == "readmemb")
                    {
                        if ((*files)[index].empty())
                        {
                            error("memory load initialization has an empty file path",
                                  opContext(op));
                            return std::nullopt;
                        }
                        InitAction action;
                        action.kind = InitActionKind::Load;
                        action.format = kind == "readmemh" ? LoadFormat::Hex
                                                          : LoadFormat::Binary;
                        action.path = internString((*files)[index]);
                        if (start < 0)
                        {
                            action.rangeKind = InitRangeKind::All;
                        }
                        else if (length <= 0)
                        {
                            action.rangeKind = InitRangeKind::From;
                            action.start = static_cast<uint64_t>(start);
                        }
                        else
                        {
                            action.rangeKind = InitRangeKind::Span;
                            action.start = static_cast<uint64_t>(start);
                            action.count = static_cast<uint64_t>(length);
                        }
                        actions.push_back(action);
                        continue;
                    }
                    if (kind != "literal")
                    {
                        error("unsupported memory initialization kind '" + (*kinds)[index] + "'",
                              opContext(op));
                        return std::nullopt;
                    }
                    const std::string literal =
                        values && index < values->size() ? (*values)[index] : std::string("0");
                    uint64_t fillStart = 0;
                    uint64_t fillCount = rows;
                    if (start >= 0)
                    {
                        if (length <= 0)
                        {
                            error("literal memory initialization requires a positive length",
                                  opContext(op));
                            return std::nullopt;
                        }
                        fillStart = static_cast<uint64_t>(start);
                        fillCount = static_cast<uint64_t>(length);
                    }
                    if (fillStart > rows || fillCount > rows - fillStart)
                    {
                        error("literal memory initialization range is outside the memory",
                              opContext(op));
                        return std::nullopt;
                    }
                    const auto expression = makeInitExpr(literal, elementType, width,
                                                         isSigned, opContext(op));
                    if (!expression)
                    {
                        return std::nullopt;
                    }
                    actions.push_back(InitAction{
                        .kind = InitActionKind::Fill,
                        .expression = *expression,
                        .start = fillStart,
                        .count = fillCount,
                    });
                }
                return amGraph_.addActionsInit(actions);
            }

            void createStateVariables()
            {
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
                    if (!isStateDeclaration(op.kind()))
                    {
                        continue;
                    }
                    if (!op.operands().empty() || !op.results().empty())
                    {
                        error("state declaration must not have operands or results", opContext(op));
                        continue;
                    }
                    const auto widthAttr = requiredAttr<int64_t>(op, "width");
                    const auto signedAttr = requiredAttr<bool>(op, "isSigned");
                    if (!widthAttr || !signedAttr || *widthAttr <= 0 ||
                        static_cast<uint64_t>(*widthAttr) > std::numeric_limits<uint32_t>::max())
                    {
                        if (widthAttr && *widthAttr <= 0)
                        {
                            error("state width must be positive", opContext(op));
                        }
                        continue;
                    }
                    const uint32_t width = static_cast<uint32_t>(*widthAttr);
                    const Signedness sign = *signedAttr ? Signedness::Signed
                                                       : Signedness::Unsigned;
                    Type type;
                    std::optional<InitId> init;
                    if (op.kind() == OperationKind::kMemory)
                    {
                        const auto rowAttr = requiredAttr<int64_t>(op, "row");
                        if (!rowAttr || *rowAttr <= 0 ||
                            static_cast<uint64_t>(*rowAttr) >
                                std::numeric_limits<uint32_t>::max())
                        {
                            if (rowAttr && *rowAttr <= 0)
                            {
                                error("memory row count must be positive", opContext(op));
                            }
                            continue;
                        }
                        const uint32_t rows = static_cast<uint32_t>(*rowAttr);
                        type = Type::array(rows, width, sign);
                        const TypeId elementType = internType(Type::bitVector(width, sign));
                        init = memoryInit(op, elementType, width, *signedAttr, rows);
                    }
                    else
                    {
                        type = Type::bitVector(width, sign);
                        const TypeId typeId = internType(type);
                        init = op.kind() == OperationKind::kRegister
                                   ? registerInit(op, typeId, width, *signedAttr)
                                   : std::optional<InitId>(amGraph_.undefInit());
                    }
                    if (!init)
                    {
                        continue;
                    }
                    std::string nodeNameStorage;
                    std::string_view labelText = op.symbolText();
                    if (const auto nodeName = optionalAttr<std::string>(op, "gsim.node_name"))
                    {
                        // gsim exports carry meaningless gsim.reg.N op symbols;
                        // the real RTL provenance lives in gsim.node_name.
                        nodeNameStorage = *nodeName;
                        labelText = nodeNameStorage;
                    }
                    const StringId label = internString(labelText);
                    VariableRole role = VariableRole::State;
                    if (declaredStateIndices_.contains(operation.index))
                    {
                        role = role | VariableRole::Observable;
                    }
                    const VariableId variable = addVariable(type, *init, label, role);
                    stateByOperation_[operation.index] = variable;
                    stateTypeByOperation_[operation.index] = type;
                }
            }

            std::optional<std::pair<Type, DpiAbiKind>> dpiType(
                std::string_view rawType, int64_t width, bool isSigned,
                const Operation &op, std::string_view what)
            {
                const std::string typeName = lowerAscii(trim(rawType));
                const Signedness sign = isSigned ? Signedness::Signed
                                                 : Signedness::Unsigned;
                auto integral = [&](uint32_t expectedWidth, bool exact) ->
                    std::optional<std::pair<Type, DpiAbiKind>> {
                    if (width <= 0 || static_cast<uint64_t>(width) >
                                          std::numeric_limits<uint32_t>::max() ||
                        (exact && width != expectedWidth))
                    {
                        error(std::string(what) + " has an invalid width for DPI type '" +
                                  typeName + "'",
                              opContext(op));
                        return std::nullopt;
                    }
                    const uint32_t actual = exact ? expectedWidth
                                                  : static_cast<uint32_t>(width);
                    return std::pair{Type::bitVector(actual, sign),
                                     DpiAbiKind::Integral};
                };
                if (typeName == "bit" || typeName == "logic")
                {
                    return integral(0, false);
                }
                if (typeName == "byte")
                {
                    return integral(8, true);
                }
                if (typeName == "shortint")
                {
                    return integral(16, true);
                }
                if (typeName == "int" || typeName == "integer")
                {
                    return integral(32, true);
                }
                if (typeName == "longint" || typeName == "time")
                {
                    return integral(64, true);
                }
                if (typeName == "real")
                {
                    return std::pair{Type::real(), DpiAbiKind::Real64};
                }
                if (typeName == "shortreal")
                {
                    return std::pair{Type::real(), DpiAbiKind::Real32};
                }
                if (typeName == "string")
                {
                    return std::pair{Type::string(), DpiAbiKind::String};
                }
                error("unsupported DPI " + std::string(what) + " type '" + typeName + "'",
                      opContext(op));
                return std::nullopt;
            }

            void createDpiImports()
            {
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
                    if (op.kind() != OperationKind::kDpicImport)
                    {
                        continue;
                    }
                    if (!op.operands().empty() || !op.results().empty())
                    {
                        error("kDpicImport must not have operands or results", opContext(op));
                        continue;
                    }
                    const auto directions =
                        requiredAttr<std::vector<std::string>>(op, "argsDirection");
                    const auto widths = requiredAttr<std::vector<int64_t>>(op, "argsWidth");
                    const auto names = requiredAttr<std::vector<std::string>>(op, "argsName");
                    const auto signs = requiredAttr<std::vector<bool>>(op, "argsSigned");
                    const auto types = requiredAttr<std::vector<std::string>>(op, "argsType");
                    const auto hasReturn = requiredAttr<bool>(op, "hasReturn");
                    if (!directions || !widths || !names || !signs || !types || !hasReturn)
                    {
                        continue;
                    }
                    const std::size_t count = directions->size();
                    if (widths->size() != count || names->size() != count ||
                        signs->size() != count || types->size() != count)
                    {
                        error("DPI import parameter attribute arrays have different lengths",
                              opContext(op));
                        continue;
                    }
                    std::vector<DpiParameter> parameters;
                    std::vector<DpiParameterInfo> parameterInfo;
                    parameters.reserve(count);
                    parameterInfo.reserve(count);
                    bool valid = true;
                    for (std::size_t index = 0; index < count; ++index)
                    {
                        DpiDirection direction;
                        const std::string normalized = lowerAscii((*directions)[index]);
                        if (normalized == "input")
                        {
                            direction = DpiDirection::Input;
                        }
                        else if (normalized == "output")
                        {
                            direction = DpiDirection::Output;
                        }
                        else if (normalized == "inout")
                        {
                            direction = DpiDirection::Inout;
                        }
                        else
                        {
                            error("invalid DPI parameter direction '" +
                                      (*directions)[index] + "'",
                                  opContext(op));
                            valid = false;
                            continue;
                        }
                        if ((*names)[index].empty())
                        {
                            error("DPI parameter name must be non-empty", opContext(op));
                            valid = false;
                            continue;
                        }
                        const auto mapped = dpiType((*types)[index], (*widths)[index],
                                                    (*signs)[index], op, "parameter");
                        if (!mapped)
                        {
                            valid = false;
                            continue;
                        }
                        const TypeId typeId = internType(mapped->first);
                        const StringId nameId = internString((*names)[index]);
                        parameters.push_back(DpiParameter{
                            .name = nameId,
                            .type = typeId,
                            .direction = direction,
                            .abi = mapped->second,
                        });
                        parameterInfo.push_back(DpiParameterInfo{
                            .name = (*names)[index],
                            .type = mapped->first,
                            .typeId = typeId,
                            .direction = direction,
                            .abi = mapped->second,
                        });
                    }
                    if (!valid)
                    {
                        continue;
                    }
                    DpiReturn returnValue;
                    Type returnType;
                    if (*hasReturn)
                    {
                        const auto returnWidth = requiredAttr<int64_t>(op, "returnWidth");
                        const auto returnSigned = requiredAttr<bool>(op, "returnSigned");
                        const auto returnName = requiredAttr<std::string>(op, "returnType");
                        if (!returnWidth || !returnSigned || !returnName)
                        {
                            continue;
                        }
                        const auto mapped = dpiType(*returnName, *returnWidth,
                                                    *returnSigned, op, "return");
                        if (!mapped)
                        {
                            continue;
                        }
                        returnType = mapped->first;
                        returnValue = DpiReturn{
                            .type = internType(mapped->first),
                            .abi = mapped->second,
                            .present = true,
                        };
                    }
                    const std::string symbol(op.symbolText());
                    if (symbol.empty())
                    {
                        error("DPI import symbol must be non-empty", opContext(op));
                        continue;
                    }
                    const StringId symbolId = internString(symbol);
                    const DpiImportId importId =
                        amGraph_.addDpiImport(symbolId, parameters, returnValue);
                    dpiImportByOperation_[operation.index] = DpiImportInfo{
                        .valid = true,
                        .symbol = symbol,
                        .symbolId = symbolId,
                        .importId = importId,
                        .parameters = std::move(parameterInfo),
                        .returnValue = returnValue,
                        .returnType = returnType,
                    };
                }
            }

            std::optional<VariableId> stateTarget(const Operation &port,
                                                  std::string_view attr,
                                                  OperationKind expected)
            {
                const auto symbol = requiredAttr<std::string>(port, attr);
                if (!symbol)
                {
                    return std::nullopt;
                }
                const OperationId declaration = graph_.findOperation(*symbol);
                if (!declaration.valid() || graph_.opKind(declaration) != expected ||
                    declaration.index >= stateByOperation_.size() ||
                    !stateByOperation_[declaration.index].valid())
                {
                    error("state port refers to missing or wrong-kind declaration '" +
                              *symbol + "'",
                          opContext(port));
                    return std::nullopt;
                }
                return stateByOperation_[declaration.index];
            }

            void createValueVariables()
            {
                for (ValueId value : graph_.values())
                {
                    const OperationId definition = graph_.valueDef(value);
                    if (definition.valid() && isStateReadPort(graph_.opKind(definition)))
                    {
                        const Operation read = graph_.getOperation(definition);
                        const auto target = stateTarget(
                            read,
                            read.kind() == OperationKind::kRegisterReadPort ? "regSymbol"
                                                                           : "latchSymbol",
                            read.kind() == OperationKind::kRegisterReadPort
                                ? OperationKind::kRegister
                                : OperationKind::kLatch);
                        if (!target)
                        {
                            continue;
                        }
                        const Type expected = typeForValue(value);
                        const Type actual = amGraph_.program().type(
                            amGraph_.program().variable(*target).type);
                        if (expected != actual)
                        {
                            error("state read result Type does not match its declaration",
                                  opContext(read));
                            continue;
                        }
                        valueMap_[value.index] = *target;
                        continue;
                    }

                    const Type type = typeForValue(value);
                    const TypeId typeId = internType(type);
                    InitId init = amGraph_.zeroInit();
                    if (definition.valid() &&
                        graph_.opKind(definition) == OperationKind::kConstant)
                    {
                        const Operation constant = graph_.getOperation(definition);
                        const auto parsed = constantInit(constant, value, typeId);
                        if (!parsed)
                        {
                            continue;
                        }
                        init = *parsed;
                    }
                    else if (definition.valid() &&
                             graph_.opKind(definition) == OperationKind::kArrayLaneConst)
                    {
                        const Operation laneConst = graph_.getOperation(definition);
                        const auto parsed = arrayLaneConstInit(laneConst, value, typeId);
                        if (!parsed)
                        {
                            continue;
                        }
                        init = *parsed;
                    }
                    std::optional<StringId> label;
                    if (exposedValues_.contains(value.index))
                    {
                        label = internString(graph_.symbolText(graph_.valueSymbol(value)));
                    }
                    VariableRole role = declaredValueIndices_.contains(value.index)
                                            ? VariableRole::Observable
                                            : VariableRole::None;
                    valueMap_[value.index] = addVariable(typeId, init, label, role);
                }
            }

            VariableId mappedValue(ValueId value, const Operation &user)
            {
                if (!value.valid() || value.index >= valueMap_.size() ||
                    !valueMap_[value.index].valid())
                {
                    error("operation refers to an unlowered Value", opContext(user));
                    return VariableId::invalid();
                }
                return valueMap_[value.index];
            }

            // Commit Blocks read their operands at the execution point, but a
            // state operand read there must observe the value from before this
            // round's commit phase (read-old), not a state written earlier in
            // the same commit sweep. Route such operands through a snapshot fed
            // by a plain compute Assign: it settles in the compute phase and is
            // reactivated by the scheduler's normal reader activation whenever
            // the source state changes.
            VariableId preCommitValue(ValueId value, VariableId source,
                                      const Operation &user)
            {
                if (!source.valid())
                {
                    return source;
                }
                const OperationId definition = graph_.valueDef(value);
                if (!definition.valid() ||
                    graph_.opKind(definition) != OperationKind::kRegisterReadPort)
                {
                    return source;
                }
                if (const auto found = preCommitSnapshots_.find(source.value);
                    found != preCommitSnapshots_.end())
                {
                    return found->second;
                }

                const TypeId type = amGraph_.program().variable(source).type;
                const VariableId snapshot = addVariable(type, amGraph_.undefInit());
                preCommitSnapshots_.emplace(source.value, snapshot);
                const std::array<VariableId, 1> results{snapshot};
                const std::array<VariableId, 1> operands{source};
                // AM clock-domain helper with no gsim node counterpart:
                // stays unowned even while a node-owned write op lowers.
                const ScopedGsimNodeId unowned(currentGsimNodeId_, -1);
                addInstruction(Opcode::Assign, results, operands);
                ++assignPreCommitCount_;
                return snapshot;
            }

            void createInterface()
            {
                for (const grh::Port &port : graph_.inputPorts())
                {
                    const VariableId input = mappedValueForInterface(port.value, port.name);
                    if (!input.valid())
                    {
                        continue;
                    }
                    interface_.ports.push_back(PortBinding{
                        .name = internString(port.name),
                        .direction = PortDirection::Input,
                        .input = input,
                    });
                    addRole(input, VariableRole::ExternalInput);
                }
                for (const grh::Port &port : graph_.outputPorts())
                {
                    const VariableId output = mappedValueForInterface(port.value, port.name);
                    if (!output.valid())
                    {
                        continue;
                    }
                    interface_.ports.push_back(PortBinding{
                        .name = internString(port.name),
                        .direction = PortDirection::Output,
                        .output = output,
                    });
                    addRole(output, VariableRole::ExternalOutput | VariableRole::Observable);
                }
                for (const grh::InoutPort &port : graph_.inoutPorts())
                {
                    const VariableId input = mappedValueForInterface(port.in, port.name);
                    const VariableId output = mappedValueForInterface(port.out, port.name);
                    const VariableId enable = mappedValueForInterface(port.oe, port.name);
                    if (!input.valid() || !output.valid() || !enable.valid())
                    {
                        continue;
                    }
                    interface_.ports.push_back(PortBinding{
                        .name = internString(port.name),
                        .direction = PortDirection::Inout,
                        .input = input,
                        .output = output,
                        .outputEnable = enable,
                    });
                    addRole(input, VariableRole::ExternalInput);
                    addRole(output, VariableRole::ExternalOutput | VariableRole::Observable);
                    addRole(enable, VariableRole::ExternalOutput | VariableRole::Observable);
                }

                std::set<std::pair<uint32_t, uint32_t>> declared;
                for (grh::SymbolId symbol : graph_.declaredSymbols())
                {
                    VariableId variable;
                    if (const ValueId value = graph_.findValue(symbol); value.valid() &&
                        value.index < valueMap_.size())
                    {
                        variable = valueMap_[value.index];
                    }
                    else if (const OperationId operation = graph_.findOperation(symbol);
                             operation.valid() && operation.index < stateByOperation_.size())
                    {
                        variable = stateByOperation_[operation.index];
                    }
                    if (!variable.valid())
                    {
                        continue;
                    }
                    const StringId label = internString(graph_.symbolText(symbol));
                    if (declared.emplace(variable.value, label.value).second)
                    {
                        interface_.declaredVariables.push_back(
                            VariableLabel{.variable = variable, .label = label});
                        addRole(variable, VariableRole::Observable);
                    }
                }
            }

            VariableId mappedValueForInterface(ValueId value, std::string_view portName)
            {
                if (!value.valid() || value.index >= valueMap_.size() ||
                    !valueMap_[value.index].valid())
                {
                    error("port refers to an unlowered Value", std::string(portName));
                    return VariableId::invalid();
                }
                return valueMap_[value.index];
            }

            bool requireShape(const Operation &op, std::size_t operands,
                              std::size_t results)
            {
                if (op.operands().size() != operands || op.results().size() != results)
                {
                    error("operation has invalid operand/result arity", opContext(op));
                    return false;
                }
                return true;
            }

            bool requireLogic(ValueId value, const Operation &op,
                              std::optional<uint32_t> width = std::nullopt)
            {
                if (graph_.valueType(value) != ValueType::Logic ||
                    (width && graph_.valueWidth(value) != static_cast<int32_t>(*width)))
                {
                    error("operation requires a Logic Value" +
                              (width ? " of width " + std::to_string(*width) : std::string()),
                          opContext(op));
                    return false;
                }
                return true;
            }

            VariableId coerceToType(VariableId source, const Type &sourceType,
                                    const Type &targetType)
            {
                if (sourceType == targetType)
                {
                    return source;
                }
                if (sourceType.kind != TypeKind::BitVector ||
                    targetType.kind != TypeKind::BitVector)
                {
                    return VariableId::invalid();
                }
                // Coercing a constant is itself a constant: resize the literal
                // instead of emitting a runtime Assign (NO0010 lowering fold).
                if (!disableLoweringConstFold_)
                {
                    if (std::optional<std::vector<uint64_t>> words =
                            constantWordsOf(source))
                    {
                        ++lowerConstFoldCount_;
                        return internConstant(
                            targetType,
                            detail::resizedWords(*words, sourceType.bitWidth,
                                                 targetType.bitWidth,
                                                 sourceType.signedness));
                    }
                }
                const VariableId converted = addVariable(targetType, amGraph_.zeroInit());
                const std::array<VariableId, 1> results{converted};
                const std::array<VariableId, 1> operands{source};
                addInstruction(Opcode::Assign, results, operands);
                ++assignCoerceCount_;
                ++freshTemporaryCount_;
                return converted;
            }

            VariableId zeroConstant(const Type &type)
            {
                const auto found = zeroConstants_.find(type);
                if (found != zeroConstants_.end())
                {
                    return found->second;
                }
                if (type.kind != TypeKind::BitVector)
                {
                    return VariableId::invalid();
                }
                const TypeId typeId = internType(type);
                const std::size_t wordCount =
                    (static_cast<std::size_t>(type.bitWidth) + 63) / 64;
                const std::vector<uint64_t> words(wordCount, 0);
                const LiteralId literal = amGraph_.addBitLiteral(typeId, words);
                const VariableId variable =
                    addVariable(typeId, amGraph_.addConstantInit(literal));
                zeroConstants_.emplace(type, variable);
                return variable;
            }

            VariableId lowerCondition(ValueId value, const Operation &op)
            {
                if (!requireLogic(value, op))
                {
                    return VariableId::invalid();
                }
                const Type sourceType = typeForValue(value);
                const VariableId source = mappedValue(value, op);
                if (!source.valid())
                {
                    return VariableId::invalid();
                }
                if (sourceType.bitWidth == 1)
                {
                    return source;
                }
                const VariableId zero = zeroConstant(sourceType);
                const VariableId condition =
                    addVariable(Type::bitVector(1), amGraph_.zeroInit());
                const std::array<VariableId, 1> results{condition};
                const std::array<VariableId, 2> operands{source, zero};
                addInstruction(Opcode::Ne, results, operands);
                ++freshTemporaryCount_;
                return condition;
            }

            std::optional<Type> nativeCombinationalResult(const Operation &op)
            {
                const auto operands = op.operands();
                const auto results = op.results();
                if (results.size() != 1)
                {
                    return std::nullopt;
                }
                const Type resultType = typeForValue(results.front());
                auto commonSign = [&](ValueId lhs, ValueId rhs) {
                    return graph_.valueSigned(lhs) && graph_.valueSigned(rhs)
                               ? Signedness::Signed
                               : Signedness::Unsigned;
                };
                switch (op.kind())
                {
                case OperationKind::kAssign:
                    return resultType;
                case OperationKind::kAdd:
                case OperationKind::kSub:
                    // The declared output width is authoritative: FIRRTL-derived
                    // graphs extend add/sub by one carry bit (operands narrower
                    // than the declared result), SV-derived graphs may truncate.
                    // Operand-width arithmetic loses the carry and cannot be
                    // recovered by the downstream widening assign.
                    return Type::bitVector(resultType.bitWidth,
                                           commonSign(operands[0], operands[1]));
                case OperationKind::kAnd:
                case OperationKind::kOr:
                case OperationKind::kXor:
                case OperationKind::kXnor:
                    return Type::bitVector(
                        static_cast<uint32_t>(std::max(graph_.valueWidth(operands[0]),
                                                       graph_.valueWidth(operands[1]))),
                        commonSign(operands[0], operands[1]));
                case OperationKind::kMul:
                {
                    const uint64_t width = static_cast<uint64_t>(graph_.valueWidth(operands[0])) +
                                           graph_.valueWidth(operands[1]);
                    if (width > std::numeric_limits<uint32_t>::max())
                    {
                        error("multiply result width exceeds AM Type limit", opContext(op));
                        return std::nullopt;
                    }
                    return Type::bitVector(static_cast<uint32_t>(width),
                                           commonSign(operands[0], operands[1]));
                }
                case OperationKind::kDiv:
                case OperationKind::kMod:
                    return Type::bitVector(
                        static_cast<uint32_t>(graph_.valueWidth(operands[0])),
                        commonSign(operands[0], operands[1]));
                case OperationKind::kEq:
                case OperationKind::kNe:
                case OperationKind::kCaseEq:
                case OperationKind::kCaseNe:
                case OperationKind::kWildcardEq:
                case OperationKind::kWildcardNe:
                case OperationKind::kLt:
                case OperationKind::kLe:
                case OperationKind::kGt:
                case OperationKind::kGe:
                case OperationKind::kLogicAnd:
                case OperationKind::kLogicOr:
                case OperationKind::kLogicNot:
                case OperationKind::kReduceAnd:
                case OperationKind::kReduceNand:
                case OperationKind::kReduceOr:
                case OperationKind::kReduceNor:
                case OperationKind::kReduceXor:
                case OperationKind::kReduceXnor:
                    return Type::bitVector(1, Signedness::Unsigned);
                case OperationKind::kNot:
                    return typeForValue(operands[0]);
                case OperationKind::kShl:
                case OperationKind::kLShr:
                case OperationKind::kAShr:
                    return Type::bitVector(
                        resultType.bitWidth, typeForValue(operands[0]).signedness);
                case OperationKind::kMux:
                    return Type::bitVector(resultType.bitWidth,
                                           commonSign(operands[1], operands[2]));
                case OperationKind::kConcat:
                {
                    uint64_t width = 0;
                    for (ValueId operand : operands)
                    {
                        width += static_cast<uint32_t>(graph_.valueWidth(operand));
                    }
                    if (width > std::numeric_limits<uint32_t>::max())
                    {
                        error("concat result width exceeds AM Type limit", opContext(op));
                        return std::nullopt;
                    }
                    return Type::bitVector(static_cast<uint32_t>(width),
                                           Signedness::Unsigned);
                }
                case OperationKind::kReplicate:
                case OperationKind::kSliceStatic:
                case OperationKind::kSliceDynamic:
                case OperationKind::kSliceArray:
                    return Type::bitVector(resultType.bitWidth, Signedness::Unsigned);
                case OperationKind::kArrayMux:
                    return Type::bitVector(resultType.bitWidth,
                                           commonSign(operands[1], operands[2]));
                case OperationKind::kArrayReduceOr:
                case OperationKind::kArrayReduceAnd:
                case OperationKind::kArrayReduceXor:
                    return Type::bitVector(1, Signedness::Unsigned);
                case OperationKind::kArrayReduceLanesOr:
                case OperationKind::kArrayReduceLanesAnd:
                case OperationKind::kArrayReduceLanesXor:
                    return Type::bitVector(resultType.bitWidth, Signedness::Unsigned);
                case OperationKind::kArrayBroadcast:
                case OperationKind::kArrayOnehot:
                    return resultType;
                default:
                    return std::nullopt;
                }
            }

            std::optional<Opcode> combinationalOpcode(OperationKind kind) const
            {
                switch (kind)
                {
                case OperationKind::kAssign: return Opcode::Assign;
                case OperationKind::kAdd: return Opcode::Add;
                case OperationKind::kSub: return Opcode::Sub;
                case OperationKind::kMul: return Opcode::Mul;
                case OperationKind::kDiv: return Opcode::Div;
                case OperationKind::kMod: return Opcode::Mod;
                case OperationKind::kAnd: return Opcode::And;
                case OperationKind::kOr: return Opcode::Or;
                case OperationKind::kXor: return Opcode::Xor;
                case OperationKind::kXnor: return Opcode::Xnor;
                case OperationKind::kNot: return Opcode::Not;
                case OperationKind::kEq:
                case OperationKind::kCaseEq:
                case OperationKind::kWildcardEq: return Opcode::Eq;
                case OperationKind::kNe:
                case OperationKind::kCaseNe:
                case OperationKind::kWildcardNe: return Opcode::Ne;
                case OperationKind::kLt: return Opcode::Lt;
                case OperationKind::kLe: return Opcode::Le;
                case OperationKind::kGt: return Opcode::Gt;
                case OperationKind::kGe: return Opcode::Ge;
                case OperationKind::kLogicAnd: return Opcode::LogicAnd;
                case OperationKind::kLogicOr: return Opcode::LogicOr;
                case OperationKind::kLogicNot: return Opcode::LogicNot;
                case OperationKind::kReduceAnd: return Opcode::ReduceAnd;
                case OperationKind::kReduceNand: return Opcode::ReduceNand;
                case OperationKind::kReduceOr: return Opcode::ReduceOr;
                case OperationKind::kReduceNor: return Opcode::ReduceNor;
                case OperationKind::kReduceXor: return Opcode::ReduceXor;
                case OperationKind::kReduceXnor: return Opcode::ReduceXnor;
                case OperationKind::kShl: return Opcode::Shl;
                case OperationKind::kLShr: return Opcode::LogicalShr;
                case OperationKind::kAShr: return Opcode::ArithmeticShr;
                case OperationKind::kMux: return Opcode::Mux;
                case OperationKind::kConcat: return Opcode::Concat;
                case OperationKind::kReplicate: return Opcode::Replicate;
                case OperationKind::kSliceStatic: return Opcode::SliceStatic;
                case OperationKind::kSliceDynamic: return Opcode::SliceDynamic;
                case OperationKind::kSliceArray: return Opcode::SliceArray;
                case OperationKind::kArrayMux: return Opcode::ArrayMux;
                case OperationKind::kArrayReduceOr: return Opcode::ArrayReduceOr;
                case OperationKind::kArrayReduceAnd: return Opcode::ArrayReduceAnd;
                case OperationKind::kArrayReduceXor: return Opcode::ArrayReduceXor;
                case OperationKind::kArrayBroadcast: return Opcode::ArrayBroadcast;
                case OperationKind::kArrayOnehot: return Opcode::ArrayOnehot;
                case OperationKind::kArrayReduceLanesOr: return Opcode::ArrayReduceLanesOr;
                case OperationKind::kArrayReduceLanesAnd: return Opcode::ArrayReduceLanesAnd;
                case OperationKind::kArrayReduceLanesXor: return Opcode::ArrayReduceLanesXor;
                default: return std::nullopt;
                }
            }

            void lowerCombinational(const Operation &op, Opcode opcode)
            {
                if (opcode == Opcode::Assign)
                {
                    ++assignFromGrhCount_;
                }
                const auto operands = op.operands();
                const auto results = op.results();
                std::size_t expectedOperands = 0;
                bool variadic = false;
                switch (opcode)
                {
                case Opcode::Not:
                case Opcode::LogicNot:
                case Opcode::ReduceAnd:
                case Opcode::ReduceNand:
                case Opcode::ReduceOr:
                case Opcode::ReduceNor:
                case Opcode::ReduceXor:
                case Opcode::ReduceXnor:
                case Opcode::Assign:
                case Opcode::Replicate:
                case Opcode::SliceStatic:
                case Opcode::ArrayReduceOr:
                case Opcode::ArrayReduceAnd:
                case Opcode::ArrayReduceXor:
                case Opcode::ArrayBroadcast:
                case Opcode::ArrayOnehot:
                case Opcode::ArrayReduceLanesOr:
                case Opcode::ArrayReduceLanesAnd:
                case Opcode::ArrayReduceLanesXor:
                    expectedOperands = 1;
                    break;
                case Opcode::Mux:
                case Opcode::ArrayMux:
                    expectedOperands = 3;
                    break;
                case Opcode::Concat:
                    variadic = true;
                    break;
                default:
                    expectedOperands = 2;
                    break;
                }
                if (results.size() != 1 ||
                    (variadic ? operands.empty() : operands.size() != expectedOperands))
                {
                    error("combinational operation has invalid arity", opContext(op));
                    return;
                }
                bool logic = requireLogic(results.front(), op);
                for (ValueId operand : operands)
                {
                    logic = requireLogic(operand, op) && logic;
                }
                if (!logic)
                {
                    return;
                }
                if (opcode == Opcode::Mux && graph_.valueWidth(operands[0]) != 1)
                {
                    error("kMux condition must be one bit", opContext(op));
                    return;
                }

                const Type resultType = typeForValue(results.front());
                const auto nativeType = nativeCombinationalResult(op);
                if (!nativeType)
                {
                    return;
                }
                if (opcode == Opcode::Concat && nativeType->bitWidth != resultType.bitWidth)
                {
                    error("kConcat result width does not equal the sum of operand widths",
                          opContext(op));
                    return;
                }
                if (opcode == Opcode::Replicate)
                {
                    const auto rep = requiredAttr<int64_t>(op, "rep");
                    const uint32_t sourceWidth =
                        static_cast<uint32_t>(graph_.valueWidth(operands.front()));
                    if (!rep || *rep <= 0 ||
                        static_cast<uint64_t>(sourceWidth) * static_cast<uint64_t>(*rep) !=
                            resultType.bitWidth)
                    {
                        error("kReplicate rep does not match its result width", opContext(op));
                        return;
                    }
                }
                uint32_t sliceLsb = 0;
                if (opcode == Opcode::SliceStatic)
                {
                    const auto start = requiredAttr<int64_t>(op, "sliceStart");
                    const auto end = requiredAttr<int64_t>(op, "sliceEnd");
                    if (!start || !end || *start < 0 || *end < *start ||
                        static_cast<uint64_t>(*end) >=
                            static_cast<uint32_t>(graph_.valueWidth(operands.front())) ||
                        static_cast<uint64_t>(*end - *start + 1) != resultType.bitWidth ||
                        static_cast<uint64_t>(*start) >
                            std::numeric_limits<uint32_t>::max())
                    {
                        error("kSliceStatic range does not match its result", opContext(op));
                        return;
                    }
                    sliceLsb = static_cast<uint32_t>(*start);
                }
                if (opcode == Opcode::SliceDynamic || opcode == Opcode::SliceArray)
                {
                    const auto width = requiredAttr<int64_t>(op, "sliceWidth");
                    if (!width || *width <= 0 ||
                        static_cast<uint64_t>(*width) != resultType.bitWidth)
                    {
                        error("dynamic/array sliceWidth does not match its result",
                              opContext(op));
                        return;
                    }
                }
                if (opcode == Opcode::SliceArray &&
                    static_cast<uint32_t>(graph_.valueWidth(operands.front())) %
                            resultType.bitWidth !=
                        0)
                {
                    error("kSliceArray element width does not divide its base width",
                          opContext(op));
                    return;
                }
                if (opcode == Opcode::ArrayMux)
                {
                    const uint64_t selectWidth =
                        static_cast<uint32_t>(graph_.valueWidth(operands[0]));
                    const uint64_t dataWidth =
                        static_cast<uint32_t>(graph_.valueWidth(operands[1]));
                    if (selectWidth == 0 ||
                        dataWidth !=
                            static_cast<uint32_t>(graph_.valueWidth(operands[2])) ||
                        dataWidth != resultType.bitWidth ||
                        dataWidth % selectWidth != 0)
                    {
                        error("kArrayMux lane width does not match its select vector",
                              opContext(op));
                        return;
                    }
                }
                if (opcode == Opcode::ArrayReduceOr || opcode == Opcode::ArrayReduceAnd ||
                    opcode == Opcode::ArrayReduceXor)
                {
                    const auto elemWidth = requiredAttr<int64_t>(op, "elemWidth");
                    if (!elemWidth || *elemWidth <= 0 ||
                        static_cast<uint32_t>(graph_.valueWidth(operands.front())) %
                                static_cast<uint64_t>(*elemWidth) !=
                            0)
                    {
                        error("kArrayReduce elemWidth does not divide its data width",
                              opContext(op));
                        return;
                    }
                }
                if (opcode == Opcode::ArrayReduceLanesOr || opcode == Opcode::ArrayReduceLanesAnd ||
                    opcode == Opcode::ArrayReduceLanesXor)
                {
                    const auto elemWidth = requiredAttr<int64_t>(op, "elemWidth");
                    if (!elemWidth || *elemWidth <= 0 ||
                        resultType.bitWidth * static_cast<uint64_t>(*elemWidth) !=
                            static_cast<uint32_t>(graph_.valueWidth(operands.front())))
                    {
                        error("kArrayReduceLanes result width times elemWidth does not equal its data width",
                              opContext(op));
                        return;
                    }
                }
                if (opcode == Opcode::ArrayBroadcast)
                {
                    const auto rows = requiredAttr<int64_t>(op, "rows");
                    const uint64_t sourceWidth =
                        static_cast<uint32_t>(graph_.valueWidth(operands.front()));
                    if (!rows || *rows <= 0 ||
                        sourceWidth * static_cast<uint64_t>(*rows) != resultType.bitWidth)
                    {
                        error("kArrayBroadcast rows does not match its result width",
                              opContext(op));
                        return;
                    }
                }
                if (opcode == Opcode::ArrayOnehot)
                {
                    const auto rows = requiredAttr<int64_t>(op, "rows");
                    if (!rows || *rows <= 0 ||
                        static_cast<uint64_t>(*rows) != resultType.bitWidth)
                    {
                        error("kArrayOnehot rows does not match its result width",
                              opContext(op));
                        return;
                    }
                }

                std::vector<VariableId> loweredOperands;
                loweredOperands.reserve(operands.size());
                for (ValueId operand : operands)
                {
                    loweredOperands.push_back(mappedValue(operand, op));
                }
                if (std::any_of(loweredOperands.begin(), loweredOperands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                if (opcode == Opcode::Shl || opcode == Opcode::LogicalShr ||
                    opcode == Opcode::ArithmeticShr)
                {
                    loweredOperands[0] = coerceToType(
                        loweredOperands[0], typeForValue(operands[0]), *nativeType);
                    if (!loweredOperands[0].valid())
                    {
                        error("shift operand cannot be coerced to its native type",
                              opContext(op));
                        return;
                    }
                }
                if (opcode == Opcode::Add || opcode == Opcode::Sub)
                {
                    // FIRRTL-derived graphs declare add/sub results one carry
                    // bit wider than their operands; extend the operands
                    // explicitly so the AM instruction keeps its operand-width
                    // Type signature while the carry is preserved.
                    for (std::size_t index = 0; index < loweredOperands.size();
                         ++index)
                    {
                        const Type *sourceType = variableTypeOf(loweredOperands[index]);
                        if (!sourceType)
                        {
                            return;
                        }
                        loweredOperands[index] = coerceToType(
                            loweredOperands[index], *sourceType, *nativeType);
                        if (!loweredOperands[index].valid())
                        {
                            error("add/sub operand cannot be coerced to its declared width",
                                  opContext(op));
                            return;
                        }
                    }
                }
                // Lowering-time constant fold (NO0010): a pure op over
                // literal operands lowers to a shared constant variable
                // instead of a runtime instruction. gsim's executable-GRH
                // export materializes per-element vector-register writes as
                // full-width slice/concat rebuild chains over constant bases;
                // without this fold each such chain costs O(width) word
                // traffic per element per activation in the emitted model.
                // Results with interface roles keep their declared variable
                // (the label binding must survive). The use-order guard keeps
                // the valueMap_ repoint safe on non-topological op orderings:
                // lowerOpOrdinal_ was already bumped for the current op.
                if (!disableLoweringConstFold_ && resultType.kind == TypeKind::BitVector &&
                    !exposedValues_.contains(results.front().index) &&
                    !declaredValueIndices_.contains(results.front().index) &&
                    results.front().index < firstUseOrdinal_.size() &&
                    firstUseOrdinal_[results.front().index] >= lowerOpOrdinal_)
                {
                    std::vector<std::vector<uint64_t>> wordStorage;
                    wordStorage.reserve(loweredOperands.size());
                    std::vector<detail::ConstOperand> constOperands;
                    constOperands.reserve(loweredOperands.size());
                    bool allConstant = !loweredOperands.empty();
                    for (VariableId operand : loweredOperands)
                    {
                        const Type *operandType = variableTypeOf(operand);
                        std::optional<std::vector<uint64_t>> words =
                            operandType ? constantWordsOf(operand) : std::nullopt;
                        if (!words)
                        {
                            allConstant = false;
                            break;
                        }
                        wordStorage.push_back(std::move(*words));
                        constOperands.push_back(detail::ConstOperand{
                            .type = *operandType, .words = wordStorage.back()});
                    }
                    if (allConstant)
                    {
                        std::vector<uint64_t> folded;
                        if (detail::evaluatePure(opcode, *nativeType, constOperands,
                                                 sliceLsb, folded))
                        {
                            if (*nativeType != resultType)
                            {
                                // Mirror the native -> declared bridge Assign
                                // on the constant itself.
                                folded = detail::resizedWords(
                                    folded, nativeType->bitWidth,
                                    resultType.bitWidth, nativeType->signedness);
                            }
                            valueMap_[results.front().index] =
                                internConstant(resultType, std::move(folded));
                            ++lowerConstFoldCount_;
                            return;
                        }
                    }
                }
                const VariableId destination = mappedValue(results.front(), op);
                if (!destination.valid())
                {
                    return;
                }
                VariableId nativeDestination = destination;
                if (*nativeType != resultType)
                {
                    nativeDestination = addVariable(*nativeType, amGraph_.zeroInit());
                    ++freshTemporaryCount_;
                }
                const std::array<VariableId, 1> loweredResults{nativeDestination};
                const InstructionId instruction =
                    addInstruction(opcode, loweredResults, loweredOperands);
                if (opcode == Opcode::SliceStatic)
                {
                    amGraph_.setSliceStaticAttributes(instruction, sliceLsb);
                }
                if (nativeDestination != destination)
                {
                    const std::array<VariableId, 1> finalResults{destination};
                    const std::array<VariableId, 1> finalOperands{nativeDestination};
                    addInstruction(Opcode::Assign, finalResults, finalOperands);
                    ++assignResultBridgeCount_;
                }
            }

            std::optional<std::vector<VariableId>> lowerEvents(
                const Operation &op, std::size_t eventStart,
                const std::vector<std::string> &edges)
            {
                const auto operands = op.operands();
                if (eventStart > operands.size() ||
                    operands.size() - eventStart != edges.size())
                {
                    error("eventEdge count does not match raw event operands", opContext(op));
                    return std::nullopt;
                }
                std::vector<VariableId> events;
                events.reserve(edges.size());
                const Type eventType = Type::bitVector(1, Signedness::Unsigned);
                for (std::size_t index = 0; index < edges.size(); ++index)
                {
                    const ValueId raw = operands[eventStart + index];
                    if (!requireLogic(raw, op, 1))
                    {
                        return std::nullopt;
                    }
                    Opcode opcode;
                    if (edges[index] == "posedge")
                    {
                        opcode = Opcode::ChangedPos;
                    }
                    else if (edges[index] == "negedge")
                    {
                        opcode = Opcode::ChangedNeg;
                    }
                    else
                    {
                        error("unsupported raw event edge '" + edges[index] + "'",
                              opContext(op));
                        return std::nullopt;
                    }
                    const VariableId rawVariable = mappedValue(raw, op);
                    if (!rawVariable.valid())
                    {
                        return std::nullopt;
                    }
                    // Share one detector per (edge kind, watched variable): every
                    // consumer observes the same raw transitions, so a single
                    // Changed instruction with its private old baseline is enough.
                    const auto memoKey = std::make_pair(opcode, rawVariable.value);
                    const auto found = eventDetectorMemo_.find(memoKey);
                    if (found != eventDetectorMemo_.end())
                    {
                        events.push_back(found->second);
                        continue;
                    }
                    const Type rawType = typeForValue(raw);
                    const VariableId old = addVariable(rawType, amGraph_.undefInit());
                    const VariableId event = addVariable(eventType, amGraph_.zeroInit());
                    const std::array<VariableId, 1> results{event};
                    const std::array<VariableId, 2> eventOperands{rawVariable, old};
                    // AM clock-domain detector with no gsim node
                    // counterpart: stays unowned (-1).
                    const ScopedGsimNodeId unowned(currentGsimNodeId_, -1);
                    addInstruction(opcode, results, eventOperands);
                    eventDetectorMemo_.emplace(memoKey, event);
                    events.push_back(event);
                }
                return events;
            }

            uint32_t stateOrderGroup(VariableId target)
            {
                const auto found = stateOrderGroups_.find(target.value);
                if (found != stateOrderGroups_.end())
                {
                    return found->second;
                }
                const uint32_t group = nextOrderedGroup_++;
                stateOrderGroups_.emplace(target.value, group);
                return group;
            }

            // Materialize the collected state writes: writes to one target are
            // ordered exactly like the legacy per-target effect order, then
            // partitioned by event signature. Every write stays one instruction
            // per write (register/latch/memory alike), keeping its cond/mask
            // operands; same-target writes commit in ordinal order inside their
            // commit block, so a later write blends onto the earlier one's
            // result (a disabled write is suppressed outright, so no read-old
            // value is ever needed).
            void materializeStateWrites()
            {
                std::map<uint32_t, std::vector<PendingStateWrite>> writesByTarget;
                for (PendingStateWrite &write : pendingStateWrites_)
                {
                    writesByTarget[write.target.value].push_back(std::move(write));
                }

                for (auto &[targetValue, writes] : writesByTarget)
                {
                    std::map<uint32_t, std::vector<PendingStateWrite>> explicitGroups;
                    for (const PendingStateWrite &write : writes)
                    {
                        if (write.explicitOrder)
                        {
                            explicitGroups[write.explicitOrder->group].push_back(write);
                        }
                    }
                    for (auto &[explicitGroup, members] : explicitGroups)
                    {
                        (void)explicitGroup;
                        std::sort(members.begin(), members.end(),
                                  [](const PendingStateWrite &lhs,
                                     const PendingStateWrite &rhs) {
                                      return lhs.explicitOrder->ordinal <
                                             rhs.explicitOrder->ordinal;
                                  });
                    }

                    std::vector<PendingStateWrite> ordered;
                    ordered.reserve(writes.size());
                    std::unordered_set<uint32_t> emittedExplicitGroups;
                    for (const PendingStateWrite &write : writes)
                    {
                        if (!write.explicitOrder)
                        {
                            ordered.push_back(write);
                            continue;
                        }
                        const uint32_t explicitGroup = write.explicitOrder->group;
                        if (!emittedExplicitGroups.insert(explicitGroup).second)
                        {
                            continue;
                        }
                        const std::vector<PendingStateWrite> &members =
                            explicitGroups.at(explicitGroup);
                        ordered.insert(ordered.end(), members.begin(), members.end());
                    }

                    // Partition by event signature, keeping first-appearance
                    // order of the signatures and in-signature write order.
                    std::vector<std::vector<PendingStateWrite>> signatureGroups;
                    std::map<std::vector<uint32_t>, std::size_t> signatureIndex;
                    for (PendingStateWrite &write : ordered)
                    {
                        std::vector<uint32_t> key;
                        key.reserve(write.events.size());
                        for (const VariableId event : write.events)
                        {
                            key.push_back(event.value);
                        }
                        std::sort(key.begin(), key.end());
                        key.erase(std::unique(key.begin(), key.end()), key.end());
                        const auto [it, inserted] =
                            signatureIndex.try_emplace(std::move(key),
                                                       signatureGroups.size());
                        if (inserted)
                        {
                            signatureGroups.emplace_back();
                        }
                        signatureGroups[it->second].push_back(std::move(write));
                    }

                    const uint32_t group = stateOrderGroup(VariableId{targetValue});
                    uint32_t ordinal = 0;
                    for (const std::vector<PendingStateWrite> &signatureGroup :
                         signatureGroups)
                    {
                        emitSignatureWrites(signatureGroup, group, ordinal);
                    }
                }
                pendingStateWrites_.clear();
            }

            // Constant classification of a lowering-time variable: only
            // Constant-initialised bit-vector literals qualify (coerced or
            // computed values stay Dynamic). Drives the cond/mask variant
            // selection of the emitted state-write instructions.
            enum class ConstClass : uint8_t
            {
                Dynamic,
                Zero,
                AllOnes,
            };

            ConstClass classifyConstant(VariableId variable, uint32_t width) const
            {
                if (!variable.valid() || width == 0)
                {
                    return ConstClass::Dynamic;
                }
                const VariableRecord &record = amGraph_.program().variable(variable);
                const InitDescriptor &init = amGraph_.program().init(record.init);
                if (init.kind != InitKind::Constant)
                {
                    return ConstClass::Dynamic;
                }
                const LiteralView literal = amGraph_.program().literal(LiteralId{init.payload});
                const Type &literalType = amGraph_.program().type(literal.type);
                if (literalType.kind != TypeKind::BitVector)
                {
                    return ConstClass::Dynamic;
                }
                bool anyOne = false;
                bool anyZero = false;
                for (uint32_t bit = 0; bit < width; ++bit)
                {
                    const bool set = bit < literalType.bitWidth &&
                                     ((literal.words[bit / 64U] >> (bit % 64U)) & 1U) != 0;
                    anyOne = anyOne || set;
                    anyZero = anyZero || !set;
                    if (anyOne && anyZero)
                    {
                        return ConstClass::Dynamic;
                    }
                }
                return anyOne ? ConstClass::AllOnes : ConstClass::Zero;
            }

            bool isConstantOne(VariableId variable) const
            {
                return classifyConstant(variable, 1) == ConstClass::AllOnes;
            }

            bool isConstantZero(VariableId variable) const
            {
                return classifyConstant(variable, 1) == ConstClass::Zero;
            }

            // Full constant words of a lowering-time variable, normalized
            // (zero-extended) to the variable's declared width. Only
            // Constant-initialized bit-vector literals qualify; anything
            // computed at runtime stays unknown.
            std::optional<std::vector<uint64_t>> constantWordsOf(
                VariableId variable) const
            {
                if (!variable.valid() ||
                    variable.value >= amGraph_.program().variableCount())
                {
                    return std::nullopt;
                }
                const VariableRecord &record = amGraph_.program().variable(variable);
                const InitDescriptor &init = amGraph_.program().init(record.init);
                if (init.kind != InitKind::Constant)
                {
                    return std::nullopt;
                }
                const LiteralView literal =
                    amGraph_.program().literal(LiteralId{init.payload});
                const Type &literalType = amGraph_.program().type(literal.type);
                const Type &variableType = amGraph_.program().type(record.type);
                if (literalType.kind != TypeKind::BitVector ||
                    variableType.kind != TypeKind::BitVector)
                {
                    return std::nullopt;
                }
                return detail::resizedWords(literal.words, literalType.bitWidth,
                                            variableType.bitWidth,
                                            Signedness::Unsigned);
            }

            // Intern a compile-time constant as a shared Constant-initialized
            // variable (one representative per (type, value), like the
            // optimizer's constant canonicalization).
            VariableId internConstant(const Type &type, std::vector<uint64_t> words)
            {
                detail::normalizeWords(words, type.bitWidth);
                const auto key = std::make_pair(type, words);
                if (const auto found = foldedConstants_.find(key);
                    found != foldedConstants_.end())
                {
                    return found->second;
                }
                const TypeId typeId = internType(type);
                const LiteralId literal = amGraph_.addBitLiteral(typeId, words);
                const VariableId variable =
                    addVariable(typeId, amGraph_.addConstantInit(literal));
                foldedConstants_.emplace(std::make_pair(type, std::move(words)),
                                         variable);
                return variable;
            }

            VariableId addPureTyped(Opcode opcode, const Type &type,
                                    std::span<const VariableId> operands)
            {
                const VariableId result = addVariable(type, amGraph_.undefInit());
                const std::array<VariableId, 1> results{result};
                addInstruction(opcode, results, operands);
                return result;
            }

            VariableId pureMux(VariableId cond, VariableId whenTrue,
                               VariableId whenFalse, const Type &type)
            {
                const std::array<VariableId, 3> operands{cond, whenTrue, whenFalse};
                return addPureTyped(Opcode::Mux, type, operands);
            }

            void recordWriteEffect(InstructionId instruction, uint32_t group,
                                   uint32_t &ordinal)
            {
                amGraph_.orderedEffects().push_back(OrderedEffect{
                    .instruction = instruction,
                    .group = group,
                    .ordinal = ordinal++,
                });
                // Refine the write target's state kind from the write opcode,
                // mirroring the AmGraph::fromLinearProgram derivation so
                // native construction and ingestion agree exactly.
                const ProgramView program = amGraph_.program();
                const Opcode opcode = program.opcode(instruction);
                const OpcodeTraits traits = opcodeTraits(opcode);
                if (traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand)
                {
                    return;
                }
                const auto operands = program.operands(instruction);
                if (traits.stateTargetOperand >= operands.size())
                {
                    return;
                }
                const VariableId target = operands[traits.stateTargetOperand];
                if (!target.valid() || target.value >= amGraph_.variableCount())
                {
                    return;
                }
                AmValueFacts facts = amGraph_.valueFacts(target);
                switch (opcode)
                {
                case Opcode::LatchWrite:
                case Opcode::LatchWriteCond:
                case Opcode::LatchWriteMask:
                case Opcode::LatchWriteCondMask:
                    facts.stateKind = AmStateKind::Latch;
                    break;
                case Opcode::MemoryWrite:
                case Opcode::MemoryWriteCond:
                case Opcode::MemoryWriteMask:
                case Opcode::MemoryWriteCondMask:
                case Opcode::MemoryFill:
                case Opcode::MemoryWriteLanes:
                    facts.stateKind = AmStateKind::Memory;
                    break;
                default:
                    facts.stateKind = AmStateKind::Register;
                    break;
                }
                amGraph_.setValueFacts(target, facts);
            }

            // Registers/latches: one write instruction per pending write,
            // with the cond/mask kept as gating operands of the selected
            // opcode variant. "All branches miss -> keep the old value" is
            // carried by NOT writing (a false cond skips the commit), so no
            // read-old snapshot, blend, or self-mux is materialized. Writes
            // in one signature group commit in ordinal order inside their
            // commit Block, so a later write overwrites/blends onto the
            // earlier one's result -- the same value the old folded
            // snapshot+blend+mux chain computed. A write that can never
            // change the target (constant-0 cond or mask) emits nothing.
            //
            // NO0010 merge: a run of ordered writes that share one cond and
            // carry constant mask+data collapses into a single write whose
            // constant image is the sequential blend folded at lowering time
            // (gsim exports per-element vector-register writes as thousands
            // of full-width constant-masked ports; the merge restores the
            // single gated write gsim emits for the same update).
            void emitScalarWrites(
                const std::vector<PendingStateWrite> &signatureGroup, uint32_t group,
                uint32_t &ordinal)
            {
                const PendingStateWrite &head = signatureGroup.front();
                const Type targetType =
                    amGraph_.program().type(amGraph_.program().variable(head.target).type);

                const auto emitOne = [&](const PendingStateWrite &write) {
                    // Commit instructions inherit the source write op's gsim
                    // node id (NO0006).
                    const ScopedGsimNodeId writeNode(currentGsimNodeId_,
                                                     write.gsimNodeId);
                    const ConstClass maskClass =
                        classifyConstant(write.mask, targetType.bitWidth);
                    const bool hasCond = !isConstantOne(write.cond);
                    const bool hasMask = maskClass != ConstClass::AllOnes;
                    Opcode opcode = write.opcode;
                    if (write.opcode == Opcode::RegisterWrite)
                    {
                        opcode = hasCond ? (hasMask ? Opcode::RegisterWriteCondMask
                                                    : Opcode::RegisterWriteCond)
                                         : (hasMask ? Opcode::RegisterWriteMask
                                                    : Opcode::RegisterWrite);
                    }
                    else
                    {
                        opcode = hasCond ? (hasMask ? Opcode::LatchWriteCondMask
                                                    : Opcode::LatchWriteCond)
                                         : (hasMask ? Opcode::LatchWriteMask
                                                    : Opcode::LatchWrite);
                    }
                    std::vector<VariableId> operands;
                    operands.reserve(4 + write.events.size());
                    if (hasCond)
                    {
                        operands.push_back(write.cond);
                    }
                    if (hasMask)
                    {
                        operands.push_back(write.mask);
                    }
                    operands.push_back(write.data);
                    operands.push_back(write.target);
                    operands.insert(operands.end(), write.events.begin(),
                                    write.events.end());
                    const InstructionId instruction = addInstruction(opcode, {}, operands);
                    recordWriteEffect(instruction, group, ordinal);
                };

                // Run state for the constant-image merge.
                bool mergeActive = false;
                Opcode mergeOpcode = Opcode::RegisterWrite;
                VariableId mergeCond = VariableId::invalid();
                int64_t mergeGsimNodeId = -1;
                std::string mergeContext;
                std::vector<VariableId> mergeEvents;
                std::vector<uint64_t> mergeMaskWords;
                std::vector<uint64_t> mergeDataWords;
                uint64_t mergeMembers = 0;

                const auto flushMerge = [&]() {
                    if (!mergeActive)
                    {
                        return;
                    }
                    mergeActive = false;
                    if (mergeMembers > 1)
                    {
                        mergedStateWriteCount_ += mergeMembers - 1;
                    }
                    if (detail::isZero(mergeMaskWords))
                    {
                        // The run never changes a bit: no effect, ever.
                        return;
                    }
                    PendingStateWrite merged;
                    merged.opcode = mergeOpcode;
                    merged.target = head.target;
                    merged.cond = mergeCond;
                    merged.mask = internConstant(targetType, mergeMaskWords);
                    merged.data = internConstant(targetType, mergeDataWords);
                    merged.events = mergeEvents;
                    merged.context = mergeContext;
                    merged.gsimNodeId = mergeGsimNodeId;
                    emitOne(merged);
                };

                for (const PendingStateWrite &write : signatureGroup)
                {
                    if (isConstantZero(write.cond) ||
                        classifyConstant(write.mask, targetType.bitWidth) ==
                            ConstClass::Zero)
                    {
                        // No effect, ever: drop without breaking the run.
                        continue;
                    }
                    std::optional<std::vector<uint64_t>> maskWords;
                    std::optional<std::vector<uint64_t>> dataWords;
                    if (!disableWriteMerge_)
                    {
                        maskWords = constantWordsOf(write.mask);
                        dataWords = constantWordsOf(write.data);
                    }
                    const bool mergeable = maskWords.has_value() && dataWords.has_value();
                    if (mergeable && mergeActive && write.opcode == mergeOpcode &&
                        (write.cond.value == mergeCond.value ||
                         (isConstantOne(write.cond) && isConstantOne(mergeCond))))
                    {
                        // Fold the sequential blend: later writes win on
                        // overlap, untouched bits keep the earlier image.
                        for (std::size_t word = 0; word < mergeDataWords.size(); ++word)
                        {
                            mergeDataWords[word] =
                                (mergeDataWords[word] & ~(*maskWords)[word]) |
                                ((*dataWords)[word] & (*maskWords)[word]);
                            mergeMaskWords[word] |= (*maskWords)[word];
                        }
                        ++mergeMembers;
                        continue;
                    }
                    flushMerge();
                    if (mergeable)
                    {
                        mergeActive = true;
                        mergeOpcode = write.opcode;
                        mergeCond = write.cond;
                        mergeGsimNodeId = write.gsimNodeId;
                        mergeContext = write.context;
                        mergeEvents = write.events;
                        mergeMaskWords = std::move(*maskWords);
                        mergeDataWords = std::move(*dataWords);
                        mergeMembers = 1;
                        continue;
                    }
                    emitOne(write);
                }
                flushMerge();
            }

            // mem.write chains: one instruction per write, cond/mask kept as
            // operands of the selected variant. A disabled write is
            // suppressed entirely, so ordered writes in the same sweep never
            // need to read the old element value -- there is no read-old
            // threading at all.
            InstructionId emitMemoryElementWrite(
                VariableId target, const PendingStateWrite &write,
                const Type &elementType)
            {
                // Variant selection folds constant gating operands away: a
                // constant-1 cond and an all-ones mask are dropped from the
                // operand list (constant-0 cond/mask writes were suppressed
                // by the caller). Non-constant operands stay on the
                // instruction; the commit side evaluates them directly.
                const bool hasCond = !isConstantOne(write.cond);
                const bool hasMask =
                    classifyConstant(write.mask, elementType.bitWidth) !=
                    ConstClass::AllOnes;
                const Opcode opcode =
                    hasCond ? (hasMask ? Opcode::MemoryWriteCondMask
                                       : Opcode::MemoryWriteCond)
                            : (hasMask ? Opcode::MemoryWriteMask
                                       : Opcode::MemoryWrite);
                std::vector<VariableId> operands;
                operands.reserve(5 + write.events.size());
                if (hasCond)
                {
                    operands.push_back(write.cond);
                }
                operands.push_back(write.addr);
                if (hasMask)
                {
                    operands.push_back(write.mask);
                }
                operands.push_back(write.data);
                operands.push_back(target);
                operands.insert(operands.end(), write.events.begin(),
                                write.events.end());
                return addInstruction(opcode, {}, operands);
            }

            const Type *variableTypeOf(VariableId variable) const
            {
                if (!variable.valid() || variable.value >= amGraph_.program().variableCount())
                {
                    return nullptr;
                }
                return &amGraph_.program().type(amGraph_.program().variable(variable).type);
            }

            void emitSignatureWrites(
                const std::vector<PendingStateWrite> &signatureGroup, uint32_t group,
                uint32_t &ordinal)
            {
                const PendingStateWrite &head = signatureGroup.front();
                if (head.opcode == Opcode::RegisterWrite ||
                    head.opcode == Opcode::LatchWrite)
                {
                    emitScalarWrites(signatureGroup, group, ordinal);
                    return;
                }
                if (head.opcode == Opcode::MemoryWriteLanes)
                {
                    for (const PendingStateWrite &write : signatureGroup)
                    {
                        const ScopedGsimNodeId writeNode(currentGsimNodeId_,
                                                         write.gsimNodeId);
                        std::vector<VariableId> operands{write.cond, write.data,
                                                         write.target};
                        operands.insert(operands.end(), write.events.begin(),
                                        write.events.end());
                        const InstructionId instruction =
                            addInstruction(Opcode::MemoryWriteLanes, {}, operands);
                        recordWriteEffect(instruction, group, ordinal);
                    }
                    return;
                }

                const Type targetType =
                    amGraph_.program().type(amGraph_.program().variable(head.target).type);
                const Type elementType =
                    Type::bitVector(targetType.bitWidth, targetType.signedness);
                for (const PendingStateWrite &write : signatureGroup)
                {
                    const ScopedGsimNodeId writeNode(currentGsimNodeId_,
                                                     write.gsimNodeId);
                    if (write.opcode == Opcode::MemoryWrite)
                    {
                        if (isConstantZero(write.cond) ||
                            classifyConstant(write.mask, elementType.bitWidth) ==
                                ConstClass::Zero)
                        {
                            // Never fires / never changes a bit: no effect.
                            continue;
                        }
                        const InstructionId instruction =
                            emitMemoryElementWrite(head.target, write, elementType);
                        recordWriteEffect(instruction, group, ordinal);
                        continue;
                    }
                    // mem.fill: fold cond into the packed fill image.
                    if (isConstantZero(write.cond))
                    {
                        continue;
                    }
                    const uint64_t packedWidth =
                        static_cast<uint64_t>(targetType.bitWidth) *
                        targetType.elementCount;
                    const Type packedType = Type::bitVector(
                        static_cast<uint32_t>(packedWidth), Signedness::Unsigned);
                    VariableId packed = write.data;
                    if (variableTypeOf(write.data) &&
                        variableTypeOf(write.data)->bitWidth != packedWidth)
                    {
                        const std::array<VariableId, 1> broadcastOperands{write.data};
                        packed = addPureTyped(Opcode::ArrayBroadcast, packedType,
                                              broadcastOperands);
                    }
                    else if (variableTypeOf(write.data) &&
                             *variableTypeOf(write.data) != packedType)
                    {
                        packed = coerceToType(write.data, *variableTypeOf(write.data),
                                              packedType);
                    }
                    VariableId merged = packed;
                    if (!isConstantOne(write.cond))
                    {
                        const std::array<VariableId, 1> readAllResults{
                            addVariable(packedType, amGraph_.undefInit())};
                        const std::array<VariableId, 1> readAllOperands{head.target};
                        addInstruction(Opcode::MemoryReadAll, readAllResults,
                                       readAllOperands);
                        merged = pureMux(write.cond, packed, readAllResults.front(),
                                         packedType);
                    }
                    std::vector<VariableId> operands{merged, head.target};
                    operands.insert(operands.end(), write.events.begin(),
                                    write.events.end());
                    const InstructionId instruction =
                        addInstruction(Opcode::MemoryFill, {}, operands);
                    recordWriteEffect(instruction, group, ordinal);
                }
            }

            void addOrderedEffect(const Operation &op, InstructionId instruction)
            {
                const std::optional<ExplicitOrder> explicitOrder =
                    op.id().index < explicitOrderByOperation_.size()
                        ? explicitOrderByOperation_[op.id().index]
                        : std::nullopt;
                if (explicitOrder)
                {
                    amGraph_.orderedEffects().push_back(OrderedEffect{
                        .instruction = instruction,
                        .group = explicitOrder->group,
                        .ordinal = explicitOrder->ordinal,
                    });
                }
            }

            void lowerRegisterWrite(const Operation &op)
            {
                if (op.results().size() != 0 || op.operands().size() < 4)
                {
                    error("kRegisterWritePort has invalid arity", opContext(op));
                    return;
                }
                const auto target = stateTarget(op, "regSymbol", OperationKind::kRegister);
                const auto edges = requiredAttr<std::vector<std::string>>(op, "eventEdge");
                if (!target || !edges || !requireLogic(op.operands()[0], op, 1))
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                if (!requireLogic(op.operands()[1], op, targetType.bitWidth) ||
                    !requireLogic(op.operands()[2], op, targetType.bitWidth))
                {
                    return;
                }
                const auto events = lowerEvents(op, 3, *edges);
                if (!events || events->empty())
                {
                    if (events)
                    {
                        error("register write requires at least one event", opContext(op));
                    }
                    return;
                }
                VariableId next = coerceToType(mappedValue(op.operands()[1], op),
                                               typeForValue(op.operands()[1]), targetType);
                if (!next.valid())
                {
                    error("register nextValue cannot be converted to target Type", opContext(op));
                    return;
                }
                next = preCommitValue(op.operands()[1], next, op);
                VariableId mask = coerceToType(mappedValue(op.operands()[2], op),
                                               typeForValue(op.operands()[2]), targetType);
                if (!mask.valid())
                {
                    error("register write mask cannot be converted to target Type",
                          opContext(op));
                    return;
                }
                mask = preCommitValue(op.operands()[2], mask, op);
                PendingStateWrite write{
                    .opcode = Opcode::RegisterWrite,
                    .target = *target,
                    .cond = preCommitValue(op.operands()[0],
                                           mappedValue(op.operands()[0], op), op),
                    .mask = mask,
                    .data = next,
                    .addr = VariableId::invalid(),
                    .events = *events,
                    .explicitOrder = op.id().index < explicitOrderByOperation_.size()
                                         ? explicitOrderByOperation_[op.id().index]
                                         : std::nullopt,
                    .context = opContext(op),
                    .gsimNodeId = currentGsimNodeId_,
                };
                if (!write.cond.valid() || !write.mask.valid() || !write.data.valid())
                {
                    return;
                }
                pendingStateWrites_.push_back(std::move(write));
            }

            void lowerLatchWrite(const Operation &op)
            {
                if (!requireShape(op, 3, 0))
                {
                    return;
                }
                const auto target = stateTarget(op, "latchSymbol", OperationKind::kLatch);
                if (!target || !requireLogic(op.operands()[0], op, 1))
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                if (!requireLogic(op.operands()[1], op, targetType.bitWidth) ||
                    !requireLogic(op.operands()[2], op, targetType.bitWidth))
                {
                    return;
                }
                VariableId next = coerceToType(
                    mappedValue(op.operands()[1], op), typeForValue(op.operands()[1]),
                    targetType);
                if (!next.valid())
                {
                    error("latch nextValue cannot be converted to target Type", opContext(op));
                    return;
                }
                next = preCommitValue(op.operands()[1], next, op);
                VariableId mask = coerceToType(
                    mappedValue(op.operands()[2], op), typeForValue(op.operands()[2]),
                    targetType);
                if (!mask.valid())
                {
                    error("latch write mask cannot be converted to target Type",
                          opContext(op));
                    return;
                }
                mask = preCommitValue(op.operands()[2], mask, op);
                PendingStateWrite write{
                    .opcode = Opcode::LatchWrite,
                    .target = *target,
                    .cond = preCommitValue(op.operands()[0],
                                           mappedValue(op.operands()[0], op), op),
                    .mask = mask,
                    .data = next,
                    .addr = VariableId::invalid(),
                    .events = {},
                    .explicitOrder = op.id().index < explicitOrderByOperation_.size()
                                         ? explicitOrderByOperation_[op.id().index]
                                         : std::nullopt,
                    .context = opContext(op),
                    .gsimNodeId = currentGsimNodeId_,
                };
                if (!write.cond.valid() || !write.mask.valid() || !write.data.valid())
                {
                    return;
                }
                pendingStateWrites_.push_back(std::move(write));
            }

            void lowerMemoryRead(const Operation &op)
            {
                if (!requireShape(op, 1, 1))
                {
                    return;
                }
                const auto target = stateTarget(op, "memSymbol", OperationKind::kMemory);
                if (!target || !requireLogic(op.operands()[0], op) ||
                    !requireLogic(op.results()[0], op))
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                const Type resultType = typeForValue(op.results()[0]);
                const Type elementType = Type::bitVector(targetType.bitWidth,
                                                         targetType.signedness);
                if (resultType != elementType)
                {
                    error("memory read result Type does not match memory element Type",
                          opContext(op));
                    return;
                }
                const std::array<VariableId, 1> results{mappedValue(op.results()[0], op)};
                const std::array<VariableId, 2> operands{
                    *target, mappedValue(op.operands()[0], op)};
                if (!results[0].valid() || !operands[1].valid())
                {
                    return;
                }
                addInstruction(Opcode::MemoryRead, results, operands);
            }

            void lowerMemoryWrite(const Operation &op)
            {
                if (!op.results().empty() || op.operands().size() < 5)
                {
                    error("kMemoryWritePort has invalid arity", opContext(op));
                    return;
                }
                const auto target = stateTarget(op, "memSymbol", OperationKind::kMemory);
                const auto edges = requiredAttr<std::vector<std::string>>(op, "eventEdge");
                if (!target || !edges || !requireLogic(op.operands()[0], op, 1) ||
                    !requireLogic(op.operands()[1], op))
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                const Type elementType = Type::bitVector(targetType.bitWidth,
                                                         targetType.signedness);
                if (!requireLogic(op.operands()[2], op, targetType.bitWidth) ||
                    !requireLogic(op.operands()[3], op, targetType.bitWidth))
                {
                    return;
                }
                const auto events = lowerEvents(op, 4, *edges);
                if (!events || events->empty())
                {
                    if (events)
                    {
                        error("memory write requires at least one event", opContext(op));
                    }
                    return;
                }
                VariableId data = coerceToType(mappedValue(op.operands()[2], op),
                                               typeForValue(op.operands()[2]), elementType);
                if (!data.valid())
                {
                    error("memory write data cannot be converted to element Type", opContext(op));
                    return;
                }
                data = preCommitValue(op.operands()[2], data, op);
                VariableId mask = coerceToType(mappedValue(op.operands()[3], op),
                                               typeForValue(op.operands()[3]), elementType);
                if (!mask.valid())
                {
                    error("memory write mask cannot be converted to element Type",
                          opContext(op));
                    return;
                }
                mask = preCommitValue(op.operands()[3], mask, op);
                PendingStateWrite write{
                    .opcode = Opcode::MemoryWrite,
                    .target = *target,
                    .cond = preCommitValue(op.operands()[0],
                                           mappedValue(op.operands()[0], op), op),
                    .mask = mask,
                    .data = data,
                    .addr = preCommitValue(op.operands()[1],
                                           mappedValue(op.operands()[1], op), op),
                    .events = *events,
                    .explicitOrder = op.id().index < explicitOrderByOperation_.size()
                                         ? explicitOrderByOperation_[op.id().index]
                                         : std::nullopt,
                    .context = opContext(op),
                    .gsimNodeId = currentGsimNodeId_,
                };
                if (!write.cond.valid() || !write.mask.valid() || !write.data.valid() ||
                    !write.addr.valid())
                {
                    return;
                }
                pendingStateWrites_.push_back(std::move(write));
            }

            void lowerMemoryReadAll(const Operation &op)
            {
                if (!op.operands().empty() || op.results().size() != 1)
                {
                    error("kMemoryReadAllPort has invalid arity", opContext(op));
                    return;
                }
                const auto target = stateTarget(op, "memSymbol", OperationKind::kMemory);
                if (!target || !requireLogic(op.results()[0], op))
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                const Type resultType = typeForValue(op.results()[0]);
                const uint64_t packedWidth =
                    static_cast<uint64_t>(targetType.elementCount) * targetType.bitWidth;
                if (resultType.kind != TypeKind::BitVector ||
                    resultType.bitWidth != packedWidth ||
                    packedWidth > std::numeric_limits<uint32_t>::max())
                {
                    error("array read-all result width does not equal rows * element width",
                          opContext(op));
                    return;
                }
                const std::array<VariableId, 1> results{mappedValue(op.results()[0], op)};
                const std::array<VariableId, 1> operands{*target};
                if (!results[0].valid())
                {
                    return;
                }
                addInstruction(Opcode::MemoryReadAll, results, operands);
            }

            void lowerMemoryWriteLanes(const Operation &op)
            {
                if (!op.results().empty() || op.operands().size() < 3)
                {
                    error("kMemoryWriteLanesPort has invalid arity", opContext(op));
                    return;
                }
                const auto target = stateTarget(op, "memSymbol", OperationKind::kMemory);
                const auto edges = requiredAttr<std::vector<std::string>>(op, "eventEdge");
                if (!target || !edges)
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                const uint64_t packedWidth =
                    static_cast<uint64_t>(targetType.elementCount) * targetType.bitWidth;
                if (packedWidth > std::numeric_limits<uint32_t>::max() ||
                    !requireLogic(op.operands()[0], op, targetType.elementCount) ||
                    !requireLogic(op.operands()[1], op,
                                  static_cast<uint32_t>(packedWidth)))
                {
                    return;
                }
                const auto events = lowerEvents(op, 2, *edges);
                if (!events || events->empty())
                {
                    if (events)
                    {
                        error("array write requires at least one event", opContext(op));
                    }
                    return;
                }
                // The lane mask already merges the write enable and the lane
                // granularity, so it stays on the instruction as the
                // address-like operand.
                PendingStateWrite write{
                    .opcode = Opcode::MemoryWriteLanes,
                    .target = *target,
                    .cond = preCommitValue(op.operands()[0],
                                           mappedValue(op.operands()[0], op), op),
                    .mask = VariableId::invalid(),
                    .data = preCommitValue(op.operands()[1],
                                           mappedValue(op.operands()[1], op), op),
                    .addr = VariableId::invalid(),
                    .events = *events,
                    .explicitOrder = op.id().index < explicitOrderByOperation_.size()
                                         ? explicitOrderByOperation_[op.id().index]
                                         : std::nullopt,
                    .context = opContext(op),
                    .gsimNodeId = currentGsimNodeId_,
                };
                if (!write.cond.valid() || !write.data.valid())
                {
                    return;
                }
                pendingStateWrites_.push_back(std::move(write));
            }

            void lowerMemoryFill(const Operation &op)
            {
                if (!op.results().empty() || op.operands().size() < 3)
                {
                    error("kMemoryFillPort has invalid arity", opContext(op));
                    return;
                }
                const auto target = stateTarget(op, "memSymbol", OperationKind::kMemory);
                const auto edges = requiredAttr<std::vector<std::string>>(op, "eventEdge");
                if (!target || !edges || !requireLogic(op.operands()[0], op, 1) ||
                    !requireLogic(op.operands()[1], op))
                {
                    return;
                }
                const Type targetType = amGraph_.program().type(
                    amGraph_.program().variable(*target).type);
                const uint64_t packedWidth =
                    static_cast<uint64_t>(targetType.bitWidth) * targetType.elementCount;
                const uint32_t dataWidth =
                    static_cast<uint32_t>(graph_.valueWidth(op.operands()[1]));
                if (dataWidth != targetType.bitWidth && dataWidth != packedWidth)
                {
                    error("memory fill data is neither one row nor a packed whole memory",
                          opContext(op));
                    return;
                }
                const auto events = lowerEvents(op, 2, *edges);
                if (!events || events->empty())
                {
                    if (events)
                    {
                        error("memory fill requires at least one event", opContext(op));
                    }
                    return;
                }
                PendingStateWrite write{
                    .opcode = Opcode::MemoryFill,
                    .target = *target,
                    .cond = preCommitValue(op.operands()[0],
                                           mappedValue(op.operands()[0], op), op),
                    .mask = VariableId::invalid(),
                    .data = preCommitValue(op.operands()[1],
                                           mappedValue(op.operands()[1], op), op),
                    .addr = VariableId::invalid(),
                    .events = *events,
                    .explicitOrder = op.id().index < explicitOrderByOperation_.size()
                                         ? explicitOrderByOperation_[op.id().index]
                                         : std::nullopt,
                    .context = opContext(op),
                    .gsimNodeId = currentGsimNodeId_,
                };
                if (!write.cond.valid() || !write.data.valid())
                {
                    return;
                }
                pendingStateWrites_.push_back(std::move(write));
            }

            CallSchedule callSchedule(const Operation &op)
            {
                const auto procKind = optionalAttr<std::string>(op, "procKind");
                if (!procKind)
                {
                    return CallSchedule::Normal;
                }
                if (*procKind == "initial")
                {
                    return CallSchedule::Once;
                }
                if (*procKind == "final")
                {
                    return CallSchedule::Final;
                }
                return CallSchedule::Normal;
            }

            void lowerSystemFunction(const Operation &op)
            {
                if (op.results().size() != 1)
                {
                    error("kSystemFunction must have exactly one result", opContext(op));
                    return;
                }
                const auto rawName = requiredAttr<std::string>(op, "name");
                const auto sideEffects = optionalAttr<bool>(op, "hasSideEffects");
                if (!rawName)
                {
                    return;
                }
                const std::string name = normalizedCallName(*rawName);
                if (name.empty())
                {
                    error("system function name must be non-empty", opContext(op));
                    return;
                }
                std::vector<VariableId> operands;
                operands.reserve(op.operands().size());
                for (ValueId operand : op.operands())
                {
                    operands.push_back(mappedValue(operand, op));
                }
                const std::array<VariableId, 1> results{mappedValue(op.results()[0], op)};
                if (!results[0].valid() ||
                    std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                const bool hasSideEffects = sideEffects.value_or(false);
                const InstructionId instruction = addInstruction(
                    Opcode::SystemFunction, results, operands,
                    hasSideEffects ? InstructionEffect::HostEffect
                                   : InstructionEffect::HostRead);
                amGraph_.setSystemFunctionAttributes(
                    instruction,
                    SystemFunctionAttributes{
                        .name = internString(name),
                        .schedule = callSchedule(op),
                        .hasSideEffects = hasSideEffects,
                    });
                addOrderedEffect(op, instruction);
            }

            void lowerSystemTask(const Operation &op)
            {
                if (!op.results().empty() || op.operands().empty())
                {
                    error("kSystemTask has invalid arity", opContext(op));
                    return;
                }
                const auto rawName = requiredAttr<std::string>(op, "name");
                const auto edges = optionalAttr<std::vector<std::string>>(op, "eventEdge");
                if (!rawName)
                {
                    return;
                }
                const std::string name = normalizedCallName(*rawName);
                if (name.empty())
                {
                    error("system task name must be non-empty", opContext(op));
                    return;
                }
                const std::size_t eventCount = edges ? edges->size() : 0;
                if (op.operands().size() < 1 + eventCount)
                {
                    error("system task event list exceeds its operands", opContext(op));
                    return;
                }
                const std::size_t eventStart = op.operands().size() - eventCount;
                const auto events = lowerEvents(op, eventStart,
                                                edges.value_or(std::vector<std::string>{}));
                if (!events)
                {
                    return;
                }
                const CallSchedule schedule = callSchedule(op);
                if (schedule == CallSchedule::Final && !events->empty())
                {
                    error("final system task cannot have raw events", opContext(op));
                    return;
                }
                const VariableId condition = lowerCondition(op.operands()[0], op);
                if (!condition.valid())
                {
                    return;
                }
                std::vector<VariableId> operands;
                operands.reserve(op.operands().size());
                operands.push_back(condition);
                for (std::size_t index = 1; index < eventStart; ++index)
                {
                    operands.push_back(mappedValue(op.operands()[index], op));
                }
                operands.insert(operands.end(), events->begin(), events->end());
                if (std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                const InstructionId instruction =
                    addInstruction(Opcode::SystemTask, {}, operands);
                amGraph_.setSystemTaskAttributes(
                    instruction,
                    SystemTaskAttributes{
                        .name = internString(name),
                        .eventCount = static_cast<uint32_t>(events->size()),
                        .schedule = schedule,
                        .eventMode = HostEventMode::Immediate,
                    });
                addOrderedEffect(op, instruction);
            }

            static std::optional<std::size_t> findName(
                const std::vector<std::string> &names, std::string_view name)
            {
                const auto found = std::find(names.begin(), names.end(), name);
                if (found == names.end())
                {
                    return std::nullopt;
                }
                return static_cast<std::size_t>(found - names.begin());
            }

            void lowerDpiCall(const Operation &op)
            {
                const auto targetSymbol = requiredAttr<std::string>(op, "targetImportSymbol");
                const auto inputNames =
                    requiredAttr<std::vector<std::string>>(op, "inArgName");
                const auto outputNames =
                    requiredAttr<std::vector<std::string>>(op, "outArgName");
                auto inoutNames = optionalAttr<std::vector<std::string>>(op, "inoutArgName");
                const auto hasReturn = requiredAttr<bool>(op, "hasReturn");
                const auto edges = optionalAttr<std::vector<std::string>>(op, "eventEdge");
                if (!targetSymbol || !inputNames || !outputNames || !hasReturn)
                {
                    return;
                }
                if (!inoutNames)
                {
                    inoutNames = std::vector<std::string>{};
                }
                const OperationId importOperation = graph_.findOperation(*targetSymbol);
                if (!importOperation.valid() ||
                    graph_.opKind(importOperation) != OperationKind::kDpicImport ||
                    importOperation.index >= dpiImportByOperation_.size() ||
                    !dpiImportByOperation_[importOperation.index].valid)
                {
                    error("DPI call refers to unknown import '" + *targetSymbol + "'",
                          opContext(op));
                    return;
                }
                const DpiImportInfo &import = dpiImportByOperation_[importOperation.index];
                if (*hasReturn != import.returnValue.present)
                {
                    error("DPI call hasReturn does not match its import", opContext(op));
                    return;
                }
                const std::size_t eventCount = edges ? edges->size() : 0;
                const std::size_t expectedSourceOperands =
                    1 + inputNames->size() + inoutNames->size() + eventCount;
                const std::size_t expectedSourceResults =
                    (*hasReturn ? 1U : 0U) + outputNames->size() + inoutNames->size();
                if (op.operands().size() != expectedSourceOperands ||
                    op.results().size() != expectedSourceResults)
                {
                    error("DPI call source operand/result groups have invalid sizes",
                          opContext(op));
                    return;
                }
                const VariableId condition = lowerCondition(op.operands()[0], op);
                if (!condition.valid())
                {
                    return;
                }
                const std::size_t rawEventStart = op.operands().size() - eventCount;
                const auto events = lowerEvents(op, rawEventStart,
                                                edges.value_or(std::vector<std::string>{}));
                if (!events)
                {
                    return;
                }

                std::vector<VariableId> operands{condition};
                std::vector<VariableId> results;
                struct ResultBridge
                {
                    VariableId target;
                    VariableId temporary;
                };
                std::vector<ResultBridge> resultBridges;
                if (*hasReturn)
                {
                    const ValueId value = op.results()[0];
                    const Type sourceType = typeForValue(value);
                    const VariableId target = mappedValue(value, op);
                    if (sourceType == import.returnType)
                    {
                        results.push_back(target);
                    }
                    else if (sourceType.kind == TypeKind::BitVector &&
                             import.returnType.kind == TypeKind::BitVector)
                    {
                        const VariableId temporary =
                            addVariable(import.returnType, amGraph_.zeroInit());
                        results.push_back(temporary);
                        resultBridges.push_back(ResultBridge{
                            .target = target,
                            .temporary = temporary,
                        });
                        ++freshTemporaryCount_;
                    }
                    else
                    {
                        error("DPI return result Type cannot be converted from import Type",
                              opContext(op));
                        return;
                    }
                }
                const std::size_t sourceInoutOperandBase = 1 + inputNames->size();
                const std::size_t sourceOutputResultBase = *hasReturn ? 1 : 0;
                const std::size_t sourceInoutResultBase =
                    sourceOutputResultBase + outputNames->size();

                for (DpiDirection direction : {DpiDirection::Input, DpiDirection::Inout})
                {
                    for (const DpiParameterInfo &parameter : import.parameters)
                    {
                        if (parameter.direction != direction)
                        {
                            continue;
                        }
                        const auto position = findName(
                            direction == DpiDirection::Input ? *inputNames : *inoutNames,
                            parameter.name);
                        if (!position)
                        {
                            error("DPI call is missing parameter '" + parameter.name + "'",
                                  opContext(op));
                            return;
                        }
                        const std::size_t sourcePosition =
                            direction == DpiDirection::Input
                                ? 1 + *position
                                : sourceInoutOperandBase + *position;
                        const ValueId value = op.operands()[sourcePosition];
                        const Type sourceType = typeForValue(value);
                        const VariableId converted = coerceToType(
                            mappedValue(value, op), sourceType, parameter.type);
                        if (!converted.valid())
                        {
                            error("DPI input/inout Type cannot be converted for parameter '" +
                                      parameter.name + "'",
                                  opContext(op));
                            return;
                        }
                        operands.push_back(converted);
                    }
                }
                operands.insert(operands.end(), events->begin(), events->end());

                for (DpiDirection direction : {DpiDirection::Output, DpiDirection::Inout})
                {
                    for (const DpiParameterInfo &parameter : import.parameters)
                    {
                        if (parameter.direction != direction)
                        {
                            continue;
                        }
                        const auto position = findName(
                            direction == DpiDirection::Output ? *outputNames : *inoutNames,
                            parameter.name);
                        if (!position)
                        {
                            error("DPI call is missing parameter '" + parameter.name + "'",
                                  opContext(op));
                            return;
                        }
                        const std::size_t sourcePosition =
                            direction == DpiDirection::Output
                                ? sourceOutputResultBase + *position
                                : sourceInoutResultBase + *position;
                        const ValueId value = op.results()[sourcePosition];
                        const Type targetType = typeForValue(value);
                        const VariableId target = mappedValue(value, op);
                        if (targetType == parameter.type)
                        {
                            results.push_back(target);
                        }
                        else if (targetType.kind == TypeKind::BitVector &&
                                 parameter.type.kind == TypeKind::BitVector)
                        {
                            const VariableId temporary =
                                addVariable(parameter.type, amGraph_.zeroInit());
                            results.push_back(temporary);
                            resultBridges.push_back(ResultBridge{
                                .target = target,
                                .temporary = temporary,
                            });
                            ++freshTemporaryCount_;
                        }
                        else
                        {
                            error("DPI output/inout Type cannot be converted for parameter '" +
                                      parameter.name + "'",
                                  opContext(op));
                            return;
                        }
                    }
                }

                if (std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }) ||
                    std::any_of(results.begin(), results.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                std::unordered_set<uint32_t> uniqueResults;
                for (VariableId result : results)
                {
                    if (!uniqueResults.insert(result.value).second ||
                        std::find(operands.begin(), operands.end(), result) != operands.end())
                    {
                        error("DPI result aliases another result or operand; normalized GRH must use fresh values",
                              opContext(op));
                        return;
                    }
                }
                const InstructionId instruction =
                    addInstruction(Opcode::DpiCall, results, operands);
                amGraph_.setDpiCallAttributes(
                    instruction,
                    DpiCallAttributes{
                        .importSymbol = import.symbolId,
                        .eventCount = static_cast<uint32_t>(events->size()),
                        .eventMode = results.empty() ? HostEventMode::Immediate
                                                     : HostEventMode::Pending,
                    });
                addOrderedEffect(op, instruction);
                for (const ResultBridge &bridge : resultBridges)
                {
                    const std::array<VariableId, 1> bridgeResults{bridge.target};
                    const std::array<VariableId, 1> bridgeOperands{bridge.temporary};
                    addInstruction(Opcode::Assign, bridgeResults, bridgeOperands);
                    ++assignDpiBridgeCount_;
                }
            }

            // Earliest consumer position (in graph_.operations() order) of
            // every value. The lowering-time constant fold repoints a folded
            // value's variable; a consumer lowered BEFORE its producer would
            // otherwise keep referencing the pre-created (never computed)
            // variable. The fold is skipped unless every use comes after the
            // producing op.
            void computeFirstUseOrdinals()
            {
                firstUseOrdinal_.assign(valueMap_.size(),
                                        std::numeric_limits<uint32_t>::max());
                uint32_t ordinal = 0;
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
                    for (ValueId operand : op.operands())
                    {
                        if (operand.valid() && operand.index < firstUseOrdinal_.size())
                        {
                            uint32_t &slot = firstUseOrdinal_[operand.index];
                            slot = std::min(slot, ordinal);
                        }
                    }
                    ++ordinal;
                }
            }

            // NO0012 Tier 1: wide concat/slice chain lazy replay plan types.
            struct ChainReplaySegment
            {
                ValueId root;      // external (non-chain) value supplying the bits
                uint32_t lsb = 0;  // bit offset inside root
                uint32_t width = 0;
            };
            struct ChainReplayItem
            {
                ValueId value;  // escaped chain-produced value to materialize
                std::vector<ChainReplaySegment> segments;  // LSB-first
                // Set when the whole chain evaluates to a constant (the
                // gsim export's constant-but-guard-blocked chains, e.g. the
                // ROB perfDebugInfo update fans): replay is one Assign from
                // an interned constant, guard-immune because it computes
                // into the escaped value's own pre-created variable.
                std::optional<std::vector<uint64_t>> constant;
            };
            struct ChainReplayGroup
            {
                int64_t nodeId = -1;
                OperationId flushOp;  // last chain op (ordinal order): replay
                                      // emits right after skipping it
                std::vector<ChainReplayItem> items;
            };

            // NO0012 Tier 2: lane-write fan merge. A fan is a set of
            // register write ops that share one target and one event key,
            // each with a constant one-hot-lane mask and pairwise-disjoint
            // lanes. The whole fan collapses into a single write whose data
            // is the per-lane mux of (member cond, member data lane window,
            // old target lane) — restoring the element-granular update gsim
            // emits natively, and removing the fan's demand for the full
            // O(steps x width) chain values that fed each member.
            struct LaneWriteMember
            {
                OperationId op;
                ValueId condValue;
                ValueId dataValue;
                uint32_t laneLo = 0;   // bit offset of the lane inside target
                uint32_t laneWidth = 0;
                std::optional<ExplicitOrder> explicitOrder;
            };
            struct LaneWriteFan
            {
                VariableId target;
                uint32_t targetWidth = 0;
                std::vector<VariableId> events;
                int64_t nodeId = -1;
                std::vector<LaneWriteMember> members;  // commit order
                // Non-fan writes to the same target ordered strictly before
                // the fan (constant mask+data only), folded into the old
                // value the merged write's per-lane muxes fall back to.
                std::vector<OperationId> absorbedPrefix;
                OperationId flushOp;
            };
            std::vector<LaneWriteFan> laneWriteFans_;
            std::unordered_map<uint32_t, uint32_t> laneWriteSkipFan_;
            std::unordered_set<uint32_t> laneWriteMemberOps_;
            // Provenance of planned chain groups, queried by the fan flush to
            // resolve a member's data lane window to its root source.
            std::unordered_map<uint32_t, std::vector<ChainReplaySegment>>
                chainProvenance_;
            bool disableLaneWriteMerge_ = false;
            uint64_t laneWriteMergedCount_ = 0;
            uint64_t laneWriteFanRejectCount_ = 0;

            // NO0012 Tier 2 pass 0: detect lane-write fans before chain
            // planning so the chain escape analysis can stop counting fan
            // members as full-value consumers (they only demand their lane
            // window of the data value).
            void detectLaneWriteFans()
            {
                if (disableLaneWriteMerge_)
                {
                    return;
                }
                struct Candidate
                {
                    OperationId op;
                    VariableId target;
                    uint32_t laneLo = 0;
                    uint32_t laneWidth = 0;
                    std::vector<uint64_t> maskWords;
                    std::string eventKey;
                };
                std::map<std::pair<uint32_t, std::string>, std::vector<Candidate>>
                    byTargetEvent;
                std::unordered_map<uint32_t, std::vector<OperationId>> writesByTarget;
                for (OperationId opId : graph_.operations())
                {
                    const Operation op = graph_.getOperation(opId);
                    if (op.kind() != OperationKind::kRegisterWritePort)
                    {
                        continue;
                    }
                    const auto target = stateTarget(op, "regSymbol",
                                                    OperationKind::kRegister);
                    if (!target || op.operands().size() < 4)
                    {
                        continue;
                    }
                    writesByTarget[target->value].push_back(opId);
                    const ValueId maskValue = op.operands()[2];
                    if (!maskValue.valid() || maskValue.index >= valueMap_.size())
                    {
                        continue;
                    }
                    std::optional<std::vector<uint64_t>> maskWords =
                        constantWordsOf(valueMap_[maskValue.index]);
                    if (!maskWords)
                    {
                        continue;
                    }
                    // Exactly one contiguous set-bit region (the lane).
                    uint64_t bitIndex = 0;
                    int64_t laneLo = -1;
                    uint64_t laneHi = 0;
                    bool multiRegion = false;
                    bool inRegion = false;
                    for (uint64_t word : *maskWords)
                    {
                        for (uint32_t bit = 0; bit < 64; ++bit, ++bitIndex)
                        {
                            const bool set = ((word >> bit) & 1U) != 0;
                            if (set && !inRegion)
                            {
                                if (laneLo >= 0)
                                {
                                    multiRegion = true;
                                }
                                inRegion = true;
                                laneLo = static_cast<int64_t>(bitIndex);
                            }
                            else if (!set && inRegion)
                            {
                                inRegion = false;
                                laneHi = bitIndex;
                            }
                        }
                    }
                    if (inRegion)
                    {
                        laneHi = bitIndex;
                    }
                    if (multiRegion || laneLo < 0)
                    {
                        continue;
                    }
                    const uint32_t targetWidth = static_cast<uint32_t>(
                        amGraph_.program()
                            .type(amGraph_.program().variable(*target).type)
                            .bitWidth);
                    if (laneHi > targetWidth)
                    {
                        continue;
                    }
                    const auto edges =
                        requiredAttr<std::vector<std::string>>(op, "eventEdge");
                    std::string eventKey;
                    if (edges)
                    {
                        for (const std::string &edge : *edges)
                        {
                            eventKey += edge;
                            eventKey += ';';
                        }
                    }
                    for (std::size_t index = 3; index < op.operands().size();
                         ++index)
                    {
                        eventKey += std::to_string(op.operands()[index].index);
                        eventKey += ',';
                    }
                    byTargetEvent[{target->value, eventKey}].push_back(Candidate{
                        .op = opId,
                        .target = *target,
                        .laneLo = static_cast<uint32_t>(laneLo),
                        .laneWidth = static_cast<uint32_t>(laneHi - laneLo),
                        .maskWords = std::move(*maskWords),
                        .eventKey = std::move(eventKey),
                    });
                }

                auto orderKeyOf = [&](OperationId opId)
                    -> std::tuple<bool, uint32_t, uint32_t, uint32_t>
                {
                    if (opId.index < explicitOrderByOperation_.size() &&
                        explicitOrderByOperation_[opId.index])
                    {
                        const ExplicitOrder &order =
                            *explicitOrderByOperation_[opId.index];
                        return {true, order.group, order.ordinal, opId.index};
                    }
                    return {false, 0, 0, opId.index};
                };

                for (auto &[key, candidates] : byTargetEvent)
                {
                    if (candidates.size() < 2)
                    {
                        continue;
                    }
                    // Disjoint lanes are required: overlapping lanes make the
                    // original fan order-dependent, which a single merged
                    // write cannot express.
                    std::vector<std::pair<uint32_t, uint32_t>> lanes;
                    for (const Candidate &candidate : candidates)
                    {
                        lanes.emplace_back(candidate.laneLo, candidate.laneWidth);
                    }
                    std::sort(lanes.begin(), lanes.end());
                    bool overlap = false;
                    for (std::size_t index = 1; index < lanes.size(); ++index)
                    {
                        if (lanes[index].first <
                            lanes[index - 1].first + lanes[index - 1].second)
                        {
                            overlap = true;
                            break;
                        }
                    }
                    if (overlap)
                    {
                        ++laneWriteFanRejectCount_;
                        continue;
                    }
                    LaneWriteFan fan;
                    fan.target = candidates.front().target;
                    fan.targetWidth = static_cast<uint32_t>(
                        amGraph_.program()
                            .type(
                                amGraph_.program().variable(fan.target).type)
                            .bitWidth);
                    bool rejected = false;
                    for (const Candidate &candidate : candidates)
                    {
                        const Operation op = graph_.getOperation(candidate.op);
                        if (op.operands()[0].invalid() ||
                            op.operands()[1].invalid())
                        {
                            rejected = true;
                            break;
                        }
                        fan.members.push_back(LaneWriteMember{
                            .op = candidate.op,
                            .condValue = op.operands()[0],
                            .dataValue = op.operands()[1],
                            .laneLo = candidate.laneLo,
                            .laneWidth = candidate.laneWidth,
                            .explicitOrder =
                                candidate.op.index <
                                        explicitOrderByOperation_.size()
                                    ? explicitOrderByOperation_[candidate.op.index]
                                    : std::nullopt,
                        });
                    }
                    if (rejected)
                    {
                        ++laneWriteFanRejectCount_;
                        continue;
                    }
                    std::sort(fan.members.begin(), fan.members.end(),
                              [&](const LaneWriteMember &lhs,
                                  const LaneWriteMember &rhs) {
                                  return orderKeyOf(lhs.op) < orderKeyOf(rhs.op);
                              });
                    fan.flushOp = fan.members.back().op;
                    // Non-fan writes to the same target: only same-event-key
                    // writes ordered strictly before every member can be
                    // absorbed (their effect blends into the old value the
                    // merged muxes fall back to); writes ordered strictly
                    // after every member commute with the merge untouched.
                    for (OperationId other : writesByTarget[key.first])
                    {
                        if (std::any_of(fan.members.begin(), fan.members.end(),
                                        [&](const LaneWriteMember &member) {
                                            return member.op == other;
                                        }))
                        {
                            continue;
                        }
                        const Operation otherOp = graph_.getOperation(other);
                        const auto otherEdges = requiredAttr<std::vector<std::string>>(
                            otherOp, "eventEdge");
                        std::string otherKey;
                        if (otherEdges)
                        {
                            for (const std::string &edge : *otherEdges)
                            {
                                otherKey += edge;
                                otherKey += ';';
                            }
                        }
                        for (std::size_t index = 3; index < otherOp.operands().size();
                             ++index)
                        {
                            otherKey += std::to_string(otherOp.operands()[index].index);
                            otherKey += ',';
                        }
                        if (otherKey != key.second)
                        {
                            ++laneWriteFanRejectCount_;
                            rejected = true;
                            break;
                        }
                        const auto otherOrder = orderKeyOf(other);
                        const bool beforeAll = std::all_of(
                            fan.members.begin(), fan.members.end(),
                            [&](const LaneWriteMember &member) {
                                return otherOrder < orderKeyOf(member.op);
                            });
                        const bool afterAll = std::all_of(
                            fan.members.begin(), fan.members.end(),
                            [&](const LaneWriteMember &member) {
                                return orderKeyOf(member.op) < otherOrder;
                            });
                        if (afterAll)
                        {
                            continue;
                        }
                        if (!beforeAll)
                        {
                            ++laneWriteFanRejectCount_;
                            rejected = true;
                            break;
                        }
                        // Absorbable prefix writes must be constant mask+data
                        // (their blend folds into the merged write's old
                        // value at flush time).
                        if (otherOp.operands().size() < 3 ||
                            otherOp.operands()[1].invalid() ||
                            otherOp.operands()[2].invalid() ||
                            !constantWordsOf(
                                valueMap_[otherOp.operands()[1].index]) ||
                            !constantWordsOf(
                                valueMap_[otherOp.operands()[2].index]))
                        {
                            ++laneWriteFanRejectCount_;
                            rejected = true;
                            break;
                        }
                        fan.absorbedPrefix.push_back(other);
                    }
                    if (rejected)
                    {
                        continue;
                    }
                    std::sort(fan.absorbedPrefix.begin(),
                              fan.absorbedPrefix.end(),
                              [&](OperationId lhs, OperationId rhs) {
                                  return orderKeyOf(lhs) < orderKeyOf(rhs);
                              });
                    const uint32_t fanIndex =
                        static_cast<uint32_t>(laneWriteFans_.size());
                    laneWriteFans_.push_back(std::move(fan));
                    for (const LaneWriteMember &member :
                         laneWriteFans_.back().members)
                    {
                        laneWriteSkipFan_.emplace(member.op.index, fanIndex);
                        laneWriteMemberOps_.insert(member.op.index);
                    }
                    for (OperationId absorbed : laneWriteFans_.back().absorbedPrefix)
                    {
                        laneWriteMemberOps_.insert(absorbed.index);
                        laneWriteSkipFan_.emplace(absorbed.index, fanIndex);
                    }
                }
            }

            // NO0012 Tier 1: plan lazy replay for wide concat/slice_static
            // chains. Groups are the gsim-node op sets (node-aligned atoms);
            // within a group, chain ops rebuild a wide value step by step
            // (O(steps x width) word traffic in the emitted model). The plan
            // replaces the whole chain with per-segment replay of the values
            // that actually escape the chain: every chain-produced value is
            // resolved to a list of (root, lsb, width) segments over values
            // NOT produced by the group's chain ops, and only escaped values
            // materialize (one slice per segment + at most one concat).
            // Chain values only referenced inside the chain never
            // materialize; their pre-created variables stay zero-initialized
            // and unreferenced. All value variables are pre-created before
            // lowering, and replay computes escaped values into their own
            // pre-created variables, so no use-order guard is needed.
            void planChainReplay()
            {
                if (disableChainReplay_)
                {
                    return;
                }
                constexpr uint32_t kMinWidth = 4096;
                constexpr std::size_t kMaxSegmentsPerValue = 16384;
                // Tier 2 fans first: their members stop counting as
                // full-value consumers in the escape analysis below.
                detectLaneWriteFans();
                // Global value -> using ops map (chain values may be consumed
                // anywhere, including state-write ops).
                std::vector<std::vector<uint32_t>> users(valueMap_.size());
                std::map<int64_t, std::vector<OperationId>> groupsByNode;
                for (OperationId opId : graph_.operations())
                {
                    const Operation op = graph_.getOperation(opId);
                    for (ValueId operand : op.operands())
                    {
                        if (operand.valid() && operand.index < users.size())
                        {
                            users[operand.index].push_back(opId.index);
                        }
                    }
                    if (op.kind() != OperationKind::kConcat &&
                        op.kind() != OperationKind::kSliceStatic)
                    {
                        continue;
                    }
                    const std::optional<int64_t> nodeId =
                        optionalAttr<int64_t>(op, kGsimNodeIdAttr);
                    if (!nodeId || *nodeId < 0)
                    {
                        continue;
                    }
                    auto isWide = [&](ValueId value) {
                        return value.valid() &&
                               static_cast<uint32_t>(graph_.valueWidth(value)) >=
                                   kMinWidth;
                    };
                    bool wide = op.results().size() == 1 && isWide(op.results().front());
                    for (ValueId operand : op.operands())
                    {
                        wide = wide || isWide(operand);
                    }
                    if (wide)
                    {
                        groupsByNode[*nodeId].push_back(opId);
                    }
                }

                for (const auto &[nodeId, opValues] : groupsByNode)
                {
                    if (opValues.size() < 2)
                    {
                        continue;
                    }
                    std::unordered_set<uint32_t> chainOps;
                    chainOps.reserve(opValues.size());
                    for (OperationId chainOp : opValues)
                    {
                        chainOps.insert(chainOp.index);
                    }
                    // Segment resolution passes over the group's ops. Op
                    // order inside a node is usually topological; stragglers
                    // are retried in later passes. Values whose definition
                    // sits outside the chain are segment roots.
                    std::unordered_map<uint32_t, std::vector<ChainReplaySegment>>
                        provenance;
                    std::unordered_map<uint32_t,
                                       std::optional<std::vector<uint64_t>>>
                        constWords;
                    bool rejected = false;
                    std::size_t unresolved = opValues.size();
                    for (std::size_t pass = 0; pass < opValues.size() && unresolved != 0;
                         ++pass)
                    {
                        bool progress = false;
                        for (OperationId chainOpId : opValues)
                        {
                            const Operation op = graph_.getOperation(chainOpId);
                            if (op.results().size() != 1)
                            {
                                rejected = true;
                                break;
                            }
                            const ValueId result = op.results().front();
                            if (!result.valid() || provenance.contains(result.index))
                            {
                                continue;
                            }
                            const uint32_t resultWidth =
                                static_cast<uint32_t>(graph_.valueWidth(result));
                            std::vector<ChainReplaySegment> segments;
                            bool ready = true;
                            auto segmentsOf = [&](ValueId value,
                                                  std::vector<ChainReplaySegment> &out) {
                                const OperationId def = graph_.valueDef(value);
                                if (def.valid() && chainOps.contains(def.index))
                                {
                                    const auto found = provenance.find(value.index);
                                    if (found == provenance.end())
                                    {
                                        return false;
                                    }
                                    out.insert(out.end(), found->second.begin(),
                                               found->second.end());
                                    return true;
                                }
                                out.push_back(ChainReplaySegment{
                                    .root = value,
                                    .lsb = 0,
                                    .width = static_cast<uint32_t>(
                                        graph_.valueWidth(value)),
                                });
                                return true;
                            };
                            if (op.kind() == OperationKind::kConcat)
                            {
                                // GRH concat places operand[0] at the most
                                // significant end; segments are LSB-first.
                                uint64_t totalWidth = 0;
                                const auto operands = op.operands();
                                for (auto it = operands.rbegin(); it != operands.rend();
                                     ++it)
                                {
                                    if (!it->valid())
                                    {
                                        rejected = true;
                                        ready = false;
                                        break;
                                    }
                                    if (!segmentsOf(*it, segments))
                                    {
                                        ready = false;
                                        break;
                                    }
                                    totalWidth += static_cast<uint32_t>(
                                        graph_.valueWidth(*it));
                                }
                                if (ready && totalWidth != resultWidth)
                                {
                                    rejected = true;
                                    break;
                                }
                            }
                            else  // kSliceStatic
                            {
                                const auto start =
                                    requiredAttr<int64_t>(op, "sliceStart");
                                const auto end = requiredAttr<int64_t>(op, "sliceEnd");
                                if (op.operands().size() != 1 || !start || !end ||
                                    *start < 0 || *end < *start ||
                                    static_cast<uint64_t>(*end - *start + 1) !=
                                        resultWidth)
                                {
                                    rejected = true;
                                    break;
                                }
                                std::vector<ChainReplaySegment> source;
                                if (!segmentsOf(op.operands().front(), source))
                                {
                                    ready = false;
                                }
                                else
                                {
                                    const uint64_t wantLo =
                                        static_cast<uint64_t>(*start);
                                    const uint64_t wantHi = wantLo + resultWidth;
                                    uint64_t position = 0;
                                    uint64_t covered = 0;
                                    for (const ChainReplaySegment &seg : source)
                                    {
                                        const uint64_t segLo = position;
                                        const uint64_t segHi = segLo + seg.width;
                                        position = segHi;
                                        const uint64_t lo = std::max(segLo, wantLo);
                                        const uint64_t hi = std::min(segHi, wantHi);
                                        if (lo < hi)
                                        {
                                            segments.push_back(ChainReplaySegment{
                                                .root = seg.root,
                                                .lsb = static_cast<uint32_t>(
                                                    seg.lsb + (lo - segLo)),
                                                .width =
                                                    static_cast<uint32_t>(hi - lo),
                                            });
                                            covered += hi - lo;
                                        }
                                    }
                                    if (covered != resultWidth)
                                    {
                                        rejected = true;
                                        break;
                                    }
                                }
                            }
                            if (!ready)
                            {
                                continue;
                            }
                            if (segments.size() > kMaxSegmentsPerValue)
                            {
                                rejected = true;
                                break;
                            }
                            // Guard-immune constant evaluation: when every
                            // operand is constant (in-group memo or a
                            // Constant-initialized outside variable), fold
                            // the op with the same evaluator the lowering
                            // fold (NO0010) uses. Unlike the lowering fold
                            // this ignores the use-order guard on purpose —
                            // replay materializes constants into the escaped
                            // value's own pre-created variable, which is safe
                            // for any consumer ordering.
                            std::optional<std::vector<uint64_t>> foldedWords;
                            {
                                std::vector<std::vector<uint64_t>> wordStorage;
                                std::vector<detail::ConstOperand> constOperands;
                                bool allConstant = !op.operands().empty();
                                for (ValueId operand : op.operands())
                                {
                                    const OperationId def = graph_.valueDef(operand);
                                    if (def.valid() && chainOps.contains(def.index))
                                    {
                                        const auto found =
                                            constWords.find(operand.index);
                                        if (found == constWords.end() ||
                                            !found->second)
                                        {
                                            allConstant = false;
                                            break;
                                        }
                                        wordStorage.push_back(*found->second);
                                    }
                                    else
                                    {
                                        std::optional<std::vector<uint64_t>> words =
                                            constantWordsOf(valueMap_[operand.index]);
                                        if (!words)
                                        {
                                            allConstant = false;
                                            break;
                                        }
                                        wordStorage.push_back(std::move(*words));
                                    }
                                }
                                if (allConstant)
                                {
                                    bool operandTypesOk = true;
                                    for (std::size_t index = 0;
                                         index < op.operands().size(); ++index)
                                    {
                                        const Type *operandType = variableTypeOf(
                                            valueMap_[op.operands()[index].index]);
                                        if (!operandType)
                                        {
                                            operandTypesOk = false;
                                            break;
                                        }
                                        constOperands.push_back(detail::ConstOperand{
                                            .type = *operandType,
                                            .words = wordStorage[index]});
                                    }
                                    if (operandTypesOk)
                                    {
                                        const Type resultType =
                                            typeForValue(result);
                                        const Opcode foldOpcode =
                                            op.kind() == OperationKind::kConcat
                                                ? Opcode::Concat
                                                : Opcode::SliceStatic;
                                        const uint32_t foldLsb =
                                            op.kind() == OperationKind::kSliceStatic
                                                ? static_cast<uint32_t>(
                                                      *requiredAttr<int64_t>(
                                                          op, "sliceStart"))
                                                : 0;
                                        const Type nativeType = Type::bitVector(
                                            resultWidth, Signedness::Unsigned);
                                        std::vector<uint64_t> folded;
                                        if (detail::evaluatePure(
                                                foldOpcode, nativeType,
                                                constOperands, foldLsb, folded))
                                        {
                                            if (nativeType != resultType)
                                            {
                                                folded = detail::resizedWords(
                                                    folded, nativeType.bitWidth,
                                                    resultType.bitWidth,
                                                    nativeType.signedness);
                                            }
                                            foldedWords = std::move(folded);
                                        }
                                    }
                                }
                            }
                            constWords.emplace(result.index,
                                               std::move(foldedWords));
                            provenance.emplace(result.index, std::move(segments));
                            --unresolved;
                            progress = true;
                        }
                        if (rejected || !progress)
                        {
                            break;
                        }
                    }
                    if (rejected || unresolved != 0)
                    {
                        ++chainReplayRejectCount_;
                        continue;
                    }

                    // Profitability gate: a group whose escaped dynamic values
                    // must (almost) all be rebuilt saves nothing over the
                    // original chain and only pays finer-grained emission
                    // overhead (the ROB lane-write fans are exactly this
                    // shape — they wait for the write-fan merge instead).
                    uint64_t chainWords = 0;
                    for (OperationId chainOpId : opValues)
                    {
                        const Operation op = graph_.getOperation(chainOpId);
                        const uint32_t width = static_cast<uint32_t>(
                            graph_.valueWidth(op.results().front()));
                        chainWords += (width + 63U) / 64U;
                    }
                    uint64_t replayWords = 0;
                    uint64_t dynamicEscaped = 0;
                    for (OperationId chainOpId : opValues)
                    {
                        const Operation op = graph_.getOperation(chainOpId);
                        const ValueId result = op.results().front();
                        if (const auto found = constWords.find(result.index);
                            found != constWords.end() && found->second)
                        {
                            continue;  // constants re-point for free
                        }
                        bool escaped = exposedValues_.contains(result.index) ||
                                       declaredValueIndices_.contains(result.index);
                        if (!escaped && result.index < users.size())
                        {
                            for (uint32_t user : users[result.index])
                            {
                                if (!chainOps.contains(user) &&
                                    !laneWriteMemberOps_.contains(user))
                                {
                                    escaped = true;
                                    break;
                                }
                            }
                        }
                        if (!escaped)
                        {
                            continue;
                        }
                        ++dynamicEscaped;
                        const uint32_t width = static_cast<uint32_t>(
                            graph_.valueWidth(result));
                        replayWords += (width + 63U) / 64U;
                        for (const ChainReplaySegment &segment :
                             provenance.at(result.index))
                        {
                            replayWords += (segment.width + 63U) / 64U;
                        }
                    }
                    if (dynamicEscaped != 0 && replayWords * 2 >= chainWords)
                    {
                        ++chainReplayRejectCount_;
                        continue;
                    }

                    // Materialization decisions per chain value:
                    //  - constant + not interface-labelled: re-point the
                    //    value map to an interned constant NOW (plan time,
                    //    before any instruction lowering), so every consumer
                    //    — including earlier-ordered state writes and the
                    //    NO0010 constant write merge — sees a plain constant.
                    //    This is the guard-immune generalization of the
                    //    lowering fold for chain shapes; the pre-created
                    //    variable is left zero-initialized and unreferenced.
                    //  - constant + labelled (exposed/declared): keep the
                    //    declared variable and emit an Assign from the
                    //    constant at flush (the label binding must survive).
                    //  - dynamic + escaped: per-segment replay at flush.
                    ChainReplayGroup group;
                    group.nodeId = nodeId;
                    group.flushOp = opValues.back();
                    for (OperationId chainOpId : opValues)
                    {
                        const Operation op = graph_.getOperation(chainOpId);
                        const ValueId result = op.results().front();
                        const bool labelled =
                            exposedValues_.contains(result.index) ||
                            declaredValueIndices_.contains(result.index);
                        bool escaped = labelled;
                        if (!escaped && result.index < users.size())
                        {
                            for (uint32_t user : users[result.index])
                            {
                                if (!chainOps.contains(user) &&
                                    !laneWriteMemberOps_.contains(user))
                                {
                                    escaped = true;
                                    break;
                                }
                            }
                        }
                        std::optional<std::vector<uint64_t>> constant;
                        if (const auto found = constWords.find(result.index);
                            found != constWords.end() && found->second)
                        {
                            constant = *found->second;
                        }
                        if (constant && !labelled)
                        {
                            valueMap_[result.index] = internConstant(
                                typeForValue(result), *constant);
                            ++chainReplayPlanFoldCount_;
                            continue;
                        }
                        if (constant || escaped)
                        {
                            group.items.push_back(ChainReplayItem{
                                .value = result,
                                .segments = provenance.at(result.index),
                                .constant = std::move(constant),
                            });
                        }
                    }
                    const uint32_t groupIndex =
                        static_cast<uint32_t>(chainReplayGroups_.size());
                    chainReplayGroups_.push_back(std::move(group));
                    for (OperationId chainOpId : opValues)
                    {
                        chainReplaySkipGroup_.emplace(chainOpId.index, groupIndex);
                    }
                    // Keep the accepted group's provenance for the Tier 2
                    // fan flush (lane-window resolution of chain-produced
                    // data values).
                    for (auto &[valueIndex, segments] : provenance)
                    {
                        chainProvenance_.emplace(valueIndex,
                                                 std::move(segments));
                    }
                }
            }

            // Emit the replay instructions for one planned group. Runs at the
            // group's flush op position inside lowerInstructions; every
            // escaped value is computed into its own pre-created variable.
            void emitChainReplay(const ChainReplayGroup &group)
            {
                const ScopedGsimNodeId nodeScope(currentGsimNodeId_,
                                                 group.nodeId);
                for (const ChainReplayItem &item : group.items)
                {
                    if (!item.value.valid() || item.value.index >= valueMap_.size())
                    {
                        error("chain replay escaped value is unmapped",
                              "grhsim-am-lowering");
                        return;
                    }
                    const VariableId destination = valueMap_[item.value.index];
                    if (!destination.valid())
                    {
                        error("chain replay escaped value is unmapped",
                              "grhsim-am-lowering");
                        return;
                    }
                    const uint32_t valueWidth =
                        static_cast<uint32_t>(graph_.valueWidth(item.value));
                    if (item.constant)
                    {
                        const VariableId constantVar = internConstant(
                            typeForValue(item.value), *item.constant);
                        const std::array<VariableId, 1> results{destination};
                        const std::array<VariableId, 1> operands{constantVar};
                        addInstruction(Opcode::Assign, results, operands);
                        ++chainReplayEmitCount_;
                        continue;
                    }
                    if (item.segments.size() == 1 &&
                        item.segments.front().lsb == 0 &&
                        item.segments.front().width == valueWidth &&
                        static_cast<uint32_t>(
                            graph_.valueWidth(item.segments.front().root)) ==
                            valueWidth)
                    {
                        const VariableId source =
                            valueMap_[item.segments.front().root.index];
                        const std::array<VariableId, 1> results{destination};
                        const std::array<VariableId, 1> operands{source};
                        addInstruction(Opcode::Assign, results, operands);
                        ++chainReplayEmitCount_;
                        continue;
                    }
                    // AM Concat operand[0] sits at the most significant end;
                    // reverse the LSB-first segment list.
                    std::vector<VariableId> concatOperands;
                    concatOperands.reserve(item.segments.size());
                    bool failed = false;
                    for (auto it = item.segments.rbegin(); it != item.segments.rend();
                         ++it)
                    {
                        const ChainReplaySegment &segment = *it;
                        if (!segment.root.valid() ||
                            segment.root.index >= valueMap_.size())
                        {
                            failed = true;
                            break;
                        }
                        const VariableId rootVar = valueMap_[segment.root.index];
                        if (!rootVar.valid())
                        {
                            failed = true;
                            break;
                        }
                        const uint32_t rootWidth = static_cast<uint32_t>(
                            graph_.valueWidth(segment.root));
                        if (segment.lsb == 0 && segment.width == rootWidth)
                        {
                            concatOperands.push_back(rootVar);
                            continue;
                        }
                        const VariableId window = addVariable(
                            Type::bitVector(segment.width), amGraph_.zeroInit());
                        ++freshTemporaryCount_;
                        const std::array<VariableId, 1> results{window};
                        const std::array<VariableId, 1> operands{rootVar};
                        const InstructionId slice =
                            addInstruction(Opcode::SliceStatic, results, operands);
                        amGraph_.setSliceStaticAttributes(slice, segment.lsb);
                        ++chainReplayEmitCount_;
                        concatOperands.push_back(window);
                    }
                    if (failed)
                    {
                        error("chain replay segment root is unmapped",
                              "grhsim-am-lowering");
                        return;
                    }
                    const InstructionId concat =
                        addInstruction(Opcode::Concat,
                                       std::span<const VariableId>{&destination, 1},
                                       std::span<const VariableId>{
                                           concatOperands.data(),
                                           concatOperands.size()});
                    (void)concat;
                    ++chainReplayEmitCount_;
                }
            }

            // Emit the merged write for one lane-write fan. Runs at the
            // fan's flush op position inside lowerInstructions. The merged
            // write's data is a full-width concat: member lanes carry
            // mux(cond, data lane window, old target lane), gaps between
            // lanes and any absorbed prefix-write effects fall back to the
            // old target value, so the single write reproduces the whole
            // fan's net update exactly.
            void emitLaneWriteFan(const LaneWriteFan &fan)
            {
                const Operation flushOperation = graph_.getOperation(fan.flushOp);
                const auto edges = requiredAttr<std::vector<std::string>>(
                    flushOperation, "eventEdge");
                if (!edges)
                {
                    error("lane write fan flush op lacks eventEdge",
                          opContext(flushOperation));
                    return;
                }
                const auto events = lowerEvents(flushOperation, 3, *edges);
                if (!events || events->empty())
                {
                    error("lane write fan event lowering failed",
                          opContext(flushOperation));
                    return;
                }
                const ScopedGsimNodeId nodeScope(currentGsimNodeId_,
                                                 fan.nodeId);

                // Old-value base: the current target blended with any
                // absorbed prefix writes in commit order.
                VariableId oldValue = fan.target;
                auto emitBinary = [&](Opcode opcode, VariableId lhs,
                                      VariableId rhs, uint32_t width) {
                    const VariableId result = addVariable(
                        Type::bitVector(width), amGraph_.zeroInit());
                    ++freshTemporaryCount_;
                    const std::array<VariableId, 1> results{result};
                    const std::array<VariableId, 2> operands{lhs, rhs};
                    addInstruction(opcode, results, operands);
                    return result;
                };
                const Type targetType = Type::bitVector(fan.targetWidth);
                for (OperationId absorbed : fan.absorbedPrefix)
                {
                    const Operation op = graph_.getOperation(absorbed);
                    const std::vector<uint64_t> maskWords =
                        *constantWordsOf(valueMap_[op.operands()[2].index]);
                    const std::vector<uint64_t> dataWords =
                        *constantWordsOf(valueMap_[op.operands()[1].index]);
                    std::vector<uint64_t> notMaskWords = maskWords;
                    std::vector<uint64_t> maskedDataWords = maskWords;
                    for (std::size_t word = 0; word < maskWords.size(); ++word)
                    {
                        notMaskWords[word] = ~maskWords[word];
                        maskedDataWords[word] &= dataWords[word];
                    }
                    const VariableId notMaskVar =
                        internConstant(targetType, std::move(notMaskWords));
                    const VariableId maskedDataVar =
                        internConstant(targetType, std::move(maskedDataWords));
                    const VariableId kept = emitBinary(
                        Opcode::And, oldValue, notMaskVar, fan.targetWidth);
                    const VariableId blended = emitBinary(
                        Opcode::Or, kept, maskedDataVar, fan.targetWidth);
                    const VariableId condVar =
                        valueMap_[op.operands()[0].index];
                    if (isConstantZero(condVar))
                    {
                        continue;
                    }
                    if (isConstantOne(condVar))
                    {
                        oldValue = blended;
                        continue;
                    }
                    const VariableId muxed = addVariable(
                        targetType, amGraph_.zeroInit());
                    ++freshTemporaryCount_;
                    const std::array<VariableId, 1> results{muxed};
                    const std::array<VariableId, 3> operands{condVar, blended,
                                                             oldValue};
                    addInstruction(Opcode::Mux, results, operands);
                    oldValue = muxed;
                }

                // Per-member lane data: mux(cond, data lane window, old
                // target lane). Assemble the full-width concat over member
                // lanes; gaps carry zero (the merged write's union mask
                // never commits them).
                struct LanePiece
                {
                    uint32_t lo;
                    uint32_t width;
                    VariableId variable;
                };
                std::vector<LanePiece> pieces;
                std::vector<uint64_t> unionMaskWords((fan.targetWidth + 63U) /
                                                         64U,
                                                     UINT64_C(0));
                bool failed = false;
                for (const LaneWriteMember &member : fan.members)
                {
                    // Resolve the data lane window.
                    VariableId windowVar;
                    const auto found =
                        chainProvenance_.find(member.dataValue.index);
                    if (found != chainProvenance_.end())
                    {
                        // Chain-produced data: the lane window must resolve
                        // to exactly one root segment.
                        std::vector<ChainReplaySegment> window;
                        const uint64_t wantLo = member.laneLo;
                        const uint64_t wantHi = wantLo + member.laneWidth;
                        uint64_t position = 0;
                        for (const ChainReplaySegment &seg : found->second)
                        {
                            const uint64_t segLo = position;
                            const uint64_t segHi = segLo + seg.width;
                            position = segHi;
                            const uint64_t lo = std::max(segLo, wantLo);
                            const uint64_t hi = std::min(segHi, wantHi);
                            if (lo < hi)
                            {
                                window.push_back(ChainReplaySegment{
                                    .root = seg.root,
                                    .lsb = static_cast<uint32_t>(
                                        seg.lsb + (lo - segLo)),
                                    .width = static_cast<uint32_t>(hi - lo),
                                });
                            }
                        }
                        if (window.size() != 1)
                        {
                            failed = true;
                            break;
                        }
                        const ChainReplaySegment &segment = window.front();
                        const VariableId rootVar =
                            valueMap_[segment.root.index];
                        const uint32_t rootWidth = static_cast<uint32_t>(
                            graph_.valueWidth(segment.root));
                        if (segment.lsb == 0 && segment.width == rootWidth)
                        {
                            windowVar = rootVar;
                        }
                        else
                        {
                            windowVar = addVariable(
                                Type::bitVector(segment.width),
                                amGraph_.zeroInit());
                            ++freshTemporaryCount_;
                            const std::array<VariableId, 1> results{windowVar};
                            const std::array<VariableId, 1> operands{rootVar};
                            const InstructionId slice = addInstruction(
                                Opcode::SliceStatic, results, operands);
                            amGraph_.setSliceStaticAttributes(slice,
                                                              segment.lsb);
                        }
                    }
                    else
                    {
                        const VariableId dataVar =
                            valueMap_[member.dataValue.index];
                        windowVar = addVariable(
                            Type::bitVector(member.laneWidth),
                            amGraph_.zeroInit());
                        ++freshTemporaryCount_;
                        const std::array<VariableId, 1> results{windowVar};
                        const std::array<VariableId, 1> operands{dataVar};
                        const InstructionId slice = addInstruction(
                            Opcode::SliceStatic, results, operands);
                        amGraph_.setSliceStaticAttributes(slice,
                                                          member.laneLo);
                    }
                    // Old target lane.
                    VariableId oldLane = oldValue;
                    if (member.laneWidth != fan.targetWidth ||
                        member.laneLo != 0)
                    {
                        oldLane = addVariable(
                            Type::bitVector(member.laneWidth),
                            amGraph_.zeroInit());
                        ++freshTemporaryCount_;
                        const std::array<VariableId, 1> results{oldLane};
                        const std::array<VariableId, 1> operands{oldValue};
                        const InstructionId slice = addInstruction(
                            Opcode::SliceStatic, results, operands);
                        amGraph_.setSliceStaticAttributes(slice,
                                                          member.laneLo);
                    }
                    const VariableId condVar =
                        valueMap_[member.condValue.index];
                    VariableId laneVar = windowVar;
                    if (!isConstantOne(condVar))
                    {
                        if (isConstantZero(condVar))
                        {
                            laneVar = oldLane;
                        }
                        else
                        {
                            laneVar = addVariable(
                                Type::bitVector(member.laneWidth),
                                amGraph_.zeroInit());
                            ++freshTemporaryCount_;
                            const std::array<VariableId, 1> results{laneVar};
                            const std::array<VariableId, 3> operands{
                                condVar, windowVar, oldLane};
                            addInstruction(Opcode::Mux, results, operands);
                        }
                    }
                    pieces.push_back(LanePiece{.lo = member.laneLo,
                                               .width = member.laneWidth,
                                               .variable = laneVar});
                    for (uint64_t bit = member.laneLo;
                         bit < static_cast<uint64_t>(member.laneLo) +
                                   member.laneWidth;
                         ++bit)
                    {
                        unionMaskWords[bit / 64U] |= UINT64_C(1) << (bit % 64U);
                    }
                }
                if (failed)
                {
                    error("lane write fan window resolution failed",
                          opContext(flushOperation));
                    return;
                }
                // Mask the tail bits of the union mask's last word.
                if (fan.targetWidth % 64U != 0)
                {
                    unionMaskWords.back() &=
                        (UINT64_C(1) << (fan.targetWidth % 64U)) - UINT64_C(1);
                }
                std::sort(pieces.begin(), pieces.end(),
                          [](const LanePiece &lhs, const LanePiece &rhs) {
                              return lhs.lo > rhs.lo;  // concat is MSB-first
                          });
                std::vector<VariableId> concatOperands;
                concatOperands.reserve(pieces.size() + 2);
                uint32_t cursor = fan.targetWidth;
                for (const LanePiece &piece : pieces)
                {
                    const uint32_t gap = cursor - (piece.lo + piece.width);
                    if (gap != 0)
                    {
                        concatOperands.push_back(internConstant(
                            Type::bitVector(gap),
                            std::vector<uint64_t>((gap + 63U) / 64U,
                                                  UINT64_C(0))));
                    }
                    concatOperands.push_back(piece.variable);
                    cursor = piece.lo;
                }
                if (cursor != 0)
                {
                    concatOperands.push_back(internConstant(
                        Type::bitVector(cursor),
                        std::vector<uint64_t>((cursor + 63U) / 64U,
                                              UINT64_C(0))));
                }
                const VariableId dataVar = addVariable(
                    targetType, amGraph_.zeroInit());
                ++freshTemporaryCount_;
                addInstruction(Opcode::Concat,
                               std::span<const VariableId>{&dataVar, 1},
                               std::span<const VariableId>{
                                   concatOperands.data(),
                                   concatOperands.size()});
                // Merged cond: constant-one if any member cond is tied on,
                // a single var when all share one, else an or-tree.
                VariableId condMerged = VariableId::invalid();
                VariableId constantOneCond = VariableId::invalid();
                for (const LaneWriteMember &member : fan.members)
                {
                    const VariableId condVar =
                        valueMap_[member.condValue.index];
                    if (isConstantOne(condVar))
                    {
                        constantOneCond = condVar;
                        break;
                    }
                    if (!condMerged.valid())
                    {
                        condMerged = condVar;
                    }
                    else if (condVar.value != condMerged.value)
                    {
                        condMerged = emitBinary(Opcode::Or, condMerged, condVar, 1);
                    }
                }
                PendingStateWrite merged{
                    .opcode = Opcode::RegisterWrite,
                    .target = fan.target,
                    .cond = constantOneCond.valid() ? constantOneCond
                                                    : condMerged,
                    .mask = internConstant(targetType,
                                           std::move(unionMaskWords)),
                    .data = dataVar,
                    .addr = VariableId::invalid(),
                    .events = *events,
                    .explicitOrder = fan.members.back().explicitOrder,
                    .context = opContext(flushOperation),
                    .gsimNodeId = fan.nodeId,
                };
                if (!merged.cond.valid() || !merged.mask.valid() ||
                    !merged.data.valid())
                {
                    error("lane write fan merged write is incomplete",
                          opContext(flushOperation));
                    return;
                }
                laneWriteMergedCount_ += fan.members.size() - 1;
                pendingStateWrites_.push_back(std::move(merged));
            }

            void lowerInstructions()
            {
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
                    const uint32_t currentOpOrdinal = lowerOpOrdinal_++;
                    // NO0006: every instruction created while lowering this
                    // op inherits the op's gsim node id (trigger-owner
                    // rule); ops without provenance lower unowned (-1).
                    const std::optional<int64_t> gsimNodeId =
                        optionalAttr<int64_t>(op, kGsimNodeIdAttr);
                    if (gsimNodeId)
                    {
                        amGraph_.setGsimNodeProvenance(true);
                    }
                    const ScopedGsimNodeId gsimNodeScope(currentGsimNodeId_,
                                                         gsimNodeId.value_or(-1));
                    // NO0012 Tier 2: fan member write ops are absorbed into
                    // the merged write emitted at the fan's flush op.
                    if (const auto fanIt = laneWriteSkipFan_.find(operation.index);
                        fanIt != laneWriteSkipFan_.end())
                    {
                        const LaneWriteFan &fan = laneWriteFans_[fanIt->second];
                        if (fan.flushOp == operation)
                        {
                            emitLaneWriteFan(fan);
                        }
                        continue;
                    }
                    // NO0012: planned chain ops skip the standard lowering;
                    // the group's replay emits at its flush op position.
                    if (const auto skipped =
                            chainReplaySkipGroup_.find(operation.index);
                        skipped != chainReplaySkipGroup_.end())
                    {
                        ++chainReplaySkipCount_;
                        const ChainReplayGroup &replayGroup =
                            chainReplayGroups_[skipped->second];
                        if (replayGroup.flushOp == operation)
                        {
                            emitChainReplay(replayGroup);
                        }
                        continue;
                    }
                    if (const auto opcode = combinationalOpcode(op.kind()))
                    {
                        lowerCombinational(op, *opcode);
                        continue;
                    }
                    switch (op.kind())
                    {
                    case OperationKind::kConstant:
                        if (!op.operands().empty() || op.results().size() != 1)
                        {
                            error("kConstant has invalid arity", opContext(op));
                        }
                        break;
                    case OperationKind::kArrayLaneConst:
                        if (!op.operands().empty() || op.results().size() != 1)
                        {
                            error("kArrayLaneConst has invalid arity", opContext(op));
                        }
                        break;
                    case OperationKind::kRegister:
                    case OperationKind::kLatch:
                    case OperationKind::kMemory:
                    case OperationKind::kDpicImport:
                        break;
                    case OperationKind::kRegisterReadPort:
                    case OperationKind::kLatchReadPort:
                        if (!op.operands().empty() || op.results().size() != 1)
                        {
                            error("state read port has invalid arity", opContext(op));
                        }
                        break;
                    case OperationKind::kRegisterWritePort:
                        lowerRegisterWrite(op);
                        break;
                    case OperationKind::kLatchWritePort:
                        lowerLatchWrite(op);
                        break;
                    case OperationKind::kMemoryReadPort:
                        lowerMemoryRead(op);
                        break;
                    case OperationKind::kMemoryWritePort:
                        lowerMemoryWrite(op);
                        break;
                    case OperationKind::kMemoryFillPort:
                        lowerMemoryFill(op);
                        break;
                    case OperationKind::kMemoryReadAllPort:
                        lowerMemoryReadAll(op);
                        break;
                    case OperationKind::kMemoryWriteLanesPort:
                        lowerMemoryWriteLanes(op);
                        break;
                    case OperationKind::kSystemFunction:
                        lowerSystemFunction(op);
                        break;
                    case OperationKind::kSystemTask:
                        lowerSystemTask(op);
                        break;
                    case OperationKind::kDpicCall:
                        lowerDpiCall(op);
                        break;
                    case OperationKind::kInstance:
                    case OperationKind::kBlackbox:
                    case OperationKind::kXMRRead:
                    case OperationKind::kXMRWrite:
                        // Rejected during preflight.
                        break;
                    default:
                        error("operation kind has no GRHSIM-AM lowering",
                              opContext(op));
                        break;
                    }
                }
            }

            const Graph &graph_;
            diag::Diagnostics &diagnostics_;
            GrhIRToGrhSimAMGraphLoweringOptions options_;
            AmGraph amGraph_;
            ProgramInterface interface_;
            bool failed_ = false;
            std::map<Type, TypeId> typeIds_;
            std::map<Type, VariableId> zeroConstants_;
            std::unordered_map<std::string, StringId> stringIds_;
            std::vector<VariableId> valueMap_;
            std::vector<VariableId> stateByOperation_;
            std::vector<Type> stateTypeByOperation_;
            std::vector<DpiImportInfo> dpiImportByOperation_;
            std::vector<std::optional<ExplicitOrder>> explicitOrderByOperation_;
            // NO0012 Tier 1: wide concat/slice chain lazy replay. One entry
            // per gsim node group whose wide chain ops lower to per-segment
            // replay instead of O(steps x width) full-width rebuilds.
            std::vector<ChainReplayGroup> chainReplayGroups_;
            std::unordered_map<uint32_t, uint32_t> chainReplaySkipGroup_;
            bool disableChainReplay_ = false;
            std::optional<int64_t> debugChainNode_;
            uint64_t chainReplaySkipCount_ = 0;
            uint64_t chainReplayEmitCount_ = 0;
            uint64_t chainReplayRejectCount_ = 0;
            uint64_t chainReplayPlanFoldCount_ = 0;

            std::unordered_set<uint32_t> exposedValues_;
            std::unordered_set<uint32_t> declaredValueIndices_;
            std::unordered_set<uint32_t> declaredStateIndices_;
            std::vector<PendingStateWrite> pendingStateWrites_;
            // gsim node id of the GRH op currently being lowered (NO0006);
            // -1 outside op lowering or for unowned ops.
            int64_t currentGsimNodeId_ = -1;
            uint32_t nextOrderedGroup_ = 0;
            std::unordered_map<uint32_t, uint32_t> stateOrderGroups_;
            std::map<std::pair<Opcode, uint32_t>, VariableId> eventDetectorMemo_;
            std::unordered_map<uint32_t, VariableId> preCommitSnapshots_;
            std::vector<uint32_t> firstUseOrdinal_;
            uint32_t lowerOpOrdinal_ = 0;
            uint64_t freshTemporaryCount_ = 0;
            std::size_t flattenedUnknownLiterals_ = 0;
            // Interning cache for lowering-time folded constants and the
            // escape hatch for the fold/merge (NO0010), see the constructor.
            std::map<std::pair<Type, std::vector<uint64_t>>, VariableId>
                foldedConstants_;
            bool disableLoweringConstFold_ = false;
            bool disableWriteMerge_ = false;
            uint64_t lowerConstFoldCount_ = 0;
            uint64_t mergedStateWriteCount_ = 0;
            // NO0004 import QC: per-site attribution of Assign instructions so
            // the pre-optimize opcode census can be reconciled exactly against
            // the source GRH op census.
            uint64_t assignFromGrhCount_ = 0;
            uint64_t assignPreCommitCount_ = 0;
            uint64_t assignCoerceCount_ = 0;
            uint64_t assignResultBridgeCount_ = 0;
            uint64_t assignDpiBridgeCount_ = 0;
        };
    } // namespace

    std::optional<AmGraph>
    GrhIRToGrhSimAMGraphLowering::lower(const grh::Graph &graph,
                           diag::Diagnostics &diagnostics)
    {
        return LoweringContext(graph, diagnostics, options_).run();
    }

} // namespace wolvrix::lib::grhsim::am
