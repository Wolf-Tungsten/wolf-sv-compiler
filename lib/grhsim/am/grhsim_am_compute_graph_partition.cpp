#include "grhsim/am/grhsim_am_compute_graph_partition.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    using namespace detail;

    namespace
    {
        // gsim mergeNodes.cpp:9: sibling-merge host member cap.
        constexpr std::size_t kMaxSiblingClusterMembers = 30;

        uint64_t elapsedMs(std::chrono::steady_clock::time_point start)
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }

        // Per-node adjacency during the merge sweeps: sorted unique vectors,
        // mirroring the reference's per-node sets (gsimpart.py nexts/prevs).
        void sortedInsert(std::vector<uint32_t> &values, uint32_t value)
        {
            const auto it = std::lower_bound(values.begin(), values.end(), value);
            if (it == values.end() || *it != value) {
                values.insert(it, value);
            }
        }

        void sortedErase(std::vector<uint32_t> &values, uint32_t value)
        {
            const auto it = std::lower_bound(values.begin(), values.end(), value);
            if (it != values.end() && *it == value) {
                values.erase(it);
            }
        }

        void sortedUnionInto(std::vector<uint32_t> &host, const std::vector<uint32_t> &extra)
        {
            if (extra.empty()) {
                return;
            }
            std::vector<uint32_t> merged;
            merged.reserve(host.size() + extra.size());
            std::set_union(host.begin(), host.end(), extra.begin(), extra.end(),
                           std::back_inserter(merged));
            host.swap(merged);
        }

        struct ClusterGraph
        {
            uint32_t count = 0;
            std::vector<uint32_t> clusterOfAtom; // computeAtomCount
            std::vector<uint32_t> rootOf;        // dense cluster -> representative local atom
            std::vector<uint32_t> outOffsets;
            std::vector<uint32_t> outTargets;
            std::vector<uint32_t> inOffsets;
            std::vector<uint32_t> inTargets;
        };

        // Post-DP local-move refinement. The initial partition assigns blocks
        // over a fixed topological sequence, so it cannot group clusters by
        // connectivity across sequence distance. This pass repairs
        // locality directly: in topo order it tries moving each cluster into
        // the block of a cluster-DAG neighbor, applying a move only when it
        // strictly reduces the exact block-level incoming-copy cost (a
        // variable counts once per (variable, block) pair when some
        // instruction in the block uses it and none defines it; permanent
        // boundaries count in every consuming block). All scans, candidate
        // orders and tie-breaks are deterministic, and the total cost
        // decreases monotonically, so the pass terminates.
        struct RefinementOutcome
        {
            std::size_t rounds = 0;
            std::size_t moves = 0;
            double costBefore = 0.0;
            double costAfter = 0.0;
        };

        RefinementOutcome
        refineClusterBlocks(const ClusterGraph &graph,
                            const AmGraphPartitionInput &input,
                            const AmComputeGraph &computeGraph,
                            std::span<const uint32_t> clusterWeights,
                            std::span<const uint32_t> clusterOrder,
                            std::span<const uint32_t> topoPos,
                            std::vector<uint32_t> &segmentOfPos,
                            uint32_t blockCount, std::size_t maxNodes,
                            std::size_t maxRounds)
        {
            RefinementOutcome outcome;
            const uint32_t clusterCount = graph.count;
            const uint32_t variableCount = input.variableCount;
            const uint32_t instructionCount =
                static_cast<uint32_t>(input.instructionAtom.size());

            // instruction -> cluster (kInvalidIndex for non-compute instructions).
            std::vector<uint32_t> clusterOfInstr(instructionCount, kInvalidIndex);
            for (uint32_t instruction = 0; instruction < instructionCount; ++instruction) {
                const uint32_t local = computeGraph.localOfAtom[input.instructionAtom[instruction]];
                if (local != kInvalidIndex) {
                    clusterOfInstr[instruction] = graph.clusterOfAtom[local];
                }
            }

            // Per-cluster (variable, using-instruction count) sets and per-cluster
            // defined-variable lists.
            std::vector<std::unordered_map<uint32_t, uint32_t>> useMaps(clusterCount);
            std::vector<std::vector<uint32_t>> defVars(clusterCount);
            std::vector<uint32_t> defClusterOfVar(variableCount, kInvalidIndex);
            for (uint32_t variable = 0; variable < variableCount; ++variable) {
                const uint32_t definition = input.definitions[variable];
                if (definition != kInvalidIndex && definition < instructionCount) {
                    const uint32_t cluster = clusterOfInstr[definition];
                    if (cluster != kInvalidIndex) {
                        defClusterOfVar[variable] = cluster;
                        defVars[cluster].push_back(variable);
                    }
                }
                uint32_t previous = kInvalidIndex;
                for (uint32_t offset = input.useOffsets[variable];
                     offset < input.useOffsets[variable + 1]; ++offset) {
                    const uint32_t use = input.uses[offset];
                    if (use == previous) {
                        continue;
                    }
                    previous = use;
                    const uint32_t cluster = clusterOfInstr[use];
                    if (cluster != kInvalidIndex) {
                        ++useMaps[cluster][variable];
                    }
                }
            }
            // Sorted CSR forms (deterministic scan order).
            std::vector<uint32_t> useEntryOffsets(clusterCount + 1, 0);
            std::vector<uint32_t> defVarOffsets(clusterCount + 1, 0);
            for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
                useEntryOffsets[cluster + 1] =
                    useEntryOffsets[cluster] + static_cast<uint32_t>(useMaps[cluster].size());
                defVarOffsets[cluster + 1] =
                    defVarOffsets[cluster] + static_cast<uint32_t>(defVars[cluster].size());
            }
            std::vector<uint32_t> useEntryVars(useEntryOffsets.back());
            std::vector<uint32_t> useEntryCounts(useEntryOffsets.back());
            std::vector<uint32_t> defVarList(defVarOffsets.back());
            for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
                uint32_t cursor = useEntryOffsets[cluster];
                for (const auto &[variable, count] : useMaps[cluster]) {
                    useEntryVars[cursor] = variable;
                    useEntryCounts[cursor] = count;
                    ++cursor;
                }
                const uint32_t begin = defVarOffsets[cluster];
                std::copy(defVars[cluster].begin(), defVars[cluster].end(),
                          defVarList.begin() + begin);
                std::sort(defVarList.begin() + begin, defVarList.begin() + defVarOffsets[cluster + 1]);
            }
            useMaps.clear();
            const auto ownUseCount = [&](uint32_t cluster, uint32_t variable) {
                for (uint32_t offset = useEntryOffsets[cluster];
                     offset < useEntryOffsets[cluster + 1]; ++offset) {
                    if (useEntryVars[offset] == variable) {
                        return useEntryCounts[offset];
                    }
                }
                return 0U;
            };

            // Current block state.
            std::vector<uint32_t> clusterBlock(clusterCount, 0);
            for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
                clusterBlock[cluster] = segmentOfPos[topoPos[cluster]];
            }
            std::vector<std::unordered_map<uint32_t, uint32_t>> blockUse(blockCount + 1);
            std::vector<std::size_t> blockSize(blockCount + 1, 0);
            std::vector<uint32_t> blockMembers(blockCount + 1, 0);
            for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
                const uint32_t block = clusterBlock[cluster];
                blockSize[block] += clusterWeights[cluster];
                ++blockMembers[block];
                for (uint32_t offset = useEntryOffsets[cluster];
                     offset < useEntryOffsets[cluster + 1]; ++offset) {
                    blockUse[block][useEntryVars[offset]] += useEntryCounts[offset];
                }
            }
            std::vector<uint32_t> defBlockOfVar(variableCount, kInvalidIndex);
            for (uint32_t variable = 0; variable < variableCount; ++variable) {
                if (defClusterOfVar[variable] != kInvalidIndex) {
                    defBlockOfVar[variable] = clusterBlock[defClusterOfVar[variable]];
                }
            }
            const auto blockUseCount = [&](uint32_t block, uint32_t variable) {
                const auto found = blockUse[block].find(variable);
                return found == blockUse[block].end() ? 0U : found->second;
            };
            double totalCost = 0.0;
            for (uint32_t block = 1; block <= blockCount; ++block) {
                for (const auto &[variable, count] : blockUse[block]) {
                    if (defBlockOfVar[variable] != block) {
                        totalCost += 1.0;
                    }
                }
            }
            outcome.costBefore = totalCost;

            std::vector<uint32_t> candidateStamp(blockCount + 1, 0);
            uint32_t stamp = 0;
            for (std::size_t round = 0; round < maxRounds; ++round) {
                std::size_t roundMoves = 0;
                for (const uint32_t cluster : clusterOrder) {
                    if (clusterWeights[cluster] > maxNodes) {
                        continue; // oversized clusters stay put
                    }
                    const uint32_t source = clusterBlock[cluster];
                    if (blockMembers[source] <= 1) {
                        continue; // never empty a block (keeps numbering stable)
                    }
                    // Topological legality: the DP assignment is def-before-use
                    // across blocks, and a move must keep it that way (a
                    // backward edge is rejected by the validator for changed
                    // results and would add convergence rounds for plain
                    // values). Cluster-DAG edges are exactly the def-use/order
                    // relations, so the block of the moved cluster must stay
                    // between its predecessors' and successors' blocks.
                    uint32_t minSuccessorBlock = blockCount;
                    uint32_t maxPredecessorBlock = 0;
                    for (uint32_t offset = graph.outOffsets[cluster];
                         offset < graph.outOffsets[cluster + 1]; ++offset) {
                        minSuccessorBlock = std::min(
                            minSuccessorBlock, clusterBlock[graph.outTargets[offset]]);
                    }
                    for (uint32_t offset = graph.inOffsets[cluster];
                         offset < graph.inOffsets[cluster + 1]; ++offset) {
                        maxPredecessorBlock = std::max(
                            maxPredecessorBlock, clusterBlock[graph.inTargets[offset]]);
                    }
                    ++stamp;
                    uint32_t bestTarget = kInvalidIndex;
                    double bestDelta = -1e-9;
                    const auto consider = [&](uint32_t neighbor) {
                        const uint32_t target = clusterBlock[neighbor];
                        if (target == source || candidateStamp[target] == stamp ||
                            target > minSuccessorBlock || target < maxPredecessorBlock ||
                            blockSize[target] + clusterWeights[cluster] > maxNodes) {
                            return;
                        }
                        candidateStamp[target] = stamp;
                        double delta = 0.0;
                        for (uint32_t offset = useEntryOffsets[cluster];
                             offset < useEntryOffsets[cluster + 1]; ++offset) {
                            const uint32_t variable = useEntryVars[offset];
                            if (defBlockOfVar[variable] != source &&
                                blockUseCount(source, variable) == useEntryCounts[offset]) {
                                delta -= 1.0;
                            }
                            if (defBlockOfVar[variable] != target &&
                                blockUseCount(target, variable) == 0) {
                                delta += 1.0;
                            }
                        }
                        for (uint32_t offset = defVarOffsets[cluster];
                             offset < defVarOffsets[cluster + 1]; ++offset) {
                            const uint32_t variable = defVarList[offset];
                            if (blockUseCount(source, variable) >
                                ownUseCount(cluster, variable)) {
                                delta += 1.0;
                            }
                            if (blockUseCount(target, variable) > 0) {
                                delta -= 1.0;
                            }
                        }
                        if (delta < bestDelta ||
                            (delta == bestDelta && target < bestTarget)) {
                            bestDelta = delta;
                            bestTarget = target;
                        }
                    };
                    for (uint32_t offset = graph.outOffsets[cluster];
                         offset < graph.outOffsets[cluster + 1]; ++offset) {
                        consider(graph.outTargets[offset]);
                    }
                    for (uint32_t offset = graph.inOffsets[cluster];
                         offset < graph.inOffsets[cluster + 1]; ++offset) {
                        consider(graph.inTargets[offset]);
                    }
                    if (bestTarget == kInvalidIndex) {
                        continue;
                    }
                    for (uint32_t offset = useEntryOffsets[cluster];
                         offset < useEntryOffsets[cluster + 1]; ++offset) {
                        const uint32_t variable = useEntryVars[offset];
                        blockUse[source][variable] -= useEntryCounts[offset];
                        blockUse[bestTarget][variable] += useEntryCounts[offset];
                    }
                    for (uint32_t offset = defVarOffsets[cluster];
                         offset < defVarOffsets[cluster + 1]; ++offset) {
                        defBlockOfVar[defVarList[offset]] = bestTarget;
                    }
                    clusterBlock[cluster] = bestTarget;
                    blockSize[source] -= clusterWeights[cluster];
                    blockSize[bestTarget] += clusterWeights[cluster];
                    --blockMembers[source];
                    ++blockMembers[bestTarget];
                    totalCost += bestDelta;
                    ++roundMoves;
                }
                outcome.rounds = round + 1;
                outcome.moves += roundMoves;
                if (roundMoves == 0) {
                    break;
                }
            }

            outcome.costAfter = totalCost;
            for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
                segmentOfPos[topoPos[cluster]] = clusterBlock[cluster];
            }
            return outcome;
        }
    } // namespace

    std::optional<AmComputeActivityGraph>
    partitionAmComputeGraph(const AmGraphPartitionInput &input,
                            const AmGraphSplit &split, std::string &error)
    {
        error.clear();
        const auto fail = [&](std::string message) {
            error = std::move(message);
            return std::optional<AmComputeActivityGraph>();
        };
        if (!validateActivityInputShape(input, error) ||
            !validateSplitShape(input, split, error)) {
            return std::nullopt;
        }
        const AmComputeGraph &computeGraph = split.computeGraph;
        const uint32_t computeAtomCount = computeGraph.atomCount;
        const uint32_t n = computeAtomCount;

        // gsim-style partitioner (mergeNodes.cpp mergeOut1/mergeIn1/
        // mergeSublings single sweeps, then graphPartition.cpp resort +
        // graphInitPartition). The AM compute DAG plays both adjacency
        // levels of the reference (data and dependency edges coincide), so
        // the dep-level-only rewiring branches degenerate to no-ops.
        const auto coarsenStart = std::chrono::steady_clock::now();

        // Deterministic topological order of the compute DAG: Kahn with ties
        // broken on the smallest member instruction index, then the local id.
        // All merge sweeps, the resort and the DP run in this id space.
        std::vector<uint32_t> atomOfRid;
        atomOfRid.reserve(n);
        {
            using Candidate = std::tuple<uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            std::vector<uint32_t> indegree(n, 0);
            for (uint32_t atom = 0; atom < n; ++atom) {
                for (uint32_t offset = computeGraph.offsets[atom];
                     offset < computeGraph.offsets[atom + 1]; ++offset) {
                    ++indegree[computeGraph.targets[offset]];
                }
            }
            for (uint32_t atom = 0; atom < n; ++atom) {
                if (indegree[atom] == 0) {
                    ready.emplace(input.atomMinInstruction[computeGraph.globalOfAtom[atom]],
                                  atom);
                }
            }
            while (!ready.empty()) {
                const uint32_t atom = std::get<1>(ready.top());
                ready.pop();
                atomOfRid.push_back(atom);
                for (uint32_t offset = computeGraph.offsets[atom];
                     offset < computeGraph.offsets[atom + 1]; ++offset) {
                    const uint32_t target = computeGraph.targets[offset];
                    if (--indegree[target] == 0) {
                        ready.emplace(
                            input.atomMinInstruction[computeGraph.globalOfAtom[target]],
                            target);
                    }
                }
            }
        }
        if (atomOfRid.size() != n) {
            return fail("internal error: AM compute atom graph is cyclic");
        }
        std::vector<uint32_t> ridOfAtom(n, 0);
        for (uint32_t rid = 0; rid < n; ++rid) {
            ridOfAtom[atomOfRid[rid]] = rid;
        }

        // Per-node state in topo-id (rid) space; the rid edge list is kept for
        // the post-merge adjacency rebuild. NO0007 P3: the atom IS the size
        // unit -- one per atom -- so the coarsen budget, the DP per-block
        // node cap, and the refinement block sizes all count atoms.
        std::vector<uint32_t> member(n, 0);
        std::vector<uint32_t> edgeSrc;
        std::vector<uint32_t> edgeDst;
        edgeSrc.reserve(computeGraph.offsets.back());
        edgeDst.reserve(computeGraph.offsets.back());
        for (uint32_t rid = 0; rid < n; ++rid) {
            member[rid] = 1;
        }
        std::vector<std::vector<uint32_t>> nexts(n);
        std::vector<std::vector<uint32_t>> prevs(n);
        for (uint32_t atom = 0; atom < n; ++atom) {
            const uint32_t source = ridOfAtom[atom];
            for (uint32_t offset = computeGraph.offsets[atom];
                 offset < computeGraph.offsets[atom + 1]; ++offset) {
                const uint32_t target = ridOfAtom[computeGraph.targets[offset]];
                edgeSrc.push_back(source);
                edgeDst.push_back(target);
                nexts[source].push_back(target);
                prevs[target].push_back(source);
            }
        }
        for (uint32_t rid = 0; rid < n; ++rid) {
            std::sort(nexts[rid].begin(), nexts[rid].end());
            nexts[rid].erase(std::unique(nexts[rid].begin(), nexts[rid].end()),
                             nexts[rid].end());
            std::sort(prevs[rid].begin(), prevs[rid].end());
            prevs[rid].erase(std::unique(prevs[rid].begin(), prevs[rid].end()),
                             prevs[rid].end());
        }

        std::string degreeHistogram;
        if (input.enableCoarsening) {
            const auto bucketize = [&](bool outgoing) {
                std::array<std::size_t, 8> buckets{};
                std::size_t total = 0;
                for (uint32_t rid = 0; rid < n; ++rid) {
                    const std::size_t degree =
                        outgoing ? nexts[rid].size() : prevs[rid].size();
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
            degreeHistogram = "clusters=" + std::to_string(n) +
                              " outdeg{" + bucketize(true) + " } indeg{" + bucketize(false) +
                              " }";
        }

        // Merge sweeps (gsim mergeNodes.cpp): one pass per rule, no fixpoint;
        // the live rewiring below collapses whole chains within the pass.
        const std::size_t mergeLimit = input.coarsenAtomBudget;
        std::vector<uint8_t> alive(n, 1);
        std::vector<uint32_t> parent(n, 0);
        std::iota(parent.begin(), parent.end(), uint32_t{0});
        std::size_t out1Merges = 0;
        std::size_t in1Merges = 0;
        std::size_t siblingMerges = 0;
        if (input.enableCoarsening) {
            // mergeOut1: reverse sweep (downstream first). A node whose
            // out-degree is exactly one merges into its unique successor t
            // when t's pre-merge member count is within the merge limit.
            for (uint32_t s = n; s-- > 0;) {
                std::vector<uint32_t> &ns = nexts[s];
                if (ns.size() != 1) {
                    continue;
                }
                const uint32_t t = ns.front();
                if (member[t] > mergeLimit) {
                    continue;
                }
                // t inherits s's in-neighborhood; s's only out-neighbor is t
                // itself, so there is nothing to re-point past t.
                std::vector<uint32_t> &pt = prevs[t];
                sortedErase(pt, s);
                for (const uint32_t p : prevs[s]) {
                    sortedErase(nexts[p], s);
                    sortedInsert(nexts[p], t);
                }
                sortedUnionInto(pt, prevs[s]);
                member[t] += member[s];
                member[s] = 0;
                alive[s] = 0;
                parent[s] = t;
                ns.clear();
                prevs[s].clear();
                ++out1Merges;
            }

            // mergeIn1: forward sweep, mirror image (merge into the unique
            // predecessor).
            for (uint32_t s = 0; s < n; ++s) {
                if (alive[s] == 0) {
                    continue;
                }
                std::vector<uint32_t> &sp = prevs[s];
                if (sp.size() != 1) {
                    continue;
                }
                const uint32_t p = sp.front();
                if (member[p] > mergeLimit) {
                    continue;
                }
                std::vector<uint32_t> &np = nexts[p];
                sortedErase(np, s);
                for (const uint32_t d : nexts[s]) {
                    sortedErase(prevs[d], s);
                    sortedInsert(prevs[d], p);
                }
                sortedUnionInto(np, nexts[s]);
                member[p] += member[s];
                member[s] = 0;
                alive[s] = 0;
                parent[s] = p;
                sp.clear();
                nexts[s].clear();
                ++in1Merges;
            }

            // mergeSublings: exact predecessor-set equality classes; members
            // merge into the first host still below the member cap, a full
            // host is replaced by the current node. Membership only moves
            // here; the adjacency is rebuilt from the edge list afterwards.
            std::vector<uint32_t> candidates;
            candidates.reserve(n);
            for (uint32_t s = 0; s < n; ++s) {
                if (alive[s] != 0 && !prevs[s].empty()) {
                    candidates.push_back(s);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [&](uint32_t lhs, uint32_t rhs) {
                return prevs[lhs] != prevs[rhs] ? prevs[lhs] < prevs[rhs] : lhs < rhs;
            });
            std::size_t begin = 0;
            while (begin < candidates.size()) {
                std::size_t end = begin + 1;
                while (end < candidates.size() &&
                       prevs[candidates[end]] == prevs[candidates[begin]]) {
                    ++end;
                }
                if (end - begin >= 2) {
                    uint32_t host = candidates[begin];
                    for (std::size_t index = begin + 1; index < end; ++index) {
                        const uint32_t s = candidates[index];
                        if (member[host] < kMaxSiblingClusterMembers) {
                            member[host] += member[s];
                            member[s] = 0;
                            alive[s] = 0;
                            parent[s] = host;
                            ++siblingMerges;
                        } else {
                            host = s;
                        }
                    }
                }
                begin = end;
            }
        }
        const uint64_t coarsenMs = elapsedMs(coarsenStart);

        const auto dpStart = std::chrono::steady_clock::now();

        // Rebuild the cluster adjacency (reconnectSuper equivalent): original
        // edges remapped through the merge DSU, self-loops dropped, deduped.
        const auto findRoot = [&parent](uint32_t value) {
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
        };
        std::vector<std::vector<uint32_t>>().swap(nexts);
        std::vector<std::vector<uint32_t>>().swap(prevs);
        std::vector<uint64_t> mappedEdges;
        mappedEdges.reserve(edgeSrc.size());
        for (std::size_t index = 0; index < edgeSrc.size(); ++index) {
            const uint32_t source = findRoot(edgeSrc[index]);
            const uint32_t target = findRoot(edgeDst[index]);
            if (source != target) {
                mappedEdges.push_back((static_cast<uint64_t>(source) << 32) | target);
            }
        }
        edgeSrc.clear();
        edgeDst.clear();
        std::sort(mappedEdges.begin(), mappedEdges.end());
        mappedEdges.erase(std::unique(mappedEdges.begin(), mappedEdges.end()),
                          mappedEdges.end());
        std::vector<uint32_t> clusterOutOffsets(n + 1, 0);
        for (const uint64_t edge : mappedEdges) {
            ++clusterOutOffsets[static_cast<uint32_t>(edge >> 32) + 1];
        }
        std::partial_sum(clusterOutOffsets.begin(), clusterOutOffsets.end(),
                         clusterOutOffsets.begin());
        std::vector<uint32_t> clusterOutTargets(mappedEdges.size());
        std::vector<uint32_t> clusterInDegree(n, 0);
        {
            std::vector<uint32_t> cursor(clusterOutOffsets.begin(),
                                         clusterOutOffsets.end() - 1);
            for (const uint64_t edge : mappedEdges) {
                const uint32_t target = static_cast<uint32_t>(edge);
                clusterOutTargets[cursor[static_cast<uint32_t>(edge >> 32)]++] = target;
                ++clusterInDegree[target];
            }
        }

        // resort (graphPartition.cpp): LIFO Kahn over the cluster adjacency;
        // sources pushed in ascending id, successors visited in ascending id
        // order.
        const uint32_t liveCount =
            static_cast<uint32_t>(std::count(alive.begin(), alive.end(), 1));
        std::vector<uint32_t> seq;
        seq.reserve(liveCount);
        {
            std::vector<uint32_t> stack;
            for (uint32_t rid = 0; rid < n; ++rid) {
                if (alive[rid] != 0 && clusterInDegree[rid] == 0) {
                    stack.push_back(rid);
                }
            }
            std::vector<uint32_t> arrived(n, 0);
            while (!stack.empty()) {
                const uint32_t top = stack.back();
                stack.pop_back();
                seq.push_back(top);
                for (uint32_t offset = clusterOutOffsets[top];
                     offset < clusterOutOffsets[top + 1]; ++offset) {
                    const uint32_t target = clusterOutTargets[offset];
                    if (++arrived[target] == clusterInDegree[target]) {
                        stack.push_back(target);
                    }
                }
            }
        }
        const uint32_t clusterCount = static_cast<uint32_t>(seq.size());
        if (clusterCount != liveCount) {
            return fail("internal error: AM coarsened cluster graph is cyclic");
        }
        std::vector<uint32_t> posOfRid(n, kInvalidIndex);
        for (uint32_t pos = 0; pos < clusterCount; ++pos) {
            posOfRid[seq[pos]] = pos;
        }

        // Kernighan DP (graphPartition.cpp graphInitPartition): over the
        // resort'ed sequence, T[j] = min over jumps i->j of T[i] + Cij +
        // segmentPenalty, where Cij is the cut size behind sequence position
        // j (accumulated out-degree minus the in-edges from positions >= i)
        // and the jump stays within maxAtomsPerBlock of cumulative
        // member atoms (a single oversized cluster forms its own
        // segment).
        std::vector<uint32_t> sequenceSizes(clusterCount, 0);
        std::vector<uint32_t> sequenceOutDegree(clusterCount, 0);
        std::vector<uint32_t> prevOffsets(clusterCount + 1, 0);
        for (uint32_t pos = 0; pos < clusterCount; ++pos) {
            const uint32_t rid = seq[pos];
            sequenceSizes[pos] = member[rid];
            sequenceOutDegree[pos] = clusterOutOffsets[rid + 1] - clusterOutOffsets[rid];
        }
        std::vector<uint32_t> inNeighborCount(clusterCount, 0);
        for (const uint64_t edge : mappedEdges) {
            ++inNeighborCount[posOfRid[static_cast<uint32_t>(edge)]];
        }
        for (uint32_t pos = 0; pos < clusterCount; ++pos) {
            prevOffsets[pos + 1] = prevOffsets[pos] + inNeighborCount[pos];
        }
        std::vector<uint32_t> prevPositions(prevOffsets.back());
        {
            std::vector<uint32_t> cursor(prevOffsets.begin(), prevOffsets.end() - 1);
            for (const uint64_t edge : mappedEdges) {
                const uint32_t source = posOfRid[static_cast<uint32_t>(edge >> 32)];
                const uint32_t target = posOfRid[static_cast<uint32_t>(edge)];
                prevPositions[cursor[target]++] = source;
            }
        }
        // Positions (not rids) are stored, so sort each segment explicitly.
        for (uint32_t pos = 0; pos < clusterCount; ++pos) {
            std::sort(prevPositions.begin() + prevOffsets[pos],
                      prevPositions.begin() + prevOffsets[pos + 1]);
        }

        const std::size_t maxNodes = input.maxAtomsPerBlock;
        constexpr double kInf = std::numeric_limits<double>::infinity();
        std::vector<double> best(static_cast<std::size_t>(clusterCount) + 1, kInf);
        std::vector<uint32_t> back(static_cast<std::size_t>(clusterCount) + 1, 0);
        best[0] = 0.0;
        for (uint32_t i = 0; i < clusterCount; ++i) {
            if (best[i] == kInf) {
                continue;
            }
            uint32_t nextBound = i + 1;
            std::size_t accumulated = sequenceSizes[i];
            while (nextBound < clusterCount &&
                   accumulated + sequenceSizes[nextBound] <= maxNodes) {
                accumulated += sequenceSizes[nextBound];
                ++nextBound;
            }
            int64_t cutCost = 0;
            for (uint32_t j = i + 1; j <= nextBound; ++j) {
                cutCost += sequenceOutDegree[j - 1];
                const uint32_t prevBegin = prevOffsets[j - 1];
                const uint32_t prevEnd = prevOffsets[j];
                // In-edges from positions >= i stay internal to the segment.
                const auto firstInSegment = std::lower_bound(
                    prevPositions.begin() + prevBegin, prevPositions.begin() + prevEnd, i);
                cutCost -= static_cast<int64_t>(prevPositions.begin() + prevEnd - firstInSegment);
                const double candidate =
                    best[i] + static_cast<double>(cutCost) + input.segmentPenalty;
                if (best[j] > candidate) {
                    best[j] = candidate;
                    back[j] = i;
                }
            }
        }

        // Backtrack the cut points; blocks are numbered 1..B in sequence order.
        std::vector<uint32_t> segmentOfPos(clusterCount, 0);
        uint32_t computeBlockCount = 0;
        if (clusterCount != 0) {
            std::vector<uint32_t> cuts;
            cuts.push_back(clusterCount);
            for (uint32_t idx = clusterCount; back[idx] != 0;) {
                idx = back[idx];
                cuts.push_back(idx);
            }
            std::reverse(cuts.begin(), cuts.end());
            uint32_t begin = 0;
            for (const uint32_t end : cuts) {
                ++computeBlockCount;
                for (uint32_t pos = begin; pos < end; ++pos) {
                    segmentOfPos[pos] = computeBlockCount;
                }
                begin = end;
            }
        }
        const uint64_t dpMs = elapsedMs(dpStart);

        // Dense cluster graph in resort order for refinement and assembly.
        ClusterGraph graph;
        graph.count = clusterCount;
        graph.clusterOfAtom.assign(n, kInvalidIndex);
        for (uint32_t atom = 0; atom < n; ++atom) {
            graph.clusterOfAtom[atom] = posOfRid[findRoot(ridOfAtom[atom])];
        }
        graph.rootOf.assign(clusterCount, 0);
        for (uint32_t pos = 0; pos < clusterCount; ++pos) {
            graph.rootOf[pos] = atomOfRid[seq[pos]];
        }
        graph.outOffsets.assign(static_cast<std::size_t>(clusterCount) + 1, 0);
        for (const uint64_t edge : mappedEdges) {
            ++graph.outOffsets[posOfRid[static_cast<uint32_t>(edge >> 32)] + 1];
        }
        std::partial_sum(graph.outOffsets.begin(), graph.outOffsets.end(),
                         graph.outOffsets.begin());
        graph.outTargets.resize(mappedEdges.size());
        graph.inOffsets.assign(static_cast<std::size_t>(clusterCount) + 1, 0);
        {
            std::vector<uint32_t> cursor(graph.outOffsets.begin(), graph.outOffsets.end() - 1);
            for (const uint64_t edge : mappedEdges) {
                const uint32_t target = posOfRid[static_cast<uint32_t>(edge)];
                graph.outTargets[cursor[posOfRid[static_cast<uint32_t>(edge >> 32)]]++] = target;
                ++graph.inOffsets[target + 1];
            }
            std::partial_sum(graph.inOffsets.begin(), graph.inOffsets.end(),
                             graph.inOffsets.begin());
            graph.inTargets.resize(graph.inOffsets.back());
            cursor.assign(graph.inOffsets.begin(), graph.inOffsets.end() - 1);
            for (uint32_t source = 0; source < clusterCount; ++source) {
                for (uint32_t offset = graph.outOffsets[source];
                     offset < graph.outOffsets[source + 1]; ++offset) {
                    graph.inTargets[cursor[graph.outTargets[offset]]++] = source;
                }
            }
        }

        std::vector<uint32_t> clusterOrder(clusterCount, 0);
        std::vector<uint32_t> topoPos(clusterCount, 0);
        std::iota(clusterOrder.begin(), clusterOrder.end(), uint32_t{0});
        std::iota(topoPos.begin(), topoPos.end(), uint32_t{0});
        std::vector<uint32_t> clusterWeights(clusterCount, 0);
        for (uint32_t pos = 0; pos < clusterCount; ++pos) {
            clusterWeights[pos] = sequenceSizes[pos];
        }

        // Post-DP local-move refinement (deterministic; see refineClusterBlocks).
        const auto refineStart = std::chrono::steady_clock::now();
        RefinementOutcome refinement;
        if (input.refinementRounds != 0) {
            refinement =
                refineClusterBlocks(graph, input, computeGraph, clusterWeights, clusterOrder,
                                    topoPos, segmentOfPos, computeBlockCount,
                                    input.maxAtomsPerBlock, input.refinementRounds);
        }
        const uint64_t refinementMs = elapsedMs(refineStart);

        AmComputeActivityGraph result;
        result.blockCount = computeBlockCount;
        result.atomBlock.assign(computeAtomCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < computeAtomCount; ++atom) {
            result.atomBlock[atom] = segmentOfPos[graph.clusterOfAtom[atom]];
        }

        // A block-grouped Kahn keeps the compute topo topological both across
        // and inside blocks.
        result.atomTopo.reserve(computeAtomCount);
        {
            using Candidate = std::tuple<uint32_t, uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            std::vector<uint32_t> indegree(computeAtomCount, 0);
            for (uint32_t atom = 0; atom < computeAtomCount; ++atom) {
                for (uint32_t offset = computeGraph.offsets[atom];
                     offset < computeGraph.offsets[atom + 1]; ++offset) {
                    ++indegree[computeGraph.targets[offset]];
                }
            }
            for (uint32_t atom = 0; atom < computeAtomCount; ++atom) {
                if (indegree[atom] == 0) {
                    ready.emplace(result.atomBlock[atom],
                                  input.atomMinInstruction[computeGraph.globalOfAtom[atom]], atom);
                }
            }
            while (!ready.empty()) {
                const uint32_t atom = std::get<2>(ready.top());
                ready.pop();
                result.atomTopo.push_back(atom);
                for (uint32_t offset = computeGraph.offsets[atom];
                     offset < computeGraph.offsets[atom + 1]; ++offset) {
                    const uint32_t target = computeGraph.targets[offset];
                    if (--indegree[target] == 0) {
                        ready.emplace(result.atomBlock[target],
                                      input.atomMinInstruction[computeGraph.globalOfAtom[target]],
                                      target);
                    }
                }
            }
        }
        if (result.atomTopo.size() != computeAtomCount) {
            return fail("internal error: AM compute atom graph is cyclic");
        }

        result.clustersAfterCoarsen = graph.count;
        result.dpSegments = computeBlockCount;
        result.coarsenMs = coarsenMs;
        result.dpMs = dpMs;
        result.coarsenRounds = input.enableCoarsening ? 3 : 0;
        result.coarsenOut1Merges = out1Merges;
        result.coarsenIn1Merges = in1Merges;
        result.coarsenSiblingMerges = siblingMerges;
        result.initialDegreeHistogram = std::move(degreeHistogram);
        result.refinementRounds = refinement.rounds;
        result.refinementMoves = refinement.moves;
        result.refinementMs = refinementMs;
        result.refinementCostBefore = refinement.costBefore;
        result.refinementCostAfter = refinement.costAfter;
        return result;
    }

} // namespace wolvrix::lib::grhsim::am
