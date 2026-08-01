#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/reg_to_mem.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[reg-to-mem-report-tests] " << message << '\n';
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

    // Group (a): mergeable 4x8 array (one decoded write per row).
    // Group (b): 4x16 array whose regs each have two write ports with guards
    // that no write-family matcher accepts, so the group is rejected with
    // reject_reason "write_count".
    Design buildReportDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr std::size_t rows = 4;
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("clk", clk);

        // Group (a): regs r0..r3, width 8.
        constexpr int32_t widthA = 8;
        std::vector<std::string> regsA;
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regsA.push_back(reg);
            addRegister(graph, reg, widthA);
        }
        const ValueId indexA = makeLogicValue(graph, "index_a", 2);
        const ValueId addrA = makeLogicValue(graph, "addr_a", 2);
        const ValueId wenA = makeLogicValue(graph, "wen_a", 1);
        const ValueId dataA = makeLogicValue(graph, "data_a", widthA);
        const ValueId maskA = addConstant(graph, "mask_a_op", "mask_a", widthA, "8'hff");
        graph.bindInputPort("index_a", indexA);
        graph.bindInputPort("addr_a", addrA);
        graph.bindInputPort("wen_a", wenA);
        graph.bindInputPort("data_a", dataA);
        ValueId selectedA;
        addSliceArrayAnchor(graph, "a0", regsA, indexA, widthA, selectedA);
        graph.bindOutputPort("selected_a", selectedA);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const ValueId rowConst = addConstant(graph,
                                                 "row_a" + std::to_string(row) + "_op",
                                                 "row_a" + std::to_string(row),
                                                 2,
                                                 "2'd" + std::to_string(row));
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "hit_a" + std::to_string(row) + "_op",
                                          "hit_a" + std::to_string(row),
                                          addrA,
                                          rowConst,
                                          1);
            const ValueId guard = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "guard_a" + std::to_string(row) + "_op",
                                            "guard_a" + std::to_string(row),
                                            wenA,
                                            hit,
                                            1);
            addRegisterWrite(graph,
                             "write_a" + std::to_string(row),
                             regsA[row],
                             guard,
                             dataA,
                             maskA,
                             clk);
        }

        // Group (b): regs s0..s3, width 16, two write ports per reg.
        constexpr int32_t widthB = 16;
        std::vector<std::string> regsB;
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "s" + std::to_string(row);
            regsB.push_back(reg);
            addRegister(graph, reg, widthB);
        }
        const ValueId indexB = makeLogicValue(graph, "index_b", 2);
        const ValueId wenB = makeLogicValue(graph, "wen_b", 1);
        const ValueId dataB = makeLogicValue(graph, "data_b", widthB);
        const ValueId maskB = addConstant(graph, "mask_b_op", "mask_b", widthB, "16'hffff");
        graph.bindInputPort("index_b", indexB);
        graph.bindInputPort("wen_b", wenB);
        graph.bindInputPort("data_b", dataB);
        ValueId selectedB;
        addSliceArrayAnchor(graph, "b0", regsB, indexB, widthB, selectedB);
        graph.bindOutputPort("selected_b", selectedB);
        for (std::size_t row = 0; row < rows; ++row)
        {
            for (int port = 0; port < 2; ++port)
            {
                addRegisterWrite(graph,
                                 "write_b" + std::to_string(row) + "_" + std::to_string(port),
                                 regsB[row],
                                 wenB,
                                 dataB,
                                 maskB,
                                 clk);
            }
        }
        return design;
    }

    int testGroupReportExport()
    {
        Design design = buildReportDesign();
        Graph &graph = *design.findGraph("top");

        PassManager manager;
        wolvrix::lib::transform::SessionStore session;
        manager.options().session = &session;
        RegToMemOptions options;
        options.minElementCount = 2;
        options.enableIntent = false;
        options.outputKey = "rtm.report";
        manager.addPass(std::make_unique<RegToMemPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("reg-to-mem pass failed");
        }
        if (!result.changed)
        {
            return fail("expected reg-to-mem to change the graph");
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1)
        {
            return fail("mergeable group was not converted to a memory");
        }
        if (countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 8)
        {
            return fail("rejected group registers/writes should remain intact");
        }

        const auto reportIt = session.find("rtm.report");
        if (reportIt == session.end() || !reportIt->second)
        {
            return fail("expected reg-to-mem report in session");
        }
        if (reportIt->second->kind() != "reg-to-mem.reports")
        {
            return fail("unexpected reg-to-mem report session kind");
        }
        const auto *typed =
            dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<std::string> *>(
                reportIt->second.get());
        if (!typed)
        {
            return fail("unexpected reg-to-mem report session type");
        }

        const std::string expected =
            "{\"groups\":["
            "{\"graph\":\"top\",\"group_id\":1,\"discovery\":\"intent\",\"module\":\"\","
            "\"element_width\":8,\"element_count\":4,\"outcome\":\"true_merged\","
            "\"reject_reason\":\"\",\"reject_detail\":\"\"},"
            "{\"graph\":\"top\",\"group_id\":2,\"discovery\":\"intent\",\"module\":\"\","
            "\"element_width\":16,\"element_count\":4,\"outcome\":\"skipped\","
            "\"reject_reason\":\"write_count\",\"reject_detail\":\"row=0 writes=2\"}"
            "],\"summary\":{"
            "\"by_reason\":{\"write_count\":{\"groups\":1,\"elements\":4}},"
            "\"by_outcome\":{\"skipped\":{\"groups\":1,\"elements\":4},"
            "\"true_merged\":{\"groups\":1,\"elements\":4}}"
            "}}";
        if (typed->value != expected)
        {
            std::cerr << "[reg-to-mem-report-tests] report mismatch\nexpected: " << expected
                      << "\nactual:   " << typed->value << '\n';
            return fail("reg-to-mem group report JSON mismatch");
        }
        return 0;
    }
    int testGroupReportExportViaMakePass()
    {
        Design design = buildReportDesign();

        PassManager manager;
        wolvrix::lib::transform::SessionStore session;
        manager.options().session = &session;
        std::string error;
        const std::vector<std::string_view> args = {
            "-no-intent", "-min-element-count", "2", "-output-key", "rtm.cli"};
        auto pass = makePass("reg-to-mem", args, error);
        if (!pass || !error.empty())
        {
            return fail("makePass rejected reg-to-mem -output-key: " + error);
        }
        manager.addPass(std::move(pass));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("reg-to-mem pass via makePass failed");
        }
        const auto reportIt = session.find("rtm.cli");
        if (reportIt == session.end() || !reportIt->second)
        {
            return fail("expected reg-to-mem report under makePass output key");
        }
        if (reportIt->second->kind() != "reg-to-mem.reports")
        {
            return fail("unexpected session kind for makePass report");
        }
        return 0;
    }
} // namespace

int main()
{
    try
    {
        if (const int rc = testGroupReportExport(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testGroupReportExportViaMakePass(); rc != 0)
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
