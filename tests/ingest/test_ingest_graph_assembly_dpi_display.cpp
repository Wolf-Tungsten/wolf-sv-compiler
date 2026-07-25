#include "core/ingest.hpp"
#include "emit/system_verilog.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <variant>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-dpi-display] " << message << '\n';
    return 1;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

struct CompilationBundle {
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> compilation;
};

std::unique_ptr<CompilationBundle> compileInput(const std::filesystem::path& sourcePath,
                                                std::string_view topModule) {
    auto bundle = std::make_unique<CompilationBundle>();
    auto& driver = bundle->driver;
    driver.addStandardArgs();
    driver.languageVersion = slang::LanguageVersion::v1800_2023;
    if (!topModule.empty()) {
        driver.options.topModules.emplace_back(topModule);
    }

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-graph-assembly-dpi-display");
    argStorage.emplace_back(sourcePath.string());
    std::vector<const char*> argv;
    argv.reserve(argStorage.size());
    for (const std::string& arg : argStorage) {
        argv.push_back(arg.c_str());
    }

    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data())) {
        return nullptr;
    }
    if (!driver.processOptions()) {
        return nullptr;
    }
    if (!driver.parseAllSources()) {
        return nullptr;
    }

    bundle->compilation = driver.createCompilation();
    if (!bundle->compilation) {
        return nullptr;
    }
    driver.reportCompilation(*bundle->compilation, /* quiet */ true);
    driver.runAnalysis(*bundle->compilation);
    return bundle;
}

