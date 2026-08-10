#include "grhsim/am/grhsim_am_compute_graph_optimize.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"
#include "grhsim/am/grhsim_am_program.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(std::string_view message)
    {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    InstructionId add(LinearProgramBuilder &builder, Opcode opcode,
                      std::initializer_list<VariableId> results,
                      std::initializer_list<VariableId> operands)
    {
        return builder.addInstruction(
            opcode, std::span<const VariableId>(results.begin(), results.size()),
            std::span<const VariableId>(operands.begin(), operands.size()));
    }

    // One shared fixture: a single-use producer cone (and1) feeding a mux, a
    // multi-use producer (or1, also read by an output assign), a mux whose
    // result is unconsumed, and an other-select mux.
    struct GroupFixture
    {
        LinearProgramArtifact artifact;
        VariableId select;
        VariableId otherSelect;
        InstructionId and1;
        InstructionId or1;
        InstructionId mux1;
        InstructionId mux2;
        InstructionId muxOther;
        InstructionId outAssign;
    };

    GroupFixture makeGroupFixture()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const StringId outName = builder.addString("out");
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId z = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId w = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId select = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId otherSelect = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId n1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId n2 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r3 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId out = builder.addVariable(u8Type, builder.undefInit());

        const InstructionId and1 = add(builder, Opcode::And, {n1}, {x, y});
        const InstructionId or1 = add(builder, Opcode::Or, {n2}, {z, w});
        const InstructionId mux1 = add(builder, Opcode::Mux, {r1}, {select, n1, n2});
        const InstructionId mux2 = add(builder, Opcode::Mux, {r2}, {select, x, y});
        const InstructionId muxOther =
            add(builder, Opcode::Mux, {r3}, {otherSelect, z, w});
        const InstructionId outAssign = add(builder, Opcode::Assign, {out}, {n2});

        ProgramInterface interface;
        interface.ports.push_back(PortBinding{
            .name = outName,
            .direction = PortDirection::Output,
            .output = out,
        });

        return GroupFixture{
            .artifact =
                LinearProgramArtifact{
                    .program = builder.finish(),
                    .interface = std::move(interface),
                    .schedulingFacts = {},
                },
            .select = select,
            .otherSelect = otherSelect,
            .and1 = and1,
            .or1 = or1,
            .mux1 = mux1,
            .mux2 = mux2,
            .muxOther = muxOther,
            .outAssign = outAssign,
        };
    }

    std::optional<AmGraphSplitContext> splitOf(AmGraph &graph,
                                               diag::Diagnostics &diagnostics)
    {
        return splitAmGraphStage(graph, ActivityScheduleOptions{}, diagnostics);
    }

    bool checkTableInvariants(const AmGraphSplitContext &context, uint32_t instructionCount)
    {
        uint64_t total = 0;
        for (uint32_t atom = 0; atom < context.atomCount; ++atom)
        {
            const uint32_t count =
                context.atomMemberOffsets[atom + 1] - context.atomMemberOffsets[atom];
            total += count;
            if (context.atomInstructions[atom] != count || count == 0)
            {
                return false;
            }
            uint32_t minInstruction = UINT32_MAX;
            for (uint32_t offset = context.atomMemberOffsets[atom];
                 offset < context.atomMemberOffsets[atom + 1]; ++offset)
            {
                const uint32_t instruction = context.atomMembers[offset];
                if (context.instructionAtom[instruction] != atom)
                {
                    return false;
                }
                minInstruction = std::min(minInstruction, instruction);
            }
            // Comb-loop carry-overs keep their split-stage first-member
            // minInstruction; fold sets always carry the true minimum.
            if (context.atomKinds[atom] !=
                    static_cast<uint8_t>(AmAtomKind::CombLoopScc) &&
                context.atomMinInstruction[atom] != minInstruction)
            {
                return false;
            }
        }
        return total == instructionCount;
    }

    // The single-use cone folds into its consumer root: and1 joins mux1's
    // atom (a mux-rooted Tree carrying the select signature); the multi-use
    // or1, the unconsumed mux2 and the other-select mux stay singletons.
    int testTreeFoldAndSignatures()
    {
        GroupFixture fixture = makeGroupFixture();
        AmGraph graph = AmGraph::fromLinearProgram(fixture.artifact);
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("tree-atom fixture did not split");
        }
        if (!foldSingleOutputTreeAtoms(graph, *context, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("tree-atom fold pass reported an error");
        }
        if (context->atomCount != 5 || !checkTableInvariants(*context, 6))
        {
            return fail("folded atom tables are inconsistent");
        }
        const uint32_t folded = context->instructionAtom[fixture.mux1.value];
        if (folded != context->instructionAtom[fixture.and1.value] ||
            context->atomInstructions[folded] != 2 ||
            context->atomMembers[context->atomMemberOffsets[folded]] !=
                fixture.and1.value ||
            context->atomMembers[context->atomMemberOffsets[folded] + 1] !=
                fixture.mux1.value)
        {
            return fail("single-use cone did not fold into its mux root in order");
        }
        if (context->atomKinds[folded] != static_cast<uint8_t>(AmAtomKind::Tree) ||
            context->atomSignatures[folded] != fixture.select.value)
        {
            return fail("mux-rooted tree atom lost its kind or select signature");
        }
        const uint32_t shared = context->instructionAtom[fixture.or1.value];
        if (shared == folded ||
            context->atomKinds[shared] != static_cast<uint8_t>(AmAtomKind::Singleton) ||
            context->atomSignatures[shared] != kInvalidAtomSignature)
        {
            return fail("multi-use producer folded or gained a signature");
        }
        const uint32_t lone = context->instructionAtom[fixture.mux2.value];
        if (context->atomInstructions[lone] != 1 ||
            context->atomSignatures[lone] != fixture.select.value)
        {
            return fail("singleton mux atom did not keep its select signature");
        }
        if (context->atomSignatures[context->instructionAtom[fixture.muxOther.value]] !=
            fixture.otherSelect.value)
        {
            return fail("other-select singleton mux carries a wrong signature");
        }
        return 0;
    }

    // A chained same-select mux pair folds into one tree (root last); the
    // tree's select signature comes from the root mux.
    int testChainFold()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId z = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId select = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        const InstructionId mux1 = add(builder, Opcode::Mux, {r1}, {select, x, y});
        const InstructionId mux2 = add(builder, Opcode::Mux, {r2}, {select, r1, z});

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = {},
            .schedulingFacts = {},
        });
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("chained-mux fixture did not split");
        }
        if (!foldSingleOutputTreeAtoms(graph, *context, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("tree-atom fold pass reported an error on the chain fixture");
        }
        const uint32_t folded = context->instructionAtom[mux1.value];
        if (folded != context->instructionAtom[mux2.value] ||
            context->atomInstructions[folded] != 2 ||
            context->atomMembers[context->atomMemberOffsets[folded]] != mux1.value ||
            context->atomMembers[context->atomMemberOffsets[folded] + 1] != mux2.value ||
            context->atomKinds[folded] != static_cast<uint8_t>(AmAtomKind::Tree) ||
            context->atomSignatures[folded] != select.value)
        {
            return fail("chained muxes did not fold into one mux-rooted tree");
        }
        return 0;
    }

    // A pinned (output-port) result never folds away, but its single-use
    // producer still folds into it: the tree root keeps the pinned value as
    // the atom's observable output.
    int testPinnedRootKeepsTree()
    {
        LinearProgramBuilder builder;
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const StringId outName = builder.addString("out");
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId n1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId out = builder.addVariable(u8Type, builder.undefInit());
        const InstructionId and1 = add(builder, Opcode::And, {n1}, {x, y});
        const InstructionId outAssign = add(builder, Opcode::Assign, {out}, {n1});
        ProgramInterface interface;
        interface.ports.push_back(PortBinding{
            .name = outName,
            .direction = PortDirection::Output,
            .output = out,
        });

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = {},
        });
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("pinned fixture did not split");
        }
        if (!foldSingleOutputTreeAtoms(graph, *context, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("tree-atom fold pass reported an error on the pinned fixture");
        }
        if (context->atomCount != 1)
        {
            return fail("pinned-root tree did not collapse to one atom");
        }
        const uint32_t atom = context->instructionAtom[outAssign.value];
        if (context->instructionAtom[and1.value] != atom ||
            context->atomKinds[atom] != static_cast<uint8_t>(AmAtomKind::Tree) ||
            context->atomSignatures[atom] != kInvalidAtomSignature ||
            context->atomMembers[context->atomMemberOffsets[atom] + 1] !=
                outAssign.value)
        {
            return fail("pinned-root tree shape or signature is wrong");
        }
        return 0;
    }

    // Comb-loop SCC atoms are fold barriers in both directions: loop members
    // never fold out, outside producers never fold in.
    int testCombLoopBarrier()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId z = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId select = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId c1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId c2 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        // Pure two-instruction comb cycle: packed into one SCC atom.
        const InstructionId loop1 = add(builder, Opcode::Or, {c1}, {c2, x});
        const InstructionId loop2 = add(builder, Opcode::And, {c2}, {c1, y});
        const InstructionId mux1 = add(builder, Opcode::Mux, {r1}, {select, c1, z});

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = {},
            .schedulingFacts = {},
        });
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("comb-loop fixture did not split");
        }
        if (context->instructionAtom[loop1.value] != context->instructionAtom[loop2.value])
        {
            return fail("comb cycle was not packed into one atom");
        }
        if (!foldSingleOutputTreeAtoms(graph, *context, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("tree-atom fold pass reported an error on the loop fixture");
        }
        const uint32_t loopAtom = context->instructionAtom[loop1.value];
        if (context->instructionAtom[loop2.value] != loopAtom ||
            context->atomInstructions[loopAtom] != 2 ||
            context->atomKinds[loopAtom] !=
                static_cast<uint8_t>(AmAtomKind::CombLoopScc) ||
            context->atomSignatures[loopAtom] != kInvalidAtomSignature)
        {
            return fail("comb-loop atom was split or relabeled by the fold pass");
        }
        const uint32_t muxAtom = context->instructionAtom[mux1.value];
        if (muxAtom == loopAtom || context->atomInstructions[muxAtom] != 1 ||
            context->atomSignatures[muxAtom] != select.value)
        {
            return fail("loop consumer did not stay a singleton mux atom");
        }
        return 0;
    }

    // Commit-side consumers are fold barriers: a producer feeding only a
    // state write stays a compute-side singleton atom.
    int testCommitBoundary()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId clock = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId clockOld = builder.addVariable(u1Type, builder.undefInit());
        const VariableId posedge = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId n1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId state = builder.addVariable(u8Type, builder.zeroInit());

        add(builder, Opcode::ChangedPos, {posedge}, {clock, clockOld});
        const InstructionId and1 = add(builder, Opcode::And, {n1}, {x, y});
        const InstructionId write = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{n1, state, posedge});

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::State,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = write, .group = 0, .ordinal = 0},
        };

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = {},
            .schedulingFacts = std::move(facts),
        });
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("commit-boundary fixture did not split");
        }
        if (!foldSingleOutputTreeAtoms(graph, *context, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("tree-atom fold pass reported an error on the commit fixture");
        }
        const uint32_t andAtom = context->instructionAtom[and1.value];
        const uint32_t writeAtom = context->instructionAtom[write.value];
        if (andAtom == writeAtom || context->atomIsCommit[andAtom] != 0 ||
            context->atomIsCommit[writeAtom] != 1 ||
            context->atomKinds[writeAtom] !=
                static_cast<uint8_t>(AmAtomKind::CommitEvent))
        {
            return fail("commit boundary was crossed by the fold pass");
        }
        if (!checkTableInvariants(*context, 3))
        {
            return fail("commit-boundary tables are inconsistent");
        }
        return 0;
    }

    int testDeterministicRebuild()
    {
        GroupFixture firstFixture = makeGroupFixture();
        GroupFixture secondFixture = makeGroupFixture();
        AmGraph first = AmGraph::fromLinearProgram(firstFixture.artifact);
        AmGraph second = AmGraph::fromLinearProgram(secondFixture.artifact);
        diag::Diagnostics firstDiagnostics;
        diag::Diagnostics secondDiagnostics;
        std::optional<AmGraphSplitContext> firstContext = splitOf(first, firstDiagnostics);
        std::optional<AmGraphSplitContext> secondContext =
            splitOf(second, secondDiagnostics);
        if (!firstContext || !secondContext)
        {
            return fail("determinism fixtures did not split");
        }
        if (!foldSingleOutputTreeAtoms(first, *firstContext, firstDiagnostics) ||
            !foldSingleOutputTreeAtoms(second, *secondContext, secondDiagnostics))
        {
            return fail("tree-atom fold pass reported an error in determinism runs");
        }
        const auto sameTables = [](const AmGraphSplitContext &lhs,
                                   const AmGraphSplitContext &rhs) {
            return lhs.atomCount == rhs.atomCount &&
                   lhs.instructionAtom == rhs.instructionAtom &&
                   lhs.atomMemberOffsets == rhs.atomMemberOffsets &&
                   lhs.atomMembers == rhs.atomMembers &&
                   lhs.atomInstructions == rhs.atomInstructions &&
                   lhs.atomStateWrites == rhs.atomStateWrites &&
                   lhs.atomIsCommit == rhs.atomIsCommit &&
                   lhs.atomMinInstruction == rhs.atomMinInstruction &&
                   lhs.commitEventRank == rhs.commitEventRank &&
                   lhs.atomKinds == rhs.atomKinds &&
                   lhs.atomSignatures == rhs.atomSignatures &&
                   lhs.atomGraph.offsets == rhs.atomGraph.offsets &&
                   lhs.atomGraph.targets == rhs.atomGraph.targets;
        };
        if (!sameTables(*firstContext, *secondContext))
        {
            return fail("tree-atom fold pass is not deterministic");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testTreeFoldAndSignatures(); result != 0)
    {
        return result;
    }
    if (const int result = testChainFold(); result != 0)
    {
        return result;
    }
    if (const int result = testPinnedRootKeepsTree(); result != 0)
    {
        return result;
    }
    if (const int result = testCombLoopBarrier(); result != 0)
    {
        return result;
    }
    if (const int result = testCommitBoundary(); result != 0)
    {
        return result;
    }
    return testDeterministicRebuild();
}
