#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_TO_PROGRAM_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_TO_PROGRAM_HPP

#include "grhsim/am/grhsim_am_graph_partition.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include <optional>

namespace wolvrix::lib::grhsim::am
{

    // materialize stage: merges the two partitioned graphs back into the
    // global atom numbering, lays out the Blocks, plans the commit gate
    // detectors and the activation edges, and finalizes the executable model.
    std::optional<ExecutableModel>
    materializeAmProgram(AmGraph &graph,
                         AmGraphSplitContext &context,
                         const AmComputeActivityGraph &computeActivity,
                         const AmCommitEventGraph &commitEvent,
                         const ActivityScheduleOptions &options,
                         wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_TO_PROGRAM_HPP
