#ifndef WOLVRIX_GRHSIM_AM_PROGRAM_INTERNAL_HPP
#define WOLVRIX_GRHSIM_AM_PROGRAM_INTERNAL_HPP

#include "grhsim/am/program.hpp"

#include <vector>

namespace wolvrix::lib::grhsim::am::detail
{

    struct LiteralRecord
    {
        TypeId type;
        Range32 words;
        Range32 bytes;
    };

    struct SliceStaticAttributeRecord
    {
        InstructionId instruction;
        SliceStaticAttributes attributes;
    };

    struct SystemFunctionAttributeRecord
    {
        InstructionId instruction;
        SystemFunctionAttributes attributes;
    };

    struct SystemTaskAttributeRecord
    {
        InstructionId instruction;
        SystemTaskAttributes attributes;
    };

    struct DpiCallAttributeRecord
    {
        InstructionId instruction;
        DpiCallAttributes attributes;
    };

    struct ActivationAttributeRecord
    {
        InstructionId instruction;
        Range32 targets;
    };

    struct DpiImportRecord
    {
        StringId symbol;
        Range32 parameters;
        DpiReturn returnValue;
    };

    struct ProgramStorage
    {
        ProgramStorage();

        std::vector<Type> types;

        std::vector<uint32_t> stringOffsets;
        std::vector<char> stringBytes;

        std::vector<InitDescriptor> initDescriptors;
        std::vector<InitAction> initActions;
        std::vector<LiteralRecord> literals;
        std::vector<uint64_t> literalWords;
        std::vector<char> literalBytes;

        std::vector<VariableRecord> variables;
        std::vector<VariableLabel> variableLabels;

        std::vector<Opcode> opcodes;
        std::vector<uint32_t> operandOffsets;
        std::vector<VariableId> operands;
        std::vector<uint32_t> resultOffsets;
        std::vector<VariableId> results;

        std::vector<SliceStaticAttributeRecord> sliceStaticAttributes;
        std::vector<SystemFunctionAttributeRecord> systemFunctionAttributes;
        std::vector<SystemTaskAttributeRecord> systemTaskAttributes;
        std::vector<DpiCallAttributeRecord> dpiCallAttributes;
        std::vector<ActivationAttributeRecord> activationAttributes;
        std::vector<BlockId> activationTargets;

        std::vector<DpiImportRecord> dpiImports;
        std::vector<DpiParameter> dpiParameters;

        std::vector<uint32_t> blockOffsets;
        std::vector<InstructionId> blockInstructions;
    };

} // namespace wolvrix::lib::grhsim::am::detail

#endif // WOLVRIX_GRHSIM_AM_PROGRAM_INTERNAL_HPP
