#include "grhsim/am/grhsim_am_graph_split.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    using namespace detail;

    namespace
    {
        // Research export switch (topo-partition-proj harness): when
        // WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL names a path, schedule()
        // dumps the pre-scheduling instruction graph (def-use + ordered-effect
        // edges plus the comb-loop SCC packing) as JSONL before block
        // formation, reusing the exact graph/SCC built above.
        constexpr char kInstructionGraphExportEnv[] =
            "WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL";
        constexpr std::string_view kInstructionGraphFormat =
            "wolvrix.am-instruction-graph.v1";

        // Iterates unique (variable, using instruction) reads: one def-use
        // edge per (defining instruction, using instruction) pair, plus
        // source-less reads of variables with no defining instruction (state
        // targets, interface inputs). Uses are instruction-ordered, so
        // duplicate reads of one variable by one instruction are adjacent.
        template <typename Callback>
        void forEachExportedValueRead(const DefUseIndex &defUse, Callback &&callback)
        {
            for (uint32_t variable = 0; variable < defUse.definitions.size(); ++variable) {
                const uint32_t definition = defUse.definitions[variable];
                uint32_t previousTarget = kInvalidIndex;
                for (uint32_t offset = defUse.useOffsets[variable];
                     offset < defUse.useOffsets[variable + 1]; ++offset) {
                    const uint32_t target = defUse.uses[offset];
                    if (target == previousTarget) {
                        continue;
                    }
                    previousTarget = target;
                    if (definition == kInvalidIndex) {
                        callback(variable, kInvalidIndex, target);
                    } else if (definition != target) {
                        callback(variable, definition, target);
                    }
                }
            }
        }

        bool exportInstructionGraphJsonl(ProgramView program, const DefUseIndex &defUse,
                                         std::span<const OrderEdge> orderedEdges,
                                         const SccResult &scc,
                                         const std::filesystem::path &path,
                                         wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const uint32_t instructionCount =
                static_cast<uint32_t>(program.instructionCount());
            std::vector<uint32_t> atomSizes(scc.count, 0);
            for (uint32_t index = 0; index < instructionCount; ++index) {
                ++atomSizes[scc.component[index]];
            }
            uint32_t combLoopAtomCount = 0;
            for (const uint32_t size : atomSizes) {
                combLoopAtomCount += size > 1 ? 1U : 0U;
            }

            uint64_t defUseEdgeCount = 0;
            uint64_t externalReadCount = 0;
            forEachExportedValueRead(defUse, [&](uint32_t, uint32_t source, uint32_t) {
                if (source == kInvalidIndex) {
                    ++externalReadCount;
                } else {
                    ++defUseEdgeCount;
                }
            });
            uint64_t orderEdgeCount = 0;
            for (const OrderEdge &edge : orderedEdges) {
                orderEdgeCount += edge.source != edge.target ? 1U : 0U;
            }

            if (path.has_parent_path()) {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) {
                    diagnostics.error("failed to create AM instruction graph export directory: " +
                                          path.parent_path().string(),
                                      std::string(kDiagnosticContext));
                    return false;
                }
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                diagnostics.error("failed to open AM instruction graph export path: " +
                                      path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }

            JsonlGraphWriter writer(output);
            writer.raw("{\"record\":\"header\",\"format\":\"")
                .raw(kInstructionGraphFormat)
                .raw("\",\"instructions\":")
                .number(instructionCount)
                .raw(",\"variables\":")
                .number(static_cast<uint64_t>(program.variableCount()))
                .raw(",\"atoms\":")
                .number(scc.count)
                .raw(",\"comb_loop_atoms\":")
                .number(combLoopAtomCount)
                .raw(",\"def_use_edges\":")
                .number(defUseEdgeCount)
                .raw(",\"external_reads\":")
                .number(externalReadCount)
                .raw(",\"order_edges\":")
                .number(orderEdgeCount)
                .raw("}");
            writer.endLine();

            for (uint32_t index = 0; index < instructionCount; ++index) {
                const InstructionId instruction{index};
                uint64_t width = 0;
                for (const VariableId result : program.results(instruction)) {
                    width += exportedVariableWidth(program, result);
                }
                const uint32_t atom = scc.component[index];
                writer.raw("{\"record\":\"node\",\"id\":")
                    .number(index)
                    .raw(",\"op\":")
                    .number(static_cast<uint8_t>(program.opcode(instruction)))
                    .raw(",\"opcode\":\"")
                    .raw(toString(program.opcode(instruction)))
                    .raw("\",\"width\":")
                    .number(width)
                    .raw(",\"state_write\":")
                    .boolean(stateWriteTarget(program, instruction).has_value())
                    .raw(",\"atom\":")
                    .number(atom)
                    .raw(",\"comb_loop_atom\":")
                    .boolean(atomSizes[atom] > 1)
                    .raw("}");
                writer.endLine();
            }

            forEachExportedValueRead(
                defUse, [&](uint32_t variable, uint32_t source, uint32_t target) {
                    const uint64_t width =
                        exportedVariableWidth(program, VariableId{variable});
                    if (source == kInvalidIndex) {
                        writer.raw("{\"record\":\"edge\",\"kind\":\"external_read\",\"dst\":")
                            .number(target)
                            .raw(",\"var\":")
                            .number(variable)
                            .raw(",\"width\":")
                            .number(width)
                            .raw("}");
                    } else {
                        writer.raw("{\"record\":\"edge\",\"kind\":\"def_use\",\"src\":")
                            .number(source)
                            .raw(",\"dst\":")
                            .number(target)
                            .raw(",\"var\":")
                            .number(variable)
                            .raw(",\"width\":")
                            .number(width)
                            .raw("}");
                    }
                    writer.endLine();
                });

            for (const OrderEdge &edge : orderedEdges) {
                if (edge.source == edge.target) {
                    continue;
                }
                writer.raw("{\"record\":\"edge\",\"kind\":\"order\",\"src\":")
                    .number(edge.source)
                    .raw(",\"dst\":")
                    .number(edge.target)
                    .raw("}");
                writer.endLine();
            }

            if (!writer.flush()) {
                diagnostics.error("failed while writing AM instruction graph export: " +
                                      path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }
            diagnostics.info("exported AM instruction graph: path=" + path.string() +
                                 " instructions=" + std::to_string(instructionCount) +
                                 " def_use_edges=" + std::to_string(defUseEdgeCount) +
                                 " external_reads=" + std::to_string(externalReadCount) +
                                 " order_edges=" + std::to_string(orderEdgeCount) +
                                 " comb_loop_atoms=" + std::to_string(combLoopAtomCount),
                             std::string(kDiagnosticContext));
            return true;
        }
    } // namespace

    AmGraphPartitionInput AmGraphSplitContext::partitionInput() const
    {
        return AmGraphPartitionInput{
            .atomCount = atomCount,
            .atomOffsets = atomGraph.offsets,
            .atomTargets = atomGraph.targets,
            .atomInstructions = atomInstructions,
            .atomStateWrites = atomStateWrites,
            .atomIsCommit = atomIsCommit,
            .atomMinInstruction = atomMinInstruction,
            .commitEventRank = commitEventRank,
            .variableCount = variableCount,
            .definitions = defUse.definitions,
            .useOffsets = defUse.useOffsets,
            .uses = defUse.uses,
            .instructionAtom = instructionAtom,
            .maxInstructionsPerBlock = maxInstructionsPerBlock,
            .maxCommitInstructionsPerBlock = maxCommitInstructionsPerBlock,
            .enableCoarsening = enableCoarsening,
            .coarsenBudget = coarsenBudget,
            .segmentPenalty = segmentPenalty,
            .variableCopyWeights = variableCopyWeights,
        };
    }

    std::optional<AmGraphSplitContext>
    splitAmGraphStage(AmGraph &graph,
                      const ActivityScheduleOptions &options,
                      wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        // The graph IS the working IR here: it arrives straight from
        // lowering/optimize and every analysis below reads its storage. The
        // linear program is only materialized at finalize time.
        const ProgramView program = graph.program();
        const uint32_t instructionCount = static_cast<uint32_t>(program.instructionCount());
        const uint32_t variableCount = static_cast<uint32_t>(program.variableCount());
        DefUseIndex defUse = buildDefUseIndex(program);
        std::vector<OrderEdge> orderedEdges;
        orderedEdges.reserve(graph.orderedEffects().size());
        std::vector<uint8_t> hasExplicitOrder(instructionCount, 0);
        std::vector<uint32_t> explicitOrderGroup(instructionCount, kInvalidIndex);
        uint32_t previousGroup = 0;
        uint32_t previousInstruction = kInvalidIndex;
        bool havePrevious = false;
        for (const OrderedEffect &effect : graph.orderedEffects()) {
            const uint32_t instruction = effect.instruction.value;
            if (hasExplicitOrder[instruction] &&
                explicitOrderGroup[instruction] != effect.group) {
                diagnostics.error(
                    "AM instruction appears in multiple explicit ordered-effect groups: "
                    "instruction=" +
                        std::to_string(instruction) +
                        " first_group=" + std::to_string(explicitOrderGroup[instruction]) +
                        " second_group=" + std::to_string(effect.group),
                    std::string(kDiagnosticContext));
                return std::nullopt;
            }
            hasExplicitOrder[instruction] = 1;
            explicitOrderGroup[instruction] = effect.group;
            if (havePrevious && effect.group == previousGroup) {
                orderedEdges.push_back(OrderEdge{
                    .source = previousInstruction,
                    .target = instruction,
                });
            }
            previousGroup = effect.group;
            previousInstruction = instruction;
            havePrevious = true;
        }

        uint32_t previousImplicitEffect = kInvalidIndex;
        for (uint32_t index = 0; index < instructionCount; ++index) {
            if (opcodeTraits(program.opcode(InstructionId{index})).hasOrderedEffect &&
                !hasExplicitOrder[index]) {
                if (previousImplicitEffect != kInvalidIndex) {
                    orderedEdges.push_back(OrderEdge{
                        .source = previousImplicitEffect,
                        .target = index,
                    });
                }
                previousImplicitEffect = index;
            }
        }

        std::vector<uint32_t> firstStateWriter(variableCount, kInvalidIndex);
        for (uint32_t index = 0; index < instructionCount; ++index) {
            const std::optional<VariableId> target =
                stateWriteTarget(program, InstructionId{index});
            if (!target) {
                continue;
            }
            if (defUse.definitions[target->value] != kInvalidIndex) {
                diagnostics.error(
                    "state-write target also has a normal instruction definition: target=" +
                        std::to_string(target->value),
                    std::string(kDiagnosticContext));
                return std::nullopt;
            }
            const uint32_t first = firstStateWriter[target->value];
            if (first == kInvalidIndex) {
                firstStateWriter[target->value] = index;
                continue;
            }
            const uint32_t firstGroup = explicitOrderGroup[first];
            const uint32_t currentGroup = explicitOrderGroup[index];
            if ((firstGroup == kInvalidIndex) != (currentGroup == kInvalidIndex) ||
                (firstGroup != kInvalidIndex && firstGroup != currentGroup)) {
                const auto targetLabelId = program.variableLabel(*target);
                const std::string targetLabel = targetLabelId
                                                    ? std::string(program.string(*targetLabelId))
                                                    : std::string("<unlabeled>");
                const auto groupText = [](uint32_t group) {
                    return group == kInvalidIndex ? std::string("implicit")
                                                  : std::to_string(group);
                };
                diagnostics.error(
                    "multiple writes to one state target require one complete ordered-effect "
                    "group or must all remain implicit: target=" +
                        std::to_string(target->value) + " label=" + targetLabel +
                        " first_instruction=" + std::to_string(first) +
                        " first_opcode=" + std::string(toString(program.opcode(InstructionId{first}))) +
                        " first_group=" + groupText(firstGroup) +
                        " current_instruction=" + std::to_string(index) +
                        " current_opcode=" + std::string(toString(program.opcode(InstructionId{index}))) +
                        " current_group=" + groupText(currentGroup),
                    std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }

        const CsrGraph instructionGraph =
            buildInstructionGraph(instructionCount, defUse, orderedEdges);
        const SccResult scc = findStronglyConnectedComponents(instructionGraph);
        const uint32_t atomCount = scc.count;
        std::vector<uint32_t> instructionAtom(instructionCount, kInvalidIndex);
        for (uint32_t index = 0; index < instructionCount; ++index) {
            instructionAtom[index] = scc.component[index];
        }

        if (const char *exportPath = std::getenv(kInstructionGraphExportEnv)) {
            if (exportPath[0] == '\0' ||
                !exportInstructionGraphJsonl(program, defUse, orderedEdges, scc,
                                             std::filesystem::path(exportPath), diagnostics)) {
                diagnostics.error("AM instruction graph export failed",
                                  std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }

        std::vector<uint32_t> atomMemberOffsets(atomCount + 1, 0);
        for (uint32_t atom : instructionAtom) {
            ++atomMemberOffsets[atom + 1];
        }
        std::partial_sum(atomMemberOffsets.begin(), atomMemberOffsets.end(),
                         atomMemberOffsets.begin());
        std::vector<uint32_t> atomMembers(instructionCount, 0);
        std::vector<uint32_t> atomCursor(atomMemberOffsets.begin(), atomMemberOffsets.end() - 1);
        for (uint32_t instruction = 0; instruction < instructionCount; ++instruction) {
            atomMembers[atomCursor[instructionAtom[instruction]]++] = instruction;
        }
        std::vector<uint32_t> localIndex(instructionCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            std::span<uint32_t> members(atomMembers.data() + atomMemberOffsets[atom],
                                        atomMemberOffsets[atom + 1] - atomMemberOffsets[atom]);
            if (!orderAtomInstructions(members, program, defUse, orderedEdges,
                                       graph.instructionEffects(), localIndex,
                                       diagnostics)) {
                return std::nullopt;
            }
        }

        CsrGraph atomGraph =
            buildCondensationGraph(instructionGraph, scc.component, atomCount);
        enum class BlockClass : uint8_t
        {
            Compute = 0,
            Commit = 1,
        };
        struct AtomCost
        {
            std::size_t instructions = 0;
            std::size_t stateWrites = 0;
            BlockClass blockClass = BlockClass::Compute;
            CommitAtomEventKey commitEvents;
        };
        std::vector<AtomCost> atomCosts(atomCount);
        std::size_t oversizedAtomCount = 0;
        uint32_t firstOversizedAtom = kInvalidIndex;
        std::size_t maxAtomInstructions = 0;
        std::size_t maxAtomStateWrites = 0;
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            AtomCost &cost = atomCosts[atom];
            cost.instructions = atomMemberOffsets[atom + 1] - atomMemberOffsets[atom];
            bool hasCommit = false;
            bool hasCompute = false;
            for (uint32_t offset = atomMemberOffsets[atom]; offset < atomMemberOffsets[atom + 1];
                 ++offset) {
                const uint32_t instruction = atomMembers[offset];
                if (stateWriteTarget(program, InstructionId{instruction})) {
                    cost.commitEvents.push_back(commitInstructionEventKey(
                        program, defUse, InstructionId{instruction}));
                    ++cost.stateWrites;
                    hasCommit = true;
                    continue;
                }
                hasCompute = true;
            }
            if (hasCommit && hasCompute) {
                diagnostics.error(
                    "AM scheduling atom mixes state commit and pre-commit instructions",
                    std::string(kDiagnosticContext));
                return std::nullopt;
            }
            if (hasCommit) {
                cost.blockClass = BlockClass::Commit;
            }
            std::sort(cost.commitEvents.begin(), cost.commitEvents.end());
            cost.commitEvents.erase(
                std::unique(cost.commitEvents.begin(), cost.commitEvents.end()),
                cost.commitEvents.end());
            maxAtomInstructions = std::max(maxAtomInstructions, cost.instructions);
            maxAtomStateWrites = std::max(maxAtomStateWrites, cost.stateWrites);
            const std::size_t instructionLimit =
                cost.blockClass == BlockClass::Commit ? options.maxCommitInstructionsPerBlock
                                                       : options.maxInstructionsPerBlock;
            if (cost.instructions > instructionLimit) {
                if (firstOversizedAtom == kInvalidIndex) {
                    firstOversizedAtom = atom;
                }
                ++oversizedAtomCount;
            }
        }
        if (oversizedAtomCount != 0) {
            const AtomCost &firstCost = atomCosts[firstOversizedAtom];
            diagnostics.warning(
                "indivisible AM scheduling atoms exceed configured block limits and will "
                "each remain in one oversized block: count=" +
                    std::to_string(oversizedAtomCount) +
                    " first_atom=" + std::to_string(firstOversizedAtom) +
                    " first_instructions=" + std::to_string(firstCost.instructions) +
                    " first_state_writes=" + std::to_string(firstCost.stateWrites) +
                    " max_atom_instructions=" + std::to_string(maxAtomInstructions) +
                    " max_atom_state_writes=" + std::to_string(maxAtomStateWrites),
                std::string(kDiagnosticContext));
        }

        std::vector<uint32_t> atomMinInstruction(atomCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            atomMinInstruction[atom] = atomMembers[atomMemberOffsets[atom]];
        }

        std::map<CommitAtomEventKey, uint32_t> eventRanks;
        std::vector<uint32_t> commitEventRank(atomCount, 0);
        std::vector<uint32_t> atomsByInstruction(atomCount);
        std::iota(atomsByInstruction.begin(), atomsByInstruction.end(), uint32_t{0});
        std::sort(atomsByInstruction.begin(), atomsByInstruction.end(),
                  [&](uint32_t lhs, uint32_t rhs) {
                      return std::tie(atomMinInstruction[lhs], lhs) <
                             std::tie(atomMinInstruction[rhs], rhs);
                  });
        for (uint32_t atom : atomsByInstruction) {
            const AtomCost &cost = atomCosts[atom];
            if (cost.blockClass != BlockClass::Commit) {
                continue;
            }
            const auto [eventIt, inserted] = eventRanks.try_emplace(
                cost.commitEvents, static_cast<uint32_t>(eventRanks.size()));
            (void)inserted;
            commitEventRank[atom] = eventIt->second;
        }

        std::vector<uint32_t> atomInstructions(atomCount, 0);
        std::vector<uint32_t> atomStateWrites(atomCount, 0);
        std::vector<uint8_t> atomIsCommit(atomCount, 0);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            atomInstructions[atom] = static_cast<uint32_t>(atomCosts[atom].instructions);
            atomStateWrites[atom] = static_cast<uint32_t>(atomCosts[atom].stateWrites);
            atomIsCommit[atom] = atomCosts[atom].blockClass == BlockClass::Commit ? 1 : 0;
        }
        // Optional width-folded DP copy weights (docs/10: the runtime copy
        // count formula, ceil(bitWidth/64) per incoming variable).
        std::vector<uint32_t> variableCopyWeights;
        if (options.dpWidthWeightedCopyCost) {
            variableCopyWeights.reserve(variableCount);
            for (uint32_t variable = 0; variable < variableCount; ++variable) {
                variableCopyWeights.push_back(static_cast<uint32_t>(std::max<uint64_t>(
                    1, (exportedVariableWidth(program, VariableId{variable}) + 63) / 64)));
            }
        }

        AmGraphSplitContext context;
        context.instructionCount = instructionCount;
        context.variableCount = variableCount;
        context.atomCount = atomCount;
        context.defUse = std::move(defUse);
        context.orderedEdges = std::move(orderedEdges);
        context.atomGraph = std::move(atomGraph);
        context.instructionAtom = std::move(instructionAtom);
        context.atomMemberOffsets = std::move(atomMemberOffsets);
        context.atomMembers = std::move(atomMembers);
        context.atomInstructions = std::move(atomInstructions);
        context.atomStateWrites = std::move(atomStateWrites);
        context.atomIsCommit = std::move(atomIsCommit);
        context.atomMinInstruction = std::move(atomMinInstruction);
        context.commitEventRank = std::move(commitEventRank);
        context.variableCopyWeights = std::move(variableCopyWeights);
        context.oversizedAtomCount = oversizedAtomCount;
        context.maxAtomInstructions = maxAtomInstructions;
        context.maxAtomStateWrites = maxAtomStateWrites;
        context.maxInstructionsPerBlock = options.maxInstructionsPerBlock;
        context.maxCommitInstructionsPerBlock = options.maxCommitInstructionsPerBlock;
        context.enableCoarsening = options.enableCoarsening;
        // Auto budget: 1.5x the per-block instruction cap. The legacy
        // 32x cap runs AM's single-instruction-atom DAG to full coarsen
        // convergence, which pushes clusters into (cap, 32x cap] as
        // DP-indivisible oversized singletons (~9.4k blocks on XiangShan,
        // avg ~470 instructions/block). 1.5x keeps clusters at cap size
        // so the segment DP's maxInstructionsPerBlock actually binds,
        // landing XiangShan at ~33.7k compute blocks (legacy: 31.5k)
        // and measurably faster host time than the old default.
        context.coarsenBudget = options.dpCoarsenBudget != 0
                                    ? options.dpCoarsenBudget
                                    : (3 * options.maxInstructionsPerBlock) / 2;
        context.segmentPenalty = options.dpSegmentPenalty;

        // ---- stage: split-am-graph --------------------------------------
        // The atom DAG is decomposed into the GRHSIM AM Compute Graph and
        // the GRHSIM AM Commit Graph (induced subgraphs; the commit->compute
        // direction is rejected inside the split).
        std::string blockError;
        auto graphSplit = splitAmGraph(context.partitionInput(), blockError);
        if (!graphSplit) {
            diagnostics.error(std::move(blockError), std::string(kDiagnosticContext));
            return std::nullopt;
        }
        context.split = std::move(*graphSplit);
        return context;
    }

} // namespace wolvrix::lib::grhsim::am
