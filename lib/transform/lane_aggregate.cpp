// lane-aggregate: re-vectorize firtool-flattened per-lane scalar registers.
//
// firtool flattens register arrays into per-lane scalar registers. Grouping
// masks every numeric name segment (`_<digits>` followed by `_`, `$`, or end)
// to `*`; within a masked group the lane segment is the one with the most
// distinct values and members are sub-grouped by the constant segments. Each
// lane register has exactly one write port and the per-lane write cones are
// structurally isomorphic, differing only in constants (affine in the lane
// index) and lane-relative register reads. Phase 1 merges a group of such
// lanes back into one wide kRegister of width span*W (lane i occupies
// [i*W +: W], lane 0 in the LSBs) with a single masked write port, and
// rewrites every read of a merged lane register to
// kSliceStatic(wide_read, i*W, W). Phase 2 (-read-select, on by default) then
// rewrites select trees over those slices (mux chains / one-hot and-or trees)
// into kSliceDynamic(wide_read, ptr*W, W). See docs/transform/lane-aggregate.md.
//
// Output modes (-output-mode, default wide):
//   wide  - the shape described above (unchanged default).
//   array - array-value shape: the merged storage is a kMemory (width = W,
//           row = span) with one kArrayReadAllPort as the packed read; the
//           write side is a single kArrayWritePort (laneMask = condVec &
//           presentLanes, data = the merged data cone); cone positions
//           materialize as kArrayBroadcast / kArrayLaneConst / kArrayOnehot /
//           kArrayMux (lane-pointwise bitwise ops widen unchanged; lane
//           parameters keep the per-lane kConcat); per-lane reductions
//           (kReduce{Or,And,Xor} over a multi-bit per-lane operand, including
//           the normalize-produced kArrayReduce* form) materialize as
//           kArrayReduceLanes{Or,And,Xor} over the packed rows, yielding the
//           per-lane guard vector; lane reads become
//           kMemoryReadPort at a constant address and phase 2 select trees
//           become one kMemoryReadPort at the dynamic pointer; the pre-pass
//           rewrites kReduce{Or,And,Xor}(kConcat(...)) to kArrayReduce* when
//           the concat elements have uniform width.
//
// Merge criteria (all must hold for a group, see docs/transform/lane-aggregate.md):
//   - name grouping, dense indices (few holes allowed), uniform lane width,
//     exactly one write port per lane, full-width (all-ones) write masks,
//     no initValue, no XMR reference, not a kept declared symbol;
//   - identical write-port event sets (eventEdge strings + event ValueIds);
//   - (updateCond, data) cones exactly structurally isomorphic. Structural
//     hashing is used only to bucket lanes; an exact N-wise comparison gates
//     the rewrite. Cone positions are classified as:
//       shared leaf   - all lanes reference the same ValueId (materialized
//                       as one kReplicate of the shared value);
//       constant leaf - per-lane kConstant values affine in the lane index
//                       (c_i = a*i + b, a == 0 allowed), materialized as one
//                       packed constant of width span*w (lane i's value in
//                       segment [i*w +: w], hole segments zero);
//       self read     - lane i reads this group's lane i register
//                       (materialized as the wide read);
//       sibling read  - lane i reads a sibling group's lane i register
//                       (materialized as the sibling wide read; the sibling
//                       group must merge with a superset lane set and equal
//                       span, otherwise this group is rejected);
//       shared reg    - every lane reads the same register R (resolved to a
//                       plain read of R, or to a slice of R's wide read when
//                       R is itself a merged lane);
//       internal node - lane-pointwise op (kAnd/kOr/kXor/kXnor/kNot/kAssign/
//                       kMux), widened to span*width. kMux is rebuilt as
//                       (t & m) | (f & ~m) with a per-lane broadcast select.
//                       Array mode additionally accepts per-lane reductions
//                       (kReduce{Or,And,Xor} over a multi-bit per-lane
//                       operand, or the normalize-produced kArrayReduce* form),
//                       materialized as kArrayReduceLanes{Or,And,Xor} over the
//                       packed rows, and treats a per-lane kConcat as a
//                       lane-parameter leaf (its materialized per-lane kConcat
//                       is already the packed-row form).
//       lane-param leaf - (-lane-param-leaves, on by default) any other
//                       lane-varying position whose per-lane values are
//                       produced by the same op kind with the same arity,
//                       attrs, and width across lanes (e.g. per-lane kEq wide
//                       compares, kSliceStatic/kSliceDynamic, kSub): the
//                       per-lane subgraphs are kept verbatim and packed by
//                       one per-lane kConcat (same materialization as the
//                       other lane-parameter leaves; register reads inside
//                       are retargeted by phase C3).
//       affine gather   - (R-level) per-lane kSliceStatic of ONE shared
//                       base X with offsets affine in the lane index (lane i
//                       reads X[base0 + i*W +: W]): the packed per-lane
//                       slices are exactly X[base0 +: span*W], so
//                       materialization is zero-cost (X itself on an exact
//                       fit, else one kSliceStatic). Non-affine offsets or
//                       mixed bases stay rejected.
//     Any other lane-varying position (mixed defined/undefined per-lane
//     leaves, cross-lane reads of another lane's register, or - with
//     -no-lane-param-leaves - non-pointwise ops such as kEq/kAdd) rejects
//     the group.
//
// Groups whose signature majority bucket is smaller than min_lanes normally
// stay scalar untouched; with -exact-fallback (on by default) the exact
// N-wise cone check then runs over ALL candidate lanes directly (one shot,
// then an incremental rescue-style bucket build capped at 32 attempts),
// because the signature is only a bucketing accelerator while the exact
// check is the ground truth. Lanes whose cones differ from the majority
// (e.g. lane 0 reset specialization) stay scalar untouched. Dead per-lane
// cone ops are left for dead-code-elim / simplify; this pass only removes
// the merged lane registers, their write ports, and the replaced read ports.

#include "transform/lane_aggregate.hpp"

#include "core/grh.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::SrcLoc;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;

        constexpr std::string_view kPassId = "lane-aggregate";

        template <typename T>
        std::optional<T> getAttr(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::optional<std::string> getStringAttr(const Operation &op, std::string_view key)
        {
            return getAttr<std::string>(op, key);
        }

        // ------------------------------------------------------------------
        // Lane name pattern: a numeric segment is `_<digits>` followed by
        // `_`, `$`, or end of string. ALL numeric segments of a register name
        // are masked to `*` to form the group key (e.g.
        // `enqPtrGenModule$enqPtrVec_7_value` -> `enqPtrGenModule$enqPtrVec_*_value`,
        // `robEntries_0_uopNum_T_2` -> `robEntries_*_uopNum_T_*`). Within one
        // masked group, exactly one segment position may vary across members;
        // that segment is the lane index.
        // ------------------------------------------------------------------
        struct LaneName
        {
            std::string maskedKey;
            std::vector<uint64_t> segmentValues;
        };

        std::optional<LaneName> parseLaneName(std::string_view name)
        {
            LaneName out;
            std::size_t cursor = 0;
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                if (name[i] != '_' || i + 1 >= name.size() ||
                    !std::isdigit(static_cast<unsigned char>(name[i + 1])))
                {
                    continue;
                }
                std::size_t j = i + 1;
                uint64_t value = 0;
                bool overflow = false;
                while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j])))
                {
                    const uint64_t digit = static_cast<uint64_t>(name[j] - '0');
                    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
                    {
                        overflow = true;
                        break;
                    }
                    value = value * 10 + digit;
                    ++j;
                }
                if (overflow)
                {
                    return std::nullopt;
                }
                if (j < name.size() && name[j] != '_' && name[j] != '$')
                {
                    continue;
                }
                out.maskedKey.append(name.substr(cursor, i - cursor));
                out.maskedKey.append("_*");
                cursor = j;
                out.segmentValues.push_back(value);
                i = j - 1;
            }
            if (out.segmentValues.empty())
            {
                return std::nullopt;
            }
            out.maskedKey.append(name.substr(cursor));
            return out;
        }

        // Render a masked group key for one sub-group: the lane segment stays
        // `_*`, every other segment is replaced by its (constant) value, e.g.
        // `data16_*$needCheck0Reg_*` + values (3, 7) + laneSeg 1 ->
        // `data16_3$needCheck0Reg_*`.
        std::string specializeMaskedKey(const std::string &maskedKey,
                                        const std::vector<uint64_t> &values,
                                        std::size_t laneSegment)
        {
            std::string out;
            std::size_t segment = 0;
            for (std::size_t p = 0; p < maskedKey.size();)
            {
                if (maskedKey.compare(p, 2, "_*") == 0)
                {
                    if (segment == laneSegment)
                    {
                        out.append("_*");
                    }
                    else
                    {
                        out.push_back('_');
                        out.append(std::to_string(values[segment]));
                    }
                    ++segment;
                    p += 2;
                    continue;
                }
                out.push_back(maskedKey[p++]);
            }
            return out;
        }

        // ------------------------------------------------------------------
        // Pre-pass normalization: kReduce{Or,And,Xor}(kConcat(e0..em)) is the
        // packed encoding of an element-wise reduction tree (the two forms
        // are semantically identical). Rewrite it into an explicit tree so
        // that signature bucketing and cone analysis see the pointwise form.
        // One pass over the op snapshot handles nesting in any order: inner
        // rewrites re-point the concat operands that outer matches consume.
        // ------------------------------------------------------------------
        bool isPackedReduceKind(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kReduceOr:
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceXor:
                return true;
            default:
                return false;
            }
        }

        OperationKind elementwiseKindForReduce(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kReduceOr:
                return OperationKind::kOr;
            case OperationKind::kReduceAnd:
                return OperationKind::kAnd;
            case OperationKind::kReduceXor:
            default:
                return OperationKind::kXor;
            }
        }

        OperationKind arrayReduceKindForReduce(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kReduceOr:
                return OperationKind::kArrayReduceOr;
            case OperationKind::kReduceAnd:
                return OperationKind::kArrayReduceAnd;
            case OperationKind::kReduceXor:
            default:
                return OperationKind::kArrayReduceXor;
            }
        }

        bool isReduceLikeKind(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kReduceOr:
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceXor:
            case OperationKind::kArrayReduceOr:
            case OperationKind::kArrayReduceAnd:
            case OperationKind::kArrayReduceXor:
                return true;
            default:
                return false;
            }
        }

        // Per-lane widening of a reduction: kReduce{Or,And,Xor} over a
        // multi-bit per-lane operand (and its normalize-produced
        // kArrayReduce{Or,And,Xor} form, whose operand is still the per-lane
        // value at analysis time) becomes a per-lane reduction over the
        // packed rows.
        OperationKind arrayReduceLanesKindForReduce(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kReduceOr:
            case OperationKind::kArrayReduceOr:
                return OperationKind::kArrayReduceLanesOr;
            case OperationKind::kReduceAnd:
            case OperationKind::kArrayReduceAnd:
                return OperationKind::kArrayReduceLanesAnd;
            case OperationKind::kReduceXor:
            case OperationKind::kArrayReduceXor:
            default:
                return OperationKind::kArrayReduceLanesXor;
            }
        }

        bool normalizeReduceConcat(Graph &graph, bool arrayMode)
        {
            bool changed = false;
            const std::vector<OperationId> ops(graph.operations().begin(), graph.operations().end());
            for (const OperationId opId : ops)
            {
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                if (!isPackedReduceKind(op.kind()) || op.operands().size() != 1 ||
                    op.results().size() != 1)
                {
                    continue;
                }
                const OperationId concatId = graph.valueDef(op.operands().front());
                if (!concatId.valid() || graph.getOperation(concatId).kind() != OperationKind::kConcat)
                {
                    continue;
                }
                const Operation concatOp = graph.getOperation(concatId);
                const OperationKind elementKind = elementwiseKindForReduce(op.kind());
                const ValueId oldResult = op.results().front();
                const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), "reduce-concat-normalize");
                if (arrayMode)
                {
                    // Array mode: kReduce{Or,And,Xor}(kConcat(e0..em)) over
                    // uniform-width elements is exactly one kArrayReduce* of
                    // the packed concat value (rows = operand count,
                    // elemWidth = element width). Non-uniform concats keep
                    // the tree expansion below.
                    int32_t elemWidth = 0;
                    bool uniform = !concatOp.operands().empty();
                    for (const ValueId element : concatOp.operands())
                    {
                        const int32_t width = graph.valueWidth(element);
                        if (elemWidth == 0)
                        {
                            elemWidth = width;
                        }
                        else if (width != elemWidth)
                        {
                            uniform = false;
                            break;
                        }
                    }
                    if (uniform && elemWidth > 0)
                    {
                        const ValueId out = graph.createValue(graph.makeInternalValSym(), 1, false,
                                                              graph.valueType(oldResult));
                        const OperationId arrayOp = graph.createOperation(
                            arrayReduceKindForReduce(op.kind()), graph.makeInternalOpSym());
                        graph.addOperand(arrayOp, op.operands().front());
                        graph.addResult(arrayOp, out);
                        graph.setAttr(arrayOp, "elemWidth", static_cast<int64_t>(elemWidth));
                        graph.setOpSrcLoc(arrayOp, srcLoc);
                        graph.setValueSrcLoc(out, srcLoc);
                        for (const auto &port : graph.outputPorts())
                        {
                            if (port.value == oldResult)
                            {
                                graph.bindOutputPort(port.name, out);
                            }
                        }
                        graph.replaceAllUses(oldResult, out);
                        changed = true;
                        // The old reduce stays behind dead; the concat stays
                        // live as the kArrayReduce* data operand.
                        continue;
                    }
                }
                ValueId root;
                for (const ValueId element : concatOp.operands())
                {
                    // A reduce over a 1-bit element is the element itself.
                    ValueId term = element;
                    if (graph.valueWidth(element) > 1)
                    {
                        term = graph.createValue(graph.makeInternalValSym(), 1, false,
                                                 graph.valueType(element));
                        const OperationId termOp =
                            graph.createOperation(op.kind(), graph.makeInternalOpSym());
                        graph.addOperand(termOp, element);
                        graph.addResult(termOp, term);
                        graph.setOpSrcLoc(termOp, srcLoc);
                        graph.setValueSrcLoc(term, srcLoc);
                    }
                    if (!root.valid())
                    {
                        root = term;
                        continue;
                    }
                    const ValueId next = graph.createValue(graph.makeInternalValSym(), 1, false,
                                                           graph.valueType(oldResult));
                    const OperationId nextOp =
                        graph.createOperation(elementKind, graph.makeInternalOpSym());
                    graph.addOperand(nextOp, root);
                    graph.addOperand(nextOp, term);
                    graph.addResult(nextOp, next);
                    graph.setOpSrcLoc(nextOp, srcLoc);
                    graph.setValueSrcLoc(next, srcLoc);
                    root = next;
                }
                if (!root.valid())
                {
                    continue;
                }
                for (const auto &port : graph.outputPorts())
                {
                    if (port.value == oldResult)
                    {
                        graph.bindOutputPort(port.name, root);
                    }
                }
                graph.replaceAllUses(oldResult, root);
                changed = true;
                // The old reduce/concat stay behind dead for dead-code-elim.
            }
            return changed;
        }

        // ------------------------------------------------------------------
        // Per-graph indexes built from a single op scan.
        // ------------------------------------------------------------------
        struct WritePortInfo
        {
            OperationId op;
            ValueId updateCond;
            ValueId nextValue;
            ValueId mask;
            std::vector<ValueId> events;
            std::vector<std::string> eventEdges;
        };

        struct GraphIndexes
        {
            std::unordered_map<std::string, OperationId> regOps;
            std::unordered_map<std::string, int64_t> regWidths;
            std::unordered_map<std::string, bool> regSigned;
            std::unordered_map<std::string, std::string> regInitValues;
            std::unordered_map<std::string, std::vector<WritePortInfo>> writesByReg;
            std::unordered_map<std::string, std::vector<OperationId>> readsByReg;
            std::unordered_set<std::string> xmrReferencedNames;
        };

        GraphIndexes buildGraphIndexes(const Graph &graph)
        {
            GraphIndexes index;
            for (OperationId opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                switch (op.kind())
                {
                case OperationKind::kRegister:
                {
                    const std::string name(op.symbolText());
                    if (name.empty())
                    {
                        break;
                    }
                    index.regOps.emplace(name, opId);
                    if (const auto width = getAttr<int64_t>(op, "width"))
                    {
                        index.regWidths.emplace(name, *width);
                    }
                    if (const auto isSigned = getAttr<bool>(op, "isSigned"))
                    {
                        index.regSigned.emplace(name, *isSigned);
                    }
                    if (const auto initValue = getStringAttr(op, "initValue"))
                    {
                        index.regInitValues.emplace(name, *initValue);
                    }
                    break;
                }
                case OperationKind::kRegisterWritePort:
                {
                    const auto regSymbol = getStringAttr(op, "regSymbol");
                    if (!regSymbol)
                    {
                        break;
                    }
                    WritePortInfo info;
                    info.op = opId;
                    const auto operands = op.operands();
                    if (operands.size() >= 3)
                    {
                        info.updateCond = operands[0];
                        info.nextValue = operands[1];
                        info.mask = operands[2];
                        info.events.assign(operands.begin() + 3, operands.end());
                    }
                    if (const auto edges = getAttr<std::vector<std::string>>(op, "eventEdge"))
                    {
                        info.eventEdges = *edges;
                    }
                    index.writesByReg[*regSymbol].push_back(std::move(info));
                    break;
                }
                case OperationKind::kRegisterReadPort:
                {
                    const auto regSymbol = getStringAttr(op, "regSymbol");
                    if (regSymbol)
                    {
                        index.readsByReg[*regSymbol].push_back(opId);
                    }
                    break;
                }
                case OperationKind::kXMRRead:
                case OperationKind::kXMRWrite:
                {
                    const auto path = getStringAttr(op, "xmrPath");
                    if (!path)
                    {
                        break;
                    }
                    const std::size_t dot = path->find_last_of('.');
                    index.xmrReferencedNames.insert(
                        dot == std::string::npos ? *path : path->substr(dot + 1));
                    break;
                }
                default:
                    break;
                }
            }
            return index;
        }

        // ------------------------------------------------------------------
        // Constants.
        // ------------------------------------------------------------------
        std::optional<uint64_t> parseLiteralUInt64(const std::string &literal, int32_t width,
                                                   int32_t *widthOut = nullptr)
        {
            slang::SVInt parsed;
            try
            {
                parsed = slang::SVInt::fromString(literal);
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
            if (parsed.hasUnknown() || width <= 0 || width > 64)
            {
                return std::nullopt;
            }
            parsed = parsed.resize(static_cast<slang::bitwidth_t>(width));
            if (widthOut != nullptr)
            {
                *widthOut = width;
            }
            return static_cast<uint64_t>(*parsed.getRawPtr());
        }

        std::optional<uint64_t> getConstantUInt64(const Graph &graph, ValueId value, int32_t *widthOut)
        {
            if (!value.valid())
            {
                return std::nullopt;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kConstant)
            {
                return std::nullopt;
            }
            const auto literal = getStringAttr(defOp, "constValue");
            if (!literal)
            {
                return std::nullopt;
            }
            return parseLiteralUInt64(*literal, std::max<int32_t>(graph.valueWidth(value), 1), widthOut);
        }

        std::string makeHexLiteral(int32_t width, const slang::SVInt &value)
        {
            const int32_t normalizedWidth = width > 0 ? width : 1;
            return value.toString(slang::LiteralBase::Hex, true,
                                  static_cast<slang::bitwidth_t>(normalizedWidth));
        }

        // ------------------------------------------------------------------
        // Signature hashing (bucketing only; the exact check gates rewrites).
        // Constants are abstracted, register reads are lane-relative.
        // ------------------------------------------------------------------
        constexpr uint64_t kHashMask = (UINT64_C(1) << 61) - 1;

        uint64_t hashMix(uint64_t acc, uint64_t value)
        {
            return ((acc * 1000003) ^ (value & kHashMask)) & kHashMask;
        }

        uint64_t hashString(std::string_view text)
        {
            uint64_t h = 0;
            for (const char ch : text)
            {
                h = (h * 131 + static_cast<unsigned char>(ch)) & kHashMask;
            }
            return h;
        }

        using RegLaneMap = std::unordered_map<std::string, std::pair<std::string, uint64_t>>;

        struct LaneSignatureContext
        {
            const Graph *graph = nullptr;
            const RegLaneMap *regLane = nullptr;
            std::string selfGroupKey;
            uint64_t selfIndex = 0;
            std::unordered_map<ValueId, uint64_t, ValueIdHash> memo;
            std::unordered_set<ValueId, ValueIdHash> visiting;
        };

        uint64_t laneValueSignature(LaneSignatureContext &ctx, ValueId root)
        {
            // Iterative post-order over the value DAG; memoized per value.
            std::vector<std::pair<ValueId, bool>> stack;
            stack.emplace_back(root, false);
            while (!stack.empty())
            {
                auto &[value, expanded] = stack.back();
                if (!value.valid())
                {
                    stack.pop_back();
                    continue;
                }
                const auto memoIt = ctx.memo.find(value);
                if (memoIt != ctx.memo.end())
                {
                    stack.pop_back();
                    continue;
                }
                const OperationId defOpId = ctx.graph->valueDef(value);
                if (!expanded && defOpId.valid())
                {
                    const Operation defOp = ctx.graph->getOperation(defOpId);
                    if (defOp.kind() != OperationKind::kConstant &&
                        defOp.kind() != OperationKind::kRegisterReadPort &&
                        !defOp.operands().empty())
                    {
                        expanded = true;
                        ctx.visiting.insert(value);
                        for (const ValueId operand : defOp.operands())
                        {
                            // Skip operands currently on the stack: the hash is
                            // bucketing-only, so comb cycles just mix a zero.
                            if (operand.valid() && ctx.memo.find(operand) == ctx.memo.end() &&
                                ctx.visiting.find(operand) == ctx.visiting.end())
                            {
                                stack.emplace_back(operand, false);
                            }
                        }
                        continue;
                    }
                }
                stack.pop_back();
                ctx.visiting.erase(value);
                uint64_t h = 0x345678;
                if (!defOpId.valid())
                {
                    // Leaf without a defining op (input ports, wires): the
                    // exact check treats per-lane-distinct bare values as
                    // lane-parameter leaves, so the hash must not split on
                    // identity.
                    h = hashMix(h, 11);
                }
                else
                {
                    const Operation defOp = ctx.graph->getOperation(defOpId);
                    const OperationKind kind = defOp.kind();
                    if (kind == OperationKind::kConstant)
                    {
                        h = hashMix(h, 7);
                        h = hashMix(h, static_cast<uint64_t>(std::max<int32_t>(ctx.graph->valueWidth(value), 1)));
                    }
                    else if (kind == OperationKind::kRegisterReadPort)
                    {
                        const auto regSymbol = getStringAttr(defOp, "regSymbol");
                        const auto laneIt = regSymbol ? ctx.regLane->find(*regSymbol) : ctx.regLane->end();
                        if (laneIt != ctx.regLane->end() &&
                            laneIt->second.first == ctx.selfGroupKey &&
                            laneIt->second.second == ctx.selfIndex)
                        {
                            h = hashMix(h, 51); // lane-self read
                        }
                        else if (laneIt != ctx.regLane->end() &&
                                 laneIt->second.second == ctx.selfIndex)
                        {
                            h = hashMix(h, 53); // sibling-group read at the same index
                            h = hashMix(h, hashString(laneIt->second.first));
                        }
                        else if (laneIt != ctx.regLane->end())
                        {
                            // Group read at an absolute index (shared across
                            // lanes when every lane reads the same index).
                            h = hashMix(h, 52);
                            h = hashMix(h, hashString(laneIt->second.first));
                            h = hashMix(h, laneIt->second.second);
                        }
                        else if (regSymbol)
                        {
                            // Register without a lane-group declaration
                            // (e.g. undeclared io_out_<i> reads): normalize by
                            // parsed name so same-index families bucket
                            // together; the exact check materializes these as
                            // lane-parameter concats.
                            const auto parsed = parseLaneName(*regSymbol);
                            if (parsed &&
                                std::find(parsed->segmentValues.begin(), parsed->segmentValues.end(),
                                          ctx.selfIndex) != parsed->segmentValues.end())
                            {
                                h = hashMix(h, 53);
                                h = hashMix(h, hashString(parsed->maskedKey));
                            }
                            else if (parsed)
                            {
                                h = hashMix(h, 52);
                                h = hashMix(h, hashString(parsed->maskedKey));
                                h = hashMix(h, parsed->segmentValues.front());
                            }
                            else
                            {
                                h = hashMix(h, 22);
                                h = hashMix(h, hashString(*regSymbol));
                            }
                        }
                        else
                        {
                            h = hashMix(h, 22);
                        }
                    }
                    else if (defOp.operands().empty())
                    {
                        h = hashMix(h, 23);
                        h = hashMix(h, static_cast<uint64_t>(kind));
                        h = hashMix(h, value.index);
                    }
                    else
                    {
                        h = hashMix(h, 31);
                        h = hashMix(h, static_cast<uint64_t>(kind));
                        h = hashMix(h, static_cast<uint64_t>(std::max<int32_t>(ctx.graph->valueWidth(value), 1)));
                        for (const ValueId operand : defOp.operands())
                        {
                            const auto it = ctx.memo.find(operand);
                            h = hashMix(h, it != ctx.memo.end() ? it->second : 0);
                        }
                    }
                }
                ctx.memo.emplace(value, h);
            }
            const auto it = ctx.memo.find(root);
            return it != ctx.memo.end() ? it->second : 0;
        }

        uint64_t laneEventSignature(const WritePortInfo &write)
        {
            uint64_t h = 0x1234567;
            for (const std::string &edge : write.eventEdges)
            {
                h = hashMix(h, hashString(edge));
            }
            for (const ValueId event : write.events)
            {
                h = hashMix(h, event.valid() ? event.index : 0);
            }
            return h;
        }
    } // namespace

} // namespace wolvrix::lib::transform

