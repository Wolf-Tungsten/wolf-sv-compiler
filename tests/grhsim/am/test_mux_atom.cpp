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

    // One shared fixture: a same-select mux group with an exclusive producer
    // cone (and1), a shared producer (or1, also read by an output assign),
    // and an out-group mux on a different select.
    struct GroupFixture
    {
        LinearProgramArtifact artifact;
        VariableId x;
        VariableId y;
        VariableId z;
        VariableId w;
        VariableId select;
        InstructionId and1;
        InstructionId or1;
        InstructionId mux1;
        InstructionId mux2;
        InstructionId muxOther;
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
        (void)outAssign;

        return GroupFixture{
            .artifact =
                LinearProgramArtifact{
                    .program = builder.finish(),
                    .interface = std::move(interface),
                    .schedulingFacts = {},
                },
            .x = x,
            .y = y,
            .z = z,
            .w = w,
            .select = select,
            .and1 = and1,
            .or1 = or1,
            .mux1 = mux1,
            .mux2 = mux2,
            .muxOther = muxOther,
        };
    }

    std::optional<AmGraphSplitContext> splitOf(AmGraph &graph,
                                               diag::Diagnostics &diagnostics)
    {
        return splitAmGraphStage(graph, ActivityScheduleOptions{}, diagnostics);
    }

    bool atomContains(const AmGraphSplitContext &context, uint32_t atom,
                      InstructionId instruction)
    {
        for (uint32_t offset = context.atomMemberOffsets[atom];
             offset < context.atomMemberOffsets[atom + 1]; ++offset)
        {
            if (context.atomMembers[offset] == instruction.value)
            {
                return true;
            }
        }
        return false;
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
            if (context.atomMinInstruction[atom] != minInstruction)
            {
                return false;
            }
        }
        return total == instructionCount;
    }

    int testGroupingAbsorptionAndRebuild()
    {
        GroupFixture fixture = makeGroupFixture();
        AmGraph graph = AmGraph::fromLinearProgram(fixture.artifact);
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("mux-atom fixture did not split");
        }
        if (context->atomCount != 6)
        {
            return fail("unexpected pre-merge atom count");
        }
        if (!mergeMuxSelectAtoms(graph, *context, 512, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("mux-merge atom pass reported an error");
        }
        // {and1, mux1, mux2} merge into one atom; or1 (shared), muxOther
        // (other select) and the output assign stay singleton.
        if (context->atomCount != 4 ||
            !checkTableInvariants(*context, 6))
        {
            return fail("merged atom tables are inconsistent");
        }
        const uint32_t merged = context->instructionAtom[fixture.mux1.value];
        if (merged != context->instructionAtom[fixture.mux2.value] ||
            merged != context->instructionAtom[fixture.and1.value])
        {
            return fail("same-select group and its cone did not share one atom");
        }
        if (merged == context->instructionAtom[fixture.or1.value] ||
            merged == context->instructionAtom[fixture.muxOther.value])
        {
            return fail("shared producer or other-select mux joined the merged atom");
        }
        // Member order: cone preamble first, then the muxes as one
        // contiguous tail run.
        if (context->atomInstructions[merged] != 3 ||
            context->atomMembers[context->atomMemberOffsets[merged]] != fixture.and1.value ||
            context->atomMembers[context->atomMemberOffsets[merged] + 1] != fixture.mux1.value ||
            context->atomMembers[context->atomMemberOffsets[merged] + 2] != fixture.mux2.value ||
            context->atomMinInstruction[merged] != fixture.and1.value ||
            context->atomIsCommit[merged] != 0 ||
            context->atomStateWrites[merged] != 0)
        {
            return fail("merged atom member order or per-atom facts are wrong");
        }
        return 0;
    }

    int testCapSkipsWholeGroup()
    {
        GroupFixture fixture = makeGroupFixture();
        AmGraph graph = AmGraph::fromLinearProgram(fixture.artifact);
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("mux-atom cap fixture did not split");
        }
        if (!mergeMuxSelectAtoms(graph, *context, 2, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("mux-merge atom pass with a tight cap reported an error");
        }
        // 2 muxes + 1 cone instruction = 3 > cap 2: the whole group stays
        // as-is, nothing merges.
        if (context->atomCount != 6)
        {
            return fail("cap-skipped group changed the atom count");
        }
        for (uint32_t atom = 0; atom < context->atomCount; ++atom)
        {
            if (context->atomInstructions[atom] != 1)
            {
                return fail("cap-skipped group was partially merged");
            }
        }
        return 0;
    }

    // Merging {mux1, mux2} when mux1's result feeds mux2's arm through an
    // external node creates an atom-DAG 2-cycle {merged atom, external}: the
    // node cannot join the cone (shared with an outside consumer here, or
    // downstream-tainted in the next test), so the merged atom both feeds and
    // reads it. The DAG safety check must unmerge the group and restore the
    // original singleton atoms (brief rule d).
    int testSharedExternalChainUnmerges()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId z = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId w = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId select = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId n1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId out = builder.addVariable(u8Type, builder.undefInit());
        const InstructionId mux1 = add(builder, Opcode::Mux, {r1}, {select, x, y});
        const InstructionId external = add(builder, Opcode::And, {n1}, {r1, w});
        const InstructionId mux2 = add(builder, Opcode::Mux, {r2}, {select, n1, z});
        add(builder, Opcode::Assign, {out}, {n1});

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = {},
            .schedulingFacts = {},
        });
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("external-chain fixture did not split");
        }
        const uint32_t originalAtoms = context->atomCount;
        if (!mergeMuxSelectAtoms(graph, *context, 512, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("mux-merge atom pass reported an error on the chain fixture");
        }
        if (context->atomCount != originalAtoms ||
            !checkTableInvariants(*context, 4))
        {
            return fail("cycle-unmerged tables do not match the pre-merge state");
        }
        for (uint32_t atom = 0; atom < context->atomCount; ++atom)
        {
            if (context->atomInstructions[atom] != 1)
            {
                return fail("cycle-forming group was not fully unmerged");
            }
        }
        if (context->instructionAtom[mux1.value] == context->instructionAtom[mux2.value] ||
            context->instructionAtom[mux1.value] ==
                context->instructionAtom[external.value])
        {
            return fail("cycle-forming group kept a merged atom");
        }
        return 0;
    }

    // Same cycle shape, but the intermediate node is exclusively used by the
    // group: it is downstream of mux1 (tainted), so it cannot join the
    // preamble cone without breaking def-before-use, and the merge must
    // unmerge as well.
    int testExclusiveChainNodeUnmerges()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId z = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId w = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId select = builder.addVariable(u1Type, builder.zeroInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId n1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        const InstructionId mux1 = add(builder, Opcode::Mux, {r1}, {select, x, y});
        const InstructionId middle = add(builder, Opcode::And, {n1}, {r1, w});
        const InstructionId mux2 = add(builder, Opcode::Mux, {r2}, {select, n1, z});

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = {},
            .schedulingFacts = {},
        });
        diag::Diagnostics diagnostics;
        std::optional<AmGraphSplitContext> context = splitOf(graph, diagnostics);
        if (!context)
        {
            return fail("exclusive-chain fixture did not split");
        }
        if (!mergeMuxSelectAtoms(graph, *context, 512, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("mux-merge atom pass reported an error on the exclusive-chain fixture");
        }
        if (context->instructionAtom[mux1.value] == context->instructionAtom[mux2.value] ||
            context->instructionAtom[mux1.value] ==
                context->instructionAtom[middle.value])
        {
            return fail("tainted-chain group kept a merged atom");
        }
        for (uint32_t atom = 0; atom < context->atomCount; ++atom)
        {
            if (context->atomInstructions[atom] != 1)
            {
                return fail("tainted-chain group was not fully unmerged");
            }
        }
        return 0;
    }

    int testCombLoopAtomsStayIndivisible()
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
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        // Pure two-instruction comb cycle: packed into one SCC atom that no
        // mux-merge cone may split or join.
        const InstructionId loop1 = add(builder, Opcode::Or, {c1}, {c2, x});
        const InstructionId loop2 = add(builder, Opcode::And, {c2}, {c1, y});
        const InstructionId mux1 = add(builder, Opcode::Mux, {r1}, {select, c1, x});
        const InstructionId mux2 = add(builder, Opcode::Mux, {r2}, {select, y, z});

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
        if (!mergeMuxSelectAtoms(graph, *context, 512, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("mux-merge atom pass reported an error on the loop fixture");
        }
        const uint32_t loopAtom = context->instructionAtom[loop1.value];
        const uint32_t merged = context->instructionAtom[mux1.value];
        if (loopAtom == merged ||
            context->instructionAtom[loop2.value] != loopAtom ||
            context->instructionAtom[mux2.value] != merged)
        {
            return fail("comb-loop atom was split or joined by the mux-merge pass");
        }
        if (context->atomInstructions[merged] != 2)
        {
            return fail("comb-loop producer was absorbed into the mux atom");
        }
        return 0;
    }

    int testChainedMuxMemberOrder()
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
        if (!mergeMuxSelectAtoms(graph, *context, 512, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("mux-merge atom pass reported an error on the chain fixture");
        }
        const uint32_t merged = context->instructionAtom[mux1.value];
        if (merged != context->instructionAtom[mux2.value] ||
            context->atomInstructions[merged] != 2 ||
            context->atomMembers[context->atomMemberOffsets[merged]] != mux1.value ||
            context->atomMembers[context->atomMemberOffsets[merged] + 1] != mux2.value)
        {
            return fail("chained muxes are not in def-before-use member order");
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
        if (!mergeMuxSelectAtoms(first, *firstContext, 512, firstDiagnostics) ||
            !mergeMuxSelectAtoms(second, *secondContext, 512, secondDiagnostics))
        {
            return fail("mux-merge atom pass reported an error in determinism runs");
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
                   lhs.atomGraph.offsets == rhs.atomGraph.offsets &&
                   lhs.atomGraph.targets == rhs.atomGraph.targets;
        };
        if (!sameTables(*firstContext, *secondContext))
        {
            return fail("mux-merge atom pass is not deterministic");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testGroupingAbsorptionAndRebuild(); result != 0)
    {
        return result;
    }
    if (const int result = testCapSkipsWholeGroup(); result != 0)
    {
        return result;
    }
    if (const int result = testSharedExternalChainUnmerges(); result != 0)
    {
        return result;
    }
    if (const int result = testExclusiveChainNodeUnmerges(); result != 0)
    {
        return result;
    }
    if (const int result = testCombLoopAtomsStayIndivisible(); result != 0)
    {
        return result;
    }
    if (const int result = testChainedMuxMemberOrder(); result != 0)
    {
        return result;
    }
    return testDeterministicRebuild();
}
