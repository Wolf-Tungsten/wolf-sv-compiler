#ifndef WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_GRAPH_HPP
#define WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_GRAPH_HPP

#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

namespace wolvrix::lib::grhsim::am
{

    enum class UnknownLogicPolicy : uint8_t
    {
        Reject = 0,
        FlattenToZero = 1,
    };

    struct GrhIRToGrhSimAMGraphLoweringOptions
    {
        UnknownLogicPolicy unknownLogic = UnknownLogicPolicy::Reject;
    };

    // Production normalized-GRH to AmGraph lowering. The graph is built
    // natively (no linear intermediate); it owns every piece of information
    // needed by the AM scheduler and never retains Graph pointers or IDs.
    class GrhIRToGrhSimAMGraphLowering final : public GrhIRToGrhSimAMGraphLoweringStage
    {
    public:
        explicit GrhIRToGrhSimAMGraphLowering(const GrhIRToGrhSimAMGraphLoweringOptions &options = {})
            : options_(options)
        {
        }

        std::optional<AmGraph>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics) override;

    private:
        GrhIRToGrhSimAMGraphLoweringOptions options_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_GRAPH_HPP
