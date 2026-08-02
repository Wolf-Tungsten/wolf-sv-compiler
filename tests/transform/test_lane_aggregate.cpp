#include "core/grh.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "transform/lane_aggregate.hpp"
#include "transform/simplify.hpp"

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

    // operands[0] maps to the most significant bits (kConcat semantics);
    // defined near the reduce-concat tests below.
    grh::ValueId makeConcat(grh::Graph &graph, std::vector<grh::ValueId> operands, int32_t width);

    grh::ValueId makeSlice(grh::Graph &graph, grh::ValueId base, int64_t start, int64_t end)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                   static_cast<int32_t>(end - start + 1), false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kSliceStatic, graph.makeInternalOpSym());
        graph.addOperand(op, base);
        graph.addResult(op, out);
        graph.setAttr(op, "sliceStart", start);
        graph.setAttr(op, "sliceEnd", end);
        return out;
    }

    grh::ValueId makeSliceDynamic(grh::Graph &graph, grh::ValueId base, grh::ValueId offset, int32_t width)
    {
        const grh::ValueId out = graph.createValue(graph.makeInternalValSym(), width, false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kSliceDynamic, graph.makeInternalOpSym());
        graph.addOperand(op, base);
        graph.addOperand(op, offset);
        graph.addResult(op, out);
        graph.setAttr(op, "sliceWidth", static_cast<int64_t>(width));
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

    // Runs the pass with a session store and copies the group report JSON to
    // `report`; returns 2 when the pass changed the graph, 0 when not, 1 on
    // pass failure.
    int runPassWithReport(grh::Design &design, LaneAggregateOptions options, std::string &report)
    {
        options.outputKey = "la.report";
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("lane-aggregate pass run failed");
        }
        const auto reportIt = session.find("la.report");
        if (reportIt != session.end() && reportIt->second)
        {
            if (const auto *value =
                    dynamic_cast<const SessionSlotValue<std::string> *>(reportIt->second.get()))
            {
                report = value->value;
            }
        }
        return res.changed ? 2 : 0;
    }

    // Exact-all fallback, form A: every lane's cond cone reads one shared
    // single-member register whose name carries a _0 numeric segment. Lane 0
    // hashes that read as a same-index family read (marker 53) while the
    // other lanes hash it as an absolute-index read (marker 52), so the
    // signature majority is 7 of 8; the exact check sees a shared leaf.
    void buildSharedLeafFallbackGroup(grh::Design &design)
    {
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        makeRegister(graph, "solo_0_q", 1);
        makeWrite(graph, "solo_0_q", en1, makeInput(graph, "solo_d", 1), {clk}, {"posedge"});
        const grh::ValueId soloRead = makeRead(graph, "solo_0_q", 1);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
            const grh::ValueId cond =
                makeBinary(graph, grh::OperationKind::kOr, en1, soloRead, 1);
            makeWrite(graph, name, cond, data, {clk}, {"posedge"});
        }
    }

    // Exact-all fallback, form B: each lane's data cone carries a per-lane
    // kReduceOr over a per-lane kConcat of the full sibling range; element i
    // of lane i's concat reads the sibling lane at its own index, so the
    // signature mixes markers 53/52 differently per lane (majority 1 of 8).
    // The exact check sees shared reads (wide mode tree expansion) or a
    // lane-parameter concat (array mode) and merges.
    void buildSiblingReduceFallbackGroup(grh::Design &design)
    {
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        // Sibling group of 1-bit registers; non-affine per-lane constants
        // keep it scalar (it only seeds the lane reads).
        std::vector<grh::ValueId> sibReads;
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "sib_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            sibReads.push_back(self);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kXor, self,
                           makeConst(graph, 1, idx % 2), 1);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            const std::vector<grh::ValueId> elems(sibReads.rbegin(), sibReads.rend());
            const grh::ValueId packed = makeConcat(graph, elems, 8);
            const grh::ValueId reduced =
                makeUnary(graph, grh::OperationKind::kReduceOr, packed, 1);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kXor, self, reduced, 1);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }
    }

    // C-level lane-parameter leaf shape: a per-lane wide kEq compare as the
    // write cond (same kind/arity/attrs/width across lanes, not the
    // eq-onehot form). Returns the per-lane kEq result values in lane order.
    std::vector<grh::ValueId> buildLaneParamEqGroup(grh::Design &design)
    {
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        std::vector<grh::ValueId> eqs;
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const grh::ValueId cond =
                makeBinary(graph, grh::OperationKind::kEq, self,
                           makeConst(graph, 4, idx), 1);
            eqs.push_back(cond);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kAnd, enW, self, 4);
            makeWrite(graph, name, cond, data, {clk}, {"posedge"});
        }
        return eqs;
    }

    // Finds the 8-operand kConcat whose operands are `laneValues` in
    // MSB-first order (operand 0 = highest lane), as produced by the
    // lane-parameter leaf materialization.
    bool hasLaneParamConcat(const grh::Graph &graph, const std::vector<grh::ValueId> &laneValues)
    {
        const std::size_t count = laneValues.size();
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kConcat || op.operands().size() != count)
            {
                continue;
            }
            bool orderOk = true;
            for (std::size_t k = 0; k < count; ++k)
            {
                if (op.operands()[k] != laneValues[count - 1 - k])
                {
                    orderOk = false;
                    break;
                }
            }
            if (orderOk)
            {
                return true;
            }
        }
        return false;
    }

    // Exact-all fallback + sibling deps (the axi4buf shape): group A
    // (lane_*_q, 9 lanes) can only be rescued by the INCREMENTAL exact
    // fallback — every cond cone of lanes 0..7 carries a per-lane reduce
    // over the full shared s_*_q range (fragmenting the signature to
    // majority 1) and lane 8 is structurally divergent, so the one-shot
    // all-candidate check fails and the incremental build keeps lanes
    // 0..7. A's data cone reads sibling group B's lane at the same index
    // (a kSiblingRead dep). bIsomorphic selects whether B itself can merge.
    void buildFallbackSiblingGroups(grh::Design &design, bool bIsomorphic)
    {
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId rst = makeInput(graph, "rst", 1);
        // Shared 1-bit registers seeding the cond-cone signature
        // fragmentation; kept scalar by non-affine per-lane constants.
        std::vector<grh::ValueId> sReads;
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "s_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            sReads.push_back(self);
            makeWrite(graph, name, en1,
                      makeBinary(graph, grh::OperationKind::kXor, self,
                                 makeConst(graph, 1, idx % 2), 1),
                      {clk}, {"posedge"});
        }
        // Sibling group B (8 lanes, width 4).
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "b_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const uint64_t constant = bIsomorphic ? idx : (idx * idx) % 16;
            makeWrite(graph, name, en1,
                      makeBinary(graph, grh::OperationKind::kXor, self,
                                 makeConst(graph, 4, constant), 4),
                      {clk}, {"posedge"});
        }
        // Group A.
        for (uint64_t idx = 0; idx < 9; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            if (idx == 8)
            {
                // Divergent lane (reset-specialized cone): the one-shot
                // exact check over all candidates fails on it.
                const grh::ValueId data =
                    makeMux(graph, rst, makeConst(graph, 4, 0), self, 4);
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
                continue;
            }
            const std::vector<grh::ValueId> elems(sReads.rbegin(), sReads.rend());
            const grh::ValueId packed = makeConcat(graph, elems, 8);
            const grh::ValueId reduced =
                makeUnary(graph, grh::OperationKind::kReduceOr, packed, 1);
            const grh::ValueId cond =
                makeBinary(graph, grh::OperationKind::kOr, en1, reduced, 1);
            const grh::ValueId bRead = makeRead(graph, "b_" + std::to_string(idx) + "_q", 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kXor, self, bRead, 4);
            makeWrite(graph, name, cond, data, {clk}, {"posedge"});
        }
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
            // B cannot merge (lane-param leaves disabled, so its kAdd cones
            // stay non-pointwise) -> A must be rejected.
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

            LaneAggregateOptions options;
            options.laneParamLeaves = false; // keep the kAdd cones rejected
            if (const int rc = runPass(design, options); rc != 0)
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
    // (e) kAdd stays rejected even with affine constants when lane-param
    // leaves are disabled (with the option on, such cones merge as C-level
    // lane-parameter leaves — see testLaneParamLeafNonPointwise).
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
        options.laneParamLeaves = false;
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
    // (h) shl-onehot bit select (DataModule family):
    //   addr_dec = onehotWidth'd1 << ptr
    //   term_i   = addr_dec[i] ? lane_i : 0   (kMux)
    //   rdata    = term_0 | term_1 | ...
    // The bit select is either kSliceStatic(addr_dec, i, i) or
    // kSliceDynamic(addr_dec, const i, 1); both convert.
    // ------------------------------------------------------------------
    grh::ValueId makeShlOnehotTree(grh::Graph &graph,
                                   grh::ValueId ptr,
                                   int32_t onehotWidth,
                                   uint64_t onehotValue,
                                   const std::vector<grh::ValueId> &laneReads,
                                   int32_t width,
                                   bool dynamicBitSelect)
    {
        const grh::ValueId one = makeConst(graph, onehotWidth, onehotValue);
        const grh::ValueId decode = makeBinary(graph, grh::OperationKind::kShl, one, ptr, onehotWidth);
        grh::ValueId tree;
        for (std::size_t i = 0; i < laneReads.size(); ++i)
        {
            const grh::ValueId bit =
                dynamicBitSelect
                    ? makeSliceDynamic(graph, decode, makeConst(graph, 8, i), 1)
                    : makeSlice(graph, decode, static_cast<int64_t>(i), static_cast<int64_t>(i));
            const grh::ValueId term =
                makeMux(graph, bit, laneReads[i], makeConst(graph, width, 0), width);
            tree = tree.valid() ? makeBinary(graph, grh::OperationKind::kOr, tree, term, width) : term;
        }
        return tree;
    }

    int testReadSelectShlOnehotTree()
    {
        auto buildCase = [&](bool arrayMode, bool dynamicBitSelect, std::string &failMsg) {
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
            const grh::ValueId tree =
                makeShlOnehotTree(graph, ptr, 8, 1, treeReads, 4, dynamicBitSelect);
            graph.bindOutputPort("sel_out", tree);

            PassManager manager;
            LaneAggregateOptions options;
            if (arrayMode)
            {
                options.outputMode = LaneAggregateOutputMode::Array;
            }
            manager.addPass(std::make_unique<LaneAggregatePass>(options));
            manager.addPass(std::make_unique<SimplifyPass>());
            PassDiagnostics diags;
            const PassManagerResult res = manager.run(design, diags);
            if (!res.success || diags.hasError())
            {
                failMsg = "pass run failed";
                return -1;
            }
            if (!res.changed)
            {
                failMsg = "expected changes";
                return -1;
            }
            const grh::ValueId outValue = graph.outputPortValue("sel_out");
            if (!outValue.valid())
            {
                failMsg = "output missing";
                return -1;
            }
            const grh::OperationId outDef = graph.getValue(outValue).definingOp();
            if (!outDef.valid())
            {
                failMsg = "output must be driven by an op";
                return -1;
            }
            if (arrayMode)
            {
                if (!graph.findOperation("lane_q__laneagg").valid() ||
                    graph.getOperation(graph.findOperation("lane_q__laneagg")).kind() !=
                        grh::OperationKind::kMemory)
                {
                    failMsg = "kMemory missing";
                    return -1;
                }
                if (graph.getOperation(outDef).kind() != grh::OperationKind::kMemoryReadPort)
                {
                    failMsg = "output must be a kMemoryReadPort";
                    return -1;
                }
                const grh::Operation read = graph.getOperation(outDef);
                if (read.operands().size() != 1 || read.operands().front() != ptr)
                {
                    failMsg = "read address must be the select pointer directly";
                    return -1;
                }
                if (getStringAttr(read, "memSymbol") != "lane_q__laneagg" ||
                    graph.getValue(read.results().front()).width() != 4)
                {
                    failMsg = "read must target the merged kMemory with width 4";
                    return -1;
                }
                // No offset scaling and the one-hot decode must be gone.
                if (countOpsOfKind(graph, grh::OperationKind::kShl) != 0 ||
                    countOpsOfKind(graph, grh::OperationKind::kSliceDynamic) != 0)
                {
                    failMsg = "no kShl / kSliceDynamic may survive";
                    return -1;
                }
            }
            else
            {
                const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
                if (!wideReg.valid())
                {
                    failMsg = "wide register missing";
                    return -1;
                }
                if (graph.getOperation(outDef).kind() != grh::OperationKind::kSliceDynamic)
                {
                    failMsg = "output must be a kSliceDynamic";
                    return -1;
                }
                const grh::Operation sliceOp = graph.getOperation(outDef);
                const auto sliceWidth = getIntAttr(sliceOp, "sliceWidth");
                if (!sliceWidth || *sliceWidth != 4)
                {
                    failMsg = "sliceWidth must be 4";
                    return -1;
                }
                const grh::OperationId wideRead = findWideReadPort(graph, "lane_q__laneagg");
                if (!wideRead.valid() ||
                    sliceOp.operands()[0] != graph.getOperation(wideRead).results().front())
                {
                    failMsg = "kSliceDynamic base must be the wide read";
                    return -1;
                }
                const grh::OperationId offsetDef = graph.getValue(sliceOp.operands()[1]).definingOp();
                if (!offsetDef.valid() ||
                    graph.getOperation(offsetDef).kind() != grh::OperationKind::kShl ||
                    graph.getOperation(offsetDef).operands().front() != ptr)
                {
                    failMsg = "offset must be ptr << 2";
                    return -1;
                }
                // Exactly one kShl survives: the offset. The one-hot decode
                // and its bit selects must be dead-eliminated.
                if (countOpsOfKind(graph, grh::OperationKind::kShl) != 1)
                {
                    failMsg = "one-hot kShl must be eliminated (only the offset kShl stays)";
                    return -1;
                }
            }
            if (!roundTripJson(design))
            {
                failMsg = "store/load round trip failed";
                return -1;
            }
            return 0;
        };
        for (const bool arrayMode : {false, true})
        {
            for (const bool dynamicBitSelect : {false, true})
            {
                std::string failMsg;
                if (const int rc = buildCase(arrayMode, dynamicBitSelect, failMsg))
                {
                    return fail(std::string("shl-onehot-tree(array=") +
                                (arrayMode ? "1" : "0") + ", dyn=" + (dynamicBitSelect ? "1" : "0") +
                                "): " + failMsg);
                }
            }
        }
        return 0;
    }

    // Rejects: the shl-onehot form requires ptrWidth == log2(span), and the
    // shifted value must be the constant 1.
    int testReadSelectShlOnehotRejects()
    {
        auto buildCase = [&](bool arrayMode, int32_t ptrWidth, uint64_t onehotValue,
                             std::string &failMsg) {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId enW = makeInput(graph, "enW", 4);
            const grh::ValueId ptr = makeInput(graph, "ptr", ptrWidth);
            const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
            const LaneGroup group = buildBasicGroup(graph, "lane", "_q", indices, 4, en1, enW, clk);
            std::vector<grh::ValueId> treeReads;
            for (const auto &name : group.names)
            {
                treeReads.push_back(makeRead(graph, name, 4));
            }
            const grh::ValueId tree =
                makeShlOnehotTree(graph, ptr, 8, onehotValue, treeReads, 4, false);
            graph.bindOutputPort("sel_out", tree);

            LaneAggregateOptions options;
            if (arrayMode)
            {
                options.outputMode = LaneAggregateOutputMode::Array;
            }
            if (const int rc = runPass(design, options); rc == 1)
            {
                failMsg = "pass run failed";
                return -1;
            }
            // Phase 1 still merges; only phase 2 must skip the tree.
            if (!graph.findOperation("lane_q__laneagg").valid())
            {
                failMsg = "merged storage missing (phase 1 must still merge)";
                return -1;
            }
            const grh::OperationId outDef =
                graph.getValue(graph.outputPortValue("sel_out")).definingOp();
            if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kOr)
            {
                failMsg = "tree root must stay a kOr (phase 2 must skip)";
                return -1;
            }
            if (countOpsOfKind(graph, grh::OperationKind::kSliceDynamic) != 0)
            {
                failMsg = "no kSliceDynamic may be produced";
                return -1;
            }
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const grh::Operation op = graph.getOperation(opId);
                if (op.kind() == grh::OperationKind::kMemoryReadPort && op.operands().size() == 1 &&
                    op.operands().front() == ptr)
                {
                    failMsg = "no dynamic kMemoryReadPort at the pointer may be produced";
                    return -1;
                }
            }
            return 0;
        };
        std::string failMsg;
        // ptr wider than log2(span): the pointer can go out of range.
        if (const int rc = buildCase(true, 7, 1, failMsg))
        {
            return fail("shl-onehot-wide-ptr(array): " + failMsg);
        }
        if (const int rc = buildCase(false, 7, 1, failMsg))
        {
            return fail("shl-onehot-wide-ptr(wide): " + failMsg);
        }
        // The shifted value is not the constant 1: not a one-hot decode.
        if (const int rc = buildCase(true, 3, 2, failMsg))
        {
            return fail("shl-onehot-not-one: " + failMsg);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // (i) shl-onehot write-decode cone: per-lane bit i of (1 << waddr),
    // written as kSliceStatic(X, i, i) and/or kSliceDynamic(X, const i, 1)
    // with X = kShl(1, waddr), classifies as eq-onehot (bit i of (1 << x)
    // is (x == i) in 2-state semantics). This is what lets the DataModule
    // family merge its decoded-write lanes even when the two slice forms
    // are mixed across lanes.
    // ------------------------------------------------------------------
    grh::ValueId makeOnehotBit(grh::Graph &graph, grh::ValueId decode, uint64_t index, bool dynamicBit)
    {
        if (dynamicBit)
        {
            return makeSliceDynamic(graph, decode, makeConst(graph, 8, index), 1);
        }
        return makeSlice(graph, decode, static_cast<int64_t>(index), static_cast<int64_t>(index));
    }

    int testShlOnehotConeMerges()
    {
        auto buildCase = [&](bool arrayMode, int variant, std::string &failMsg) {
            // Decoded write: cond_i = wen & (1 << waddr)[i], data_i = wdata.
            // variant 0: all kSliceStatic; 1: all kSliceDynamic; 2: mixed
            // (lanes 0..3 static, 4..7 dynamic — the DataModule split).
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId wen = makeInput(graph, "wen", 1);
            const grh::ValueId waddr = makeInput(graph, "waddr", 3);
            const grh::ValueId wdata = makeInput(graph, "wdata", 4);
            const grh::ValueId decode =
                makeBinary(graph, grh::OperationKind::kShl, makeConst(graph, 8, 1), waddr, 8);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                const bool dynamicBit = variant == 1 || (variant == 2 && idx >= 4);
                const grh::ValueId bit = makeOnehotBit(graph, decode, idx, dynamicBit);
                const grh::ValueId cond = makeBinary(graph, grh::OperationKind::kAnd, wen, bit, 1);
                makeWrite(graph, name, cond, wdata, {clk}, {"posedge"});
            }
            LaneAggregateOptions options;
            options.minLanes = 4;
            if (arrayMode)
            {
                options.outputMode = LaneAggregateOutputMode::Array;
            }
            if (const int rc = runPass(design, options); rc == 1)
            {
                failMsg = "pass run failed";
                return -1;
            }
            else if (rc == 0)
            {
                failMsg = "expected the group to merge";
                return -1;
            }
            if (arrayMode)
            {
                const grh::OperationId mem = graph.findOperation("lane_q__laneagg");
                if (!mem.valid() || graph.getOperation(mem).kind() != grh::OperationKind::kMemory)
                {
                    failMsg = "kMemory missing";
                    return -1;
                }
                const auto row = getIntAttr(graph.getOperation(mem), "row");
                if (!row || *row != 8)
                {
                    failMsg = "kMemory must cover all 8 lanes";
                    return -1;
                }
                if (countOpsOfKind(graph, grh::OperationKind::kArrayOnehot) != 1)
                {
                    failMsg = "expected exactly one kArrayOnehot for the write decode";
                    return -1;
                }
                for (const auto opId : graph.operations())
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    const grh::Operation op = graph.getOperation(opId);
                    if (op.kind() != grh::OperationKind::kArrayOnehot)
                    {
                        continue;
                    }
                    const auto rows = getIntAttr(op, "rows");
                    if (op.operands().size() != 1 || op.operands().front() != waddr || !rows ||
                        *rows != 8 || graph.getValue(op.results().front()).width() != 8)
                    {
                        failMsg = "kArrayOnehot(waddr) must have rows=8 and an 8-bit result";
                        return -1;
                    }
                }
            }
            else
            {
                if (!graph.findOperation("lane_q__laneagg").valid())
                {
                    failMsg = "wide register missing";
                    return -1;
                }
                // The onehot materializes as kShl(8'd1, waddr) with an 8-bit
                // result (the original decode kShl stays behind dead).
                bool foundOnehot = false;
                for (const auto opId : graph.operations())
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    const grh::Operation op = graph.getOperation(opId);
                    if (op.kind() != grh::OperationKind::kShl || op.operands().size() != 2 ||
                        op.operands()[1] != waddr ||
                        graph.getValue(op.results().front()).width() != 8)
                    {
                        continue;
                    }
                    foundOnehot = true;
                }
                if (!foundOnehot)
                {
                    failMsg = "expected kShl(1, waddr) onehot materialization";
                    return -1;
                }
            }
            if (!roundTripJson(design))
            {
                failMsg = "store/load round trip failed";
                return -1;
            }
            return 0;
        };
        for (const bool arrayMode : {false, true})
        {
            for (int variant = 0; variant < 3; ++variant)
            {
                std::string failMsg;
                if (const int rc = buildCase(arrayMode, variant, failMsg))
                {
                    return fail(std::string("shl-onehot-cone(array=") + (arrayMode ? "1" : "0") +
                                ", variant=" + std::to_string(variant) + "): " + failMsg);
                }
            }
        }
        return 0;
    }

    // Rejects: the onehot cone requires every lane's bit index to equal the
    // lane index, one shared base, and a non-constant pointer. (A constant
    // pointer is out of scope for the onehot decode, but per-lane slices of
    // one shared base at lane-affine offsets are still an exact affine
    // gather, so variant 2 merges through the R-level leaf instead of
    // rejecting.)
    int testShlOnehotConeRejects()
    {
        auto buildCase = [&](int variant, std::string &failMsg) {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId wen = makeInput(graph, "wen", 1);
            const grh::ValueId waddr = makeInput(graph, "waddr", 3);
            const grh::ValueId wdata = makeInput(graph, "wdata", 4);
            const grh::ValueId other = makeInput(graph, "other", 3);
            const grh::ValueId decode =
                makeBinary(graph, grh::OperationKind::kShl, makeConst(graph, 8, 1), waddr, 8);
            const grh::ValueId decode2 =
                makeBinary(graph, grh::OperationKind::kShl, makeConst(graph, 8, 1), other, 8);
            const grh::ValueId decodeConst =
                makeBinary(graph, grh::OperationKind::kShl, makeConst(graph, 8, 1),
                           makeConst(graph, 3, 2), 8);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                grh::ValueId bit;
                if (variant == 0)
                {
                    // Alternating bases: not one shared decode.
                    bit = makeOnehotBit(graph, idx % 2 == 0 ? decode : decode2, idx, false);
                }
                else if (variant == 1)
                {
                    // Misaligned bit index (lane i takes bit i+1 mod 8).
                    bit = makeOnehotBit(graph, decode, (idx + 1) % 8, false);
                }
                else
                {
                    // Constant pointer: a fold, not a select — but still an
                    // exact affine gather of one shared base, so it merges.
                    bit = makeOnehotBit(graph, decodeConst, idx, false);
                }
                const grh::ValueId cond = makeBinary(graph, grh::OperationKind::kAnd, wen, bit, 1);
                makeWrite(graph, name, cond, wdata, {clk}, {"posedge"});
            }
            LaneAggregateOptions options;
            options.minLanes = 4;
            const int rc = runPass(design, options);
            if (rc == 1)
            {
                failMsg = "pass run failed";
                return -1;
            }
            if (variant == 2)
            {
                // Affine-gather leaf: the packed per-lane bits are exactly
                // the shared decode vector itself.
                if (rc == 0)
                {
                    failMsg = "constant-pointer gather must merge via the affine leaf";
                    return -1;
                }
                if (!graph.findOperation("lane_q__laneagg").valid())
                {
                    failMsg = "wide register missing";
                    return -1;
                }
                if (!roundTripJson(design))
                {
                    failMsg = "store/load round trip failed";
                    return -1;
                }
                return 0;
            }
            if (rc != 0)
            {
                failMsg = "must not merge (variant " + std::to_string(variant) + ")";
                return -1;
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
            {
                failMsg = "all lane registers must survive";
                return -1;
            }
            return 0;
        };
        for (int variant = 0; variant < 3; ++variant)
        {
            std::string failMsg;
            if (const int rc = buildCase(variant, failMsg))
            {
                return fail("shl-onehot-cone-reject(variant=" + std::to_string(variant) +
                            "): " + failMsg);
            }
        }
        return 0;
    }

    // End-to-end DataModule shape: decoded write with mixed slice forms
    // (lanes 0..3 kSliceStatic, 4..7 kSliceDynamic), a shl-onehot read-select
    // tree, and a top-level bypass mux. The group must merge all 8 lanes and
    // the tree becomes one dynamic read inside the bypass mux.
    int testShlOnehotConeDataModule()
    {
        auto buildCase = [&](bool arrayMode, std::string &failMsg) {
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId wen = makeInput(graph, "wen", 1);
            const grh::ValueId waddr = makeInput(graph, "waddr", 3);
            const grh::ValueId wdata = makeInput(graph, "wdata", 4);
            const grh::ValueId raddr = makeInput(graph, "raddr", 3);
            const grh::ValueId bypassAny = makeInput(graph, "bypass_any", 1);
            const grh::ValueId bypassVal = makeInput(graph, "bypass_val", 4);
            const grh::ValueId wdec =
                makeBinary(graph, grh::OperationKind::kShl, makeConst(graph, 8, 1), waddr, 8);
            std::vector<std::string> names;
            std::vector<grh::ValueId> treeReads;
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "data_" + std::to_string(idx) + "_q";
                names.push_back(name);
                makeRegister(graph, name, 4);
                const grh::ValueId self = makeRead(graph, name, 4);
                const grh::ValueId wbit = makeOnehotBit(graph, wdec, idx, idx >= 4);
                const grh::ValueId enable = makeBinary(graph, grh::OperationKind::kAnd, wen, wbit, 1);
                const grh::ValueId data = makeMux(graph, enable, wdata, self, 4);
                makeWrite(graph, name, enable, data, {clk}, {"posedge"});
                treeReads.push_back(makeRead(graph, name, 4));
            }
            // Read-select tree with mixed bit-select forms + bypass mux on top.
            const grh::ValueId rdec =
                makeBinary(graph, grh::OperationKind::kShl, makeConst(graph, 8, 1), raddr, 8);
            grh::ValueId tree;
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const grh::ValueId rbit = makeOnehotBit(graph, rdec, idx, idx >= 4);
                const grh::ValueId term =
                    makeMux(graph, rbit, treeReads[idx], makeConst(graph, 4, 0), 4);
                tree = tree.valid() ? makeBinary(graph, grh::OperationKind::kOr, tree, term, 4) : term;
            }
            const grh::ValueId rdata = makeMux(graph, bypassAny, bypassVal, tree, 4);
            graph.bindOutputPort("rdata", rdata);

            LaneAggregateOptions options;
            options.minLanes = 4;
            if (arrayMode)
            {
                options.outputMode = LaneAggregateOutputMode::Array;
            }
            PassManager manager;
            manager.addPass(std::make_unique<LaneAggregatePass>(options));
            manager.addPass(std::make_unique<SimplifyPass>());
            PassDiagnostics diags;
            const PassManagerResult res = manager.run(design, diags);
            if (!res.success || diags.hasError())
            {
                failMsg = "pass run failed";
                return -1;
            }
            if (!res.changed)
            {
                failMsg = "expected changes";
                return -1;
            }
            // All 8 lanes merged (the mixed write decode must not split).
            if (arrayMode)
            {
                const grh::OperationId mem = graph.findOperation("data_q__laneagg");
                if (!mem.valid() || graph.getOperation(mem).kind() != grh::OperationKind::kMemory)
                {
                    failMsg = "kMemory missing";
                    return -1;
                }
                const auto row = getIntAttr(graph.getOperation(mem), "row");
                if (!row || *row != 8)
                {
                    failMsg = "kMemory must cover all 8 lanes";
                    return -1;
                }
            }
            else if (!graph.findOperation("data_q__laneagg").valid())
            {
                failMsg = "wide register missing";
                return -1;
            }
            // rdata = mux(bypass_any, bypass_val, <one dynamic read>).
            const grh::ValueId outValue = graph.outputPortValue("rdata");
            if (!outValue.valid())
            {
                failMsg = "output missing";
                return -1;
            }
            const grh::OperationId outDef = graph.getValue(outValue).definingOp();
            if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kMux)
            {
                failMsg = "the bypass mux must be preserved";
                return -1;
            }
            const grh::Operation bypassMux = graph.getOperation(outDef);
            if (bypassMux.operands().size() != 3 || bypassMux.operands()[0] != bypassAny ||
                bypassMux.operands()[1] != bypassVal)
            {
                failMsg = "bypass mux must keep bypass_any / bypass_val";
                return -1;
            }
            const grh::OperationId readDef = graph.getValue(bypassMux.operands()[2]).definingOp();
            if (!readDef.valid())
            {
                failMsg = "bypass mux false branch must be driven by an op";
                return -1;
            }
            if (arrayMode)
            {
                const grh::Operation read = graph.getOperation(readDef);
                if (read.kind() != grh::OperationKind::kMemoryReadPort ||
                    getStringAttr(read, "memSymbol") != "data_q__laneagg" ||
                    read.operands().size() != 1 || read.operands().front() != raddr)
                {
                    failMsg = "tree must become one kMemoryReadPort(mem, raddr)";
                    return -1;
                }
            }
            else
            {
                if (graph.getOperation(readDef).kind() != grh::OperationKind::kSliceDynamic)
                {
                    failMsg = "tree must become one kSliceDynamic";
                    return -1;
                }
            }
            if (!roundTripJson(design))
            {
                failMsg = "store/load round trip failed";
                return -1;
            }
            return 0;
        };
        for (const bool arrayMode : {false, true})
        {
            std::string failMsg;
            if (const int rc = buildCase(arrayMode, failMsg))
            {
                return fail(std::string("shl-onehot-datamodule(array=") + (arrayMode ? "1" : "0") +
                            "): " + failMsg);
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
    // Exact-all fallback, form A (wide): the shared single-member read
    // splits lane 0's signature (marker 53 vs 52), majority 7 < minLanes;
    // the exact check over all candidates merges every lane. Disabling
    // -exact-fallback keeps the no_majority rejection.
    // ------------------------------------------------------------------
    int testExactFallbackSharedLeaf()
    {
        {
            grh::Design design;
            buildSharedLeafFallbackGroup(design);
            grh::Graph &graph = *design.findGraph("g");
            std::string report;
            if (runPassWithReport(design, LaneAggregateOptions{}, report) != 2)
            {
                return fail("exact-fallback shared-leaf: expected the group to merge");
            }
            const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
            if (!wideReg.valid())
            {
                return fail("exact-fallback shared-leaf: wide register missing");
            }
            const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
            if (!wideWidth || *wideWidth != 32)
            {
                return fail("exact-fallback shared-leaf: wide register width must be 32");
            }
            // All 8 lanes merged; the shared register stays scalar.
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 2 ||
                !graph.findOperation("solo_0_q").valid())
            {
                return fail(
                    "exact-fallback shared-leaf: expected wide register + solo_0_q scalar");
            }
            if (report.find("\"outcome\":\"merged\"") == std::string::npos ||
                report.find("\"reject_reason\":\"no_majority_exact\"") == std::string::npos)
            {
                return fail("exact-fallback shared-leaf: report must record merged + "
                            "no_majority_exact");
            }
            if (!roundTripJson(design))
            {
                return fail("exact-fallback shared-leaf: store/load round trip failed");
            }
        }
        {
            grh::Design design;
            buildSharedLeafFallbackGroup(design);
            grh::Graph &graph = *design.findGraph("g");
            LaneAggregateOptions options;
            options.exactFallback = false;
            std::string report;
            if (runPassWithReport(design, options, report) != 0)
            {
                return fail("exact-fallback disabled: pass must not merge the group");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 9)
            {
                return fail("exact-fallback disabled: all lane registers must survive");
            }
            if (report.find("\"reject_reason\":\"no_majority\"") == std::string::npos)
            {
                return fail("exact-fallback disabled: report must record no_majority");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Exact-all fallback, form B (wide): the per-lane full-range sibling
    // reduction fragments every signature (majority 1); the exact check
    // merges. The sibling group stays scalar (non-affine constants).
    // ------------------------------------------------------------------
    int testExactFallbackSiblingReduce()
    {
        grh::Design design;
        buildSiblingReduceFallbackGroup(design);
        grh::Graph &graph = *design.findGraph("g");
        std::string report;
        if (runPassWithReport(design, LaneAggregateOptions{}, report) != 2)
        {
            return fail("exact-fallback sibling-reduce: expected the group to merge");
        }
        const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
        if (!wideReg.valid())
        {
            return fail("exact-fallback sibling-reduce: wide register missing");
        }
        const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
        if (!wideWidth || *wideWidth != 8)
        {
            return fail("exact-fallback sibling-reduce: wide register width must be 8");
        }
        // 8 lanes merged into the wide register; 8 sibling lanes stay scalar.
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 9 ||
            !graph.findOperation("sib_3_q").valid())
        {
            return fail("exact-fallback sibling-reduce: expected wide register + 8 scalar "
                        "siblings");
        }
        if (report.find("\"reject_reason\":\"no_majority_exact\"") == std::string::npos)
        {
            return fail("exact-fallback sibling-reduce: report must record no_majority_exact");
        }
        if (!roundTripJson(design))
        {
            return fail("exact-fallback sibling-reduce: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // C-level lane-parameter leaf (wide): a per-lane wide kEq compare no
    // longer rejects; the leaf is materialized as one per-lane kConcat of
    // the frozen kEq subgraphs, whose self reads become lane slices in C3.
    // Disabling -lane-param-leaves keeps the unsupported_op rejection.
    // ------------------------------------------------------------------
    int testLaneParamLeafNonPointwise()
    {
        {
            grh::Design design;
            const std::vector<grh::ValueId> eqs = buildLaneParamEqGroup(design);
            grh::Graph &graph = *design.findGraph("g");
            if (const int rc = runPass(design); rc != 2)
            {
                return rc == 1 ? rc : fail("lane-param leaf: expected the group to merge");
            }
            if (!graph.findOperation("lane_q__laneagg").valid())
            {
                return fail("lane-param leaf: wide register missing");
            }
            if (!hasLaneParamConcat(graph, eqs))
            {
                return fail("lane-param leaf: expected per-lane kConcat of the kEq leaves");
            }
            // The frozen per-lane kEq ops survive and read lane slices.
            for (const grh::ValueId eq : eqs)
            {
                const grh::OperationId eqDef = graph.getValue(eq).definingOp();
                if (!eqDef.valid() ||
                    graph.getOperation(eqDef).kind() != grh::OperationKind::kEq)
                {
                    return fail("lane-param leaf: per-lane kEq must survive");
                }
                const grh::Operation eqOp = graph.getOperation(eqDef);
                const grh::OperationId baseDef = graph.getValue(eqOp.operands()[0]).definingOp();
                if (!baseDef.valid() ||
                    graph.getOperation(baseDef).kind() != grh::OperationKind::kSliceStatic)
                {
                    return fail("lane-param leaf: kEq self operand must become a lane slice");
                }
            }
            if (!roundTripJson(design))
            {
                return fail("lane-param leaf: store/load round trip failed");
            }
        }
        {
            grh::Design design;
            buildLaneParamEqGroup(design);
            grh::Graph &graph = *design.findGraph("g");
            LaneAggregateOptions options;
            options.laneParamLeaves = false;
            std::string report;
            if (runPassWithReport(design, options, report) != 0)
            {
                return fail("lane-param leaves disabled: pass must not merge");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
            {
                return fail("lane-param leaves disabled: all lane registers must survive");
            }
            if (report.find("unsupported_op") == std::string::npos)
            {
                return fail("lane-param leaves disabled: report must record unsupported_op");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // R-level rejection boundary: per-lane kSliceStatic positions whose
    // offsets are NOT affine in the lane index (overlapping/crossing
    // segments included: with a fixed slope W and distinct lane indices an
    // overlap is always non-affine), or whose bases differ across lanes,
    // still fail the uniform-attrs check (structure_mismatch).
    // ------------------------------------------------------------------
    int testLaneParamLeafAttrsDifferRejected()
    {
        {
            // Non-affine offsets: lane i slices wide_in at i*i.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId wideIn = makeInput(graph, "wide_in", 64);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 2);
                makeRead(graph, name, 2);
                const grh::ValueId data = makeSlice(graph, wideIn,
                                                    static_cast<int64_t>(idx * idx),
                                                    static_cast<int64_t>(idx * idx + 1));
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            std::string report;
            if (runPassWithReport(design, LaneAggregateOptions{}, report) != 0)
            {
                return fail("attrs-differ non-affine: pass must not merge");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
            {
                return fail("attrs-differ non-affine: all lane registers must survive");
            }
            if (report.find("structure_mismatch") == std::string::npos)
            {
                return fail("attrs-differ non-affine: report must record structure_mismatch");
            }
        }
        {
            // Mixed bases: even lanes slice wide_a, odd lanes slice wide_b
            // at the same affine offsets.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId wideA = makeInput(graph, "wide_a", 16);
            const grh::ValueId wideB = makeInput(graph, "wide_b", 16);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 2);
                makeRead(graph, name, 2);
                const grh::ValueId data = makeSlice(graph, idx % 2 == 0 ? wideA : wideB,
                                                    static_cast<int64_t>(idx * 2),
                                                    static_cast<int64_t>(idx * 2 + 1));
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            std::string report;
            if (runPassWithReport(design, LaneAggregateOptions{}, report) != 0)
            {
                return fail("attrs-differ mixed-base: pass must not merge");
            }
            if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
            {
                return fail("attrs-differ mixed-base: all lane registers must survive");
            }
            if (report.find("structure_mismatch") == std::string::npos)
            {
                return fail("attrs-differ mixed-base: report must record structure_mismatch");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // R-level affine-gather leaf: lane i = kSliceStatic(X, base0 + i*W, ...)
    // of one shared base X. The packed per-lane slices are exactly
    // X[base0 +: span*W] — zero-cost materialization (X itself on an exact
    // fit, else one kSliceStatic), never a per-lane kConcat.
    // ------------------------------------------------------------------
    int testAffineGatherLeaf()
    {
        // Finds the (single) write-port data-masked operand's source value:
        // nextValue = kOr(kAnd(dataVec, mask), kAnd(wide, ~mask)).
        auto dataVecOf = [](grh::Graph &graph, const std::string &wideName) -> grh::ValueId {
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const grh::Operation op = graph.getOperation(opId);
                if (op.kind() != grh::OperationKind::kRegisterWritePort ||
                    getStringAttr(op, "regSymbol") != wideName)
                {
                    continue;
                }
                const grh::OperationId orDef = graph.getValue(op.operands()[1]).definingOp();
                if (!orDef.valid() || graph.getOperation(orDef).kind() != grh::OperationKind::kOr)
                {
                    return grh::ValueId::invalid();
                }
                // Bind the Operation copies: spanning into a temporary's
                // operands() inside a range-for dangles in C++20.
                const grh::Operation orOp = graph.getOperation(orDef);
                for (const grh::ValueId operand : orOp.operands())
                {
                    const grh::OperationId andDef = graph.getValue(operand).definingOp();
                    if (!andDef.valid() ||
                        graph.getOperation(andDef).kind() != grh::OperationKind::kAnd)
                    {
                        continue;
                    }
                    const grh::Operation andOp = graph.getOperation(andDef);
                    for (const grh::ValueId andOperand : andOp.operands())
                    {
                        // The data-masked kAnd's other operand is dataVec
                        // (the mask side is a kConcat of kReplicate bits).
                        const grh::OperationId andOperandDef =
                            graph.getValue(andOperand).definingOp();
                        if (!andOperandDef.valid() ||
                            graph.getOperation(andOperandDef).kind() != grh::OperationKind::kConcat)
                        {
                            return andOperand;
                        }
                    }
                }
            }
            return grh::ValueId::invalid();
        };
        {
            // W = 1 (the writeReqValid shape), exact fit: dataVec IS X.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId xvec = makeInput(graph, "xvec", 8);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 1);
                makeRead(graph, name, 1);
                const grh::ValueId data = makeSlice(graph, xvec, static_cast<int64_t>(idx),
                                                    static_cast<int64_t>(idx));
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            if (const int rc = runPass(design); rc != 2)
            {
                return rc == 1 ? rc : fail("affine-gather w1: expected the group to merge");
            }
            const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
            if (!wideReg.valid())
            {
                return fail("affine-gather w1: wide register missing");
            }
            // Zero-cost: the write data cone references X directly (no
            // per-lane kConcat, no extra slice).
            if (dataVecOf(graph, "lane_q__laneagg") != xvec)
            {
                return fail("affine-gather w1: dataVec must be X itself");
            }
            if (!roundTripJson(design))
            {
                return fail("affine-gather w1: store/load round trip failed");
            }
        }
        {
            // W = 4 with a non-zero base: dataVec = kSliceStatic(X, 4, 35).
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId xvec = makeInput(graph, "xvec", 36);
            for (uint64_t idx = 0; idx < 8; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                makeRegister(graph, name, 4);
                makeRead(graph, name, 4);
                const grh::ValueId data = makeSlice(graph, xvec,
                                                    static_cast<int64_t>(4 + idx * 4),
                                                    static_cast<int64_t>(4 + idx * 4 + 3));
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            if (const int rc = runPass(design); rc != 2)
            {
                return rc == 1 ? rc : fail("affine-gather base4: expected the group to merge");
            }
            const grh::ValueId dataVec = dataVecOf(graph, "lane_q__laneagg");
            if (!dataVec.valid())
            {
                return fail("affine-gather base4: dataVec missing");
            }
            const grh::OperationId sliceDef = graph.getValue(dataVec).definingOp();
            if (!sliceDef.valid() ||
                graph.getOperation(sliceDef).kind() != grh::OperationKind::kSliceStatic)
            {
                return fail("affine-gather base4: dataVec must be one kSliceStatic");
            }
            const grh::Operation sliceOp = graph.getOperation(sliceDef);
            const auto sliceStart = getIntAttr(sliceOp, "sliceStart");
            const auto sliceEnd = getIntAttr(sliceOp, "sliceEnd");
            if (!sliceStart || !sliceEnd || *sliceStart != 4 || *sliceEnd != 35 ||
                sliceOp.operands().size() != 1 || sliceOp.operands().front() != xvec)
            {
                return fail("affine-gather base4: expected kSliceStatic(X, 4, 35)");
            }
            if (!roundTripJson(design))
            {
                return fail("affine-gather base4: store/load round trip failed");
            }
        }
        {
            // Hole lane: lane 3 keeps an unparseable init and stays scalar;
            // the bucket {0,1,2,4,5,6,7,8} still packs against span 9.
            grh::Design design;
            grh::Graph &graph = design.createGraph("g");
            design.markAsTop("g");
            const grh::ValueId clk = makeInput(graph, "clk", 1);
            const grh::ValueId en1 = makeInput(graph, "en1", 1);
            const grh::ValueId xvec = makeInput(graph, "xvec", 9);
            for (uint64_t idx = 0; idx < 9; ++idx)
            {
                const std::string name = "lane_" + std::to_string(idx) + "_q";
                const grh::OperationId reg = makeRegister(graph, name, 1);
                if (idx == 3)
                {
                    graph.setAttr(reg, "initValue", std::string("$random"));
                }
                makeRead(graph, name, 1);
                const grh::ValueId data = makeSlice(graph, xvec, static_cast<int64_t>(idx),
                                                    static_cast<int64_t>(idx));
                makeWrite(graph, name, en1, data, {clk}, {"posedge"});
            }
            if (const int rc = runPass(design); rc != 2)
            {
                return rc == 1 ? rc : fail("affine-gather hole: expected the group to merge");
            }
            const grh::OperationId wideReg = graph.findOperation("lane_q__laneagg");
            if (!wideReg.valid())
            {
                return fail("affine-gather hole: wide register missing");
            }
            const auto wideWidth = getIntAttr(graph.getOperation(wideReg), "width");
            if (!wideWidth || *wideWidth != 9)
            {
                return fail("affine-gather hole: wide register width must be span(9)*1=9");
            }
            if (!graph.findOperation("lane_3_q").valid())
            {
                return fail("affine-gather hole: the excluded lane must stay scalar");
            }
            if (dataVecOf(graph, "lane_q__laneagg") != xvec)
            {
                return fail("affine-gather hole: dataVec must be X itself");
            }
            if (!roundTripJson(design))
            {
                return fail("affine-gather hole: store/load round trip failed");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Fallback + sibling deps, negative (wide): A is rescued only by the
    // incremental exact fallback and depends on sibling group B, which
    // cannot merge (non-affine constants). A must be rejected with
    // sibling_not_merged by the dependency fixpoint — BEFORE any graph
    // rewrite — so the pass reports no error and the graph is untouched.
    // ------------------------------------------------------------------
    int testExactFallbackSiblingNotMerged()
    {
        grh::Design design;
        buildFallbackSiblingGroups(design, /*bIsomorphic=*/false);
        grh::Graph &graph = *design.findGraph("g");
        std::string report;
        // rc==1 would mean the pass reported an error (e.g. the C2
        // "sibling group not merged" failure); rc may be 2 because the
        // reduce-concat pre-pass normalization rewrites cones even when no
        // group merges.
        if (runPassWithReport(design, LaneAggregateOptions{}, report) == 1)
        {
            return fail("fallback sibling-reject: pass must not fail");
        }
        // 9 A lanes + 8 B lanes + 8 shared s registers all survive.
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 25)
        {
            return fail("fallback sibling-reject: all lane registers must survive");
        }
        if (graph.findOperation("lane_q__laneagg").valid() ||
            graph.findOperation("b_q__laneagg").valid())
        {
            return fail("fallback sibling-reject: no wide register may be created");
        }
        if (report.find("sibling_not_merged") == std::string::npos)
        {
            return fail("fallback sibling-reject: report must record sibling_not_merged");
        }
        if (!roundTripJson(design))
        {
            return fail("fallback sibling-reject: store/load round trip failed");
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Fallback + sibling deps, positive (wide): same shape, but B merges;
    // the dependency fixpoint then lets both A (incremental-fallback
    // bucket {0..7}) and B merge, with divergent lane 8 staying scalar.
    // ------------------------------------------------------------------
    int testExactFallbackSiblingMerged()
    {
        grh::Design design;
        buildFallbackSiblingGroups(design, /*bIsomorphic=*/true);
        grh::Graph &graph = *design.findGraph("g");
        std::string report;
        if (runPassWithReport(design, LaneAggregateOptions{}, report) != 2)
        {
            return fail("fallback sibling-merge: expected both groups to merge");
        }
        const grh::OperationId wideA = graph.findOperation("lane_q__laneagg");
        const grh::OperationId wideB = graph.findOperation("b_q__laneagg");
        if (!wideA.valid() || !wideB.valid())
        {
            return fail("fallback sibling-merge: both wide registers must exist");
        }
        const auto widthA = getIntAttr(graph.getOperation(wideA), "width");
        const auto widthB = getIntAttr(graph.getOperation(wideB), "width");
        if (!widthA || *widthA != 32 || !widthB || *widthB != 32)
        {
            return fail("fallback sibling-merge: wide registers must be span(8)*4=32 bits");
        }
        // 2 wide registers + 8 scalar s registers + divergent lane 8.
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 11 ||
            !graph.findOperation("lane_8_q").valid())
        {
            return fail("fallback sibling-merge: expected wide registers + s scalars + "
                        "divergent lane 8");
        }
        // A was rescued by the exact fallback.
        if (report.find("\"reject_reason\":\"no_majority_exact\"") == std::string::npos)
        {
            return fail("fallback sibling-merge: report must record no_majority_exact");
        }
        if (!roundTripJson(design))
        {
            return fail("fallback sibling-merge: store/load round trip failed");
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
            "-min-lanes", "4", "-max-index-holes=1", "-output-key", "la.report", "-output-mode=array"};
        if (!makePass("lane-aggregate", goodArgs, error))
        {
            return fail("makePass must accept lane-aggregate options: " + error);
        }
        const std::vector<std::string_view> wideArgs = {"-output-mode", "wide"};
        if (!makePass("lane-aggregate", wideArgs, error))
        {
            return fail("makePass must accept -output-mode wide: " + error);
        }
        const std::vector<std::string_view> badModeArgs = {"-output-mode=bogus"};
        if (makePass("lane-aggregate", badModeArgs, error))
        {
            return fail("lane-aggregate must reject an unknown -output-mode");
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

    // ------------------------------------------------------------------
    // Array output mode (-output-mode=array).
    // ------------------------------------------------------------------
    LaneAggregateOptions arrayOptions()
    {
        LaneAggregateOptions options;
        options.outputMode = LaneAggregateOutputMode::Array;
        return options;
    }

    std::optional<std::vector<int64_t>> getIntListAttr(const grh::Operation &op, std::string_view key)
    {
        auto attr = op.attr(key);
        if (!attr)
        {
            return std::nullopt;
        }
        if (const auto *value = std::get_if<std::vector<int64_t>>(&*attr))
        {
            return *value;
        }
        return std::nullopt;
    }

    std::optional<std::vector<std::string>> getStringListAttr(const grh::Operation &op, std::string_view key)
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

    std::optional<uint64_t> parseLiteralUInt(const std::string &literal)
    {
        slang::SVInt parsed;
        try
        {
            parsed = slang::SVInt::fromString(literal);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
        if (parsed.hasUnknown())
        {
            return std::nullopt;
        }
        return static_cast<uint64_t>(*parsed.getRawPtr());
    }

    std::optional<uint64_t> constResultAsUInt(const grh::Graph &graph, grh::ValueId value)
    {
        const grh::OperationId defId = graph.getValue(value).definingOp();
        if (!defId.valid())
        {
            return std::nullopt;
        }
        const grh::Operation def = graph.getOperation(defId);
        if (def.kind() != grh::OperationKind::kConstant)
        {
            return std::nullopt;
        }
        const auto literal = getStringAttr(def, "constValue");
        if (!literal)
        {
            return std::nullopt;
        }
        return parseLiteralUInt(*literal);
    }

    // Finds the single op of `kind` with attr `attrKey` == attrValue.
    grh::OperationId findPortOp(const grh::Graph &graph, grh::OperationKind kind,
                                std::string_view attrKey, const std::string &attrValue)
    {
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() == kind && getStringAttr(op, attrKey) == attrValue)
            {
                return opId;
            }
        }
        return grh::OperationId::invalid();
    }

    // Array-mode basic group: kMemory + kArrayReadAllPort + kArrayWritePort,
    // kArrayBroadcast shared leaves, kArrayLaneConst tables, kArrayMux, and
    // kMemoryReadPort(constant address) lane reads.
    int testArrayBasicGroup()
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

        LaneAggregateOptions options = arrayOptions();
        options.outputKey = "la.report";
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<LaneAggregatePass>(options));
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("array basic: pass run failed");
        }
        if (!res.changed)
        {
            return fail("array basic: expected changes");
        }

        const std::string memName = "lane_q__laneagg";
        const grh::OperationId memOpId = graph.findOperation(memName);
        if (!memOpId.valid() || graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array basic: kMemory missing");
        }
        const grh::Operation memOp = graph.getOperation(memOpId);
        const auto memWidth = getIntAttr(memOp, "width");
        const auto memRow = getIntAttr(memOp, "row");
        if (!memWidth || *memWidth != 4 || !memRow || *memRow != 8)
        {
            return fail("array basic: kMemory must have width=4 row=8");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 0 ||
            countOpsOfKind(graph, grh::OperationKind::kRegisterWritePort) != 0 ||
            countOpsOfKind(graph, grh::OperationKind::kRegisterReadPort) != 0)
        {
            return fail("array basic: no kRegister / register ports may survive");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kReplicate) != 0 ||
            countOpsOfKind(graph, grh::OperationKind::kSliceStatic) != 0 ||
            countOpsOfKind(graph, grh::OperationKind::kShl) != 0)
        {
            return fail("array basic: wide-mode artifacts must not appear");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayReadAllPort) != 1 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayWritePort) != 1 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayMux) != 1 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayLaneConst) != 2 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayBroadcast) != 2 ||
            countOpsOfKind(graph, grh::OperationKind::kMemoryReadPort) != 8)
        {
            return fail("array basic: unexpected array op counts");
        }

        // Whole-array read port.
        const grh::OperationId readAllId =
            findPortOp(graph, grh::OperationKind::kArrayReadAllPort, "memSymbol", memName);
        if (!readAllId.valid())
        {
            return fail("array basic: kArrayReadAllPort missing");
        }
        const grh::Operation readAllOp = graph.getOperation(readAllId);
        if (readAllOp.results().size() != 1 || graph.getValue(readAllOp.results().front()).width() != 32)
        {
            return fail("array basic: kArrayReadAllPort result must be 32 bits");
        }
        const grh::ValueId readAllValue = readAllOp.results().front();

        // Write port: [laneMask, data, clk], eventEdge=[posedge].
        const grh::OperationId writeId =
            findPortOp(graph, grh::OperationKind::kArrayWritePort, "memSymbol", memName);
        if (!writeId.valid())
        {
            return fail("array basic: kArrayWritePort missing");
        }
        const grh::Operation writeOp = graph.getOperation(writeId);
        const auto writeOperands = writeOp.operands();
        if (writeOperands.size() != 3 || writeOperands[2] != clk)
        {
            return fail("array basic: kArrayWritePort must be [laneMask, data, clk]");
        }
        const auto edges = writeOp.attr("eventEdge");
        const auto *edgeList = edges ? std::get_if<std::vector<std::string>>(&*edges) : nullptr;
        if (!edgeList || edgeList->size() != 1 || edgeList->front() != "posedge")
        {
            return fail("array basic: kArrayWritePort must keep eventEdge=[posedge]");
        }
        // laneMask = kAnd(condVec, present): condVec = kArrayBroadcast(en1),
        // present = kArrayLaneConst(elemWidth=1, rows=8, values all 1).
        const grh::OperationId laneMaskDef = graph.getValue(writeOperands[0]).definingOp();
        if (!laneMaskDef.valid() || graph.getOperation(laneMaskDef).kind() != grh::OperationKind::kAnd)
        {
            return fail("array basic: laneMask must be a kAnd");
        }
        const grh::Operation laneMaskAnd = graph.getOperation(laneMaskDef);
        const grh::OperationId condDef = graph.getValue(laneMaskAnd.operands()[0]).definingOp();
        if (!condDef.valid() || graph.getOperation(condDef).kind() != grh::OperationKind::kArrayBroadcast ||
            graph.getOperation(condDef).operands().front() != en1)
        {
            return fail("array basic: laneMask cond side must be kArrayBroadcast(en1)");
        }
        const grh::OperationId presentDef = graph.getValue(laneMaskAnd.operands()[1]).definingOp();
        if (!presentDef.valid() || graph.getOperation(presentDef).kind() != grh::OperationKind::kArrayLaneConst)
        {
            return fail("array basic: laneMask present side must be kArrayLaneConst");
        }
        const grh::Operation presentOp = graph.getOperation(presentDef);
        const auto presentRows = getIntAttr(presentOp, "rows");
        const auto presentElemWidth = getIntAttr(presentOp, "elemWidth");
        const auto presentValues = getIntListAttr(presentOp, "values");
        if (!presentRows || *presentRows != 8 || !presentElemWidth || *presentElemWidth != 1 ||
            !presentValues || presentValues->size() != 8)
        {
            return fail("array basic: present kArrayLaneConst must be rows=8 elemWidth=1");
        }
        for (const int64_t bit : *presentValues)
        {
            if (bit != 1)
            {
                return fail("array basic: present values must be all ones");
            }
        }
        // data = kArrayMux(sel=kArrayBroadcast(en1), t=kXor(...), f=readAll).
        const grh::OperationId dataDef = graph.getValue(writeOperands[1]).definingOp();
        if (!dataDef.valid() || graph.getOperation(dataDef).kind() != grh::OperationKind::kArrayMux)
        {
            return fail("array basic: write data must be a kArrayMux");
        }
        const grh::Operation muxOp = graph.getOperation(dataDef);
        if (muxOp.operands().size() != 3 || muxOp.operands()[2] != readAllValue)
        {
            return fail("array basic: kArrayMux false side must be the read-all value");
        }
        if (muxOp.operands()[0] != laneMaskAnd.operands()[0])
        {
            return fail("array basic: kArrayMux select must be the shared kArrayBroadcast(en1)");
        }
        const grh::OperationId trueDef = graph.getValue(muxOp.operands()[1]).definingOp();
        if (!trueDef.valid() || graph.getOperation(trueDef).kind() != grh::OperationKind::kXor)
        {
            return fail("array basic: kArrayMux true side must be the widened kXor");
        }
        // Affine constant leaf -> kArrayLaneConst values [0..7].
        bool foundTable = false;
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kArrayLaneConst)
            {
                continue;
            }
            const auto elemWidth = getIntAttr(op, "elemWidth");
            const auto values = getIntListAttr(op, "values");
            if (!elemWidth || *elemWidth != 4 || !values || values->size() != 8)
            {
                continue;
            }
            bool orderOk = true;
            for (std::size_t k = 0; k < 8; ++k)
            {
                if ((*values)[k] != static_cast<int64_t>(k))
                {
                    orderOk = false;
                    break;
                }
            }
            foundTable = foundTable || orderOk;
        }
        if (!foundTable)
        {
            return fail("array basic: affine kArrayLaneConst table [0..7] missing");
        }

        // Read side: every lane read became kMemoryReadPort(mem, constant i).
        for (std::size_t i = 0; i < group.reads.size(); ++i)
        {
            const grh::ValueId outValue = graph.outputPortValue("out_" + std::to_string(i));
            if (!outValue.valid())
            {
                return fail("array basic: output port missing after rewrite");
            }
            const grh::OperationId readDef = graph.getValue(outValue).definingOp();
            if (!readDef.valid() || graph.getOperation(readDef).kind() != grh::OperationKind::kMemoryReadPort)
            {
                return fail("array basic: lane read must become kMemoryReadPort");
            }
            const grh::Operation read = graph.getOperation(readDef);
            if (getStringAttr(read, "memSymbol") != memName)
            {
                return fail("array basic: lane read must target the merged kMemory");
            }
            if (read.operands().size() != 1 || graph.getValue(read.results().front()).width() != 4)
            {
                return fail("array basic: lane read must be a 4-bit single-address read");
            }
            const auto addr = constResultAsUInt(graph, read.operands().front());
            if (!addr || *addr != i)
            {
                return fail("array basic: lane read address must be the constant lane index");
            }
        }

        // Report carries the output mode.
        const auto reportIt = session.find("la.report");
        if (reportIt == session.end() || !reportIt->second)
        {
            return fail("array basic: missing lane-aggregate.reports session value");
        }
        const auto *reportValue =
            dynamic_cast<const SessionSlotValue<std::string> *>(reportIt->second.get());
        if (!reportValue || reportValue->value.find("\"output_mode\":\"array\"") == std::string::npos)
        {
            return fail("array basic: report must record output_mode=array");
        }

        if (!roundTripJson(design))
        {
            return fail("array basic: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode lane-0 specialization: the majority bucket merges with a hole
    // at index 0; the present-lanes guard masks the hole lane.
    int testArrayLaneZeroSpecialization()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId enW = makeInput(graph, "enW", 4);
        const grh::ValueId rst = makeInput(graph, "rst", 1);

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

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array lane-0: expected changes");
        }

        const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
        if (!memOpId.valid() || graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array lane-0: kMemory missing");
        }
        const auto memRow = getIntAttr(graph.getOperation(memOpId), "row");
        if (!memRow || *memRow != 9)
        {
            return fail("array lane-0: kMemory must have row=9 (span over the hole)");
        }
        if (!graph.findOperation("lane_0_q").valid() ||
            countOpsOfKind(graph, grh::OperationKind::kRegister) != 1)
        {
            return fail("array lane-0: specialized lane register must survive");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegisterReadPort) != 1)
        {
            return fail("array lane-0: lane 0 read port must survive");
        }
        // present-lanes guard: bit 0 (hole) cleared, bits 1..8 set.
        bool foundPresent = false;
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kArrayLaneConst)
            {
                continue;
            }
            const auto elemWidth = getIntAttr(op, "elemWidth");
            const auto values = getIntListAttr(op, "values");
            if (!elemWidth || *elemWidth != 1 || !values || values->size() != 9)
            {
                continue;
            }
            if ((*values)[0] == 0)
            {
                bool restOk = true;
                for (std::size_t k = 1; k < 9; ++k)
                {
                    if ((*values)[k] != 1)
                    {
                        restOk = false;
                        break;
                    }
                }
                foundPresent = foundPresent || restOk;
            }
        }
        if (!foundPresent)
        {
            return fail("array lane-0: present guard must mask the hole lane (values 0,1,1,...)");
        }
        // Affine table: hole lane 0 zeroed, lane i holds i.
        bool foundTable = false;
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kArrayLaneConst)
            {
                continue;
            }
            const auto elemWidth = getIntAttr(op, "elemWidth");
            const auto values = getIntListAttr(op, "values");
            if (!elemWidth || *elemWidth != 4 || !values || values->size() != 9)
            {
                continue;
            }
            if ((*values)[0] == 0 && (*values)[8] == 8)
            {
                foundTable = true;
            }
        }
        if (!foundTable)
        {
            return fail("array lane-0: affine kArrayLaneConst must zero the hole lane");
        }
        if (!roundTripJson(design))
        {
            return fail("array lane-0: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode eq-onehot: kEq(ptr, i) materializes as kArrayOnehot(ptr).
    int testArrayEqOnehot()
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

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array eq-onehot: expected the group to merge");
        }
        const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
        if (!memOpId.valid() || graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array eq-onehot: kMemory missing");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kShl) != 0)
        {
            return fail("array eq-onehot: no kShl may be produced");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayOnehot) != 1)
        {
            return fail("array eq-onehot: expected exactly one kArrayOnehot");
        }
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kArrayOnehot)
            {
                continue;
            }
            const auto rows = getIntAttr(op, "rows");
            if (op.operands().size() != 1 || op.operands().front() != ptr || !rows || *rows != 8 ||
                graph.getValue(op.results().front()).width() != 8)
            {
                return fail("array eq-onehot: kArrayOnehot(ptr) must have rows=8 and an 8-bit result");
            }
        }
        // The write data must be the onehot vector directly.
        const grh::OperationId writeId =
            findPortOp(graph, grh::OperationKind::kArrayWritePort, "memSymbol", "lane_q__laneagg");
        if (!writeId.valid())
        {
            return fail("array eq-onehot: kArrayWritePort missing");
        }
        const grh::OperationId dataDef =
            graph.getValue(graph.getOperation(writeId).operands()[1]).definingOp();
        if (!dataDef.valid() || graph.getOperation(dataDef).kind() != grh::OperationKind::kArrayOnehot)
        {
            return fail("array eq-onehot: write data must be the kArrayOnehot result");
        }
        if (!roundTripJson(design))
        {
            return fail("array eq-onehot: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode lane-parameter leaves: per-lane distinct values have no
    // constant-table form, so the per-lane kConcat is kept (kArrayLaneConst
    // only covers constant leaves).
    int testArrayLaneParamConcat()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
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

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array lane-param: expected the group to merge");
        }
        if (!graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("array lane-param: kMemory missing");
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
            return fail("array lane-param: expected per-lane kConcat in lane order");
        }
        // Only the present-lanes guard constant may be a kArrayLaneConst.
        if (countOpsOfKind(graph, grh::OperationKind::kArrayLaneConst) != 1)
        {
            return fail("array lane-param: lane parameters must not become kArrayLaneConst");
        }
        if (!roundTripJson(design))
        {
            return fail("array lane-param: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode shared register read: one plain kRegisterReadPort of R
    // broadcast to all lanes via kArrayBroadcast.
    int testArraySharedRegRead()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        makeRegister(graph, "shared_r", 4);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            const grh::ValueId self = makeRead(graph, name, 4);
            const grh::ValueId sharedRead = makeRead(graph, "shared_r", 4);
            const grh::ValueId data =
                makeBinary(graph, grh::OperationKind::kXor, sharedRead, self, 4);
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array shared-reg: expected the group to merge");
        }
        if (!graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("array shared-reg: kMemory missing");
        }
        if (!graph.findOperation("shared_r").valid())
        {
            return fail("array shared-reg: the shared register must survive");
        }
        bool foundBroadcast = false;
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() != grh::OperationKind::kArrayBroadcast || op.operands().size() != 1)
            {
                continue;
            }
            const grh::OperationId baseDef = graph.getValue(op.operands().front()).definingOp();
            if (baseDef.valid() &&
                graph.getOperation(baseDef).kind() == grh::OperationKind::kRegisterReadPort &&
                getStringAttr(graph.getOperation(baseDef), "regSymbol") == "shared_r")
            {
                foundBroadcast = true;
                break;
            }
        }
        if (!foundBroadcast)
        {
            return fail("array shared-reg: expected kArrayBroadcast of one kRegisterReadPort(shared_r)");
        }
        if (!roundTripJson(design))
        {
            return fail("array shared-reg: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode phase 2: a mux chain over lane reads becomes one
    // kMemoryReadPort with the select pointer as the dynamic row address.
    int testArrayReadSelectMuxChain()
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

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array read-select: expected changes");
        }
        if (!graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("array read-select: kMemory missing");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kSliceDynamic) != 0 ||
            countOpsOfKind(graph, grh::OperationKind::kShl) != 0)
        {
            return fail("array read-select: no kSliceDynamic / offset scaling may be produced");
        }
        const grh::ValueId outValue = graph.outputPortValue("sel_out");
        if (!outValue.valid())
        {
            return fail("array read-select: output missing");
        }
        const grh::OperationId outDef = graph.getValue(outValue).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != grh::OperationKind::kMemoryReadPort)
        {
            return fail("array read-select: output must be a kMemoryReadPort");
        }
        const grh::Operation read = graph.getOperation(outDef);
        if (read.operands().size() != 1 || read.operands().front() != ptr)
        {
            return fail("array read-select: read address must be the select pointer directly");
        }
        if (getStringAttr(read, "memSymbol") != "lane_q__laneagg" ||
            graph.getValue(read.results().front()).width() != 4)
        {
            return fail("array read-select: read must target the merged kMemory with width 4");
        }
        if (!roundTripJson(design))
        {
            return fail("array read-select: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode pre-pass: kReduce{Or,And,Xor}(kConcat(...)) over uniform
    // elements becomes one kArrayReduce* of the packed concat value.
    int testArrayReduceConcat()
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

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array reduce-concat: expected the group to merge");
        }
        if (!graph.findOperation("lane_q__laneagg").valid())
        {
            return fail("array reduce-concat: kMemory missing");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayReduceOr) != 1)
        {
            return fail("array reduce-concat: expected exactly one kArrayReduceOr");
        }
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() == grh::OperationKind::kArrayReduceOr)
            {
                const auto elemWidth = getIntAttr(op, "elemWidth");
                if (!elemWidth || *elemWidth != 1 || op.operands().size() != 1)
                {
                    return fail("array reduce-concat: kArrayReduceOr must have elemWidth=1");
                }
                const grh::OperationId dataDef = graph.getValue(op.operands().front()).definingOp();
                if (!dataDef.valid() || graph.getOperation(dataDef).kind() != grh::OperationKind::kConcat)
                {
                    return fail("array reduce-concat: kArrayReduceOr operand must be the packed concat");
                }
                if (graph.getValue(op.results().front()).users().empty())
                {
                    return fail("array reduce-concat: kArrayReduceOr result must be live");
                }
            }
            if (op.kind() == grh::OperationKind::kReduceOr && op.operands().size() == 1)
            {
                // The original packed reduce must be dead now.
                if (!graph.getValue(op.results().front()).users().empty())
                {
                    return fail("array reduce-concat: packed kReduceOr must be replaced");
                }
            }
        }
        if (!roundTripJson(design))
        {
            return fail("array reduce-concat: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode per-lane reduction cone (robEntries shape): a per-lane
    // kReduceOr(kConcat(1-bit elements)) with lane-varying elements normalizes
    // to a per-lane kArrayReduceOr; the group merges and the reduction
    // materializes as kArrayReduceLanesOr over the packed rows.
    int testArrayPerLaneReduceMerge()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const std::vector<uint64_t> indices{0, 1, 2, 3, 4, 5, 6, 7};
        // Sibling 1-bit group with a trivial cone (merges independently).
        for (const uint64_t idx : indices)
        {
            const std::string name = "flag_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            makeWrite(graph, name, en1, self, {clk}, {"posedge"});
        }
        for (const uint64_t idx : indices)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 1);
            const grh::ValueId self = makeRead(graph, name, 1);
            const grh::ValueId flag = makeRead(graph, "flag_" + std::to_string(idx) + "_q", 1);
            const grh::ValueId packed = makeConcat(graph, {self, flag, en1}, 3);
            const grh::ValueId reduced =
                makeUnary(graph, grh::OperationKind::kReduceOr, packed, 1);
            makeWrite(graph, name, en1, reduced, {clk}, {"posedge"});
        }

        if (const int rc = runPass(design, arrayOptions()); rc == 1)
        {
            return rc;
        }
        else if (rc == 0)
        {
            return fail("array per-lane reduce: expected the group to merge");
        }
        if (!graph.findOperation("lane_q__laneagg").valid() ||
            !graph.findOperation("flag_q__laneagg").valid())
        {
            return fail("array per-lane reduce: merged kMemory missing");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayReduceLanesOr) != 1)
        {
            return fail("array per-lane reduce: expected exactly one kArrayReduceLanesOr");
        }
        bool writeChecked = false;
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            if (op.kind() == grh::OperationKind::kArrayReduceLanesOr)
            {
                const auto elemWidth = getIntAttr(op, "elemWidth");
                if (!elemWidth || *elemWidth != 3 || op.operands().size() != 1)
                {
                    return fail("array per-lane reduce: kArrayReduceLanesOr must have elemWidth=3");
                }
                if (graph.getValue(op.results().front()).width() != 8)
                {
                    return fail("array per-lane reduce: kArrayReduceLanesOr result must be the 8-bit guard vector");
                }
                const grh::ValueId data = op.operands().front();
                const grh::OperationId dataDef = graph.getValue(data).definingOp();
                if (!dataDef.valid() || graph.getOperation(dataDef).kind() != grh::OperationKind::kConcat ||
                    graph.getValue(data).width() != 24)
                {
                    return fail("array per-lane reduce: kArrayReduceLanesOr operand must be the packed 24-bit rows");
                }
            }
            if (op.kind() == grh::OperationKind::kArrayWritePort &&
                getStringAttr(op, "memSymbol") == "lane_q__laneagg")
            {
                writeChecked = true;
                const grh::OperationId dataDef = graph.getValue(op.operands()[1]).definingOp();
                if (!dataDef.valid() ||
                    graph.getOperation(dataDef).kind() != grh::OperationKind::kArrayReduceLanesOr)
                {
                    return fail("array per-lane reduce: write data must be the kArrayReduceLanesOr guard vector");
                }
            }
            if (op.kind() == grh::OperationKind::kArrayReduceOr &&
                !graph.getValue(op.results().front()).users().empty())
            {
                return fail("array per-lane reduce: per-lane kArrayReduceOr must be dead after the merge");
            }
        }
        if (!writeChecked)
        {
            return fail("array per-lane reduce: merged kArrayWritePort missing");
        }
        if (!roundTripJson(design))
        {
            return fail("array per-lane reduce: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode init packing: per-lane constant inits become kMemory literal
    // init rows; non-affine / non-constant inits stay rejected.
    int testArrayInitValuePacking()
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
                    graph.setAttr(reg, "initValue", "4'd" + std::to_string(idx));
                }
                else if (variant == 2)
                {
                    graph.setAttr(reg, "initValue", "4'd" + std::to_string(idx * idx));
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
            LaneAggregateOptions options = arrayOptions();
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
            const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
            if (variant <= 1)
            {
                if (!res.changed || !memOpId.valid() ||
                    graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
                {
                    failMsg = "expected merge";
                    return -1;
                }
                const grh::Operation memOp = graph.getOperation(memOpId);
                const auto initKinds = getStringListAttr(memOp, "initKind");
                const auto initFiles = getStringListAttr(memOp, "initFile");
                const auto initValues = getStringListAttr(memOp, "initValue");
                const auto initStarts = getIntListAttr(memOp, "initStart");
                const auto initLens = getIntListAttr(memOp, "initLen");
                if (!initKinds || initKinds->size() != 8 || !initFiles || initFiles->size() != 8 ||
                    !initValues || initValues->size() != 8 || !initStarts || initStarts->size() != 8 ||
                    !initLens || initLens->size() != 8)
                {
                    failMsg = "kMemory init attr arrays must have one entry per row";
                    return -1;
                }
                for (std::size_t row = 0; row < 8; ++row)
                {
                    if ((*initKinds)[row] != "literal" || (*initStarts)[row] != static_cast<int64_t>(row) ||
                        (*initLens)[row] != 1)
                    {
                        failMsg = "kMemory init rows must be literal row inits";
                        return -1;
                    }
                    const auto rowValue = parseLiteralUInt((*initValues)[row]);
                    const uint64_t expect = variant == 0 ? 0 : row;
                    if (!rowValue || *rowValue != expect)
                    {
                        failMsg = "kMemory init row value mismatch";
                        return -1;
                    }
                }
                if (!roundTripJson(design))
                {
                    failMsg = "store/load round trip failed";
                    return -1;
                }
                return 0;
            }
            if (res.changed || countOpsOfKind(graph, grh::OperationKind::kMemory) != 0 ||
                countOpsOfKind(graph, grh::OperationKind::kRegister) != 8)
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
                return fail("array init-packing: " + failMsg);
            }
        }
        return 0;
    }

    // Array-mode output flows through simplify (const-fold / redundant-elim /
    // dead-code-elim) without breaking the array shapes.
    int testArraySimplifySurvives()
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

        PassManager manager;
        manager.addPass(std::make_unique<LaneAggregatePass>(arrayOptions()));
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("array simplify: pass run failed");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kMemory) != 1 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayWritePort) != 1 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayReadAllPort) != 1 ||
            countOpsOfKind(graph, grh::OperationKind::kArrayMux) != 1)
        {
            return fail("array simplify: array shapes must survive simplify");
        }
        const grh::OperationId writeId =
            findPortOp(graph, grh::OperationKind::kArrayWritePort, "memSymbol", "lane_q__laneagg");
        if (!writeId.valid())
        {
            return fail("array simplify: kArrayWritePort missing");
        }
        const grh::Operation writeOp = graph.getOperation(writeId);
        const grh::OperationId laneMaskDef = graph.getValue(writeOp.operands()[0]).definingOp();
        const grh::OperationId dataDef = graph.getValue(writeOp.operands()[1]).definingOp();
        if (!laneMaskDef.valid() || graph.getOperation(laneMaskDef).kind() != grh::OperationKind::kAnd ||
            !dataDef.valid() || graph.getOperation(dataDef).kind() != grh::OperationKind::kArrayMux)
        {
            return fail("array simplify: write port laneMask/data shape changed");
        }
        for (std::size_t i = 0; i < group.reads.size(); ++i)
        {
            const grh::ValueId outValue = graph.outputPortValue("out_" + std::to_string(i));
            if (!outValue.valid())
            {
                return fail("array simplify: output port missing after simplify");
            }
            const grh::OperationId readDef = graph.getValue(outValue).definingOp();
            if (!readDef.valid() || graph.getOperation(readDef).kind() != grh::OperationKind::kMemoryReadPort)
            {
                return fail("array simplify: lane reads must stay kMemoryReadPort");
            }
        }
        if (!roundTripJson(design))
        {
            return fail("array simplify: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode exact-all fallback, form A: same shared single-member read
    // signature split as the wide variant; the exact check merges all lanes
    // into one kMemory.
    int testArrayExactFallbackSharedLeaf()
    {
        grh::Design design;
        buildSharedLeafFallbackGroup(design);
        grh::Graph &graph = *design.findGraph("g");
        std::string report;
        if (runPassWithReport(design, arrayOptions(), report) != 2)
        {
            return fail("array exact-fallback shared-leaf: expected the group to merge");
        }
        const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
        if (!memOpId.valid() ||
            graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array exact-fallback shared-leaf: kMemory missing");
        }
        const grh::Operation memOp = graph.getOperation(memOpId);
        const auto memWidth = getIntAttr(memOp, "width");
        const auto memRow = getIntAttr(memOp, "row");
        if (!memWidth || *memWidth != 4 || !memRow || *memRow != 8)
        {
            return fail("array exact-fallback shared-leaf: kMemory must have width=4 row=8");
        }
        // The shared register stays a scalar kRegister.
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 1 ||
            !graph.findOperation("solo_0_q").valid())
        {
            return fail("array exact-fallback shared-leaf: solo_0_q must stay scalar");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayWritePort) != 1)
        {
            return fail("array exact-fallback shared-leaf: expected one kArrayWritePort");
        }
        if (report.find("\"reject_reason\":\"no_majority_exact\"") == std::string::npos)
        {
            return fail("array exact-fallback shared-leaf: report must record "
                        "no_majority_exact");
        }
        if (!roundTripJson(design))
        {
            return fail("array exact-fallback shared-leaf: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode exact-all fallback, form B: the per-lane full-range sibling
    // reduction normalizes to kArrayReduceOr over a per-lane kConcat
    // (lane-parameter leaf) and materializes as one kArrayReduceLanesOr over
    // the packed rows.
    int testArrayExactFallbackSiblingReduce()
    {
        grh::Design design;
        buildSiblingReduceFallbackGroup(design);
        grh::Graph &graph = *design.findGraph("g");
        std::string report;
        if (runPassWithReport(design, arrayOptions(), report) != 2)
        {
            return fail("array exact-fallback sibling-reduce: expected the group to merge");
        }
        const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
        if (!memOpId.valid() ||
            graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array exact-fallback sibling-reduce: kMemory missing");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayReduceLanesOr) != 1)
        {
            return fail("array exact-fallback sibling-reduce: expected one "
                        "kArrayReduceLanesOr");
        }
        // The 8 sibling lanes stay scalar (non-affine constants).
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 8 ||
            !graph.findOperation("sib_5_q").valid())
        {
            return fail("array exact-fallback sibling-reduce: siblings must stay scalar");
        }
        if (report.find("\"reject_reason\":\"no_majority_exact\"") == std::string::npos)
        {
            return fail("array exact-fallback sibling-reduce: report must record "
                        "no_majority_exact");
        }
        if (!roundTripJson(design))
        {
            return fail("array exact-fallback sibling-reduce: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode C-level lane-parameter leaf: the per-lane wide kEq cond
    // packs into one per-lane kConcat feeding the kArrayWritePort laneMask.
    int testArrayLaneParamLeafNonPointwise()
    {
        grh::Design design;
        const std::vector<grh::ValueId> eqs = buildLaneParamEqGroup(design);
        grh::Graph &graph = *design.findGraph("g");
        if (const int rc = runPass(design, arrayOptions()); rc != 2)
        {
            return rc == 1 ? rc : fail("array lane-param leaf: expected the group to merge");
        }
        const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
        if (!memOpId.valid() ||
            graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array lane-param leaf: kMemory missing");
        }
        if (!hasLaneParamConcat(graph, eqs))
        {
            return fail("array lane-param leaf: expected per-lane kConcat of the kEq leaves");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kArrayWritePort) != 1)
        {
            return fail("array lane-param leaf: expected one kArrayWritePort");
        }
        // The frozen per-lane kEq ops survive and read kMemoryReadPort rows.
        for (const grh::ValueId eq : eqs)
        {
            const grh::OperationId eqDef = graph.getValue(eq).definingOp();
            if (!eqDef.valid() || graph.getOperation(eqDef).kind() != grh::OperationKind::kEq)
            {
                return fail("array lane-param leaf: per-lane kEq must survive");
            }
            const grh::Operation eqOp = graph.getOperation(eqDef);
            const grh::OperationId baseDef = graph.getValue(eqOp.operands()[0]).definingOp();
            if (!baseDef.valid() ||
                graph.getOperation(baseDef).kind() != grh::OperationKind::kMemoryReadPort)
            {
                return fail("array lane-param leaf: kEq self operand must become a "
                            "kMemoryReadPort");
            }
        }
        if (!roundTripJson(design))
        {
            return fail("array lane-param leaf: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode fallback + sibling deps, negative: same shape as the wide
    // variant; A must be rejected with sibling_not_merged before any graph
    // rewrite (no kMemory created, no error diagnostics).
    int testArrayExactFallbackSiblingNotMerged()
    {
        grh::Design design;
        buildFallbackSiblingGroups(design, /*bIsomorphic=*/false);
        grh::Graph &graph = *design.findGraph("g");
        std::string report;
        // rc==1 would mean the pass reported an error; rc may be 2 because
        // the reduce-concat pre-pass normalization rewrites cones even when
        // no group merges.
        if (runPassWithReport(design, arrayOptions(), report) == 1)
        {
            return fail("array fallback sibling-reject: pass must not fail");
        }
        if (countOpsOfKind(graph, grh::OperationKind::kRegister) != 25 ||
            countOpsOfKind(graph, grh::OperationKind::kMemory) != 0)
        {
            return fail("array fallback sibling-reject: all lane registers must survive");
        }
        if (report.find("sibling_not_merged") == std::string::npos)
        {
            return fail("array fallback sibling-reject: report must record "
                        "sibling_not_merged");
        }
        if (!roundTripJson(design))
        {
            return fail("array fallback sibling-reject: store/load round trip failed");
        }
        return 0;
    }

    // Array-mode R-level affine gather: the kArrayWritePort's data is the
    // shared base vector X itself (zero-cost, no per-lane kConcat).
    int testArrayAffineGatherLeaf()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        design.markAsTop("g");
        const grh::ValueId clk = makeInput(graph, "clk", 1);
        const grh::ValueId en1 = makeInput(graph, "en1", 1);
        const grh::ValueId xvec = makeInput(graph, "xvec", 32);
        for (uint64_t idx = 0; idx < 8; ++idx)
        {
            const std::string name = "lane_" + std::to_string(idx) + "_q";
            makeRegister(graph, name, 4);
            makeRead(graph, name, 4);
            const grh::ValueId data = makeSlice(graph, xvec,
                                                static_cast<int64_t>(idx * 4),
                                                static_cast<int64_t>(idx * 4 + 3));
            makeWrite(graph, name, en1, data, {clk}, {"posedge"});
        }
        if (const int rc = runPass(design, arrayOptions()); rc != 2)
        {
            return rc == 1 ? rc : fail("array affine-gather: expected the group to merge");
        }
        const grh::OperationId memOpId = graph.findOperation("lane_q__laneagg");
        if (!memOpId.valid() ||
            graph.getOperation(memOpId).kind() != grh::OperationKind::kMemory)
        {
            return fail("array affine-gather: kMemory missing");
        }
        const grh::OperationId writeId =
            findPortOp(graph, grh::OperationKind::kArrayWritePort, "memSymbol", "lane_q__laneagg");
        if (!writeId.valid())
        {
            return fail("array affine-gather: kArrayWritePort missing");
        }
        const grh::Operation writeOp = graph.getOperation(writeId);
        if (writeOp.operands().size() < 2 || writeOp.operands()[1] != xvec)
        {
            return fail("array affine-gather: write data must be X itself");
        }
        if (!roundTripJson(design))
        {
            return fail("array affine-gather: store/load round trip failed");
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
    if (const int rc = testReadSelectShlOnehotTree())
    {
        return rc;
    }
    if (const int rc = testReadSelectShlOnehotRejects())
    {
        return rc;
    }
    if (const int rc = testShlOnehotConeMerges())
    {
        return rc;
    }
    if (const int rc = testShlOnehotConeRejects())
    {
        return rc;
    }
    if (const int rc = testShlOnehotConeDataModule())
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
    if (const int rc = testExactFallbackSharedLeaf())
    {
        return rc;
    }
    if (const int rc = testExactFallbackSiblingReduce())
    {
        return rc;
    }
    if (const int rc = testLaneParamLeafNonPointwise())
    {
        return rc;
    }
    if (const int rc = testLaneParamLeafAttrsDifferRejected())
    {
        return rc;
    }
    if (const int rc = testAffineGatherLeaf())
    {
        return rc;
    }
    if (const int rc = testExactFallbackSiblingNotMerged())
    {
        return rc;
    }
    if (const int rc = testExactFallbackSiblingMerged())
    {
        return rc;
    }
    if (const int rc = testPassRegistration())
    {
        return rc;
    }
    if (const int rc = testArrayBasicGroup())
    {
        return rc;
    }
    if (const int rc = testArrayLaneZeroSpecialization())
    {
        return rc;
    }
    if (const int rc = testArrayEqOnehot())
    {
        return rc;
    }
    if (const int rc = testArrayLaneParamConcat())
    {
        return rc;
    }
    if (const int rc = testArraySharedRegRead())
    {
        return rc;
    }
    if (const int rc = testArrayReadSelectMuxChain())
    {
        return rc;
    }
    if (const int rc = testArrayReduceConcat())
    {
        return rc;
    }
    if (const int rc = testArrayPerLaneReduceMerge())
    {
        return rc;
    }
    if (const int rc = testArrayInitValuePacking())
    {
        return rc;
    }
    if (const int rc = testArraySimplifySurvives())
    {
        return rc;
    }
    if (const int rc = testArrayExactFallbackSharedLeaf())
    {
        return rc;
    }
    if (const int rc = testArrayExactFallbackSiblingReduce())
    {
        return rc;
    }
    if (const int rc = testArrayLaneParamLeafNonPointwise())
    {
        return rc;
    }
    if (const int rc = testArrayExactFallbackSiblingNotMerged())
    {
        return rc;
    }
    if (const int rc = testArrayAffineGatherLeaf())
    {
        return rc;
    }
    return 0;
}
