#ifndef WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP
#define WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP

#include "core/grh.hpp"
#include "core/transform.hpp"

#include <cstdint>
#include <limits>
#include <map>
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
        std::string costModel = "edge-cut";
        // NO0207 概率驱动划分（框架；算法尚未实现）。默认 "plain" 与现状逐字节一致。
        // 以下数值均为占位默认，实现 Phase A–G 时由参数扫描标定，现阶段不参与划分决策。
        std::string partitionPolicy = "plain"; // "plain" | "prob" | "cbaw"
        double piDataInput = 0.1;              // 数据 InputPort 静态变化概率先验
        double piRegRead = 0.2;                // RegisterRead 静态变化概率先验
        double piHighThreshold = 0.9;          // 高活跃节点阈值
        double phiMin = 0.6;                   // 内聚度下限
        double cBpMiss = 8.0;                  // 分支预测失误惩罚（相对 C_check_fast）
        std::size_t footprintMaxBytes = 32 * 1024; // F_max：宿主 x86 L1D 容量假设
        std::size_t fmRefineMaxRounds = 4;     // FM 边界精修轮数
        bool probDpCost = false;               // prob 路径下 DP 是否使用概率加权 boundary cost（实验项）
        std::string probDpCostMode = "mixed-pi"; // "pi" | "mixed-pi" | "change" | "mixed-change"
        double probDpAlpha = 1.0;              // mixed-* cost 中概率/变化权重项系数
        double probDpSegmentPenalty = 1.25;    // probDpCost=true 时 DP 每段固定惩罚
        std::string exportComputeDagPath;
        std::size_t cbawPlainBoundaryBaseline = 0;
        std::size_t cbawPlainDagBaseline = 0;
        std::size_t cbawPlainComputeComputeBaseline = 0;
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

    struct ActivityScheduleSummaryStats
    {
        using KindCountMap = std::map<std::string, std::size_t>;
        using KindDoubleMap = std::map<std::string, double>;

        std::size_t supernodes = 0;
        std::size_t computeSupernodes = 0;
        std::size_t commitSupernodes = 0;
        std::size_t dagEdges = 0;
        std::size_t boundaryValues = 0;
        std::size_t boundaryActivationEdges = 0;
        std::size_t computeComputeValuePairs = 0;
        std::size_t computeCommitValuePairs = 0;
        std::size_t stateReadActivationEdges = 0;
        std::size_t memoryReadActivationEdges = 0;
        std::size_t constantActivationEdges = 0;
        std::size_t otherComputeActivationEdges = 0;
        std::size_t otherComputeSingleTargetValues = 0;
        std::size_t otherComputeMultiTargetValues = 0;
        std::size_t otherComputeSingleTargetActivationEdges = 0;
        std::size_t otherComputeMultiTargetActivationEdges = 0;
        std::size_t otherComputeUniqueSupernodePairs = 0;
        std::size_t otherComputeDuplicateActivationEdges = 0;
        double boundaryValuePiSum = 0.0;
        double boundaryEdgePiSum = 0.0;
        double computeComputeEdgePiSum = 0.0;
        double computeCommitEdgePiSum = 0.0;
        double boundaryValueChangeWeightSum = 0.0;
        double boundaryEdgeChangeWeightSum = 0.0;
        double computeComputeEdgeChangeWeightSum = 0.0;
        double computeCommitEdgeChangeWeightSum = 0.0;
        std::size_t computeNodes = 0;
        std::size_t computeNodeOpsTotal = 0;
        std::size_t computeNodeCycleSplitIters = 0;
        std::size_t initialComputeSupernodes = 0;
        std::size_t initialComputeSupernodeOpsTotal = 0;
        std::size_t initialComputeSupernodeDagEdges = 0;
        std::size_t initialBoundaryValues = 0;
        std::size_t initialBoundaryActivationEdges = 0;
        std::size_t initialComputeComputeValuePairs = 0;
        std::size_t initialComputeCommitValuePairs = 0;
        std::size_t sourceClonesInComputeNodes = 0;
        std::size_t localSharedComputeClonesInComputeNodes = 0;
        std::size_t directSourceInputsToCommitSupernodes = 0;
        std::size_t commonExprComputeNodes = 0;
        std::size_t computeNodeBoundaryInputsTotal = 0;
        std::size_t computeNodeBoundaryInputNoDef = 0;
        std::size_t computeNodeBoundaryInputDefOutOfRange = 0;
        std::size_t computeNodeBoundaryInputDeclared = 0;
        std::size_t computeNodeBoundaryInputSourceSpill = 0;
        std::size_t computeNodeBoundaryInputUnsupported = 0;
        std::size_t computeNodeBoundaryInputExistingOwner = 0;
        std::size_t computeNodeBoundaryInputExistingCommonOwner = 0;
        std::size_t computeNodeBoundaryInputShared = 0;
        std::size_t computeNodeBoundaryInputCapacity = 0;
        std::size_t computeNodeBoundaryValues = 0;
        std::size_t commitInputRootValues = 0;
        std::size_t commitSinkOps = 0;
        std::size_t commitEventKeyRuns = 0;
        std::size_t commitEventKeys = 0;
        std::size_t topoEdges = 0;
        std::size_t graphOps = 0;
        std::size_t graphValues = 0;
        KindCountMap activationEdgesBySourceKind;
        KindCountMap activationSourceValuesBySourceKind;
        KindDoubleMap activationEdgePiBySourceKind;
        KindDoubleMap activationEdgeChangeWeightBySourceKind;
        KindCountMap computeNodeBoundaryExistingCommonOwnerByKind;
        KindCountMap computeNodeBoundaryExistingCommonOwnerByWidthBucket;
        KindCountMap computeNodeBoundaryExistingCommonOwnerByFanoutBucket;
    };

    struct ActivityScheduleCbawStats
    {
        using KindCountMap = std::map<std::string, std::size_t>;

        struct TopRoot
        {
            std::size_t valueIndex = 0;
            std::size_t targetCount = 0;
            std::size_t computeTargetCount = 0;
            std::size_t commitTargetCount = 0;
            std::size_t consumerUseCount = 0;
            std::uint64_t valueBytes = 0;
            std::string sourceKind;
        };

        std::size_t valueUseGroups = 0;
        std::size_t crossBoundaryTargetCount = 0;
        std::size_t supernodeDependencyEdgeCount = 0;
        std::size_t computeMaterializedValueTargetCount = 0;
        std::size_t computeCommitValueTargetCount = 0;
        std::uint64_t crossBoundaryValueBytes = 0;
        std::size_t crossBoundaryConsumerUseCount = 0;
        std::size_t sourceCloneCanonicalizedGroups = 0;
        std::size_t cloneWidthMismatchGroups = 0;
        std::size_t canonicalValueUseGroups = 0;
        std::size_t canonicalCrossBoundaryTargetCount = 0;
        std::size_t canonicalSupernodeDependencyEdgeCount = 0;
        std::size_t canonicalComputeMaterializedValueTargetCount = 0;
        std::size_t canonicalComputeCommitValueTargetCount = 0;
        std::size_t canonicalCrossBoundaryConsumerUseCount = 0;
        std::size_t canonicalBoundaryActivationDelta = 0;
        std::size_t canonicalDagEdgeDelta = 0;
        std::size_t canonicalComputeComputeDelta = 0;
        std::size_t quotientDagCycleDetected = 0;
        std::size_t replayBoundaryActivationDelta = 0;
        std::size_t replayDagEdgeDelta = 0;
        std::size_t replayComputeComputeDelta = 0;
        std::size_t computeSupernodes = 0;
        std::size_t computeSupernodeOpCountP50 = 0;
        std::size_t computeSupernodeOpCountP90 = 0;
        std::size_t computeSupernodeOpCountP99 = 0;
        std::size_t computeSupernodeOpCountP995 = 0;
        std::size_t computeSupernodeOpCountMax = 0;
        std::size_t resourceOpCountCap = 0;
        std::size_t resourceOpCountBaselineExceptions = 0;
        std::size_t triggerSignatureBits = 0;
        std::size_t triggerSignatureHashFunctions = 0;
        std::size_t triggerSaturationThresholdBits = 0;
        std::size_t triggerVolatileSourceValues = 0;
        std::size_t triggerComputeSupernodesWithTrigger = 0;
        std::size_t triggerEmptyComputeSupernodes = 0;
        std::size_t triggerSignaturePopcountP50 = 0;
        std::size_t triggerSignaturePopcountP90 = 0;
        std::size_t triggerSignaturePopcountP99 = 0;
        std::size_t triggerSignaturePopcountP995 = 0;
        std::size_t triggerSignaturePopcountMax = 0;
        std::size_t triggerEstimatedCountP50 = 0;
        std::size_t triggerEstimatedCountP90 = 0;
        std::size_t triggerEstimatedCountP99 = 0;
        std::size_t triggerEstimatedCountP995 = 0;
        std::size_t triggerEstimatedCountMax = 0;
        std::size_t triggerSignatureSaturatedComputeSupernodes = 0;
        std::size_t triggerSignatureSaturatedRatioPpm = 0;
        std::size_t triggerEqualBucketCount = 0;
        std::size_t triggerEqualBucketMultiCount = 0;
        std::size_t triggerEqualBucketCoveredSupernodes = 0;
        std::size_t triggerEqualBucketCoveredSupernodeRatioPpm = 0;
        std::size_t triggerEqualBucketLargest = 0;
        std::size_t triggerNonEmptyEqualBucketCount = 0;
        std::size_t triggerNonEmptyEqualBucketMultiCount = 0;
        std::size_t triggerNonEmptyEqualBucketCoveredSupernodes = 0;
        std::size_t triggerNonEmptyEqualBucketCoveredSupernodeRatioPpm = 0;
        std::size_t triggerNonEmptyEqualBucketLargest = 0;
        std::size_t triggerEqualBucketInternalizableBoundaryTargets = 0;
        std::size_t triggerNonEmptyEqualBucketInternalizableBoundaryTargets = 0;
        std::size_t triggerEqualBucketInternalizableComputeTargets = 0;
        std::size_t triggerNonEmptyEqualBucketInternalizableComputeTargets = 0;
        std::size_t triggerEqualBucketInternalizableDependencyEdges = 0;
        std::size_t triggerNonEmptyEqualBucketInternalizableDependencyEdges = 0;
        std::size_t triggerAteEqualMergeRecommended = 0;
        std::size_t semanticSeedGroups = 0;
        std::size_t semanticMergeHintGroups = 0;
        std::size_t semanticDebugLabels = 0;
        std::size_t semanticRtmIntentGroups = 0;
        std::size_t semanticRtmIntentOps = 0;
        std::size_t semanticMffcGroups = 0;
        std::size_t semanticMffcCoveredOps = 0;
        std::size_t semanticMffcSplitGroups = 0;
        std::size_t semanticPlainOut1Hints = 0;
        std::size_t semanticPlainIn1Hints = 0;
        std::size_t semanticPlainSiblingGroups = 0;
        std::size_t semanticPlainSiblingMembers = 0;
        std::size_t semanticAggregateFamilies = 0;
        std::size_t semanticAggregateSeedGroups = 0;
        std::size_t semanticAggregateMergeHintGroups = 0;
        std::size_t semanticGuardDomains = 0;
        std::size_t semanticGuardDomainMembers = 0;
        std::size_t semanticGuardUnknownOps = 0;
        std::size_t semanticSinkConeLabels = 0;
        std::size_t semanticSinkConeMembers = 0;
        std::size_t semanticSinkConeMultiSinkOps = 0;
        std::size_t semanticPassthroughChains = 0;
        std::size_t semanticPassthroughOps = 0;
        std::size_t semanticHierarchyDebugLabels = 0;
        std::size_t semanticTopRootAttributedCount = 0;
        std::size_t semanticTopRootRtmCount = 0;
        std::size_t semanticTopRootAggregateCount = 0;
        std::size_t semanticTopRootGuardCount = 0;
        std::size_t semanticTopRootSinkCount = 0;
        std::size_t semanticTopRootPassthroughCount = 0;
        std::size_t cbawAtomCount = 0;
        std::size_t cbawAtomOpCountP50 = 0;
        std::size_t cbawAtomOpCountP90 = 0;
        std::size_t cbawAtomOpCountP99 = 0;
        std::size_t cbawAtomOpCountP995 = 0;
        std::size_t cbawAtomOpCountMax = 0;
        std::size_t cbawAtomQuotientEdges = 0;
        std::size_t cbawAtomQuotientCycleDetected = 0;
        std::size_t cbawAtomResourceOpCountCap = 0;
        std::size_t cbawAtomResourceOpCountBaselineExceptions = 0;
        std::size_t cbawAtomRtmIntentAtoms = 0;
        std::size_t cbawAtomMffcAtoms = 0;
        std::size_t cbawAtomPassthroughAtoms = 0;
        std::size_t cbawAtomAggregateAtoms = 0;
        std::size_t cbawAtomGuardAtoms = 0;
        std::size_t cbawAtomPlainReplaySupernodes = 0;
        std::size_t cbawAtomPlainReplayCrossBoundaryTargetCount = 0;
        std::size_t cbawAtomPlainReplaySupernodeDependencyEdgeCount = 0;
        std::size_t cbawAtomPlainReplayComputeMaterializedValueTargetCount = 0;
        std::size_t cbawAtomPlainReplayBoundaryDelta = 0;
        std::size_t cbawAtomPlainReplayDagDelta = 0;
        std::size_t cbawAtomPlainReplayComputeComputeDelta = 0;
        std::string triggerAteNoGoReason;
        KindCountMap targetKindMatrix;
        KindCountMap sourceKindMatrix;
        KindCountMap sourceTargetKindMatrix;
        KindCountMap resourceP50;
        KindCountMap resourceP90;
        KindCountMap resourceP99;
        KindCountMap resourceP995;
        KindCountMap resourceMax;
        KindCountMap resourceCap;
        KindCountMap resourceBaselineExceptions;
        KindCountMap semanticRuleSeedGroups;
        KindCountMap semanticRuleMergeHintGroups;
        KindCountMap semanticRuleDebugLabels;
        KindCountMap semanticTopRootAttribution;
        KindCountMap cbawAtomResourceP50;
        KindCountMap cbawAtomResourceP90;
        KindCountMap cbawAtomResourceP99;
        KindCountMap cbawAtomResourceP995;
        KindCountMap cbawAtomResourceMax;
        KindCountMap cbawAtomResourceCap;
        KindCountMap cbawAtomResourceBaselineExceptions;
        KindCountMap cbawAtomKindCounts;
        std::vector<TopRoot> topRoots;
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
