// array-lower: expand the twelve array-value ops (grh-ir.md section 6.10) back
// into plain wide-scalar form. This is the semantic inverse of lane-aggregate's
// array output mode, used (a) as a degradation step in front of consumers that
// do not support the array ops (SystemVerilog emit, legacy toolchains) and
// (b) as an equivalence witness for dual-shape difftests (array shape vs.
// lowered shape must produce identical results). See
// docs/transform/array-lower.md.
//
// Expansion rules (bit-exact, lane i occupies [i*W +: W], lane 0 in the LSBs):
//   kArrayReadAllPort  -> one kMemoryReadPort per row (constant address) +
//                         one kConcat (row R-1 in the MSBs, row 0 in the LSBs;
//                         row == 1 uses the read port result directly).
//   kArrayWritePort    -> one kMemoryWritePort per row: updateCond =
//                         kSliceStatic(laneMask, i, i), addr = kConstant(i),
//                         data = kSliceStatic(data, i*W, i*W+W-1), mask =
//                         all-ones constant (W bits); events and eventEdge are
//                         forwarded verbatim. memoryWrite.priority* attrs are
//                         DROPPED (with a warning): the expanded ports write
//                         pairwise distinct constant addresses and can never
//                         collide with each other, so intra-group ordering is
//                         meaningless for them, and keeping one port's
//                         priority on row fresh ports would also violate the
//                         priority-group uniqueness/continuity invariant
//                         ([0, N) per group). Order against OTHER grouped
//                         ports with dynamic addresses becomes unordered --
//                         the warning makes that visible.
//   kArrayMux          -> m = kConcat over lanes of kReplicate(kSliceStatic
//                         (sel, i, i), W); res = kOr(kAnd(t, m), kAnd(f,
//                         kNot(m))).
//   kArrayReduceOr/And/Xor -> single kReduceOr/kReduceAnd/kReduceXor over the
//                         full packed operand (per-lane then cross-lane
//                         reduction equals full-width reduction by
//                         associativity).
//   kArrayReduceLanesOr/And/Xor -> per lane kReduceOr/kReduceAnd/kReduceXor
//                         over kSliceStatic(data, i*W, i*W+W-1), plus one
//                         kConcat of the per-lane bits (lane R-1 in the MSBs,
//                         lane 0 in the LSBs; row == 1 uses the bit directly).
//   kArrayBroadcast    -> kReplicate(scalar, rows).
//   kArrayLaneConst    -> one packed kConstant (values[i] in [i*W +: W]).
//   kArrayOnehot       -> kShl(kConstant(rows'd1), x) at width rows; x >= rows
//                         naturally yields 0 under fixed-width shl, matching
//                         the "out of range -> 0" semantics.
//
// kMemory declarations and plain kMemoryReadPort row reads are already
// standard form and pass through untouched. Equal constants (row addresses,
// all-ones masks, the onehot '1') are memoized per graph. Dead leftovers
// (replaced values) are left for dead-code-elim; this pass only erases the
// array ops themselves. New ops carry srcLoc pass "array-lower" with
// "expand-*" notes.

#include "transform/array_lower.hpp"

