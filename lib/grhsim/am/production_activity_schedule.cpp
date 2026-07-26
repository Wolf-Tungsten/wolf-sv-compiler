#include "grhsim/am/production_activity_schedule.hpp"

#include "grhsim/am/builder.hpp"
#include "grhsim/am/grhsim_am_activity_schedule.hpp"
#include "grhsim/am/opcode_traits.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();
        constexpr std::string_view kDiagnosticContext = "grhsim-am-production-activity-schedule";

        bool isChanged(Opcode opcode) noexcept
        {
            return opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                   opcode == Opcode::ChangedNeg;
        }

        bool reportValidation(const ValidationResult &validation,
                              wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            for (const std::string &error : validation.errors) {
                diagnostics.error(error, std::string(kDiagnosticContext));
            }
            return validation.success();
        }

        struct DefUseIndex
        {
            std::vector<uint32_t> definitions;
            std::vector<uint32_t> useOffsets;
            std::vector<uint32_t> uses;
        };

        bool isDependencyOperand(ProgramView program, InstructionId instruction,
                                 std::size_t position)
        {
            const Opcode opcode = program.opcode(instruction);
            if (isChanged(opcode) && position == 1) {
                return false;
            }
            const OpcodeTraits traits = opcodeTraits(opcode);
            return traits.effect != OpcodeEffect::StateReadWrite ||
                   traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand ||
                   position != traits.stateTargetOperand;
        }

        DefUseIndex buildDefUseIndex(ProgramView program)
        {
            DefUseIndex index;
            index.definitions.assign(program.variableCount(), kInvalidIndex);
            index.useOffsets.assign(program.variableCount() + 1, 0);

            for (uint32_t instructionIndex = 0; instructionIndex < program.instructionCount();
                 ++instructionIndex) {
                const InstructionId instruction{instructionIndex};
                for (VariableId result : program.results(instruction)) {
                    index.definitions[result.value] = instructionIndex;
                }
                const auto operands = program.operands(instruction);
                for (std::size_t position = 0; position < operands.size(); ++position) {
                    if (isDependencyOperand(program, instruction, position)) {
                        ++index.useOffsets[operands[position].value + 1];
                    }
                }
            }
            std::partial_sum(index.useOffsets.begin(), index.useOffsets.end(),
                             index.useOffsets.begin());
            index.uses.resize(index.useOffsets.back());
            std::vector<uint32_t> cursor(index.useOffsets.begin(), index.useOffsets.end() - 1);
            for (uint32_t instructionIndex = 0; instructionIndex < program.instructionCount();
                 ++instructionIndex) {
                const InstructionId instruction{instructionIndex};
                const auto operands = program.operands(instruction);
                for (std::size_t position = 0; position < operands.size(); ++position) {
                    if (!isDependencyOperand(program, instruction, position)) {
                        continue;
                    }
                    const uint32_t variable = operands[position].value;
                    index.uses[cursor[variable]++] = instructionIndex;
                }
            }
            return index;
        }

        struct OrderEdge
        {
            uint32_t source = 0;
            uint32_t target = 0;
        };

        static_assert(sizeof(OrderEdge) == 2 * sizeof(uint32_t));

        struct DirectedEdge
        {
            uint32_t source = 0;
            uint32_t target = 0;
            bool hard = false;
        };

        struct CsrGraph
        {
            std::vector<uint32_t> offsets;
            std::vector<uint32_t> targets;
        };

        CsrGraph buildInstructionGraph(uint32_t instructionCount, const DefUseIndex &defUse,
                                       std::span<const OrderEdge> orderedEdges)
        {
            CsrGraph graph;
            graph.offsets.assign(static_cast<std::size_t>(instructionCount) + 1, 0);
            for (uint32_t variable = 0; variable < defUse.definitions.size(); ++variable) {
                const uint32_t definition = defUse.definitions[variable];
                if (definition == kInvalidIndex) {
                    continue;
                }
                for (uint32_t offset = defUse.useOffsets[variable];
                     offset < defUse.useOffsets[variable + 1]; ++offset) {
                    const uint32_t source = definition;
                    const uint32_t target = defUse.uses[offset];
                    if (source != target) {
                        ++graph.offsets[source + 1];
                    }
                }
            }
            for (const OrderEdge &edge : orderedEdges) {
                if (edge.source != edge.target) {
                    ++graph.offsets[edge.source + 1];
                }
            }
            std::partial_sum(graph.offsets.begin(), graph.offsets.end(), graph.offsets.begin());
            graph.targets.resize(graph.offsets.back());
            std::vector<uint32_t> cursor(graph.offsets.begin(), graph.offsets.end() - 1);
            for (uint32_t variable = 0; variable < defUse.definitions.size(); ++variable) {
                const uint32_t definition = defUse.definitions[variable];
                if (definition == kInvalidIndex) {
                    continue;
                }
                for (uint32_t offset = defUse.useOffsets[variable];
                     offset < defUse.useOffsets[variable + 1]; ++offset) {
                    const uint32_t source = definition;
                    const uint32_t target = defUse.uses[offset];
                    if (source != target) {
                        graph.targets[cursor[source]++] = target;
                    }
                }
            }
            for (const OrderEdge &edge : orderedEdges) {
                if (edge.source != edge.target) {
                    graph.targets[cursor[edge.source]++] = edge.target;
                }
            }
            return graph;
        }

        CsrGraph buildCondensationGraph(const CsrGraph &source, std::span<const uint32_t> component,
                                        uint32_t componentCount)
        {
            CsrGraph graph;
            graph.offsets.assign(static_cast<std::size_t>(componentCount) + 1, 0);
            for (uint32_t node = 0; node < component.size(); ++node) {
                const uint32_t sourceComponent = component[node];
                for (uint32_t offset = source.offsets[node]; offset < source.offsets[node + 1];
                     ++offset) {
                    const uint32_t targetComponent = component[source.targets[offset]];
                    if (sourceComponent != targetComponent) {
                        ++graph.offsets[sourceComponent + 1];
                    }
                }
            }
            std::partial_sum(graph.offsets.begin(), graph.offsets.end(), graph.offsets.begin());
            graph.targets.resize(graph.offsets.back());
            std::vector<uint32_t> cursor(graph.offsets.begin(), graph.offsets.end() - 1);
            for (uint32_t node = 0; node < component.size(); ++node) {
                const uint32_t sourceComponent = component[node];
                for (uint32_t offset = source.offsets[node]; offset < source.offsets[node + 1];
                     ++offset) {
                    const uint32_t targetComponent = component[source.targets[offset]];
                    if (sourceComponent != targetComponent) {
                        graph.targets[cursor[sourceComponent]++] = targetComponent;
                    }
                }
            }
            return graph;
        }

        struct SccResult
        {
            std::vector<uint32_t> component;
            uint32_t count = 0;
        };

        SccResult findStronglyConnectedComponents(const CsrGraph &graph)
        {
            const uint32_t nodeCount = static_cast<uint32_t>(graph.offsets.size() - 1);
            std::vector<uint32_t> discovery(nodeCount, kInvalidIndex);
            std::vector<uint32_t> low(nodeCount, 0);
            std::vector<uint8_t> onStack(nodeCount, 0);
            std::vector<uint32_t> stack;
            stack.reserve(nodeCount);

            struct Frame
            {
                uint32_t node = 0;
                uint32_t nextEdge = 0;
                uint32_t parent = kInvalidIndex;
            };
            std::vector<Frame> frames;
            frames.reserve(nodeCount);

            SccResult result;
            result.component.assign(nodeCount, kInvalidIndex);
            uint32_t nextDiscovery = 0;
            const auto discover = [&](uint32_t node, uint32_t parent) {
                discovery[node] = nextDiscovery;
                low[node] = nextDiscovery;
                ++nextDiscovery;
                stack.push_back(node);
                onStack[node] = 1;
                frames.push_back(Frame{
                    .node = node,
                    .nextEdge = graph.offsets[node],
                    .parent = parent,
                });
            };

            for (uint32_t root = 0; root < nodeCount; ++root) {
                if (discovery[root] != kInvalidIndex) {
                    continue;
                }
                discover(root, kInvalidIndex);
                while (!frames.empty()) {
                    Frame &frame = frames.back();
                    const uint32_t node = frame.node;
                    if (frame.nextEdge < graph.offsets[node + 1]) {
                        const uint32_t target = graph.targets[frame.nextEdge++];
                        if (discovery[target] == kInvalidIndex) {
                            discover(target, node);
                        } else if (onStack[target]) {
                            low[node] = std::min(low[node], discovery[target]);
                        }
                        continue;
                    }

                    const uint32_t parent = frame.parent;
                    if (low[node] == discovery[node]) {
                        while (true) {
                            const uint32_t member = stack.back();
                            stack.pop_back();
                            onStack[member] = 0;
                            result.component[member] = result.count;
                            if (member == node) {
                                break;
                            }
                        }
                        ++result.count;
                    }
                    frames.pop_back();
                    if (parent != kInvalidIndex) {
                        low[parent] = std::min(low[parent], low[node]);
                    }
                }
            }
            return result;
        }

        std::optional<VariableId> stateWriteTarget(ProgramView program, InstructionId instruction)
        {
            if (isChanged(program.opcode(instruction))) {
                return std::nullopt;
            }
            const OpcodeTraits traits = opcodeTraits(program.opcode(instruction));
            if (traits.effect != OpcodeEffect::StateReadWrite ||
                traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand) {
                return std::nullopt;
            }
            const auto operands = program.operands(instruction);
            return traits.stateTargetOperand < operands.size()
                       ? std::optional<VariableId>(operands[traits.stateTargetOperand])
                       : std::nullopt;
        }

        std::size_t commitOperandCaptureCount(Opcode opcode) noexcept
        {
            switch (opcode) {
            case Opcode::RegisterWrite:
                return 3;
            case Opcode::MemoryWrite:
                return 4;
            case Opcode::MemoryFill:
                return 2;
            default:
                return 0;
            }
        }

        using CommitEventPart = std::pair<uint8_t, uint32_t>;
        using CommitInstructionEventKey = std::vector<CommitEventPart>;
        using CommitAtomEventKey = std::vector<CommitInstructionEventKey>;
        using CommitAtomGuardKey = std::vector<uint32_t>;

        uint8_t changedEventKind(Opcode opcode) noexcept
        {
            switch (opcode) {
            case Opcode::ChangedAny:
                return 1;
            case Opcode::ChangedPos:
                return 2;
            case Opcode::ChangedNeg:
                return 3;
            default:
                return 0;
            }
        }

        CommitEventPart canonicalCommitEvent(ProgramView program, const DefUseIndex &defUse,
                                             VariableId event)
        {
            const uint32_t definition = defUse.definitions[event.value];
            if (definition != kInvalidIndex) {
                const InstructionId instruction{definition};
                const uint8_t kind = changedEventKind(program.opcode(instruction));
                const auto operands = program.operands(instruction);
                if (kind != 0 && !operands.empty()) {
                    return CommitEventPart{kind, operands.front().value};
                }
            }
            return CommitEventPart{0, event.value};
        }

        struct CommitInstructionBucketKey
        {
            CommitInstructionEventKey events;
            uint32_t guard = kInvalidIndex;
        };

        CommitInstructionBucketKey commitInstructionBucketKey(ProgramView program,
                                                               const DefUseIndex &defUse,
                                                               InstructionId instruction)
        {
            const auto operands = program.operands(instruction);
            CommitInstructionBucketKey key;
            if (!operands.empty()) {
                key.guard = operands.front().value;
            }

            std::size_t eventBegin = operands.size();
            switch (program.opcode(instruction)) {
            case Opcode::RegisterWrite:
                eventBegin = 4;
                break;
            case Opcode::MemoryWrite:
                eventBegin = 5;
                break;
            case Opcode::MemoryFill:
                eventBegin = 3;
                break;
            case Opcode::LatchWrite:
                break;
            default:
                return key;
            }
            eventBegin = std::min(eventBegin, operands.size());
            key.events.reserve(operands.size() - eventBegin);
            for (std::size_t index = eventBegin; index < operands.size(); ++index) {
                key.events.push_back(canonicalCommitEvent(program, defUse, operands[index]));
            }
            std::sort(key.events.begin(), key.events.end());
            key.events.erase(std::unique(key.events.begin(), key.events.end()), key.events.end());
            return key;
        }

        bool isEffectfulForCycle(InstructionEffect effect) noexcept
        {
            return effect != InstructionEffect::Pure && effect != InstructionEffect::StateRead;
        }

        bool orderAtomInstructions(std::span<uint32_t> members, ProgramView program,
                                   const DefUseIndex &defUse,
                                   const std::vector<OrderEdge> &orderedEdges,
                                   const std::vector<InstructionEffect> &effects,
                                   std::vector<uint32_t> &localIndex,
                                   wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            if (members.size() <= 1) {
                return true;
            }
            for (uint32_t local = 0; local < members.size(); ++local) {
                localIndex[members[local]] = local;
            }

            std::vector<DirectedEdge> edges;
            for (uint32_t sourceInstruction : members) {
                const InstructionId source{sourceInstruction};
                const bool sourceIsChanged = isChanged(program.opcode(source));
                for (VariableId result : program.results(source)) {
                    for (uint32_t offset = defUse.useOffsets[result.value];
                         offset < defUse.useOffsets[result.value + 1]; ++offset) {
                        const uint32_t targetInstruction = defUse.uses[offset];
                        if (targetInstruction == sourceInstruction ||
                            localIndex[targetInstruction] == kInvalidIndex) {
                            continue;
                        }
                        edges.push_back(DirectedEdge{
                            .source = localIndex[sourceInstruction],
                            .target = localIndex[targetInstruction],
                            .hard =
                                sourceIsChanged || isEffectfulForCycle(effects[targetInstruction]),
                        });
                    }
                }
            }
            for (const OrderEdge &edge : orderedEdges) {
                if (localIndex[edge.source] != kInvalidIndex &&
                    localIndex[edge.target] != kInvalidIndex && edge.source != edge.target) {
                    edges.push_back(DirectedEdge{
                        .source = localIndex[edge.source],
                        .target = localIndex[edge.target],
                        .hard = true,
                    });
                }
            }

            std::vector<uint32_t> indegree(members.size(), 0);
            std::vector<uint32_t> hardIndegree(members.size(), 0);
            std::vector<uint32_t> outgoingOffsets(members.size() + 1, 0);
            std::vector<uint32_t> outgoingHardOffsets(members.size() + 1, 0);
            for (const DirectedEdge &edge : edges) {
                ++indegree[edge.target];
                ++outgoingOffsets[edge.source + 1];
                if (edge.hard) {
                    ++hardIndegree[edge.target];
                    ++outgoingHardOffsets[edge.source + 1];
                }
            }
            std::partial_sum(outgoingOffsets.begin(), outgoingOffsets.end(),
                             outgoingOffsets.begin());
            std::partial_sum(outgoingHardOffsets.begin(), outgoingHardOffsets.end(),
                             outgoingHardOffsets.begin());
            std::vector<uint32_t> outgoing(outgoingOffsets.back());
            std::vector<uint32_t> outgoingHard(outgoingHardOffsets.back());
            std::vector<uint32_t> outgoingCursor(outgoingOffsets.begin(),
                                                 outgoingOffsets.end() - 1);
            std::vector<uint32_t> outgoingHardCursor(outgoingHardOffsets.begin(),
                                                     outgoingHardOffsets.end() - 1);
            for (const DirectedEdge &edge : edges) {
                outgoing[outgoingCursor[edge.source]++] = edge.target;
                if (edge.hard) {
                    outgoingHard[outgoingHardCursor[edge.source]++] = edge.target;
                }
            }

            using Candidate = std::pair<uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            for (uint32_t local = 0; local < members.size(); ++local) {
                if (indegree[local] == 0) {
                    ready.emplace(members[local], local);
                }
            }
            std::vector<uint8_t> placed(members.size(), 0);
            std::vector<uint32_t> order;
            order.reserve(members.size());
            while (order.size() != members.size()) {
                uint32_t local = kInvalidIndex;
                while (!ready.empty()) {
                    const uint32_t candidate = ready.top().second;
                    ready.pop();
                    if (!placed[candidate] && indegree[candidate] == 0) {
                        local = candidate;
                        break;
                    }
                }
                if (local == kInvalidIndex) {
                    bool hasEffectfulCycle = false;
                    for (uint32_t candidate = 0; candidate < members.size(); ++candidate) {
                        hasEffectfulCycle =
                            hasEffectfulCycle || (!placed[candidate] &&
                                                  isEffectfulForCycle(effects[members[candidate]]));
                    }
                    if (hasEffectfulCycle) {
                        diagnostics.error("AM dependency cycle contains a state/host effect "
                                          "and cannot be scheduled conservatively",
                                          std::string(kDiagnosticContext));
                        for (uint32_t instruction : members) {
                            localIndex[instruction] = kInvalidIndex;
                        }
                        return false;
                    }
                    for (uint32_t candidate = 0; candidate < members.size(); ++candidate) {
                        if (!placed[candidate] && hardIndegree[candidate] == 0 &&
                            (local == kInvalidIndex || members[candidate] < members[local])) {
                            local = candidate;
                        }
                    }
                    if (local == kInvalidIndex) {
                        diagnostics.error(
                            "AM ordered/raw-event constraints form an unschedulable cycle",
                            std::string(kDiagnosticContext));
                        for (uint32_t instruction : members) {
                            localIndex[instruction] = kInvalidIndex;
                        }
                        return false;
                    }
                }

                placed[local] = 1;
                order.push_back(members[local]);
                for (uint32_t offset = outgoingOffsets[local]; offset < outgoingOffsets[local + 1];
                     ++offset) {
                    const uint32_t target = outgoing[offset];
                    if (!placed[target] && --indegree[target] == 0) {
                        ready.emplace(members[target], target);
                    }
                }
                for (uint32_t offset = outgoingHardOffsets[local];
                     offset < outgoingHardOffsets[local + 1]; ++offset) {
                    const uint32_t target = outgoingHard[offset];
                    if (!placed[target]) {
                        --hardIndegree[target];
                    }
                }
            }

            std::copy(order.begin(), order.end(), members.begin());
            for (uint32_t instruction : members) {
                localIndex[instruction] = kInvalidIndex;
            }
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

        struct CommitExecutionPlan
        {
            std::vector<BlockId> order;
            std::vector<uint32_t> groupOffsets;
        };

        CommitExecutionPlan buildCommitExecutionPlan(
            uint32_t blockCount, uint32_t commitBegin, uint32_t commitEnd,
            std::span<const ActivationEdge> activationEdges,
            std::span<const OrderEdge> extraEdges)
        {
            CommitExecutionPlan plan;
            if (commitBegin == commitEnd) {
                return plan;
            }

            std::vector<std::vector<uint32_t>> blockTargets(blockCount);
            for (const ActivationEdge &edge : activationEdges) {
                blockTargets[edge.sourceBlock].push_back(edge.targetBlock);
            }
            for (const OrderEdge &edge : extraEdges) {
                blockTargets[edge.source].push_back(edge.target);
            }
            for (std::vector<uint32_t> &targets : blockTargets) {
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            }

            const uint32_t commitCount = commitEnd - commitBegin;
            using ActivationKey = std::tuple<uint32_t, uint32_t, bool, uint32_t>;
            std::vector<ActivationKey> firstActivation;
            firstActivation.reserve(commitCount);
            for (uint32_t block = commitBegin; block < commitEnd; ++block) {
                firstActivation.emplace_back(kInvalidIndex, kInvalidIndex, true, block);
            }
            for (const ActivationEdge &edge : activationEdges) {
                if (edge.targetBlock < commitBegin || edge.targetBlock >= commitEnd) {
                    continue;
                }
                const ActivationKey candidate{edge.sourceBlock, edge.variable,
                                              edge.directEvent, edge.targetBlock};
                ActivationKey &current = firstActivation[edge.targetBlock - commitBegin];
                current = std::min(current, candidate);
            }

            std::vector<std::vector<uint32_t>> hardCommitTargets(commitCount);
            for (const OrderEdge &edge : extraEdges) {
                if (edge.source < commitBegin || edge.source >= commitEnd ||
                    edge.target < commitBegin || edge.target >= commitEnd ||
                    edge.source == edge.target) {
                    continue;
                }
                hardCommitTargets[edge.source - commitBegin].push_back(
                    edge.target - commitBegin);
            }
            for (std::vector<uint32_t> &targets : hardCommitTargets) {
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            }

            std::vector<std::vector<uint32_t>> commitTargets(commitCount);
            std::vector<uint32_t> visited(blockCount, 0);
            std::vector<uint32_t> worklist;
            uint32_t stamp = 0;
            for (uint32_t source = commitBegin; source < commitEnd; ++source) {
                ++stamp;
                worklist.clear();
                visited[source] = stamp;
                for (uint32_t target : blockTargets[source]) {
                    if (visited[target] != stamp) {
                        visited[target] = stamp;
                        worklist.push_back(target);
                    }
                }
                for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor) {
                    const uint32_t block = worklist[cursor];
                    if (block >= commitBegin && block < commitEnd) {
                        if (block != source) {
                            commitTargets[source - commitBegin].push_back(block - commitBegin);
                        }
                        continue;
                    }
                    for (uint32_t target : blockTargets[block]) {
                        if (visited[target] == stamp) {
                            continue;
                        }
                        visited[target] = stamp;
                        worklist.push_back(target);
                    }
                }
                std::vector<uint32_t> &targets = commitTargets[source - commitBegin];
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            }

            CsrGraph commitGraph;
            commitGraph.offsets.resize(static_cast<std::size_t>(commitCount) + 1, 0);
            for (uint32_t source = 0; source < commitCount; ++source) {
                commitGraph.offsets[source + 1] =
                    commitGraph.offsets[source] + commitTargets[source].size();
                commitGraph.targets.insert(commitGraph.targets.end(),
                                           commitTargets[source].begin(),
                                           commitTargets[source].end());
            }
            const SccResult scc = findStronglyConnectedComponents(commitGraph);
            std::vector<std::vector<uint32_t>> members(scc.count);
            std::vector<uint32_t> minimumBlock(scc.count, kInvalidIndex);
            for (uint32_t block = 0; block < commitCount; ++block) {
                const uint32_t component = scc.component[block];
                members[component].push_back(block);
                minimumBlock[component] =
                    std::min(minimumBlock[component], commitBegin + block);
            }

            std::vector<std::vector<uint32_t>> componentTargets(scc.count);
            std::vector<uint32_t> indegree(scc.count, 0);
            for (uint32_t source = 0; source < commitCount; ++source) {
                const uint32_t sourceComponent = scc.component[source];
                for (uint32_t target : commitTargets[source]) {
                    const uint32_t targetComponent = scc.component[target];
                    if (sourceComponent != targetComponent) {
                        componentTargets[sourceComponent].push_back(targetComponent);
                    }
                }
            }
            for (std::vector<uint32_t> &targets : componentTargets) {
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                for (uint32_t target : targets) {
                    ++indegree[target];
                }
            }

            using ComponentCandidate = std::pair<uint32_t, uint32_t>;
            std::priority_queue<ComponentCandidate, std::vector<ComponentCandidate>,
                                std::greater<>> ready;
            for (uint32_t component = 0; component < scc.count; ++component) {
                if (indegree[component] == 0) {
                    ready.emplace(minimumBlock[component], component);
                }
            }
            plan.order.reserve(commitCount);
            plan.groupOffsets.reserve(static_cast<std::size_t>(commitCount) + 1);
            plan.groupOffsets.push_back(0);
            std::vector<uint32_t> localIndex(commitCount, kInvalidIndex);
            while (!ready.empty()) {
                const uint32_t component = ready.top().second;
                ready.pop();

                std::vector<uint32_t> &componentMembers = members[component];
                for (uint32_t local = 0; local < componentMembers.size(); ++local) {
                    localIndex[componentMembers[local]] = local;
                }
                std::vector<uint32_t> localIndegree(componentMembers.size(), 0);
                std::vector<uint32_t> localHardIndegree(componentMembers.size(), 0);
                for (uint32_t source : componentMembers) {
                    for (uint32_t target : commitTargets[source]) {
                        if (scc.component[target] == component) {
                            ++localIndegree[localIndex[target]];
                        }
                    }
                    for (uint32_t target : hardCommitTargets[source]) {
                        if (scc.component[target] == component) {
                            ++localHardIndegree[localIndex[target]];
                        }
                    }
                }

                using BlockCandidate = std::tuple<ActivationKey, uint32_t, uint32_t>;
                std::priority_queue<BlockCandidate, std::vector<BlockCandidate>,
                                    std::greater<>> localReady;
                std::priority_queue<BlockCandidate, std::vector<BlockCandidate>,
                                    std::greater<>> cutReady;
                const auto makeCandidate = [&](uint32_t local) {
                    const uint32_t block = componentMembers[local];
                    return BlockCandidate{firstActivation[block], commitBegin + block, local};
                };
                for (uint32_t local = 0; local < componentMembers.size(); ++local) {
                    if (localIndegree[local] == 0) {
                        localReady.push(makeCandidate(local));
                    }
                    if (localHardIndegree[local] == 0) {
                        cutReady.push(makeCandidate(local));
                    }
                }

                std::vector<uint8_t> placed(componentMembers.size(), 0);
                for (std::size_t placedCount = 0; placedCount < componentMembers.size();
                     ++placedCount) {
                    uint32_t local = kInvalidIndex;
                    while (!localReady.empty()) {
                        const uint32_t candidate = std::get<2>(localReady.top());
                        localReady.pop();
                        if (!placed[candidate] && localIndegree[candidate] == 0) {
                            local = candidate;
                            break;
                        }
                    }
                    if (local == kInvalidIndex) {
                        while (!cutReady.empty()) {
                            const uint32_t candidate = std::get<2>(cutReady.top());
                            cutReady.pop();
                            if (!placed[candidate] && localHardIndegree[candidate] == 0) {
                                local = candidate;
                                break;
                            }
                        }
                    }

                    // Writer-frontier edges are ordered by increasing commit BlockId, so their
                    // subgraph is acyclic and always leaves at least one legal cycle cut.
                    if (local == kInvalidIndex) {
                        break;
                    }
                    const uint32_t block = componentMembers[local];
                    placed[local] = 1;
                    plan.order.push_back(BlockId{commitBegin + block});
                    for (uint32_t target : commitTargets[block]) {
                        if (scc.component[target] != component) {
                            continue;
                        }
                        const uint32_t targetLocal = localIndex[target];
                        if (!placed[targetLocal] && --localIndegree[targetLocal] == 0) {
                            localReady.push(makeCandidate(targetLocal));
                        }
                    }
                    for (uint32_t target : hardCommitTargets[block]) {
                        if (scc.component[target] == component) {
                            const uint32_t targetLocal = localIndex[target];
                            if (!placed[targetLocal] && --localHardIndegree[targetLocal] == 0) {
                                cutReady.push(makeCandidate(targetLocal));
                            }
                        }
                    }
                }
                plan.groupOffsets.push_back(static_cast<uint32_t>(plan.order.size()));
                for (uint32_t block : componentMembers) {
                    localIndex[block] = kInvalidIndex;
                }
                for (uint32_t target : componentTargets[component]) {
                    if (--indegree[target] == 0) {
                        ready.emplace(minimumBlock[target], target);
                    }
                }
            }
            return plan;
        }

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

        bool appendWatchGroups(ScheduledProgramBuilder &builder, uint32_t sourceBlock,
                               std::span<const ActivationEdge> edges, std::size_t &edgeCursor,
                               TypeId eventType, wolvrix::lib::diag::Diagnostics &diagnostics)
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
                if (first.directEvent && !backward.empty()) {
                    diagnostics.error("raw changed event would cross an epoch before its consumer",
                                      std::string(kDiagnosticContext));
                    return false;
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
            return true;
        }
    } // namespace

    std::optional<ExecutableModel>
    ProductionActivityScheduleStage::schedule(LinearProgramArtifact &&linear,
                                              const ActivityScheduleOptions &options,
                                              wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        if (diagnostics.hasError()) {
            return std::nullopt;
        }
        if (options.maxInstructionsPerBlock == 0 || options.maxCommitInstructionsPerBlock == 0 ||
            options.maxStateWritesPerBlock == 0) {
            diagnostics.error("AM activity scheduling limits must be non-zero",
                              std::string(kDiagnosticContext));
            return std::nullopt;
        }
        if (!reportValidation(
                validate(linear, ValidationOptions{.level = ValidationLevel::Semantic}),
                diagnostics)) {
            return std::nullopt;
        }

        const ProgramView program = linear.program.view();
        const uint32_t instructionCount = static_cast<uint32_t>(program.instructionCount());
        const uint32_t variableCount = static_cast<uint32_t>(program.variableCount());
        const DefUseIndex defUse = buildDefUseIndex(program);
        std::vector<OrderEdge> orderedEdges;
        orderedEdges.reserve(linear.schedulingFacts.orderedEffects.size());
        std::vector<uint8_t> hasExplicitOrder(instructionCount, 0);
        std::vector<uint32_t> explicitOrderGroup(instructionCount, kInvalidIndex);
        uint32_t previousGroup = 0;
        uint32_t previousInstruction = kInvalidIndex;
        bool havePrevious = false;
        for (const OrderedEffect &effect : linear.schedulingFacts.orderedEffects) {
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
        std::vector<uint32_t> stateWriteTargets;
        stateWriteTargets.reserve(instructionCount);
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
            stateWriteTargets.push_back(target->value);
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
        std::sort(stateWriteTargets.begin(), stateWriteTargets.end());
        stateWriteTargets.erase(std::unique(stateWriteTargets.begin(), stateWriteTargets.end()),
                                stateWriteTargets.end());

        const CsrGraph instructionGraph =
            buildInstructionGraph(instructionCount, defUse, orderedEdges);
        const SccResult scc = findStronglyConnectedComponents(instructionGraph);
        const uint32_t atomCount = scc.count;
        std::vector<uint32_t> instructionAtom(instructionCount, kInvalidIndex);
        for (uint32_t index = 0; index < instructionCount; ++index) {
            instructionAtom[index] = scc.component[index];
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
                                       linear.schedulingFacts.instructionEffects, localIndex,
                                       diagnostics)) {
                return std::nullopt;
            }
        }

        const CsrGraph atomGraph =
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
            CommitAtomGuardKey commitGuards;
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
                    CommitInstructionBucketKey bucket = commitInstructionBucketKey(
                        program, defUse, InstructionId{instruction});
                    cost.commitEvents.push_back(std::move(bucket.events));
                    cost.commitGuards.push_back(bucket.guard);
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
            std::sort(cost.commitGuards.begin(), cost.commitGuards.end());
            cost.commitGuards.erase(
                std::unique(cost.commitGuards.begin(), cost.commitGuards.end()),
                cost.commitGuards.end());
            maxAtomInstructions = std::max(maxAtomInstructions, cost.instructions);
            maxAtomStateWrites = std::max(maxAtomStateWrites, cost.stateWrites);
            const std::size_t instructionLimit =
                cost.blockClass == BlockClass::Commit ? options.maxCommitInstructionsPerBlock
                                                       : options.maxInstructionsPerBlock;
            if (cost.instructions > instructionLimit ||
                cost.stateWrites > options.maxStateWritesPerBlock) {
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

        std::vector<uint32_t> atomIndegree(atomCount, 0);
        std::vector<uint32_t> atomMinInstruction(atomCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            atomMinInstruction[atom] = atomMembers[atomMemberOffsets[atom]];
            for (uint32_t offset = atomGraph.offsets[atom]; offset < atomGraph.offsets[atom + 1];
                 ++offset) {
                ++atomIndegree[atomGraph.targets[offset]];
            }
        }

        struct EventBucketRanks
        {
            uint32_t event = 0;
            uint32_t nextGuard = 0;
            std::map<CommitAtomGuardKey, uint32_t> guards;
        };
        std::map<CommitAtomEventKey, EventBucketRanks> bucketRanks;
        std::vector<uint32_t> commitEventRank(atomCount, 0);
        std::vector<uint32_t> commitGuardRank(atomCount, 0);
        std::vector<uint32_t> atomsByInstruction(atomCount);
        std::iota(atomsByInstruction.begin(), atomsByInstruction.end(), uint32_t{0});
        std::sort(atomsByInstruction.begin(), atomsByInstruction.end(),
                  [&](uint32_t lhs, uint32_t rhs) {
                      return std::tie(atomMinInstruction[lhs], lhs) <
                             std::tie(atomMinInstruction[rhs], rhs);
                  });
        uint32_t nextEventRank = 0;
        for (uint32_t atom : atomsByInstruction) {
            const AtomCost &cost = atomCosts[atom];
            if (cost.blockClass != BlockClass::Commit) {
                continue;
            }
            auto [eventIt, eventInserted] = bucketRanks.try_emplace(cost.commitEvents);
            if (eventInserted) {
                eventIt->second.event = nextEventRank++;
            }
            auto [guardIt, guardInserted] =
                eventIt->second.guards.try_emplace(cost.commitGuards);
            if (guardInserted) {
                guardIt->second = eventIt->second.nextGuard++;
            }
            commitEventRank[atom] = eventIt->second.event;
            commitGuardRank[atom] = guardIt->second;
        }

        using AtomCandidate = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
        using ReadyQueue =
            std::priority_queue<AtomCandidate, std::vector<AtomCandidate>, std::greater<>>;
        std::array<ReadyQueue, 2> readyByClass;
        const auto readyIndex = [](BlockClass blockClass) {
            return static_cast<std::size_t>(blockClass);
        };
        const auto addReady = [&](uint32_t atom) {
            const AtomCost &cost = atomCosts[atom];
            readyByClass[readyIndex(cost.blockClass)]
                .emplace(commitEventRank[atom], commitGuardRank[atom],
                         atomMinInstruction[atom], atom);
        };
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            if (atomIndegree[atom] == 0) {
                addReady(atom);
            }
        }
        std::size_t remainingPreCommitAtoms = 0;
        for (const AtomCost &cost : atomCosts) {
            remainingPreCommitAtoms += cost.blockClass != BlockClass::Commit;
        }
        const auto popNextReady = [&]() -> uint32_t {
            ReadyQueue &compute = readyByClass[readyIndex(BlockClass::Compute)];
            if (!compute.empty()) {
                const uint32_t atom = std::get<3>(compute.top());
                compute.pop();
                return atom;
            }
            if (remainingPreCommitAtoms != 0) {
                return kInvalidIndex;
            }
            ReadyQueue &commit = readyByClass[readyIndex(BlockClass::Commit)];
            if (!commit.empty()) {
                const uint32_t atom = std::get<3>(commit.top());
                commit.pop();
                return atom;
            }
            return kInvalidIndex;
        };
        const auto popReadyClass = [&](BlockClass blockClass) -> uint32_t {
            ReadyQueue &ready = readyByClass[readyIndex(blockClass)];
            if (ready.empty()) {
                return kInvalidIndex;
            }
            const uint32_t atom = std::get<3>(ready.top());
            ready.pop();
            return atom;
        };

        std::vector<uint32_t> atomBlock(atomCount, kInvalidIndex);
        std::vector<uint32_t> atomTopo;
        atomTopo.reserve(atomCount);
        uint32_t normalBlockCount = 0;
        std::array<uint32_t, 2> blockCountsByClass{};
        if (options.blockFormation == AmBlockFormation::CoarsenDp) {
            std::vector<uint32_t> atomInstructions(atomCount, 0);
            std::vector<uint32_t> atomStateWrites(atomCount, 0);
            std::vector<uint8_t> atomIsCommit(atomCount, 0);
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                atomInstructions[atom] = static_cast<uint32_t>(atomCosts[atom].instructions);
                atomStateWrites[atom] = static_cast<uint32_t>(atomCosts[atom].stateWrites);
                atomIsCommit[atom] = atomCosts[atom].blockClass == BlockClass::Commit ? 1 : 0;
            }
            const GrhSimAmActivityScheduleInput blockInput{
                .atomCount = atomCount,
                .atomOffsets = atomGraph.offsets,
                .atomTargets = atomGraph.targets,
                .atomInstructions = atomInstructions,
                .atomStateWrites = atomStateWrites,
                .atomIsCommit = atomIsCommit,
                .atomMinInstruction = atomMinInstruction,
                .commitEventRank = commitEventRank,
                .commitGuardRank = commitGuardRank,
                .variableCount = variableCount,
                .definitions = defUse.definitions,
                .useOffsets = defUse.useOffsets,
                .uses = defUse.uses,
                .instructionAtom = instructionAtom,
                .maxInstructionsPerBlock = options.maxInstructionsPerBlock,
                .maxCommitInstructionsPerBlock = options.maxCommitInstructionsPerBlock,
                .maxStateWritesPerBlock = options.maxStateWritesPerBlock,
                .coarsenBudget = options.dpCoarsenBudget != 0
                                     ? options.dpCoarsenBudget
                                     : std::max<std::size_t>(
                                           options.maxInstructionsPerBlock / 8, std::size_t{16}),
                .segmentPenalty = options.dpSegmentPenalty,
            };
            std::string blockError;
            std::optional<GrhSimAmActivityScheduleResult> scheduled =
                scheduleGrhSimAmActivityBlocks(blockInput, blockError);
            if (!scheduled) {
                diagnostics.error(std::move(blockError), std::string(kDiagnosticContext));
                return std::nullopt;
            }
            atomBlock = std::move(scheduled->atomBlock);
            atomTopo = std::move(scheduled->atomTopo);
            normalBlockCount = scheduled->normalBlockCount;
            blockCountsByClass[readyIndex(BlockClass::Compute)] = scheduled->computeBlockCount;
            blockCountsByClass[readyIndex(BlockClass::Commit)] = scheduled->commitBlockCount;
            if (options.collectStats) {
                diagnostics.info("coarsen-dp block formation stats: clusters_after_coarsen=" +
                                     std::to_string(scheduled->clustersAfterCoarsen) +
                                     " dp_segments=" + std::to_string(scheduled->dpSegments) +
                                     " coarsen_ms=" + std::to_string(scheduled->coarsenMs) +
                                     " dp_ms=" + std::to_string(scheduled->dpMs) +
                                     " rounds=" + std::to_string(scheduled->coarsenRounds) +
                                     " out1_merges=" +
                                     std::to_string(scheduled->coarsenOut1Merges) +
                                     " in1_merges=" +
                                     std::to_string(scheduled->coarsenIn1Merges) +
                                     " sibling_merges=" +
                                     std::to_string(scheduled->coarsenSiblingMerges),
                                 std::string(kDiagnosticContext));
                diagnostics.info("coarsen-dp initial degree histogram: " +
                                     scheduled->initialDegreeHistogram,
                                 std::string(kDiagnosticContext));
            }
        } else {
        std::size_t currentInstructions = 0;
        std::size_t currentStateWrites = 0;
        BlockClass currentClass = BlockClass::Compute;
        uint32_t currentBucketAtom = kInvalidIndex;
        bool haveCurrent = false;
        while (atomTopo.size() != atomCount) {
            uint32_t atom = kInvalidIndex;
            if (!haveCurrent) {
                atom = popNextReady();
                if (atom == kInvalidIndex) {
                    diagnostics.error(
                        remainingPreCommitAtoms != 0 &&
                                !readyByClass[readyIndex(BlockClass::Commit)].empty()
                            ? "AM dependency requires a state commit before pre-commit work"
                            : "internal error: AM SCC condensation is cyclic",
                        std::string(kDiagnosticContext));
                    return std::nullopt;
                }
                ++normalBlockCount;
                currentInstructions = 0;
                currentStateWrites = 0;
                currentClass = atomCosts[atom].blockClass;
                currentBucketAtom = atom;
                ++blockCountsByClass[readyIndex(currentClass)];
                haveCurrent = true;
            } else if (options.enableCoarsening) {
                ReadyQueue &ready = readyByClass[readyIndex(currentClass)];
                if (!ready.empty()) {
                    const uint32_t candidate = std::get<3>(ready.top());
                    const AtomCost &candidateCost = atomCosts[candidate];
                    const std::size_t instructionLimit =
                        currentClass == BlockClass::Commit
                            ? options.maxCommitInstructionsPerBlock
                            : options.maxInstructionsPerBlock;
                    const bool sameCommitEvent =
                        currentClass != BlockClass::Commit ||
                        candidateCost.commitEvents ==
                            atomCosts[currentBucketAtom].commitEvents;
                    if (sameCommitEvent &&
                        currentInstructions + candidateCost.instructions <= instructionLimit &&
                        currentStateWrites + candidateCost.stateWrites <=
                            options.maxStateWritesPerBlock) {
                        atom = popReadyClass(currentClass);
                    }
                }
                if (atom == kInvalidIndex) {
                    haveCurrent = false;
                    continue;
                }
            } else {
                haveCurrent = false;
                continue;
            }

            const AtomCost &cost = atomCosts[atom];
            atomBlock[atom] = normalBlockCount;
            currentInstructions += cost.instructions;
            currentStateWrites += cost.stateWrites;
            atomTopo.push_back(atom);
            if (cost.blockClass != BlockClass::Commit) {
                --remainingPreCommitAtoms;
            }
            for (uint32_t offset = atomGraph.offsets[atom]; offset < atomGraph.offsets[atom + 1];
                 ++offset) {
                const uint32_t target = atomGraph.targets[offset];
                if (--atomIndegree[target] == 0) {
                    addReady(target);
                }
            }
            if (!options.enableCoarsening) {
                haveCurrent = false;
            }
        }
        }

        const uint32_t commitBlockCount =
            blockCountsByClass[readyIndex(BlockClass::Commit)];
        const uint32_t commitBlockBegin =
            commitBlockCount == 0
                ? 0U
                : 1U + blockCountsByClass[readyIndex(BlockClass::Compute)];
        const uint32_t commitBlockEnd =
            commitBlockCount == 0 ? 0U : commitBlockBegin + commitBlockCount;

        std::vector<uint32_t> instructionBlock(instructionCount, 0);
        std::vector<uint32_t> instructionPosition(instructionCount, 0);
        std::vector<uint32_t> semanticBlockCounts(normalBlockCount + 1, 0);
        for (uint32_t atom : atomTopo) {
            semanticBlockCounts[atomBlock[atom]] +=
                static_cast<uint32_t>(atomCosts[atom].instructions);
        }
        std::vector<uint32_t> semanticBlockOffsets(normalBlockCount + 2, 0);
        for (uint32_t block = 1; block <= normalBlockCount; ++block) {
            semanticBlockOffsets[block + 1] =
                semanticBlockOffsets[block] + semanticBlockCounts[block];
        }
        std::vector<uint32_t> semanticInstructions(instructionCount, 0);
        std::vector<uint32_t> semanticCursor(semanticBlockOffsets.begin(),
                                             semanticBlockOffsets.end() - 1);
        for (uint32_t atom : atomTopo) {
            const uint32_t block = atomBlock[atom];
            for (uint32_t offset = atomMemberOffsets[atom]; offset < atomMemberOffsets[atom + 1];
                 ++offset) {
                const uint32_t instruction = atomMembers[offset];
                const uint32_t position = semanticCursor[block]++;
                semanticInstructions[position] = instruction;
                instructionBlock[instruction] = block;
                instructionPosition[instruction] = position - semanticBlockOffsets[block];
            }
        }

        std::vector<VariableId> liveSourceByPreCommitTarget(variableCount,
                                                            VariableId::invalid());
        for (const PreCommitSnapshot &snapshot : linear.preCommitSnapshots) {
            liveSourceByPreCommitTarget[snapshot.target.value] = snapshot.source;
        }

        std::size_t commitOperandCaptureUpperBound = 0;
        for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
            for (uint32_t offset = semanticBlockOffsets[block];
                 offset < semanticBlockOffsets[block + 1]; ++offset) {
                const InstructionId instruction{semanticInstructions[offset]};
                commitOperandCaptureUpperBound += std::min(
                    commitOperandCaptureCount(program.opcode(instruction)),
                    program.operands(instruction).size());
            }
        }

        std::vector<uint32_t> externalInputs;
        externalInputs.reserve(linear.interface.ports.size());
        for (const PortBinding &port : linear.interface.ports) {
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
        uint32_t inputSinkBlock = 0;
        if (needsInputSink) {
            inputSinkBlock = ++normalBlockCount;
            semanticBlockCounts.push_back(0);
            semanticBlockOffsets.push_back(semanticBlockOffsets.back());
        }

        std::vector<ActivationEdge> activationEdges;
        activationEdges.reserve(defUse.uses.size() + externalInputs.size());
        for (uint32_t input : externalInputs) {
            bool used = false;
            for (uint32_t offset = defUse.useOffsets[input]; offset < defUse.useOffsets[input + 1];
                 ++offset) {
                used = true;
                activationEdges.push_back(ActivationEdge{
                    .sourceBlock = 0,
                    .variable = input,
                    .targetBlock = instructionBlock[defUse.uses[offset]],
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

        for (uint32_t variable = 0; variable < variableCount; ++variable) {
            const uint32_t definition = defUse.definitions[variable];
            if (definition == kInvalidIndex) {
                continue;
            }
            const uint32_t sourceBlock = instructionBlock[definition];
            const bool directEvent = isChanged(program.opcode(InstructionId{definition}));
            for (uint32_t offset = defUse.useOffsets[variable];
                 offset < defUse.useOffsets[variable + 1]; ++offset) {
                const uint32_t use = defUse.uses[offset];
                const uint32_t targetBlock = instructionBlock[use];
                if (sourceBlock == targetBlock &&
                    instructionPosition[definition] < instructionPosition[use]) {
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

        std::vector<uint32_t> stateWriterBlock(variableCount, 0);
        std::vector<uint32_t> finalWriterPosition(variableCount, 0);
        std::vector<uint8_t> hasStateWriter(variableCount, 0);
        for (uint32_t instruction = 0; instruction < instructionCount; ++instruction) {
            const std::optional<VariableId> target =
                stateWriteTarget(program, InstructionId{instruction});
            if (target) {
                const uint32_t block = instructionBlock[instruction];
                const uint32_t position = instructionPosition[instruction];
                if (!hasStateWriter[target->value] || block > stateWriterBlock[target->value] ||
                    (block == stateWriterBlock[target->value] &&
                     position > finalWriterPosition[target->value])) {
                    stateWriterBlock[target->value] = block;
                    finalWriterPosition[target->value] = position;
                    hasStateWriter[target->value] = 1;
                }
            }
        }
        for (uint32_t target : stateWriteTargets) {
            const uint32_t sourceBlock = stateWriterBlock[target];
            for (uint32_t offset = defUse.useOffsets[target];
                 offset < defUse.useOffsets[target + 1]; ++offset) {
                const uint32_t reader = defUse.uses[offset];
                const uint32_t targetBlock = instructionBlock[reader];
                if (sourceBlock == targetBlock &&
                    instructionPosition[reader] > finalWriterPosition[target]) {
                    continue;
                }
                activationEdges.push_back(ActivationEdge{
                    .sourceBlock = sourceBlock,
                    .variable = target,
                    .targetBlock = targetBlock,
                });
            }
        }

        // A target split across commit Blocks has one watcher at its final writer frontier.
        // A guarded forward activation immediately before each earlier writer makes that
        // frontier run whenever the earlier writer can mutate the target.
        std::vector<VariableId> writerFrontierGuards(instructionCount, VariableId::invalid());
        std::vector<uint32_t> writerFrontierTargets(instructionCount, kInvalidIndex);
        std::size_t writerFrontierActivations = 0;
        for (uint32_t instruction = 0; instruction < instructionCount; ++instruction) {
            const std::optional<VariableId> target =
                stateWriteTarget(program, InstructionId{instruction});
            if (!target) {
                continue;
            }
            const uint32_t sourceBlock = instructionBlock[instruction];
            const uint32_t finalBlock = stateWriterBlock[target->value];
            if (sourceBlock != finalBlock) {
                const auto operands = program.operands(InstructionId{instruction});
                if (operands.empty()) {
                    diagnostics.error("state writer is missing its firing guard",
                                      std::string(kDiagnosticContext));
                    return std::nullopt;
                }
                writerFrontierGuards[instruction] = operands.front();
                writerFrontierTargets[instruction] = finalBlock;
                ++writerFrontierActivations;
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
            if (edge.directEvent && edge.targetBlock <= edge.sourceBlock) {
                diagnostics.error(
                    "raw changed event cannot be preserved across a backward activation",
                    std::string(kDiagnosticContext));
                return std::nullopt;
            }
        }

        std::vector<OrderEdge> writerFrontierEdges;
        writerFrontierEdges.reserve(writerFrontierActivations);
        for (uint32_t instruction = 0; instruction < instructionCount; ++instruction) {
            if (writerFrontierTargets[instruction] != kInvalidIndex) {
                writerFrontierEdges.push_back(OrderEdge{
                    .source = instructionBlock[instruction],
                    .target = writerFrontierTargets[instruction],
                });
            }
        }
        const CommitExecutionPlan commitPlan =
            buildCommitExecutionPlan(normalBlockCount + 1, commitBlockBegin, commitBlockEnd,
                                     activationEdges, writerFrontierEdges);

        const MaterializationCounts materialization = countMaterialization(activationEdges);
        const bool needsEventType =
            materialization.detectors != 0 || writerFrontierActivations != 0;
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

        ProgramInterface interface = std::move(linear.interface);
        linear.preCommitSnapshots.clear();
        linear.preCommitSnapshots.shrink_to_fit();
        linear.schedulingFacts.clearAndRelease();
        try {
            ScheduledProgramBuilder builder(std::move(linear.program));
            builder.reserve(ScheduledProgramReserve{
                .additionalTypes = needsEventType && !eventType.valid() ? 1U : 0U,
                .additionalVariables =
                    materialization.detectors * 2 + writerFrontierActivations +
                    commitOperandCaptureUpperBound,
                .additionalInstructions = materialization.detectors + materialization.activations +
                                          writerFrontierActivations * 2,
                .additionalOperands = materialization.detectors * 2 + materialization.activations +
                                      writerFrontierActivations * 2,
                .additionalResults =
                    materialization.detectors + writerFrontierActivations,
                .blocks = static_cast<std::size_t>(normalBlockCount) + 1,
                .blockInstructionIds = static_cast<std::size_t>(instructionCount) +
                                       materialization.detectors + materialization.activations +
                                       writerFrontierActivations * 2,
                .activationInstructions =
                    materialization.activations + writerFrontierActivations,
                .activationTargets = materialization.targets + writerFrontierActivations,
            });
            if (needsEventType && !eventType.valid()) {
                eventType = builder.addType(Type::bitVector(1));
            }

            std::vector<CommitOperandCapture> commitOperandCaptures;
            commitOperandCaptures.reserve(commitOperandCaptureUpperBound);
            std::vector<uint32_t> commitOperandCaptureOffsets;
            if (commitBlockCount != 0) {
                commitOperandCaptureOffsets.reserve(
                    static_cast<std::size_t>(commitBlockCount) + 1);
                commitOperandCaptureOffsets.push_back(0);
            }
            for (uint32_t block = commitBlockBegin; block < commitBlockEnd; ++block) {
                std::map<uint32_t, VariableId> captureTargetBySource;
                for (uint32_t offset = semanticBlockOffsets[block];
                     offset < semanticBlockOffsets[block + 1]; ++offset) {
                    const InstructionId instruction{semanticInstructions[offset]};
                    const std::size_t captureCount = std::min(
                        commitOperandCaptureCount(builder.view().opcode(instruction)),
                        builder.view().operands(instruction).size());
                    for (std::size_t position = 0; position < captureCount; ++position) {
                        VariableId source = builder.view().operands(instruction)[position];
                        if (source.value < liveSourceByPreCommitTarget.size() &&
                            liveSourceByPreCommitTarget[source.value].valid()) {
                            source = liveSourceByPreCommitTarget[source.value];
                        }
                        const ProgramView current = builder.view();
                        const auto &sourceVariable = current.variable(source);
                        if (current.init(sourceVariable.init).kind == InitKind::Constant) {
                            builder.setInstructionOperand(instruction, position, source);
                            continue;
                        }
                        auto found = captureTargetBySource.find(source.value);
                        if (found == captureTargetBySource.end()) {
                            const Type &type = current.type(sourceVariable.type);
                            if (type.kind != TypeKind::BitVector) {
                                diagnostics.error(
                                    "commit operand capture requires a bit-vector source",
                                    std::string(kDiagnosticContext));
                                return std::nullopt;
                            }
                            const VariableId target =
                                builder.addVariable(sourceVariable.type, builder.undefInit());
                            found = captureTargetBySource.emplace(source.value, target).first;
                            commitOperandCaptures.push_back(CommitOperandCapture{
                                .source = source,
                                .target = target,
                            });
                        }
                        builder.setInstructionOperand(instruction, position, found->second);
                    }
                    if (writerFrontierTargets[instruction.value] != kInvalidIndex) {
                        writerFrontierGuards[instruction.value] =
                            builder.view().operands(instruction).front();
                    }
                }
                commitOperandCaptureOffsets.push_back(
                    static_cast<uint32_t>(commitOperandCaptures.size()));
            }

            std::size_t edgeCursor = 0;
            builder.beginBlock();
            if (!appendWatchGroups(builder, 0, activationEdges, edgeCursor, eventType,
                                   diagnostics)) {
                return std::nullopt;
            }
            builder.endBlock();

            for (uint32_t block = 1; block <= normalBlockCount; ++block) {
                builder.beginBlock();
                if (block < semanticBlockOffsets.size() - 1) {
                    for (uint32_t offset = semanticBlockOffsets[block];
                         offset < semanticBlockOffsets[block + 1]; ++offset) {
                        const uint32_t instruction = semanticInstructions[offset];
                        if (writerFrontierTargets[instruction] != kInvalidIndex) {
                            const VariableId localGuard =
                                builder.addVariable(eventType, builder.undefInit());
                            const std::array<VariableId, 1> guardResults = {localGuard};
                            const std::array<VariableId, 1> guardOperands = {
                                writerFrontierGuards[instruction],
                            };
                            const InstructionId snapshot = builder.addInstruction(
                                Opcode::ReduceOr, guardResults, guardOperands);
                            builder.appendBlockInstruction(snapshot);
                            const std::array<VariableId, 1> activateOperands = {localGuard};
                            const InstructionId activate =
                                builder.addInstruction(Opcode::ActForward, {}, activateOperands);
                            const std::array<BlockId, 1> targets = {
                                BlockId{writerFrontierTargets[instruction]},
                            };
                            builder.setActivationTargets(activate, targets);
                            builder.appendBlockInstruction(activate);
                        }
                        builder.appendBlockInstruction(InstructionId{instruction});
                    }
                }
                if (!appendWatchGroups(builder, block, activationEdges, edgeCursor, eventType,
                                       diagnostics)) {
                    return std::nullopt;
                }
                builder.endBlock();
            }
            if (edgeCursor != activationEdges.size()) {
                diagnostics.error("internal error: not all AM activation edges were materialized",
                                  std::string(kDiagnosticContext));
                return std::nullopt;
            }

            ExecutableModel model{
                .program = builder.finish(),
                .interface = std::move(interface),
                .commitBlockBegin = commitBlockBegin,
                .commitBlockEnd = commitBlockEnd,
                .commitBlockOrder = commitPlan.order,
                .commitGroupOffsets = commitPlan.groupOffsets,
                .commitOperandCaptures = std::move(commitOperandCaptures),
                .commitOperandCaptureOffsets =
                    std::move(commitOperandCaptureOffsets),
            };
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
                        std::to_string(blockCountsByClass[readyIndex(BlockClass::Compute)]) +
                        " commit_blocks=" +
                        std::to_string(blockCountsByClass[readyIndex(BlockClass::Commit)]) +
                        " commit_groups=" +
                        std::to_string(model.commitGroupOffsets.empty()
                                           ? 0
                                           : model.commitGroupOffsets.size() - 1) +
                        " commit_operand_captures=" +
                        std::to_string(model.commitOperandCaptures.size()) +
                        " detectors=" + std::to_string(materialization.detectors) +
                        " activation_edges=" +
                        std::to_string(materialization.targets + writerFrontierActivations) +
                        " writer_frontier_activations=" +
                        std::to_string(writerFrontierActivations) +
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
