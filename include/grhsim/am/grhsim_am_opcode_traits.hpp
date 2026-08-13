#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_OPCODE_TRAITS_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_OPCODE_TRAITS_HPP

#include "grhsim/am/grhsim_am_program.hpp"

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
        case Opcode::RegisterWriteCond:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 2,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::RegisterWriteMask:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 2,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::RegisterWriteCondMask:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 3,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::RegisterWriteDynLane:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 3,
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
                .stateTargetOperand = 2,
                .memoryAccess = true,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::MemoryWriteCond:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 3,
                .memoryAccess = true,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::MemoryWriteMask:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 3,
                .memoryAccess = true,
                .hasOrderedEffect = true,
                .variadicOperands = true,
            };
        case Opcode::MemoryWriteCondMask:
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
        case Opcode::LatchWriteCond:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 2,
                .hasOrderedEffect = true,
            };
        case Opcode::LatchWriteMask:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 2,
                .hasOrderedEffect = true,
            };
        case Opcode::LatchWriteCondMask:
            return OpcodeTraits{
                .effect = OpcodeEffect::StateReadWrite,
                .stateTargetOperand = 3,
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

    // Fixed-operand layout of one state-write instruction variant. The
    // operand order is always [cond?, addr?(memory only), mask?, data,
    // target, events...]: cond sits first when present, then the memory
    // address, then the write mask, then the data, then the state target;
    // event operands trail the fixed ones. Latch variants carry no events.
    struct StateWriteLayout
    {
        bool isStateWrite = false;
        bool memory = false;
        bool hasCond = false;
        bool hasMask = false;
        uint8_t fixedCount = 0;
        uint8_t dataIndex = 0;
        uint8_t targetIndex = 0;
    };

    constexpr StateWriteLayout stateWriteLayout(Opcode opcode) noexcept
    {
        const auto make = [](bool memory, bool cond, bool mask) {
            const uint8_t leading = static_cast<uint8_t>((cond ? 1 : 0) +
                                                         (memory ? 1 : 0) + (mask ? 1 : 0));
            return StateWriteLayout{
                .isStateWrite = true,
                .memory = memory,
                .hasCond = cond,
                .hasMask = mask,
                .fixedCount = static_cast<uint8_t>(leading + 2),
                .dataIndex = leading,
                .targetIndex = static_cast<uint8_t>(leading + 1),
            };
        };
        switch (opcode)
        {
        case Opcode::RegisterWrite:
        case Opcode::LatchWrite:
            return make(false, false, false);
        case Opcode::RegisterWriteCond:
        case Opcode::LatchWriteCond:
            return make(false, true, false);
        case Opcode::RegisterWriteMask:
        case Opcode::LatchWriteMask:
            return make(false, false, true);
        case Opcode::RegisterWriteCondMask:
        case Opcode::LatchWriteCondMask:
            return make(false, true, true);
        case Opcode::RegisterWriteDynLane:
            // Operand layout [cond, bitOffset, data, target, events...]: the
            // mask slot carries the runtime bit offset; the generic blend
            // paths must never see this opcode (emitter and interpreter
            // intercept it first).
            return make(false, true, true);
        case Opcode::MemoryWrite:
            return make(true, false, false);
        case Opcode::MemoryWriteCond:
            return make(true, true, false);
        case Opcode::MemoryWriteMask:
            return make(true, false, true);
        case Opcode::MemoryWriteCondMask:
            return make(true, true, true);
        default:
            return {};
        }
    }

    constexpr bool isRegisterWriteOpcode(Opcode opcode) noexcept
    {
        return opcode == Opcode::RegisterWrite || opcode == Opcode::RegisterWriteCond ||
               opcode == Opcode::RegisterWriteMask ||
               opcode == Opcode::RegisterWriteCondMask ||
               opcode == Opcode::RegisterWriteDynLane;
    }

    constexpr bool isLatchWriteOpcode(Opcode opcode) noexcept
    {
        return opcode == Opcode::LatchWrite || opcode == Opcode::LatchWriteCond ||
               opcode == Opcode::LatchWriteMask || opcode == Opcode::LatchWriteCondMask;
    }

    constexpr bool isMemoryWriteOpcode(Opcode opcode) noexcept
    {
        return opcode == Opcode::MemoryWrite || opcode == Opcode::MemoryWriteCond ||
               opcode == Opcode::MemoryWriteMask || opcode == Opcode::MemoryWriteCondMask;
    }

    // Any instruction that commits a state target: the 12 reg/latch/mem
    // write variants plus the whole-array MemoryFill and MemoryWriteLanes.
    constexpr bool isStateWriteOpcode(Opcode opcode) noexcept
    {
        return stateWriteLayout(opcode).isStateWrite || opcode == Opcode::MemoryFill ||
               opcode == Opcode::MemoryWriteLanes;
    }

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_OPCODE_TRAITS_HPP