#include "core/grh.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::SrcLoc;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueType;

        constexpr std::string_view kPassId = "array-lower";

        bool isArrayOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kArrayReadAllPort:
            case OperationKind::kArrayWritePort:
            case OperationKind::kArrayMux:
            case OperationKind::kArrayReduceOr:
            case OperationKind::kArrayReduceAnd:
            case OperationKind::kArrayReduceXor:
            case OperationKind::kArrayBroadcast:
            case OperationKind::kArrayLaneConst:
            case OperationKind::kArrayOnehot:
            case OperationKind::kArrayReduceLanesOr:
            case OperationKind::kArrayReduceLanesAnd:
            case OperationKind::kArrayReduceLanesXor:
                return true;
            default:
                return false;
            }
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

        std::optional<std::vector<int64_t>> getIntListAttr(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<std::vector<int64_t>>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::string makeHexLiteral(int32_t width, const slang::SVInt &value)
        {
            const int32_t normalizedWidth = width > 0 ? width : 1;
            return value.toString(slang::LiteralBase::Hex, true,
                                  static_cast<slang::bitwidth_t>(normalizedWidth));
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

        // ------------------------------------------------------------------
        // Op construction helpers (mirrors the lane-aggregate conventions).
        // ------------------------------------------------------------------
        SrcLoc expandSrcLoc(std::string_view note)
        {
            return makeTransformSrcLoc(std::string(kPassId), note);
        }

        ValueId createConstantValue(Graph &graph, int32_t width, std::string literal,
                                    bool isSigned, ValueType type, std::string_view note)
        {
            const ValueId value = graph.createValue(graph.makeInternalValSym(),
                                                    width > 0 ? width : 1, isSigned, type);
            const OperationId op = graph.createOperation(OperationKind::kConstant, graph.makeInternalOpSym());
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::move(literal));
            const SrcLoc srcLoc = expandSrcLoc(note);
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
            const SrcLoc srcLoc = expandSrcLoc(note);
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
            const SrcLoc srcLoc = expandSrcLoc(note);
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
            const SrcLoc srcLoc = expandSrcLoc(note);
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
            const SrcLoc srcLoc = expandSrcLoc(note);
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
            const SrcLoc srcLoc = expandSrcLoc(note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
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
            const SrcLoc srcLoc = expandSrcLoc(note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        // Memo for equal constants (row addresses, all-ones masks, onehot
        // '1') so expanded rows do not each mint their own kConstant.
        class ConstantMemo
        {
        public:
            ValueId address(Graph &graph, int32_t addrWidth, uint64_t value, std::string_view note)
            {
                return get(graph, 'a', addrWidth, value,
                           std::to_string(addrWidth) + "'d" + std::to_string(value), note);
            }

            ValueId allOnes(Graph &graph, int32_t width, std::string_view note)
            {
                return get(graph, 'm', width, 0, makeHexLiteral(width, allOnesValue(width)), note);
            }

            ValueId one(Graph &graph, int32_t width, std::string_view note)
            {
                return get(graph, 'o', width, 1, std::to_string(width) + "'d1", note);
            }

        private:
            static slang::SVInt allOnesValue(int32_t width)
            {
                slang::SVInt value(static_cast<slang::bitwidth_t>(width > 0 ? width : 1), 0, false);
                return ~value;
            }

            ValueId get(Graph &graph, char tag, int32_t width, uint64_t value,
                        std::string literal, std::string_view note)
            {
                const auto key = std::make_tuple(tag, width, value);
                if (const auto it = entries_.find(key); it != entries_.end())
                {
                    return it->second;
                }
                const ValueId created =
                    createConstantValue(graph, width, std::move(literal), false, ValueType::Logic, note);
                entries_.emplace(key, created);
                return created;
            }

            std::map<std::tuple<char, int32_t, uint64_t>, ValueId> entries_;
        };

        struct MemoryShape
        {
            int64_t width = 0;
            int64_t rows = 0;
            bool isSigned = false;
        };

        bool resolveMemoryShape(Graph &graph, const std::string &memSymbol,
                                MemoryShape &out, std::string &errorOut)
        {
            const OperationId memId = graph.findOperation(memSymbol);
            if (!memId.valid())
            {
                errorOut = "memSymbol target not found: " + memSymbol;
                return false;
            }
            const Operation mem = graph.getOperation(memId);
            if (mem.kind() != OperationKind::kMemory)
            {
                errorOut = "memSymbol target is not a kMemory: " + memSymbol;
                return false;
            }
            const auto width = getIntAttr(mem, "width");
            const auto rows = getIntAttr(mem, "row");
            if (!width || !rows || *width <= 0 || *rows <= 0)
            {
                errorOut = "kMemory missing positive width/row attrs: " + memSymbol;
                return false;
            }
            out.width = *width;
            out.rows = *rows;
            if (const auto isSigned = mem.attr("isSigned"))
            {
                if (const auto *flag = std::get_if<bool>(&*isSigned))
                {
                    out.isSigned = *flag;
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Per-op expansions. On success `replacement` holds the value that
        // takes over all uses of the array op's (single) result; the write
        // port has no result and only creates new ops.
        // ------------------------------------------------------------------
        bool expandReadAll(Graph &graph, const Operation &op, ConstantMemo &memo,
                           ValueId &replacement, std::string &errorOut)
        {
            const auto memSymbol = getStringAttr(op, "memSymbol");
            if (!memSymbol)
            {
                errorOut = "kArrayReadAllPort missing memSymbol attr";
                return false;
            }
            if (op.results().size() != 1)
            {
                errorOut = "kArrayReadAllPort with unexpected results";
                return false;
            }
            MemoryShape shape;
            if (!resolveMemoryShape(graph, *memSymbol, shape, errorOut))
            {
                return false;
            }
            const ValueId result = op.results().front();
            const int64_t totalWidth = shape.width * shape.rows;
            if (graph.valueWidth(result) != totalWidth)
            {
                errorOut = "kArrayReadAllPort result width != row * width of " + *memSymbol;
                return false;
            }
            const int32_t addrWidth = addressWidthForRows(static_cast<uint64_t>(shape.rows));
            std::vector<ValueId> reads;
            reads.reserve(static_cast<std::size_t>(shape.rows));
            for (int64_t row = 0; row < shape.rows; ++row)
            {
                const ValueId addr = memo.address(graph, addrWidth, static_cast<uint64_t>(row),
                                                  "expand-readall-addr");
                reads.push_back(createMemoryReadOp(graph, *memSymbol, addr,
                                                   static_cast<int32_t>(shape.width), shape.isSigned,
                                                   ValueType::Logic, "expand-readall-read"));
            }
            if (reads.size() == 1)
            {
                replacement = reads.front();
                return true;
            }
            // Row R-1 maps to the most significant bits, row 0 to the LSBs.
            const std::vector<ValueId> concatOperands(reads.rbegin(), reads.rend());
            replacement = createConcatOp(graph, concatOperands, static_cast<int32_t>(totalWidth),
                                         graph.valueSigned(result), graph.valueType(result),
                                         "expand-readall-concat");
            return true;
        }

        bool expandWrite(Graph &graph, const Operation &op, ConstantMemo &memo,
                         bool &droppedPriority, std::string &errorOut)
        {
            const auto memSymbol = getStringAttr(op, "memSymbol");
            if (!memSymbol)
            {
                errorOut = "kArrayWritePort missing memSymbol attr";
                return false;
            }
            if (op.operands().size() < 2)
            {
                errorOut = "kArrayWritePort with fewer than 2 operands";
                return false;
            }
            MemoryShape shape;
            if (!resolveMemoryShape(graph, *memSymbol, shape, errorOut))
            {
                return false;
            }
            const ValueId laneMask = op.operands()[0];
            const ValueId data = op.operands()[1];
            if (graph.valueWidth(laneMask) != shape.rows)
            {
                errorOut = "kArrayWritePort laneMask width != row of " + *memSymbol;
                return false;
            }
            if (graph.valueWidth(data) != shape.width * shape.rows)
            {
                errorOut = "kArrayWritePort data width != row * width of " + *memSymbol;
                return false;
            }
            droppedPriority = op.attr("memoryWrite.priorityGroup").has_value() ||
                              op.attr("memoryWrite.priority").has_value();
            const int32_t addrWidth = addressWidthForRows(static_cast<uint64_t>(shape.rows));
            for (int64_t row = 0; row < shape.rows; ++row)
            {
                const uint64_t laneLow = static_cast<uint64_t>(row) * static_cast<uint64_t>(shape.width);
                const ValueId enable = createSliceOp(graph, laneMask, static_cast<uint64_t>(row),
                                                     static_cast<uint64_t>(row), false,
                                                     ValueType::Logic, "expand-write-enable");
                const ValueId addr = memo.address(graph, addrWidth, static_cast<uint64_t>(row),
                                                  "expand-write-addr");
                const ValueId laneData = createSliceOp(graph, data, laneLow,
                                                       laneLow + static_cast<uint64_t>(shape.width) - 1,
                                                       shape.isSigned, ValueType::Logic,
                                                       "expand-write-data");
                const ValueId mask = memo.allOnes(graph, static_cast<int32_t>(shape.width),
                                                  "expand-write-mask");
                const OperationId writeOp =
                    graph.createOperation(OperationKind::kMemoryWritePort, graph.makeInternalOpSym());
                graph.addOperand(writeOp, enable);
                graph.addOperand(writeOp, addr);
                graph.addOperand(writeOp, laneData);
                graph.addOperand(writeOp, mask);
                for (std::size_t i = 2; i < op.operands().size(); ++i)
                {
                    graph.addOperand(writeOp, op.operands()[i]);
                }
                graph.setAttr(writeOp, "memSymbol", *memSymbol);
                if (const auto eventEdge = op.attr("eventEdge"))
                {
                    graph.setAttr(writeOp, "eventEdge", *eventEdge);
                }
                graph.setOpSrcLoc(writeOp, expandSrcLoc("expand-write-port"));
            }
            return true;
        }

        bool expandMux(Graph &graph, const Operation &op, ValueId &replacement, std::string &errorOut)
        {
            if (op.operands().size() != 3 || op.results().size() != 1)
            {
                errorOut = "kArrayMux with unexpected operands/results";
                return false;
            }
            const ValueId sel = op.operands()[0];
            const ValueId t = op.operands()[1];
            const ValueId f = op.operands()[2];
            const ValueId result = op.results().front();
            const int64_t rows = graph.valueWidth(sel);
            const int64_t totalWidth = graph.valueWidth(result);
            if (rows <= 0 || totalWidth <= 0 || totalWidth % rows != 0)
            {
                errorOut = "kArrayMux width mismatch between sel and result";
                return false;
            }
            if (graph.valueWidth(t) != totalWidth || graph.valueWidth(f) != totalWidth)
            {
                errorOut = "kArrayMux operand width mismatch";
                return false;
            }
            const int64_t laneWidth = totalWidth / rows;
            std::vector<ValueId> broadcasts;
            broadcasts.reserve(static_cast<std::size_t>(rows));
            for (int64_t row = 0; row < rows; ++row)
            {
                const ValueId bit = createSliceOp(graph, sel, static_cast<uint64_t>(row),
                                                  static_cast<uint64_t>(row), false,
                                                  ValueType::Logic, "expand-mux-bit");
                broadcasts.push_back(createReplicateOp(graph, bit, static_cast<uint64_t>(laneWidth),
                                                       "expand-mux-bcast"));
            }
            ValueId mask = broadcasts.front();
            if (broadcasts.size() > 1)
            {
                const std::vector<ValueId> concatOperands(broadcasts.rbegin(), broadcasts.rend());
                mask = createConcatOp(graph, concatOperands, static_cast<int32_t>(totalWidth),
                                      false, ValueType::Logic, "expand-mux-mask");
            }
            const ValueId invMask = createUnaryOp(graph, OperationKind::kNot, mask,
                                                  static_cast<int32_t>(totalWidth), false,
                                                  ValueType::Logic, "expand-mux-mask-not");
            const ValueId tMasked = createBinaryOp(graph, OperationKind::kAnd, t, mask,
                                                   static_cast<int32_t>(totalWidth), false,
                                                   ValueType::Logic, "expand-mux-and-t");
            const ValueId fMasked = createBinaryOp(graph, OperationKind::kAnd, f, invMask,
                                                   static_cast<int32_t>(totalWidth), false,
                                                   ValueType::Logic, "expand-mux-and-f");
            replacement = createBinaryOp(graph, OperationKind::kOr, tMasked, fMasked,
                                         static_cast<int32_t>(totalWidth),
                                         graph.valueSigned(result), graph.valueType(result),
                                         "expand-mux-or");
            return true;
        }

        bool expandReduce(Graph &graph, const Operation &op, ValueId &replacement, std::string &errorOut)
        {
            if (op.operands().size() != 1 || op.results().size() != 1)
            {
                errorOut = "kArrayReduce* with unexpected operands/results";
                return false;
            }
            OperationKind reduceKind = OperationKind::kReduceOr;
            switch (op.kind())
            {
            case OperationKind::kArrayReduceOr:
                reduceKind = OperationKind::kReduceOr;
                break;
            case OperationKind::kArrayReduceAnd:
                reduceKind = OperationKind::kReduceAnd;
                break;
            case OperationKind::kArrayReduceXor:
                reduceKind = OperationKind::kReduceXor;
                break;
            default:
                errorOut = "not an array reduce op";
                return false;
            }
            const ValueId result = op.results().front();
            replacement = createUnaryOp(graph, reduceKind, op.operands().front(), 1,
                                        graph.valueSigned(result), graph.valueType(result),
                                        "expand-reduce");
            return true;
        }

        bool expandReduceLanes(Graph &graph, const Operation &op, ValueId &replacement, std::string &errorOut)
        {
            if (op.operands().size() != 1 || op.results().size() != 1)
            {
                errorOut = "kArrayReduceLanes* with unexpected operands/results";
                return false;
            }
            OperationKind reduceKind = OperationKind::kReduceOr;
            switch (op.kind())
            {
            case OperationKind::kArrayReduceLanesOr:
                reduceKind = OperationKind::kReduceOr;
                break;
            case OperationKind::kArrayReduceLanesAnd:
                reduceKind = OperationKind::kReduceAnd;
                break;
            case OperationKind::kArrayReduceLanesXor:
                reduceKind = OperationKind::kReduceXor;
                break;
            default:
                errorOut = "not an array reduce-lanes op";
                return false;
            }
            const auto elemWidth = getIntAttr(op, "elemWidth");
            if (!elemWidth || *elemWidth <= 0)
            {
                errorOut = "kArrayReduceLanes* missing positive elemWidth attr";
                return false;
            }
            const ValueId data = op.operands().front();
            const ValueId result = op.results().front();
            const int64_t rows = graph.valueWidth(result);
            if (rows <= 0 ||
                static_cast<int64_t>(graph.valueWidth(data)) != rows * *elemWidth)
            {
                errorOut = "kArrayReduceLanes* result width * elemWidth != data width";
                return false;
            }
            // Per lane: kReduce*(kSliceStatic(data, i*W, i*W+W-1)); the row-bit
            // result is the kConcat of the per-lane bits (lane R-1 in the
            // MSBs, lane 0 in the LSBs; row == 1 uses the bit directly).
            std::vector<ValueId> bits;
            bits.reserve(static_cast<std::size_t>(rows));
            for (int64_t row = rows; row-- > 0;)
            {
                const uint64_t low = static_cast<uint64_t>(row) * static_cast<uint64_t>(*elemWidth);
                const ValueId laneValue = createSliceOp(graph, data, low,
                                                        low + static_cast<uint64_t>(*elemWidth) - 1,
                                                        false, ValueType::Logic, "expand-reduce-lanes-slice");
                bits.push_back(createUnaryOp(graph, reduceKind, laneValue, 1,
                                             false, ValueType::Logic, "expand-reduce-lanes-bit"));
            }
            if (bits.size() == 1)
            {
                replacement = bits.front();
                return true;
            }
            replacement = createConcatOp(graph, bits, static_cast<int32_t>(rows),
                                         graph.valueSigned(result), graph.valueType(result),
                                         "expand-reduce-lanes-concat");
            return true;
        }

        bool expandBroadcast(Graph &graph, const Operation &op, ValueId &replacement, std::string &errorOut)
        {
            if (op.operands().size() != 1 || op.results().size() != 1)
            {
                errorOut = "kArrayBroadcast with unexpected operands/results";
                return false;
            }
            const auto rows = getIntAttr(op, "rows");
            if (!rows || *rows <= 0)
            {
                errorOut = "kArrayBroadcast missing positive rows attr";
                return false;
            }
            const ValueId result = op.results().front();
            if (static_cast<int64_t>(graph.valueWidth(op.operands().front())) * *rows !=
                graph.valueWidth(result))
            {
                errorOut = "kArrayBroadcast result width != scalar width * rows";
                return false;
            }
            replacement = createReplicateOp(graph, op.operands().front(),
                                            static_cast<uint64_t>(*rows), "expand-broadcast");
            return true;
        }

        bool expandLaneConst(Graph &graph, const Operation &op, ValueId &replacement, std::string &errorOut)
        {
            if (!op.operands().empty() || op.results().size() != 1)
            {
                errorOut = "kArrayLaneConst with unexpected operands/results";
                return false;
            }
            const auto elemWidth = getIntAttr(op, "elemWidth");
            const auto rows = getIntAttr(op, "rows");
            const auto values = getIntListAttr(op, "values");
            if (!elemWidth || *elemWidth <= 0 || !rows || *rows <= 0 || !values ||
                values->size() != static_cast<std::size_t>(*rows))
            {
                errorOut = "kArrayLaneConst with malformed attrs";
                return false;
            }
            const ValueId result = op.results().front();
            const int64_t totalWidth = *elemWidth * *rows;
            if (graph.valueWidth(result) != totalWidth)
            {
                errorOut = "kArrayLaneConst result width != elemWidth * rows";
                return false;
            }
            const auto total = static_cast<slang::bitwidth_t>(totalWidth);
            slang::SVInt table(total, 0, false);
            for (int64_t row = 0; row < *rows; ++row)
            {
                uint64_t raw = static_cast<uint64_t>((*values)[static_cast<std::size_t>(row)]);
                if (*elemWidth < 64)
                {
                    raw &= (UINT64_C(1) << *elemWidth) - 1;
                }
                slang::SVInt segment(total, raw, false);
                table = table | segment.shl(static_cast<slang::bitwidth_t>(
                                                row * static_cast<uint64_t>(*elemWidth)));
            }
            replacement = createConstantValue(graph, static_cast<int32_t>(totalWidth),
                                              makeHexLiteral(static_cast<int32_t>(totalWidth), table),
                                              graph.valueSigned(result), graph.valueType(result),
                                              "expand-laneconst");
            return true;
        }

        bool expandOnehot(Graph &graph, const Operation &op, ConstantMemo &memo,
                          ValueId &replacement, std::string &errorOut)
        {
            if (op.operands().size() != 1 || op.results().size() != 1)
            {
                errorOut = "kArrayOnehot with unexpected operands/results";
                return false;
            }
            const auto rows = getIntAttr(op, "rows");
            if (!rows || *rows <= 0)
            {
                errorOut = "kArrayOnehot missing positive rows attr";
                return false;
            }
            const ValueId result = op.results().front();
            if (graph.valueWidth(result) != *rows)
            {
                errorOut = "kArrayOnehot result width != rows";
                return false;
            }
            // (rows'd1 << x) truncated to rows bits: x >= rows naturally
            // yields 0, matching the out-of-range semantics of kArrayOnehot.
            const ValueId one = memo.one(graph, static_cast<int32_t>(*rows), "expand-onehot-one");
            replacement = createBinaryOp(graph, OperationKind::kShl, one, op.operands().front(),
                                         static_cast<int32_t>(*rows), graph.valueSigned(result),
                                         graph.valueType(result), "expand-onehot");
            return true;
        }

        // Moves the old result's symbol onto the replacement, rebinds output
        // ports, then erases the array op (replaceAllUses handled inside).
        bool replaceResultAndErase(Graph &graph, OperationId opId, ValueId replacement,
                                   std::string &errorOut)
        {
            const Operation op = graph.getOperation(opId);
            const ValueId oldResult = op.results().front();
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
            if (!graph.eraseOp(opId, std::array<ValueId, 1>{replacement}))
            {
                errorOut = "failed to erase array op";
                return false;
            }
            return true;
        }

    } // namespace

    ArrayLowerPass::ArrayLowerPass()
        : Pass("array-lower", "array-lower",
               "Expand array-value ops into plain wide-scalar ops (SV-emit compatible form)")
    {
    }

    PassResult ArrayLowerPass::run()
    {
        PassResult result;
        std::size_t expanded = 0;
        std::size_t skipped = 0;
        std::size_t droppedPriority = 0;

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            Graph &graph = *entry.second;

            std::vector<OperationId> targets;
            for (const OperationId opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                if (isArrayOp(graph.getOperation(opId).kind()))
                {
                    targets.push_back(opId);
                }
            }
            if (targets.empty())
            {
                continue;
            }

            ConstantMemo memo;
            for (const OperationId opId : targets)
            {
                const Operation op = graph.getOperation(opId);
                std::string errorOut;
                ValueId replacement;
                bool writeDroppedPriority = false;
                bool ok = true;
                switch (op.kind())
                {
                case OperationKind::kArrayReadAllPort:
                    ok = expandReadAll(graph, op, memo, replacement, errorOut);
                    break;
                case OperationKind::kArrayWritePort:
                    ok = expandWrite(graph, op, memo, writeDroppedPriority, errorOut);
                    break;
                case OperationKind::kArrayMux:
                    ok = expandMux(graph, op, replacement, errorOut);
                    break;
                case OperationKind::kArrayReduceOr:
                case OperationKind::kArrayReduceAnd:
                case OperationKind::kArrayReduceXor:
                    ok = expandReduce(graph, op, replacement, errorOut);
                    break;
                case OperationKind::kArrayReduceLanesOr:
                case OperationKind::kArrayReduceLanesAnd:
                case OperationKind::kArrayReduceLanesXor:
                    ok = expandReduceLanes(graph, op, replacement, errorOut);
                    break;
                case OperationKind::kArrayBroadcast:
                    ok = expandBroadcast(graph, op, replacement, errorOut);
                    break;
                case OperationKind::kArrayLaneConst:
                    ok = expandLaneConst(graph, op, replacement, errorOut);
                    break;
                case OperationKind::kArrayOnehot:
                    ok = expandOnehot(graph, op, memo, replacement, errorOut);
                    break;
                default:
                    ok = false;
                    errorOut = "unsupported op kind";
                    break;
                }
                if (!ok)
                {
                    warning(graph, op, "array-lower: skipped " + std::string(op.symbolText()) +
                                           ": " + errorOut);
                    ++skipped;
                    continue;
                }
                if (op.kind() == OperationKind::kArrayWritePort)
                {
                    if (writeDroppedPriority)
                    {
                        ++droppedPriority;
                        warning(graph, op,
                                "array-lower: dropping memoryWrite.priority* attrs; the expanded "
                                "per-row write ports use distinct constant addresses and never "
                                "collide with each other");
                    }
                    if (!graph.eraseOp(opId))
                    {
                        warning(graph, op,
                                "array-lower: failed to erase " + std::string(op.symbolText()));
                        ++skipped;
                        continue;
                    }
                }
                else if (!replaceResultAndErase(graph, opId, replacement, errorOut))
                {
                    warning(graph, op, "array-lower: failed to replace " +
                                           std::string(op.symbolText()) + ": " + errorOut);
                    ++skipped;
                    continue;
                }
                ++expanded;
                result.changed = true;
            }
        }

        const std::string summary = "array-lower summary expanded=" + std::to_string(expanded) +
                                    " skipped=" + std::to_string(skipped) +
                                    " dropped_priority=" + std::to_string(droppedPriority);
        info(summary);
        logInfo(summary);
        return result;
    }

} // namespace wolvrix::lib::transform
