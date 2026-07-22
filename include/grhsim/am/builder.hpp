#ifndef WOLVRIX_GRHSIM_AM_BUILDER_HPP
#define WOLVRIX_GRHSIM_AM_BUILDER_HPP

#include "grhsim/am/program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace wolvrix::lib::grhsim::am
{

    struct ProgramReserve
    {
        std::size_t types = 0;
        std::size_t strings = 0;
        std::size_t stringBytes = 0;
        std::size_t initDescriptors = 0;
        std::size_t initActions = 0;
        std::size_t literals = 0;
        std::size_t literalWords = 0;
        std::size_t literalBytes = 0;
        std::size_t variables = 0;
        std::size_t variableLabels = 0;
        std::size_t instructions = 0;
        std::size_t operands = 0;
        std::size_t results = 0;
        std::size_t sliceStaticAttributes = 0;
        std::size_t systemFunctionAttributes = 0;
        std::size_t systemTaskAttributes = 0;
        std::size_t dpiCallAttributes = 0;
        std::size_t dpiImports = 0;
        std::size_t dpiParameters = 0;
    };

    struct ScheduledProgramReserve
    {
        // Every count is additional to the builder's current scheduled storage/layout.
        std::size_t additionalTypes = 0;
        std::size_t additionalStrings = 0;
        std::size_t additionalStringBytes = 0;
        std::size_t additionalVariables = 0;
        std::size_t additionalVariableLabels = 0;
        std::size_t additionalInstructions = 0;
        std::size_t additionalOperands = 0;
        std::size_t additionalResults = 0;
        std::size_t additionalSliceStaticAttributes = 0;
        std::size_t additionalSystemFunctionAttributes = 0;
        std::size_t additionalSystemTaskAttributes = 0;
        std::size_t additionalDpiCallAttributes = 0;
        std::size_t blocks = 0;
        std::size_t blockInstructionIds = 0;
        std::size_t activationInstructions = 0;
        std::size_t activationTargets = 0;
    };

    class LinearProgramBuilder
    {
    public:
        LinearProgramBuilder();
        ~LinearProgramBuilder();
        LinearProgramBuilder(LinearProgramBuilder &&) noexcept;
        LinearProgramBuilder &operator=(LinearProgramBuilder &&) noexcept;
        LinearProgramBuilder(const LinearProgramBuilder &) = delete;
        LinearProgramBuilder &operator=(const LinearProgramBuilder &) = delete;

        void reserve(const ProgramReserve &reserve);

        TypeId addType(const Type &type);
        StringId addString(std::string_view text);

        InitId undefInit() const noexcept;
        InitId zeroInit() const noexcept;
        LiteralId addBitLiteral(TypeId type, std::span<const uint64_t> words);
        LiteralId addStringLiteral(TypeId type, std::string_view bytes);
        InitId addConstantInit(LiteralId literal);
        InitId addActionsInit(std::span<const InitAction> actions);

        VariableId addVariable(TypeId type,
                               InitId init,
                               std::optional<StringId> label = std::nullopt);
        InstructionId addInstruction(Opcode opcode,
                                     std::span<const VariableId> results,
                                     std::span<const VariableId> operands);

        void setSliceStaticAttributes(InstructionId instruction, uint32_t lsb);
        void setSystemFunctionAttributes(InstructionId instruction,
                                         const SystemFunctionAttributes &attributes);
        void setSystemTaskAttributes(InstructionId instruction,
                                     const SystemTaskAttributes &attributes);
        void setDpiCallAttributes(InstructionId instruction,
                                  const DpiCallAttributes &attributes);

        DpiImportId addDpiImport(StringId symbol,
                                 std::span<const DpiParameter> parameters,
                                 DpiReturn returnValue = {});

        ProgramView view() const noexcept;
        LinearProgram finish();

    private:
        std::unique_ptr<detail::ProgramStorage> storage_;
        bool finished_ = false;
    };

    class ScheduledProgramBuilder
    {
    public:
        explicit ScheduledProgramBuilder(LinearProgram &&linearProgram);
        ~ScheduledProgramBuilder();
        ScheduledProgramBuilder(ScheduledProgramBuilder &&) noexcept;
        ScheduledProgramBuilder &operator=(ScheduledProgramBuilder &&) noexcept;
        ScheduledProgramBuilder(const ScheduledProgramBuilder &) = delete;
        ScheduledProgramBuilder &operator=(const ScheduledProgramBuilder &) = delete;

        void reserve(const ScheduledProgramReserve &reserve);

        TypeId addType(const Type &type);
        StringId addString(std::string_view text);
        InitId undefInit() const noexcept;
        InitId zeroInit() const noexcept;
        VariableId addVariable(TypeId type,
                               InitId init,
                               std::optional<StringId> label = std::nullopt);
        InstructionId addInstruction(Opcode opcode,
                                     std::span<const VariableId> results,
                                     std::span<const VariableId> operands);
        void setSliceStaticAttributes(InstructionId instruction, uint32_t lsb);
        void setSystemFunctionAttributes(InstructionId instruction,
                                         const SystemFunctionAttributes &attributes);
        void setSystemTaskAttributes(InstructionId instruction,
                                     const SystemTaskAttributes &attributes);
        void setDpiCallAttributes(InstructionId instruction,
                                  const DpiCallAttributes &attributes);
        void setActivationTargets(InstructionId instruction,
                                  std::span<const BlockId> targets);

        void beginBlock();
        void appendBlockInstruction(InstructionId instruction);
        void endBlock();
        void addBlock(std::span<const InstructionId> instructions);

        ProgramView view() const noexcept;
        std::size_t pendingBlockCount() const noexcept;
        ScheduledProgram finish();

    private:
        std::unique_ptr<detail::ProgramStorage> storage_;
        bool finished_ = false;
        bool blockOpen_ = false;
        bool blockLayoutIdentity_ = true;
        uint32_t layoutInstructionCount_ = 0;
        std::size_t blockInstructionReserve_ = 0;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_BUILDER_HPP
