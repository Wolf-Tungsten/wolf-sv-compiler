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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    using namespace detail;

    namespace
    {
        // Research export switch (topo-partition-proj harness): when
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

    } // namespace

    // Research export of the pre-scheduling instruction graph (def-use +
    // ordered-effect edges plus the atom packing). Atom fields come from
    // the split context, so the export reflects the tree-atom fold pass
    // (the orchestrator invokes it after that pass).
    bool exportInstructionGraphJsonl(ProgramView program,
                                     const AmGraphSplitContext &context,
                                     const std::filesystem::path &path,
                                     wolvrix::lib::diag::Diagnostics &diagnostics)
    {
            const DefUseIndex &defUse = context.defUse;
            const std::vector<OrderEdge> &orderedEdges = context.orderedEdges;
            const uint32_t instructionCount =
                static_cast<uint32_t>(program.instructionCount());
            uint32_t combLoopAtomCount = 0;
            for (uint32_t atom = 0; atom < context.atomCount; ++atom) {
                combLoopAtomCount +=
                    context.atomKinds[atom] ==
                            static_cast<uint8_t>(AmAtomKind::CombLoopScc)
                        ? 1U
                        : 0U;
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
                .number(context.atomCount)
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
                const uint32_t atom = context.instructionAtom[index];
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
                    .boolean(context.atomKinds[atom] ==
                             static_cast<uint8_t>(AmAtomKind::CombLoopScc))
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

    namespace
    {
        // Third research export: the split-am-graph stage product itself.
        // WOLVRIX_GRHSIM_AM_SPLIT_GRAPH_JSONL=<prefix> writes two files,
        // <prefix>.compute.jsonl (the GRHSIM AM Compute Graph) and
        // <prefix>.commit.jsonl (the GRHSIM AM Commit Graph), taken directly
        // from the split context -- node membership, induced edges and atom
        // annotations are exactly what the partition passes see.
        constexpr char kSplitGraphExportEnv[] = "WOLVRIX_GRHSIM_AM_SPLIT_GRAPH_JSONL";
        constexpr std::string_view kSplitGraphFormat = "wolvrix.am-split-graph.v1";

        bool exportSplitSideJsonl(ProgramView program, const AmGraphSplitContext &context,
                                  bool commitSide, const std::filesystem::path &path,
                                  wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const auto inSide = [&](uint32_t instruction) {
                const uint32_t atom = context.instructionAtom[instruction];
                return (context.atomIsCommit[atom] != 0) == commitSide;
            };
            const auto localAtomOf = [&](uint32_t instruction) {
                const uint32_t atom = context.instructionAtom[instruction];
                return commitSide ? context.split.commitGraph.localOfAtom[atom]
                                  : context.split.computeGraph.localOfAtom[atom];
            };
            const uint32_t instructionCount = context.instructionCount;

            uint32_t sideInstructions = 0;
            uint64_t defUseEdgeCount = 0;
            uint64_t externalReadCount = 0;
            forEachExportedValueRead(
                context.defUse, [&](uint32_t, uint32_t source, uint32_t target) {
                    if (!inSide(target)) {
                        return;
                    }
                    if (source != kInvalidIndex && inSide(source)) {
                        ++defUseEdgeCount;
                        return;
                    }
                    // Boundary inputs: compute side sees only truly source-less
                    // reads (state targets, interface inputs); the commit side
                    // additionally counts reads defined on the compute side --
                    // its inputs are exactly those compute-produced operands.
                    if (source == kInvalidIndex || commitSide) {
                        ++externalReadCount;
                    }
                });
            uint64_t orderEdgeCount = 0;
            for (const OrderEdge &edge : context.orderedEdges) {
                if (edge.source != edge.target && inSide(edge.source) && inSide(edge.target)) {
                    ++orderEdgeCount;
                }
            }
            uint32_t combLoopAtomCount = 0;
            for (uint32_t atom = 0; atom < context.atomCount; ++atom) {
                if ((context.atomIsCommit[atom] != 0) == commitSide &&
                    context.atomMemberOffsets[atom + 1] - context.atomMemberOffsets[atom] > 1) {
                    ++combLoopAtomCount;
                }
            }
            const uint32_t sideAtoms =
                commitSide ? context.split.commitGraph.atomCount
                           : context.split.computeGraph.atomCount;
            for (uint32_t index = 0; index < instructionCount; ++index) {
                sideInstructions += inSide(index) ? 1U : 0U;
            }

            if (path.has_parent_path()) {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) {
                    diagnostics.error("failed to create AM split graph export directory: " +
                                          path.parent_path().string(),
                                      std::string(kDiagnosticContext));
                    return false;
                }
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                diagnostics.error("failed to open AM split graph export path: " + path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }

            JsonlGraphWriter writer(output);
            writer.raw("{\\\"record\\\":\\\"header\\\",\\\"format\\\":\\\"")
                .raw(kSplitGraphFormat)
                .raw("\\\",\\\"side\\\":\\\"")
                .raw(commitSide ? std::string_view("commit") : std::string_view("compute"))
                .raw("\\\",\\\"instructions\\\":")
                .number(sideInstructions)
                .raw(",\\\"variables\\\":")
                .number(static_cast<uint64_t>(program.variableCount()))
                .raw(",\\\"atoms\\\":")
                .number(sideAtoms)
                .raw(",\\\"comb_loop_atoms\\\":")
                .number(combLoopAtomCount)
                .raw(",\\\"def_use_edges\\\":")
                .number(defUseEdgeCount)
                .raw(",\\\"external_reads\\\":")
                .number(externalReadCount)
                .raw(",\\\"order_edges\\\":")
                .number(orderEdgeCount)
                .raw("}");
            writer.endLine();

            for (uint32_t index = 0; index < instructionCount; ++index) {
                if (!inSide(index)) {
                    continue;
                }
                const InstructionId instruction{index};
                uint64_t width = 0;
                for (const VariableId result : program.results(instruction)) {
                    width += exportedVariableWidth(program, result);
                }
                const uint32_t atom = context.instructionAtom[index];
                writer.raw("{\\\"record\\\":\\\"node\\\",\\\"id\\\":")
                    .number(index)
                    .raw(",\\\"op\\\":")
                    .number(static_cast<uint8_t>(program.opcode(instruction)))
                    .raw(",\\\"opcode\\\":\\\"")
                    .raw(toString(program.opcode(instruction)))
                    .raw("\\\",\\\"width\\\":")
                    .number(width)
                    .raw(",\\\"state_write\\\":")
                    .boolean(stateWriteTarget(program, instruction).has_value())
                    .raw(",\\\"atom\\\":")
                    .number(localAtomOf(index))
                    .raw(",\\\"min_instruction\\\":")
                    .number(context.atomMinInstruction[atom])
                    .raw(",\\\"comb_loop_atom\\\":")
                    .boolean(context.atomMemberOffsets[atom + 1] -
                                 context.atomMemberOffsets[atom] >
                             1);
                for (const VariableId result : program.results(instruction)) {
                    if (const std::optional<StringId> label = program.variableLabel(result)) {
                        writer.raw(",\\\"name\\\":\\\"");
                        // Minimal JSON escaping for quotes and backslashes.
                        for (const char ch : program.string(*label)) {
                            if (ch == '\\' || ch == '"') {
                                writer.raw("\\");
                            }
                            writer.raw(std::string_view(&ch, 1));
                        }
                        writer.raw("\\\"");
                        break;
                    }
                }
                if (commitSide) {
                    writer.raw(",\\\"event_rank\\\":").number(context.commitEventRank[atom]);
                }
                writer.raw("}");
                writer.endLine();
            }

            forEachExportedValueRead(
                context.defUse,
                [&](uint32_t variable, uint32_t source, uint32_t target) {
                    if (!inSide(target)) {
                        return;
                    }
                    const uint64_t width =
                        exportedVariableWidth(program, VariableId{variable});
                    if (source != kInvalidIndex && inSide(source)) {
                        writer.raw("{\\\"record\\\":\\\"edge\\\",\\\"kind\\\":\\\"def_use\\\",\\\"src\\\":")
                            .number(source)
                            .raw(",\\\"dst\\\":")
                            .number(target)
                            .raw(",\\\"var\\\":")
                            .number(variable)
                            .raw(",\\\"width\\\":")
                            .number(width);
                        // Operand slot of the read inside the target
                        // instruction (first match); -1 when not found.
                        const auto targetOperands =
                            program.operands(InstructionId{target});
                        int64_t operandSlot = -1;
                        for (std::size_t slot = 0; slot < targetOperands.size(); ++slot) {
                            if (targetOperands[slot].value == variable) {
                                operandSlot = static_cast<int64_t>(slot);
                                break;
                            }
                        }
                        writer.raw(",\\\"operand\\\":").number(operandSlot).raw("}");
                        writer.endLine();
                        return;
                    }
                    if (source == kInvalidIndex || commitSide) {
                        writer.raw("{\\\"record\\\":\\\"edge\\\",\\\"kind\\\":\\\"external_read\\\",\\\"dst\\\":")
                            .number(target)
                            .raw(",\\\"var\\\":")
                            .number(variable)
                            .raw(",\\\"width\\\":")
                            .number(width);
                        if (source != kInvalidIndex) {
                            writer.raw(",\\\"src_side\\\":\\\"compute\\\",\\\"src\\\":").number(source);
                        }
                        writer.raw("}");
                        writer.endLine();
                    }
                });

            for (const OrderEdge &edge : context.orderedEdges) {
                if (edge.source == edge.target || !inSide(edge.source) || !inSide(edge.target)) {
                    continue;
                }
                writer.raw("{\\\"record\\\":\\\"edge\\\",\\\"kind\\\":\\\"order\\\",\\\"src\\\":")
                    .number(edge.source)
                    .raw(",\\\"dst\\\":")
                    .number(edge.target)
                    .raw("}");
                writer.endLine();
            }

            if (!writer.flush()) {
                diagnostics.error("failed while writing AM split graph export: " + path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }
            diagnostics.info(
                std::string("exported AM split graph (") + (commitSide ? "commit" : "compute") +
                    "): path=" + path.string() + " instructions=" +
                    std::to_string(sideInstructions) + " atoms=" + std::to_string(sideAtoms) +
                    " def_use_edges=" + std::to_string(defUseEdgeCount) +
                    " external_reads=" + std::to_string(externalReadCount) +
                    " order_edges=" + std::to_string(orderEdgeCount),
                std::string(kDiagnosticContext));
            return true;
        }

        // NO0006 bijection audit: per-atom JSONL dump of the node-aligned
        // atomization. One line per atom carrying its gsim node id (-1 for
        // unowned clock-domain helpers), kind, member count and multi-sink
        // flag. Written at the end of splitAmGraphStage, where the atom
        // tables are final on the node-aligned path (no atom-rewriting pass
        // runs afterwards: tree-atom fold and fanout absorb are skipped).
        constexpr char kNodeAtomAuditEnv[] = "WOLVRIX_GRHSIM_AM_NODE_ATOM_AUDIT_JSONL";

        std::string_view atomKindName(uint8_t kind)
        {
            switch (static_cast<AmAtomKind>(kind))
            {
            case AmAtomKind::Singleton:
                return "Singleton";
            case AmAtomKind::Tree:
                return "Tree";
            case AmAtomKind::CombLoopScc:
                return "CombLoopScc";
            case AmAtomKind::CommitEvent:
                return "CommitEvent";
            }
            return "Unknown";
        }

        bool exportNodeAtomAuditJsonl(const AmGraphSplitContext &context,
                                      const std::vector<int64_t> &atomGsimNodeId,
                                      const std::vector<uint8_t> &atomMultiSink,
                                      const std::filesystem::path &path,
                                      wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            if (path.has_parent_path()) {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) {
                    diagnostics.error("failed to create AM node-atom audit directory: " +
                                          path.parent_path().string(),
                                      std::string(kDiagnosticContext));
                    return false;
                }
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                diagnostics.error("failed to open AM node-atom audit path: " + path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }

            JsonlGraphWriter writer(output);
            for (uint32_t atom = 0; atom < context.atomCount; ++atom) {
                writer.raw("{\"atom\":").number(atom).raw(",\"node_id\":");
                const int64_t nodeId = atomGsimNodeId[atom];
                if (nodeId < 0) {
                    writer.raw("-1");
                } else {
                    writer.number(static_cast<uint64_t>(nodeId));
                }
                writer.raw(",\"kind\":\"").raw(atomKindName(context.atomKinds[atom]))
                    .raw("\",\"instructions\":")
                    .number(context.atomInstructions[atom])
                    .raw(",\"multi_sink\":")
                    .number(atomMultiSink[atom] != 0 ? 1 : 0)
                    .raw("}");
                writer.endLine();
            }
            if (!writer.flush()) {
                diagnostics.error("failed while writing AM node-atom audit: " + path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }
            diagnostics.info("exported AM node-atom audit: path=" + path.string() +
                                 " atoms=" + std::to_string(context.atomCount),
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
            .atomSignatures = atomSignatures,
            .variableCount = variableCount,
            .definitions = defUse.definitions,
            .useOffsets = defUse.useOffsets,
            .uses = defUse.uses,
            .instructionAtom = instructionAtom,
            .maxAtomsPerBlock = maxAtomsPerBlock,
            .maxCommitAtomsPerBlock = maxCommitAtomsPerBlock,
            .enableCoarsening = enableCoarsening,
            .coarsenAtomBudget = coarsenAtomBudget,
            .coarsenInstructionBudget = coarsenInstructionBudget,
            .segmentPenalty = segmentPenalty,
            .refinementRounds = refinementRounds,
            .mergeWhenMinGroup = mergeWhenMinGroup,
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

        // NO0006 gsim node-aligned atomization: when the graph carries gsim
        // node provenance (mode On/Auto), atoms are the gsim node groups
        // instead of SCC packings. Commit instructions stay
        // singleton-per-instruction (today's commit behavior), compute
        // instructions with a node id group per node, and unowned compute
        // instructions (AM clock-domain helpers: changed detectors,
        // pre-commit snapshots) stay singleton. The SCC decomposition still
        // runs for cycle safety: a multi-instruction SCC spanning several
        // groups unions them into one CombLoopScc atom.
        const bool nodeAligned = gsimNodeAlignedScheduling(graph, options);
        uint32_t atomCount = 0;
        std::vector<uint32_t> instructionAtom(instructionCount, kInvalidIndex);
        // Node-aligned per-atom extras: audit node id, multi-sink flag and
        // the comb-loop marker (atom contains a multi-instruction SCC).
        std::vector<int64_t> atomGsimNodeId;
        std::vector<uint8_t> atomMultiSink;
        std::vector<uint8_t> atomCombLoop;
        std::size_t sccUnionFallbackCount = 0;
        if (!nodeAligned) {
            atomCount = scc.count;
            for (uint32_t index = 0; index < instructionCount; ++index) {
                instructionAtom[index] = scc.component[index];
            }
        } else {
            std::vector<uint32_t> groupOf(instructionCount, kInvalidIndex);
            std::vector<int64_t> groupNodeId;
            groupNodeId.reserve(instructionCount);
            std::unordered_map<int64_t, uint32_t> nodeGroup;
            for (uint32_t index = 0; index < instructionCount; ++index) {
                const InstructionId instruction{index};
                const int64_t nodeId = graph.gsimNodeId(instruction);
                if (nodeId < 0 || stateWriteTarget(program, instruction).has_value()) {
                    groupOf[index] = static_cast<uint32_t>(groupNodeId.size());
                    groupNodeId.push_back(nodeId);
                    continue;
                }
                const auto [it, inserted] = nodeGroup.try_emplace(
                    nodeId, static_cast<uint32_t>(groupNodeId.size()));
                if (inserted) {
                    groupNodeId.push_back(nodeId);
                }
                groupOf[index] = it->second;
            }

            // Cycle safety: union the groups touched by one
            // multi-instruction SCC. gsim-flatten input is acyclic per
            // node, so this is expected to fire ~never.
            const uint32_t groupCount = static_cast<uint32_t>(groupNodeId.size());
            std::vector<uint32_t> groupParent(groupCount);
            std::iota(groupParent.begin(), groupParent.end(), uint32_t{0});
            const auto findGroup = [&](uint32_t group) {
                uint32_t root = group;
                while (groupParent[root] != root) {
                    root = groupParent[root];
                }
                while (groupParent[group] != root) {
                    const uint32_t next = groupParent[group];
                    groupParent[group] = root;
                    group = next;
                }
                return root;
            };
            std::vector<uint32_t> sccSize(scc.count, 0);
            for (uint32_t index = 0; index < instructionCount; ++index) {
                ++sccSize[scc.component[index]];
            }
            std::vector<uint32_t> sccFirstGroup(scc.count, kInvalidIndex);
            std::vector<uint8_t> sccSpanning(scc.count, 0);
            for (uint32_t index = 0; index < instructionCount; ++index) {
                const uint32_t component = scc.component[index];
                if (sccSize[component] == 1) {
                    continue;
                }
                const uint32_t root = findGroup(groupOf[index]);
                if (sccFirstGroup[component] == kInvalidIndex) {
                    sccFirstGroup[component] = root;
                } else if (sccFirstGroup[component] != root) {
                    groupParent[findGroup(sccFirstGroup[component])] = root;
                    sccSpanning[component] = 1;
                }
            }
            for (uint32_t component = 0; component < scc.count; ++component) {
                sccUnionFallbackCount += sccSpanning[component] != 0 ? 1 : 0;
            }

            // Atom per surviving group root, in first-appearance order; the
            // audit node id is the first merged group's id.
            std::vector<uint32_t> atomOfGroup(groupCount, kInvalidIndex);
            atomGsimNodeId.reserve(groupCount);
            for (uint32_t group = 0; group < groupCount; ++group) {
                const uint32_t root = findGroup(group);
                if (atomOfGroup[root] == kInvalidIndex) {
                    atomOfGroup[root] = atomCount;
                    atomGsimNodeId.push_back(groupNodeId[group]);
                    ++atomCount;
                }
            }
            for (uint32_t index = 0; index < instructionCount; ++index) {
                instructionAtom[index] = atomOfGroup[findGroup(groupOf[index])];
            }
            atomCombLoop.assign(atomCount, 0);
            atomMultiSink.assign(atomCount, 0);
            for (uint32_t index = 0; index < instructionCount; ++index) {
                if (sccSize[scc.component[index]] > 1) {
                    atomCombLoop[instructionAtom[index]] = 1;
                }
            }
        }

        std::vector<uint32_t> atomMemberOffsets(atomCount + 1, 0);        for (uint32_t atom : instructionAtom) {
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

        // NO0006 node-aligned anchor ordering: the node anchor (the member
        // whose result leaves the atom — the node's output variable)
        // orders last when it is the unique external-defining member and
        // has no intra-atom successors. Groups with several
        // external-defining members (EXT multi-output nodes) keep their
        // order and are counted as multi-sink in the audit.
        if (nodeAligned) {
            std::vector<uint8_t> pinnedVariable(variableCount, 0);
            for (const PortBinding &port : graph.interface().ports) {
                if (port.input.valid()) {
                    pinnedVariable[port.input.value] = 1;
                }
                if (port.output.valid()) {
                    pinnedVariable[port.output.value] = 1;
                }
                if (port.outputEnable.valid()) {
                    pinnedVariable[port.outputEnable.value] = 1;
                }
            }
            for (const VariableLabel &label : graph.interface().declaredVariables) {
                if (label.variable.valid()) {
                    pinnedVariable[label.variable.value] = 1;
                }
            }
            for (uint32_t variable = 0; variable < variableCount; ++variable) {
                const VariableRole role = graph.valueFacts(VariableId{variable}).roles;
                if (hasRole(role, VariableRole::ExternalOutput) ||
                    hasRole(role, VariableRole::Observable)) {
                    pinnedVariable[variable] = 1;
                }
            }
            // An anchor that sources an intra-atom ordered-effect edge must
            // not move past its target.
            std::vector<uint8_t> orderConstrained(instructionCount, 0);
            for (const OrderEdge &edge : orderedEdges) {
                if (edge.source != edge.target &&
                    instructionAtom[edge.source] == instructionAtom[edge.target]) {
                    orderConstrained[edge.source] = 1;
                }
            }
            const auto usedOutsideAtom = [&](uint32_t instruction, uint32_t atom) {
                for (const VariableId result : program.results(InstructionId{instruction})) {
                    if (!result.valid()) {
                        continue;
                    }
                    if (pinnedVariable[result.value]) {
                        return true;
                    }
                    for (uint32_t use = defUse.useOffsets[result.value];
                         use < defUse.useOffsets[result.value + 1]; ++use) {
                        if (instructionAtom[defUse.uses[use]] != atom) {
                            return true;
                        }
                    }
                }
                return false;
            };
            const auto usedInsideAtom = [&](uint32_t instruction, uint32_t atom) {
                for (const VariableId result : program.results(InstructionId{instruction})) {
                    if (!result.valid()) {
                        continue;
                    }
                    for (uint32_t use = defUse.useOffsets[result.value];
                         use < defUse.useOffsets[result.value + 1]; ++use) {
                        if (instructionAtom[defUse.uses[use]] == atom) {
                            return true;
                        }
                    }
                }
                return false;
            };
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                const uint32_t begin = atomMemberOffsets[atom];
                const uint32_t end = atomMemberOffsets[atom + 1];
                if (end - begin <= 1 || atomCombLoop[atom] != 0) {
                    continue;
                }
                uint32_t anchor = kInvalidIndex;
                uint32_t externalDefs = 0;
                for (uint32_t offset = begin; offset < end; ++offset) {
                    if (usedOutsideAtom(atomMembers[offset], atom)) {
                        anchor = atomMembers[offset];
                        ++externalDefs;
                    }
                }
                if (externalDefs > 1) {
                    atomMultiSink[atom] = 1;
                    continue;
                }
                if (externalDefs == 0 || anchor == atomMembers[end - 1] ||
                    orderConstrained[anchor] != 0 || usedInsideAtom(anchor, atom)) {
                    continue;
                }
                // Nothing inside the atom depends on the anchor, so moving
                // it to the end keeps the topological order valid.
                for (uint32_t offset = begin; offset < end; ++offset) {
                    if (atomMembers[offset] == anchor) {
                        std::rotate(atomMembers.begin() + offset,
                                    atomMembers.begin() + offset + 1,
                                    atomMembers.begin() + end);
                        break;
                    }
                }
            }
        }

        CsrGraph atomGraph = buildCondensationGraph(
            instructionGraph,
            nodeAligned ? std::span<const uint32_t>(instructionAtom)
                        : std::span<const uint32_t>(scc.component),
            atomCount);
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
                cost.blockClass == BlockClass::Commit ? options.maxCommitAtomsPerBlock
                                                      : options.maxAtomsPerBlock;
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

        // Atom taxonomy (NO0007/NO0008): commit atoms carry their
        // event-signature rank, multi-instruction SCC packings are comb-loop
        // atoms, and everything else is a singleton. A mux-rooted compute
        // atom records its select variable id as the signature; the
        // tree-atom fold pass keeps the same convention when it rebuilds the
        // tables. Other compute atoms use kInvalidAtomSignature.
        // NO0006 node-aligned path: comb-loop atoms are exactly the
        // cycle-unioned groups, multi-instruction node groups are trees
        // (anchor root orders last) and singleton node groups stay
        // singletons; the mux signature reads the root (last member), same
        // as the fold-pass convention.
        std::vector<uint8_t> atomKinds(atomCount, 0);
        std::vector<uint32_t> atomSignatures(atomCount, kInvalidAtomSignature);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            if (atomIsCommit[atom] != 0) {
                atomKinds[atom] = static_cast<uint8_t>(AmAtomKind::CommitEvent);
                atomSignatures[atom] = commitEventRank[atom];
            } else if (!nodeAligned) {
                if (atomInstructions[atom] > 1) {
                    atomKinds[atom] = static_cast<uint8_t>(AmAtomKind::CombLoopScc);
                } else {
                    atomKinds[atom] = static_cast<uint8_t>(AmAtomKind::Singleton);
                    const InstructionId root{atomMembers[atomMemberOffsets[atom]]};
                    const auto operands = program.operands(root);
                    if (program.opcode(root) == Opcode::Mux && operands.size() == 3 &&
                        operands[0].valid()) {
                        atomSignatures[atom] = operands[0].value;
                    }
                }
            } else if (atomCombLoop[atom] != 0) {
                atomKinds[atom] = static_cast<uint8_t>(AmAtomKind::CombLoopScc);
            } else {
                atomKinds[atom] = atomInstructions[atom] > 1
                                      ? static_cast<uint8_t>(AmAtomKind::Tree)
                                      : static_cast<uint8_t>(AmAtomKind::Singleton);
                const InstructionId root{atomMembers[atomMemberOffsets[atom + 1] - 1]};
                const auto operands = program.operands(root);
                if (program.opcode(root) == Opcode::Mux && operands.size() == 3 &&
                    operands[0].valid()) {
                    atomSignatures[atom] = operands[0].value;
                }
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
        context.atomKinds = std::move(atomKinds);
        context.atomSignatures = std::move(atomSignatures);
        context.oversizedAtomCount = oversizedAtomCount;
        context.maxAtomInstructions = maxAtomInstructions;
        context.maxAtomStateWrites = maxAtomStateWrites;
        context.maxAtomsPerBlock = options.maxAtomsPerBlock;
        context.maxCommitAtomsPerBlock = options.maxCommitAtomsPerBlock;
        context.enableCoarsening = options.enableCoarsening;
        // Merge host member atom limit for the out1/in1/sibling merge
        // sweeps (gsim MAX_NODES_PER_SUPER); 0 selects the 256 default.
        context.coarsenAtomBudget =
            options.dpCoarsenAtomBudget != 0 ? options.dpCoarsenAtomBudget : 256;
        context.coarsenInstructionBudget = options.dpCoarsenInstrBudget;
        context.segmentPenalty = options.dpSegmentPenalty;
        context.refinementRounds = options.dpRefinementRounds;
        context.mergeWhenMinGroup = options.mergeWhenMinGroup;

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

        if (const char *exportPrefix = std::getenv(kSplitGraphExportEnv)) {
            const std::string prefix(exportPrefix);
            if (prefix.empty() ||
                !exportSplitSideJsonl(program, context, false, prefix + ".compute.jsonl",
                                      diagnostics) ||
                !exportSplitSideJsonl(program, context, true, prefix + ".commit.jsonl",
                                      diagnostics)) {
                diagnostics.error("AM split graph export failed",
                                  std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }

        // NO0006 node-aligned audit: a one-line summary (atom counts, the
        // node bijection check, unowned-helper opcode mix, SCC-union
        // fallbacks) plus the optional per-atom JSONL dump. On this path no
        // atom-rewriting pass runs after split, so the tables are final.
        if (nodeAligned) {
            std::size_t commitAtoms = 0;
            std::size_t nodeMappedAtoms = 0;
            std::size_t multiSinkAtoms = 0;
            std::unordered_set<int64_t> distinctNodes;
            std::map<std::string, uint64_t> unownedOpcodes;
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (context.atomIsCommit[atom] != 0) {
                    ++commitAtoms;
                    continue;
                }
                multiSinkAtoms += atomMultiSink[atom] != 0 ? 1 : 0;
                if (atomGsimNodeId[atom] >= 0) {
                    ++nodeMappedAtoms;
                    distinctNodes.insert(atomGsimNodeId[atom]);
                    continue;
                }
                for (uint32_t offset = context.atomMemberOffsets[atom];
                     offset < context.atomMemberOffsets[atom + 1]; ++offset) {
                    ++unownedOpcodes[std::string(
                        toString(program.opcode(
                            InstructionId{context.atomMembers[offset]})))];
                }
            }
            std::string unownedMix;
            for (const auto &[name, count] : unownedOpcodes) {
                unownedMix += " " + name + "=" + std::to_string(count);
            }
            diagnostics.info(
                "am.node-atoms: atoms=" + std::to_string(atomCount) +
                    " compute=" + std::to_string(atomCount - commitAtoms) +
                    " commit=" + std::to_string(commitAtoms) +
                    " node_mapped=" + std::to_string(nodeMappedAtoms) +
                    " distinct_nodes=" + std::to_string(distinctNodes.size()) +
                    " unowned_compute=" +
                    std::to_string(atomCount - commitAtoms - nodeMappedAtoms) +
                    " unowned_opcode_mix[" + unownedMix + " ]" +
                    " scc_union_fallback=" + std::to_string(sccUnionFallbackCount) +
                    " multi_sink_atoms=" + std::to_string(multiSinkAtoms),
                std::string(kDiagnosticContext));
            if (const char *auditPath = std::getenv(kNodeAtomAuditEnv)) {
                if (auditPath[0] == '\0' ||
                    !exportNodeAtomAuditJsonl(context, atomGsimNodeId, atomMultiSink,
                                              std::filesystem::path(auditPath),
                                              diagnostics)) {
                    diagnostics.error("AM node-atom audit export failed",
                                      std::string(kDiagnosticContext));
                    return std::nullopt;
                }
            }
        }
        return context;
    }

    void propagateAtomGsimNodeIds(const AmGraph &graph,
                                  const AmGraphSplitContext &context,
                                  AmComputeActivityGraph &computeActivity,
                                  AmCommitEventGraph &commitEvent)
    {
        // Per-atom value semantics: one shared member node id -> that id;
        // every member unowned -> -1; mixed member node ids -> -2. Computed
        // from the context's final atom member tables so atom-rewriting
        // passes (tree-atom fold, fanout absorb) stay transparent to it.
        std::vector<int64_t> globalNodeId(context.atomCount, -1);
        for (uint32_t atom = 0; atom < context.atomCount; ++atom) {
            const uint32_t begin = context.atomMemberOffsets[atom];
            const uint32_t end = context.atomMemberOffsets[atom + 1];
            if (begin == end) {
                continue;
            }
            const int64_t first = graph.gsimNodeId(InstructionId{context.atomMembers[begin]});
            bool mixed = false;
            for (uint32_t offset = begin + 1; offset < end; ++offset) {
                if (graph.gsimNodeId(InstructionId{context.atomMembers[offset]}) != first) {
                    mixed = true;
                    break;
                }
            }
            globalNodeId[atom] = mixed ? -2 : first;
        }

        const AmComputeGraph &computeGraph = context.split.computeGraph;
        computeActivity.atomGsimNodeId.assign(computeGraph.atomCount, -1);
        for (uint32_t local = 0; local < computeGraph.atomCount; ++local) {
            computeActivity.atomGsimNodeId[local] = globalNodeId[computeGraph.globalOfAtom[local]];
        }
        const AmCommitGraph &commitGraph = context.split.commitGraph;
        commitEvent.atomGsimNodeId.assign(commitGraph.atomCount, -1);
        for (uint32_t local = 0; local < commitGraph.atomCount; ++local) {
            commitEvent.atomGsimNodeId[local] = globalNodeId[commitGraph.globalOfAtom[local]];
        }
    }

} // namespace wolvrix::lib::grhsim::am
