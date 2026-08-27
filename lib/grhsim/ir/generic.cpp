#include "grhsim/ir/generic.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <utility>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        constexpr uint32_t kMany = std::numeric_limits<uint32_t>::max();

        struct OpcodeName
        {
            GenericOpcode opcode;
            std::string_view name;
        };

        constexpr std::array kOpcodeNames{
            OpcodeName{GenericOpcode::Const, "const"},
            OpcodeName{GenericOpcode::Add, "add"},
            OpcodeName{GenericOpcode::Sub, "sub"},
            OpcodeName{GenericOpcode::Mul, "mul"},
            OpcodeName{GenericOpcode::Div, "div"},
            OpcodeName{GenericOpcode::Mod, "mod"},
            OpcodeName{GenericOpcode::And, "and"},
            OpcodeName{GenericOpcode::Or, "or"},
            OpcodeName{GenericOpcode::Xor, "xor"},
            OpcodeName{GenericOpcode::Xnor, "xnor"},
            OpcodeName{GenericOpcode::Not, "not"},
            OpcodeName{GenericOpcode::Lt, "lt"},
            OpcodeName{GenericOpcode::Le, "le"},
            OpcodeName{GenericOpcode::Gt, "gt"},
            OpcodeName{GenericOpcode::Ge, "ge"},
            OpcodeName{GenericOpcode::Eq, "eq"},
            OpcodeName{GenericOpcode::Ne, "ne"},
            OpcodeName{GenericOpcode::CaseEq, "case_eq"},
            OpcodeName{GenericOpcode::CaseNe, "case_ne"},
            OpcodeName{GenericOpcode::WildEq, "wild_eq"},
            OpcodeName{GenericOpcode::WildNe, "wild_ne"},
            OpcodeName{GenericOpcode::LogicAnd, "logic_and"},
            OpcodeName{GenericOpcode::LogicOr, "logic_or"},
            OpcodeName{GenericOpcode::LogicNot, "logic_not"},
            OpcodeName{GenericOpcode::ReduceAnd, "reduce_and"},
            OpcodeName{GenericOpcode::ReduceNand, "reduce_nand"},
            OpcodeName{GenericOpcode::ReduceOr, "reduce_or"},
            OpcodeName{GenericOpcode::ReduceNor, "reduce_nor"},
            OpcodeName{GenericOpcode::ReduceXor, "reduce_xor"},
            OpcodeName{GenericOpcode::ReduceXnor, "reduce_xnor"},
            OpcodeName{GenericOpcode::Shl, "shl"},
            OpcodeName{GenericOpcode::LShr, "lshr"},
            OpcodeName{GenericOpcode::AShr, "ashr"},
            OpcodeName{GenericOpcode::Mux, "mux"},
            OpcodeName{GenericOpcode::Assign, "assign"},
            OpcodeName{GenericOpcode::Concat, "concat"},
            OpcodeName{GenericOpcode::Replicate, "replicate"},
            OpcodeName{GenericOpcode::SliceStatic, "slice_static"},
            OpcodeName{GenericOpcode::SliceDynamic, "slice_dynamic"},
            OpcodeName{GenericOpcode::SliceArray, "slice_array"},
            OpcodeName{GenericOpcode::ArrayLaneConst, "array_lane_const"},
            OpcodeName{GenericOpcode::ArrayMux, "array_mux"},
            OpcodeName{GenericOpcode::ArrayOnehot, "array_onehot"},
            OpcodeName{GenericOpcode::ArrayReduceOr, "array_reduce_or"},
            OpcodeName{GenericOpcode::ArrayReduceAnd, "array_reduce_and"},
            OpcodeName{GenericOpcode::ArrayReduceXor, "array_reduce_xor"},
            OpcodeName{GenericOpcode::ArrayReduceLanesOr, "array_reduce_lanes_or"},
            OpcodeName{GenericOpcode::ArrayReduceLanesAnd, "array_reduce_lanes_and"},
            OpcodeName{GenericOpcode::ArrayReduceLanesXor, "array_reduce_lanes_xor"},
            OpcodeName{GenericOpcode::ArrayBroadcast, "array_broadcast"},
            OpcodeName{GenericOpcode::InRead, "in_read"},
            OpcodeName{GenericOpcode::OutWrite, "out_write"},
            OpcodeName{GenericOpcode::RegRead, "reg_read"},
            OpcodeName{GenericOpcode::RegWrite, "reg_write"},
            OpcodeName{GenericOpcode::LatchRead, "latch_read"},
            OpcodeName{GenericOpcode::LatchWrite, "latch_write"},
            OpcodeName{GenericOpcode::MemRead, "mem_read"},
            OpcodeName{GenericOpcode::MemReadAll, "mem_read_all"},
            OpcodeName{GenericOpcode::MemWrite, "mem_write"},
            OpcodeName{GenericOpcode::MemWriteLanes, "mem_write_lanes"},
            OpcodeName{GenericOpcode::MemFill, "mem_fill"},
            OpcodeName{GenericOpcode::HostCall, "host_call"},
        };

        AttrSchema required(std::string name, AttrType type)
        {
            return AttrSchema{std::move(name), type, true};
        }

        AttrSchema optional(std::string name, AttrType type)
        {
            return AttrSchema{std::move(name), type, false};
        }

        OpSchema schema(GenericOpcode opcode, uint32_t operands, uint32_t results,
                        std::vector<AttrSchema> attrs = {})
        {
            return OpSchema{
                .kind = genericOp(opcode),
                .name = "generic." + std::string(genericOpcodeName(opcode)),
                .minOperands = operands,
                .maxOperands = operands,
                .minResults = results,
                .maxResults = results,
                .attrs = std::move(attrs),
            };
        }

        OpSchema variadic(GenericOpcode opcode,
                          uint32_t minOperands, uint32_t maxOperands,
                          uint32_t minResults, uint32_t maxResults,
                          std::vector<AttrSchema> attrs = {})
        {
            OpSchema result = schema(opcode, minOperands, minResults, std::move(attrs));
            result.maxOperands = maxOperands;
            result.maxResults = maxResults;
            return result;
        }

        DialectRegistry makeRegistry()
        {
            DialectRegistry registry;
            registry.registerDialect(kGenericDialect, "generic");

            registry.registerOp(schema(GenericOpcode::Const, 0, 1,
                                       {required("value", AttrType::String)}));
            for (GenericOpcode opcode : {
                     GenericOpcode::Add, GenericOpcode::Sub, GenericOpcode::Mul,
                     GenericOpcode::Div, GenericOpcode::Mod, GenericOpcode::And,
                     GenericOpcode::Or, GenericOpcode::Xor, GenericOpcode::Xnor,
                     GenericOpcode::Lt, GenericOpcode::Le, GenericOpcode::Gt,
                     GenericOpcode::Ge, GenericOpcode::Eq, GenericOpcode::Ne,
                     GenericOpcode::CaseEq, GenericOpcode::CaseNe,
                     GenericOpcode::WildEq, GenericOpcode::WildNe,
                     GenericOpcode::LogicAnd, GenericOpcode::LogicOr,
                     GenericOpcode::Shl, GenericOpcode::LShr, GenericOpcode::AShr})
            {
                registry.registerOp(schema(opcode, 2, 1));
            }
            for (GenericOpcode opcode : {
                     GenericOpcode::Not, GenericOpcode::LogicNot,
                     GenericOpcode::ReduceAnd, GenericOpcode::ReduceNand,
                     GenericOpcode::ReduceOr, GenericOpcode::ReduceNor,
                     GenericOpcode::ReduceXor, GenericOpcode::ReduceXnor,
                     GenericOpcode::Assign})
            {
                registry.registerOp(schema(opcode, 1, 1));
            }
            registry.registerOp(schema(GenericOpcode::Mux, 3, 1));
            registry.registerOp(variadic(GenericOpcode::Concat, 1, kMany, 1, 1));
            registry.registerOp(schema(GenericOpcode::Replicate, 1, 1,
                                       {required("count", AttrType::Int)}));
            registry.registerOp(schema(GenericOpcode::SliceStatic, 1, 1,
                                       {required("lsb", AttrType::Int)}));
            registry.registerOp(schema(GenericOpcode::SliceDynamic, 2, 1));
            registry.registerOp(schema(GenericOpcode::SliceArray, 2, 1));

            registry.registerOp(schema(GenericOpcode::ArrayLaneConst, 0, 1,
                                       {required("elem_width", AttrType::Int),
                                        required("rows", AttrType::Int),
                                        required("values", AttrType::IntArray)}));
            registry.registerOp(schema(GenericOpcode::ArrayMux, 3, 1));
            registry.registerOp(schema(GenericOpcode::ArrayOnehot, 1, 1,
                                       {required("rows", AttrType::Int)}));
            for (GenericOpcode opcode : {
                     GenericOpcode::ArrayReduceOr, GenericOpcode::ArrayReduceAnd,
                     GenericOpcode::ArrayReduceXor, GenericOpcode::ArrayReduceLanesOr,
                     GenericOpcode::ArrayReduceLanesAnd,
                     GenericOpcode::ArrayReduceLanesXor})
            {
                registry.registerOp(schema(opcode, 1, 1,
                                           {required("elem_width", AttrType::Int)}));
            }
            registry.registerOp(schema(GenericOpcode::ArrayBroadcast, 1, 1,
                                       {required("rows", AttrType::Int)}));

            registry.registerOp(schema(GenericOpcode::InRead, 0, 1,
                                       {required("port", AttrType::String)}));
            registry.registerOp(schema(GenericOpcode::OutWrite, 1, 0,
                                       {required("port", AttrType::String),
                                        optional("eventState", AttrType::Bool)}));
            registry.registerOp(schema(GenericOpcode::RegRead, 0, 1,
                                       {required("state", AttrType::String)}));
            registry.registerOp(schema(GenericOpcode::RegWrite, 3, 0,
                                       {required("state", AttrType::String),
                                        required("events", AttrType::StringArray),
                                        required("eventEdge", AttrType::StringArray)}));
            registry.registerOp(schema(GenericOpcode::LatchRead, 0, 1,
                                       {required("state", AttrType::String)}));
            registry.registerOp(schema(GenericOpcode::LatchWrite, 3, 0,
                                       {required("state", AttrType::String)}));
            registry.registerOp(schema(GenericOpcode::MemRead, 1, 1,
                                       {required("state", AttrType::String)}));
            registry.registerOp(schema(GenericOpcode::MemReadAll, 0, 1,
                                       {required("state", AttrType::String)}));
            const std::vector<AttrSchema> memoryWriteAttrs{
                required("state", AttrType::String),
                required("events", AttrType::StringArray),
                required("eventEdge", AttrType::StringArray),
                optional("memoryWrite.priorityGroup", AttrType::String),
                optional("memoryWrite.priority", AttrType::Int),
            };
            registry.registerOp(schema(GenericOpcode::MemWrite, 4, 0, memoryWriteAttrs));
            registry.registerOp(schema(GenericOpcode::MemWriteLanes, 2, 0, memoryWriteAttrs));
            registry.registerOp(schema(GenericOpcode::MemFill, 2, 0, memoryWriteAttrs));
            registry.registerOp(variadic(
                GenericOpcode::HostCall, 0, kMany, 0, kMany,
                {required("entry", AttrType::String),
                 optional("events", AttrType::StringArray),
                 optional("eventEdge", AttrType::StringArray)}));
            return registry;
        }

        bool valueMatchesType(const AttrValue &value, AttrType type)
        {
            switch (type)
            {
            case AttrType::Bool: return std::holds_alternative<bool>(value);
            case AttrType::Int: return std::holds_alternative<int64_t>(value);
            case AttrType::Double: return std::holds_alternative<double>(value);
            case AttrType::String: return std::holds_alternative<SymbolId>(value);
            case AttrType::BoolArray: return std::holds_alternative<std::vector<bool>>(value);
            case AttrType::IntArray: return std::holds_alternative<std::vector<int64_t>>(value);
            case AttrType::DoubleArray: return std::holds_alternative<std::vector<double>>(value);
            case AttrType::StringArray:
                return std::holds_alternative<std::vector<SymbolId>>(value);
            }
            return false;
        }

        const TypeRec *edgeType(const Module &module, EdgeId edge)
        {
            return module.type(module.edgeType(edge));
        }

        bool isLogic(const Module &module, EdgeId edge, std::optional<uint32_t> width = {})
        {
            const TypeRec *type = edgeType(module, edge);
            return type && type->track == TypeTrack::Generic &&
                   type->kind == static_cast<uint8_t>(GenericTypeKind::Logic) &&
                   (!width || type->width == *width);
        }

        const StateEntry *stateAttr(const Module &module, OpId op, std::string_view attrName,
                                    StateId &id)
        {
            const AttrValue *attr = module.attr(op, attrName);
            const SymbolId *name = attr ? std::get_if<SymbolId>(attr) : nullptr;
            if (!name)
            {
                return nullptr;
            }
            id = module.findState(module.symbol(*name));
            return module.state(id);
        }

        bool sameType(const Module &module, EdgeId edge, TypeId type)
        {
            return module.edgeType(edge) == type;
        }

        std::string opContext(OpId op, std::string_view name)
        {
            return std::string(name) + " op=" + std::to_string(op.raw);
        }

        bool validateEvents(const Module &module, OpId op,
                            wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const AttrValue *eventsAttr = module.attr(op, "events");
            const AttrValue *edgesAttr = module.attr(op, "eventEdge");
            const auto *events = eventsAttr ? std::get_if<std::vector<SymbolId>>(eventsAttr) : nullptr;
            const auto *eventEdges = edgesAttr ? std::get_if<std::vector<SymbolId>>(edgesAttr) : nullptr;
            if (!events && !eventEdges)
            {
                return true;
            }
            if (!events || !eventEdges || events->size() != eventEdges->size())
            {
                diagnostics.error("events and eventEdge must be equally-sized string arrays",
                                  "op=" + std::to_string(op.raw));
                return false;
            }
            bool success = true;
            for (std::size_t index = 0; index < events->size(); ++index)
            {
                const StateId state = module.findState(module.symbol((*events)[index]));
                const StateEntry *entry = module.state(state);
                const TypeRec *type = entry ? module.type(entry->genType) : nullptr;
                if (!entry || !type || type->kind != static_cast<uint8_t>(GenericTypeKind::Logic) ||
                    type->width != 1)
                {
                    diagnostics.error("event must name a one-bit StateDecl",
                                      "op=" + std::to_string(op.raw));
                    success = false;
                }
                const std::string_view edge = module.symbol((*eventEdges)[index]);
                if (edge != "posedge" && edge != "negedge")
                {
                    diagnostics.error("eventEdge must be posedge or negedge",
                                      "op=" + std::to_string(op.raw));
                    success = false;
                }
            }
            return success;
        }

        std::optional<int64_t> intAttr(const Module &module, OpId op, std::string_view name)
        {
            const AttrValue *value = module.attr(op, name);
            if (!value)
            {
                return std::nullopt;
            }
            if (const auto *integer = std::get_if<int64_t>(value))
            {
                return *integer;
            }
            return std::nullopt;
        }

        bool validateComputeContract(const Module &module, OpId op, GenericOpcode opcode,
                                     std::string_view name,
                                     wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            if (opcode == GenericOpcode::Const || opcode >= GenericOpcode::InRead)
            {
                return true;
            }

            const auto operands = module.operands(op);
            const auto results = module.results(op);
            const auto context = opContext(op, name);
            bool success = true;
            const auto report = [&](std::string message) {
                diagnostics.error(std::move(message), context);
                success = false;
            };
            const auto width = [&](EdgeId edge) -> std::optional<uint32_t> {
                const TypeRec *type = edgeType(module, edge);
                if (!type || type->track != TypeTrack::Generic ||
                    type->kind != static_cast<uint8_t>(GenericTypeKind::Logic))
                {
                    return std::nullopt;
                }
                return type->width;
            };
            const auto hasShape = [&](std::size_t operandCount, std::size_t resultCount) {
                return operands.size() == operandCount && results.size() == resultCount;
            };
            const auto checkBinaryResultWidth = [&](uint64_t expected) {
                if (!hasShape(2, 1))
                {
                    return;
                }
                const auto resultWidth = width(results[0]);
                if (resultWidth && *resultWidth != expected)
                {
                    report("generic operation result width does not match its operands");
                }
            };
            const auto checkOneBitResult = [&]() {
                if (!results.empty())
                {
                    const auto resultWidth = width(results[0]);
                    if (resultWidth && *resultWidth != 1)
                    {
                        report("generic comparison, logical, or reduction result must be one bit");
                    }
                }
            };
            const auto checkPositiveProduct = [&](std::optional<int64_t> count,
                                                  uint32_t inputWidth,
                                                  uint32_t resultWidth,
                                                  std::string_view countMessage,
                                                  std::string_view widthMessage) {
                if (!count || *count <= 0)
                {
                    report(std::string(countMessage));
                    return;
                }
                if (static_cast<uint64_t>(*count) > std::numeric_limits<uint32_t>::max() ||
                    static_cast<uint64_t>(inputWidth) * static_cast<uint64_t>(*count) !=
                        resultWidth)
                {
                    report(std::string(widthMessage));
                }
            };

            switch (opcode)
            {
            case GenericOpcode::Add:
            case GenericOpcode::Sub:
            case GenericOpcode::And:
            case GenericOpcode::Or:
            case GenericOpcode::Xor:
            case GenericOpcode::Xnor:
                if (hasShape(2, 1))
                {
                    const auto lhs = width(operands[0]);
                    const auto rhs = width(operands[1]);
                    if (lhs && rhs)
                    {
                        checkBinaryResultWidth(std::max(*lhs, *rhs));
                    }
                }
                break;
            case GenericOpcode::Mul:
                if (hasShape(2, 1))
                {
                    const auto lhs = width(operands[0]);
                    const auto rhs = width(operands[1]);
                    if (lhs && rhs)
                    {
                        checkBinaryResultWidth(static_cast<uint64_t>(*lhs) + *rhs);
                    }
                }
                break;
            case GenericOpcode::Div:
            case GenericOpcode::Mod:
                if (hasShape(2, 1))
                {
                    if (const auto lhs = width(operands[0]))
                    {
                        checkBinaryResultWidth(*lhs);
                    }
                }
                break;
            case GenericOpcode::Not:
                if (hasShape(1, 1))
                {
                    const auto inputWidth = width(operands[0]);
                    const auto resultWidth = width(results[0]);
                    if (inputWidth && resultWidth && *inputWidth != *resultWidth)
                    {
                        report("generic.not result width must match its operand");
                    }
                }
                break;
            case GenericOpcode::Lt:
            case GenericOpcode::Le:
            case GenericOpcode::Gt:
            case GenericOpcode::Ge:
            case GenericOpcode::Eq:
            case GenericOpcode::Ne:
            case GenericOpcode::CaseEq:
            case GenericOpcode::CaseNe:
            case GenericOpcode::WildEq:
            case GenericOpcode::WildNe:
            case GenericOpcode::LogicAnd:
            case GenericOpcode::LogicOr:
            case GenericOpcode::LogicNot:
            case GenericOpcode::ReduceAnd:
            case GenericOpcode::ReduceNand:
            case GenericOpcode::ReduceOr:
            case GenericOpcode::ReduceNor:
            case GenericOpcode::ReduceXor:
            case GenericOpcode::ReduceXnor:
                checkOneBitResult();
                break;
            case GenericOpcode::Mux:
                if (hasShape(3, 1))
                {
                    if (!isLogic(module, operands[0], 1))
                    {
                        report("generic.mux condition must be one bit");
                    }
                    const auto trueWidth = width(operands[1]);
                    const auto falseWidth = width(operands[2]);
                    const auto resultWidth = width(results[0]);
                    if (trueWidth && falseWidth && resultWidth &&
                        (*falseWidth != *trueWidth || *resultWidth != *trueWidth))
                    {
                        report("generic.mux data operands and result must have identical widths");
                    }
                }
                break;
            case GenericOpcode::Assign:
            case GenericOpcode::Shl:
            case GenericOpcode::LShr:
            case GenericOpcode::AShr:
                break;
            case GenericOpcode::Concat:
                if (results.size() == 1)
                {
                    uint64_t expected = 0;
                    bool widthsValid = true;
                    for (EdgeId operand : operands)
                    {
                        const auto operandWidth = width(operand);
                        if (!operandWidth ||
                            expected > std::numeric_limits<uint64_t>::max() - *operandWidth)
                        {
                            widthsValid = false;
                            break;
                        }
                        expected += *operandWidth;
                    }
                    const auto resultWidth = width(results[0]);
                    if (widthsValid && resultWidth && *resultWidth != expected)
                    {
                        report("generic.concat result width must equal the operand width sum");
                    }
                }
                break;
            case GenericOpcode::Replicate:
                if (hasShape(1, 1))
                {
                    const auto inputWidth = width(operands[0]);
                    const auto resultWidth = width(results[0]);
                    if (inputWidth && resultWidth)
                    {
                        checkPositiveProduct(intAttr(module, op, "count"), *inputWidth,
                                             *resultWidth,
                                             "replicate count must be positive",
                                             "generic.replicate result width is inconsistent");
                    }
                }
                break;
            case GenericOpcode::SliceStatic:
                if (hasShape(1, 1))
                {
                    const auto lsb = intAttr(module, op, "lsb");
                    const auto inputWidth = width(operands[0]);
                    const auto resultWidth = width(results[0]);
                    if (!lsb || *lsb < 0)
                    {
                        report("slice_static lsb must be non-negative");
                    }
                    else if (inputWidth && resultWidth &&
                             static_cast<uint64_t>(*lsb) + *resultWidth > *inputWidth)
                    {
                        report("generic.slice_static range exceeds its input width");
                    }
                }
                break;
            case GenericOpcode::SliceDynamic:
            case GenericOpcode::SliceArray:
                if (hasShape(2, 1))
                {
                    const auto inputWidth = width(operands[0]);
                    const auto resultWidth = width(results[0]);
                    if (inputWidth && resultWidth && *resultWidth > *inputWidth)
                    {
                        report("generic dynamic slice result width exceeds its input width");
                    }
                    if (opcode == GenericOpcode::SliceArray && inputWidth && resultWidth &&
                        *inputWidth % *resultWidth != 0)
                    {
                        report("generic.slice_array result width must divide its input width");
                    }
                }
                break;
            case GenericOpcode::ArrayLaneConst:
                if (hasShape(0, 1))
                {
                    const auto rows = intAttr(module, op, "rows");
                    const auto elemWidth = intAttr(module, op, "elem_width");
                    const AttrValue *valuesAttr = module.attr(op, "values");
                    const auto *values = valuesAttr
                                             ? std::get_if<std::vector<int64_t>>(valuesAttr)
                                             : nullptr;
                    if (!rows || *rows <= 0)
                    {
                        report("array_lane_const rows must be positive");
                    }
                    if (!elemWidth || *elemWidth <= 0)
                    {
                        report("array_lane_const elem_width must be positive");
                    }
                    if (rows && *rows > 0 && values &&
                        static_cast<uint64_t>(*rows) != values->size())
                    {
                        report("array_lane_const values count must equal rows");
                    }
                    const auto resultWidth = width(results[0]);
                    if (rows && *rows > 0 && elemWidth && *elemWidth > 0 && resultWidth &&
                        (static_cast<uint64_t>(*rows) > std::numeric_limits<uint32_t>::max() ||
                         static_cast<uint64_t>(*elemWidth) >
                             std::numeric_limits<uint32_t>::max() ||
                         static_cast<uint64_t>(*rows) * static_cast<uint64_t>(*elemWidth) !=
                             *resultWidth))
                    {
                        report("array_lane_const result width must equal rows times elem_width");
                    }
                }
                break;
            case GenericOpcode::ArrayMux:
                if (hasShape(3, 1))
                {
                    const auto rows = width(operands[0]);
                    const auto dataWidth = width(operands[1]);
                    const TypeId dataType = module.edgeType(operands[1]);
                    if (module.edgeType(operands[2]) != dataType ||
                        module.edgeType(results[0]) != dataType)
                    {
                        report("array_mux data operands and result must have identical Types");
                    }
                    if (rows && dataWidth && *dataWidth % *rows != 0)
                    {
                        report("array_mux packed data width must be divisible by its row count");
                    }
                }
                break;
            case GenericOpcode::ArrayOnehot:
                if (hasShape(1, 1))
                {
                    const auto rows = intAttr(module, op, "rows");
                    const auto resultWidth = width(results[0]);
                    if (!rows || *rows <= 0)
                    {
                        report("array_onehot rows must be positive");
                    }
                    else if (resultWidth &&
                             (static_cast<uint64_t>(*rows) >
                                  std::numeric_limits<uint32_t>::max() ||
                              *resultWidth != static_cast<uint64_t>(*rows)))
                    {
                        report("array_onehot result width must equal rows");
                    }
                }
                break;
            case GenericOpcode::ArrayReduceOr:
            case GenericOpcode::ArrayReduceAnd:
            case GenericOpcode::ArrayReduceXor:
            case GenericOpcode::ArrayReduceLanesOr:
            case GenericOpcode::ArrayReduceLanesAnd:
            case GenericOpcode::ArrayReduceLanesXor:
                if (hasShape(1, 1))
                {
                    const auto elemWidth = intAttr(module, op, "elem_width");
                    const auto inputWidth = width(operands[0]);
                    const auto resultWidth = width(results[0]);
                    if (!elemWidth || *elemWidth <= 0)
                    {
                        report("array reduction elem_width must be positive");
                    }
                    else if (static_cast<uint64_t>(*elemWidth) >
                                 std::numeric_limits<uint32_t>::max() ||
                             (inputWidth && *inputWidth % static_cast<uint32_t>(*elemWidth) != 0))
                    {
                        report("array reduction elem_width must divide its input width");
                    }
                    else if (inputWidth && resultWidth)
                    {
                        const bool reduceToBit = opcode == GenericOpcode::ArrayReduceOr ||
                                                 opcode == GenericOpcode::ArrayReduceAnd ||
                                                 opcode == GenericOpcode::ArrayReduceXor;
                        const uint32_t expected = reduceToBit
                                                      ? 1U
                                                      : *inputWidth /
                                                            static_cast<uint32_t>(*elemWidth);
                        if (*resultWidth != expected)
                        {
                            report("array reduction result width is inconsistent");
                        }
                    }
                }
                break;
            case GenericOpcode::ArrayBroadcast:
                if (hasShape(1, 1))
                {
                    const auto inputWidth = width(operands[0]);
                    const auto resultWidth = width(results[0]);
                    if (inputWidth && resultWidth)
                    {
                        checkPositiveProduct(intAttr(module, op, "rows"), *inputWidth,
                                             *resultWidth,
                                             "array_broadcast rows must be positive",
                                             "array_broadcast result width is inconsistent");
                    }
                }
                break;
            default:
                break;
            }
            return success;
        }
    } // namespace

    bool DialectRegistry::registerDialect(uint16_t id, std::string name)
    {
        if (name.empty())
        {
            return false;
        }
        if (dialects_.size() <= id)
        {
            dialects_.resize(static_cast<std::size_t>(id) + 1U);
        }
        if (!dialects_[id].empty())
        {
            return false;
        }
        dialects_[id] = std::move(name);
        return true;
    }

    bool DialectRegistry::registerOp(OpSchema schemaValue)
    {
        if (!schemaValue.kind.valid() || schemaValue.name.empty() ||
            schemaValue.kind.dialect() >= dialects_.size() ||
            dialects_[schemaValue.kind.dialect()].empty() || find(schemaValue.kind))
        {
            return false;
        }
        ops_.push_back(std::move(schemaValue));
        return true;
    }

    const OpSchema *DialectRegistry::find(OpKind kind) const noexcept
    {
        const auto found = std::find_if(ops_.begin(), ops_.end(),
                                        [&](const OpSchema &schema) { return schema.kind == kind; });
        return found == ops_.end() ? nullptr : &*found;
    }

    std::optional<OpKind> DialectRegistry::find(std::string_view qualifiedName) const noexcept
    {
        const auto found = std::find_if(ops_.begin(), ops_.end(), [&](const OpSchema &schema) {
            return schema.name == qualifiedName;
        });
        return found == ops_.end() ? std::nullopt : std::optional<OpKind>(found->kind);
    }

    std::string_view DialectRegistry::dialectName(uint16_t id) const noexcept
    {
        return id < dialects_.size() ? std::string_view(dialects_[id]) : std::string_view{};
    }

    std::string_view DialectRegistry::opName(OpKind kind) const noexcept
    {
        const OpSchema *schemaValue = find(kind);
        return schemaValue ? std::string_view(schemaValue->name) : std::string_view{};
    }

    std::vector<std::string> DialectRegistry::availableOps() const
    {
        std::vector<std::string> result;
        result.reserve(ops_.size());
        for (const OpSchema &schemaValue : ops_)
        {
            result.push_back(schemaValue.name);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    bool DialectRegistry::validateOp(const Module &module, OpId op,
                                     wolvrix::lib::diag::Diagnostics &diagnostics) const
    {
        const OpSchema *schemaValue = find(module.kind(op));
        if (!schemaValue)
        {
            diagnostics.error("operation is not registered by a GRHSIM dialect",
                              "op=" + std::to_string(op.raw));
            return false;
        }
        bool success = true;
        const std::size_t operandCount = module.operands(op).size();
        const std::size_t resultCount = module.results(op).size();
        if (operandCount < schemaValue->minOperands || operandCount > schemaValue->maxOperands ||
            resultCount < schemaValue->minResults || resultCount > schemaValue->maxResults)
        {
            diagnostics.error("operation operand/result count does not match its schema",
                              opContext(op, schemaValue->name));
            success = false;
        }

        std::unordered_set<std::string_view> seenAttrs;
        for (const AttrKV &attr : module.attrs(op))
        {
            const std::string_view name = module.symbol(attr.key);
            const auto found = std::find_if(schemaValue->attrs.begin(), schemaValue->attrs.end(),
                                            [&](const AttrSchema &candidate) {
                                                return candidate.name == name;
                                            });
            if (found == schemaValue->attrs.end())
            {
                diagnostics.error("unknown attribute '" + std::string(name) + "'",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            else if (!valueMatchesType(attr.value, found->type))
            {
                diagnostics.error("attribute '" + std::string(name) + "' has the wrong type",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            if (!seenAttrs.insert(name).second)
            {
                diagnostics.error("operation has duplicate attributes",
                                  opContext(op, schemaValue->name));
                success = false;
            }
        }
        for (const AttrSchema &attr : schemaValue->attrs)
        {
            if (attr.required && !seenAttrs.contains(attr.name))
            {
                diagnostics.error("required attribute '" + attr.name + "' is missing",
                                  opContext(op, schemaValue->name));
                success = false;
            }
        }

        if (module.kind(op).dialect() != kGenericDialect)
        {
            return success;
        }
        for (EdgeId result : module.results(op))
        {
            const TypeRec *type = edgeType(module, result);
            if (!type || type->track != TypeTrack::Generic)
            {
                diagnostics.error("generic operation must produce generic edge Types",
                                  opContext(op, schemaValue->name));
                success = false;
            }
        }

        const GenericOpcode opcode = static_cast<GenericOpcode>(module.kind(op).opcode());
        if (opcode == GenericOpcode::Const)
        {
            const TypeRec *type = module.results(op).empty()
                                      ? nullptr
                                      : edgeType(module, module.results(op)[0]);
            if (!type || type->track != TypeTrack::Generic ||
                (type->kind != static_cast<uint8_t>(GenericTypeKind::Logic) &&
                 type->kind != static_cast<uint8_t>(GenericTypeKind::Real) &&
                 type->kind != static_cast<uint8_t>(GenericTypeKind::String)))
            {
                diagnostics.error("generic.const must produce a non-array generic Type",
                                  opContext(op, schemaValue->name));
                success = false;
            }
        }
        if (opcode != GenericOpcode::Const && opcode != GenericOpcode::HostCall &&
            opcode < GenericOpcode::InRead)
        {
            for (EdgeId edge : module.operands(op))
            {
                if (!isLogic(module, edge))
                {
                    diagnostics.error("generic compute operation requires logic operands",
                                      opContext(op, schemaValue->name));
                    success = false;
                }
            }
            for (EdgeId edge : module.results(op))
            {
                if (!isLogic(module, edge))
                {
                    diagnostics.error("generic compute operation requires logic results",
                                      opContext(op, schemaValue->name));
                    success = false;
                }
            }
        }
        success = validateComputeContract(module, op, opcode, schemaValue->name, diagnostics) &&
                  success;

        StateId stateId = StateId::invalid();
        const StateEntry *state = nullptr;
        switch (opcode)
        {
        case GenericOpcode::InRead:
            state = stateAttr(module, op, "port", stateId);
            if (!state || state->kind != StateKind::Input || module.results(op).empty() ||
                !sameType(module, module.results(op)[0], state->genType))
            {
                diagnostics.error("in_read must read a matching input StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            break;
        case GenericOpcode::OutWrite:
            state = stateAttr(module, op, "port", stateId);
            if (!state || state->kind != StateKind::Output || module.operands(op).empty() ||
                !sameType(module, module.operands(op)[0], state->genType))
            {
                diagnostics.error("out_write must write a matching output StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            break;
        case GenericOpcode::RegRead:
        case GenericOpcode::LatchRead:
            state = stateAttr(module, op, "state", stateId);
            if (!state || state->kind != StateKind::State || module.results(op).empty() ||
                !sameType(module, module.results(op)[0], state->genType))
            {
                diagnostics.error("state read must reference a matching StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            break;
        case GenericOpcode::RegWrite:
        case GenericOpcode::LatchWrite:
            state = stateAttr(module, op, "state", stateId);
            if (!state || state->kind != StateKind::State || module.operands(op).size() < 3 ||
                !isLogic(module, module.operands(op)[0], 1) ||
                !sameType(module, module.operands(op)[1], state->genType) ||
                !sameType(module, module.operands(op)[2], state->genType))
            {
                diagnostics.error("state write operands do not match the target StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            if (opcode == GenericOpcode::RegWrite)
            {
                success = validateEvents(module, op, diagnostics) && success;
            }
            break;
        case GenericOpcode::MemRead:
        case GenericOpcode::MemReadAll:
        case GenericOpcode::MemWrite:
        case GenericOpcode::MemWriteLanes:
        case GenericOpcode::MemFill:
        {
            state = stateAttr(module, op, "state", stateId);
            const TypeRec *arrayType = state ? module.type(state->genType) : nullptr;
            const TypeRec *elementType = arrayType ? module.type(arrayType->elementType) : nullptr;
            if (!state || state->kind != StateKind::State || !arrayType ||
                arrayType->kind != static_cast<uint8_t>(GenericTypeKind::Array) || !elementType)
            {
                diagnostics.error("memory operation must reference an array StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
                break;
            }
            const TypeId element = arrayType->elementType;
            const auto opOperands = module.operands(op);
            const auto opResults = module.results(op);
            if (opcode == GenericOpcode::MemRead &&
                (opOperands.empty() || !isLogic(module, opOperands[0]) || opResults.empty() ||
                 !sameType(module, opResults[0], element)))
            {
                diagnostics.error("mem_read address/result Types are invalid",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            const auto isPacked = [&](EdgeId edge) {
                const TypeRec *type = edgeType(module, edge);
                const uint64_t width = static_cast<uint64_t>(arrayType->rows) * elementType->width;
                return type && type->kind == static_cast<uint8_t>(GenericTypeKind::Logic) &&
                       type->width == width;
            };
            if (opcode == GenericOpcode::MemReadAll &&
                (opResults.empty() || !isPacked(opResults[0])))
            {
                diagnostics.error("mem_read_all result must be the packed array view",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            if (opcode == GenericOpcode::MemWrite &&
                (opOperands.size() < 4 || !isLogic(module, opOperands[0], 1) ||
                 !isLogic(module, opOperands[1]) || !sameType(module, opOperands[2], element) ||
                 !sameType(module, opOperands[3], element)))
            {
                diagnostics.error("mem_write operands do not match the array StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            if (opcode == GenericOpcode::MemWriteLanes &&
                (opOperands.size() < 2 || !isLogic(module, opOperands[0], arrayType->rows) ||
                 !isPacked(opOperands[1])))
            {
                diagnostics.error("mem_write_lanes operands do not match the array StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            if (opcode == GenericOpcode::MemFill &&
                (opOperands.size() < 2 || !isLogic(module, opOperands[0], 1) ||
                 !isPacked(opOperands[1])))
            {
                diagnostics.error("mem_fill operands do not match the array StateDecl",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            if (opcode == GenericOpcode::MemWrite ||
                opcode == GenericOpcode::MemWriteLanes || opcode == GenericOpcode::MemFill)
            {
                success = validateEvents(module, op, diagnostics) && success;
                const bool hasGroup = module.attr(op, "memoryWrite.priorityGroup") != nullptr;
                const bool hasPriority = module.attr(op, "memoryWrite.priority") != nullptr;
                if (hasGroup != hasPriority)
                {
                    diagnostics.error("memory write priority attributes must appear together",
                                      opContext(op, schemaValue->name));
                    success = false;
                }
            }
            break;
        }
        case GenericOpcode::HostCall:
        {
            const AttrValue *entryAttr = module.attr(op, "entry");
            const SymbolId *entryName = entryAttr ? std::get_if<SymbolId>(entryAttr) : nullptr;
            const HostId hostId = entryName ? module.findHost(module.symbol(*entryName))
                                            : HostId::invalid();
            const HostEntry *host = module.host(hostId);
            if (!host)
            {
                diagnostics.error("host_call references an unknown HostTable entry",
                                  opContext(op, schemaValue->name));
                success = false;
                break;
            }
            const bool hasEvents = module.attr(op, "events") != nullptr;
            const bool hasEventEdges = module.attr(op, "eventEdge") != nullptr;
            if (host->kind == HostKind::Query && (hasEvents || hasEventEdges))
            {
                diagnostics.error("query host_call must not carry event attributes",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            else if (host->kind == HostKind::Effect && (!hasEvents || !hasEventEdges))
            {
                diagnostics.error("effect host_call requires events and eventEdge attributes",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            uint32_t inputCount = 0;
            uint32_t resultCount = 0;
            for (const HostParam &parameter : module.hostSignature(hostId))
            {
                if (parameter.direction == HostParamDirection::Input ||
                    parameter.direction == HostParamDirection::InOut)
                {
                    ++inputCount;
                }
                if (parameter.direction == HostParamDirection::Output ||
                    parameter.direction == HostParamDirection::InOut ||
                    parameter.direction == HostParamDirection::Return)
                {
                    ++resultCount;
                }
            }
            const uint32_t conditionCount = host->kind == HostKind::Effect ? 1U : 0U;
            const auto operands = module.operands(op);
            const auto results = module.results(op);
            if (operands.size() != inputCount + conditionCount ||
                results.size() != resultCount ||
                (conditionCount && !isLogic(module, operands[0], 1)))
            {
                diagnostics.error("host_call operand/result groups do not match its signature",
                                  opContext(op, schemaValue->name));
                success = false;
            }
            std::size_t operandIndex = conditionCount;
            std::size_t resultIndex = 0;
            for (const HostParam &parameter : module.hostSignature(hostId))
            {
                if (parameter.direction == HostParamDirection::Input ||
                    parameter.direction == HostParamDirection::InOut)
                {
                    if (operandIndex >= operands.size() ||
                        module.edgeType(operands[operandIndex]) != parameter.type)
                    {
                        diagnostics.error("host_call argument Types do not match its signature",
                                          opContext(op, schemaValue->name));
                        success = false;
                    }
                    ++operandIndex;
                }
                if (parameter.direction == HostParamDirection::Output ||
                    parameter.direction == HostParamDirection::InOut ||
                    parameter.direction == HostParamDirection::Return)
                {
                    if (resultIndex >= results.size() ||
                        module.edgeType(results[resultIndex]) != parameter.type)
                    {
                        diagnostics.error("host_call result Types do not match its signature",
                                          opContext(op, schemaValue->name));
                        success = false;
                    }
                    ++resultIndex;
                }
            }
            success = validateEvents(module, op, diagnostics) && success;
            break;
        }
        default:
            break;
        }
        return success;
    }

    const DialectRegistry &dialectRegistry()
    {
        static const DialectRegistry registry = makeRegistry();
        return registry;
    }

    std::string_view genericOpcodeName(GenericOpcode opcode) noexcept
    {
        const auto found = std::find_if(kOpcodeNames.begin(), kOpcodeNames.end(),
                                        [&](OpcodeName entry) { return entry.opcode == opcode; });
        return found == kOpcodeNames.end() ? std::string_view{} : found->name;
    }

    std::optional<GenericOpcode> parseGenericOpcode(std::string_view name) noexcept
    {
        if (name.starts_with("generic."))
        {
            name.remove_prefix(std::string_view("generic.").size());
        }
        const auto found = std::find_if(kOpcodeNames.begin(), kOpcodeNames.end(),
                                        [&](OpcodeName entry) { return entry.name == name; });
        return found == kOpcodeNames.end() ? std::nullopt
                                           : std::optional<GenericOpcode>(found->opcode);
    }

} // namespace wolvrix::lib::grhsim
