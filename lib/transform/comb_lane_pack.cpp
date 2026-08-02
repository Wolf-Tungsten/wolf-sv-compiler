#include "transform/comb_lane_pack.hpp"

#include "core/grh.hpp"
#include "transform/dead_code_elim.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::AttrKV;
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::SrcLoc;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;

        struct AnalyzedNode
        {
            bool valid = false;
            bool internal = false;
            std::string signature;
            std::size_t nodeCount = 0;
        };

        struct RootCandidate
        {
            ValueId value;
            OperationId rootOp;
            OperationId anchorOp;
            std::size_t anchorIndex = 0;
            std::string signature;
            std::string rootSource;
            int32_t laneWidth = 0;
            bool isSigned = false;
            ValueType type = ValueType::Logic;
            std::size_t treeNodes = 0;
        };

        struct CandidateGroup
        {
            std::vector<RootCandidate> roots;
            std::size_t anchorIndex = 0;
        };

        struct AnalyzeState
        {
            const Graph &graph;
            const CombLanePackOptions &options;
            std::unordered_map<ValueId, AnalyzedNode, ValueIdHash> memo;
            std::unordered_set<ValueId, ValueIdHash> stack;
        };

        std::optional<int64_t> getIntAttr(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<int64_t>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::optional<std::string> getStringAttr(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<std::string>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        bool isPointwiseBitwiseKind(OperationKind kind) noexcept
        {
            switch (kind)
            {
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
            case OperationKind::kNot:
                return true;
            default:
                return false;
            }
        }

        std::optional<std::size_t> storageDataOperandIndex(OperationKind kind) noexcept
        {
            switch (kind)
            {
            case OperationKind::kRegisterWritePort:
                return 1;
            case OperationKind::kMemoryWritePort:
                return 2;
            case OperationKind::kMemoryFillPort:
                return 1;
            default:
                return std::nullopt;
            }
        }

        bool isSupportedInternalOp(const Graph &graph,
                                   const Operation &op,
                                   const Value &result,
                                   const CombLanePackOptions &options) noexcept
        {
            const auto operands = op.operands();
            switch (op.kind())
            {
            case OperationKind::kNot:
                return operands.size() == 1 &&
                       operands[0].valid() &&
                       graph.getValue(operands[0]).width() == result.width();
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
                return operands.size() == 2 &&
                       operands[0].valid() &&
                       operands[1].valid() &&
                       graph.getValue(operands[0]).width() == result.width() &&
                       graph.getValue(operands[1]).width() == result.width();
            case OperationKind::kAssign:
                return operands.size() == 1 &&
                       operands[0].valid() &&
                       graph.getValue(operands[0]).width() == result.width();
            case OperationKind::kMux:
                return options.enableMux &&
                       operands.size() == 3 &&
                       operands[0].valid() &&
                       operands[1].valid() &&
                       operands[2].valid() &&
                       graph.getValue(operands[0]).width() == 1 &&
                       graph.getValue(operands[1]).width() == result.width() &&
                       graph.getValue(operands[2]).width() == result.width();
            default:
                return false;
            }
        }

        std::string makeLeafSignature(const Value &value)
        {
            std::ostringstream oss;
            oss << "leaf(" << value.width() << "," << (value.isSigned() ? 1 : 0) << ","
                << static_cast<int>(value.type()) << ")";
            return oss.str();
        }

        void appendAttrs(std::ostringstream &oss, const Operation &op)
        {
            auto appendAttrValue = [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, bool> ||
                              std::is_same_v<T, int64_t> ||
                              std::is_same_v<T, double> ||
                              std::is_same_v<T, std::string>)
                {
                    oss << value;
                }
                else
                {
                    oss << "[";
                    for (std::size_t i = 0; i < value.size(); ++i)
                    {
                        if (i != 0)
                        {
                            oss << ",";
                        }
                        oss << value[i];
                    }
                    oss << "]";
                }
            };
            std::vector<AttrKV> attrs(op.attrs().begin(), op.attrs().end());
            std::sort(attrs.begin(), attrs.end(),
                      [](const AttrKV &lhs, const AttrKV &rhs) { return lhs.key < rhs.key; });
            oss << "{";
            for (std::size_t i = 0; i < attrs.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ",";
                }
                oss << attrs[i].key;
                oss << ":";
                std::visit(appendAttrValue, attrs[i].value);
            }
            oss << "}";
        }

        AnalyzedNode analyzeValue(const Graph &graph,
                                  ValueId valueId,
                                  AnalyzeState &state)
        {
            auto memoIt = state.memo.find(valueId);
            if (memoIt != state.memo.end())
            {
                return memoIt->second;
            }

            AnalyzedNode node;
            if (!valueId.valid())
            {
                state.memo.emplace(valueId, node);
                return node;
            }

            const Value value = graph.getValue(valueId);
            const OperationId defOpId = value.definingOp();
            if (!defOpId.valid())
            {
                node.valid = true;
                node.internal = false;
                node.signature = makeLeafSignature(value);
                node.nodeCount = 0;
                state.memo.emplace(valueId, node);
                return node;
            }

            if (state.stack.find(valueId) != state.stack.end())
            {
                state.memo.emplace(valueId, node);
                return node;
            }

            const Operation defOp = graph.getOperation(defOpId);
            if (!isSupportedInternalOp(graph, defOp, value, state.options))
            {
                node.valid = true;
                node.internal = false;
                node.signature = makeLeafSignature(value);
                node.nodeCount = 0;
                state.memo.emplace(valueId, node);
                return node;
            }

            state.stack.insert(valueId);
            std::vector<AnalyzedNode> operands;
            operands.reserve(defOp.operands().size());
            std::size_t nodeCount = 1;
            bool ok = true;
            for (const ValueId operandId : defOp.operands())
            {
                AnalyzedNode operandNode = analyzeValue(graph, operandId, state);
                if (!operandNode.valid)
                {
                    ok = false;
                    break;
                }
                nodeCount += operandNode.nodeCount;
                if (nodeCount > state.options.maxTreeNodes)
                {
                    ok = false;
                    break;
                }
                operands.push_back(std::move(operandNode));
            }
            state.stack.erase(valueId);

            if (!ok)
            {
                state.memo.emplace(valueId, node);
                return node;
            }

            std::ostringstream oss;
            oss << static_cast<int>(defOp.kind()) << "("
                << value.width() << ","
                << (value.isSigned() ? 1 : 0) << ","
                << static_cast<int>(value.type());
            appendAttrs(oss, defOp);
            oss << ":";
            for (std::size_t i = 0; i < operands.size(); ++i)
            {
                if (i != 0)
                {
                    oss << "|";
                }
                oss << operands[i].signature;
            }
            oss << ")";

            node.valid = true;
            node.internal = true;
            node.signature = std::move(oss).str();
            node.nodeCount = nodeCount;
            state.memo.emplace(valueId, node);
            return node;
        }

        std::size_t totalPackedWidth(std::span<const ValueId> lanes, const Graph &graph)
        {
            std::size_t width = 0;
            for (ValueId lane : lanes)
            {
                width += static_cast<std::size_t>(graph.getValue(lane).width());
            }
            return width;
        }

        bool dependsOnAnyOtherRoot(const Graph &graph,
                                   ValueId start,
                                   const std::unordered_set<ValueId, ValueIdHash> &roots,
                                   ValueId self)
        {
            std::vector<ValueId> stack{start};
            std::unordered_set<ValueId, ValueIdHash> visited;
            while (!stack.empty())
            {
                const ValueId current = stack.back();
                stack.pop_back();
                if (!current.valid())
                {
                    continue;
                }
                if (!visited.insert(current).second)
                {
                    continue;
                }
                if (current != self && roots.find(current) != roots.end())
                {
                    return true;
                }
                const OperationId defOpId = graph.getValue(current).definingOp();
                if (!defOpId.valid())
                {
                    continue;
                }
                const Operation defOp = graph.getOperation(defOpId);
                for (ValueId operand : defOp.operands())
                {
                    if (operand.valid())
                    {
                        stack.push_back(operand);
                    }
                }
            }
            return false;
        }

        bool groupHasCrossRootDependency(const Graph &graph, std::span<const ValueId> roots)
        {
            std::unordered_set<ValueId, ValueIdHash> rootSet;
            rootSet.reserve(roots.size());
            for (ValueId root : roots)
            {
                rootSet.insert(root);
            }
            for (ValueId root : roots)
            {
                if (dependsOnAnyOtherRoot(graph, root, rootSet, root))
                {
                    return true;
                }
            }
            return false;
        }

        ValueId createConcat(Graph &graph, std::span<const ValueId> lanes, std::string_view note)
        {
            if (lanes.size() == 1)
            {
                return lanes.front();
            }
            std::size_t width = totalPackedWidth(lanes, graph);
            const Value first = graph.getValue(lanes.front());
            const auto out = graph.createValue(static_cast<int32_t>(width),
                                               first.isSigned(),
                                               first.type());
            const auto op = graph.createOperation(OperationKind::kConcat);
            for (std::size_t i = lanes.size(); i > 0; --i)
            {
                graph.addOperand(op, lanes[i - 1]);
            }
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        bool allLanesShared(std::span<const ValueId> lanes)
        {
            return std::all_of(lanes.begin() + 1, lanes.end(),
                               [&](ValueId lane) { return lane == lanes.front(); });
        }

        ValueId createUnary(Graph &graph,
                            OperationKind kind,
                            ValueId operand,
                            int32_t width,
                            bool isSigned,
                            ValueType type,
                            std::string_view note)
        {
            const auto out = graph.createValue(width, isSigned, type);
            const auto op = graph.createOperation(kind);
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createBinary(Graph &graph,
                             OperationKind kind,
                             ValueId lhs,
                             ValueId rhs,
                             int32_t width,
                             bool isSigned,
                             ValueType type,
                             std::string_view note)
        {
            const auto out = graph.createValue(width, isSigned, type);
            const auto op = graph.createOperation(kind);
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createReplicate(Graph &graph,
                                ValueId operand,
                                std::size_t rep,
                                std::string_view note)
        {
            const Value value = graph.getValue(operand);
            const auto out = graph.createValue(static_cast<int32_t>(value.width() * rep),
                                               value.isSigned(),
                                               value.type());
            const auto op = graph.createOperation(OperationKind::kReplicate);
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            graph.setAttr(op, "rep", static_cast<int64_t>(rep));
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createSliceStatic(Graph &graph,
                                  ValueId base,
                                  int64_t low,
                                  int64_t high,
                                  bool isSigned,
                                  ValueType type,
                                  std::string_view note)
        {
            const auto width = static_cast<int32_t>(high - low + 1);
            const auto out = graph.createValue(width, isSigned, type);
            const auto op = graph.createOperation(OperationKind::kSliceStatic);
            graph.addOperand(op, base);
            graph.addResult(op, out);
            graph.setAttr(op, "sliceStart", low);
            graph.setAttr(op, "sliceEnd", high);
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createArrayMux(Graph &graph,
                               ValueId sel,
                               ValueId whenTrue,
                               ValueId whenFalse,
                               int32_t width,
                               bool isSigned,
                               ValueType type,
                               std::string_view note)
        {
            const auto out = graph.createValue(width, isSigned, type);
            const auto op = graph.createOperation(OperationKind::kArrayMux);
            graph.addOperand(op, sel);
            graph.addOperand(op, whenTrue);
            graph.addOperand(op, whenFalse);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createArrayBroadcast(Graph &graph,
                                     ValueId scalar,
                                     std::size_t rows,
                                     std::string_view note)
        {
            const Value value = graph.getValue(scalar);
            const auto out = graph.createValue(static_cast<int32_t>(value.width() * rows),
                                               value.isSigned(),
                                               value.type());
            const auto op = graph.createOperation(OperationKind::kArrayBroadcast);
            graph.addOperand(op, scalar);
            graph.addResult(op, out);
            graph.setAttr(op, "rows", static_cast<int64_t>(rows));
            const SrcLoc srcLoc = makeTransformSrcLoc("comb-lane-pack", note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        void rebindOutputPorts(Graph &graph, ValueId from, ValueId to)
        {
            std::vector<std::string> names;
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == from)
                {
                    names.push_back(port.name);
                }
            }
            for (const std::string &name : names)
            {
                graph.bindOutputPort(name, to);
            }
        }

        class PackedBuilder
        {
        public:
            PackedBuilder(Graph &graph, const CombLanePackOptions &options)
                : graph_(graph), options_(options)
            {
            }

            std::optional<ValueId> build(std::span<const ValueId> lanes)
            {
                if (lanes.empty())
                {
                    return std::nullopt;
                }
                AnalyzeState state{graph_, options_};
                const AnalyzedNode firstAnalysis = analyzeValue(graph_, lanes.front(), state);
                if (!firstAnalysis.valid)
                {
                    return std::nullopt;
                }

                const Value firstValue = graph_.getValue(lanes.front());
                if (!firstAnalysis.internal)
                {
                    if (arrayMode() && lanes.size() > 1 && allLanesShared(lanes))
                    {
                        // A pack of the same shared scalar in every lane is a
                        // replicate; array mode emits one kArrayBroadcast.
                        return createArrayBroadcast(graph_, lanes.front(), lanes.size(),
                                                    "pack-array-broadcast");
                    }
                    return createConcat(graph_, lanes, "pack-leaf");
                }

                std::vector<Operation> defs;
                defs.reserve(lanes.size());
                for (ValueId lane : lanes)
                {
                    const Value laneValue = graph_.getValue(lane);
                    if (laneValue.width() != firstValue.width() ||
                        laneValue.isSigned() != firstValue.isSigned() ||
                        laneValue.type() != firstValue.type())
                    {
                        return std::nullopt;
                    }
                    const AnalyzedNode laneAnalysis = analyzeValue(graph_, lane, state);
                    if (!laneAnalysis.valid || laneAnalysis.signature != firstAnalysis.signature)
                    {
                        return std::nullopt;
                    }
                    defs.push_back(graph_.getOperation(laneValue.definingOp()));
                }

                const Operation &proto = defs.front();
                const auto operands = proto.operands();
                const std::size_t laneWidth = static_cast<std::size_t>(firstValue.width());
                const std::size_t packedWidth = laneWidth * lanes.size();

                if (proto.kind() == OperationKind::kMux)
                {
                    std::vector<ValueId> conds;
                    std::vector<ValueId> whenTrue;
                    std::vector<ValueId> whenFalse;
                    conds.reserve(lanes.size());
                    whenTrue.reserve(lanes.size());
                    whenFalse.reserve(lanes.size());
                    for (const Operation &def : defs)
                    {
                        conds.push_back(def.operands()[0]);
                        whenTrue.push_back(def.operands()[1]);
                        whenFalse.push_back(def.operands()[2]);
                    }
                    auto packedTrue = build(whenTrue);
                    auto packedFalse = build(whenFalse);
                    if (!packedTrue || !packedFalse)
                    {
                        return std::nullopt;
                    }
                    if (arrayMode())
                    {
                        // Array mode: one kArrayMux. sel is the per-lane guard
                        // vector (lane 0 at LSB) — one kConcat of the per-lane
                        // conds; when every lane selects on the same scalar the
                        // shared cond is reused via one kArrayBroadcast.
                        ValueId sel;
                        if (allLanesShared(conds))
                        {
                            sel = createArrayBroadcast(graph_, conds.front(), conds.size(),
                                                       "pack-array-broadcast");
                        }
                        else
                        {
                            sel = createConcat(graph_, conds, "pack-array-mux-sel");
                        }
                        return createArrayMux(graph_,
                                              sel,
                                              *packedTrue,
                                              *packedFalse,
                                              static_cast<int32_t>(packedWidth),
                                              firstValue.isSigned(),
                                              firstValue.type(),
                                              "pack-array-mux");
                    }
                    std::vector<ValueId> masks;
                    masks.reserve(conds.size());
                    for (ValueId cond : conds)
                    {
                        masks.push_back(laneWidth == 1 ? cond : createReplicate(graph_, cond, laneWidth, "pack-mux-mask"));
                    }
                    const ValueId packedMask = createConcat(graph_, masks, "pack-mux-mask-concat");
                    const ValueId invMask = createUnary(graph_,
                                                        OperationKind::kNot,
                                                        packedMask,
                                                        static_cast<int32_t>(packedWidth),
                                                        false,
                                                        firstValue.type(),
                                                        "pack-mux-not");
                    const ValueId trueMasked = createBinary(graph_,
                                                            OperationKind::kAnd,
                                                            *packedTrue,
                                                            packedMask,
                                                            static_cast<int32_t>(packedWidth),
                                                            firstValue.isSigned(),
                                                            firstValue.type(),
                                                            "pack-mux-true");
                    const ValueId falseMasked = createBinary(graph_,
                                                             OperationKind::kAnd,
                                                             *packedFalse,
                                                             invMask,
                                                             static_cast<int32_t>(packedWidth),
                                                             firstValue.isSigned(),
                                                             firstValue.type(),
                                                             "pack-mux-false");
                    return createBinary(graph_,
                                        OperationKind::kOr,
                                        trueMasked,
                                        falseMasked,
                                        static_cast<int32_t>(packedWidth),
                                        firstValue.isSigned(),
                                        firstValue.type(),
                                        "pack-mux-or");
                }

                if (operands.size() == 1)
                {
                    std::vector<ValueId> childLanes;
                    childLanes.reserve(lanes.size());
                    for (const Operation &def : defs)
                    {
                        childLanes.push_back(def.operands()[0]);
                    }
                    auto packedChild = build(childLanes);
                    if (!packedChild)
                    {
                        return std::nullopt;
                    }
                    return createUnary(graph_,
                                       proto.kind(),
                                       *packedChild,
                                       static_cast<int32_t>(packedWidth),
                                       firstValue.isSigned(),
                                       firstValue.type(),
                                       "pack-unary");
                }

                if (operands.size() == 2 && isPointwiseBitwiseKind(proto.kind()))
                {
                    std::vector<ValueId> lhsLanes;
                    std::vector<ValueId> rhsLanes;
                    lhsLanes.reserve(lanes.size());
                    rhsLanes.reserve(lanes.size());
                    for (const Operation &def : defs)
                    {
                        lhsLanes.push_back(def.operands()[0]);
                        rhsLanes.push_back(def.operands()[1]);
                    }
                    auto packedLhs = build(lhsLanes);
                    auto packedRhs = build(rhsLanes);
                    if (!packedLhs || !packedRhs)
                    {
                        return std::nullopt;
                    }
                    return createBinary(graph_,
                                        proto.kind(),
                                        *packedLhs,
                                        *packedRhs,
                                        static_cast<int32_t>(packedWidth),
                                        firstValue.isSigned(),
                                        firstValue.type(),
                                        "pack-binary");
                }

                return std::nullopt;
            }

        private:
            bool arrayMode() const
            {
                return options_.outputMode == CombLanePackOutputMode::Array;
            }

            Graph &graph_;
            const CombLanePackOptions &options_;
        };

        std::vector<CandidateGroup> collectCandidateGroups(Graph &graph,
                                                           const CombLanePackOptions &options)
        {
            AnalyzeState state{graph, options};
            std::unordered_map<OperationId, std::size_t, OperationIdHash> opIndex;
            const auto ops = graph.operations();
            opIndex.reserve(ops.size());
            for (std::size_t i = 0; i < ops.size(); ++i)
            {
                opIndex.emplace(ops[i], i);
            }

            std::unordered_map<std::string, std::vector<RootCandidate>> buckets;
            std::unordered_set<ValueId, ValueIdHash> seenRoots;
            auto addCandidate = [&](ValueId rootValueId,
                                    OperationId anchorOpId,
                                    std::string_view rootSource,
                                    bool requireDeclaredRoot) {
                if (!rootValueId.valid())
                {
                    return;
                }
                const Value result = graph.getValue(rootValueId);
                if (result.width() <= 0 || result.isInput() || result.isInout())
                {
                    return;
                }
                if (requireDeclaredRoot &&
                    !graph.isDeclaredSymbol(result.symbol()) &&
                    !result.isOutput())
                {
                    return;
                }
                const OperationId rootOpId = result.definingOp();
                if (!rootOpId.valid())
                {
                    return;
                }
                if (!seenRoots.insert(rootValueId).second)
                {
                    return;
                }
                const AnalyzedNode analyzed = analyzeValue(graph, rootValueId, state);
                if (!analyzed.valid || !analyzed.internal)
                {
                    return;
                }
                const std::size_t packedWidth =
                    static_cast<std::size_t>(result.width()) * options.minGroupSize;
                if (packedWidth < options.minPackedWidth ||
                    packedWidth > options.maxPackedWidth)
                {
                    return;
                }
                RootCandidate candidate;
                candidate.value = rootValueId;
                candidate.rootOp = rootOpId;
                candidate.anchorOp = anchorOpId;
                candidate.anchorIndex = opIndex.at(anchorOpId);
                candidate.signature = analyzed.signature;
                candidate.rootSource = std::string(rootSource);
                candidate.laneWidth = result.width();
                candidate.isSigned = result.isSigned();
                candidate.type = result.type();
                candidate.treeNodes = analyzed.nodeCount;
                buckets[candidate.signature].push_back(std::move(candidate));
            };
            for (OperationId opId : ops)
            {
                const Operation op = graph.getOperation(opId);
                if (options.enableDeclaredRoots && op.results().size() == 1)
                {
                    addCandidate(op.results()[0], opId, "declared", options.requireDeclaredRoots);
                }
                if (!options.enableStorageDataRoots)
                {
                    continue;
                }
                const auto dataOperandIndex = storageDataOperandIndex(op.kind());
                if (!dataOperandIndex)
                {
                    continue;
                }
                const auto operands = op.operands();
                if (*dataOperandIndex >= operands.size())
                {
                    continue;
                }
                addCandidate(operands[*dataOperandIndex], opId, "storage-data", false);
            }

            std::vector<CandidateGroup> groups;

            for (auto &[signature, candidates] : buckets)
            {
                (void)signature;
                std::sort(candidates.begin(), candidates.end(),
                          [](const RootCandidate &lhs, const RootCandidate &rhs) {
                              return lhs.anchorIndex < rhs.anchorIndex;
                          });
                std::size_t begin = 0;
                while (begin < candidates.size())
                {
                    std::size_t end = begin + 1;
                    while (end < candidates.size() &&
                           candidates[end].anchorIndex - candidates[end - 1].anchorIndex <= options.maxRootGap)
                    {
                        ++end;
                    }

                    std::size_t cursor = begin;
                    while (cursor < end)
                    {
                        bool accepted = false;
                        std::size_t limit = std::min<std::size_t>(end - cursor, options.maxGroupSize);
                        while (limit >= options.minGroupSize)
                        {
                            CandidateGroup group;
                            group.anchorIndex = candidates[cursor].anchorIndex;
                            group.roots.reserve(limit);
                            std::vector<ValueId> groupValues;
                            groupValues.reserve(limit);
                            for (std::size_t i = 0; i < limit; ++i)
                            {
                                group.roots.push_back(candidates[cursor + i]);
                                groupValues.push_back(candidates[cursor + i].value);
                            }
                            const std::size_t packedWidth = totalPackedWidth(groupValues, graph);
                            if (packedWidth >= options.minPackedWidth &&
                                packedWidth <= options.maxPackedWidth &&
                                !groupHasCrossRootDependency(graph, groupValues))
                            {
                                groups.push_back(std::move(group));
                                cursor += limit;
                                accepted = true;
                                break;
                            }
                            --limit;
                        }
                        if (!accepted)
                        {
                            ++cursor;
                        }
                    }

                    begin = end;
                }
            }

            std::sort(groups.begin(), groups.end(),
                      [](const CandidateGroup &lhs, const CandidateGroup &rhs) {
                          return lhs.anchorIndex < rhs.anchorIndex;
                      });
            return groups;
        }

        bool rewriteGroup(Graph &graph,
                          const CombLanePackOptions &options,
                          std::span<const RootCandidate> group,
                          std::optional<CombLanePackReport> &reportOut)
        {
            if (group.size() < options.minGroupSize)
            {
                return false;
            }
            std::vector<ValueId> groupValues;
            groupValues.reserve(group.size());
            for (const RootCandidate &candidate : group)
            {
                groupValues.push_back(candidate.value);
            }
            PackedBuilder builder(graph, options);
            auto packedRoot = builder.build(groupValues);
            if (!packedRoot)
            {
                return false;
            }

            const Value first = graph.getValue(group.front().value);
            const int64_t laneWidth = first.width();
            CombLanePackReport report;
            report.graphName = graph.symbol();
            report.rootSource = group.front().rootSource;
            report.groupSize = group.size();
            report.laneWidth = static_cast<std::size_t>(laneWidth);
            report.packedWidth = static_cast<std::size_t>(laneWidth) * group.size();
            report.treeNodes = group.front().treeNodes;
            report.rootKind =
                std::string(wolvrix::lib::grh::toString(graph.getOperation(group.front().rootOp).kind()));
            report.anchorKind =
                std::string(wolvrix::lib::grh::toString(graph.getOperation(group.front().anchorOp).kind()));
            report.signature = group.front().signature;
            report.packedRootValueId = packedRoot->index;
            report.rootSymbols.reserve(group.size());
            report.anchorSymbols.reserve(group.size());
            report.storageTargetSymbols.reserve(group.size());
            for (std::size_t lane = 0; lane < group.size(); ++lane)
            {
                const ValueId oldValueId = group[lane].value;
                const Value oldValue = graph.getValue(oldValueId);
                const Operation anchorOp = graph.getOperation(group[lane].anchorOp);
                const int64_t low = static_cast<int64_t>(lane) * laneWidth;
                const int64_t high = low + laneWidth - 1;
                const ValueId replacement =
                    createSliceStatic(graph,
                                      *packedRoot,
                                      low,
                                      high,
                                      oldValue.isSigned(),
                                      oldValue.type(),
                                      "pack-result-slice");
                if (oldValue.srcLoc())
                {
                    graph.setValueSrcLoc(replacement, *oldValue.srcLoc());
                }
                if (oldValue.symbol().valid())
                {
                    const auto oldSymbol = oldValue.symbol();
                    report.rootSymbols.push_back(std::string(graph.symbolText(oldSymbol)));
                    graph.setValueSymbol(oldValueId, graph.makeInternalValSym());
                    graph.setValueSymbol(replacement, oldSymbol);
                }
                else
                {
                    report.rootSymbols.push_back("value#" + std::to_string(oldValueId.index));
                }
                if (!anchorOp.symbolText().empty())
                {
                    report.anchorSymbols.push_back(std::string(anchorOp.symbolText()));
                }
                else
                {
                    report.anchorSymbols.push_back("op#" + std::to_string(group[lane].anchorOp.index));
                }
                if (anchorOp.kind() == OperationKind::kRegisterWritePort)
                {
                    if (const auto regSymbol = getStringAttr(anchorOp, "regSymbol"))
                    {
                        report.storageTargetSymbols.push_back(*regSymbol);
                    }
                    else
                    {
                        report.storageTargetSymbols.push_back({});
                    }
                }
                else if (anchorOp.kind() == OperationKind::kMemoryWritePort)
                {
                    if (const auto memSymbol = getStringAttr(anchorOp, "memSymbol"))
                    {
                        report.storageTargetSymbols.push_back(*memSymbol);
                    }
                    else
                    {
                        report.storageTargetSymbols.push_back({});
                    }
                }
                else
                {
                    report.storageTargetSymbols.push_back({});
                }
                rebindOutputPorts(graph, oldValueId, replacement);
                graph.replaceAllUses(oldValueId, replacement);
            }
            std::ostringstream description;
            description << "comb-lane-pack merged graph=" << report.graphName
                        << " source=" << report.rootSource
                        << " roots=" << report.groupSize
                        << " lane-width=" << report.laneWidth
                        << " packed-width=" << report.packedWidth
                        << " root-kind=" << report.rootKind
                        << " anchor-kind=" << report.anchorKind;
            report.description = description.str();
            reportOut = std::move(report);
            return true;
        }
    } // namespace

    CombLanePackPass::CombLanePackPass()
        : Pass("comb-lane-pack",
               "comb-lane-pack",
               "Pack repeated combinational sibling lanes into wider pointwise logic"),
          options_({})
    {
    }

    CombLanePackPass::CombLanePackPass(CombLanePackOptions options)
        : Pass("comb-lane-pack",
               "comb-lane-pack",
               "Pack repeated combinational sibling lanes into wider pointwise logic"),
          options_(options)
    {
    }

    PassResult CombLanePackPass::run()
    {
        PassResult result;
        if (options_.minGroupSize < 2)
        {
            error("comb-lane-pack min_group_size must be >= 2");
            result.failed = true;
            return result;
        }
        if (options_.maxGroupSize < options_.minGroupSize)
        {
            error("comb-lane-pack max_group_size must be >= min_group_size");
            result.failed = true;
            return result;
        }
        if (options_.maxPackedWidth < options_.minPackedWidth)
        {
            error("comb-lane-pack max_packed_width must be >= min_packed_width");
            result.failed = true;
            return result;
        }
        if (options_.maxTreeNodes == 0)
        {
            error("comb-lane-pack max_tree_nodes must be >= 1");
            result.failed = true;
            return result;
        }
        if (!options_.enableDeclaredRoots && !options_.enableStorageDataRoots)
        {
            error("comb-lane-pack requires at least one root source");
            result.failed = true;
            return result;
        }

        bool anyChanged = false;
        std::vector<CombLanePackReport> reports;
        for (const auto &entry : design().graphs())
        {
            Graph &graph = *entry.second;
            std::size_t groupsPacked = 0;
            const std::vector<CandidateGroup> groups = collectCandidateGroups(graph, options_);
            for (const CandidateGroup &group : groups)
            {
                std::optional<CombLanePackReport> report;
                if (!rewriteGroup(graph, options_, group.roots, report))
                {
                    continue;
                }
                anyChanged = true;
                ++groupsPacked;
                if (report)
                {
                    reports.push_back(std::move(*report));
                }
            }
            if (groupsPacked != 0)
            {
                info(graph, "comb-lane-pack packed groups=" + std::to_string(groupsPacked));
            }
        }

        if (anyChanged)
        {
            PassManagerOptions pmOptions;
            pmOptions.stopOnError = true;
            pmOptions.emitTiming = false;
            pmOptions.verbosity = verbosity();
            pmOptions.logLevel = LogLevel::Warn;
            pmOptions.keepDeclaredSymbols = keepDeclaredSymbols();
            pmOptions.logSink = [this](LogLevel level, std::string_view tag, std::string_view message) {
                this->log(level, tag, std::string(message));
            };
            PassManager cleanup(pmOptions);
            cleanup.addPass(std::make_unique<DeadCodeElimPass>());
            const PassManagerResult cleanupResult = cleanup.run(design(), diags());
            if (!cleanupResult.success)
            {
                result.failed = true;
                return result;
            }
        }

        result.changed = anyChanged;
        std::string summary = "comb-lane-pack summary groups=" + std::to_string(reports.size());
        if (!reports.empty())
        {
            std::size_t totalRoots = 0;
            std::size_t totalPackedBits = 0;
            for (const auto &report : reports)
            {
                totalRoots += report.groupSize;
                totalPackedBits += report.packedWidth;
            }
            summary.append(" roots=");
            summary.append(std::to_string(totalRoots));
            summary.append(" packed-width=");
            summary.append(std::to_string(totalPackedBits));
            info(summary);
        }
        logInfo(summary);
        if (!options_.outputKey.empty())
        {
            setSessionValue(options_.outputKey, std::move(reports), "comb-lane-pack.reports");
        }
        return result;
    }

} // namespace wolvrix::lib::transform
