#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/reg_to_mem.hpp"

#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[reg-to-mem-tests] " << message << '\n';
        return 1;
    }

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

    ValueId addUnary(Graph &graph,
                     OperationKind kind,
                     std::string_view opName,
                     std::string_view valueName,
                     ValueId operand,
                     int32_t width = 1)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(kind, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, operand);
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

    int runPass(Design &design, bool trueMerge)
    {
        PassManager manager;
        RegToMemOptions options;
        options.minElementCount = 2;
        options.enableTrueMerge = trueMerge;
        manager.addPass(std::make_unique<RegToMemPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("reg-to-mem pass failed");
        }
        return 0;
    }

    int runIntentPass(Design &design)
    {
        return runPass(design, false);
    }

    int runTruePass(Design &design)
    {
        return runPass(design, true);
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

    std::vector<OperationId> opsOfKind(const Graph &graph, OperationKind kind)
    {
        std::vector<OperationId> ops;
        for (OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ops.push_back(opId);
            }
        }
        return ops;
    }

    bool hasIntentGroup(const Graph &graph, OperationId opId)
    {
        return getAttr<std::string>(graph.getOperation(opId), "regToMem.intent.group").has_value();
    }

    Design buildTrueMergeDesign(bool multiAnchor,
                                bool withReset,
                                bool reduceAndLastRow = false,
                                bool secondWriteFamily = false)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        std::vector<std::string> regs;
        regs.reserve(rows);
        std::vector<ValueId> firstGuards(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
        }

        const ValueId index = makeLogicValue(graph, "index", 2);
        const ValueId index2 = makeLogicValue(graph, "index2", 2);
        const ValueId addr = makeLogicValue(graph, "addr", 2);
        const ValueId wen = makeLogicValue(graph, "wen", 1);
        const ValueId data = makeLogicValue(graph, "data", width);
        const ValueId addr2 = secondWriteFamily ? makeLogicValue(graph, "addr2", 2) : ValueId{};
        const ValueId wen2 = secondWriteFamily ? makeLogicValue(graph, "wen2", 1) : ValueId{};
        const ValueId data2 = secondWriteFamily ? makeLogicValue(graph, "data2", width) : ValueId{};
        const ValueId mask = addConstant(graph, "mask_op", "mask", width, "8'hff");
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("index", index);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("data", data);
        graph.bindInputPort("clk", clk);
        if (secondWriteFamily)
        {
            graph.bindInputPort("addr2", addr2);
            graph.bindInputPort("wen2", wen2);
            graph.bindInputPort("data2", data2);
        }

        ValueId selected;
        addSliceArrayAnchor(graph, "a0", regs, index, width, selected);
        graph.bindOutputPort("selected", selected);
        if (multiAnchor)
        {
            graph.bindInputPort("index2", index2);
            ValueId selected2;
            addSliceArrayAnchor(graph, "a1", regs, index2, width, selected2);
            graph.bindOutputPort("selected2", selected2);
        }

        for (std::size_t row = 0; row < rows; ++row)
        {
            ValueId hit;
            if (reduceAndLastRow && row + 1 == rows)
            {
                hit = addUnary(graph,
                               OperationKind::kReduceAnd,
                               "hit" + std::to_string(row) + "_op",
                               "hit" + std::to_string(row),
                               addr,
                               1);
            }
            else
            {
                const ValueId rowConst = addConstant(graph,
                                                     "row" + std::to_string(row) + "_op",
                                                     "row" + std::to_string(row),
                                                     2,
                                                     "2'd" + std::to_string(row));
                hit = addBinary(graph,
                                OperationKind::kEq,
                                "hit" + std::to_string(row) + "_op",
                                "hit" + std::to_string(row),
                                addr,
                                rowConst,
                                1);
            }
            const ValueId guard = addBinary(graph,
                                            reduceAndLastRow ? OperationKind::kAnd : OperationKind::kLogicAnd,
                                            "guard" + std::to_string(row) + "_op",
                                            "guard" + std::to_string(row),
                                            wen,
                                            hit,
                                            1);
            firstGuards[row] = guard;
            if (!secondWriteFamily)
            {
                addRegisterWrite(graph,
                                 "write" + std::to_string(row),
                                 regs[row],
                                 guard,
                                 data,
                                 mask,
                                 clk);
            }
        }

        if (secondWriteFamily)
        {
            const ValueId sharedEnable = addBinary(graph,
                                                   OperationKind::kLogicAnd,
                                                   "shared_write_enable_op",
                                                   "shared_write_enable",
                                                   wen,
                                                   wen2,
                                                   1);
            ValueId consolidatedReset;
            ValueId activeEnable = sharedEnable;
            if (withReset)
            {
                consolidatedReset = makeLogicValue(graph, "priority_reset", 1);
                graph.bindInputPort("priority_reset", consolidatedReset);
                const ValueId notReset = addUnary(graph,
                                                  OperationKind::kLogicNot,
                                                  "priority_not_reset_op",
                                                  "priority_not_reset",
                                                  consolidatedReset);
                activeEnable = addBinary(graph,
                                         OperationKind::kLogicAnd,
                                         "priority_active_enable_op",
                                         "priority_active_enable",
                                         sharedEnable,
                                         notReset,
                                         1);
            }
            const ValueId fallback = addConstant(graph, "write_fallback_op", "write_fallback", width, "8'h00");
            for (std::size_t row = 0; row < rows; ++row)
            {
                const ValueId rowConst = addConstant(graph,
                                                     "second_row" + std::to_string(row) + "_op",
                                                     "second_row" + std::to_string(row),
                                                     2,
                                                     "2'd" + std::to_string(row));
                const ValueId hit = addBinary(graph,
                                              OperationKind::kEq,
                                              "second_hit" + std::to_string(row) + "_op",
                                              "second_hit" + std::to_string(row),
                                              addr2,
                                              rowConst,
                                              1);
                const ValueId secondGuard = addBinary(graph,
                                                      OperationKind::kLogicAnd,
                                                      "second_guard" + std::to_string(row) + "_op",
                                                      "second_guard" + std::to_string(row),
                                                      activeEnable,
                                                      hit,
                                                      1);
                const ValueId notSecondHit = addUnary(graph,
                                                      OperationKind::kLogicNot,
                                                      "not_second_hit" + std::to_string(row) + "_op",
                                                      "not_second_hit" + std::to_string(row),
                                                      hit);
                const ValueId firstEligible = addBinary(graph,
                                                        OperationKind::kLogicAnd,
                                                        "first_eligible" + std::to_string(row) + "_op",
                                                        "first_eligible" + std::to_string(row),
                                                        activeEnable,
                                                        notSecondHit,
                                                        1);
                const ValueId firstGuard = addBinary(graph,
                                                     OperationKind::kLogicAnd,
                                                     "priority_first_guard" + std::to_string(row) + "_op",
                                                     "priority_first_guard" + std::to_string(row),
                                                     firstEligible,
                                                     firstGuards[row],
                                                     1);
                ValueId updateCond = addBinary(graph,
                                               OperationKind::kLogicOr,
                                               "priority_update" + std::to_string(row) + "_op",
                                               "priority_update" + std::to_string(row),
                                               firstGuard,
                                               secondGuard,
                                               1);
                ValueId rowFallback = fallback;
                if (withReset)
                {
                    rowFallback = addConstant(graph,
                                              "priority_reset_data" + std::to_string(row) + "_op",
                                              "priority_reset_data" + std::to_string(row),
                                              width,
                                              "8'd" + std::to_string(0x10 + row));
                    updateCond = addBinary(graph,
                                           OperationKind::kLogicOr,
                                           "priority_reset_update" + std::to_string(row) + "_op",
                                           "priority_reset_update" + std::to_string(row),
                                           consolidatedReset,
                                           updateCond,
                                           1);
                }
                const ValueId firstNext = addMux(graph,
                                                 "priority_first_mux" + std::to_string(row) + "_op",
                                                 "priority_first_mux" + std::to_string(row),
                                                 firstGuard,
                                                 data,
                                                 rowFallback,
                                                 width);
                const ValueId next = addMux(graph,
                                            "priority_second_mux" + std::to_string(row) + "_op",
                                            "priority_second_mux" + std::to_string(row),
                                            secondGuard,
                                            data2,
                                            firstNext,
                                            width);
                addRegisterWrite(graph,
                                 "priority_write" + std::to_string(row),
                                 regs[row],
                                 updateCond,
                                 next,
                                 mask,
                                 clk);
            }
        }

        if (withReset && !secondWriteFamily)
        {
            const ValueId rstN = makeLogicValue(graph, "rst_n", 1);
            const ValueId rstMask = addConstant(graph, "rst_mask_op", "rst_mask", width, "8'hff");
            graph.bindInputPort("rst_n", rstN);
            for (std::size_t row = 0; row < rows; ++row)
            {
                const ValueId rstData = addConstant(graph,
                                                    "rst_data" + std::to_string(row) + "_op",
                                                    "rst_data" + std::to_string(row),
                                                    width,
                                                    "8'd" + std::to_string(0x10 + row));
                addRegisterWrite(graph,
                                 "reset" + std::to_string(row),
                                 regs[row],
                                 rstN,
                                 rstData,
                                 rstMask,
                                 clk,
                                 "negedge");
            }
        }

        return design;
    }

    Design buildTrueMergeCompoundResetDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        std::vector<std::string> regs;
        regs.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
        }

        const ValueId index = makeLogicValue(graph, "index", 2);
        const ValueId addr = makeLogicValue(graph, "addr", 2);
        const ValueId wen = makeLogicValue(graph, "wen", 1);
        const ValueId reset = makeLogicValue(graph, "reset", 1);
        const ValueId data = makeLogicValue(graph, "data", width);
        const ValueId mask = addConstant(graph, "mask_op", "mask", width, "8'hff");
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("index", index);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("reset", reset);
        graph.bindInputPort("data", data);
        graph.bindInputPort("clk", clk);

        ValueId selected;
        addSliceArrayAnchor(graph, "a0", regs, index, width, selected);
        graph.bindOutputPort("selected", selected);

        const ValueId notReset = addUnary(graph,
                                          OperationKind::kLogicNot,
                                          "not_reset_op",
                                          "not_reset",
                                          reset,
                                          1);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const ValueId rowConst = addConstant(graph,
                                                 "row" + std::to_string(row) + "_op",
                                                 "row" + std::to_string(row),
                                                 2,
                                                 "2'd" + std::to_string(row));
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "hit" + std::to_string(row) + "_op",
                                          "hit" + std::to_string(row),
                                          addr,
                                          rowConst,
                                          1);
            const ValueId wenHit = addBinary(graph,
                                             OperationKind::kAnd,
                                             "wen_hit" + std::to_string(row) + "_op",
                                             "wen_hit" + std::to_string(row),
                                             wen,
                                             hit,
                                             1);
            const ValueId active = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "active" + std::to_string(row) + "_op",
                                             "active" + std::to_string(row),
                                             notReset,
                                             wenHit,
                                             1);
            const ValueId update = addBinary(graph,
                                             OperationKind::kLogicOr,
                                             "update" + std::to_string(row) + "_op",
                                             "update" + std::to_string(row),
                                             reset,
                                             active,
                                             1);
            const ValueId resetData = addConstant(graph,
                                                  "rst_data" + std::to_string(row) + "_op",
                                                  "rst_data" + std::to_string(row),
                                                  width,
                                                  "8'd" + std::to_string(0x20 + row));
            const ValueId next = addMux(graph,
                                        "next" + std::to_string(row) + "_op",
                                        "next" + std::to_string(row),
                                        active,
                                        data,
                                        resetData,
                                        width);
            addRegisterWrite(graph,
                             "write" + std::to_string(row),
                             regs[row],
                             update,
                             next,
                             mask,
                             clk);
        }

        return design;
    }

    int testSliceDynamicIntent()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        std::vector<ValueId> readValues;
        std::vector<OperationId> readOps;
        readValues.resize(4);
        readOps.resize(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            addRegister(graph, reg, 8);
            readOps[static_cast<std::size_t>(row)] =
                addRegisterRead(graph,
                                reg + "_read_op",
                                reg + "_read",
                                reg,
                                8,
                                readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed", 32);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId index = makeLogicValue(graph, "index", 2);
        graph.bindInputPort("index", index);
        const ValueId elementWidth = addConstant(graph, "const_elem_width", "elem_width", 4, "4'd8");
        ValueId start = makeLogicValue(graph, "start", 4);
        const OperationId mul = graph.createOperation(OperationKind::kMul,
                                                      graph.internSymbol("start_mul"));
        graph.addOperand(mul, index);
        graph.addOperand(mul, elementWidth);
        graph.addResult(mul, start);

        ValueId selected = makeLogicValue(graph, "selected", 8);
        const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                        graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        graph.bindOutputPort("selected", selected);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation sliceOp = graph.getOperation(slice);
        if (getAttr<std::string>(sliceOp, "regToMem.intent.sliceKind").value_or("") != "slice-dynamic")
        {
            return fail("sliceDynamic anchor was not annotated");
        }
        const std::string group = getAttr<std::string>(sliceOp, "regToMem.intent.group").value_or("");
        if (group.empty())
        {
            return fail("sliceDynamic anchor missing group attr");
        }
        if (getAttr<int64_t>(sliceOp, "regToMem.intent.elementWidth").value_or(0) != 8 ||
            getAttr<int64_t>(sliceOp, "regToMem.intent.elementCount").value_or(0) != 4)
        {
            return fail("sliceDynamic anchor has wrong element shape attrs");
        }

        const Operation concatOp = graph.getOperation(concat);
        const auto regSymbols = getAttr<std::vector<std::string>>(concatOp, "regToMem.intent.regSymbols");
        const auto operandRows = getAttr<std::vector<int64_t>>(concatOp, "regToMem.intent.operandRows");
        if (!regSymbols || *regSymbols != std::vector<std::string>{"r0", "r1", "r2", "r3"})
        {
            return fail("concat regSymbols attr does not preserve row order");
        }
        if (!operandRows || *operandRows != std::vector<int64_t>{3, 2, 1, 0})
        {
            return fail("concat operandRows attr is wrong");
        }

        for (int row = 0; row < 4; ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation("r" + std::to_string(row)));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("register row attrs are wrong");
            }
            const Operation readOp = graph.getOperation(readOps[static_cast<std::size_t>(row)]);
            if (getAttr<std::string>(readOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(readOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("read row attrs are wrong");
            }
        }
        return 0;
    }

    int testSliceDynamicWidthOneBareIndexIntent()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        std::vector<ValueId> readValues(4);
        std::vector<OperationId> readOps(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            addRegister(graph, reg, 1);
            readOps[static_cast<std::size_t>(row)] =
                addRegisterRead(graph,
                                reg + "_read_op",
                                reg + "_read",
                                reg,
                                1,
                                readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed", 4);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId index = makeLogicValue(graph, "index", 2);
        graph.bindInputPort("index", index);

        ValueId selected = makeLogicValue(graph, "selected", 1);
        const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                        graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{1});
        graph.bindOutputPort("selected", selected);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation sliceOp = graph.getOperation(slice);
        if (getAttr<std::string>(sliceOp, "regToMem.intent.sliceKind").value_or("") != "slice-dynamic")
        {
            return fail("width-one dynamic slice with bare index was not annotated");
        }
        const std::string group = getAttr<std::string>(sliceOp, "regToMem.intent.group").value_or("");
        if (group.empty())
        {
            return fail("width-one dynamic slice missing group attr");
        }
        if (getAttr<int64_t>(sliceOp, "regToMem.intent.elementWidth").value_or(0) != 1 ||
            getAttr<int64_t>(sliceOp, "regToMem.intent.elementCount").value_or(0) != 4)
        {
            return fail("width-one dynamic slice has wrong element shape attrs");
        }

        for (int row = 0; row < 4; ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation("r" + std::to_string(row)));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("width-one register row attrs are wrong");
            }
            const Operation readOp = graph.getOperation(readOps[static_cast<std::size_t>(row)]);
            if (getAttr<std::string>(readOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(readOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("width-one read row attrs are wrong");
            }
        }

        return 0;
    }

    int testIntentRejectsSharedConcat()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        std::vector<std::string> regs;
        std::vector<ValueId> readValues(4);
        regs.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
            addRegisterRead(graph,
                            reg + "_read_op",
                            reg + "_read",
                            reg,
                            width,
                            readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed", 32);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 2);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0 = makeLogicValue(graph, "selected0", width);
        const OperationId slice0 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected0_slice"));
        graph.addOperand(slice0, packed);
        graph.addOperand(slice0, index0);
        graph.addResult(slice0, selected0);
        graph.setAttr(slice0, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected0", selected0);

        ValueId selected1 = makeLogicValue(graph, "selected1", width);
        const OperationId slice1 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected1_slice"));
        graph.addOperand(slice1, packed);
        graph.addOperand(slice1, index1);
        graph.addResult(slice1, selected1);
        graph.setAttr(slice1, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }

        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kConcat) != 1)
        {
            return fail("shared-concat reject should not true-merge or delete original ops");
        }
        if (hasIntentGroup(graph, slice0) || hasIntentGroup(graph, slice1))
        {
            return fail("shared-concat slices should not be annotated under single-user anchor rules");
        }
        const Operation concatOp = graph.getOperation(concat);
        if (getAttr<std::string>(concatOp, "regToMem.intent.group").has_value())
        {
            return fail("shared concat should not get intent attrs under single-user anchor rules");
        }
        return 0;
    }

    int testIntentRejectsReadWithExtraUser()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        std::vector<std::string> regs;
        std::vector<ValueId> readValues(4);
        regs.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
            addRegisterRead(graph,
                            reg + "_read_op",
                            reg + "_read",
                            reg,
                            width,
                            readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed_extra_user", 32);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_extra_user_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        ValueId extra = makeLogicValue(graph, "extra_user", width);
        const OperationId extraAssign = graph.createOperation(OperationKind::kAssign,
                                                              graph.internSymbol("extra_user_assign"));
        graph.addOperand(extraAssign, readValues.front());
        graph.addResult(extraAssign, extra);
        graph.bindOutputPort("extra", extra);

        const ValueId index = makeLogicValue(graph, "index", 2);
        graph.bindInputPort("index", index);
        ValueId selected = makeLogicValue(graph, "selected_extra_user", width);
        const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                        graph.internSymbol("selected_extra_user_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected", selected);

        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }

        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 4 ||
            countKind(graph, OperationKind::kAssign) != 1)
        {
            return fail("extra-user reject should not true-merge or delete original ops");
        }
        if (hasIntentGroup(graph, slice))
        {
            return fail("slice with extra read user should not be annotated under single-user anchor rules");
        }
        const Operation concatOp = graph.getOperation(concat);
        if (getAttr<std::string>(concatOp, "regToMem.intent.group").has_value())
        {
            return fail("concat with extra read user should not get intent attrs under single-user anchor rules");
        }
        return 0;
    }

    int testIntentKeepsStableFamilyForConflictingRowOrder()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 2);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        const std::vector<std::string> reversedRegs{"r3", "r2", "r1", "r0"};
        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", reversedRegs, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        if (group0.empty())
        {
            return fail("stable row-order family should remain annotated");
        }
        if (hasIntentGroup(graph, slice1))
        {
            return fail("conflicting reversed row-order anchor should not be annotated");
        }
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation(regs[row]));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != group0 ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != static_cast<int64_t>(row))
            {
                return fail("stable row-order family should own register storage attrs");
            }
        }
        return 0;
    }

    int testIntentAllowsOverlappingSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        const std::vector<std::string> subsetRegs{"r0", "r1"};
        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", subsetRegs, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        const std::string group1 = getAttr<std::string>(slice1Op, "regToMem.intent.group").value_or("");
        if (group0.empty() || group1.empty() || group0 == group1)
        {
            return fail("overlapping subset anchors should be separate intent views");
        }

        const std::string storage0 =
            getAttr<std::string>(slice0Op, "regToMem.intent.storageGroup").value_or(group0);
        const std::string storage1 =
            getAttr<std::string>(slice1Op, "regToMem.intent.storageGroup").value_or(group1);
        if (storage0.empty() || storage0 != storage1)
        {
            return fail("overlapping subset view should share the superset storage group");
        }
        if (getAttr<int64_t>(slice0Op, "regToMem.intent.elementCount").value_or(0) != 4 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.elementCount").value_or(0) != 2)
        {
            return fail("overlapping subset views should preserve local element counts");
        }
        if (getAttr<int64_t>(slice0Op, "regToMem.intent.storageElementCount").value_or(0) != 4 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageElementCount").value_or(0) != 4)
        {
            return fail("overlapping subset views should use superset storage element count");
        }
        if (getAttr<int64_t>(slice0Op, "regToMem.intent.storageRowOffset").value_or(-1) != 0 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageRowOffset").value_or(-1) != 0)
        {
            return fail("prefix subset view should have zero storage row offset");
        }
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation(regs[row]));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != storage0 ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != static_cast<int64_t>(row))
            {
                return fail("storage register attrs should be owned by the superset storage group");
            }
        }
        return 0;
    }

    int testIntentAllowsMiddleSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        const std::vector<std::string> middleRegs{"r1", "r2"};
        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", middleRegs, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        const std::string group1 = getAttr<std::string>(slice1Op, "regToMem.intent.group").value_or("");
        if (group0.empty() || group1.empty() || group0 == group1)
        {
            return fail("middle subset anchors should be separate intent views");
        }

        const std::string storage0 =
            getAttr<std::string>(slice0Op, "regToMem.intent.storageGroup").value_or(group0);
        const std::string storage1 =
            getAttr<std::string>(slice1Op, "regToMem.intent.storageGroup").value_or(group1);
        if (storage0.empty() || storage0 != storage1)
        {
            return fail("middle subset view should share superset storage");
        }
        if (getAttr<int64_t>(slice1Op, "regToMem.intent.elementCount").value_or(0) != 2 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageElementCount").value_or(0) != 4 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageRowOffset").value_or(-1) != 1)
        {
            return fail("middle subset view should preserve local count and storage row offset");
        }
        return 0;
    }

    int testIntentRejectsSharedReadSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        std::vector<ValueId> readValues(regs.size());
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            addRegister(graph, regs[row], width);
            addRegisterRead(graph,
                            regs[row] + "_read_op",
                            regs[row] + "_read",
                            regs[row],
                            width,
                            readValues[row]);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId packedFull = makeLogicValue(graph, "packed_full", 32);
        const OperationId concatFull = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol("packed_full_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concatFull, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concatFull, packedFull);

        ValueId selectedFull = makeLogicValue(graph, "selected_full", width);
        const OperationId sliceFull = graph.createOperation(OperationKind::kSliceArray,
                                                            graph.internSymbol("selected_full_slice"));
        graph.addOperand(sliceFull, packedFull);
        graph.addOperand(sliceFull, index0);
        graph.addResult(sliceFull, selectedFull);
        graph.setAttr(sliceFull, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected_full", selectedFull);

        ValueId packedSubset = makeLogicValue(graph, "packed_subset", 16);
        const OperationId concatSubset = graph.createOperation(OperationKind::kConcat,
                                                               graph.internSymbol("packed_subset_concat"));
        graph.addOperand(concatSubset, readValues[2]);
        graph.addOperand(concatSubset, readValues[1]);
        graph.addResult(concatSubset, packedSubset);

        ValueId selectedSubset = makeLogicValue(graph, "selected_subset", width);
        const OperationId sliceSubset = graph.createOperation(OperationKind::kSliceArray,
                                                              graph.internSymbol("selected_subset_slice"));
        graph.addOperand(sliceSubset, packedSubset);
        graph.addOperand(sliceSubset, index1);
        graph.addResult(sliceSubset, selectedSubset);
        graph.setAttr(sliceSubset, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected_subset", selectedSubset);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        if (hasIntentGroup(graph, sliceFull))
        {
            return fail("shared-read superset should not be annotated under single-user anchor rules");
        }
        if (hasIntentGroup(graph, sliceSubset))
        {
            return fail("shared-read subset should not be annotated under single-user anchor rules");
        }
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            const Operation readOp = graph.getOperation(graph.valueDef(readValues[row]));
            if (getAttr<std::string>(readOp, "regToMem.intent.group").has_value() ||
                getAttr<int64_t>(readOp, "regToMem.intent.row").has_value())
            {
                return fail("shared-read rejected anchors should not leave read attrs");
            }
        }
        return 0;
    }

    int testIntentRejectsSiblingSharedReadSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        const ValueId index2 = makeLogicValue(graph, "index2", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);
        graph.bindInputPort("index2", index2);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        std::vector<ValueId> sharedReads(3);
        for (std::size_t row = 0; row < sharedReads.size(); ++row)
        {
            addRegisterRead(graph,
                            "shared_r" + std::to_string(row) + "_read_op",
                            "shared_r" + std::to_string(row) + "_read",
                            regs[row],
                            width,
                            sharedReads[row]);
        }

        ValueId packed1 = makeLogicValue(graph, "packed1", 16);
        const OperationId concat1 = graph.createOperation(OperationKind::kConcat,
                                                          graph.internSymbol("packed1_concat"));
        graph.addOperand(concat1, sharedReads[1]);
        graph.addOperand(concat1, sharedReads[0]);
        graph.addResult(concat1, packed1);

        ValueId selected1 = makeLogicValue(graph, "selected1", width);
        const OperationId slice1 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected1_slice"));
        graph.addOperand(slice1, packed1);
        graph.addOperand(slice1, index1);
        graph.addResult(slice1, selected1);
        graph.setAttr(slice1, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected1", selected1);

        ValueId packed2 = makeLogicValue(graph, "packed2", 16);
        const OperationId concat2 = graph.createOperation(OperationKind::kConcat,
                                                          graph.internSymbol("packed2_concat"));
        graph.addOperand(concat2, sharedReads[2]);
        graph.addOperand(concat2, sharedReads[1]);
        graph.addResult(concat2, packed2);

        ValueId selected2 = makeLogicValue(graph, "selected2", width);
        const OperationId slice2 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected2_slice"));
        graph.addOperand(slice2, packed2);
        graph.addOperand(slice2, index2);
        graph.addResult(slice2, selected2);
        graph.setAttr(slice2, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected2", selected2);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        if (group0.empty())
        {
            return fail("independent root view should remain annotated");
        }
        if (hasIntentGroup(graph, slice1))
        {
            return fail("first sibling view with a shared read should not be annotated");
        }
        if (hasIntentGroup(graph, slice2))
        {
            return fail("second sibling view with a shared read should not be annotated");
        }
        const Operation sharedRead1 = graph.getOperation(graph.valueDef(sharedReads[1]));
        if (getAttr<std::string>(sharedRead1, "regToMem.intent.group").has_value() ||
            getAttr<int64_t>(sharedRead1, "regToMem.intent.row").has_value())
        {
            return fail("rejected sibling views should not leave shared read attrs");
        }
        return 0;
    }

    int testIntentKeepsStableFamilyForPartialOverlap()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 1);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", std::vector<std::string>{"r0", "r1"}, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", std::vector<std::string>{"r1", "r2"}, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const std::string group0 =
            getAttr<std::string>(graph.getOperation(slice0), "regToMem.intent.group").value_or("");
        if (group0.empty())
        {
            return fail("stable partial-overlap family should remain annotated");
        }
        if (hasIntentGroup(graph, slice1))
        {
            return fail("later partial-overlap family without superset storage should not be annotated");
        }
        if (getAttr<std::string>(graph.getOperation(graph.findOperation("r2")), "regToMem.intent.group").has_value())
        {
            return fail("register only used by dropped partial-overlap family should not get intent attrs");
        }
        return 0;
    }

    int testIntentDoesNotDropCompatibleFamilyBecauseOfBridgeConflict()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3", "r4"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        const ValueId index2 = makeLogicValue(graph, "index2", 2);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);
        graph.bindInputPort("index2", index2);

        ValueId selected0;
        const OperationId slice0 =
            addSliceArrayAnchor(graph, "a0", std::vector<std::string>{"r0", "r1", "r2", "r3"}, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        ValueId selected1;
        const OperationId slice1 =
            addSliceArrayAnchor(graph, "a1", std::vector<std::string>{"r0", "r1"}, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        ValueId selected2;
        const OperationId slice2 =
            addSliceArrayAnchor(graph, "a2", std::vector<std::string>{"r2", "r3", "r4"}, index2, width, selected2);
        graph.bindOutputPort("selected2", selected2);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        const std::string group1 = getAttr<std::string>(slice1Op, "regToMem.intent.group").value_or("");
        if (group0.empty() || group1.empty() || group0 == group1)
        {
            return fail("compatible superset/subset family should survive bridge conflict");
        }
        const std::string storage0 =
            getAttr<std::string>(slice0Op, "regToMem.intent.storageGroup").value_or(group0);
        const std::string storage1 =
            getAttr<std::string>(slice1Op, "regToMem.intent.storageGroup").value_or(group1);
        if (storage0.empty() || storage0 != storage1)
        {
            return fail("compatible family should still share storage under bridge conflict");
        }
        if (hasIntentGroup(graph, slice2))
        {
            return fail("bridge conflict family should not be annotated");
        }
        if (getAttr<std::string>(graph.getOperation(graph.findOperation("r4")), "regToMem.intent.group").has_value())
        {
            return fail("register only used by bridge conflict should not get intent attrs");
        }
        return 0;
    }

    int testTrueMergeSliceArray()
    {
        Design design = buildTrueMergeDesign(false, false);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0 ||
            countKind(graph, OperationKind::kConcat) != 0 ||
            countKind(graph, OperationKind::kSliceArray) != 0)
        {
            return fail("true merge did not remove register/read/write/concat/slice ops");
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1)
        {
            return fail("true merge did not create expected memory ports");
        }
        const Operation mem = graph.getOperation(opsOfKind(graph, OperationKind::kMemory).front());
        const std::string memSymbol = std::string(mem.symbolText());
        if (getAttr<int64_t>(mem, "width").value_or(0) != 8 ||
            getAttr<int64_t>(mem, "row").value_or(0) != 4)
        {
            return fail("true merge memory shape is wrong");
        }
        const Operation read = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryReadPort).front());
        const Operation write = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryWritePort).front());
        if (getAttr<std::string>(read, "memSymbol").value_or("") != memSymbol ||
            getAttr<std::string>(write, "memSymbol").value_or("") != memSymbol)
        {
            return fail("true merge memory ports reference wrong memSymbol");
        }
        const ValueId output = graph.outputPortValue("selected");
        if (!output.valid() || graph.valueDef(output) != read.id())
        {
            return fail("true merge did not rebind output port to memory read result");
        }
        return 0;
    }

    int testTrueMergeReduceAndLastRowGuard()
    {
        Design design = buildTrueMergeDesign(false, false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge did not match reduce-and all-ones row guard");
        }
        return 0;
    }

    int testTrueMergeMultiAnchor()
    {
        Design design = buildTrueMergeDesign(true, false);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 2 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0)
        {
            return fail("true merge multi-anchor shape is wrong");
        }
        return 0;
    }

    int testTrueMergeMultipleWriteFamilies()
    {
        Design design = buildTrueMergeDesign(false, false, false, true);
        Graph &graph = *design.findGraph("top");
        const ValueId addr = graph.inputPortValue("addr");
        const ValueId data = graph.inputPortValue("data");
        const ValueId addr2 = graph.inputPortValue("addr2");
        const ValueId data2 = graph.inputPortValue("data2");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 2 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge multiple-write family shape is wrong");
        }
        const std::vector<OperationId> writes = opsOfKind(graph, OperationKind::kMemoryWritePort);
        if (writes.size() != 2)
        {
            return fail("true merge multiple-write family count is wrong");
        }
        const Operation first = graph.getOperation(writes[0]);
        const Operation second = graph.getOperation(writes[1]);
        if (first.operands().size() < 4 || second.operands().size() < 4 ||
            first.operands()[1] != addr2 || first.operands()[2] != data2 ||
            second.operands()[1] != addr || second.operands()[2] != data)
        {
            return fail("true merge multiple-write family order or operands are wrong");
        }
        return 0;
    }

    int testTrueMergeConsolidatedWriteFamiliesWithReset()
    {
        Design design = buildTrueMergeDesign(false, true, false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 2 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge consolidated write families with reset shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() != 3 || graph.valueWidth(fill.operands()[1]) != 32)
        {
            return fail("true merge consolidated reset did not create packed fill data");
        }
        return 0;
    }

    int testTrueMergeResetFill()
    {
        Design design = buildTrueMergeDesign(false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge reset/fill shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() != 3)
        {
            return fail("memory fill operand count is wrong");
        }
        if (graph.valueWidth(fill.operands()[1]) != 32)
        {
            return fail("memory fill did not use packed reset data for per-row reset values");
        }
        return 0;
    }

    int testTrueMergeCompoundResetFill()
    {
        Design design = buildTrueMergeCompoundResetDesign();
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge compound reset/write shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() != 3)
        {
            return fail("compound reset memory fill operand count is wrong");
        }
        if (graph.valueWidth(fill.operands()[1]) != 32)
        {
            return fail("compound reset memory fill did not use packed reset data");
        }
        return 0;
    }
} // namespace

int main()
{
    try
    {
        if (const int rc = testSliceDynamicIntent(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testSliceDynamicWidthOneBareIndexIntent(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsSharedConcat(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsReadWithExtraUser(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentKeepsStableFamilyForConflictingRowOrder(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentAllowsOverlappingSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentAllowsMiddleSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsSharedReadSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsSiblingSharedReadSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentKeepsStableFamilyForPartialOverlap(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentDoesNotDropCompatibleFamilyBecauseOfBridgeConflict(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeSliceArray(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeReduceAndLastRowGuard(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeMultiAnchor(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeMultipleWriteFamilies(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeConsolidatedWriteFamiliesWithReset(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeResetFill(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeCompoundResetFill(); rc != 0)
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
