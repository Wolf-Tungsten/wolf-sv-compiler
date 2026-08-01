// array-select-recovery: recover scalar register arrays accessed through
// one-hot select logic into kMemory + kMemoryReadPort/kMemoryWritePort.
//
// Motivation: designs like XiangShan keep entry arrays (e.g. Rob robEntries,
// MSHR fields) as one scalar kRegister per entry per field, surrounded by
// one-hot select logic. reg-to-mem skips them because the write/guard shapes
// do not match its anchors. This pass recognizes a self-contained strict
// shape and recovers the array as a true memory.
//
// Pattern (read side drives family discovery):
//   1. A read-out tree is either
//      - a kMux chain: mux(sel_k, leaf_k, rest) where the true branch is the
//        payload leaf and the false branch is the next mux or the default
//        leaf (a bare kRegisterReadPort), or
//      - a kOr/kLogicOr tree whose terms are kAnd/kLogicAnd(sel_k, leaf_k).
//      Every select sel_k is a one-hot bit of a shared index:
//      kSliceStatic(S, k, k) / kSliceDynamic(S, const k, width=1) with
//      S = kShl(constant 1, idx), or kSliceArray(onehotConcat, const k,
//      width=1) where onehotConcat is a kConcat whose bit j is kEq(idx, j)
//      for every j (provably the one-hot decode of the same idx). Leaves are
//      kRegisterReadPort results whose width equals the tree root width, and
//      each gated row appears at most once per tree.
//   2. Trees sharing at least one register are unioned into a family. The
//      family registers must number >= minEntries (default 4), share width,
//      signedness and regSymbol shape (digit runs normalized to '#'), and
//      their rows (from the gating selects, mux-chain defaults resolved by
//      elimination) must be exactly [0, N). Every tree must cover all N rows
//      (an or-tree yields 0 for uncovered rows and a mux chain falls to its
//      default, neither of which a kMemoryReadPort can reproduce).
//   3. Read closure: every kRegisterReadPort of a family register must be a
//      leaf of a family tree or the self-read operand of the register's own
//      write mux (see below); every use of every such read result must stay
//      inside the matched trees or that write mux. Any other read ("wild
//      read") skips the whole family.
//   4. Write side: each family register has exactly one kRegisterWritePort,
//      the register carries no init/reset attributes, and updateCond
//      decomposes into an OR of AND terms where each term contains exactly
//      one row select for the register's own row k - either the same
//      shift-onehot slice form as the read side or kEq(idx_w, C) with
//      C == k - sharing one idx_w across all terms; the remaining AND terms
//      form the common guard. nextValue is either free of self references
//      (data = nextValue) or kMux(sel, newValue, selfRead) with selfRead a
//      read of the same register (data = newValue, sel is ANDed into the
//      write condition, modeling "conditional update else hold"). A self
//      reference in any other shape skips the family.
//
// Rewrite:
//   - One kMemory (width = field width, row = N, isSigned from the register
//     attribute), named asr_mem$<first reg symbol> (reg-to-mem naming
//     convention with an asr_ prefix).
//   - Each read tree becomes one kMemoryReadPort with addr = the tree's
//     shared idx; the tree root value is replaced via replaceAllUses +
//     bindOutputPort (logic-normalize convention).
//   - Each register's write port becomes one kMemoryWritePort with
//     addr = idx_w of its guard's row selects, updateCond rebuilt as the OR
//     of (common guard AND row select AND mux sel) terms - computed as
//     kLogicAnd(original updateCond, mux sel), which is the same expression
//     because the row select is a common factor of every OR term - data =
//     newValue, mask/events/eventEdge copied from the register write port.
//   - The kRegister decls and the old read/write port ops are erased (dead
//     tree/guard logic is left for dead-code elimination, logic-normalize
//     convention).
//
// Equivalence (2-state): S = (1 << idx) has at most one bit set, so the row
// selects are mutually exclusive: the read tree yields leaf_k exactly when
// idx == k, which is mem[idx]; on the write side at most one row's condition
// fires, so the kMemoryWritePorts update mem[k] with newValue_k exactly when
// register k would have updated, and mux(sel, new, self) hold semantics
// equal "write only when sel" because a disabled memory write holds the row.
// Rows are only ever written through these ports, so mem[k] always holds
// entry k's value. Caveat: for out-of-range addresses (idx >= N) the
// original tree yields its default leaf (mux form) or 0 (or form) while
// kMemoryReadPort yields X; the rewrite is claimed 2-state equivalent for
// in-range addresses only, matching the one-hot write side which also never
// updates any row for out-of-range idx. Under 4-state simulation X
// propagation may differ.
//
// Option rewrite=false runs a census: the same matching pipeline without
// modifying the graph, logging matched families/rows/read trees/write ports
// and per-reason skip counts (wild_read, write_count, guard_form,
// selfref_form, reset_attr, row_mismatch, small_family, tree_incomplete,
// field_mismatch).

#include "transform/array_select_recovery.hpp"

