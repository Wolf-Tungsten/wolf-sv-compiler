#ifndef WOLVRIX_GRHSIM_AM_OPTIMIZE_HPP
#define WOLVRIX_GRHSIM_AM_OPTIMIZE_HPP

#include "core/diagnostics.hpp"
#include "grhsim/am/pipeline.hpp"

namespace wolvrix::lib::grhsim::am
{

    // AmOptimizeOptions is declared in grhsim/am/pipeline.hpp (included above)
    // so that GrhSimAmPipeline can hold it by value without a circular include.

    // Runs the AM instruction-stream optimizations (dead-code elimination,
    // constant folding, pure-op common subexpression elimination, assign
    // alias bypassing, and constant-address never-written memory-read
    // folding) on a lowered LinearProgramArtifact, then compacts the program
    // so the dense SchedulingFacts indices stay aligned. Interface entries
    // that referenced eliminated variables are re-pointed to their alias
    // representatives. On success the artifact is replaced in place and
    // passes validate(); on failure the artifact is left unmodified, an
    // error diagnostic is emitted, and false is returned.
    bool optimizeLinearProgram(LinearProgramArtifact &artifact,
                               const AmOptimizeOptions &options,
                               wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_OPTIMIZE_HPP
