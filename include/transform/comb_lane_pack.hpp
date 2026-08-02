#ifndef WOLVRIX_TRANSFORM_COMB_LANE_PACK_HPP
#define WOLVRIX_TRANSFORM_COMB_LANE_PACK_HPP

#include "core/transform.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

    // Output mode of the pack rewrite. Wide keeps the historical shape (one
    // wide pointwise tree; kMux lanes rebuild as (t & m) | (f & ~m) with a
    // per-lane replicated mask); Array emits array-value forms for the packed
    // products: mux lanes become one kArrayMux over the per-lane guard
    // kConcat, and shared-scalar replicate shapes become kArrayBroadcast.
    enum class CombLanePackOutputMode
    {
        Wide,
        Array,
    };

    struct CombLanePackReport
    {
        std::string graphName;
        std::string rootSource;
        std::vector<std::string> rootSymbols;
        std::string anchorKind;
        std::vector<std::string> anchorSymbols;
        std::vector<std::string> storageTargetSymbols;
        std::size_t groupSize = 0;
        std::size_t laneWidth = 0;
        std::size_t packedWidth = 0;
        std::size_t treeNodes = 0;
        std::string rootKind;
        std::string signature;
        uint32_t packedRootValueId = 0;
        std::string description;
    };

    struct CombLanePackOptions
    {
        std::size_t minGroupSize = 4;
        std::size_t maxGroupSize = 16;
        std::size_t minPackedWidth = 32;
        std::size_t maxPackedWidth = 1024;
        std::size_t maxTreeNodes = 64;
        std::size_t maxRootGap = 128;
        bool requireDeclaredRoots = true;
        bool enableDeclaredRoots = true;
        bool enableStorageDataRoots = true;
        bool enableMux = true;
        CombLanePackOutputMode outputMode = CombLanePackOutputMode::Wide;
        std::string outputKey;
    };

    class CombLanePackPass : public Pass
    {
    public:
        CombLanePackPass();
        explicit CombLanePackPass(CombLanePackOptions options);

        PassResult run() override;

    private:
        CombLanePackOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_COMB_LANE_PACK_HPP
