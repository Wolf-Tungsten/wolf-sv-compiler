// NO0017 D-layer debug driver: run a post-stats GRH design through the AM
// pipeline (lower -> optimize -> schedule) and execute it under the AM
// interpreter with a minimal coremark host (flash + RAM services), so the
// imported gsim executable-GRH model can be stepped and inspected in-process
// without building the C++ emu. Usage:
//   grhsim-am-import-debug <post-stats.json> <top> <image.bin> [cycles]
#include "core/grh.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_graph.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_graph_optimize.hpp"
#include "grhsim/am/grhsim_am_program_interpreter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

bool readFile(const std::filesystem::path &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(out.data(), static_cast<std::streamsize>(size));
    return in.good() || in.eof();
}

class CoremarkHost final : public HostEnvironment {
public:
    explicit CoremarkHost(const std::string &imagePath) {
        std::ifstream in(imagePath, std::ios::binary);
        image.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool resolveSystemFunction(ProgramView, InstructionId, std::string &) override { return true; }
    bool resolveSystemTask(ProgramView, InstructionId, std::string &) override { return true; }
    bool resolveDpiCall(ProgramView, InstructionId, std::string &) override { return true; }

    bool invokeSystemFunction(ProgramView program, InstructionId, std::span<const InterpreterValue>,
                              InterpreterValue &result, std::string &) override {
        result = zeroOf(program.type(program.dpiImport(DpiImportId{0}).returnValue.type));
        return true;
    }

    bool invokeSystemTask(ProgramView program, InstructionId instruction,
                          std::span<const InterpreterValue> arguments, std::string &) override {
        const auto attributes = program.systemTaskAttributes(instruction);
        const std::string name(attributes ? program.string(attributes->name) : "");
        if (name == "finish" || name == "fatal") {
            if (name == "fatal") {
                ++fatalCount;
                std::string message;
                for (const InterpreterValue &arg : arguments) {
                    if (arg.type().kind == TypeKind::String) {
                        if (!message.empty()) message += " | ";
                        message += arg.bytes();
                    }
                }
                fatalMessages[message] += 1;
                if (fatalCount <= 8 ||
                    (rowsumPrints < 8 && message.find("row sum") != std::string::npos)) {
                    if (message.find("row sum") != std::string::npos) ++rowsumPrints;
                    std::string argDump;
                    char buf[40];
                    for (std::size_t ai = 0; ai < arguments.size(); ++ai) {
                        if (arguments[ai].type().kind == TypeKind::String) continue;
                        std::snprintf(buf, sizeof(buf), " a%zu=%llx", ai,
                                      (unsigned long long)arguments[ai].lowWord());
                        argDump += buf;
                    }
                    std::fprintf(stderr,
                                 "[host] fatal at eval %llu instr %u:%s [%s]\n",
                                 (unsigned long long)evalCount, instruction.value,
                                 message.c_str(), argDump.c_str());
                }
            }
            if (name == "finish") finishSeen = true;
        }
        return true;
    }

    bool invokeDpiCall(ProgramView program, InstructionId instruction,
                       std::span<const InterpreterValue> arguments,
                       std::vector<InterpreterValue> &results, std::string &error) override {
        const auto attributes = program.dpiCallAttributes(instruction);
        if (!attributes) {
            error = "missing dpi attributes";
            return false;
        }
        const std::string symbol(program.string(attributes->importSymbol));
        ++dpiCalls[symbol];
        if (symbol == "v_difftest_TrapEvent" && trapPrints < 6) {
            ++trapPrints;
            std::string dump;
            for (std::size_t index = 0; index < arguments.size() && index < 10; ++index) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%s%llx", index == 0 ? "" : " ",
                              (unsigned long long)arguments[index].lowWord());
                dump += buf;
            }
            std::fprintf(stderr, "[host] TrapEvent args: %s\n", dump.c_str());
        }
        if (symbol == "v_difftest_InstrCommit" && !arguments.empty() &&
            arguments[0].lowWord() != 0) {
            ++commitCount;
        }
        results.clear();
        // Build results with the exact AM Types of the instruction's result
        // variables (the interpreter type-checks against them).
        for (const VariableId resultVar : program.results(instruction)) {
            results.push_back(produce(symbol, program.type(program.variable(resultVar).type),
                                      arguments));
        }
        if (dpiStream) {
            std::string line;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "c=%llu e=%llu s=%s",
                          (unsigned long long)cycle, (unsigned long long)evalCount,
                          symbol.c_str());
            line += buf;
            for (std::size_t i = 0; i < arguments.size() && i < 8; ++i) {
                std::snprintf(buf, sizeof(buf), " a%zu=%llx", i,
                              (unsigned long long)arguments[i].lowWord());
                line += buf;
            }
            for (std::size_t i = 0; i < results.size() && i < 2; ++i) {
                std::snprintf(buf, sizeof(buf), " r%zu=%llx", i,
                              (unsigned long long)results[i].lowWord());
                line += buf;
            }
            line += '\n';
            std::fwrite(line.data(), 1, line.size(), dpiStream);
        }
        return true;
    }

    uint64_t evalCount = 0;
    uint64_t commitCount = 0;
    uint64_t fatalCount = 0;
    uint64_t trapPrints = 0;
    uint64_t rowsumPrints = 0;
    uint64_t cycle = 0;
    FILE *dpiStream = nullptr;
    // NO0017: per-eval probes on selected instructions' operand values.
    std::vector<std::pair<uint32_t, uint32_t>> probes;  // (instruction, operand index)
    bool finishSeen = false;
    std::unordered_map<std::string, uint64_t> dpiCalls;
    std::unordered_map<std::string, uint64_t> fatalMessages;

