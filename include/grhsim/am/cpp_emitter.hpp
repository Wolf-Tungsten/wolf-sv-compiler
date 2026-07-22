#ifndef WOLVRIX_GRHSIM_AM_CPP_EMITTER_HPP
#define WOLVRIX_GRHSIM_AM_CPP_EMITTER_HPP

#include "grhsim/am/pipeline.hpp"

namespace wolvrix::lib::grhsim::am
{

    class GrhSimAmCppEmitter final : public GrhSimAmCppEmitStage
    {
    public:
        GrhSimAmCppResult
        emit(const ExecutableModel &model,
             const GrhSimAmCppOptions &options,
             wolvrix::lib::diag::Diagnostics &diagnostics) override;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_CPP_EMITTER_HPP
