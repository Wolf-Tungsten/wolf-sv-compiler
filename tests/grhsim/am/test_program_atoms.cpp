#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_program_validate.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(std::string_view message)
    {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    struct TwoAssignFixture
    {
        ScheduledProgram program;
        InstructionId first;
        InstructionId second;
    };

    int testImplicitSingletonAtoms()
    {
        LinearProgramBuilder linear;
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId a = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId b = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId c = linear.addVariable(u8Type, linear.zeroInit());
        const std::array<VariableId, 1> firstResults = {b};
        const std::array<VariableId, 1> firstOperands = {a};
        const InstructionId first =
            linear.addInstruction(Opcode::Assign, firstResults, firstOperands);
        const std::array<VariableId, 1> secondResults = {c};
        const std::array<VariableId, 1> secondOperands = {b};
        const InstructionId second =
            linear.addInstruction(Opcode::Assign, secondResults, secondOperands);

        ScheduledProgramBuilder builder(linear.finish());
        builder.addBlock(std::array{first, second});
        ScheduledProgram program = builder.finish();
        const std::size_t blockCount = program.blockCount();
        if (blockCount != 1 || program.atomCount() != 2 ||
            program.blockAtomCount(BlockId{0}) != 2)
        {
            return fail("addBlock did not wrap one Singleton atom per instruction");
        }
        for (std::size_t index = 0; index < 2; ++index)
        {
            const AtomId atom = program.blockAtom(BlockId{0}, index);
            const InstructionId expected = index == 0 ? first : second;
            if (atom.value != index ||
                program.atomKind(atom) != AmAtomKind::Singleton ||
                program.atomSignature(atom) != kInvalidAtomSignature ||
                program.atomInstructionCount(atom) != 1 ||
                program.atomInstruction(atom, 0) != expected ||
                program.atomInstruction(atom, 0) !=
                    program.blockInstruction(BlockId{0}, index))
            {
                return fail("implicit Singleton atom view is inconsistent");
            }
        }
        if (!validate(program, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("implicit Singleton atoms did not validate");
        }
        return 0;
    }

    int testExplicitAtomMetadata()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId sel = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId a = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId b = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId c = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId d = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId r1 = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId r2 = linear.addVariable(u8Type, linear.zeroInit());
        const InstructionId mux1 =
            linear.addInstruction(Opcode::Mux, std::array{r1}, std::array{sel, a, b});
        const InstructionId mux2 =
            linear.addInstruction(Opcode::Mux, std::array{r2}, std::array{sel, c, d});
        const InstructionId assign =
            linear.addInstruction(Opcode::Assign, std::array{d}, std::array{r1});

        ScheduledProgramBuilder builder(linear.finish());
        builder.beginBlock();
        builder.beginAtom(AmAtomKind::Tree, sel.value);
        builder.appendBlockInstruction(mux1);
        builder.appendBlockInstruction(mux2);
        builder.endAtom();
        builder.appendBlockInstruction(assign);
        builder.endBlock();
        ScheduledProgram program = builder.finish();

        if (program.atomCount() != 2 || program.blockAtomCount(BlockId{0}) != 2)
        {
            return fail("explicit atom construction produced the wrong atom count");
        }
        const AtomId merged = program.blockAtom(BlockId{0}, 0);
        const AtomId singleton = program.blockAtom(BlockId{0}, 1);
        if (program.atomKind(merged) != AmAtomKind::Tree ||
            program.atomSignature(merged) != sel.value ||
            program.atomInstructionCount(merged) != 2 ||
            program.atomInstruction(merged, 0) != mux1 ||
            program.atomInstruction(merged, 1) != mux2)
        {
            return fail("explicit Tree atom metadata did not round-trip");
        }
        if (program.atomKind(singleton) != AmAtomKind::Singleton ||
            program.atomInstructionCount(singleton) != 1 ||
            program.atomInstruction(singleton, 0) != assign)
        {
            return fail("trailing instruction did not become an implicit Singleton");
        }
        if (!validate(program, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("explicit atoms did not validate");
        }
        return 0;
    }

    int testBuilderRejectsMalformedAtoms()
    {
        LinearProgramBuilder linear;
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId a = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId b = linear.addVariable(u8Type, linear.zeroInit());
        const InstructionId assign =
            linear.addInstruction(Opcode::Assign, std::array{b}, std::array{a});

        // endAtom without beginAtom.
        {
            LinearProgramBuilder copy;
            const TypeId type = copy.addType(Type::bitVector(8));
            const VariableId x = copy.addVariable(type, copy.zeroInit());
            const VariableId y = copy.addVariable(type, copy.zeroInit());
            copy.addInstruction(Opcode::Assign, std::array{y}, std::array{x});
            ScheduledProgramBuilder builder(copy.finish());
            builder.beginBlock();
            bool caught = false;
            try
            {
                builder.endAtom();
            }
            catch (const std::logic_error &)
            {
                caught = true;
            }
            if (!caught)
            {
                return fail("endAtom without beginAtom was not rejected");
            }
        }
        // endBlock with an unfinished atom.
        {
            LinearProgramBuilder copy;
            const TypeId type = copy.addType(Type::bitVector(8));
            const VariableId x = copy.addVariable(type, copy.zeroInit());
            const VariableId y = copy.addVariable(type, copy.zeroInit());
            copy.addInstruction(Opcode::Assign, std::array{y}, std::array{x});
            ScheduledProgramBuilder builder(copy.finish());
            builder.beginBlock();
            builder.beginAtom(AmAtomKind::Singleton, 0);
            builder.appendBlockInstruction(assign);
            bool caught = false;
            try
            {
                builder.endBlock();
            }
            catch (const std::logic_error &)
            {
                caught = true;
            }
            if (!caught)
            {
                return fail("endBlock with an unfinished atom was not rejected");
            }
        }
        // An empty atom.
        {
            LinearProgramBuilder copy;
            const TypeId type = copy.addType(Type::bitVector(8));
            const VariableId x = copy.addVariable(type, copy.zeroInit());
            const VariableId y = copy.addVariable(type, copy.zeroInit());
            copy.addInstruction(Opcode::Assign, std::array{y}, std::array{x});
            ScheduledProgramBuilder builder(copy.finish());
            builder.beginBlock();
            builder.beginAtom(AmAtomKind::Singleton, 0);
            bool caught = false;
            try
            {
                builder.endAtom();
            }
            catch (const std::logic_error &)
            {
                caught = true;
            }
            if (!caught)
            {
                return fail("an empty atom was not rejected");
            }
        }
        return 0;
    }

    int testValidateCatchesBadAtomShape()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId sel = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId a = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId b = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId r1 = linear.addVariable(u8Type, linear.zeroInit());
        (void)sel;
        // A Tree atom needs >= 2 member instructions (a single-output tree);
        // a lone instruction fails the shape rule.
        const InstructionId assign =
            linear.addInstruction(Opcode::Assign, std::array{r1}, std::array{a});
        const InstructionId assign2 =
            linear.addInstruction(Opcode::Assign, std::array{b}, std::array{r1});
        ScheduledProgramBuilder builder(linear.finish());
        builder.beginBlock();
        builder.beginAtom(AmAtomKind::Tree, kInvalidAtomSignature);
        builder.appendBlockInstruction(assign);
        builder.endAtom();
        builder.appendBlockInstruction(assign2);
        builder.endBlock();
        ScheduledProgram program = builder.finish();
        const ValidationResult result =
            validate(program, ValidationOptions{.level = ValidationLevel::Semantic});
        bool found = false;
        for (const std::string &message : result.errors)
        {
            if (message.find("atom kind/signature") != std::string::npos)
            {
                found = true;
                break;
            }
        }
        if (result.success() || !found)
        {
            return fail("validator did not reject the single-member Tree atom");
        }
        return 0;
    }

    // End to end: a register write plus mux/cone logic through graphToProgram
    // must surface the atom layer (Singleton / Tree / CommitEvent): single-use
    // chains fold into their root's Tree atom, a multi-use mux stays a
    // select-signed Singleton, and the commit Block carries CommitEvent atoms.
    int testMaterializeCarriesAtomLayer()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId clock = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId clockOld = builder.addVariable(u1Type, builder.undefInit());
        const VariableId posedge = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId sel = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId a = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId b = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId c = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId d = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId next = builder.addVariable(u8Type, builder.undefInit());
        const VariableId out = builder.addVariable(u8Type, builder.undefInit());
        const VariableId state = builder.addVariable(u8Type, builder.zeroInit());

        builder.addInstruction(Opcode::ChangedPos, std::array{posedge},
                               std::array{clock, clockOld});
        const InstructionId mux1 =
            builder.addInstruction(Opcode::Mux, std::array{r1}, std::array{sel, a, b});
        const InstructionId mux2 =
            builder.addInstruction(Opcode::Mux, std::array{r2}, std::array{sel, c, d});
        const InstructionId or1 =
            builder.addInstruction(Opcode::Or, std::array{next}, std::array{r1, r2});
        const InstructionId write = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{next, state, posedge});
        // A second consumer for r1 (through the pinned output) keeps mux1
        // mux-rooted instead of folding into or1's tree.
        builder.addInstruction(Opcode::Assign, std::array{out}, std::array{r1});

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::ExternalOutput,
            VariableRole::State,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = write, .group = 0, .ordinal = 0},
        };
        ProgramInterface interface;
        interface.ports.push_back(PortBinding{
            .name = builder.addString("out"),
            .direction = PortDirection::Output,
            .output = out,
        });

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        });
        diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            GrhIRToGrhSimAMProgram::graphToProgram(std::move(graph),
                                                   ActivityScheduleOptions{
                                                       .fanoutAbsorbMaxInstructions = 2,
                                                   },
                                                   diagnostics);
        if (!model || diagnostics.hasError())
        {
            for (const auto &message : diagnostics.messages())
            {
                std::cerr << message.message << " [" << message.context << "]\n";
            }
            return fail("atom-layer fixture did not schedule");
        }
        if (!validate(*model, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("materialized model failed atom validation");
        }
        const ScheduledProgram &program = model->program;
        if (program.atomCount() == 0)
        {
            return fail("materialized program carries no atoms");
        }
        std::size_t treeAtoms = 0;
        std::size_t muxRootedSingletons = 0;
        std::size_t commitEventAtoms = 0;
        std::size_t coveredInstructions = 0;
        for (uint32_t blockIndex = 0; blockIndex < program.blockCount(); ++blockIndex)
        {
            const BlockId block{blockIndex};
            for (std::size_t index = 0; index < program.blockAtomCount(block); ++index)
            {
                const AtomId atom = program.blockAtom(block, index);
                coveredInstructions += program.atomInstructionCount(atom);
                const AmAtomKind kind = program.atomKind(atom);
                if (kind == AmAtomKind::Tree)
                {
                    ++treeAtoms;
                }
                if (kind == AmAtomKind::Singleton &&
                    program.atomSignature(atom) == sel.value)
                {
                    ++muxRootedSingletons;
                }
                if (kind == AmAtomKind::CommitEvent)
                {
                    ++commitEventAtoms;
                }
            }
        }
        if (coveredInstructions != program.view().instructionCount())
        {
            return fail("atoms do not tile the materialized program");
        }
        // mux2 folds into or1's tree (single-use chain); mux1 is then
        // absorbed by fanout absorption (NO0015): it joins or1's tree and a
        // copy lands in the assign's atom, so no select-signed singleton mux
        // survives and both consumer atoms become Trees.
        if (treeAtoms != 2)
        {
            return fail("expected two Tree atoms after scheduling (or1 tree + assign copy)");
        }
        if (muxRootedSingletons != 0)
        {
            return fail("expected no select-signed singleton mux atom after absorption");
        }
        if (commitEventAtoms == 0)
        {
            return fail("expected CommitEvent atoms in the commit Block");
        }
        (void)mux1;
        (void)mux2;
        (void)or1;
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testImplicitSingletonAtoms(); result != 0)
    {
        return result;
    }
    if (const int result = testExplicitAtomMetadata(); result != 0)
    {
        return result;
    }
    if (const int result = testBuilderRejectsMalformedAtoms(); result != 0)
    {
        return result;
    }
    if (const int result = testValidateCatchesBadAtomShape(); result != 0)
    {
        return result;
    }
    return testMaterializeCarriesAtomLayer();
}
