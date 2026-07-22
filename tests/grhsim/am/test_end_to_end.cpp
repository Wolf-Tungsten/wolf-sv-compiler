#include "core/grh.hpp"
#include "grhsim/am/cpp_emitter.hpp"
#include "grhsim/am/interpreter.hpp"
#include "grhsim/am/lowering.hpp"
#include "grhsim/am/production_activity_schedule.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(std::string_view message)
    {
        std::cerr << message << '\n';
        return 1;
    }

    grh::ValueId addLogicValue(grh::Graph &graph,
                               std::string_view name,
                               int32_t width)
    {
        return graph.createValue(
            graph.internSymbol(name), width, false, grh::ValueType::Logic);
    }

    std::optional<VariableId> findPort(const ExecutableModel &model,
                                       std::string_view name,
                                       bool input)
    {
        const ProgramView program = model.program.view();
        for (const PortBinding &port : model.interface.ports)
        {
            if (program.string(port.name) != name)
            {
                continue;
            }
            return input ? port.input : port.output;
        }
        return std::nullopt;
    }

} // namespace

int main()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("EndToEnd");
    design.markAsTop(graph.symbol());
    const grh::ValueId lhs = addLogicValue(graph, "lhs", 8);
    const grh::ValueId rhs = addLogicValue(graph, "rhs", 8);
    const grh::ValueId sum = addLogicValue(graph, "sum", 8);
    graph.bindInputPort("lhs", lhs);
    graph.bindInputPort("rhs", rhs);
    graph.bindOutputPort("sum", sum);
    const grh::OperationId add =
        graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("add"));
    graph.addOperand(add, lhs);
    graph.addOperand(add, rhs);
    graph.addResult(add, sum);
    graph.freeze();

    diag::Diagnostics diagnostics;
    GrhToAmLowering lowering;
    std::optional<LinearProgramArtifact> linear = lowering.lower(graph, diagnostics);
    if (!linear || diagnostics.hasError())
    {
        return fail("concrete GRH-to-AM lowering failed");
    }
    ProductionActivityScheduleStage scheduler;
    std::optional<ExecutableModel> model = scheduler.schedule(
        std::move(*linear),
        ActivityScheduleOptions{
            .maxInstructionsPerBlock = 8,
            .enableCoarsening = false,
        },
        diagnostics);
    if (!model || diagnostics.hasError() || model->program.blockCount() < 2)
    {
        return fail("production AM scheduler failed");
    }

    const std::optional<VariableId> lhsVariable = findPort(*model, "lhs", true);
    const std::optional<VariableId> rhsVariable = findPort(*model, "rhs", true);
    const std::optional<VariableId> sumVariable = findPort(*model, "sum", false);
    if (!lhsVariable || !rhsVariable || !sumVariable)
    {
        return fail("lowering did not preserve the public port ABI");
    }
    const std::array<uint64_t, 1> lhsWords = {37};
    const std::array<uint64_t, 1> rhsWords = {5};
    Interpreter reference(*model);
    if (!reference.ready() ||
        !reference.write(
                      *lhsVariable,
                      InterpreterValue::bitVector(
                          8, Signedness::Unsigned, lhsWords))
             .success() ||
        !reference.write(
                      *rhsVariable,
                      InterpreterValue::bitVector(
                          8, Signedness::Unsigned, rhsWords))
             .success() ||
        !reference.eval().success() ||
        reference.value(*sumVariable).lowWord() != 42)
    {
        return fail("reference executor failed the lowered/scheduled model");
    }

    const std::filesystem::path outputDirectory =
        std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "end-to-end";
    std::filesystem::remove_all(outputDirectory);
    GrhSimAmCppEmitter emitter;
    const GrhSimAmCppResult emitResult = emitter.emit(
        *model,
        GrhSimAmCppOptions{
            .outputDirectory = outputDirectory,
            .modelName = "EndToEnd",
            .maxOutputFileBytes = 1024 * 1024,
        },
        diagnostics);
    if (!emitResult.success || diagnostics.hasError())
    {
        return fail("AM C++ emitter rejected the lowered/scheduled model");
    }

    const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
    std::ofstream harness(harnessPath);
    harness << R"CPP(#include "grhsim_EndToEnd.hpp"
int main()
{
    GrhSIM_EndToEnd model;
    model.init();
    model.lhs = 37;
    model.rhs = 5;
    model.eval();
    if (model.sum != 42)
        return 1;
    model.rhs = 9;
    model.eval();
    return model.sum == 46 ? 0 : 2;
}
)CPP";
    harness.close();
    if (!harness)
    {
        return fail("failed to write end-to-end generated model harness");
    }

    const std::string buildCommand =
        "make -C '" + outputDirectory.string() +
        "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
    if (std::system(buildCommand.c_str()) != 0)
    {
        return fail("end-to-end generated model failed to compile");
    }
    const std::filesystem::path executable = outputDirectory / "harness";
    const std::string compileHarness =
        "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
        harnessPath.string() + "' '" +
        (outputDirectory / "libgrhsim_EndToEnd.a").string() + "' -o '" +
        executable.string() + "'";
    if (std::system(compileHarness.c_str()) != 0)
    {
        return fail("end-to-end generated model harness failed to compile");
    }
    const std::string runHarness = "'" + executable.string() + "'";
    if (std::system(runHarness.c_str()) != 0)
    {
        return fail("generated model disagreed with the reference executor");
    }
    return 0;
}
