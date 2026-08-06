#include "grhsim/am/grhsim_am_graph_to_program.hpp"

#include "grhsim/am/grhsim_am_program.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    using namespace detail;

    namespace
    {
        // Second research export (topo-partition-proj harness): the production
        // block assignment itself (the plain baseline) plus the structural
        // scoreboard numbers recomputed from production internals, so the
        // offline scorer can be reconciled against them.
        constexpr char kBlockAssignmentExportEnv[] =
            "WOLVRIX_GRHSIM_AM_BLOCK_ASSIGNMENT_JSONL";
        constexpr std::string_view kBlockAssignmentFormat =
            "wolvrix.am-block-assignment.v1";

        bool exportBlockAssignmentJsonl(ProgramView program, const DefUseIndex &defUse,
                                        std::span<const uint32_t> instructionBlock,
                                        uint32_t normalBlockCount, uint32_t computeBlockCount,
                                        uint32_t commitBlockBegin, uint32_t commitBlockEnd,
                                        uint32_t inputSinkBlock,
                                        std::span<const uint32_t> blockInstructionCounts,
                                        const std::filesystem::path &path,
                                        wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const uint32_t instructionCount =
                static_cast<uint32_t>(program.instructionCount());
            const auto isCommitBlock = [&](uint32_t block) {
                return commitBlockBegin != 0 && block >= commitBlockBegin;
            };

            // Scoreboard, mirroring the segment DP's incoming-activation cost:
            // per variable, each distinct consuming compute block whose block
            // does not define the variable contributes one (value, block) pair
            // and ceil(width/64) copies. State targets and interface inputs
            // have no defining instruction and count as permanent boundaries.
            // dag_edges dedups (producer block, consumer block) over def-use
            // reads across all block kinds.
            uint64_t computeComputeValuePairs = 0;
            uint64_t incomingCopyCost = 0;
            std::vector<uint64_t> dagPairs;
            std::vector<uint32_t> pairMark(normalBlockCount + 1, 0);
            for (uint32_t variable = 0; variable < defUse.definitions.size(); ++variable) {
                const uint32_t definition = defUse.definitions[variable];
                const uint32_t sourceBlock =
                    definition == kInvalidIndex ? kInvalidIndex : instructionBlock[definition];
                const uint32_t stamp = variable + 1;
                const uint64_t copyCost = std::max<uint64_t>(
                    1, (exportedVariableWidth(program, VariableId{variable}) + 63) / 64);
                for (uint32_t offset = defUse.useOffsets[variable];
                     offset < defUse.useOffsets[variable + 1]; ++offset) {
                    const uint32_t targetBlock = instructionBlock[defUse.uses[offset]];
                    if (pairMark[targetBlock] == stamp) {
                        continue;
                    }
                    pairMark[targetBlock] = stamp;
                    if (sourceBlock != kInvalidIndex) {
                        if (targetBlock == sourceBlock) {
                            continue;
                        }
                        dagPairs.push_back((static_cast<uint64_t>(sourceBlock) << 32) |
                                           targetBlock);
                    }
                    if (!isCommitBlock(targetBlock)) {
                        ++computeComputeValuePairs;
                        incomingCopyCost += copyCost;
                    }
                }
            }
            std::sort(dagPairs.begin(), dagPairs.end());
            const uint64_t dagEdges = static_cast<uint64_t>(
                std::unique(dagPairs.begin(), dagPairs.end()) - dagPairs.begin());

            if (path.has_parent_path()) {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) {
                    diagnostics.error("failed to create AM block assignment export directory: " +
                                          path.parent_path().string(),
                                      std::string(kDiagnosticContext));
                    return false;
                }
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                diagnostics.error("failed to open AM block assignment export path: " +
                                      path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }

            JsonlGraphWriter writer(output);
            writer.raw("{\"record\":\"header\",\"format\":\"")
                .raw(kBlockAssignmentFormat)
                .raw("\",\"instructions\":")
                .number(instructionCount)
                .raw(",\"variables\":")
                .number(static_cast<uint64_t>(program.variableCount()))
                .raw(",\"blocks\":")
                .number(normalBlockCount)
                .raw(",\"compute_blocks\":")
                .number(computeBlockCount)
                .raw(",\"commit_blocks\":")
                .number(commitBlockBegin == 0 ? 0 : commitBlockEnd - commitBlockBegin)
                .raw(",\"input_sink_block\":")
                .number(inputSinkBlock)
                .raw(",\"dag_edges\":")
                .number(dagEdges)
                .raw(",\"compute_compute_value_pairs\":")
                .number(computeComputeValuePairs)
                .raw(",\"incoming_copy_cost\":")
                .number(incomingCopyCost)
                .raw("}");
            writer.endLine();

            for (uint32_t block = 1; block <= normalBlockCount; ++block) {
                writer.raw("{\"record\":\"block\",\"id\":")
                    .number(block)
                    .raw(",\"kind\":\"")
                    .raw(isCommitBlock(block) ? std::string_view("commit")
                                              : std::string_view("compute"))
                    .raw("\",\"size\":")
                    .number(blockInstructionCounts[block])
                    .raw("}");
                writer.endLine();
            }

            for (uint32_t instruction = 0; instruction < instructionCount; ++instruction) {
                writer.raw("{\"record\":\"assign\",\"instr\":")
                    .number(instruction)
                    .raw(",\"block\":")
                    .number(instructionBlock[instruction])
                    .raw("}");
                writer.endLine();
            }

            if (!writer.flush()) {
                diagnostics.error("failed while writing AM block assignment export: " +
                                      path.string(),
                                  std::string(kDiagnosticContext));
                return false;
            }
            diagnostics.info("exported AM block assignment: path=" + path.string() +
                                 " blocks=" + std::to_string(normalBlockCount) +
                                 " dag_edges=" + std::to_string(dagEdges) +
                                 " compute_compute_value_pairs=" +
                                 std::to_string(computeComputeValuePairs) +
                                 " incoming_copy_cost=" + std::to_string(incomingCopyCost),
                             std::string(kDiagnosticContext));
            return true;
        }

        struct ActivationEdge
        {
            uint32_t sourceBlock = 0;
            uint32_t variable = 0;
            uint32_t targetBlock = 0;
            bool directEvent = false;

            friend bool operator<(const ActivationEdge &lhs, const ActivationEdge &rhs)
            {
                return std::tie(lhs.sourceBlock, lhs.variable, lhs.directEvent, lhs.targetBlock) <
                       std::tie(rhs.sourceBlock, rhs.variable, rhs.directEvent, rhs.targetBlock);
            }

            friend bool operator==(const ActivationEdge &, const ActivationEdge &) = default;
        };

        struct MaterializationCounts
        {
            std::size_t detectors = 0;
            std::size_t activations = 0;
            std::size_t targets = 0;
        };

        MaterializationCounts countMaterialization(std::span<const ActivationEdge> edges)
        {
            MaterializationCounts counts;
            std::size_t begin = 0;
            while (begin < edges.size()) {
                std::size_t end = begin + 1;
                while (end < edges.size() && edges[end].sourceBlock == edges[begin].sourceBlock &&
                       edges[end].variable == edges[begin].variable &&
                       edges[end].directEvent == edges[begin].directEvent) {
                    ++end;
                }
                counts.detectors += edges[begin].directEvent ? 0 : 1;
                bool forward = false;
                bool backward = false;
                for (std::size_t index = begin; index < end; ++index) {
                    forward = forward || edges[index].targetBlock > edges[index].sourceBlock;
                    backward = backward || edges[index].targetBlock <= edges[index].sourceBlock;
                }
                counts.activations +=
                    static_cast<std::size_t>(forward) + static_cast<std::size_t>(backward);
                counts.targets += end - begin;
                begin = end;
            }
            return counts;
        }

        void appendWatchGroups(ScheduledProgramBuilder &builder, uint32_t sourceBlock,
                               std::span<const ActivationEdge> edges, std::size_t &edgeCursor,
                               TypeId eventType)
        {
            while (edgeCursor < edges.size() && edges[edgeCursor].sourceBlock == sourceBlock) {
                const ActivationEdge &first = edges[edgeCursor];
                std::size_t end = edgeCursor + 1;
                while (end < edges.size() && edges[end].sourceBlock == sourceBlock &&
                       edges[end].variable == first.variable &&
                       edges[end].directEvent == first.directEvent) {
                    ++end;
                }

                VariableId event{first.variable};
                if (!first.directEvent) {
                    const VariableId watched{first.variable};
                    const TypeId watchedType = builder.view().variable(watched).type;
                    const VariableId oldValue =
                        builder.addVariable(watchedType, builder.undefInit());
                    event = builder.addVariable(eventType, builder.zeroInit());
                    const std::array<VariableId, 1> results = {event};
                    const std::array<VariableId, 2> operands = {watched, oldValue};
                    const InstructionId changed =
                        builder.addInstruction(Opcode::ChangedAny, results, operands);
                    builder.appendBlockInstruction(changed);
                }

                std::vector<BlockId> forward;
                std::vector<BlockId> backward;
                for (std::size_t index = edgeCursor; index < end; ++index) {
                    if (edges[index].targetBlock > sourceBlock) {
                        forward.push_back(BlockId{edges[index].targetBlock});
                    } else {
                        backward.push_back(BlockId{edges[index].targetBlock});
                    }
                }
                const std::array<VariableId, 1> operands = {event};
                if (!forward.empty()) {
                    const InstructionId activate =
                        builder.addInstruction(Opcode::ActForward, {}, operands);
                    builder.setActivationTargets(activate, forward);
                    builder.appendBlockInstruction(activate);
                }
                if (!backward.empty()) {
                    const InstructionId activate =
                        builder.addInstruction(Opcode::ActBackward, {}, operands);
                    builder.setActivationTargets(activate, backward);
                    builder.appendBlockInstruction(activate);
                }
                edgeCursor = end;
            }
        }

        // The finalizer: turns the settled schedule (per-Block node lists,
        // gate-detector plan, activation edges) into the executable program.
        // This is the single place that assigns the Block layout, materializes
        // the changed.* gate detectors and the Act instructions, and fixes the
        // intra-Block instruction order.
        std::optional<ExecutableModel> finalizeScheduledModel(
            AmGraph &graph, ProgramView program, const DefUseIndex &defUse,
            const std::vector<std::vector<uint32_t>> &blockNodes,
            const std::vector<std::vector<CommitEventPart>> &commitGateParts,
            std::vector<ActivationEdge> &activationEdges, uint32_t normalBlockCount,
            uint32_t commitBlockBegin, uint32_t commitBlockEnd, TypeId eventType,
            bool needsEventType, const MaterializationCounts &materialization,
            std::size_t headDetectorCount, uint32_t instructionCount,
            diag::Diagnostics &diagnostics)
        {
            const auto isCommitBlock = [&](uint32_t block) {
                return commitBlockBegin != 0 && block >= commitBlockBegin;
            };
            LinearProgramArtifact scheduledInput = graph.toLinearProgram();
            ProgramInterface interface = std::move(scheduledInput.interface);
            try {
                ScheduledProgramBuilder builder(std::move(scheduledInput.program));
                builder.reserve(ScheduledProgramReserve{
                    .additionalTypes = needsEventType && !eventType.valid() ? 1U : 0U,
                    .additionalVariables = (materialization.detectors + headDetectorCount) * 2,
                    .additionalInstructions =
                        materialization.detectors + headDetectorCount + materialization.activations,
                    .additionalOperands =
                        (materialization.detectors + headDetectorCount) * 2 +
                        materialization.activations,
                    .additionalResults = materialization.detectors + headDetectorCount,
                    .blocks = static_cast<std::size_t>(normalBlockCount) + 1,
                    .blockInstructionIds = static_cast<std::size_t>(instructionCount) +
                                           materialization.detectors + headDetectorCount +
                                           materialization.activations,
                    .activationInstructions = materialization.activations,
                    .activationTargets = materialization.targets,
                });
                if (needsEventType && !eventType.valid()) {
                    eventType = builder.addType(Type::bitVector(1));
                }

                std::size_t edgeCursor = 0;
                builder.beginBlock();
                appendWatchGroups(builder, 0, activationEdges, edgeCursor, eventType);
                builder.endBlock();

                for (uint32_t block = 1; block <= normalBlockCount; ++block) {
                    builder.beginBlock();
                    std::map<CommitEventPart, VariableId> gateEvents;
                    if (isCommitBlock(block)) {
                        // The Block's aggregated gate detectors come first: one
                        // changed.* per watched (kind, variable) part, so the
                        // whole Block shares a single merged event check.
                        for (const CommitEventPart &part : commitGateParts[block]) {
                            const VariableId watched{part.second};
                            const TypeId watchedType = builder.view().variable(watched).type;
                            const VariableId oldValue =
                                builder.addVariable(watchedType, builder.undefInit());
                            const VariableId event =
                                builder.addVariable(eventType, builder.zeroInit());
                            const std::array<VariableId, 1> results = {event};
                            const std::array<VariableId, 2> operands = {watched, oldValue};
                            Opcode detectorOpcode = Opcode::ChangedAny;
                            if (part.first == changedEventKind(Opcode::ChangedPos)) {
                                detectorOpcode = Opcode::ChangedPos;
                            } else if (part.first == changedEventKind(Opcode::ChangedNeg)) {
                                detectorOpcode = Opcode::ChangedNeg;
                            }
                            const InstructionId detector =
                                builder.addInstruction(detectorOpcode, results, operands);
                            builder.appendBlockInstruction(detector);
                            gateEvents.emplace(part, event);
                        }
                    }
                    for (const uint32_t instructionValue : blockNodes[block]) {
                        const InstructionId instruction{instructionValue};
                        if (isCommitBlock(block) && !gateEvents.empty()) {
                            // Re-point the write's event operands at the in-Block
                            // detector results so the Block-level gate and the
                            // writes observe one shared detector set.
                            const auto operands = program.operands(instruction);
                            const std::size_t eventBegin = stateWriteEventBegin(
                                program.opcode(instruction), operands.size());
                            for (std::size_t index = eventBegin; index < operands.size();
                                 ++index) {
                                const CommitEventPart part =
                                    canonicalCommitEvent(program, defUse, operands[index]);
                                const auto found = gateEvents.find(part);
                                if (found != gateEvents.end()) {
                                    builder.setInstructionOperand(instruction, index,
                                                                  found->second);
                                }
                            }
                        }
                        builder.appendBlockInstruction(instruction);
                    }
                    appendWatchGroups(builder, block, activationEdges, edgeCursor, eventType);
                    builder.endBlock();
                }
                if (edgeCursor != activationEdges.size()) {
                    diagnostics.error("internal error: not all AM activation edges were materialized",
                                      std::string(kDiagnosticContext));
                    return std::nullopt;
                }

                return ExecutableModel{
                    .program = builder.finish(),
                    .interface = std::move(interface),
                    .commitBlockBegin = commitBlockBegin,
                    .commitBlockEnd = commitBlockEnd,
                };
            } catch (const std::exception &error) {
                diagnostics.error(std::string("AM activity scheduling failed: ") + error.what(),
                                  std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }
    } // namespace

    std::optional<ExecutableModel>
    materializeAmProgram(AmGraph &graph,
                         AmGraphSplitContext &context,
                         const AmComputeActivityGraph &computeActivity,
                         const AmCommitEventGraph &commitEvent,
                         const ActivityScheduleOptions &options,
                         wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        const ProgramView program = graph.program();
        const uint32_t instructionCount = context.instructionCount;
        const uint32_t variableCount = context.variableCount;
        const uint32_t atomCount = context.atomCount;
        const DefUseIndex &defUse = context.defUse;
        const std::vector<uint32_t> &atomMemberOffsets = context.atomMemberOffsets;
        const std::vector<uint32_t> &atomMembers = context.atomMembers;
        const std::vector<uint32_t> &atomInstructions = context.atomInstructions;
        const std::vector<uint8_t> &atomIsCommit = context.atomIsCommit;
        const std::size_t oversizedAtomCount = context.oversizedAtomCount;
        const std::size_t maxAtomInstructions = context.maxAtomInstructions;
        const std::size_t maxAtomStateWrites = context.maxAtomStateWrites;
        const AmGraphSplit &graphSplit = context.split;

        // ---- stage: materialize -----------------------------------------
        // Merge the two partitioned graphs back into the global atom
        // numbering: commit blocks sit past the compute range (the input
        // sink slot, if needed, is inserted below).
        std::vector<uint32_t> atomBlock(atomCount, kInvalidIndex);
        std::vector<uint32_t> atomTopo;
        atomTopo.reserve(atomCount);
        for (uint32_t local = 0; local < graphSplit.computeGraph.atomCount; ++local) {
            atomBlock[graphSplit.computeGraph.globalOfAtom[local]] =
                computeActivity.atomBlock[local];
        }
        for (const uint32_t local : computeActivity.atomTopo) {
            atomTopo.push_back(graphSplit.computeGraph.globalOfAtom[local]);
        }
        for (uint32_t local = 0; local < graphSplit.commitGraph.atomCount; ++local) {
            atomBlock[graphSplit.commitGraph.globalOfAtom[local]] =
                computeActivity.blockCount + commitEvent.atomBlock[local];
        }
        for (const uint32_t local : commitEvent.atomTopo) {
            atomTopo.push_back(graphSplit.commitGraph.globalOfAtom[local]);
        }
        uint32_t normalBlockCount = computeActivity.blockCount + commitEvent.blockCount;
        if (options.collectStats) {
            diagnostics.info("coarsen-dp block formation stats: clusters_after_coarsen=" +
                                 std::to_string(computeActivity.clustersAfterCoarsen) +
                                 " dp_segments=" +
                                 std::to_string(computeActivity.dpSegments) +
                                 " coarsen_ms=" + std::to_string(computeActivity.coarsenMs) +
                                 " dp_ms=" + std::to_string(computeActivity.dpMs) +
                                 " rounds=" + std::to_string(computeActivity.coarsenRounds) +
                                 " out1_merges=" +
                                 std::to_string(computeActivity.coarsenOut1Merges) +
                                 " in1_merges=" +
                                 std::to_string(computeActivity.coarsenIn1Merges) +
                                 " sibling_merges=" +
                                 std::to_string(computeActivity.coarsenSiblingMerges),
                             std::string(kDiagnosticContext));
            diagnostics.info("coarsen-dp initial degree histogram: " +
                                 computeActivity.initialDegreeHistogram,
                             std::string(kDiagnosticContext));
        }

        std::vector<uint32_t> externalInputs;
        externalInputs.reserve(graph.interface().ports.size());
        for (const PortBinding &port : graph.interface().ports) {
            if (port.direction == PortDirection::Input || port.direction == PortDirection::Inout) {
                externalInputs.push_back(port.input.value);
            }
        }
        std::sort(externalInputs.begin(), externalInputs.end());
        externalInputs.erase(std::unique(externalInputs.begin(), externalInputs.end()),
                             externalInputs.end());

        bool needsInputSink = false;
        for (uint32_t input : externalInputs) {
            if (defUse.useOffsets[input] == defUse.useOffsets[input + 1]) {
                needsInputSink = true;
                break;
            }
        }

        const uint32_t computeBlockCount = computeActivity.blockCount;
        const uint32_t commitBlockCount = commitEvent.blockCount;
        // The input sink is an empty compute Block taking the activation of
        // otherwise-unused inputs. It sits immediately before the commit range
        // so commit Blocks remain the Program's trailing Block range.
        uint32_t inputSinkBlock = 0;
        if (needsInputSink) {
            inputSinkBlock = computeBlockCount + 1;
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (atomIsCommit[atom] != 0) {
                    ++atomBlock[atom];
                }
            }
            ++normalBlockCount;
        }
        const uint32_t commitBlockBegin =
            commitBlockCount == 0 ? 0U : computeBlockCount + (needsInputSink ? 2U : 1U);
        const uint32_t commitBlockEnd =
            commitBlockCount == 0 ? 0U : commitBlockBegin + commitBlockCount;

        std::vector<uint32_t> instructionBlock(instructionCount, 0);
        // Per-Block ordered node lists: the graph-level block assignment.
        // Block membership and intra-Block order live here (not in a flat
        // layout array) so passes annotate and the finalizer serializes once.
        std::vector<std::vector<uint32_t>> blockNodes(normalBlockCount + 1);
        for (uint32_t atom : atomTopo) {
            const uint32_t block = atomBlock[atom];
            auto &nodes = blockNodes[block];
            nodes.reserve(nodes.size() + atomInstructions[atom]);
            for (uint32_t offset = atomMemberOffsets[atom]; offset < atomMemberOffsets[atom + 1];
                 ++offset) {
                const uint32_t instruction = atomMembers[offset];
                nodes.push_back(instruction);
                instructionBlock[instruction] = block;
            }
        }
        std::vector<uint32_t> semanticBlockCounts(normalBlockCount + 1, 0);
        for (uint32_t block = 1; block <= normalBlockCount; ++block) {
            semanticBlockCounts[block] = static_cast<uint32_t>(blockNodes[block].size());
        }

        const auto isCommitBlock = [&](uint32_t block) {
            return commitBlockBegin != 0 && block >= commitBlockBegin;
        };

        // Cone packing has been removed: register/latch merged write chains
        // settle correctly from pre-commit compute snapshots, and memory
        // writes carry cond/mask at the write site (masked_write_words), so
        // nothing needs to be pulled into commit Blocks. The full pass was a
        // measurable net loss on the production design (commit-Block code
        // ballooned 28x, 533s vs 330s for 50k coremark cycles) and the only
        // remaining mandatory piece (read-old element evaluation) is gone
        // with the cond/mask revert of mem.write.
        for (uint32_t block = 1; block <= normalBlockCount; ++block) {
            semanticBlockCounts[block] =
                static_cast<uint32_t>(blockNodes[block].size());
        }

        // Intra-Block order guard for commit Blocks: a use must not precede
        // its definition when both live in the same commit Block. Commit
        // Blocks interleave gate detectors with ordered state writes, and a
        // stale emission order silently corrupts simulation (a use then
        // observes the previous round's value), so fail loudly instead.
        // Compute Blocks are exempt: a def-use SCC atom intentionally
        // iterates within its Block, so a use may legitimately precede its
        // definition there.
        if (commitBlockBegin != 0) {
            std::vector<uint32_t> positionOf(instructionCount, kInvalidIndex);
            for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
                const auto &nodes = blockNodes[block];
                for (std::size_t position = 0; position < nodes.size(); ++position) {
                    positionOf[nodes[position]] = static_cast<uint32_t>(position);
                }
                for (std::size_t position = 0; position < nodes.size(); ++position) {
                    const auto operands = program.operands(InstructionId{nodes[position]});
                    for (const VariableId operand : operands) {
                        if (!operand.valid() || operand.value >= defUse.definitions.size()) {
                            continue;
                        }
                        const uint32_t definition = defUse.definitions[operand.value];
                        if (definition == kInvalidIndex ||
                            instructionBlock[definition] != block) {
                            continue;
                        }
                        if (positionOf[definition] >= position) {
                            diagnostics.error(
                                "AM intra-Block order violation: instruction=" +
                                    std::to_string(nodes[position]) + " uses variable=" +
                                    std::to_string(operand.value) + " defined by instruction=" +
                                    std::to_string(definition) + " later in block=" +
                                    std::to_string(block),
                                std::string(kDiagnosticContext));
                            return std::nullopt;
                        }
                    }
                }
                for (const uint32_t instruction : nodes) {
                    positionOf[instruction] = kInvalidIndex;
                }
            }
        }

        // Clock-domain view of every commit Block: the (kind, variable) parts
        // its aggregated gate detectors watch. Eventful writes contribute
        // their canonical raw event sources; latch writes are eventless and
        // contribute their merged nextValue, so an eventless Block gates on an
        // actual nextValue change. Every watched part becomes one in-Block
        // changed.* detector during materialization.
        std::vector<std::vector<CommitEventPart>> commitGateParts(normalBlockCount + 1);
        std::unordered_map<uint32_t, std::vector<uint32_t>> commitStateWriters;
        std::size_t headDetectorCount = 0;
        for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
            std::vector<CommitEventPart> parts;
            for (const uint32_t instructionValue : blockNodes[block]) {
                const InstructionId instruction{instructionValue};
                const std::optional<VariableId> target =
                    stateWriteTarget(program, instruction);
                if (!target) {
                    continue;
                }
                commitStateWriters[target->value].push_back(block);
                const auto operands = program.operands(instruction);
                if (program.opcode(instruction) == Opcode::LatchWrite) {
                    parts.push_back(CommitEventPart{changedEventKind(Opcode::ChangedAny),
                                                    operands.front().value});
                    continue;
                }
                const std::size_t eventBegin =
                    stateWriteEventBegin(program.opcode(instruction), operands.size());
                for (std::size_t index = eventBegin; index < operands.size(); ++index) {
                    parts.push_back(canonicalCommitEvent(program, defUse, operands[index]));
                }
            }
            std::sort(parts.begin(), parts.end());
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
            headDetectorCount += parts.size();
            commitGateParts[block] = std::move(parts);
        }

        // Phase-discipline audit (debug): a state variable read inside a
        // commit Block must be a target that Block itself writes (an
        // intra-Block RMW / ordered-chain live read). Reading a state variable
        // written only by *other* Blocks observes a live, possibly
        // just-committed value where a pre-commit snapshot is expected -- the
        // phase-shear hazard. Report those, they are never legitimate here.
        if (std::getenv("WOLVRIX_GRHSIM_AM_AUDIT_PHASE") != nullptr) {
            // writerBlocks[v] = sorted set of commit Blocks writing v.
            std::unordered_map<uint32_t, std::vector<uint32_t>> writersOf;
            for (const auto &[target, blocks] : commitStateWriters) {
                writersOf[target] = blocks;
            }
            std::map<std::string, std::size_t> violationsByName;
            std::size_t violations = 0;
            for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
                for (const uint32_t instructionValue : blockNodes[block]) {
                    const InstructionId instruction{instructionValue};
                    const auto operands = program.operands(instruction);
                    for (std::size_t position = 0; position < operands.size(); ++position) {
                        const VariableId operand = operands[position];
                        if (!operand.valid()) {
                            continue;
                        }
                        const auto found = writersOf.find(operand.value);
                        if (found == writersOf.end()) {
                            continue;  // not a state variable
                        }
                        const auto &writers = found->second;
                        if (std::find(writers.begin(), writers.end(), block) !=
                            writers.end()) {
                            continue;  // this Block writes it: RMW/chain read
                        }
                        const auto label = program.variableLabel(operand);
                        const std::string name =
                            label ? std::string(program.string(*label)) : std::string("<anon>");
                        ++violationsByName[name];
                        ++violations;
                    }
                }
            }
            std::fprintf(stderr, "[phase-audit] foreign-state live reads: %zu\n", violations);
            for (const auto &[name, count] : violationsByName) {
                std::fprintf(stderr, "[phase-audit]   %6zu  %s\n", count, name.c_str());
            }
        }

        if (const char *exportPath = std::getenv(kBlockAssignmentExportEnv)) {
            if (exportPath[0] == '\0' ||
                !exportBlockAssignmentJsonl(program, defUse, instructionBlock, normalBlockCount,
                                            computeBlockCount, commitBlockBegin, commitBlockEnd,
                                            inputSinkBlock, semanticBlockCounts,
                                            std::filesystem::path(exportPath), diagnostics)) {
                diagnostics.error("AM block assignment export failed",
                                  std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }

        std::vector<ActivationEdge> activationEdges;
        activationEdges.reserve(defUse.uses.size() + externalInputs.size());
        for (uint32_t input : externalInputs) {
            bool used = false;
            for (uint32_t offset = defUse.useOffsets[input]; offset < defUse.useOffsets[input + 1];
                 ++offset) {
                used = true;
                const uint32_t targetBlock = instructionBlock[defUse.uses[offset]];
                if (isCommitBlock(targetBlock)) {
                    // Commit Blocks draw their activation exclusively from
                    // gate-detector variables (explicit pass below).
                    continue;
                }
                activationEdges.push_back(ActivationEdge{
                    .sourceBlock = 0,
                    .variable = input,
                    .targetBlock = targetBlock,
                });
            }
            if (!used) {
                activationEdges.push_back(ActivationEdge{
                    .sourceBlock = 0,
                    .variable = input,
                    .targetBlock = inputSinkBlock,
                });
            }
        }

        // A def->use edge activates the using Block only when it is a different
        // compute Block; same-Block uses are covered by in-Block instruction
        // order. Edges into commit Blocks are NOT taken from def-use: a commit
        // Block may only be activated through one of its gate-detector
        // variables (see the explicit pass below).
        for (uint32_t variable = 0; variable < variableCount; ++variable) {
            const uint32_t definition = defUse.definitions[variable];
            if (definition == kInvalidIndex) {
                continue;
            }
            const uint32_t sourceBlock = instructionBlock[definition];
            const bool directEvent = isChanged(program.opcode(InstructionId{definition}));
            for (uint32_t offset = defUse.useOffsets[variable];
                 offset < defUse.useOffsets[variable + 1]; ++offset) {
                const uint32_t targetBlock = instructionBlock[defUse.uses[offset]];
                if (targetBlock == sourceBlock || isCommitBlock(targetBlock)) {
                    continue;
                }
                activationEdges.push_back(ActivationEdge{
                    .sourceBlock = sourceBlock,
                    .variable = variable,
                    .targetBlock = targetBlock,
                    .directEvent = directEvent,
                });
            }
        }

        // Commit Block activation (clock-domain rule): a compute or commit
        // Block may forward activation into a commit Block only through a
        // variable that at least one of the target Block's gate detectors
        // watches. Sources: the variable's defining Block, the entry Block
        // for source-less values (interface inputs), or every commit Block
        // writing the watched state variable (derived clocks).
        for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
            for (const CommitEventPart &part : commitGateParts[block]) {
                const uint32_t watched = part.second;
                const uint32_t definition = defUse.definitions[watched];
                if (definition != kInvalidIndex) {
                    const uint32_t sourceBlock = instructionBlock[definition];
                    if (sourceBlock != block) {
                        activationEdges.push_back(ActivationEdge{
                            .sourceBlock = sourceBlock,
                            .variable = watched,
                            .targetBlock = block,
                            .directEvent =
                                isChanged(program.opcode(InstructionId{definition})),
                        });
                    }
                    continue;
                }
                const auto writers = commitStateWriters.find(watched);
                if (writers == commitStateWriters.end()) {
                    // Source-less non-state value (e.g. an interface input):
                    // the entry Block watches it and forwards the activation.
                    activationEdges.push_back(ActivationEdge{
                        .sourceBlock = 0,
                        .variable = watched,
                        .targetBlock = block,
                    });
                    continue;
                }
                for (const uint32_t writerBlock : writers->second) {
                    activationEdges.push_back(ActivationEdge{
                        .sourceBlock = writerBlock,
                        .variable = watched,
                        .targetBlock = block,
                    });
                }
            }
        }

        // Each commit Block watches the state targets written inside it and
        // reactivates the compute Blocks reading them (ActBackward). Edges
        // sharing one (commit Block, state target) pair collapse into a single
        // detector during materialization.
        for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
            for (const uint32_t instruction : blockNodes[block]) {
                const std::optional<VariableId> target =
                    stateWriteTarget(program, InstructionId{instruction});
                if (!target) {
                    continue;
                }
                for (uint32_t useOffset = defUse.useOffsets[target->value];
                     useOffset < defUse.useOffsets[target->value + 1]; ++useOffset) {
                    const uint32_t readerBlock = instructionBlock[defUse.uses[useOffset]];
                    if (readerBlock == 0 || isCommitBlock(readerBlock)) {
                        continue;
                    }
                    activationEdges.push_back(ActivationEdge{
                        .sourceBlock = block,
                        .variable = target->value,
                        .targetBlock = readerBlock,
                    });
                }
            }
        }

        std::sort(activationEdges.begin(), activationEdges.end());
        activationEdges.erase(std::unique(activationEdges.begin(), activationEdges.end()),
                              activationEdges.end());
        for (const ActivationEdge &edge : activationEdges) {
            if (edge.sourceBlock >= normalBlockCount + 1 || edge.targetBlock == 0 ||
                edge.targetBlock > normalBlockCount) {
                diagnostics.error("internal error: invalid AM activation dependency",
                                  std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }

        const MaterializationCounts materialization = countMaterialization(activationEdges);
        const bool needsEventType = materialization.detectors != 0 || headDetectorCount != 0;
        TypeId eventType;
        if (needsEventType) {
            for (uint32_t type = 0; type < program.typeCount(); ++type) {
                const Type &candidate = program.type(TypeId{type});
                if (candidate.kind == TypeKind::BitVector && candidate.bitWidth == 1 &&
                    candidate.signedness == Signedness::Unsigned) {
                    eventType = TypeId{type};
                    break;
                }
            }
        }

        std::optional<ExecutableModel> finalized = finalizeScheduledModel(
            graph, program, defUse, blockNodes, commitGateParts, activationEdges,
            normalBlockCount, commitBlockBegin, commitBlockEnd, eventType, needsEventType,
            materialization, headDetectorCount, instructionCount, diagnostics);
        if (!finalized) {
            return std::nullopt;
        }
        ExecutableModel model = std::move(*finalized);
        try {
            if (const char *dumpVars = std::getenv("WOLVRIX_GRHSIM_AM_DUMP_VARIABLES")) {
                if (dumpVars[0] != '\0') {
                    const ProgramView scheduledView = model.program.view();
                    for (const VariableLabel &label : scheduledView.variableLabels()) {
                        const std::string_view name = scheduledView.string(label.label);
                        if (name.find(dumpVars) != std::string_view::npos) {
                            std::fprintf(stderr, "[var-dump] v%u %.*s\n", label.variable.value,
                                         static_cast<int>(name.size()), name.data());
                        }
                    }
                }
            }
            if (const char *dumpFilter = std::getenv("WOLVRIX_GRHSIM_AM_DUMP_COMMIT_BLOCKS")) {
                if (dumpFilter[0] != '\0') {
                    // Debug dump: print every commit Block whose writes touch a
                    // labeled variable containing the filter substring.
                    const ProgramView scheduledView = model.program.view();
                    std::unordered_map<uint32_t, std::string> variableNames;
                    for (const VariableLabel &label : scheduledView.variableLabels()) {
                        variableNames[label.variable.value] =
                            std::string(scheduledView.string(label.label));
                    }
                    for (uint32_t block = model.commitBlockBegin; block < model.commitBlockEnd;
                         ++block) {
                        const BlockId blockId{block};
                        bool matches = false;
                        for (std::size_t position = 0; position < model.program.blockSize(blockId);
                             ++position) {
                            const InstructionId instruction =
                                model.program.blockInstruction(blockId, position);
                            const std::optional<VariableId> target =
                                stateWriteTarget(scheduledView, instruction);
                            if (target) {
                                const auto name = variableNames.find(target->value);
                                if (name != variableNames.end() &&
                                    name->second.find(dumpFilter) != std::string::npos) {
                                    matches = true;
                                    break;
                                }
                            }
                        }
                        if (!matches) {
                            continue;
                        }
                        std::fprintf(stderr, "[commit-dump] block %u size %zu:\n", block,
                                     model.program.blockSize(blockId));
                        for (std::size_t position = 0;
                             position < model.program.blockSize(blockId); ++position) {
                            const InstructionId instruction =
                                model.program.blockInstruction(blockId, position);
                            const auto operands = scheduledView.operands(instruction);
                            std::fprintf(stderr, "  [%4zu] i%u %-16s", position,
                                         instruction.value,
                                         std::string(toString(scheduledView.opcode(instruction)))
                                             .c_str());
                            for (const VariableId operand : operands) {
                                std::fprintf(stderr, " v%u", operand.value);
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
            if (!reportValidation(
                    validate(model, ValidationOptions{.level = ValidationLevel::Semantic}),
                    diagnostics)) {
                return std::nullopt;
            }
            if (options.collectStats) {
                const ProgramStorageStats stats = model.program.view().storageStats();
                diagnostics.info(
                    "production schedule stats: linear_instructions=" +
                        std::to_string(instructionCount) + " def_use_edges=" +
                        std::to_string(defUse.uses.size()) + " atoms=" + std::to_string(atomCount) +
                        " oversized_atoms=" + std::to_string(oversizedAtomCount) +
                        " max_atom_instructions=" + std::to_string(maxAtomInstructions) +
                        " max_atom_state_writes=" + std::to_string(maxAtomStateWrites) +
                        " normal_blocks=" + std::to_string(normalBlockCount) +
                        " compute_blocks=" +
                        std::to_string(computeBlockCount) +
                        " commit_blocks=" +
                        std::to_string(commitBlockCount) +
                        " detectors=" + std::to_string(materialization.detectors) +
                        " activation_edges=" + std::to_string(materialization.targets) +
                        " scheduled_instructions=" + std::to_string(stats.instructions) +
                        " storage_bytes=" + std::to_string(stats.estimatedBytes) +
                        " reserved_bytes=" + std::to_string(stats.reservedBytes),
                    std::string(kDiagnosticContext));
            }
            return std::optional<ExecutableModel>(std::move(model));
        } catch (const std::exception &error) {
            diagnostics.error(std::string("AM activity scheduling failed: ") + error.what(),
                              std::string(kDiagnosticContext));
            return std::nullopt;
        }
    }

} // namespace wolvrix::lib::grhsim::am
