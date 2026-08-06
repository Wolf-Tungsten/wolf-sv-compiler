#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_PROGRAM_INTERPRETER_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_PROGRAM_INTERPRETER_HPP

#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grhsim::am {

    class InterpreterValue {
    public:
        InterpreterValue();

        static InterpreterValue zero(const Type &type);
        static InterpreterValue bitVector(uint32_t width, Signedness signedness,
                                          std::span<const uint64_t> words);
        static InterpreterValue real(uint64_t bits);
        static InterpreterValue string(std::string_view bytes);
        static InterpreterValue array(uint32_t elementCount,
                                      uint32_t elementWidth,
                                      Signedness signedness,
                                      std::span<const uint64_t> elementWords);

        const Type &type() const noexcept { return type_; }
        std::span<const uint64_t> words() const noexcept { return words_; }
        std::span<const uint64_t> arrayElementWords(std::size_t index) const;
        std::string_view bytes() const noexcept { return bytes_; }
        uint64_t realBits() const;
        uint64_t lowWord() const noexcept {
            return words_.empty() ? 0 : words_.front();
        }

        friend bool operator==(const InterpreterValue &,
                               const InterpreterValue &) = default;

    private:
        Type type_ = Type::bitVector(1);
        std::vector<uint64_t> words_ = {0};
        std::string bytes_;

        InterpreterValue(const Type &type, std::vector<uint64_t> words,
                         std::string bytes);

        friend class Interpreter;
    };

    class HostEnvironment {
    public:
        virtual ~HostEnvironment() = default;

        virtual bool resolveSystemFunction(ProgramView program,
                                           InstructionId instruction,
                                           std::string &error);
        virtual bool resolveSystemTask(ProgramView program,
                                       InstructionId instruction,
                                       std::string &error);
        virtual bool resolveDpiCall(ProgramView program,
                                    InstructionId instruction,
                                    std::string &error);

        virtual bool
        invokeSystemFunction(ProgramView program, InstructionId instruction,
                             std::span<const InterpreterValue> arguments,
                             InterpreterValue &result, std::string &error);
        virtual bool
        invokeSystemTask(ProgramView program, InstructionId instruction,
                         std::span<const InterpreterValue> arguments,
                         std::string &error);
        virtual bool invokeDpiCall(ProgramView program,
                                   InstructionId instruction,
                                   std::span<const InterpreterValue> arguments,
                                   std::vector<InterpreterValue> &results,
                                   std::string &error);

        virtual bool readInitFile(std::string_view path, std::string &contents,
                                  std::string &error);
    };

    enum class InterpreterErrorCode : uint8_t {
        InvalidModel = 0,
        InitializationFailed,
        InvalidAccess,
        UnsupportedOpcode,
        MissingHostBinding,
        HostError,
        NonConvergent,
        InvalidLifecycle,
    };

    struct InterpreterDiagnostic {
        InterpreterErrorCode code = InterpreterErrorCode::InvalidModel;
        std::string message;
        BlockId block;
        InstructionId instruction;
        VariableId variable;
    };

    struct InterpreterResult {
        std::optional<InterpreterDiagnostic> diagnostic;
        uint64_t roundsExecuted = 0;

        bool success() const noexcept { return !diagnostic.has_value(); }
    };

    struct InterpreterOptions {
        uint64_t maxRounds = 1000000;
        uint64_t randomSeed = 0;
        ValidationLevel validationLevel = ValidationLevel::Semantic;
    };

    class Interpreter {
    public:
        explicit Interpreter(const ExecutableModel &model,
                             HostEnvironment *host = nullptr,
                             const InterpreterOptions &options = {});
        ~Interpreter();
        Interpreter(Interpreter &&) noexcept;
        Interpreter &operator=(Interpreter &&) noexcept;
        Interpreter(const Interpreter &) = delete;
        Interpreter &operator=(const Interpreter &) = delete;

        bool ready() const noexcept;
        const std::optional<InterpreterDiagnostic> &
        initializationDiagnostic() const noexcept;

        InterpreterResult eval();
        InterpreterResult finalize();
        InterpreterResult write(VariableId variable,
                                const InterpreterValue &value);

        const InterpreterValue &value(VariableId variable) const;
        std::span<const InterpreterValue> values() const noexcept;

        bool firstEval() const noexcept;
        bool finalized() const noexcept;
        uint64_t roundCounter() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_PROGRAM_INTERPRETER_HPP
