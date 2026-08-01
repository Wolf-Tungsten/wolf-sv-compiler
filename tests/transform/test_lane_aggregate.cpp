#include "core/grh.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "transform/lane_aggregate.hpp"

#include "slang/numeric/SVInt.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    namespace grh = wolvrix::lib::grh;

    int fail(const std::string &message)
    {
        std::cerr << "[lane-aggregate-tests] " << message << '\n';
        return 1;
    }

    // ------------------------------------------------------------------
    // Graph construction helpers.
    // ------------------------------------------------------------------
    grh::ValueId makeConst(grh::Graph &graph, int32_t width, uint64_t value)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kConstant, graph.makeInternalOpSym());
        graph.addResult(op, out);
        graph.setAttr(op, "constValue",
                      std::to_string(width) + "'d" + std::to_string(value));
        return out;
    }

    grh::ValueId makeInput(grh::Graph &graph, const std::string &name, int32_t width)
    {
        const grh::ValueId value = graph.createValue(graph.internSymbol(name), width, false);
        graph.bindInputPort(name, value);
        return value;
    }

    grh::OperationId makeRegister(grh::Graph &graph, const std::string &name, int32_t width)
    {
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol(name));
        graph.setAttr(op, "width", static_cast<int64_t>(width));
        graph.setAttr(op, "isSigned", false);
        return op;
    }

    grh::ValueId makeRead(grh::Graph &graph, const std::string &regName, int32_t width)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kRegisterReadPort, graph.makeInternalOpSym());
        graph.addResult(op, out);
        graph.setAttr(op, "regSymbol", regName);
        return out;
    }

    void makeWrite(grh::Graph &graph,
                   const std::string &regName,
                   grh::ValueId cond,
                   grh::ValueId data,
                   std::vector<grh::ValueId> events,
                   std::vector<std::string> edges)
    {
        const int32_t width = graph.getValue(data).width();
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kRegisterWritePort, graph.makeInternalOpSym());
        graph.addOperand(op, cond);
        graph.addOperand(op, data);
        slang::SVInt allOnes(static_cast<slang::bitwidth_t>(width), 0, false);
        allOnes = ~allOnes;
        const grh::ValueId mask = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId maskOp =
            graph.createOperation(grh::OperationKind::kConstant, graph.makeInternalOpSym());
        graph.addResult(maskOp, mask);
        graph.setAttr(maskOp, "constValue",
                      allOnes.toString(slang::LiteralBase::Hex, true,
                                       static_cast<slang::bitwidth_t>(width)));
        graph.addOperand(op, mask);
        for (const grh::ValueId event : events)
        {
            graph.addOperand(op, event);
        }
        graph.setAttr(op, "regSymbol", regName);
        graph.setAttr(op, "eventEdge", std::move(edges));
    }

    grh::ValueId makeUnary(grh::Graph &graph, grh::OperationKind kind, grh::ValueId operand, int32_t width)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
        graph.addOperand(op, operand);
        graph.addResult(op, out);
        return out;
    }

    grh::ValueId makeBinary(grh::Graph &graph, grh::OperationKind kind,
                            grh::ValueId lhs, grh::ValueId rhs, int32_t width)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, out);
        return out;
    }

    grh::ValueId makeMux(grh::Graph &graph, grh::ValueId sel, grh::ValueId whenTrue,
                         grh::ValueId whenFalse, int32_t width)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kMux, graph.makeInternalOpSym());
        graph.addOperand(op, sel);
        graph.addOperand(op, whenTrue);
        graph.addOperand(op, whenFalse);
        graph.addResult(op, out);
        return out;
    }

    // ------------------------------------------------------------------
    // Inspection helpers.
    // ------------------------------------------------------------------
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

    grh::OperationId findWideReadPort(const grh::Graph &graph, const std::string &regSymbol)
    {
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() == grh::OperationKind::kRegisterReadPort &&
                getStringAttr(op, "regSymbol") == regSymbol)
            {
                return opId;
            }
        }
        return grh::OperationId::invalid();
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

    struct LaneGroup
    {
        std::vector<std::string> names;
        std::vector<grh::ValueId> reads;
    };

    // Builds one basic isomorphic lane group:
    //   data_i = mux(en1, (enW & self_i) ^ const(i), self_i)
    //   cond_i = en1
    LaneGroup buildBasicGroup(grh::Graph &graph,
                              const std::string &prefix,
                              const std::string &suffix,
                              const std::vector<uint64_t> &indices,
                              int32_t width,
                              grh::ValueId en1,
                              grh::ValueId enW,
                              grh::ValueId clk)
    {
        LaneGroup group;
        for (const uint64_t idx : indices)
        {
            const std::string name =
                prefix + "_" + std::to_string(idx) + suffix;
            group.names.push_back(name);
            makeRegister(graph, name, width);
            const grh::ValueId self = makeRead(graph, name, width);
            group.reads.push_back(self);
            const grh::ValueId masked = makeBinary(graph, grh::OperationKind::kAnd, enW, self, width);
            const grh::ValueId laneConst = makeConst(graph, width, idx);
            const grh::ValueId xored = makeBinary(graph, grh::OperationKind::kXor, masked, laneConst, width);
            const grh::ValueId data = makeMux(graph, en1, xored, self, width);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }
        return group;
    }

    int runPass(grh::Design &design, LaneAggregateOptions options = {})
    {
        PassManager manager;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("lane-aggregate pass run failed");
        }
        return res.changed ? 2 : 0;
    }

    // ------------------------------------------------------------------
    // (a)+(g): 8-lane isomorphic group merges (affine const, self read,
    // shared leaves, kMux); read side becomes slices with lane 0 in LSB.
    // ------------------------------------------------------------------
    int testMergeBasicGroup()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
        for (std::size_t i = 0; i < group.reads.size(); ++i)
        {
            graph.bindOutputPort("out_" + std::to_string(i), group.reads[i]);
        }

        LaneAggregateOptions options;
        options.outputKey = "la.report";
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("basic group: pass run failed");
        }
        if (!res.changed)
        {
            return fail("basic group: expected changes");
        }

        const std::string wideName = "lane_q__laneagg";
        const grh::OperationId wideReg = graph.findOperation(wideName);
        if (!wideReg.valid())
        {
            return fail("basic group: wide register missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 32)
        {
            return fail("basic group: wide register width must be 32");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 1)
        {
            return fail("basic group: expected exactly one kRegister after merge");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegisterWritePort) != 1)
        {
            return fail("basic group: expected exactly one kRegisterWritePort after merge");
        }
        for (const std::string &name : group.names)
        {
            if (graph.findOperation(name).valid())
            {
                return fail("basic group: lane register must be erased: " + name);
            }
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegisterReadPort) != 1)
        {
            return fail("basic group: expected exactly one (wide) kRegisterReadPort");
        }
        const grh::OperationId wideRead = findWideReadPort(graph, wideName);
        if (!wideRead.valid())
        {
            return fail("basic group: wide read port missing");
        }
        const grh::ValueId wideReadValue = graph.getOperation(wideRead).results().front();

        // Write port shape: [updateCond, nextValue, mask, clk], posedge.
        for (const auto opId : graph.operations())
        {
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kRegisterWritePort)
            {
                continue;
            }
            if (getStringAttr(op, "regSymbol") != wideName)
            {
                return fail("basic group: write port targets the wide register");
            }
            const auto operands = op.operands();
            if (operands.size() != 4 || operands[3] != clk)
            {
                return fail("basic group: write port must keep the clk event");
            }
            const auto edges = op.attr("eventEdge");
            const auto *edgeList = edges ? std::get_if<std::vector<std::string>>(&*edges) : nullptr;
            if (!edgeList || edgeList->size() != 1 || edgeList->front() != "posedge")
            {
                return fail("basic group: write port must keep eventEdge=[posedge]");
            }
            const grh::OperationId condDef = graph.getValue(operands[0]).definingOp();
            if (!condDef.valid() || graph.getOperation(condDef).kind() != grh::OperationKind::kReduceOr)
            {
                return fail("basic group: updateCond must be a kReduceOr");
            }
            const grh::OperationId dataDef = graph.getValue(operands[1]).definingOp();
            if (!dataDef.valid() || graph.getOperation(dataDef).kind() != grh::OperationKind::kOr)
            {
                return fail("basic group: nextValue must be an kOr of masked terms");
            }
        }

        // (g): every lane read became kSliceStatic(wide, i*W +: W), lane 0 LSB.
        for (std::size_t i = 0; i < group.reads.size(); ++i)
        {
            const grh::ValueId outValue = graph.outputPortValue("out_" + std::to_string(i));
            if (!outValue.valid())
            {
                return fail("basic group: output port missing after rewrite");
            }
            const grh::OperationId sliceDef = graph.getValue(outValue).definingOp();
            if (!sliceDef.valid() ||
                graph.getOperation(sliceDef).kind() != grh::OperationKind::kSliceStatic)
            {
                return fail("basic group: lane read must become kSliceStatic");
            }
            const grh::Operation slice = graph.getOperation(sliceDef);
            const auto sliceStart = getIntAttr(slice, "sliceStart");
            const auto sliceEnd = getIntAttr(slice, "sliceEnd");
            if (!sliceStart || !sliceEnd ||
                *sliceStart != static_cast<int64_t>(i * 4) ||
                *sliceEnd != static_cast<int64_t>(i * 4 + 3))
            {
                return fail("basic group: slice bit range mismatch for lane " + std::to_string(i));
            }
            if (slice.operands().size() != 1 || slice.operands().front() != wideReadValue)
            {
                return fail("basic group: slice base must be the wide read value");
            }
        }

        // Report session value.
        const auto reportIt = session.find("la.report");
        if (reportIt == session.end() || !reportIt->second ||
            reportIt->second->kind() != "lane-aggregate.reports")
        {
            return fail("basic group: missing lane-aggregate.reports session value");
        }
        const auto *reportValue =
            dynamic_cast<const SessionSlotValue<std::string> *>(reportIt->second.get());
        if (!reportValue || reportValue->value.find("\"outcome\":\"merged\"") == std::string::npos)
        {
            return fail("basic group: report must record a merged outcome");
        }

        if (!roundTripJson(design))
        {
            return fail("basic group: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (b): lane 0 reset specialization -> majority merges, lane 0 stays.
    // ------------------------------------------------------------------
    int testLaneZeroSpecialization()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const grh::ValueId rst = makeInput(graph, "rst", 1);

        // Lane 0: data = mux(rst, 0, body) -> different signature.
        makeRegister(graph, "lane_0_q", 4);
        const grh::ValueId self0 = makeRead(graph, "lane_0_q", 4);
        const grh::ValueId body0 =
            makeBinary(graph, grh::OperationKind::kXor,
                       makeBinary(graph, grh::OperationKind::kAnd, enW, self0, 4),
                       makeConst(graph, 4, 0), 4);
        const grh::ValueId data0 = makeMux(graph, rst, makeConst(graph, 4, 0), body0, 4);
        makeWrite(graph, "lane_0_q", en1, data0, {clk}, {"posedge"});

        const std::vector<uint64_t> indices{1, 2, 3, 4, 5, 6, 7, 8};
        const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("lane-0 specialization: expected changes");
        }

        const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
        if (!wideReg.valid())
        {
            return fail("lane-0 specialization: wide register missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 36)
        {
            return fail("lane-0 specialization: wide register width must be span(9)*4=36");
        }
        if (!graph.findOperation("lane_0_q").valid())
        {
            return fail("lane-0 specialization: specialized lane register must survive");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 2)
        {
            return fail("lane-0 specialization: expected wide register + lane 0 scalar");
        }
        for (const std::string &name : group.names)
        {
            if (graph.findOperation(name).valid())
            {
                return fail("lane-0 specialization: merged lane register must be erased: " + name);
            }
        }
        // Lane 0 keeps its plain read port (not rewritten to a slice).
        const grh::OperationId lane0Read = findWideReadPort(graph, "lane_0_q");
        if (!lane0Read.valid())
        {
            return fail("lane-0 specialization: lane 0 read port must survive");
        }
        if (!roundTripJson(design))
        {
            return fail("lane-0 specialization: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (c): mixed lane-varying leaves (some defined, some not) still
    // reject. Per-lane bare wires alone are a lane-parameter leaf (legal).
    // ------------------------------------------------------------------
    int testSharedLeafMismatchRejected()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            // Even lanes AND with a private input (no defining op), odd lanes
            // AND with a private constant: mixed leaf kinds cannot merge.
            const grh::ValueId sel = idx % 2 == 0
                                         ? makeInput(graph, "sel_" + std::to_string(idx), 4)
                                         : makeConst(graph, 4, idx);
            const grh::ValueId data = makeBinary(graph, grh::OperationKind::kAnd, sel, self, 4);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design); rc != 0)
        {
            return rc == 1 ? rc : fail("mixed-leaf mismatch: pass must not change the graph");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
        {
            return fail("mixed-leaf mismatch: all lane registers must survive");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (d): non-affine per-lane constants -> rejected by the exact check.
    // ------------------------------------------------------------------
    int testNonAffineConstantsRejected()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            // c_i = i*i: identical structure, but not affine in the index.
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kXor, self, makeConst(graph, 4, idx * idx), 4);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        LaneAggregateOptions options;
        options.outputKey = "la.report";
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("non-affine: pass run failed");
        }
        if (res.changed)
        {
            return fail("non-affine: pass must not merge non-affine constants");
        }
        const auto reportIt = session.find("la.report");
        const auto *reportValue =
            reportIt == session.end()
                ? nullptr
                : dynamic_cast<const SessionSlotValue<std::string> *>(reportIt->second.get());
        if (!reportValue ||
            reportValue->value.find("non_affine_constant") == std::string::npos)
        {
            return fail("non-affine: report must record non_affine_constant");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (e): cross-group coupling. A reads B's lanes at the same index.
    // Both merge when B merges; A is rejected when B cannot merge.
    // ------------------------------------------------------------------
    int testCrossGroupCoupling()
    {
        {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string bName = "b_" + std::to_string(idx) + "_q";
                const std::string aName = "a_" + std::to_string(idx) + "_q";
                makeRegister(graph, bName, 4);
                const grh::ValueId bSelf = makeRead(graph, bName, 4);
                const grh::ValueId bData =
                    makeBinary(graph, grh::OperationKind::kAnd, enW, bSelf, 4);
                makeWrite(graph, bName, en1, bData, {clk}, {"posedge"});

                makeRegister(graph, aName, 4);
                const grh::ValueId aSelf = makeRead(graph, aName, 4);
                (void)aSelf;
                // A's data reads sibling group B at the same lane index.
                const grh::ValueId aData =
                    makeBinary(graph, grh::OperationKind::kXor, bSelf,
                                   makeConst(graph, 4, idx), 4);
                makeWrite(graph, aName, en1, aData, {clk}, {"posedge"});
            }

            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            else if (rc == 0)
            {
                return fail("coupling: expected both groups to merge");
            }
            if (!graph.findOperation("a_q__laneagg").valid() ||
                !graph.findOperation("b_q__laneagg").valid())
            {
                return fail("coupling: both wide registers must exist");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 2 ||
                countOpsOfKind(graph, grh::OperationKind::kRegisterWritePort) != 2)
            {
                return fail("coupling: expected exactly two wide registers + write ports");
            }
            if (!roundTripJson(design))
            {
                return fail("coupling: store/load round trip failed");
            }
        }
        {
            // B cannot merge (kAdd is not lane-pointwise) -> A must be rejected.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string bName = "b_" + std::to_string(idx) + "_q";
                const std::string aName = "a_" + std::to_string(idx) + "_q";
                makeRegister(graph, bName, 4);
                const grh::ValueId bSelf = makeRead(graph, bName, 4);
                const grh::ValueId bData =
                    makeBinary(graph, grh::OperationKind::kAdd, enW, bSelf, 4);
                makeWrite(graph, bName, en1, bData, {clk}, {"posedge"});

                makeRegister(graph, aName, 4);
                makeRead(graph, aName, 4);
                const grh::ValueId aData =
                    makeBinary(graph, grh::OperationKind::kXor, bSelf,
                               makeConst(graph, 4, idx), 4);
                makeWrite(graph, aName, en1, aData, {clk}, {"posedge"});
            }

            if (const int rc = runPass(design); rc != 0)
            {
                return rc == 1 ? rc
                               : fail("coupling-reject: pass must not merge when sibling cannot");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 16)
            {
                return fail("coupling-reject: all lane registers must survive");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (f): group-level rejects: width mismatch / sparse indices.
    // ------------------------------------------------------------------
    int testGroupLevelRejects()
    {
        {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const int32_t width = idx == 5 ? 8 : 4;
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, width);
                const grh::ValueId self = makeRead(graph, name, width);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd,
                               width == 4 ? enW : makeInput(graph, "enW8", 8), self, width);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            LaneAggregateOptions options;
            options.outputKey = "la.report";
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<LaneAggregatePass>(options));
            PassDiagnostics diags;
            const PassManagerResult res = manager.run(design, diags);
            if (!res.success || diags.hasError() || res.changed)
            {
                return fail("width-mismatch: pass must reject the group");
            }
            const auto reportIt = session.find("la.report");
            if (reportIt == session.end() || !reportIt->second)
            {
                return fail("width-mismatch: missing report session value");
            }
            const auto *reportValue = dynamic_cast<const SessionSlotValue<std::string> *>(
                reportIt->second.get());
            if (!reportValue || reportValue->value.find("width_mismatch") == std::string::npos)
            {
                return fail("width-mismatch: report must record width_mismatch");
            }
        }
        {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 8);
            const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 50};
            buildBasicGroup(graph, "lane", "_q", indices, 8, en1, enW, clk);
            LaneAggregateOptions options;
            options.outputKey = "la.report";
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<LaneAggregatePass>(options));
            PassDiagnostics diags;
            const PassManagerResult res = manager.run(design, diags);
            if (!res.success || diags.hasError() || res.changed)
            {
                return fail("not-dense: pass must reject the sparse group");
            }
            const auto reportIt = session.find("la.report");
            if (reportIt == session.end() || !reportIt->second)
            {
                return fail("not-dense: missing report session value");
            }
            const auto *reportValue = dynamic_cast<const SessionSlotValue<std::string> *>(
                reportIt->second.get());
            if (!reportValue || reportValue->value.find("not_dense") == std::string::npos)
            {
                return fail("not-dense: report must record not_dense");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (h): write-port event mismatch -> lane excluded, majority merges.
    // ------------------------------------------------------------------
    int testEventMismatchLaneExcluded()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId rst = makeInput(graph, "rst", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        for (uint64_t idx = 0; idx < 9; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
            if (idx == 2)
            {
                makeWrite(graph, name, en1, data, {clk, rst}, {"posedge", "negedge"});
            }
            else
            {
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("event-mismatch: expected the 8 consistent lanes to merge");
        }
        if (!graph.findOperation("lane_2_q").valid())
        {
            return fail("event-mismatch: specialized lane register must survive");
        }
        const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
        if (!wideReg.valid())
        {
            return fail("event-mismatch: wide register missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 36)
        {
            return fail("event-mismatch: wide register width must be span(9)*4=36");
        }
        if (!roundTripJson(design))
        {
            return fail("event-mismatch: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Grouping rule: masked segments + varying-segment detection.
    // (a) constant module-instance segment + varying port segment groups.
    // ------------------------------------------------------------------
    int testConstantSegmentGrouping()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        // enqPtrVec style: first segment constant (module instance 0),
        // second segment is the lane index 0..7.
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        buildBasicGroup(graph, "top_0_enqPtrVec", "_value", indices, 4, en1, enW, clk);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("masked-grouping: expected the enqPtrVec-style group to merge");
        }
        const grh::OperationId wideReg = graph.findOperation("top_0_enqPtrVec_value__laneagg");
        if (!wideReg.valid())
        {
            return fail("masked-grouping: wide register top_0_enqPtrVec_value__laneagg missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 32)
        {
            return fail("masked-grouping: wide register width must be 8*4=32");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 1)
        {
            return fail("masked-grouping: expected all lane registers merged");
        }
        if (!roundTripJson(design))
        {
            return fail("masked-grouping: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (b) `_T_2`-style constant tail segment must not become the index.
    // ------------------------------------------------------------------
    int testConstantTailSegmentGrouping()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        // lane_<i>_q_T_2: second numeric segment constant (Chisel temp id 2).
        buildBasicGroup(graph, "lane", "_q_T_2", indices, 4, en1, enW, clk);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("tail-segment: expected the _T_2-style group to merge");
        }
        const grh::OperationId wideReg = graph.findOperation("lane_q_T_2__laneagg");
        if (!wideReg.valid())
        {
            return fail("tail-segment: wide register lane_q_T_2__laneagg missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 32)
        {
            return fail("tail-segment: wide register width must be 8*4=32");
        }
        if (!roundTripJson(design))
        {
            return fail("tail-segment: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (c) two varying segments with no decomposable slice (diagonal
    // matrix) -> multi_varying_segment.
    // ------------------------------------------------------------------
    int testMultiVaryingSegmentRejected()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        // Diagonal: arr_0_0, arr_1_1, ... — every constant-segment slice
        // has a single member, so no lane group can be formed.
        for (uint64_t i = 0; i < 8; ++i)
        {
            const std::string name = "arr_" + std::to_string(i) + "_" + std::to_string(i);
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        LaneAggregateOptions options;
        options.outputKey = "la.report";
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError() || res.changed)
        {
            return fail("multi-varying: pass must reject the diagonal 2-D group");
        }
        const auto reportIt = session.find("la.report");
        if (reportIt == session.end() || !reportIt->second)
        {
            return fail("multi-varying: missing report session value");
        }
        const auto *reportValue = dynamic_cast<const SessionSlotValue<std::string> *>(
            reportIt->second.get());
        if (!reportValue ||
            reportValue->value.find("multi_varying_segment") == std::string::npos)
        {
            return fail("multi-varying: report must record multi_varying_segment");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
        {
            return fail("multi-varying: all lane registers must survive");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (c') a full 2-D block decomposes into per-slice lane groups that
    // merge independently (constant-segment sub-grouping).
    // ------------------------------------------------------------------
    int testTwoDimensionalSubGrouping()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        for (uint64_t i = 0; i < 8; ++i)
        {
            for (uint64_t j = 0; j < 2; ++j)
            {
                const std::string name =
                    "arr_" + std::to_string(i) + "_" + std::to_string(j);
                makeRegister(graph, name, 4);
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("2d-subgroup: expected both constant-segment slices to merge");
        }
        // Wide names derive from the specialized keys `arr_*_0` / `arr_*_1`.
        const bool slice0 = graph.findOperation("arr_0__laneagg").valid();
        const bool slice1 = graph.findOperation("arr_1__laneagg").valid();
        if (!slice0 || !slice1)
        {
            return fail("2d-subgroup: expected wide registers arr_0__laneagg and arr_1__laneagg");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 2)
        {
            return fail("2d-subgroup: expected exactly two wide registers");
        }
        if (!roundTripJson(design))
        {
            return fail("2d-subgroup: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (d) enqRob_req-style sibling groups (valid + bits) merge together.
    // ------------------------------------------------------------------
    int testSiblingGroupingEnqRob()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string validName = "enqRob_req_" + std::to_string(idx) + "_valid";
            const std::string bitsName = "enqRob_req_" + std::to_string(idx) + "_bits_r_firstUop";
            makeRegister(graph, validName, 4);
            const grh::ValueId validSelf = makeRead(graph, validName, 4);
            const grh::ValueId validData =
                makeBinary(graph, grh::OperationKind::kAnd, enW, validSelf, 4);
            makeWrite(graph, validName, en1, validData, {clk}, {"posedge"});

            makeRegister(graph, bitsName, 4);
            makeRead(graph, bitsName, 4);
            // bits lane i reads sibling valid lane i, affine constant c_i = i.
            const grh::ValueId bitsData =
                makeBinary(graph, grh::OperationKind::kXor, validSelf,
                           makeConst(graph, 4, idx), 4);
            makeWrite(graph, bitsName, en1, bitsData, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("enqRob-sibling: expected both groups to merge");
        }
        if (!graph.findOperation("enqRob_req_valid__laneagg").valid() ||
            !graph.findOperation("enqRob_req_bits_r_firstUop__laneagg").valid())
        {
            return fail("enqRob-sibling: both wide registers must exist");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 2)
        {
            return fail("enqRob-sibling: expected exactly two wide registers");
        }
        if (!roundTripJson(design))
        {
            return fail("enqRob-sibling: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (a) shared dispatch-port reads: every lane reads the same registers
    // of another lane group (absolute-index sharing). Lanes whose own
    // index coincides with the port index hash differently (same-index
    // sibling marker) and stay scalar; the majority merges.
    // ------------------------------------------------------------------
    int testSharedDispatchPortRead()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const std::vector<uint64_t> dpIndices{0, 1, 2, 3, 4, 5, 6, 7};
        buildBasicGroup(graph, "dp", "_q", dpIndices, 4, en1, enW, clk);
        for (uint64_t idx = 0; idx < 10; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            // Every lane reads the same two dispatch-port registers.
            const grh::ValueId port0 = makeRead(graph, "dp_0_q", 4);
            const grh::ValueId port3 = makeRead(graph, "dp_3_q", 4);
            const grh::ValueId both =
                makeBinary(graph, grh::OperationKind::kAnd, port0, port3, 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kXor, both, self, 4);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("dispatch-read: expected both groups to merge");
        }
        if (!graph.findOperation("dp_q__laneagg").valid() ||
            !graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("dispatch-read: both wide registers must exist");
        }
        const auto wideWidth =
            getIntAttr(graph.getOperation(graph.findOperation("lane_q__laneagg")), "width");
        if (!wideWidth || *wideWidth != 40)
        {
            return fail("dispatch-read: lane wide register width must be span(10)*4=40");
        }
        // Boundary lanes (own index == port index) hash differently but are
        // structurally identical: the minority-lane rescue merges them too.
        if (graph.findOperation("lane_0_q").valid() || graph.findOperation("lane_3_q").valid())
        {
            return fail("dispatch-read: boundary lanes must be rescued into the merge");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 2)
        {
            return fail("dispatch-read: expected exactly two wide registers");
        }
        if (!roundTripJson(design))
        {
            return fail("dispatch-read: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (b) reduce(concat) is normalized into an element tree before
    // signature bucketing, so packed reductions no longer block merging.
    // ------------------------------------------------------------------
    grh::ValueId makeConcat(grh::Graph &graph, std::vector<grh::ValueId> operands, int32_t width)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kConcat, graph.makeInternalOpSym());
        for (const grh::ValueId operand : operands)
        {
            graph.addOperand(op, operand);
        }
        graph.addResult(op, out);
        return out;
    }

    int testReduceConcatNormalized()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const grh::ValueId a = makeInput(graph, "a", 1);
        const grh::ValueId b = makeInput(graph, "b", 1);
        const grh::ValueId c = makeInput(graph, "c", 1);
        const grh::ValueId packed = makeConcat(graph, {a, b, c}, 3);
        const grh::ValueId reduced =
            makeUnary(graph, grh::OperationKind::kReduceOr, packed, 1);
        const grh::ValueId cond = makeBinary(graph, grh::OperationKind::kOr, en1, reduced, 1);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        for (const uint64_t idx : indices)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
            makeWrite(graph, name, cond, data, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("reduce-concat: expected the group to merge after normalization");
        }
        if (!graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("reduce-concat: wide register missing");
        }
        // The packed reduce must be gone from live logic: no kReduceOr may
        // consume a kConcat (the wide write port's own kReduceOr stays).
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kReduceOr || op.operands().size() != 1)
            {
                continue;
            }
            const grh::OperationId defId = graph.getValue(op.operands().front()).definingOp();
            if (defId.valid() && graph.getOperation(defId).kind() == grh::OperationKind::kConcat &&
                graph.getValue(op.results().front()).users().size() != 0)
            {
                return fail("reduce-concat: packed reduce(concat) must be normalized");
            }
        }
        if (!roundTripJson(design))
        {
            return fail("reduce-concat: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (c) eq-onehot: kEq(ptr, i) materializes as kShl(span'd1, ptr).
    // ------------------------------------------------------------------
    int testEqOnehot()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId ptr = makeInput(graph, "ptr", 3);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            (void)self;
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kEq, ptr, makeConst(graph, 3, idx), 1);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("eq-onehot: expected the group to merge");
        }
        const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
        if (!wideReg.valid())
        {
            return fail("eq-onehot: wide register missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 8)
        {
            return fail("eq-onehot: wide register width must be 8*1=8");
        }
        // Exactly one kShl with operands [span'd1 constant, ptr].
        std::size_t shlCount = 0;
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kShl)
            {
                continue;
            }
            ++shlCount;
            const auto operands = op.operands();
            if (operands.size() != 2 || operands[1] != ptr)
            {
                return fail("eq-onehot: kShl shift amount must be the shared ptr");
            }
            const grh::OperationId oneDef = graph.getValue(operands[0]).definingOp();
            if (!oneDef.valid() || graph.getOperation(oneDef).kind() != grh::OperationKind::kConstant ||
                graph.getValue(operands[0]).width() != 8)
            {
                return fail("eq-onehot: kShl value must be an 8-bit constant 1");
            }
            if (graph.getValue(op.results().front()).width() != 8)
            {
                return fail("eq-onehot: kShl result must be 8 bits wide");
            }
        }
        if (shlCount != 1)
        {
            return fail("eq-onehot: expected exactly one kShl");
        }
        if (!roundTripJson(design))
        {
            return fail("eq-onehot: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (d) lane-parameter leaves: per-lane bare wires / undeclared register
    // reads materialize as a per-lane kConcat.
    // ------------------------------------------------------------------
    int testLaneParamLeaf()
    {
        {
            // Per-lane bare wires (no defining op).
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            std::vector<grh::ValueId> wires;
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                wires.push_back(makeInput(graph, "w_" + std::to_string(idx), 4));
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd, wires.back(), self, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }

            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            else if (rc == 0)
            {
                return fail("lane-param-wire: expected the group to merge");
            }
            if (!graph.findOperation("lane_q__laneagg").valid())
            {
                return fail("lane-param-wire: wide register missing");
            }
            // The lane-parameter concat: operands are w_7..w_0 (MSB first).
            bool foundConcat = false;
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const grh::Operation op = graph.getOperation(opId);
                if (op.kind() != grh::OperationKind::kConcat || op.operands().size() != 8)
                {
                    continue;
                }
                bool orderOk = true;
                for (std::size_t k = 0; k < 8; ++k)
                {
                    if (op.operands()[k] != wires[7 - k])
                    {
                        orderOk = false;
                        break;
                    }
                }
                if (orderOk)
                {
                    foundConcat = true;
                    break;
                }
            }
            if (!foundConcat)
            {
                return fail("lane-param-wire: expected per-lane kConcat in lane order");
            }
            if (!roundTripJson(design))
            {
                return fail("lane-param-wire: store/load round trip failed");
            }
        }
        {
            // Per-lane reads of undeclared registers (no kRegister ops).
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId ext =
                    makeRead(graph, "ext_" + std::to_string(idx) + "_x", 4);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kXor, ext, self, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }

            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            else if (rc == 0)
            {
                return fail("lane-param-extread: expected the group to merge");
            }
            if (!graph.findOperation("lane_q__laneagg").valid())
            {
                return fail("lane-param-extread: wide register missing");
            }
            if (!roundTripJson(design))
            {
                return fail("lane-param-extread: store/load round trip failed");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (e') 1-bit kLogicAnd/kLogicOr/kLogicNot widen as kAnd/kOr/kNot.
    // ------------------------------------------------------------------
    int testLogicOpsWiden()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId zero1 = makeConst(graph, 1, 0);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kLogicAnd, self, en1, 1);
            const grh::ValueId cond =
                makeBinary(graph, grh::OperationKind::kLogicOr, en1, zero1, 1);
            makeWrite(graph, name, cond, data, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("logic-ops: expected the group to merge");
        }
        if (!graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("logic-ops: wide register missing");
        }
        // No kLogic* op may remain live: the widened forms are kAnd/kOr/kNot.
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if ((op.kind() == grh::OperationKind::kLogicAnd ||
                 op.kind() == grh::OperationKind::kLogicOr ||
                 op.kind() == grh::OperationKind::kLogicNot) &&
                op.results().size() == 1 && !graph.getValue(op.results().front()).users().empty())
            {
                return fail("logic-ops: live kLogic* op must not remain after widening");
            }
        }
        if (!roundTripJson(design))
        {
            return fail("logic-ops: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (e) kAdd stays rejected even with affine constants.
    // ------------------------------------------------------------------
    int testAddRejected()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            makeRead(graph, name, 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAdd, enW, makeConst(graph, 4, idx), 4);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        LaneAggregateOptions options;
        options.outputKey = "la.report";
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError() || res.changed)
        {
            return fail("kadd-reject: pass must not merge kAdd cones");
        }
        const auto reportIt = session.find("la.report");
        if (reportIt == session.end() || !reportIt->second)
        {
            return fail("kadd-reject: missing report session value");
        }
        const auto *reportValue = dynamic_cast<const SessionSlotValue<std::string> *>(
            reportIt->second.get());
        if (!reportValue || reportValue->value.find("unsupported_op") == std::string::npos)
        {
            return fail("kadd-reject: report must record unsupported_op");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Phase 2 (read side): select trees over lane slices -> kSliceDynamic.
    // ------------------------------------------------------------------
    grh::ValueId makeEqConst(grh::Graph &graph, grh::ValueId ptr, uint64_t constant, int32_t ptrWidth)
    {
        return makeBinary(graph, grh::OperationKind::kEq, ptr,
                          makeConst(graph, ptrWidth, constant), 1);
    }

    grh::ValueId makeReplicate(grh::Graph &graph, grh::ValueId value, int64_t rep, int32_t outWidth)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), outWidth, false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kReplicate, graph.makeInternalOpSym());
        graph.addOperand(op, value);
        graph.addResult(op, out);
        graph.setAttr(op, "rep", rep);
        return out;
    }

    // Builds a mux chain over per-lane reads in the given constant order,
    // ending in a zero constant default.
    grh::ValueId makeMuxChain(grh::Graph &graph,
                              grh::ValueId ptr,
                              int32_t ptrWidth,
                              const std::vector<uint64_t> &order,
                              const std::vector<grh::ValueId> &laneReads,
                              int32_t width,
                              grh::ValueId defaultValue)
    {
        grh::ValueId tail = defaultValue;
        for (std::size_t k = order.size(); k-- > 0;)
        {
            const grh::ValueId sel = makeEqConst(graph, ptr, order[k], ptrWidth);
            tail = makeMux(graph, sel, laneReads[order[k]], tail, width);
        }
        return tail;
    }

    bool hasSliceDynamicOf(const grh::Graph &graph, grh::ValueId base)
    {
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() == grh::OperationKind::kSliceDynamic &&
                op.operands().size() == 2 && op.operands()[0] == base)
            {
                return true;
            }
        }
        return false;
    }

    std::size_t countOpsOfKind2(const grh::Graph &graph, grh::OperationKind kind)
    {
        return countOpsOfKind(graph, kind);
    }

    // (a)+(f): phase 1 merges the group, phase 2 rewrites the mux chain.
    int testReadSelectMuxChain()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const grh::ValueId ptr = makeInput(graph, "ptr", 3);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
        std::vector<grh::ValueId> treeReads;
        for (const auto &name : group.names)
        {
            treeReads.push_back(makeRead(graph, name, 4));
        }
        const grh::ValueId chain =
            makeMuxChain(graph, ptr, 3, {0, 1, 2, 3, 4, 5, 6, 7}, treeReads, 4,
                         makeConst(graph, 4, 0));
        graph.bindOutputPort("sel_out", chain);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("read-select-mux: expected changes");
        }
        // Phase 1 merged the group.
        const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
        if (!wideReg.valid())
        {
            return fail("read-select-mux: wide register missing");
        }
        for (const std::string &name : group.names)
        {
            if (graph.findOperation(name).valid())
            {
                return fail("read-select-mux: lane register must be erased: " + name);
            }
        }
        // Phase 2 replaced the chain with one kSliceDynamic of the wide read.
        const grh::OperationId wideRead = findWideReadPort(graph, "lane_q__laneagg");
        if (!wideRead.valid())
        {
            return fail("read-select-mux: wide read port missing");
        }
        const grh::ValueId wideReadValue = graph.getOperation(wideRead).results().front();
        const grh::ValueId outValue = graph.outputPortValue("sel_out");
        if (!outValue.valid())
        {
            return fail("read-select-mux: output missing");
        }
        const grh::OperationId outDef = graph.getValue(outValue).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kSliceDynamic)
        {
            return fail("read-select-mux: output must be a kSliceDynamic");
        }
        const grh::Operation sliceOp = graph.getOperation(outDef);
        const auto sliceWidth = getIntAttr(sliceOp, "sliceWidth");
        if (!sliceWidth || *sliceWidth != 4)
        {
            return fail("read-select-mux: sliceWidth must be 4");
        }
        if (sliceOp.operands()[0] != wideReadValue)
        {
            return fail("read-select-mux: kSliceDynamic base must be the wide read");
        }
        // offset = ptr << 2 for W=4.
        const grh::OperationId offsetDef = graph.getValue(sliceOp.operands()[1]).definingOp();
        if (!offsetDef.valid() || graph.getOperation(offsetDef).kind() != grh::OperationKind::kShl ||
            graph.getOperation(offsetDef).operands().front() != ptr)
        {
            return fail("read-select-mux: offset must be ptr << 2");
        }
        if (!roundTripJson(design))
        {
            return fail("read-select-mux: store/load round trip failed");
        }
        return 0;
    }

    // (b) and/or onehot tree.
    int testReadSelectOrTree()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const grh::ValueId ptr = makeInput(graph, "ptr", 3);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
        grh::ValueId tree;
        for (const uint64_t idx : indices)
        {
            const grh::ValueId laneRead = makeRead(graph, group.names[idx], 4);
            const grh::ValueId sel = makeEqConst(graph, ptr, idx, 3);
            const grh::ValueId mask = makeReplicate(graph, sel, 4, 4);
            const grh::ValueId term =
                makeBinary(graph, grh::OperationKind::kAnd, mask, laneRead, 4);
            tree = tree.valid() ? makeBinary(graph, grh::OperationKind::kOr, tree, term, 4) : term;
        }
        graph.bindOutputPort("sel_out", tree);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("read-select-or: expected changes");
        }
        const grh::ValueId outValue = graph.outputPortValue("sel_out");
        const grh::OperationId outDef = graph.getValue(outValue).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kSliceDynamic)
        {
            return fail("read-select-or: output must be a kSliceDynamic");
        }
        if (!roundTripJson(design))
        {
            return fail("read-select-or: store/load round trip failed");
        }
        return 0;
    }

    // (b') onehot-bit tree at W == 1: bare kAnd(kEq(ptr, i), lane_i) terms,
    // the walkPtrOH-style form from the XiangShan rename buffer.
    int testReadSelectBareOnehot()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 1);
        const grh::ValueId ptr = makeInput(graph, "ptr", 3);
        // 1-bit lane group with a trivial isomorphic cone (no constants).
        std::vector<std::string> names;
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            names.push_back(name);
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAnd, enW, self, 1);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }
        // Shared onehot select bits: oh_i = kEq(ptr, i), each used once here
        // but shared across many field trees on real designs.
        grh::ValueId tree;
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const grh::ValueId laneRead = makeRead(graph, names[idx], 1);
            const grh::ValueId sel = makeEqConst(graph, ptr, idx, 3);
            const grh::ValueId term =
                makeBinary(graph, grh::OperationKind::kAnd, sel, laneRead, 1);
            tree = tree.valid() ? makeBinary(graph, grh::OperationKind::kOr, tree, term, 1) : term;
        }
        graph.bindOutputPort("sel_out", tree);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("read-select-bare-onehot: expected changes");
        }
        const grh::OperationId outDef =
            graph.getValue(graph.outputPortValue("sel_out")).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kSliceDynamic)
        {
            return fail("read-select-bare-onehot: output must be a kSliceDynamic");
        }
        // W == 1: the offset is ptr itself (no scaling op).
        const grh::Operation sliceOp = graph.getOperation(outDef);
        if (sliceOp.operands()[1] != ptr)
        {
            return fail("read-select-bare-onehot: offset must be ptr directly");
        }
        if (!roundTripJson(design))
        {
            return fail("read-select-bare-onehot: store/load round trip failed");
        }
        return 0;
    }

    // (c) out-of-order eq constants still convert.
    int testReadSelectOutOfOrder()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const grh::ValueId ptr = makeInput(graph, "ptr", 3);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
        std::vector<grh::ValueId> treeReads;
        for (const auto &name : group.names)
        {
            treeReads.push_back(makeRead(graph, name, 4));
        }
        const grh::ValueId chain =
            makeMuxChain(graph, ptr, 3, {3, 1, 6, 0, 7, 2, 5, 4}, treeReads, 4,
                         makeConst(graph, 4, 0));
        graph.bindOutputPort("sel_out", chain);

        if (const int rc = runPass(design); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("read-select-order: expected changes");
        }
        const grh::OperationId outDef =
            graph.getValue(graph.outputPortValue("sel_out")).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kSliceDynamic)
        {
            return fail("read-select-order: output must be a kSliceDynamic");
        }
        return 0;
    }

    // (d) rejects: duplicate constant / nonzero default / incomplete coverage.
    int testReadSelectRejects()
    {
        auto buildChainCase = [&](int variant) {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            const grh::ValueId ptr = makeInput(graph, "ptr", 3);
            const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
            const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
            std::vector<grh::ValueId> treeReads;
            for (const auto &name : group.names)
            {
                treeReads.push_back(makeRead(graph, name, 4));
            }
            grh::ValueId chain;
            if (variant == 0)
            {
                chain = makeMuxChain(graph, ptr, 3, {0, 1, 2, 3, 3, 5, 6, 7}, treeReads, 4,
                                     makeConst(graph, 4, 0));
            }
            else if (variant == 1)
            {
                chain = makeMuxChain(graph, ptr, 3, {0, 1, 2, 3, 4, 5, 6, 7}, treeReads, 4,
                                     makeConst(graph, 4, 5));
            }
            else
            {
                chain = makeMuxChain(graph, ptr, 3, {0, 1, 2, 3, 4, 6, 7}, treeReads, 4,
                                     makeConst(graph, 4, 0));
            }
            graph.bindOutputPort("sel_out", chain);
            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            const grh::OperationId outDef =
                graph.getValue(graph.outputPortValue("sel_out")).definingOp();
            if (outDef.valid() &&
                graph.getOperation(outDef).kind() == grh::OperationKind::kSliceDynamic)
            {
                return fail("read-select-reject: tree must not become kSliceDynamic (variant " +
                            std::to_string(variant) + ")");
            }
            return 0;
        };
        if (const int rc = buildChainCase(0))
        {
            return rc;
        }
        if (const int rc = buildChainCase(1))
        {
            return rc;
        }
        if (const int rc = buildChainCase(2))
        {
            return rc;
        }
        return 0;
    }

    // (e) cross-group mixes and scalar-lane mixes are rejected.
    int testReadSelectMixedRejects()
    {
        {
            // Two groups: mixing their lane slices in one chain must not convert.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            const grh::ValueId ptr = makeInput(graph, "ptr", 3);
            const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
            const LaneGroup groupA = buildBasicGroup(graph, "a", "_q", indices, 4, en1, enW, clk);
            const LaneGroup groupB = buildBasicGroup(graph, "b", "_q", indices, 4, en1, enW, clk);
            std::vector<grh::ValueId> treeReads;
            for (std::size_t k = 0; k < indices.size(); ++k)
            {
                treeReads.push_back(makeRead(graph, k % 2 == 0 ? groupA.names[k] : groupB.names[k], 4));
            }
            const grh::ValueId chain =
                makeMuxChain(graph, ptr, 3, {0, 1, 2, 3, 4, 5, 6, 7}, treeReads, 4,
                             makeConst(graph, 4, 0));
            graph.bindOutputPort("sel_out", chain);
            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            const grh::OperationId outDef =
                graph.getValue(graph.outputPortValue("sel_out")).definingOp();
            if (outDef.valid() &&
                graph.getOperation(outDef).kind() == grh::OperationKind::kSliceDynamic)
            {
                return fail("read-select-mixed: cross-group chain must not convert");
            }
        }
        {
            // A chain that also selects a scalar (unmerged) register is skipped.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            const grh::ValueId ptr = makeInput(graph, "ptr", 3);
            const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
            const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
            makeRegister(graph, "ext_q", 4);
            const grh::ValueId extRead = makeRead(graph, "ext_q", 4);
            const grh::ValueId extData =
                makeBinary(graph, grh::OperationKind::kAnd, enW, extRead, 4);
            makeWrite(graph, "ext_q", en1, extData, {clk}, {"posedge"});
            std::vector<grh::ValueId> treeReads;
            for (std::size_t k = 0; k < indices.size(); ++k)
            {
                treeReads.push_back(k == 5 ? extRead : makeRead(graph, group.names[k], 4));
            }
            const grh::ValueId chain =
                makeMuxChain(graph, ptr, 3, {0, 1, 2, 3, 4, 5, 6, 7}, treeReads, 4,
                             makeConst(graph, 4, 0));
            graph.bindOutputPort("sel_out", chain);
            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            const grh::OperationId outDef =
                graph.getValue(graph.outputPortValue("sel_out")).definingOp();
            if (outDef.valid() &&
                graph.getOperation(outDef).kind() == grh::OperationKind::kSliceDynamic)
            {
                return fail("read-select-scalar-mix: scalar-mixed chain must not convert");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Extension: kReplicate broadcast (1-bit operand) + 1-bit reduce
    // identity widen in place.
    // ------------------------------------------------------------------
    int testReplicateBroadcastMerges()
    {
        {
            // data_i = replicate(eq(ptr, i), 4) & (enW ^ self_i): the
            // replicate broadcasts a per-lane 1-bit value (eq-onehot inside).
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            const grh::ValueId ptr = makeInput(graph, "ptr", 3);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId sel = makeEqConst(graph, ptr, idx, 3);
                const grh::ValueId mask = makeReplicate(graph, sel, 4, 4);
                const grh::ValueId body =
                    makeBinary(graph, grh::OperationKind::kXor, enW, self, 4);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd, mask, body, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            else if (rc == 0)
            {
                return fail("replicate-broadcast: expected the group to merge");
            }
            if (!graph.findOperation("lane_q__laneagg").valid())
            {
                return fail("replicate-broadcast: wide register missing");
            }
            if (!roundTripJson(design))
            {
                return fail("replicate-broadcast: store/load round trip failed");
            }
        }
        {
            // data_i = replicate(reduceOr(or(en1, 1'b0)), 4) & (enW ^ self_i):
            // a 1-bit reduction is the identity and drops away.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            const grh::ValueId zero1 = makeConst(graph, 1, 0);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId oneBit =
                    makeBinary(graph, grh::OperationKind::kOr, en1, zero1, 1);
                const grh::ValueId reduced =
                    makeUnary(graph, grh::OperationKind::kReduceOr, oneBit, 1);
                const grh::ValueId mask = makeReplicate(graph, reduced, 4, 4);
                const grh::ValueId body =
                    makeBinary(graph, grh::OperationKind::kXor, enW, self, 4);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd, mask, body, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            if (const int rc = runPass(design); rc == 1)
            {
                return rc;
            }
            else if (rc == 0)
            {
                return fail("reduce-identity: expected the group to merge");
            }
            if (!graph.findOperation("lane_q__laneagg").valid())
            {
                return fail("reduce-identity: wide register missing");
            }
            if (!roundTripJson(design))
            {
                return fail("reduce-identity: store/load round trip failed");
            }
        }
        {
            // replicate of a multi-bit operand is not lane-pointwise.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 8);
                const grh::ValueId self = makeRead(graph, name, 8);
                const grh::ValueId doubled = makeReplicate(graph, enW, 2, 8);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd, doubled, self, 8);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            if (const int rc = runPass(design); rc != 0)
            {
                return rc == 1 ? rc : fail("replicate-multibit: pass must not merge");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
            {
                return fail("replicate-multibit: all lane registers must survive");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Extension: initValue packing. Uniform and affine constant inits
    // merge; non-affine / non-constant inits stay rejected.
    // ------------------------------------------------------------------
    std::optional<uint64_t> initSegment(const grh::Graph &graph, const grh::Operation &wideReg,
                                        uint64_t lane, int32_t width)
    {
        const auto init = getStringAttr(wideReg, "initValue");
        if (!init)
        {
            return std::nullopt;
        }
        slang::SVInt parsed;
        try
        {
            parsed = slang::SVInt::fromString(*init);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
        if (parsed.hasUnknown())
        {
            return std::nullopt;
        }
        const uint64_t low = lane * static_cast<uint64_t>(width);
        parsed = parsed.resize(static_cast<slang::bitwidth_t>(parsed.getBitWidth()));
        const auto *raw = parsed.getRawPtr();
        const uint64_t word = raw[low / 64] >> (low % 64);
        return word & ((UINT64_C(1) << width) - 1);
    }

    int testInitValuePacking()
    {
        auto buildInitCase = [&](int variant, std::string &failMsg) {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                const grh::OperationId reg = makeRegister(graph, name, 4);
                if (variant == 0)
                {
                    graph.setAttr(reg, "initValue", std::string("4'd0"));
                }
                else if (variant == 1)
                {
                    graph.setAttr(reg, "initValue",
                                  "4'd" + std::to_string(idx));
                }
                else if (variant == 2)
                {
                    graph.setAttr(reg, "initValue",
                                  "4'd" + std::to_string(idx * idx));
                }
                else
                {
                    graph.setAttr(reg, "initValue", std::string("$random"));
                }
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId data =
                    makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            LaneAggregateOptions options;
            options.outputKey = "la.report";
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<LaneAggregatePass>(options));
            PassDiagnostics diags;
            const PassManagerResult res = manager.run(design, diags);
            if (!res.success || diags.hasError())
            {
                failMsg = "pass run failed";
                return -1;
            }
            const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
            if (variant <= 1)
            {
                if (!res.changed || !wideReg.valid())
                {
                    failMsg = "expected merge";
                    return -1;
                }
                const auto expectSeg = [&](uint64_t lane, uint64_t expect) {
                    const auto seg = initSegment(graph, graph.getOperation(wideReg), lane, 4);
                    return seg && *seg == expect;
                };
                if (variant == 0 && (!expectSeg(0, 0) || !expectSeg(7, 0)))
                {
                    failMsg = "uniform init must pack to all-zero";
                    return -1;
                }
                if (variant == 1 && (!expectSeg(3, 3) || !expectSeg(7, 7) || !expectSeg(0, 0)))
                {
                    failMsg = "affine init must pack per-lane values";
                    return -1;
                }
                if (!roundTripJson(design))
                {
                    failMsg = "store/load round trip failed";
                    return -1;
                }
                return 0;
            }
            if (res.changed || countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
            {
                failMsg = "must not merge (variant " + std::to_string(variant) + ")";
                return -1;
            }
            if (variant == 2)
            {
                const auto *reportValue = dynamic_cast<const SessionSlotValue<std::string> *>(
                    session.find("la.report")->second.get());
                if (!reportValue || reportValue->value.find("non_affine_init") == std::string::npos)
                {
                    failMsg = "report must record non_affine_init";
                    return -1;
                }
            }
            return 0;
        };
        for (int variant = 0; variant < 4; ++variant)
        {
            std::string failMsg;
            if (const int rc = buildInitCase(variant, failMsg))
            {
                return fail("init-packing: " + failMsg);
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Registration and option parsing.
    // ------------------------------------------------------------------
    int testPassRegistration()
    {
        bool listed = false;
        for (const std::string &name : availableTransformPasses())
        {
            if (name == "lane-aggregate")
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            return fail("lane-aggregate must be listed by availableTransformPasses");
        }
        std::string error;
        const std::vector<std::string_view> noArgs;
        if (!makePass("lane-aggregate", noArgs, error))
        {
            return fail("makePass must create lane-aggregate: " + error);
        }
        const std::vector<std::string_view> goodArgs = {
            "-min-lanes", "4", "-max-index-holes=1", "-output-key", "la.report"};
        if (!makePass("lane-aggregate", goodArgs, error))
        {
            return fail("makePass must accept lane-aggregate options: " + error);
        }
        const std::vector<std::string_view> badArgs = {"-bogus"};
        if (makePass("lane-aggregate", badArgs, error))
        {
            return fail("lane-aggregate must reject unknown options");
        }
        const std::vector<std::string_view> lowArgs = {"-min-lanes", "1"};
        if (makePass("lane-aggregate", lowArgs, error))
        {
            return fail("lane-aggregate must reject -min-lanes < 2");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int rc = testMergeBasicGroup())
    {
        return rc;
    }
    if (const int rc = testLaneZeroSpecialization())
    {
        return rc;
    }
    if (const int rc = testSharedLeafMismatchRejected())
    {
        return rc;
    }
    if (const int rc = testNonAffineConstantsRejected())
    {
        return rc;
    }
    if (const int rc = testCrossGroupCoupling())
    {
        return rc;
    }
    if (const int rc = testGroupLevelRejects())
    {
        return rc;
    }
    if (const int rc = testEventMismatchLaneExcluded())
    {
        return rc;
    }
    if (const int rc = testConstantSegmentGrouping())
    {
        return rc;
    }
    if (const int rc = testConstantTailSegmentGrouping())
    {
        return rc;
    }
    if (const int rc = testMultiVaryingSegmentRejected())
    {
        return rc;
    }
    if (const int rc = testTwoDimensionalSubGrouping())
    {
        return rc;
    }
    if (const int rc = testSiblingGroupingEnqRob())
    {
        return rc;
    }
    if (const int rc = testSharedDispatchPortRead())
    {
        return rc;
    }
    if (const int rc = testReduceConcatNormalized())
    {
        return rc;
    }
    if (const int rc = testEqOnehot())
    {
        return rc;
    }
    if (const int rc = testLaneParamLeaf())
    {
        return rc;
    }
    if (const int rc = testLogicOpsWiden())
    {
        return rc;
    }
    if (const int rc = testAddRejected())
    {
        return rc;
    }
    if (const int rc = testReadSelectMuxChain())
    {
        return rc;
    }
    if (const int rc = testReadSelectOrTree())
    {
        return rc;
    }
    if (const int rc = testReadSelectBareOnehot())
    {
        return rc;
    }
    if (const int rc = testReadSelectOutOfOrder())
    {
        return rc;
    }
    if (const int rc = testReadSelectRejects())
    {
        return rc;
    }
    if (const int rc = testReadSelectMixedRejects())
    {
        return rc;
    }
    if (const int rc = testReplicateBroadcastMerges())
    {
        return rc;
    }
    if (const int rc = testInitValuePacking())
    {
        return rc;
    }
    if (const int rc = testPassRegistration())
    {
        return rc;
    }
    return 0;
}
