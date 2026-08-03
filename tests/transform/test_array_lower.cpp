#include "core/grh.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "transform/array_lower.hpp"

#include "slang/numeric/SVInt.h"

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
    namespace grh = wolvrix::lib::grh;

    int fail(const std::string &message)
    {
        std::cerr << "[array-lower-tests] " << message << '\n';
        return 1;
    }

    // ------------------------------------------------------------------
    // Graph builders.
    // ------------------------------------------------------------------
    grh::ValueId makeInput(grh::Graph &graph, const std::string &name, int32_t width)
    {
        const grh::ValueId val = graph.createValue(graph.internSymbol(name), width, false);
        graph.bindInputPort(name, val);
        return val;
    }

    grh::OperationId makeMemory(grh::Graph &graph, const std::string &name, int32_t width, int32_t rows)
    {
        const grh::OperationId mem =
            graph.createOperation(grh::OperationKind::kMemory, graph.internSymbol(name));
        graph.setAttr(mem, "width", static_cast<int64_t>(width));
        graph.setAttr(mem, "row", static_cast<int64_t>(rows));
        graph.setAttr(mem, "isSigned", false);
        return mem;
    }

    grh::ValueId makeNamedValue(grh::Graph &graph, const std::string &name, int32_t width)
    {
        return graph.createValue(graph.internSymbol(name), width, false);
    }

    grh::ValueId makeMemoryReadAll(grh::Graph &graph, const std::string &name,
                                  const std::string &memSymbol, int32_t width)
    {
        const grh::ValueId out = makeNamedValue(graph, name, width);
        const grh::OperationId op = graph.createOperation(grh::OperationKind::kMemoryReadAllPort);
        graph.addResult(op, out);
        graph.setAttr(op, "memSymbol", memSymbol);
        return out;
    }

    grh::ValueId makeArrayBroadcast(grh::Graph &graph, const std::string &name,
                                    grh::ValueId scalar, int64_t rows, int32_t width)
    {
        const grh::ValueId out = makeNamedValue(graph, name, width);
        const grh::OperationId op = graph.createOperation(grh::OperationKind::kArrayBroadcast);
        graph.addOperand(op, scalar);
        graph.addResult(op, out);
        graph.setAttr(op, "rows", rows);
        return out;
    }

    grh::ValueId makeArrayLaneConst(grh::Graph &graph, const std::string &name, int64_t elemWidth,
                                    int64_t rows, std::vector<int64_t> values, int32_t width)
    {
        const grh::ValueId out = makeNamedValue(graph, name, width);
        const grh::OperationId op = graph.createOperation(grh::OperationKind::kArrayLaneConst);
        graph.addResult(op, out);
        graph.setAttr(op, "elemWidth", elemWidth);
        graph.setAttr(op, "rows", rows);
        graph.setAttr(op, "values", std::move(values));
        return out;
    }

    grh::ValueId makeArrayOnehot(grh::Graph &graph, const std::string &name,
                                 grh::ValueId x, int64_t rows)
    {
        const grh::ValueId out = makeNamedValue(graph, name, static_cast<int32_t>(rows));
        const grh::OperationId op = graph.createOperation(grh::OperationKind::kArrayOnehot);
        graph.addOperand(op, x);
        graph.addResult(op, out);
        graph.setAttr(op, "rows", rows);
        return out;
    }

    grh::ValueId makeArrayMux(grh::Graph &graph, const std::string &name, grh::ValueId sel,
                              grh::ValueId t, grh::ValueId f, int32_t width)
    {
        const grh::ValueId out = makeNamedValue(graph, name, width);
        const grh::OperationId op = graph.createOperation(grh::OperationKind::kArrayMux);
        graph.addOperand(op, sel);
        graph.addOperand(op, t);
        graph.addOperand(op, f);
        graph.addResult(op, out);
        return out;
    }

    grh::ValueId makeArrayReduce(grh::Graph &graph, const std::string &name,
                                 grh::OperationKind kind, grh::ValueId data)
    {
        const grh::ValueId out = makeNamedValue(graph, name, 1);
        const grh::OperationId op = graph.createOperation(kind);
        graph.addOperand(op, data);
        graph.addResult(op, out);
        graph.setAttr(op, "elemWidth", static_cast<int64_t>(4));
        return out;
    }

    grh::ValueId makeArrayReduceLanes(grh::Graph &graph, const std::string &name,
                                      grh::OperationKind kind, grh::ValueId data,
                                      int64_t elemWidth, int32_t rows)
    {
        const grh::ValueId out = makeNamedValue(graph, name, rows);
        const grh::OperationId op = graph.createOperation(kind);
        graph.addOperand(op, data);
        graph.addResult(op, out);
        graph.setAttr(op, "elemWidth", elemWidth);
        return out;
    }

    grh::OperationId makeMemoryWriteLanes(grh::Graph &graph, const std::string &memSymbol,
                                    grh::ValueId laneMask, grh::ValueId data,
                                    std::vector<grh::ValueId> events, bool withPriority)
    {
        const grh::OperationId op = graph.createOperation(grh::OperationKind::kMemoryWriteLanesPort);
        graph.addOperand(op, laneMask);
        graph.addOperand(op, data);
        for (const grh::ValueId event : events)
        {
            graph.addOperand(op, event);
        }
        graph.setAttr(op, "memSymbol", memSymbol);
        graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
        if (withPriority)
        {
            graph.setAttr(op, "memoryWrite.priorityGroup", std::string("writes"));
            graph.setAttr(op, "memoryWrite.priority", static_cast<int64_t>(0));
        }
        return op;
    }

    // ------------------------------------------------------------------
    // Inspection helpers.
    // ------------------------------------------------------------------
    std::optional<std::string> getStringAttr(const grh::Operation &op, std::string_view key)
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

    std::optional<int64_t> getIntAttr(const grh::Operation &op, std::string_view key)
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

    std::optional<std::vector<std::string>> getStringListAttr(const grh::Operation &op,
                                                              std::string_view key)
    {
        auto attr = op.attr(key);
        if (!attr)
        {
            return std::nullopt;
        }
        if (const auto *value = std::get_if<std::vector<std::string>>(&*attr))
        {
            return *value;
        }
        return std::nullopt;
    }

    std::size_t countOpsOfKind(const grh::Graph &graph, grh::OperationKind kind)
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

    grh::Operation defOpOf(const grh::Graph &graph, grh::ValueId value)
    {
        return graph.getOperation(graph.getValue(value).definingOp());
    }

    bool isDefinedBy(const grh::Graph &graph, grh::ValueId value, grh::OperationKind kind)
    {
        const grh::OperationId def = graph.getValue(value).definingOp();
        return def.valid() && graph.getOperation(def).kind() == kind;
    }

    std::optional<slang::SVInt> constLiteralOf(const grh::Graph &graph, grh::ValueId value)
    {
        const grh::OperationId def = graph.getValue(value).definingOp();
        if (!def.valid())
        {
            return std::nullopt;
        }
        const grh::Operation op = graph.getOperation(def);
        if (op.kind() != grh::OperationKind::kConstant)
        {
            return std::nullopt;
        }
        const auto literal = getStringAttr(op, "constValue");
        if (!literal)
        {
            return std::nullopt;
        }
        try
        {
            return slang::SVInt::fromString(*literal);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    bool constEquals(const grh::Graph &graph, grh::ValueId value, const slang::SVInt &expected)
    {
        const auto literal = constLiteralOf(graph, value);
        return literal.has_value() && static_cast<bool>((*literal == expected));
    }

    // Returns the row index of a constant address value, or -1.
    int addrRowOf(const grh::Graph &graph, grh::ValueId value, int32_t addrWidth, int32_t maxRows)
    {
        const auto literal = constLiteralOf(graph, value);
        if (!literal)
        {
            return -1;
        }
        for (int32_t row = 0; row < maxRows; ++row)
        {
            if (static_cast<bool>(
                    (*literal == slang::SVInt(static_cast<slang::bitwidth_t>(addrWidth),
                                              static_cast<uint64_t>(row), false))))
            {
                return row;
            }
        }
        return -1;
    }

    bool isSliceOf(const grh::Graph &graph, grh::ValueId value, grh::ValueId base,
                   int64_t low, int64_t high)
    {
        if (!isDefinedBy(graph, value, grh::OperationKind::kSliceStatic))
        {
            return false;
        }
        const grh::Operation op = defOpOf(graph, value);
        return op.operands().size() == 1 && op.operands().front() == base &&
               getIntAttr(op, "sliceStart") == low && getIntAttr(op, "sliceEnd") == high;
    }

    bool roundTripJson(const grh::Design &design)
    {
        wolvrix::lib::store::StoreDiagnostics storeDiags;
        wolvrix::lib::store::StoreJson store(&storeDiags);
        const auto json = store.storeToString(design);
        if (!json || storeDiags.hasError())
        {
            return false;
        }
        try
        {
            const grh::Design reloaded = grh::Design::fromJsonString(*json);
            return reloaded.findGraph(design.topGraphs().empty() ? "" : design.topGraphs().front()) !=
                   nullptr;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    int runPass(grh::Design &design, bool &changed)
    {
        PassManager manager;
        manager.addPass(std::make_unique<ArrayLowerPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("array-lower pass run failed");
        }
        changed = res.changed;
        return 0;
    }

    bool noArrayOpsRemain(const grh::Graph &graph)
    {
        static constexpr grh::OperationKind kKinds[] = {
            grh::OperationKind::kMemoryReadAllPort, grh::OperationKind::kMemoryWriteLanesPort,
            grh::OperationKind::kArrayMux,         grh::OperationKind::kArrayReduceOr,
            grh::OperationKind::kArrayReduceAnd,   grh::OperationKind::kArrayReduceXor,
            grh::OperationKind::kArrayBroadcast,   grh::OperationKind::kArrayLaneConst,
            grh::OperationKind::kArrayOnehot,      grh::OperationKind::kArrayReduceLanesOr,
            grh::OperationKind::kArrayReduceLanesAnd, grh::OperationKind::kArrayReduceLanesXor,
        };
        for (const grh::OperationKind kind : kKinds)
        {
            if (countOpsOfKind(graph, kind) != 0)
            {
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Main case: one graph carrying all nine array ops (8 lanes x 4 bits),
    // a readall -> mux -> write loop, events + priority on the write port,
    // reduce/broadcast/laneconst/onehot, and a row==1 readall.
    // ------------------------------------------------------------------
    int testExpandAllArrayOps()
    {
        constexpr int32_t kLaneWidth = 4;
        constexpr int32_t kRows = 8;
        constexpr int32_t kPacked = kLaneWidth * kRows;

        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId idx = makeInput(graph, "idx", 4); // wider than log2(8) on purpose
        const grh::ValueId scalar = makeInput(graph, "scalar", kLaneWidth);
        makeMemory(graph, "mem", kLaneWidth, kRows);
        makeMemory(graph, "mem1", kLaneWidth, 1);

        const grh::ValueId readAll = makeMemoryReadAll(graph, "read_all", "mem", kPacked);
        const grh::ValueId bcast =
            makeArrayBroadcast(graph, "bcast_out", scalar, kRows, kPacked);
        const grh::ValueId laneConst =
            makeArrayLaneConst(graph, "lane_const", kLaneWidth, kRows,
                               {0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x0, 0x1}, kPacked);
        const grh::ValueId onehot = makeArrayOnehot(graph, "onehot_out", idx, kRows);
        const grh::ValueId mux = makeArrayMux(graph, "mux_out", onehot, readAll, bcast, kPacked);
        const grh::ValueId redOr =
            makeArrayReduce(graph, "red_or", grh::OperationKind::kArrayReduceOr, mux);
        const grh::ValueId redAnd =
            makeArrayReduce(graph, "red_and", grh::OperationKind::kArrayReduceAnd, mux);
        const grh::ValueId redXor =
            makeArrayReduce(graph, "red_xor", grh::OperationKind::kArrayReduceXor, laneConst);
        makeMemoryWriteLanes(graph, "mem", onehot, mux, {clk}, /*withPriority=*/true);
        const grh::ValueId readAll1 = makeMemoryReadAll(graph, "read_all_1", "mem1", kLaneWidth);

        graph.bindOutputPort("out_m", mux);
        graph.bindOutputPort("out_ro", redOr);
        graph.bindOutputPort("out_ra", redAnd);
        graph.bindOutputPort("out_rx", redXor);
        graph.bindOutputPort("out_oh", onehot);
        graph.bindOutputPort("out_r1", readAll1);

        bool changed = false;
        if (const int rc = runPass(design, changed))
        {
            return rc;
        }
        if (!changed)
        {
            return fail("expected the pass to change the graph");
        }
        if (!noArrayOpsRemain(graph))
        {
            return fail("array ops must be fully expanded");
        }

        // kMemory declarations pass through untouched.
        if (countOpsOfKind(graph, grh::OperationKind::kMemory) != 2)
        {
            return fail("kMemory declarations must be preserved");
        }
        const grh::OperationId memOp = graph.findOperation("mem");
        if (!memOp.valid() || graph.getOperation(memOp).kind() != grh::OperationKind::kMemory ||
            getIntAttr(graph.getOperation(memOp), "width") != kLaneWidth ||
            getIntAttr(graph.getOperation(memOp), "row") != kRows)
        {
            return fail("kMemory mem must keep width/row attrs");
        }

        // Symbols move onto the replacement values.
        const grh::ValueId muxResult = graph.findValue("mux_out");
        const grh::ValueId concatResult = graph.findValue("read_all");
        const grh::ValueId bcastResult = graph.findValue("bcast_out");
        const grh::ValueId laneConstResult = graph.findValue("lane_const");
        const grh::ValueId onehotResult = graph.findValue("onehot_out");
        if (!muxResult.valid() || !concatResult.valid() || !bcastResult.valid() ||
            !laneConstResult.valid() || !onehotResult.valid())
        {
            return fail("array op result symbols must move to the replacement values");
        }
        if (graph.outputPortValue("out_m") != muxResult ||
            graph.outputPortValue("out_oh") != onehotResult)
        {
            return fail("output ports must be rebound to the replacement values");
        }

        // kArrayOnehot -> kShl(kConstant(8'd1), idx).
        if (!isDefinedBy(graph, onehotResult, grh::OperationKind::kShl))
        {
            return fail("onehot must expand to kShl");
        }
        {
            const grh::Operation shl = defOpOf(graph, onehotResult);
            if (shl.operands().size() != 2 || shl.operands()[1] != idx ||
                !constEquals(graph, shl.operands()[0], slang::SVInt::fromString("8'd1")))
            {
                return fail("onehot expansion must be kShl(8'd1, idx)");
            }
        }

        // kMemoryReadAllPort -> 8 kMemoryReadPort + kConcat (row 7 in MSBs).
        if (!isDefinedBy(graph, concatResult, grh::OperationKind::kConcat))
        {
            return fail("readall must expand to a kConcat of row reads");
        }
        {
            const grh::Operation concat = defOpOf(graph, concatResult);
            if (concat.operands().size() != static_cast<std::size_t>(kRows))
            {
                return fail("readall concat must have one operand per row");
            }
            for (std::size_t pos = 0; pos < concat.operands().size(); ++pos)
            {
                const grh::ValueId rowRead = concat.operands()[pos];
                if (!isDefinedBy(graph, rowRead, grh::OperationKind::kMemoryReadPort))
                {
                    return fail("readall concat operands must be kMemoryReadPort");
                }
                const grh::Operation read = defOpOf(graph, rowRead);
                if (getStringAttr(read, "memSymbol") != "mem" || read.operands().size() != 1)
                {
                    return fail("row read must target mem with one address operand");
                }
                const int expectedRow = kRows - 1 - static_cast<int>(pos);
                if (addrRowOf(graph, read.operands().front(), 3, kRows) != expectedRow)
                {
                    return fail("row read address must be the constant row index");
                }
                if (graph.valueWidth(rowRead) != kLaneWidth)
                {
                    return fail("row read width must be the lane width");
                }
            }
        }

        // kArrayMux -> kOr(kAnd(t, m), kAnd(f, kNot(m))).
        if (!isDefinedBy(graph, muxResult, grh::OperationKind::kOr))
        {
            return fail("mux must expand to a kOr");
        }
        grh::ValueId mask;
        {
            const grh::Operation orOp = defOpOf(graph, muxResult);
            if (orOp.operands().size() != 2)
            {
                return fail("mux kOr must have two operands");
            }
            const grh::ValueId andT = orOp.operands()[0];
            const grh::ValueId andF = orOp.operands()[1];
            if (!isDefinedBy(graph, andT, grh::OperationKind::kAnd) ||
                !isDefinedBy(graph, andF, grh::OperationKind::kAnd))
            {
                return fail("mux kOr operands must be kAnd ops");
            }
            const grh::Operation andTOp = defOpOf(graph, andT);
            const grh::Operation andFOp = defOpOf(graph, andF);
            if (andTOp.operands().size() != 2 || andTOp.operands()[0] != concatResult)
            {
                return fail("mux true arm must be kAnd(t, mask)");
            }
            mask = andTOp.operands()[1];
            if (andFOp.operands().size() != 2 || andFOp.operands()[0] != bcastResult)
            {
                return fail("mux false arm must be kAnd(f, ~mask)");
            }
            const grh::ValueId notMask = andFOp.operands()[1];
            if (!isDefinedBy(graph, notMask, grh::OperationKind::kNot) ||
                defOpOf(graph, notMask).operands().front() != mask)
            {
                return fail("mux false arm select must be kNot(mask)");
            }
        }
        // mask = kConcat of per-lane kReplicate(kSliceStatic(sel, i, i), W).
        {
            if (!isDefinedBy(graph, mask, grh::OperationKind::kConcat))
            {
                return fail("mux mask must be a kConcat");
            }
            const grh::Operation maskConcat = defOpOf(graph, mask);
            if (maskConcat.operands().size() != static_cast<std::size_t>(kRows))
            {
                return fail("mux mask concat must have one operand per lane");
            }
            for (std::size_t pos = 0; pos < maskConcat.operands().size(); ++pos)
            {
                const grh::ValueId repVal = maskConcat.operands()[pos];
                if (!isDefinedBy(graph, repVal, grh::OperationKind::kReplicate))
                {
                    return fail("mux mask lanes must be kReplicate");
                }
                const grh::Operation rep = defOpOf(graph, repVal);
                const int lane = kRows - 1 - static_cast<int>(pos);
                if (getIntAttr(rep, "rep") != kLaneWidth || rep.operands().size() != 1 ||
                    !isSliceOf(graph, rep.operands().front(), onehotResult, lane, lane))
                {
                    return fail("mux mask lane must replicate kSliceStatic(sel, i, i)");
                }
            }
        }

        // kArrayBroadcast -> kReplicate(scalar, rows).
        {
            if (!isDefinedBy(graph, bcastResult, grh::OperationKind::kReplicate))
            {
                return fail("broadcast must expand to kReplicate");
            }
            const grh::Operation rep = defOpOf(graph, bcastResult);
            if (getIntAttr(rep, "rep") != kRows || rep.operands().size() != 1 ||
                rep.operands().front() != scalar)
            {
                return fail("broadcast expansion must be kReplicate(scalar, rows)");
            }
        }

        // kArrayLaneConst -> packed kConstant 32'h10FEDCBA.
        if (!constEquals(graph, laneConstResult, slang::SVInt::fromString("32'h10FEDCBA")))
        {
            return fail("lane const must pack values[i] into [i*W +: W]");
        }

        // kArrayReduce* -> plain full-width reductions.
        {
            const grh::ValueId redOrOut = graph.outputPortValue("out_ro");
            const grh::ValueId redAndOut = graph.outputPortValue("out_ra");
            const grh::ValueId redXorOut = graph.outputPortValue("out_rx");
            if (!isDefinedBy(graph, redOrOut, grh::OperationKind::kReduceOr) ||
                defOpOf(graph, redOrOut).operands().front() != muxResult)
            {
                return fail("kArrayReduceOr must expand to kReduceOr over the packed data");
            }
            if (!isDefinedBy(graph, redAndOut, grh::OperationKind::kReduceAnd) ||
                defOpOf(graph, redAndOut).operands().front() != muxResult)
            {
                return fail("kArrayReduceAnd must expand to kReduceAnd over the packed data");
            }
            if (!isDefinedBy(graph, redXorOut, grh::OperationKind::kReduceXor) ||
                defOpOf(graph, redXorOut).operands().front() != laneConstResult)
            {
                return fail("kArrayReduceXor must expand to kReduceXor over the packed data");
            }
        }

        // kMemoryWriteLanesPort -> 8 kMemoryWritePort, priority attrs dropped.
        {
            std::vector<grh::OperationId> writePorts;
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const grh::Operation op = graph.getOperation(opId);
                if (op.kind() == grh::OperationKind::kMemoryWritePort &&
                    getStringAttr(op, "memSymbol") == "mem")
                {
                    writePorts.push_back(opId);
                }
            }
            if (writePorts.size() != static_cast<std::size_t>(kRows))
            {
                return fail("write port must expand to one kMemoryWritePort per row");
            }
            std::vector<bool> seen(kRows, false);
            for (const grh::OperationId wpId : writePorts)
            {
                const grh::Operation wp = graph.getOperation(wpId);
                if (wp.operands().size() != 5)
                {
                    return fail("expanded write port must have 4 operands + 1 event");
                }
                const int row = addrRowOf(graph, wp.operands()[1], 3, kRows);
                if (row < 0 || seen[static_cast<std::size_t>(row)])
                {
                    return fail("expanded write port must use distinct constant addresses");
                }
                seen[static_cast<std::size_t>(row)] = true;
                if (!isSliceOf(graph, wp.operands()[0], onehotResult, row, row))
                {
                    return fail("write updateCond must be kSliceStatic(laneMask, i, i)");
                }
                if (!isSliceOf(graph, wp.operands()[2], muxResult, row * kLaneWidth,
                               row * kLaneWidth + kLaneWidth - 1))
                {
                    return fail("write data must be kSliceStatic(data, i*W, i*W+W-1)");
                }
                if (!constEquals(graph, wp.operands()[3], slang::SVInt::fromString("4'hF")))
                {
                    return fail("write mask must be the all-ones constant");
                }
                if (wp.operands()[4] != clk)
                {
                    return fail("write events must be forwarded verbatim");
                }
                if (getStringListAttr(wp, "eventEdge") != std::vector<std::string>{"posedge"})
                {
                    return fail("write eventEdge attr must be copied");
                }
                if (wp.attr("memoryWrite.priorityGroup").has_value() ||
                    wp.attr("memoryWrite.priority").has_value())
                {
                    return fail("expanded write ports must drop memoryWrite.priority*");
                }
            }
        }

        // row == 1 readall: replaced by the read port result directly.
        {
            const grh::ValueId r1 = graph.outputPortValue("out_r1");
            if (!isDefinedBy(graph, r1, grh::OperationKind::kMemoryReadPort))
            {
                return fail("row == 1 readall must become a single kMemoryReadPort");
            }
            const grh::Operation read = defOpOf(graph, r1);
            if (getStringAttr(read, "memSymbol") != "mem1" || read.operands().size() != 1 ||
                addrRowOf(graph, read.operands().front(), 1, 1) != 0)
            {
                return fail("row == 1 readall must read mem1 at constant address 0");
            }
        }

        // Constant memoization: 8 shared row addresses (mem) + 1 (mem1) +
        // 1 all-ones mask + 1 onehot '1' + 1 packed lane const.
        if (countOpsOfKind(graph, grh::OperationKind::kConstant) != 12)
        {
            return fail("equal constants must be memoized (expected 12 kConstant ops)");
        }

        // New ops are tagged with the pass srcLoc.
        {
            const grh::Operation orOp = defOpOf(graph, muxResult);
            const auto loc = orOp.srcLoc();
            if (!loc || loc->pass != "array-lower" || loc->note != "expand-mux-or")
            {
                return fail("expanded ops must carry srcLoc pass=array-lower");
            }
        }

        if (!roundTripJson(design))
        {
            return fail("store/load JSON round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // kArrayReduceLanes* -> per-lane kReduce*(kSliceStatic) + kConcat.
    // ------------------------------------------------------------------
    int testExpandReduceLanes()
    {
        constexpr int32_t kElemWidth = 3;
        constexpr int32_t kRows = 8;
        constexpr int32_t kPacked = kElemWidth * kRows;

        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId data = makeInput(graph, "data", kPacked);
        const grh::ValueId data1 = makeInput(graph, "data1", kElemWidth);

        const grh::ValueId redOr = makeArrayReduceLanes(
            graph, "lanes_or", grh::OperationKind::kArrayReduceLanesOr, data, kElemWidth, kRows);
        const grh::ValueId redAnd = makeArrayReduceLanes(
            graph, "lanes_and", grh::OperationKind::kArrayReduceLanesAnd, data, kElemWidth, kRows);
        const grh::ValueId redXor = makeArrayReduceLanes(
            graph, "lanes_xor", grh::OperationKind::kArrayReduceLanesXor, data, kElemWidth, kRows);
        const grh::ValueId redOne = makeArrayReduceLanes(
            graph, "lanes_one", grh::OperationKind::kArrayReduceLanesOr, data1, kElemWidth, 1);

        graph.bindOutputPort("out_lo", redOr);
        graph.bindOutputPort("out_la", redAnd);
        graph.bindOutputPort("out_lx", redXor);
        graph.bindOutputPort("out_l1", redOne);

        bool changed = false;
        if (const int rc = runPass(design, changed))
        {
            return rc;
        }
        if (!changed)
        {
            return fail("reduce-lanes: expected the pass to change the graph");
        }
        if (!noArrayOpsRemain(graph))
        {
            return fail("reduce-lanes: array ops must be fully expanded");
        }

        // Symbols move onto the replacement values.
        const grh::ValueId orResult = graph.findValue("lanes_or");
        const grh::ValueId andResult = graph.findValue("lanes_and");
        const grh::ValueId xorResult = graph.findValue("lanes_xor");
        if (!orResult.valid() || !andResult.valid() || !xorResult.valid())
        {
            return fail("reduce-lanes: result symbols must move onto the replacements");
        }

        // Each rows-bit result is one kConcat of the per-lane reduce bits;
        // concat operand j carries lane (rows-1-j) (lane 0 in the LSBs).
        const auto checkLanes = [&](grh::ValueId result, grh::OperationKind reduceKind,
                                    const char *message) -> int {
            if (!isDefinedBy(graph, result, grh::OperationKind::kConcat))
            {
                return fail(message);
            }
            const grh::Operation concat = defOpOf(graph, result);
            if (concat.operands().size() != static_cast<std::size_t>(kRows))
            {
                return fail(message);
            }
            for (int64_t lane = 0; lane < kRows; ++lane)
            {
                const grh::ValueId bit =
                    concat.operands()[static_cast<std::size_t>(kRows - 1 - lane)];
                if (!isDefinedBy(graph, bit, reduceKind))
                {
                    return fail(message);
                }
                const grh::Operation reduce = defOpOf(graph, bit);
                if (reduce.operands().size() != 1 ||
                    !isSliceOf(graph, reduce.operands().front(), data,
                               lane * kElemWidth, lane * kElemWidth + kElemWidth - 1))
                {
                    return fail(message);
                }
            }
            return 0;
        };
        if (const int rc = checkLanes(orResult, grh::OperationKind::kReduceOr,
                                      "kArrayReduceLanesOr must expand to per-lane kReduceOr + kConcat"))
        {
            return rc;
        }
        if (const int rc = checkLanes(andResult, grh::OperationKind::kReduceAnd,
                                      "kArrayReduceLanesAnd must expand to per-lane kReduceAnd + kConcat"))
        {
            return rc;
        }
        if (const int rc = checkLanes(xorResult, grh::OperationKind::kReduceXor,
                                      "kArrayReduceLanesXor must expand to per-lane kReduceXor + kConcat"))
        {
            return rc;
        }

        // row == 1: the single per-lane bit replaces the result directly.
        {
            const grh::ValueId one = graph.outputPortValue("out_l1");
            if (!isDefinedBy(graph, one, grh::OperationKind::kReduceOr))
            {
                return fail("reduce-lanes: row == 1 must become a single kReduceOr");
            }
            const grh::Operation reduce = defOpOf(graph, one);
            if (reduce.operands().size() != 1 ||
                !isSliceOf(graph, reduce.operands().front(), data1, 0, kElemWidth - 1))
            {
                return fail("reduce-lanes: row == 1 operand must be the full data slice");
            }
        }

        if (!roundTripJson(design))
        {
            return fail("reduce-lanes: store/load JSON round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Graphs without array ops pass through unchanged.
    // ------------------------------------------------------------------
    int testNoArrayOpsUnchanged()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("plain");
        design.markAsTop("plain");
        const grh::ValueId a = makeInput(graph, "a", 4);
        const grh::ValueId b = makeInput(graph, "b", 4);
        const grh::ValueId out = graph.createValue(4, false);
        const grh::OperationId andOp = graph.createOperation(grh::OperationKind::kAnd);
        graph.addOperand(andOp, a);
        graph.addOperand(andOp, b);
        graph.addResult(andOp, out);
        graph.bindOutputPort("out", out);

        const std::size_t opCountBefore = graph.operations().size();
        bool changed = true;
        if (const int rc = runPass(design, changed))
        {
            return rc;
        }
        if (changed)
        {
            return fail("graph without array ops must report unchanged");
        }
        if (graph.operations().size() != opCountBefore)
        {
            return fail("graph without array ops must keep its ops");
        }
        return 0;
    }

    int testPassRegistration()
    {
        bool listed = false;
        for (const std::string &name : availableTransformPasses())
        {
            if (name == "array-lower")
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            return fail("array-lower must be listed by availableTransformPasses");
        }
        std::string error;
        const std::vector<std::string_view> noArgs;
        std::unique_ptr<Pass> pass = makePass("array-lower", noArgs, error);
        if (!pass)
        {
            return fail("makePass must create array-lower: " + error);
        }
        const std::vector<std::string_view> badArgs = {"unexpected"};
        if (makePass("array-lower", badArgs, error))
        {
            return fail("array-lower must reject arguments");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int rc = testExpandAllArrayOps())
    {
        return rc;
    }
    if (const int rc = testExpandReduceLanes())
    {
        return rc;
    }
    if (const int rc = testNoArrayOpsUnchanged())
    {
        return rc;
    }
    if (const int rc = testPassRegistration())
    {
        return rc;
    }
    return 0;
}
