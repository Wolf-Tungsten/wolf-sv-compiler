#include "core/grh.hpp"
#include "transform/const_fold.hpp"
#include "core/transform.hpp"

#include "slang/numeric/SVInt.h"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[const-fold-tests] " << message << '\n';
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

} // namespace

int main()
{
    // Case 1: multi-iteration folding rewires downstream users
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g");
        wolvrix::lib::grh::ValueId c0 = makeConst(graph, "c0", "c0_op", 4, false, "4'h3");
        wolvrix::lib::grh::ValueId c1 = makeConst(graph, "c1", "c1_op", 4, false, "4'h1");

        wolvrix::lib::grh::ValueId sum = graph.createValue(graph.internSymbol("sum"), 4, false);
        wolvrix::lib::grh::OperationId add = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd, graph.internSymbol("add0"));
        graph.addOperand(add, c0);
        graph.addOperand(add, c1);
        graph.addResult(add, sum);

        wolvrix::lib::grh::ValueId pass = graph.createValue(graph.internSymbol("pass"), 4, false);
        wolvrix::lib::grh::OperationId assign = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign, graph.internSymbol("assign0"));
        graph.addOperand(assign, sum);
        graph.addResult(assign, pass);

        wolvrix::lib::grh::ValueId neg = graph.createValue(graph.internSymbol("neg"), 4, false);
        wolvrix::lib::grh::OperationId invert = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot, graph.internSymbol("not0"));
        graph.addOperand(invert, pass);
        graph.addResult(invert, neg);

        wolvrix::lib::grh::ValueId finalSum = graph.createValue(graph.internSymbol("finalSum"), 4, false);
        wolvrix::lib::grh::OperationId add2 = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd, graph.internSymbol("add1"));
        graph.addOperand(add2, neg);
        graph.addOperand(add2, c1);
        graph.addResult(add2, finalSum);

        wolvrix::lib::grh::ValueId out = graph.createValue(graph.internSymbol("out"), 4, false);
        graph.bindOutputPort("out", out);
        wolvrix::lib::grh::OperationId assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign, graph.internSymbol("assign1"));
        graph.addOperand(assignOut, finalSum);
        graph.addResult(assignOut, out);

        PassManager manager;
        manager.addPass(std::make_unique<ConstantFoldPass>());

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
            return fail("Expected constant propagation to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected constant propagation to mark changes");
        }

        // Downstream user should be rewired to a constant.
        if (graph.findOperation("assign1").valid())
        {
            return fail("assign1 should be removed after folding");
        }
        wolvrix::lib::grh::ValueId outVal = graph.outputPortValue("out");
        if (!outVal.valid())
        {
            return fail("Output port was not rewired to a constant");
        }
        wolvrix::lib::grh::Value outValue = graph.getValue(outVal);
        if (!outValue.definingOp().valid() ||
            graph.getOperation(outValue.definingOp()).kind() != wolvrix::lib::grh::OperationKind::kConstant)
        {
            return fail("Output port was not rewired to a constant");
        }
        auto literal = getConstLiteral(graph, graph.getOperation(outValue.definingOp()));
        slang::SVInt expected = slang::SVInt::fromString("4'hc").resize(4);
        if (!literal || !static_cast<bool>((*literal == expected)))
        {
            return fail("Final constant value mismatch");
        }
        if (!graph.getValue(neg).users().empty())
        {
            return fail("Expected users of intermediate value to be rewired");
        }
    }

    // Case 2: X operand blocks folding when xFold=known (default)
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g2");
        wolvrix::lib::grh::ValueId xval = makeConst(graph, "cx", "cx_op", 1, false, "1'bx");
        wolvrix::lib::grh::ValueId one = makeConst(graph, "c1", "c1_op", 1, false, "1'b1");

        wolvrix::lib::grh::ValueId andOut = graph.createValue(graph.internSymbol("andOut"), 1, false);
        wolvrix::lib::grh::OperationId op = graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd, graph.internSymbol("and0"));
        graph.addOperand(op, xval);
        graph.addOperand(op, one);
        graph.addResult(op, andOut);

        PassManager manager;
        manager.addPass(std::make_unique<ConstantFoldPass>());
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
            return fail("Unexpected failure when X folding is blocked");
        }
        if (res.changed)
        {
            return fail("Pass should not change graph when blocked by X");
        }
        const wolvrix::lib::grh::Operation opView = graph.getOperation(op);
        if (opView.operands()[0] != xval || opView.operands()[1] != one)
        {
            return fail("Operands should remain unchanged when folding is skipped");
        }
    }

    // Case 3: missing attribute triggers failure
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g3");
        wolvrix::lib::grh::ValueId c = makeConst(graph, "c", "c_op", 2, false, "2'h1");

        wolvrix::lib::grh::ValueId repOut = graph.createValue(graph.internSymbol("repOut"), 4, false);
        wolvrix::lib::grh::OperationId rep = graph.createOperation(wolvrix::lib::grh::OperationKind::kReplicate, graph.internSymbol("rep0"));
        graph.addOperand(rep, c);
        graph.addResult(rep, repOut);
        // Intentionally omit the "rep" attribute.

        PassManager manager;
        manager.addPass(std::make_unique<ConstantFoldPass>());
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
        if (res.success || !diags.hasError())
        {
            return fail("Missing attribute should fail the pass and emit an error");
        }
    }

    // Case 4: kSystemFunction $clog2 folds to constant
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g4");
        wolvrix::lib::grh::ValueId arg = makeConst(graph, "arg", "arg_op", 8, false, "8'h8");

        wolvrix::lib::grh::ValueId result = graph.createValue(graph.internSymbol("clog2_out"), 32, false);
        wolvrix::lib::grh::OperationId sys = graph.createOperation(wolvrix::lib::grh::OperationKind::kSystemFunction,
                                                         graph.internSymbol("sys_clog2"));
        graph.addOperand(sys, arg);
        graph.addResult(sys, result);
        graph.setAttr(sys, "name", std::string("clog2"));
        graph.setAttr(sys, "hasSideEffects", false);

        wolvrix::lib::grh::ValueId out = graph.createValue(graph.internSymbol("out"), 32, false);
        graph.bindOutputPort("out", out);
        wolvrix::lib::grh::OperationId assign = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                            graph.internSymbol("assign_out"));
        graph.addOperand(assign, result);
        graph.addResult(assign, out);

        PassManager manager;
        manager.addPass(std::make_unique<ConstantFoldPass>());

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
            return fail("Expected $clog2 constant folding to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected $clog2 folding to mark changes");
        }
        if (graph.findOperation("sys_clog2").valid())
        {
            return fail("kSystemFunction $clog2 op should be removed after folding");
        }

        wolvrix::lib::grh::ValueId outVal = graph.outputPortValue("out");
        if (!outVal.valid())
        {
            return fail("Output port missing after $clog2 folding");
        }
        wolvrix::lib::grh::Value outValue = graph.getValue(outVal);
        if (!outValue.definingOp().valid() ||
            graph.getOperation(outValue.definingOp()).kind() != wolvrix::lib::grh::OperationKind::kConstant)
        {
            return fail("Output port was not rewired to a constant after $clog2 folding");
        }
        auto literal = getConstLiteral(graph, graph.getOperation(outValue.definingOp()));
        if (!literal)
        {
            return fail("Missing folded constant literal for $clog2");
        }
        auto value = literal->as<uint64_t>();
        if (!value || *value != 3u)
        {
            return fail("Unexpected $clog2 constant value");
        }
    }

    // Case 5: context-sized shifts resize the lhs before constant evaluation.
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g5");
        const wolvrix::lib::grh::ValueId one =
            makeConst(graph, "one", "one_op", 1, false, "1'b1");
        const wolvrix::lib::grh::ValueId negative =
            makeConst(graph, "negative", "negative_op", 8, true, "8'h80");
        const wolvrix::lib::grh::ValueId amount =
            makeConst(graph, "amount", "amount_op", 32, false, "32'd65");

        const auto addShift = [&](wolvrix::lib::grh::OperationKind kind,
                                  std::string_view name,
                                  wolvrix::lib::grh::ValueId lhs) {
            const auto result = graph.createValue(
                graph.internSymbol(name), 130, false);
            const auto op = graph.createOperation(
                kind, graph.internSymbol(std::string(name) + "_op"));
            graph.addOperand(op, lhs);
            graph.addOperand(op, amount);
            graph.addResult(op, result);
            graph.bindOutputPort(name, result);
        };
        addShift(wolvrix::lib::grh::OperationKind::kShl, "shl", one);
        addShift(wolvrix::lib::grh::OperationKind::kLShr, "lshr", negative);
        addShift(wolvrix::lib::grh::OperationKind::kAShr, "ashr", negative);

        PassManager manager;
        manager.addPass(std::make_unique<ConstantFoldPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || !res.changed || diags.hasError())
        {
            return fail("Expected context-sized shifts to fold");
        }

        const auto checkOutput = [&](std::string_view name,
                                     slang::SVInt expected) {
            const auto output = graph.outputPortValue(name);
            if (!output.valid())
            {
                return false;
            }
            const auto definition = graph.getValue(output).definingOp();
            if (!definition.valid())
            {
                return false;
            }
            const auto operation = graph.getOperation(definition);
            const auto literal = getConstLiteral(graph, operation);
            if (operation.kind() != wolvrix::lib::grh::OperationKind::kConstant ||
                !literal)
            {
                return false;
            }
            slang::SVInt actual = literal->resize(130);
            actual.setSigned(false);
            expected.setSigned(false);
            return static_cast<bool>(actual == expected);
        };

        const slang::SVInt allOnes = ~slang::SVInt(130, 0, false);
        if (!checkOutput("shl", slang::SVInt(130, 1, false).shl(65)) ||
            !checkOutput("lshr", allOnes.lshr(65)) ||
            !checkOutput("ashr", allOnes))
        {
            return fail("Context-sized shift constant value mismatch");
        }
    }

    return 0;
}
