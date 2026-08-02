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
#include <vector>

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

    int runArrayEndToEnd()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("ArrayEndToEnd");
        design.markAsTop(graph.symbol());
        const grh::ValueId clock = addLogicValue(graph, "clk", 1);
        const grh::ValueId laneMask = addLogicValue(graph, "lane_mask", 8);
        const grh::ValueId data = addLogicValue(graph, "data", 64);
        const grh::ValueId scalar = addLogicValue(graph, "scalar", 8);
        const grh::ValueId index = addLogicValue(graph, "index", 4);
        graph.bindInputPort("clk", clock);
        graph.bindInputPort("lane_mask", laneMask);
        graph.bindInputPort("data", data);
        graph.bindInputPort("scalar", scalar);
        graph.bindInputPort("index", index);

        const grh::OperationId memory = graph.createOperation(
            grh::OperationKind::kMemory, graph.internSymbol("arr"));
        graph.setAttr(memory, "width", int64_t{8});
        graph.setAttr(memory, "row", int64_t{8});
        graph.setAttr(memory, "isSigned", false);
        graph.setAttr(memory, "initKind", std::vector<std::string>{"literal"});
        graph.setAttr(memory, "initFile", std::vector<std::string>{""});
        graph.setAttr(memory, "initValue", std::vector<std::string>{"8'h00"});
        graph.setAttr(memory, "initStart", std::vector<int64_t>{0});
        graph.setAttr(memory, "initLen", std::vector<int64_t>{8});

        const grh::ValueId all = addLogicValue(graph, "all", 64);
        const grh::OperationId readAll = graph.createOperation(
            grh::OperationKind::kArrayReadAllPort, graph.internSymbol("read_all"));
        graph.addResult(readAll, all);
        graph.setAttr(readAll, "memSymbol", std::string("arr"));

        const grh::ValueId lanes = addLogicValue(graph, "lanes", 64);
        const grh::OperationId laneConst = graph.createOperation(
            grh::OperationKind::kArrayLaneConst, graph.internSymbol("lane_const"));
        graph.addResult(laneConst, lanes);
        graph.setAttr(laneConst, "elemWidth", int64_t{8});
        graph.setAttr(laneConst, "rows", int64_t{8});
        graph.setAttr(laneConst, "values",
                      std::vector<int64_t>{0x11, 0x22, 0x33, 0x44,
                                           0x55, 0x66, 0x77, 0x88});

        const grh::ValueId muxed = addLogicValue(graph, "muxed", 64);
        const grh::OperationId mux = graph.createOperation(
            grh::OperationKind::kArrayMux, graph.internSymbol("lane_mux"));
        graph.addOperand(mux, laneMask);
        graph.addOperand(mux, all);
        graph.addOperand(mux, lanes);
        graph.addResult(mux, muxed);

        const grh::ValueId bcast = addLogicValue(graph, "bcast", 64);
        const grh::OperationId broadcast = graph.createOperation(
            grh::OperationKind::kArrayBroadcast, graph.internSymbol("bcast_op"));
        graph.addOperand(broadcast, scalar);
        graph.addResult(broadcast, bcast);
        graph.setAttr(broadcast, "rows", int64_t{8});

        const grh::ValueId onehot = addLogicValue(graph, "onehot", 8);
        const grh::OperationId onehotOp = graph.createOperation(
            grh::OperationKind::kArrayOnehot, graph.internSymbol("onehot_op"));
        graph.addOperand(onehotOp, index);
        graph.addResult(onehotOp, onehot);
        graph.setAttr(onehotOp, "rows", int64_t{8});

        const grh::ValueId redOr = addLogicValue(graph, "red_or", 1);
        const grh::OperationId reduceOp = graph.createOperation(
            grh::OperationKind::kArrayReduceOr, graph.internSymbol("red_or_op"));
        graph.addOperand(reduceOp, muxed);
        graph.addResult(reduceOp, redOr);
        graph.setAttr(reduceOp, "elemWidth", int64_t{8});

        const grh::ValueId lanesOr = addLogicValue(graph, "lanes_or", 8);
        const grh::OperationId lanesOrOp = graph.createOperation(
            grh::OperationKind::kArrayReduceLanesOr, graph.internSymbol("lanes_or_op"));
        graph.addOperand(lanesOrOp, muxed);
        graph.addResult(lanesOrOp, lanesOr);
        graph.setAttr(lanesOrOp, "elemWidth", int64_t{8});

        const grh::ValueId lanesAnd = addLogicValue(graph, "lanes_and", 8);
        const grh::OperationId lanesAndOp = graph.createOperation(
            grh::OperationKind::kArrayReduceLanesAnd, graph.internSymbol("lanes_and_op"));
        graph.addOperand(lanesAndOp, muxed);
        graph.addResult(lanesAndOp, lanesAnd);
        graph.setAttr(lanesAndOp, "elemWidth", int64_t{8});

        const grh::ValueId lanesXor = addLogicValue(graph, "lanes_xor", 8);
        const grh::OperationId lanesXorOp = graph.createOperation(
            grh::OperationKind::kArrayReduceLanesXor, graph.internSymbol("lanes_xor_op"));
        graph.addOperand(lanesXorOp, muxed);
        graph.addResult(lanesXorOp, lanesXor);
        graph.setAttr(lanesXorOp, "elemWidth", int64_t{8});

        const grh::OperationId write = graph.createOperation(
            grh::OperationKind::kArrayWritePort, graph.internSymbol("lane_write"));
        graph.addOperand(write, laneMask);
        graph.addOperand(write, data);
        graph.addOperand(write, clock);
        graph.setAttr(write, "memSymbol", std::string("arr"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        graph.bindOutputPort("all", all);
        graph.bindOutputPort("muxed", muxed);
        graph.bindOutputPort("bcast", bcast);
        graph.bindOutputPort("onehot", onehot);
        graph.bindOutputPort("red_or", redOr);
        graph.bindOutputPort("lanes_or", lanesOr);
        graph.bindOutputPort("lanes_and", lanesAnd);
        graph.bindOutputPort("lanes_xor", lanesXor);
        graph.freeze();

        diag::Diagnostics diagnostics;
        GrhToAmLowering lowering;
        std::optional<LinearProgramArtifact> linear = lowering.lower(graph, diagnostics);
        if (!linear || diagnostics.hasError())
        {
            return fail("array GRH-to-AM lowering failed");
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
            return fail("production AM scheduler failed the array model");
        }

        const std::optional<VariableId> clockVariable = findPort(*model, "clk", true);
        const std::optional<VariableId> laneMaskVariable =
            findPort(*model, "lane_mask", true);
        const std::optional<VariableId> dataVariable = findPort(*model, "data", true);
        const std::optional<VariableId> scalarVariable =
            findPort(*model, "scalar", true);
        const std::optional<VariableId> indexVariable = findPort(*model, "index", true);
        const std::optional<VariableId> allVariable = findPort(*model, "all", false);
        const std::optional<VariableId> muxedVariable =
            findPort(*model, "muxed", false);
        const std::optional<VariableId> bcastVariable =
            findPort(*model, "bcast", false);
        const std::optional<VariableId> onehotVariable =
            findPort(*model, "onehot", false);
        const std::optional<VariableId> redOrVariable =
            findPort(*model, "red_or", false);
        const std::optional<VariableId> lanesOrVariable =
            findPort(*model, "lanes_or", false);
        const std::optional<VariableId> lanesAndVariable =
            findPort(*model, "lanes_and", false);
        const std::optional<VariableId> lanesXorVariable =
            findPort(*model, "lanes_xor", false);
        if (!clockVariable || !laneMaskVariable || !dataVariable ||
            !scalarVariable || !indexVariable || !allVariable || !muxedVariable ||
            !bcastVariable || !onehotVariable || !redOrVariable ||
            !lanesOrVariable || !lanesAndVariable || !lanesXorVariable)
        {
            return fail("array lowering did not preserve the public port ABI");
        }

        struct Transaction
        {
            uint64_t laneMask;
            uint64_t data;
            uint64_t scalar;
            uint64_t index;
            uint64_t clock;
        };
        const std::array<Transaction, 3> transactions = {
            Transaction{0xa5, UINT64_C(0x1122314455667788), 0x5c, 3, 1},
            Transaction{0xa5, UINT64_C(0x1122314455667788), 0x5c, 3, 0},
            Transaction{0xff, UINT64_MAX, 0x5c, 3, 1},
        };
        Interpreter reference(*model);
        if (!reference.ready() || !reference.eval().success())
        {
            return fail("array reference executor failed its initial eval");
        }
        std::vector<std::array<uint64_t, 8>> oracle;
        const auto writePort = [&](VariableId variable, uint32_t width, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return reference
                .write(variable,
                       InterpreterValue::bitVector(width, Signedness::Unsigned, words))
                .success();
        };
        for (const Transaction &transaction : transactions)
        {
            if (!writePort(*laneMaskVariable, 8, transaction.laneMask) ||
                !writePort(*dataVariable, 64, transaction.data) ||
                !writePort(*scalarVariable, 8, transaction.scalar) ||
                !writePort(*indexVariable, 4, transaction.index) ||
                !writePort(*clockVariable, 1, transaction.clock) ||
                !reference.eval().success())
            {
                return fail("array reference executor failed a transaction");
            }
            oracle.push_back({reference.value(*allVariable).lowWord(),
                              reference.value(*muxedVariable).lowWord(),
                              reference.value(*bcastVariable).lowWord(),
                              reference.value(*onehotVariable).lowWord(),
                              reference.value(*redOrVariable).lowWord(),
                              reference.value(*lanesOrVariable).lowWord(),
                              reference.value(*lanesAndVariable).lowWord(),
                              reference.value(*lanesXorVariable).lowWord()});
        }
        // Hand-computed: lanes 0/2/5/7 take the packed data lanes on the
        // first posedge, the rest stay zero; the reduce sees a nonzero array.
        // The per-lane reduces see muxed lanes [88 22 66 44 55 31 77 11]
        // (lane mask picks the array on 0/2/5/7, the lane constants
        // elsewhere): or = 0xff, and = 0x00, xor = 0x20.
        if (oracle[0][0] != UINT64_C(0x1100310000660088) || oracle[0][4] != 1 ||
            oracle[0][5] != 0xff || oracle[0][6] != 0x00 || oracle[0][7] != 0x20)
        {
            return fail("array reference executor disagreed with the hand oracle");
        }

        const std::filesystem::path outputDirectory =
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "end-to-end-array";
        std::filesystem::remove_all(outputDirectory);
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "ArrayEndToEnd",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter rejected the array lowered/scheduled model");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << "#include \"grhsim_ArrayEndToEnd.hpp\"\n"
                   "#include <cstdint>\n\n"
                   "int main()\n"
                   "{\n"
                   "    GrhSIM_ArrayEndToEnd model;\n"
                   "    model.init();\n";
        int returnCode = 1;
        for (std::size_t t = 0; t < transactions.size(); ++t)
        {
            const Transaction &transaction = transactions[t];
            harness << "    model.lane_mask = " << transaction.laneMask << ";\n"
                    << "    model.data = UINT64_C(" << transaction.data << ");\n"
                    << "    model.scalar = " << transaction.scalar << ";\n"
                    << "    model.index = " << transaction.index << ";\n"
                    << "    model.clk = " << transaction.clock << ";\n"
                    << "    model.eval();\n"
                    << "    if (static_cast<std::uint64_t>(model.all) != UINT64_C("
                    << oracle[t][0] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.muxed) != UINT64_C("
                    << oracle[t][1] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.bcast) != UINT64_C("
                    << oracle[t][2] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.onehot) != UINT64_C("
                    << oracle[t][3] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.red_or) != UINT64_C("
                    << oracle[t][4] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.lanes_or) != UINT64_C("
                    << oracle[t][5] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.lanes_and) != UINT64_C("
                    << oracle[t][6] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.lanes_xor) != UINT64_C("
                    << oracle[t][7] << ")) return " << returnCode++ << ";\n";
        }
        harness << "    return 0;\n}\n";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the array end-to-end harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("array end-to-end generated model failed to compile");
        }
        const std::filesystem::path executable = outputDirectory / "harness";
        const std::string compileHarness =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_ArrayEndToEnd.a").string() + "' -o '" +
            executable.string() + "'";
        if (std::system(compileHarness.c_str()) != 0)
        {
            return fail("array end-to-end harness failed to compile");
        }
        const std::string runHarness = "'" + executable.string() + "'";
        if (std::system(runHarness.c_str()) != 0)
        {
            return fail("array generated model disagreed with the reference executor");
        }
        return 0;
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
    return runArrayEndToEnd();
}
