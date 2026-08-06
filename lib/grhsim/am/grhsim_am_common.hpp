#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMMON_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMMON_HPP

#include "core/diagnostics.hpp"
#include "grhsim/am/grhsim_am_graph_partition.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_opcode_traits.hpp"
#include "grhsim/am/grhsim_am_program_validate.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Scheduling helpers shared by the grhsim AM scheduling stages (split,
// partition, materialize). Everything is inline so the stages can live in
// separate translation units; this header is internal to lib/grhsim/am.
namespace wolvrix::lib::grhsim::am::detail
{

    constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();
    constexpr std::string_view kDiagnosticContext = "grhsim-am-production-activity-schedule";

    inline bool isChanged(Opcode opcode) noexcept
    {
        return opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
               opcode == Opcode::ChangedNeg;
    }

    inline bool reportValidation(const ValidationResult &validation,
                                 wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        for (const std::string &error : validation.errors) {
            diagnostics.error(error, std::string(kDiagnosticContext));
        }
        return validation.success();
    }

    // DefUseIndex / OrderEdge / CsrGraph are split-stage working types,
    // declared publicly in include/grhsim/am/grhsim_am_graph_split.hpp.

    inline bool isDependencyOperand(ProgramView program, InstructionId instruction,
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

    inline DefUseIndex buildDefUseIndex(ProgramView program)
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

    struct DirectedEdge
    {
        uint32_t source = 0;
        uint32_t target = 0;
        bool hard = false;
    };

    inline CsrGraph buildInstructionGraph(uint32_t instructionCount, const DefUseIndex &defUse,
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

    inline CsrGraph buildCondensationGraph(const CsrGraph &source,
                                           std::span<const uint32_t> component,
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

    inline SccResult findStronglyConnectedComponents(const CsrGraph &graph)
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

    inline std::optional<VariableId> stateWriteTarget(ProgramView program,
                                                      InstructionId instruction)
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

    inline uint64_t exportedVariableWidth(ProgramView program, VariableId variable)
    {
        const Type &type = program.type(program.variable(variable).type);
        switch (type.kind) {
        case TypeKind::BitVector:
            return type.bitWidth;
        case TypeKind::Array:
            return static_cast<uint64_t>(type.bitWidth) * type.elementCount;
        case TypeKind::Real:
            return 64;
        case TypeKind::String:
            break;
        }
        return 0;
    }

    // Instruction-graph JSONL runs to tens of millions of lines on
    // XiangShan, so lines are assembled in a large scratch buffer and
    // flushed in chunks instead of per-line iostream traffic.
    class JsonlGraphWriter
    {
    public:
        explicit JsonlGraphWriter(std::ostream &output) : output_(output)
        {
            buffer_.reserve(kFlushThreshold * 2);
        }

        JsonlGraphWriter &raw(std::string_view text)
        {
            buffer_.append(text);
            return *this;
        }

        JsonlGraphWriter &number(uint64_t value)
        {
            std::array<char, 20> digits{};
            const auto [end, error] =
                std::to_chars(digits.data(), digits.data() + digits.size(), value);
            buffer_.append(digits.data(), end);
            return *this;
        }

        JsonlGraphWriter &boolean(bool value)
        {
            return raw(value ? std::string_view("true") : std::string_view("false"));
        }

        void endLine()
        {
            buffer_.push_back('\n');
            if (buffer_.size() >= kFlushThreshold) {
                flush();
            }
        }

        bool flush()
        {
            output_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            buffer_.clear();
            return output_.good();
        }

    private:
        static constexpr std::size_t kFlushThreshold = std::size_t{1} << 20;
        std::ostream &output_;
        std::string buffer_;
    };

    using CommitEventPart = std::pair<uint8_t, uint32_t>;
    using CommitInstructionEventKey = std::vector<CommitEventPart>;
    using CommitAtomEventKey = std::vector<CommitInstructionEventKey>;

    inline uint8_t changedEventKind(Opcode opcode) noexcept
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

    inline CommitEventPart canonicalCommitEvent(ProgramView program, const DefUseIndex &defUse,
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

    // Event operands trail the fixed operands of a state write. Returns
    // the first event-operand position (operandCount when none).
    inline std::size_t stateWriteEventBegin(Opcode opcode, std::size_t operandCount) noexcept
    {
        std::size_t eventBegin = operandCount;
        switch (opcode) {
        case Opcode::RegisterWrite:
            eventBegin = 2;
            break;
        case Opcode::MemoryWrite:
            eventBegin = 5;
            break;
        case Opcode::MemoryFill:
            eventBegin = 2;
            break;
        case Opcode::MemoryWriteLanes:
            eventBegin = 3;
            break;
        default:
            break;
        }
        return std::min(eventBegin, operandCount);
    }

    // The commit bucket key of a state write is its canonical event
    // signature; the update condition is folded into nextValue and no
    // longer exists as an operand, so there is no guard component.
    inline CommitInstructionEventKey commitInstructionEventKey(ProgramView program,
                                                               const DefUseIndex &defUse,
                                                               InstructionId instruction)
    {
        const auto operands = program.operands(instruction);
        CommitInstructionEventKey key;
        const std::size_t eventBegin =
            stateWriteEventBegin(program.opcode(instruction), operands.size());
        key.reserve(operands.size() - eventBegin);
        for (std::size_t index = eventBegin; index < operands.size(); ++index) {
            key.push_back(canonicalCommitEvent(program, defUse, operands[index]));
        }
        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());
        return key;
    }

    inline bool isEffectfulForCycle(InstructionEffect effect) noexcept
    {
        return effect != InstructionEffect::Pure && effect != InstructionEffect::StateRead;
    }

    inline bool orderAtomInstructions(std::span<uint32_t> members, ProgramView program,
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

    inline bool validateActivityInputShape(const AmGraphPartitionInput &input,
                                           std::string &error)
    {
        const uint32_t atomCount = input.atomCount;
        if (input.atomOffsets.size() != static_cast<std::size_t>(atomCount) + 1 ||
            input.atomInstructions.size() != atomCount ||
            input.atomStateWrites.size() != atomCount ||
            input.atomIsCommit.size() != atomCount ||
            input.atomMinInstruction.size() != atomCount ||
            input.commitEventRank.size() != atomCount ||
            input.definitions.size() != input.variableCount ||
            input.useOffsets.size() != static_cast<std::size_t>(input.variableCount) + 1 ||
            (!input.variableCopyWeights.empty() &&
             input.variableCopyWeights.size() != input.variableCount)) {
            error = "internal error: malformed coarsen-dp block formation input";
            return false;
        }
        return true;
    }

    inline bool validateSplitShape(const AmGraphPartitionInput &input,
                                   const AmGraphSplit &split, std::string &error)
    {
        if (split.computeGraph.localOfAtom.size() != input.atomCount ||
            split.commitGraph.localOfAtom.size() != input.atomCount ||
            split.computeGraph.globalOfAtom.size() != split.computeGraph.atomCount ||
            split.commitGraph.globalOfAtom.size() != split.commitGraph.atomCount ||
            split.computeGraph.offsets.size() !=
                static_cast<std::size_t>(split.computeGraph.atomCount) + 1 ||
            split.commitGraph.offsets.size() !=
                static_cast<std::size_t>(split.commitGraph.atomCount) + 1) {
            error = "internal error: malformed compute/commit graph split";
            return false;
        }
        return true;
    }

} // namespace wolvrix::lib::grhsim::am::detail

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_COMMON_HPP
