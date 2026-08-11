#include "grhsim/am/grhsim_am_compute_graph_optimize.hpp"

#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        constexpr char kDiagnosticContext[] = "grhsim-am-tree-atom";
        constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

        // Deterministic topological order of `members` restricted to def-use
        // edges internal to the set; ties break by ascending instruction id,
        // so the member order of a folded atom is fully determined by the
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
            // The member set is a DAG by construction (the fold contracts
            // def-use edges of a DAG); fall back to ascending ids defensively.
            if (order.size() != members.size())
            {
                order = members;
                std::sort(order.begin(), order.end());
            }
            return order;
        }
    } // namespace

    bool absorbFanoutAtoms(AmGraph &graph, AmGraphSplitContext &context,
                           std::size_t maxAtomInstructions, double budgetMultiplier,
                           std::size_t maxConsumers,
                           wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        ProgramView program = graph.program();
        const uint32_t atomCount = context.atomCount;
        const DefUseIndex &defUse = context.defUse;

        // Host-visible / interface-referenced variables keep an orphan atom
        // (same pin rule as the tree-atom fold).
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

        // Instructions referenced by ordered-effect edges can never be
        // absorbed (pure members make this defensive, but keep it exact).
        std::vector<uint8_t> orderedInstruction(context.instructionCount, 0);
        for (const OrderEdge &edge : context.orderedEdges)
        {
            orderedInstruction[edge.source] = 1;
            orderedInstruction[edge.target] = 1;
        }

        const uint8_t kCombLoop = static_cast<uint8_t>(AmAtomKind::CombLoopScc);

        // Atom roots by the fold/split invariant (members store root-last).
        // Member lists grow as the sweep absorbs upstream trees into
        // downstream atoms, so members.back() is NOT stable -- always
        // consult this table for an atom's root.
        std::vector<uint32_t> rootOfAtom(atomCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < atomCount; ++atom)
        {
            if (context.atomMemberOffsets[atom + 1] >
                context.atomMemberOffsets[atom])
            {
                rootOfAtom[atom] =
                    context.atomMembers[context.atomMemberOffsets[atom + 1] - 1];
            }
        }

        // Live tables (extended by copies / rewired by moves as the sweep
        // proceeds).
        std::vector<std::vector<uint32_t>> membersOf(atomCount);
        for (uint32_t atom = 0; atom < atomCount; ++atom)
        {
            membersOf[atom].assign(context.atomMembers.begin() +
                                       context.atomMemberOffsets[atom],
                                   context.atomMembers.begin() +
                                       context.atomMemberOffsets[atom + 1]);
        }
        std::vector<uint32_t> atomOfInstr = context.instructionAtom;
        std::vector<uint32_t> definitionOf = defUse.definitions;

        std::vector<std::vector<uint32_t>> consumersOf(atomCount);
        for (uint32_t variable = 0; variable < defUse.definitions.size(); ++variable)
        {
            const uint32_t definition = defUse.definitions[variable];
            if (definition == kInvalidIndex)
            {
                continue;
            }
            const uint32_t producerAtom = atomOfInstr[definition];
            if (producerAtom == kInvalidIndex)
            {
                continue;
            }
            for (uint32_t offset = defUse.useOffsets[variable];
                 offset < defUse.useOffsets[variable + 1]; ++offset)
            {
                const uint32_t consumerAtom = atomOfInstr[defUse.uses[offset]];
                if (consumerAtom != kInvalidIndex && consumerAtom != producerAtom)
                {
                    consumersOf[producerAtom].push_back(consumerAtom);
                }
            }
        }
        for (std::vector<uint32_t> &consumers : consumersOf)
        {
            std::sort(consumers.begin(), consumers.end());
            consumers.erase(std::unique(consumers.begin(), consumers.end()),
                            consumers.end());
        }

        // Atom-DAG topological order (Kahn, ascending-id tie-break).
        std::vector<uint32_t> topo;
        topo.reserve(atomCount);
        {
            std::vector<uint32_t> indegree(atomCount, 0);
            for (uint32_t atom = 0; atom < atomCount; ++atom)
            {
                for (uint32_t offset = context.atomGraph.offsets[atom];
                     offset < context.atomGraph.offsets[atom + 1]; ++offset)
                {
                    ++indegree[context.atomGraph.targets[offset]];
                }
            }
            std::priority_queue<uint32_t, std::vector<uint32_t>,
                                std::greater<uint32_t>>
                ready;
            for (uint32_t atom = 0; atom < atomCount; ++atom)
            {
                if (indegree[atom] == 0)
                {
                    ready.push(atom);
                }
            }
            while (!ready.empty())
            {
                const uint32_t top = ready.top();
                ready.pop();
                topo.push_back(top);
                for (uint32_t offset = context.atomGraph.offsets[top];
                     offset < context.atomGraph.offsets[top + 1]; ++offset)
                {
                    if (--indegree[context.atomGraph.targets[offset]] == 0)
                    {
                        ready.push(context.atomGraph.targets[offset]);
                    }
                }
            }
            if (topo.size() != atomCount)
            {
                diagnostics.error("AM fanout absorption saw a cyclic atom graph",
                                  std::string(kDiagnosticContext));
                return false;
            }
        }

        std::size_t totalComputeInstructions = 0;
        for (uint32_t atom = 0; atom < atomCount; ++atom)
        {
            if (context.atomIsCommit[atom] == 0)
            {
                totalComputeInstructions += context.atomInstructions[atom];
            }
        }
        const std::size_t instructionBudget = static_cast<std::size_t>(
            budgetMultiplier * static_cast<double>(totalComputeInstructions));

        const auto absorbableSource = [&](uint32_t atom) {
            if (context.atomIsCommit[atom] != 0 ||
                context.atomKinds[atom] == kCombLoop)
            {
                return false;
            }
            const std::vector<uint32_t> &members = membersOf[atom];
            if (members.empty())
            {
                return false;
            }
            const InstructionId root{rootOfAtom[atom]};
            if (program.results(root).size() != 1)
            {
                return false;
            }
            for (const uint32_t member : members)
            {
                if (orderedInstruction[member] ||
                    opcodeTraits(program.opcode(InstructionId{member})).effect !=
                        OpcodeEffect::Pure)
                {
                    return false;
                }
            }
            return true;
        };
        // A consumer atom is an absorbable target iff it is compute-side,
        // not a comb-loop packing, and its root is not part of the
        // activation/host machinery (StateRead roots are fine: the cone is
        // absorbed into the read port's operand tree, the port itself is
        // never duplicated).
        const auto absorbableTarget = [&](uint32_t atom) {
            if (context.atomIsCommit[atom] != 0 ||
                context.atomKinds[atom] == kCombLoop)
            {
                return false;
            }
            if (membersOf[atom].empty())
            {
                return false;
            }
            const OpcodeEffect effect = opcodeTraits(
                program.opcode(InstructionId{rootOfAtom[atom]})).effect;
            return effect == OpcodeEffect::Pure || effect == OpcodeEffect::StateRead;
        };

        std::size_t absorbedAtoms = 0;
        std::size_t orphanAtoms = 0;
        std::size_t duplicatedInstructions = 0;
        std::vector<uint8_t> dirtyAtom(atomCount, 0);
        // Deferred rewire rules per target atom: (absorbed root variable ->
        // per-target copy variable), replayed against the FINAL member lists
        // at rebuild time -- later absorptions can move/copy new users of the
        // variable into the target after it absorbed the source. Targets of
        // an absorption are always downstream atoms whose own sweep turn has
        // already passed, so a recorded target is never absorbed afterwards
        // and stays alive.
        std::vector<std::vector<std::pair<uint32_t, uint32_t>>> rewireRules(
            atomCount);

        for (auto it = topo.rbegin(); it != topo.rend(); ++it)
        {
            const uint32_t atom = *it;
            if (membersOf[atom].empty() || !absorbableSource(atom))
            {
                continue;
            }
            const uint32_t rootInstruction = rootOfAtom[atom];
            const uint32_t rootVariable =
                program.results(InstructionId{rootInstruction}).front().value;

            std::vector<uint32_t> targets;
            bool external = pinnedVariable[rootVariable] != 0;
            for (const uint32_t consumer : consumersOf[atom])
            {
                if (absorbableTarget(consumer))
                {
                    targets.push_back(consumer);
                }
                else
                {
                    external = true;
                }
            }
            const std::size_t fanout = targets.size();
            if (fanout < 2 || fanout > maxConsumers)
            {
                continue;
            }
            const std::size_t cost = membersOf[atom].size();
            if (cost > maxAtomInstructions)
            {
                continue;
            }
            const std::size_t dup =
                (external ? fanout : fanout - 1) * cost;
            if (duplicatedInstructions + dup > instructionBudget)
            {
                break;
            }

            // The tree's external input producers, computed from the source
            // atom's ORIGINAL members before any mutation (scanning a
            // target's mixed member list after a move would spuriously add
            // producers that only the target's own members use).
            std::vector<uint32_t> inputProducers;
            for (const uint32_t member : membersOf[atom])
            {
                for (const VariableId operand :
                     program.operands(InstructionId{member}))
                {
                    if (!operand.valid())
                    {
                        continue;
                    }
                    const uint32_t definition = definitionOf[operand.value];
                    if (definition == kInvalidIndex)
                    {
                        continue;
                    }
                    const uint32_t producer = atomOfInstr[definition];
                    if (producer == kInvalidIndex || producer == atom)
                    {
                        continue;
                    }
                    inputProducers.push_back(producer);
                }
            }
            std::sort(inputProducers.begin(), inputProducers.end());
            inputProducers.erase(
                std::unique(inputProducers.begin(), inputProducers.end()),
                inputProducers.end());

            // The first target inherits the original members when no orphan
            // is needed; every other target gets a fresh copy of the tree.
            const std::size_t copyBegin = external ? 0 : 1;
            for (std::size_t index = copyBegin; index < fanout; ++index)
            {
                const uint32_t target = targets[index];
                std::unordered_map<uint32_t, uint32_t> varMap;
                uint32_t copyRootVariable = kInvalidIndex;
                for (const uint32_t member : membersOf[atom])
                {
                    const InstructionId source{member};
                    const auto results = program.results(source);
                    const auto operands = program.operands(source);
                    std::vector<VariableId> newResults;
                    newResults.reserve(results.size());
                    for (const VariableId result : results)
                    {
                        const VariableRecord &record = program.variable(result);
                        AmValueFacts facts = graph.valueFacts(result);
                        facts.roles = VariableRole::None;
                        const VariableId fresh =
                            graph.addVariable(record.type, record.init,
                                              std::nullopt, facts);
                        varMap.emplace(result.value, fresh.value);
                        definitionOf.push_back(kInvalidIndex);
                        newResults.push_back(fresh);
                    }
                    std::vector<VariableId> newOperands;
                    newOperands.reserve(operands.size());
                    for (const VariableId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            newOperands.push_back(operand);
                            continue;
                        }
                        const auto found = varMap.find(operand.value);
                        newOperands.push_back(
                            found != varMap.end()
                                ? VariableId{found->second}
                                : operand);
                    }
                    const InstructionId copy = graph.addInstruction(
                        program.opcode(source), newResults, newOperands);
                    if (const auto slice =
                            program.sliceStaticAttributes(source))
                    {
                        graph.setSliceStaticAttributes(copy, slice->lsb);
                    }
                    atomOfInstr.push_back(target);
                    if (!newResults.empty())
                    {
                        definitionOf[newResults.front().value] = copy.value;
                        if (member == rootInstruction)
                        {
                            copyRootVariable = newResults.front().value;
                        }
                    }
                    membersOf[target].push_back(copy.value);
                }
                dirtyAtom[target] = 1;
                // Rewiring is deferred to the rebuild: later absorptions can
                // still move/copy new users of the absorbed root variable
                // into this target, so the rule is applied to the FINAL
                // member list.
                rewireRules[target].emplace_back(rootVariable,
                                                 copyRootVariable);
            }
            if (!external)
            {
                // Move: the first target inherits the original members; the
                // source atom dies (no orphan needed).
                const uint32_t target = targets.front();
                auto &inherited = membersOf[atom];
                auto &destination = membersOf[target];
                destination.insert(destination.end(), inherited.begin(),
                                   inherited.end());
                for (const uint32_t member : inherited)
                {
                    atomOfInstr[member] = target;
                }
                inherited.clear();
                dirtyAtom[target] = 1;
            }
            else
            {
                ++orphanAtoms;
                // Targets no longer consume the orphan's root variable.
                for (const uint32_t target : targets)
                {
                    auto &consumers = consumersOf[atom];
                    consumers.erase(
                        std::remove(consumers.begin(), consumers.end(), target),
                        consumers.end());
                }
            }

            // Fanout bookkeeping for upstream producers of the absorbed
            // atom's external inputs: the source atom leaves their consumer
            // sets (move case), every target joins them.
            for (const uint32_t producer : inputProducers)
            {
                auto &consumers = consumersOf[producer];
                if (!external)
                {
                    consumers.erase(
                        std::remove(consumers.begin(), consumers.end(), atom),
                        consumers.end());
                }
                for (const uint32_t target : targets)
                {
                    if (std::find(consumers.begin(), consumers.end(), target) ==
                        consumers.end())
                    {
                        consumers.push_back(target);
                    }
                }
                std::sort(consumers.begin(), consumers.end());
                consumers.erase(
                    std::unique(consumers.begin(), consumers.end()),
                    consumers.end());
            }

            duplicatedInstructions += dup;
            ++absorbedAtoms;
        }

        if (absorbedAtoms == 0)
        {
            return true;
        }

        // ---- rebuild atom tables on the mutated program ------------------
        // Replay the deferred rewire rules against the final member lists.
        for (uint32_t atom = 0; atom < atomCount; ++atom)
        {
            if (rewireRules[atom].empty() || membersOf[atom].empty())
            {
                continue;
            }
            for (const uint32_t member : membersOf[atom])
            {
                const InstructionId user{member};
                const auto operands = program.operands(user);
                for (std::size_t position = 0; position < operands.size();
                     ++position)
                {
                    if (!operands[position].valid())
                    {
                        continue;
                    }
                    for (const auto &[from, to] : rewireRules[atom])
                    {
                        if (operands[position].value == from)
                        {
                            graph.setInstructionOperand(user, position,
                                                        VariableId{to});
                            break;
                        }
                    }
                }
            }
        }

        context.instructionCount =
            static_cast<uint32_t>(graph.instructionCount());
        context.variableCount = static_cast<uint32_t>(graph.variableCount());
        DefUseIndex newDefUse = detail::buildDefUseIndex(program);

        std::vector<uint32_t> oldToNew(atomCount, kInvalidIndex);
        uint32_t newAtomCount = 0;
        for (uint32_t atom = 0; atom < atomCount; ++atom)
        {
            if (!membersOf[atom].empty())
            {
                oldToNew[atom] = newAtomCount++;
            }
        }

        std::vector<uint32_t> newInstructionAtom(context.instructionCount,
                                                 kInvalidIndex);
        std::vector<uint32_t> newAtomMemberOffsets(newAtomCount + 1, 0);
        std::vector<uint32_t> newAtomMembers;
        newAtomMembers.reserve(context.atomMembers.size() +
                               duplicatedInstructions);
        std::vector<uint32_t> newAtomInstructions(newAtomCount, 0);
        std::vector<uint32_t> newAtomStateWrites(newAtomCount, 0);
        std::vector<uint8_t> newAtomIsCommit(newAtomCount, 0);
        std::vector<uint32_t> newAtomMinInstruction(newAtomCount,
                                                    kInvalidIndex);
        std::vector<uint32_t> newCommitEventRank(newAtomCount, 0);
        std::vector<uint8_t> newAtomKinds(newAtomCount, 0);
        std::vector<uint32_t> newAtomSignatures(newAtomCount,
                                                kInvalidAtomSignature);

        for (uint32_t atom = 0; atom < atomCount; ++atom)
        {
            if (oldToNew[atom] == kInvalidIndex)
            {
                continue;
            }
            const uint32_t slot = oldToNew[atom];
            newAtomMemberOffsets[slot] =
                static_cast<uint32_t>(newAtomMembers.size());
            std::vector<uint32_t> members = membersOf[atom];
            if (dirtyAtom[atom] != 0)
            {
                members = topoOrderSubset(program, newDefUse, members);
                // The atom's own root must remain the unique sink of the
                // enlarged tree.
                if (members.back() != rootOfAtom[atom])
                {
                    std::string dump;
                    for (const uint32_t member : members)
                    {
                        const InstructionId mi{member};
                        uint32_t internalUses = 0;
                        for (const VariableId result : program.results(mi))
                        {
                            for (uint32_t uo = newDefUse.useOffsets[result.value];
                                 uo < newDefUse.useOffsets[result.value + 1]; ++uo)
                            {
                                if (std::find(members.begin(), members.end(),
                                              newDefUse.uses[uo]) != members.end())
                                {
                                    ++internalUses;
                                }
                            }
                        }
                        dump += " [" + std::to_string(member) + " op=" +
                                std::to_string(static_cast<int>(program.opcode(mi))) +
                                " iuses=" + std::to_string(internalUses) + "]";
                    }
                    diagnostics.error(
                        "AM fanout absorption changed an atom's sink: atom=" +
                            std::to_string(atom) +
                            " root=" + std::to_string(rootOfAtom[atom]) +
                            " back=" + std::to_string(members.back()) +
                            " members=" + std::to_string(members.size()) + dump,
                        std::string(kDiagnosticContext));
                    return false;
                }
            }
            uint32_t minInstruction = kInvalidIndex;
            for (const uint32_t member : members)
            {
                newInstructionAtom[member] = slot;
                newAtomMembers.push_back(member);
                minInstruction = std::min(minInstruction, member);
            }
            newAtomInstructions[slot] =
                static_cast<uint32_t>(members.size());
            newAtomStateWrites[slot] = context.atomStateWrites[atom];
            newAtomIsCommit[slot] = context.atomIsCommit[atom];
            newAtomMinInstruction[slot] = minInstruction;
            newCommitEventRank[slot] = context.commitEventRank[atom];
            if (context.atomIsCommit[atom] != 0)
            {
                newAtomKinds[slot] = context.atomKinds[atom];
                newAtomSignatures[slot] = context.atomSignatures[atom];
            }
            else if (context.atomKinds[atom] == kCombLoop)
            {
                newAtomKinds[slot] = kCombLoop;
            }
            else
            {
                newAtomKinds[slot] =
                    members.size() > 1
                        ? static_cast<uint8_t>(AmAtomKind::Tree)
                        : static_cast<uint8_t>(AmAtomKind::Singleton);
                const InstructionId root{members.back()};
                const auto rootOperands = program.operands(root);
                if (program.opcode(root) == Opcode::Mux &&
                    rootOperands.size() == 3 && rootOperands[0].valid())
                {
                    newAtomSignatures[slot] = rootOperands[0].value;
                }
            }
        }
        newAtomMemberOffsets[newAtomCount] =
            static_cast<uint32_t>(newAtomMembers.size());

        for (uint32_t instruction = 0; instruction < context.instructionCount;
             ++instruction)
        {
            if (newInstructionAtom[instruction] == kInvalidIndex)
            {
                diagnostics.error(
                    "AM fanout absorption left an instruction atomless: " +
                        std::to_string(instruction),
                    std::string(kDiagnosticContext));
                return false;
            }
        }

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
        context.defUse = std::move(newDefUse);

        const CsrGraph instructionGraph =
            detail::buildInstructionGraph(context.instructionCount,
                                          context.defUse,
                                          context.orderedEdges);
        context.atomGraph = detail::buildCondensationGraph(
            instructionGraph, context.instructionAtom, newAtomCount);

        // Absorption contracts consumer-side memberships only; a multi-atom
        // SCC here means the rewire rule was broken.
        const detail::SccResult scc =
            detail::findStronglyConnectedComponents(context.atomGraph);
        if (static_cast<uint32_t>(scc.count) != newAtomCount)
        {
            diagnostics.error(
                "AM fanout absorption produced a cyclic atom graph",
                std::string(kDiagnosticContext));
            return false;
        }

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

        std::string blockError;
        std::optional<AmGraphSplit> split =
            splitAmGraph(context.partitionInput(), blockError);
        if (!split)
        {
            diagnostics.error("AM fanout absorption produced an unsplittable "
                              "graph: " +
                                  blockError,
                              std::string(kDiagnosticContext));
            return false;
        }
        context.split = std::move(*split);

        diagnostics.info(
            "AM fanout absorption: absorbed_atoms=" +
                std::to_string(absorbedAtoms) +
                " orphan_atoms=" + std::to_string(orphanAtoms) +
                " duplicated_instructions=" +
                std::to_string(duplicatedInstructions) +
                " atoms=" + std::to_string(newAtomCount),
            std::string(kDiagnosticContext));
        return true;
    }

    void optAmComputeGraph(AmComputeGraph &computeGraph,
                           const AmGraphPartitionInput &input)
    {
        // Reserved stage boundary (framework: opt-am-compute-graph). Graph-level
        // compute optimizations land here; intentionally a no-op today.
        (void)computeGraph;
        (void)input;
    }

    bool foldSingleOutputTreeAtoms(AmGraph &graph, AmGraphSplitContext &context,
                                   wolvrix::lib::diag::Diagnostics &diagnostics,
                                   std::size_t foldMaxInstructions)
    {
        const ProgramView program = graph.program();
        const uint32_t instructionCount = context.instructionCount;
        const DefUseIndex &defUse = context.defUse;
        const uint32_t originalAtomCount = context.atomCount;

        // ---- fold decision (NO0008): a compute instruction whose single
        // result feeds exactly one consumer folds into that consumer's atom,
        // so every atom becomes a single-output expression tree (the gsim
        // "node = signal + assignTree" analogue). Fold barriers: commit-side
        // atoms, comb-loop SCC atoms (multi-instruction), non-pure effects,
        // and pinned (interface / observable / external) results. The fold
        // relation is a static forest over the instruction DAG, so it is
        // confluent and cannot create atom-level cycles (edge contraction in
        // a DAG).

        // Host-visible / interface-referenced variables never fold away:
        // their value must stay observable outside the atom.
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

        const auto singletonComputeAtom = [&](uint32_t instruction) {
            const uint32_t atom = context.instructionAtom[instruction];
            return context.atomIsCommit[atom] == 0 &&
                   context.atomInstructions[atom] == 1;
        };

        std::vector<uint32_t> foldInto(instructionCount, kInvalidIndex);
        for (uint32_t index = 0; index < instructionCount; ++index)
        {
            if (!singletonComputeAtom(index))
            {
                continue;
            }
            const InstructionId instruction{index};
            if (opcodeTraits(program.opcode(instruction)).effect != OpcodeEffect::Pure)
            {
                continue;
            }
            const auto results = program.results(instruction);
            if (results.size() != 1 || !results.front().valid())
            {
                continue;
            }
            const uint32_t variable = results.front().value;
            if (pinnedVariable[variable] ||
                defUse.useOffsets[variable + 1] - defUse.useOffsets[variable] != 1)
            {
                continue;
            }
            const uint32_t consumer = defUse.uses[defUse.useOffsets[variable]];
            if (consumer == index || !singletonComputeAtom(consumer))
            {
                continue;
            }
            foldInto[index] = consumer;
        }

        // NO0002 L2 alignment: optional fold-set size cap. The fold relation
        // forms in-trees keyed by their sink; with a cap, each tree is
        // partitioned into connected sub-trees of at most foldMaxInstructions
        // members by detaching the fold link of the first node that would
        // overflow a set (deterministic DFS from each root, children in
        // ascending instruction order). 0 = uncapped (default).
        if (foldMaxInstructions != 0)
        {
            std::vector<uint32_t> childOffsets(instructionCount + 1, 0);
            for (uint32_t index = 0; index < instructionCount; ++index)
            {
                if (foldInto[index] != kInvalidIndex)
                {
                    ++childOffsets[foldInto[index] + 1];
                }
            }
            for (uint32_t index = 0; index < instructionCount; ++index)
            {
                childOffsets[index + 1] += childOffsets[index];
            }
            std::vector<uint32_t> children(childOffsets[instructionCount]);
            {
                std::vector<uint32_t> cursor(childOffsets.begin(),
                                             childOffsets.end() - 1);
                for (uint32_t index = 0; index < instructionCount; ++index)
                {
                    if (foldInto[index] != kInvalidIndex)
                    {
                        children[cursor[foldInto[index]]++] = index;
                    }
                }
            }
            std::vector<uint8_t> visited(instructionCount, 0);
            std::vector<uint32_t> roots;
            std::vector<uint32_t> stack;
            for (uint32_t index = 0; index < instructionCount; ++index)
            {
                if (foldInto[index] == kInvalidIndex &&
                    childOffsets[index + 1] != childOffsets[index])
                {
                    roots.push_back(index);
                }
            }
            std::size_t rootCursor = 0;
            while (rootCursor < roots.size())
            {
                const uint32_t root = roots[rootCursor++];
                if (visited[root] != 0)
                {
                    continue;
                }
                std::size_t size = 0;
                stack.push_back(root);
                while (!stack.empty())
                {
                    const uint32_t node = stack.back();
                    stack.pop_back();
                    if (visited[node] != 0)
                    {
                        continue;
                    }
                    if (size == foldMaxInstructions)
                    {
                        // Overflow: node starts a fresh sub-tree (its fold
                        // link to the full parent is cut); its children are
                        // traversed when the new root is processed.
                        foldInto[node] = kInvalidIndex;
                        roots.push_back(node);
                        continue;
                    }
                    visited[node] = 1;
                    ++size;
                    for (uint32_t offset = childOffsets[node + 1];
                         offset-- > childOffsets[node];)
                    {
                        stack.push_back(children[offset]);
                    }
                }
            }
        }

        // Root of each fold chain (memoized walk; chains are def-use paths).
        std::vector<uint32_t> rootOf(instructionCount, kInvalidIndex);
        std::vector<uint32_t> chain;
        for (uint32_t index = 0; index < instructionCount; ++index)
        {
            if (rootOf[index] != kInvalidIndex)
            {
                continue;
            }
            uint32_t cursor = index;
            chain.clear();
            while (rootOf[cursor] == kInvalidIndex &&
                   foldInto[cursor] != kInvalidIndex)
            {
                chain.push_back(cursor);
                cursor = foldInto[cursor];
            }
            const uint32_t root =
                rootOf[cursor] != kInvalidIndex ? rootOf[cursor] : cursor;
            rootOf[cursor] = root;
            for (const uint32_t member : chain)
            {
                rootOf[member] = root;
            }
        }

        // Fold sets keyed by their root instruction. Only singleton compute
        // atoms take part; commit and comb-loop atoms carry over unchanged.
        std::unordered_map<uint32_t, std::vector<uint32_t>> setMembers;
        for (uint32_t index = 0; index < instructionCount; ++index)
        {
            if (!singletonComputeAtom(index))
            {
                continue;
            }
            setMembers[rootOf[index]].push_back(index);
        }

        // New atom order: carry-over atoms keep their relative order; fold
        // sets slot in at their earliest member's original atom id (stable,
        // deterministic; mirrors the retired mux-merge slot rule).
        struct AtomSlot
        {
            uint32_t sortKey;
            uint32_t oldAtom;
            uint32_t root;
        };
        std::vector<AtomSlot> slots;
        slots.reserve(originalAtomCount);
        for (uint32_t atom = 0; atom < originalAtomCount; ++atom)
        {
            if (context.atomIsCommit[atom] != 0 || context.atomInstructions[atom] != 1)
            {
                slots.push_back(AtomSlot{atom, atom, kInvalidIndex});
            }
        }
        for (auto &[root, members] : setMembers)
        {
            uint32_t sortKey = kInvalidIndex;
            for (const uint32_t member : members)
            {
                sortKey = std::min(sortKey, context.instructionAtom[member]);
            }
            slots.push_back(AtomSlot{sortKey, kInvalidIndex, root});
        }
        std::sort(slots.begin(), slots.end(),
                  [](const AtomSlot &lhs, const AtomSlot &rhs) {
                      return std::tie(lhs.sortKey, lhs.oldAtom, lhs.root) <
                             std::tie(rhs.sortKey, rhs.oldAtom, rhs.root);
                  });

        const CsrGraph instructionGraph =
            detail::buildInstructionGraph(instructionCount, defUse,
                                          context.orderedEdges);

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
        std::vector<uint32_t> newAtomSignatures(newAtomCount, kInvalidAtomSignature);

        std::size_t treeAtomCount = 0;
        std::size_t muxRootedAtomCount = 0;
        std::size_t foldedInstructions = 0;
        std::size_t maxTreeInstructions = 0;

        for (uint32_t slotIndex = 0; slotIndex < newAtomCount; ++slotIndex)
        {
            const AtomSlot &slot = slots[slotIndex];
            newAtomMemberOffsets[slotIndex] =
                static_cast<uint32_t>(newAtomMembers.size());
            if (slot.oldAtom != kInvalidIndex)
            {
                // Carry-over atom (commit or comb-loop SCC): keep the original
                // member list and all per-atom properties verbatim, except the
                // signature bookkeeping (no-select marker for comb loops).
                const uint32_t oldAtom = slot.oldAtom;
                for (uint32_t offset = context.atomMemberOffsets[oldAtom];
                     offset < context.atomMemberOffsets[oldAtom + 1]; ++offset)
                {
                    const uint32_t instruction = context.atomMembers[offset];
                    newInstructionAtom[instruction] = slotIndex;
                    newAtomMembers.push_back(instruction);
                }
                newAtomInstructions[slotIndex] = context.atomInstructions[oldAtom];
                newAtomStateWrites[slotIndex] = context.atomStateWrites[oldAtom];
                newAtomIsCommit[slotIndex] = context.atomIsCommit[oldAtom];
                newAtomMinInstruction[slotIndex] = context.atomMinInstruction[oldAtom];
                newCommitEventRank[slotIndex] = context.commitEventRank[oldAtom];
                newAtomKinds[slotIndex] = context.atomKinds[oldAtom];
                newAtomSignatures[slotIndex] =
                    context.atomIsCommit[oldAtom] != 0
                        ? context.atomSignatures[oldAtom]
                        : kInvalidAtomSignature;
                continue;
            }

            // Fold set: members in internal def-use order; the root (the one
            // member that did not fold) is the unique sink and orders last.
            const std::vector<uint32_t> &members = setMembers[slot.root];
            const std::vector<uint32_t> ordered = topoOrderSubset(program, defUse, members);
            uint32_t minInstruction = kInvalidIndex;
            for (const uint32_t instruction : ordered)
            {
                newInstructionAtom[instruction] = slotIndex;
                newAtomMembers.push_back(instruction);
                minInstruction = std::min(minInstruction, instruction);
            }
            if (ordered.back() != slot.root)
            {
                diagnostics.error(
                    "AM tree-atom fold produced a set whose sink is not the fold root: "
                    "root=" +
                        std::to_string(slot.root),
                    std::string(kDiagnosticContext));
                return false;
            }
            newAtomInstructions[slotIndex] = static_cast<uint32_t>(ordered.size());
            newAtomIsCommit[slotIndex] = 0;
            newAtomMinInstruction[slotIndex] = minInstruction;
            const InstructionId rootInstruction{slot.root};
            const auto rootOperands = program.operands(rootInstruction);
            if (program.opcode(rootInstruction) == Opcode::Mux &&
                rootOperands.size() == 3 && rootOperands[0].valid())
            {
                newAtomSignatures[slotIndex] = rootOperands[0].value;
                ++muxRootedAtomCount;
            }
            if (ordered.size() > 1)
            {
                newAtomKinds[slotIndex] = static_cast<uint8_t>(AmAtomKind::Tree);
                ++treeAtomCount;
                foldedInstructions += ordered.size() - 1;
                maxTreeInstructions = std::max(maxTreeInstructions, ordered.size());
            }
            else
            {
                newAtomKinds[slotIndex] = static_cast<uint8_t>(AmAtomKind::Singleton);
            }
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
                                           context.instructionAtom, newAtomCount);

        // DAG safety net: contracting def-use edges of a DAG cannot create a
        // cycle, so a multi-atom SCC here means the fold rule was broken.
        const detail::SccResult scc =
            detail::findStronglyConnectedComponents(context.atomGraph);
        if (static_cast<uint32_t>(scc.count) != newAtomCount)
        {
            diagnostics.error("AM tree-atom fold produced a cyclic atom graph",
                              std::string(kDiagnosticContext));
            return false;
        }

        // Oversized statistics follow the same per-class limits as the split
        // stage (oversized atoms intentionally occupy one oversized block
        // each, the gsim oversized-super-node analogue).
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

        // Rebuild the compute/commit induced subgraphs on the folded atom DAG.
        std::string blockError;
        std::optional<AmGraphSplit> split =
            splitAmGraph(context.partitionInput(), blockError);
        if (!split)
        {
            diagnostics.error("AM tree-atom fold produced an unsplittable graph: " +
                                  blockError,
                              std::string(kDiagnosticContext));
            return false;
        }
        context.split = std::move(*split);

        diagnostics.info("AM tree-atom fold: atoms=" + std::to_string(newAtomCount) +
                             " tree_atoms=" + std::to_string(treeAtomCount) +
                             " mux_rooted_atoms=" +
                             std::to_string(muxRootedAtomCount) +
                             " folded_instructions=" +
                             std::to_string(foldedInstructions) +
                             " max_tree_instructions=" +
                             std::to_string(maxTreeInstructions),
                         std::string(kDiagnosticContext));
        return true;
    }

} // namespace wolvrix::lib::grhsim::am
