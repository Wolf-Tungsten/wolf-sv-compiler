#include "grhsim/ir/lower.hpp"

#include "grhsim/ir/generic.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        using wolvrix::lib::grh::AttributeValue;
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;

        template <typename T>
        std::optional<T> sourceAttr(const Operation &op, std::string_view key)
        {
            const auto value = op.attr(key);
            if (!value)
            {
                return std::nullopt;
            }
            if (const auto *typed = std::get_if<T>(&*value))
            {
                return *typed;
            }
            return std::nullopt;
        }

        AttrValue convertAttrValue(Module &module, const AttributeValue &value)
        {
            return std::visit(
                [&](const auto &typed) -> AttrValue {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<T, std::string>)
                    {
                        return module.intern(typed);
                    }
                    else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                    {
                        std::vector<SymbolId> result;
                        result.reserve(typed.size());
                        for (const std::string &item : typed)
                        {
                            result.push_back(module.intern(item));
                        }
                        return result;
                    }
                    else
                    {
                        return typed;
                    }
                },
                value);
        }

        AttrKV convertAttr(Module &module, std::string_view key, const AttributeValue &value)
        {
            return AttrKV{module.intern(key), convertAttrValue(module, value)};
        }

        std::vector<AttrKV> convertAttrs(Module &module,
                                         std::span<const wolvrix::lib::grh::AttrKV> attrs)
        {
            std::vector<AttrKV> result;
            result.reserve(attrs.size());
            for (const auto &attr : attrs)
            {
                result.push_back(convertAttr(module, attr.key, attr.value));
            }
            return result;
        }

        std::string normalizeHostName(std::string_view raw)
        {
            while (!raw.empty() && raw.front() == '$')
            {
                raw.remove_prefix(1);
            }
            return std::string(raw);
        }

        std::optional<GenericOpcode> mappedOpcode(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kConstant: return GenericOpcode::Const;
            case OperationKind::kAdd: return GenericOpcode::Add;
            case OperationKind::kSub: return GenericOpcode::Sub;
            case OperationKind::kMul: return GenericOpcode::Mul;
            case OperationKind::kDiv: return GenericOpcode::Div;
            case OperationKind::kMod: return GenericOpcode::Mod;
            case OperationKind::kAnd: return GenericOpcode::And;
            case OperationKind::kOr: return GenericOpcode::Or;
            case OperationKind::kXor: return GenericOpcode::Xor;
            case OperationKind::kXnor: return GenericOpcode::Xnor;
            case OperationKind::kNot: return GenericOpcode::Not;
            case OperationKind::kLt: return GenericOpcode::Lt;
            case OperationKind::kLe: return GenericOpcode::Le;
            case OperationKind::kGt: return GenericOpcode::Gt;
            case OperationKind::kGe: return GenericOpcode::Ge;
            case OperationKind::kEq: return GenericOpcode::Eq;
            case OperationKind::kNe: return GenericOpcode::Ne;
            case OperationKind::kCaseEq: return GenericOpcode::CaseEq;
            case OperationKind::kCaseNe: return GenericOpcode::CaseNe;
            case OperationKind::kWildcardEq: return GenericOpcode::WildEq;
            case OperationKind::kWildcardNe: return GenericOpcode::WildNe;
            case OperationKind::kLogicAnd: return GenericOpcode::LogicAnd;
            case OperationKind::kLogicOr: return GenericOpcode::LogicOr;
            case OperationKind::kLogicNot: return GenericOpcode::LogicNot;
            case OperationKind::kReduceAnd: return GenericOpcode::ReduceAnd;
            case OperationKind::kReduceNand: return GenericOpcode::ReduceNand;
            case OperationKind::kReduceOr: return GenericOpcode::ReduceOr;
            case OperationKind::kReduceNor: return GenericOpcode::ReduceNor;
            case OperationKind::kReduceXor: return GenericOpcode::ReduceXor;
            case OperationKind::kReduceXnor: return GenericOpcode::ReduceXnor;
            case OperationKind::kShl: return GenericOpcode::Shl;
            case OperationKind::kLShr: return GenericOpcode::LShr;
            case OperationKind::kAShr: return GenericOpcode::AShr;
            case OperationKind::kMux: return GenericOpcode::Mux;
            case OperationKind::kAssign: return GenericOpcode::Assign;
            case OperationKind::kConcat: return GenericOpcode::Concat;
            case OperationKind::kReplicate: return GenericOpcode::Replicate;
            case OperationKind::kSliceStatic: return GenericOpcode::SliceStatic;
            case OperationKind::kSliceDynamic: return GenericOpcode::SliceDynamic;
            case OperationKind::kSliceArray: return GenericOpcode::SliceArray;
            case OperationKind::kArrayLaneConst: return GenericOpcode::ArrayLaneConst;
            case OperationKind::kArrayMux: return GenericOpcode::ArrayMux;
            case OperationKind::kArrayOnehot: return GenericOpcode::ArrayOnehot;
            case OperationKind::kArrayReduceOr: return GenericOpcode::ArrayReduceOr;
            case OperationKind::kArrayReduceAnd: return GenericOpcode::ArrayReduceAnd;
            case OperationKind::kArrayReduceXor: return GenericOpcode::ArrayReduceXor;
            case OperationKind::kArrayReduceLanesOr: return GenericOpcode::ArrayReduceLanesOr;
            case OperationKind::kArrayReduceLanesAnd: return GenericOpcode::ArrayReduceLanesAnd;
            case OperationKind::kArrayReduceLanesXor: return GenericOpcode::ArrayReduceLanesXor;
            case OperationKind::kArrayBroadcast: return GenericOpcode::ArrayBroadcast;
            case OperationKind::kRegisterReadPort: return GenericOpcode::RegRead;
            case OperationKind::kRegisterWritePort: return GenericOpcode::RegWrite;
            case OperationKind::kLatchReadPort: return GenericOpcode::LatchRead;
            case OperationKind::kLatchWritePort: return GenericOpcode::LatchWrite;
            case OperationKind::kMemoryReadPort: return GenericOpcode::MemRead;
            case OperationKind::kMemoryReadAllPort: return GenericOpcode::MemReadAll;
            case OperationKind::kMemoryWritePort: return GenericOpcode::MemWrite;
            case OperationKind::kMemoryWriteLanesPort: return GenericOpcode::MemWriteLanes;
            case OperationKind::kMemoryFillPort: return GenericOpcode::MemFill;
            case OperationKind::kSystemFunction:
            case OperationKind::kSystemTask:
            case OperationKind::kDpicCall:
                return GenericOpcode::HostCall;
            default:
                return std::nullopt;
            }
        }

        bool isDeclaration(OperationKind kind)
        {
            return kind == OperationKind::kRegister || kind == OperationKind::kLatch ||
                   kind == OperationKind::kMemory || kind == OperationKind::kDpicImport;
        }

        bool isResidualHierarchy(OperationKind kind)
        {
            return kind == OperationKind::kInstance || kind == OperationKind::kBlackbox ||
                   kind == OperationKind::kXMRRead || kind == OperationKind::kXMRWrite;
        }

        class Lowering
        {
        public:
            Lowering(const Graph &graph, wolvrix::lib::diag::Diagnostics &diagnostics)
                : graph_(graph), diagnostics_(diagnostics), module_(graph.symbol())
            {
            }

            std::optional<Module> run()
            {
                if (!checkSourceShape() || !lowerStateDeclarations() ||
                    !lowerPorts() || !lowerDpiImports() || !createOpSkeletons() ||
                    !materializeSourceLessValues() || !wireOperations() ||
                    !canonicalizeMemoryWritePriorities() || !writeOutputPorts())
                {
                    return std::nullopt;
                }
                module_.freeze();
                if (!module_.validate(diagnostics_))
                {
                    return std::nullopt;
                }
                return module_;
            }

        private:
            struct OutputPlan
            {
                StateId state;
                ValueId value;
            };

            struct MemoryWriterPlan
            {
                OpId target;
                StateId state;
                std::optional<std::string> priorityGroup;
                std::optional<int64_t> priority;
            };

            TypeId typeForValue(ValueId value)
            {
                const ValueType valueType = graph_.valueType(value);
                if (valueType == ValueType::Real)
                {
                    return module_.internRealType();
                }
                if (valueType == ValueType::String)
                {
                    return module_.internStringType();
                }
                const int32_t width = graph_.valueWidth(value);
                return width > 0 ? module_.internLogicType(static_cast<uint32_t>(width),
                                                           graph_.valueSigned(value))
                                 : TypeId::invalid();
            }

            SymbolId valueSymbol(ValueId value)
            {
                const auto symbol = graph_.valueSymbol(value);
                return symbol.valid() ? module_.intern(graph_.symbolText(symbol))
                                      : SymbolId::invalid();
            }

            SymbolId operationSymbol(OperationId op)
            {
                const auto symbol = graph_.operationSymbol(op);
                return symbol.valid() ? module_.intern(graph_.symbolText(symbol))
                                      : SymbolId::invalid();
            }

            std::string uniqueStateName(std::string_view requested, std::string_view suffix,
                                        bool forceSuffix = false)
            {
                std::string result(requested);
                if (!forceSuffix && !result.empty() && !module_.findState(result).valid())
                {
                    return result;
                }
                result = std::string(requested) + std::string(suffix);
                uint32_t ordinal = 0;
                while (result.empty() || module_.findState(result).valid())
                {
                    result = std::string(requested) + std::string(suffix) +
                             std::to_string(++ordinal);
                }
                return result;
            }

            bool checkSourceShape()
            {
                bool success = true;
                for (OperationId opId : graph_.operations())
                {
                    const OperationKind kind = graph_.opKind(opId);
                    if (isResidualHierarchy(kind))
                    {
                        diagnostics_.error(
                            "lower_grhsim requires hierarchy and XMR operations to be resolved",
                            "graph=" + graph_.symbol() + " op=" + std::to_string(opId.index));
                        success = false;
                    }
                    else if (!isDeclaration(kind) && !mappedOpcode(kind))
                    {
                        diagnostics_.error("GRH operation has no generic GRHSIM mapping: " +
                                               std::string(wolvrix::lib::grh::toString(kind)),
                                           "graph=" + graph_.symbol());
                        success = false;
                    }
                }
                return success;
            }

            std::vector<AttrKV> declarationInitAttrs(const Operation &op)
            {
                std::vector<AttrKV> result;
                for (const auto &attr : op.attrs())
                {
                    if (attr.key == "initValue" || attr.key == "initKind" ||
                        attr.key == "initFile" || attr.key == "initStart" ||
                        attr.key == "initLen")
                    {
                        result.push_back(convertAttr(module_, attr.key, attr.value));
                    }
                }
                return result;
            }

            bool lowerStateDeclarations()
            {
                bool success = true;
                for (OperationId opId : graph_.operations())
                {
                    const OperationKind kind = graph_.opKind(opId);
                    if (kind != OperationKind::kRegister && kind != OperationKind::kLatch &&
                        kind != OperationKind::kMemory)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(opId);
                    if (op.symbolText().empty())
                    {
                        diagnostics_.error("state declaration must have a symbol",
                                           "op=" + std::to_string(opId.index));
                        success = false;
                        continue;
                    }
                    const auto width = sourceAttr<int64_t>(op, "width");
                    const bool isSigned = sourceAttr<bool>(op, "isSigned").value_or(false);
                    if (!width || *width <= 0 || *width > std::numeric_limits<uint32_t>::max())
                    {
                        diagnostics_.error("state declaration has an invalid width",
                                           std::string(op.symbolText()));
                        success = false;
                        continue;
                    }
                    TypeId type = module_.internLogicType(static_cast<uint32_t>(*width), isSigned);
                    if (kind == OperationKind::kMemory)
                    {
                        const auto rows = sourceAttr<int64_t>(op, "row");
                        if (!rows || *rows <= 0 || *rows > std::numeric_limits<uint32_t>::max())
                        {
                            diagnostics_.error("memory declaration has an invalid row count",
                                               std::string(op.symbolText()));
                            success = false;
                            continue;
                        }
                        type = module_.internArrayType(static_cast<uint32_t>(*rows), type);
                    }
                    const auto init = declarationInitAttrs(op);
                    const StateId state = module_.addState(op.symbolText(), StateKind::State,
                                                          type, init);
                    if (!state.valid())
                    {
                        diagnostics_.error("duplicate or invalid StateDecl",
                                           std::string(op.symbolText()));
                        success = false;
                        continue;
                    }
                    stateByDeclaration_.emplace(std::string(op.symbolText()), state);
                }
                return success;
            }

            bool addInput(std::string_view portName, ValueId value,
                          std::string_view suffix)
            {
                const TypeId type = typeForValue(value);
                if (!type.valid())
                {
                    diagnostics_.error("input port has an invalid Type", std::string(portName));
                    return false;
                }
                const std::string stateName = uniqueStateName(portName, suffix);
                const StateId state = module_.addState(stateName, StateKind::Input, type);
                const OpId read = module_.createOp(genericOp(GenericOpcode::InRead));
                const EdgeId edge = module_.addResult(read, type, valueSymbol(value));
                if (!state.valid() || !read.valid() || !edge.valid() ||
                    !module_.setAttr(read, "port", module_.intern(stateName)))
                {
                    diagnostics_.error("failed to create input port mapping", std::string(portName));
                    return false;
                }
                if (valueMap_.contains(value))
                {
                    diagnostics_.error("one GRH Value is bound to multiple input ports",
                                       std::string(portName));
                    return false;
                }
                valueMap_.emplace(value, edge);
                eventStateByValue_.emplace(value, state);
                return true;
            }

            bool addOutput(std::string_view portName, ValueId value,
                           std::string_view suffix)
            {
                const TypeId type = typeForValue(value);
                if (!type.valid())
                {
                    diagnostics_.error("output port has an invalid Type", std::string(portName));
                    return false;
                }
                const std::string stateName = uniqueStateName(portName, suffix);
                const StateId state = module_.addState(stateName, StateKind::Output, type);
                if (!state.valid())
                {
                    diagnostics_.error("failed to create output StateDecl", std::string(portName));
                    return false;
                }
                outputs_.push_back(OutputPlan{state, value});
                return true;
            }

            bool lowerPorts()
            {
                bool success = true;
                for (const auto &port : graph_.inputPorts())
                {
                    success = addInput(port.name, port.value, "$in") && success;
                }
                for (const auto &port : graph_.outputPorts())
                {
                    success = addOutput(port.name, port.value, "$out") && success;
                }
                for (const auto &port : graph_.inoutPorts())
                {
                    success = addInput(port.name, port.in, "$in") && success;
                    success = addOutput(port.name, port.out, "$out") && success;
                    success = addOutput(port.name, port.oe, "$oe") && success;
                }
                return success;
            }

            TypeId dpiType(std::string_view typeName, int64_t width, bool isSigned)
            {
                std::string lowered;
                lowered.reserve(typeName.size());
                for (unsigned char ch : typeName)
                {
                    lowered.push_back(static_cast<char>(std::tolower(ch)));
                }
                if (lowered == "real" || lowered == "shortreal")
                {
                    return module_.internRealType();
                }
                if (lowered == "string")
                {
                    return module_.internStringType();
                }
                return width > 0 && width <= std::numeric_limits<uint32_t>::max()
                           ? module_.internLogicType(static_cast<uint32_t>(width), isSigned)
                           : TypeId::invalid();
            }

            bool lowerDpiImports()
            {
                bool success = true;
                for (OperationId opId : graph_.operations())
                {
                    if (graph_.opKind(opId) != OperationKind::kDpicImport)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(opId);
                    const std::string entry(op.symbolText());
                    const auto directions = sourceAttr<std::vector<std::string>>(op, "argsDirection");
                    const auto widths = sourceAttr<std::vector<int64_t>>(op, "argsWidth");
                    const auto names = sourceAttr<std::vector<std::string>>(op, "argsName");
                    const auto signs = sourceAttr<std::vector<bool>>(op, "argsSigned");
                    const auto types = sourceAttr<std::vector<std::string>>(op, "argsType");
                    if (entry.empty() || !directions || !widths || !names || !signs ||
                        directions->size() != widths->size() || directions->size() != names->size() ||
                        directions->size() != signs->size())
                    {
                        diagnostics_.error("DPI import has an incomplete signature", entry);
                        success = false;
                        continue;
                    }
                    std::vector<HostParam> signature;
                    if (sourceAttr<bool>(op, "hasReturn").value_or(false))
                    {
                        const int64_t width = sourceAttr<int64_t>(op, "returnWidth").value_or(1);
                        const bool sign = sourceAttr<bool>(op, "returnSigned").value_or(false);
                        const std::string typeName = sourceAttr<std::string>(op, "returnType")
                                                         .value_or("logic");
                        signature.push_back(HostParam{
                            .name = module_.intern("return"),
                            .type = dpiType(typeName, width, sign),
                            .direction = HostParamDirection::Return,
                        });
                    }
                    for (std::size_t index = 0; index < directions->size(); ++index)
                    {
                        HostParamDirection direction = HostParamDirection::Input;
                        if ((*directions)[index] == "output")
                        {
                            direction = HostParamDirection::Output;
                        }
                        else if ((*directions)[index] == "inout")
                        {
                            direction = HostParamDirection::InOut;
                        }
                        else if ((*directions)[index] != "input")
                        {
                            diagnostics_.error("DPI import has an invalid parameter direction", entry);
                            success = false;
                            continue;
                        }
                        const std::string_view typeName = types && index < types->size()
                                                              ? std::string_view((*types)[index])
                                                              : std::string_view("logic");
                        signature.push_back(HostParam{
                            .name = module_.intern((*names)[index]),
                            .type = dpiType(typeName, (*widths)[index], (*signs)[index]),
                            .direction = direction,
                        });
                    }
                    if (std::any_of(signature.begin(), signature.end(),
                                    [](const HostParam &param) { return !param.type.valid(); }))
                    {
                        diagnostics_.error("DPI import has an invalid parameter Type", entry);
                        success = false;
                        continue;
                    }
                    const auto attrs = convertAttrs(module_, op.attrs());
                    const HostId host = module_.addHost(entry, HostKind::Effect, signature,
                                                        entry, attrs);
                    if (!host.valid())
                    {
                        diagnostics_.error("duplicate or invalid DPI HostTable entry", entry);
                        success = false;
                        continue;
                    }
                    dpiHosts_.emplace(entry, host);
                }
                return success;
            }

            bool createOpSkeletons()
            {
                bool success = true;
                for (OperationId sourceOp : graph_.operations())
                {
                    const OperationKind sourceKind = graph_.opKind(sourceOp);
                    if (isDeclaration(sourceKind))
                    {
                        continue;
                    }
                    const auto opcode = mappedOpcode(sourceKind);
                    if (!opcode)
                    {
                        continue;
                    }
                    const OpId targetOp = module_.createOp(genericOp(*opcode),
                                                           operationSymbol(sourceOp));
                    if (!targetOp.valid())
                    {
                        diagnostics_.error("failed to create GRHSIM operation",
                                           "source_op=" + std::to_string(sourceOp.index));
                        success = false;
                        continue;
                    }
                    opMap_.emplace(sourceOp, targetOp);
                    for (ValueId sourceResult : graph_.opResults(sourceOp))
                    {
                        if (valueMap_.contains(sourceResult))
                        {
                            diagnostics_.error("GRH result Value is already mapped",
                                               "value=" + std::to_string(sourceResult.index));
                            success = false;
                            continue;
                        }
                        const TypeId type = typeForValue(sourceResult);
                        TypeId nativeType = type;
                        const Operation source = graph_.getOperation(sourceOp);
                        if ((sourceKind == OperationKind::kMul ||
                             sourceKind == OperationKind::kAnd ||
                             sourceKind == OperationKind::kOr ||
                             sourceKind == OperationKind::kXor ||
                             sourceKind == OperationKind::kXnor) &&
                            source.operands().size() == 2 && source.results().size() == 1 &&
                            graph_.valueType(source.operands()[0]) == ValueType::Logic &&
                            graph_.valueType(source.operands()[1]) == ValueType::Logic)
                        {
                            const int32_t lhsWidth = graph_.valueWidth(source.operands()[0]);
                            const int32_t rhsWidth = graph_.valueWidth(source.operands()[1]);
                            const uint64_t width =
                                lhsWidth > 0 && rhsWidth > 0
                                    ? sourceKind == OperationKind::kMul
                                          ? static_cast<uint64_t>(lhsWidth) +
                                                static_cast<uint64_t>(rhsWidth)
                                          : static_cast<uint64_t>(std::max(lhsWidth, rhsWidth))
                                    : 0;
                            if (width > 0 && width <= std::numeric_limits<uint32_t>::max())
                            {
                                nativeType = module_.internLogicType(
                                    static_cast<uint32_t>(width),
                                    graph_.valueSigned(source.operands()[0]) &&
                                        graph_.valueSigned(source.operands()[1]));
                            }
                        }

                        const bool needsAdapter = nativeType != type;
                        const EdgeId targetResult = module_.addResult(
                            targetOp, nativeType,
                            needsAdapter ? SymbolId::invalid() : valueSymbol(sourceResult));
                        if (!targetResult.valid())
                        {
                            diagnostics_.error("failed to map GRH result Type",
                                               "value=" + std::to_string(sourceResult.index));
                            success = false;
                            continue;
                        }
                        EdgeId mappedResult = targetResult;
                        if (needsAdapter)
                        {
                            const OpId assign = module_.createOp(genericOp(GenericOpcode::Assign));
                            const std::array<EdgeId, 1> operands{targetResult};
                            mappedResult = module_.addResult(assign, type, valueSymbol(sourceResult));
                            if (!assign.valid() || !mappedResult.valid() ||
                                !module_.setOperands(assign, operands))
                            {
                                diagnostics_.error("failed to adapt native GRHSIM result Type",
                                                   "value=" +
                                                       std::to_string(sourceResult.index));
                                success = false;
                                continue;
                            }
                        }
                        valueMap_.emplace(sourceResult, mappedResult);
                    }
                }

                for (OperationId sourceOp : graph_.operations())
                {
                    const OperationKind kind = graph_.opKind(sourceOp);
                    if (kind != OperationKind::kRegisterReadPort &&
                        kind != OperationKind::kLatchReadPort)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(sourceOp);
                    const char *key = kind == OperationKind::kRegisterReadPort
                                          ? "regSymbol"
                                          : "latchSymbol";
                    const auto target = sourceAttr<std::string>(op, key);
                    if (!target || op.results().empty())
                    {
                        continue;
                    }
                    if (const auto found = stateByDeclaration_.find(*target);
                        found != stateByDeclaration_.end())
                    {
                        eventStateByValue_.emplace(op.results()[0], found->second);
                    }
                }
                return success;
            }

            bool materializeSourceLessValue(ValueId value)
            {
                if (valueMap_.contains(value) || graph_.valueDef(value).valid())
                {
                    return true;
                }

                const TypeId type = typeForValue(value);
                if (!type.valid())
                {
                    diagnostics_.error("source-less GRH Value has an invalid Type",
                                       "value=" + std::to_string(value.index));
                    return false;
                }

                std::string literal;
                switch (graph_.valueType(value))
                {
                case ValueType::Logic:
                    literal = std::to_string(graph_.valueWidth(value)) + "'b0";
                    break;
                case ValueType::Real:
                    literal = "0.0";
                    break;
                case ValueType::String:
                    literal = "\"\"";
                    break;
                }

                const OpId constant = module_.createOp(genericOp(GenericOpcode::Const));
                const EdgeId edge = module_.addResult(constant, type, valueSymbol(value));
                if (!constant.valid() || !edge.valid() ||
                    !module_.setAttr(constant, "value", module_.intern(literal)))
                {
                    diagnostics_.error("failed to materialize source-less GRH Value",
                                       "value=" + std::to_string(value.index));
                    return false;
                }
                valueMap_.emplace(value, edge);
                return true;
            }

            bool materializeSourceLessValues()
            {
                bool success = true;
                for (OperationId sourceOp : graph_.operations())
                {
                    if (!opMap_.contains(sourceOp))
                    {
                        continue;
                    }
                    for (ValueId operand : graph_.opOperands(sourceOp))
                    {
                        success = materializeSourceLessValue(operand) && success;
                    }
                }
                for (const OutputPlan &output : outputs_)
                {
                    success = materializeSourceLessValue(output.value) && success;
                }
                return success;
            }

            EdgeId mappedValue(ValueId value, const Operation &op)
            {
                const auto found = valueMap_.find(value);
                if (found == valueMap_.end())
                {
                    diagnostics_.error("operation operand has no lowered edge",
                                       "op=" + std::to_string(op.id().index) +
                                           " value=" + std::to_string(value.index));
                    return EdgeId::invalid();
                }
                return found->second;
            }

            EdgeId adaptLogicValue(EdgeId actual, TypeId target,
                                   const Operation &source,
                                   std::string_view incompatibleMessage,
                                   std::string_view failureMessage)
            {
                if (module_.edgeType(actual) == target)
                {
                    return actual;
                }
                const TypeRec *actualType = module_.type(module_.edgeType(actual));
                const TypeRec *targetType = module_.type(target);
                const auto isLogicType = [](const TypeRec *type) {
                    return type && type->track == TypeTrack::Generic &&
                           type->kind == static_cast<uint8_t>(GenericTypeKind::Logic);
                };
                if (!isLogicType(actualType) || !isLogicType(targetType))
                {
                    diagnostics_.error(std::string(incompatibleMessage),
                                       "op=" + std::to_string(source.id().index));
                    return EdgeId::invalid();
                }

                const OpId assign = module_.createOp(genericOp(GenericOpcode::Assign));
                const std::array<EdgeId, 1> operands{actual};
                const EdgeId adapted = module_.addResult(assign, target);
                if (!assign.valid() || !adapted.valid() ||
                    !module_.setOperands(assign, operands))
                {
                    diagnostics_.error(std::string(failureMessage),
                                       "op=" + std::to_string(source.id().index));
                    return EdgeId::invalid();
                }
                return adapted;
            }

            EdgeId adaptDpiArgument(EdgeId actual, TypeId formal,
                                    const Operation &source)
            {
                return adaptLogicValue(
                    actual, formal, source,
                    "DPI argument Type is incompatible with its formal Type",
                    "failed to create a DPI argument Type adapter");
            }

            EdgeId adaptCondition(EdgeId actual, const Operation &source)
            {
                const TypeId actualTypeId = module_.edgeType(actual);
                const TypeRec *actualType = module_.type(actualTypeId);
                if (!actualType || actualType->track != TypeTrack::Generic ||
                    actualType->kind != static_cast<uint8_t>(GenericTypeKind::Logic))
                {
                    diagnostics_.error("condition operand must be logic",
                                       "op=" + std::to_string(source.id().index));
                    return EdgeId::invalid();
                }
                if (actualType->width == 1)
                {
                    return actual;
                }

                EdgeId zero = EdgeId::invalid();
                if (const auto found = logicZeroByType_.find(actualTypeId.raw);
                    found != logicZeroByType_.end())
                {
                    zero = found->second;
                }
                else
                {
                    const OpId constant = module_.createOp(genericOp(GenericOpcode::Const));
                    zero = module_.addResult(constant, actualTypeId);
                    const std::string literal = std::to_string(actualType->width) + "'b0";
                    if (!constant.valid() || !zero.valid() ||
                        !module_.setAttr(constant, "value", module_.intern(literal)))
                    {
                        diagnostics_.error("failed to create condition zero constant",
                                           "op=" + std::to_string(source.id().index));
                        return EdgeId::invalid();
                    }
                    logicZeroByType_.emplace(actualTypeId.raw, zero);
                }

                const OpId compare = module_.createOp(genericOp(GenericOpcode::Ne));
                const std::array<EdgeId, 2> operands{actual, zero};
                const EdgeId condition =
                    module_.addResult(compare, module_.internLogicType(1, false));
                if (!compare.valid() || !condition.valid() ||
                    !module_.setOperands(compare, operands))
                {
                    diagnostics_.error("failed to reduce condition operand to one bit",
                                       "op=" + std::to_string(source.id().index));
                    return EdgeId::invalid();
                }
                return condition;
            }

            StateId targetState(const Operation &op, std::string_view key)
            {
                const auto name = sourceAttr<std::string>(op, key);
                if (!name)
                {
                    diagnostics_.error("state operation is missing target attribute",
                                       "op=" + std::to_string(op.id().index));
                    return StateId::invalid();
                }
                const auto found = stateByDeclaration_.find(*name);
                if (found == stateByDeclaration_.end())
                {
                    diagnostics_.error("state operation references an unknown declaration", *name);
                    return StateId::invalid();
                }
                return found->second;
            }

            StateId eventState(ValueId value)
            {
                if (const auto found = eventStateByValue_.find(value);
                    found != eventStateByValue_.end())
                {
                    return found->second;
                }
                const TypeId type = typeForValue(value);
                const TypeRec *record = module_.type(type);
                if (!record || record->kind != static_cast<uint8_t>(GenericTypeKind::Logic) ||
                    record->width != 1)
                {
                    diagnostics_.error("event operand must be a one-bit logic Value",
                                       "value=" + std::to_string(value.index));
                    return StateId::invalid();
                }
                const EdgeId edge = valueMap_.contains(value) ? valueMap_.at(value)
                                                              : EdgeId::invalid();
                if (!edge.valid())
                {
                    diagnostics_.error("event operand has no lowered edge",
                                       "value=" + std::to_string(value.index));
                    return StateId::invalid();
                }
                std::string base = "event";
                const auto symbol = graph_.valueSymbol(value);
                if (symbol.valid() && !graph_.symbolText(symbol).empty())
                {
                    base = std::string(graph_.symbolText(symbol));
                }
                const std::string stateName = uniqueStateName(base, "$event", true);
                const StateId state = module_.addState(stateName, StateKind::Output, type);
                const OpId write = module_.createOp(genericOp(GenericOpcode::OutWrite));
                const std::array<EdgeId, 1> operands{edge};
                if (!state.valid() || !write.valid() || !module_.setOperands(write, operands) ||
                    !module_.setAttr(write, "port", module_.intern(stateName)) ||
                    !module_.setAttr(write, "eventState", true))
                {
                    diagnostics_.error("failed to promote internal event Value",
                                       "value=" + std::to_string(value.index));
                    return StateId::invalid();
                }
                eventStateByValue_.emplace(value, state);
                return state;
            }

            bool setEventAttrs(const Operation &source, OpId target,
                               std::size_t eventStart,
                               const std::vector<std::string> &eventEdges,
                               bool requireEvent)
            {
                if (source.operands().size() != eventStart + eventEdges.size() ||
                    (requireEvent && eventEdges.empty()))
                {
                    diagnostics_.error("operation event operands do not match eventEdge",
                                       "op=" + std::to_string(source.id().index));
                    return false;
                }
                std::vector<SymbolId> events;
                std::vector<SymbolId> edges;
                events.reserve(eventEdges.size());
                edges.reserve(eventEdges.size());
                for (std::size_t index = 0; index < eventEdges.size(); ++index)
                {
                    if (eventEdges[index] != "posedge" && eventEdges[index] != "negedge")
                    {
                        diagnostics_.error("eventEdge must be posedge or negedge",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    const StateId state = eventState(source.operands()[eventStart + index]);
                    const StateEntry *entry = module_.state(state);
                    if (!entry)
                    {
                        return false;
                    }
                    events.push_back(entry->name);
                    edges.push_back(module_.intern(eventEdges[index]));
                }
                return module_.setAttr(target, "events", std::move(events)) &&
                       module_.setAttr(target, "eventEdge", std::move(edges));
            }

            bool setStateAttr(OpId op, StateId state)
            {
                const StateEntry *entry = module_.state(state);
                return entry && module_.setAttr(op, "state", entry->name);
            }

            bool copyMemoryPriority(const Operation &source, OpId target)
            {
                for (std::string_view key : {
                         wolvrix::lib::grh::kMemoryWritePriorityGroupAttr,
                         wolvrix::lib::grh::kMemoryWritePriorityAttr})
                {
                    if (const auto value = source.attr(key))
                    {
                        if (!module_.setAttr(target, key, convertAttrValue(module_, *value)))
                        {
                            return false;
                        }
                    }
                }
                return true;
            }

            std::pair<HostId, std::string> ensureHost(
                std::string baseName, HostKind kind, const std::vector<HostParam> &signature,
                const std::vector<AttrKV> &attrs)
            {
                if (baseName.empty())
                {
                    return {HostId::invalid(), {}};
                }
                for (uint32_t ordinal = 0;; ++ordinal)
                {
                    const std::string entry = ordinal == 0 ? baseName
                                                            : baseName + "#" + std::to_string(ordinal);
                    const HostId existing = module_.findHost(entry);
                    if (existing.valid())
                    {
                        const HostEntry *host = module_.host(existing);
                        const auto existingSignature = module_.hostSignature(existing);
                        const auto existingAttrs = module_.hostAttrs(existing);
                        const bool sameSignature = existingSignature.size() == signature.size() &&
                            std::equal(existingSignature.begin(), existingSignature.end(),
                                       signature.begin());
                        const bool sameAttrs = existingAttrs.size() == attrs.size() &&
                            std::all_of(existingAttrs.begin(), existingAttrs.end(),
                                        [&](const AttrKV &existingAttr) {
                                            return std::find(attrs.begin(), attrs.end(),
                                                             existingAttr) != attrs.end();
                                        });
                        if (host && host->kind == kind && sameSignature && sameAttrs)
                        {
                            return {existing, entry};
                        }
                        continue;
                    }
                    const HostId created = module_.addHost(entry, kind, signature, baseName, attrs);
                    return {created, entry};
                }
            }

            std::vector<HostParam> callSignature(const Operation &source,
                                                 std::size_t argumentStart,
                                                 std::size_t argumentEnd)
            {
                std::vector<HostParam> signature;
                uint32_t ordinal = 0;
                for (std::size_t index = argumentStart; index < argumentEnd; ++index)
                {
                    signature.push_back(HostParam{
                        .name = module_.intern("arg" + std::to_string(ordinal++)),
                        .type = typeForValue(source.operands()[index]),
                        .direction = HostParamDirection::Input,
                    });
                }
                for (ValueId result : source.results())
                {
                    signature.push_back(HostParam{
                        .name = module_.intern("result" + std::to_string(ordinal++)),
                        .type = typeForValue(result),
                        .direction = HostParamDirection::Return,
                    });
                }
                return signature;
            }

            bool wireHostCall(const Operation &source, OpId target)
            {
                std::vector<EdgeId> operands;
                std::vector<std::string> eventEdges =
                    sourceAttr<std::vector<std::string>>(source, "eventEdge").value_or(
                        std::vector<std::string>{});
                const std::size_t eventCount = eventEdges.size();
                if (source.operands().size() < eventCount)
                {
                    diagnostics_.error("host call event list exceeds operand list",
                                       "op=" + std::to_string(source.id().index));
                    return false;
                }
                const std::size_t eventStart = source.operands().size() - eventCount;
                HostId host = HostId::invalid();
                std::string entry;
                if (source.kind() == OperationKind::kDpicCall)
                {
                    const auto targetImport = sourceAttr<std::string>(source, "targetImportSymbol");
                    const auto found = targetImport ? dpiHosts_.find(*targetImport) : dpiHosts_.end();
                    if (!targetImport || found == dpiHosts_.end())
                    {
                        diagnostics_.error("DPI call references an unknown import",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    host = found->second;
                    entry = std::string(module_.symbol(module_.host(host)->entry));
                }
                else
                {
                    const auto rawName = sourceAttr<std::string>(source, "name");
                    if (!rawName)
                    {
                        diagnostics_.error("system call is missing name",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    const HostKind kind = source.kind() == OperationKind::kSystemFunction
                                              ? HostKind::Query
                                              : HostKind::Effect;
                    const std::size_t argumentStart = kind == HostKind::Effect ? 1U : 0U;
                    const auto signature = callSignature(source, argumentStart, eventStart);
                    const auto hostAttrs = convertAttrs(module_, source.attrs());
                    std::tie(host, entry) = ensureHost(normalizeHostName(*rawName), kind,
                                                       signature, hostAttrs);
                }
                if (!host.valid())
                {
                    diagnostics_.error("failed to create HostTable entry",
                                       "op=" + std::to_string(source.id().index));
                    return false;
                }
                if (source.kind() == OperationKind::kDpicCall)
                {
                    const HostEntry *hostEntry = module_.host(host);
                    if (!hostEntry)
                    {
                        diagnostics_.error("DPI call references an invalid HostTable entry",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    const std::size_t conditionCount = hostEntry->kind == HostKind::Effect ? 1U : 0U;
                    if (eventStart < conditionCount)
                    {
                        diagnostics_.error("DPI call is missing its condition operand",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    for (std::size_t index = 0; index < conditionCount; ++index)
                    {
                        const EdgeId edge = adaptCondition(
                            mappedValue(source.operands()[index], source), source);
                        if (!edge.valid())
                        {
                            return false;
                        }
                        operands.push_back(edge);
                    }

                    std::size_t actualIndex = conditionCount;
                    for (const HostParam &parameter : module_.hostSignature(host))
                    {
                        if (parameter.direction != HostParamDirection::Input &&
                            parameter.direction != HostParamDirection::InOut)
                        {
                            continue;
                        }
                        if (actualIndex >= eventStart)
                        {
                            diagnostics_.error("DPI call has fewer actual inputs than its signature",
                                               "op=" + std::to_string(source.id().index));
                            return false;
                        }
                        const EdgeId actual =
                            mappedValue(source.operands()[actualIndex++], source);
                        if (!actual.valid())
                        {
                            return false;
                        }
                        const EdgeId adapted = adaptDpiArgument(actual, parameter.type, source);
                        if (!adapted.valid())
                        {
                            return false;
                        }
                        operands.push_back(adapted);
                    }
                    if (actualIndex != eventStart)
                    {
                        diagnostics_.error("DPI call has more actual inputs than its signature",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                }
                else
                {
                    for (std::size_t index = 0; index < eventStart; ++index)
                    {
                        EdgeId edge = mappedValue(source.operands()[index], source);
                        if (index == 0 && source.kind() != OperationKind::kSystemFunction)
                        {
                            edge = adaptCondition(edge, source);
                        }
                        if (!edge.valid())
                        {
                            return false;
                        }
                        operands.push_back(edge);
                    }
                }
                if (!module_.setOperands(target, operands) ||
                    !module_.setAttr(target, "entry", module_.intern(entry)))
                {
                    return false;
                }
                if (source.kind() != OperationKind::kSystemFunction &&
                    !setEventAttrs(source, target, eventStart, eventEdges, false))
                {
                    return false;
                }
                return true;
            }

            bool wireOperation(const Operation &source, OpId target)
            {
                const OperationKind kind = source.kind();
                if (kind == OperationKind::kSystemFunction ||
                    kind == OperationKind::kSystemTask || kind == OperationKind::kDpicCall)
                {
                    return wireHostCall(source, target);
                }

                std::vector<EdgeId> operands;
                std::size_t sourceOperandCount = source.operands().size();
                std::vector<std::string> eventEdges;
                bool requireEvent = false;
                StateId state = StateId::invalid();
                switch (kind)
                {
                case OperationKind::kRegisterReadPort:
                    state = targetState(source, "regSymbol");
                    sourceOperandCount = 0;
                    break;
                case OperationKind::kRegisterWritePort:
                    state = targetState(source, "regSymbol");
                    sourceOperandCount = 3;
                    eventEdges = sourceAttr<std::vector<std::string>>(source, "eventEdge")
                                     .value_or(std::vector<std::string>{});
                    requireEvent = true;
                    break;
                case OperationKind::kLatchReadPort:
                    state = targetState(source, "latchSymbol");
                    sourceOperandCount = 0;
                    break;
                case OperationKind::kLatchWritePort:
                    state = targetState(source, "latchSymbol");
                    sourceOperandCount = 3;
                    break;
                case OperationKind::kMemoryReadPort:
                    state = targetState(source, "memSymbol");
                    sourceOperandCount = 1;
                    break;
                case OperationKind::kMemoryReadAllPort:
                    state = targetState(source, "memSymbol");
                    sourceOperandCount = 0;
                    break;
                case OperationKind::kMemoryWritePort:
                    state = targetState(source, "memSymbol");
                    sourceOperandCount = 4;
                    eventEdges = sourceAttr<std::vector<std::string>>(source, "eventEdge")
                                     .value_or(std::vector<std::string>{});
                    requireEvent = true;
                    break;
                case OperationKind::kMemoryWriteLanesPort:
                    state = targetState(source, "memSymbol");
                    sourceOperandCount = 2;
                    eventEdges = sourceAttr<std::vector<std::string>>(source, "eventEdge")
                                     .value_or(std::vector<std::string>{});
                    requireEvent = true;
                    break;
                case OperationKind::kMemoryFillPort:
                    state = targetState(source, "memSymbol");
                    sourceOperandCount = 2;
                    eventEdges = sourceAttr<std::vector<std::string>>(source, "eventEdge")
                                     .value_or(std::vector<std::string>{});
                    requireEvent = true;
                    break;
                default:
                    break;
                }
                if (source.operands().size() < sourceOperandCount)
                {
                    diagnostics_.error("GRH operation has too few operands",
                                       "op=" + std::to_string(source.id().index));
                    return false;
                }
                for (std::size_t index = 0; index < sourceOperandCount; ++index)
                {
                    const EdgeId edge = mappedValue(source.operands()[index], source);
                    if (!edge.valid())
                    {
                        return false;
                    }
                    operands.push_back(edge);
                }
                if (kind == OperationKind::kMux && operands.size() == 3)
                {
                    const auto results = module_.results(target);
                    if (results.size() != 1)
                    {
                        diagnostics_.error("GRH mux has no lowered result",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    const TypeId resultType = module_.edgeType(results[0]);
                    for (std::size_t index = 1; index < operands.size(); ++index)
                    {
                        operands[index] = adaptLogicValue(
                            operands[index], resultType, source,
                            "mux data operand Type is incompatible with its result Type",
                            "failed to create a mux data Type adapter");
                        if (!operands[index].valid())
                        {
                            return false;
                        }
                    }
                }
                if (kind == OperationKind::kArrayMux && operands.size() == 3)
                {
                    const auto results = module_.results(target);
                    if (results.size() != 1)
                    {
                        diagnostics_.error("GRH array mux has no lowered result",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    const TypeId resultType = module_.edgeType(results[0]);
                    for (std::size_t index = 1; index < operands.size(); ++index)
                    {
                        operands[index] = adaptLogicValue(
                            operands[index], resultType, source,
                            "array mux data operand Type is incompatible with its result Type",
                            "failed to create an array mux data Type adapter");
                        if (!operands[index].valid())
                        {
                            return false;
                        }
                    }
                }
                if (state.valid() &&
                    (kind == OperationKind::kRegisterWritePort ||
                     kind == OperationKind::kLatchWritePort))
                {
                    const StateEntry *entry = module_.state(state);
                    if (!entry || operands.size() < 3)
                    {
                        return false;
                    }
                    operands[0] = adaptCondition(operands[0], source);
                    operands[1] = adaptLogicValue(
                        operands[1], entry->genType, source,
                        "state write data Type is incompatible with its StateDecl",
                        "failed to create a state write data Type adapter");
                    operands[2] = adaptLogicValue(
                        operands[2], entry->genType, source,
                        "state write mask Type is incompatible with its StateDecl",
                        "failed to create a state write mask Type adapter");
                    if (std::any_of(operands.begin(), operands.end(),
                                    [](EdgeId edge) { return !edge.valid(); }))
                    {
                        return false;
                    }
                }
                if (state.valid() && kind == OperationKind::kMemoryWritePort)
                {
                    const StateEntry *entry = module_.state(state);
                    const TypeRec *array = entry ? module_.type(entry->genType) : nullptr;
                    if (!array || operands.size() < 4)
                    {
                        return false;
                    }
                    operands[0] = adaptCondition(operands[0], source);
                    operands[2] = adaptLogicValue(
                        operands[2], array->elementType, source,
                        "memory write data Type is incompatible with its StateDecl",
                        "failed to create a memory write data Type adapter");
                    operands[3] = adaptLogicValue(
                        operands[3], array->elementType, source,
                        "memory write mask Type is incompatible with its StateDecl",
                        "failed to create a memory write mask Type adapter");
                    if (std::any_of(operands.begin(), operands.end(),
                                    [](EdgeId edge) { return !edge.valid(); }))
                    {
                        return false;
                    }
                }
                if (state.valid() && kind == OperationKind::kMemoryFillPort)
                {
                    const StateEntry *entry = module_.state(state);
                    const TypeRec *array = entry ? module_.type(entry->genType) : nullptr;
                    const TypeRec *element = array ? module_.type(array->elementType) : nullptr;
                    const TypeRec *data = operands.size() >= 2
                                              ? module_.type(module_.edgeType(operands[1]))
                                              : nullptr;
                    const uint64_t packedWidth = array && element
                                                     ? static_cast<uint64_t>(array->rows) *
                                                           element->width
                                                     : 0;
                    if (!array || !element || !data || operands.size() < 2 ||
                        packedWidth == 0 || packedWidth > std::numeric_limits<uint32_t>::max())
                    {
                        return false;
                    }
                    operands[0] = adaptCondition(operands[0], source);
                    if (data->width == element->width)
                    {
                        const OpId broadcast =
                            module_.createOp(genericOp(GenericOpcode::ArrayBroadcast));
                        const std::array<EdgeId, 1> inputs{operands[1]};
                        const EdgeId packed = module_.addResult(
                            broadcast,
                            module_.internLogicType(static_cast<uint32_t>(packedWidth), false));
                        if (!broadcast.valid() || !packed.valid() ||
                            !module_.setOperands(broadcast, inputs) ||
                            !module_.setAttr(broadcast, "rows",
                                             static_cast<int64_t>(array->rows)))
                        {
                            diagnostics_.error("failed to broadcast memory fill data",
                                               "op=" + std::to_string(source.id().index));
                            return false;
                        }
                        operands[1] = packed;
                    }
                    else if (data->width != packedWidth)
                    {
                        diagnostics_.error(
                            "memory fill data is neither one row nor a packed whole memory",
                            "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    if (!operands[0].valid())
                    {
                        return false;
                    }
                }
                if (!module_.setOperands(target, operands))
                {
                    return false;
                }
                if (state.valid() && !setStateAttr(target, state))
                {
                    return false;
                }
                if (!eventEdges.empty() || requireEvent)
                {
                    if (!setEventAttrs(source, target, sourceOperandCount,
                                       eventEdges, requireEvent))
                    {
                        return false;
                    }
                }

                if (kind == OperationKind::kMemoryWritePort ||
                    kind == OperationKind::kMemoryWriteLanesPort ||
                    kind == OperationKind::kMemoryFillPort)
                {
                    if (!copyMemoryPriority(source, target))
                    {
                        return false;
                    }
                    const auto priorityGroup = sourceAttr<std::string>(
                        source, wolvrix::lib::grh::kMemoryWritePriorityGroupAttr);
                    const auto priority = sourceAttr<int64_t>(
                        source, wolvrix::lib::grh::kMemoryWritePriorityAttr);
                    if (priorityGroup.has_value() != priority.has_value())
                    {
                        diagnostics_.error("memory write priority attributes must appear together",
                                           "op=" + std::to_string(source.id().index));
                        return false;
                    }
                    memoryWriters_.push_back(MemoryWriterPlan{
                        .target = target,
                        .state = state,
                        .priorityGroup = priorityGroup,
                        .priority = priority,
                    });
                }
                if (kind == OperationKind::kConstant)
                {
                    const auto value = sourceAttr<std::string>(source, "constValue");
                    return value && module_.setAttr(target, "value", module_.intern(*value));
                }
                if (kind == OperationKind::kSliceStatic)
                {
                    const auto lsb = sourceAttr<int64_t>(source, "sliceStart");
                    return lsb && module_.setAttr(target, "lsb", *lsb);
                }
                if (kind == OperationKind::kReplicate)
                {
                    std::optional<int64_t> count = sourceAttr<int64_t>(source, "rep");
                    if (!count && !source.operands().empty() && !source.results().empty())
                    {
                        const int32_t inputWidth = graph_.valueWidth(source.operands()[0]);
                        const int32_t resultWidth = graph_.valueWidth(source.results()[0]);
                        if (inputWidth > 0 && resultWidth > 0 && resultWidth % inputWidth == 0)
                        {
                            count = resultWidth / inputWidth;
                        }
                    }
                    return count && module_.setAttr(target, "count", *count);
                }
                if (kind == OperationKind::kArrayLaneConst)
                {
                    const auto elemWidth = sourceAttr<int64_t>(source, "elemWidth");
                    const auto rows = sourceAttr<int64_t>(source, "rows");
                    const auto values = sourceAttr<std::vector<int64_t>>(source, "values");
                    return elemWidth && rows && values &&
                           module_.setAttr(target, "elem_width", *elemWidth) &&
                           module_.setAttr(target, "rows", *rows) &&
                           module_.setAttr(target, "values", *values);
                }
                if (kind == OperationKind::kArrayReduceOr ||
                    kind == OperationKind::kArrayReduceAnd ||
                    kind == OperationKind::kArrayReduceXor ||
                    kind == OperationKind::kArrayReduceLanesOr ||
                    kind == OperationKind::kArrayReduceLanesAnd ||
                    kind == OperationKind::kArrayReduceLanesXor)
                {
                    const auto elemWidth = sourceAttr<int64_t>(source, "elemWidth");
                    return elemWidth && module_.setAttr(target, "elem_width", *elemWidth);
                }
                if (kind == OperationKind::kArrayBroadcast ||
                    kind == OperationKind::kArrayOnehot)
                {
                    const auto rows = sourceAttr<int64_t>(source, "rows");
                    return rows && module_.setAttr(target, "rows", *rows);
                }
                return true;
            }

            bool wireOperations()
            {
                bool success = true;
                for (OperationId sourceId : graph_.operations())
                {
                    if (isDeclaration(graph_.opKind(sourceId)))
                    {
                        continue;
                    }
                    const auto found = opMap_.find(sourceId);
                    if (found == opMap_.end())
                    {
                        continue;
                    }
                    success = wireOperation(graph_.getOperation(sourceId), found->second) && success;
                }
                return success;
            }

            bool canonicalizeMemoryWritePriorities()
            {
                std::map<uint32_t, std::vector<const MemoryWriterPlan *>> writersByState;
                for (const MemoryWriterPlan &writer : memoryWriters_)
                {
                    writersByState[writer.state.raw].push_back(&writer);
                }

                for (const auto &[stateRaw, writers] : writersByState)
                {
                    if (writers.size() < 2)
                    {
                        continue;
                    }

                    std::map<std::string, std::vector<const MemoryWriterPlan *>, std::less<>>
                        explicitGroups;
                    for (const MemoryWriterPlan *writer : writers)
                    {
                        if (writer->priorityGroup)
                        {
                            explicitGroups[*writer->priorityGroup].push_back(writer);
                        }
                    }
                    for (auto &[name, members] : explicitGroups)
                    {
                        (void)name;
                        std::stable_sort(
                            members.begin(), members.end(),
                            [](const MemoryWriterPlan *lhs, const MemoryWriterPlan *rhs) {
                                return *lhs->priority > *rhs->priority;
                            });
                    }

                    std::vector<const MemoryWriterPlan *> order;
                    order.reserve(writers.size());
                    std::set<std::string, std::less<>> emittedGroups;
                    for (const MemoryWriterPlan *writer : writers)
                    {
                        if (!writer->priorityGroup)
                        {
                            order.push_back(writer);
                            continue;
                        }
                        if (!emittedGroups.insert(*writer->priorityGroup).second)
                        {
                            continue;
                        }
                        const auto &members = explicitGroups.at(*writer->priorityGroup);
                        order.insert(order.end(), members.begin(), members.end());
                    }

                    const StateEntry *state = module_.state(StateId{stateRaw});
                    if (!state || order.size() != writers.size())
                    {
                        diagnostics_.error("failed to canonicalize memory write order",
                                           "state=" + std::to_string(stateRaw));
                        return false;
                    }
                    const std::string group =
                        "$lower_grhsim$" + std::string(module_.symbol(state->name));
                    for (std::size_t index = 0; index < order.size(); ++index)
                    {
                        const int64_t priority =
                            static_cast<int64_t>(order.size() - 1U - index);
                        if (!module_.setAttr(order[index]->target,
                                             wolvrix::lib::grh::kMemoryWritePriorityGroupAttr,
                                             module_.intern(group)) ||
                            !module_.setAttr(order[index]->target,
                                             wolvrix::lib::grh::kMemoryWritePriorityAttr,
                                             priority))
                        {
                            diagnostics_.error("failed to write canonical memory priority",
                                               "state=" + std::to_string(stateRaw));
                            return false;
                        }
                    }
                }
                return true;
            }

            bool writeOutputPorts()
            {
                bool success = true;
                for (const OutputPlan &output : outputs_)
                {
                    const auto found = valueMap_.find(output.value);
                    const StateEntry *state = module_.state(output.state);
                    if (found == valueMap_.end() || !state)
                    {
                        diagnostics_.error("output port Value has no lowered edge",
                                           "value=" + std::to_string(output.value.index));
                        success = false;
                        continue;
                    }
                    const OpId write = module_.createOp(genericOp(GenericOpcode::OutWrite));
                    const std::array<EdgeId, 1> operands{found->second};
                    if (!write.valid() || !module_.setOperands(write, operands) ||
                        !module_.setAttr(write, "port", state->name))
                    {
                        diagnostics_.error("failed to create output port mapping",
                                           std::string(module_.symbol(state->name)));
                        success = false;
                    }
                }
                return success;
            }

            const Graph &graph_;
            wolvrix::lib::diag::Diagnostics &diagnostics_;
            Module module_;
            std::unordered_map<ValueId, EdgeId, ValueIdHash> valueMap_;
            std::unordered_map<OperationId, OpId, OperationIdHash> opMap_;
            std::unordered_map<std::string, StateId> stateByDeclaration_;
            std::unordered_map<ValueId, StateId, ValueIdHash> eventStateByValue_;
            std::unordered_map<std::string, HostId> dpiHosts_;
            std::unordered_map<uint32_t, EdgeId> logicZeroByType_;
            std::vector<OutputPlan> outputs_;
            std::vector<MemoryWriterPlan> memoryWriters_;
        };
    } // namespace

    std::optional<Module> lowerGrhToGrhsim(
        const wolvrix::lib::grh::Graph &graph,
        wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        return Lowering(graph, diagnostics).run();
    }

    std::optional<Module> lowerGrhToGrhsim(
        const wolvrix::lib::grh::Design &design,
        const LowerGrhsimOptions &options,
        wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        const wolvrix::lib::grh::Graph *graph = nullptr;
        if (options.top)
        {
            graph = design.findGraph(*options.top);
            if (!graph)
            {
                diagnostics.error("lower_grhsim top graph was not found", *options.top);
                return std::nullopt;
            }
        }
        else if (design.topGraphs().size() == 1)
        {
            graph = design.findGraph(design.topGraphs().front());
        }
        else if (design.graphs().size() == 1)
        {
            graph = design.graphs().begin()->second.get();
        }
        else
        {
            diagnostics.error("lower_grhsim requires an explicit top when the design does not "
                              "have exactly one top graph",
                              "design");
            return std::nullopt;
        }
        if (!graph)
        {
            diagnostics.error("lower_grhsim could not resolve a source graph", "design");
            return std::nullopt;
        }
        return lowerGrhToGrhsim(*graph, diagnostics);
    }

} // namespace wolvrix::lib::grhsim
