#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/const_fold.hpp"
#include "transform/logic_normalize.hpp"

#include "slang/numeric/SVInt.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[logic-normalize-tests] " << message << '\n';
        return 1;
    }

    wolvrix::lib::grh::ValueId makeConst(wolvrix::lib::grh::Graph &graph, const std::string &valueName, const std::string &opName, int64_t width, bool isSigned, const std::string &literal)
    {
        const wolvrix::lib::grh::SymbolId valueSym = graph.internSymbol(valueName);
        const wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
        const wolvrix::lib::grh::ValueId val = graph.createValue(valueSym, static_cast<int32_t>(width), isSigned);
        const wolvrix::lib::grh::OperationId op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
        graph.addResult(op, val);
        graph.setAttr(op, "constValue", literal);
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

    // Runs const-fold (optionally preceded by logic-normalize) once.
    bool runFoldPipeline(wolvrix::lib::grh::Design &design, bool normalizeFirst)
    {
        PassManager manager;
        if (normalizeFirst)
        {
            manager.addPass(std::make_unique<LogicNormalizePass>());
        }
        manager.addPass(std::make_unique<ConstantFoldPass>());

        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        return res.success && !diags.hasError();
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

    int testOneBitOperandsRenameOnly()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g1");

        wolvrix::lib::grh::ValueId a = graph.createValue(graph.internSymbol("a"), 1, false);
        wolvrix::lib::grh::ValueId b = graph.createValue(graph.internSymbol("b"), 1, false);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        wolvrix::lib::grh::ValueId r = graph.createValue(graph.internSymbol("r"), 1, false);
        const wolvrix::lib::grh::OperationId andOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicAnd, graph.internSymbol("and_op"));
        graph.addOperand(andOp, a);
        graph.addOperand(andOp, b);
        graph.addResult(andOp, r);
        graph.bindOutputPort("out", r);

        const std::size_t opsBefore = graph.operations().size();

        PassManager manager;
        manager.addPass(std::make_unique<LogicNormalizePass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected logic-normalize to succeed on 1-bit operands");
        }
        if (!res.changed)
        {
            return fail("Expected logic-normalize to report changes");
        }
        // 1-bit operands need no truthify helper ops.
        if (countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kReduceOr) != 0 ||
            countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kReduceNor) != 0)
        {
            return fail("1-bit operands must not insert kReduceOr/kReduceNor helper ops");
        }
        if (countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kAnd) != 1)
        {
            return fail("Expected exactly one kAnd after rewriting kLogicAnd");
        }
        // The rewrite adds one replacement op; the old op stays dead for DCE.
        if (graph.operations().size() != opsBefore + 1)
        {
            return fail("Expected exactly one replacement op to be added");
        }
        const wolvrix::lib::grh::ValueId outVal = graph.outputPortValue("out");
        if (!outVal.valid() || !isDefinedBy(graph, outVal, wolvrix::lib::grh::OperationKind::kAnd))
        {
            return fail("Output must be redefined by the new kAnd op");
        }
        if (graph.getValue(outVal).width() != 1)
        {
            return fail("kAnd result width must stay 1");
        }
        const wolvrix::lib::grh::Operation andDef = defOpOf(graph, outVal);
        const auto operands = andDef.operands();
        if (operands.size() != 2 || operands[0] != a || operands[1] != b)
        {
            return fail("kAnd must keep the original 1-bit operands in order");
        }
        return 0;
    }

    int testMultiBitOperandsInsertReduceOps()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g2");

        wolvrix::lib::grh::ValueId a = graph.createValue(graph.internSymbol("a"), 8, false);
        wolvrix::lib::grh::ValueId b = graph.createValue(graph.internSymbol("b"), 8, false);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        wolvrix::lib::grh::ValueId rAnd = graph.createValue(graph.internSymbol("r_and"), 1, false);
        const wolvrix::lib::grh::OperationId logicAnd =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicAnd, graph.internSymbol("logic_and_op"));
        graph.addOperand(logicAnd, a);
        graph.addOperand(logicAnd, b);
        graph.addResult(logicAnd, rAnd);
        graph.bindOutputPort("out_and", rAnd);

        wolvrix::lib::grh::ValueId rOr = graph.createValue(graph.internSymbol("r_or"), 1, false);
        const wolvrix::lib::grh::OperationId logicOr =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicOr, graph.internSymbol("logic_or_op"));
        graph.addOperand(logicOr, a);
        graph.addOperand(logicOr, b);
        graph.addResult(logicOr, rOr);
        graph.bindOutputPort("out_or", rOr);

        wolvrix::lib::grh::ValueId rNot = graph.createValue(graph.internSymbol("r_not"), 1, false);
        const wolvrix::lib::grh::OperationId logicNot =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicNot, graph.internSymbol("logic_not_op"));
        graph.addOperand(logicNot, a);
        graph.addResult(logicNot, rNot);
        graph.bindOutputPort("out_not", rNot);

        PassManager manager;
        manager.addPass(std::make_unique<LogicNormalizePass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during run: ") + ex.what());
        }
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected logic-normalize to succeed and report changes");
        }

        // kLogicAnd(a, b) -> kAnd(kReduceOr(a), kReduceOr(b)).
        const wolvrix::lib::grh::ValueId outAnd = graph.outputPortValue("out_and");
        if (!outAnd.valid() || !isDefinedBy(graph, outAnd, wolvrix::lib::grh::OperationKind::kAnd) ||
            graph.getValue(outAnd).width() != 1)
        {
            return fail("out_and must be a 1-bit kAnd result");
        }
        const wolvrix::lib::grh::Operation outAndDef = defOpOf(graph, outAnd);
        const auto andOperands = outAndDef.operands();
        if (andOperands.size() != 2 ||
            !isDefinedBy(graph, andOperands[0], wolvrix::lib::grh::OperationKind::kReduceOr) ||
            !isDefinedBy(graph, andOperands[1], wolvrix::lib::grh::OperationKind::kReduceOr))
        {
            return fail("kAnd operands must come from kReduceOr truthify ops");
        }
        const wolvrix::lib::grh::Operation truthADef = defOpOf(graph, andOperands[0]);
        const wolvrix::lib::grh::Operation truthBDef = defOpOf(graph, andOperands[1]);
        const auto truthA = truthADef.operands();
        const auto truthB = truthBDef.operands();
        if (truthA.size() != 1 || truthA[0] != a || truthB.size() != 1 || truthB[0] != b)
        {
            return fail("kReduceOr ops must wrap the original 8-bit operands");
        }
        if (graph.getValue(andOperands[0]).width() != 1 ||
            graph.getValue(andOperands[1]).width() != 1)
        {
            return fail("kReduceOr results must be 1-bit");
        }

        // kLogicOr(a, b) -> kOr(kReduceOr(a), kReduceOr(b)).
        const wolvrix::lib::grh::ValueId outOr = graph.outputPortValue("out_or");
        if (!outOr.valid() || !isDefinedBy(graph, outOr, wolvrix::lib::grh::OperationKind::kOr) ||
            graph.getValue(outOr).width() != 1)
        {
            return fail("out_or must be a 1-bit kOr result");
        }
        const wolvrix::lib::grh::Operation outOrDef = defOpOf(graph, outOr);
        const auto orOperands = outOrDef.operands();
        if (orOperands.size() != 2 ||
            !isDefinedBy(graph, orOperands[0], wolvrix::lib::grh::OperationKind::kReduceOr) ||
            !isDefinedBy(graph, orOperands[1], wolvrix::lib::grh::OperationKind::kReduceOr))
        {
            return fail("kOr operands must come from kReduceOr truthify ops");
        }

        // kLogicNot(a) with a wider than 1 bit -> kReduceNor(a).
        const wolvrix::lib::grh::ValueId outNot = graph.outputPortValue("out_not");
        if (!outNot.valid() || !isDefinedBy(graph, outNot, wolvrix::lib::grh::OperationKind::kReduceNor) ||
            graph.getValue(outNot).width() != 1)
        {
            return fail("out_not must be a 1-bit kReduceNor result");
        }
        const wolvrix::lib::grh::Operation outNotDef = defOpOf(graph, outNot);
        const auto notOperands = outNotDef.operands();
        if (notOperands.size() != 1 || notOperands[0] != a)
        {
            return fail("kReduceNor must wrap the original operand directly");
        }
        return 0;
    }

    int testLogicNotOneBitBecomesKNot()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g3");

        wolvrix::lib::grh::ValueId a = graph.createValue(graph.internSymbol("a"), 1, false);
        graph.bindInputPort("a", a);
        wolvrix::lib::grh::ValueId r = graph.createValue(graph.internSymbol("r"), 1, false);
        const wolvrix::lib::grh::OperationId logicNot =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicNot, graph.internSymbol("logic_not_op"));
        graph.addOperand(logicNot, a);
        graph.addResult(logicNot, r);
        graph.bindOutputPort("out", r);

        PassManager manager;
        manager.addPass(std::make_unique<LogicNormalizePass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected logic-normalize to succeed and report changes");
        }
        if (countOpsOfKind(graph, wolvrix::lib::grh::OperationKind::kReduceNor) != 0)
        {
            return fail("1-bit kLogicNot must not insert kReduceNor");
        }
        const wolvrix::lib::grh::ValueId outVal = graph.outputPortValue("out");
        if (!outVal.valid() || !isDefinedBy(graph, outVal, wolvrix::lib::grh::OperationKind::kNot))
        {
            return fail("1-bit kLogicNot must become kNot");
        }
        const wolvrix::lib::grh::Operation notDef = defOpOf(graph, outVal);
        const auto operands = notDef.operands();
        if (operands.size() != 1 || operands[0] != a)
        {
            return fail("kNot must keep the original 1-bit operand");
        }
        return 0;
    }

    int testOtherKindsUntouched()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g4");

        wolvrix::lib::grh::ValueId a = graph.createValue(graph.internSymbol("a"), 8, false);
        wolvrix::lib::grh::ValueId b = graph.createValue(graph.internSymbol("b"), 8, false);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        wolvrix::lib::grh::ValueId x = graph.createValue(graph.internSymbol("x"), 8, false);
        const wolvrix::lib::grh::OperationId xorOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kXor, graph.internSymbol("xor_op"));
        graph.addOperand(xorOp, a);
        graph.addOperand(xorOp, b);
        graph.addResult(xorOp, x);
        graph.bindOutputPort("out_x", x);

        // 1-bit result alone must not trigger the rewrite.
        wolvrix::lib::grh::ValueId e = graph.createValue(graph.internSymbol("e"), 1, false);
        const wolvrix::lib::grh::OperationId eqOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kEq, graph.internSymbol("eq_op"));
        graph.addOperand(eqOp, a);
        graph.addOperand(eqOp, b);
        graph.addResult(eqOp, e);
        graph.bindOutputPort("out_e", e);

        wolvrix::lib::grh::ValueId c = graph.createValue(graph.internSymbol("c"), 1, false);
        wolvrix::lib::grh::ValueId d = graph.createValue(graph.internSymbol("d"), 1, false);
        graph.bindInputPort("c", c);
        graph.bindInputPort("d", d);
        wolvrix::lib::grh::ValueId k = graph.createValue(graph.internSymbol("k"), 1, false);
        const wolvrix::lib::grh::OperationId andOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd, graph.internSymbol("and_op"));
        graph.addOperand(andOp, c);
        graph.addOperand(andOp, d);
        graph.addResult(andOp, k);
        graph.bindOutputPort("out_k", k);

        const std::size_t opsBefore = graph.operations().size();

        PassManager manager;
        manager.addPass(std::make_unique<LogicNormalizePass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("Expected logic-normalize to succeed on non-logic kinds");
        }
        if (res.changed)
        {
            return fail("logic-normalize must not touch non-logic kinds");
        }
        if (graph.operations().size() != opsBefore)
        {
            return fail("No ops may be added for non-logic kinds");
        }
        if (!graph.findOperation("xor_op").valid() || !graph.findOperation("eq_op").valid() ||
            !graph.findOperation("and_op").valid())
        {
            return fail("Non-logic ops must be left in place");
        }
        return 0;
    }

    // Builds kLogicOr(1'b1, 1'b0) -> out and returns the folded output constant.
    std::optional<slang::SVInt> foldOneBitLogicOr(bool normalizeFirst)
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("fold1");
        const wolvrix::lib::grh::ValueId one = makeConst(graph, "one", "one_op", 1, false, "1'b1");
        const wolvrix::lib::grh::ValueId zero = makeConst(graph, "zero", "zero_op", 1, false, "1'b0");
        wolvrix::lib::grh::ValueId r = graph.createValue(graph.internSymbol("r"), 1, false);
        const wolvrix::lib::grh::OperationId logicOr =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicOr, graph.internSymbol("logic_or_op"));
        graph.addOperand(logicOr, one);
        graph.addOperand(logicOr, zero);
        graph.addResult(logicOr, r);
        graph.bindOutputPort("out", r);
        if (!runFoldPipeline(design, normalizeFirst))
        {
            return std::nullopt;
        }
        return foldedOutputLiteral(graph, "out");
    }

    // Builds { kLogicAnd(8'd0, 8'd5) -> out_and, kLogicNot(8'd0) -> out_not }
    // and returns the folded output constants.
    std::optional<std::pair<slang::SVInt, slang::SVInt>> foldMultiBitLogic(bool normalizeFirst)
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("fold2");
        const wolvrix::lib::grh::ValueId zero = makeConst(graph, "zero", "zero_op", 8, false, "8'd0");
        const wolvrix::lib::grh::ValueId five = makeConst(graph, "five", "five_op", 8, false, "8'd5");

        wolvrix::lib::grh::ValueId rAnd = graph.createValue(graph.internSymbol("r_and"), 1, false);
        const wolvrix::lib::grh::OperationId logicAnd =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicAnd, graph.internSymbol("logic_and_op"));
        graph.addOperand(logicAnd, zero);
        graph.addOperand(logicAnd, five);
        graph.addResult(logicAnd, rAnd);
        graph.bindOutputPort("out_and", rAnd);

        wolvrix::lib::grh::ValueId rNot = graph.createValue(graph.internSymbol("r_not"), 1, false);
        const wolvrix::lib::grh::OperationId logicNot =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicNot, graph.internSymbol("logic_not_op"));
        graph.addOperand(logicNot, zero);
        graph.addResult(logicNot, rNot);
        graph.bindOutputPort("out_not", rNot);

        if (!runFoldPipeline(design, normalizeFirst))
        {
            return std::nullopt;
        }
        auto andLit = foldedOutputLiteral(graph, "out_and");
        auto notLit = foldedOutputLiteral(graph, "out_not");
        if (!andLit || !notLit)
        {
            return std::nullopt;
        }
        return std::make_pair(*andLit, *notLit);
    }

    int testConstFoldConsistency()
    {
        // 1-bit case: logic-normalize + const-fold must match const-fold alone.
        {
            const auto normalized = foldOneBitLogicOr(true);
            const auto reference = foldOneBitLogicOr(false);
            const slang::SVInt expected = slang::SVInt::fromString("1'b1").resize(1);
            if (!normalized || !reference ||
                !static_cast<bool>((*normalized == expected)) ||
                !static_cast<bool>((*normalized == *reference)))
            {
                return fail("1-bit kLogicOr fold mismatch between normalize+fold and fold-only");
            }
        }

        // Multi-bit case: kLogicAnd(8'd0, 8'd5) == 0 and kLogicNot(8'd0) == 1.
        {
            const auto normalized = foldMultiBitLogic(true);
            const auto reference = foldMultiBitLogic(false);
            if (!normalized || !reference)
            {
                return fail("multi-bit fold failed");
            }
            const slang::SVInt expectedAnd = slang::SVInt::fromString("1'b0").resize(1);
            const slang::SVInt expectedNot = slang::SVInt::fromString("1'b1").resize(1);
            if (!static_cast<bool>((normalized->first == expectedAnd)) ||
                !static_cast<bool>((normalized->second == expectedNot)) ||
                !static_cast<bool>((normalized->first == reference->first)) ||
                !static_cast<bool>((normalized->second == reference->second)))
            {
                return fail("multi-bit fold mismatch between normalize+fold and fold-only");
            }
        }
        return 0;
    }

    int testPassRegistration()
    {
        bool listed = false;
        for (const std::string &name : availableTransformPasses())
        {
            if (name == "logic-normalize")
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            return fail("logic-normalize must be listed by availableTransformPasses");
        }
        std::string error;
        const std::vector<std::string_view> noArgs;
        std::unique_ptr<Pass> pass = makePass("logic-normalize", noArgs, error);
        if (!pass)
        {
            return fail("makePass must create logic-normalize: " + error);
        }
        const std::vector<std::string_view> badArgs = {"unexpected"};
        if (makePass("logic-normalize", badArgs, error))
        {
            return fail("logic-normalize must reject arguments");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int rc = testOneBitOperandsRenameOnly())
    {
        return rc;
    }
    if (const int rc = testMultiBitOperandsInsertReduceOps())
    {
        return rc;
    }
    if (const int rc = testLogicNotOneBitBecomesKNot())
    {
        return rc;
    }
    if (const int rc = testOtherKindsUntouched())
    {
        return rc;
    }
    if (const int rc = testConstFoldConsistency())
    {
        return rc;
    }
    if (const int rc = testPassRegistration())
    {
        return rc;
    }
    return 0;
}
