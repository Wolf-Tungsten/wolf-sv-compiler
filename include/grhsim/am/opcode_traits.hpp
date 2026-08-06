#ifndef WOLVRIX_GRHSIM_AM_OPCODE_TRAITS_HPP
#define WOLVRIX_GRHSIM_AM_OPCODE_TRAITS_HPP

#include "grhsim/am/program.hpp"

#include <cstdint>

namespace wolvrix::lib::grhsim::am
{

    enum class OpcodeEffect : uint8_t
    {
        Pure = 0,
        ChangeDetector = 1,
        StateRead = 2,
        StateReadWrite = 3,
        HostRead = 4,
        HostEffect = 5,
        Activation = 6,
    };

    struct OpcodeTraits
    {
        static constexpr uint8_t kNoTargetOperand = UINT8_MAX;

        OpcodeEffect effect = OpcodeEffect::Pure;
        uint8_t stateTargetOperand = kNoTargetOperand;
        bool memoryAccess = false;
        bool hasOrderedEffect = false;
        bool variadicOperands = false;
        bool variadicResults = false;
    };

    constexpr OpcodeTraits opcodeTraits(Opcode opcode) noexcept
    {
        switch (opcode)
        {
        case Opcode::Concat:
            return OpcodeTraits{.variadicOperands = true};
        case Opcode::ChangedAny:
        case Opcode::ChangedPos:
        case Opcode::ChangedNeg:
            return OpcodeTraits{
                .effect = OpcodeEffect::ChangeDetector,
                .stateTargetOperand = 1,
            };
        case Opcode::RegisterWrite:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 1,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::MemoryRead:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateRead,
                .stateTargetOperand = 0,
                .memoryAccess = true,
            };
        case Opcode::MemoryWrite:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 4,
                .memoryAccess = true,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::MemoryFill:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 1,
                .memoryAccess = true,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::LatchWrite:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 1,
                .hasOrderedEffect = true,
            };
        case Opcode::SystemFunction:
            return OpcodeTraits{
                .effect = OpcodeEffect::HostRead,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::SystemTask:
            return OpcodeTraits{
                .effect = OpcodeEffect::HostEffect,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::DpiCall:
            return OpcodeTraits{
                .effect = OpcodeEffect::HostEffect,
                .hasOrderedEffect = true,
                .variadicOperands = true,
                .variadicResults = true,
            };
        case Opcode::ActForward:
        case Opcode::ActBackward:
            return OpcodeTraits{.effect = OpcodeEffect::Activation};
        case Opcode::MemoryReadAll:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateRead,
                .stateTargetOperand = 0,
                .memoryAccess = true,
            };
        case Opcode::MemoryWriteLanes:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 2,
                .memoryAccess = true,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        default:
            return {};
        }
    }

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_OPCODE_TRAITS_HPP