private:
    std::vector<char> image;
    std::unordered_map<uint64_t, uint64_t> ram;

    static InterpreterValue zeroOf(const Type &type) { return InterpreterValue::zero(type); }

    DpiImportView findImport(ProgramView program, const std::string &symbol) {
        const auto it = importCache.find(symbol);
        if (it != importCache.end()) return it->second;
        for (std::size_t index = 0; index < program.dpiImportCount(); ++index) {
            DpiImportView view = program.dpiImport(DpiImportId{static_cast<uint32_t>(index)});
            if (program.string(view.symbol) == symbol) {
                DpiImportView copy{view.symbol, {}, view.returnValue};
                // Copy parameters into owned storage (the view spans program memory;
                // keep a persistent copy of the records we use).
                ownedParams[symbol] = std::vector<DpiParameter>(view.parameters.begin(),
                                                                view.parameters.end());
                copy.parameters = ownedParams[symbol];
                importCache.emplace(symbol, copy);
                return importCache[symbol];
            }
        }
        static const DpiImportView empty{};
        return empty;
    }

    InterpreterValue produce(const std::string &symbol, const Type &type,
                             std::span<const InterpreterValue> arguments) {
        if (symbol == "flash_read") {
            const uint64_t addr = arguments.empty() ? 0 : arguments[0].lowWord();
            const uint64_t aligned = addr & ~UINT64_C(7);
            if (dpiCalls[symbol] <= 12) {
                std::fprintf(stderr, "[host] flash_read #%llu addr=0x%llx\n",
                             (unsigned long long)dpiCalls[symbol],
                             (unsigned long long)aligned);
            }
            uint64_t value = 0;
            if (aligned + 8 <= image.size()) {
                std::memcpy(&value, image.data() + aligned, 8);
            }
            return withWords(type, value);
        }
        if (symbol == "difftest_ram_read") {
            const uint64_t wordIndex = arguments.empty() ? 0 : arguments[0].lowWord();
            if (dpiCalls[symbol] <= 12) {
                std::fprintf(stderr, "[host] ram_read #%llu widx=0x%llx\n",
                             (unsigned long long)dpiCalls[symbol],
                             (unsigned long long)wordIndex);
            }
            uint64_t value = 0;
            const auto it = ram.find(wordIndex);
            if (it != ram.end()) {
                value = it->second;
            } else if (wordIndex * 8 + 8 <= image.size()) {
                std::memcpy(&value, image.data() + wordIndex * 8, 8);
            }
            return withWords(type, value);
        }
        if (symbol == "difftest_ram_write" && arguments.size() >= 3) {
            const uint64_t wordIndex = arguments[0].lowWord();
            const uint64_t data = arguments[1].lowWord();
            const uint64_t mask = arguments[2].lowWord();
            uint64_t &slot = ram[wordIndex];
            for (unsigned byte = 0; byte < 8; ++byte) {
                if ((mask >> byte) & 1U) {
                    slot = (slot & ~(UINT64_C(0xff) << (byte * 8))) |
                           ((data >> (byte * 8) & 0xffU) << (byte * 8));
                }
            }
        }
        return InterpreterValue::zero(type);
    }

    static InterpreterValue withWords(const Type &type, uint64_t value) {
        return InterpreterValue::bitVector(type.bitWidth, type.signedness,
                                           std::vector<uint64_t>{value});
    }

    std::unordered_map<std::string, DpiImportView> importCache;
    std::unordered_map<std::string, std::vector<DpiParameter>> ownedParams;
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: grhsim-am-import-debug <post-stats.json> <top> <image.bin> [cycles]\n";
        return 2;
    }
    const std::filesystem::path jsonPath = argv[1];
    const std::string top = argv[2];
    const std::string imagePath = argv[3];
    const uint64_t maxCycles = argc > 4 ? std::stoull(argv[4]) : 2000;

    std::string json;
    if (!readFile(jsonPath, json)) {
        std::cerr << "failed to read " << jsonPath << '\n';
        return 2;
    }
    grh::Design design = grh::Design::fromJsonString(json);
    std::string().swap(json);
    grh::Graph *graph = design.findGraph(top);
    if (!graph) {
        std::cerr << "top graph not found: " << top << '\n';
        return 2;
    }

    diag::Diagnostics diagnostics;
    GrhIRToGrhSimAMGraphLowering lowering(GrhIRToGrhSimAMGraphLoweringOptions{
        .unknownLogic = UnknownLogicPolicy::FlattenToZero,
    });
    std::optional<AmGraph> amGraph = lowering.lower(*graph, diagnostics);
    if (!amGraph || diagnostics.hasError()) {
        std::cerr << "lower failed\n";
        for (const auto &message : diagnostics.messages()) {
            std::cerr << "  [diag] " << message.message;
            if (!message.context.empty()) std::cerr << "  @ " << message.context;
            std::cerr << '\n';
        }
        return 1;
    }
    if (!optimizeAmGraph(*amGraph, AmOptimizeOptions{}, diagnostics) || diagnostics.hasError()) {
        std::cerr << "optimize failed\n";
        return 1;
    }
    std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
        std::move(*amGraph),
        ActivityScheduleOptions{
            .maxAtomsPerBlock = 15,
            .dpCoarsenAtomBudget = 7000,
            .dpRefinementRounds = 0,
            .mergeWhenMinGroup = 5,
        },
        diagnostics);
    if (!model || diagnostics.hasError()) {
        std::cerr << "schedule failed\n";
        return 1;
    }
    const ProgramView program = model->program.view();

    const auto findPort = [&](std::string_view name) -> VariableId {
        for (const PortBinding &port : model->interface.ports) {
            if (program.string(port.name) == name) {
                return port.direction == PortDirection::Input ? port.input : port.output;
            }
        }
        return VariableId::invalid();
    };
    const VariableId clockVar = findPort("clock");
    const VariableId resetVar = findPort("reset");
    const VariableId uartValidVar = findPort("difftest$$uart$$out$$valid");
    const VariableId uartChVar = findPort("difftest$$uart$$out$$ch");
    if (!clockVar.valid() || !resetVar.valid()) {
        std::cerr << "clock/reset ports not found\n";
        return 2;
    }

    // NO0017: name-based watch list — variable labels carry the
    // gsim.node_name provenance (freeList / replacer / mshr / priority).
    // Filter substrings and cap are env-overridable for trace comparisons.
    struct Watch {
        VariableId id;
        std::string name;
        uint32_t width;
    };
    std::vector<std::string> watchKeys = {"freeList", "replacer", "mshr", "priority"};
    if (const char *env = std::getenv("AM_DEBUG_WATCH_NAMES")) {
        watchKeys.clear();
        std::string list(env);
        std::size_t pos = 0;
        while (pos <= list.size()) {
            const std::size_t comma = list.find(',', pos);
            const std::string token = list.substr(pos, comma == std::string::npos
                                                            ? std::string::npos
                                                            : comma - pos);
            if (!token.empty()) watchKeys.push_back(token);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    std::size_t watchMax = 64;
    if (const char *env = std::getenv("AM_DEBUG_WATCH_MAX")) {
        watchMax = static_cast<std::size_t>(std::stoull(env));
    }
    std::vector<Watch> watches;
    for (const VariableLabel &label : program.variableLabels()) {
        if (watches.size() >= watchMax) break;
        const std::string name(program.string(label.label));
        bool match = false;
        for (const std::string &key : watchKeys) {
            if (name.find(key) != std::string::npos) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        const Type &type = program.type(program.variable(label.variable).type);
        if (type.kind != TypeKind::BitVector) continue;
        watches.push_back(Watch{label.variable, name, type.bitWidth});
    }
    std::fprintf(stderr, "[driver] watching %zu named state variables\n", watches.size());

    CoremarkHost host(imagePath);
    if (const char *env = std::getenv("AM_DEBUG_DPI_STREAM")) {
        host.dpiStream = std::fopen(env, "w");
    }
    FILE *watchStream = nullptr;
    if (const char *env = std::getenv("AM_DEBUG_WATCH_STREAM")) {
        watchStream = std::fopen(env, "w");
    }
    uint64_t snapCycle = UINT64_MAX;
    FILE *snapFile = nullptr;
    if (const char *env = std::getenv("AM_DEBUG_SNAPSHOT_AT")) {
        const std::string spec(env);
        const std::size_t comma = spec.find(',');
        if (comma != std::string::npos) {
            snapCycle = std::stoull(spec.substr(0, comma));
            snapFile = std::fopen(spec.substr(comma + 1).c_str(), "w");
        }
    }
    const auto hexOf = [](const InterpreterValue &value) {
        std::string out;
        char buf[20];
        const std::span<const uint64_t> words = value.words();
        for (std::size_t i = words.size(); i-- > 0;) {
            std::snprintf(buf, sizeof(buf), i == words.size() - 1 ? "%llx" : "%016llx",
                          (unsigned long long)words[i]);
            out += buf;
        }
        return out;
    };
    Interpreter interpreter(*model, &host);
    // Probe the flash_read request-valid operand (instruction id from the
    // dpi-trace pend-set lines) plus its immediate producer cone values.
    const char *probeEnv = std::getenv("AM_DEBUG_PROBE_INSTR");
    std::vector<VariableId> probeVars;
    InstructionId probeInstr{0};
    if (probeEnv) {
        probeInstr = InstructionId{static_cast<uint32_t>(std::stoul(probeEnv))};
        const auto operands = program.operands(probeInstr);
        for (VariableId operand : operands) probeVars.push_back(operand);
    }
    const char *probeVarsEnv = std::getenv("AM_DEBUG_PROBE_VARS");
    if (probeVarsEnv) {
        std::string list(probeVarsEnv);
        std::size_t pos = 0;
        while (pos < list.size()) {
            const std::size_t comma = list.find(',', pos);
            const std::string token = list.substr(pos, comma == std::string::npos
                                                        ? std::string::npos
                                                        : comma - pos);
            if (!token.empty()) {
                probeVars.push_back(VariableId{static_cast<uint32_t>(std::stoul(token))});
            }
            pos = comma == std::string::npos ? list.size() : comma + 1;
        }
    }
    if (!interpreter.ready()) {
        std::cerr << "interpreter init failed\n";
        if (interpreter.initializationDiagnostic()) {
            std::cerr << interpreter.initializationDiagnostic()->message << '\n';
        }
        return 1;
    }

    const auto drive = [&](VariableId var, uint64_t value) {
        const uint64_t words[1] = {value};
        interpreter.write(var, InterpreterValue::bitVector(1, Signedness::Unsigned, words));
    };
    const auto readOut = [&](VariableId var) -> uint64_t {
        if (!var.valid()) return 0;
        return interpreter.value(var).lowWord();
    };

    // NO0017: state-change survey — snapshot all variable values at chosen
    // evals and count diffs, to see whether state keeps moving under reset.
    std::vector<uint64_t> snapshot;
    const auto snapshotNow = [&]() {
        const std::span<const InterpreterValue> values = interpreter.values();
        snapshot.clear();
        snapshot.reserve(values.size());
        for (const InterpreterValue &value : values) snapshot.push_back(value.lowWord());
    };
    const auto diffSince = [&]() -> uint64_t {
        const std::span<const InterpreterValue> values = interpreter.values();
        uint64_t diffs = 0;
        const std::size_t n = std::min(values.size(), snapshot.size());
        for (std::size_t index = 0; index < n; ++index) {
            if (values[index].lowWord() != snapshot[index]) ++diffs;
        }
        return diffs;
    };
    const std::unordered_map<uint64_t, bool> surveyEvals = {
        {1, true}, {2, true}, {4, true}, {8, true}, {12, true}, {16, true},
        {20, true}, {24, true}, {30, true}, {40, true}, {60, true}, {100, true},
    };

    for (uint64_t cycle = 0; cycle < maxCycles; ++cycle) {
        const uint64_t reset = cycle < 10 ? 1 : 0;
        host.cycle = cycle;
        uint64_t roundsThisCycle = 0;
        for (const uint64_t clk : {1, 0}) {
            drive(resetVar, reset);
            drive(clockVar, clk);
            const InterpreterResult result = interpreter.eval();
            roundsThisCycle += result.roundsExecuted;
            ++host.evalCount;
            if (!result.success()) {
                std::cerr << "[driver] eval failed at eval " << host.evalCount << ": "
                          << result.diagnostic->message << '\n';
                return 1;
            }
            if (readOut(uartValidVar) != 0) {
                std::fprintf(stderr, "%c", static_cast<char>(readOut(uartChVar) & 0xff));
            }
            if (surveyEvals.count(host.evalCount)) {
                const uint64_t diffs = snapshot.empty() ? 0 : diffSince();
                std::fprintf(stderr, "[survey] eval=%llu changedVars=%llu\n",
                             (unsigned long long)host.evalCount,
                             (unsigned long long)diffs);
                snapshotNow();
            }
            if (!probeVars.empty()) {
                std::string dump;
                char buf[32];
                for (const VariableId var : probeVars) {
                    std::snprintf(buf, sizeof(buf), " %u:%llx", var.value,
                                  (unsigned long long)interpreter.value(var).lowWord());
                    dump += buf;
                }
                std::fprintf(stderr, "[probe] eval=%llu%s\n",
                             (unsigned long long)host.evalCount, dump.c_str());
            }
        }
        if (watchStream) {
            for (const Watch &watch : watches) {
                std::fprintf(watchStream, "c=%llu %s=%s\n", (unsigned long long)cycle,
                             watch.name.c_str(), hexOf(interpreter.value(watch.id)).c_str());
            }
            std::fflush(watchStream);
        }
        if (cycle == snapCycle && snapFile) {
            for (const VariableLabel &label : program.variableLabels()) {
                const std::string_view name = program.string(label.label);
                std::fprintf(snapFile, "%.*s=%s\n", static_cast<int>(name.size()),
                             name.data(),
                             hexOf(interpreter.value(label.variable)).c_str());
            }
            std::fflush(snapFile);
            std::fprintf(stderr, "[driver] snapshot dumped at cycle %llu\n",
                         (unsigned long long)cycle);
        }
        if ((cycle + 1) % 200 == 0) {
            std::fprintf(stderr,
                         "\n[driver] cycle=%llu rounds=%llu flash=%llu ram_r=%llu ram_w=%llu commits=%llu fatals=%llu\n",
                         (unsigned long long)(cycle + 1),
                         (unsigned long long)roundsThisCycle,
                         (unsigned long long)host.dpiCalls["flash_read"],
                         (unsigned long long)host.dpiCalls["difftest_ram_read"],
                         (unsigned long long)host.dpiCalls["difftest_ram_write"],
                         (unsigned long long)host.commitCount,
                         (unsigned long long)host.fatalCount);
            // NO0017: dump set-bit positions of wide (>=64b) state variables
            // matching ROB-valid-vector widths, to test bit-layout hypotheses.
            const char *dumpWidthEnv = std::getenv("AM_DEBUG_DUMP_WIDTH");
            if (dumpWidthEnv) {
                const uint32_t want = static_cast<uint32_t>(std::stoul(dumpWidthEnv));
                const std::span<const InterpreterValue> values = interpreter.values();
                for (uint32_t vi = 0; vi < values.size(); ++vi) {
                    if (values[vi].type().kind != TypeKind::BitVector ||
                        values[vi].type().bitWidth != want) {
                        continue;
                    }
                    std::string bits;
                    char buf[16];
                    uint32_t width = want;
                    for (uint32_t b = 0; b < width && bits.size() < 240; ++b) {
                        if ((values[vi].words()[b / 64] >> (b % 64)) & 1U) {
                            std::snprintf(buf, sizeof(buf), "%u ", b);
                            bits += buf;
                        }
                    }
                    std::fprintf(stderr, "[bits] var=%u w=%u set={%s}\n", vi, width,
                                 bits.c_str());
                }
            }
            for (const Watch &watch : watches) {
                const InterpreterValue &value = interpreter.value(watch.id);
                uint64_t popcount = 0;
                for (const uint64_t word : value.words()) {
                    popcount += static_cast<uint64_t>(__builtin_popcountll(word));
                }
                std::fprintf(stderr, "[watch] %s (w=%u) popcount=%llu\n",
                             watch.name.c_str(), watch.width,
                             (unsigned long long)popcount);
            }
        }
    }
    std::fprintf(stderr,
                 "\n[driver] done cycles=%llu flash=%llu ram_r=%llu ram_w=%llu commits=%llu fatals=%llu finish=%d\n",
                 (unsigned long long)maxCycles,
                 (unsigned long long)host.dpiCalls["flash_read"],
                 (unsigned long long)host.dpiCalls["difftest_ram_read"],
                 (unsigned long long)host.dpiCalls["difftest_ram_write"],
                 (unsigned long long)host.commitCount,
                 (unsigned long long)host.fatalCount,
                 host.finishSeen ? 1 : 0);
    std::vector<std::pair<std::string, uint64_t>> ranked(host.fatalMessages.begin(),
                                                         host.fatalMessages.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (std::size_t index = 0; index < ranked.size() && index < 12; ++index) {
        std::fprintf(stderr, "[driver] fatal x%llu: %s\n",
                     (unsigned long long)ranked[index].second, ranked[index].first.c_str());
    }
    std::vector<std::pair<std::string, uint64_t>> dpiRanked(host.dpiCalls.begin(),
                                                            host.dpiCalls.end());
    std::sort(dpiRanked.begin(), dpiRanked.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (const auto &[symbol, count] : dpiRanked) {
        std::fprintf(stderr, "[driver] dpi %s x%llu\n", symbol.c_str(),
                     (unsigned long long)count);
    }
    return 0;
}
