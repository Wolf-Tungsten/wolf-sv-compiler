#include "grhsim/am/grhsim_am_compute_graph_optimize.hpp"

namespace wolvrix::lib::grhsim::am
{

    void optAmComputeGraph(AmComputeGraph &computeGraph,
                           const AmGraphPartitionInput &input)
    {
        // Reserved stage boundary (framework: opt-am-compute-graph). Graph-level
        // compute optimizations land here; intentionally a no-op today.
        (void)computeGraph;
        (void)input;
    }

} // namespace wolvrix::lib::grhsim::am