namespace wolvrix::lib::transform
{

    namespace
    {
        bool isAllOnesConstant(const Graph &graph, ValueId value, int32_t width)
        {
            if (width <= 0 || !value.valid())
            {
                return false;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return false;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kConstant)
            {
                return false;
            }
            const auto literal = getStringAttr(defOp, "constValue");
            if (!literal)
            {
                return false;
            }
            slang::SVInt parsed;
            try
            {
                parsed = slang::SVInt::fromString(*literal);
            }
            catch (const std::exception &)
            {
                return false;
            }
            if (parsed.hasUnknown())
            {
                return false;
            }
            parsed = parsed.resize(static_cast<slang::bitwidth_t>(width));
            const std::size_t wordCount = static_cast<std::size_t>((width + 63) / 64);
            const std::uint64_t *raw = parsed.getRawPtr();
            for (std::size_t i = 0; i < wordCount; ++i)
            {
                const std::size_t bits = (i + 1u == wordCount) ? static_cast<std::size_t>(width) - i * 64u : 64u;
                const std::uint64_t expected = bits >= 64u ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - 1u);
                if (raw[i] != expected)
                {
                    return false;
                }
            }
            return true;
        }

        bool opAttrsEqual(const Operation &lhs, const Operation &rhs)
        {
            std::vector<AttrKV> lhsAttrs(lhs.attrs().begin(), lhs.attrs().end());
            std::vector<AttrKV> rhsAttrs(rhs.attrs().begin(), rhs.attrs().end());
            auto byKey = [](const AttrKV &a, const AttrKV &b) { return a.key < b.key; };
            std::sort(lhsAttrs.begin(), lhsAttrs.end(), byKey);
            std::sort(rhsAttrs.begin(), rhsAttrs.end(), byKey);
            if (lhsAttrs.size() != rhsAttrs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhsAttrs.size(); ++i)
            {
                if (lhsAttrs[i].key != rhsAttrs[i].key || !(lhsAttrs[i].value == rhsAttrs[i].value))
                {
                    return false;
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Cone analysis: exact N-wise structural comparison of the per-lane
        // (updateCond, data) cones. Constants must be affine in the lane
        // index; only lane-pointwise ops are widened; lane-relative register
        // reads become parameters. Hashing is never used here.
        // ------------------------------------------------------------------
        enum class PosKind
        {
            kShared,
            kConstant,
            kSelfRead,
            kSiblingRead,
            kSharedRegRead,
            kLaneParam,
            kEqOnehot,
            kAffineGather,
            kInternal,
        };

        struct ConePosition
        {
            ValueId templateValue;
            PosKind kind = PosKind::kShared;
            std::vector<ValueId> tuple;
            int32_t laneWidth = 0;
            bool isSigned = false;
            ValueType valueType = ValueType::Logic;
            // kConstant: per-lane values (parallel to the lane list).
            std::vector<uint64_t> constValues;
            // kSiblingRead: sibling lane-group key; kSharedRegRead: reg symbol.
            std::string refGroupKey;
            std::string refRegSymbol;
            // kEqOnehot: the shared compared value x (result bit i = (x == i)).
            ValueId sharedX;
            // kAffineGather: the shared gathered base X and the offset of
            // lane 0's segment (lane i reads X[gatherBase + i*W +: W]).
            ValueId gatherX;
            uint64_t gatherBase = 0;
            // kInternal.
            OperationKind opKind = OperationKind::kAssign;
            std::vector<int32_t> children;
        };

        struct ConeAnalysis
        {
            bool ok = false;
            std::string rejectReason;
            std::string rejectDetail;
            std::vector<ConePosition> positions;
            int32_t condRoot = -1;
            int32_t dataRoot = -1;
            std::vector<std::string> siblingDeps;
        };

        bool checkAffineConstants(const std::vector<uint64_t> &values,
                                  const std::vector<uint64_t> &indices)
        {
            if (values.size() < 2)
            {
                return true;
            }
            const __int128 c0 = static_cast<__int128>(values[0]);
            const __int128 dc = static_cast<__int128>(values[1]) - c0;
            const __int128 di = static_cast<__int128>(indices[1] - indices[0]);
            if (di <= 0 || dc % di != 0)
            {
                return false;
            }
            const __int128 a = dc / di;
            for (std::size_t k = 1; k < values.size(); ++k)
            {
                const __int128 expect = c0 + a * static_cast<__int128>(indices[k] - indices[0]);
                if (static_cast<__int128>(values[k]) != expect)
                {
                    return false;
                }
            }
            return true;
        }

        bool isPointwiseKind(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
            case OperationKind::kNot:
            case OperationKind::kAssign:
            case OperationKind::kMux:
            // 1-bit logical ops are bitwise ops there; the operand-shape
            // check rejects any multi-bit use, and materialization maps them
            // back to kAnd/kOr/kNot.
            case OperationKind::kLogicAnd:
            case OperationKind::kLogicOr:
            case OperationKind::kLogicNot:
            // kReplicate of a per-lane 1-bit value is a per-lane broadcast
            // (materialized via broadcastLaneBits); kReduce{Or,And,Xor} of a
            // 1-bit operand is the identity. Both shapes are verified in
            // classifyInternal. A multi-bit reduction is a per-lane
            // reduction, materialized as kArrayReduceLanes* in array mode
            // (also verified in classifyInternal).
            case OperationKind::kReplicate:
            case OperationKind::kReduceOr:
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceXor:
            // kArrayReduce{Or,And,Xor} reaches cones as the array-mode
            // normalize product of kReduce{Or,And,Xor}(kConcat(...)); its
            // operand is still the per-lane value here, so it is a per-lane
            // reduction with the same widening rules as kReduce*.
            case OperationKind::kArrayReduceOr:
            case OperationKind::kArrayReduceAnd:
            case OperationKind::kArrayReduceXor:
                return true;
            default:
                return false;
            }
        }

        OperationKind materializedPointwiseKind(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kLogicAnd:
                return OperationKind::kAnd;
            case OperationKind::kLogicOr:
                return OperationKind::kOr;
            case OperationKind::kLogicNot:
                return OperationKind::kNot;
            default:
                return kind;
            }
        }

        class ConeAnalyzer
        {
        public:
            ConeAnalyzer(const Graph &graph,
                         std::string selfGroupKey,
                         const std::vector<uint64_t> &laneIndices,
                         const std::vector<std::string> &laneNames,
                         const RegLaneMap &regLane,
                         bool arrayMode,
                         bool laneParamLeaves)
                : graph_(graph),
                  selfGroupKey_(std::move(selfGroupKey)),
                  laneIndices_(laneIndices),
                  laneNames_(laneNames),
                  regLane_(regLane),
                  arrayMode_(arrayMode),
                  laneParamLeaves_(laneParamLeaves)
            {
            }

            bool analyze(std::span<const ValueId> condTuple,
                         std::span<const ValueId> dataTuple,
                         ConeAnalysis &out)
            {
                out_ = &out;
                const int32_t cond = visit(condTuple);
                if (cond < 0)
                {
                    return false;
                }
                const int32_t data = visit(dataTuple);
                if (data < 0)
                {
                    return false;
                }
                out.condRoot = cond;
                out.dataRoot = data;
                out.ok = true;
                return true;
            }

        private:
            bool reject(std::string_view reason, std::string detail)
            {
                if (out_ && out_->rejectReason.empty())
                {
                    out_->rejectReason = std::string(reason);
                    out_->rejectDetail = std::move(detail);
                }
                failed_ = true;
                return false;
            }

            bool sameTupleValues(std::span<const ValueId> tuple, int32_t &width, bool &isSigned, ValueType &type)
            {
                const ValueId first = tuple.front();
                if (!first.valid())
                {
                    return false;
                }
                width = graph_.valueWidth(first);
                isSigned = graph_.valueSigned(first);
                type = graph_.valueType(first);
                for (const ValueId value : tuple)
                {
                    if (!value.valid() || graph_.valueWidth(value) != width ||
                        graph_.valueSigned(value) != isSigned || graph_.valueType(value) != type)
                    {
                        return false;
                    }
                }
                return true;
            }

            int32_t emitPosition(ConePosition pos)
            {
                const int32_t index = static_cast<int32_t>(out_->positions.size());
                memo_.emplace(pos.templateValue, index);
                out_->positions.push_back(std::move(pos));
                return index;
            }

            int32_t visit(std::span<const ValueId> tuple)
            {
                if (failed_ || tuple.size() != laneIndices_.size())
                {
                    return -1;
                }
                const ValueId t0 = tuple.front();
                if (const auto it = memo_.find(t0); it != memo_.end())
                {
                    const ConePosition &existing = out_->positions[static_cast<std::size_t>(it->second)];
                    if (existing.tuple.size() != tuple.size() ||
                        !std::equal(existing.tuple.begin(), existing.tuple.end(), tuple.begin()))
                    {
                        reject("shared_subtree_divergence",
                               "template value maps to different per-lane tuples");
                        return -1;
                    }
                    return it->second;
                }

                int32_t laneWidth = 0;
                bool laneSigned = false;
                ValueType laneType = ValueType::Logic;
                if (!sameTupleValues(tuple, laneWidth, laneSigned, laneType))
                {
                    reject("structure_mismatch", "lane value type/width mismatch");
                    return -1;
                }

                bool allSame = true;
                bool allConstant = true;
                bool allRegRead = true;
                for (const ValueId value : tuple)
                {
                    if (value != t0)
                    {
                        allSame = false;
                    }
                    const OperationId defOpId = graph_.valueDef(value);
                    if (!defOpId.valid())
                    {
                        allConstant = false;
                        allRegRead = false;
                        continue;
                    }
                    const Operation defOp = graph_.getOperation(defOpId);
                    if (defOp.kind() != OperationKind::kConstant)
                    {
                        allConstant = false;
                    }
                    if (defOp.kind() != OperationKind::kRegisterReadPort)
                    {
                        allRegRead = false;
                    }
                }

                ConePosition pos;
                pos.templateValue = t0;
                pos.tuple.assign(tuple.begin(), tuple.end());
                pos.laneWidth = laneWidth;
                pos.isSigned = laneSigned;
                pos.valueType = laneType;

                if (allSame)
                {
                    pos.kind = PosKind::kShared;
                    return emitPosition(std::move(pos));
                }

                if (allConstant)
                {
                    pos.kind = PosKind::kConstant;
                    pos.constValues.reserve(tuple.size());
                    for (const ValueId value : tuple)
                    {
                        const auto parsed = getConstantUInt64(graph_, value, nullptr);
                        if (!parsed)
                        {
                            reject("constant_not_supported",
                                   "constant has unknown bits or width > 64");
                            return -1;
                        }
                        pos.constValues.push_back(*parsed);
                    }
                    if (!checkAffineConstants(pos.constValues, laneIndices_))
                    {
                        reject("non_affine_constant", "per-lane constants are not affine in the lane index");
                        return -1;
                    }
                    return emitPosition(std::move(pos));
                }

                if (allRegRead)
                {
                    return classifyRegisterReads(pos);
                }

                return classifyInternal(std::move(pos));
            }

            int32_t classifyRegisterReads(ConePosition &pos)
            {
                bool allSelf = true;
                bool allSibling = true;
                bool allSameReg = true;
                std::string siblingKey;
                std::string sameReg;
                for (std::size_t k = 0; k < pos.tuple.size(); ++k)
                {
                    const Operation defOp = graph_.getOperation(graph_.valueDef(pos.tuple[k]));
                    const auto regSymbol = getStringAttr(defOp, "regSymbol");
                    if (!regSymbol)
                    {
                        reject("structure_mismatch", "kRegisterReadPort missing regSymbol");
                        return -1;
                    }
                    if (*regSymbol != laneNames_[k])
                    {
                        allSelf = false;
                    }
                    const auto laneIt = regLane_.find(*regSymbol);
                    const bool isSibling = laneIt != regLane_.end() &&
                                           laneIt->second.first != selfGroupKey_ &&
                                           laneIt->second.second == laneIndices_[k];
                    if (!isSibling)
                    {
                        allSibling = false;
                    }
                    else if (siblingKey.empty())
                    {
                        siblingKey = laneIt->second.first;
                    }
                    else if (siblingKey != laneIt->second.first)
                    {
                        allSibling = false;
                    }
                    if (sameReg.empty())
                    {
                        sameReg = *regSymbol;
                    }
                    else if (sameReg != *regSymbol)
                    {
                        allSameReg = false;
                    }
                    if (!allSelf && !allSibling && !allSameReg)
                    {
                        break;
                    }
                }
                if (allSelf)
                {
                    pos.kind = PosKind::kSelfRead;
                    return emitPosition(std::move(pos));
                }
                if (allSibling)
                {
                    pos.kind = PosKind::kSiblingRead;
                    pos.refGroupKey = siblingKey;
                    if (out_)
                    {
                        out_->siblingDeps.push_back(siblingKey);
                    }
                    return emitPosition(std::move(pos));
                }
                if (allSameReg)
                {
                    // Every lane reads the same register: a lane-invariant
                    // shared read. If that register is itself a merged lane
                    // (of this or another group), materialization resolves it
                    // to a slice of the wide read; otherwise to a plain read.
                    pos.kind = PosKind::kSharedRegRead;
                    pos.refRegSymbol = sameReg;
                    return emitPosition(std::move(pos));
                }
                // Lane-varying register reads that are neither self, sibling,
                // nor one shared register. Cross-lane reads of THIS group stay
                // rejected (shift-register shapes); every other register-read
                // tuple becomes a lane-parameter leaf materialized as a
                // per-lane kConcat (e.g. reads of dispatch-port registers at
                // another index, or of undeclared io_out_<i> registers).
                for (std::size_t k = 0; k < pos.tuple.size(); ++k)
                {
                    const Operation defOp = graph_.getOperation(graph_.valueDef(pos.tuple[k]));
                    const auto regSymbol = getStringAttr(defOp, "regSymbol");
                    const auto laneIt = regSymbol ? regLane_.find(*regSymbol) : regLane_.end();
                    if (laneIt != regLane_.end() && laneIt->second.first == selfGroupKey_)
                    {
                        reject("cross_lane_read",
                               "lane " + std::to_string(laneIndices_[k]) + " reads lane " +
                                   std::to_string(laneIt->second.second));
                        return -1;
                    }
                }
                pos.kind = PosKind::kLaneParam;
                return emitPosition(std::move(pos));
            }

            // R-level affine-gather leaf: every lane's value is a kSliceStatic
            // of the SAME shared base X with the offset affine in the lane
            // index (lane i reads X[base0 + i*W +: W], W = lane width). The
            // packed per-lane concatenation of such slices is exactly
            // X[base0 +: span*W] (segments of hole lanes carry X's bits,
            // which are don't-care: the present-lanes cond mask clears them
            // like any other shared leaf), so materialization is zero-cost.
            // Runs before the attrs check because sliceStart/sliceEnd
            // legitimately differ per lane. Mixed bases, non-affine offsets,
            // negative bases, and out-of-range segments fall through to the
            // normal (rejecting) path.
            bool classifyAffineGather(ConePosition &pos)
            {
                const OperationId t0DefId = graph_.valueDef(pos.templateValue);
                const Operation t0Def = graph_.getOperation(t0DefId);
                if (t0Def.kind() != OperationKind::kSliceStatic || t0Def.operands().size() != 1)
                {
                    return false;
                }
                const ValueId base = t0Def.operands().front();
                const int64_t baseWidth = std::max<int32_t>(graph_.valueWidth(base), 0);
                std::optional<__int128> gatherBase;
                for (std::size_t k = 0; k < pos.tuple.size(); ++k)
                {
                    const OperationId defId = graph_.valueDef(pos.tuple[k]);
                    if (!defId.valid())
                    {
                        return false;
                    }
                    const Operation def = graph_.getOperation(defId);
                    if (def.kind() != OperationKind::kSliceStatic || def.operands().size() != 1 ||
                        def.operands().front() != base)
                    {
                        return false;
                    }
                    const auto sliceStart = getAttr<int64_t>(def, "sliceStart");
                    const auto sliceEnd = getAttr<int64_t>(def, "sliceEnd");
                    if (!sliceStart || !sliceEnd || *sliceStart < 0 || *sliceEnd < *sliceStart ||
                        *sliceEnd - *sliceStart + 1 != pos.laneWidth || *sliceEnd >= baseWidth)
                    {
                        return false;
                    }
                    const __int128 offset = static_cast<__int128>(*sliceStart) -
                                            static_cast<__int128>(laneIndices_[k]) *
                                                static_cast<__int128>(pos.laneWidth);
                    if (!gatherBase)
                    {
                        gatherBase = offset;
                    }
                    else if (*gatherBase != offset)
                    {
                        return false;
                    }
                }
                if (!gatherBase || *gatherBase < 0)
                {
                    return false;
                }
                pos.kind = PosKind::kAffineGather;
                pos.gatherX = base;
                pos.gatherBase = static_cast<uint64_t>(*gatherBase);
                return true;
            }

            int32_t classifyInternal(ConePosition pos)
            {
                const OperationId t0DefId = graph_.valueDef(pos.templateValue);
                if (!t0DefId.valid())
                {
                    // Per-lane bare values (input ports, _GEN_N/_val_N wires):
                    // a lane-parameter leaf materialized as a per-lane
                    // kConcat. Mixed defined/undefined tuples stay rejected.
                    bool anyDef = false;
                    for (const ValueId value : pos.tuple)
                    {
                        if (graph_.valueDef(value).valid())
                        {
                            anyDef = true;
                            break;
                        }
                    }
                    if (anyDef)
                    {
                        reject("lane_varying_leaf", "mixed lane-varying leaf");
                        return -1;
                    }
                    pos.kind = PosKind::kLaneParam;
                    return emitPosition(std::move(pos));
                }
                const Operation t0Def = graph_.getOperation(t0DefId);
                const OperationKind kind = t0Def.kind();
                const std::size_t operandCount = t0Def.operands().size();
                if ((kind == OperationKind::kSliceStatic || kind == OperationKind::kSliceDynamic) &&
                    pos.laneWidth == 1)
                {
                    // shl-onehot cone: per lane k, bit laneIndices_[k] of a
                    // shared (1 << x) decode, written as kSliceStatic(X, i, i)
                    // or kSliceDynamic(X, const i, 1) with
                    // X = kShl(kConstant 1, x). This is the DataModule-family
                    // write/read one-hot decode; bit i of (1 << x) is
                    // (x == i) in 2-state semantics (same equivalence class
                    // as onehot-to-mux's x-prop note), so the position
                    // classifies exactly like the kEq onehot below. The two
                    // slice forms (and mixed tuples of them) are accepted;
                    // kSliceStatic sliceStart/sliceEnd legitimately differ
                    // per lane, so this runs before the attrs check.
                    ValueId sharedBase;
                    bool onehot = true;
                    for (std::size_t k = 0; k < pos.tuple.size(); ++k)
                    {
                        const OperationId defId = graph_.valueDef(pos.tuple[k]);
                        if (!defId.valid())
                        {
                            onehot = false;
                            break;
                        }
                        const Operation def = graph_.getOperation(defId);
                        uint64_t bitIndex = 0;
                        ValueId base;
                        if (def.kind() == OperationKind::kSliceStatic && def.operands().size() == 1)
                        {
                            const auto sliceStart = getAttr<int64_t>(def, "sliceStart");
                            const auto sliceEnd = getAttr<int64_t>(def, "sliceEnd");
                            if (!sliceStart || !sliceEnd || *sliceStart != *sliceEnd || *sliceStart < 0)
                            {
                                onehot = false;
                                break;
                            }
                            bitIndex = static_cast<uint64_t>(*sliceStart);
                            base = def.operands().front();
                        }
                        else if (def.kind() == OperationKind::kSliceDynamic && def.operands().size() == 2)
                        {
                            const auto sliceWidth = getAttr<int64_t>(def, "sliceWidth");
                            if (!sliceWidth || *sliceWidth != 1)
                            {
                                onehot = false;
                                break;
                            }
                            const auto offset = getConstantUInt64(graph_, def.operands()[1], nullptr);
                            if (!offset)
                            {
                                onehot = false;
                                break;
                            }
                            bitIndex = *offset;
                            base = def.operands()[0];
                        }
                        else
                        {
                            onehot = false;
                            break;
                        }
                        // The equivalence bit i of (1 << x) == (x == i) needs
                        // i < width(X); an out-of-range bit select is not a
                        // onehot decode.
                        if (bitIndex != laneIndices_[k] ||
                            bitIndex >= static_cast<uint64_t>(std::max<int32_t>(graph_.valueWidth(base), 0)))
                        {
                            onehot = false;
                            break;
                        }
                        if (k == 0)
                        {
                            sharedBase = base;
                        }
                        else if (base != sharedBase)
                        {
                            onehot = false;
                            break;
                        }
                    }
                    if (onehot && sharedBase.valid())
                    {
                        const OperationId baseDefId = graph_.valueDef(sharedBase);
                        if (baseDefId.valid())
                        {
                            const Operation baseDef = graph_.getOperation(baseDefId);
                            if (baseDef.kind() == OperationKind::kShl && baseDef.operands().size() == 2)
                            {
                                const auto one = getConstantUInt64(graph_, baseDef.operands()[0], nullptr);
                                if (one && *one == 1 &&
                                    !getConstantUInt64(graph_, baseDef.operands()[1], nullptr))
                                {
                                    pos.kind = PosKind::kEqOnehot;
                                    pos.sharedX = baseDef.operands()[1];
                                    return emitPosition(std::move(pos));
                                }
                            }
                        }
                    }
                }
                if (kind == OperationKind::kSliceStatic && classifyAffineGather(pos))
                {
                    // R-level affine gather: the packed per-lane slices are
                    // exactly X[gatherBase +: span*W]; materialization is
                    // zero-cost (see the kAffineGather case).
                    return emitPosition(std::move(pos));
                }
                bool kindsMatch = true;
                for (const ValueId value : pos.tuple)
                {
                    const OperationId defId = graph_.valueDef(value);
                    if (!defId.valid())
                    {
                        kindsMatch = false;
                        break;
                    }
                    const Operation def = graph_.getOperation(defId);
                    if (def.kind() != kind || def.operands().size() != operandCount ||
                        !opAttrsEqual(def, t0Def))
                    {
                        kindsMatch = false;
                        break;
                    }
                }
                if (!kindsMatch)
                {
                    reject("structure_mismatch", "op kinds/arities/attrs differ across lanes");
                    return -1;
                }
                if (kind == OperationKind::kEq && pos.laneWidth == 1 && operandCount == 2)
                {
                    // eq-onehot: per lane k, kEq(x, c_k) with the same shared x
                    // and c_k == lane index k; materialized as kShl(1, x).
                    ValueId sharedX;
                    bool onehot = true;
                    for (std::size_t k = 0; k < pos.tuple.size(); ++k)
                    {
                        const Operation def = graph_.getOperation(graph_.valueDef(pos.tuple[k]));
                        const auto operands = def.operands();
                        bool laneOk = false;
                        for (std::size_t side = 0; side < 2 && !laneOk; ++side)
                        {
                            const ValueId constValue = operands[side];
                            const ValueId xValue = operands[1 - side];
                            const auto parsed = getConstantUInt64(graph_, constValue, nullptr);
                            if (!parsed || *parsed != laneIndices_[k])
                            {
                                continue;
                            }
                            // Reject the ambiguous both-constant case.
                            if (getConstantUInt64(graph_, xValue, nullptr))
                            {
                                continue;
                            }
                            if (k == 0)
                            {
                                sharedX = xValue;
                            }
                            else if (xValue != sharedX)
                            {
                                continue;
                            }
                            laneOk = true;
                        }
                        if (!laneOk)
                        {
                            onehot = false;
                            break;
                        }
                    }
                    if (onehot && sharedX.valid())
                    {
                        pos.kind = PosKind::kEqOnehot;
                        pos.sharedX = sharedX;
                        return emitPosition(std::move(pos));
                    }
                }
                if (kind == OperationKind::kConcat && arrayMode_)
                {
                    // Array mode: a per-lane concat is a lane-parameter leaf.
                    // The materialized per-lane kConcat keeps lane i's
                    // original concat value in segment i, which is exactly the
                    // packed-row form packed-row consumers (kArrayReduceLanes*,
                    // kArrayMux, widened bitwise ops) expect. In wide mode a
                    // per-lane concat only merges via the C-level
                    // lane-parameter leaf below (-lane-param-leaves).
                    pos.kind = PosKind::kLaneParam;
                    return emitPosition(std::move(pos));
                }
                if (!isPointwiseKind(kind))
                {
                    if (laneParamLeaves_)
                    {
                        // C-level lane-parameter leaf: a non-pointwise op that
                        // is uniform across lanes (kind/arity/attrs/width
                        // verified above). The per-lane subgraphs are kept
                        // verbatim and packed by one per-lane kConcat (the
                        // same materialization as every other lane-parameter
                        // leaf); register reads inside them are retargeted by
                        // phase C3 like any other lane read. Attrs-varying
                        // shapes already failed the attrs check above; the
                        // kSliceStatic affine-gather form among them is
                        // recognized separately (R level, see
                        // classifyAffineGather), the rest stays rejected.
                        pos.kind = PosKind::kLaneParam;
                        return emitPosition(std::move(pos));
                    }
                    reject("unsupported_op",
                           std::string(wolvrix::lib::grh::toString(kind)) + " is not lane-pointwise");
                    return -1;
                }
                // Operand shape checks for lane-pointwise widening.
                const auto t0Operands = t0Def.operands();
                if (kind == OperationKind::kMux)
                {
                    if (operandCount != 3 || graph_.valueWidth(t0Operands[0]) != 1 ||
                        graph_.valueWidth(t0Operands[1]) != pos.laneWidth ||
                        graph_.valueWidth(t0Operands[2]) != pos.laneWidth)
                    {
                        reject("structure_mismatch", "kMux operand shape mismatch");
                        return -1;
                    }
                }
                else if (kind == OperationKind::kReplicate)
                {
                    // Widenable only as a per-lane broadcast of a 1-bit value.
                    const auto rep = getAttr<int64_t>(t0Def, "rep");
                    if (operandCount != 1 || !rep || graph_.valueWidth(t0Operands[0]) != 1 ||
                        *rep != pos.laneWidth)
                    {
                        reject("unsupported_op",
                               "kReplicate with a multi-bit operand or rep != lane width is not lane-pointwise");
                        return -1;
                    }
                }
                else if (isReduceLikeKind(kind))
                {
                    // A reduction of a 1-bit operand is the identity. A wider
                    // reduction is a per-lane reduction: lane i reduces its
                    // own W-bit operand down to guard bit i. Array mode
                    // materializes it as kArrayReduceLanes* over the packed
                    // rows; wide mode has no per-lane guard vector and keeps
                    // the rejection. kArrayReduce{Or,And,Xor} reaches here as
                    // the normalize product whose operand is still the
                    // per-lane value, so the same rule applies.
                    if (operandCount != 1 ||
                        (graph_.valueWidth(t0Operands[0]) != 1 && !arrayMode_))
                    {
                        reject("unsupported_op",
                               std::string(wolvrix::lib::grh::toString(kind)) +
                                   " with a multi-bit operand is not lane-pointwise");
                        return -1;
                    }
                }
                else
                {
                    for (const ValueId operand : t0Operands)
                    {
                        if (graph_.valueWidth(operand) != pos.laneWidth)
                        {
                            reject("structure_mismatch", "pointwise operand width mismatch");
                            return -1;
                        }
                    }
                }
                if (!visiting_.insert(pos.templateValue).second)
                {
                    reject("cycle_detected", "combinational cycle through cone");
                    return -1;
                }
                pos.kind = PosKind::kInternal;
                pos.opKind = kind;
                pos.children.reserve(operandCount);
                std::vector<ValueId> childTuple(pos.tuple.size());
                for (std::size_t operandIdx = 0; operandIdx < operandCount; ++operandIdx)
                {
                    for (std::size_t k = 0; k < pos.tuple.size(); ++k)
                    {
                        const Operation def = graph_.getOperation(graph_.valueDef(pos.tuple[k]));
                        childTuple[k] = def.operands()[operandIdx];
                    }
                    const int32_t child = visit(childTuple);
                    if (child < 0)
                    {
                        visiting_.erase(pos.templateValue);
                        return -1;
                    }
                    pos.children.push_back(child);
                }
                visiting_.erase(pos.templateValue);
                if (failed_)
                {
                    return -1;
                }
                return emitPosition(std::move(pos));
            }

            const Graph &graph_;
            std::string selfGroupKey_;
            const std::vector<uint64_t> &laneIndices_;
            const std::vector<std::string> &laneNames_;
            const RegLaneMap &regLane_;
            bool arrayMode_ = false;
            bool laneParamLeaves_ = false;
            ConeAnalysis *out_ = nullptr;
            bool failed_ = false;
            std::unordered_map<ValueId, int32_t, ValueIdHash> memo_;
            std::unordered_set<ValueId, ValueIdHash> visiting_;
        };
    } // namespace

} // namespace wolvrix::lib::transform

