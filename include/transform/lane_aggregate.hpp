#ifndef WOLVRIX_TRANSFORM_LANE_AGGREGATE_HPP
#define WOLVRIX_TRANSFORM_LANE_AGGREGATE_HPP

#include "core/transform.hpp"

#include <cstddef>
#include <string>

namespace wolvrix::lib::transform
{

    // Output mode of the merge rewrite. Wide keeps the historical shape (one
    // wide kRegister + masked kRegisterWritePort + kSliceStatic reads); Array
    // emits the array-value shape (kMemory + kMemoryReadAllPort +
    // kMemoryWriteLanesPort + kMemoryReadPort reads).
    enum class LaneAggregateOutputMode
    {
        Wide,
        Array,
    };

    struct LaneAggregateOptions
    {
        std::size_t minLanes = 8;
        std::size_t maxIndexHoles = 2;
        bool readSelect = true;
        LaneAggregateOutputMode outputMode = LaneAggregateOutputMode::Wide;
        // Exact-all fallback: when no signature bucket reaches minLanes, run
        // the exact cone check over all candidate lanes directly (the
        // signature is only an accelerator; the exact check is the ground
        // truth). On by default: it only recovers groups the exact check
        // gates, so already-merged results are unchanged.
        bool exactFallback = true;
        // C-level lane-parameter leaves: a lane-varying cone position whose
        // per-lane values are produced by the same non-pointwise op (same
        // kind/arity/attrs/width across lanes) classifies as a lane-parameter
        // leaf (materialized as one per-lane kConcat) instead of rejecting
        // with unsupported_op. On by default: the per-lane subgraphs are
        // preserved verbatim, so the rewrite stays exact.
        bool laneParamLeaves = true;
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
