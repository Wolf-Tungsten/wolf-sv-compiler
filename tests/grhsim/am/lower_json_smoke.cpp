#include "core/grh.hpp"
#include "grhsim/am/cpp_emitter.hpp"
#include "grhsim/am/lowering.hpp"
#include "grhsim/am/optimize.hpp"
#include "grhsim/am/production_activity_schedule.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
                     "[--max-instructions-per-block <count>] "
                     "[--dp-segment-penalty <value>] "
                     "[--dp-coarsen-budget <count>] [--disable-coarsening] "
                     "[--dp-width-weighted-cost] [--runtime-profile] "
                     "[--am-optimize=<dce,fold,cse,alias,memfold,ifacealias>] [--no-am-optimize]\n";
        return 2;
    }
    const std::string path = argv[1];
    bool runSchedule = false;
    std::string explicitTop;
    std::filesystem::path emitDirectory;
    std::optional<std::string> blocksPerSource;
    std::optional<std::string> maxSourceBytes;
    std::optional<std::string> maxCommitSourceBytes;
    std::size_t maxInstructionsPerBlock = 128;
    double dpSegmentPenalty = 1.0;
    std::size_t dpCoarsenBudget = 0;
    bool enableCoarsening = true;
    bool dpWidthWeightedCopyCost = false;
    bool runtimeProfile = false;
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
        else if (argument == "--max-instructions-per-block" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            {
                std::cerr << "invalid --max-instructions-per-block value: " << text << '\n';
                return 2;
            }
            maxInstructionsPerBlock = value;
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
        else if (argument == "--dp-coarsen-budget" && index + 1 < argc)
        {
            const std::string_view text(argv[++index]);
            std::size_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                std::cerr << "invalid --dp-coarsen-budget value: " << text << '\n';
                return 2;
            }
            dpCoarsenBudget = value;
        }
        else if (argument == "--disable-coarsening")
        {
            enableCoarsening = false;
        }
        else if (argument == "--dp-width-weighted-cost")
        {
            dpWidthWeightedCopyCost = true;
        }
        else if (argument == "--runtime-profile")
        {
            runtimeProfile = true;
        }
        else if (argument == "--no-am-optimize")
        {
            amOptimize = AmOptimizeOptions{
                .dce = false,
                .constFold = false,
                .cse = false,
                .assignAlias = false,
                .constMemFold = false,
                .interfaceAlias = false,
            };
        }
        else if (argument.starts_with("--am-optimize="))
        {
            const std::string_view list =
                argument.substr(std::string_view("--am-optimize=").size());
            AmOptimizeOptions parsed{
                .dce = false,
                .constFold = false,
                .cse = false,
                .assignAlias = false,
                .constMemFold = false,
                .interfaceAlias = false,
            };
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
        GrhToAmLowering lowering(GrhToAmLoweringOptions{
            .unknownLogic = UnknownLogicPolicy::FlattenToZero,
        });
        std::optional<LinearProgramArtifact> artifact =
            lowering.lower(*graph, diagnostics);
        const uint64_t lowerMs = elapsedMilliseconds(phaseStart);
        std::cerr << "[grhsim-am-lower-json] lower complete ms=" << lowerMs
                  << " peak_rss_kib=" << peakRssKiB() << '\n';
        if (!artifact || diagnostics.hasError())
        {
            printDiagnostics(diagnostics);
            std::cerr << "lowering failed after " << lowerMs << " ms"
                      << " peak_rss_kib=" << peakRssKiB() << '\n';
            return 1;
        }
        if (amOptimize.dce || amOptimize.constFold || amOptimize.cse)
        {
            phaseStart = std::chrono::steady_clock::now();
            const bool optimized =
                optimizeLinearProgram(*artifact, amOptimize, diagnostics);
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
        }
        const ProgramStorageStats stats = artifact->program.view().storageStats();
        const std::size_t interfacePorts = artifact->interface.ports.size();
        const std::size_t declaredVariables =
            artifact->interface.declaredVariables.size();
        const std::size_t orderedEffects =
            artifact->schedulingFacts.orderedEffects.size();
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
        if (runSchedule)
        {
            phaseStart = std::chrono::steady_clock::now();
            ProductionActivityScheduleStage scheduler;
            model = scheduler.schedule(
                std::move(*artifact),
                ActivityScheduleOptions{
                    .maxInstructionsPerBlock = maxInstructionsPerBlock,
                    .enableCoarsening = enableCoarsening,
                    .collectStats = true,
                    .dpSegmentPenalty = dpSegmentPenalty,
                    .dpWidthWeightedCopyCost = dpWidthWeightedCopyCost,
                    .dpCoarsenBudget = dpCoarsenBudget,
                },
                diagnostics);
            scheduleMs = elapsedMilliseconds(phaseStart);
            std::cerr << "[grhsim-am-lower-json] schedule complete ms="
                      << scheduleMs << " peak_rss_kib=" << peakRssKiB()
                      << " current_rss_kib=" << currentRssKiB() << '\n';
            if (!model || diagnostics.hasError())
            {
                printDiagnostics(diagnostics);
                return 1;
            }
            printDiagnostics(diagnostics);
            scheduledStats = model->program.view().storageStats();
            blockCount = model->program.blockCount();
            activationTargets = scheduledStats
                                    .arena(ProgramArena::ActivationTargets)
                                    .elements;
            commitBlocks = model->commitBlockBegin == 0
                               ? 0
                               : model->commitBlockEnd - model->commitBlockBegin;
            if (!emitDirectory.empty())
            {
                phaseStart = std::chrono::steady_clock::now();
                GrhSimAmCppEmitter emitter;
                GrhSimAmCppOptions emitOptions{
                    .outputDirectory = emitDirectory,
                    .modelName = top,
                    .attributes = {{"collectStats", "true"}},
                };
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
                if (runtimeProfile)
                {
                    emitOptions.attributes.emplace("runtimeProfile", "true");
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
