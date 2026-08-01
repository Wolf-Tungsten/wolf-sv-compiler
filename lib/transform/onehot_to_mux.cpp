// onehot-to-mux: rewrite one-hot decode or-trees into right-leaning kMux chains.
//
// Pattern (Form A, conservative): starting from a kOr root, expand through
// binary kOr operands. The whole tree is rewritten only if ALL of the
// following hold:
//   1. Every intermediate kOr result has exactly one user (the tree edge it
//      was reached from); shared subtrees are never split. The root itself is
//      only a candidate when its result does not feed another kOr, so each
//      maximal tree is considered exactly once.
//   2. Every leaf is kAnd(a, b) where one operand (either position) is the
//      result of kEq(idx, C) with
//        - the same idx ValueId across the whole tree,
//        - C produced by a kConstant op with no unknown bits, pairwise
//          distinct constants across leaves,
//      and the other operand is an arbitrary payload v_i. If both kAnd
//      operands match the eq form the leaf is ambiguous and rejected.
//   3. Term count is in [3, 1024] (2-term trees already look like a mux).
//   4. All payloads have the same width as the tree root result.
//
// Rewrite: reuse the leaf eq results and payloads and build
//   mux(eq(idx, C1), v1, mux(eq(idx, C2), v2, ... mux(eq(idx, Cn), vn, 0)))
// in original leaf order (DFS, operand 0 first), where 0 is a fresh kConstant
// of the tree width (dedup is left to RedundantElim). The root result is
// re-pointed at the chain head via replaceAllUses + bindOutputPort, following
// the logic-normalize convention; the old kOr/kAnd ops stay behind dead for
// DCE.
//
// kMux semantics (verified against the code base): operands[0] is the 1-bit
// select, operands[1] the true value, operands[2] the false value, and the
// result takes the (common) branch width. See grh-ir.md section 6.2.7,
// const_fold.cpp folding kMux as SVInt::conditional(operands[0], operands[1],
// operands[2]), and ingest.cpp makeMux(cond, lhs, rhs) emitting
// operands {cond, lhs, rhs}.
//
// Equivalence (2-state): the constants C_i are pairwise distinct, so at most
// one select eq(idx, C_i) is true at a time. When eq(idx, C_k) is true the
// or-tree yields v_k (every other and-term is 0) and the mux chain picks
// v_k; when no select is true the or-tree yields 0, which is exactly the mux
// chain default. Under 4-state simulation X-propagation may differ (or-of-
// ands is symmetric, the mux chain prioritizes earlier selects); the rewrite
// only claims 2-state equivalence.
//
// Soundness restriction: GRH kAnd follows SV resize semantics (grh-ir.md
// section 6.2.2: result width max(L, R), the narrower operand is zero/sign
// extended, NOT broadcast), so kAnd(eq, v) equals (eq ? v : 0) only when the
// select and payload widths match. A bare kEq result is 1-bit, hence Form A
// rewrites 1-bit trees only (root width == 1). Wider decode trees use
// kReplicate-broadcast selects or kMux already and are left to a future
// extension.

#include "transform/onehot_to_mux.hpp"

#include "core/grh.hpp"

