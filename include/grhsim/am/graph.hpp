#ifndef WOLVRIX_GRHSIM_AM_GRAPH_HPP
#define WOLVRIX_GRHSIM_AM_GRAPH_HPP

#include "grhsim/am/pipeline.hpp"
#include "grhsim/am/program.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    // ------------------------------------------------------------------
    // AM graph: the mutable graph form between the GRH IR and the linear
    // AM program. Instructions are the ops, variables are the values.
    // Variables carry their declaration semantics (state vs non-state,
    // state kind, init, roles) natively; state reads are explicitly
    // classified so the pre-commit snapshot boundary (the cycle break) is
    // part of the IR instead of an implicit scheduling convention.
    // ------------------------------------------------------------------

    enum class AmValueKind : uint8_t
    {
        Comb = 0,  // defined by an instruction (or a plain net)
        Constant,  // compile-time constant (InitKind::Constant)
        Input,     // external interface input
        State,     // state element (register / latch / memory)
    };

    enum class AmStateKind : uint8_t
    {
        Register = 0,
        Latch,
        Memory,
    };

    // Access mode of an operand edge that references a state variable.
    // PreCommit observes the snapshot taken before the current commit
    // phase (the non-blocking "old value"); Live observes the in-flight
    // value and is legal only inside the owning commit Block's cone.
    enum class AmStateAccess : uint8_t
    {
        PreCommit = 0,
        Live = 1,
    };

    struct AmValueFacts
    {
        AmValueKind kind = AmValueKind::Comb;
        AmStateKind stateKind = AmStateKind::Register;
        VariableRole roles = VariableRole::None;
    };

    class AmGraph
    {
    public:
        using ValueFacts = AmValueFacts;

        AmGraph();
        ~AmGraph();
        AmGraph(AmGraph &&) noexcept;
        AmGraph &operator=(AmGraph &&) noexcept;
        AmGraph(const AmGraph &) = delete;
        AmGraph &operator=(const AmGraph &) = delete;

        // Lossless conversion from the lowered linear artifact. Node and
        // variable ids are preserved one-to-one.
        static AmGraph fromLinearProgram(const LinearProgramArtifact &artifact);

        // Serializes back into a linear artifact. Live (non-removed)
        // instructions are emitted in id order, so a graph that was only
        // produced by fromLinearProgram round-trips byte-identically.
        LinearProgramArtifact toLinearProgram() const;

        ProgramView program() const noexcept;
        const ProgramInterface &interface() const noexcept;
        ProgramInterface &mutableInterface() noexcept;

        std::size_t variableCount() const noexcept;
        std::size_t instructionCount() const noexcept;

        const ValueFacts &valueFacts(VariableId variable) const;
        void setValueFacts(VariableId variable, ValueFacts facts);

        bool instructionRemoved(InstructionId instruction) const;

        // Mutation. All mutators invalidate nothing structural: node and
        // variable ids stay stable, removals tombstone the instruction.
        VariableId addVariable(TypeId type, InitId init, std::optional<StringId> label,
                               ValueFacts facts = {});
        InstructionId addInstruction(Opcode opcode, std::span<const VariableId> results,
                                     std::span<const VariableId> operands);
        void setInstructionOperand(InstructionId instruction, std::size_t position,
                                   VariableId operand);
        void removeInstruction(InstructionId instruction);

        void setSliceStaticAttributes(InstructionId instruction, uint32_t lsb);
        void setSystemFunctionAttributes(InstructionId instruction,
                                         const SystemFunctionAttributes &attributes);
        void setSystemTaskAttributes(InstructionId instruction,
                                     const SystemTaskAttributes &attributes);
        void setDpiCallAttributes(InstructionId instruction,
                                  const DpiCallAttributes &attributes);

        DpiImportId addDpiImport(StringId symbol, std::span<const DpiParameter> parameters,
                                 DpiReturn returnValue = {});
        TypeId addType(const Type &type);
        StringId addString(std::string_view text);
        LiteralId addBitLiteral(TypeId type, std::span<const uint64_t> words);
        InitId addConstantInit(LiteralId literal);

        // State-access classification of one operand edge. Anything never
        // marked is PreCommit.
        AmStateAccess stateAccess(InstructionId instruction, std::size_t operandPosition) const;
        void setStateAccess(InstructionId instruction, std::size_t operandPosition,
                            AmStateAccess access);

        // Ordered-effect groups ride along as graph-level facts; scheduling
        // passes rewrite them as they merge or duplicate instructions.
        std::vector<OrderedEffect> &orderedEffects() noexcept;
        const std::vector<OrderedEffect> &orderedEffects() const noexcept;

        // Per-instruction effect classification (mirrors SchedulingFacts).
        InstructionEffect instructionEffect(InstructionId instruction) const;
        void setInstructionEffect(InstructionId instruction, InstructionEffect effect);
        const std::vector<InstructionEffect> &instructionEffects() const noexcept;

        // Bulk role view matching SchedulingFacts.variableRoles order.
        std::vector<VariableRole> variableRoles() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRAPH_HPP
