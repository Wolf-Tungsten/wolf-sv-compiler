#include "grhsim/am/grhsim_am_compute_graph_optimize.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"
#include "grhsim/am/grhsim_am_program.hpp"

#include <algorithm>
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

    // Shared fixture: a two-consumer producer (or1 -> mux1 atom and outAssign
    // atom), a single-use cone (and1 -> mux1), an unconsumed mux, and an
    // other-select mux. After the tree fold: {and1,mux1} tree, or1 singleton
    // (multi-use), mux2/muxOther singletons, outAssign singleton.
    struct AbsorbFixture
    {
        LinearProgramArtifact artifact;
        VariableId select;
        VariableId sharedVar;
        InstructionId and1;
        InstructionId or1;
        InstructionId mux1;
        InstructionId mux2;
        InstructionId muxOther;
        InstructionId outAssign;
    };

    AbsorbFixture makeFixture(bool pinShared)
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const StringId outName = builder.addString("out");
        const StringId sharedName = builder.addString("shared");
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
        if (pinShared)
        {
            interface.declaredVariables.push_back(
                VariableLabel{.variable = n2, .label = sharedName});
        }

        return AbsorbFixture{
            .artifact =
                LinearProgramArtifact{
                    .program = builder.finish(),
                    .interface = std::move(interface),
                    .schedulingFacts = {},
                },
            .select = select,
            .sharedVar = n2,
            .and1 = and1,
            .or1 = or1,
            .mux1 = mux1,
            .mux2 = mux2,
            .muxOther = muxOther,
            .outAssign = outAssign,
        };
    }

    bool tablesConsistent(const AmGraphSplitContext &context)
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
            for (uint32_t offset = context.atomMemberOffsets[atom];
                 offset < context.atomMemberOffsets[atom + 1]; ++offset)
            {
                if (context.instructionAtom[context.atomMembers[offset]] != atom)
                {
                    return false;
                }
            }
        }
        return total == context.instructionCount;
    }

    struct Prepared
    {
        AmGraph graph;
        AmGraphSplitContext context;
    };

    std::optional<Prepared> prepare(LinearProgramArtifact artifact,
                                    diag::Diagnostics &diagnostics)
    {
        AmGraph graph = AmGraph::fromLinearProgram(std::move(artifact));
        std::optional<AmGraphSplitContext> context =
            splitAmGraphStage(graph, ActivityScheduleOptions{}, diagnostics);
        if (!context)
        {
            return std::nullopt;
        }
        if (!foldSingleOutputTreeAtoms(graph, *context, diagnostics) ||
            diagnostics.hasError())
        {
            return std::nullopt;
        }
        Prepared prepared{std::move(graph), std::move(*context)};
        return prepared;
    }

    // Two-consumer singleton: the first consumer atom inherits the original
    // instruction, the second receives a copy; the producer atom disappears
    // and the crossing value vanishes from the atom DAG.
    int testBasicAbsorption()
    {
        AbsorbFixture fixture = makeFixture(false);
        diag::Diagnostics diagnostics;
        std::optional<Prepared> prepared =
            prepare(std::move(fixture.artifact), diagnostics);
        if (!prepared)
        {
            return fail("absorb fixture did not fold");
        }
        if (!absorbFanoutAtoms(prepared->graph, prepared->context,
                               /*maxAtomInstructions=*/2,
                               /*budgetMultiplier=*/1.0,
                               /*maxConsumers=*/256, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("absorption pass reported an error");
        }
        const AmGraphSplitContext &context = prepared->context;
        if (context.atomCount != 4)
        {
            return fail("producer atom did not disappear after absorption");
        }
        if (context.instructionCount != 7)
        {
            return fail("absorption did not add exactly one copy");
        }
        if (!tablesConsistent(context))
        {
            return fail("post-absorption tables are inconsistent");
        }
        // The original shared variable's uses must all live in one atom (the
        // inheritor); the copy's uses in the other.
        const DefUseIndex &defUse = context.defUse;
        const uint32_t shared = fixture.sharedVar.value;
        uint32_t useAtom = kInvalidAtomSignature;
        for (uint32_t offset = defUse.useOffsets[shared];
             offset < defUse.useOffsets[shared + 1]; ++offset)
        {
            const uint32_t atom = context.instructionAtom[defUse.uses[offset]];
            if (useAtom == kInvalidAtomSignature)
            {
                useAtom = atom;
            }
            else if (useAtom != atom)
            {
                return fail("original shared variable still spans two atoms");
            }
        }
        const uint32_t muxAtom = context.instructionAtom[fixture.mux1.value];
        if (context.atomKinds[muxAtom] != static_cast<uint8_t>(AmAtomKind::Tree) ||
            context.atomSignatures[muxAtom] != fixture.select.value)
        {
            return fail("mux-rooted tree lost its kind/signature after absorption");
        }
        // The absorbing tree holds and1 + or1(original-or-copy) + mux1.
        if (context.atomInstructions[muxAtom] != 3)
        {
            return fail("absorbing mux tree does not hold three members");
        }
        const uint32_t assignAtom = context.instructionAtom[fixture.outAssign.value];
        if (context.atomInstructions[assignAtom] != 2)
        {
            return fail("assign atom did not receive the shared copy");
        }
        return 0;
    }

    // A pinned (declared) shared variable keeps its orphan atom; every
    // consumer receives a copy and no longer references the original.
    int testPinnedOrphan()
    {
        AbsorbFixture fixture = makeFixture(true);
        diag::Diagnostics diagnostics;
        std::optional<Prepared> prepared =
            prepare(std::move(fixture.artifact), diagnostics);
        if (!prepared)
        {
            return fail("pinned absorb fixture did not fold");
        }
        if (!absorbFanoutAtoms(prepared->graph, prepared->context,
                               /*maxAtomInstructions=*/2,
                               /*budgetMultiplier=*/1.0,
                               /*maxConsumers=*/256, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("absorption pass reported an error on the pinned fixture");
        }
        const AmGraphSplitContext &context = prepared->context;
        if (context.atomCount != 5)
        {
            return fail("pinned producer atom was not kept as an orphan");
        }
        if (context.instructionCount != 8)
        {
            return fail("pinned absorption did not add two copies");
        }
        if (!tablesConsistent(context))
        {
            return fail("pinned-absorption tables are inconsistent");
        }
        const DefUseIndex &defUse = context.defUse;
        if (defUse.useOffsets[fixture.sharedVar.value + 1] -
                defUse.useOffsets[fixture.sharedVar.value] !=
            0)
        {
            return fail("orphan variable still has instruction uses");
        }
        const uint32_t orphanAtom = context.instructionAtom[fixture.or1.value];
        if (context.atomInstructions[orphanAtom] != 1)
        {
            return fail("orphan atom should hold only the original producer");
        }
        return 0;
    }

    // Zero budget absorbs nothing.
    int testBudgetExhaustion()
    {
        AbsorbFixture fixture = makeFixture(false);
        diag::Diagnostics diagnostics;
        std::optional<Prepared> prepared =
            prepare(std::move(fixture.artifact), diagnostics);
        if (!prepared)
        {
            return fail("budget fixture did not fold");
        }
        if (!absorbFanoutAtoms(prepared->graph, prepared->context,
                               /*maxAtomInstructions=*/2,
                               /*budgetMultiplier=*/0.0,
                               /*maxConsumers=*/256, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("absorption pass reported an error on the budget fixture");
        }
        if (prepared->context.atomCount != 5 ||
            prepared->context.instructionCount != 6)
        {
            return fail("zero budget still absorbed atoms");
        }
        return 0;
    }

    // A two-instruction producer tree is absorbed only when the cost cap
    // covers it: chain a->b with b shared by two consumers.
    int testCostCap()
    {
        LinearProgramBuilder builder;
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const VariableId x = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId y = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId z = builder.addVariable(u8Type, builder.zeroInit());
        const VariableId a = builder.addVariable(u8Type, builder.undefInit());
        const VariableId b = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r1 = builder.addVariable(u8Type, builder.undefInit());
        const VariableId r2 = builder.addVariable(u8Type, builder.undefInit());
        add(builder, Opcode::And, {a}, {x, y});
        add(builder, Opcode::Or, {b}, {a, z});
        add(builder, Opcode::Xor, {r1}, {b, x});
        add(builder, Opcode::Xnor, {r2}, {b, y});

        diag::Diagnostics diagnostics;
        std::optional<Prepared> prepared =
            prepare(LinearProgramArtifact{
                        .program = builder.finish(),
                        .interface = {},
                        .schedulingFacts = {},
                    },
                    diagnostics);
        if (!prepared)
        {
            return fail("cost-cap fixture did not fold");
        }
        // Post-fold: {a, or} tree (cost 2) shared by xor/xnor singletons.
        if (!absorbFanoutAtoms(prepared->graph, prepared->context,
                               /*maxAtomInstructions=*/1,
                               /*budgetMultiplier=*/1.0,
                               /*maxConsumers=*/256, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("absorption pass reported an error at cap 1");
        }
        if (prepared->context.atomCount != 3 || prepared->context.instructionCount != 4)
        {
            return fail("cap 1 must reject the two-instruction tree");
        }
        if (!absorbFanoutAtoms(prepared->graph, prepared->context,
                               /*maxAtomInstructions=*/2,
                               /*budgetMultiplier=*/1.0,
                               /*maxConsumers=*/256, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("absorption pass reported an error at cap 2");
        }
        if (prepared->context.atomCount != 2 || prepared->context.instructionCount != 6)
        {
            return fail("cap 2 must absorb the two-instruction tree");
        }
        if (!tablesConsistent(prepared->context))
        {
            return fail("cap-2 absorption tables are inconsistent");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testBasicAbsorption(); result != 0)
    {
        return result;
    }
    if (const int result = testPinnedOrphan(); result != 0)
    {
        return result;
    }
    if (const int result = testBudgetExhaustion(); result != 0)
    {
        return result;
    }
    return testCostCap();
}
