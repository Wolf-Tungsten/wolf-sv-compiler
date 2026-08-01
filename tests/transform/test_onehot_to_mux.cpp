#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/const_fold.hpp"
#include "transform/onehot_to_mux.hpp"

#include "slang/numeric/SVInt.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[onehot-to-mux-tests] " << message << '\n';
        return 1;
    }

    wolvrix::lib::grh::ValueId makeConst(wolvrix::lib::grh::Graph &graph, const std::string &valueName, const std::string &opName, int32_t width, bool isSigned, const std::string &literal)
    {
        const wolvrix::lib::grh::SymbolId valueSym = graph.internSymbol(valueName);
        const wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
        const wolvrix::lib::grh::ValueId val = graph.createValue(valueSym, width, isSigned);
        const wolvrix::lib::grh::OperationId op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
        graph.addResult(op, val);
        graph.setAttr(op, "constValue", literal);
        return val;
    }

    wolvrix::lib::grh::ValueId makeInput(wolvrix::lib::grh::Graph &graph, const std::string &name, int32_t width)
    {
        const wolvrix::lib::grh::ValueId val = graph.createValue(graph.internSymbol(name), width, false);
        graph.bindInputPort(name, val);
        return val;
    }

    std::optional<slang::SVInt> getConstLiteral(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op)
    {
        (void)graph;
        auto attr = op.attr("constValue");
        if (!attr)
        {
            return std::nullopt;
        }
        const auto *val = std::get_if<std::string>(&*attr);
        if (!val)
        {
            return std::nullopt;
        }
        try
        {
            return slang::SVInt::fromString(*val);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::size_t countOpsOfKind(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::OperationKind kind)
    {
        std::size_t count = 0;
        for (const auto opId : graph.operations())
        {
            if (opId.valid() && graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    wolvrix::lib::grh::Operation defOpOf(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId value)
    {
        return graph.getOperation(graph.getValue(value).definingOp());
    }

    bool isDefinedBy(wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId value, wolvrix::lib::grh::OperationKind kind)
    {
        const wolvrix::lib::grh::OperationId def = graph.getValue(value).definingOp();
        return def.valid() && graph.getOperation(def).kind() == kind;
    }

    // Reads the constant driving `outputName` after folding.
    std::optional<slang::SVInt> foldedOutputLiteral(wolvrix::lib::grh::Graph &graph, const std::string &outputName)
    {
        const wolvrix::lib::grh::ValueId outVal = graph.outputPortValue(outputName);
        if (!outVal.valid() || !isDefinedBy(graph, outVal, wolvrix::lib::grh::OperationKind::kConstant))
        {
            return std::nullopt;
        }
        return getConstLiteral(graph, defOpOf(graph, outVal));
    }

    PassManagerResult runOnehotToMux(wolvrix::lib::grh::Design &design, PassDiagnostics &diags)
    {
        PassManager manager;
        manager.addPass(std::make_unique<OnehotToMuxPass>());
        return manager.run(design, diags);
    }

    // Runs const-fold, optionally preceded by onehot-to-mux.
    bool runFoldPipeline(wolvrix::lib::grh::Design &design, bool rewriteFirst)
    {
        PassManager manager;
        if (rewriteFirst)
        {
            manager.addPass(std::make_unique<OnehotToMuxPass>());
        }
        manager.addPass(std::make_unique<ConstantFoldPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        return res.success && !diags.hasError();
    }

    struct LeafParts
    {
        wolvrix::lib::grh::ValueId andValue;
        wolvrix::lib::grh::ValueId eqValue;
    };

    // Builds kAnd(kEq(idx, C), payload) with selectable operand orders.
    LeafParts buildLeaf(wolvrix::lib::grh::Graph &graph,
                        wolvrix::lib::grh::ValueId index,
                        int32_t indexWidth,
                        int32_t payloadWidth,
                        int64_t constant,
                        wolvrix::lib::grh::ValueId payload,
                        const std::string &prefix,
                        bool andSwapped = false,
                        bool eqSwapped = false)
    {
        const wolvrix::lib::grh::ValueId constVal =
            makeConst(graph, prefix + "_c", prefix + "_cop", indexWidth, false,
                      std::to_string(indexWidth) + "'d" + std::to_string(constant));
        const wolvrix::lib::grh::ValueId eqVal = graph.createValue(1, false);
        const wolvrix::lib::grh::OperationId eqOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kEq);
        graph.addOperand(eqOp, eqSwapped ? constVal : index);
        graph.addOperand(eqOp, eqSwapped ? index : constVal);
        graph.addResult(eqOp, eqVal);
        const wolvrix::lib::grh::ValueId andVal = graph.createValue(payloadWidth, false);
        const wolvrix::lib::grh::OperationId andOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd);
        graph.addOperand(andOp, andSwapped ? payload : eqVal);
        graph.addOperand(andOp, andSwapped ? eqVal : payload);
        graph.addResult(andOp, andVal);
        return LeafParts{andVal, eqVal};
    }

    // Builds a right-leaning kOr chain: or(l0, or(l1, ... or(l_{n-2}, l_{n-1}))).
    wolvrix::lib::grh::ValueId buildOrChain(wolvrix::lib::grh::Graph &graph,
                                            const std::vector<wolvrix::lib::grh::ValueId> &leaves,
                                            int32_t width)
    {
        wolvrix::lib::grh::ValueId tail = leaves.back();
        for (std::size_t i = leaves.size() - 1; i-- > 0;)
        {
            const wolvrix::lib::grh::ValueId orVal = graph.createValue(width, false);
            const wolvrix::lib::grh::OperationId orOp =
                graph.createOperation(wolvrix::lib::grh::OperationKind::kOr);
            graph.addOperand(orOp, leaves[i]);
            graph.addOperand(orOp, tail);
            graph.addResult(orOp, orVal);
            tail = orVal;
        }
        return tail;
    }

    struct BuiltTree
    {
        wolvrix::lib::grh::ValueId root;
        std::vector<wolvrix::lib::grh::ValueId> selects;
        std::vector<wolvrix::lib::grh::ValueId> payloads;
    };

    BuiltTree buildOnehotTree(wolvrix::lib::grh::Graph &graph,
                              wolvrix::lib::grh::ValueId index,
                              int32_t indexWidth,
                              int32_t payloadWidth,
                              const std::vector<int64_t> &constants,
                              const std::vector<wolvrix::lib::grh::ValueId> &payloads,
                              const std::string &prefix)
    {
        BuiltTree tree;
        std::vector<wolvrix::lib::grh::ValueId> leaves;
        for (std::size_t i = 0; i < constants.size(); ++i)
        {
            const LeafParts leaf =
                buildLeaf(graph, index, indexWidth, payloadWidth, constants[i], payloads[i],
                          prefix + std::to_string(i));
            leaves.push_back(leaf.andValue);
            tree.selects.push_back(leaf.eqValue);
            tree.payloads.push_back(payloads[i]);
        }
        tree.root = buildOrChain(graph, leaves, payloadWidth);
        return tree;
    }

    // Walks the rewritten mux chain from `head` and checks
    // mux(sel0, p0, mux(sel1, p1, ... mux(selN-1, pN-1, zero))).
    bool checkMuxChain(wolvrix::lib::grh::Graph &graph,
                       wolvrix::lib::grh::ValueId head,
                       const BuiltTree &tree)
    {
        wolvrix::lib::grh::ValueId current = head;
        for (std::size_t i = 0; i < tree.selects.size(); ++i)
        {
            if (!current.valid() ||
                !isDefinedBy(graph, current, wolvrix::lib::grh::OperationKind::kMux))
            {
                return false;
            }
            const wolvrix::lib::grh::Operation muxOp = defOpOf(graph, current);
            const auto operands = muxOp.operands();
            if (operands.size() != 3 || operands[0] != tree.selects[i] ||
                operands[1] != tree.payloads[i])
            {
                return false;
            }
            current = operands[2];
        }
        if (!current.valid() ||
            !isDefinedBy(graph, current, wolvrix::lib::grh::OperationKind::kConstant))
        {
            return false;
        }
        const std::optional<slang::SVInt> zeroLit = getConstLiteral(graph, defOpOf(graph, current));
        if (!zeroLit)
        {
            return false;
        }
        const slang::SVInt expectedZero = slang::SVInt::fromString("1'b0").resize(1);
        return static_cast<bool>((*zeroLit == expectedZero));
    }

    int testThreeTermTreeRewrites()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_pos3");

        const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
        const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
        const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
        const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
        const BuiltTree tree = buildOnehotTree(graph, idx, 2, 1, {0, 1, 2}, {p0, p1, p2}, "t");
        graph.bindOutputPort("out", tree.root);

        const std::size_t opsBefore = graph.operations().size();
        if (opsBefore != 11)
        {
            return fail("3-term fixture must start with 11 ops");
        }

        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected onehot-to-mux to succeed and report changes on 3-term tree");
        }

        // 3 mux + 1 zero constant are added; old ops stay dead for DCE.
        if (graph.operations().size() != opsBefore + 4)
        {
            return fail("Expected exactly 4 new ops (3 mux + 1 constant)");
        }
        if (countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kMux) != 3 ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kConstant) != 4 ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kEq) != 3 ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kAnd) != 3 ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kOr) != 2)
        {
            return fail("Unexpected op kind counts after rewriting 3-term tree");
        }

        const wolvrix::lib::grh::ValueId out = graph.outputPortValue("out");
        if (!out.valid() || !isDefinedBy(graph, out, wolvrix::lib::grh::OperationKind::kMux))
        {
            return fail("Output must be redefined by the mux chain head");
        }
        const wolvrix::lib::grh::Value outVal = graph.getValue(out);
        if (outVal.width() != 1 || outVal.isSigned() ||
            outVal.type() != wolvrix::lib::grh::ValueType::Logic)
        {
            return fail("Mux chain head must keep the root result width/sign/type");
        }
        if (!checkMuxChain(graph, out, tree))
        {
            return fail("Mux chain must reuse eq results and payloads in leaf order with zero default");
        }
        return 0;
    }

    int testFourTermTreeRewrites()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_pos4");

        const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
        const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
        const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
        const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
        const wolvrix::lib::grh::ValueId p3 = makeInput(graph, "p3", 1);

        // Mix operand orders: the matcher must accept eq(idx, C) and
        // eq(C, idx), and the eq result in either kAnd position.
        BuiltTree tree;
        std::vector<wolvrix::lib::grh::ValueId> leaves;
        const std::vector<int64_t> constants = {0, 1, 2, 3};
        const std::vector<wolvrix::lib::grh::ValueId> payloadVals = {p0, p1, p2, p3};
        const std::vector<bool> andSwapped = {false, true, false, true};
        const std::vector<bool> eqSwapped = {false, false, true, true};
        for (std::size_t i = 0; i < constants.size(); ++i)
        {
            const LeafParts leaf =
                buildLeaf(graph, idx, 2, 1, constants[i], payloadVals[i],
                          "t" + std::to_string(i), andSwapped[i], eqSwapped[i]);
            leaves.push_back(leaf.andValue);
            tree.selects.push_back(leaf.eqValue);
            tree.payloads.push_back(payloadVals[i]);
        }
        tree.root = buildOrChain(graph, leaves, 1);
        graph.bindOutputPort("out", tree.root);

        const std::size_t opsBefore = graph.operations().size();
        if (opsBefore != 15)
        {
            return fail("4-term fixture must start with 15 ops");
        }

        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected onehot-to-mux to succeed and report changes on 4-term tree");
        }
        if (graph.operations().size() != opsBefore + 5)
        {
            return fail("Expected exactly 5 new ops (4 mux + 1 constant)");
        }
        if (countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kMux) != 4 ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kConstant) != 5)
        {
            return fail("Unexpected op kind counts after rewriting 4-term tree");
        }
        const wolvrix::lib::grh::ValueId out = graph.outputPortValue("out");
        if (!out.valid() || !checkMuxChain(graph, out, tree))
        {
            return fail("4-term mux chain shape mismatch (order or operand reuse)");
        }
        return 0;
    }

    // Builds the 4-term tree with idx and payloads bound to constants, folds
    // it (with or without a preceding onehot-to-mux), and returns the folded
    // output constant.
    std::optional<slang::SVInt> evalFoldedTree(int64_t idxValue, bool rewriteFirst)
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_tt");
        const wolvrix::lib::grh::ValueId idx =
            makeConst(graph, "idx", "idx_op", 3, false, "3'd" + std::to_string(idxValue));
        const std::vector<int64_t> constants = {0, 1, 2, 5};
        const std::vector<int64_t> payloadBits = {1, 0, 1, 1};
        std::vector<wolvrix::lib::grh::ValueId> payloadVals;
        for (std::size_t i = 0; i < payloadBits.size(); ++i)
        {
            payloadVals.push_back(makeConst(graph, "pv" + std::to_string(i),
                                            "pv_op" + std::to_string(i), 1, false,
                                            payloadBits[i] ? "1'b1" : "1'b0"));
        }
        const BuiltTree tree = buildOnehotTree(graph, idx, 3, 1, constants, payloadVals, "t");
        graph.bindOutputPort("out", tree.root);
        if (!runFoldPipeline(design, rewriteFirst))
        {
            return std::nullopt;
        }
        return foldedOutputLiteral(graph, "out");
    }

    int testTruthTableConstFold()
    {
        for (int64_t idxValue = 0; idxValue < 8; ++idxValue)
        {
            const std::optional<slang::SVInt> rewritten = evalFoldedTree(idxValue, true);
            const std::optional<slang::SVInt> reference = evalFoldedTree(idxValue, false);
            if (!rewritten || !reference)
            {
                return fail("const-fold must reduce both tree forms to an output constant");
            }
            const int64_t bit =
                (idxValue == 0 || idxValue == 2 || idxValue == 5) ? 1 : 0;
            const slang::SVInt expected =
                slang::SVInt::fromString(bit ? "1'b1" : "1'b0").resize(1);
            if (!static_cast<bool>((*rewritten == expected)) ||
                !static_cast<bool>((*rewritten == *reference)))
            {
                return fail("truth mismatch between rewritten and original tree at idx=" +
                            std::to_string(idxValue));
            }
        }
        return 0;
    }

    int testSharedIntermediateNotRewritten()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_shared");

        const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
        const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
        const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
        const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
        const LeafParts l0 = buildLeaf(graph, idx, 2, 1, 0, p0, "t0");
        const LeafParts l1 = buildLeaf(graph, idx, 2, 1, 1, p1, "t1");
        const LeafParts l2 = buildLeaf(graph, idx, 2, 1, 2, p2, "t2");

        // sub = or(l0, l1) is shared between the tree root and a side user.
        const wolvrix::lib::grh::ValueId subVal = graph.createValue(1, false);
        const wolvrix::lib::grh::OperationId subOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kOr);
        graph.addOperand(subOp, l0.andValue);
        graph.addOperand(subOp, l1.andValue);
        graph.addResult(subOp, subVal);
        const wolvrix::lib::grh::ValueId rootVal = graph.createValue(1, false);
        const wolvrix::lib::grh::OperationId rootOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kOr);
        graph.addOperand(rootOp, subVal);
        graph.addOperand(rootOp, l2.andValue);
        graph.addResult(rootOp, rootVal);
        graph.bindOutputPort("out", rootVal);
        const wolvrix::lib::grh::ValueId sideVal = graph.createValue(1, false);
        const wolvrix::lib::grh::OperationId sideOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kNot);
        graph.addOperand(sideOp, subVal);
        graph.addResult(sideOp, sideVal);
        graph.bindOutputPort("side", sideVal);

        const std::size_t opsBefore = graph.operations().size();
        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("Expected onehot-to-mux to succeed on shared-subtree graph");
        }
        if (res.changed)
        {
            return fail("Tree with a shared intermediate kOr must not be rewritten");
        }
        if (graph.operations().size() != opsBefore ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kMux) != 0)
        {
            return fail("No ops may be added when the tree is rejected");
        }
        const wolvrix::lib::grh::ValueId out = graph.outputPortValue("out");
        if (!out.valid() || !isDefinedBy(graph, out, wolvrix::lib::grh::OperationKind::kOr))
        {
            return fail("Output must stay defined by the original kOr root");
        }
        return 0;
    }

    int testNonMatchingShapesNotRewritten()
    {
        wolvrix::lib::grh::Design design;

        // (a) A leaf that is a bare eq result instead of kAnd.
        {
            wolvrix::lib::grh::Graph &graph = design.createGraph("g_bad_leaf");
            const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
            const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
            const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
            const LeafParts l0 = buildLeaf(graph, idx, 2, 1, 0, p0, "t0");
            const LeafParts l1 = buildLeaf(graph, idx, 2, 1, 1, p1, "t1");
            const LeafParts l2 = buildLeaf(graph, idx, 2, 1, 2, p0, "t2");
            const wolvrix::lib::grh::ValueId root =
                buildOrChain(graph, {l0.andValue, l1.andValue, l2.eqValue}, 1);
            graph.bindOutputPort("out", root);
        }

        // (b) One leaf decodes a different index value.
        {
            wolvrix::lib::grh::Graph &graph = design.createGraph("g_bad_idx");
            const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
            const wolvrix::lib::grh::ValueId idx2 = makeInput(graph, "idx2", 2);
            const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
            const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
            const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
            const LeafParts l0 = buildLeaf(graph, idx, 2, 1, 0, p0, "t0");
            const LeafParts l1 = buildLeaf(graph, idx, 2, 1, 1, p1, "t1");
            const LeafParts l2 = buildLeaf(graph, idx2, 2, 1, 2, p2, "t2");
            const wolvrix::lib::grh::ValueId root =
                buildOrChain(graph, {l0.andValue, l1.andValue, l2.andValue}, 1);
            graph.bindOutputPort("out", root);
        }

        // (c) Two leaves share the same constant.
        {
            wolvrix::lib::grh::Graph &graph = design.createGraph("g_dup_const");
            const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
            const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
            const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
            const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
            const LeafParts l0 = buildLeaf(graph, idx, 2, 1, 0, p0, "t0");
            const LeafParts l1 = buildLeaf(graph, idx, 2, 1, 0, p1, "t1");
            const LeafParts l2 = buildLeaf(graph, idx, 2, 1, 2, p2, "t2");
            const wolvrix::lib::grh::ValueId root =
                buildOrChain(graph, {l0.andValue, l1.andValue, l2.andValue}, 1);
            graph.bindOutputPort("out", root);
        }

        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("Expected onehot-to-mux to succeed on non-matching shapes");
        }
        if (res.changed)
        {
            return fail("Non-matching trees must not be rewritten");
        }
        for (const std::string name : {"g_bad_leaf", "g_bad_idx", "g_dup_const"})
        {
            wolvrix::lib::grh::Graph *graph = design.findGraph(name);
            if (graph == nullptr)
            {
                return fail("Missing graph " + name);
            }
            if (countOpsOfKind(*graph, wolvrix::lib::grh::OperationKind::kMux) != 0)
            {
                return fail("No kMux may appear in rejected graph " + name);
            }
            const wolvrix::lib::grh::ValueId out = graph->outputPortValue("out");
            if (!out.valid() || !isDefinedBy(*graph, out, wolvrix::lib::grh::OperationKind::kOr))
            {
                return fail("Output must stay kOr-defined in rejected graph " + name);
            }
        }
        return 0;
    }

    int testTwoTermTreeNotRewritten()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_two");

        const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 1);
        const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
        const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
        const BuiltTree tree = buildOnehotTree(graph, idx, 1, 1, {0, 1}, {p0, p1}, "t");
        graph.bindOutputPort("out", tree.root);

        const std::size_t opsBefore = graph.operations().size();
        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError() || res.changed)
        {
            return fail("2-term tree must not be rewritten");
        }
        if (graph.operations().size() != opsBefore ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kMux) != 0)
        {
            return fail("No ops may be added for a 2-term tree");
        }
        return 0;
    }

    int testPayloadWidthMismatchNotRewritten()
    {
        wolvrix::lib::grh::Design design;

        // (a) Root-width tree with one payload wider than the root.
        {
            wolvrix::lib::grh::Graph &graph = design.createGraph("g_mixed_width");
            const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
            const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
            const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 2);
            const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
            const LeafParts l0 = buildLeaf(graph, idx, 2, 1, 0, p0, "t0");
            const LeafParts l1 = buildLeaf(graph, idx, 2, 1, 1, p1, "t1");
            const LeafParts l2 = buildLeaf(graph, idx, 2, 1, 2, p2, "t2");
            const wolvrix::lib::grh::ValueId root =
                buildOrChain(graph, {l0.andValue, l1.andValue, l2.andValue}, 1);
            graph.bindOutputPort("out", root);
        }

        // (b) Uniformly wider tree: kAnd(eq, v) zero-extends the 1-bit select
        // instead of broadcasting it, so this is NOT a mux candidate.
        {
            wolvrix::lib::grh::Graph &graph = design.createGraph("g_wide");
            const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
            const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 2);
            const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 2);
            const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 2);
            const BuiltTree tree = buildOnehotTree(graph, idx, 2, 2, {0, 1, 2}, {p0, p1, p2}, "t");
            graph.bindOutputPort("out", tree.root);
        }

        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("Expected onehot-to-mux to succeed on width-mismatch graphs");
        }
        if (res.changed)
        {
            return fail("Trees with payload widths != root width must not be rewritten");
        }
        for (const std::string name : {"g_mixed_width", "g_wide"})
        {
            wolvrix::lib::grh::Graph *graph = design.findGraph(name);
            if (graph == nullptr)
            {
                return fail("Missing graph " + name);
            }
            if (countOpsOfKind(*graph, wolvrix::lib::grh::OperationKind::kMux) != 0)
            {
                return fail("No kMux may appear in width-rejected graph " + name);
            }
        }
        return 0;
    }

    int testOutputPortAndUsersRebound()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_rebind");

        const wolvrix::lib::grh::ValueId idx = makeInput(graph, "idx", 2);
        const wolvrix::lib::grh::ValueId p0 = makeInput(graph, "p0", 1);
        const wolvrix::lib::grh::ValueId p1 = makeInput(graph, "p1", 1);
        const wolvrix::lib::grh::ValueId p2 = makeInput(graph, "p2", 1);
        const BuiltTree tree = buildOnehotTree(graph, idx, 2, 1, {0, 1, 2}, {p0, p1, p2}, "t");
        graph.bindOutputPort("out", tree.root);
        // A second user of the root result, besides the output port.
        const wolvrix::lib::grh::ValueId inv = graph.createValue(1, false);
        const wolvrix::lib::grh::OperationId notOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kNot);
        graph.addOperand(notOp, tree.root);
        graph.addResult(notOp, inv);
        graph.bindOutputPort("out_inv", inv);

        PassDiagnostics diags;
        const PassManagerResult res = runOnehotToMux(design, diags);
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected onehot-to-mux to rewrite the rebind fixture");
        }
        const wolvrix::lib::grh::ValueId out = graph.outputPortValue("out");
        if (!out.valid() || !isDefinedBy(graph, out, wolvrix::lib::grh::OperationKind::kMux))
        {
            return fail("Output port must be rebound to the mux chain head");
        }
        const wolvrix::lib::grh::ValueId outInv = graph.outputPortValue("out_inv");
        if (!outInv.valid() || !isDefinedBy(graph, outInv, wolvrix::lib::grh::OperationKind::kNot))
        {
            return fail("out_inv must stay kNot-defined");
        }
        const wolvrix::lib::grh::Operation notDef = defOpOf(graph, outInv);
        const auto notOperands = notDef.operands();
        if (notOperands.size() != 1 || notOperands[0] != out)
        {
            return fail("Existing users of the root must be rewired to the mux chain head");
        }
        if (!graph.getValue(tree.root).users().empty())
        {
            return fail("Old root value must have no users after the rewrite");
        }
        return 0;
    }

    int testPassRegistration()
    {
        bool listed = false;
        for (const std::string &name : availableTransformPasses())
        {
            if (name == "onehot-to-mux")
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            return fail("onehot-to-mux must be listed by availableTransformPasses");
        }
        std::string error;
        const std::vector<std::string_view> noArgs;
        std::unique_ptr<Pass> pass = makePass("onehot-to-mux", noArgs, error);
        if (!pass)
        {
            return fail("makePass must create onehot-to-mux: " + error);
        }
        const std::vector<std::string_view> badArgs = {"unexpected"};
        if (makePass("onehot-to-mux", badArgs, error))
        {
            return fail("onehot-to-mux must reject arguments");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int rc = testThreeTermTreeRewrites())
    {
        return rc;
    }
    if (const int rc = testFourTermTreeRewrites())
    {
        return rc;
    }
    if (const int rc = testTruthTableConstFold())
    {
        return rc;
    }
    if (const int rc = testSharedIntermediateNotRewritten())
    {
        return rc;
    }
    if (const int rc = testNonMatchingShapesNotRewritten())
    {
        return rc;
    }
    if (const int rc = testTwoTermTreeNotRewritten())
    {
        return rc;
    }
    if (const int rc = testPayloadWidthMismatchNotRewritten())
    {
        return rc;
    }
    if (const int rc = testOutputPortAndUsersRebound())
    {
        return rc;
    }
    if (const int rc = testPassRegistration())
    {
        return rc;
    }
    return 0;
}
