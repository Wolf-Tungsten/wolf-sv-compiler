#include "core/grh.hpp"
#include "core/store.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::store;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[store_json_array_ops] " << message << '\n';
        return 1;
    }

    struct OpExpectation
    {
        std::string symbol;
        OperationKind kind;
        std::vector<int32_t> operandWidths;
        std::vector<int32_t> resultWidths;
        std::vector<AttrKV> attrs;
    };

    std::optional<std::string> checkOp(const Graph &graph, const OpExpectation &expect)
    {
        const OperationId opId = graph.findOperation(expect.symbol);
        if (!opId.valid())
        {
            return "missing op " + expect.symbol;
        }
        const Operation op = graph.getOperation(opId);
        if (op.kind() != expect.kind)
        {
            return "kind mismatch on " + expect.symbol;
        }
        if (op.operands().size() != expect.operandWidths.size())
        {
            return "operand count mismatch on " + expect.symbol;
        }
        for (std::size_t i = 0; i < expect.operandWidths.size(); ++i)
        {
            if (graph.getValue(op.operands()[i]).width() != expect.operandWidths[i])
            {
                return "operand width mismatch on " + expect.symbol;
            }
        }
        if (op.results().size() != expect.resultWidths.size())
        {
            return "result count mismatch on " + expect.symbol;
        }
        for (std::size_t i = 0; i < expect.resultWidths.size(); ++i)
        {
            if (graph.getValue(op.results()[i]).width() != expect.resultWidths[i])
            {
                return "result width mismatch on " + expect.symbol;
            }
        }
        if (op.attrs().size() != expect.attrs.size())
        {
            return "attr count mismatch on " + expect.symbol;
        }
        for (const AttrKV &kv : expect.attrs)
        {
            const std::optional<AttributeValue> actual = op.attr(kv.key);
            if (!actual || *actual != kv.value)
            {
                return "attr mismatch on " + expect.symbol + " key " + kv.key;
            }
        }
        return std::nullopt;
    }

} // namespace