#include "core/grh.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
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
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;

        constexpr std::string_view kPassId = "array-select-recovery";

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

        // logic-normalize convention: replace all uses of a value and rebind
        // any output ports that referenced it.
        void replaceUsers(Graph &graph,
                          ValueId from,
                          ValueId to,
                          const std::function<void(std::string)> &onError)
        {
            try
            {
                graph.replaceAllUses(from, to);
            }
            catch (const std::exception &ex)
            {
                onError(std::string("Failed to replace operands: ") + ex.what());
                return;
            }

            std::vector<std::string> outputPortsToUpdate;
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == from)
                {
                    outputPortsToUpdate.push_back(port.name);
                }
            }
            for (const auto &portName : outputPortsToUpdate)
            {
                try
                {
                    graph.bindOutputPort(portName, to);
                }
                catch (const std::exception &ex)
                {
                    onError(std::string("Failed to rebind output port: ") + ex.what());
                }
            }
        }

        std::optional<ValueId> unwrapAssign(const Graph &graph, ValueId value)
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

        void flattenLogicAndTerms(const Graph &graph,
                                  ValueId value,
                                  std::vector<ValueId> &terms,
                                  std::unordered_set<ValueId, ValueIdHash> &seen)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped)
            {
                terms.push_back(value);
                return;
            }
            value = *unwrapped;
            if (!seen.insert(value).second)
            {
                return;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                terms.push_back(value);
                return;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if ((defOp.kind() == OperationKind::kLogicAnd ||
                 (defOp.kind() == OperationKind::kAnd && graph.valueWidth(value) == 1)) &&
                defOp.operands().size() == 2)
            {
                flattenLogicAndTerms(graph, defOp.operands()[0], terms, seen);
                flattenLogicAndTerms(graph, defOp.operands()[1], terms, seen);
                return;
            }
            terms.push_back(value);
        }

        std::vector<ValueId> flattenLogicAndTerms(const Graph &graph, ValueId value)
        {
            std::vector<ValueId> terms;
            std::unordered_set<ValueId, ValueIdHash> seen;
            flattenLogicAndTerms(graph, value, terms, seen);
            return terms;
        }

        void flattenLogicOrTerms(const Graph &graph,
                                 ValueId value,
                                 std::vector<ValueId> &terms,
                                 std::unordered_set<ValueId, ValueIdHash> &seen)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped)
            {
                terms.push_back(value);
                return;
            }
            value = *unwrapped;
            if (!seen.insert(value).second)
            {
                return;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                terms.push_back(value);
                return;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if ((defOp.kind() == OperationKind::kLogicOr ||
                 (defOp.kind() == OperationKind::kOr && graph.valueWidth(value) == 1)) &&
                defOp.operands().size() == 2)
            {
                flattenLogicOrTerms(graph, defOp.operands()[0], terms, seen);
                flattenLogicOrTerms(graph, defOp.operands()[1], terms, seen);
                return;
            }
            terms.push_back(value);
        }

        std::vector<ValueId> flattenLogicOrTerms(const Graph &graph, ValueId value)
        {
            std::vector<ValueId> terms;
            std::unordered_set<ValueId, ValueIdHash> seen;
            flattenLogicOrTerms(graph, value, terms, seen);
            return terms;
        }

        std::optional<uint64_t> getConstantUInt64(const Graph &graph, ValueId value)
        {
            if (!value.valid() || graph.valueType(value) != ValueType::Logic)
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
            slang::SVInt parsed;
            try
            {
                parsed = slang::SVInt::fromString(*literal);
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
            if (parsed.hasUnknown())
            {
                return std::nullopt;
            }
            const int32_t width = std::max<int32_t>(graph.valueWidth(value), 1);
            parsed = parsed.resize(static_cast<slang::bitwidth_t>(width));
            const std::size_t wordCount = static_cast<std::size_t>((width + 63) / 64);
            const std::uint64_t *raw = parsed.getRawPtr();
            for (std::size_t i = 1; i < wordCount; ++i)
            {
                if (raw[i] != UINT64_C(0))
                {
                    return std::nullopt;
                }
            }
            return raw[0];
        }

        struct RowSelectMatch
        {
            ValueId index;
            uint64_t row = 0;
        };

        // base must be kShl(constant 1, idx): the shift-onehot vector whose
        // bit k is set exactly when idx == k.
        std::optional<RowSelectMatch> matchShiftOnehotBase(const Graph &graph, ValueId base, uint64_t row)
        {
            const OperationId defOpId = graph.valueDef(base);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kShl || defOp.operands().size() != 2 ||
                defOp.results().size() != 1)
            {
                return std::nullopt;
            }
            const auto one = getConstantUInt64(graph, defOp.operands()[0]);
            if (!one || *one != 1)
            {
                return std::nullopt;
            }
            return RowSelectMatch{defOp.operands()[1], row};
        }

        // base must be a kConcat whose bit j is kEq(idx, j) for every j,
        // i.e. a provable one-hot decode of a single shared index.
        std::optional<RowSelectMatch> matchOnehotConcatBase(const Graph &graph, ValueId base, uint64_t row);

        // value must be kEq(idx, C) (either operand order, not both
        // constant), returning the index and C.
        std::optional<RowSelectMatch> matchEqConst(const Graph &graph, ValueId value)
        {
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kEq || defOp.operands().size() != 2 ||
                defOp.results().size() != 1)
            {
                return std::nullopt;
            }
            std::optional<RowSelectMatch> match;
            for (std::size_t i = 0; i < 2; ++i)
            {
                const auto constant = getConstantUInt64(graph, defOp.operands()[i]);
                if (!constant)
                {
                    continue;
                }
                if (match)
                {
                    // Both operands constant: the index is ambiguous.
                    return std::nullopt;
                }
                match = RowSelectMatch{defOp.operands()[1 - i], *constant};
            }
            return match;
        }

        std::optional<RowSelectMatch> matchOnehotConcatBase(const Graph &graph, ValueId base, uint64_t row)
        {
            const OperationId defOpId = graph.valueDef(base);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kConcat || defOp.results().size() != 1)
            {
                return std::nullopt;
            }
            const auto operands = defOp.operands();
            const std::size_t count = operands.size();
            if (count == 0 || row >= count)
            {
                return std::nullopt;
            }
            ValueId index;
            for (std::size_t j = 0; j < count; ++j)
            {
                // kConcat operand[0] is the MSB, so bit j is operand[count-1-j].
                const ValueId bit = operands[count - 1 - j];
                if (graph.valueWidth(bit) != 1)
                {
                    return std::nullopt;
                }
                const auto eq = matchEqConst(graph, bit);
                if (!eq || eq->row != j)
                {
                    return std::nullopt;
                }
                if (index.valid() && index != eq->index)
                {
                    return std::nullopt;
                }
                index = eq->index;
            }
            if (!index.valid())
            {
                return std::nullopt;
            }
            return RowSelectMatch{index, row};
        }

        // Read-side row select: a one-hot bit of the shared index, either
        // kSliceStatic/kSliceDynamic of the shift-onehot vector at constant
        // row k, or kSliceArray of a provable one-hot concat at constant k.
        std::optional<RowSelectMatch> matchReadRowSelect(const Graph &graph, ValueId value)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped || graph.valueWidth(*unwrapped) != 1)
            {
                return std::nullopt;
            }
            const OperationId defOpId = graph.valueDef(*unwrapped);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            const auto operands = defOp.operands();
            if (defOp.kind() == OperationKind::kSliceStatic && operands.size() == 1)
            {
                const auto start = getAttr<int64_t>(defOp, "sliceStart");
                const auto end = getAttr<int64_t>(defOp, "sliceEnd");
                if (!start || !end || *start != *end || *start < 0)
                {
                    return std::nullopt;
                }
                return matchShiftOnehotBase(graph, operands[0], static_cast<uint64_t>(*start));
            }
            if (defOp.kind() == OperationKind::kSliceDynamic && operands.size() == 2)
            {
                const auto sliceWidth = getAttr<int64_t>(defOp, "sliceWidth");
                if (!sliceWidth || *sliceWidth != 1)
                {
                    return std::nullopt;
                }
                const auto row = getConstantUInt64(graph, operands[1]);
                if (!row)
                {
                    return std::nullopt;
                }
                return matchShiftOnehotBase(graph, operands[0], *row);
            }
            if (defOp.kind() == OperationKind::kSliceArray && operands.size() == 2)
            {
                const auto sliceWidth = getAttr<int64_t>(defOp, "sliceWidth");
                if (!sliceWidth || *sliceWidth != 1)
                {
                    return std::nullopt;
                }
                const auto row = getConstantUInt64(graph, operands[1]);
                if (!row)
                {
                    return std::nullopt;
                }
                return matchOnehotConcatBase(graph, operands[0], *row);
            }
            return std::nullopt;
        }

        // Write-side row select: the shift-onehot slice form (same as the
        // read side) or kEq(idx, C).
        std::optional<RowSelectMatch> matchWriteRowSelect(const Graph &graph, ValueId value)
        {
            if (auto shift = matchReadRowSelect(graph, value))
            {
                return shift;
            }
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped || graph.valueWidth(*unwrapped) != 1)
            {
                return std::nullopt;
            }
            return matchEqConst(graph, *unwrapped);
        }

        struct ReadItem
        {
            uint64_t row = 0; // gated row; resolved at family level for defaults
            bool isDefault = false;
            OperationId readOp;
            std::string regSymbol;
            ValueId leafValue;
            ValueId selectValue; // invalid for default items
        };

        struct ReadTreeMatch
        {
            OperationId rootOp;
            ValueId rootValue;
            ValueId index;
            std::vector<ReadItem> items;
            std::unordered_set<OperationId, OperationIdHash> internalOps;
        };

        bool matchReadLeaf(const Graph &graph, ValueId value, ReadItem &item)
        {
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return false;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kRegisterReadPort || defOp.results().size() != 1)
            {
                return false;
            }
            const auto regSymbol = getStringAttr(defOp, "regSymbol");
            if (!regSymbol || regSymbol->empty())
            {
                return false;
            }
            const OperationId regOpId = graph.findOperation(*regSymbol);
            if (!regOpId.valid() || graph.getOperation(regOpId).kind() != OperationKind::kRegister)
            {
                return false;
            }
            item.readOp = defOpId;
            item.regSymbol = *regSymbol;
            item.leafValue = value;
            return true;
        }

        // kMux chain: mux(sel_k, leaf_k, rest), walking from the root
        // (outermost) mux inward along false branches.
        bool matchMuxChain(const Graph &graph, OperationId rootOp, ReadTreeMatch &tree)
        {
            const int32_t rootWidth = graph.valueWidth(tree.rootValue);
            OperationId current = rootOp;
            while (true)
            {
                const Operation op = graph.getOperation(current);
                const auto operands = op.operands();
                if (op.results().size() != 1 || operands.size() != 3)
                {
                    return false;
                }
                if (current != rootOp)
                {
                    // Interior node: exactly one user (the enclosing mux).
                    if (graph.getValue(op.results().front()).users().size() != 1)
                    {
                        return false;
                    }
                }
                tree.internalOps.insert(current);
                const auto select = matchReadRowSelect(graph, operands[0]);
                if (!select)
                {
                    return false;
                }
                if (tree.index.valid() && tree.index != select->index)
                {
                    return false;
                }
                tree.index = select->index;
                for (const ReadItem &existing : tree.items)
                {
                    if (!existing.isDefault && existing.row == select->row)
                    {
                        return false;
                    }
                }
                ReadItem item;
                item.row = select->row;
                item.selectValue = operands[0];
                if (!matchReadLeaf(graph, operands[1], item) ||
                    graph.valueWidth(operands[1]) != rootWidth)
                {
                    return false;
                }
                tree.items.push_back(std::move(item));

                const ValueId falseBranch = operands[2];
                const OperationId falseDefId = graph.valueDef(falseBranch);
                if (falseDefId.valid() &&
                    graph.getOperation(falseDefId).kind() == OperationKind::kMux)
                {
                    current = falseDefId;
                    continue;
                }
                ReadItem defaultItem;
                defaultItem.isDefault = true;
                if (!matchReadLeaf(graph, falseBranch, defaultItem) ||
                    graph.valueWidth(falseBranch) != rootWidth)
                {
                    return false;
                }
                tree.items.push_back(std::move(defaultItem));
                return tree.items.size() >= 2;
            }
        }

        // kOr/kLogicOr tree of kAnd/kLogicAnd(sel_k, leaf_k) terms.
        bool matchOrTree(const Graph &graph, OperationId rootOp, ReadTreeMatch &tree)
        {
            const int32_t rootWidth = graph.valueWidth(tree.rootValue);
            std::vector<OperationId> stack{rootOp};
            while (!stack.empty())
            {
                const OperationId nodeId = stack.back();
                stack.pop_back();
                const Operation node = graph.getOperation(nodeId);
                const auto operands = node.operands();
                const OperationKind kind = node.kind();
                const bool isOr = kind == OperationKind::kOr || kind == OperationKind::kLogicOr;
                if (!isOr || node.results().size() != 1 || operands.size() != 2)
                {
                    return false;
                }
                if (nodeId != rootOp &&
                    graph.getValue(node.results().front()).users().size() != 1)
                {
                    return false;
                }
                tree.internalOps.insert(nodeId);
                for (std::size_t i = 0; i < 2; ++i)
                {
                    const ValueId term = operands[i];
                    const OperationId termDefId = graph.valueDef(term);
                    if (!termDefId.valid())
                    {
                        return false;
                    }
                    const Operation termDef = graph.getOperation(termDefId);
                    const OperationKind termKind = termDef.kind();
                    if (termKind == OperationKind::kOr || termKind == OperationKind::kLogicOr)
                    {
                        stack.push_back(termDefId);
                        continue;
                    }
                    const bool isAnd = termKind == OperationKind::kAnd || termKind == OperationKind::kLogicAnd;
                    if (!isAnd || termDef.results().size() != 1 || termDef.operands().size() != 2 ||
                        graph.valueWidth(term) != rootWidth)
                    {
                        return false;
                    }
                    // Interior tree node: exactly one user (the or node).
                    if (graph.getValue(term).users().size() != 1)
                    {
                        return false;
                    }
                    std::optional<RowSelectMatch> select;
                    ValueId leaf;
                    ValueId selectValue;
                    for (std::size_t j = 0; j < 2; ++j)
                    {
                        auto candidate = matchReadRowSelect(graph, termDef.operands()[j]);
                        if (!candidate)
                        {
                            continue;
                        }
                        select = std::move(candidate);
                        selectValue = termDef.operands()[j];
                        leaf = termDef.operands()[1 - j];
                        break;
                    }
                    if (!select)
                    {
                        return false;
                    }
                    if (tree.index.valid() && tree.index != select->index)
                    {
                        return false;
                    }
                    tree.index = select->index;
                    for (const ReadItem &existing : tree.items)
                    {
                        if (!existing.isDefault && existing.row == select->row)
                        {
                            return false;
                        }
                    }
                    ReadItem item;
                    item.row = select->row;
                    item.selectValue = selectValue;
                    if (!matchReadLeaf(graph, leaf, item) ||
                        graph.valueWidth(leaf) != rootWidth)
                    {
                        return false;
                    }
                    tree.internalOps.insert(termDefId);
                    tree.items.push_back(std::move(item));
                }
            }
            return tree.items.size() >= 2;
        }

        // Attempts to match a read-out tree rooted at rootOp (kMux/kOr/
        // kLogicOr whose result does not feed another tree node, so each
        // maximal tree is considered exactly once).
        std::optional<ReadTreeMatch> matchReadTree(const Graph &graph, OperationId rootOp)
        {
            const Operation root = graph.getOperation(rootOp);
            const OperationKind kind = root.kind();
            if (kind != OperationKind::kMux && kind != OperationKind::kOr &&
                kind != OperationKind::kLogicOr)
            {
                return std::nullopt;
            }
            if (root.results().size() != 1)
            {
                return std::nullopt;
            }
            ReadTreeMatch tree;
            tree.rootOp = rootOp;
            tree.rootValue = root.results().front();
            // Root check: the result must not feed a potential tree node, or
            // this op is an interior node of a larger tree. The Value must be
            // named: in C++20 the temporary in a range-for range expression
            // dies before the loop body, dangling the users() span.
            const wolvrix::lib::grh::Value rootValue = graph.getValue(tree.rootValue);
            for (const auto &user : rootValue.users())
            {
                if (!user.operation.valid())
                {
                    continue;
                }
                const OperationKind userKind = graph.getOperation(user.operation).kind();
                if (userKind == OperationKind::kMux || userKind == OperationKind::kOr ||
                    userKind == OperationKind::kLogicOr)
                {
                    return std::nullopt;
                }
            }
            const bool matched = kind == OperationKind::kMux ? matchMuxChain(graph, rootOp, tree)
                                                             : matchOrTree(graph, rootOp, tree);
            if (!matched || !tree.index.valid())
            {
                return std::nullopt;
            }
            return tree;
        }

        struct WritePortInfo
        {
            OperationId op;
            ValueId updateCond;
            ValueId nextValue;
            ValueId mask;
            std::vector<ValueId> events;
            std::vector<std::string> eventEdges;
        };

        struct WriteMatch
        {
            WritePortInfo write;
            ValueId index;      // idx_w shared by all row selects
            ValueId data;       // memory write data
            ValueId extraGuard; // mux sel for the hold form; invalid otherwise
            OperationId selfReadOp;  // the write mux self-read; invalid otherwise
            OperationId nextValueMuxOp; // the kMux producing nextValue; invalid otherwise
        };

        struct FamilyReg
        {
            std::string regSymbol;
            OperationId regOp;
            std::optional<uint64_t> row;
            std::optional<WriteMatch> write;
        };

        struct FamilyWork
        {
            std::vector<std::size_t> trees; // indices into the graph tree list
            std::map<std::string, std::size_t> regIndices;
            std::vector<FamilyReg> regs;
        };

        struct FamilyStats
        {
            std::size_t candidateTrees = 0;
            std::size_t candidateFamilies = 0;
            std::size_t matchedFamilies = 0;
            std::size_t matchedRows = 0;
            std::size_t matchedReadTrees = 0;
            std::size_t matchedWritePorts = 0;
            std::map<std::string, std::size_t> skipReasons;
        };

        void recordSkip(FamilyStats &stats, std::string_view reason)
        {
            ++stats.skipReasons[std::string(reason)];
        }

        // Normalizes a register symbol for the field-shape check: digit runs
        // collapse to '#', so robEntries_0_x and robEntries_12_x share the
        // shape robEntries_#_x.
        std::string regSymbolShape(std::string_view symbol)
        {
            std::string shape;
            shape.reserve(symbol.size());
            bool inDigits = false;
            for (const char ch : symbol)
            {
                if (ch >= '0' && ch <= '9')
                {
                    if (!inDigits)
                    {
                        shape.push_back('#');
                        inDigits = true;
                    }
                    continue;
                }
                inDigits = false;
                shape.push_back(ch);
            }
            return shape;
        }

        std::string makeUniqueMemoryName(const Graph &graph, const std::vector<std::string> &regSymbols)
        {
            std::string base = "asr_mem";
            if (!regSymbols.empty())
            {
                base += "$";
                base += Graph::normalizeComponent(regSymbols.front());
            }
            std::string candidate = base;
            int64_t suffix = 0;
            while (graph.findOperation(candidate).valid() || graph.findValue(candidate).valid())
            {
                ++suffix;
                candidate = base + "$" + std::to_string(suffix);
            }
            return candidate;
        }

        bool registerHasInitOrResetAttr(const Operation &regOp)
        {
            for (const auto &attr : regOp.attrs())
            {
                if (attr.key.rfind("init", 0) == 0 || attr.key.rfind("reset", 0) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        // Collects kRegisterReadPort ops of value's fanin cone that target
        // regSymbol. Bounded by the visited set.
        bool coneReferencesRegister(const Graph &graph, ValueId root, const std::string &regSymbol)
        {
            std::vector<ValueId> pending{root};
            std::unordered_set<ValueId, ValueIdHash> visited;
            while (!pending.empty())
            {
                const ValueId value = pending.back();
                pending.pop_back();
                if (!value.valid() || !visited.insert(value).second)
                {
                    continue;
                }
                const OperationId defId = graph.valueDef(value);
                if (!defId.valid())
                {
                    continue;
                }
                const Operation def = graph.getOperation(defId);
                if (def.kind() == OperationKind::kRegisterReadPort)
                {
                    const auto target = getStringAttr(def, "regSymbol");
                    if (target && *target == regSymbol)
                    {
                        return true;
                    }
                }
                pending.insert(pending.end(), def.operands().begin(), def.operands().end());
            }
            return false;
        }

        // Validates the write side of one family register (row k). On success
        // fills WriteMatch; on failure returns the skip reason.
        std::optional<std::string_view> matchRegisterWrite(const Graph &graph,
                                                           const FamilyReg &reg,
                                                           uint64_t row,
                                                           const std::vector<WritePortInfo> &writes,
                                                           WriteMatch &out)
        {
            if (writes.size() != 1)
            {
                return std::string_view("write_count");
            }
            const WritePortInfo &write = writes.front();
            const Operation regOp = graph.getOperation(reg.regOp);
            if (registerHasInitOrResetAttr(regOp))
            {
                return std::string_view("reset_attr");
            }

            // updateCond: OR of AND terms; each term must contain exactly one
            // row select for this register's own row, all sharing one index.
            ValueId writeIndex;
            const std::vector<ValueId> orTerms = flattenLogicOrTerms(graph, write.updateCond);
            for (const ValueId orTerm : orTerms)
            {
                const std::vector<ValueId> andTerms = flattenLogicAndTerms(graph, orTerm);
                std::optional<RowSelectMatch> termSelect;
                for (const ValueId andTerm : andTerms)
                {
                    auto candidate = matchWriteRowSelect(graph, andTerm);
                    if (!candidate)
                    {
                        continue;
                    }
                    if (termSelect)
                    {
                        // More than one row select in a single AND term.
                        return std::string_view("guard_form");
                    }
                    termSelect = std::move(candidate);
                }
                if (!termSelect || termSelect->row != row)
                {
                    return std::string_view("guard_form");
                }
                if (writeIndex.valid() && writeIndex != termSelect->index)
                {
                    return std::string_view("guard_form");
                }
                writeIndex = termSelect->index;
            }
            if (!writeIndex.valid())
            {
                return std::string_view("guard_form");
            }

            // nextValue: plain data, or the mux-hold form
            // kMux(sel, newValue, selfRead).
            ValueId data = write.nextValue;
            ValueId extraGuard;
            OperationId selfReadOp;
            OperationId nextValueMuxOp;
            if (coneReferencesRegister(graph, write.nextValue, reg.regSymbol))
            {
                const OperationId muxId = graph.valueDef(write.nextValue);
                if (!muxId.valid())
                {
                    return std::string_view("selfref_form");
                }
                const Operation mux = graph.getOperation(muxId);
                if (mux.kind() != OperationKind::kMux || mux.operands().size() != 3 ||
                    mux.results().size() != 1)
                {
                    return std::string_view("selfref_form");
                }
                const OperationId selfReadId = graph.valueDef(mux.operands()[2]);
                if (!selfReadId.valid())
                {
                    return std::string_view("selfref_form");
                }
                const Operation selfRead = graph.getOperation(selfReadId);
                if (selfRead.kind() != OperationKind::kRegisterReadPort ||
                    selfRead.results().size() != 1)
                {
                    return std::string_view("selfref_form");
                }
                const auto selfSymbol = getStringAttr(selfRead, "regSymbol");
                if (!selfSymbol || *selfSymbol != reg.regSymbol)
                {
                    return std::string_view("selfref_form");
                }
                data = mux.operands()[1];
                extraGuard = mux.operands()[0];
                selfReadOp = selfReadId;
                nextValueMuxOp = muxId;
            }

            out.write = write;
            out.index = writeIndex;
            out.data = data;
            out.extraGuard = extraGuard;
            out.selfReadOp = selfReadOp;
            out.nextValueMuxOp = nextValueMuxOp;
            return std::nullopt;
        }

        ValueId createBinaryLogicOp(Graph &graph,
                                    OperationKind kind,
                                    ValueId lhs,
                                    ValueId rhs,
                                    std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(), 1, false, ValueType::Logic);
            const OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        struct RewriteContext
        {
            Graph &graph;
            const std::vector<ReadTreeMatch> &allTrees;
            const std::unordered_map<std::string, std::vector<OperationId>> &readsByReg;
            FamilyStats &stats;
            std::function<void(std::string)> onError;
            bool failed = false;
            // Tree roots already replaced by earlier families' read ports.
            // Match snapshots taken before any rewrite may still reference
            // them (e.g. a bypass: family B's write data reads family A's
            // tree result); rewrite-time operands must be remapped.
            std::unordered_map<ValueId, ValueId, ValueIdHash> replacedRoots;
        };

        ValueId remapValue(const RewriteContext &ctx, ValueId value)
        {
            const auto it = ctx.replacedRoots.find(value);
            return it == ctx.replacedRoots.end() ? value : it->second;
        }

        void rewriteFamily(RewriteContext &ctx, const FamilyWork &family, int32_t elementWidth, bool isSigned)
        {
            Graph &graph = ctx.graph;
            std::vector<std::string> regSymbols;
            regSymbols.reserve(family.regs.size());
            for (const FamilyReg &reg : family.regs)
            {
                regSymbols.push_back(reg.regSymbol);
            }
            const std::string memSymbol = makeUniqueMemoryName(graph, regSymbols);

            const OperationId memOp = graph.createOperation(OperationKind::kMemory,
                                                            graph.internSymbol(memSymbol));
            graph.setAttr(memOp, "width", static_cast<int64_t>(elementWidth));
            graph.setAttr(memOp, "row", static_cast<int64_t>(family.regs.size()));
            graph.setAttr(memOp, "isSigned", isSigned);
            graph.setOpSrcLoc(memOp, makeTransformSrcLoc(std::string(kPassId), "memory"));

            // One kMemoryReadPort per read tree; the tree root value is
            // replaced by the port result.
            std::unordered_map<OperationId, ValueId, OperationIdHash> readReplacements;
            ValueId firstReadResult;
            for (const std::size_t treeIndex : family.trees)
            {
                const ReadTreeMatch &tree = ctx.allTrees[treeIndex];
                const wolvrix::lib::grh::Value rootValue = graph.getValue(tree.rootValue);
                const ValueId readResult = graph.createValue(graph.makeInternalValSym(),
                                                             elementWidth,
                                                             rootValue.isSigned(),
                                                             rootValue.type());
                const OperationId readOp = graph.createOperation(OperationKind::kMemoryReadPort,
                                                                 graph.makeInternalOpSym());
                graph.addOperand(readOp, remapValue(ctx, tree.index));
                graph.addResult(readOp, readResult);
                graph.setAttr(readOp, "memSymbol", memSymbol);
                const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), "read");
                graph.setOpSrcLoc(readOp, srcLoc);
                graph.setValueSrcLoc(readResult, srcLoc);
                if (!firstReadResult.valid())
                {
                    firstReadResult = readResult;
                }
                for (const ReadItem &item : tree.items)
                {
                    readReplacements.emplace(item.readOp, readResult);
                }
                replaceUsers(graph, tree.rootValue, readResult, [&](const std::string &msg) {
                    ctx.onError(msg);
                    ctx.failed = true;
                });
                if (ctx.failed)
                {
                    return;
                }
                ctx.replacedRoots.emplace(tree.rootValue, readResult);
            }

            // One kMemoryWritePort per register.
            for (const FamilyReg &reg : family.regs)
            {
                const WriteMatch &match = *reg.write;
                ValueId cond = remapValue(ctx, match.write.updateCond);
                if (match.extraGuard.valid())
                {
                    // OR-of-ANDs(updateCond) has the row select as a common
                    // factor of every term, so ANDing the mux sel on top is
                    // the same expression as rebuilding the OR of
                    // (common guard AND row select AND sel) terms.
                    cond = createBinaryLogicOp(graph,
                                               OperationKind::kLogicAnd,
                                               cond,
                                               remapValue(ctx, match.extraGuard),
                                               "write_guard");
                }
                const OperationId writeOp = graph.createOperation(OperationKind::kMemoryWritePort,
                                                                  graph.makeInternalOpSym());
                graph.addOperand(writeOp, cond);
                graph.addOperand(writeOp, remapValue(ctx, match.index));
                graph.addOperand(writeOp, remapValue(ctx, match.data));
                graph.addOperand(writeOp, remapValue(ctx, match.write.mask));
                for (const ValueId event : match.write.events)
                {
                    graph.addOperand(writeOp, remapValue(ctx, event));
                }
                graph.setAttr(writeOp, "memSymbol", memSymbol);
                graph.setAttr(writeOp, "eventEdge", match.write.eventEdges);
                graph.setOpSrcLoc(writeOp, makeTransformSrcLoc(std::string(kPassId), "write"));
            }

            // Erase the old write ports, read ports and register decls; the
            // dead tree/guard logic is left for dead-code elimination.
            for (const FamilyReg &reg : family.regs)
            {
                if (!graph.eraseOp(reg.write->write.op))
                {
                    ctx.onError("Failed to erase register write port for " + reg.regSymbol);
                    ctx.failed = true;
                    return;
                }
            }
            for (const FamilyReg &reg : family.regs)
            {
                const auto readsIt = ctx.readsByReg.find(reg.regSymbol);
                if (readsIt != ctx.readsByReg.end())
                {
                    for (const OperationId readOp : readsIt->second)
                    {
                        ValueId replacement = firstReadResult;
                        const auto replaceIt = readReplacements.find(readOp);
                        if (replaceIt != readReplacements.end())
                        {
                            replacement = replaceIt->second;
                        }
                        if (!graph.eraseOp(readOp, std::array<ValueId, 1>{replacement}))
                        {
                            ctx.onError("Failed to erase register read port for " + reg.regSymbol);
                            ctx.failed = true;
                            return;
                        }
                    }
                }
                if (!graph.eraseOp(reg.regOp))
                {
                    ctx.onError("Failed to erase register " + reg.regSymbol);
                    ctx.failed = true;
                    return;
                }
            }

            ++ctx.stats.matchedFamilies;
            ctx.stats.matchedRows += family.regs.size();
            ctx.stats.matchedReadTrees += family.trees.size();
            ctx.stats.matchedWritePorts += family.regs.size();
        }

        // Runs the full match pipeline for one graph. When rewrite is true,
        // matched families are rewritten; otherwise only stats are collected.
        bool processGraph(Graph &graph,
                          std::size_t minEntries,
                          bool rewrite,
                          FamilyStats &stats,
                          const std::function<void(std::string)> &onError)
        {
            // Index read/write ports by target register symbol.
            std::unordered_map<std::string, std::vector<OperationId>> readsByReg;
            std::unordered_map<std::string, std::vector<WritePortInfo>> writesByReg;
            for (const OperationId opId : graph.operations())
            {
                const Operation op = graph.getOperation(opId);
                if (op.kind() == OperationKind::kRegisterReadPort && op.results().size() == 1)
                {
                    const auto regSymbol = getStringAttr(op, "regSymbol");
                    if (regSymbol && !regSymbol->empty())
                    {
                        readsByReg[*regSymbol].push_back(opId);
                    }
                    continue;
                }
                if (op.kind() == OperationKind::kRegisterWritePort && op.operands().size() >= 4 &&
                    op.results().empty())
                {
                    const auto regSymbol = getStringAttr(op, "regSymbol");
                    if (!regSymbol || regSymbol->empty())
                    {
                        continue;
                    }
                    const auto eventEdges = getAttr<std::vector<std::string>>(op, "eventEdge");
                    const std::size_t eventCount = op.operands().size() - 3;
                    if (!eventEdges || eventEdges->size() != eventCount)
                    {
                        continue;
                    }
                    WritePortInfo info;
                    info.op = opId;
                    info.updateCond = op.operands()[0];
                    info.nextValue = op.operands()[1];
                    info.mask = op.operands()[2];
                    info.events.assign(op.operands().begin() + 3, op.operands().end());
                    info.eventEdges = *eventEdges;
                    writesByReg[*regSymbol].push_back(std::move(info));
                }
            }

            // Discover candidate read trees.
            std::vector<ReadTreeMatch> trees;
            for (const OperationId opId : graph.operations())
            {
                const OperationKind kind = graph.getOperation(opId).kind();
                if (kind != OperationKind::kMux && kind != OperationKind::kOr &&
                    kind != OperationKind::kLogicOr)
                {
                    continue;
                }
                auto tree = matchReadTree(graph, opId);
                if (tree)
                {
                    trees.push_back(std::move(*tree));
                }
            }
            stats.candidateTrees += trees.size();
            if (trees.empty())
            {
                return true;
            }

            // Union trees into families via shared register symbols.
            std::unordered_map<std::string, std::size_t> treeByReg;
            std::vector<std::size_t> parent(trees.size());
            for (std::size_t i = 0; i < trees.size(); ++i)
            {
                parent[i] = i;
            }
            const std::function<std::size_t(std::size_t)> find = [&](std::size_t x) -> std::size_t {
                while (parent[x] != x)
                {
                    parent[x] = parent[parent[x]];
                    x = parent[x];
                }
                return x;
            };
            auto unite = [&](std::size_t a, std::size_t b) {
                parent[find(a)] = find(b);
            };
            for (std::size_t i = 0; i < trees.size(); ++i)
            {
                for (const ReadItem &item : trees[i].items)
                {
                    const auto [it, inserted] = treeByReg.emplace(item.regSymbol, i);
                    if (!inserted)
                    {
                        unite(it->second, i);
                    }
                }
            }
            std::map<std::size_t, FamilyWork> families;
            for (std::size_t i = 0; i < trees.size(); ++i)
            {
                FamilyWork &family = families[find(i)];
                family.trees.push_back(i);
                for (const ReadItem &item : trees[i].items)
                {
                    if (family.regIndices.emplace(item.regSymbol, family.regs.size()).second)
                    {
                        FamilyReg reg;
                        reg.regSymbol = item.regSymbol;
                        reg.regOp = graph.findOperation(item.regSymbol);
                        family.regs.push_back(std::move(reg));
                    }
                }
            }

            // One rewrite context per graph: replacedRoots accumulates so
            // later families remap match snapshots that referenced earlier
            // families' tree roots.
            RewriteContext ctx{graph, trees, readsByReg, stats, onError, false, {}};
            for (auto &[leader, family] : families)
            {
                (void)leader;
                ++stats.candidateFamilies;
                auto skip = [&](std::string_view reason) {
                    recordSkip(stats, reason);
                };

                // Row assignment from gated items; consistency across trees.
                bool rowConflict = false;
                for (const std::size_t treeIndex : family.trees)
                {
                    const ReadTreeMatch &tree = trees[treeIndex];
                    for (const ReadItem &item : tree.items)
                    {
                        if (item.isDefault)
                        {
                            continue;
                        }
                        FamilyReg &reg = family.regs[family.regIndices[item.regSymbol]];
                        if (reg.row && *reg.row != item.row)
                        {
                            rowConflict = true;
                            break;
                        }
                        reg.row = item.row;
                    }
                    if (rowConflict)
                    {
                        break;
                    }
                }
                if (rowConflict)
                {
                    skip("row_mismatch");
                    continue;
                }

                const std::size_t entryCount = family.regs.size();
                if (entryCount < minEntries)
                {
                    skip("small_family");
                    continue;
                }

                // Field shape: width, signedness, symbol shape must agree.
                int32_t elementWidth = 0;
                bool elementSigned = false;
                std::string elementShape;
                bool fieldMismatch = false;
                for (const FamilyReg &reg : family.regs)
                {
                    const Operation regOp = graph.getOperation(reg.regOp);
                    const auto width = getAttr<int64_t>(regOp, "width");
                    const bool isSigned = getAttr<bool>(regOp, "isSigned").value_or(false);
                    if (!width || *width <= 0 || *width > INT32_MAX)
                    {
                        fieldMismatch = true;
                        break;
                    }
                    if (elementWidth == 0)
                    {
                        elementWidth = static_cast<int32_t>(*width);
                        elementSigned = isSigned;
                        elementShape = regSymbolShape(reg.regSymbol);
                        continue;
                    }
                    if (elementWidth != static_cast<int32_t>(*width) || elementSigned != isSigned ||
                        elementShape != regSymbolShape(reg.regSymbol))
                    {
                        fieldMismatch = true;
                        break;
                    }
                }
                if (fieldMismatch)
                {
                    skip("field_mismatch");
                    continue;
                }
                for (const std::size_t treeIndex : family.trees)
                {
                    if (graph.valueWidth(trees[treeIndex].rootValue) != elementWidth)
                    {
                        fieldMismatch = true;
                        break;
                    }
                }
                if (fieldMismatch)
                {
                    skip("field_mismatch");
                    continue;
                }

                // Rows must be exactly [0, N); mux-chain default rows are
                // resolved by elimination.
                std::set<uint64_t> gatedRows;
                bool rowMismatch = false;
                std::vector<std::size_t> ungatedRegs;
                for (std::size_t i = 0; i < family.regs.size(); ++i)
                {
                    const std::optional<uint64_t> row = family.regs[i].row;
                    if (!row)
                    {
                        ungatedRegs.push_back(i);
                        continue;
                    }
                    if (*row >= entryCount || !gatedRows.insert(*row).second)
                    {
                        rowMismatch = true;
                        break;
                    }
                }
                if (!rowMismatch)
                {
                    const std::size_t missing = entryCount - gatedRows.size();
                    if (missing != ungatedRegs.size())
                    {
                        rowMismatch = true;
                    }
                    else if (missing == 1)
                    {
                        for (uint64_t row = 0; row < entryCount; ++row)
                        {
                            if (!gatedRows.contains(row))
                            {
                                family.regs[ungatedRegs.front()].row = row;
                                break;
                            }
                        }
                    }
                    else if (missing > 1)
                    {
                        // Ambiguous default rows: cannot tell which register
                        // holds which row.
                        rowMismatch = true;
                    }
                }
                if (rowMismatch)
                {
                    skip("row_mismatch");
                    continue;
                }

                // Every tree must cover the full row range; a partial tree
                // computes something a plain kMemoryReadPort cannot
                // reproduce for the uncovered rows.
                bool treeIncomplete = false;
                for (const std::size_t treeIndex : family.trees)
                {
                    std::set<uint64_t> covered;
                    for (const ReadItem &item : trees[treeIndex].items)
                    {
                        covered.insert(*family.regs[family.regIndices[item.regSymbol]].row);
                    }
                    if (covered.size() != entryCount)
                    {
                        treeIncomplete = true;
                        break;
                    }
                }
                if (treeIncomplete)
                {
                    skip("tree_incomplete");
                    continue;
                }

                // Write side per register.
                bool writeFailed = false;
                for (FamilyReg &reg : family.regs)
                {
                    const auto writesIt = writesByReg.find(reg.regSymbol);
                    static const std::vector<WritePortInfo> kNoWrites;
                    const std::vector<WritePortInfo> &writes =
                        writesIt == writesByReg.end() ? kNoWrites : writesIt->second;
                    WriteMatch match;
                    const auto reason = matchRegisterWrite(graph, reg, *reg.row, writes, match);
                    if (reason)
                    {
                        skip(*reason);
                        writeFailed = true;
                        break;
                    }
                    reg.write = std::move(match);
                }
                if (writeFailed)
                {
                    continue;
                }

                // Read closure: every read of a family register must be a
                // tree leaf or the register's write-mux self-read, and every
                // use of such a read result must stay inside the family
                // trees or that write mux.
                std::unordered_set<OperationId, OperationIdHash> familyInternalOps;
                std::unordered_set<OperationId, OperationIdHash> coveredReadOps;
                for (const std::size_t treeIndex : family.trees)
                {
                    const ReadTreeMatch &tree = trees[treeIndex];
                    familyInternalOps.insert(tree.internalOps.begin(), tree.internalOps.end());
                    for (const ReadItem &item : tree.items)
                    {
                        coveredReadOps.insert(item.readOp);
                    }
                }
                bool wildRead = false;
                for (const FamilyReg &reg : family.regs)
                {
                    const auto readsIt = readsByReg.find(reg.regSymbol);
                    if (readsIt == readsByReg.end())
                    {
                        continue;
                    }
                    for (const OperationId readOp : readsIt->second)
                    {
                        const bool covered = coveredReadOps.contains(readOp) ||
                                             (reg.write->selfReadOp.valid() &&
                                              reg.write->selfReadOp == readOp);
                        if (!covered)
                        {
                            wildRead = true;
                            break;
                        }
                        const Operation readOperation = graph.getOperation(readOp);
                        // Named Value: see the C++20 range-for lifetime note
                        // in matchReadTree.
                        const wolvrix::lib::grh::Value readValue =
                            graph.getValue(readOperation.results().front());
                        for (const auto &user : readValue.users())
                        {
                            if (!user.operation.valid())
                            {
                                continue;
                            }
                            if (familyInternalOps.contains(user.operation))
                            {
                                continue;
                            }
                            if (reg.write->nextValueMuxOp.valid() &&
                                reg.write->nextValueMuxOp == user.operation)
                            {
                                continue;
                            }
                            wildRead = true;
                            break;
                        }
                        if (wildRead)
                        {
                            break;
                        }
                        // A read result bound to an output port is a use
                        // outside the trees as well.
                        for (const auto &port : graph.outputPorts())
                        {
                            if (port.value == readOperation.results().front())
                            {
                                wildRead = true;
                                break;
                            }
                        }
                        if (wildRead)
                        {
                            break;
                        }
                    }
                    if (wildRead)
                    {
                        break;
                    }
                }
                if (wildRead)
                {
                    skip("wild_read");
                    continue;
                }

                if (!rewrite)
                {
                    ++stats.matchedFamilies;
                    stats.matchedRows += family.regs.size();
                    stats.matchedReadTrees += family.trees.size();
                    stats.matchedWritePorts += family.regs.size();
                    continue;
                }

                rewriteFamily(ctx, family, elementWidth, elementSigned);
                if (ctx.failed)
                {
                    return false;
                }
            }
            return true;
        }

        std::string formatSkipReasons(const std::map<std::string, std::size_t> &reasons)
        {
            std::string out;
            for (const auto &[reason, count] : reasons)
            {
                if (!out.empty())
                {
                    out += " ";
                }
                out += reason;
                out += "=";
                out += std::to_string(count);
            }
            return out;
        }
    } // namespace

    ArraySelectRecoveryPass::ArraySelectRecoveryPass()
        : ArraySelectRecoveryPass(ArraySelectRecoveryOptions{})
    {
    }

    ArraySelectRecoveryPass::ArraySelectRecoveryPass(ArraySelectRecoveryOptions options)
        : Pass("array-select-recovery",
               "array-select-recovery",
               "Recover scalar register arrays with one-hot select read/write into kMemory"),
          options_(std::move(options))
    {
    }

    PassResult ArraySelectRecoveryPass::run()
    {
        PassResult result;
        FamilyStats total;

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            wolvrix::lib::grh::Graph &graph = *entry.second;
            FamilyStats graphStats;
            bool ok = processGraph(graph,
                                   options_.minEntries,
                                   options_.rewrite,
                                   graphStats,
                                   [&](const std::string &msg) { this->error(graph, msg); });
            if (!ok)
            {
                result.failed = true;
            }
            if (graphStats.candidateTrees > 0)
            {
                logInfo("array-select-recovery: graph=" + graph.symbol() +
                        " trees=" + std::to_string(graphStats.candidateTrees) +
                        " families=" + std::to_string(graphStats.candidateFamilies) +
                        " matched=" + std::to_string(graphStats.matchedFamilies) +
                        " rows=" + std::to_string(graphStats.matchedRows) +
                        " read_trees=" + std::to_string(graphStats.matchedReadTrees) +
                        " write_ports=" + std::to_string(graphStats.matchedWritePorts) +
                        (graphStats.skipReasons.empty()
                             ? std::string()
                             : " skip[" + formatSkipReasons(graphStats.skipReasons) + "]"));
            }
            total.candidateTrees += graphStats.candidateTrees;
            total.candidateFamilies += graphStats.candidateFamilies;
            total.matchedFamilies += graphStats.matchedFamilies;
            total.matchedRows += graphStats.matchedRows;
            total.matchedReadTrees += graphStats.matchedReadTrees;
            total.matchedWritePorts += graphStats.matchedWritePorts;
            for (const auto &[reason, count] : graphStats.skipReasons)
            {
                total.skipReasons[reason] += count;
            }
            if (graphStats.matchedFamilies > 0 && options_.rewrite)
            {
                result.changed = true;
            }
        }

        logInfo(std::string("array-select-recovery") + (options_.rewrite ? "" : " (census)") +
                ": trees=" + std::to_string(total.candidateTrees) +
                " families=" + std::to_string(total.candidateFamilies) +
                " matched=" + std::to_string(total.matchedFamilies) +
                " rows=" + std::to_string(total.matchedRows) +
                " read_trees=" + std::to_string(total.matchedReadTrees) +
                " write_ports=" + std::to_string(total.matchedWritePorts) +
                (total.skipReasons.empty()
                     ? std::string()
                     : " skip[" + formatSkipReasons(total.skipReasons) + "]"));
        return result;
    }

} // namespace wolvrix::lib::transform