namespace wolvrix::lib::transform
{

    namespace
    {
        // ------------------------------------------------------------------
        // Group evaluation (phase A): name grouping, per-lane candidate
        // checks, signature bucketing, exact cone analysis.
        // ------------------------------------------------------------------
        struct LaneGroupEval
        {
            std::string key;
            std::string maskedKey;
            std::size_t laneSegment = 0; // which numeric segment is the lane index
            std::map<uint64_t, std::string> members; // lane index -> register symbol
            int32_t width = 0;
            bool isSigned = false;
            std::vector<uint64_t> candidates;
            std::vector<uint64_t> bucket;
            uint64_t minIdx = 0;
            uint64_t maxIdx = 0;
            uint64_t span = 0;
            bool merge = false;
            std::string outcome = "skipped";
            std::string rejectReason;
            std::string rejectDetail;
            // Set when the bucket was built by the exact-all fallback instead
            // of a signature majority (reported as no_majority_exact).
            bool exactFallbackUsed = false;
            std::vector<std::string> siblingDeps;
            // Per-lane constant init values (lanes whose initValue parses as a
            // constant); initValues is parallel to bucket after evaluation.
            std::map<uint64_t, uint64_t> laneInits;
            std::vector<uint64_t> initValues;
            // Rewrite artifacts (phase C).
            OperationId wideRegOp;
            OperationId wideReadOp;
            ValueId wideReadValue;
            std::string wideName;
        };

