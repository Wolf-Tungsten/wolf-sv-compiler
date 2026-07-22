#ifndef WOLVRIX_GRHSIM_AM_LOWERING_HPP
#define WOLVRIX_GRHSIM_AM_LOWERING_HPP

#include "grhsim/am/pipeline.hpp"

namespace wolvrix::lib::grhsim::am
{

    enum class UnknownLogicPolicy : uint8_t
    {
        Reject = 0,
        FlattenToZero = 1,
    };

    struct GrhToAmLoweringOptions
    {
        UnknownLogicPolicy unknownLogic = UnknownLogicPolicy::Reject;
    };

    // Production normalized-GRH to LinearProgram lowering.  This stage owns
    // every piece of information needed by the AM scheduler; the returned
    // artifact never retains Graph pointers or IDs.
    class GrhToAmLowering final : public GrhToAmLoweringStage
    {
    public:
        explicit GrhToAmLowering(const GrhToAmLoweringOptions &options = {})
            : options_(options)
        {
        }

        std::optional<LinearProgramArtifact>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics) override;

    private:
        GrhToAmLoweringOptions options_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_LOWERING_HPP
