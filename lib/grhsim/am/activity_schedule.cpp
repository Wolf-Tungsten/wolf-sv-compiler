#include "grhsim/am/activity_schedule.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();
        // Aligns with the legacy guard/event merge op cap for one commit block.
        constexpr std::size_t kMaxGuardEventMergeOps = 4096;
        constexpr std::size_t kCoarsenTailLargeClusterThreshold = 100000;
        constexpr std::size_t kCoarsenTailMaxClusterDeltaExclusive = 1024;
        constexpr std::size_t kCoarsenTailMaxConsecutiveIters = 3;
        constexpr std::size_t kMaxCoarsenIterations = 256;

        uint64_t elapsedMs(std::chrono::steady_clock::time_point start)
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }

        struct DisjointSet
        {
            std::vector<uint32_t> parent;
            std::vector<uint64_t> weight; // cluster instruction count
            std::vector<uint32_t> minInstruction;
            std::vector<uint8_t> oversized;
            std::vector<uint32_t> minLevel;
            std::vector<uint32_t> maxLevel;

            uint32_t find(uint32_t value)
            {
                uint32_t root = value;
                while (parent[root] != root) {
                    root = parent[root];
                }
                while (parent[value] != root) {
                    const uint32_t next = parent[value];
                    parent[value] = root;
                    value = next;
                }
                return root;
            }

            uint32_t unite(uint32_t lhs, uint32_t rhs)
            {
                lhs = find(lhs);
                rhs = find(rhs);
                if (lhs == rhs) {
                    return lhs;
                }
                if (rhs < lhs) {
                    std::swap(lhs, rhs);
                }
                parent[rhs] = lhs;
                weight[lhs] += weight[rhs];
                minInstruction[lhs] = std::min(minInstruction[lhs], minInstruction[rhs]);
                oversized[lhs] = static_cast<uint8_t>(oversized[lhs] | oversized[rhs]);
                minLevel[lhs] = std::min(minLevel[lhs], minLevel[rhs]);
                maxLevel[lhs] = std::max(maxLevel[lhs], maxLevel[rhs]);
                return lhs;
            }
        };

        struct ClusterGraph
        {
            uint32_t count = 0;
            std::vector<uint32_t> clusterOfAtom; // atomCount, kInvalidIndex for commit atoms
            std::vector<uint32_t> rootOf;        // dense cluster -> representative atom
            std::vector<uint32_t> outOffsets;
            std::vector<uint32_t> outTargets;
            std::vector<uint32_t> inOffsets;
            std::vector<uint32_t> inTargets;
        };
    } // namespace

    std::optional<GrhSimAmActivityScheduleResult>
    scheduleGrhSimAmActivityBlocks(const GrhSimAmActivityScheduleInput &input, std::string &error)
    {
        error.clear();
        const uint32_t atomCount = input.atomCount;
        const auto fail = [&](std::string message) {
            error = std::move(message);
            return std::optional<GrhSimAmActivityScheduleResult>();
        };
        if (input.atomOffsets.size() != static_cast<std::size_t>(atomCount) + 1 ||
            input.atomInstructions.size() != atomCount || input.atomStateWrites.size() != atomCount ||
            input.atomIsCommit.size() != atomCount ||
            input.atomMinInstruction.size() != atomCount ||
            input.commitEventRank.size() != atomCount ||
            input.commitGuardRank.size() != atomCount ||
            input.definitions.size() != input.variableCount ||
            input.useOffsets.size() != static_cast<std::size_t>(input.variableCount) + 1 ||
            (!input.variableCopyWeights.empty() &&
             input.variableCopyWeights.size() != input.variableCount)) {
            return fail("internal error: malformed coarsen-dp block formation input");
        }
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            if (input.atomIsCommit[atom] == 0) {
                continue;
            }
            for (uint32_t offset = input.atomOffsets[atom]; offset < input.atomOffsets[atom + 1];
                 ++offset) {
                const uint32_t target = input.atomTargets[offset];
                if (target >= atomCount) {
                    return fail("internal error: atom DAG target out of range");
                }
                if (input.atomIsCommit[target] == 0) {
                    return fail("AM dependency requires a state commit before pre-commit work");
                }
            }
        }

        const auto coarsenStart = std::chrono::steady_clock::now();

        DisjointSet dsu;
        dsu.parent.resize(atomCount);
        dsu.weight.resize(atomCount);
        dsu.minInstruction.resize(atomCount);
        dsu.oversized.resize(atomCount);
        dsu.minLevel.resize(atomCount, 0);
        dsu.maxLevel.resize(atomCount, 0);
        uint32_t computeAtomCount = 0;
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            dsu.parent[atom] = atom;
            dsu.weight[atom] = input.atomInstructions[atom];
            dsu.minInstruction[atom] = input.atomMinInstruction[atom];
            dsu.oversized[atom] = static_cast<uint8_t>(
                input.atomInstructions[atom] > input.maxInstructionsPerBlock ? 1 : 0);
            computeAtomCount += input.atomIsCommit[atom] == 0 ? 1U : 0U;
        }

        // Rebuilds the dense cluster DAG from the current union-find state.
        // Cluster ids are assigned in atom order so every round is deterministic.
        std::vector<uint32_t> denseOfRoot(atomCount, kInvalidIndex);
        std::vector<uint32_t> touchedRoots;
        touchedRoots.reserve(computeAtomCount);
        ClusterGraph graph;
        const auto rebuildClusterGraph = [&]() {
            graph.clusterOfAtom.assign(atomCount, kInvalidIndex);
            graph.rootOf.clear();
            touchedRoots.clear();
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (input.atomIsCommit[atom] != 0) {
                    continue;
                }
                const uint32_t root = dsu.find(atom);
                uint32_t dense = denseOfRoot[root];
                if (dense == kInvalidIndex) {
                    dense = static_cast<uint32_t>(graph.rootOf.size());
                    denseOfRoot[root] = dense;
                    touchedRoots.push_back(root);
                    graph.rootOf.push_back(root);
                }
                graph.clusterOfAtom[atom] = dense;
            }
            for (const uint32_t root : touchedRoots) {
                denseOfRoot[root] = kInvalidIndex;
            }
            graph.count = static_cast<uint32_t>(graph.rootOf.size());
            graph.outOffsets.assign(static_cast<std::size_t>(graph.count) + 1, 0);
            std::vector<uint32_t> targetMark(graph.count, 0);
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                const uint32_t source = graph.clusterOfAtom[atom];
                if (source == kInvalidIndex) {
                    continue;
                }
                for (uint32_t offset = input.atomOffsets[atom];
                     offset < input.atomOffsets[atom + 1]; ++offset) {
                    const uint32_t target = graph.clusterOfAtom[input.atomTargets[offset]];
                    if (target == kInvalidIndex || target == source ||
                        targetMark[target] == source + 1) {
                        continue;
                    }
                    targetMark[target] = source + 1;
                    ++graph.outOffsets[source + 1];
                }
            }
            std::partial_sum(graph.outOffsets.begin(), graph.outOffsets.end(),
                             graph.outOffsets.begin());
            graph.outTargets.resize(graph.outOffsets.back());
            std::fill(targetMark.begin(), targetMark.end(), 0);
            std::vector<uint32_t> cursor(graph.outOffsets.begin(), graph.outOffsets.end() - 1);
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                const uint32_t source = graph.clusterOfAtom[atom];
                if (source == kInvalidIndex) {
                    continue;
                }
                for (uint32_t offset = input.atomOffsets[atom];
                     offset < input.atomOffsets[atom + 1]; ++offset) {
                    const uint32_t target = graph.clusterOfAtom[input.atomTargets[offset]];
                    if (target == kInvalidIndex || target == source ||
                        targetMark[target] == source + 1) {
                        continue;
                    }
                    targetMark[target] = source + 1;
                    graph.outTargets[cursor[source]++] = target;
                }
            }
            graph.inOffsets.assign(static_cast<std::size_t>(graph.count) + 1, 0);
            for (const uint32_t target : graph.outTargets) {
                ++graph.inOffsets[target + 1];
            }
            std::partial_sum(graph.inOffsets.begin(), graph.inOffsets.end(),
                             graph.inOffsets.begin());
            graph.inTargets.resize(graph.inOffsets.back());
            cursor.assign(graph.inOffsets.begin(), graph.inOffsets.end() - 1);
            for (uint32_t source = 0; source < graph.count; ++source) {
                for (uint32_t offset = graph.outOffsets[source];
                     offset < graph.outOffsets[source + 1]; ++offset) {
                    graph.inTargets[cursor[graph.outTargets[offset]]++] = source;
                }
            }
        };

        // Coarsening runs one merge rule per round with a cluster-graph rebuild
        // between rounds. Contracting an edge u->v creates a cycle iff an indirect
        // path u~>v exists, so merges in one round only touch round-start clusters
        // (mergeMark blocks a second merge within the round): an out-degree-1
        // cluster can only reach its unique successor through that one edge, an
        // in-degree-1 cluster can only be reached from its unique predecessor
        // through that one edge, and equal longest-path levels rule out any path
        // between sibling pairs. Mixing rules inside one round is not safe: an
        // out1-merged cluster absorbs extra in-edges that invalidate the
        // in-degree-1 argument.
        enum class CoarsenPass : uint8_t
        {
            Out1 = 0,
            In1 = 1,
            Sibling = 2,
        };
        std::vector<uint32_t> level;
        std::vector<uint32_t> mergeMark(atomCount, 0);
        uint32_t passStamp = 0;
        std::vector<std::pair<uint32_t, uint32_t>> siblingBuffer;
        std::size_t lastRoundMerges = 0;
        const auto coarsenRound = [&](CoarsenPass pass) -> bool {
            bool changed = false;
            ++passStamp;
            const auto tryMerge = [&](uint32_t lhs, uint32_t rhs) {
                lhs = dsu.find(lhs);
                rhs = dsu.find(rhs);
                if (lhs == rhs || mergeMark[lhs] == passStamp ||
                    mergeMark[rhs] == passStamp || dsu.oversized[lhs] != 0 ||
                    dsu.oversized[rhs] != 0 ||
                    dsu.weight[lhs] + dsu.weight[rhs] > input.coarsenBudget) {
                    return false;
                }
                mergeMark[dsu.unite(lhs, rhs)] = passStamp;
                ++lastRoundMerges;
                return true;
            };
            if (pass == CoarsenPass::Out1) {
                for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                    if (graph.outOffsets[cluster + 1] - graph.outOffsets[cluster] == 1 &&
                        tryMerge(graph.rootOf[cluster],
                                 graph.rootOf[graph.outTargets[graph.outOffsets[cluster]]])) {
                        changed = true;
                    }
                }
                return changed;
            }
            if (pass == CoarsenPass::In1) {
                for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                    if (graph.inOffsets[cluster + 1] - graph.inOffsets[cluster] == 1 &&
                        tryMerge(graph.rootOf[graph.inTargets[graph.inOffsets[cluster]]],
                                 graph.rootOf[cluster])) {
                        changed = true;
                    }
                }
                return changed;
            }

            // Sibling pass: longest-path levels on the round-start cluster DAG.
            level.assign(graph.count, 0);
            std::vector<uint32_t> indegree(graph.count, 0);
            std::vector<uint32_t> queue;
            queue.reserve(graph.count);
            for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                indegree[cluster] = graph.inOffsets[cluster + 1] - graph.inOffsets[cluster];
                if (indegree[cluster] == 0) {
                    queue.push_back(cluster);
                }
            }
            for (std::size_t head = 0; head < queue.size(); ++head) {
                const uint32_t source = queue[head];
                for (uint32_t offset = graph.outOffsets[source];
                     offset < graph.outOffsets[source + 1]; ++offset) {
                    const uint32_t target = graph.outTargets[offset];
                    level[target] = std::max(level[target], level[source] + 1);
                    if (--indegree[target] == 0) {
                        queue.push_back(target);
                    }
                }
            }
            if (queue.size() != graph.count) {
                return false; // cyclic round-start graph; leave clusters untouched
            }
            for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                const uint32_t root = graph.rootOf[cluster];
                dsu.minLevel[root] = level[cluster];
                dsu.maxLevel[root] = level[cluster];
            }
            for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                const uint32_t predRoot = dsu.find(graph.rootOf[cluster]);
                siblingBuffer.clear();
                for (uint32_t offset = graph.outOffsets[cluster];
                     offset < graph.outOffsets[cluster + 1]; ++offset) {
                    const uint32_t root = dsu.find(graph.rootOf[graph.outTargets[offset]]);
                    if (root == predRoot || dsu.minLevel[root] != dsu.maxLevel[root]) {
                        continue;
                    }
                    siblingBuffer.emplace_back(dsu.minLevel[root], root);
                }
                if (siblingBuffer.size() < 2) {
                    continue;
                }
                std::sort(siblingBuffer.begin(), siblingBuffer.end());
                uint32_t representative = kInvalidIndex;
                uint32_t previousRoot = kInvalidIndex;
                uint32_t previousLevel = kInvalidIndex;
                for (const auto [siblingLevel, root] : siblingBuffer) {
                    if (root == previousRoot) {
                        continue;
                    }
                    previousRoot = root;
                    if (siblingLevel != previousLevel) {
                        previousLevel = siblingLevel;
                        representative = kInvalidIndex;
                    }
                    if (representative == kInvalidIndex) {
                        representative = root;
                        continue;
                    }
                    if (tryMerge(representative, root)) {
                        representative = dsu.find(representative);
                        changed = true;
                    } else {
                        representative = root;
                    }
                }
            }
            return changed;
        };

        std::size_t tailIterations = 0;
        uint32_t idlePassMask = 0;
        std::size_t mergeCounts[3] = {0, 0, 0};
        std::size_t roundsUsed = 0;
        std::string degreeHistogram;
        CoarsenPass pass = CoarsenPass::Out1;
        for (std::size_t iteration = 0;
             input.enableCoarsening && iteration < kMaxCoarsenIterations && idlePassMask != 0x7;
             ++iteration) {
            rebuildClusterGraph();
            if (iteration == 0) {
                const auto bucketize = [&](bool outgoing) {
                    std::array<std::size_t, 8> buckets{};
                    std::size_t total = 0;
                    for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                        const std::size_t degree =
                            outgoing ? graph.outOffsets[cluster + 1] - graph.outOffsets[cluster]
                                     : graph.inOffsets[cluster + 1] - graph.inOffsets[cluster];
                        total += degree;
                        const std::size_t bucket =
                            degree == 0 ? 0
                            : degree == 1 ? 1
                            : degree == 2 ? 2
                            : degree == 3 ? 3
                            : degree <= 7 ? 4
                            : degree <= 15 ? 5
                            : degree <= 63 ? 6
                                          : 7;
                        ++buckets[bucket];
                    }
                    std::string text = " total=" + std::to_string(total);
                    const char *labels[8] = {"0", "1", "2", "3", "4-7", "8-15", "16-63", ">=64"};
                    for (std::size_t index = 0; index < buckets.size(); ++index) {
                        text += " ";
                        text += labels[index];
                        text += "=";
                        text += std::to_string(buckets[index]);
                    }
                    return text;
                };
                degreeHistogram = "clusters=" + std::to_string(graph.count) +
                                  " outdeg{" + bucketize(true) + " } indeg{" + bucketize(false) +
                                  " }";
            }
            const std::size_t clustersBefore = graph.count;
            lastRoundMerges = 0;
            const bool changed = coarsenRound(pass);
            mergeCounts[static_cast<uint8_t>(pass)] += lastRoundMerges;
            ++roundsUsed;
            const uint32_t passBit = 1U << static_cast<uint8_t>(pass);
            idlePassMask = changed ? (idlePassMask & ~passBit) : (idlePassMask | passBit);
            const std::size_t clusterDelta = lastRoundMerges;
            const bool smallDeltaTail =
                clustersBefore >= kCoarsenTailLargeClusterThreshold &&
                clusterDelta < kCoarsenTailMaxClusterDeltaExclusive;
            tailIterations = smallDeltaTail ? tailIterations + 1 : 0;
            if (tailIterations >= kCoarsenTailMaxConsecutiveIters) {
                break;
            }
            pass = pass == CoarsenPass::Out1
                       ? CoarsenPass::In1
                       : (pass == CoarsenPass::In1 ? CoarsenPass::Sibling : CoarsenPass::Out1);
        }
        rebuildClusterGraph();
        const uint64_t coarsenMs = elapsedMs(coarsenStart);

        // Deterministic Kahn over the cluster DAG: ties break on the smallest
        // member instruction index, then on the dense cluster id.
        const auto dpStart = std::chrono::steady_clock::now();
        std::vector<uint32_t> clusterOrder;
        clusterOrder.reserve(graph.count);
        {
            using Candidate = std::tuple<uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            std::vector<uint32_t> indegree(graph.count, 0);
            for (uint32_t cluster = 0; cluster < graph.count; ++cluster) {
                indegree[cluster] = graph.inOffsets[cluster + 1] - graph.inOffsets[cluster];
                if (indegree[cluster] == 0) {
                    ready.emplace(dsu.minInstruction[graph.rootOf[cluster]], cluster);
                }
            }
            while (!ready.empty()) {
                const uint32_t cluster = std::get<1>(ready.top());
                ready.pop();
                clusterOrder.push_back(cluster);
                for (uint32_t offset = graph.outOffsets[cluster];
                     offset < graph.outOffsets[cluster + 1]; ++offset) {
                    const uint32_t target = graph.outTargets[offset];
                    if (--indegree[target] == 0) {
                        ready.emplace(dsu.minInstruction[graph.rootOf[target]], target);
                    }
                }
            }
        }
        if (clusterOrder.size() != graph.count) {
            return fail("internal error: AM coarsened cluster graph is cyclic");
        }
        std::vector<uint32_t> topoPos(graph.count, 0);
        for (uint32_t pos = 0; pos < graph.count; ++pos) {
            topoPos[clusterOrder[pos]] = pos;
        }

        // Variable-granularity activation costs. A variable defined outside the
        // compute atoms (commit-defined or input-like without a definition) is a
        // permanent boundary: it contributes to the incoming cost of every
        // consuming segment and is never subtracted back.
        std::vector<uint32_t> sourcePos(input.variableCount, kInvalidIndex);
        for (uint32_t variable = 0; variable < input.variableCount; ++variable) {
            const uint32_t definition = input.definitions[variable];
            if (definition == kInvalidIndex || definition >= input.instructionAtom.size()) {
                continue;
            }
            const uint32_t cluster = graph.clusterOfAtom[input.instructionAtom[definition]];
            if (cluster != kInvalidIndex) {
                sourcePos[variable] = topoPos[cluster];
            }
        }
        std::vector<uint32_t> sourceOffsets(static_cast<std::size_t>(graph.count) + 1, 0);
        for (const uint32_t pos : sourcePos) {
            if (pos != kInvalidIndex) {
                ++sourceOffsets[pos + 1];
            }
        }
        std::partial_sum(sourceOffsets.begin(), sourceOffsets.end(), sourceOffsets.begin());
        std::vector<uint32_t> sourceValues(sourceOffsets.back());
        {
            std::vector<uint32_t> cursor(sourceOffsets.begin(), sourceOffsets.end() - 1);
            for (uint32_t variable = 0; variable < input.variableCount; ++variable) {
                if (sourcePos[variable] != kInvalidIndex) {
                    sourceValues[cursor[sourcePos[variable]]++] = variable;
                }
            }
        }
        std::vector<uint32_t> targetOffsets(static_cast<std::size_t>(graph.count) + 1, 0);
        std::vector<uint32_t> clusterMark(graph.count, 0);
        for (uint32_t variable = 0; variable < input.variableCount; ++variable) {
            for (uint32_t offset = input.useOffsets[variable];
                 offset < input.useOffsets[variable + 1]; ++offset) {
                const uint32_t use = input.uses[offset];
                if (use >= input.instructionAtom.size()) {
                    return fail("internal error: use instruction out of range");
                }
                const uint32_t cluster = graph.clusterOfAtom[input.instructionAtom[use]];
                if (cluster == kInvalidIndex) {
                    continue; // uses inside commit atoms do not participate in this DP
                }
                const uint32_t pos = topoPos[cluster];
                if (clusterMark[pos] == variable + 1) {
                    continue;
                }
                clusterMark[pos] = variable + 1;
                ++targetOffsets[pos + 1];
            }
        }
        std::partial_sum(targetOffsets.begin(), targetOffsets.end(), targetOffsets.begin());
        std::vector<uint32_t> targetValues(targetOffsets.back());
        {
            std::fill(clusterMark.begin(), clusterMark.end(), 0);
            std::vector<uint32_t> cursor(targetOffsets.begin(), targetOffsets.end() - 1);
            for (uint32_t variable = 0; variable < input.variableCount; ++variable) {
                for (uint32_t offset = input.useOffsets[variable];
                     offset < input.useOffsets[variable + 1]; ++offset) {
                    const uint32_t cluster =
                        graph.clusterOfAtom[input.instructionAtom[input.uses[offset]]];
                    if (cluster == kInvalidIndex) {
                        continue;
                    }
                    const uint32_t pos = topoPos[cluster];
                    if (clusterMark[pos] == variable + 1) {
                        continue;
                    }
                    clusterMark[pos] = variable + 1;
                    targetValues[cursor[pos]++] = variable;
                }
            }
        }

        // Segment DP ported from the legacy activity schedule: minimize the
        // deduplicated incoming-copy cost of each segment (per-variable unit
        // cost, or variableCopyWeights when provided — e.g. ceil(width/64),
        // matching the runtime copy count), plus one segmentPenalty per
        // segment. Oversized singleton clusters are allowed to form their own
        // segment.
        const auto copyCostOf = [&](uint32_t variable) -> double {
            return input.variableCopyWeights.empty()
                       ? 1.0
                       : static_cast<double>(input.variableCopyWeights[variable]);
        };
        const std::size_t count = graph.count;
        const std::size_t maxNodes = input.maxInstructionsPerBlock;
        std::vector<std::size_t> prefixSize(count + 1, 0);
        for (std::size_t index = 0; index < count; ++index) {
            prefixSize[index + 1] =
                prefixSize[index] +
                static_cast<std::size_t>(dsu.weight[graph.rootOf[clusterOrder[index]]]);
        }
        constexpr double kInf = std::numeric_limits<double>::infinity();
        std::vector<double> dp(count + 1, kInf);
        std::vector<std::size_t> prev(count + 1, 0);
        std::vector<uint32_t> targetSeen(input.variableCount, 0);
        std::vector<uint32_t> countedIncoming(input.variableCount, 0);
        dp[0] = 0.0;
        for (std::size_t end = 1; end <= count; ++end) {
            const uint32_t stamp = static_cast<uint32_t>(end);
            double incomingActivationCost = 0.0;
            for (std::size_t begin = end; begin > 0; --begin) {
                const std::size_t start = begin - 1;
                const std::size_t size = prefixSize[end] - prefixSize[start];
                if (size > maxNodes && start + 1 < end) {
                    break;
                }
                if (size > maxNodes && start + 1 == end) {
                    continue;
                }
                for (uint32_t offset = targetOffsets[start]; offset < targetOffsets[start + 1];
                     ++offset) {
                    const uint32_t variable = targetValues[offset];
                    if (targetSeen[variable] == stamp) {
                        continue;
                    }
                    targetSeen[variable] = stamp;
                    if (sourcePos[variable] == kInvalidIndex || sourcePos[variable] < start) {
                        countedIncoming[variable] = stamp;
                        incomingActivationCost += copyCostOf(variable);
                    }
                }
                for (uint32_t offset = sourceOffsets[start]; offset < sourceOffsets[start + 1];
                     ++offset) {
                    const uint32_t variable = sourceValues[offset];
                    if (countedIncoming[variable] == stamp) {
                        countedIncoming[variable] = 0;
                        incomingActivationCost -= copyCostOf(variable);
                    }
                }
                if (dp[start] == kInf) {
                    continue;
                }
                const double candidate = dp[start] + incomingActivationCost + input.segmentPenalty;
                if (candidate + 1e-12 < dp[end] ||
                    (std::fabs(candidate - dp[end]) <= 1e-12 &&
                     (end - start) > (end - prev[end]))) {
                    dp[end] = candidate;
                    prev[end] = start;
                }
            }
            if (dp[end] == kInf) {
                dp[end] = dp[end - 1] + 1.0;
                prev[end] = end - 1;
            }
        }
        std::vector<uint32_t> segmentOfPos(count, 0);
        uint32_t computeBlockCount = 0;
        for (std::size_t end = count; end > 0;) {
            const std::size_t begin = prev[end];
            ++computeBlockCount;
            for (std::size_t pos = begin; pos < end; ++pos) {
                segmentOfPos[pos] = computeBlockCount;
            }
            end = begin;
        }
        // Segments were numbered from the back; flip to forward block numbers.
        for (uint32_t &segment : segmentOfPos) {
            segment = computeBlockCount - segment + 1;
        }
        const uint64_t dpMs = elapsedMs(dpStart);

        GrhSimAmActivityScheduleResult result;
        result.atomBlock.assign(atomCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            const uint32_t cluster = graph.clusterOfAtom[atom];
            if (cluster != kInvalidIndex) {
                result.atomBlock[atom] = segmentOfPos[topoPos[cluster]];
            }
        }

        // Compute atoms first: a block-grouped Kahn keeps atomTopo topological
        // both across and inside blocks.
        result.atomTopo.reserve(atomCount);
        {
            using Candidate = std::tuple<uint32_t, uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            std::vector<uint32_t> indegree(atomCount, 0);
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (input.atomIsCommit[atom] != 0) {
                    continue;
                }
                for (uint32_t offset = input.atomOffsets[atom];
                     offset < input.atomOffsets[atom + 1]; ++offset) {
                    const uint32_t target = input.atomTargets[offset];
                    if (input.atomIsCommit[target] == 0) {
                        ++indegree[target];
                    }
                }
            }
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (input.atomIsCommit[atom] == 0 && indegree[atom] == 0) {
                    ready.emplace(result.atomBlock[atom], input.atomMinInstruction[atom], atom);
                }
            }
            while (!ready.empty()) {
                const uint32_t atom = std::get<2>(ready.top());
                ready.pop();
                result.atomTopo.push_back(atom);
                for (uint32_t offset = input.atomOffsets[atom];
                     offset < input.atomOffsets[atom + 1]; ++offset) {
                    const uint32_t target = input.atomTargets[offset];
                    if (input.atomIsCommit[target] == 0 && --indegree[target] == 0) {
                        ready.emplace(result.atomBlock[target], input.atomMinInstruction[target],
                                      target);
                    }
                }
            }
        }
        if (result.atomTopo.size() != computeAtomCount) {
            return fail("internal error: AM compute atom graph is cyclic");
        }

        // Commit atoms form blocks after all compute blocks: Kahn on the commit
        // subgraph prioritized by (event rank, guard rank, min instruction), with
        // bounded merging inside one commit-events bucket under the block limit.
        uint32_t normalBlockCount = computeBlockCount;
        {
            using Candidate = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            std::vector<uint32_t> indegree(atomCount, 0);
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (input.atomIsCommit[atom] == 0) {
                    continue;
                }
                for (uint32_t offset = input.atomOffsets[atom];
                     offset < input.atomOffsets[atom + 1]; ++offset) {
                    ++indegree[input.atomTargets[offset]];
                }
            }
            for (uint32_t atom = 0; atom < atomCount; ++atom) {
                if (input.atomIsCommit[atom] != 0 && indegree[atom] == 0) {
                    ready.emplace(input.commitEventRank[atom], input.commitGuardRank[atom],
                                  input.atomMinInstruction[atom], atom);
                }
            }
            const std::size_t commitMergeLimit =
                std::min(input.maxCommitInstructionsPerBlock, kMaxGuardEventMergeOps);
            bool haveCurrent = false;
            std::size_t currentInstructions = 0;
            uint32_t currentBucketAtom = kInvalidIndex;
            while (!ready.empty()) {
                const Candidate top = ready.top();
                const uint32_t atom = std::get<3>(top);
                if (haveCurrent &&
                    (input.commitEventRank[atom] != input.commitEventRank[currentBucketAtom] ||
                     currentInstructions + input.atomInstructions[atom] > commitMergeLimit)) {
                    haveCurrent = false;
                }
                if (!haveCurrent) {
                    ++normalBlockCount;
                    currentInstructions = 0;
                    currentBucketAtom = atom;
                    haveCurrent = true;
                }
                ready.pop();
                result.atomBlock[atom] = normalBlockCount;
                result.atomTopo.push_back(atom);
                currentInstructions += input.atomInstructions[atom];
                for (uint32_t offset = input.atomOffsets[atom];
                     offset < input.atomOffsets[atom + 1]; ++offset) {
                    const uint32_t target = input.atomTargets[offset];
                    if (--indegree[target] == 0) {
                        ready.emplace(input.commitEventRank[target],
                                      input.commitGuardRank[target],
                                      input.atomMinInstruction[target], target);
                    }
                }
            }
        }
        if (result.atomTopo.size() != atomCount) {
            return fail("internal error: AM commit atom graph is cyclic");
        }

        result.normalBlockCount = normalBlockCount;
        result.computeBlockCount = computeBlockCount;
        result.commitBlockCount = normalBlockCount - computeBlockCount;
        result.clustersAfterCoarsen = graph.count;
        result.dpSegments = computeBlockCount;
        result.coarsenMs = coarsenMs;
        result.dpMs = dpMs;
        result.coarsenRounds = roundsUsed;
        result.coarsenOut1Merges = mergeCounts[0];
        result.coarsenIn1Merges = mergeCounts[1];
        result.coarsenSiblingMerges = mergeCounts[2];
        result.initialDegreeHistogram = std::move(degreeHistogram);
        return result;
    }

} // namespace wolvrix::lib::grhsim::am
