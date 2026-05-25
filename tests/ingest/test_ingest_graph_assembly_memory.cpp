#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-memory] " << message << '\n';
    return 1;
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
    argStorage.emplace_back("ingest-graph-assembly-memory");
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

std::optional<std::vector<bool>> getAttrBools(const wolvrix::lib::grh::Operation& op,
                                              std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::vector<bool>>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

int testGraphAssemblyMemory(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_memory");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    if (design.topGraphs().size() != 1) {
        return fail("Expected exactly one top graph");
    }
    if (design.topGraphs().front() != "graph_assembly_memory") {
        return fail("Unexpected top graph name");
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_memory");
    if (!graph) {
        return fail("Missing graph_assembly_memory graph");
    }

    int memoryOps = 0;
    int readOps = 0;
    int writeOps = 0;
    bool memoryAttrsOk = false;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kMemory: {
            ++memoryOps;
            auto width = getAttrInt(op, "width");
            auto rows = getAttrInt(op, "row");
            auto isSigned = getAttrBool(op, "isSigned");
            if (!width || !rows || !isSigned) {
                return fail("kMemory missing width/row/isSigned attributes");
            }
            if (*width != 8 || *rows != 16 || *isSigned) {
                return fail("kMemory attributes do not match expected width/row/isSigned");
            }
            auto initKinds = getAttrStrings(op, "initKind");
            auto initFiles = getAttrStrings(op, "initFile");
            auto starts = getAttrInts(op, "initStart");
            auto lens = getAttrInts(op, "initLen");
            if (!initKinds || !initFiles || !starts || !lens) {
                return fail("kMemory missing init attributes");
            }
            if (initKinds->size() != 2 || initFiles->size() != 2 || starts->size() != 2 ||
                lens->size() != 2) {
                return fail("kMemory init attribute sizes do not match expected count");
            }
            if ((*initKinds)[0] != "readmemh" || (*initKinds)[1] != "readmemb") {
                return fail("kMemory initKind order mismatch");
            }
            if ((*initFiles)[0] != "mem_init.hex" || (*initFiles)[1] != "mem_init.bin") {
                return fail("kMemory initFile order mismatch");
            }
            if ((*starts)[0] != -1 || (*lens)[0] != 0) {
                return fail("kMemory initStart/initLen mismatch for readmemh");
            }
            if ((*starts)[1] != 2 || (*lens)[1] != 6) {
                return fail("kMemory initStart/initLen mismatch for readmemb");
            }
            memoryAttrsOk = true;
            break;
        }
        case wolvrix::lib::grh::OperationKind::kMemoryReadPort: {
            ++readOps;
            auto memSymbol = getAttrString(op, "memSymbol");
            if (!memSymbol || *memSymbol != "mem") {
                return fail("kMemoryReadPort missing or unexpected memSymbol");
            }
            break;
        }
        case wolvrix::lib::grh::OperationKind::kMemoryWritePort: {
            ++writeOps;
            auto memSymbol = getAttrString(op, "memSymbol");
            if (!memSymbol || *memSymbol != "mem") {
                return fail("kMemoryWritePort missing or unexpected memSymbol");
            }
            auto edges = getAttrStrings(op, "eventEdge");
            if (!edges || edges->empty() || (*edges)[0] != "posedge") {
                return fail("kMemoryWritePort missing eventEdge attribute");
            }
            break;
        }
        default:
            break;
        }
    }

    if (!memoryAttrsOk || memoryOps != 1) {
        return fail("Expected exactly one kMemory op with valid attributes");
    }
    if (readOps < 2) {
        return fail("Expected at least two kMemoryReadPort ops");
    }
    if (writeOps != 1) {
        return fail("Expected exactly one kMemoryWritePort op");
    }

    return 0;
}

int testGraphAssemblyPackedAggregateReg(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_packed_aggregate_reg");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("graph_assembly_packed_aggregate_reg");
    if (!graph) {
        return fail("Missing graph_assembly_packed_aggregate_reg graph");
    }

    int memoryOps = 0;
    int r0Regs = 0;
    int r1Regs = 0;
    bool sawWideR0Read = false;
    bool sawWideR1Read = false;
    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kMemory) {
            ++memoryOps;
            continue;
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister) {
            auto width = getAttrInt(op, "width");
            if (!width) {
                return fail("Packed aggregate register missing width attribute");
            }
            if (op.symbolText() == std::string_view("r0")) {
                ++r0Regs;
                if (*width != 32) {
                    return fail("Packed aggregate r0 register width should be flattened to 32");
                }
            }
            if (op.symbolText() == std::string_view("r1")) {
                ++r1Regs;
                if (*width != 32) {
                    return fail("Packed aggregate r1 register width should be flattened to 32");
                }
            }
            continue;
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterReadPort) {
            auto regSymbol = getAttrString(op, "regSymbol");
            if (!regSymbol || op.results().empty()) {
                continue;
            }
            wolvrix::lib::grh::Value value = graph->getValue(op.results().front());
            if (*regSymbol == "r0" && value.width() == 32) {
                sawWideR0Read = true;
            }
            if (*regSymbol == "r1" && value.width() == 32) {
                sawWideR1Read = true;
            }
        }
    }

    if (memoryOps != 0) {
        return fail("Whole packed aggregate registers should not create kMemory ops");
    }
    if (r0Regs != 1 || r1Regs != 1 || !sawWideR0Read || !sawWideR1Read) {
        return fail("Expected flattened packed aggregate register/read ports for r0/r1");
    }
    return 0;
}

int testGraphAssemblyMemoryReadCoalesce(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_memory_read_coalesce");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("graph_assembly_memory_read_coalesce");
    if (!graph) {
        return fail("Missing graph_assembly_memory_read_coalesce graph");
    }

    int readOps = 0;
    int sliceOps = 0;
    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kMemoryReadPort) {
            ++readOps;
            auto memSymbol = getAttrString(op, "memSymbol");
            if (!memSymbol || *memSymbol != "mem") {
                return fail("Coalesced kMemoryReadPort missing memSymbol");
            }
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kSliceStatic ||
            op.kind() == wolvrix::lib::grh::OperationKind::kSliceDynamic) {
            ++sliceOps;
        }
    }

    if (readOps != 1) {
        return fail("Expected same-address memory reads to coalesce into one read port");
    }
    if (sliceOps < 2) {
        return fail("Expected bit outputs to be emitted as slices from coalesced read");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_GRAPH_ASSEMBLY_MEMORY_DATA_PATH;
    if (int result = testGraphAssemblyMemory(sourcePath); result != 0) {
        return result;
    }
    if (int result = testGraphAssemblyPackedAggregateReg(sourcePath); result != 0) {
        return result;
    }
    return testGraphAssemblyMemoryReadCoalesce(sourcePath);
}
