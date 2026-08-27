#include "grhsim/ir/pass.hpp"
#include "grhsim/pass/schedule_topo.hpp"

#include <chrono>
#include <utility>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        class AnalyzeValidatePass final : public SimPass
        {
        public:
            AnalyzeValidatePass()
                : SimPass("analyze-validate", "analyze-validate",
                          "Validate GRHSIM IR structure and generic dialect contracts",
                          SimPassEffects{.mutatesGraph = false, .preservesSchedule = true})
            {
            }

            SimPassResult run(Module &module, SimPassContext &context) override
            {
                const bool success = module.validate(context.diagnostics);
                return SimPassResult{.changed = false, .failed = !success};
            }
        };

        std::string normalizeName(std::string_view name)
        {
            std::string result(name);
            for (char &ch : result)
            {
                if (ch == '_')
                {
                    ch = '-';
                }
            }
            return result;
        }
    } // namespace

    SimPass::SimPass(std::string id, std::string name, std::string description,
                     SimPassEffects effects)
        : id_(std::move(id)), name_(std::move(name)),
          description_(std::move(description)), effects_(effects)
    {
    }

    SimPassManager::SimPassManager(SimPassManagerOptions options)
        : options_(std::move(options))
    {
    }

    void SimPassManager::addPass(std::unique_ptr<SimPass> pass)
    {
        passes_.push_back(std::move(pass));
    }

    void SimPassManager::clear()
    {
        passes_.clear();
    }

    SimPipelineResult SimPassManager::run(
        Module &module,
        wolvrix::lib::transform::PassDiagnostics &diagnostics)
    {
        SimPipelineResult result;
        wolvrix::lib::transform::SessionStore localSession;
        SimPassContext context{
            .diagnostics = diagnostics,
            .logLevel = options_.logLevel,
            .logSink = options_.logSink,
            .session = options_.session ? options_.session : &localSession,
        };
        for (const auto &pass : passes_)
        {
            if (options_.stopOnError && diagnostics.hasError())
            {
                result.success = false;
                break;
            }
            if (!pass)
            {
                diagnostics.error("sim-pass-manager", "null SimPass in pipeline", "pipeline");
                result.success = false;
                if (options_.stopOnError)
                {
                    break;
                }
                continue;
            }

            if (!pass->effects().mutatesGraph && !module.frozen())
            {
                module.freeze();
            }

            const auto start = std::chrono::steady_clock::now();
            SimPassResult passResult = pass->run(module, context);
            const auto end = std::chrono::steady_clock::now();
            result.changed = result.changed || passResult.changed;
            result.artifacts.insert(result.artifacts.end(),
                                    passResult.artifacts.begin(), passResult.artifacts.end());

            if (passResult.changed && pass->effects().mutatesGraph)
            {
                if (!pass->effects().preservesSchedule && module.hasSchedule())
                {
                    module.clearSchedule();
                    diagnostics.info(pass->id(),
                                     "cleared Schedule invalidated by SimPass",
                                     "module=" + module.name());
                }
                module.compact();
            }

            if (options_.emitTiming && options_.logSink &&
                static_cast<int>(LogLevel::Info) >= static_cast<int>(options_.logLevel))
            {
                const auto millis =
                    std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                options_.logSink(LogLevel::Info, "sim-pass-timing",
                                 pass->id() + " " + std::to_string(millis) + "ms");
            }
            if (passResult.failed || diagnostics.hasError())
            {
                result.success = false;
                if (options_.stopOnError)
                {
                    break;
                }
            }
        }
        result.success = result.success && !diagnostics.hasError();
        return result;
    }

    std::unique_ptr<SimPass> makeSimPass(
        std::string_view name,
        std::span<const std::string_view> args,
        std::string &error)
    {
        const std::string normalized = normalizeName(name);
        if (normalized == "analyze-validate")
        {
            if (!args.empty())
            {
                error = "analyze-validate does not accept arguments";
                return nullptr;
            }
            return std::make_unique<AnalyzeValidatePass>();
        }
        if (normalized == "schedule-topo")
        {
            if (!args.empty())
            {
                error = "schedule-topo does not accept arguments";
                return nullptr;
            }
            return makeScheduleTopoPass();
        }
        error = "unknown GRHSIM SimPass: " + normalized;
        return nullptr;
    }

    std::vector<std::string> availableSimPasses()
    {
        return {"analyze-validate", "schedule-topo"};
    }

} // namespace wolvrix::lib::grhsim
