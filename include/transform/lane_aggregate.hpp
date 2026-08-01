#ifndef WOLVRIX_TRANSFORM_LANE_AGGREGATE_HPP
#define WOLVRIX_TRANSFORM_LANE_AGGREGATE_HPP

#include "core/transform.hpp"

#include <cstddef>
#include <string>

namespace wolvrix::lib::transform
{

    struct LaneAggregateOptions
    {
        std::size_t minLanes = 8;
        std::size_t maxIndexHoles = 2;
        bool readSelect = true;
        std::string outputKey;
    };

    class LaneAggregatePass : public Pass
    {
    public:
        LaneAggregatePass();
        explicit LaneAggregatePass(LaneAggregateOptions options);

        PassResult run() override;

    private:
        LaneAggregateOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_LANE_AGGREGATE_HPP
