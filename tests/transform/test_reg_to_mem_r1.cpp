#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/reg_to_mem.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

// R1 write-side matching coverage for reg-to-mem:
// - R1a: one-hot row-select guard terms (shift-onehot static/dynamic slice and
//   one-hot concat array select) and two-dimensional guards where exactly one
//   equality selects the storage row.
// - R1b: single-branch writes whose nextValue is not a mux chain.
namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[reg-to-mem-r1-tests] " << message << '\n';
        return 1;
    }

    ValueId makeLogicValue(Graph &graph, std::string_view name, int32_t width)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), width, false, ValueType::Logic);
    }

    ValueId addConstant(Graph &graph,
                        std::string_view opName,
                        std::string_view valueName,
                        int32_t width,
                        std::string literal)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    OperationId addRegister(Graph &graph, std::string_view name, int32_t width)
    {
        const OperationId op = graph.createOperation(OperationKind::kRegister,
                                                     graph.internSymbol(std::string(name)));
        graph.setAttr(op, "width", static_cast<int64_t>(width));
        graph.setAttr(op, "isSigned", false);
        return op;
    }

    OperationId addRegisterRead(Graph &graph,
                                std::string_view opName,
                                std::string_view valueName,
                                std::string regSymbol,
                                int32_t width,
                                ValueId &result)
    {
        result = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kRegisterReadPort,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, result);
        graph.setAttr(op, "regSymbol", std::move(regSymbol));
        return op;
    }

    ValueId addBinary(Graph &graph,
                      OperationKind kind,
                      std::string_view opName,
                      std::string_view valueName,
                      ValueId lhs,
                      ValueId rhs,
                      int32_t width = 1)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(kind, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, value);
        return value;
    }

    ValueId addMux(Graph &graph,
                   std::string_view opName,
                   std::string_view valueName,
                   ValueId cond,
                   ValueId trueValue,
                   ValueId falseValue,
                   int32_t width)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kMux, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, cond);
        graph.addOperand(op, trueValue);
        graph.addOperand(op, falseValue);
        graph.addResult(op, value);
        return value;
    }

    OperationId addRegisterWrite(Graph &graph,
                                 std::string_view opName,
                                 std::string regSymbol,
                                 ValueId guard,
                                 ValueId data,
                                 ValueId mask,
                                 ValueId clk,
                                 std::string edge = "posedge")
    {
        const OperationId op = graph.createOperation(OperationKind::kRegisterWritePort,
                                                     graph.internSymbol(std::string(opName)));
        graph.addOperand(op, guard);
        graph.addOperand(op, data);
        graph.addOperand(op, mask);
        graph.addOperand(op, clk);
        graph.setAttr(op, "regSymbol", std::move(regSymbol));
        graph.setAttr(op, "eventEdge", std::vector<std::string>{std::move(edge)});
        return op;
    }

    OperationId addSliceArrayAnchor(Graph &graph,
                                    std::string_view prefix,
                                    const std::vector<std::string> &regSymbols,
                                    ValueId index,
                                    int32_t width,
                                    ValueId &selected)
    {
        std::vector<ValueId> readValues(regSymbols.size());
        for (std::size_t row = 0; row < regSymbols.size(); ++row)
        {
            addRegisterRead(graph,
                            std::string(prefix) + "_r" + std::to_string(row) + "_read_op",
                            std::string(prefix) + "_r" + std::to_string(row) + "_read",
                            regSymbols[row],
                            width,
                            readValues[row]);
        }

        ValueId packed = makeLogicValue(graph,
                                        std::string(prefix) + "_packed",
                                        static_cast<int32_t>(width * regSymbols.size()));
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol(std::string(prefix) + "_concat"));
        for (auto it = readValues.rbegin(); it != readValues.rend(); ++it)
        {
            graph.addOperand(concat, *it);
        }
        graph.addResult(concat, packed);

        selected = makeLogicValue(graph, std::string(prefix) + "_selected", width);
        const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                        graph.internSymbol(std::string(prefix) + "_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(width));
        return slice;
    }

    std::size_t countKind(const Graph &graph, OperationKind kind)
    {
        std::size_t count = 0;
        for (OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    constexpr int32_t kWidth = 8;
    constexpr std::size_t kRows = 4;

    // 4x8 register array with one slice-array read anchor; tests add the
    // write-side shape they exercise on top.
    struct BaseDesign
    {
        Design design;
        Graph *graph = nullptr;
        std::vector<std::string> regs;
        ValueId index;
        ValueId addr;
        ValueId wen;
        ValueId data;
        ValueId data2;
        ValueId mask;
        ValueId clk;
        ValueId selected;
    };

    BaseDesign buildBase()
    {
        BaseDesign base;
        base.graph = &base.design.createGraph("top");
        base.design.markAsTop(base.graph->symbol());
        Graph &graph = *base.graph;
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            base.regs.push_back(reg);
            addRegister(graph, reg, kWidth);
        }
        base.index = makeLogicValue(graph, "index", 2);
        base.addr = makeLogicValue(graph, "addr", 2);
        base.wen = makeLogicValue(graph, "wen", 1);
        base.data = makeLogicValue(graph, "data", kWidth);
        base.data2 = makeLogicValue(graph, "data2", kWidth);
        base.mask = addConstant(graph, "mask_op", "mask", kWidth, "8'hff");
        base.clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("index", base.index);
        graph.bindInputPort("addr", base.addr);
        graph.bindInputPort("wen", base.wen);
        graph.bindInputPort("data", base.data);
        graph.bindInputPort("data2", base.data2);
        graph.bindInputPort("clk", base.clk);
        addSliceArrayAnchor(graph, "a0", base.regs, base.index, kWidth, base.selected);
        graph.bindOutputPort("selected", base.selected);
        return base;
    }

    struct PassRunResult
    {
        bool ok = false;
        std::string report;
    };

    PassRunResult runPassWithReport(Design &design, std::string_view key)
    {
        PassManager manager;
        SessionStore session;
        manager.options().session = &session;
        RegToMemOptions options;
        options.minElementCount = 2;
        options.enableIntent = false;
        options.outputKey = std::string(key);
        manager.addPass(std::make_unique<RegToMemPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        PassRunResult out;
        out.ok = result.success && !diags.hasError();
        const auto it = session.find(std::string(key));
        if (it != session.end() && it->second)
        {
            if (const auto *typed =
                    dynamic_cast<const SessionSlotValue<std::string> *>(it->second.get()))
            {
                out.report = typed->value;
            }
        }
        return out;
    }

    bool reportHasOutcome(const std::string &report, std::string_view outcome)
    {
        return report.find("\"outcome\":\"" + std::string(outcome) + "\"") != std::string::npos;
    }

    int expectMerged(const Graph &graph,
                     const PassRunResult &run,
                     std::size_t expectedWritePorts,
                     const char *caseName)
    {
        if (!run.ok)
        {
            return fail(std::string(caseName) + ": reg-to-mem pass failed");
        }
        if (!reportHasOutcome(run.report, "true_merged"))
        {
            std::cerr << "[reg-to-mem-r1-tests] " << caseName << " report: " << run.report << '\n';
            return fail(std::string(caseName) + ": expected true_merged outcome");
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != expectedWritePorts ||
            countKind(graph, OperationKind::kMemoryFillPort) != 0 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail(std::string(caseName) + ": unexpected post-merge op counts");
        }
        return 0;
    }

    int expectSkipped(const Graph &graph,
                      const PassRunResult &run,
                      const char *caseName)
    {
        if (!run.ok)
        {
            return fail(std::string(caseName) + ": reg-to-mem pass failed");
        }
        if (!reportHasOutcome(run.report, "skipped"))
        {
            std::cerr << "[reg-to-mem-r1-tests] " << caseName << " report: " << run.report << '\n';
            return fail(std::string(caseName) + ": expected skipped outcome");
        }
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != kRows ||
            countKind(graph, OperationKind::kRegisterWritePort) != kRows)
        {
            return fail(std::string(caseName) + ": rejected group should remain intact");
        }
        return 0;
    }

    // R1a form 1: guard row select is `(4'b0001 << addr)[row]` as a static
    // 1-bit slice. The mux nextValue keeps the mux-chain matcher satisfied so
    // the guard term is what decides the outcome.
    int testOneHotStaticSliceGuard()
    {
        BaseDesign base = buildBase();
        Graph &graph = *base.graph;
        const ValueId one = addConstant(graph, "one_op", "one", 4, "4'b0001");
        const ValueId shifted = addBinary(graph,
                                          OperationKind::kShl,
                                          "shifted_op",
                                          "shifted",
                                          one,
                                          base.addr,
                                          4);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId bit = makeLogicValue(graph, "bit" + suffix, 1);
            const OperationId slice = graph.createOperation(OperationKind::kSliceStatic,
                                                            graph.internSymbol("slice" + suffix));
            graph.addOperand(slice, shifted);
            graph.addResult(slice, bit);
            graph.setAttr(slice, "sliceStart", static_cast<int64_t>(row));
            graph.setAttr(slice, "sliceEnd", static_cast<int64_t>(row));
            const ValueId guard = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "guard" + suffix + "_op",
                                            "guard" + suffix,
                                            base.wen,
                                            bit,
                                            1);
            const ValueId next = addMux(graph,
                                        "next" + suffix + "_op",
                                        "next" + suffix,
                                        guard,
                                        base.data,
                                        base.data2,
                                        kWidth);
            addRegisterWrite(graph, "write" + suffix, base.regs[row], guard, next, base.mask, base.clk);
        }
        const PassRunResult run = runPassWithReport(base.design, "rtm.r1");
        return expectMerged(graph, run, 1, "onehot_static_slice");
    }

    // R1a form 2: guard row select is `(4'b0001 << addr)[row +: 1]` as a
    // dynamic 1-bit slice with a constant start.
    int testOneHotDynamicSliceGuard()
    {
        BaseDesign base = buildBase();
        Graph &graph = *base.graph;
        const ValueId one = addConstant(graph, "one_op", "one", 4, "4'b0001");
        const ValueId shifted = addBinary(graph,
                                          OperationKind::kShl,
                                          "shifted_op",
                                          "shifted",
                                          one,
                                          base.addr,
                                          4);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "row" + suffix + "_op",
                                                 "row" + suffix,
                                                 4,
                                                 "4'd" + suffix);
            const ValueId bit = makeLogicValue(graph, "bit" + suffix, 1);
            const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                            graph.internSymbol("slice" + suffix));
            graph.addOperand(slice, shifted);
            graph.addOperand(slice, rowConst);
            graph.addResult(slice, bit);
            graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(1));
            const ValueId guard = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "guard" + suffix + "_op",
                                            "guard" + suffix,
                                            base.wen,
                                            bit,
                                            1);
            const ValueId next = addMux(graph,
                                        "next" + suffix + "_op",
                                        "next" + suffix,
                                        guard,
                                        base.data,
                                        base.data2,
                                        kWidth);
            addRegisterWrite(graph, "write" + suffix, base.regs[row], guard, next, base.mask, base.clk);
        }
        const PassRunResult run = runPassWithReport(base.design, "rtm.r1");
        return expectMerged(graph, run, 1, "onehot_dynamic_slice");
    }

    // R1a form 3: guard row select is `onehotConcat[row]` where bit j of the
    // concat is eq(addr, j).
    int testOneHotConcatArrayGuard()
    {
        BaseDesign base = buildBase();
        Graph &graph = *base.graph;
        std::vector<ValueId> bits(kRows);
        for (std::size_t bitIndex = 0; bitIndex < kRows; ++bitIndex)
        {
            const std::string suffix = std::to_string(bitIndex);
            const ValueId bitConst = addConstant(graph,
                                                 "oh_c" + suffix + "_op",
                                                 "oh_c" + suffix,
                                                 2,
                                                 "2'd" + suffix);
            bits[bitIndex] = addBinary(graph,
                                       OperationKind::kEq,
                                       "oh_eq" + suffix + "_op",
                                       "oh_eq" + suffix,
                                       base.addr,
                                       bitConst,
                                       1);
        }
        const ValueId packed = makeLogicValue(graph, "oh_packed", static_cast<int32_t>(kRows));
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("oh_concat"));
        for (auto it = bits.rbegin(); it != bits.rend(); ++it)
        {
            graph.addOperand(concat, *it);
        }
        graph.addResult(concat, packed);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "row" + suffix + "_op",
                                                 "row" + suffix,
                                                 2,
                                                 "2'd" + suffix);
            const ValueId bit = makeLogicValue(graph, "bit" + suffix, 1);
            const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                            graph.internSymbol("slice" + suffix));
            graph.addOperand(slice, packed);
            graph.addOperand(slice, rowConst);
            graph.addResult(slice, bit);
            graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(1));
            const ValueId guard = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "guard" + suffix + "_op",
                                            "guard" + suffix,
                                            base.wen,
                                            bit,
                                            1);
            const ValueId next = addMux(graph,
                                        "next" + suffix + "_op",
                                        "next" + suffix,
                                        guard,
                                        base.data,
                                        base.data2,
                                        kWidth);
            addRegisterWrite(graph, "write" + suffix, base.regs[row], guard, next, base.mask, base.clk);
        }
        const PassRunResult run = runPassWithReport(base.design, "rtm.r1");
        return expectMerged(graph, run, 1, "onehot_concat_array");
    }

    // R1a two-dimensional guard: `wen && bankSel == 1 && addr == row` with a
    // shared bank equality. Exactly the addr equality selects the row; the
    // bank equality must become a common term.
    int testTwoDimensionalGuard(int64_t bankValue, bool expectMerge)
    {
        BaseDesign base = buildBase();
        Graph &graph = *base.graph;
        const ValueId bankSel = makeLogicValue(graph, "bank_sel", 2);
        graph.bindInputPort("bank_sel", bankSel);
        const ValueId bankConst = addConstant(graph,
                                              "bank_c_op",
                                              "bank_c",
                                              2,
                                              "2'd" + std::to_string(bankValue));
        const ValueId bankHit = addBinary(graph,
                                          OperationKind::kEq,
                                          "bank_hit_op",
                                          "bank_hit",
                                          bankSel,
                                          bankConst,
                                          1);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "row" + suffix + "_op",
                                                 "row" + suffix,
                                                 2,
                                                 "2'd" + suffix);
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "hit" + suffix + "_op",
                                          "hit" + suffix,
                                          base.addr,
                                          rowConst,
                                          1);
            const ValueId bankGate = addBinary(graph,
                                               OperationKind::kLogicAnd,
                                               "bank_gate" + suffix + "_op",
                                               "bank_gate" + suffix,
                                               base.wen,
                                               bankHit,
                                               1);
            const ValueId guard = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "guard" + suffix + "_op",
                                            "guard" + suffix,
                                            bankGate,
                                            hit,
                                            1);
            const ValueId next = addMux(graph,
                                        "next" + suffix + "_op",
                                        "next" + suffix,
                                        guard,
                                        base.data,
                                        base.data2,
                                        kWidth);
            addRegisterWrite(graph, "write" + suffix, base.regs[row], guard, next, base.mask, base.clk);
        }
        const PassRunResult run = runPassWithReport(base.design, "rtm.r1");
        if (expectMerge)
        {
            return expectMerged(graph, run, 1, "two_dimensional_guard");
        }
        return expectSkipped(graph, run, "two_dimensional_guard_ambiguous");
    }

    // R1b: nextValue is a direct data reference (no mux chain) and updateCond
    // is an OR of two row-selecting guards, so the regular fallback matcher
    // cannot take over.
    int testSingleBranchDirectData()
    {
        BaseDesign base = buildBase();
        Graph &graph = *base.graph;
        const ValueId wen2 = makeLogicValue(graph, "wen2", 1);
        graph.bindInputPort("wen2", wen2);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "row" + suffix + "_op",
                                                 "row" + suffix,
                                                 2,
                                                 "2'd" + suffix);
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "hit" + suffix + "_op",
                                          "hit" + suffix,
                                          base.addr,
                                          rowConst,
                                          1);
            const ValueId guard1 = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "guard1_" + suffix + "_op",
                                             "guard1_" + suffix,
                                             base.wen,
                                             hit,
                                             1);
            const ValueId guard2 = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "guard2_" + suffix + "_op",
                                             "guard2_" + suffix,
                                             wen2,
                                             hit,
                                             1);
            const ValueId cond = addBinary(graph,
                                           OperationKind::kLogicOr,
                                           "cond" + suffix + "_op",
                                           "cond" + suffix,
                                           guard1,
                                           guard2,
                                           1);
            addRegisterWrite(graph, "write" + suffix, base.regs[row], cond, base.data, base.mask, base.clk);
        }
        const PassRunResult run = runPassWithReport(base.design, "rtm.r1");
        return expectMerged(graph, run, 2, "single_branch_direct_data");
    }

    // R1b: nextValue reads the register array back through the read anchor,
    // so the single-branch acceptance must not apply.
    int testSingleBranchSelfReferenceRejected()
    {
        BaseDesign base = buildBase();
        Graph &graph = *base.graph;
        const ValueId wen2 = makeLogicValue(graph, "wen2", 1);
        graph.bindInputPort("wen2", wen2);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "row" + suffix + "_op",
                                                 "row" + suffix,
                                                 2,
                                                 "2'd" + suffix);
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "hit" + suffix + "_op",
                                          "hit" + suffix,
                                          base.addr,
                                          rowConst,
                                          1);
            const ValueId guard1 = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "guard1_" + suffix + "_op",
                                             "guard1_" + suffix,
                                             base.wen,
                                             hit,
                                             1);
            const ValueId guard2 = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "guard2_" + suffix + "_op",
                                             "guard2_" + suffix,
                                             wen2,
                                             hit,
                                             1);
            const ValueId cond = addBinary(graph,
                                           OperationKind::kLogicOr,
                                           "cond" + suffix + "_op",
                                           "cond" + suffix,
                                           guard1,
                                           guard2,
                                           1);
            addRegisterWrite(graph, "write" + suffix, base.regs[row], cond, base.selected, base.mask, base.clk);
        }
        const PassRunResult run = runPassWithReport(base.design, "rtm.r1");
        if (const int rc = expectSkipped(graph, run, "single_branch_self_reference"); rc != 0)
        {
            return rc;
        }
        if (run.report.find("\"reject_reason\":\"mux_chain\"") == std::string::npos)
        {
            std::cerr << "[reg-to-mem-r1-tests] single_branch_self_reference report: "
                      << run.report << '\n';
            return fail("single_branch_self_reference: expected mux_chain reject reason");
        }
        return 0;
    }
} // namespace

int main()
{
    try
    {
        if (const int rc = testOneHotStaticSliceGuard(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testOneHotDynamicSliceGuard(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testOneHotConcatArrayGuard(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTwoDimensionalGuard(1, true); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTwoDimensionalGuard(0, false); rc != 0)
        {
            return rc;
        }
        if (const int rc = testSingleBranchDirectData(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testSingleBranchSelfReferenceRejected(); rc != 0)
        {
            return rc;
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
    return 0;
}
