#ifndef WOLVRIX_GRHSIM_IR_PASS_HPP
#define WOLVRIX_GRHSIM_IR_PASS_HPP

#include "core/logging.hpp"
#include "core/transform.hpp"
#include "grhsim/ir/module.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grhsim
{

    struct SimPassResult
    {
        bool changed = false;
        bool failed = false;
        std::vector<std::string> artifacts;
    };

    struct SimPassEffects
    {
        bool mutatesGraph = true;
        bool preservesSchedule = true;
    };

    struct SimPassContext
    {
        wolvrix::lib::transform::PassDiagnostics &diagnostics;
        LogLevel logLevel = LogLevel::Warn;
        std::function<void(LogLevel, std::string_view, std::string_view)> logSink;
        wolvrix::lib::transform::SessionStore *session = nullptr;
    };

    class SimPass
    {
    public:
        SimPass(std::string id, std::string name, std::string description,
                SimPassEffects effects = {});
        virtual ~SimPass() = default;

        virtual SimPassResult run(Module &module, SimPassContext &context) = 0;

        const std::string &id() const noexcept { return id_; }
        const std::string &name() const noexcept { return name_; }
        const std::string &description() const noexcept { return description_; }
        SimPassEffects effects() const noexcept { return effects_; }

    private:
        std::string id_;
        std::string name_;
        std::string description_;
        SimPassEffects effects_;
    };

    struct SimPassManagerOptions
    {
        bool stopOnError = true;
        bool emitTiming = false;
        LogLevel logLevel = LogLevel::Warn;
        std::function<void(LogLevel, std::string_view, std::string_view)> logSink;
        wolvrix::lib::transform::SessionStore *session = nullptr;
    };

    struct SimPipelineResult
    {
        bool success = true;
        bool changed = false;
        std::vector<std::string> artifacts;
    };

    class SimPassManager
    {
    public:
        explicit SimPassManager(SimPassManagerOptions options = {});

        void addPass(std::unique_ptr<SimPass> pass);
        void clear();
        SimPipelineResult run(Module &module,
                              wolvrix::lib::transform::PassDiagnostics &diagnostics);

        SimPassManagerOptions &options() noexcept { return options_; }
        const SimPassManagerOptions &options() const noexcept { return options_; }

    private:
        std::vector<std::unique_ptr<SimPass>> passes_;
        SimPassManagerOptions options_;
    };

    std::unique_ptr<SimPass> makeSimPass(
        std::string_view name,
        std::span<const std::string_view> args,
        std::string &error);
    std::vector<std::string> availableSimPasses();

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_PASS_HPP
