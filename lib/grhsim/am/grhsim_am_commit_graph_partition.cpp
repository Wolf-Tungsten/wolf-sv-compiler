#include "grhsim/am/grhsim_am_commit_graph_partition.hpp"

#include "grhsim_am_common.hpp"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    using namespace detail;

    namespace
    {
        // Aligns with the legacy guard/event merge op cap for one commit block.
        constexpr std::size_t kMaxGuardEventMergeOps = 4096;
    } // namespace

    std::optional<AmCommitEventGraph>
    partitionAmCommitGraph(const AmGraphPartitionInput &input,
                           const AmGraphSplit &split, std::string &error)
    {
        error.clear();
        if (!validateActivityInputShape(input, error) ||
            !validateSplitShape(input, split, error)) {
            return std::nullopt;
        }
        const AmCommitGraph &commitGraph = split.commitGraph;
        const uint32_t commitAtomCount = commitGraph.atomCount;

        // Event clustering: Kahn over the commit subgraph prioritized by
        // (event rank, min instruction), with bounded merging inside one
        // commit-events bucket under the block limit.
        AmCommitEventGraph result;
        result.atomBlock.assign(commitAtomCount, kInvalidIndex);
        result.atomTopo.reserve(commitAtomCount);
        {
            using Candidate = std::tuple<uint32_t, uint32_t, uint32_t>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
            std::vector<uint32_t> indegree(commitAtomCount, 0);
            for (uint32_t atom = 0; atom < commitAtomCount; ++atom) {
                for (uint32_t offset = commitGraph.offsets[atom];
                     offset < commitGraph.offsets[atom + 1]; ++offset) {
                    ++indegree[commitGraph.targets[offset]];
                }
            }
            for (uint32_t atom = 0; atom < commitAtomCount; ++atom) {
                if (indegree[atom] == 0) {
                    const uint32_t global = commitGraph.globalOfAtom[atom];
                    ready.emplace(input.commitEventRank[global], input.atomMinInstruction[global],
                                  atom);
                }
            }
            const std::size_t commitMergeLimit =
                std::min(input.maxCommitInstructionsPerBlock, kMaxGuardEventMergeOps);
            bool haveCurrent = false;
            std::size_t currentInstructions = 0;
            uint32_t currentBucketAtom = kInvalidIndex;
            while (!ready.empty()) {
                const Candidate top = ready.top();
                const uint32_t atom = std::get<2>(top);
                const uint32_t global = commitGraph.globalOfAtom[atom];
                if (haveCurrent &&
                    (input.commitEventRank[global] !=
                         input.commitEventRank[commitGraph.globalOfAtom[currentBucketAtom]] ||
                     currentInstructions + input.atomInstructions[global] > commitMergeLimit)) {
                    haveCurrent = false;
                }
                if (!haveCurrent) {
                    ++result.blockCount;
                    currentInstructions = 0;
                    currentBucketAtom = atom;
                    haveCurrent = true;
                }
                ready.pop();
                result.atomBlock[atom] = result.blockCount;
                result.atomTopo.push_back(atom);
                currentInstructions += input.atomInstructions[global];
                for (uint32_t offset = commitGraph.offsets[atom];
                     offset < commitGraph.offsets[atom + 1]; ++offset) {
                    const uint32_t target = commitGraph.targets[offset];
                    if (--indegree[target] == 0) {
                        const uint32_t targetGlobal = commitGraph.globalOfAtom[target];
                        ready.emplace(input.commitEventRank[targetGlobal],
                                      input.atomMinInstruction[targetGlobal], target);
                    }
                }
            }
        }
        if (result.atomTopo.size() != commitAtomCount) {
            error = "internal error: AM commit atom graph is cyclic";
            return std::nullopt;
        }
        return result;
    }

} // namespace wolvrix::lib::grhsim::am