#include "slang/numeric/SVInt.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        constexpr std::size_t kMinTerms = 3;
        constexpr std::size_t kMaxTerms = 1024;

        void replaceUsers(wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::ValueId from,
                          wolvrix::lib::grh::ValueId to,
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

        struct EqConstMatch
        {
            wolvrix::lib::grh::ValueId index;
            slang::SVInt constant;
        };

        struct LeafMatch
        {
            wolvrix::lib::grh::ValueId select;
            wolvrix::lib::grh::ValueId payload;
            slang::SVInt constant;
        };

        struct TreeMatch
        {
            wolvrix::lib::grh::ValueId rootValue;
            std::vector<LeafMatch> leaves;
        };

        // Matches `value` against kEq(index, C) in either operand order, where
        // C is a kConstant result without unknown bits. The constant's declared
        // width must equal the index width so plain bit-pattern equality
        // decides whether two selects can fire at the same time (no extension
        // semantics involved). Returns std::nullopt when there is no match or
        // when both operands are constants (ambiguous index).
        std::optional<EqConstMatch> matchEqConst(const wolvrix::lib::grh::Graph &graph,
                                                 wolvrix::lib::grh::ValueId value)
        {
            const wolvrix::lib::grh::OperationId defId = graph.getValue(value).definingOp();
            if (!defId.valid())
            {
                return std::nullopt;
            }
            const wolvrix::lib::grh::Operation def = graph.getOperation(defId);
            if (def.kind() != wolvrix::lib::grh::OperationKind::kEq)
            {
                return std::nullopt;
            }
            const auto operands = def.operands();
            if (def.results().size() != 1 || operands.size() != 2)
            {
                return std::nullopt;
            }
            // The value doubles as the kMux select, which must be 1-bit.
            if (graph.getValue(value).width() != 1)
            {
                return std::nullopt;
            }

            std::optional<EqConstMatch> match;
            for (std::size_t i = 0; i < 2; ++i)
            {
                const wolvrix::lib::grh::ValueId constId = operands[i];
                const wolvrix::lib::grh::ValueId indexId = operands[1 - i];
                const wolvrix::lib::grh::OperationId constDefId =
                    graph.getValue(constId).definingOp();
                if (!constDefId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation constDef = graph.getOperation(constDefId);
                if (constDef.kind() != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    continue;
                }
                const auto attr = constDef.attr("constValue");
                if (!attr)
                {
                    continue;
                }
                const auto *literal = std::get_if<std::string>(&*attr);
                if (literal == nullptr)
                {
                    continue;
                }
                const int32_t constWidth = graph.getValue(constId).width();
                const int32_t indexWidth = graph.getValue(indexId).width();
                if (constWidth <= 0 || constWidth != indexWidth)
                {
                    continue;
                }
                slang::SVInt parsed;
                try
                {
                    parsed = slang::SVInt::fromString(*literal);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                if (parsed.hasUnknown())
                {
                    continue;
                }
                parsed.setSigned(false);
                parsed = parsed.resize(static_cast<slang::bitwidth_t>(constWidth));
                if (match)
                {
                    // Both operands are constants: the index is ambiguous.
                    return std::nullopt;
                }
                match = EqConstMatch{indexId, parsed};
            }
            return match;
        }

        // Collects the leaves of the or-tree rooted at rootOp (which must be a
        // kOr). Returns false unless the whole tree satisfies the Form A
        // pattern documented at the top of this file.
        bool matchTree(const wolvrix::lib::grh::Graph &graph,
                       const wolvrix::lib::grh::Operation &rootOp,
                       TreeMatch &out)
        {
            namespace grh = wolvrix::lib::grh;
            const auto rootOperands = rootOp.operands();
            if (rootOp.results().size() != 1 || rootOperands.size() != 2)
            {
                return false;
            }
            const grh::ValueId rootValue = rootOp.results().front();
            if (!rootValue.valid())
            {
                return false;
            }
            const grh::Value rootVal = graph.getValue(rootValue);
            const int32_t rootWidth = rootVal.width();
            // See the soundness restriction in the file header: kAnd(eq, v)
            // equals (eq ? v : 0) only at matching widths, and a bare kEq
            // result is 1-bit.
            if (rootWidth != 1)
            {
                return false;
            }
            // A kOr result feeding another kOr is an intermediate node of a
            // larger tree, not a root; the enclosing kOr is the candidate.
            for (const auto &user : rootVal.users())
            {
                if (user.operation.valid() &&
                    graph.getOperation(user.operation).kind() == grh::OperationKind::kOr)
                {
                    return false;
                }
            }

            std::unordered_set<grh::OperationId, grh::OperationIdHash> visited;
            visited.insert(rootOp.id());
            std::vector<grh::ValueId> stack;
            stack.push_back(rootOperands[1]);
            stack.push_back(rootOperands[0]);
            std::vector<LeafMatch> leaves;
            grh::ValueId index;

            while (!stack.empty())
            {
                const grh::ValueId current = stack.back();
                stack.pop_back();
                if (!current.valid())
                {
                    return false;
                }
                const grh::Value currentVal = graph.getValue(current);
                const grh::OperationId defId = currentVal.definingOp();
                if (!defId.valid())
                {
                    return false;
                }
                const grh::Operation def = graph.getOperation(defId);
                const auto defOperands = def.operands();
                if (def.kind() == grh::OperationKind::kOr)
                {
                    // Intermediate kOr: exactly one user (the tree edge it was
                    // reached from); never split a shared subtree.
                    if (currentVal.users().size() != 1)
                    {
                        return false;
                    }
                    if (def.results().size() != 1 || defOperands.size() != 2)
                    {
                        return false;
                    }
                    if (currentVal.width() != rootWidth)
                    {
                        return false;
                    }
                    if (!visited.insert(defId).second)
                    {
                        return false;
                    }
                    stack.push_back(defOperands[1]);
                    stack.push_back(defOperands[0]);
                    continue;
                }
                if (def.kind() != grh::OperationKind::kAnd)
                {
                    return false;
                }
                if (def.results().size() != 1 || defOperands.size() != 2)
                {
                    return false;
                }
                if (currentVal.width() != rootWidth)
                {
                    return false;
                }
                // Leaf: exactly one operand must match kEq(idx, C); the other
                // is the payload.
                std::optional<EqConstMatch> eqMatch;
                grh::ValueId select;
                grh::ValueId payload;
                for (std::size_t i = 0; i < 2; ++i)
                {
                    std::optional<EqConstMatch> candidate = matchEqConst(graph, defOperands[i]);
                    if (!candidate)
                    {
                        continue;
                    }
                    if (eqMatch)
                    {
                        // Both operands are eq(idx, C) results: ambiguous.
                        return false;
                    }
                    eqMatch = candidate;
                    select = defOperands[i];
                    payload = defOperands[1 - i];
                }
                if (!eqMatch || !payload.valid())
                {
                    return false;
                }
                if (graph.getValue(payload).width() != rootWidth)
                {
                    return false;
                }
                if (index.valid() && index != eqMatch->index)
                {
                    return false;
                }
                index = eqMatch->index;
                for (const auto &leaf : leaves)
                {
                    if (static_cast<bool>((leaf.constant == eqMatch->constant)))
                    {
                        // Duplicate constant: two selects may fire together.
                        return false;
                    }
                }
                leaves.push_back(LeafMatch{select, payload, eqMatch->constant});
                if (leaves.size() > kMaxTerms)
                {
                    return false;
                }
            }

            if (leaves.size() < kMinTerms || leaves.size() > kMaxTerms)
            {
                return false;
            }
            out.rootValue = rootValue;
            out.leaves = std::move(leaves);
            return true;
        }
    } // namespace

    OnehotToMuxPass::OnehotToMuxPass()
        : Pass("onehot-to-mux", "onehot-to-mux",
               "Rewrite one-hot or-of-ands decode trees into mux chains")
    {
    }

    PassResult OnehotToMuxPass::run()
    {
        PassResult result;
        std::size_t treesRewritten = 0;
        std::size_t termsTotal = 0;
        std::size_t muxCreated = 0;
        std::size_t zeroConstsCreated = 0;
        std::size_t opsRetired = 0;

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            wolvrix::lib::grh::Graph &graph = *entry.second;

            // Snapshot the op list: ops created below are appended to the
            // graph and must not be visited by this rewrite loop.
            const std::vector<wolvrix::lib::grh::OperationId> ops(graph.operations().begin(),
                                                                  graph.operations().end());
            for (const auto opId : ops)
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kOr)
                {
                    continue;
                }
                TreeMatch match;
                if (!matchTree(graph, op, match))
                {
                    continue;
                }

                const wolvrix::lib::grh::Value rootVal = graph.getValue(match.rootValue);
                const int32_t rootWidth = rootVal.width();
                const wolvrix::lib::grh::SrcLoc genLoc =
                    makeTransformSrcLoc("onehot-to-mux", "rewrite");

                // Default zero of the tree width; no dedup here, RedundantElim
                // merges identical constants later.
                const slang::SVInt zeroInt(static_cast<slang::bitwidth_t>(rootWidth), 0, false);
                const wolvrix::lib::grh::ValueId zeroValue =
                    graph.createValue(rootWidth, rootVal.isSigned(), rootVal.type());
                const wolvrix::lib::grh::OperationId zeroOp =
                    graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant);
                graph.addResult(zeroOp, zeroValue);
                graph.setAttr(zeroOp, "constValue",
                              zeroInt.toString(slang::LiteralBase::Hex, true,
                                               static_cast<slang::bitwidth_t>(rootWidth)));
                graph.setValueSrcLoc(zeroValue, genLoc);
                graph.setOpSrcLoc(zeroOp, genLoc);
                ++zeroConstsCreated;

                // Right-leaning mux chain in original leaf order, built from
                // the tail: mux(eq_n, v_n, 0) first, mux(eq_1, v_1, ...) last.
                wolvrix::lib::grh::ValueId tail = zeroValue;
                for (std::size_t i = match.leaves.size(); i-- > 0;)
                {
                    const wolvrix::lib::grh::ValueId muxValue =
                        graph.createValue(rootWidth, rootVal.isSigned(), rootVal.type());
                    const wolvrix::lib::grh::OperationId muxOp =
                        graph.createOperation(wolvrix::lib::grh::OperationKind::kMux);
                    graph.addOperand(muxOp, match.leaves[i].select);
                    graph.addOperand(muxOp, match.leaves[i].payload);
                    graph.addOperand(muxOp, tail);
                    graph.addResult(muxOp, muxValue);
                    graph.setValueSrcLoc(muxValue, genLoc);
                    graph.setOpSrcLoc(muxOp, genLoc);
                    tail = muxValue;
                    ++muxCreated;
                }

                bool replaceFailed = false;
                replaceUsers(graph, match.rootValue, tail, [&](const std::string &msg) {
                    this->error(graph, op, msg);
                    replaceFailed = true;
                });
                if (replaceFailed)
                {
                    result.failed = true;
                    continue;
                }
                // The old kOr/kAnd ops are now dead and left for DCE.
                ++treesRewritten;
                termsTotal += match.leaves.size();
                opsRetired += match.leaves.size() + (match.leaves.size() - 1);
                result.changed = true;
            }
        }

        logInfo("onehot-to-mux: trees=" + std::to_string(treesRewritten) +
                " terms=" + std::to_string(termsTotal) +
                " mux_created=" + std::to_string(muxCreated) +
                " zero_consts_created=" + std::to_string(zeroConstsCreated) +
                " ops_retired=" + std::to_string(opsRetired));
        return result;
    }

} // namespace wolvrix::lib::transform
