#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[emit_grhsim_cpp_memory_fill] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    ValueId makeLogicValue(Graph &graph, std::string_view name, int32_t width, bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), width, isSigned, ValueType::Logic);
    }

    ValueId addConstant(Graph &graph,
                        std::string_view opName,
                        std::string_view valueName,
                        int32_t width,
                        std::string literal)
    {
        const ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    Design buildDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        const ValueId clk = makeLogicValue(graph, "clk", 1);
        const ValueId fillEn = makeLogicValue(graph, "fill_en", 1);
        const ValueId data = makeLogicValue(graph, "data", 8);
        const ValueId packedData = makeLogicValue(graph, "packed_data", 8);
        const ValueId widePackedData = makeLogicValue(graph, "wide_packed_data", 128);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("fill_en", fillEn);
        graph.bindInputPort("data", data);
        graph.bindInputPort("packed_data", packedData);
        graph.bindInputPort("wide_packed_data", widePackedData);

        const OperationId mem = graph.createOperation(OperationKind::kMemory, graph.internSymbol("mem"));
        graph.setAttr(mem, "width", static_cast<int64_t>(8));
        graph.setAttr(mem, "row", static_cast<int64_t>(4));
        graph.setAttr(mem, "isSigned", false);

        const OperationId packedMem =
            graph.createOperation(OperationKind::kMemory, graph.internSymbol("mem_packed"));
        graph.setAttr(packedMem, "width", static_cast<int64_t>(2));
        graph.setAttr(packedMem, "row", static_cast<int64_t>(4));
        graph.setAttr(packedMem, "isSigned", false);

        const OperationId widePackedMem =
            graph.createOperation(OperationKind::kMemory, graph.internSymbol("mem_wide_packed"));
        graph.setAttr(widePackedMem, "width", static_cast<int64_t>(32));
        graph.setAttr(widePackedMem, "row", static_cast<int64_t>(4));
        graph.setAttr(widePackedMem, "isSigned", false);

        const OperationId fill = graph.createOperation(OperationKind::kMemoryFillPort,
                                                       graph.internSymbol("mem_fill"));
        graph.setAttr(fill, "memSymbol", std::string("mem"));
        graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(fill, fillEn);
        graph.addOperand(fill, data);
        graph.addOperand(fill, clk);

        const ValueId addr0 = addConstant(graph, "addr0_op", "addr0", 2, "2'd0");
        const OperationId packedFill = graph.createOperation(OperationKind::kMemoryFillPort,
                                                             graph.internSymbol("mem_packed_fill"));
        graph.setAttr(packedFill, "memSymbol", std::string("mem_packed"));
        graph.setAttr(packedFill, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(packedFill, fillEn);
        graph.addOperand(packedFill, packedData);
        graph.addOperand(packedFill, clk);

        const OperationId widePackedFill = graph.createOperation(OperationKind::kMemoryFillPort,
                                                                 graph.internSymbol("mem_wide_packed_fill"));
        graph.setAttr(widePackedFill, "memSymbol", std::string("mem_wide_packed"));
        graph.setAttr(widePackedFill, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(widePackedFill, fillEn);
        graph.addOperand(widePackedFill, widePackedData);
        graph.addOperand(widePackedFill, clk);

        const ValueId out = makeLogicValue(graph, "out", 8);
        const OperationId read = graph.createOperation(OperationKind::kMemoryReadPort,
                                                       graph.internSymbol("mem_read"));
        graph.setAttr(read, "memSymbol", std::string("mem"));
        graph.addOperand(read, addr0);
        graph.addResult(read, out);
        graph.bindOutputPort("out", out);

        const ValueId packedOut = makeLogicValue(graph, "packed_out", 2);
        const OperationId packedRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                            graph.internSymbol("mem_packed_read"));
        graph.setAttr(packedRead, "memSymbol", std::string("mem_packed"));
        graph.addOperand(packedRead, addr0);
        graph.addResult(packedRead, packedOut);
        graph.bindOutputPort("packed_out", packedOut);

        const ValueId widePackedOut = makeLogicValue(graph, "wide_packed_out", 32);
        const OperationId widePackedRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                                graph.internSymbol("mem_wide_packed_read"));
        graph.setAttr(widePackedRead, "memSymbol", std::string("mem_wide_packed"));
        graph.addOperand(widePackedRead, addr0);
        graph.addResult(widePackedRead, widePackedOut);
        graph.bindOutputPort("wide_packed_out", widePackedOut);

        return design;
    }

    bool runActivitySchedule(Design &design, SessionStore &session)
    {
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{.path = "top"}));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        return result.success && !diags.hasError();
    }
} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    Design design = buildDesign();
    SessionStore session;
    if (!runActivitySchedule(design, session))
    {
        return fail("activity schedule failed");
    }

    const std::filesystem::path outDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_memory_fill";
    std::filesystem::create_directories(outDir);

    EmitDiagnostics diag;
    EmitGrhSimCpp emitter(&diag);
    EmitOptions options;
    options.outputDir = outDir.string();
    options.session = &session;
    options.sessionPathPrefix = std::string("top");
    options.attributes["sched_batch_max_ops"] = "8";
    options.attributes["sched_batch_max_estimated_lines"] = "96";
    options.attributes["emit_parallelism"] = "1";

    const EmitResult result = emitter.emit(design, options);
    if (!result.success || diag.hasError())
    {
        return fail("grhsim cpp emit failed");
    }

    const std::string header = readFile(outDir / "grhsim_top.hpp");
    std::string sched;
    for (const auto &entry : std::filesystem::directory_iterator(outDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("grhsim_top_sched_", 0) == 0 && entry.path().extension() == ".cpp")
        {
            sched += readFile(entry.path());
            sched.push_back('\n');
        }
    }
    if (header.empty() || sched.empty())
    {
        return fail("missing generated grhsim files");
    }
    if (sched.find("for (std::size_t fill_row = 0; fill_row < 4u; ++fill_row)") == std::string::npos ||
        sched.find("any_row_changed = true;") == std::string::npos ||
        sched.find("state_mem_mem_") == std::string::npos)
    {
        return fail("memory fill emit body is missing from generated schedule");
    }
    if (sched.find("grhsim_slice_dynamic_u64") == std::string::npos ||
        sched.find("fill_row * 2u") == std::string::npos ||
        sched.find("state_mem_mem_packed_") == std::string::npos)
    {
        return fail("packed memory fill emit body is missing row slice extraction");
    }
    if (sched.find("fill_row * 32u") == std::string::npos ||
        sched.find("grhsim_slice_words<1>") == std::string::npos ||
        sched.find("state_mem_mem_wide_packed_") == std::string::npos)
    {
        return fail("wide packed memory fill emit body is missing words row slice extraction");
    }
    if (sched.find("static_cast<std::uint64_t>(value_words_") != std::string::npos)
    {
        return fail("wide packed memory fill emitted invalid value_words scalar cast");
    }

    const std::string buildCmd = "make -C " + outDir.string() + " CXX=clang++ CXXFLAGS='-std=c++20 -O0'";
    if (std::system(buildCmd.c_str()) != 0)
    {
        return fail("generated grhsim memory-fill model failed to compile");
    }

    return 0;
}
