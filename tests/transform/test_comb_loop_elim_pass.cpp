#include "core/grh.hpp"
#include "transform/comb_loop_elim.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[comb-loop-elim-tests] " << message << '\n';
        return 1;
    }

    wolvrix::lib::grh::ValueId makeValue(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        return graph.createValue(sym, width, false);
    }

    wolvrix::lib::grh::OperationId makeSliceStatic(wolvrix::lib::grh::Graph &graph,
                                                   wolvrix::lib::grh::ValueId base,
                                                   wolvrix::lib::grh::ValueId result,
                                                   int64_t low,
                                                   int64_t high,
                                                   const std::string &name)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, sym);
        graph.addOperand(op, base);
        graph.addResult(op, result);
        graph.setAttr(op, "sliceStart", low);
        graph.setAttr(op, "sliceEnd", high);
        return op;
    }

    wolvrix::lib::grh::OperationId makeConcat(wolvrix::lib::grh::Graph &graph,
                                              const std::string &name,
                                              const std::vector<wolvrix::lib::grh::ValueId> &operands,
                                              wolvrix::lib::grh::ValueId result)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat, sym);
        for (const auto operand : operands)
        {
            graph.addOperand(op, operand);
        }
        graph.addResult(op, result);
        return op;
    }

    wolvrix::lib::grh::ValueId makeConst(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width,
                                         const std::string &literal)
    {
        const auto value = makeValue(graph, name, width);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.internSymbol(name + "_op"));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", literal);
        return value;
    }

    wolvrix::lib::grh::OperationId makeSliceDynamic(wolvrix::lib::grh::Graph &graph,
                                                    wolvrix::lib::grh::ValueId base,
                                                    wolvrix::lib::grh::ValueId index,
                                                    wolvrix::lib::grh::ValueId result,
                                                    int64_t width,
                                                    const std::string &name)
    {
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                              graph.internSymbol(name));
        graph.addOperand(op, base);
        graph.addOperand(op, index);
        graph.addResult(op, result);
        graph.setAttr(op, "sliceWidth", width);
        return op;
    }

    int test_false_loop_fixed()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_false");

        auto a_high = makeValue(graph, "a_high", 4);
        auto b_high = makeValue(graph, "b_high", 4);

        auto a_low_from_b = makeValue(graph, "a_low_from_b", 4);
        auto a = makeValue(graph, "a", 8);
        auto b_low_from_a = makeValue(graph, "b_low_from_a", 4);
        auto b = makeValue(graph, "b", 8);

        makeSliceStatic(graph, b, a_low_from_b, 0, 3, "slice_b_low");
        makeConcat(graph, "concat_a", {a_high, a_low_from_b}, a);

        makeSliceStatic(graph, a, b_low_from_a, 4, 7, "slice_a_high");
        makeConcat(graph, "concat_b", {b_high, b_low_from_a}, b);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

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
            return fail("Expected comb-loop-elim to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected comb-loop-elim to report changes for false loop");
        }
        bool foundResolvedInfo = false;
        bool foundSummaryInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected fixed false loop to avoid warning diagnostics");
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("comb-loop-elim resolved false loops") != std::string::npos)
            {
                foundResolvedInfo = true;
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("comb-loop-elim summary") != std::string::npos)
            {
                foundSummaryInfo = true;
            }
        }
        if (!foundResolvedInfo)
        {
            return fail("Expected fixed false loop to emit an info diagnostic");
        }
        if (!foundSummaryInfo)
        {
            return fail("Expected fixed false loop to emit an info summary diagnostic");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed during verify run");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after split");
            }
        }

        return 0;
    }

    int test_true_loop_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_true");

        auto a = makeValue(graph, "a", 1);
        auto x = makeValue(graph, "x", 1);
        auto y = makeValue(graph, "y", 1);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_xy"));
        graph.addOperand(xorOp, a);
        graph.addOperand(xorOp, y);
        graph.addResult(xorOp, x);

        const auto notOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                 graph.internSymbol("not_x"));
        graph.addOperand(notOp, x);
        graph.addResult(notOp, y);

        CombLoopElimOptions options;
        options.fixFalseLoops = false;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));

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
            return fail("Expected comb-loop-elim to succeed with warnings");
        }
        if (res.changed)
        {
            return fail("Expected comb-loop-elim to avoid changes for true loop");
        }

        bool foundWarning = false;
        bool foundSummaryInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                foundWarning = true;
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("comb-loop-elim summary") != std::string::npos)
            {
                foundSummaryInfo = true;
            }
        }
        if (!foundWarning)
        {
            return fail("Expected comb-loop-elim warning for true loop");
        }
        if (!foundSummaryInfo)
        {
            return fail("Expected comb-loop-elim info summary for true loop");
        }
        return 0;
    }

    int test_false_loop_dynamic_slice_retargeted()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_dyn_false");

        const auto ext = makeValue(graph, "ext", 1);
        graph.bindInputPort("ext", ext);

        const auto low = makeValue(graph, "low", 1);
        const auto hi = makeValue(graph, "hi", 1);
        const auto busView = makeValue(graph, "bus_view", 2);
        const auto bus = makeValue(graph, "bus", 2);
        const auto idx0 = makeConst(graph, "idx0", 32, "32'd0");

        const auto notOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                 graph.internSymbol("not_low"));
        graph.addOperand(notOp, low);
        graph.addResult(notOp, hi);

        makeConcat(graph, "concat_bus_view", {hi, ext}, busView);
        const auto assignBus = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_bus"));
        graph.addOperand(assignBus, busView);
        graph.addResult(assignBus, bus);

        const auto sliceOp = makeSliceDynamic(graph, bus, idx0, low, 1, "slice_low");

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected dynamic false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected dynamic false-loop case to report changes");
        }

        const auto rewritten = graph.getOperation(sliceOp);
        if (rewritten.kind() != wolvrix::lib::grh::OperationKind::kAssign)
        {
            return fail("Expected dynamic slice to retarget into a direct assign");
        }
        if (rewritten.operands().size() != 1 || rewritten.operands()[0] != ext)
        {
            return fail("Expected retargeted assign to use the selected concat operand");
        }
        if (rewritten.attr("sliceWidth") || rewritten.attr("sliceStart") || rewritten.attr("sliceEnd"))
        {
            return fail("Expected retargeted assign to drop slice attributes");
        }

        bool foundRetargetInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim" || msg.kind != PassDiagnosticKind::Info)
            {
                continue;
            }
            if (msg.message.find("retargeted-slices=") != std::string::npos)
            {
                foundRetargetInfo = true;
            }
            if (msg.message.find("resolved false loops") != std::string::npos &&
                msg.message.find("split-values=0") == std::string::npos)
            {
                return fail("Expected dynamic retarget fast path to avoid generic value splitting");
            }
        }
        if (!foundRetargetInfo)
        {
            return fail("Expected dynamic false-loop case to report retargeted slices");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected dynamic verify run to succeed");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after dynamic slice retarget");
            }
        }

        return 0;
    }

    int test_false_loop_bitwise_partition_propagation()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_bitwise_false");

        const auto mask = makeValue(graph, "mask", 6);
        const auto ext = makeValue(graph, "ext", 4);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 6);
        const auto x = makeValue(graph, "x", 6);
        const auto mid = makeValue(graph, "mid", 2);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_y_mask"));
        graph.addOperand(xorOp, y);
        graph.addOperand(xorOp, mask);
        graph.addResult(xorOp, x);

        makeSliceStatic(graph, x, mid, 1, 2, "slice_x_mid");
        makeConcat(graph, "concat_y", {mid, ext}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during bitwise false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected bitwise false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected bitwise false-loop case to report changes");
        }

        bool foundSplitInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected bitwise false-loop case to avoid warning diagnostics");
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("resolved false loops") != std::string::npos &&
                msg.message.find("split-values=0") == std::string::npos)
            {
                foundSplitInfo = true;
            }
        }
        if (!foundSplitInfo)
        {
            return fail("Expected bitwise false-loop case to use value splitting");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during bitwise verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected bitwise verify run to succeed");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after bitwise split");
            }
        }

        return 0;
    }

    int test_false_loop_compare_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_compare_false");

        const auto mask = makeValue(graph, "mask", 4);
        const auto ext = makeValue(graph, "ext", 3);
        const auto rhs = makeConst(graph, "rhs", 3, "3'h3");
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 4);
        const auto x = makeValue(graph, "x", 4);
        const auto low = makeValue(graph, "low", 3);
        const auto bit = makeValue(graph, "bit", 1);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_y_mask"));
        graph.addOperand(xorOp, y);
        graph.addOperand(xorOp, mask);
        graph.addResult(xorOp, x);

        makeSliceStatic(graph, x, low, 0, 2, "slice_x_low");

        const auto eqOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kEq,
                                                graph.internSymbol("eq_x_rhs"));
        graph.addOperand(eqOp, low);
        graph.addOperand(eqOp, rhs);
        graph.addResult(eqOp, bit);

        makeConcat(graph, "concat_y", {bit, ext}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during compare false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected compare false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected compare false-loop case to report changes");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected compare false-loop case to avoid warning diagnostics");
            }
        }
        return 0;
    }

    int test_false_loop_replicate_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_replicate_false");

        const auto ext = makeValue(graph, "ext", 2);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 4);
        const auto low = makeValue(graph, "low", 1);
        const auto repeated = makeValue(graph, "repeated", 2);

        makeSliceStatic(graph, y, low, 0, 0, "slice_y_low");

        const auto repOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kReplicate,
                                                 graph.internSymbol("rep_low"));
        graph.addOperand(repOp, low);
        graph.addResult(repOp, repeated);
        graph.setAttr(repOp, "rep", int64_t{2});

        makeConcat(graph, "concat_y", {repeated, ext}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during replicate false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected replicate false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected replicate false-loop case to report changes");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected replicate false-loop case to avoid warning diagnostics");
            }
        }
        return 0;
    }

    int test_false_loop_reduce_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_reduce_false");

        const auto ext = makeValue(graph, "ext", 2);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 4);
        const auto low = makeValue(graph, "low", 1);
        const auto high = makeValue(graph, "high", 1);
        const auto pair = makeValue(graph, "pair", 2);
        const auto bit = makeValue(graph, "bit", 1);

        makeSliceStatic(graph, y, low, 0, 0, "slice_y_low");
        makeSliceStatic(graph, y, high, 1, 1, "slice_y_high");
        makeConcat(graph, "concat_pair", {high, low}, pair);

        const auto reduceOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kReduceAnd,
                                                    graph.internSymbol("reduce_pair"));
        graph.addOperand(reduceOp, pair);
        graph.addResult(reduceOp, bit);

        makeConcat(graph, "concat_y", {bit, ext, high}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during reduce false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected reduce false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected reduce false-loop case to report changes");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected reduce false-loop case to avoid warning diagnostics");
            }
        }
        return 0;
    }

} // namespace

int main()
{
    if (int rc = test_false_loop_fixed(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_true_loop_reported(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_dynamic_slice_retargeted(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_bitwise_partition_propagation(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_compare_split(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_replicate_split(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_reduce_split(); rc != 0)
    {
        return rc;
    }
    return 0;
}
