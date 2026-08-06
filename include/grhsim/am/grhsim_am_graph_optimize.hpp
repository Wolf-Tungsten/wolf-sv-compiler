#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_OPTIMIZE_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_OPTIMIZE_HPP

#include "core/diagnostics.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

namespace wolvrix::lib::grhsim::am
{

    // AmOptimizeOptions is declared in grhsim/am/grh_ir_to_grhsim_am_program.hpp
    // (included above) so that GrhIRToGrhSimAMProgram can hold it by value
    // without a circular include.

    // Runs the AM instruction-stream optimizations (dead-code elimination,
    // constant folding, pure-op common subexpression elimination, assign
    // alias bypassing, and constant-address never-written memory-read
    // folding) on the AmGraph, then rebuilds the graph so the dense fact
    // tables stay aligned. Interface entries that referenced eliminated
    // variables are re-pointed to their alias representatives. On success the
    // graph is replaced in place and passes validate(); on failure the graph
    // is left unmodified, an error diagnostic is emitted, and false is
    // returned.
    bool optimizeAmGraph(AmGraph &graph,
                         const AmOptimizeOptions &options,
                         wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_GRAPH_OPTIMIZE_HPP
