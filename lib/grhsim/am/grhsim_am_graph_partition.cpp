#include "grhsim/am/grhsim_am_graph_partition.hpp"

#include "grhsim_am_common.hpp"

#include <numeric>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    using namespace detail;

    std::optional<AmGraphSplit>
    splitAmGraph(const AmGraphPartitionInput &input, std::string &error)
    {
        error.clear();
        if (!validateActivityInputShape(input, error)) {
            return std::nullopt;
        }
        const uint32_t atomCount = input.atomCount;

        AmGraphSplit split;
        AmComputeGraph &computeGraph = split.computeGraph;
        AmCommitGraph &commitGraph = split.commitGraph;
        computeGraph.localOfAtom.assign(atomCount, kInvalidIndex);
        commitGraph.localOfAtom.assign(atomCount, kInvalidIndex);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            if (input.atomIsCommit[atom] != 0) {
                commitGraph.localOfAtom[atom] = commitGraph.atomCount;
                commitGraph.globalOfAtom.push_back(atom);
                ++commitGraph.atomCount;
            } else {
                computeGraph.localOfAtom[atom] = computeGraph.atomCount;
                computeGraph.globalOfAtom.push_back(atom);
                ++computeGraph.atomCount;
            }
        }

        // A commit atom may only reach other commit atoms: a state commit that
        // feeds pre-commit work is a phase violation, reject it here.
        computeGraph.offsets.assign(static_cast<std::size_t>(computeGraph.atomCount) + 1, 0);
        commitGraph.offsets.assign(static_cast<std::size_t>(commitGraph.atomCount) + 1, 0);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            for (uint32_t offset = input.atomOffsets[atom]; offset < input.atomOffsets[atom + 1];
                 ++offset) {
                const uint32_t target = input.atomTargets[offset];
                if (target >= atomCount) {
                    error = "internal error: atom DAG target out of range";
                    return std::nullopt;
                }
                if (input.atomIsCommit[atom] != 0 && input.atomIsCommit[target] == 0) {
                    error = "AM dependency requires a state commit before pre-commit work";
                    return std::nullopt;
                }
                if (input.atomIsCommit[atom] == 0 && input.atomIsCommit[target] == 0) {
                    ++computeGraph.offsets[computeGraph.localOfAtom[atom] + 1];
                } else if (input.atomIsCommit[atom] != 0 && input.atomIsCommit[target] != 0) {
                    ++commitGraph.offsets[commitGraph.localOfAtom[atom] + 1];
                }
            }
        }
        std::partial_sum(computeGraph.offsets.begin(), computeGraph.offsets.end(),
                         computeGraph.offsets.begin());
        std::partial_sum(commitGraph.offsets.begin(), commitGraph.offsets.end(),
                         commitGraph.offsets.begin());
        computeGraph.targets.resize(computeGraph.offsets.back());
        commitGraph.targets.resize(commitGraph.offsets.back());
        std::vector<uint32_t> computeCursor(computeGraph.offsets.begin(),
                                            computeGraph.offsets.end() - 1);
        std::vector<uint32_t> commitCursor(commitGraph.offsets.begin(),
                                           commitGraph.offsets.end() - 1);
        for (uint32_t atom = 0; atom < atomCount; ++atom) {
            for (uint32_t offset = input.atomOffsets[atom]; offset < input.atomOffsets[atom + 1];
                 ++offset) {
                const uint32_t target = input.atomTargets[offset];
                if (input.atomIsCommit[atom] == 0 && input.atomIsCommit[target] == 0) {
                    computeGraph.targets[computeCursor[computeGraph.localOfAtom[atom]]++] =
                        computeGraph.localOfAtom[target];
                } else if (input.atomIsCommit[atom] != 0 && input.atomIsCommit[target] != 0) {
                    commitGraph.targets[commitCursor[commitGraph.localOfAtom[atom]]++] =
                        commitGraph.localOfAtom[target];
                }
            }
        }
        return split;
    }

} // namespace wolvrix::lib::grhsim::am
