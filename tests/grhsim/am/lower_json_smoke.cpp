#include "core/grh.hpp"
#include "grhsim/am/grhsim_am_program_cpp_emitter.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_graph_optimize.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

namespace
{

    uint64_t elapsedMilliseconds(std::chrono::steady_clock::time_point start)
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }

    uint64_t peakRssKiB()
    {
        rusage usage{};
        return getrusage(RUSAGE_SELF, &usage) == 0
                   ? static_cast<uint64_t>(usage.ru_maxrss)
                   : 0;
    }

    uint64_t currentRssKiB()
    {
        std::ifstream statm("/proc/self/statm");
        uint64_t totalPages = 0;
        uint64_t residentPages = 0;
        if (!(statm >> totalPages >> residentPages))
        {
            return 0;
        }
        (void)totalPages;
        const long pageSize = sysconf(_SC_PAGESIZE);
        return pageSize > 0
                   ? residentPages * static_cast<uint64_t>(pageSize) / 1024
                   : 0;
    }

    void printDiagnostics(const diag::Diagnostics &diagnostics)
    {
        std::size_t printed = 0;
        for (const diag::Diagnostic &message : diagnostics.messages())
        {
            if (message.kind == diag::DiagnosticKind::Debug)
            {
                continue;
            }
            if (printed == 100)
            {
                break;
            }
            std::cerr << message.message;
            if (!message.context.empty())
            {
                std::cerr << " [" << message.context << ']';
            }
            std::cerr << '\n';
            ++printed;
        }
        if (diagnostics.messages().size() > printed)
        {
            std::cerr << "... additional diagnostics suppressed\n";
        }
    }

    // Incremental variant for the success path: prints messages appended since
    // the cursor position and advances it, so phase stats (lowering, optimize)
    // surface at the point they are produced without duplicate output.
    void printNewDiagnostics(const diag::Diagnostics &diagnostics,
                             std::size_t &cursor)
    {
        const auto &messages = diagnostics.messages();
        while (cursor < messages.size())
        {
            const diag::Diagnostic &message = messages[cursor++];
            if (message.kind == diag::DiagnosticKind::Debug)
            {
                continue;
            }
            std::cerr << message.message;
            if (!message.context.empty())
            {
                std::cerr << " [" << message.context << ']';
            }
            std::cerr << '\n';
        }
    }

    bool readFile(const std::string &path, std::string &contents)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return false;
        }
        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        if (size <= 0)
        {
            return false;
        }
        contents.resize(static_cast<std::size_t>(size));
        input.seekg(0, std::ios::beg);
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        return input.good() || input.eof();
    }

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: grhsim-am-lower-json <design.json> [top] [--schedule] "
                     "[--emit <output-directory>] [--blocks-per-source <count>] "
                     "[--max-source-bytes <count>] [--max-commit-source-bytes <count>] "
                     "[--block-chunk-instructions <count>] "
                     "[--max-atoms-per-block <count>] "
                     "[--dp-segment-penalty <value>] [--merge-when-min-group <count>] "
                     "[--dp-coarsen-atom-budget <count>] [--dp-coarsen-instr-budget <count>] [--disable-coarsening] "
                     "[--tree-atom-fold-max-instr <count>] "
                     "[--runtime-profile] [--full-evaluation] [--changed-trace] "
                     "[--branchy-mux] [--resize-elision] [--init-zero-elision] "
                     "[--source-part-activity-guard] [--source-word-activity-guard] "
                     "[--wide-storage-first-touch] [--no-trace-comments] "
                     "[--am-optimize=<dce,fold,cse,alias,statealias,unify,muxabsorb,notunify,slicefuse,memfold,ifacealias>] [--no-am-optimize]\n";
        return 2;
    }
    const std::string path = argv[1];
    bool runSchedule = false;
    std::string explicitTop;
    std::filesystem::path emitDirectory;
    std::optional<std::string> blocksPerSource;
    std::optional<std::string> maxSourceBytes;
    std::optional<std::string> maxCommitSourceBytes;
    std::optional<std::string> blockChunkInstructions;
    // Default partition configuration = the gsim-aligned point locked by
    // supernode-align NO0018 (2026-08-12): on the flatten graph the mapping
    // to gsim's supernodes is measured by pair-F1 against the members join —
    // 15 atoms/block + 7000-atom coarsen budget + mergeWhen off lands
    // pair-F1 0.9255 (was 0.4260 at the NO0002 point); state-anchor sweeps
    // were evaluated and rejected by F1 (mode 1: 0.8935, mode 2: 0.8268).
    // Fusion anchors are computed block-locally after the DP, so disabling
    // mergeWhen no longer costs emitter same-select fusion.
    std::size_t maxAtomsPerBlock = 15;
    std::size_t maxCommitAtomsPerBlock = 4096;
    double dpSegmentPenalty = 0.0;
    std::size_t dpCoarsenAtomBudget = 7000;
    std::size_t dpCoarsenInstrBudget = 0;
    std::size_t treeAtomFoldMaxInstr = 2;
    std::size_t mergeWhenMinGroup = 1;
    std::size_t stateAnchorMode = 0;
    std::size_t dpRefinementRounds = 0;
    std::size_t fanoutAbsorbMaxInstructions = 0;
    double fanoutAbsorbBudgetMult = 1.0;
    std::size_t fanoutAbsorbMaxConsumers = 256;
    bool enableCoarsening = true;
    bool runtimeProfile = false;
    bool fullEvaluation = false;
    bool changedTrace = false;
    bool branchyMux = false;
    bool resizeElision = false;
    bool initZeroElision = false;
    bool sourcePartActivityGuard = false;
    bool sourceWordActivityGuard = false;
    bool wideStorageFirstTouch = false;
    bool traceComments = true;
    AmOptimizeOptions amOptimize;
    for (int index = 2; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--schedule")
        {
            runSchedule = true;
        }
        else if (argument == "--emit" && index + 1 < argc)
        {
            emitDirectory = argv[++index];
            runSchedule = true;
        }
        else if (argument == "--blocks-per-source" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --blocks-per-source value: " << text << '\n';
                return 2;
            }
            blocksPerSource = std::to_string(value);
        }
        else if (argument == "--max-source-bytes" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            uint64_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --max-source-bytes value: " << text << '\n';
                return 2;
            }
            maxSourceBytes = std::to_string(value);
        }
        else if (argument == "--max-commit-source-bytes" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            uint64_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --max-commit-source-bytes value: " << text << '\n';
                return 2;
            }
            maxCommitSourceBytes = std::to_string(value);
        }
        else if (argument == "--block-chunk-instructions" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            uint64_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --block-chunk-instructions value: " << text << '\n';
                return 2;
            }
            blockChunkInstructions = std::to_string(value);
        }
        else if (argument == "--max-atoms-per-block" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --max-atoms-per-block value: " << text << '\n';
                return 2;
            }
            maxAtomsPerBlock = value;
        }
        else if (argument == "--max-commit-atoms-per-block" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --max-commit-atoms-per-block value: " << text << '\n';
                return 2;
            }
            maxCommitAtomsPerBlock = value;
        }
        else if (argument == "--dp-segment-penalty" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            try
            {
                dpSegmentPenalty = std::stod(std::string(text));
            }
            catch (const std::exception &)
            {
                std::cerr << "invalid --dp-segment-penalty value: " << text << '\n';
                return 2;
            }
        }
        else if (argument == "--dp-coarsen-atom-budget" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --dp-coarsen-atom-budget value: " << text << '\n';
                return 2;
            }
            dpCoarsenAtomBudget = value;
        }
        else if (argument == "--dp-coarsen-instr-budget" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --dp-coarsen-instr-budget value: " << text << '\n';
                return 2;
            }
            dpCoarsenInstrBudget = value;
        }
        else if (argument == "--tree-atom-fold-max-instr" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --tree-atom-fold-max-instr value: " << text << '\n';
                return 2;
            }
            treeAtomFoldMaxInstr = value;
        }
        else if (argument == "--merge-when-min-group" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --merge-when-min-group value: " << text << '\n';
                return 2;
            }
            mergeWhenMinGroup = value;
        }
        else if (argument == "--dp-refinement-rounds" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --dp-refinement-rounds value: " << text << '\n';
                return 2;
            }
            dpRefinementRounds = value;
        }
        else if (argument == "--dp-state-anchor-mode" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value > 2)
            {
                std::cerr << "invalid --dp-state-anchor-mode value: " << text << '\n';
                return 2;
            }
            stateAnchorMode = value;
        }
        else if (argument == "--fanout-absorb-max-instructions" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --fanout-absorb-max-instructions value: " << text << '\n';
                return 2;
            }
            fanoutAbsorbMaxInstructions = value;
        }
        else if (argument == "--fanout-absorb-budget-mult" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            try
            {
                fanoutAbsorbBudgetMult = std::stod(std::string(text));
            }
            catch (const std::exception &)
            {
                std::cerr << "invalid --fanout-absorb-budget-mult value: " << text << '\n';
                return 2;
            }
        }
        else if (argument == "--fanout-absorb-max-consumers" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --fanout-absorb-max-consumers value: " << text << '\n';
                return 2;
            }
            fanoutAbsorbMaxConsumers = value;
        }
        else if (argument == "--disable-coarsening")
        {
            enableCoarsening = false;
        }
        else if (argument == "--runtime-profile")
        {
            runtimeProfile = true;
        }
        else if (argument == "--full-evaluation")
        {
            fullEvaluation = true;
        }
        else if (argument == "--changed-trace")
        {
            changedTrace = true;
        }
        else if (argument == "--branchy-mux")
        {
            branchyMux = true;
        }
        else if (argument == "--resize-elision")
        {
            resizeElision = true;
        }
        else if (argument == "--init-zero-elision")
        {
            initZeroElision = true;
        }
        else if (argument == "--source-part-activity-guard")
        {
            sourcePartActivityGuard = true;
        }
        else if (argument == "--source-word-activity-guard")
        {
            sourceWordActivityGuard = true;
        }
        else if (argument == "--wide-storage-first-touch")
        {
            wideStorageFirstTouch = true;
        }
        else if (argument == "--no-trace-comments")
        {
            traceComments = false;
        }
        else if (argument == "--no-am-optimize")
        {
            amOptimize = AmOptimizeOptions{};
            amOptimize.dce = false;
            amOptimize.constFold = false;
            amOptimize.cse = false;
            amOptimize.assignAlias = false;
            amOptimize.stateReadAlias = false;
            amOptimize.logicUnify = false;
            amOptimize.muxNotAbsorb = false;
            amOptimize.notUnify = false;
            amOptimize.sliceFuse = false;
            amOptimize.insertFuse = false;
            amOptimize.constMemFold = false;
            amOptimize.interfaceAlias = false;
        }
        else if (argument.starts_with("--am-optimize="))
        {
            const std::string_view list =
                argument.substr(std::string_view("--am-optimize=").size());
            AmOptimizeOptions parsed{};
            parsed.dce = false;
            parsed.constFold = false;
            parsed.cse = false;
            parsed.assignAlias = false;
            parsed.stateReadAlias = false;
            parsed.logicUnify = false;
            parsed.muxNotAbsorb = false;
            parsed.notUnify = false;
            parsed.sliceFuse = false;
            parsed.insertFuse = false;
            parsed.constMemFold = false;
            parsed.interfaceAlias = false;
            bool valid = true;
            std::size_t begin = 0;
            while (valid && begin <= list.size())
            {
                const std::size_t comma = list.find(',', begin);
                const std::string_view token =
                    comma == std::string_view::npos
                        ? list.substr(begin)
                        : list.substr(begin, comma - begin);
                if (token == "dce")
                {
                    parsed.dce = true;
                }
                else if (token == "fold")
                {
                    parsed.constFold = true;
                }
                else if (token == "cse")
                {
                    parsed.cse = true;
                }
                else if (token == "alias")
                {
                    parsed.assignAlias = true;
                }
                else if (token == "statealias")
                {
                    parsed.stateReadAlias = true;
                }
                else if (token == "unify")
                {
                    parsed.logicUnify = true;
                }
                else if (token == "muxabsorb")
                {
                    parsed.muxNotAbsorb = true;
                }
                else if (token == "notunify")
                {
                    parsed.notUnify = true;
                }
                else if (token == "slicefuse")
                {
                    parsed.sliceFuse = true;
                }
                else if (token == "insertfuse")
                {
                    parsed.insertFuse = true;
                }
                else if (token == "memfold")
                {
                    parsed.constMemFold = true;
                }
                else if (token == "ifacealias")
                {
                    parsed.interfaceAlias = true;
                }
                else if (!token.empty())
                {
                    valid = false;
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                begin = comma + 1;
            }
            if (!valid)
            {
                std::cerr << "invalid --am-optimize value: " << list << '\n';
                return 2;
            }
            amOptimize = parsed;
        }
        else if (!argument.starts_with("--") && explicitTop.empty())
        {
            explicitTop = argument;
        }
        else
        {
            std::cerr << "invalid argument: " << argument << '\n';
            return 2;
        }
    }
    if (blocksPerSource && emitDirectory.empty())
    {
        std::cerr << "--blocks-per-source requires --emit\n";
        return 2;
    }
    if (maxSourceBytes && emitDirectory.empty())
    {
        std::cerr << "--max-source-bytes requires --emit\n";
        return 2;
    }
    try
    {
        auto phaseStart = std::chrono::steady_clock::now();
        std::string json;
        if (!readFile(path, json))
        {
            std::cerr << "failed to read " << path << '\n';
            return 2;
        }
        const uint64_t readMs = elapsedMilliseconds(phaseStart);
        std::cerr << "[grhsim-am-lower-json] read complete ms=" << readMs
                  << " peak_rss_kib=" << peakRssKiB() << '\n';

        phaseStart = std::chrono::steady_clock::now();
        grh::Design design = grh::Design::fromJsonString(json);
        std::string().swap(json);
        const uint64_t parseMs = elapsedMilliseconds(phaseStart);
        std::cerr << "[grhsim-am-lower-json] parse complete ms=" << parseMs
                  << " peak_rss_kib=" << peakRssKiB() << '\n';
        std::string top = explicitTop;
        if (top.empty())
        {
            if (design.topGraphs().size() != 1)
            {
                std::cerr << "design does not contain exactly one top graph\n";
                return 2;
            }
            top = design.topGraphs().front();
        }
        grh::Graph *graph = design.findGraph(top);
        if (!graph)
        {
            std::cerr << "top graph not found: " << top << '\n';
            return 2;
        }
        const std::size_t graphOperations = graph->operations().size();
        const std::size_t graphValues = graph->values().size();

        phaseStart = std::chrono::steady_clock::now();
        diag::Diagnostics diagnostics;
        std::size_t diagnosticsCursor = 0;
        GrhIRToGrhSimAMGraphLowering lowering(GrhIRToGrhSimAMGraphLoweringOptions{
            .unknownLogic = UnknownLogicPolicy::FlattenToZero,
        });
        std::optional<AmGraph> amGraph = lowering.lower(*graph, diagnostics);
        const uint64_t lowerMs = elapsedMilliseconds(phaseStart);
        std::cerr << "[grhsim-am-lower-json] lower complete ms=" << lowerMs
                  << " peak_rss_kib=" << peakRssKiB() << '\n';
        if (!amGraph || diagnostics.hasError())
        {
            printDiagnostics(diagnostics);
            std::cerr << "lowering failed after " << lowerMs << " ms"
                      << " peak_rss_kib=" << peakRssKiB() << '\n';
            return 1;
        }
        printNewDiagnostics(diagnostics, diagnosticsCursor);
        // NO0004 import QC point: pre-optimize instruction opcode census,
        // same shape as the schedule-time opcode_mix so the three stages
        // (import -> optimize -> schedule) can be compared directly.
        {
            const ProgramView importView = amGraph->program();
            const std::size_t importInstructions = importView.instructionCount();
            std::map<std::string, uint64_t> importOpcodeCounts;
            for (uint32_t instr = 0; instr < importInstructions; ++instr)
            {
                importOpcodeCounts[std::string(toString(
                    importView.opcode(InstructionId{instr})))] += 1;
            }
            std::string importMix;
            for (const auto &[name, count] : importOpcodeCounts)
            {
                importMix += " " + name + "=" + std::to_string(count);
            }
            std::cerr << "am.import: instructions=" << importInstructions
                      << " opcode_mix[" << importMix << " ] [grhsim.am.import]\n";
        }
        // NO0006: gsim node-aligned scheduling skips the AM optimize stage
        // (the gsim flatten graph is pre-optimized; the passes would erase
        // the node anchors). WOLVRIX_GRHSIM_AM_NODE_ALIGNED_OPTIMIZE=1
        // force-runs it for A/B.
        const ActivityScheduleOptions scheduleOptions{
            .maxAtomsPerBlock = maxAtomsPerBlock,
            .maxCommitAtomsPerBlock = maxCommitAtomsPerBlock,
            .enableCoarsening = enableCoarsening,
            .collectStats = true,
            .dpSegmentPenalty = dpSegmentPenalty,
            .dpCoarsenAtomBudget = dpCoarsenAtomBudget,
            .dpCoarsenInstrBudget = dpCoarsenInstrBudget,
            .dpRefinementRounds = dpRefinementRounds,
            .mergeWhenMinGroup = mergeWhenMinGroup,
            .stateAnchorMode = stateAnchorMode,
            .fanoutAbsorbMaxInstructions = fanoutAbsorbMaxInstructions,
            .fanoutAbsorbBudgetMult = fanoutAbsorbBudgetMult,
            .fanoutAbsorbMaxConsumers = fanoutAbsorbMaxConsumers,
            .treeAtomFoldMaxInstr = treeAtomFoldMaxInstr,
        };
        const bool nodeAligned = gsimNodeAlignedScheduling(*amGraph, scheduleOptions);
        const bool skipAmOptimize = nodeAligned && !gsimNodeAlignedOptimizeForced();
        if (skipAmOptimize)
        {
            std::cerr << "[grhsim-am-lower-json] optimize skipped (gsim node-aligned)\n";
        }
        if (!skipAmOptimize &&
            (amOptimize.dce || amOptimize.constFold || amOptimize.cse ||
             amOptimize.assignAlias || amOptimize.stateReadAlias ||
             amOptimize.logicUnify || amOptimize.muxNotAbsorb ||
             amOptimize.notUnify || amOptimize.sliceFuse ||
             amOptimize.insertFuse || amOptimize.constMemFold))
        {
            phaseStart = std::chrono::steady_clock::now();
            const bool optimized =
                optimizeAmGraph(*amGraph, amOptimize, diagnostics);
            const uint64_t optimizeMs = elapsedMilliseconds(phaseStart);
            std::cerr << "[grhsim-am-lower-json] optimize complete ms="
                      << optimizeMs << " peak_rss_kib=" << peakRssKiB() << '\n';
            if (!optimized || diagnostics.hasError())
            {
                printDiagnostics(diagnostics);
                std::cerr << "optimize failed after " << optimizeMs << " ms"
                          << " peak_rss_kib=" << peakRssKiB() << '\n';
                return 1;
            }
            printNewDiagnostics(diagnostics, diagnosticsCursor);
        }
        const ProgramStorageStats stats = amGraph->program().storageStats();
        const std::size_t interfacePorts = amGraph->interface().ports.size();
        const std::size_t declaredVariables =
            amGraph->interface().declaredVariables.size();
        const std::size_t orderedEffects = amGraph->orderedEffects().size();
        design = grh::Design{};
        std::cerr << "[grhsim-am-lower-json] graph released current_rss_kib="
                  << currentRssKiB() << '\n';

        std::optional<ExecutableModel> model;
        uint64_t scheduleMs = 0;
        ProgramStorageStats scheduledStats;
        uint64_t blockCount = 0;
        uint64_t activationTargets = 0;
        uint64_t commitBlocks = 0;
        uint64_t emitMs = 0;
        uint64_t emittedArtifacts = 0;
        uint64_t muxAtomFused = 0;
        uint64_t windowedChains = 0;
        uint64_t windowedSteps = 0;
        uint64_t windowedConcatsF2 = 0;
        uint64_t windowedSkippedSlices = 0;
        uint64_t windowedRemappedSlices = 0;
        uint64_t windowedMaterialized = 0;
        uint64_t windowedBailedChains = 0;
        uint64_t dynBlendChains = 0;
        uint64_t dynBlendCones = 0;
        uint64_t dynBlendSkipped = 0;
        uint64_t dynBlendRemapped = 0;
        uint64_t dynBlendMaterialized = 0;
        uint64_t dynBlendBailed = 0;
        uint64_t initZeroElisionNarrow = 0;
        uint64_t initZeroElisionWide = 0;
        uint64_t initZeroElisionReal = 0;
        uint64_t wideStorageVariables = 0;
        uint64_t wideStorageTouchedVariables = 0;
        uint64_t wideStorageIdBlockFirstLines = 0;
        uint64_t wideStorageFirstTouchBlockFirstLines = 0;
        uint64_t wideStorageTouchedWords = 0;
        uint64_t wideStorageIdTouchedSpanWords = 0;
        uint64_t wideStorageFirstTouchTouchedSpanWords = 0;
        uint64_t wideStorageIdTouchedPages = 0;
        uint64_t wideStorageFirstTouchTouchedPages = 0;
        if (runSchedule)
        {
            phaseStart = std::chrono::steady_clock::now();
            model = GrhIRToGrhSimAMProgram::graphToProgram(
                std::move(*amGraph), scheduleOptions, diagnostics);
            scheduleMs = elapsedMilliseconds(phaseStart);
            std::cerr << "[grhsim-am-lower-json] schedule complete ms="
                      << scheduleMs << " peak_rss_kib=" << peakRssKiB()
                      << " current_rss_kib=" << currentRssKiB() << '\n';
            if (!model || diagnostics.hasError())
            {
                printDiagnostics(diagnostics);
                return 1;
            }
            printNewDiagnostics(diagnostics, diagnosticsCursor);
            scheduledStats = model->program.view().storageStats();
            blockCount = model->program.blockCount();
            activationTargets = scheduledStats
                                    .arena(ProgramArena::ActivationTargets)
                                    .elements;
            commitBlocks = model->commitBlockBegin == 0
                               ? 0
                               : model->commitBlockEnd - model->commitBlockBegin;
            // Block/atom membership export (NO0006 trace): same env-gated
            // strictness as the split-stage audit dumps — set to a path to
            // write, empty string is an error.
            if (const char *blockAtomPath =
                    std::getenv("WOLVRIX_GRHSIM_AM_BLOCK_ATOM_JSONL"))
            {
                if (blockAtomPath[0] == '\0' ||
                    !exportGrhSimAmBlockAtomJsonl(
                        *model, std::filesystem::path(blockAtomPath), diagnostics))
                {
                    diagnostics.error("AM block/atom export failed",
                                      "grhsim-am-lower-json");
                    printDiagnostics(diagnostics);
                    return 1;
                }
                printNewDiagnostics(diagnostics, diagnosticsCursor);
            }
            if (!emitDirectory.empty())
            {
                phaseStart = std::chrono::steady_clock::now();
                GrhSimAmCppEmitter emitter;
                GrhSimAmCppOptions emitOptions{
                    .outputDirectory = emitDirectory,
                    .modelName = top,
                    .attributes = {{"collectStats", "true"}},
                };
                emitOptions.traceComments = traceComments;
                if (blocksPerSource)
                {
                    emitOptions.attributes.emplace("blocksPerSource", *blocksPerSource);
                }
                if (maxSourceBytes)
                {
                    emitOptions.attributes.emplace("maxSourceBytes", *maxSourceBytes);
                }
                if (maxCommitSourceBytes)
                {
                    emitOptions.attributes.emplace("maxCommitSourceBytes",
                                                   *maxCommitSourceBytes);
                }
                if (blockChunkInstructions)
                {
                    emitOptions.attributes.emplace("blockChunkInstructions",
                                                   *blockChunkInstructions);
                }
                if (runtimeProfile)
                {
                    emitOptions.attributes.emplace("runtimeProfile", "true");
                }
                if (fullEvaluation)
                {
                    emitOptions.attributes.emplace("fullEvaluation", "true");
                }
                if (changedTrace)
                {
                    emitOptions.attributes.emplace("changedTrace", "true");
                }
                if (branchyMux)
                {
                    emitOptions.attributes.emplace("branchyMux", "true");
                }
                if (resizeElision)
                {
                    emitOptions.attributes.emplace("resizeElision", "true");
                }
                if (initZeroElision)
                {
                    emitOptions.attributes.emplace("initZeroElision", "true");
                }
                if (sourcePartActivityGuard)
                {
                    emitOptions.attributes.emplace("sourcePartActivityGuard", "true");
                }
                if (sourceWordActivityGuard)
                {
                    emitOptions.attributes.emplace("sourceWordActivityGuard", "true");
                }
                if (wideStorageFirstTouch)
                {
                    emitOptions.attributes.emplace("wideStorageFirstTouch", "true");
                }
                const GrhSimAmCppResult emitResult = emitter.emit(
                    *model,
                    emitOptions,
                    diagnostics);
                emitMs = elapsedMilliseconds(phaseStart);
                std::cerr << "[grhsim-am-lower-json] emit complete ms=" << emitMs
                          << " peak_rss_kib=" << peakRssKiB()
                          << " current_rss_kib=" << currentRssKiB() << '\n';
                if (!emitResult.success || diagnostics.hasError())
                {
                    printDiagnostics(diagnostics);
                    return 1;
                }
                emittedArtifacts = emitResult.artifacts.size();
                muxAtomFused = emitResult.muxAtomFused;
                windowedChains = emitResult.windowedChains;
                windowedSteps = emitResult.windowedSteps;
                windowedConcatsF2 = emitResult.windowedConcatsF2;
                windowedSkippedSlices = emitResult.windowedSkippedSlices;
                windowedRemappedSlices = emitResult.windowedRemappedSlices;
                windowedMaterialized = emitResult.windowedMaterialized;
                windowedBailedChains = emitResult.windowedBailedChains;
                dynBlendChains = emitResult.dynBlendChains;
                dynBlendCones = emitResult.dynBlendCones;
                dynBlendSkipped = emitResult.dynBlendSkipped;
                dynBlendRemapped = emitResult.dynBlendRemapped;
                dynBlendMaterialized = emitResult.dynBlendMaterialized;
                dynBlendBailed = emitResult.dynBlendBailed;
                initZeroElisionNarrow = emitResult.initZeroElisionNarrow;
                initZeroElisionWide = emitResult.initZeroElisionWide;
                initZeroElisionReal = emitResult.initZeroElisionReal;
                wideStorageVariables = emitResult.wideStorageVariables;
                wideStorageTouchedVariables = emitResult.wideStorageTouchedVariables;
                wideStorageIdBlockFirstLines = emitResult.wideStorageIdBlockFirstLines;
                wideStorageFirstTouchBlockFirstLines =
                    emitResult.wideStorageFirstTouchBlockFirstLines;
                wideStorageTouchedWords = emitResult.wideStorageTouchedWords;
                wideStorageIdTouchedSpanWords = emitResult.wideStorageIdTouchedSpanWords;
                wideStorageFirstTouchTouchedSpanWords =
                    emitResult.wideStorageFirstTouchTouchedSpanWords;
                wideStorageIdTouchedPages = emitResult.wideStorageIdTouchedPages;
                wideStorageFirstTouchTouchedPages =
                    emitResult.wideStorageFirstTouchTouchedPages;
                printNewDiagnostics(diagnostics, diagnosticsCursor);
            }
        }
        std::cout << "{\n"
                  << "  \"top\": \"" << top << "\",\n"
                  << "  \"graph_operations\": " << graphOperations << ",\n"
                  << "  \"graph_values\": " << graphValues << ",\n"
                  << "  \"am_instructions\": " << stats.instructions << ",\n"
                  << "  \"am_variables\": " << stats.variables << ",\n"
                  << "  \"am_operands\": " << stats.operands << ",\n"
                  << "  \"am_results\": " << stats.results << ",\n"
                  << "  \"am_estimated_bytes\": " << stats.estimatedBytes << ",\n"
                  << "  \"am_reserved_bytes\": " << stats.reservedBytes << ",\n"
                  << "  \"interface_ports\": " << interfacePorts << ",\n"
                  << "  \"declared_variables\": " << declaredVariables << ",\n"
                  << "  \"ordered_effects\": " << orderedEffects << ",\n"
                  << "  \"scheduled\": " << (runSchedule ? "true" : "false") << ",\n"
                  << "  \"scheduled_instructions\": " << scheduledStats.instructions << ",\n"
                  << "  \"scheduled_variables\": " << scheduledStats.variables << ",\n"
                  << "  \"blocks\": " << blockCount << ",\n"
                  << "  \"activation_targets\": " << activationTargets << ",\n"
                  << "  \"commit_blocks\": " << commitBlocks << ",\n"
                  << "  \"scheduled_estimated_bytes\": "
                  << scheduledStats.estimatedBytes << ",\n"
                  << "  \"read_ms\": " << readMs << ",\n"
                  << "  \"parse_ms\": " << parseMs << ",\n"
                  << "  \"lower_ms\": " << lowerMs << ",\n"
                  << "  \"schedule_ms\": " << scheduleMs << ",\n"
                  << "  \"emit_ms\": " << emitMs << ",\n"
                  << "  \"emitted_artifacts\": " << emittedArtifacts << ",\n"
                  << "  \"mux_atom_fused\": " << muxAtomFused << ",\n"
                  << "  \"windowed_chains\": " << windowedChains << ",\n"
                  << "  \"windowed_steps\": " << windowedSteps << ",\n"
                  << "  \"windowed_concats_f2\": " << windowedConcatsF2 << ",\n"
                  << "  \"windowed_skipped_slices\": " << windowedSkippedSlices << ",\n"
                  << "  \"windowed_remapped_slices\": " << windowedRemappedSlices << ",\n"
                  << "  \"windowed_materialized\": " << windowedMaterialized << ",\n"
                  << "  \"windowed_bailed_chains\": " << windowedBailedChains << ",\n"
                  << "  \"dynblend_chains\": " << dynBlendChains << ",\n"
                  << "  \"dynblend_cones\": " << dynBlendCones << ",\n"
                  << "  \"dynblend_skipped\": " << dynBlendSkipped << ",\n"
                  << "  \"dynblend_remapped\": " << dynBlendRemapped << ",\n"
                  << "  \"dynblend_materialized\": " << dynBlendMaterialized << ",\n"
                  << "  \"dynblend_bailed\": " << dynBlendBailed << ",\n"
                  << "  \"init_zero_elision_narrow\": " << initZeroElisionNarrow << ",\n"
                  << "  \"init_zero_elision_wide\": " << initZeroElisionWide << ",\n"
                  << "  \"init_zero_elision_real\": " << initZeroElisionReal << ",\n"
                  << "  \"wide_storage_variables\": " << wideStorageVariables << ",\n"
                  << "  \"wide_storage_touched_variables\": "
                  << wideStorageTouchedVariables << ",\n"
                  << "  \"wide_storage_id_block_first_lines\": "
                  << wideStorageIdBlockFirstLines << ",\n"
                  << "  \"wide_storage_first_touch_block_first_lines\": "
                  << wideStorageFirstTouchBlockFirstLines << ",\n"
                  << "  \"wide_storage_touched_words\": " << wideStorageTouchedWords << ",\n"
                  << "  \"wide_storage_id_touched_span_words\": "
                  << wideStorageIdTouchedSpanWords << ",\n"
                  << "  \"wide_storage_first_touch_touched_span_words\": "
                  << wideStorageFirstTouchTouchedSpanWords << ",\n"
                  << "  \"wide_storage_id_touched_pages\": "
                  << wideStorageIdTouchedPages << ",\n"
                  << "  \"wide_storage_first_touch_touched_pages\": "
                  << wideStorageFirstTouchTouchedPages << ",\n"
                  << "  \"current_rss_kib\": " << currentRssKiB() << ",\n"
                  << "  \"peak_rss_kib\": " << peakRssKiB() << '\n'
                  << "}\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "lowering smoke failed: " << error.what()
                  << " peak_rss_kib=" << peakRssKiB() << '\n';
        return 1;
    }
}
