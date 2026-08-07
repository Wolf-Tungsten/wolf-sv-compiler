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
        // Instruction-count secondary guard (P3): the atom count is the
        // primary bucket-merge limit (maxCommitAtomsPerBlock), but the
        // emitter-side guard/event merge op ceiling is an instruction
        // semantic, so an over-long instruction tail still splits the bucket.
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
            bool haveCurrent = false;
            std::size_t currentAtoms = 0;
            std::size_t currentInstructions = 0;
            uint32_t currentBucketAtom = kInvalidIndex;
            while (!ready.empty()) {
                const Candidate top = ready.top();
                const uint32_t atom = std::get<2>(top);
                const uint32_t global = commitGraph.globalOfAtom[atom];
                if (haveCurrent &&
                    (input.commitEventRank[global] !=
                         input.commitEventRank[commitGraph.globalOfAtom[currentBucketAtom]] ||
                     currentAtoms + 1 > input.maxCommitAtomsPerBlock ||
                     currentInstructions + input.atomInstructions[global] >
                         kMaxGuardEventMergeOps)) {
                    haveCurrent = false;
                }
                if (!haveCurrent) {
                    ++result.blockCount;
                    currentAtoms = 0;
                    currentInstructions = 0;
                    currentBucketAtom = atom;
                    haveCurrent = true;
                }
                ready.pop();
                result.atomBlock[atom] = result.blockCount;
                result.atomTopo.push_back(atom);
                ++currentAtoms;
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
