#ifndef WOLVRIX_GRHSIM_AM_PRODUCTION_ACTIVITY_SCHEDULE_HPP
#define WOLVRIX_GRHSIM_AM_PRODUCTION_ACTIVITY_SCHEDULE_HPP

#include "grhsim/am/pipeline.hpp"

namespace wolvrix::lib::grhsim::am
{

    class ProductionActivityScheduleStage final : public AmActivityScheduleStage
    {
    public:
        std::optional<ExecutableModel>
        schedule(LinearProgramArtifact &&linear,
                 const ActivityScheduleOptions &options,
                 wolvrix::lib::diag::Diagnostics &diagnostics) override;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_PRODUCTION_ACTIVITY_SCHEDULE_HPP