std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::string>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<bool> getAttrBool(const wolvrix::lib::grh::Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<bool>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<int64_t> getAttrInt(const wolvrix::lib::grh::Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<int64_t>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> getAttrStrings(const wolvrix::lib::grh::Operation& op,
                                                       std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::vector<std::string>>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::vector<int64_t>> getAttrInts(const wolvrix::lib::grh::Operation& op,
                                                std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::vector<int64_t>>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::vector<bool>> getAttrBools(const wolvrix::lib::grh::Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::vector<bool>>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::string> getConstLiteral(const wolvrix::lib::grh::Graph& graph, wolvrix::lib::grh::ValueId valueId) {
    if (!valueId.valid()) {
        return std::nullopt;
    }
    const wolvrix::lib::grh::Value value = graph.getValue(valueId);
    const wolvrix::lib::grh::OperationId defOpId = value.definingOp();
    if (!defOpId.valid()) {
        return std::nullopt;
    }
    const wolvrix::lib::grh::Operation defOp = graph.getOperation(defOpId);
    if (defOp.kind() != wolvrix::lib::grh::OperationKind::kConstant) {
        return std::nullopt;
    }
    return getAttrString(defOp, "constValue");
}

bool valueDependsOn(const wolvrix::lib::grh::Graph& graph,
                    wolvrix::lib::grh::ValueId value,
                    wolvrix::lib::grh::ValueId source,
                    std::unordered_set<wolvrix::lib::grh::ValueId,
                                       wolvrix::lib::grh::ValueIdHash>& visited) {
    if (!value.valid()) {
        return false;
    }
    if (value == source) {
        return true;
    }
    if (!visited.insert(value).second) {
        return false;
    }
    const wolvrix::lib::grh::OperationId definingOp = graph.getValue(value).definingOp();
    if (!definingOp.valid()) {
        return false;
    }
    const wolvrix::lib::grh::Operation operation = graph.getOperation(definingOp);
    for (const auto operand : operation.operands()) {
        if (valueDependsOn(graph, operand, source, visited)) {
            return true;
        }
    }
    return false;
}

int testGraphAssemblyDpiDisplay(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_dpi_display");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    if (design.topGraphs().size() != 1) {
        return fail("Expected exactly one top graph");
    }
    if (design.topGraphs().front() != "graph_assembly_dpi_display") {
        return fail("Unexpected top graph name");
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_dpi_display");
    if (!graph) {
        return fail("Missing graph_assembly_dpi_display graph");
    }

    wolvrix::lib::grh::OperationId displayOpId = wolvrix::lib::grh::OperationId::invalid();
    wolvrix::lib::grh::OperationId errorDisplayOpId = wolvrix::lib::grh::OperationId::invalid();
    std::unordered_map<std::string, wolvrix::lib::grh::OperationId> importOps;
    std::unordered_map<std::string, wolvrix::lib::grh::OperationId> callOps;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kSystemTask:
            if (auto name = getAttrString(op, "name")) {
                if (*name == "display") {
                    displayOpId = opId;
                } else if (*name == "error") {
                    errorDisplayOpId = opId;
                }
            }
            break;
        case wolvrix::lib::grh::OperationKind::kDpicImport:
            importOps.emplace(std::string(op.symbolText()), opId);
            break;
        case wolvrix::lib::grh::OperationKind::kDpicCall: {
            auto target = getAttrString(op, "targetImportSymbol");
            if (target) {
                callOps.emplace(*target, opId);
            }
            break;
        }
        default:
            break;
        }
    }

    if (!displayOpId.valid()) {
        return fail("Missing kSystemTask display op");
    }
    if (!errorDisplayOpId.valid()) {
        return fail("Missing kSystemTask error op");
    }

    const wolvrix::lib::grh::Operation displayOp = graph->getOperation(displayOpId);
    auto displayEdges = getAttrStrings(displayOp, "eventEdge");
    if (!displayEdges || displayEdges->size() != 1 || (*displayEdges)[0] != "posedge") {
        return fail("kSystemTask display missing eventEdge");
    }
    const auto displayOperands = displayOp.operands();
    if (displayOperands.size() != 4) {
        return fail("kSystemTask display operand count mismatch");
    }
    auto displayFormat = getConstLiteral(*graph, displayOperands[1]);
    if (!displayFormat || (*displayFormat != "a=%0d" && *displayFormat != "\"a=%0d\"")) {
        return fail("kSystemTask display format literal mismatch");
    }
    if (graph->getValue(displayOperands[2]).symbolText() != "a") {
        return fail("kSystemTask display arg operand mismatch");
    }
    if (graph->getValue(displayOperands[3]).symbolText() != "clk") {
        return fail("kSystemTask display event operand mismatch");
    }

    const wolvrix::lib::grh::Operation errorOp = graph->getOperation(errorDisplayOpId);
    auto errorEdges = getAttrStrings(errorOp, "eventEdge");
    if (!errorEdges || errorEdges->size() != 1 || (*errorEdges)[0] != "posedge") {
        return fail("error system task missing eventEdge");
    }
    const auto errorOperands = errorOp.operands();
    if (errorOperands.size() != 3) {
        return fail("error system task operand count mismatch");
    }
    auto errorFormat = getConstLiteral(*graph, errorOperands[1]);
    if (!errorFormat || (*errorFormat != "oops" && *errorFormat != "\"oops\"")) {
        return fail("error system task format literal mismatch");
    }
    if (graph->getValue(errorOperands[2]).symbolText() != "clk") {
        return fail("error system task event operand mismatch");
    }

    auto itCaptureImport = importOps.find("dpi_capture");
    if (itCaptureImport == importOps.end()) {
        return fail("Missing dpi_capture import op");
    }
    auto itAddImport = importOps.find("dpi_add");
    if (itAddImport == importOps.end()) {
        return fail("Missing dpi_add import op");
    }

    const wolvrix::lib::grh::Operation captureImport = graph->getOperation(itCaptureImport->second);
    auto capDirs = getAttrStrings(captureImport, "argsDirection");
    auto capWidths = getAttrInts(captureImport, "argsWidth");
    auto capNames = getAttrStrings(captureImport, "argsName");
    auto capSigned = getAttrBools(captureImport, "argsSigned");
    auto capReturn = getAttrBool(captureImport, "hasReturn");
    if (!capDirs || !capWidths || !capNames || !capSigned) {
        return fail("dpi_capture import missing arg metadata");
    }
    if (capDirs->size() != 2 || (*capDirs)[0] != "input" || (*capDirs)[1] != "output") {
        return fail("dpi_capture arg directions mismatch");
    }
    if (capWidths->size() != 2 || (*capWidths)[0] != 8 || (*capWidths)[1] != 8) {
        return fail("dpi_capture arg widths mismatch");
    }
    if (capNames->size() != 2 || (*capNames)[0] != "in_val" || (*capNames)[1] != "out_val") {
        return fail("dpi_capture arg names mismatch");
    }
    if (capSigned->size() != 2 || (*capSigned)[0] || (*capSigned)[1]) {
        return fail("dpi_capture arg signed mismatch");
    }
    if (!capReturn || *capReturn) {
        return fail("dpi_capture hasReturn mismatch");
    }

    const wolvrix::lib::grh::Operation addImport = graph->getOperation(itAddImport->second);
    auto addDirs = getAttrStrings(addImport, "argsDirection");
    auto addWidths = getAttrInts(addImport, "argsWidth");
    auto addNames = getAttrStrings(addImport, "argsName");
    auto addSigned = getAttrBools(addImport, "argsSigned");
    auto addReturn = getAttrBool(addImport, "hasReturn");
    auto addReturnWidth = getAttrInt(addImport, "returnWidth");
    auto addReturnSigned = getAttrBool(addImport, "returnSigned");
    if (!addDirs || !addWidths || !addNames || !addSigned || !addReturn || !addReturnWidth || !addReturnSigned) {
        return fail("dpi_add import missing metadata");
    }
    if (addDirs->size() != 2 || (*addDirs)[0] != "input" || (*addDirs)[1] != "input") {
        return fail("dpi_add arg directions mismatch");
    }
    if (addWidths->size() != 2 || (*addWidths)[0] != 32 || (*addWidths)[1] != 32) {
        return fail("dpi_add arg widths mismatch");
    }
    if (addNames->size() != 2 || (*addNames)[0] != "lhs" || (*addNames)[1] != "rhs") {
        return fail("dpi_add arg names mismatch");
    }
    if (addSigned->size() != 2 || !(*addSigned)[0] || !(*addSigned)[1]) {
        return fail("dpi_add arg signed mismatch");
    }
    if (!*addReturn || *addReturnWidth != 32 || !*addReturnSigned) {
        return fail("dpi_add return metadata mismatch");
    }

    auto itCaptureCall = callOps.find("dpi_capture");
    if (itCaptureCall == callOps.end()) {
        return fail("Missing dpi_capture call op");
    }
    auto itAddCall = callOps.find("dpi_add");
    if (itAddCall == callOps.end()) {
        return fail("Missing dpi_add call op");
    }

    const wolvrix::lib::grh::Operation captureCall = graph->getOperation(itCaptureCall->second);
    auto capCallEdges = getAttrStrings(captureCall, "eventEdge");
    auto capCallIn = getAttrStrings(captureCall, "inArgName");
    auto capCallOut = getAttrStrings(captureCall, "outArgName");
    auto capCallReturn = getAttrBool(captureCall, "hasReturn");
    if (!capCallEdges || capCallEdges->size() != 1 || (*capCallEdges)[0] != "posedge") {
        return fail("dpi_capture call eventEdge mismatch");
    }
    if (!capCallIn || capCallIn->size() != 1 || (*capCallIn)[0] != "in_val") {
        return fail("dpi_capture call inArgName mismatch");
    }
    if (!capCallOut || capCallOut->size() != 1 || (*capCallOut)[0] != "out_val") {
        return fail("dpi_capture call outArgName mismatch");
    }
    if (!capCallReturn || *capCallReturn) {
        return fail("dpi_capture call hasReturn mismatch");
    }
    const auto capCallOperands = captureCall.operands();
    if (capCallOperands.size() != 3) {
        return fail("dpi_capture call operand count mismatch");
    }
    const auto capInputValue = graph->getValue(capCallOperands[1]);
    if (capInputValue.width() != 8 || capInputValue.isSigned()) {
        return fail("dpi_capture call input operand width/signed mismatch");
    }
    if (graph->getValue(capCallOperands[2]).symbolText() != "clk") {
        return fail("dpi_capture call event operand mismatch");
    }
    const auto capCallResults = captureCall.results();
    if (capCallResults.size() != 1) {
        return fail("dpi_capture call result count mismatch");
    }
    if (graph->getValue(capCallResults[0]).symbolText().find("_val_") != 0) {
        return fail("dpi_capture call result mismatch");
    }

    const wolvrix::lib::grh::Operation addCall = graph->getOperation(itAddCall->second);
    auto addCallEdges = getAttrStrings(addCall, "eventEdge");
    auto addCallIn = getAttrStrings(addCall, "inArgName");
    auto addCallOut = getAttrStrings(addCall, "outArgName");
    auto addCallReturn = getAttrBool(addCall, "hasReturn");
    if (!addCallEdges || addCallEdges->size() != 1 || (*addCallEdges)[0] != "posedge") {
        return fail("dpi_add call eventEdge mismatch");
    }
    if (!addCallIn || addCallIn->size() != 2 || (*addCallIn)[0] != "lhs" || (*addCallIn)[1] != "rhs") {
        return fail("dpi_add call inArgName mismatch");
    }
    if (!addCallOut || !addCallOut->empty()) {
        return fail("dpi_add call outArgName mismatch");
    }
    if (!addCallReturn || !*addCallReturn) {
        return fail("dpi_add call hasReturn mismatch");
    }
    const auto addCallOperands = addCall.operands();
    if (addCallOperands.size() != 4) {
        return fail("dpi_add call operand count mismatch");
    }
    const auto addLhsValue = graph->getValue(addCallOperands[1]);
    const auto addRhsValue = graph->getValue(addCallOperands[2]);
    if (addLhsValue.width() != 32 || addRhsValue.width() != 32) {
        return fail("dpi_add call input operands width mismatch");
    }
    if (graph->getValue(addCallOperands[3]).symbolText() != "clk") {
        return fail("dpi_add call event operand mismatch");
    }
    const auto addCallResults = addCall.results();
    if (addCallResults.size() != 1) {
        return fail("dpi_add call result count mismatch");
    }
    if (graph->getValue(addCallResults[0]).symbolText().find("_val_") != 0) {
        return fail("dpi_add call return symbol mismatch");
    }

    return 0;
}

int testGraphAssemblyDpiCombReturn(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_dpi_comb_return");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile combinational DPI return fixture");
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("graph_assembly_dpi_comb_return");
    if (!graph) {
        return fail("Missing graph_assembly_dpi_comb_return graph");
    }

    wolvrix::lib::grh::OperationId importId =
        wolvrix::lib::grh::OperationId::invalid();
    wolvrix::lib::grh::OperationId callId =
        wolvrix::lib::grh::OperationId::invalid();
    for (const auto opId : graph->operations()) {
        const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kDpicImport &&
            op.symbolText() == "difftest_ram_read") {
            importId = opId;
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kDpicCall) {
            const auto target = getAttrString(op, "targetImportSymbol");
            if (target && *target == "difftest_ram_read") {
                if (callId.valid()) {
                    return fail("Expected one combinational difftest_ram_read call");
                }
                callId = opId;
            }
        }
    }
    if (!importId.valid() || !callId.valid()) {
        return fail("Missing combinational DPI import or call op");
    }

    const wolvrix::lib::grh::Operation call = graph->getOperation(callId);
    const auto callEdges = getAttrStrings(call, "eventEdge");
    const auto inputNames = getAttrStrings(call, "inArgName");
    const auto outputNames = getAttrStrings(call, "outArgName");
    const auto hasReturn = getAttrBool(call, "hasReturn");
    if (!callEdges || !callEdges->empty()) {
        return fail("Combinational DPI call should have an empty eventEdge list");
    }
    if (!inputNames || inputNames->size() != 1 || inputNames->front() != "rIdx" ||
        !outputNames || !outputNames->empty() || !hasReturn || !*hasReturn) {
        return fail("Combinational DPI call metadata mismatch");
    }
    if (call.operands().size() != 2 || call.results().size() != 1) {
        return fail("Combinational DPI call operand/result count mismatch");
    }
    if (graph->getValue(call.operands()[0]).symbolText() != "r_enable" ||
        graph->getValue(call.operands()[1]).symbolText() != "r_index") {
        return fail("Combinational DPI call guard/input binding mismatch");
    }
    const wolvrix::lib::grh::Value result = graph->getValue(call.results().front());
    if (result.width() != 64 || !result.isSigned()) {
        return fail("Combinational DPI return type mismatch");
    }

    const wolvrix::lib::grh::ValueId output = graph->outputPortValue("r_data");
    std::unordered_set<wolvrix::lib::grh::ValueId,
                       wolvrix::lib::grh::ValueIdHash> visited;
    if (!valueDependsOn(*graph, output, call.results().front(), visited)) {
        return fail("r_data does not depend on the combinational DPI result");
    }

    const std::filesystem::path artifactDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
        "ingest_graph_assembly_dpi_comb_return";
    const std::filesystem::path emittedPath = artifactDir / "dpi_comb_return.sv";
    std::error_code ec;
    std::filesystem::remove(emittedPath, ec);

    wolvrix::lib::emit::EmitDiagnostics emitDiagnostics;
    wolvrix::lib::emit::EmitSystemVerilog emitter(&emitDiagnostics);
    wolvrix::lib::emit::EmitOptions emitOptions;
    emitOptions.outputDir = artifactDir.string();
    emitOptions.outputFilename = emittedPath.filename().string();
    emitOptions.topOverrides = {"graph_assembly_dpi_comb_return"};
    const wolvrix::lib::emit::EmitResult emitResult = emitter.emit(design, emitOptions);
    if (!emitResult.success || emitDiagnostics.hasError()) {
        return fail("Failed to emit combinational DPI return fixture");
    }
    if (emitResult.artifacts.size() != 1) {
        return fail("Combinational DPI return emit artifact count mismatch");
    }

    const std::string emitted = readFile(emittedPath);
    if (emitted.empty()) {
        return fail("Failed to read emitted combinational DPI return fixture");
    }
    const std::string resultName(result.symbolText());
    if (emitted.find("import \"DPI-C\" function longint difftest_ram_read") ==
            std::string::npos ||
        emitted.find("always @* begin") == std::string::npos ||
        emitted.find("if (r_enable) begin") == std::string::npos ||
        emitted.find(resultName + "_intm = difftest_ram_read(r_index);") ==
            std::string::npos ||
        emitted.find("assign " + resultName + " = " + resultName + "_intm;") ==
            std::string::npos ||
        emitted.find("posedge") != std::string::npos) {
        return fail("Emitted combinational DPI return lost guard, input, or result binding");
    }

    auto readbackBundle =
        compileInput(emittedPath, "graph_assembly_dpi_comb_return");
    if (!readbackBundle || !readbackBundle->compilation) {
        return fail("Slang failed to read emitted combinational DPI return fixture");
    }
    if (!readbackBundle->driver.reportDiagnostics(/* quiet */ true)) {
        return fail("Slang reported diagnostics for emitted combinational DPI return fixture");
    }
    wolvrix::lib::ingest::ConvertDriver readbackDriver;
    wolvrix::lib::grh::Design readbackDesign =
        readbackDriver.convert(readbackBundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* readbackGraph =
        readbackDesign.findGraph("graph_assembly_dpi_comb_return");
    if (!readbackGraph) {
        return fail("Readback design is missing graph_assembly_dpi_comb_return");
    }

    wolvrix::lib::grh::OperationId readbackCallId =
        wolvrix::lib::grh::OperationId::invalid();
    for (const auto opId : readbackGraph->operations()) {
        const wolvrix::lib::grh::Operation op = readbackGraph->getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kDpicCall) {
            continue;
        }
        const auto target = getAttrString(op, "targetImportSymbol");
        if (!target || *target != "difftest_ram_read") {
            continue;
        }
        if (readbackCallId.valid()) {
            return fail("Readback contains multiple combinational difftest_ram_read calls");
        }
        readbackCallId = opId;
    }
    if (!readbackCallId.valid()) {
        return fail("Readback lost combinational difftest_ram_read call");
    }

    const wolvrix::lib::grh::Operation readbackCall =
        readbackGraph->getOperation(readbackCallId);
    const auto readbackEdges = getAttrStrings(readbackCall, "eventEdge");
    if (!readbackEdges || !readbackEdges->empty() ||
        readbackCall.operands().size() != 2 || readbackCall.results().size() != 1) {
        return fail("Readback combinational DPI call shape mismatch");
    }
    if (readbackGraph->getValue(readbackCall.operands()[0]).symbolText() != "r_enable" ||
        readbackGraph->getValue(readbackCall.operands()[1]).symbolText() != "r_index") {
        return fail("Readback combinational DPI guard/input binding mismatch");
    }
    const wolvrix::lib::grh::Value readbackResult =
        readbackGraph->getValue(readbackCall.results().front());
    if (readbackResult.width() != 64 || !readbackResult.isSigned()) {
        return fail("Readback combinational DPI return type mismatch");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_GRAPH_ASSEMBLY_DPI_DISPLAY_DATA_PATH;
    if (int status = testGraphAssemblyDpiDisplay(sourcePath); status != 0) {
        return status;
    }
    return testGraphAssemblyDpiCombReturn(sourcePath);
}
