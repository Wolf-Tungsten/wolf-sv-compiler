#ifndef WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP
#define WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP

#include "core/grh.hpp"
#include "core/transform.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{
    enum class ActivityOpClass : uint8_t
    {
        Source,
        Sink,
        Compute,
        Declaration,
        Unsupported,
    };

    enum class ActivityScheduleSupernodeKind : uint8_t
    {
        Compute = 0,
        Commit = 1,
    };

    struct ActivityScheduleOptions
    {
        std::string path;
        std::size_t maxOpInComputeSupernode = 128;
        std::size_t maxOpInComputeNode = 8192;
        std::size_t maxOpInCommitSupernode = 4096;
        std::size_t localSharedComputeMaxFanout = 4;
        std::size_t localSharedComputeMaxWidth = 256;
        std::size_t splitOversizeComputeNodeMaxOps = 0;
        bool enableCoarsen = true;
        bool enableChainMerge = true;
        bool enableLocalSharedCompute = false;
        bool commitGuardEventBuckets = true;
        bool splitOversizeComputeNodes = false;
        bool declaredValueComputeNodeBoundary = false;
        std::string exportComputeDagPath;
    };

    struct ActivityScheduleSymbolIdHash
    {
        std::size_t operator()(wolvrix::lib::grh::SymbolId id) const noexcept
        {
            return static_cast<std::size_t>(id.value);
        }
    };

    using ActivityScheduleSupernodeToOps = std::vector<std::vector<wolvrix::lib::grh::OperationId>>;
    using ActivityScheduleOpToSupernode = std::vector<uint32_t>;
    using ActivityScheduleDag = std::vector<std::vector<uint32_t>>;
    using ActivityScheduleValueFanout = std::vector<std::vector<uint32_t>>;
    using ActivityScheduleTopoOrder = std::vector<uint32_t>;
    using ActivityScheduleStateReadSupernodes = std::unordered_map<std::string, std::vector<uint32_t>>;
    using ActivityScheduleSupernodeKinds = std::vector<ActivityScheduleSupernodeKind>;
    using ActivityScheduleComputeNodesBySupernode = std::vector<std::vector<uint32_t>>;

    struct ActivityScheduleComputeNodeOutDegreeBucket
    {
        std::string label;
        std::size_t count = 0;
    };

    struct ActivityScheduleComputeNodeOutDegreeStats
    {
        std::string semantics;
        std::size_t nodes = 0;
        std::size_t edges = 0;
        std::size_t meanMilli = 0;
        std::size_t p50 = 0;
        std::size_t p90 = 0;
        std::size_t p99 = 0;
        std::size_t max = 0;
        std::vector<ActivityScheduleComputeNodeOutDegreeBucket> buckets;
    };

    inline constexpr uint32_t kInvalidActivitySupernodeId = std::numeric_limits<uint32_t>::max();

    class ActivitySchedulePass : public Pass
    {
    public:
        ActivitySchedulePass();
        explicit ActivitySchedulePass(ActivityScheduleOptions options);

        PassResult run() override;

    private:
        ActivityScheduleOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP
