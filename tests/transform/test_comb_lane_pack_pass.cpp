#include "core/grh.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "transform/comb_lane_pack.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[comb-lane-pack-tests] " << message << '\n';
        return 1;
    }

    wolvrix::lib::grh::ValueId makeLogicValue(wolvrix::lib::grh::Graph &graph,
                                              const std::string &name,
                                              int32_t width)
    {
        return graph.createValue(graph.internSymbol(name), width, false);
    }

    wolvrix::lib::grh::ValueId makeUnary(wolvrix::lib::grh::Graph &graph,
                                         wolvrix::lib::grh::OperationKind kind,
                                         const std::string &valueName,
                                         const std::string &opName,
                                         wolvrix::lib::grh::ValueId operand,
                                         int32_t width)
    {
        auto value = makeLogicValue(graph, valueName, width);
        auto op = graph.createOperation(kind, graph.internSymbol(opName));
        graph.addOperand(op, operand);
        graph.addResult(op, value);
        return value;
    }

    wolvrix::lib::grh::ValueId makeBinary(wolvrix::lib::grh::Graph &graph,
                                          wolvrix::lib::grh::OperationKind kind,
                                          const std::string &valueName,
                                          const std::string &opName,
                                          wolvrix::lib::grh::ValueId lhs,
                                          wolvrix::lib::grh::ValueId rhs,
                                          int32_t width)
    {
        auto value = makeLogicValue(graph, valueName, width);
        auto op = graph.createOperation(kind, graph.internSymbol(opName));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, value);
        return value;
    }

    wolvrix::lib::grh::ValueId makeMux(wolvrix::lib::grh::Graph &graph,
                                       const std::string &valueName,
                                       const std::string &opName,
                                       wolvrix::lib::grh::ValueId sel,
                                       wolvrix::lib::grh::ValueId whenTrue,
                                       wolvrix::lib::grh::ValueId whenFalse,
                                       int32_t width)
    {
        auto value = makeLogicValue(graph, valueName, width);
        auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kMux,
                                        graph.internSymbol(opName));
        graph.addOperand(op, sel);
        graph.addOperand(op, whenTrue);
        graph.addOperand(op, whenFalse);
        graph.addResult(op, value);
        return value;
    }

    std::size_t countKind(const wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::OperationKind kind)
    {
        std::size_t count = 0;
        for (auto opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    std::size_t countOps(const wolvrix::lib::grh::Graph &graph)
    {
        std::size_t count = 0;
        for (auto opId : graph.operations())
        {
            (void)opId;
            ++count;
        }
        return count;
    }

} // namespace

int main()
{
    // Case 1: repeated declared bitwise roots pack into one wide pointwise cone.
    {
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("bitwise");
        for (int lane = 0; lane < 4; ++lane)
        {
            const auto a = makeLogicValue(graph, "a" + std::to_string(lane), 8);
            const auto b = makeLogicValue(graph, "b" + std::to_string(lane), 8);
            const auto c = makeLogicValue(graph, "c" + std::to_string(lane), 8);
            graph.bindInputPort("a" + std::to_string(lane), a);
            graph.bindInputPort("b" + std::to_string(lane), b);
            graph.bindInputPort("c" + std::to_string(lane), c);

            const auto andValue = makeBinary(graph,
                                             wolvrix::lib::grh::OperationKind::kAnd,
                                             "lane_and_" + std::to_string(lane),
                                             "lane_and_op_" + std::to_string(lane),
                                             a,
                                             b,
                                             8);
            const std::string rootName = "lane_out_" + std::to_string(lane);
            const auto root = makeBinary(graph,
                                         wolvrix::lib::grh::OperationKind::kOr,
                                         rootName,
                                         "lane_or_op_" + std::to_string(lane),
                                         andValue,
                                         c,
                                         8);
            graph.addDeclaredSymbol(graph.lookupSymbol(rootName));
            graph.bindOutputPort(rootName, root);
        }

        PassManager manager;
        wolvrix::lib::transform::SessionStore session;
        manager.options().session = &session;
        CombLanePackOptions options;
        options.outputKey = "comb_lane_pack.report";
        manager.addPass(std::make_unique<CombLanePackPass>(options));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("Expected bitwise comb-lane-pack to succeed");
        }
        if (!result.changed)
        {
            return fail("Expected bitwise comb-lane-pack to change graph");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kAnd) != 1)
        {
            return fail("Expected one packed kAnd after bitwise rewrite");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kOr) != 1)
        {
            return fail("Expected one packed kOr after bitwise rewrite");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kSliceStatic) != 4)
        {
            return fail("Expected four result slices after bitwise rewrite");
        }
        const auto reportIt = session.find("comb_lane_pack.report");
        if (reportIt == session.end() || !reportIt->second)
        {
            return fail("Expected comb-lane-pack report in session");
        }
        const auto *typed =
            dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<
                std::vector<wolvrix::lib::transform::CombLanePackReport>> *>(reportIt->second.get());
        if (!typed)
        {
            return fail("Unexpected comb-lane-pack report session type");
        }
        if (typed->value.size() != 1)
        {
            return fail("Expected one comb-lane-pack report entry");
        }
        const auto &report = typed->value.front();
        if (report.graphName != "bitwise" ||
            report.rootSource != "declared" ||
            report.groupSize != 4 ||
            report.laneWidth != 8 ||
            report.packedWidth != 32 ||
            report.anchorKind != "kOr" ||
            report.rootKind != "kOr" ||
            report.rootSymbols.size() != 4 ||
            report.anchorSymbols.size() != 4 ||
            report.storageTargetSymbols.size() != 4 ||
            report.packedRootValueId == 0)
        {
            return fail("Unexpected comb-lane-pack report payload");
        }

        for (int lane = 0; lane < 4; ++lane)
        {
            const std::string rootName = "lane_out_" + std::to_string(lane);
            auto outValue = graph.outputPortValue(rootName);
            if (!outValue.valid())
            {
                return fail("Missing output port after bitwise rewrite");
            }
            const auto value = graph.getValue(outValue);
            if (value.symbolText() != rootName)
            {
                return fail("Expected declared symbol to move to replacement slice");
            }
            const auto defOp = graph.getOperation(value.definingOp());
            if (defOp.kind() != wolvrix::lib::grh::OperationKind::kSliceStatic)
            {
                return fail("Bitwise output should be driven by kSliceStatic");
            }
            const auto startAttr = defOp.attr("sliceStart");
            const auto endAttr = defOp.attr("sliceEnd");
            const auto *start = startAttr ? std::get_if<int64_t>(&*startAttr) : nullptr;
            const auto *end = endAttr ? std::get_if<int64_t>(&*endAttr) : nullptr;
            if (!start || !end ||
                *start != lane * 8 ||
                *end != lane * 8 + 7)
            {
                return fail("Unexpected bitwise slice range");
            }
        }
    }

    // Case 2: repeated declared mux roots pack into one wide masked-select cone.
    {
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("mux");
        for (int lane = 0; lane < 4; ++lane)
        {
            const auto sel = makeLogicValue(graph, "sel" + std::to_string(lane), 1);
            const auto whenTrue = makeLogicValue(graph, "t" + std::to_string(lane), 8);
            const auto whenFalse = makeLogicValue(graph, "f" + std::to_string(lane), 8);
            graph.bindInputPort("sel" + std::to_string(lane), sel);
            graph.bindInputPort("t" + std::to_string(lane), whenTrue);
            graph.bindInputPort("f" + std::to_string(lane), whenFalse);

            const std::string rootName = "mux_out_" + std::to_string(lane);
            const auto root = makeMux(graph,
                                      rootName,
                                      "mux_op_" + std::to_string(lane),
                                      sel,
                                      whenTrue,
                                      whenFalse,
                                      8);
            graph.addDeclaredSymbol(graph.lookupSymbol(rootName));
            graph.bindOutputPort(rootName, root);
        }

        CombLanePackOptions options;
        options.minGroupSize = 4;
        options.minPackedWidth = 32;
        PassManager manager;
        manager.addPass(std::make_unique<CombLanePackPass>(options));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("Expected mux comb-lane-pack to succeed");
        }
        if (!result.changed)
        {
            return fail("Expected mux comb-lane-pack to change graph");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kMux) != 0)
        {
            return fail("Expected mux roots to be removed after packing");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kReplicate) != 4)
        {
            return fail("Expected one kReplicate per mux lane");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kSliceStatic) != 4)
        {
            return fail("Expected four result slices after mux rewrite");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kAnd) != 2 ||
            countKind(graph, wolvrix::lib::grh::OperationKind::kOr) != 1 ||
            countKind(graph, wolvrix::lib::grh::OperationKind::kNot) != 1)
        {
            return fail("Expected masked-select shape for packed mux");
        }

        for (int lane = 0; lane < 4; ++lane)
        {
            const std::string rootName = "mux_out_" + std::to_string(lane);
            auto outValue = graph.outputPortValue(rootName);
            if (!outValue.valid())
            {
                return fail("Missing mux output port after rewrite");
            }
            const auto value = graph.getValue(outValue);
            if (value.symbolText() != rootName)
            {
                return fail("Expected mux declared symbol to move to replacement slice");
            }
        }
    }

    // Case 3: array output mode packs mux roots into one kArrayMux over a
    // per-lane guard concat, with fewer ops than the wide masked-select form.
    {
        auto buildMuxGraph = [](const std::string &graphName) {
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph(graphName);
            design.markAsTop(graphName);
            for (int lane = 0; lane < 4; ++lane)
            {
                const auto sel = makeLogicValue(graph, "sel" + std::to_string(lane), 1);
                const auto whenTrue = makeLogicValue(graph, "t" + std::to_string(lane), 8);
                const auto whenFalse = makeLogicValue(graph, "f" + std::to_string(lane), 8);
                graph.bindInputPort("sel" + std::to_string(lane), sel);
                graph.bindInputPort("t" + std::to_string(lane), whenTrue);
                graph.bindInputPort("f" + std::to_string(lane), whenFalse);

                const std::string rootName = "mux_out_" + std::to_string(lane);
                const auto root = makeMux(graph,
                                          rootName,
                                          "mux_op_" + std::to_string(lane),
                                          sel,
                                          whenTrue,
                                          whenFalse,
                                          8);
                graph.addDeclaredSymbol(graph.lookupSymbol(rootName));
                graph.bindOutputPort(rootName, root);
            }
            return design;
        };

        // Wide-mode reference run on the identical fixture for the op count
        // comparison (wide keeps the historical masked-select shape).
        std::size_t wideOpCount = 0;
        {
            auto wideDesign = buildMuxGraph("mux_wide");
            PassManager wideManager;
            wideManager.addPass(std::make_unique<CombLanePackPass>());
            PassDiagnostics wideDiags;
            const auto wideResult = wideManager.run(wideDesign, wideDiags);
            if (!wideResult.success || wideDiags.hasError() || !wideResult.changed)
            {
                return fail("Expected wide mux reference comb-lane-pack to change graph");
            }
            wideOpCount = countOps(*wideDesign.graphs().begin()->second);
        }

        auto design = buildMuxGraph("mux_array");
        auto &graph = *design.graphs().begin()->second;
        PassManager manager;
        CombLanePackOptions options;
        options.outputMode = CombLanePackOutputMode::Array;
        manager.addPass(std::make_unique<CombLanePackPass>(options));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("Expected array mux comb-lane-pack to succeed");
        }
        if (!result.changed)
        {
            return fail("Expected array mux comb-lane-pack to change graph");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kMux) != 0)
        {
            return fail("Expected array mux roots to be removed after packing");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kArrayMux) != 1)
        {
            return fail("Expected one kArrayMux for array mux packing");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kConcat) != 3)
        {
            return fail("Expected sel + true + false concats for array mux packing");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kReplicate) != 0 ||
            countKind(graph, wolvrix::lib::grh::OperationKind::kAnd) != 0 ||
            countKind(graph, wolvrix::lib::grh::OperationKind::kOr) != 0 ||
            countKind(graph, wolvrix::lib::grh::OperationKind::kNot) != 0)
        {
            return fail("Array mux packing must not emit the wide mask tree");
        }
        if (countKind(graph, wolvrix::lib::grh::OperationKind::kSliceStatic) != 4)
        {
            return fail("Expected four result slices for array mux packing");
        }
        if (countOps(graph) >= wideOpCount)
        {
            return fail("Expected array mux packing to use fewer ops than wide mode");
        }

        // kArrayMux shape: sel is the per-lane guard concat (lane 0 at LSB),
        // t/f are the packed leaf concats.
        wolvrix::lib::grh::OperationId arrayMuxId = wolvrix::lib::grh::OperationId::invalid();
        for (auto opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == wolvrix::lib::grh::OperationKind::kArrayMux)
            {
                arrayMuxId = opId;
            }
        }
        if (!arrayMuxId.valid())
        {
            return fail("Missing kArrayMux op after array mux packing");
        }
        const auto arrayMux = graph.getOperation(arrayMuxId);
        if (arrayMux.operands().size() != 3 || arrayMux.results().size() != 1)
        {
            return fail("kArrayMux must have [sel, t, f] operands and one result");
        }
        if (graph.getValue(arrayMux.results().front()).width() != 32)
        {
            return fail("kArrayMux result must be the packed width");
        }
        const auto muxSrcLoc = arrayMux.srcLoc();
        if (!muxSrcLoc || muxSrcLoc->pass != "comb-lane-pack" ||
            muxSrcLoc->note != "pack-array-mux")
        {
            return fail("kArrayMux must carry the pack-array-mux note");
        }
        const auto selDefId = graph.getValue(arrayMux.operands()[0]).definingOp();
        if (!selDefId.valid() ||
            graph.getOperation(selDefId).kind() != wolvrix::lib::grh::OperationKind::kConcat)
        {
            return fail("kArrayMux sel must be the per-lane guard concat");
        }
        const auto selConcat = graph.getOperation(selDefId);
        if (selConcat.operands().size() != 4 ||
            graph.getValue(selConcat.results().front()).width() != 4)
        {
            return fail("Guard concat must pack four 1-bit conds");
        }
        const auto selSrcLoc = selConcat.srcLoc();
        if (!selSrcLoc || selSrcLoc->note != "pack-array-mux-sel")
        {
            return fail("Guard concat must carry the pack-array-mux-sel note");
        }
        for (int lane = 0; lane < 4; ++lane)
        {
            // kConcat operand 0 is the MSB lane; lane 0 sits at the LSB.
            const auto expected = graph.inputPortValue("sel" + std::to_string(lane));
            if (selConcat.operands()[3 - static_cast<std::size_t>(lane)] != expected)
            {
                return fail("Guard concat lane order must put lane 0 at the LSB");
            }
        }
        for (std::size_t operand = 1; operand <= 2; ++operand)
        {
            const auto dataDefId = graph.getValue(arrayMux.operands()[operand]).definingOp();
            if (!dataDefId.valid() ||
                graph.getOperation(dataDefId).kind() != wolvrix::lib::grh::OperationKind::kConcat ||
                graph.getValue(graph.getOperation(dataDefId).results().front()).width() != 32)
            {
                return fail("kArrayMux data operands must be packed leaf concats");
            }
        }

        // JSON round trip keeps the array ops intact.
        wolvrix::lib::store::StoreDiagnostics storeDiags;
        wolvrix::lib::store::StoreJson store(&storeDiags);
        const auto json = store.storeToString(design);
        if (!json || storeDiags.hasError())
        {
            return fail("Expected array mux graph to store to JSON");
        }
        try
        {
            const auto reloaded = wolvrix::lib::grh::Design::fromJsonString(*json);
            const auto *reloadedGraph = reloaded.findGraph("mux_array");
            if (reloadedGraph == nullptr)
            {
                return fail("Expected reloaded graph after JSON round trip");
            }
            if (countKind(*reloadedGraph, wolvrix::lib::grh::OperationKind::kArrayMux) != 1 ||
                countKind(*reloadedGraph, wolvrix::lib::grh::OperationKind::kConcat) != 3 ||
                countKind(*reloadedGraph, wolvrix::lib::grh::OperationKind::kReplicate) != 0)
            {
                return fail("JSON round trip must preserve the array mux shape");
            }
        }
        catch (const std::exception &)
        {
            return fail("Expected array mux JSON to reload");
        }
    }

    // Case 4: array output mode reuses shared scalars as kArrayBroadcast.
    {
        // Shared mux cond across lanes -> sel is one kArrayBroadcast.
        {
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph("mux_shared_cond");
            const auto en = makeLogicValue(graph, "en", 1);
            graph.bindInputPort("en", en);
            for (int lane = 0; lane < 4; ++lane)
            {
                const auto a = makeLogicValue(graph, "a" + std::to_string(lane), 8);
                const auto b = makeLogicValue(graph, "b" + std::to_string(lane), 8);
                graph.bindInputPort("a" + std::to_string(lane), a);
                graph.bindInputPort("b" + std::to_string(lane), b);
                const std::string rootName = "shared_out_" + std::to_string(lane);
                const auto root = makeMux(graph,
                                          rootName,
                                          "shared_mux_op_" + std::to_string(lane),
                                          en,
                                          a,
                                          b,
                                          8);
                graph.addDeclaredSymbol(graph.lookupSymbol(rootName));
                graph.bindOutputPort(rootName, root);
            }
            PassManager manager;
            CombLanePackOptions options;
            options.outputMode = CombLanePackOutputMode::Array;
            manager.addPass(std::make_unique<CombLanePackPass>(options));
            PassDiagnostics diags;
            const auto result = manager.run(design, diags);
            if (!result.success || diags.hasError() || !result.changed)
            {
                return fail("Expected shared-cond array comb-lane-pack to change graph");
            }
            if (countKind(graph, wolvrix::lib::grh::OperationKind::kArrayMux) != 1 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kArrayBroadcast) != 1 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kConcat) != 2 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kReplicate) != 0)
            {
                return fail("Expected kArrayMux over a kArrayBroadcast sel for shared cond");
            }
            wolvrix::lib::grh::OperationId broadcastId = wolvrix::lib::grh::OperationId::invalid();
            for (auto opId : graph.operations())
            {
                if (graph.getOperation(opId).kind() ==
                    wolvrix::lib::grh::OperationKind::kArrayBroadcast)
                {
                    broadcastId = opId;
                }
            }
            if (!broadcastId.valid())
            {
                return fail("Missing kArrayBroadcast for shared cond");
            }
            const auto broadcast = graph.getOperation(broadcastId);
            const auto rowsAttr = broadcast.attr("rows");
            const auto *rows = rowsAttr ? std::get_if<int64_t>(&*rowsAttr) : nullptr;
            if (broadcast.operands().size() != 1 || broadcast.operands().front() != en ||
                rows == nullptr || *rows != 4 ||
                graph.getValue(broadcast.results().front()).width() != 4)
            {
                return fail("kArrayBroadcast must replicate the shared cond across four lanes");
            }
            const auto broadcastSrcLoc = broadcast.srcLoc();
            if (!broadcastSrcLoc || broadcastSrcLoc->note != "pack-array-broadcast")
            {
                return fail("kArrayBroadcast must carry the pack-array-broadcast note");
            }
            for (auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kArrayMux &&
                    op.operands().front() != broadcast.results().front())
                {
                    return fail("kArrayMux sel must reuse the shared-cond broadcast");
                }
            }
        }

        // Shared multi-bit leaf across lanes -> leaf pack is one kArrayBroadcast.
        {
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph("shared_leaf");
            const auto en = makeLogicValue(graph, "en", 8);
            graph.bindInputPort("en", en);
            for (int lane = 0; lane < 4; ++lane)
            {
                const auto a = makeLogicValue(graph, "a" + std::to_string(lane), 8);
                graph.bindInputPort("a" + std::to_string(lane), a);
                const std::string rootName = "shared_leaf_out_" + std::to_string(lane);
                const auto root = makeBinary(graph,
                                             wolvrix::lib::grh::OperationKind::kAnd,
                                             rootName,
                                             "shared_leaf_and_op_" + std::to_string(lane),
                                             a,
                                             en,
                                             8);
                graph.addDeclaredSymbol(graph.lookupSymbol(rootName));
                graph.bindOutputPort(rootName, root);
            }
            PassManager manager;
            CombLanePackOptions options;
            options.outputMode = CombLanePackOutputMode::Array;
            manager.addPass(std::make_unique<CombLanePackPass>(options));
            PassDiagnostics diags;
            const auto result = manager.run(design, diags);
            if (!result.success || diags.hasError() || !result.changed)
            {
                return fail("Expected shared-leaf array comb-lane-pack to change graph");
            }
            if (countKind(graph, wolvrix::lib::grh::OperationKind::kAnd) != 1 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kArrayBroadcast) != 1 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kConcat) != 1 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kReplicate) != 0 ||
                countKind(graph, wolvrix::lib::grh::OperationKind::kSliceStatic) != 4)
            {
                return fail("Expected shared leaf to pack as kArrayBroadcast + widened kAnd");
            }
        }
    }

    // Case 5: repeated storage-write next-value cones pack, while write ports stay intact.
    {
        auto buildStorageFrontierGraph = []() {
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph("storage_frontier");
            const auto enable = makeLogicValue(graph, "enable", 1);
            const auto mask = makeLogicValue(graph, "mask", 8);
            const auto clock = makeLogicValue(graph, "clock", 1);
            graph.bindInputPort("enable", enable);
            graph.bindInputPort("mask", mask);
            graph.bindInputPort("clock", clock);

            for (int lane = 0; lane < 4; ++lane)
            {
                const auto a = makeLogicValue(graph, "wa" + std::to_string(lane), 8);
                const auto b = makeLogicValue(graph, "wb" + std::to_string(lane), 8);
                const auto c = makeLogicValue(graph, "wc" + std::to_string(lane), 8);
                graph.bindInputPort("wa" + std::to_string(lane), a);
                graph.bindInputPort("wb" + std::to_string(lane), b);
                graph.bindInputPort("wc" + std::to_string(lane), c);

                const auto regSym = graph.internSymbol("reg_q_" + std::to_string(lane));
                const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister, regSym);
                graph.setAttr(reg, "width", static_cast<int64_t>(8));
                graph.setAttr(reg, "isSigned", false);

                const auto readValue =
                    makeLogicValue(graph, "reg_q_read_" + std::to_string(lane), 8);
                const auto read = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                    graph.internSymbol("reg_read_" + std::to_string(lane)));
                graph.setAttr(read, "regSymbol", std::string("reg_q_" + std::to_string(lane)));
                graph.addResult(read, readValue);
                graph.bindOutputPort("reg_q_out_" + std::to_string(lane), readValue);

                const auto andValue = makeBinary(graph,
                                                 wolvrix::lib::grh::OperationKind::kAnd,
                                                 "storage_and_" + std::to_string(lane),
                                                 "storage_and_op_" + std::to_string(lane),
                                                 a,
                                                 b,
                                                 8);
                const auto nextValue = makeBinary(graph,
                                                  wolvrix::lib::grh::OperationKind::kOr,
                                                  "storage_or_" + std::to_string(lane),
                                                  "storage_or_op_" + std::to_string(lane),
                                                  andValue,
                                                  c,
                                                  8);

                const auto write =
                    graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                          graph.internSymbol("reg_write_" + std::to_string(lane)));
                graph.setAttr(write, "regSymbol", std::string("reg_q_" + std::to_string(lane)));
                graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
                graph.addOperand(write, enable);
                graph.addOperand(write, nextValue);
                graph.addOperand(write, mask);
                graph.addOperand(write, clock);
            }
            return design;
        };

        {
            auto design = buildStorageFrontierGraph();
            auto &graph = *design.graphs().begin()->second;
            PassManager manager;
            manager.addPass(std::make_unique<CombLanePackPass>());
            PassDiagnostics diags;
            const auto result = manager.run(design, diags);
            if (!result.success || diags.hasError())
            {
                return fail("Expected storage frontier comb-lane-pack to succeed");
            }
            if (!result.changed)
            {
                return fail("Expected storage frontier comb-lane-pack to change graph");
            }
            const std::size_t packedAndCount =
                countKind(graph, wolvrix::lib::grh::OperationKind::kAnd);
            const std::size_t packedOrCount =
                countKind(graph, wolvrix::lib::grh::OperationKind::kOr);
            const std::size_t resultSliceCount =
                countKind(graph, wolvrix::lib::grh::OperationKind::kSliceStatic);
            if (packedAndCount != 1)
            {
                return fail("Expected one packed kAnd for storage frontier rewrite, got " +
                            std::to_string(packedAndCount));
            }
            if (packedOrCount != 1)
            {
                return fail("Expected one packed kOr for storage frontier rewrite, got " +
                            std::to_string(packedOrCount));
            }
            if (resultSliceCount != 4)
            {
                return fail("Expected four result slices for storage frontier rewrite, got " +
                            std::to_string(resultSliceCount));
            }
            if (countKind(graph, wolvrix::lib::grh::OperationKind::kRegisterWritePort) != 4)
            {
                return fail("Expected register write ports to remain intact");
            }
            auto designWithReport = buildStorageFrontierGraph();
            PassManager managerWithReport;
            wolvrix::lib::transform::SessionStore session;
            managerWithReport.options().session = &session;
            CombLanePackOptions options;
            options.outputKey = "comb_lane_pack.report";
            managerWithReport.addPass(std::make_unique<CombLanePackPass>(options));
            PassDiagnostics diagsWithReport;
            const auto resultWithReport = managerWithReport.run(designWithReport, diagsWithReport);
            if (!resultWithReport.success || diagsWithReport.hasError())
            {
                return fail("Expected storage frontier comb-lane-pack report run to succeed");
            }
            const auto reportIt = session.find("comb_lane_pack.report");
            if (reportIt == session.end() || !reportIt->second)
            {
                return fail("Expected storage frontier comb-lane-pack report in session");
            }
            const auto *typed =
                dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<
                    std::vector<wolvrix::lib::transform::CombLanePackReport>> *>(reportIt->second.get());
            if (!typed || typed->value.size() != 1)
            {
                return fail("Unexpected storage frontier comb-lane-pack report session type");
            }
            const auto &report = typed->value.front();
            if (report.rootSource != "storage-data" ||
                report.anchorKind != "kRegisterWritePort" ||
                report.anchorSymbols.size() != 4 ||
                report.storageTargetSymbols.size() != 4)
            {
                return fail("Unexpected storage frontier report metadata");
            }
            for (int lane = 0; lane < 4; ++lane)
            {
                if (report.anchorSymbols[static_cast<std::size_t>(lane)] !=
                        "reg_write_" + std::to_string(lane) ||
                    report.storageTargetSymbols[static_cast<std::size_t>(lane)] !=
                        "reg_q_" + std::to_string(lane))
                {
                    return fail("Unexpected storage frontier anchor metadata");
                }
            }
            for (int lane = 0; lane < 4; ++lane)
            {
                const auto write =
                    graph.findOperation("reg_write_" + std::to_string(lane));
                if (!write.valid())
                {
                    return fail("Missing register write after storage frontier rewrite");
                }
                const auto operands = graph.getOperation(write).operands();
                if (operands.size() != 4)
                {
                    return fail("Unexpected register write operand count after rewrite");
                }
                const auto nextValue = operands[1];
                const auto nextOp = graph.getOperation(graph.getValue(nextValue).definingOp());
                if (nextOp.kind() != wolvrix::lib::grh::OperationKind::kSliceStatic)
                {
                    return fail("Register write next-value should be rewritten to kSliceStatic");
                }
            }
        }

        {
            auto design = buildStorageFrontierGraph();
            PassManager manager;
            CombLanePackOptions options;
            options.enableDeclaredRoots = false;
            options.enableStorageDataRoots = false;
            manager.addPass(std::make_unique<CombLanePackPass>(options));
            PassDiagnostics diags;
            const auto result = manager.run(design, diags);
            if (result.success || !diags.hasError())
            {
                return fail("Expected comb-lane-pack to reject empty root sources");
            }

            auto design2 = buildStorageFrontierGraph();
            PassManager manager2;
            CombLanePackOptions options2;
            options2.enableDeclaredRoots = false;
            manager2.addPass(std::make_unique<CombLanePackPass>(options2));
            PassDiagnostics diags2;
            const auto result2 = manager2.run(design2, diags2);
            if (!result2.success || diags2.hasError())
            {
                return fail("Expected storage-frontier-only comb-lane-pack to succeed");
            }
            if (!result2.changed)
            {
                return fail("Expected storage-frontier-only comb-lane-pack to rewrite write-frontier roots");
            }
        }
    }

    return 0;
}
