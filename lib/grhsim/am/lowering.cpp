#include "grhsim/am/lowering.hpp"

#include "grhsim/am/builder.hpp"
#include "grhsim/am/opcode_traits.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
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

        struct PendingStateOrderedEffect
        {
            InstructionId instruction;
            VariableId target;
            std::optional<ExplicitOrder> explicitOrder;
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
                            const GrhToAmLoweringOptions &options)
                : graph_(graph), diagnostics_(diagnostics), options_(options)
            {
            }

            std::optional<LinearProgramArtifact> run()
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
                    lowerInstructions();
                    if (failed_)
                    {
                        return std::nullopt;
                    }
                    materializeStateOrderedEffects();

                    std::sort(orderedEffects_.begin(), orderedEffects_.end(),
                              [](const OrderedEffect &lhs, const OrderedEffect &rhs) {
                                  if (lhs.group != rhs.group)
                                  {
                                      return lhs.group < rhs.group;
                                  }
                                  return lhs.ordinal < rhs.ordinal;
                              });

                    LinearProgramArtifact artifact{
                        .program = builder_.finish(),
                        .interface = std::move(interface_),
                        .schedulingFacts = SchedulingFacts{
                            .variableRoles = std::move(variableRoles_),
                            .instructionEffects = std::move(instructionEffects_),
                            .orderedEffects = std::move(orderedEffects_),
                        },
                        .preCommitSnapshots =
                            std::move(preCommitSnapshotBindings_),
                    };
                    const ValidationResult validation =
                        validate(artifact, ValidationOptions{.level = ValidationLevel::Semantic,
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
                    return artifact;
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
                        if (op.kind() != OperationKind::kMemoryWritePort)
                        {
                            error("memory write priority attributes are only valid on kMemoryWritePort",
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
                        if (operands.size() >= 4)
                        {
                            std::vector<ValueId> events(operands.begin() + 4, operands.end());
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
                std::size_t preCommitSnapshotUpperBound = 0;
                std::size_t instructionCount = 0;
                std::size_t operandCount = 0;
                std::size_t resultCount = 0;
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
                    std::size_t sampledOperandCount = 0;
                    switch (op.kind())
                    {
                    case OperationKind::kRegisterWritePort:
                        sampledOperandCount = std::min<std::size_t>(3, op.operands().size());
                        break;
                    case OperationKind::kMemoryWritePort:
                        sampledOperandCount = std::min<std::size_t>(4, op.operands().size());
                        break;
                    case OperationKind::kMemoryFillPort:
                        sampledOperandCount = std::min<std::size_t>(2, op.operands().size());
                        break;
                    default:
                        break;
                    }
                    for (std::size_t index = 0; index < sampledOperandCount; ++index)
                    {
                        const OperationId definition = graph_.valueDef(op.operands()[index]);
                        preCommitSnapshotUpperBound +=
                            definition.valid() &&
                            graph_.opKind(definition) == OperationKind::kRegisterReadPort;
                    }
                }
                instructionCount += eventCount;
                operandCount += 2 * eventCount;
                resultCount += eventCount;

                ProgramReserve reserve;
                reserve.types = 64;
                reserve.strings = stringCount + 64;
                reserve.stringBytes = stringBytes + 4096;
                reserve.initDescriptors = 2 + graph_.operations().size() / 16;
                reserve.initActions = graph_.operations().size() / 16;
                reserve.literals = graph_.operations().size() / 16;
                reserve.literalWords = graph_.operations().size() / 8;
                reserve.variables = graph_.values().size() + stateCount + 2 * eventCount +
                                    preCommitSnapshotUpperBound;
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
                builder_.reserve(reserve);
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
                const TypeId id = builder_.addType(type);
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
                const StringId id = builder_.addString(text);
                stringIds_.emplace(std::string(text), id);
                return id;
            }

            VariableId addVariable(TypeId type, InitId init,
                                   std::optional<StringId> label = std::nullopt,
                                   VariableRole role = VariableRole::None)
            {
                const VariableId id = builder_.addVariable(type, init, label);
                if (id.value != variableRoles_.size())
                {
                    throw std::logic_error("AM lowering variable role table lost dense alignment");
                }
                variableRoles_.push_back(role);
                return id;
            }

            VariableId addVariable(const Type &type, InitId init,
                                   std::optional<StringId> label = std::nullopt,
                                   VariableRole role = VariableRole::None)
            {
                return addVariable(internType(type), init, label, role);
            }

            void addRole(VariableId variable, VariableRole role)
            {
                if (!variable.valid() || variable.value >= variableRoles_.size())
                {
                    throw std::logic_error("invalid AM variable role update");
                }
                variableRoles_[variable.value] = variableRoles_[variable.value] | role;
            }

            InstructionId addInstruction(Opcode opcode,
                                         std::span<const VariableId> results,
                                         std::span<const VariableId> operands,
                                         std::optional<InstructionEffect> effect = std::nullopt)
            {
                const InstructionId id = builder_.addInstruction(opcode, results, operands);
                if (id.value != instructionEffects_.size())
                {
                    throw std::logic_error("AM lowering effect table lost dense alignment");
                }
                if (effect)
                {
                    instructionEffects_.push_back(*effect);
                }
                else
                {
                    switch (opcodeTraits(opcode).effect)
                    {
                    case OpcodeEffect::Pure:
                        instructionEffects_.push_back(InstructionEffect::Pure);
                        break;
                    case OpcodeEffect::ChangeDetector:
                    case OpcodeEffect::StateReadWrite:
                        instructionEffects_.push_back(InstructionEffect::StateReadWrite);
                        break;
                    case OpcodeEffect::StateRead:
                        instructionEffects_.push_back(InstructionEffect::StateRead);
                        break;
                    case OpcodeEffect::HostRead:
                        instructionEffects_.push_back(InstructionEffect::HostRead);
                        break;
                    case OpcodeEffect::HostEffect:
                    case OpcodeEffect::Activation:
                        instructionEffects_.push_back(InstructionEffect::HostEffect);
                        break;
                    }
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
                const LiteralId literal = builder_.addBitLiteral(scalarType, *words);
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
                    literalId = builder_.addBitLiteral(typeId, *words);
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
                        literalId = builder_.addBitLiteral(typeId,
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
                    literalId = builder_.addStringLiteral(typeId, *literal);
                }
                else
                {
                    error("GRH constant cannot have Array type", opContext(op));
                    return std::nullopt;
                }
                return builder_.addConstantInit(literalId);
            }

            std::optional<InitId> registerInit(const Operation &op, TypeId type,
                                               uint32_t width, bool isSigned)
            {
                const auto value = optionalAttr<std::string>(op, "initValue");
                if (!value)
                {
                    return builder_.undefInit();
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
                return builder_.addActionsInit(std::span<const InitAction>(&action, 1));
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
                    return builder_.undefInit();
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
                return builder_.addActionsInit(actions);
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
                                   : std::optional<InitId>(builder_.undefInit());
                    }
                    if (!init)
                    {
                        continue;
                    }
                    const StringId label = internString(op.symbolText());
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
                        builder_.addDpiImport(symbolId, parameters, returnValue);
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
                        const Type actual = builder_.view().type(
                            builder_.view().variable(*target).type);
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
                    InitId init = builder_.zeroInit();
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

                const TypeId type = builder_.view().variable(source).type;
                const VariableId snapshot = addVariable(type, builder_.undefInit());
                preCommitSnapshots_.emplace(source.value, snapshot);
                preCommitSnapshotBindings_.push_back(PreCommitSnapshot{
                    .source = source,
                    .target = snapshot,
                });
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
                const VariableId converted = addVariable(targetType, builder_.zeroInit());
                const std::array<VariableId, 1> results{converted};
                const std::array<VariableId, 1> operands{source};
                addInstruction(Opcode::Assign, results, operands);
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
                const LiteralId literal = builder_.addBitLiteral(typeId, words);
                const VariableId variable =
                    addVariable(typeId, builder_.addConstantInit(literal));
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
                    addVariable(Type::bitVector(1), builder_.zeroInit());
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
                default: return std::nullopt;
                }
            }

            void lowerCombinational(const Operation &op, Opcode opcode)
            {
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
                    expectedOperands = 1;
                    break;
                case Opcode::Mux:
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
                const VariableId destination = mappedValue(results.front(), op);
                if (!destination.valid())
                {
                    return;
                }
                VariableId nativeDestination = destination;
                if (*nativeType != resultType)
                {
                    nativeDestination = addVariable(*nativeType, builder_.zeroInit());
                    ++freshTemporaryCount_;
                }
                const std::array<VariableId, 1> loweredResults{nativeDestination};
                const InstructionId instruction =
                    addInstruction(opcode, loweredResults, loweredOperands);
                if (opcode == Opcode::SliceStatic)
                {
                    builder_.setSliceStaticAttributes(instruction, sliceLsb);
                }
                if (nativeDestination != destination)
                {
                    const std::array<VariableId, 1> finalResults{destination};
                    const std::array<VariableId, 1> finalOperands{nativeDestination};
                    addInstruction(Opcode::Assign, finalResults, finalOperands);
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
                    const Type rawType = typeForValue(raw);
                    const VariableId old = addVariable(rawType, builder_.undefInit());
                    const VariableId event = addVariable(eventType, builder_.zeroInit());
                    const std::array<VariableId, 1> results{event};
                    const std::array<VariableId, 2> eventOperands{
                        mappedValue(raw, op), old};
                    if (!eventOperands[0].valid())
                    {
                        return std::nullopt;
                    }
                    addInstruction(opcode, results, eventOperands);
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

            void materializeStateOrderedEffects()
            {
                std::map<uint32_t, std::vector<PendingStateOrderedEffect>> effectsByTarget;
                for (const PendingStateOrderedEffect &effect : pendingStateOrderedEffects_)
                {
                    effectsByTarget[effect.target.value].push_back(effect);
                }

                for (auto &[targetValue, effects] : effectsByTarget)
                {
                    std::map<uint32_t, std::vector<PendingStateOrderedEffect>>
                        explicitGroups;
                    for (const PendingStateOrderedEffect &effect : effects)
                    {
                        if (effect.explicitOrder)
                        {
                            explicitGroups[effect.explicitOrder->group].push_back(effect);
                        }
                    }

                    for (auto &[explicitGroup, members] : explicitGroups)
                    {
                        (void)explicitGroup;
                        std::sort(members.begin(), members.end(),
                                  [](const PendingStateOrderedEffect &lhs,
                                     const PendingStateOrderedEffect &rhs) {
                                      return lhs.explicitOrder->ordinal <
                                             rhs.explicitOrder->ordinal;
                                  });
                    }

                    std::vector<PendingStateOrderedEffect> ordered;
                    ordered.reserve(effects.size());
                    std::unordered_set<uint32_t> emittedExplicitGroups;
                    for (const PendingStateOrderedEffect &effect : effects)
                    {
                        if (!effect.explicitOrder)
                        {
                            ordered.push_back(effect);
                            continue;
                        }
                        const uint32_t explicitGroup = effect.explicitOrder->group;
                        if (!emittedExplicitGroups.insert(explicitGroup).second)
                        {
                            continue;
                        }
                        const std::vector<PendingStateOrderedEffect> &members =
                            explicitGroups.at(explicitGroup);
                        ordered.insert(ordered.end(), members.begin(), members.end());
                    }

                    const uint32_t group = stateOrderGroup(VariableId{targetValue});
                    for (std::size_t ordinal = 0; ordinal < ordered.size(); ++ordinal)
                    {
                        orderedEffects_.push_back(OrderedEffect{
                            .instruction = ordered[ordinal].instruction,
                            .group = group,
                            .ordinal = static_cast<uint32_t>(ordinal),
                        });
                    }
                }
                pendingStateOrderedEffects_.clear();
            }

            void addOrderedEffect(const Operation &op, InstructionId instruction,
                                  VariableId stateTarget = VariableId::invalid())
            {
                const std::optional<ExplicitOrder> explicitOrder =
                    op.id().index < explicitOrderByOperation_.size()
                        ? explicitOrderByOperation_[op.id().index]
                        : std::nullopt;
                if (stateTarget.valid())
                {
                    pendingStateOrderedEffects_.push_back(PendingStateOrderedEffect{
                        .instruction = instruction,
                        .target = stateTarget,
                        .explicitOrder = explicitOrder,
                    });
                    return;
                }
                if (explicitOrder)
                {
                    orderedEffects_.push_back(OrderedEffect{
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
                const Type targetType = builder_.view().type(
                    builder_.view().variable(*target).type);
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
                std::vector<VariableId> operands{
                    preCommitValue(op.operands()[0], mappedValue(op.operands()[0], op), op),
                    preCommitValue(op.operands()[2], mappedValue(op.operands()[2], op), op),
                    next,
                    *target,
                };
                operands.insert(operands.end(), events->begin(), events->end());
                if (std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                const InstructionId instruction =
                    addInstruction(Opcode::RegisterWrite, {}, operands);
                addOrderedEffect(op, instruction, *target);
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
                const Type targetType = builder_.view().type(
                    builder_.view().variable(*target).type);
                if (!requireLogic(op.operands()[1], op, targetType.bitWidth) ||
                    !requireLogic(op.operands()[2], op, targetType.bitWidth))
                {
                    return;
                }
                const VariableId next = coerceToType(
                    mappedValue(op.operands()[1], op), typeForValue(op.operands()[1]),
                    targetType);
                if (!next.valid())
                {
                    error("latch nextValue cannot be converted to target Type", opContext(op));
                    return;
                }
                const std::array<VariableId, 4> operands{
                    mappedValue(op.operands()[0], op),
                    mappedValue(op.operands()[2], op),
                    next,
                    *target,
                };
                if (std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                const InstructionId instruction =
                    addInstruction(Opcode::LatchWrite, {}, operands);
                addOrderedEffect(op, instruction, *target);
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
                const Type targetType = builder_.view().type(
                    builder_.view().variable(*target).type);
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
                const Type targetType = builder_.view().type(
                    builder_.view().variable(*target).type);
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
                std::vector<VariableId> operands{
                    preCommitValue(op.operands()[0], mappedValue(op.operands()[0], op), op),
                    preCommitValue(op.operands()[1], mappedValue(op.operands()[1], op), op),
                    preCommitValue(op.operands()[3], mappedValue(op.operands()[3], op), op),
                    data,
                    *target,
                };
                operands.insert(operands.end(), events->begin(), events->end());
                if (std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                const InstructionId instruction =
                    addInstruction(Opcode::MemoryWrite, {}, operands);
                addOrderedEffect(op, instruction, *target);
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
                const Type targetType = builder_.view().type(
                    builder_.view().variable(*target).type);
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
                std::vector<VariableId> operands{
                    preCommitValue(op.operands()[0], mappedValue(op.operands()[0], op), op),
                    preCommitValue(op.operands()[1], mappedValue(op.operands()[1], op), op),
                    *target,
                };
                operands.insert(operands.end(), events->begin(), events->end());
                if (std::any_of(operands.begin(), operands.end(),
                                [](VariableId value) { return !value.valid(); }))
                {
                    return;
                }
                const InstructionId instruction =
                    addInstruction(Opcode::MemoryFill, {}, operands);
                addOrderedEffect(op, instruction, *target);
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
                builder_.setSystemFunctionAttributes(
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
                builder_.setSystemTaskAttributes(
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
                            addVariable(import.returnType, builder_.zeroInit());
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
                                addVariable(parameter.type, builder_.zeroInit());
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
                builder_.setDpiCallAttributes(
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
                }
            }

            void lowerInstructions()
            {
                for (OperationId operation : graph_.operations())
                {
                    const Operation op = graph_.getOperation(operation);
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
            GrhToAmLoweringOptions options_;
            LinearProgramBuilder builder_;
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
            std::unordered_set<uint32_t> exposedValues_;
            std::unordered_set<uint32_t> declaredValueIndices_;
            std::unordered_set<uint32_t> declaredStateIndices_;
            std::vector<VariableRole> variableRoles_;
            std::vector<InstructionEffect> instructionEffects_;
            std::vector<OrderedEffect> orderedEffects_;
            std::vector<PendingStateOrderedEffect> pendingStateOrderedEffects_;
            uint32_t nextOrderedGroup_ = 0;
            std::unordered_map<uint32_t, uint32_t> stateOrderGroups_;
            std::unordered_map<uint32_t, VariableId> preCommitSnapshots_;
            std::vector<PreCommitSnapshot> preCommitSnapshotBindings_;
            uint64_t freshTemporaryCount_ = 0;
            std::size_t flattenedUnknownLiterals_ = 0;
        };
    } // namespace

    std::optional<LinearProgramArtifact>
    GrhToAmLowering::lower(const grh::Graph &graph,
                           diag::Diagnostics &diagnostics)
    {
        return LoweringContext(graph, diagnostics, options_).run();
    }

} // namespace wolvrix::lib::grhsim::am