        struct GroupReportRecord
        {
            std::string graph;
            std::size_t groupId = 0;
            std::string discovery;
            std::string group;
            std::string module;
            std::string outputMode;
            int32_t elementWidth = 0;
            std::size_t elementCount = 0;
            std::size_t laneCount = 0;
            std::string outcome;
            std::string rejectReason;
            std::string rejectDetail;
        };

        bool eventsEqual(const WritePortInfo &lhs, const WritePortInfo &rhs)
        {
            if (lhs.eventEdges != rhs.eventEdges || lhs.events.size() != rhs.events.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.events.size(); ++i)
            {
                if (lhs.events[i] != rhs.events[i])
                {
                    return false;
                }
            }
            return true;
        }

        std::string groupModuleName(const Graph &graph, const LaneGroupEval &group)
        {
            for (const auto &[idx, regSymbol] : group.members)
            {
                (void)idx;
                const OperationId regOpId = graph.findOperation(regSymbol);
                if (!regOpId.valid())
                {
                    continue;
                }
                const auto &srcLoc = graph.getOperation(regOpId).srcLoc();
                if (!srcLoc || srcLoc->file.empty())
                {
                    continue;
                }
                const std::size_t slash = srcLoc->file.find_last_of("/\\");
                return slash == std::string::npos ? srcLoc->file : srcLoc->file.substr(slash + 1);
            }
            return {};
        }

        void evaluateGroup(Graph &graph,
                           const GraphIndexes &index,
                           const RegLaneMap &regLane,
                           const LaneAggregateOptions &options,
                           bool keepDeclared,
                           LaneGroupEval &group)
        {
            auto reject = [&](std::string reason, std::string detail = {}) {
                group.merge = false;
                group.outcome = "rejected";
                group.rejectReason = std::move(reason);
                group.rejectDetail = std::move(detail);
            };
            auto skip = [&](std::string reason, std::string detail = {}) {
                group.merge = false;
                group.outcome = "skipped";
                group.rejectReason = std::move(reason);
                group.rejectDetail = std::move(detail);
            };

            if (group.members.size() < options.minLanes)
            {
                skip("too_few_lanes");
                return;
            }

            // Uniform width/signedness across all members (group level).
            bool first = true;
            for (const auto &[idx, regSymbol] : group.members)
            {
                (void)idx;
                const auto widthIt = index.regWidths.find(regSymbol);
                if (widthIt == index.regWidths.end() || widthIt->second <= 0 ||
                    widthIt->second > std::numeric_limits<int32_t>::max())
                {
                    reject("invalid_register", regSymbol);
                    return;
                }
                const bool regSigned = index.regSigned.count(regSymbol) != 0 && index.regSigned.at(regSymbol);
                if (first)
                {
                    group.width = static_cast<int32_t>(widthIt->second);
                    group.isSigned = regSigned;
                    first = false;
                }
                else if (widthIt->second != group.width || regSigned != group.isSigned)
                {
                    reject("width_mismatch", regSymbol);
                    return;
                }
            }

            // Per-lane candidate checks; failing lanes stay scalar.
            std::map<std::string, std::size_t> excludedReasons;
            for (const auto &[idx, regSymbol] : group.members)
            {
                auto exclude = [&](std::string_view reason) { ++excludedReasons[std::string(reason)]; };
                const auto initIt = index.regInitValues.find(regSymbol);
                if (initIt != index.regInitValues.end())
                {
                    // Constant, parseable init values may merge (packed into
                    // the wide register); anything else ($random, unparseable)
                    // keeps the lane scalar.
                    const auto parsedInit = parseLiteralUInt64(initIt->second, group.width);
                    if (!parsedInit)
                    {
                        exclude("init_value");
                        continue;
                    }
                    group.laneInits.emplace(idx, *parsedInit);
                }
                if (index.xmrReferencedNames.count(regSymbol) != 0)
                {
                    exclude("xmr_reference");
                    continue;
                }
                const OperationId regOpId = graph.findOperation(regSymbol);
                if (!regOpId.valid())
                {
                    exclude("missing_register");
                    continue;
                }
                if (keepDeclared && graph.isDeclaredSymbol(graph.getOperation(regOpId).symbol()))
                {
                    exclude("declared_symbol");
                    continue;
                }
                const auto writesIt = index.writesByReg.find(regSymbol);
                if (writesIt == index.writesByReg.end() || writesIt->second.empty())
                {
                    exclude("missing_write_port");
                    continue;
                }
                if (writesIt->second.size() != 1)
                {
                    exclude("multi_write_port");
                    continue;
                }
                const WritePortInfo &write = writesIt->second.front();
                const Operation writeOp = graph.getOperation(write.op);
                if (writeOp.operands().size() < 4 || write.events.size() != write.eventEdges.size())
                {
                    exclude("invalid_write_port");
                    continue;
                }
                if (!write.updateCond.valid() || graph.valueWidth(write.updateCond) != 1 ||
                    !write.nextValue.valid() || graph.valueWidth(write.nextValue) != group.width)
                {
                    exclude("invalid_write_port");
                    continue;
                }
                if (!isAllOnesConstant(graph, write.mask, group.width))
                {
                    exclude("partial_mask");
                    continue;
                }
                group.candidates.push_back(idx);
            }
            if (group.candidates.size() < options.minLanes)
            {
                std::ostringstream detail;
                bool firstReason = true;
                for (const auto &[reason, count] : excludedReasons)
                {
                    if (!firstReason)
                    {
                        detail << ',';
                    }
                    firstReason = false;
                    detail << reason << ':' << count;
                }
                skip("too_few_lanes", detail.str());
                return;
            }

            // Signature bucketing (hash is only an accelerator).
            std::map<uint64_t, std::map<uint64_t, std::map<uint64_t, std::vector<uint64_t>>>> buckets;
            for (const uint64_t idx : group.candidates)
            {
                const WritePortInfo &write = index.writesByReg.at(group.members.at(idx)).front();
                LaneSignatureContext ctx;
                ctx.graph = &graph;
                ctx.regLane = &regLane;
                ctx.selfGroupKey = group.key;
                ctx.selfIndex = idx;
                const uint64_t condSig = laneValueSignature(ctx, write.updateCond);
                const uint64_t dataSig = laneValueSignature(ctx, write.nextValue);
                const uint64_t eventSig = laneEventSignature(write);
                buckets[condSig][dataSig][eventSig].push_back(idx);
            }
            const std::vector<uint64_t> *majority = nullptr;
            for (const auto &[cond, byData] : buckets)
            {
                (void)cond;
                for (const auto &[data, byEvents] : byData)
                {
                    (void)data;
                    for (const auto &[events, lanes] : byEvents)
                    {
                        (void)events;
                        if (majority == nullptr || lanes.size() > majority->size())
                        {
                            majority = &lanes;
                        }
                    }
                }
            }
            // Exact structural analysis of (updateCond, data) cones for one
            // bucket. Returned sibling deps merge into the group's set.
            auto analyzeBucket = [&](const std::vector<uint64_t> &bucket,
                                     ConeAnalysis &out) -> bool {
                std::vector<std::string> laneNames;
                std::vector<ValueId> condTuple;
                std::vector<ValueId> dataTuple;
                laneNames.reserve(bucket.size());
                condTuple.reserve(bucket.size());
                dataTuple.reserve(bucket.size());
                for (const uint64_t idx : bucket)
                {
                    laneNames.push_back(group.members.at(idx));
                    const WritePortInfo &write = index.writesByReg.at(group.members.at(idx)).front();
                    condTuple.push_back(write.updateCond);
                    dataTuple.push_back(write.nextValue);
                }
                ConeAnalyzer analyzer(graph, group.key, bucket, laneNames, regLane,
                                      options.outputMode == LaneAggregateOutputMode::Array,
                                      options.laneParamLeaves);
                return analyzer.analyze(condTuple, dataTuple, out);
            };

            ConeAnalysis analysis;
            std::vector<std::string> mergedDeps;
            if (majority == nullptr || majority->size() < options.minLanes)
            {
                if (!options.exactFallback)
                {
                    skip("no_majority",
                         "largest_signature_bucket=" +
                             std::to_string(majority == nullptr ? 0 : majority->size()));
                    return;
                }
                // Exact-all fallback: the signature is only a bucketing
                // accelerator; cone analysis is the ground truth. Groups whose
                // signatures over-split (a shared cone leaf hashing 53 vs 52
                // for the lane matching its absolute index, or a per-lane
                // full-range sibling reduction mixing 53/52 per element) run
                // the exact N-wise check over ALL candidates directly. One
                // shot first; on failure build the bucket incrementally like
                // the minority-lane rescue (keep a lane when the exact check
                // still passes and its event set matches the first kept
                // lane), capped at 32 attempts to bound evaluation cost.
                if (analyzeBucket(group.candidates, analysis))
                {
                    group.bucket = group.candidates; // ascending already
                    mergedDeps = analysis.siblingDeps;
                }
                else
                {
                    std::vector<uint64_t> built;
                    ConeAnalysis builtAnalysis;
                    const WritePortInfo *referenceWrite = nullptr;
                    std::size_t attempts = 0;
                    for (const uint64_t idx : group.candidates)
                    {
                        if (attempts++ >= 32)
                        {
                            break;
                        }
                        const WritePortInfo &laneWrite =
                            index.writesByReg.at(group.members.at(idx)).front();
                        if (referenceWrite != nullptr && !eventsEqual(*referenceWrite, laneWrite))
                        {
                            continue;
                        }
                        std::vector<uint64_t> trial = built;
                        trial.insert(std::lower_bound(trial.begin(), trial.end(), idx), idx);
                        ConeAnalysis trialAnalysis;
                        if (!analyzeBucket(trial, trialAnalysis))
                        {
                            continue;
                        }
                        built = std::move(trial);
                        referenceWrite = &laneWrite;
                        // Collect the sibling deps BEFORE moving the analysis:
                        // reading trialAnalysis after the move would see an
                        // emptied siblingDeps and bypass resolveSiblingDeps.
                        mergedDeps.insert(mergedDeps.end(), trialAnalysis.siblingDeps.begin(),
                                          trialAnalysis.siblingDeps.end());
                        builtAnalysis = std::move(trialAnalysis);
                    }
                    if (built.size() < options.minLanes)
                    {
                        skip("no_majority_exact",
                             "largest_signature_bucket=" +
                                 std::to_string(majority == nullptr ? 0 : majority->size()) +
                                 ",largest_exact_bucket=" + std::to_string(built.size()));
                        return;
                    }
                    group.bucket = std::move(built);
                    analysis = std::move(builtAnalysis);
                }
                group.exactFallbackUsed = true;
            }
            else
            {
                group.bucket = *majority; // ascending: candidates were visited in order

                if (!analyzeBucket(group.bucket, analysis))
                {
                    reject(analysis.rejectReason.empty() ? "structure_mismatch" : analysis.rejectReason,
                           analysis.rejectDetail);
                    return;
                }

                // Minority-lane rescue: signature bucketing over-splits lanes that
                // read a dispatch port at their own index (marker 53 vs 52); those
                // cones are usually identical to the majority. Try appending every
                // remaining candidate lane once; keep it when the exact check
                // still passes AND its write-port event set matches the majority.
                // Lanes failing the basic per-lane checks are not candidates and
                // never reach here.
                const WritePortInfo &majorityReferenceWrite =
                    index.writesByReg.at(group.members.at(group.bucket.front())).front();
                mergedDeps = analysis.siblingDeps;
                std::size_t rescueAttempts = 0;
                for (const uint64_t idx : group.candidates)
                {
                    if (std::binary_search(group.bucket.begin(), group.bucket.end(), idx))
                    {
                        continue;
                    }
                    if (rescueAttempts++ >= 32)
                    {
                        break;
                    }
                    const WritePortInfo &laneWrite = index.writesByReg.at(group.members.at(idx)).front();
                    if (!eventsEqual(majorityReferenceWrite, laneWrite))
                    {
                        continue;
                    }
                    std::vector<uint64_t> trial = group.bucket;
                    trial.insert(std::lower_bound(trial.begin(), trial.end(), idx), idx);
                    ConeAnalysis trialAnalysis;
                    if (!analyzeBucket(trial, trialAnalysis))
                    {
                        continue;
                    }
                    group.bucket = std::move(trial);
                    mergedDeps.insert(mergedDeps.end(), trialAnalysis.siblingDeps.begin(),
                                      trialAnalysis.siblingDeps.end());
                }
            }

            // Density of the final bucket.
            group.minIdx = group.bucket.front();
            group.maxIdx = group.bucket.back();
            const uint64_t holes =
                (group.maxIdx - group.minIdx + 1) - static_cast<uint64_t>(group.bucket.size());
            if (holes > options.maxIndexHoles)
            {
                reject("not_dense", "holes=" + std::to_string(holes));
                return;
            }
            group.span = group.maxIdx + 1;
            const uint64_t wideBits = group.span * static_cast<uint64_t>(group.width);
            if (wideBits > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
            {
                reject("too_wide", "span*width=" + std::to_string(wideBits));
                return;
            }

            // Exact event-set equality across the bucket (reset structure).
            const WritePortInfo &referenceWrite =
                index.writesByReg.at(group.members.at(group.bucket.front())).front();
            for (const uint64_t idx : group.bucket)
            {
                const WritePortInfo &write = index.writesByReg.at(group.members.at(idx)).front();
                if (!eventsEqual(referenceWrite, write))
                {
                    reject("event_mismatch", group.members.at(idx));
                    return;
                }
            }

            // initValue packing: either every bucket lane has a constant init
            // or none does (mixed init/no-init cannot be represented on one
            // wide register); per-lane init values must be affine in the lane
            // index (all-equal included).
            std::vector<uint64_t> bucketInits;
            bucketInits.reserve(group.bucket.size());
            bool anyInit = false;
            bool allInit = true;
            for (const uint64_t idx : group.bucket)
            {
                const auto initIt = group.laneInits.find(idx);
                if (initIt == group.laneInits.end())
                {
                    allInit = false;
                    continue;
                }
                anyInit = true;
                bucketInits.push_back(initIt->second);
            }
            if (anyInit && !allInit)
            {
                reject("init_mixed", "bucket mixes lanes with and without initValue");
                return;
            }
            if (anyInit)
            {
                if (!checkAffineConstants(bucketInits, group.bucket))
                {
                    reject("non_affine_init", "init values are not affine in the lane index");
                    return;
                }
                group.initValues = std::move(bucketInits);
            }

            std::sort(mergedDeps.begin(), mergedDeps.end());
            mergedDeps.erase(std::unique(mergedDeps.begin(), mergedDeps.end()), mergedDeps.end());
            group.siblingDeps = std::move(mergedDeps);
            group.merge = true;
            group.outcome = "merged";
        }

        // Phase B: a group that reads sibling lanes can only merge if every
        // referenced sibling group merges with a superset lane set and the
        // same span. Iterate to a fixpoint (decisions only flip merge->reject).
        void resolveSiblingDeps(std::vector<LaneGroupEval> &groups,
                                const std::unordered_map<std::string, std::size_t> &groupByKey)
        {
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (LaneGroupEval &group : groups)
                {
                    if (!group.merge)
                    {
                        continue;
                    }
                    for (const std::string &dep : group.siblingDeps)
                    {
                        const auto it = groupByKey.find(dep);
                        if (it == groupByKey.end() || !groups[it->second].merge)
                        {
                            group.merge = false;
                            group.outcome = "rejected";
                            group.rejectReason = "sibling_not_merged";
                            group.rejectDetail = dep;
                            changed = true;
                            break;
                        }
                        const LaneGroupEval &sibling = groups[it->second];
                        if (sibling.maxIdx != group.maxIdx ||
                            !std::includes(sibling.bucket.begin(), sibling.bucket.end(),
                                           group.bucket.begin(), group.bucket.end()))
                        {
                            group.merge = false;
                            group.outcome = "rejected";
                            group.rejectReason = "sibling_lane_mismatch";
                            group.rejectDetail = dep;
                            changed = true;
                            break;
                        }
                    }
                }
            }
        }
    } // namespace

} // namespace wolvrix::lib::transform

