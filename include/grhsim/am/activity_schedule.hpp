#ifndef WOLVRIX_GRHSIM_AM_ACTIVITY_SCHEDULE_HPP
#define WOLVRIX_GRHSIM_AM_ACTIVITY_SCHEDULE_HPP

#include "grhsim/am/pipeline.hpp"

namespace wolvrix::lib::grhsim::am
{

    // Limited smoke bridge for the new stage API and validators. It keeps one
    // normal Block, watches inputs in B0, and rejects host interactions.
    class BaselineActivityScheduleStage final : public AmActivityScheduleStage
    {
    public:
        std::optional<ExecutableModel>
        schedule(LinearProgramArtifact &&linear,
                 const ActivityScheduleOptions &options,
                 wolvrix::lib::diag::Diagnostics &diagnostics) override;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_ACTIVITY_SCHEDULE_HPP