int main()
{
    try
    {
        constexpr int32_t kRows = 4;
        constexpr int32_t kElemWidth = 8;
        constexpr int32_t kPackedWidth = kRows * kElemWidth;

        Design design;
        Graph &graph = design.createGraph("array_ops");

        const OperationId mem = graph.createOperation(OperationKind::kMemory, graph.internSymbol("mem"));
        graph.setAttr(mem, "width", AttributeValue(int64_t(kElemWidth)));
        graph.setAttr(mem, "row", AttributeValue(int64_t(kRows)));

        const ValueId laneMask = graph.createValue(graph.internSymbol("lane_mask"), kRows, false);
        const ValueId dataIn = graph.createValue(graph.internSymbol("data_in"), kPackedWidth, false);
        const ValueId clk = graph.createValue(graph.internSymbol("clk"), 1, false);
        const ValueId sel = graph.createValue(graph.internSymbol("sel"), kRows, false);
        const ValueId valT = graph.createValue(graph.internSymbol("val_t"), kPackedWidth, false);
        const ValueId valF = graph.createValue(graph.internSymbol("val_f"), kPackedWidth, false);
        const ValueId scalar = graph.createValue(graph.internSymbol("scalar"), kElemWidth, false);
        const ValueId index = graph.createValue(graph.internSymbol("index"), 2, false);
        graph.bindInputPort("lane_mask", laneMask);
        graph.bindInputPort("data_in", dataIn);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("sel", sel);
        graph.bindInputPort("val_t", valT);
        graph.bindInputPort("val_f", valF);
        graph.bindInputPort("scalar", scalar);
        graph.bindInputPort("index", index);

        std::vector<OpExpectation> expectations;

        const OperationId readAll =
            graph.createOperation(OperationKind::kMemoryReadAllPort, graph.internSymbol("array_read0"));
        const ValueId readAllOut = graph.createValue(graph.internSymbol("read_all"), kPackedWidth, false);
        graph.addResult(readAll, readAllOut);
        graph.setAttr(readAll, "memSymbol", AttributeValue(std::string("mem")));
        expectations.push_back(OpExpectation{"array_read0",
                                             OperationKind::kMemoryReadAllPort,
                                             {},
                                             {kPackedWidth},
                                             {{"memSymbol", AttributeValue(std::string("mem"))}}});

        const OperationId write =
            graph.createOperation(OperationKind::kMemoryWriteLanesPort, graph.internSymbol("array_write0"));
        graph.addOperand(write, laneMask);
        graph.addOperand(write, dataIn);
        graph.addOperand(write, clk);
        graph.setAttr(write, "memSymbol", AttributeValue(std::string("mem")));
        graph.setAttr(write, "eventEdge",
                      AttributeValue(std::vector<std::string>{std::string("posedge")}));
        graph.setAttr(write, std::string(kMemoryWritePriorityGroupAttr),
                      AttributeValue(std::string("writes")));
        graph.setAttr(write, std::string(kMemoryWritePriorityAttr), AttributeValue(int64_t(0)));
        expectations.push_back(
            OpExpectation{"array_write0",
                          OperationKind::kMemoryWriteLanesPort,
                          {kRows, kPackedWidth, 1},
                          {},
                          {{"memSymbol", AttributeValue(std::string("mem"))},
                           {"eventEdge",
                            AttributeValue(std::vector<std::string>{std::string("posedge")})},
                           {std::string(kMemoryWritePriorityGroupAttr),
                            AttributeValue(std::string("writes"))},
                           {std::string(kMemoryWritePriorityAttr), AttributeValue(int64_t(0))}}});

        const OperationId mux =
            graph.createOperation(OperationKind::kArrayMux, graph.internSymbol("array_mux0"));
        const ValueId muxOut = graph.createValue(graph.internSymbol("mux_out"), kPackedWidth, false);
        graph.addOperand(mux, sel);
        graph.addOperand(mux, valT);
        graph.addOperand(mux, valF);
        graph.addResult(mux, muxOut);
        expectations.push_back(OpExpectation{
            "array_mux0", OperationKind::kArrayMux, {kRows, kPackedWidth, kPackedWidth}, {kPackedWidth}, {}});

        struct ReduceCase
        {
            std::string symbol;
            OperationKind kind;
            std::string resultSymbol;
        };
        const std::vector<ReduceCase> reduceCases = {
            {"array_reduce_or0", OperationKind::kArrayReduceOr, "reduce_or_out"},
            {"array_reduce_and0", OperationKind::kArrayReduceAnd, "reduce_and_out"},
            {"array_reduce_xor0", OperationKind::kArrayReduceXor, "reduce_xor_out"},
        };
        for (const ReduceCase &reduceCase : reduceCases)
        {
            const OperationId op = graph.createOperation(reduceCase.kind, graph.internSymbol(reduceCase.symbol));
            const ValueId out = graph.createValue(graph.internSymbol(reduceCase.resultSymbol), 1, false);
            graph.addOperand(op, dataIn);
            graph.addResult(op, out);
            graph.setAttr(op, "elemWidth", AttributeValue(int64_t(kElemWidth)));
            expectations.push_back(OpExpectation{reduceCase.symbol,
                                                 reduceCase.kind,
                                                 {kPackedWidth},
                                                 {1},
                                                 {{"elemWidth", AttributeValue(int64_t(kElemWidth))}}});
        }

        const std::vector<ReduceCase> reduceLanesCases = {
            {"array_reduce_lanes_or0", OperationKind::kArrayReduceLanesOr, "reduce_lanes_or_out"},
            {"array_reduce_lanes_and0", OperationKind::kArrayReduceLanesAnd, "reduce_lanes_and_out"},
            {"array_reduce_lanes_xor0", OperationKind::kArrayReduceLanesXor, "reduce_lanes_xor_out"},
        };
        for (const ReduceCase &reduceCase : reduceLanesCases)
        {
            const OperationId op = graph.createOperation(reduceCase.kind, graph.internSymbol(reduceCase.symbol));
            const ValueId out = graph.createValue(graph.internSymbol(reduceCase.resultSymbol), kRows, false);
            graph.addOperand(op, dataIn);
            graph.addResult(op, out);
            graph.setAttr(op, "elemWidth", AttributeValue(int64_t(kElemWidth)));
            expectations.push_back(OpExpectation{reduceCase.symbol,
                                                 reduceCase.kind,
                                                 {kPackedWidth},
                                                 {kRows},
                                                 {{"elemWidth", AttributeValue(int64_t(kElemWidth))}}});
        }

        const OperationId broadcast =
            graph.createOperation(OperationKind::kArrayBroadcast, graph.internSymbol("array_broadcast0"));
        const ValueId broadcastOut =
            graph.createValue(graph.internSymbol("broadcast_out"), kPackedWidth, false);
        graph.addOperand(broadcast, scalar);
        graph.addResult(broadcast, broadcastOut);
        graph.setAttr(broadcast, "rows", AttributeValue(int64_t(kRows)));
        expectations.push_back(OpExpectation{"array_broadcast0",
                                             OperationKind::kArrayBroadcast,
                                             {kElemWidth},
                                             {kPackedWidth},
                                             {{"rows", AttributeValue(int64_t(kRows))}}});

        const OperationId laneConst =
            graph.createOperation(OperationKind::kArrayLaneConst, graph.internSymbol("array_lane_const0"));
        const ValueId laneConstOut =
            graph.createValue(graph.internSymbol("lane_const_out"), kPackedWidth, false);
        graph.addResult(laneConst, laneConstOut);
        graph.setAttr(laneConst, "elemWidth", AttributeValue(int64_t(kElemWidth)));
        graph.setAttr(laneConst, "rows", AttributeValue(int64_t(kRows)));
        graph.setAttr(laneConst, "values", AttributeValue(std::vector<int64_t>{1, 2, 3, 4}));
        expectations.push_back(OpExpectation{"array_lane_const0",
                                             OperationKind::kArrayLaneConst,
                                             {},
                                             {kPackedWidth},
                                             {{"elemWidth", AttributeValue(int64_t(kElemWidth))},
                                              {"rows", AttributeValue(int64_t(kRows))},
                                              {"values", AttributeValue(std::vector<int64_t>{1, 2, 3, 4})}}});

        const OperationId onehot =
            graph.createOperation(OperationKind::kArrayOnehot, graph.internSymbol("array_onehot0"));
        const ValueId onehotOut = graph.createValue(graph.internSymbol("onehot_out"), kRows, false);
        graph.addOperand(onehot, index);
        graph.addResult(onehot, onehotOut);
        graph.setAttr(onehot, "rows", AttributeValue(int64_t(kRows)));
        expectations.push_back(OpExpectation{"array_onehot0",
                                             OperationKind::kArrayOnehot,
                                             {2},
                                             {kRows},
                                             {{"rows", AttributeValue(int64_t(kRows))}}});

        graph.bindOutputPort("read_all", readAllOut);
        graph.bindOutputPort("mux_out", muxOut);
        graph.bindOutputPort("onehot_out", onehotOut);

        design.markAsTop(graph.symbol());

        for (const OpExpectation &expect : expectations)
        {
            const OperationKind parsed = parseOperationKind(toString(expect.kind)).value_or(OperationKind::kAssign);
            if (parsed != expect.kind)
            {
                return fail("toString/parseOperationKind roundtrip failed for " + expect.symbol);
            }
        }

        StoreDiagnostics diagnostics;
        StoreJson emitter(&diagnostics);
        const std::optional<std::string> jsonText = emitter.storeToString(design);
        if (!jsonText || diagnostics.hasError())
        {
            return fail("storeToString failed");
        }
        if (jsonText->find("\"kMemoryWriteLanesPort\"") == std::string::npos ||
            jsonText->find("\"kArrayLaneConst\"") == std::string::npos)
        {
            return fail("array op kinds missing in JSON output");
        }

        Design parsed = Design::fromJsonString(*jsonText);
        const Graph *loaded = parsed.findGraph("array_ops");
        if (!loaded)
        {
            return fail("parsed design missing array_ops graph");
        }
        for (const OpExpectation &expect : expectations)
        {
            if (const std::optional<std::string> mismatch = checkOp(*loaded, expect))
            {
                return fail(*mismatch);
            }
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception: ") + ex.what());
    }
    return 0;
}