namespace wolvrix::lib::transform
{

    namespace
    {
        // ------------------------------------------------------------------
        // Op construction helpers.
        // ------------------------------------------------------------------
        ValueId createConstantValue(Graph &graph, int32_t width, std::string literal, std::string_view note)
        {
            const ValueId value = graph.createValue(graph.makeInternalValSym(),
                                                    width > 0 ? width : 1, false, ValueType::Logic);
            const OperationId op = graph.createOperation(OperationKind::kConstant, graph.makeInternalOpSym());
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::move(literal));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(value, srcLoc);
            return value;
        }

        ValueId createUnaryOp(Graph &graph, OperationKind kind, ValueId operand,
                              int32_t width, bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createBinaryOp(Graph &graph, OperationKind kind, ValueId lhs, ValueId rhs,
                               int32_t width, bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        // operands[0] maps to the most significant bits (kConcat semantics).
        ValueId createConcatOp(Graph &graph, std::span<const ValueId> operands,
                               int32_t width, bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kConcat, graph.makeInternalOpSym());
            for (const ValueId operand : operands)
            {
                graph.addOperand(op, operand);
            }
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createReplicateOp(Graph &graph, ValueId operand, uint64_t rep, std::string_view note)
        {
            const int64_t width = static_cast<int64_t>(graph.valueWidth(operand)) * static_cast<int64_t>(rep);
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  static_cast<int32_t>(width),
                                                  graph.valueSigned(operand), graph.valueType(operand));
            const OperationId op = graph.createOperation(OperationKind::kReplicate, graph.makeInternalOpSym());
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            graph.setAttr(op, "rep", static_cast<int64_t>(rep));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createSliceOp(Graph &graph, ValueId base, uint64_t low, uint64_t high,
                              bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  static_cast<int32_t>(high - low + 1), isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kSliceStatic, graph.makeInternalOpSym());
            graph.addOperand(op, base);
            graph.addResult(op, out);
            graph.setAttr(op, "sliceStart", static_cast<int64_t>(low));
            graph.setAttr(op, "sliceEnd", static_cast<int64_t>(high));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        // Address width of a kMemory with the given row count (reg-to-mem
        // convention): ceil(log2(rows)), at least 1.
        int32_t addressWidthForRows(uint64_t rows)
        {
            int32_t addrWidth = 1;
            uint64_t addressSpace = 2;
            while (addressSpace < rows)
            {
                ++addrWidth;
                addressSpace <<= 1;
            }
            return addrWidth;
        }

        ValueId createMemoryReadOp(Graph &graph, const std::string &memSymbol, ValueId addr,
                                   int32_t width, bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kMemoryReadPort, graph.makeInternalOpSym());
            graph.addOperand(op, addr);
            graph.addResult(op, out);
            graph.setAttr(op, "memSymbol", memSymbol);
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createArrayBroadcastOp(Graph &graph, ValueId operand, uint64_t rows, std::string_view note)
        {
            const int64_t width = static_cast<int64_t>(graph.valueWidth(operand)) * static_cast<int64_t>(rows);
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  static_cast<int32_t>(width),
                                                  graph.valueSigned(operand), graph.valueType(operand));
            const OperationId op = graph.createOperation(OperationKind::kArrayBroadcast, graph.makeInternalOpSym());
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            graph.setAttr(op, "rows", static_cast<int64_t>(rows));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        // values has one entry per row (holes already zeroed by the caller).
        ValueId createArrayLaneConstOp(Graph &graph, uint64_t rows, int32_t elemWidth,
                                       std::vector<int64_t> values, bool isSigned, ValueType type,
                                       std::string_view note)
        {
            const int64_t width = static_cast<int64_t>(elemWidth) * static_cast<int64_t>(rows);
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  static_cast<int32_t>(width), isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kArrayLaneConst, graph.makeInternalOpSym());
            graph.addResult(op, out);
            graph.setAttr(op, "elemWidth", static_cast<int64_t>(elemWidth));
            graph.setAttr(op, "rows", static_cast<int64_t>(rows));
            graph.setAttr(op, "values", std::move(values));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createArrayOnehotOp(Graph &graph, ValueId x, uint64_t rows, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  static_cast<int32_t>(rows), false, ValueType::Logic);
            const OperationId op = graph.createOperation(OperationKind::kArrayOnehot, graph.makeInternalOpSym());
            graph.addOperand(op, x);
            graph.addResult(op, out);
            graph.setAttr(op, "rows", static_cast<int64_t>(rows));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createArrayMuxOp(Graph &graph, ValueId sel, ValueId whenTrue, ValueId whenFalse,
                                 int32_t width, bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kArrayMux, graph.makeInternalOpSym());
            graph.addOperand(op, sel);
            graph.addOperand(op, whenTrue);
            graph.addOperand(op, whenFalse);
            graph.addResult(op, out);
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        // Per-lane reduction over packed rows: data is rows*elemWidth bits,
        // result is the rows-bit guard vector (bit i = reduce of lane i).
        ValueId createArrayReduceLanesOp(Graph &graph, OperationKind kind, ValueId data,
                                         uint64_t rows, int32_t elemWidth, std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  static_cast<int32_t>(rows), false, ValueType::Logic);
            const OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
            graph.addOperand(op, data);
            graph.addResult(op, out);
            graph.setAttr(op, "elemWidth", static_cast<int64_t>(elemWidth));
            const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        slang::SVInt packedConstantTable(uint64_t span, int32_t laneWidth,
                                         const std::vector<uint64_t> &laneIndices,
                                         const std::vector<uint64_t> &values)
        {
            const auto totalWidth = static_cast<slang::bitwidth_t>(span * static_cast<uint64_t>(laneWidth));
            slang::SVInt acc(totalWidth, 0, false);
            for (std::size_t k = 0; k < laneIndices.size(); ++k)
            {
                slang::SVInt segment(totalWidth, values[k], false);
                acc = acc | segment.shl(static_cast<slang::bitwidth_t>(laneIndices[k] * static_cast<uint64_t>(laneWidth)));
            }
            return acc;
        }

        // ------------------------------------------------------------------
        // Phase C: materialize one merged group.
        // ------------------------------------------------------------------
        bool analyzeGroupCones(const Graph &graph,
                               const GraphIndexes &index,
                               const RegLaneMap &regLane,
                               const LaneGroupEval &group,
                               bool arrayMode,
                               bool laneParamLeaves,
                               ConeAnalysis &out)
        {
            std::vector<std::string> laneNames;
            std::vector<ValueId> condTuple;
            std::vector<ValueId> dataTuple;
            laneNames.reserve(group.bucket.size());
            condTuple.reserve(group.bucket.size());
            dataTuple.reserve(group.bucket.size());
            for (const uint64_t idx : group.bucket)
            {
                laneNames.push_back(group.members.at(idx));
                const WritePortInfo &write = index.writesByReg.at(group.members.at(idx)).front();
                condTuple.push_back(write.updateCond);
                dataTuple.push_back(write.nextValue);
            }
            ConeAnalyzer analyzer(graph, group.key, group.bucket, laneNames, regLane, arrayMode,
                                  laneParamLeaves);
            return analyzer.analyze(condTuple, dataTuple, out);
        }

        ValueId broadcastLaneBits(Graph &graph, ValueId source, uint64_t span,
                                  int32_t laneWidth, std::string_view note)
        {
            // source is span bits; produce span*laneWidth bits with each source
            // bit replicated laneWidth times (bit i -> segment [i*laneWidth +: laneWidth]).
            if (laneWidth == 1)
            {
                return source;
            }
            std::vector<ValueId> segments;
            segments.reserve(span);
            for (uint64_t i = span; i-- > 0;)
            {
                const ValueId bit = createSliceOp(graph, source, i, i, false, ValueType::Logic, note);
                segments.push_back(createReplicateOp(graph, bit, static_cast<uint64_t>(laneWidth), note));
            }
            return createConcatOp(graph, segments,
                                  static_cast<int32_t>(span * static_cast<uint64_t>(laneWidth)),
                                  false, ValueType::Logic, note);
        }

        bool materializeGroup(Graph &graph,
                              const GraphIndexes &index,
                              std::vector<LaneGroupEval> &groups,
                              const std::unordered_map<std::string, std::size_t> &groupByKey,
                              const RegLaneMap &regLane,
                              LaneGroupEval &group,
                              bool arrayMode,
                              bool laneParamLeaves,
                              std::string &errorOut)
        {
            ConeAnalysis analysis;
            if (!analyzeGroupCones(graph, index, regLane, group, arrayMode, laneParamLeaves,
                                   analysis))
            {
                errorOut = "re-analysis failed: " + analysis.rejectReason;
                return false;
            }
            const uint64_t span = group.span;
            const int32_t width = group.width;
            const uint64_t wideBits = span * static_cast<uint64_t>(width);
            if (wideBits > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
            {
                errorOut = "span*width overflow";
                return false;
            }
            const int32_t wideWidth = static_cast<int32_t>(wideBits);

            std::vector<ValueId> merged(analysis.positions.size());
            std::unordered_map<ValueId, ValueId, ValueIdHash> sharedCache;
            std::unordered_map<std::string, ValueId> sharedRegCache;
            std::unordered_map<std::string, ValueId> scalarReadCache;

            for (std::size_t p = 0; p < analysis.positions.size(); ++p)
            {
                const ConePosition &pos = analysis.positions[p];
                const uint64_t outBits = span * static_cast<uint64_t>(pos.laneWidth);
                if (outBits > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
                {
                    errorOut = "merged width overflow";
                    return false;
                }
                const int32_t outWidth = static_cast<int32_t>(outBits);
                ValueId value;
                switch (pos.kind)
                {
                case PosKind::kShared:
                {
                    auto it = sharedCache.find(pos.templateValue);
                    if (it == sharedCache.end())
                    {
                        it = sharedCache
                                 .emplace(pos.templateValue,
                                          arrayMode
                                              ? createArrayBroadcastOp(graph, pos.templateValue, span,
                                                                       "array-broadcast")
                                              : createReplicateOp(graph, pos.templateValue, span, "shared-leaf"))
                                 .first;
                    }
                    value = it->second;
                    break;
                }
                case PosKind::kConstant:
                {
                    if (arrayMode)
                    {
                        // Per-lane constant table, hole lanes zeroed.
                        std::vector<int64_t> laneValues(span, 0);
                        for (std::size_t k = 0; k < group.bucket.size(); ++k)
                        {
                            laneValues[group.bucket[k]] = static_cast<int64_t>(pos.constValues[k]);
                        }
                        value = createArrayLaneConstOp(graph, span, pos.laneWidth, std::move(laneValues),
                                                       pos.isSigned, pos.valueType, "array-lane-const");
                        break;
                    }
                    const slang::SVInt table =
                        packedConstantTable(span, pos.laneWidth, group.bucket, pos.constValues);
                    value = createConstantValue(graph, outWidth, makeHexLiteral(outWidth, table),
                                                "lane-const-table");
                    break;
                }
                case PosKind::kLaneParam:
                {
                    // Per-lane values concatenated in segment order: operand 0
                    // is the highest segment (kConcat puts oper[0] in the
                    // MSBs); hole segments get a zero constant.
                    ValueId zero;
                    std::vector<ValueId> operands;
                    operands.reserve(span);
                    for (uint64_t seg = span; seg-- > 0;)
                    {
                        const auto bucketIt =
                            std::lower_bound(group.bucket.begin(), group.bucket.end(), seg);
                        if (bucketIt != group.bucket.end() && *bucketIt == seg)
                        {
                            const std::size_t lanePos =
                                static_cast<std::size_t>(bucketIt - group.bucket.begin());
                            operands.push_back(pos.tuple[lanePos]);
                            continue;
                        }
                        if (!zero.valid())
                        {
                            zero = createConstantValue(graph, pos.laneWidth,
                                                       std::to_string(pos.laneWidth) + "'d0",
                                                       "lane-param-hole");
                        }
                        operands.push_back(zero);
                    }
                    value = createConcatOp(graph, operands, outWidth, pos.isSigned, pos.valueType,
                                           "lane-param-concat");
                    break;
                }
                case PosKind::kEqOnehot:
                {
                    // Result bit i = (x == i) for i in [0, span); x >= span
                    // naturally yields 0 under fixed-width shl / kArrayOnehot.
                    if (arrayMode)
                    {
                        value = createArrayOnehotOp(graph, pos.sharedX, span, "array-onehot");
                        break;
                    }
                    const ValueId one = createConstantValue(graph, static_cast<int32_t>(span),
                                                            std::to_string(span) + "'d1",
                                                            "eq-onehot-one");
                    value = createBinaryOp(graph, OperationKind::kShl, one, pos.sharedX,
                                           static_cast<int32_t>(span), false, ValueType::Logic,
                                           "eq-onehot-shl");
                    break;
                }
                case PosKind::kAffineGather:
                {
                    // Zero-cost: the packed per-lane slices are exactly
                    // X[gatherBase +: span*W]. Use X directly when it exactly
                    // fits, else one kSliceStatic. Identical in both output
                    // modes (the packed-row layout is the same).
                    if (pos.gatherBase == 0 &&
                        graph.valueWidth(pos.gatherX) == outWidth)
                    {
                        value = pos.gatherX;
                        break;
                    }
                    value = createSliceOp(graph, pos.gatherX, pos.gatherBase,
                                          pos.gatherBase + static_cast<uint64_t>(outWidth) - 1,
                                          pos.isSigned, pos.valueType, "affine-gather-slice");
                    break;
                }
                case PosKind::kSelfRead:
                    value = group.wideReadValue;
                    break;
                case PosKind::kSiblingRead:
                {
                    const auto gIt = groupByKey.find(pos.refGroupKey);
                    if (gIt == groupByKey.end() || !groups[gIt->second].merge)
                    {
                        errorOut = "sibling group not merged: " + pos.refGroupKey;
                        return false;
                    }
                    value = groups[gIt->second].wideReadValue;
                    break;
                }
                case PosKind::kSharedRegRead:
                {
                    auto it = sharedRegCache.find(pos.refRegSymbol);
                    if (it == sharedRegCache.end())
                    {
                        ValueId base;
                        bool resolved = false;
                        const auto laneIt = regLane.find(pos.refRegSymbol);
                        if (laneIt != regLane.end())
                        {
                            const auto gIt = groupByKey.find(laneIt->second.first);
                            if (gIt != groupByKey.end() && groups[gIt->second].merge &&
                                std::binary_search(groups[gIt->second].bucket.begin(),
                                                   groups[gIt->second].bucket.end(),
                                                   laneIt->second.second))
                            {
                                const LaneGroupEval &source = groups[gIt->second];
                                if (arrayMode)
                                {
                                    const int32_t addrWidth = addressWidthForRows(source.span);
                                    const ValueId addr = createConstantValue(
                                        graph, addrWidth,
                                        std::to_string(addrWidth) + "'d" + std::to_string(laneIt->second.second),
                                        "shared-lane-addr");
                                    base = createMemoryReadOp(graph, source.wideName, addr, pos.laneWidth,
                                                              pos.isSigned, pos.valueType, "shared-lane-row");
                                }
                                else
                                {
                                    const uint64_t low = laneIt->second.second * static_cast<uint64_t>(pos.laneWidth);
                                    base = createSliceOp(graph, source.wideReadValue, low,
                                                         low + static_cast<uint64_t>(pos.laneWidth) - 1,
                                                         pos.isSigned, pos.valueType, "shared-lane-slice");
                                }
                                resolved = true;
                            }
                        }
                        if (!resolved)
                        {
                            if (index.regOps.find(pos.refRegSymbol) == index.regOps.end())
                            {
                                errorOut = "shared register missing: " + pos.refRegSymbol;
                                return false;
                            }
                            auto readIt = scalarReadCache.find(pos.refRegSymbol);
                            if (readIt == scalarReadCache.end())
                            {
                                const ValueId readValue = graph.createValue(
                                    graph.makeInternalValSym(), pos.laneWidth, pos.isSigned, pos.valueType);
                                const OperationId readOp = graph.createOperation(
                                    OperationKind::kRegisterReadPort, graph.makeInternalOpSym());
                                graph.addResult(readOp, readValue);
                                graph.setAttr(readOp, "regSymbol", pos.refRegSymbol);
                                const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), "shared-reg-read");
                                graph.setOpSrcLoc(readOp, srcLoc);
                                graph.setValueSrcLoc(readValue, srcLoc);
                                readIt = scalarReadCache.emplace(pos.refRegSymbol, readValue).first;
                            }
                            base = readIt->second;
                        }
                        it = sharedRegCache
                                 .emplace(pos.refRegSymbol,
                                          arrayMode
                                              ? createArrayBroadcastOp(graph, base, span, "shared-reg-broadcast")
                                              : createReplicateOp(graph, base, span, "shared-reg-broadcast"))
                                 .first;
                    }
                    value = it->second;
                    break;
                }
                case PosKind::kInternal:
                {
                    std::vector<ValueId> operands;
                    operands.reserve(pos.children.size());
                    for (const int32_t child : pos.children)
                    {
                        operands.push_back(merged[static_cast<std::size_t>(child)]);
                    }
                    if (pos.opKind == OperationKind::kReplicate)
                    {
                        // Per-lane 1-bit broadcast: merged operand is span
                        // bits, each lane's bit replicated laneWidth times.
                        value = broadcastLaneBits(graph, operands[0], span, pos.laneWidth,
                                                  "replicate-broadcast");
                    }
                    else if (isReduceLikeKind(pos.opKind))
                    {
                        // A reduction of a 1-bit operand is the identity. A
                        // reduction of a wider per-lane operand (accepted
                        // only in array mode) becomes a per-lane reduction
                        // over the packed rows: kArrayReduceLanes* takes the
                        // span*W child and yields the span-bit guard vector.
                        // Keeping the global kArrayReduce* kind here would
                        // instead collapse ALL lanes into one bit.
                        const int32_t childWidth =
                            analysis.positions[static_cast<std::size_t>(pos.children.front())].laneWidth;
                        if (childWidth <= 1)
                        {
                            value = operands[0];
                        }
                        else
                        {
                            value = createArrayReduceLanesOp(
                                graph, arrayReduceLanesKindForReduce(pos.opKind), operands[0],
                                span, childWidth, "array-reduce-lanes");
                        }
                    }
                    else if (pos.opKind == OperationKind::kMux)
                    {
                        const ValueId select = operands[0];
                        if (arrayMode)
                        {
                            // The merged select is already the span-wide
                            // per-lane guard vector kArrayMux expects.
                            value = createArrayMuxOp(graph, select, operands[1], operands[2],
                                                     outWidth, pos.isSigned, pos.valueType, "array-mux");
                            break;
                        }
                        const ValueId mask = broadcastLaneBits(graph, select, span, pos.laneWidth, "mux-select-broadcast");
                        const ValueId invMask = createUnaryOp(graph, OperationKind::kNot, mask,
                                                              outWidth, pos.isSigned, pos.valueType, "mux-mask-not");
                        const ValueId trueMasked = createBinaryOp(graph, OperationKind::kAnd, operands[1], mask,
                                                                  outWidth, pos.isSigned, pos.valueType, "mux-true");
                        const ValueId falseMasked = createBinaryOp(graph, OperationKind::kAnd, operands[2], invMask,
                                                                   outWidth, pos.isSigned, pos.valueType, "mux-false");
                        value = createBinaryOp(graph, OperationKind::kOr, trueMasked, falseMasked,
                                               outWidth, pos.isSigned, pos.valueType, "mux-or");
                    }
                    else if (operands.size() == 1)
                    {
                        value = createUnaryOp(graph, materializedPointwiseKind(pos.opKind), operands[0],
                                              outWidth, pos.isSigned, pos.valueType, "widen-unary");
                    }
                    else
                    {
                        value = createBinaryOp(graph, materializedPointwiseKind(pos.opKind), operands[0],
                                               operands[1], outWidth, pos.isSigned, pos.valueType,
                                               "widen-binary");
                    }
                    break;
                }
                }
                if (!value.valid())
                {
                    errorOut = "materialization produced invalid value";
                    return false;
                }
                merged[p] = value;
            }

            const ValueId condVec = merged[static_cast<std::size_t>(analysis.condRoot)];
            const ValueId dataVec = merged[static_cast<std::size_t>(analysis.dataRoot)];

            // laneMask/updateCond base: condVec & presentLanes (hole lanes
            // never write). Array mode uses a kArrayLaneConst guard constant.
            ValueId presentConst;
            if (arrayMode)
            {
                std::vector<int64_t> presentValues(span, 0);
                for (const uint64_t idx : group.bucket)
                {
                    presentValues[idx] = 1;
                }
                presentConst = createArrayLaneConstOp(graph, span, 1, std::move(presentValues),
                                                      false, ValueType::Logic, "present-lanes");
            }
            else
            {
                const auto spanWidth = static_cast<slang::bitwidth_t>(span);
                slang::SVInt present(spanWidth, 0, false);
                for (const uint64_t idx : group.bucket)
                {
                    present = present | slang::SVInt(spanWidth, 1, false).shl(static_cast<slang::bitwidth_t>(idx));
                }
                presentConst =
                    createConstantValue(graph, static_cast<int32_t>(span), makeHexLiteral(static_cast<int32_t>(span), present), "present-lanes");
            }
            const ValueId condMasked = createBinaryOp(graph, OperationKind::kAnd, condVec, presentConst,
                                                      static_cast<int32_t>(span), false, ValueType::Logic, "cond-present");

            const WritePortInfo &referenceWrite =
                index.writesByReg.at(group.members.at(group.bucket.front())).front();
            if (arrayMode)
            {
                // One kArrayWritePort: laneMask is the per-lane enable, the
                // whole packed data cone is the write data.
                const OperationId writeOp =
                    graph.createOperation(OperationKind::kArrayWritePort, graph.makeInternalOpSym());
                graph.addOperand(writeOp, condMasked);
                graph.addOperand(writeOp, dataVec);
                for (const ValueId event : referenceWrite.events)
                {
                    graph.addOperand(writeOp, event);
                }
                graph.setAttr(writeOp, "memSymbol", group.wideName);
                graph.setAttr(writeOp, "eventEdge", referenceWrite.eventEdges);
                const SrcLoc writeLoc = makeTransformSrcLoc(std::string(kPassId), "array-write");
                graph.setOpSrcLoc(writeOp, writeLoc);
                return true;
            }

            const ValueId updateCond = createUnaryOp(graph, OperationKind::kReduceOr, condMasked,
                                                     1, false, ValueType::Logic, "update-cond");

            // mask = per-lane broadcast of the masked cond; nextValue =
            // (wide & ~mask) | (dataVec & mask).
            const ValueId mask = broadcastLaneBits(graph, condMasked, span, width, "lane-mask");
            const ValueId invMask = createUnaryOp(graph, OperationKind::kNot, mask,
                                                  wideWidth, group.isSigned, ValueType::Logic, "lane-mask-not");
            const ValueId dataMasked = createBinaryOp(graph, OperationKind::kAnd, dataVec, mask,
                                                      wideWidth, group.isSigned, ValueType::Logic, "lane-data-masked");
            const ValueId holdMasked = createBinaryOp(graph, OperationKind::kAnd, group.wideReadValue, invMask,
                                                      wideWidth, group.isSigned, ValueType::Logic, "lane-hold-masked");
            const ValueId nextValue = createBinaryOp(graph, OperationKind::kOr, dataMasked, holdMasked,
                                                     wideWidth, group.isSigned, ValueType::Logic, "lane-next-value");

            slang::SVInt allOnes(static_cast<slang::bitwidth_t>(wideWidth), 0, false);
            allOnes = ~allOnes;
            const ValueId maskAll = createConstantValue(graph, wideWidth, makeHexLiteral(wideWidth, allOnes), "write-mask-all");

            const OperationId writeOp =
                graph.createOperation(OperationKind::kRegisterWritePort, graph.makeInternalOpSym());
            graph.addOperand(writeOp, updateCond);
            graph.addOperand(writeOp, nextValue);
            graph.addOperand(writeOp, maskAll);
            for (const ValueId event : referenceWrite.events)
            {
                graph.addOperand(writeOp, event);
            }
            graph.setAttr(writeOp, "regSymbol", group.wideName);
            graph.setAttr(writeOp, "eventEdge", referenceWrite.eventEdges);
            const SrcLoc writeLoc = makeTransformSrcLoc(std::string(kPassId), "wide-write");
            graph.setOpSrcLoc(writeOp, writeLoc);
            return true;
        }

        // Phase C3: read side rewrite + old op cleanup. Runs only after every
        // merged group's cones have been materialized: erasing a lane read
        // port replaces its uses (including dead per-lane cones of sibling
        // groups) with slices, which must not happen while sibling groups
        // still re-analyze those cones.
        bool eraseGroupLanes(Graph &graph,
                             const GraphIndexes &index,
                             LaneGroupEval &group,
                             bool arrayMode,
                             std::string &errorOut)
        {
            const int32_t width = group.width;
            // Read side: every read of a merged lane register becomes
            // kSliceStatic(wide_read, idx*W +: W) in wide mode, or one
            // kMemoryReadPort at constant address idx in array mode.
            for (const uint64_t idx : group.bucket)
            {
                const std::string &laneName = group.members.at(idx);
                const auto readsIt = index.readsByReg.find(laneName);
                if (readsIt != index.readsByReg.end())
                {
                    for (const OperationId readOpId : readsIt->second)
                    {
                        const Operation readOp = graph.getOperation(readOpId);
                        if (readOp.results().size() != 1)
                        {
                            errorOut = "read port with unexpected results: " + laneName;
                            return false;
                        }
                        const ValueId oldResult = readOp.results().front();
                        ValueId replacement;
                        if (arrayMode)
                        {
                            const int32_t addrWidth = addressWidthForRows(group.span);
                            const ValueId addr = createConstantValue(
                                graph, addrWidth,
                                std::to_string(addrWidth) + "'d" + std::to_string(idx), "lane-read-addr");
                            replacement = createMemoryReadOp(graph, group.wideName, addr, width,
                                                             graph.valueSigned(oldResult),
                                                             graph.valueType(oldResult), "lane-read-row");
                        }
                        else
                        {
                            const uint64_t low = idx * static_cast<uint64_t>(width);
                            replacement =
                                createSliceOp(graph, group.wideReadValue, low, low + static_cast<uint64_t>(width) - 1,
                                              graph.valueSigned(oldResult), graph.valueType(oldResult), "lane-read-slice");
                        }
                        if (const auto loc = graph.valueSrcLoc(oldResult))
                        {
                            graph.setValueSrcLoc(replacement, *loc);
                        }
                        const Value oldValue = graph.getValue(oldResult);
                        if (oldValue.symbol().valid())
                        {
                            const auto oldSymbol = oldValue.symbol();
                            graph.setValueSymbol(oldResult, graph.makeInternalValSym());
                            graph.setValueSymbol(replacement, oldSymbol);
                        }
                        for (const auto &port : graph.outputPorts())
                        {
                            if (port.value == oldResult)
                            {
                                graph.bindOutputPort(port.name, replacement);
                            }
                        }
                        if (!graph.eraseOp(readOpId, std::array<ValueId, 1>{replacement}))
                        {
                            errorOut = "failed to replace read port of " + laneName;
                            return false;
                        }
                    }
                }
                const WritePortInfo &write = index.writesByReg.at(laneName).front();
                if (!graph.eraseOp(write.op))
                {
                    errorOut = "failed to erase write port of " + laneName;
                    return false;
                }
                if (!graph.eraseOp(index.regOps.at(laneName)))
                {
                    errorOut = "failed to erase register " + laneName;
                    return false;
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Phase 2 (read side): select trees over lane slices become one
        // kSliceDynamic over the wide read (wide mode) or one kMemoryReadPort
        // at the select pointer (array mode).
        //
        // Matched forms (leaves are kSliceStatic(wideRead, i*W, W) /
        // kMemoryReadPort(mem, const i) produced by phase 1, optionally
        // through kAssign chains):
        //   mux chain: kMux(kEq(ptr, C_i), slice_i, rest), recursing on the
        //     false branch; constants may be in any order but must cover all
        //     span indices; the final default must be a zero constant
        //     (2-state kSliceDynamic out-of-range reads yield 0).
        //   and/or onehot tree: kOr over mutually exclusive terms
        //     kAnd(kReplicate(sel_i, W), slice_i) or kMux(sel_i, slice_i, 0)
        //     (bare kAnd(sel_i, slice_i) at W == 1), where sel_i is either
        //     kEq(ptr, i) or a shl-onehot bit select
        //     kSlice{kStatic,Dynamic}(kShl(1, ptr), const i) (the DataModule
        //     `addr_dec = 1 << raddr` decode). The shl-onehot form additionally
        //     requires ptrWidth == log2(span) so the pointer can never go out
        //     of range.
        // Every select shares the same ptr ValueId; every leaf slice belongs
        // to the same merged group; a tree that touches any other value is
        // skipped.
        // ------------------------------------------------------------------
        struct ReadSelectStats
        {
            std::size_t trees = 0;
            std::size_t opsRetired = 0;
            std::size_t opsCreated = 0;
        };

        struct LaneSliceMatch
        {
            const LaneGroupEval *group = nullptr;
            uint64_t laneIndex = 0;
            int32_t width = 0;
        };

        struct EqConstMatch
        {
            ValueId ptr;
            uint64_t constant = 0;
            // true when the select was matched as a shl-onehot bit select
            // (kSlice{kStatic,Dynamic}(kShl(1, ptr), const)) instead of
            // kEq(ptr, const).
            bool shlOnehot = false;
        };

        std::optional<ValueId> unwrapAssignValue(const Graph &graph, ValueId value)
        {
            ValueId current = value;
            for (int depth = 0; depth < 8; ++depth)
            {
                const OperationId defOpId = graph.valueDef(current);
                if (!defOpId.valid())
                {
                    return current;
                }
                const Operation defOp = graph.getOperation(defOpId);
                if (defOp.kind() != OperationKind::kAssign || defOp.operands().size() != 1)
                {
                    return current;
                }
                current = defOp.operands().front();
            }
            return std::nullopt;
        }

        std::optional<EqConstMatch> matchEqConstant(const Graph &graph, ValueId value)
        {
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kEq || defOp.operands().size() != 2 ||
                graph.valueWidth(value) != 1)
            {
                return std::nullopt;
            }
            const auto operands = defOp.operands();
            std::optional<EqConstMatch> match;
            for (std::size_t side = 0; side < 2; ++side)
            {
                const auto constant = getConstantUInt64(graph, operands[side], nullptr);
                if (!constant)
                {
                    continue;
                }
                if (getConstantUInt64(graph, operands[1 - side], nullptr))
                {
                    return std::nullopt; // both operands constant: ambiguous
                }
                if (match)
                {
                    return std::nullopt;
                }
                match = EqConstMatch{operands[1 - side], *constant};
            }
            return match;
        }

        // Shl-onehot bit select: bit `c` of `(1 << ptr)`, written as
        // kSliceStatic(X, c, c) or kSliceDynamic(X, <const c>, sliceWidth = 1)
        // with X = kShl(kConstant 1, ptr). This is the DataModule-family
        // read-port decode (`addr_dec = 64'h1 << raddr; addr_dec[i] ? ...`).
        //
        // Equivalence (2-state): bit c of (1 << ptr) is (ptr == c) — for
        // ptr < width(X) the shift lands exactly on bit ptr, and for
        // ptr >= width(X) the 2-state shift result is 0, which agrees with
        // (ptr == c) because c < width(X). Under 4-state simulation
        // X-propagation may differ (same caveat class as onehot-to-mux);
        // the rewrite only claims 2-state equivalence.
        std::optional<EqConstMatch> matchShlOnehotBit(const Graph &graph, ValueId value)
        {
            if (graph.valueWidth(value) != 1)
            {
                return std::nullopt;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            uint64_t bitIndex = 0;
            ValueId base;
            if (defOp.kind() == OperationKind::kSliceStatic && defOp.operands().size() == 1)
            {
                const auto sliceStart = getAttr<int64_t>(defOp, "sliceStart");
                const auto sliceEnd = getAttr<int64_t>(defOp, "sliceEnd");
                if (!sliceStart || !sliceEnd || *sliceStart != *sliceEnd || *sliceStart < 0)
                {
                    return std::nullopt;
                }
                bitIndex = static_cast<uint64_t>(*sliceStart);
                base = defOp.operands().front();
            }
            else if (defOp.kind() == OperationKind::kSliceDynamic && defOp.operands().size() == 2)
            {
                const auto sliceWidth = getAttr<int64_t>(defOp, "sliceWidth");
                if (!sliceWidth || *sliceWidth != 1)
                {
                    return std::nullopt;
                }
                const auto offset = getConstantUInt64(graph, defOp.operands()[1], nullptr);
                if (!offset)
                {
                    return std::nullopt;
                }
                bitIndex = *offset;
                base = defOp.operands()[0];
            }
            else
            {
                return std::nullopt;
            }
            const OperationId baseDefId = graph.valueDef(base);
            if (!baseDefId.valid())
            {
                return std::nullopt;
            }
            const Operation baseDef = graph.getOperation(baseDefId);
            if (baseDef.kind() != OperationKind::kShl || baseDef.operands().size() != 2)
            {
                return std::nullopt;
            }
            // The equivalence bit c of (1 << ptr) == (ptr == c) needs
            // c < width(X); an out-of-range bit select is not a select.
            if (bitIndex >= static_cast<uint64_t>(std::max<int32_t>(graph.valueWidth(base), 0)))
            {
                return std::nullopt;
            }
            // kShl(kConstant 1, ptr): operands[0] is the shifted value,
            // operands[1] the shift amount. A constant shift amount makes
            // this a constant fold, not a select.
            const auto one = getConstantUInt64(graph, baseDef.operands()[0], nullptr);
            if (!one || *one != 1)
            {
                return std::nullopt;
            }
            if (getConstantUInt64(graph, baseDef.operands()[1], nullptr))
            {
                return std::nullopt;
            }
            return EqConstMatch{baseDef.operands()[1], bitIndex, true};
        }

        // Select condition of a read-select tree term: either kEq(ptr, c) or
        // the shl-onehot bit select above.
        std::optional<EqConstMatch> matchSelectCondition(const Graph &graph, ValueId value)
        {
            if (const auto eq = matchEqConstant(graph, value))
            {
                return eq;
            }
            return matchShlOnehotBit(graph, value);
        }

        class ReadSelectRewriter
        {
        public:
            ReadSelectRewriter(Graph &graph,
                               const std::unordered_map<ValueId, const LaneGroupEval *, ValueIdHash> &wideReads,
                               const std::unordered_map<std::string, const LaneGroupEval *> &memGroups,
                               bool arrayMode)
                : graph_(graph), wideReads_(wideReads), memGroups_(memGroups), arrayMode_(arrayMode)
            {
            }

            void run(ReadSelectStats &stats)
            {
                const std::vector<OperationId> ops(graph_.operations().begin(), graph_.operations().end());
                for (const OperationId opId : ops)
                {
                    if (!opId.valid() || consumed_.count(opId) != 0)
                    {
                        continue;
                    }
                    const Operation op = graph_.getOperation(opId);
                    if (op.results().size() != 1 || !rootIsLive(op.results().front()))
                    {
                        continue;
                    }
                    if (op.kind() == OperationKind::kMux)
                    {
                        tryMuxChain(op, stats);
                    }
                    else if (op.kind() == OperationKind::kOr)
                    {
                        tryOrTree(op, stats);
                    }
                }
            }

        private:
            bool rootIsLive(ValueId root) const
            {
                if (!graph_.getValue(root).users().empty())
                {
                    return true;
                }
                for (const auto &port : graph_.outputPorts())
                {
                    if (port.value == root)
                    {
                        return true;
                    }
                }
                return false;
            }

            std::optional<LaneSliceMatch> matchLaneSlice(ValueId value)
            {
                const auto unwrapped = unwrapAssignValue(graph_, value);
                if (!unwrapped)
                {
                    return std::nullopt;
                }
                const OperationId defOpId = graph_.valueDef(*unwrapped);
                if (!defOpId.valid())
                {
                    return std::nullopt;
                }
                const Operation defOp = graph_.getOperation(defOpId);
                if (arrayMode_)
                {
                    // Array-mode leaf: kMemoryReadPort(mem, constant address)
                    // produced by phase 1 lane-read replacement.
                    if (defOp.kind() != OperationKind::kMemoryReadPort || defOp.operands().size() != 1)
                    {
                        return std::nullopt;
                    }
                    const auto memSymbol = getStringAttr(defOp, "memSymbol");
                    if (!memSymbol)
                    {
                        return std::nullopt;
                    }
                    const auto it = memGroups_.find(*memSymbol);
                    if (it == memGroups_.end())
                    {
                        return std::nullopt;
                    }
                    const LaneGroupEval *group = it->second;
                    const int32_t width = graph_.valueWidth(*unwrapped);
                    if (width != group->width)
                    {
                        return std::nullopt;
                    }
                    const auto addr = getConstantUInt64(graph_, defOp.operands().front(), nullptr);
                    if (!addr || !std::binary_search(group->bucket.begin(), group->bucket.end(), *addr))
                    {
                        return std::nullopt;
                    }
                    return LaneSliceMatch{group, *addr, width};
                }
                if (defOp.kind() != OperationKind::kSliceStatic || defOp.operands().size() != 1)
                {
                    return std::nullopt;
                }
                const auto sliceStart = getAttr<int64_t>(defOp, "sliceStart");
                const auto sliceEnd = getAttr<int64_t>(defOp, "sliceEnd");
                if (!sliceStart || !sliceEnd || *sliceStart < 0 || *sliceEnd < *sliceStart)
                {
                    return std::nullopt;
                }
                const auto it = wideReads_.find(defOp.operands().front());
                if (it == wideReads_.end())
                {
                    return std::nullopt;
                }
                const LaneGroupEval *group = it->second;
                const int32_t width = static_cast<int32_t>(*sliceEnd - *sliceStart + 1);
                if (width != group->width || *sliceStart % width != 0)
                {
                    return std::nullopt;
                }
                const uint64_t laneIndex = static_cast<uint64_t>(*sliceStart) / static_cast<uint64_t>(width);
                if (!std::binary_search(group->bucket.begin(), group->bucket.end(), laneIndex))
                {
                    return std::nullopt;
                }
                return LaneSliceMatch{group, laneIndex, width};
            }

            bool isZeroConstant(ValueId value, int32_t width)
            {
                const auto constant = getConstantUInt64(graph_, value, nullptr);
                return constant && *constant == 0 && graph_.valueWidth(value) == width;
            }

            bool coverageComplete(const LaneGroupEval *group,
                                  const std::map<uint64_t, ValueId> &branches) const
            {
                // The dynamic read reproduces the chain exactly only when
                // every span segment has a branch: hole segments read as
                // hold bits of the wide register, not the chain's default.
                if (branches.size() != group->span)
                {
                    return false;
                }
                for (uint64_t idx = 0; idx < group->span; ++idx)
                {
                    if (branches.find(idx) == branches.end())
                    {
                        return false;
                    }
                }
                return true;
            }

            void replaceRoot(const Operation &rootOp,
                             const LaneGroupEval *group,
                             ValueId ptr,
                             int32_t width,
                             std::size_t retiredOps,
                             ReadSelectStats &stats)
            {
                const ValueId rootValue = rootOp.results().front();
                if (arrayMode_)
                {
                    // One kMemoryReadPort with the select pointer as the
                    // (dynamic) row address; no offset scaling.
                    const ValueId read = graph_.createValue(graph_.makeInternalValSym(), width,
                                                            graph_.valueSigned(rootValue),
                                                            graph_.valueType(rootValue));
                    const OperationId readOp =
                        graph_.createOperation(OperationKind::kMemoryReadPort, graph_.makeInternalOpSym());
                    graph_.addOperand(readOp, ptr);
                    graph_.addResult(readOp, read);
                    graph_.setAttr(readOp, "memSymbol", group->wideName);
                    const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), "read-select-row");
                    graph_.setOpSrcLoc(readOp, srcLoc);
                    graph_.setValueSrcLoc(read, srcLoc);
                    for (const auto &port : graph_.outputPorts())
                    {
                        if (port.value == rootValue)
                        {
                            graph_.bindOutputPort(port.name, read);
                        }
                    }
                    graph_.replaceAllUses(rootValue, read);
                    ++stats.trees;
                    stats.opsRetired += retiredOps;
                    stats.opsCreated += 1;
                    return;
                }
                // offset = ptr * width (shift for powers of two).
                ValueId offset = ptr;
                std::size_t created = 1;
                const int32_t ptrWidth = graph_.valueWidth(ptr);
                if (width > 1)
                {
                    if ((width & (width - 1)) == 0)
                    {
                        int32_t shift = 0;
                        while ((1 << shift) < width)
                        {
                            ++shift;
                        }
                        const int32_t constWidth = ptrWidth > 0 ? ptrWidth : 1;
                        const ValueId shiftConst = createConstantValue(
                            graph_, constWidth,
                            std::to_string(constWidth) + "'d" + std::to_string(shift),
                            "read-select-shift");
                        offset = createBinaryOp(graph_, OperationKind::kShl, ptr, shiftConst,
                                                ptrWidth + shift, false, ValueType::Logic, "read-select-offset");
                        created += 2;
                    }
                    else
                    {
                        const int32_t constWidth = ptrWidth > 0 ? ptrWidth : 1;
                        const ValueId widthConst = createConstantValue(
                            graph_, constWidth,
                            std::to_string(constWidth) + "'d" + std::to_string(width),
                            "read-select-width");
                        offset = createBinaryOp(graph_, OperationKind::kMul, ptr, widthConst,
                                                ptrWidth + 8, false, ValueType::Logic, "read-select-offset");
                        created += 2;
                    }
                }
                const ValueId sliced = graph_.createValue(graph_.makeInternalValSym(), width,
                                                          graph_.valueSigned(rootValue),
                                                          graph_.valueType(rootValue));
                const OperationId sliceOp =
                    graph_.createOperation(OperationKind::kSliceDynamic, graph_.makeInternalOpSym());
                graph_.addOperand(sliceOp, group->wideReadValue);
                graph_.addOperand(sliceOp, offset);
                graph_.addResult(sliceOp, sliced);
                graph_.setAttr(sliceOp, "sliceWidth", static_cast<int64_t>(width));
                const SrcLoc srcLoc = makeTransformSrcLoc(std::string(kPassId), "read-select-slice");
                graph_.setOpSrcLoc(sliceOp, srcLoc);
                graph_.setValueSrcLoc(sliced, srcLoc);
                for (const auto &port : graph_.outputPorts())
                {
                    if (port.value == rootValue)
                    {
                        graph_.bindOutputPort(port.name, sliced);
                    }
                }
                graph_.replaceAllUses(rootValue, sliced);
                ++stats.trees;
                stats.opsRetired += retiredOps;
                stats.opsCreated += created;
            }

            void tryMuxChain(const Operation &rootOp, ReadSelectStats &stats)
            {
                std::map<uint64_t, ValueId> branches;
                const LaneGroupEval *group = nullptr;
                ValueId ptr;
                ValueId cursor = rootOp.results().front();
                ValueId defaultValue;
                std::size_t treeOps = 0;
                bool ok = true;
                while (true)
                {
                    const OperationId curDefId = graph_.valueDef(cursor);
                    if (!curDefId.valid())
                    {
                        defaultValue = cursor;
                        break;
                    }
                    const Operation curOp = graph_.getOperation(curDefId);
                    const auto operands = curOp.operands();
                    if (curOp.kind() != OperationKind::kMux || operands.size() != 3 ||
                        graph_.valueWidth(operands[0]) != 1)
                    {
                        defaultValue = cursor;
                        break;
                    }
                    const auto eq = matchEqConstant(graph_, operands[0]);
                    if (!eq || (ptr.valid() && eq->ptr != ptr))
                    {
                        ok = false;
                        break;
                    }
                    ptr = eq->ptr;
                    const auto slice = matchLaneSlice(operands[1]);
                    if (!slice || (group != nullptr && slice->group != group))
                    {
                        ok = false;
                        break;
                    }
                    group = slice->group;
                    if (slice->laneIndex != eq->constant ||
                        !branches.emplace(eq->constant, operands[1]).second)
                    {
                        ok = false;
                        break;
                    }
                    consumed_.insert(curDefId);
                    ++treeOps; // the mux itself
                    ++treeOps; // its eq
                    cursor = operands[2];
                    if (branches.size() > 4096)
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok || group == nullptr || branches.size() < 3 || !defaultValue.valid())
                {
                    return;
                }
                if (!isZeroConstant(defaultValue, group->width))
                {
                    return;
                }
                if (!coverageComplete(group, branches))
                {
                    return;
                }
                replaceRoot(rootOp, group, ptr, group->width, treeOps, stats);
            }

            void tryOrTree(const Operation &rootOp, ReadSelectStats &stats)
            {
                // Flatten the kOr chain (DFS, operands first).
                std::vector<ValueId> terms;
                {
                    std::vector<ValueId> stack{rootOp.results().front()};
                    std::unordered_set<ValueId, ValueIdHash> seen;
                    bool flattenOk = true;
                    while (!stack.empty())
                    {
                        const ValueId current = stack.back();
                        stack.pop_back();
                        if (!seen.insert(current).second)
                        {
                            continue;
                        }
                        const OperationId defId = graph_.valueDef(current);
                        if (!defId.valid())
                        {
                            flattenOk = false;
                            break;
                        }
                        const Operation def = graph_.getOperation(defId);
                        if (def.kind() == OperationKind::kOr && def.operands().size() == 2)
                        {
                            stack.push_back(def.operands()[1]);
                            stack.push_back(def.operands()[0]);
                            consumed_.insert(defId);
                            continue;
                        }
                        terms.push_back(current);
                    }
                    if (!flattenOk)
                    {
                        return;
                    }
                }
                std::map<uint64_t, ValueId> branches;
                const LaneGroupEval *group = nullptr;
                ValueId ptr;
                std::size_t treeOps = 0;
                bool sawShlOnehot = false;
                bool ok = true;
                for (const ValueId term : terms)
                {
                    uint64_t constant = 0;
                    ValueId eqValue;
                    ValueId sliceValue;
                    const OperationId termDefId = graph_.valueDef(term);
                    if (!termDefId.valid())
                    {
                        ok = false;
                        break;
                    }
                    const Operation termDef = graph_.getOperation(termDefId);
                    const auto operands = termDef.operands();
                    if (termDef.kind() == OperationKind::kMux && operands.size() == 3)
                    {
                        const auto eq = matchSelectCondition(graph_, operands[0]);
                        if (!eq || !isZeroConstant(operands[2], graph_.valueWidth(operands[1])))
                        {
                            ok = false;
                            break;
                        }
                        constant = eq->constant;
                        eqValue = operands[0];
                        sliceValue = operands[1];
                        treeOps += 2; // mux + select (eq or bit slice)
                    }
                    else if (termDef.kind() == OperationKind::kAnd && operands.size() == 2)
                    {
                        // kAnd(kReplicate(sel_i), slice_i) in either order;
                        // at W == 1 the bare kAnd(sel_i, slice_i) form
                        // appears (no replicate needed). sel_i is either
                        // kEq(ptr, i) or a shl-onehot bit select.
                        for (std::size_t side = 0; side < 2 && eqValue == ValueId{}; ++side)
                        {
                            const OperationId repDefId = graph_.valueDef(operands[side]);
                            if (!repDefId.valid())
                            {
                                continue;
                            }
                            const Operation repDef = graph_.getOperation(repDefId);
                            if (repDef.kind() == OperationKind::kReplicate &&
                                repDef.operands().size() == 1)
                            {
                                const auto eq = matchSelectCondition(graph_, repDef.operands().front());
                                if (!eq)
                                {
                                    continue;
                                }
                                constant = eq->constant;
                                eqValue = repDef.operands().front();
                                sliceValue = operands[1 - side];
                                treeOps += 3; // and + replicate + select
                                continue;
                            }
                            if (graph_.valueWidth(operands[1 - side]) != 1)
                            {
                                continue;
                            }
                            const auto eq = matchSelectCondition(graph_, operands[side]);
                            if (!eq)
                            {
                                continue;
                            }
                            constant = eq->constant;
                            eqValue = operands[side];
                            sliceValue = operands[1 - side];
                            treeOps += 2; // and + select
                        }
                        if (!eqValue.valid())
                        {
                            ok = false;
                            break;
                        }
                    }
                    else
                    {
                        ok = false;
                        break;
                    }
                    const auto eq = matchSelectCondition(graph_, eqValue);
                    if (!eq || (ptr.valid() && eq->ptr != ptr))
                    {
                        ok = false;
                        break;
                    }
                    ptr = eq->ptr;
                    sawShlOnehot = sawShlOnehot || eq->shlOnehot;
                    const auto slice = matchLaneSlice(sliceValue);
                    if (!slice || (group != nullptr && slice->group != group))
                    {
                        ok = false;
                        break;
                    }
                    group = slice->group;
                    if (slice->laneIndex != constant ||
                        !branches.emplace(constant, sliceValue).second)
                    {
                        ok = false;
                        break;
                    }
                    consumed_.insert(termDefId);
                    if (branches.size() > 4096)
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok || group == nullptr || branches.size() < 3)
                {
                    return;
                }
                if (!coverageComplete(group, branches))
                {
                    return;
                }
                if (sawShlOnehot)
                {
                    // The shl-onehot bit select is rewritten only when the
                    // pointer exactly covers the span (ptrWidth == log2(span),
                    // so the dynamic address can never go out of range).
                    const int32_t ptrWidth = graph_.valueWidth(ptr);
                    if (ptrWidth <= 0 || ptrWidth >= 64 ||
                        (UINT64_C(1) << ptrWidth) != group->span)
                    {
                        return;
                    }
                }
                treeOps += terms.size() - 1; // the kOr ops
                replaceRoot(rootOp, group, ptr, group->width, treeOps, stats);
            }

            Graph &graph_;
            const std::unordered_map<ValueId, const LaneGroupEval *, ValueIdHash> &wideReads_;
            const std::unordered_map<std::string, const LaneGroupEval *> &memGroups_;
            bool arrayMode_ = false;
            std::unordered_set<OperationId, OperationIdHash> consumed_;
        };

        // ------------------------------------------------------------------
        // Group report JSON (same shape as reg-to-mem's group report).
        // ------------------------------------------------------------------
        void appendJsonString(std::ostringstream &out, std::string_view text)
        {
            out << '"';
            for (const char ch : text)
            {
                switch (ch)
                {
                case '"':
                    out << "\\\"";
                    break;
                case '\\':
                    out << "\\\\";
                    break;
                case '\n':
                    out << "\\n";
                    break;
                case '\r':
                    out << "\\r";
                    break;
                case '\t':
                    out << "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                    {
                        out << "\\u";
                        const char *digits = "0123456789abcdef";
                        out << '0' << '0' << digits[(ch >> 4) & 0xF] << digits[ch & 0xF];
                    }
                    else
                    {
                        out << ch;
                    }
                    break;
                }
            }
            out << '"';
        }

        std::string buildGroupReportsJson(const std::vector<GroupReportRecord> &records,
                                          const ReadSelectStats &readSelect)
        {
            struct ReasonSummary
            {
                std::size_t groups = 0;
                std::size_t elements = 0;
            };
            std::map<std::string, ReasonSummary> byReason;
            std::map<std::string, ReasonSummary> byOutcome;

            std::ostringstream out;
            out << "{\"groups\":[";
            bool first = true;
            for (const GroupReportRecord &record : records)
            {
                if (!first)
                {
                    out << ',';
                }
                first = false;
                out << "{\"graph\":";
                appendJsonString(out, record.graph);
                out << ",\"group_id\":" << record.groupId;
                out << ",\"discovery\":";
                appendJsonString(out, record.discovery);
                out << ",\"group\":";
                appendJsonString(out, record.group);
                out << ",\"module\":";
                appendJsonString(out, record.module);
                out << ",\"output_mode\":";
                appendJsonString(out, record.outputMode);
                out << ",\"element_width\":" << record.elementWidth;
                out << ",\"element_count\":" << record.elementCount;
                out << ",\"lane_count\":" << record.laneCount;
                out << ",\"outcome\":";
                appendJsonString(out, record.outcome);
                out << ",\"reject_reason\":";
                appendJsonString(out, record.rejectReason);
                out << ",\"reject_detail\":";
                appendJsonString(out, record.rejectDetail);
                out << '}';

                ++byOutcome[record.outcome].groups;
                byOutcome[record.outcome].elements += record.elementCount;
                if (!record.rejectReason.empty())
                {
                    ++byReason[record.rejectReason].groups;
                    byReason[record.rejectReason].elements += record.elementCount;
                }
            }
            out << "],\"summary\":{\"by_reason\":{";
            bool firstReason = true;
            for (const auto &[reason, summary] : byReason)
            {
                if (!firstReason)
                {
                    out << ',';
                }
                firstReason = false;
                appendJsonString(out, reason);
                out << ":{\"groups\":" << summary.groups << ",\"elements\":" << summary.elements << '}';
            }
            out << "},\"by_outcome\":{";
            bool firstOutcome = true;
            for (const auto &[outcome, summary] : byOutcome)
            {
                if (!firstOutcome)
                {
                    out << ',';
                }
                firstOutcome = false;
                appendJsonString(out, outcome);
                out << ":{\"groups\":" << summary.groups << ",\"elements\":" << summary.elements << '}';
            }
            out << "},\"read_select\":{\"trees\":" << readSelect.trees
                << ",\"ops_retired\":" << readSelect.opsRetired
                << ",\"ops_created\":" << readSelect.opsCreated << "}}}";
            return out.str();
        }
    } // namespace

    LaneAggregatePass::LaneAggregatePass()
        : Pass("lane-aggregate", "lane-aggregate",
               "Merge firtool-flattened per-lane scalar registers into wide vector registers")
    {
    }

    LaneAggregatePass::LaneAggregatePass(LaneAggregateOptions options)
        : Pass("lane-aggregate", "lane-aggregate",
               "Merge firtool-flattened per-lane scalar registers into wide vector registers"),
          options_(std::move(options))
    {
    }

    PassResult LaneAggregatePass::run()
    {
        PassResult result;
        if (options_.minLanes < 2)
        {
            error("lane-aggregate min_lanes must be >= 2");
            result.failed = true;
            return result;
        }
        const bool arrayMode = options_.outputMode == LaneAggregateOutputMode::Array;

        std::vector<GroupReportRecord> reports;
        std::size_t totalMergedGroups = 0;
        std::size_t totalMergedLanes = 0;
        ReadSelectStats readSelectStats;

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            Graph &graph = *entry.second;

            // Pre-pass: expand kReduce{Or,And,Xor}(kConcat(...)) into
            // element-wise trees (semantics-preserving) so that packed
            // reductions do not block signature bucketing. Array mode
            // rewrites uniform-width matches to kArrayReduce* instead.
            if (normalizeReduceConcat(graph, arrayMode))
            {
                result.changed = true;
            }

            const GraphIndexes index = buildGraphIndexes(graph);

            // Name grouping over kRegister symbols: mask every numeric
            // segment, then keep groups where exactly one segment varies.
            struct RawMember
            {
                std::string name;
                std::vector<uint64_t> segments;
            };
            std::map<std::string, std::vector<RawMember>> rawByKey;
            for (const auto &[name, regOp] : index.regOps)
            {
                (void)regOp;
                const auto parsed = parseLaneName(name);
                if (!parsed)
                {
                    continue;
                }
                rawByKey[parsed->maskedKey].push_back(RawMember{name, parsed->segmentValues});
            }

            std::vector<LaneGroupEval> groups;
            std::unordered_map<std::string, std::size_t> groupByKey;
            RegLaneMap regLane;
            groups.reserve(rawByKey.size());
            for (auto &[key, raw] : rawByKey)
            {
                if (raw.size() < 2)
                {
                    continue;
                }
                // Pick the lane segment: the segment position with the most
                // distinct values (ties resolve to the leftmost position).
                // Members are then sub-grouped by the values of all other
                // segments, so each emitted lane group has exactly one
                // varying segment. A masked group with several varying
                // segments (e.g. `data16_*$needCheck0Reg_*` bank x entry, or
                // `robEntries_*_uopNum_T_*` index x chisel-temp-id) is thus
                // decomposed into per-slice lane groups instead of being
                // rejected wholesale; only a group that decomposes into
                // nothing (e.g. a diagonal `arr_0_0, arr_1_1, ...` matrix) is
                // reported as multi_varying_segment.
                const std::size_t segmentCount = raw.front().segments.size();
                std::vector<std::unordered_set<uint64_t>> distinct(segmentCount);
                for (const RawMember &member : raw)
                {
                    for (std::size_t s = 0; s < segmentCount; ++s)
                    {
                        distinct[s].insert(member.segments[s]);
                    }
                }
                std::size_t laneSegment = 0;
                std::size_t laneSegmentDistinct = 0;
                std::size_t varyingSegments = 0;
                for (std::size_t s = 0; s < segmentCount; ++s)
                {
                    if (distinct[s].size() > 1)
                    {
                        ++varyingSegments;
                    }
                    if (distinct[s].size() > laneSegmentDistinct)
                    {
                        laneSegmentDistinct = distinct[s].size();
                        laneSegment = s;
                    }
                }
                if (laneSegmentDistinct <= 1)
                {
                    // All segments constant: the names would be identical.
                    continue;
                }
                // Sub-group members by the values of every segment except the
                // lane segment; each sub-group has exactly one varying segment.
                std::map<std::vector<uint64_t>, std::vector<const RawMember *>> subGroups;
                for (const RawMember &member : raw)
                {
                    std::vector<uint64_t> subKey;
                    subKey.reserve(segmentCount - 1);
                    for (std::size_t s = 0; s < segmentCount; ++s)
                    {
                        if (s != laneSegment)
                        {
                            subKey.push_back(member.segments[s]);
                        }
                    }
                    subGroups[std::move(subKey)].push_back(&member);
                }
                std::size_t emitted = 0;
                for (const auto &[subKey, members] : subGroups)
                {
                    (void)subKey;
                    if (members.size() < 2)
                    {
                        continue;
                    }
                    LaneGroupEval group;
                    group.maskedKey = specializeMaskedKey(key, members.front()->segments, laneSegment);
                    group.key = group.maskedKey;
                    group.laneSegment = laneSegment;
                    bool duplicateIndex = false;
                    for (const RawMember *member : members)
                    {
                        if (!group.members
                                 .emplace(member->segments[laneSegment], member->name)
                                 .second)
                        {
                            duplicateIndex = true;
                            break;
                        }
                    }
                    if (duplicateIndex)
                    {
                        continue;
                    }
                    for (const auto &[idx, name] : group.members)
                    {
                        regLane.emplace(name, std::make_pair(group.key, idx));
                    }
                    groupByKey.emplace(group.key, groups.size());
                    groups.push_back(std::move(group));
                    ++emitted;
                }
                if (emitted == 0 && varyingSegments > 1)
                {
                    LaneGroupEval group;
                    group.key = key;
                    group.maskedKey = key;
                    group.outcome = "rejected";
                    group.rejectReason = "multi_varying_segment";
                    group.rejectDetail = "segments=" + std::to_string(segmentCount);
                    for (const RawMember &member : raw)
                    {
                        // Populate members for report/module naming only; the
                        // group is never evaluated further. Keys are synthetic
                        // (insertion order) so the count stays accurate.
                        group.members.emplace(static_cast<uint64_t>(group.members.size()),
                                              member.name);
                    }
                    groupByKey.emplace(key, groups.size());
                    groups.push_back(std::move(group));
                }
            }

            for (LaneGroupEval &group : groups)
            {
                if (!group.rejectReason.empty())
                {
                    continue; // already rejected during grouping
                }
                evaluateGroup(graph, index, regLane, options_, keepDeclaredSymbols(), group);
            }
            if (arrayMode)
            {
                // init_unmappable must be decided BEFORE resolveSiblingDeps:
                // flipping a group to rejected after the fixpoint would let a
                // dependent group reach materialization with an unmerged
                // sibling (the same failure class as a missing sibling dep).
                for (LaneGroupEval &group : groups)
                {
                    if (group.merge && !group.initValues.empty() && group.width > 64)
                    {
                        // Defensive: per-lane init values are parsed as uint64
                        // in evaluateGroup, so lanes wider than 64 bits can
                        // never reach here with an init; guard the kMemory
                        // literal mapping anyway.
                        group.merge = false;
                        group.outcome = "rejected";
                        group.rejectReason = "init_unmappable";
                        group.rejectDetail =
                            "lane width > 64 cannot map initValue to kMemory literal init";
                    }
                }
            }
            resolveSiblingDeps(groups, groupByKey);

            // Phase C1: create the merged storage + one whole-array read per
            // merged group (sibling references resolve against these). Wide
            // mode creates a wide kRegister + kRegisterReadPort; array mode
            // creates a kMemory (width = W, row = span) + kArrayReadAllPort.
            for (LaneGroupEval &group : groups)
            {
                if (!group.merge)
                {
                    continue;
                }
                const int64_t wideWidth =
                    static_cast<int64_t>(group.span) * static_cast<int64_t>(group.width);
                std::string base;
                base.reserve(group.maskedKey.size());
                for (std::size_t p = 0; p < group.maskedKey.size();)
                {
                    if (group.maskedKey.compare(p, 2, "_*") == 0)
                    {
                        p += 2;
                        continue;
                    }
                    if (group.maskedKey[p] == '*')
                    {
                        ++p;
                        continue;
                    }
                    base.push_back(group.maskedKey[p++]);
                }
                base += "__laneagg";
                std::string name = base;
                for (std::size_t suffix = 1;
                     graph.findOperation(name).valid() || graph.findValue(name).valid(); ++suffix)
                {
                    name = base + "_" + std::to_string(suffix);
                }
                group.wideName = name;
                if (arrayMode)
                {
                    const SrcLoc memLoc = makeTransformSrcLoc(std::string(kPassId), "array-storage");
                    group.wideRegOp =
                        graph.createOperation(OperationKind::kMemory, graph.internSymbol(name));
                    graph.setAttr(group.wideRegOp, "width", static_cast<int64_t>(group.width));
                    graph.setAttr(group.wideRegOp, "row", static_cast<int64_t>(group.span));
                    graph.setAttr(group.wideRegOp, "isSigned", group.isSigned);
                    graph.setOpSrcLoc(group.wideRegOp, memLoc);
                    if (!group.initValues.empty())
                    {
                        // One literal init entry per row: bucket lanes get
                        // their value, hole rows get zero (exactly mirrors
                        // the wide packed init table).
                        std::vector<std::string> initKinds;
                        std::vector<std::string> initFiles;
                        std::vector<std::string> initValues;
                        std::vector<int64_t> initStarts;
                        std::vector<int64_t> initLens;
                        initKinds.reserve(group.span);
                        initFiles.reserve(group.span);
                        initValues.reserve(group.span);
                        initStarts.reserve(group.span);
                        initLens.reserve(group.span);
                        for (uint64_t row = 0; row < group.span; ++row)
                        {
                            uint64_t rowInit = 0;
                            const auto bucketIt =
                                std::lower_bound(group.bucket.begin(), group.bucket.end(), row);
                            if (bucketIt != group.bucket.end() && *bucketIt == row)
                            {
                                rowInit = group.initValues[static_cast<std::size_t>(
                                    bucketIt - group.bucket.begin())];
                            }
                            initKinds.push_back("literal");
                            initFiles.emplace_back();
                            initValues.push_back(makeHexLiteral(
                                group.width,
                                slang::SVInt(static_cast<slang::bitwidth_t>(group.width), rowInit, false)));
                            initStarts.push_back(static_cast<int64_t>(row));
                            initLens.push_back(1);
                        }
                        graph.setAttr(group.wideRegOp, "initKind", std::move(initKinds));
                        graph.setAttr(group.wideRegOp, "initFile", std::move(initFiles));
                        graph.setAttr(group.wideRegOp, "initValue", std::move(initValues));
                        graph.setAttr(group.wideRegOp, "initStart", std::move(initStarts));
                        graph.setAttr(group.wideRegOp, "initLen", std::move(initLens));
                    }

                    group.wideReadValue = graph.createValue(graph.makeInternalValSym(),
                                                            static_cast<int32_t>(wideWidth),
                                                            group.isSigned, ValueType::Logic);
                    group.wideReadOp =
                        graph.createOperation(OperationKind::kArrayReadAllPort, graph.makeInternalOpSym());
                    graph.addResult(group.wideReadOp, group.wideReadValue);
                    graph.setAttr(group.wideReadOp, "memSymbol", name);
                    const SrcLoc readLoc = makeTransformSrcLoc(std::string(kPassId), "array-read-all");
                    graph.setOpSrcLoc(group.wideReadOp, readLoc);
                    graph.setValueSrcLoc(group.wideReadValue, readLoc);
                    continue;
                }
                const SrcLoc regLoc = makeTransformSrcLoc(std::string(kPassId), "wide-register");
                group.wideRegOp =
                    graph.createOperation(OperationKind::kRegister, graph.internSymbol(name));
                graph.setAttr(group.wideRegOp, "width", wideWidth);
                graph.setAttr(group.wideRegOp, "isSigned", group.isSigned);
                graph.setOpSrcLoc(group.wideRegOp, regLoc);
                if (!group.initValues.empty())
                {
                    // Pack per-lane init values exactly like a cone constant
                    // table: lane i's init in segment [i*W +: W], holes zero.
                    const slang::SVInt initTable =
                        packedConstantTable(group.span, group.width, group.bucket, group.initValues);
                    graph.setAttr(group.wideRegOp, "initValue",
                                  makeHexLiteral(static_cast<int32_t>(wideWidth), initTable));
                }

                group.wideReadValue = graph.createValue(graph.makeInternalValSym(),
                                                        static_cast<int32_t>(wideWidth),
                                                        group.isSigned, ValueType::Logic);
                group.wideReadOp =
                    graph.createOperation(OperationKind::kRegisterReadPort, graph.makeInternalOpSym());
                graph.addResult(group.wideReadOp, group.wideReadValue);
                graph.setAttr(group.wideReadOp, "regSymbol", name);
                graph.setOpSrcLoc(group.wideReadOp, regLoc);
                graph.setValueSrcLoc(group.wideReadValue, regLoc);
            }

            // Phase C2: materialize merged cones and write ports for ALL
            // merged groups first. Re-analysis walks the untouched per-lane
            // cones; lane cleanup happens in C3 so that a sibling group's
            // erasures cannot invalidate another group's re-analysis.
            for (LaneGroupEval &group : groups)
            {
                if (!group.merge)
                {
                    continue;
                }
                std::string materializeError;
                if (!materializeGroup(graph, index, groups, groupByKey, regLane, group, arrayMode,
                                      options_.laneParamLeaves, materializeError))
                {
                    error(graph, "lane-aggregate failed to materialize group " +
                                     group.maskedKey + ": " + materializeError);
                    result.failed = true;
                    group.merge = false;
                    group.outcome = "rejected";
                    group.rejectReason = "rewrite_failed";
                    group.rejectDetail = materializeError;
                }
            }

            // Phase C3: replace lane reads with slices and erase the old lane
            // registers / write ports.
            for (LaneGroupEval &group : groups)
            {
                if (!group.merge)
                {
                    continue;
                }
                std::string eraseError;
                if (!eraseGroupLanes(graph, index, group, arrayMode, eraseError))
                {
                    error(graph, "lane-aggregate failed to erase lanes of group " +
                                     group.maskedKey + ": " + eraseError);
                    result.failed = true;
                    group.outcome = "rejected";
                    group.rejectReason = "rewrite_failed";
                    group.rejectDetail = eraseError;
                    continue;
                }
                ++totalMergedGroups;
                totalMergedLanes += group.bucket.size();
                result.changed = true;
            }

            // Phase 2 (read side): rewrite select trees over the lane slices
            // into kSliceDynamic reads of the wide registers (wide mode) or
            // into single kMemoryReadPort dynamic-row reads (array mode).
            // Runs after C3 so every merged lane read exists as a
            // kSliceStatic / kMemoryReadPort(const addr) leaf; trees that
            // touch unmerged lanes (scalar read ports) simply do not match.
            if (options_.readSelect)
            {
                std::unordered_map<ValueId, const LaneGroupEval *, ValueIdHash> wideReads;
                std::unordered_map<std::string, const LaneGroupEval *> memGroups;
                for (const LaneGroupEval &group : groups)
                {
                    if (!group.merge)
                    {
                        continue;
                    }
                    if (arrayMode)
                    {
                        if (!group.wideName.empty())
                        {
                            memGroups.emplace(group.wideName, &group);
                        }
                    }
                    else if (group.wideReadValue.valid())
                    {
                        wideReads.emplace(group.wideReadValue, &group);
                    }
                }
                if (!wideReads.empty() || !memGroups.empty())
                {
                    ReadSelectRewriter rewriter(graph, wideReads, memGroups, arrayMode);
                    rewriter.run(readSelectStats);
                    if (readSelectStats.trees != 0)
                    {
                        result.changed = true;
                    }
                }
            }

            std::size_t groupId = 0;
            for (const LaneGroupEval &group : groups)
            {
                GroupReportRecord record;
                record.graph = graph.symbol();
                record.groupId = ++groupId;
                record.discovery = "name_pattern";
                record.group = group.maskedKey;
                record.module = groupModuleName(graph, group);
                record.outputMode = arrayMode ? "array" : "wide";
                record.elementWidth = group.width;
                record.elementCount = group.members.size();
                record.laneCount = group.bucket.size();
                record.outcome = group.outcome;
                record.rejectReason = group.rejectReason;
                if (group.merge && group.exactFallbackUsed)
                {
                    // Rescued by the exact-all fallback: distinguish from a
                    // signature-majority merge (a group the fallback could
                    // not save reports skipped/no_majority_exact instead).
                    record.rejectReason = "no_majority_exact";
                }
                record.rejectDetail = group.rejectDetail;
                reports.push_back(std::move(record));
            }
        }

        const std::string summary = "lane-aggregate summary merged_groups=" +
                                    std::to_string(totalMergedGroups) +
                                    " merged_lanes=" + std::to_string(totalMergedLanes) +
                                    " output_mode=" + std::string(arrayMode ? "array" : "wide") +
                                    " read_select_trees=" + std::to_string(readSelectStats.trees) +
                                    " read_select_ops_retired=" + std::to_string(readSelectStats.opsRetired);
        info(summary);
        logInfo(summary);
        if (!options_.outputKey.empty())
        {
            setSessionValue(options_.outputKey, buildGroupReportsJson(reports, readSelectStats),
                            "lane-aggregate.reports");
        }
        return result;
    }

} // namespace wolvrix::lib::transform
