#include "grhsim/am/grhsim_am_compute_graph_optimize.hpp"

#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{


    namespace
    {
        constexpr char kDiagnosticContext[] = "grhsim-am-mux-merge-atom";
        constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

        struct MuxMergeGroup
        {
            uint32_t select = 0;
            std::vector<uint32_t> muxes; // ascending instruction ids
            std::vector<uint32_t> cone;  // absorbed producer instructions
        };

        // Deterministic topological order of `members` restricted to def-use
        // edges internal to the set; ties break by ascending instruction id,
        // so the member order of a merged atom is fully determined by the
        // program, not by traversal order.
        std::vector<uint32_t> topoOrderSubset(ProgramView program,
                                              const DefUseIndex &defUse,
                                              const std::vector<uint32_t> &members)
        {
            std::unordered_map<uint32_t, uint32_t> localOf;
            localOf.reserve(members.size() * 2U);
            for (uint32_t local = 0; local < members.size(); ++local)
            {
                localOf.emplace(members[local], local);
            }
            std::vector<uint32_t> indegree(members.size(), 0);
            std::vector<std::vector<uint32_t>> outgoing(members.size());
            for (uint32_t local = 0; local < members.size(); ++local)
            {
                for (VariableId operand :
                     program.operands(InstructionId{members[local]}))
                {
                    if (!operand.valid())
                    {
                        continue;
                    }
                    const uint32_t producer = defUse.definitions[operand.value];
                    const auto found = localOf.find(producer);
                    if (found == localOf.end())
                    {
                        continue;
                    }
                    outgoing[found->second].push_back(local);
                    ++indegree[local];
                }
            }
            std::priority_queue<uint32_t, std::vector<uint32_t>,
                                std::greater<uint32_t>>
                ready;
            for (uint32_t local = 0; local < members.size(); ++local)
            {
                if (indegree[local] == 0)
                {
                    ready.push(local);
                }
            }
            std::vector<uint32_t> order;
            order.reserve(members.size());
            while (!ready.empty())
            {
                const uint32_t local = ready.top();
                ready.pop();
                order.push_back(members[local]);
                for (uint32_t target : outgoing[local])
                {
                    if (--indegree[target] == 0)
                    {
                        ready.push(target);
                    }
                }
            }
            // The member set is a DAG by construction (atom-level cycles were
            // excluded); fall back to ascending ids defensively.
            if (order.size() != members.size())
            {
                order = members;
                std::sort(order.begin(), order.end());
            }
            return order;
        }
    } // namespace

    void optAmComputeGraph(AmComputeGraph &computeGraph,
                           const AmGraphPartitionInput &input)
    {
        // Reserved stage boundary (framework: opt-am-compute-graph). Graph-level
        // compute optimizations land here; intentionally a no-op today.
        (void)computeGraph;
        (void)input;
    }

    bool mergeMuxSelectAtoms(AmGraph &graph, AmGraphSplitContext &context,
                             std::size_t muxAtomMax,
                             wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        if (muxAtomMax == 0)
        {
            return true;
        }
        const ProgramView program = graph.program();
        const uint32_t instructionCount = context.instructionCount;
        const DefUseIndex &defUse = context.defUse;
        const uint32_t originalAtomCount = context.atomCount;

        // ---- grouping: same-select compute muxes in single-instruction
        // atoms; only groups of at least two are atomization candidates.
        std::map<uint32_t, std::vector<uint32_t>> groupsBySelect;
        for (uint32_t index = 0; index < instructionCount; ++index)
        {
            const InstructionId instruction{index};
            if (program.opcode(instruction) != Opcode::Mux)
            {
                continue;
            }
            const uint32_t atom = context.instructionAtom[index];
            if (atom == kInvalidIndex || context.atomIsCommit[atom] != 0 ||
                context.atomInstructions[atom] != 1)
            {
                continue;
            }
            const auto operands = program.operands(instruction);
            if (operands.size() != 3 || !operands[0].valid())
            {
                continue;
            }
            groupsBySelect[operands[0].value].push_back(index);
        }
        std::vector<MuxMergeGroup> groups;
        for (auto &[select, muxes] : groupsBySelect)
        {
            if (muxes.size() >= 2)
            {
                groups.push_back(MuxMergeGroup{select, std::move(muxes), {}});
            }
        }
        if (groups.empty())
        {
            return true;
        }
        // Greedy priority: larger groups claim first (doc §8.2); the select
        // variable id tiebreak keeps the order total and deterministic.
        std::sort(groups.begin(), groups.end(),
                  [](const MuxMergeGroup &lhs, const MuxMergeGroup &rhs) {
                      return lhs.muxes.size() != rhs.muxes.size()
                                 ? lhs.muxes.size() > rhs.muxes.size()
                                 : lhs.select < rhs.select;
                  });

        // Host-visible / interface-referenced variables never join an
        // absorbed cone: their value must stay observable outside the atom.
        std::vector<uint8_t> pinnedVariable(context.variableCount, 0);
        for (const PortBinding &port : graph.interface().ports)
        {
            if (port.input.valid())
            {
                pinnedVariable[port.input.value] = 1;
            }
            if (port.output.valid())
            {
                pinnedVariable[port.output.value] = 1;
            }
        }
        for (const VariableLabel &label : graph.interface().declaredVariables)
        {
            if (label.variable.valid())
            {
                pinnedVariable[label.variable.value] = 1;
            }
        }
        for (uint32_t variable = 0; variable < context.variableCount; ++variable)
        {
            const VariableRole role = graph.valueFacts(VariableId{variable}).roles;
            if (hasRole(role, VariableRole::ExternalOutput) ||
                hasRole(role, VariableRole::Observable))
            {
                pinnedVariable[variable] = 1;
            }
        }

        std::vector<uint8_t> claimed(instructionCount, 0);
        std::vector<uint8_t> memberMark(instructionCount, 0);
        std::vector<uint8_t> tainted(instructionCount, 0);
        std::vector<uint8_t> inClosure(instructionCount, 0);
        std::vector<uint8_t> inCone(instructionCount, 0);
        std::vector<uint32_t> worklist;

        std::vector<MuxMergeGroup> accepted;
        std::size_t capSkippedGroups = 0;
        std::size_t capSkippedMuxes = 0;

        for (MuxMergeGroup &group : groups)
        {
            std::vector<uint32_t> members;
            for (uint32_t instruction : group.muxes)
            {
                if (!claimed[instruction])
                {
                    members.push_back(instruction);
                }
            }
            if (members.size() < 2)
            {
                continue;
            }
            for (uint32_t instruction : members)
            {
                claimed[instruction] = 1;
                memberMark[instruction] = 1;
            }

            // Forward taint: everything downstream of the group muxes. A
            // cone node depending on a group mux would break the
            // cone-preamble / contiguous-mux member order, so downstream
            // instructions stay outside as interface producers.
            std::vector<uint32_t> taintList;
            const auto taint = [&](uint32_t instruction) {
                if (!tainted[instruction])
                {
                    tainted[instruction] = 1;
                    taintList.push_back(instruction);
                }
            };
            for (uint32_t instruction : members)
            {
                taint(instruction);
                for (VariableId result : program.results(InstructionId{instruction}))
                {
                    if (!result.valid())
                    {
                        continue;
                    }
                    for (uint32_t offset = defUse.useOffsets[result.value];
                         offset < defUse.useOffsets[result.value + 1]; ++offset)
                    {
                        worklist.push_back(defUse.uses[offset]);
                    }
                }
            }
            while (!worklist.empty())
            {
                const uint32_t instruction = worklist.back();
                worklist.pop_back();
                if (tainted[instruction])
                {
                    continue;
                }
                taint(instruction);
                for (VariableId result : program.results(InstructionId{instruction}))
                {
                    if (!result.valid())
                    {
                        continue;
                    }
                    for (uint32_t offset = defUse.useOffsets[result.value];
                         offset < defUse.useOffsets[result.value + 1]; ++offset)
                    {
                        worklist.push_back(defUse.uses[offset]);
                    }
                }
            }

            // Backward closure from the arm producers. Candidates are
            // complete single-instruction compute atoms (comb-loop atoms are
            // indivisible and never join a cone: doc §10.3); the select
            // variable's own cone is a barrier (select stays the atom's
            // interface).
            std::vector<uint32_t> closureList;
            const auto eligible = [&](uint32_t instruction) {
                if (claimed[instruction] || tainted[instruction])
                {
                    return false;
                }
                const uint32_t atom = context.instructionAtom[instruction];
                if (atom == kInvalidIndex || context.atomIsCommit[atom] != 0 ||
                    context.atomInstructions[atom] != 1)
                {
                    return false;
                }
                if (opcodeTraits(program.opcode(InstructionId{instruction})).effect !=
                    OpcodeEffect::Pure)
                {
                    return false;
                }
                const auto results = program.results(InstructionId{instruction});
                return results.size() == 1 && results.front().valid() &&
                       !pinnedVariable[results.front().value];
            };
            const auto pushProducer = [&](VariableId variable) {
                if (!variable.valid() || variable.value == group.select)
                {
                    return;
                }
                const uint32_t producer = defUse.definitions[variable.value];
                if (producer == kInvalidIndex || inClosure[producer] ||
                    !eligible(producer))
                {
                    return;
                }
                inClosure[producer] = 1;
                closureList.push_back(producer);
                worklist.push_back(producer);
            };
            for (uint32_t instruction : members)
            {
                const auto operands = program.operands(InstructionId{instruction});
                pushProducer(operands[1]);
                pushProducer(operands[2]);
            }
            while (!worklist.empty())
            {
                const uint32_t instruction = worklist.back();
                worklist.pop_back();
                for (VariableId operand : program.operands(InstructionId{instruction}))
                {
                    pushProducer(operand);
                }
            }

            // Exclusive use, greatest fixpoint: a cone node survives only
            // while every consumer of its result is a group mux or a
            // surviving cone node; shared values stay interface inputs.
            const auto violates = [&](uint32_t instruction) {
                const VariableId result =
                    program.results(InstructionId{instruction}).front();
                for (uint32_t offset = defUse.useOffsets[result.value];
                     offset < defUse.useOffsets[result.value + 1]; ++offset)
                {
                    const uint32_t consumer = defUse.uses[offset];
                    if (!memberMark[consumer] && !inCone[consumer])
                    {
                        return true;
                    }
                }
                return false;
            };
            for (uint32_t instruction : closureList)
            {
                inCone[instruction] = 1;
                if (violates(instruction))
                {
                    worklist.push_back(instruction);
                }
            }
            while (!worklist.empty())
            {
                const uint32_t instruction = worklist.back();
                worklist.pop_back();
                if (!inCone[instruction])
                {
                    continue;
                }
                inCone[instruction] = 0;
                for (VariableId operand : program.operands(InstructionId{instruction}))
                {
                    if (!operand.valid())
                    {
                        continue;
                    }
                    const uint32_t producer = defUse.definitions[operand.value];
                    if (producer != kInvalidIndex && inCone[producer] &&
                        violates(producer))
                    {
                        worklist.push_back(producer);
                    }
                }
            }
            std::vector<uint32_t> cone;
            for (uint32_t instruction : closureList)
            {
                if (inCone[instruction])
                {
                    cone.push_back(instruction);
                }
            }

            const uint64_t total =
                static_cast<uint64_t>(members.size()) + cone.size();
            if (total > muxAtomMax)
            {
                // Activation-granularity guard: the whole group stays as-is;
                // the emitter's block-level same-select fusion still applies.
                ++capSkippedGroups;
                capSkippedMuxes += members.size();
                for (uint32_t instruction : members)
                {
                    claimed[instruction] = 0;
                }
            }
            else
            {
                for (uint32_t instruction : cone)
                {
                    claimed[instruction] = 1;
                }
                accepted.push_back(MuxMergeGroup{group.select, std::move(members),
                                                 std::move(cone)});
            }

            for (uint32_t instruction : group.muxes)
            {
                memberMark[instruction] = 0;
            }
            for (uint32_t instruction : taintList)
            {
                tainted[instruction] = 0;
            }
            for (uint32_t instruction : closureList)
            {
                inClosure[instruction] = 0;
                inCone[instruction] = 0;
            }
        }

        if (accepted.empty())
        {
            if (capSkippedGroups != 0)
            {
                diagnostics.info("AM mux-merge atom: no group fit the cap: skipped_groups=" +
                                     std::to_string(capSkippedGroups) +
                                     " skipped_muxes=" + std::to_string(capSkippedMuxes),
                                 std::string(kDiagnosticContext));
            }
            return true;
        }

        // ---- atom table rebuild (with DAG-safety repair) ------------------
        const CsrGraph instructionGraph =
            detail::buildInstructionGraph(instructionCount, defUse,
                                          context.orderedEdges);
        // Original (pre-merge) tables: the unmerged-atom carry-over and the
        // merged-group sort keys must keep addressing them even when a DAG
        // repair round restarts the rebuild after context was rewritten.
        const std::vector<uint32_t> originalInstructionAtom = context.instructionAtom;
        const std::vector<uint32_t> originalAtomMemberOffsets = context.atomMemberOffsets;
        const std::vector<uint32_t> originalAtomMembers = context.atomMembers;
        const std::vector<uint32_t> originalAtomInstructions = context.atomInstructions;
        const std::vector<uint32_t> originalAtomStateWrites = context.atomStateWrites;
        const std::vector<uint8_t> originalAtomIsCommit = context.atomIsCommit;
        const std::vector<uint32_t> originalAtomMinInstruction = context.atomMinInstruction;
        const std::vector<uint32_t> originalCommitEventRank = context.commitEventRank;
        const std::vector<uint8_t> originalAtomKinds = context.atomKinds;
        const std::vector<uint32_t> originalAtomSignatures = context.atomSignatures;

        std::vector<uint32_t> groupOfAtom;
        std::size_t repairRounds = 0;
        std::size_t unmergedGroups = 0;
        for (;;)
        {
            // New atom order: unmerged atoms keep their relative order;
            // merged groups slot in at their earliest member's original atom
            // id, so the relative atom order is stable.
            std::vector<uint8_t> coveredAtom(originalAtomCount, 0);
            std::vector<uint32_t> groupMinAtom(accepted.size(), kInvalidIndex);
            for (uint32_t groupIndex = 0; groupIndex < accepted.size(); ++groupIndex)
            {
                const auto track = [&](uint32_t instruction) {
                    const uint32_t atom = originalInstructionAtom[instruction];
                    coveredAtom[atom] = 1;
                    groupMinAtom[groupIndex] =
                        std::min(groupMinAtom[groupIndex], atom);
                };
                for (uint32_t instruction : accepted[groupIndex].muxes)
                {
                    track(instruction);
                }
                for (uint32_t instruction : accepted[groupIndex].cone)
                {
                    track(instruction);
                }
            }
            struct AtomSlot
            {
                uint32_t sortKey;
                uint32_t oldAtom;
                uint32_t groupIndex;
            };
            std::vector<AtomSlot> slots;
            slots.reserve(originalAtomCount);
            for (uint32_t atom = 0; atom < originalAtomCount; ++atom)
            {
                if (!coveredAtom[atom])
                {
                    slots.push_back(AtomSlot{atom, atom, kInvalidIndex});
                }
            }
            for (uint32_t groupIndex = 0; groupIndex < accepted.size(); ++groupIndex)
            {
                slots.push_back(AtomSlot{groupMinAtom[groupIndex], kInvalidIndex,
                                         groupIndex});
            }
            std::sort(slots.begin(), slots.end(),
                      [](const AtomSlot &lhs, const AtomSlot &rhs) {
                          return std::tie(lhs.sortKey, lhs.oldAtom, lhs.groupIndex) <
                                 std::tie(rhs.sortKey, rhs.oldAtom, rhs.groupIndex);
                      });

            const uint32_t newAtomCount = static_cast<uint32_t>(slots.size());
            std::vector<uint32_t> newInstructionAtom(instructionCount, kInvalidIndex);
            std::vector<uint32_t> newAtomMemberOffsets(newAtomCount + 1, 0);
            std::vector<uint32_t> newAtomMembers;
            newAtomMembers.reserve(instructionCount);
            std::vector<uint32_t> newAtomInstructions(newAtomCount, 0);
            std::vector<uint32_t> newAtomStateWrites(newAtomCount, 0);
            std::vector<uint8_t> newAtomIsCommit(newAtomCount, 0);
            std::vector<uint32_t> newAtomMinInstruction(newAtomCount, kInvalidIndex);
            std::vector<uint32_t> newCommitEventRank(newAtomCount, 0);
            std::vector<uint8_t> newAtomKinds(newAtomCount, 0);
            std::vector<uint32_t> newAtomSignatures(newAtomCount, 0);
            groupOfAtom.assign(newAtomCount, kInvalidIndex);

            for (uint32_t slotIndex = 0; slotIndex < newAtomCount; ++slotIndex)
            {
                const AtomSlot &slot = slots[slotIndex];
                newAtomMemberOffsets[slotIndex] =
                    static_cast<uint32_t>(newAtomMembers.size());
                if (slot.groupIndex == kInvalidIndex)
                {
                    // Unmerged atom: carry the original member list and all
                    // per-atom properties verbatim.
                    const uint32_t oldAtom = slot.oldAtom;
                    for (uint32_t offset = originalAtomMemberOffsets[oldAtom];
                         offset < originalAtomMemberOffsets[oldAtom + 1]; ++offset)
                    {
                        const uint32_t instruction = originalAtomMembers[offset];
                        newInstructionAtom[instruction] = slotIndex;
                        newAtomMembers.push_back(instruction);
                    }
                    newAtomInstructions[slotIndex] =
                        originalAtomInstructions[oldAtom];
                    newAtomStateWrites[slotIndex] =
                        originalAtomStateWrites[oldAtom];
                    newAtomIsCommit[slotIndex] = originalAtomIsCommit[oldAtom];
                    newAtomMinInstruction[slotIndex] =
                        originalAtomMinInstruction[oldAtom];
                    newCommitEventRank[slotIndex] =
                        originalCommitEventRank[oldAtom];
                    newAtomKinds[slotIndex] = originalAtomKinds[oldAtom];
                    newAtomSignatures[slotIndex] = originalAtomSignatures[oldAtom];
                    continue;
                }
                const MuxMergeGroup &group = accepted[slot.groupIndex];
                groupOfAtom[slotIndex] = slot.groupIndex;
                // Absorbed cone first (topological preamble), then the group
                // muxes as one contiguous run (the emitter's if-else fusion
                // consumes that run).
                const std::vector<uint32_t> orderedCone =
                    topoOrderSubset(program, defUse, group.cone);
                const std::vector<uint32_t> orderedMuxes =
                    topoOrderSubset(program, defUse, group.muxes);
                uint32_t minInstruction = kInvalidIndex;
                for (uint32_t instruction : orderedCone)
                {
                    newInstructionAtom[instruction] = slotIndex;
                    newAtomMembers.push_back(instruction);
                    minInstruction = std::min(minInstruction, instruction);
                }
                for (uint32_t instruction : orderedMuxes)
                {
                    newInstructionAtom[instruction] = slotIndex;
                    newAtomMembers.push_back(instruction);
                    minInstruction = std::min(minInstruction, instruction);
                }
                newAtomInstructions[slotIndex] =
                    static_cast<uint32_t>(group.cone.size() + group.muxes.size());
                newAtomStateWrites[slotIndex] = 0;
                newAtomIsCommit[slotIndex] = 0;
                newAtomMinInstruction[slotIndex] = minInstruction;
                newCommitEventRank[slotIndex] = 0;
                newAtomKinds[slotIndex] = static_cast<uint8_t>(AmAtomKind::MuxMerge);
                newAtomSignatures[slotIndex] = group.select;
            }
            newAtomMemberOffsets[newAtomCount] =
                static_cast<uint32_t>(newAtomMembers.size());

            context.atomCount = newAtomCount;
            context.instructionAtom = std::move(newInstructionAtom);
            context.atomMemberOffsets = std::move(newAtomMemberOffsets);
            context.atomMembers = std::move(newAtomMembers);
            context.atomInstructions = std::move(newAtomInstructions);
            context.atomStateWrites = std::move(newAtomStateWrites);
            context.atomIsCommit = std::move(newAtomIsCommit);
            context.atomMinInstruction = std::move(newAtomMinInstruction);
            context.commitEventRank = std::move(newCommitEventRank);
            context.atomKinds = std::move(newAtomKinds);
            context.atomSignatures = std::move(newAtomSignatures);
            context.atomGraph =
                detail::buildCondensationGraph(instructionGraph,
                                               context.instructionAtom,
                                               newAtomCount);

            // DAG safety: a merge must not turn the atom DAG cyclic (a member
            // pair connected externally in both directions). Exclusive use
            // plus the downstream taint makes this impossible in practice;
            // treat any multi-atom SCC involving a merged atom as a hard
            // failure of that group and rebuild once without it.
            const detail::SccResult scc =
                detail::findStronglyConnectedComponents(context.atomGraph);
            if (static_cast<uint32_t>(scc.count) == newAtomCount)
            {
                break;
            }
            std::vector<uint32_t> sccSize(scc.count, 0);
            for (uint32_t atom = 0; atom < newAtomCount; ++atom)
            {
                ++sccSize[scc.component[atom]];
            }
            std::vector<uint8_t> unmerge(accepted.size(), 0);
            bool any = false;
            for (uint32_t atom = 0; atom < newAtomCount; ++atom)
            {
                if (sccSize[scc.component[atom]] > 1 &&
                    groupOfAtom[atom] != kInvalidIndex)
                {
                    unmerge[groupOfAtom[atom]] = 1;
                    any = true;
                }
            }
            if (!any)
            {
                break;
            }
            ++repairRounds;
            std::vector<MuxMergeGroup> surviving;
            for (uint32_t groupIndex = 0; groupIndex < accepted.size(); ++groupIndex)
            {
                if (!unmerge[groupIndex])
                {
                    surviving.push_back(std::move(accepted[groupIndex]));
                }
                else
                {
                    ++unmergedGroups;
                }
            }
            accepted = std::move(surviving);
        }

        // Oversized statistics follow the same per-class limits as the split
        // stage (merged atoms exceeding them intentionally occupy one
        // oversized block each, the gsim oversized-super-node analogue).
        context.oversizedAtomCount = 0;
        context.maxAtomInstructions = 0;
        context.maxAtomStateWrites = 0;
        for (uint32_t atom = 0; atom < context.atomCount; ++atom)
        {
            context.maxAtomInstructions =
                std::max<std::size_t>(context.maxAtomInstructions,
                                      context.atomInstructions[atom]);
            context.maxAtomStateWrites =
                std::max<std::size_t>(context.maxAtomStateWrites,
                                      context.atomStateWrites[atom]);
            const std::size_t instructionLimit =
                context.atomIsCommit[atom] != 0
                    ? context.maxCommitAtomsPerBlock
                    : context.maxAtomsPerBlock;
            if (context.atomInstructions[atom] > instructionLimit)
            {
                ++context.oversizedAtomCount;
            }
        }

        // Rebuild the compute/commit induced subgraphs on the merged atom DAG.
        std::string blockError;
        std::optional<AmGraphSplit> split =
            splitAmGraph(context.partitionInput(), blockError);
        if (!split)
        {
            diagnostics.error("AM mux-merge atom produced an unsplittable graph: " +
                                  blockError,
                              std::string(kDiagnosticContext));
            return false;
        }
        context.split = std::move(*split);

        std::size_t mergedMuxes = 0;
        std::size_t absorbedCone = 0;
        for (const MuxMergeGroup &group : accepted)
        {
            mergedMuxes += group.muxes.size();
            absorbedCone += group.cone.size();
        }
        diagnostics.info("AM mux-merge atom: groups=" + std::to_string(accepted.size()) +
                             " merged_muxes=" + std::to_string(mergedMuxes) +
                             " absorbed_cone=" + std::to_string(absorbedCone) +
                             " cap_skipped_groups=" + std::to_string(capSkippedGroups) +
                             " cap_skipped_muxes=" + std::to_string(capSkippedMuxes) +
                             " cycle_unmerged_groups=" + std::to_string(unmergedGroups) +
                             " repair_rounds=" + std::to_string(repairRounds),
                         std::string(kDiagnosticContext));
        return true;
    }

} // namespace wolvrix::lib::grhsim::am
