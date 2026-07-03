#include "transform/activity_schedule.hpp"

#include "core/toposort.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {

        std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                                 std::string_view key)
        {
            const auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<std::string>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::string escapeJsonString(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (const char ch : text)
            {
                switch (ch)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(ch); break;
                }
            }
            return out;
        }

        template <typename T>
        std::optional<T> getAttrValue(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            const auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        using ValueCanonicalMap =
            std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>;

        wolvrix::lib::grh::ValueId canonicalActivityValue(wolvrix::lib::grh::ValueId value,
                                                          const ValueCanonicalMap *canonicalValues)
        {
            if (canonicalValues == nullptr)
            {
                return value;
            }
            const auto it = canonicalValues->find(value);
            if (it == canonicalValues->end())
            {
                return value;
            }
            return it->second;
        }

        bool isSinkPartitionOp(const wolvrix::lib::grh::Operation &op)
        {
            if (!op.results().empty())
            {
                return false;
            }
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
                return true;
            default:
                return false;
            }
        }

        std::string describeOp(const wolvrix::lib::grh::Graph &graph,
                               wolvrix::lib::grh::OperationId opId)
        {
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            if (!op.symbolText().empty())
            {
                return std::string(op.symbolText());
            }
            std::ostringstream oss;
            oss << wolvrix::lib::grh::toString(op.kind()) << "#" << opId.index;
            return oss.str();
        }

        std::string describeValue(const wolvrix::lib::grh::Graph &graph,
                                  wolvrix::lib::grh::ValueId value)
        {
            if (!value.valid())
            {
                return "<invalid>";
            }
            const wolvrix::lib::grh::Value valueInfo = graph.getValue(value);
            std::ostringstream oss;
            if (!valueInfo.symbolText().empty())
            {
                oss << valueInfo.symbolText();
            }
            else
            {
                oss << "value#" << value.index;
            }
            oss << "(id=" << value.index << ",width=" << valueInfo.width() << ")";
            return oss.str();
        }

        std::vector<std::string> splitPath(std::string_view path)
        {
            std::vector<std::string> out;
            std::string current;
            for (const char ch : path)
            {
                if (ch == '.')
                {
                    if (!current.empty())
                    {
                        out.push_back(current);
                        current.clear();
                    }
                    continue;
                }
                current.push_back(ch);
            }
            if (!current.empty())
            {
                out.push_back(current);
            }
            return out;
        }

        wolvrix::lib::grh::OperationId findUniqueInstance(const wolvrix::lib::grh::Graph &graph,
                                                          std::string_view instanceName)
        {
            wolvrix::lib::grh::OperationId found = wolvrix::lib::grh::OperationId::invalid();
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance)
                {
                    continue;
                }
                const auto name = getAttrString(op, "instanceName");
                if (!name || *name != instanceName)
                {
                    continue;
                }
                if (found.valid())
                {
                    return wolvrix::lib::grh::OperationId::invalid();
                }
                found = opId;
            }
            return found;
        }

        std::optional<std::string> resolveTargetGraphName(wolvrix::lib::grh::Design &design,
                                                          std::string_view path,
                                                          std::string &error)
        {
            const std::vector<std::string> segments = splitPath(path);
            if (segments.empty())
            {
                error = "activity-schedule path must not be empty";
                return std::nullopt;
            }
            if (segments.size() == 1)
            {
                if (design.findGraph(segments.front()) == nullptr)
                {
                    error = "activity-schedule graph not found: " + segments.front();
                    return std::nullopt;
                }
                return segments.front();
            }

            auto *current = design.findGraph(segments.front());
            if (current == nullptr)
            {
                error = "activity-schedule root graph not found: " + segments.front();
                return std::nullopt;
            }
            for (std::size_t i = 1; i < segments.size(); ++i)
            {
                const auto instOp = findUniqueInstance(*current, segments[i]);
                if (!instOp.valid())
                {
                    error = "activity-schedule instance not found or not unique: " + segments[i];
                    return std::nullopt;
                }
                const auto op = current->getOperation(instOp);
                const auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    error = "activity-schedule instance missing moduleName: " + segments[i];
                    return std::nullopt;
                }
                current = design.findGraph(*moduleName);
                if (current == nullptr)
                {
                    error = "activity-schedule target module graph not found: " + *moduleName;
                    return std::nullopt;
                }
            }
            return current->symbol();
        }

        bool isHierLikeOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
            case wolvrix::lib::grh::OperationKind::kXMRRead:
            case wolvrix::lib::grh::OperationKind::kXMRWrite:
                return true;
            default:
                return false;
            }
        }

        bool isStorageDeclOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegister:
            case wolvrix::lib::grh::OperationKind::kMemory:
            case wolvrix::lib::grh::OperationKind::kLatch:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
                return true;
            default:
                return false;
            }
        }

        bool isStateReadOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                return true;
            default:
                return false;
            }
        }

        bool isSideEffectBoundaryKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            (void)kind;
            // GrhSIM side effects are controlled by the emitted event / commit semantics rather
            // than by hard schedule boundaries, so activity-schedule does not isolate any op here.
            return false;
        }

        bool isPartitionableOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            return !isStorageDeclOpKind(kind) && !isHierLikeOpKind(kind);
        }

        std::optional<std::string> stateSymbolForReadOp(const wolvrix::lib::grh::Operation &op)
        {
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                return getAttrString(op, "regSymbol");
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                return getAttrString(op, "latchSymbol");
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return getAttrString(op, "memSymbol");
            default:
                return std::nullopt;
            }
        }

        struct ActivityScheduleBuild
        {
            ActivityScheduleSupernodeToOps supernodeToOps;
            ActivityScheduleOpToSupernode opToSupernode;
            std::vector<std::vector<uint32_t>> dag;
            ActivityScheduleValueFanout valueFanout;
            std::vector<wolvrix::lib::grh::OperationKind> valueSourceKind;
            std::vector<uint32_t> valueSourceSupernode;
            ActivityScheduleTopoOrder topoOrder;
            ActivityScheduleStateReadSupernodes stateReadSupernodes;
            ActivityScheduleSupernodeKinds supernodeKinds;
            ActivityScheduleComputeNodesBySupernode computeNodesBySupernode;
        };

        bool isRegToMemIntentSlice(const wolvrix::lib::grh::Operation &op);

        std::optional<wolvrix::lib::grh::ValueId>
        regToMemIntentSliceIndexValue(const wolvrix::lib::grh::Graph &graph,
                                      const wolvrix::lib::grh::Operation &op);

        wolvrix::lib::grh::OperationId activityCanonicalDataOpForOp(
            const wolvrix::lib::grh::Graph &graph,
            wolvrix::lib::grh::OperationId opId,
            const ValueCanonicalMap &canonicalValues);

        std::uint64_t activityComputeUnitsForWidth(int32_t width) noexcept;

        int32_t activityOpResultWidth(const wolvrix::lib::grh::Graph &graph,
                                      wolvrix::lib::grh::OperationId opId);

        std::vector<std::string>
        regToMemIntentSliceStorageReadSymbols(const wolvrix::lib::grh::Graph &graph,
                                              const wolvrix::lib::grh::Operation &op);

        struct ActivityScheduleValueWeightStats
        {
            std::vector<double> piByValueIndex;
            std::vector<double> changeWeightByValueIndex;
        };

        double activityScheduleValueWeightAt(const std::vector<double> *weights,
                                             std::size_t valueIndex)
        {
            if (weights == nullptr || valueIndex >= weights->size())
            {
                return 0.0;
            }
            return std::max(0.0, (*weights)[valueIndex]);
        }

        std::string activityScheduleCbawSourceKindName(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                return "state_read";
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return "memory_read";
            case wolvrix::lib::grh::OperationKind::kConstant:
                return "constant";
            default:
                return "compute_like";
            }
        }

        std::string activityScheduleCbawTargetKindName(const ActivityScheduleBuild &build,
                                                       uint32_t supernode)
        {
            if (supernode >= build.supernodeKinds.size())
            {
                return "unknown";
            }
            return build.supernodeKinds[supernode] == ActivityScheduleSupernodeKind::Commit ? "commit"
                                                                                            : "compute";
        }

        std::size_t activityScheduleValueByteCost(const wolvrix::lib::grh::Graph &graph,
                                                  std::size_t valueIndex)
        {
            if (valueIndex == 0 || valueIndex > graph.values().size())
            {
                return 1;
            }
            wolvrix::lib::grh::ValueId value;
            value.graph = graph.id();
            value.index = static_cast<uint32_t>(valueIndex);
            const int32_t width = graph.valueWidth(value);
            if (width <= 0)
            {
                return 1;
            }
            return static_cast<std::size_t>((static_cast<std::uint64_t>(width) + 7u) / 8u);
        }

        std::size_t percentileFromSorted(const std::vector<std::size_t> &values,
                                         std::size_t numerator,
                                         std::size_t denominator)
        {
            if (values.empty())
            {
                return 0;
            }
            const std::size_t index =
                ((values.size() - 1) * numerator + denominator - 1) / denominator;
            return values[std::min(index, values.size() - 1)];
        }

        bool activityScheduleDagHasCycle(const ActivityScheduleDag &dag)
        {
            std::vector<uint32_t> indegree(dag.size(), 0);
            for (const auto &succs : dag)
            {
                for (const uint32_t succ : succs)
                {
                    if (succ < indegree.size())
                    {
                        ++indegree[succ];
                    }
                }
            }
            std::vector<uint32_t> stack;
            stack.reserve(dag.size());
            for (uint32_t node = 0; node < indegree.size(); ++node)
            {
                if (indegree[node] == 0)
                {
                    stack.push_back(node);
                }
            }
            std::size_t visited = 0;
            while (!stack.empty())
            {
                const uint32_t node = stack.back();
                stack.pop_back();
                ++visited;
                if (node >= dag.size())
                {
                    continue;
                }
                for (const uint32_t succ : dag[node])
                {
                    if (succ < indegree.size() && --indegree[succ] == 0)
                    {
                        stack.push_back(succ);
                    }
                }
            }
            return visited != dag.size();
        }

        std::uint64_t packActivitySchedulePair(uint32_t lhs, uint32_t rhs) noexcept
        {
            return (static_cast<std::uint64_t>(lhs) << 32) | static_cast<std::uint64_t>(rhs);
        }

        bool activityScheduleOpUsesValue(const wolvrix::lib::grh::Graph &graph,
                                         wolvrix::lib::grh::OperationId opId,
                                         wolvrix::lib::grh::ValueId value)
        {
            if (!opId.valid() || !value.valid())
            {
                return false;
            }
            const auto op = graph.getOperation(opId);
            for (const auto operand : op.operands())
            {
                if (operand == value)
                {
                    return true;
                }
            }
            if (isRegToMemIntentSlice(op))
            {
                if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                {
                    return *indexValue == value;
                }
            }
            return false;
        }

        std::size_t activityScheduleHelperCallEstimate(wolvrix::lib::grh::OperationKind kind,
                                                       int32_t width) noexcept
        {
            const bool wide = width > 64;
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kDiv:
            case wolvrix::lib::grh::OperationKind::kMod:
                return 1;
            case wolvrix::lib::grh::OperationKind::kConcat:
            case wolvrix::lib::grh::OperationKind::kReplicate:
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
            case wolvrix::lib::grh::OperationKind::kSliceArray:
            case wolvrix::lib::grh::OperationKind::kShl:
            case wolvrix::lib::grh::OperationKind::kLShr:
            case wolvrix::lib::grh::OperationKind::kAShr:
                return wide ? 1 : 0;
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                return 1;
            default:
                return 0;
            }
        }

        std::size_t activityScheduleBranchEstimate(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kMux:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
                return 1;
            default:
                return 0;
            }
        }

        void recordActivityScheduleResourceDistribution(
            ActivityScheduleCbawStats::KindCountMap &p50,
            ActivityScheduleCbawStats::KindCountMap &p90,
            ActivityScheduleCbawStats::KindCountMap &p99,
            ActivityScheduleCbawStats::KindCountMap &p995,
            ActivityScheduleCbawStats::KindCountMap &maxValues,
            ActivityScheduleCbawStats::KindCountMap &caps,
            ActivityScheduleCbawStats::KindCountMap &exceptions,
            std::string_view name,
            std::vector<std::size_t> values)
        {
            std::sort(values.begin(), values.end());
            const std::string key(name);
            p50[key] = percentileFromSorted(values, 50, 100);
            p90[key] = percentileFromSorted(values, 90, 100);
            p99[key] = percentileFromSorted(values, 99, 100);
            p995[key] = percentileFromSorted(values, 995, 1000);
            maxValues[key] = values.empty() ? 0 : values.back();
            const std::size_t cap = p995[key] == 0 ? maxValues[key] : p995[key];
            caps[key] = cap;
            exceptions[key] =
                cap == 0
                    ? 0
                    : static_cast<std::size_t>(std::count_if(values.begin(),
                                                             values.end(),
                                                             [&](std::size_t value)
                                                             { return value > cap; }));
        }

        struct ActivityScheduleTriggerSignature
        {
            std::array<std::uint64_t, 4> words{};

            bool operator==(const ActivityScheduleTriggerSignature &other) const noexcept
            {
                return words == other.words;
            }

            bool empty() const noexcept
            {
                return words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == 0;
            }

            void mergeFrom(const ActivityScheduleTriggerSignature &other) noexcept
            {
                for (std::size_t i = 0; i < words.size(); ++i)
                {
                    words[i] |= other.words[i];
                }
            }
        };

        struct ActivityScheduleTriggerSignatureHash
        {
            std::size_t operator()(const ActivityScheduleTriggerSignature &signature) const noexcept
            {
                std::uint64_t hash = 0x9e3779b97f4a7c15ull;
                for (const auto word : signature.words)
                {
                    hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                }
                return static_cast<std::size_t>(hash);
            }
        };

        std::uint64_t activityScheduleSplitMix64(std::uint64_t value) noexcept
        {
            value += 0x9e3779b97f4a7c15ull;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
            return value ^ (value >> 31);
        }

        void activityScheduleAddTrigger(ActivityScheduleTriggerSignature &signature,
                                        wolvrix::lib::grh::ValueId canonicalValue) noexcept
        {
            constexpr std::size_t kTriggerSignatureBits = 256;
            constexpr std::size_t kTriggerSignatureHashFunctions = 4;
            const std::uint64_t seed = static_cast<std::uint64_t>(canonicalValue.index);
            for (std::size_t hashIndex = 0; hashIndex < kTriggerSignatureHashFunctions; ++hashIndex)
            {
                const std::uint64_t hash =
                    activityScheduleSplitMix64(seed ^ (0xd6e8feb86659fd93ull * (hashIndex + 1)));
                const std::size_t bit = static_cast<std::size_t>(hash % kTriggerSignatureBits);
                signature.words[bit / 64] |= std::uint64_t{1} << (bit % 64);
            }
        }

        std::size_t activityScheduleTriggerPopcount(const ActivityScheduleTriggerSignature &signature) noexcept
        {
            std::size_t count = 0;
            for (const auto word : signature.words)
            {
                count += static_cast<std::size_t>(__builtin_popcountll(word));
            }
            return count;
        }

        std::size_t activityScheduleEstimatedTriggerCount(std::size_t popcount) noexcept
        {
            constexpr double kTriggerSignatureBits = 256.0;
            constexpr double kTriggerSignatureHashFunctions = 4.0;
            if (popcount == 0)
            {
                return 0;
            }
            if (popcount >= static_cast<std::size_t>(kTriggerSignatureBits))
            {
                return static_cast<std::size_t>(kTriggerSignatureBits);
            }
            const double fill = static_cast<double>(popcount) / kTriggerSignatureBits;
            const double estimate =
                -(kTriggerSignatureBits / kTriggerSignatureHashFunctions) * std::log(1.0 - fill);
            return static_cast<std::size_t>(std::llround(std::max(0.0, estimate)));
        }

        bool activityScheduleIsVolatileTriggerKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return true;
            default:
                return false;
            }
        }

        std::size_t activityScheduleRatioPpm(std::size_t numerator, std::size_t denominator) noexcept
        {
            if (denominator == 0)
            {
                return 0;
            }
            return static_cast<std::size_t>(
                (static_cast<std::uint64_t>(numerator) * 1000000ull) /
                static_cast<std::uint64_t>(denominator));
        }

        bool activityScheduleIsPassthroughKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kAssign:
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
            case wolvrix::lib::grh::OperationKind::kSliceArray:
            case wolvrix::lib::grh::OperationKind::kReplicate:
                return true;
            case wolvrix::lib::grh::OperationKind::kConcat:
                return true;
            default:
                return false;
            }
        }

        bool activityScheduleIsGuardLikeKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kMux:
            case wolvrix::lib::grh::OperationKind::kAnd:
            case wolvrix::lib::grh::OperationKind::kOr:
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
            case wolvrix::lib::grh::OperationKind::kLogicOr:
            case wolvrix::lib::grh::OperationKind::kNot:
            case wolvrix::lib::grh::OperationKind::kLogicNot:
            case wolvrix::lib::grh::OperationKind::kEq:
            case wolvrix::lib::grh::OperationKind::kNe:
                return true;
            default:
                return false;
            }
        }

        bool activityScheduleIsAggregateShapeKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConcat:
            case wolvrix::lib::grh::OperationKind::kSliceArray:
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
                return true;
            default:
                return false;
            }
        }

        bool activityScheduleHasRegToMemIntent(const wolvrix::lib::grh::Operation &op) noexcept
        {
            const auto group = getAttrString(op, "regToMem.intent.group");
            const auto role = getAttrString(op, "regToMem.intent.role");
            const auto mode = getAttrString(op, "regToMem.intent.mode");
            return group && !group->empty() && role && !role->empty() &&
                   (!mode || *mode == "array-index");
        }

        template <typename RewriteBuildT, typename OpDataT>
        ActivityScheduleCbawStats buildActivityScheduleCbawStats(const ActivityScheduleBuild &build,
                                                                 const RewriteBuildT &rewrite,
                                                                 const OpDataT &,
                                                                 const wolvrix::lib::grh::Graph &graph,
                                                                 const ActivityScheduleSummaryStats &summaryStats)
        {
            struct ValueUseGroup
            {
                wolvrix::lib::grh::ValueId canonicalValue;
                uint32_t sourceSupernode = kInvalidActivitySupernodeId;
                wolvrix::lib::grh::OperationKind sourceKind = wolvrix::lib::grh::OperationKind::kConstant;
                std::vector<uint32_t> targetSupernodes;
                std::size_t consumerUseCount = 0;
                std::size_t cloneUseCount = 0;
                bool cloneWidthMismatch = false;
            };

            std::unordered_map<wolvrix::lib::grh::ValueId,
                               ValueUseGroup,
                               wolvrix::lib::grh::ValueIdHash>
                valueGroups;

            const auto noteUse = [&](wolvrix::lib::grh::ValueId usedValue, uint32_t targetSupernode)
            {
                if (!usedValue.valid() || usedValue.index == 0 || targetSupernode >= build.supernodeToOps.size())
                {
                    return;
                }
                const auto canonical = canonicalActivityValue(usedValue, &rewrite.canonicalValues);
                if (!canonical.valid())
                {
                    return;
                }
                const auto defOp = graph.valueDef(canonical);
                if (!defOp.valid())
                {
                    return;
                }
                uint32_t sourceSupernode = kInvalidActivitySupernodeId;
                const wolvrix::lib::grh::OperationKind sourceKind = graph.opKind(defOp);
                if (defOp.index > 0 && defOp.index - 1 < build.opToSupernode.size())
                {
                    sourceSupernode = build.opToSupernode[defOp.index - 1];
                }
                if (sourceSupernode == targetSupernode)
                {
                    return;
                }
                auto [it, inserted] = valueGroups.emplace(canonical, ValueUseGroup{});
                auto &group = it->second;
                if (inserted)
                {
                    group.canonicalValue = canonical;
                    group.sourceSupernode = sourceSupernode;
                    group.sourceKind = sourceKind;
                }
                ++group.consumerUseCount;
                if (canonical != usedValue)
                {
                    ++group.cloneUseCount;
                    if (graph.valueWidth(canonical) != graph.valueWidth(usedValue))
                    {
                        group.cloneWidthMismatch = true;
                    }
                }
                if (std::find(group.targetSupernodes.begin(),
                              group.targetSupernodes.end(),
                              targetSupernode) == group.targetSupernodes.end())
                {
                    group.targetSupernodes.push_back(targetSupernode);
                }
            };

            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    for (const auto operand : op.operands())
                    {
                        noteUse(operand, supernodeId);
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            noteUse(*indexValue, supernodeId);
                        }
                    }
                }
            }

            ActivityScheduleCbawStats stats;
            std::unordered_set<uint64_t> canonicalDependencyEdges;
            std::vector<ActivityScheduleCbawStats::TopRoot> topRoots;
            topRoots.reserve(valueGroups.size());
            for (auto &[_, group] : valueGroups)
            {
                auto &targets = group.targetSupernodes;
                if (targets.empty())
                {
                    continue;
                }
                std::sort(targets.begin(), targets.end());
                ++stats.canonicalValueUseGroups;
                stats.canonicalCrossBoundaryTargetCount += targets.size();
                stats.canonicalCrossBoundaryConsumerUseCount += group.consumerUseCount;
                if (group.cloneUseCount != 0)
                {
                    ++stats.sourceCloneCanonicalizedGroups;
                }
                if (group.cloneWidthMismatch)
                {
                    ++stats.cloneWidthMismatchGroups;
                }
                const std::size_t valueIndex = group.canonicalValue.index;
                const std::string sourceKindName = activityScheduleCbawSourceKindName(group.sourceKind);
                std::size_t computeTargets = 0;
                std::size_t commitTargets = 0;
                for (const uint32_t target : targets)
                {
                    if (target < build.supernodeKinds.size() &&
                        build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++stats.canonicalComputeMaterializedValueTargetCount;
                        ++computeTargets;
                    }
                    else if (target < build.supernodeKinds.size() &&
                             build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Commit)
                    {
                        ++stats.canonicalComputeCommitValueTargetCount;
                        ++commitTargets;
                    }
                    if (group.sourceSupernode != kInvalidActivitySupernodeId)
                    {
                        canonicalDependencyEdges.insert(packActivitySchedulePair(group.sourceSupernode, target));
                    }
                }
                topRoots.push_back(ActivityScheduleCbawStats::TopRoot{
                    .valueIndex = valueIndex,
                    .targetCount = targets.size(),
                    .computeTargetCount = computeTargets,
                    .commitTargetCount = commitTargets,
                    .consumerUseCount = group.consumerUseCount,
                    .valueBytes = static_cast<std::uint64_t>(activityScheduleValueByteCost(graph, valueIndex)),
                    .sourceKind = sourceKindName,
                });
            }
            stats.canonicalSupernodeDependencyEdgeCount = canonicalDependencyEdges.size();

            std::unordered_set<uint64_t> replayDependencyEdges;
            for (std::size_t valueOffset = 0; valueOffset < build.valueFanout.size(); ++valueOffset)
            {
                const auto &targets = build.valueFanout[valueOffset];
                if (targets.empty())
                {
                    continue;
                }
                ++stats.valueUseGroups;
                stats.crossBoundaryTargetCount += targets.size();
                stats.crossBoundaryValueBytes +=
                    static_cast<std::uint64_t>(activityScheduleValueByteCost(graph, valueOffset + 1)) *
                    static_cast<std::uint64_t>(targets.size());
                const uint32_t sourceSupernode =
                    valueOffset + 1 < build.valueSourceSupernode.size()
                        ? build.valueSourceSupernode[valueOffset + 1]
                        : kInvalidActivitySupernodeId;
                const auto sourceKind =
                    valueOffset + 1 < build.valueSourceKind.size()
                        ? build.valueSourceKind[valueOffset + 1]
                        : wolvrix::lib::grh::OperationKind::kConstant;
                const std::string sourceKindName = activityScheduleCbawSourceKindName(sourceKind);
                stats.sourceKindMatrix[sourceKindName] += targets.size();
                for (const uint32_t target : targets)
                {
                    const std::string targetKindName = activityScheduleCbawTargetKindName(build, target);
                    stats.targetKindMatrix[targetKindName] += 1;
                    stats.sourceTargetKindMatrix[sourceKindName + "->" + targetKindName] += 1;
                    if (target < build.supernodeKinds.size() &&
                        build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++stats.computeMaterializedValueTargetCount;
                    }
                    else if (target < build.supernodeKinds.size() &&
                             build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Commit)
                    {
                        ++stats.computeCommitValueTargetCount;
                    }
                    if (sourceSupernode != kInvalidActivitySupernodeId)
                    {
                        replayDependencyEdges.insert(packActivitySchedulePair(sourceSupernode, target));
                    }
                }
            }
            stats.supernodeDependencyEdgeCount = replayDependencyEdges.size();

            std::vector<std::size_t> computeOpCounts;
            std::vector<std::size_t> liveValueBytes;
            std::vector<std::size_t> temporaryBytes;
            std::vector<std::size_t> emittedCodeUnits;
            std::vector<std::size_t> helperCallCounts;
            std::vector<std::size_t> branchCounts;
            computeOpCounts.reserve(build.supernodeToOps.size());
            liveValueBytes.reserve(build.supernodeToOps.size());
            temporaryBytes.reserve(build.supernodeToOps.size());
            emittedCodeUnits.reserve(build.supernodeToOps.size());
            helperCallCounts.reserve(build.supernodeToOps.size());
            branchCounts.reserve(build.supernodeToOps.size());
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                ++stats.computeSupernodes;
                computeOpCounts.push_back(build.supernodeToOps[supernodeId].size());
                std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
                    liveValues;
                std::size_t liveBytes = 0;
                std::size_t tempBytes = 0;
                std::size_t codeUnits = 0;
                std::size_t helperCalls = 0;
                std::size_t branches = 0;
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    const auto canonicalDataOp = activityCanonicalDataOpForOp(graph, opId, rewrite.canonicalValues);
                    const auto dataOp = canonicalDataOp.valid() ? graph.getOperation(canonicalDataOp) : op;
                    const auto kind = dataOp.kind();
                    const int32_t width = activityOpResultWidth(graph, canonicalDataOp.valid() ? canonicalDataOp
                                                                                               : opId);
                    codeUnits += static_cast<std::size_t>(activityComputeUnitsForWidth(width));
                    helperCalls += activityScheduleHelperCallEstimate(kind, width);
                    branches += activityScheduleBranchEstimate(kind);
                    for (const auto result : op.results())
                    {
                        const auto canonical = canonicalActivityValue(result, &rewrite.canonicalValues);
                        if (canonical.valid() && liveValues.insert(canonical).second)
                        {
                            const std::size_t bytes = activityScheduleValueByteCost(graph, canonical.index);
                            liveBytes += bytes;
                            tempBytes += bytes;
                        }
                    }
                    for (const auto operand : op.operands())
                    {
                        const auto canonical = canonicalActivityValue(operand, &rewrite.canonicalValues);
                        if (canonical.valid() && liveValues.insert(canonical).second)
                        {
                            liveBytes += activityScheduleValueByteCost(graph, canonical.index);
                        }
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            const auto canonical = canonicalActivityValue(*indexValue, &rewrite.canonicalValues);
                            if (canonical.valid() && liveValues.insert(canonical).second)
                            {
                                liveBytes += activityScheduleValueByteCost(graph, canonical.index);
                            }
                        }
                    }
                }
                liveValueBytes.push_back(liveBytes);
                temporaryBytes.push_back(tempBytes);
                emittedCodeUnits.push_back(codeUnits);
                helperCallCounts.push_back(helperCalls);
                branchCounts.push_back(branches);
            }

            recordActivityScheduleResourceDistribution(stats.resourceP50,
                                                       stats.resourceP90,
                                                       stats.resourceP99,
                                                       stats.resourceP995,
                                                       stats.resourceMax,
                                                       stats.resourceCap,
                                                       stats.resourceBaselineExceptions,
                                                       "op_count",
                                                       computeOpCounts);
            recordActivityScheduleResourceDistribution(stats.resourceP50,
                                                       stats.resourceP90,
                                                       stats.resourceP99,
                                                       stats.resourceP995,
                                                       stats.resourceMax,
                                                       stats.resourceCap,
                                                       stats.resourceBaselineExceptions,
                                                       "live_value_bytes",
                                                       liveValueBytes);
            recordActivityScheduleResourceDistribution(stats.resourceP50,
                                                       stats.resourceP90,
                                                       stats.resourceP99,
                                                       stats.resourceP995,
                                                       stats.resourceMax,
                                                       stats.resourceCap,
                                                       stats.resourceBaselineExceptions,
                                                       "temporary_bytes",
                                                       temporaryBytes);
            recordActivityScheduleResourceDistribution(stats.resourceP50,
                                                       stats.resourceP90,
                                                       stats.resourceP99,
                                                       stats.resourceP995,
                                                       stats.resourceMax,
                                                       stats.resourceCap,
                                                       stats.resourceBaselineExceptions,
                                                       "emitted_code_units",
                                                       emittedCodeUnits);
            recordActivityScheduleResourceDistribution(stats.resourceP50,
                                                       stats.resourceP90,
                                                       stats.resourceP99,
                                                       stats.resourceP995,
                                                       stats.resourceMax,
                                                       stats.resourceCap,
                                                       stats.resourceBaselineExceptions,
                                                       "helper_call_count",
                                                       helperCallCounts);
            recordActivityScheduleResourceDistribution(stats.resourceP50,
                                                       stats.resourceP90,
                                                       stats.resourceP99,
                                                       stats.resourceP995,
                                                       stats.resourceMax,
                                                       stats.resourceCap,
                                                       stats.resourceBaselineExceptions,
                                                       "branch_count",
                                                       branchCounts);
            stats.computeSupernodeOpCountP50 = stats.resourceP50["op_count"];
            stats.computeSupernodeOpCountP90 = stats.resourceP90["op_count"];
            stats.computeSupernodeOpCountP99 = stats.resourceP99["op_count"];
            stats.computeSupernodeOpCountP995 = stats.resourceP995["op_count"];
            stats.computeSupernodeOpCountMax = stats.resourceMax["op_count"];
            stats.resourceOpCountCap = stats.resourceCap["op_count"];
            stats.resourceOpCountBaselineExceptions = stats.resourceBaselineExceptions["op_count"];

            constexpr std::size_t kTriggerSignatureBits = 256;
            constexpr std::size_t kTriggerSignatureHashFunctions = 4;
            constexpr std::size_t kTriggerSaturationThresholdBits = 192;
            stats.triggerSignatureBits = kTriggerSignatureBits;
            stats.triggerSignatureHashFunctions = kTriggerSignatureHashFunctions;
            stats.triggerSaturationThresholdBits = kTriggerSaturationThresholdBits;

            std::vector<ActivityScheduleTriggerSignature> triggerSignatures(build.supernodeToOps.size());
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
                volatileTriggerValues;
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                auto &signature = triggerSignatures[supernodeId];
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    if (!activityScheduleIsVolatileTriggerKind(op.kind()))
                    {
                        continue;
                    }
                    for (const auto result : op.results())
                    {
                        const auto canonical = canonicalActivityValue(result, &rewrite.canonicalValues);
                        if (!canonical.valid())
                        {
                            continue;
                        }
                        volatileTriggerValues.insert(canonical);
                        activityScheduleAddTrigger(signature, canonical);
                    }
                }
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    const auto noteInputTrigger = [&](wolvrix::lib::grh::ValueId value)
                    {
                        const auto canonical = canonicalActivityValue(value, &rewrite.canonicalValues);
                        if (!canonical.valid())
                        {
                            return;
                        }
                        if (graph.valueDef(canonical).valid() || !graph.valueIsInput(canonical))
                        {
                            return;
                        }
                        volatileTriggerValues.insert(canonical);
                        activityScheduleAddTrigger(signature, canonical);
                    };
                    for (const auto operand : op.operands())
                    {
                        noteInputTrigger(operand);
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            noteInputTrigger(*indexValue);
                        }
                    }
                }
            }
            stats.triggerVolatileSourceValues = volatileTriggerValues.size();

            for (const uint32_t supernodeId : build.topoOrder)
            {
                if (supernodeId >= build.dag.size())
                {
                    continue;
                }
                const auto &signature = triggerSignatures[supernodeId];
                if (signature.empty())
                {
                    continue;
                }
                for (const uint32_t succ : build.dag[supernodeId])
                {
                    if (succ < triggerSignatures.size())
                    {
                        triggerSignatures[succ].mergeFrom(signature);
                    }
                }
            }

            std::vector<std::size_t> triggerPopcounts;
            std::vector<std::size_t> triggerEstimatedCounts;
            triggerPopcounts.reserve(stats.computeSupernodes);
            triggerEstimatedCounts.reserve(stats.computeSupernodes);
            std::unordered_map<ActivityScheduleTriggerSignature,
                               std::size_t,
                               ActivityScheduleTriggerSignatureHash>
                triggerBuckets;
            std::unordered_map<ActivityScheduleTriggerSignature,
                               std::size_t,
                               ActivityScheduleTriggerSignatureHash>
                nonEmptyTriggerBuckets;
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                const auto &signature = triggerSignatures[supernodeId];
                const std::size_t popcount = activityScheduleTriggerPopcount(signature);
                triggerPopcounts.push_back(popcount);
                triggerEstimatedCounts.push_back(activityScheduleEstimatedTriggerCount(popcount));
                if (signature.empty())
                {
                    ++stats.triggerEmptyComputeSupernodes;
                }
                else
                {
                    ++stats.triggerComputeSupernodesWithTrigger;
                    ++nonEmptyTriggerBuckets[signature];
                }
                if (popcount >= kTriggerSaturationThresholdBits)
                {
                    ++stats.triggerSignatureSaturatedComputeSupernodes;
                }
                ++triggerBuckets[signature];
            }

            std::sort(triggerPopcounts.begin(), triggerPopcounts.end());
            std::sort(triggerEstimatedCounts.begin(), triggerEstimatedCounts.end());
            stats.triggerSignaturePopcountP50 = percentileFromSorted(triggerPopcounts, 50, 100);
            stats.triggerSignaturePopcountP90 = percentileFromSorted(triggerPopcounts, 90, 100);
            stats.triggerSignaturePopcountP99 = percentileFromSorted(triggerPopcounts, 99, 100);
            stats.triggerSignaturePopcountP995 = percentileFromSorted(triggerPopcounts, 995, 1000);
            stats.triggerSignaturePopcountMax = triggerPopcounts.empty() ? 0 : triggerPopcounts.back();
            stats.triggerEstimatedCountP50 = percentileFromSorted(triggerEstimatedCounts, 50, 100);
            stats.triggerEstimatedCountP90 = percentileFromSorted(triggerEstimatedCounts, 90, 100);
            stats.triggerEstimatedCountP99 = percentileFromSorted(triggerEstimatedCounts, 99, 100);
            stats.triggerEstimatedCountP995 = percentileFromSorted(triggerEstimatedCounts, 995, 1000);
            stats.triggerEstimatedCountMax =
                triggerEstimatedCounts.empty() ? 0 : triggerEstimatedCounts.back();
            stats.triggerSignatureSaturatedRatioPpm =
                activityScheduleRatioPpm(stats.triggerSignatureSaturatedComputeSupernodes,
                                         stats.computeSupernodes);

            stats.triggerEqualBucketCount = triggerBuckets.size();
            for (const auto &[signature, count] : triggerBuckets)
            {
                (void)signature;
                stats.triggerEqualBucketLargest = std::max(stats.triggerEqualBucketLargest, count);
                if (count > 1)
                {
                    ++stats.triggerEqualBucketMultiCount;
                    stats.triggerEqualBucketCoveredSupernodes += count;
                }
            }
            stats.triggerEqualBucketCoveredSupernodeRatioPpm =
                activityScheduleRatioPpm(stats.triggerEqualBucketCoveredSupernodes,
                                         stats.computeSupernodes);

            stats.triggerNonEmptyEqualBucketCount = nonEmptyTriggerBuckets.size();
            for (const auto &[signature, count] : nonEmptyTriggerBuckets)
            {
                (void)signature;
                stats.triggerNonEmptyEqualBucketLargest =
                    std::max(stats.triggerNonEmptyEqualBucketLargest, count);
                if (count > 1)
                {
                    ++stats.triggerNonEmptyEqualBucketMultiCount;
                    stats.triggerNonEmptyEqualBucketCoveredSupernodes += count;
                }
            }
            stats.triggerNonEmptyEqualBucketCoveredSupernodeRatioPpm =
                activityScheduleRatioPpm(stats.triggerNonEmptyEqualBucketCoveredSupernodes,
                                         stats.computeSupernodes);

            std::unordered_set<std::uint64_t> equalTriggerDependencyEdges;
            std::unordered_set<std::uint64_t> nonEmptyEqualTriggerDependencyEdges;
            for (std::size_t valueOffset = 0; valueOffset < build.valueFanout.size(); ++valueOffset)
            {
                const auto &targets = build.valueFanout[valueOffset];
                if (targets.empty())
                {
                    continue;
                }
                const uint32_t sourceSupernode =
                    valueOffset + 1 < build.valueSourceSupernode.size()
                        ? build.valueSourceSupernode[valueOffset + 1]
                        : kInvalidActivitySupernodeId;
                if (sourceSupernode >= build.supernodeKinds.size() ||
                    build.supernodeKinds[sourceSupernode] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                const auto &sourceSignature = triggerSignatures[sourceSupernode];
                for (const uint32_t target : targets)
                {
                    if (target >= build.supernodeKinds.size() ||
                        build.supernodeKinds[target] != ActivityScheduleSupernodeKind::Compute)
                    {
                        continue;
                    }
                    if (!(sourceSignature == triggerSignatures[target]))
                    {
                        continue;
                    }
                    ++stats.triggerEqualBucketInternalizableBoundaryTargets;
                    ++stats.triggerEqualBucketInternalizableComputeTargets;
                    equalTriggerDependencyEdges.insert(packActivitySchedulePair(sourceSupernode, target));
                    if (!sourceSignature.empty())
                    {
                        ++stats.triggerNonEmptyEqualBucketInternalizableBoundaryTargets;
                        ++stats.triggerNonEmptyEqualBucketInternalizableComputeTargets;
                        nonEmptyEqualTriggerDependencyEdges.insert(
                            packActivitySchedulePair(sourceSupernode, target));
                    }
                }
            }
            stats.triggerEqualBucketInternalizableDependencyEdges =
                equalTriggerDependencyEdges.size();
            stats.triggerNonEmptyEqualBucketInternalizableDependencyEdges =
                nonEmptyEqualTriggerDependencyEdges.size();
            const bool triggerSaturated =
                stats.triggerSignatureSaturatedRatioPpm > 250000;
            const bool equalCoverageLow =
                stats.triggerNonEmptyEqualBucketCoveredSupernodeRatioPpm < 10000 ||
                stats.triggerNonEmptyEqualBucketInternalizableComputeTargets == 0;
            if (stats.computeSupernodes == 0)
            {
                stats.triggerAteNoGoReason = "no_compute_supernodes";
            }
            else if (triggerSaturated)
            {
                stats.triggerAteNoGoReason = "trigger_signature_saturation";
            }
            else if (equalCoverageLow)
            {
                stats.triggerAteNoGoReason = "low_equal_trigger_coverage";
            }
            else
            {
                stats.triggerAteEqualMergeRecommended = 1;
            }

            const auto recordSeedGroup = [&](std::string_view rule, std::size_t members)
            {
                if (members == 0)
                {
                    return;
                }
                ++stats.semanticSeedGroups;
                stats.semanticRuleSeedGroups[std::string(rule)] += 1;
            };
            const auto recordMergeHintGroup = [&](std::string_view rule, std::size_t members)
            {
                if (members == 0)
                {
                    return;
                }
                ++stats.semanticMergeHintGroups;
                stats.semanticRuleMergeHintGroups[std::string(rule)] += 1;
            };
            const auto recordDebugLabel = [&](std::string_view rule, std::size_t count = 1)
            {
                if (count == 0)
                {
                    return;
                }
                stats.semanticDebugLabels += count;
                stats.semanticRuleDebugLabels[std::string(rule)] += count;
            };

            std::unordered_set<std::string> rtmGroups;
            std::unordered_set<std::string> aggregateFamilies;
            std::unordered_map<uint32_t, std::size_t> predCountBySupernode;
            std::map<std::vector<uint32_t>, std::size_t> siblingMembersByPreds;
            std::unordered_map<std::string, std::size_t> guardDomainMembers;
            std::unordered_map<std::string, std::size_t> sinkConeMembers;
            std::unordered_set<std::string> sinkConeLabels;
            std::unordered_set<uint32_t> rtmSupernodes;
            std::unordered_set<uint32_t> aggregateSupernodes;
            std::unordered_set<uint32_t> guardSupernodes;
            std::unordered_set<uint32_t> sinkSupernodes;
            std::unordered_set<uint32_t> passthroughSupernodes;
            std::vector<std::vector<uint32_t>> computePredsBySupernode(build.supernodeToOps.size());

            for (uint32_t source = 0; source < build.dag.size(); ++source)
            {
                const auto &succs = build.dag[source];
                if (source < build.supernodeKinds.size() &&
                    build.supernodeKinds[source] == ActivityScheduleSupernodeKind::Compute &&
                    succs.size() == 1)
                {
                    ++stats.semanticPlainOut1Hints;
                    recordMergeHintGroup("plain_out1", 2);
                }
                for (const uint32_t succ : succs)
                {
                    if (succ < build.supernodeKinds.size() &&
                        build.supernodeKinds[succ] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++predCountBySupernode[succ];
                        if (source < build.supernodeKinds.size() &&
                            build.supernodeKinds[source] == ActivityScheduleSupernodeKind::Compute &&
                            succ < computePredsBySupernode.size())
                        {
                            computePredsBySupernode[succ].push_back(source);
                        }
                    }
                }
            }
            for (const auto &[supernode, predCount] : predCountBySupernode)
            {
                if (supernode < build.supernodeKinds.size() &&
                    build.supernodeKinds[supernode] == ActivityScheduleSupernodeKind::Compute &&
                    predCount == 1)
                {
                    ++stats.semanticPlainIn1Hints;
                    recordMergeHintGroup("plain_in1", 2);
                }
            }
            for (uint32_t target = 0; target < build.supernodeToOps.size(); ++target)
            {
                if (target >= build.supernodeKinds.size() ||
                    build.supernodeKinds[target] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                auto preds = target < computePredsBySupernode.size()
                                 ? computePredsBySupernode[target]
                                 : std::vector<uint32_t>{};
                std::sort(preds.begin(), preds.end());
                preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
                if (!preds.empty())
                {
                    ++siblingMembersByPreds[preds];
                }
            }
            for (const auto &[preds, members] : siblingMembersByPreds)
            {
                (void)preds;
                if (members > 1)
                {
                    ++stats.semanticPlainSiblingGroups;
                    stats.semanticPlainSiblingMembers += members;
                    recordMergeHintGroup("plain_siblings", members);
                }
            }

            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                uint32_t currentPassthroughRun = 0;
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    const auto kind = op.kind();
                    if (activityScheduleHasRegToMemIntent(op))
                    {
                        const auto group = getAttrString(op, "regToMem.intent.group").value_or(std::string());
                        if (!group.empty())
                        {
                            rtmGroups.insert(group);
                        }
                        ++stats.semanticRtmIntentOps;
                        rtmSupernodes.insert(supernodeId);
                        recordDebugLabel("rtm_intent");
                    }
                    if (activityScheduleIsAggregateShapeKind(kind))
                    {
                        std::string key;
                        if (const auto group = getAttrString(op, "regToMem.intent.group"))
                        {
                            key = "rtm:" + *group;
                        }
                        else if (!op.operands().empty())
                        {
                            const auto canonical = canonicalActivityValue(op.operands().front(),
                                                                          &rewrite.canonicalValues);
                            key = canonical.valid() ? "value:" + std::to_string(canonical.index)
                                                    : "op:" + std::to_string(opId.index);
                        }
                        else
                        {
                            key = "op:" + std::to_string(opId.index);
                        }
                        aggregateFamilies.insert(key);
                        aggregateSupernodes.insert(supernodeId);
                        recordDebugLabel("aggregate_shape");
                    }
                    if (activityScheduleIsGuardLikeKind(kind))
                    {
                        std::string guardKey;
                        if (kind == wolvrix::lib::grh::OperationKind::kMux && !op.operands().empty())
                        {
                            const auto canonical = canonicalActivityValue(op.operands().front(),
                                                                          &rewrite.canonicalValues);
                            guardKey = canonical.valid() ? "mux:" + std::to_string(canonical.index)
                                                         : "mux:unknown";
                        }
                        else if ((kind == wolvrix::lib::grh::OperationKind::kAnd ||
                                  kind == wolvrix::lib::grh::OperationKind::kLogicAnd ||
                                  kind == wolvrix::lib::grh::OperationKind::kOr ||
                                  kind == wolvrix::lib::grh::OperationKind::kLogicOr) &&
                                 !op.operands().empty())
                        {
                            std::vector<std::size_t> oneBitInputs;
                            for (const auto operand : op.operands())
                            {
                                const auto canonical = canonicalActivityValue(operand,
                                                                              &rewrite.canonicalValues);
                                if (canonical.valid() && graph.valueWidth(canonical) == 1)
                                {
                                    oneBitInputs.push_back(canonical.index);
                                }
                            }
                            if (!oneBitInputs.empty())
                            {
                                std::sort(oneBitInputs.begin(), oneBitInputs.end());
                                guardKey = "logic:" + std::to_string(oneBitInputs.front());
                            }
                        }
                        if (guardKey.empty())
                        {
                            ++stats.semanticGuardUnknownOps;
                            recordDebugLabel("guard_unknown");
                        }
                        else
                        {
                            ++guardDomainMembers[guardKey];
                            guardSupernodes.insert(supernodeId);
                            recordDebugLabel("guard_domain");
                        }
                    }
                    if (activityScheduleIsPassthroughKind(kind))
                    {
                        ++currentPassthroughRun;
                        ++stats.semanticPassthroughOps;
                        passthroughSupernodes.insert(supernodeId);
                        recordDebugLabel("passthrough_chain");
                    }
                    else
                    {
                        if (currentPassthroughRun >= 2)
                        {
                            ++stats.semanticPassthroughChains;
                            recordSeedGroup("passthrough_chain", currentPassthroughRun);
                        }
                        currentPassthroughRun = 0;
                    }
                    if (isHierLikeOpKind(kind))
                    {
                        ++stats.semanticHierarchyDebugLabels;
                        recordDebugLabel("hierarchy_info");
                    }
                }
                if (currentPassthroughRun >= 2)
                {
                    ++stats.semanticPassthroughChains;
                    recordSeedGroup("passthrough_chain", currentPassthroughRun);
                }
            }

            stats.semanticRtmIntentGroups = rtmGroups.size();
            for (const auto &group : rtmGroups)
            {
                (void)group;
                recordSeedGroup("rtm_intent", 1);
                recordMergeHintGroup("aggregate_array", 1);
            }
            stats.semanticAggregateFamilies = aggregateFamilies.size();
            for (const auto &family : aggregateFamilies)
            {
                (void)family;
                ++stats.semanticAggregateMergeHintGroups;
                recordMergeHintGroup("aggregate_family", 1);
            }
            stats.semanticAggregateSeedGroups = stats.semanticRtmIntentGroups;

            for (const auto &node : rewrite.computeNodes)
            {
                if (node.ops.empty())
                {
                    continue;
                }
                ++stats.semanticMffcGroups;
                stats.semanticMffcCoveredOps += node.ops.size();
                if (node.boundaryInputs.size() > 1 || node.commonExpr)
                {
                    ++stats.semanticMffcSplitGroups;
                }
            }
            if (stats.semanticMffcGroups != 0)
            {
                stats.semanticRuleSeedGroups["mffc"] += stats.semanticMffcGroups;
                stats.semanticSeedGroups += stats.semanticMffcGroups;
            }
            if (stats.semanticMffcSplitGroups != 0)
            {
                stats.semanticRuleMergeHintGroups["mffc_split"] += stats.semanticMffcSplitGroups;
                stats.semanticMergeHintGroups += stats.semanticMffcSplitGroups;
            }

            for (const auto &[key, members] : guardDomainMembers)
            {
                (void)key;
                if (members == 0)
                {
                    continue;
                }
                ++stats.semanticGuardDomains;
                stats.semanticGuardDomainMembers += members;
                if (members > 1)
                {
                    recordMergeHintGroup("guard_domain", members);
                }
            }

            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                constexpr std::size_t kMaxSinkLabelsPerSupernode = 8;
                std::unordered_set<std::string> localSinkLabels;
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    if (localSinkLabels.size() >= kMaxSinkLabelsPerSupernode)
                    {
                        break;
                    }
                    const auto op = graph.getOperation(opId);
                    for (const auto result : op.results())
                    {
                        if (!result.valid())
                        {
                            continue;
                        }
                        const auto value = graph.getValue(result);
                        for (const auto &user : value.users())
                        {
                            const auto userOp = graph.getOperation(user.operation);
                            if (!isSinkPartitionOp(userOp))
                            {
                                continue;
                            }
                            std::string label =
                                std::string(wolvrix::lib::grh::toString(userOp.kind()));
                            if (const auto reg = getAttrString(userOp, "regSymbol"))
                            {
                                label += ":" + *reg;
                            }
                            else if (const auto latch = getAttrString(userOp, "latchSymbol"))
                            {
                                label += ":" + *latch;
                            }
                            else if (const auto mem = getAttrString(userOp, "memSymbol"))
                            {
                                label += ":" + *mem;
                            }
                            label += ":operand" + std::to_string(user.operandIndex);
                            localSinkLabels.insert(label);
                            sinkConeLabels.insert(label);
                            if (localSinkLabels.size() >= kMaxSinkLabelsPerSupernode)
                            {
                                break;
                            }
                        }
                    }
                }
                if (localSinkLabels.size() > 1)
                {
                    ++stats.semanticSinkConeMultiSinkOps;
                    recordDebugLabel("sink_cone_multi_sink");
                }
                for (const auto &label : localSinkLabels)
                {
                    ++sinkConeMembers[label];
                    sinkSupernodes.insert(supernodeId);
                    recordDebugLabel("sink_cone_exact");
                }
            }
            stats.semanticSinkConeLabels = sinkConeLabels.size();
            for (const auto &[label, members] : sinkConeMembers)
            {
                (void)label;
                stats.semanticSinkConeMembers += members;
                if (members > 1)
                {
                    recordMergeHintGroup("sink_cone", members);
                }
            }

            struct CbawAtom
            {
                std::vector<wolvrix::lib::grh::OperationId> ops;
                std::vector<uint32_t> computeNodes;
                uint32_t plainSupernode = kInvalidActivitySupernodeId;
                bool rtmIntent = false;
                bool mffc = false;
                bool passthrough = false;
                bool aggregate = false;
                bool guard = false;
            };

            std::size_t maxOpIndex = build.opToSupernode.size();
            for (const auto opId : graph.operations())
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            std::vector<uint32_t> opToCbawAtom(maxOpIndex + 1, kInvalidActivitySupernodeId);
            std::vector<CbawAtom> cbawAtoms;
            cbawAtoms.reserve(stats.computeSupernodes);

            std::vector<uint32_t> computeNodeMaterializedUses(rewrite.computeNodes.size(), 0);
            for (uint32_t supernodeId = 0; supernodeId < build.computeNodesBySupernode.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                for (const uint32_t computeNodeId : build.computeNodesBySupernode[supernodeId])
                {
                    if (computeNodeId < computeNodeMaterializedUses.size())
                    {
                        ++computeNodeMaterializedUses[computeNodeId];
                    }
                }
            }

            const auto addCbawAtom = [&](uint32_t plainSupernode,
                                         std::vector<wolvrix::lib::grh::OperationId> ops,
                                         std::vector<uint32_t> computeNodes)
            {
                if (ops.empty())
                {
                    return;
                }
                CbawAtom atom;
                atom.ops = std::move(ops);
                atom.computeNodes = std::move(computeNodes);
                atom.plainSupernode = plainSupernode;
                atom.mffc = atom.computeNodes.size() == 1;
                bool allPassthrough = true;
                for (const auto opId : atom.ops)
                {
                    const auto op = graph.getOperation(opId);
                    const auto kind = op.kind();
                    atom.rtmIntent = atom.rtmIntent || activityScheduleHasRegToMemIntent(op);
                    atom.aggregate = atom.aggregate || activityScheduleIsAggregateShapeKind(kind);
                    atom.guard = atom.guard || activityScheduleIsGuardLikeKind(kind);
                    allPassthrough = allPassthrough && activityScheduleIsPassthroughKind(kind);
                }
                atom.passthrough = allPassthrough;

                const uint32_t atomId = static_cast<uint32_t>(cbawAtoms.size());
                for (const auto opId : atom.ops)
                {
                    if (opId.index < opToCbawAtom.size())
                    {
                        opToCbawAtom[opId.index] = atomId;
                    }
                }
                if (atom.rtmIntent)
                {
                    ++stats.cbawAtomRtmIntentAtoms;
                }
                if (atom.mffc)
                {
                    ++stats.cbawAtomMffcAtoms;
                }
                if (atom.passthrough)
                {
                    ++stats.cbawAtomPassthroughAtoms;
                }
                if (atom.aggregate)
                {
                    ++stats.cbawAtomAggregateAtoms;
                }
                if (atom.guard)
                {
                    ++stats.cbawAtomGuardAtoms;
                }
                if (atom.rtmIntent)
                {
                    ++stats.cbawAtomKindCounts["rtm_intent"];
                }
                else if (atom.passthrough)
                {
                    ++stats.cbawAtomKindCounts["passthrough"];
                }
                else if (atom.aggregate)
                {
                    ++stats.cbawAtomKindCounts["aggregate"];
                }
                else if (atom.guard)
                {
                    ++stats.cbawAtomKindCounts["guard"];
                }
                else if (atom.mffc)
                {
                    ++stats.cbawAtomKindCounts["mffc"];
                }
                else
                {
                    ++stats.cbawAtomKindCounts["plain_chunk"];
                }
                cbawAtoms.push_back(std::move(atom));
            };

            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                std::vector<uint8_t> assigned(build.supernodeToOps[supernodeId].size(), 0);
                const auto &memberComputeNodes =
                    supernodeId < build.computeNodesBySupernode.size()
                        ? build.computeNodesBySupernode[supernodeId]
                        : std::vector<uint32_t>{};
                if (memberComputeNodes.empty())
                {
                    addCbawAtom(supernodeId, build.supernodeToOps[supernodeId], {});
                    continue;
                }
                for (const uint32_t computeNodeId : memberComputeNodes)
                {
                    if (computeNodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    std::vector<wolvrix::lib::grh::OperationId> atomOps;
                    for (const auto opId : rewrite.computeNodes[computeNodeId].ops)
                    {
                        if (opId.index == 0 || opId.index - 1 >= build.opToSupernode.size() ||
                            build.opToSupernode[opId.index - 1] != supernodeId)
                        {
                            continue;
                        }
                        atomOps.push_back(opId);
                        const auto posIt = std::find(build.supernodeToOps[supernodeId].begin(),
                                                     build.supernodeToOps[supernodeId].end(),
                                                     opId);
                        if (posIt != build.supernodeToOps[supernodeId].end())
                        {
                            assigned[static_cast<std::size_t>(
                                std::distance(build.supernodeToOps[supernodeId].begin(), posIt))] = 1;
                        }
                    }
                    addCbawAtom(supernodeId, std::move(atomOps), {computeNodeId});
                }
                std::vector<wolvrix::lib::grh::OperationId> residualOps;
                for (std::size_t i = 0; i < build.supernodeToOps[supernodeId].size(); ++i)
                {
                    if (assigned[i] == 0)
                    {
                        residualOps.push_back(build.supernodeToOps[supernodeId][i]);
                    }
                }
                addCbawAtom(supernodeId, std::move(residualOps), {});
            }

            stats.cbawAtomCount = cbawAtoms.size();
            std::vector<std::size_t> atomOpCounts;
            std::vector<std::size_t> atomLiveValueBytes;
            std::vector<std::size_t> atomTemporaryBytes;
            std::vector<std::size_t> atomEmittedCodeUnits;
            std::vector<std::size_t> atomHelperCallCounts;
            std::vector<std::size_t> atomBranchCounts;
            atomOpCounts.reserve(cbawAtoms.size());
            atomLiveValueBytes.reserve(cbawAtoms.size());
            atomTemporaryBytes.reserve(cbawAtoms.size());
            atomEmittedCodeUnits.reserve(cbawAtoms.size());
            atomHelperCallCounts.reserve(cbawAtoms.size());
            atomBranchCounts.reserve(cbawAtoms.size());
            for (const auto &atom : cbawAtoms)
            {
                atomOpCounts.push_back(atom.ops.size());
                std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
                    liveValues;
                std::size_t liveBytes = 0;
                std::size_t tempBytes = 0;
                std::size_t codeUnits = 0;
                std::size_t helperCalls = 0;
                std::size_t branches = 0;
                for (const auto opId : atom.ops)
                {
                    const auto op = graph.getOperation(opId);
                    const auto canonicalDataOp = activityCanonicalDataOpForOp(graph, opId, rewrite.canonicalValues);
                    const auto dataOp = canonicalDataOp.valid() ? graph.getOperation(canonicalDataOp) : op;
                    const auto kind = dataOp.kind();
                    const int32_t width = activityOpResultWidth(graph, canonicalDataOp.valid() ? canonicalDataOp
                                                                                               : opId);
                    codeUnits += static_cast<std::size_t>(activityComputeUnitsForWidth(width));
                    helperCalls += activityScheduleHelperCallEstimate(kind, width);
                    branches += activityScheduleBranchEstimate(kind);
                    for (const auto result : op.results())
                    {
                        const auto canonical = canonicalActivityValue(result, &rewrite.canonicalValues);
                        if (canonical.valid() && liveValues.insert(canonical).second)
                        {
                            const std::size_t bytes = activityScheduleValueByteCost(graph, canonical.index);
                            liveBytes += bytes;
                            tempBytes += bytes;
                        }
                    }
                    for (const auto operand : op.operands())
                    {
                        const auto canonical = canonicalActivityValue(operand, &rewrite.canonicalValues);
                        if (canonical.valid() && liveValues.insert(canonical).second)
                        {
                            liveBytes += activityScheduleValueByteCost(graph, canonical.index);
                        }
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            const auto canonical = canonicalActivityValue(*indexValue, &rewrite.canonicalValues);
                            if (canonical.valid() && liveValues.insert(canonical).second)
                            {
                                liveBytes += activityScheduleValueByteCost(graph, canonical.index);
                            }
                        }
                    }
                }
                atomLiveValueBytes.push_back(liveBytes);
                atomTemporaryBytes.push_back(tempBytes);
                atomEmittedCodeUnits.push_back(codeUnits);
                atomHelperCallCounts.push_back(helperCalls);
                atomBranchCounts.push_back(branches);
            }

            recordActivityScheduleResourceDistribution(stats.cbawAtomResourceP50,
                                                       stats.cbawAtomResourceP90,
                                                       stats.cbawAtomResourceP99,
                                                       stats.cbawAtomResourceP995,
                                                       stats.cbawAtomResourceMax,
                                                       stats.cbawAtomResourceCap,
                                                       stats.cbawAtomResourceBaselineExceptions,
                                                       "op_count",
                                                       atomOpCounts);
            recordActivityScheduleResourceDistribution(stats.cbawAtomResourceP50,
                                                       stats.cbawAtomResourceP90,
                                                       stats.cbawAtomResourceP99,
                                                       stats.cbawAtomResourceP995,
                                                       stats.cbawAtomResourceMax,
                                                       stats.cbawAtomResourceCap,
                                                       stats.cbawAtomResourceBaselineExceptions,
                                                       "live_value_bytes",
                                                       atomLiveValueBytes);
            recordActivityScheduleResourceDistribution(stats.cbawAtomResourceP50,
                                                       stats.cbawAtomResourceP90,
                                                       stats.cbawAtomResourceP99,
                                                       stats.cbawAtomResourceP995,
                                                       stats.cbawAtomResourceMax,
                                                       stats.cbawAtomResourceCap,
                                                       stats.cbawAtomResourceBaselineExceptions,
                                                       "temporary_bytes",
                                                       atomTemporaryBytes);
            recordActivityScheduleResourceDistribution(stats.cbawAtomResourceP50,
                                                       stats.cbawAtomResourceP90,
                                                       stats.cbawAtomResourceP99,
                                                       stats.cbawAtomResourceP995,
                                                       stats.cbawAtomResourceMax,
                                                       stats.cbawAtomResourceCap,
                                                       stats.cbawAtomResourceBaselineExceptions,
                                                       "emitted_code_units",
                                                       atomEmittedCodeUnits);
            recordActivityScheduleResourceDistribution(stats.cbawAtomResourceP50,
                                                       stats.cbawAtomResourceP90,
                                                       stats.cbawAtomResourceP99,
                                                       stats.cbawAtomResourceP995,
                                                       stats.cbawAtomResourceMax,
                                                       stats.cbawAtomResourceCap,
                                                       stats.cbawAtomResourceBaselineExceptions,
                                                       "helper_call_count",
                                                       atomHelperCallCounts);
            recordActivityScheduleResourceDistribution(stats.cbawAtomResourceP50,
                                                       stats.cbawAtomResourceP90,
                                                       stats.cbawAtomResourceP99,
                                                       stats.cbawAtomResourceP995,
                                                       stats.cbawAtomResourceMax,
                                                       stats.cbawAtomResourceCap,
                                                       stats.cbawAtomResourceBaselineExceptions,
                                                       "branch_count",
                                                       atomBranchCounts);
            stats.cbawAtomOpCountP50 = stats.cbawAtomResourceP50["op_count"];
            stats.cbawAtomOpCountP90 = stats.cbawAtomResourceP90["op_count"];
            stats.cbawAtomOpCountP99 = stats.cbawAtomResourceP99["op_count"];
            stats.cbawAtomOpCountP995 = stats.cbawAtomResourceP995["op_count"];
            stats.cbawAtomOpCountMax = stats.cbawAtomResourceMax["op_count"];
            stats.cbawAtomResourceOpCountCap = stats.cbawAtomResourceCap["op_count"];
            stats.cbawAtomResourceOpCountBaselineExceptions =
                stats.cbawAtomResourceBaselineExceptions["op_count"];

            ActivityScheduleDag atomDag(cbawAtoms.size());
            std::unordered_set<std::uint64_t> atomEdges;
            const auto addAtomDependency = [&](wolvrix::lib::grh::ValueId value, uint32_t toAtom)
            {
                if (!value.valid() || toAtom >= cbawAtoms.size())
                {
                    return;
                }
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid() || defOp.index >= opToCbawAtom.size())
                {
                    return;
                }
                const uint32_t fromAtom = opToCbawAtom[defOp.index];
                if (fromAtom == kInvalidActivitySupernodeId || fromAtom == toAtom)
                {
                    return;
                }
                const auto packed = packActivitySchedulePair(fromAtom, toAtom);
                if (atomEdges.insert(packed).second)
                {
                    atomDag[fromAtom].push_back(toAtom);
                }
            };
            for (uint32_t atomId = 0; atomId < cbawAtoms.size(); ++atomId)
            {
                for (const auto opId : cbawAtoms[atomId].ops)
                {
                    const auto op = graph.getOperation(opId);
                    for (const auto operand : op.operands())
                    {
                        addAtomDependency(operand, atomId);
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            addAtomDependency(*indexValue, atomId);
                        }
                    }
                }
            }
            for (auto &succs : atomDag)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            stats.cbawAtomQuotientEdges = atomEdges.size();
            stats.cbawAtomQuotientCycleDetected = activityScheduleDagHasCycle(atomDag) ? 1 : 0;

            std::vector<uint32_t> cbawAtomPlainPartition(cbawAtoms.size(), kInvalidActivitySupernodeId);
            std::unordered_set<uint32_t> plainReplayComputeSupernodes;
            for (uint32_t atomId = 0; atomId < cbawAtoms.size(); ++atomId)
            {
                cbawAtomPlainPartition[atomId] = cbawAtoms[atomId].plainSupernode;
                if (cbawAtoms[atomId].plainSupernode != kInvalidActivitySupernodeId)
                {
                    plainReplayComputeSupernodes.insert(cbawAtoms[atomId].plainSupernode);
                }
            }
            stats.cbawAtomPlainReplaySupernodes = plainReplayComputeSupernodes.size();

            std::vector<uint32_t> splitOwnerComputeNodeBySupernode(build.supernodeToOps.size(),
                                                                   kInvalidActivitySupernodeId);
            std::vector<uint32_t> splitOrdinalBySupernode(build.supernodeToOps.size(),
                                                          kInvalidActivitySupernodeId);
            std::vector<uint32_t> splitOrdinalByComputeNode(rewrite.computeNodes.size(), 0);
            for (uint32_t supernodeId = 0; supernodeId < build.computeNodesBySupernode.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute ||
                    build.computeNodesBySupernode[supernodeId].size() != 1)
                {
                    continue;
                }
                const uint32_t computeNodeId = build.computeNodesBySupernode[supernodeId].front();
                if (computeNodeId >= computeNodeMaterializedUses.size() ||
                    computeNodeMaterializedUses[computeNodeId] <= 1)
                {
                    continue;
                }
                splitOwnerComputeNodeBySupernode[supernodeId] = computeNodeId;
                splitOrdinalBySupernode[supernodeId] = splitOrdinalByComputeNode[computeNodeId]++;
            }

            ActivityScheduleValueFanout atomPlainReplayFanout;
            if (!graph.values().empty())
            {
                atomPlainReplayFanout.assign(graph.values().back().index, {});
            }
            std::unordered_set<std::uint64_t> atomPlainReplayDependencyEdges;
            const auto plainReplaySupernodeForOp = [&](wolvrix::lib::grh::OperationId opId)
            {
                if (opId.valid() && opId.index < opToCbawAtom.size())
                {
                    const uint32_t atomId = opToCbawAtom[opId.index];
                    if (atomId != kInvalidActivitySupernodeId &&
                        atomId < cbawAtomPlainPartition.size())
                    {
                        return cbawAtomPlainPartition[atomId];
                    }
                }
                if (opId.valid() && opId.index > 0 && opId.index - 1 < build.opToSupernode.size())
                {
                    return build.opToSupernode[opId.index - 1];
                }
                return kInvalidActivitySupernodeId;
            };
            const auto addAtomPlainReplayDependency = [&](wolvrix::lib::grh::ValueId value,
                                                          uint32_t to,
                                                          bool skipDagEdge = false)
            {
                if (!value.valid() || to >= build.supernodeKinds.size())
                {
                    return;
                }
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    if (!skipDagEdge && value.index > 0 && value.index <= atomPlainReplayFanout.size())
                    {
                        atomPlainReplayFanout[value.index - 1].push_back(to);
                    }
                    return;
                }
                const uint32_t from = plainReplaySupernodeForOp(defOp);
                if (from == kInvalidActivitySupernodeId || from == to)
                {
                    return;
                }
                if (from < build.supernodeKinds.size() &&
                    build.supernodeKinds[from] == ActivityScheduleSupernodeKind::Commit)
                {
                    return;
                }
                if (!skipDagEdge)
                {
                    atomPlainReplayDependencyEdges.insert(packActivitySchedulePair(from, to));
                    if (value.index > 0 && value.index <= atomPlainReplayFanout.size())
                    {
                        atomPlainReplayFanout[value.index - 1].push_back(to);
                    }
                }
            };

            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto toOpId : build.supernodeToOps[supernodeId])
                {
                    const auto toOp = graph.getOperation(toOpId);
                    const uint32_t to = plainReplaySupernodeForOp(toOpId);
                    if (to == kInvalidActivitySupernodeId)
                    {
                        continue;
                    }
                    for (const auto operand : toOp.operands())
                    {
                        const auto defOp = graph.valueDef(operand);
                        if (!defOp.valid())
                        {
                            continue;
                        }
                        const uint32_t from = plainReplaySupernodeForOp(defOp);
                        bool skipDagEdge = false;
                        if (defOp.index < rewrite.computeNodeOfOp.size() &&
                            toOpId.index < rewrite.computeNodeOfOp.size())
                        {
                            const uint32_t defComputeNode = rewrite.computeNodeOfOp[defOp.index];
                            const uint32_t useComputeNode = rewrite.computeNodeOfOp[toOpId.index];
                            if (defComputeNode != kInvalidActivitySupernodeId &&
                                defComputeNode == useComputeNode &&
                                from != to)
                            {
                                const bool splitForward =
                                    from < splitOwnerComputeNodeBySupernode.size() &&
                                    to < splitOwnerComputeNodeBySupernode.size() &&
                                    splitOwnerComputeNodeBySupernode[from] == defComputeNode &&
                                    splitOwnerComputeNodeBySupernode[to] == defComputeNode &&
                                    from < splitOrdinalBySupernode.size() &&
                                    to < splitOrdinalBySupernode.size() &&
                                    splitOrdinalBySupernode[from] < splitOrdinalBySupernode[to];
                                skipDagEdge = !splitForward;
                            }
                        }
                        addAtomPlainReplayDependency(operand, to, skipDagEdge);
                    }
                    if (isRegToMemIntentSlice(toOp))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, toOp))
                        {
                            addAtomPlainReplayDependency(*indexValue, to);
                        }
                    }
                }
            }
            for (auto &fanout : atomPlainReplayFanout)
            {
                std::sort(fanout.begin(), fanout.end());
                fanout.erase(std::unique(fanout.begin(), fanout.end()), fanout.end());
            }
            for (const auto &targets : atomPlainReplayFanout)
            {
                if (targets.empty())
                {
                    continue;
                }
                stats.cbawAtomPlainReplayCrossBoundaryTargetCount += targets.size();
                for (const uint32_t target : targets)
                {
                    if (target < build.supernodeKinds.size() &&
                        build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++stats.cbawAtomPlainReplayComputeMaterializedValueTargetCount;
                    }
                }
            }
            stats.cbawAtomPlainReplaySupernodeDependencyEdgeCount =
                atomPlainReplayDependencyEdges.size();
            stats.cbawAtomPlainReplayBoundaryDelta =
                stats.cbawAtomPlainReplayCrossBoundaryTargetCount > summaryStats.boundaryActivationEdges
                    ? stats.cbawAtomPlainReplayCrossBoundaryTargetCount - summaryStats.boundaryActivationEdges
                    : summaryStats.boundaryActivationEdges - stats.cbawAtomPlainReplayCrossBoundaryTargetCount;
            stats.cbawAtomPlainReplayDagDelta =
                stats.cbawAtomPlainReplaySupernodeDependencyEdgeCount > summaryStats.dagEdges
                    ? stats.cbawAtomPlainReplaySupernodeDependencyEdgeCount - summaryStats.dagEdges
                    : summaryStats.dagEdges - stats.cbawAtomPlainReplaySupernodeDependencyEdgeCount;
            stats.cbawAtomPlainReplayComputeComputeDelta =
                stats.cbawAtomPlainReplayComputeMaterializedValueTargetCount >
                        summaryStats.computeComputeValuePairs
                    ? stats.cbawAtomPlainReplayComputeMaterializedValueTargetCount -
                          summaryStats.computeComputeValuePairs
                    : summaryStats.computeComputeValuePairs -
                          stats.cbawAtomPlainReplayComputeMaterializedValueTargetCount;

            std::sort(topRoots.begin(),
                      topRoots.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.targetCount != rhs.targetCount)
                          {
                              return lhs.targetCount > rhs.targetCount;
                          }
                          if (lhs.consumerUseCount != rhs.consumerUseCount)
                          {
                              return lhs.consumerUseCount > rhs.consumerUseCount;
                          }
                          if (lhs.valueBytes != rhs.valueBytes)
                          {
                              return lhs.valueBytes > rhs.valueBytes;
                          }
                          return lhs.valueIndex < rhs.valueIndex;
                      });
            if (topRoots.size() > 16)
            {
                topRoots.resize(16);
            }
            const auto noteTopRootAttribution = [&](std::string_view rule)
            {
                stats.semanticTopRootAttribution[std::string(rule)] += 1;
                ++stats.semanticTopRootAttributedCount;
            };
            for (const auto &root : topRoots)
            {
                if (root.valueIndex == 0 || root.valueIndex > graph.values().size())
                {
                    continue;
                }
                wolvrix::lib::grh::ValueId value;
                value.graph = graph.id();
                value.index = static_cast<uint32_t>(root.valueIndex);
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid() || defOp.index == 0 || defOp.index > build.opToSupernode.size())
                {
                    continue;
                }
                const uint32_t owner = build.opToSupernode[defOp.index - 1];
                if (owner == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                if (rtmSupernodes.find(owner) != rtmSupernodes.end())
                {
                    ++stats.semanticTopRootRtmCount;
                    noteTopRootAttribution("rtm_intent");
                }
                if (aggregateSupernodes.find(owner) != aggregateSupernodes.end())
                {
                    ++stats.semanticTopRootAggregateCount;
                    noteTopRootAttribution("aggregate_family");
                }
                if (guardSupernodes.find(owner) != guardSupernodes.end())
                {
                    ++stats.semanticTopRootGuardCount;
                    noteTopRootAttribution("guard_domain");
                }
                if (sinkSupernodes.find(owner) != sinkSupernodes.end())
                {
                    ++stats.semanticTopRootSinkCount;
                    noteTopRootAttribution("sink_cone");
                }
                if (passthroughSupernodes.find(owner) != passthroughSupernodes.end())
                {
                    ++stats.semanticTopRootPassthroughCount;
                    noteTopRootAttribution("passthrough_chain");
                }
            }
            stats.topRoots = std::move(topRoots);
            stats.crossBoundaryConsumerUseCount = stats.canonicalCrossBoundaryConsumerUseCount;

            stats.quotientDagCycleDetected = activityScheduleDagHasCycle(build.dag) ? 1 : 0;
            stats.replayBoundaryActivationDelta =
                stats.crossBoundaryTargetCount > summaryStats.boundaryActivationEdges
                    ? stats.crossBoundaryTargetCount - summaryStats.boundaryActivationEdges
                    : summaryStats.boundaryActivationEdges - stats.crossBoundaryTargetCount;
            stats.replayDagEdgeDelta =
                stats.supernodeDependencyEdgeCount > summaryStats.dagEdges
                    ? stats.supernodeDependencyEdgeCount - summaryStats.dagEdges
                    : summaryStats.dagEdges - stats.supernodeDependencyEdgeCount;
            stats.replayComputeComputeDelta =
                stats.computeMaterializedValueTargetCount > summaryStats.computeComputeValuePairs
                    ? stats.computeMaterializedValueTargetCount - summaryStats.computeComputeValuePairs
                    : summaryStats.computeComputeValuePairs - stats.computeMaterializedValueTargetCount;
            stats.canonicalBoundaryActivationDelta =
                stats.canonicalCrossBoundaryTargetCount > summaryStats.boundaryActivationEdges
                    ? stats.canonicalCrossBoundaryTargetCount - summaryStats.boundaryActivationEdges
                    : summaryStats.boundaryActivationEdges - stats.canonicalCrossBoundaryTargetCount;
            stats.canonicalDagEdgeDelta =
                stats.canonicalSupernodeDependencyEdgeCount > summaryStats.dagEdges
                    ? stats.canonicalSupernodeDependencyEdgeCount - summaryStats.dagEdges
                    : summaryStats.dagEdges - stats.canonicalSupernodeDependencyEdgeCount;
            stats.canonicalComputeComputeDelta =
                stats.canonicalComputeMaterializedValueTargetCount > summaryStats.computeComputeValuePairs
                    ? stats.canonicalComputeMaterializedValueTargetCount - summaryStats.computeComputeValuePairs
                    : summaryStats.computeComputeValuePairs - stats.canonicalComputeMaterializedValueTargetCount;
            return stats;
        }

        struct CbawStructureGateReport
        {
            bool hasPlainBaseline = false;
            bool structuralPass = false;
            bool triggerPass = false;
            bool resourcePass = false;
            bool dagPass = false;
            bool runtimeAllowed = false;
            std::string reason;
            std::size_t plainCrossBoundaryTargets = 0;
            std::size_t plainDagEdges = 0;
            std::size_t plainComputeMaterializedTargets = 0;
            std::size_t cbawCrossBoundaryTargets = 0;
            std::size_t cbawDagEdges = 0;
            std::size_t cbawComputeMaterializedTargets = 0;
            std::size_t cbawTriggerEstimatedP99 = 0;
            std::size_t cbawResourceOpCountExceptions = 0;
        };

        CbawStructureGateReport buildCbawStructureGateReport(
            const ActivityScheduleSummaryStats *plainStats,
            const ActivityScheduleCbawStats &cbawStats)
        {
            CbawStructureGateReport report;
            report.cbawCrossBoundaryTargets = cbawStats.crossBoundaryTargetCount;
            report.cbawDagEdges = cbawStats.supernodeDependencyEdgeCount;
            report.cbawComputeMaterializedTargets =
                cbawStats.computeMaterializedValueTargetCount;
            report.cbawTriggerEstimatedP99 = cbawStats.triggerEstimatedCountP99;
            report.cbawResourceOpCountExceptions =
                cbawStats.resourceOpCountBaselineExceptions;
            report.dagPass = cbawStats.quotientDagCycleDetected == 0;
            report.resourcePass = cbawStats.resourceOpCountBaselineExceptions == 0;

            if (plainStats == nullptr)
            {
                report.reason = "missing_plain_baseline";
                return report;
            }
            report.hasPlainBaseline = true;
            report.plainCrossBoundaryTargets = plainStats->boundaryActivationEdges;
            report.plainDagEdges = plainStats->dagEdges;
            report.plainComputeMaterializedTargets =
                plainStats->computeComputeValuePairs;

            report.structuralPass =
                cbawStats.crossBoundaryTargetCount <= plainStats->boundaryActivationEdges &&
                cbawStats.supernodeDependencyEdgeCount <=
                    plainStats->dagEdges &&
                cbawStats.computeMaterializedValueTargetCount <=
                    plainStats->computeComputeValuePairs;
            report.triggerPass = cbawStats.triggerAteEqualMergeRecommended == 0 ||
                                 cbawStats.triggerAteNoGoReason != "trigger_regression";
            report.runtimeAllowed =
                report.structuralPass && report.triggerPass && report.resourcePass && report.dagPass;
            if (report.runtimeAllowed)
            {
                report.reason = "pass";
            }
            else if (!report.structuralPass)
            {
                report.reason = "structure_regression";
            }
            else if (!report.triggerPass)
            {
                report.reason = "trigger_regression";
            }
            else if (!report.resourcePass)
            {
                report.reason = "resource_exception";
            }
            else
            {
                report.reason = "quotient_cycle";
            }
            return report;
        }

        std::string encodeActivityScheduleCbawStatsJson(const ActivityScheduleCbawStats &stats)
        {
            const auto emitCountMap = [](std::ostringstream &out,
                                         std::string_view key,
                                         const ActivityScheduleCbawStats::KindCountMap &counts)
            {
                out << ",\"" << key << "\":{";
                bool first = true;
                for (const auto &[name, count] : counts)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "\"" << name << "\":" << count;
                }
                out << "}";
            };
            const auto emitTopRoots = [](std::ostringstream &out,
                                         const std::vector<ActivityScheduleCbawStats::TopRoot> &roots)
            {
                out << ",\"top_roots\":[";
                bool first = true;
                for (const auto &root : roots)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "{";
                    out << "\"value_index\":" << root.valueIndex;
                    out << ",\"target_count\":" << root.targetCount;
                    out << ",\"compute_target_count\":" << root.computeTargetCount;
                    out << ",\"commit_target_count\":" << root.commitTargetCount;
                    out << ",\"consumer_use_count\":" << root.consumerUseCount;
                    out << ",\"value_bytes\":" << root.valueBytes;
                    out << ",\"source_kind\":\"" << root.sourceKind << "\"";
                    out << "}";
                }
                out << "]";
            };
            const auto emitTopRootStageReports =
                [](std::ostringstream &out,
                   const std::vector<ActivityScheduleCbawStats::TopRootStageReport> &reports)
            {
                out << ",\"cbaw_top_root_stage_reports\":[";
                bool first = true;
                for (const auto &report : reports)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "{";
                    out << "\"value_index\":" << report.valueIndex;
                    out << ",\"rank_reasons\":\"" << escapeJsonString(report.rankReasons) << "\"";
                    out << ",\"defining_op_kind\":\"" << escapeJsonString(report.definingOpKind) << "\"";
                    out << ",\"source_kind\":\"" << escapeJsonString(report.sourceKind) << "\"";
                    out << ",\"value_width\":" << report.valueWidth;
                    out << ",\"width_bucket\":\"" << escapeJsonString(report.widthBucket) << "\"";
                    out << ",\"fanout_bucket\":\"" << escapeJsonString(report.fanoutBucket) << "\"";
                    out << ",\"shared_source_bucket\":\""
                        << escapeJsonString(report.sharedSourceBucket) << "\"";
                    out << ",\"value_bytes\":" << report.valueBytes;
                    out << ",\"producer_atom_id\":" << report.producerAtomId;
                    out << ",\"producer_compute_node_id\":" << report.producerComputeNodeId;
                    out << ",\"producer_supernode_id\":" << report.producerSupernodeId;
                    out << ",\"semantic_tags\":\"" << escapeJsonString(report.semanticTags) << "\"";
                    out << ",\"after_p5_target_count\":" << report.afterP5TargetCount;
                    out << ",\"after_p5_compute_target_count\":"
                        << report.afterP5ComputeTargetCount;
                    out << ",\"after_p5_commit_target_count\":"
                        << report.afterP5CommitTargetCount;
                    out << ",\"after_p5_source_cluster\":" << report.afterP5SourceCluster;
                    out << ",\"after_p5_source_segment\":" << report.afterP5SourceSegment;
                    out << ",\"after_p5_target_segment_count\":"
                        << report.afterP5TargetSegmentCount;
                    out << ",\"after_dp_target_count\":" << report.afterDpTargetCount;
                    out << ",\"after_dp_compute_target_count\":"
                        << report.afterDpComputeTargetCount;
                    out << ",\"after_dp_commit_target_count\":"
                        << report.afterDpCommitTargetCount;
                    out << ",\"after_dp_source_cluster\":" << report.afterDpSourceCluster;
                    out << ",\"after_dp_source_segment\":" << report.afterDpSourceSegment;
                    out << ",\"after_dp_target_segment_count\":"
                        << report.afterDpTargetSegmentCount;
                    out << ",\"after_fm_target_count\":" << report.afterFmTargetCount;
                    out << ",\"after_fm_compute_target_count\":"
                        << report.afterFmComputeTargetCount;
                    out << ",\"after_fm_commit_target_count\":"
                        << report.afterFmCommitTargetCount;
                    out << ",\"after_fm_source_cluster\":" << report.afterFmSourceCluster;
                    out << ",\"after_fm_source_segment\":" << report.afterFmSourceSegment;
                    out << ",\"after_fm_target_segment_count\":"
                        << report.afterFmTargetSegmentCount;
                    out << ",\"final_target_count\":" << report.finalTargetCount;
                    out << ",\"final_compute_target_count\":"
                        << report.finalComputeTargetCount;
                    out << ",\"final_commit_target_count\":"
                        << report.finalCommitTargetCount;
                    out << ",\"delta_p5_to_dp\":" << report.deltaP5ToDp;
                    out << ",\"delta_dp_to_fm\":" << report.deltaDpToFm;
                    out << ",\"final_replay_delta\":" << report.finalReplayDelta;
                    out << ",\"dp_to_fm_repair\":" << report.dpToFmRepair;
                    out << ",\"compute_node_op_count\":" << report.computeNodeOpCount;
                    out << ",\"source_clone_count\":" << report.sourceCloneCount;
                    out << ",\"compute_like_op_count\":" << report.computeLikeOpCount;
                    out << ",\"compute_user_count\":" << report.computeUserCount;
                    out << ",\"external_target_count\":" << report.externalTargetCount;
                    out << ",\"split_atom_count\":" << report.splitAtomCount;
                    out << ",\"split_segment_count\":" << report.splitSegmentCount;
                    out << ",\"high_fanout_bucket\":"
                        << (report.highFanoutBucket ? "true" : "false");
                    out << ",\"shared_source_bucket_hit\":"
                        << (report.sharedSourceBucketHit ? "true" : "false");
                    out << "}";
                }
                out << "]";
            };
            const auto emitTopRootStageDeltas =
                [](std::ostringstream &out,
                   const std::vector<ActivityScheduleCbawStats::TopRootStageDelta> &deltas)
            {
                out << ",\"cbaw_top_root_stage_deltas\":[";
                bool first = true;
                for (const auto &delta : deltas)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "{";
                    out << "\"transition\":\"" << escapeJsonString(delta.transition) << "\"";
                    out << ",\"boundary_target_delta\":" << delta.boundaryTargetDelta;
                    out << ",\"compute_target_delta\":" << delta.computeTargetDelta;
                    out << "}";
                }
                out << "]";
            };
            std::ostringstream out;
            out << "{";
            out << "\"value_use_groups\":" << stats.valueUseGroups;
            out << ",\"cross_boundary_target_count\":" << stats.crossBoundaryTargetCount;
            out << ",\"supernode_dependency_edge_count\":" << stats.supernodeDependencyEdgeCount;
            out << ",\"compute_materialized_value_target_count\":"
                << stats.computeMaterializedValueTargetCount;
            out << ",\"compute_commit_value_target_count\":" << stats.computeCommitValueTargetCount;
            out << ",\"cross_boundary_value_bytes\":" << stats.crossBoundaryValueBytes;
            out << ",\"cross_boundary_consumer_use_count\":" << stats.crossBoundaryConsumerUseCount;
            out << ",\"source_clone_canonicalized_groups\":" << stats.sourceCloneCanonicalizedGroups;
            out << ",\"clone_width_mismatch_groups\":" << stats.cloneWidthMismatchGroups;
            out << ",\"canonical_value_use_groups\":" << stats.canonicalValueUseGroups;
            out << ",\"canonical_cross_boundary_target_count\":"
                << stats.canonicalCrossBoundaryTargetCount;
            out << ",\"canonical_supernode_dependency_edge_count\":"
                << stats.canonicalSupernodeDependencyEdgeCount;
            out << ",\"canonical_compute_materialized_value_target_count\":"
                << stats.canonicalComputeMaterializedValueTargetCount;
            out << ",\"canonical_compute_commit_value_target_count\":"
                << stats.canonicalComputeCommitValueTargetCount;
            out << ",\"canonical_cross_boundary_consumer_use_count\":"
                << stats.canonicalCrossBoundaryConsumerUseCount;
            out << ",\"canonical_boundary_activation_delta\":"
                << stats.canonicalBoundaryActivationDelta;
            out << ",\"canonical_dag_edge_delta\":" << stats.canonicalDagEdgeDelta;
            out << ",\"canonical_compute_compute_delta\":"
                << stats.canonicalComputeComputeDelta;
            out << ",\"quotient_dag_cycle_detected\":" << stats.quotientDagCycleDetected;
            out << ",\"replay_boundary_activation_delta\":" << stats.replayBoundaryActivationDelta;
            out << ",\"replay_dag_edge_delta\":" << stats.replayDagEdgeDelta;
            out << ",\"replay_compute_compute_delta\":" << stats.replayComputeComputeDelta;
            out << ",\"compute_supernodes\":" << stats.computeSupernodes;
            out << ",\"compute_supernode_op_count_p50\":" << stats.computeSupernodeOpCountP50;
            out << ",\"compute_supernode_op_count_p90\":" << stats.computeSupernodeOpCountP90;
            out << ",\"compute_supernode_op_count_p99\":" << stats.computeSupernodeOpCountP99;
            out << ",\"compute_supernode_op_count_p995\":" << stats.computeSupernodeOpCountP995;
            out << ",\"compute_supernode_op_count_max\":" << stats.computeSupernodeOpCountMax;
            out << ",\"resource_op_count_cap\":" << stats.resourceOpCountCap;
            out << ",\"resource_op_count_baseline_exceptions\":"
                << stats.resourceOpCountBaselineExceptions;
            out << ",\"trigger_signature_bits\":" << stats.triggerSignatureBits;
            out << ",\"trigger_signature_hash_functions\":"
                << stats.triggerSignatureHashFunctions;
            out << ",\"trigger_saturation_threshold_bits\":"
                << stats.triggerSaturationThresholdBits;
            out << ",\"trigger_volatile_source_values\":"
                << stats.triggerVolatileSourceValues;
            out << ",\"trigger_compute_supernodes_with_trigger\":"
                << stats.triggerComputeSupernodesWithTrigger;
            out << ",\"trigger_empty_compute_supernodes\":"
                << stats.triggerEmptyComputeSupernodes;
            out << ",\"trigger_signature_popcount_p50\":"
                << stats.triggerSignaturePopcountP50;
            out << ",\"trigger_signature_popcount_p90\":"
                << stats.triggerSignaturePopcountP90;
            out << ",\"trigger_signature_popcount_p99\":"
                << stats.triggerSignaturePopcountP99;
            out << ",\"trigger_signature_popcount_p995\":"
                << stats.triggerSignaturePopcountP995;
            out << ",\"trigger_signature_popcount_max\":"
                << stats.triggerSignaturePopcountMax;
            out << ",\"trigger_estimated_count_p50\":"
                << stats.triggerEstimatedCountP50;
            out << ",\"trigger_estimated_count_p90\":"
                << stats.triggerEstimatedCountP90;
            out << ",\"trigger_estimated_count_p99\":"
                << stats.triggerEstimatedCountP99;
            out << ",\"trigger_estimated_count_p995\":"
                << stats.triggerEstimatedCountP995;
            out << ",\"trigger_estimated_count_max\":"
                << stats.triggerEstimatedCountMax;
            out << ",\"trigger_signature_saturated_compute_supernodes\":"
                << stats.triggerSignatureSaturatedComputeSupernodes;
            out << ",\"trigger_signature_saturated_ratio_ppm\":"
                << stats.triggerSignatureSaturatedRatioPpm;
            out << ",\"trigger_equal_bucket_count\":"
                << stats.triggerEqualBucketCount;
            out << ",\"trigger_equal_bucket_multi_count\":"
                << stats.triggerEqualBucketMultiCount;
            out << ",\"trigger_equal_bucket_covered_supernodes\":"
                << stats.triggerEqualBucketCoveredSupernodes;
            out << ",\"trigger_equal_bucket_covered_supernode_ratio_ppm\":"
                << stats.triggerEqualBucketCoveredSupernodeRatioPpm;
            out << ",\"trigger_equal_bucket_largest\":"
                << stats.triggerEqualBucketLargest;
            out << ",\"trigger_non_empty_equal_bucket_count\":"
                << stats.triggerNonEmptyEqualBucketCount;
            out << ",\"trigger_non_empty_equal_bucket_multi_count\":"
                << stats.triggerNonEmptyEqualBucketMultiCount;
            out << ",\"trigger_non_empty_equal_bucket_covered_supernodes\":"
                << stats.triggerNonEmptyEqualBucketCoveredSupernodes;
            out << ",\"trigger_non_empty_equal_bucket_covered_supernode_ratio_ppm\":"
                << stats.triggerNonEmptyEqualBucketCoveredSupernodeRatioPpm;
            out << ",\"trigger_non_empty_equal_bucket_largest\":"
                << stats.triggerNonEmptyEqualBucketLargest;
            out << ",\"trigger_equal_bucket_internalizable_boundary_targets\":"
                << stats.triggerEqualBucketInternalizableBoundaryTargets;
            out << ",\"trigger_non_empty_equal_bucket_internalizable_boundary_targets\":"
                << stats.triggerNonEmptyEqualBucketInternalizableBoundaryTargets;
            out << ",\"trigger_equal_bucket_internalizable_compute_targets\":"
                << stats.triggerEqualBucketInternalizableComputeTargets;
            out << ",\"trigger_non_empty_equal_bucket_internalizable_compute_targets\":"
                << stats.triggerNonEmptyEqualBucketInternalizableComputeTargets;
            out << ",\"trigger_equal_bucket_internalizable_dependency_edges\":"
                << stats.triggerEqualBucketInternalizableDependencyEdges;
            out << ",\"trigger_non_empty_equal_bucket_internalizable_dependency_edges\":"
                << stats.triggerNonEmptyEqualBucketInternalizableDependencyEdges;
            out << ",\"trigger_ate_equal_merge_recommended\":"
                << stats.triggerAteEqualMergeRecommended;
            out << ",\"trigger_ate_no_go_reason\":\""
                << escapeJsonString(stats.triggerAteNoGoReason) << "\"";
            out << ",\"semantic_seed_groups\":" << stats.semanticSeedGroups;
            out << ",\"semantic_merge_hint_groups\":" << stats.semanticMergeHintGroups;
            out << ",\"semantic_debug_labels\":" << stats.semanticDebugLabels;
            out << ",\"semantic_rtm_intent_groups\":" << stats.semanticRtmIntentGroups;
            out << ",\"semantic_rtm_intent_ops\":" << stats.semanticRtmIntentOps;
            out << ",\"semantic_mffc_groups\":" << stats.semanticMffcGroups;
            out << ",\"semantic_mffc_covered_ops\":" << stats.semanticMffcCoveredOps;
            out << ",\"semantic_mffc_split_groups\":" << stats.semanticMffcSplitGroups;
            out << ",\"semantic_plain_out1_hints\":" << stats.semanticPlainOut1Hints;
            out << ",\"semantic_plain_in1_hints\":" << stats.semanticPlainIn1Hints;
            out << ",\"semantic_plain_sibling_groups\":" << stats.semanticPlainSiblingGroups;
            out << ",\"semantic_plain_sibling_members\":" << stats.semanticPlainSiblingMembers;
            out << ",\"semantic_aggregate_families\":" << stats.semanticAggregateFamilies;
            out << ",\"semantic_aggregate_seed_groups\":" << stats.semanticAggregateSeedGroups;
            out << ",\"semantic_aggregate_merge_hint_groups\":"
                << stats.semanticAggregateMergeHintGroups;
            out << ",\"semantic_guard_domains\":" << stats.semanticGuardDomains;
            out << ",\"semantic_guard_domain_members\":" << stats.semanticGuardDomainMembers;
            out << ",\"semantic_guard_unknown_ops\":" << stats.semanticGuardUnknownOps;
            out << ",\"semantic_sink_cone_labels\":" << stats.semanticSinkConeLabels;
            out << ",\"semantic_sink_cone_members\":" << stats.semanticSinkConeMembers;
            out << ",\"semantic_sink_cone_multi_sink_ops\":"
                << stats.semanticSinkConeMultiSinkOps;
            out << ",\"semantic_passthrough_chains\":" << stats.semanticPassthroughChains;
            out << ",\"semantic_passthrough_ops\":" << stats.semanticPassthroughOps;
            out << ",\"semantic_hierarchy_debug_labels\":"
                << stats.semanticHierarchyDebugLabels;
            out << ",\"semantic_top_root_attributed_count\":"
                << stats.semanticTopRootAttributedCount;
            out << ",\"semantic_top_root_rtm_count\":" << stats.semanticTopRootRtmCount;
            out << ",\"semantic_top_root_aggregate_count\":"
                << stats.semanticTopRootAggregateCount;
            out << ",\"semantic_top_root_guard_count\":" << stats.semanticTopRootGuardCount;
            out << ",\"semantic_top_root_sink_count\":" << stats.semanticTopRootSinkCount;
            out << ",\"semantic_top_root_passthrough_count\":"
                << stats.semanticTopRootPassthroughCount;
            out << ",\"cbaw_atom_count\":" << stats.cbawAtomCount;
            out << ",\"cbaw_atom_op_count_p50\":" << stats.cbawAtomOpCountP50;
            out << ",\"cbaw_atom_op_count_p90\":" << stats.cbawAtomOpCountP90;
            out << ",\"cbaw_atom_op_count_p99\":" << stats.cbawAtomOpCountP99;
            out << ",\"cbaw_atom_op_count_p995\":" << stats.cbawAtomOpCountP995;
            out << ",\"cbaw_atom_op_count_max\":" << stats.cbawAtomOpCountMax;
            out << ",\"cbaw_atom_quotient_edges\":" << stats.cbawAtomQuotientEdges;
            out << ",\"cbaw_atom_quotient_cycle_detected\":"
                << stats.cbawAtomQuotientCycleDetected;
            out << ",\"cbaw_atom_resource_op_count_cap\":"
                << stats.cbawAtomResourceOpCountCap;
            out << ",\"cbaw_atom_resource_op_count_baseline_exceptions\":"
                << stats.cbawAtomResourceOpCountBaselineExceptions;
            out << ",\"cbaw_atom_rtm_intent_atoms\":" << stats.cbawAtomRtmIntentAtoms;
            out << ",\"cbaw_atom_mffc_atoms\":" << stats.cbawAtomMffcAtoms;
            out << ",\"cbaw_atom_passthrough_atoms\":" << stats.cbawAtomPassthroughAtoms;
            out << ",\"cbaw_atom_aggregate_atoms\":" << stats.cbawAtomAggregateAtoms;
            out << ",\"cbaw_atom_guard_atoms\":" << stats.cbawAtomGuardAtoms;
            out << ",\"cbaw_atom_plain_replay_supernodes\":"
                << stats.cbawAtomPlainReplaySupernodes;
            out << ",\"cbaw_atom_plain_replay_cross_boundary_target_count\":"
                << stats.cbawAtomPlainReplayCrossBoundaryTargetCount;
            out << ",\"cbaw_atom_plain_replay_supernode_dependency_edge_count\":"
                << stats.cbawAtomPlainReplaySupernodeDependencyEdgeCount;
            out << ",\"cbaw_atom_plain_replay_compute_materialized_value_target_count\":"
                << stats.cbawAtomPlainReplayComputeMaterializedValueTargetCount;
            out << ",\"cbaw_atom_plain_replay_boundary_delta\":"
                << stats.cbawAtomPlainReplayBoundaryDelta;
            out << ",\"cbaw_atom_plain_replay_dag_delta\":"
                << stats.cbawAtomPlainReplayDagDelta;
            out << ",\"cbaw_atom_plain_replay_compute_compute_delta\":"
                << stats.cbawAtomPlainReplayComputeComputeDelta;
            out << ",\"cbaw_top_root_report_limit\":"
                << stats.cbawTopRootReportLimit;
            out << ",\"cbaw_top_root_after_dp_total_targets\":"
                << stats.cbawTopRootAfterDpTotalTargets;
            out << ",\"cbaw_top_root_after_dp_reported_targets\":"
                << stats.cbawTopRootAfterDpReportedTargets;
            out << ",\"cbaw_top_root_after_dp_coverage_ppm\":"
                << stats.cbawTopRootAfterDpCoveragePpm;
            emitCountMap(out, "target_kind_matrix", stats.targetKindMatrix);
            emitCountMap(out, "source_kind_matrix", stats.sourceKindMatrix);
            emitCountMap(out, "source_target_kind_matrix", stats.sourceTargetKindMatrix);
            emitCountMap(out, "resource_p50", stats.resourceP50);
            emitCountMap(out, "resource_p90", stats.resourceP90);
            emitCountMap(out, "resource_p99", stats.resourceP99);
            emitCountMap(out, "resource_p995", stats.resourceP995);
            emitCountMap(out, "resource_max", stats.resourceMax);
            emitCountMap(out, "resource_cap", stats.resourceCap);
            emitCountMap(out, "resource_baseline_exceptions", stats.resourceBaselineExceptions);
            emitCountMap(out, "semantic_rule_seed_groups", stats.semanticRuleSeedGroups);
            emitCountMap(out, "semantic_rule_merge_hint_groups", stats.semanticRuleMergeHintGroups);
            emitCountMap(out, "semantic_rule_debug_labels", stats.semanticRuleDebugLabels);
            emitCountMap(out, "semantic_top_root_attribution", stats.semanticTopRootAttribution);
            emitCountMap(out, "cbaw_atom_resource_p50", stats.cbawAtomResourceP50);
            emitCountMap(out, "cbaw_atom_resource_p90", stats.cbawAtomResourceP90);
            emitCountMap(out, "cbaw_atom_resource_p99", stats.cbawAtomResourceP99);
            emitCountMap(out, "cbaw_atom_resource_p995", stats.cbawAtomResourceP995);
            emitCountMap(out, "cbaw_atom_resource_max", stats.cbawAtomResourceMax);
            emitCountMap(out, "cbaw_atom_resource_cap", stats.cbawAtomResourceCap);
            emitCountMap(out,
                         "cbaw_atom_resource_baseline_exceptions",
                         stats.cbawAtomResourceBaselineExceptions);
            emitCountMap(out, "cbaw_atom_kind_counts", stats.cbawAtomKindCounts);
            emitTopRoots(out, stats.topRoots);
            emitTopRootStageReports(out, stats.topRootStageReports);
            emitTopRootStageDeltas(out, stats.topRootStageDeltas);
            out << "}";
            return out.str();
        }

        std::string encodeActivityScheduleSummaryStatsJson(const ActivityScheduleSummaryStats &stats)
        {
            const auto emitCountMap = [](std::ostringstream &out,
                                         std::string_view key,
                                         const ActivityScheduleSummaryStats::KindCountMap &counts)
            {
                out << ",\"" << key << "\":{";
                bool first = true;
                for (const auto &[name, count] : counts)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "\"" << name << "\":" << count;
                }
                out << "}";
            };
            const auto emitDoubleMap = [](std::ostringstream &out,
                                          std::string_view key,
                                          const ActivityScheduleSummaryStats::KindDoubleMap &counts)
            {
                out << ",\"" << key << "\":{";
                bool first = true;
                for (const auto &[name, value] : counts)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "\"" << name << "\":" << value;
                }
                out << "}";
            };
            std::ostringstream out;
            out << std::setprecision(17);
            out << "{";
            out << "\"supernodes\":" << stats.supernodes;
            out << ",\"compute_supernodes\":" << stats.computeSupernodes;
            out << ",\"commit_supernodes\":" << stats.commitSupernodes;
            out << ",\"dag_edges\":" << stats.dagEdges;
            out << ",\"boundary_values\":" << stats.boundaryValues;
            out << ",\"boundary_activation_edges\":" << stats.boundaryActivationEdges;
            out << ",\"compute_compute_value_pairs\":" << stats.computeComputeValuePairs;
            out << ",\"compute_commit_value_pairs\":" << stats.computeCommitValuePairs;
            out << ",\"state_read_activation_edges\":" << stats.stateReadActivationEdges;
            out << ",\"memory_read_activation_edges\":" << stats.memoryReadActivationEdges;
            out << ",\"constant_activation_edges\":" << stats.constantActivationEdges;
            out << ",\"other_compute_activation_edges\":" << stats.otherComputeActivationEdges;
            out << ",\"other_compute_single_target_values\":" << stats.otherComputeSingleTargetValues;
            out << ",\"other_compute_multi_target_values\":" << stats.otherComputeMultiTargetValues;
            out << ",\"other_compute_single_target_activation_edges\":"
                << stats.otherComputeSingleTargetActivationEdges;
            out << ",\"other_compute_multi_target_activation_edges\":"
                << stats.otherComputeMultiTargetActivationEdges;
            out << ",\"other_compute_unique_supernode_pairs\":" << stats.otherComputeUniqueSupernodePairs;
            out << ",\"other_compute_duplicate_activation_edges\":" << stats.otherComputeDuplicateActivationEdges;
            out << ",\"boundary_value_pi_sum\":" << stats.boundaryValuePiSum;
            out << ",\"boundary_edge_pi_sum\":" << stats.boundaryEdgePiSum;
            out << ",\"compute_compute_edge_pi_sum\":" << stats.computeComputeEdgePiSum;
            out << ",\"compute_commit_edge_pi_sum\":" << stats.computeCommitEdgePiSum;
            out << ",\"boundary_value_change_weight_sum\":" << stats.boundaryValueChangeWeightSum;
            out << ",\"boundary_edge_change_weight_sum\":" << stats.boundaryEdgeChangeWeightSum;
            out << ",\"compute_compute_edge_change_weight_sum\":"
                << stats.computeComputeEdgeChangeWeightSum;
            out << ",\"compute_commit_edge_change_weight_sum\":"
                << stats.computeCommitEdgeChangeWeightSum;
            out << ",\"compute_nodes\":" << stats.computeNodes;
            out << ",\"compute_node_ops_total\":" << stats.computeNodeOpsTotal;
            out << ",\"compute_node_cycle_split_iters\":" << stats.computeNodeCycleSplitIters;
            out << ",\"initial_compute_supernodes\":" << stats.initialComputeSupernodes;
            out << ",\"initial_compute_supernode_ops_total\":"
                << stats.initialComputeSupernodeOpsTotal;
            out << ",\"initial_compute_supernode_dag_edges\":"
                << stats.initialComputeSupernodeDagEdges;
            out << ",\"initial_boundary_values\":" << stats.initialBoundaryValues;
            out << ",\"initial_boundary_activation_edges\":"
                << stats.initialBoundaryActivationEdges;
            out << ",\"initial_compute_compute_value_pairs\":"
                << stats.initialComputeComputeValuePairs;
            out << ",\"initial_compute_commit_value_pairs\":"
                << stats.initialComputeCommitValuePairs;
            out << ",\"source_clones_in_compute_nodes\":" << stats.sourceClonesInComputeNodes;
            out << ",\"local_shared_compute_clones_in_compute_nodes\":"
                << stats.localSharedComputeClonesInComputeNodes;
            out << ",\"direct_source_inputs_to_commit_supernodes\":"
                << stats.directSourceInputsToCommitSupernodes;
            out << ",\"common_expr_compute_nodes\":" << stats.commonExprComputeNodes;
            out << ",\"compute_node_boundary_inputs_total\":" << stats.computeNodeBoundaryInputsTotal;
            out << ",\"compute_node_boundary_input_no_def\":" << stats.computeNodeBoundaryInputNoDef;
            out << ",\"compute_node_boundary_input_def_out_of_range\":"
                << stats.computeNodeBoundaryInputDefOutOfRange;
            out << ",\"compute_node_boundary_input_declared\":" << stats.computeNodeBoundaryInputDeclared;
            out << ",\"compute_node_boundary_input_source_spill\":"
                << stats.computeNodeBoundaryInputSourceSpill;
            out << ",\"compute_node_boundary_input_unsupported\":"
                << stats.computeNodeBoundaryInputUnsupported;
            out << ",\"compute_node_boundary_input_existing_owner\":"
                << stats.computeNodeBoundaryInputExistingOwner;
            out << ",\"compute_node_boundary_input_existing_common_owner\":"
                << stats.computeNodeBoundaryInputExistingCommonOwner;
            out << ",\"compute_node_boundary_input_shared\":" << stats.computeNodeBoundaryInputShared;
            out << ",\"compute_node_boundary_input_capacity\":" << stats.computeNodeBoundaryInputCapacity;
            out << ",\"compute_node_boundary_values\":" << stats.computeNodeBoundaryValues;
            out << ",\"commit_input_root_values\":" << stats.commitInputRootValues;
            out << ",\"commit_sink_ops\":" << stats.commitSinkOps;
            out << ",\"commit_event_key_runs\":" << stats.commitEventKeyRuns;
            out << ",\"commit_event_keys\":" << stats.commitEventKeys;
            out << ",\"topo_edges\":" << stats.topoEdges;
            out << ",\"graph_ops\":" << stats.graphOps;
            out << ",\"graph_values\":" << stats.graphValues;
            emitCountMap(out, "activation_edges_by_source_kind", stats.activationEdgesBySourceKind);
            emitCountMap(out, "activation_source_values_by_source_kind", stats.activationSourceValuesBySourceKind);
            emitDoubleMap(out, "activation_edge_pi_by_source_kind", stats.activationEdgePiBySourceKind);
            emitDoubleMap(out,
                          "activation_edge_change_weight_by_source_kind",
                          stats.activationEdgeChangeWeightBySourceKind);
            emitCountMap(out,
                         "compute_node_boundary_existing_common_owner_by_kind",
                         stats.computeNodeBoundaryExistingCommonOwnerByKind);
            emitCountMap(out,
                         "compute_node_boundary_existing_common_owner_by_width_bucket",
                         stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket);
            emitCountMap(out,
                         "compute_node_boundary_existing_common_owner_by_fanout_bucket",
                         stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket);
            out << "}";
            return out.str();
        }

        template <typename RewriteBuildT, typename OpDataT>
        ActivityScheduleSummaryStats buildActivityScheduleSummaryStats(const ActivityScheduleBuild &build,
                                                                       const RewriteBuildT &rewrite,
                                                                       const OpDataT &opData,
                                                                       const wolvrix::lib::grh::Graph &graph,
                                                                       const ActivityScheduleValueWeightStats *valueWeights)
        {
            ActivityScheduleSummaryStats stats;
            std::unordered_set<uint64_t> otherComputeUniquePairs;
            stats.supernodes = build.supernodeToOps.size();
            stats.computeNodes = rewrite.stats.computeNodes;
            stats.computeNodeOpsTotal = rewrite.stats.computeNodeOpsTotal;
            stats.computeNodeCycleSplitIters = rewrite.stats.computeNodeCycleSplitIters;
            stats.initialComputeSupernodes = rewrite.stats.initialComputeSupernodes;
            stats.initialComputeSupernodeOpsTotal =
                rewrite.stats.initialComputeSupernodeOpsTotal;
            stats.initialComputeSupernodeDagEdges =
                rewrite.stats.initialComputeSupernodeDagEdges;
            stats.initialBoundaryValues = rewrite.stats.initialBoundaryValues;
            stats.initialBoundaryActivationEdges =
                rewrite.stats.initialBoundaryActivationEdges;
            stats.initialComputeComputeValuePairs =
                rewrite.stats.initialComputeComputeValuePairs;
            stats.initialComputeCommitValuePairs =
                rewrite.stats.initialComputeCommitValuePairs;
            stats.sourceClonesInComputeNodes = rewrite.stats.sourceClonesInComputeNodes;
            stats.localSharedComputeClonesInComputeNodes =
                rewrite.stats.localSharedComputeClonesInComputeNodes;
            stats.directSourceInputsToCommitSupernodes = rewrite.stats.directSourceInputsToCommitSupernodes;
            stats.commonExprComputeNodes = rewrite.stats.commonExprComputeNodes;
            stats.computeNodeBoundaryInputsTotal = rewrite.stats.computeNodeBoundaryInputsTotal;
            stats.computeNodeBoundaryInputNoDef = rewrite.stats.computeNodeBoundaryInputNoDef;
            stats.computeNodeBoundaryInputDefOutOfRange = rewrite.stats.computeNodeBoundaryInputDefOutOfRange;
            stats.computeNodeBoundaryInputDeclared = rewrite.stats.computeNodeBoundaryInputDeclared;
            stats.computeNodeBoundaryInputSourceSpill = rewrite.stats.computeNodeBoundaryInputSourceSpill;
            stats.computeNodeBoundaryInputUnsupported = rewrite.stats.computeNodeBoundaryInputUnsupported;
            stats.computeNodeBoundaryInputExistingOwner = rewrite.stats.computeNodeBoundaryInputExistingOwner;
            stats.computeNodeBoundaryInputExistingCommonOwner = rewrite.stats.computeNodeBoundaryInputExistingCommonOwner;
            stats.computeNodeBoundaryInputShared = rewrite.stats.computeNodeBoundaryInputShared;
            stats.computeNodeBoundaryInputCapacity = rewrite.stats.computeNodeBoundaryInputCapacity;
            stats.computeNodeBoundaryValues = rewrite.stats.computeNodeBoundaryValues;
            stats.commitInputRootValues = rewrite.stats.commitInputRootValues;
            stats.commitSinkOps = rewrite.stats.commitSinkOps;
            stats.commitEventKeyRuns = rewrite.stats.commitEventKeyRuns;
            stats.commitEventKeys = rewrite.stats.commitEventKeys;
            stats.computeNodeBoundaryExistingCommonOwnerByKind =
                rewrite.stats.computeNodeBoundaryExistingCommonOwnerByKind;
            stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket =
                rewrite.stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket;
            stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket =
                rewrite.stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket;
            stats.topoEdges = opData.topoEdges.size();
            stats.graphOps = graph.operations().size();
            stats.graphValues = graph.values().size();
            for (const auto kind : build.supernodeKinds)
            {
                if (kind == ActivityScheduleSupernodeKind::Compute)
                {
                    ++stats.computeSupernodes;
                }
                else if (kind == ActivityScheduleSupernodeKind::Commit)
                {
                    ++stats.commitSupernodes;
                }
            }
            for (const auto &succs : build.dag)
            {
                stats.dagEdges += succs.size();
            }
            for (std::size_t valueIndex = 0; valueIndex < build.valueFanout.size(); ++valueIndex)
            {
                const auto &fanout = build.valueFanout[valueIndex];
                if (fanout.empty())
                {
                    continue;
                }
                ++stats.boundaryValues;
                stats.boundaryActivationEdges += fanout.size();
                const std::size_t sourceValueIndex = valueIndex + 1;
                const double valuePi =
                    activityScheduleValueWeightAt(valueWeights == nullptr ? nullptr : &valueWeights->piByValueIndex,
                                                  sourceValueIndex);
                const double valueChangeWeight =
                    activityScheduleValueWeightAt(valueWeights == nullptr
                                                      ? nullptr
                                                      : &valueWeights->changeWeightByValueIndex,
                                                  sourceValueIndex);
                stats.boundaryValuePiSum += valuePi;
                stats.boundaryValueChangeWeightSum += valueChangeWeight;
                stats.boundaryEdgePiSum += valuePi * static_cast<double>(fanout.size());
                stats.boundaryEdgeChangeWeightSum += valueChangeWeight * static_cast<double>(fanout.size());
                const std::string sourceKindName =
                    valueIndex + 1 < build.valueSourceKind.size()
                        ? std::string(wolvrix::lib::grh::toString(build.valueSourceKind[valueIndex + 1]))
                        : std::string("unknown");
                stats.activationEdgesBySourceKind[sourceKindName] += fanout.size();
                stats.activationSourceValuesBySourceKind[sourceKindName] += 1;
                if (valueIndex + 1 < build.valueSourceKind.size())
                {
                    switch (build.valueSourceKind[valueIndex + 1])
                    {
                    case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                    case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                        stats.stateReadActivationEdges += fanout.size();
                        break;
                    case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                        stats.memoryReadActivationEdges += fanout.size();
                        break;
                    case wolvrix::lib::grh::OperationKind::kConstant:
                        stats.constantActivationEdges += fanout.size();
                        break;
                    default:
                        stats.otherComputeActivationEdges += fanout.size();
                        if (fanout.size() <= 1)
                        {
                            ++stats.otherComputeSingleTargetValues;
                            stats.otherComputeSingleTargetActivationEdges += fanout.size();
                        }
                        else
                        {
                            ++stats.otherComputeMultiTargetValues;
                            stats.otherComputeMultiTargetActivationEdges += fanout.size();
                        }
                        break;
                    }
                }
                else
                {
                    stats.otherComputeActivationEdges += fanout.size();
                    if (fanout.size() <= 1)
                    {
                        ++stats.otherComputeSingleTargetValues;
                        stats.otherComputeSingleTargetActivationEdges += fanout.size();
                    }
                    else
                    {
                        ++stats.otherComputeMultiTargetValues;
                        stats.otherComputeMultiTargetActivationEdges += fanout.size();
                    }
                }
                const bool isOtherCompute =
                    valueIndex + 1 >= build.valueSourceKind.size() ||
                    (build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kRegisterReadPort &&
                     build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kLatchReadPort &&
                     build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kMemoryReadPort &&
                     build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kConstant);
                const uint32_t sourceSupernode =
                    valueIndex + 1 < build.valueSourceSupernode.size() ? build.valueSourceSupernode[valueIndex + 1]
                                                                       : kInvalidActivitySupernodeId;
                for (const auto targetSupernode : fanout)
                {
                    if (targetSupernode >= build.supernodeKinds.size())
                    {
                        continue;
                    }
                    if (isOtherCompute && sourceSupernode != kInvalidActivitySupernodeId)
                    {
                        const uint64_t packed =
                            (static_cast<uint64_t>(sourceSupernode) << 32) | targetSupernode;
                        otherComputeUniquePairs.insert(packed);
                    }
                    stats.activationEdgePiBySourceKind[sourceKindName] += valuePi;
                    stats.activationEdgeChangeWeightBySourceKind[sourceKindName] += valueChangeWeight;
                    if (build.supernodeKinds[targetSupernode] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++stats.computeComputeValuePairs;
                        stats.computeComputeEdgePiSum += valuePi;
                        stats.computeComputeEdgeChangeWeightSum += valueChangeWeight;
                    }
                    else if (build.supernodeKinds[targetSupernode] == ActivityScheduleSupernodeKind::Commit)
                    {
                        ++stats.computeCommitValuePairs;
                        stats.computeCommitEdgePiSum += valuePi;
                        stats.computeCommitEdgeChangeWeightSum += valueChangeWeight;
                    }
                }
            }
            stats.otherComputeUniqueSupernodePairs = otherComputeUniquePairs.size();
            if (stats.otherComputeActivationEdges >= stats.otherComputeUniqueSupernodePairs)
            {
                stats.otherComputeDuplicateActivationEdges =
                    stats.otherComputeActivationEdges - stats.otherComputeUniqueSupernodePairs;
            }
            return stats;
        }
        struct ActivityOpData
        {
            std::vector<wolvrix::lib::grh::OperationId> topoOps;
            std::vector<wolvrix::lib::grh::SymbolId> topoSymbols;
            std::vector<wolvrix::lib::grh::OperationKind> topoKinds;
            std::vector<uint8_t> topoSinkOnly;
            std::vector<std::pair<uint32_t, uint32_t>> topoEdges;
            std::vector<uint32_t> topoPosByOpIndex;
            std::size_t maxOpIndex = 0;
        };

        struct WorkingPartition
        {
            std::vector<std::vector<uint32_t>> clusters;
            std::vector<uint8_t> fixedBoundary;
        };

        struct ClusterView
        {
            std::vector<std::vector<uint32_t>> members;
            std::vector<uint8_t> fixedBoundary;
            std::vector<uint8_t> sinkOnly;
            std::vector<std::vector<uint32_t>> preds;
            std::vector<std::vector<uint32_t>> succs;
            std::vector<uint32_t> clusterOfTopoPos;
        };

        struct SymbolPartition
        {
            std::vector<std::vector<wolvrix::lib::grh::SymbolId>> clusters;
            std::vector<uint8_t> fixedBoundary;
        };

        struct LiveCluster
        {
            uint32_t minTopoPos = kInvalidActivitySupernodeId;
            uint32_t maxTopoPos = 0;
            bool sinkOnly = true;
            std::vector<uint32_t> topoPositions;
        };

        struct FinalMaterializePerfStats
        {
            std::uint64_t rebuildOpDataMs = 0;
            std::uint64_t mapLiveOpsMs = 0;
            std::uint64_t collectLiveClustersMs = 0;
            std::uint64_t buildSupernodeMapsMs = 0;
            std::uint64_t buildDagMs = 0;
            std::uint64_t buildValueFanoutMs = 0;
            std::uint64_t buildStateReadSetsMs = 0;
            std::uint64_t finalTopoMs = 0;
        };

        struct ComputeNodeMaterializePerfStats
        {
            using KindCountMap = std::map<std::string, std::size_t>;

            struct CbawStageStats
            {
                std::size_t boundaryActivationEdges = 0;
                std::size_t dagEdges = 0;
                std::size_t computeComputeValuePairs = 0;
                std::size_t clusterCount = 0;
                std::size_t segmentCount = 0;
                std::size_t computeSupernodeCount = 0;
                std::size_t opCountP50 = 0;
                std::size_t opCountP90 = 0;
                std::size_t opCountP99 = 0;
                std::size_t opCountMax = 0;
            };

            struct CbawRootStageEntry
            {
                std::size_t valueIndex = 0;
                std::size_t targetCount = 0;
                std::size_t computeTargetCount = 0;
                std::size_t commitTargetCount = 0;
                uint32_t sourceCluster = kInvalidActivitySupernodeId;
                uint32_t sourceSegment = kInvalidActivitySupernodeId;
                std::size_t targetSegmentCount = 0;
            };

            struct CoarsenIteration
            {
                std::size_t iteration = 0;
                std::size_t clusters = 0;
                std::size_t clusterDelta = 0;
                bool changed = false;
                bool out1Changed = false;
                bool in1Changed = false;
                bool siblingsChanged = false;
                bool probChanged = false;
                bool tailStopped = false;
                std::uint64_t elapsedMs = 0;
            };

            std::uint64_t initClustersMs = 0;
            std::uint64_t topoBeforeCoarsenMs = 0;
            std::uint64_t coarsenMs = 0;
            std::uint64_t topoAfterCoarsenMs = 0;
            std::uint64_t buildClusterViewMs = 0;
            std::uint64_t dpSegmentMs = 0;
            std::uint64_t fmRefineMs = 0;
            std::uint64_t flattenSegmentsMs = 0;
            std::uint64_t buildFinalSupernodesMs = 0;
            std::uint64_t buildFinalDagMs = 0;
            std::uint64_t buildStateReadSetsMs = 0;
            std::uint64_t finalTopoMs = 0;
            std::size_t clustersBeforeCoarsen = 0;
            std::size_t clustersAfterCoarsen = 0;
            std::size_t coarsenIterations = 0;
            std::size_t coarsenOut1Merges = 0;
            std::size_t coarsenIn1Merges = 0;
            std::size_t coarsenSiblingMerges = 0;
            std::size_t probCoarsenCandidates = 0;
            std::size_t probCoarsenMerges = 0;
            std::size_t probCoarsenRejectedFootprint = 0;
            std::size_t probCoarsenRejectedPhi = 0;
            std::size_t probCoarsenRejectedWeight = 0;
            std::size_t probCoarsenRejectedSize = 0;
            std::size_t probCoarsenRejectedCycle = 0;
            std::size_t probCoarsenSeedAggregates = 0;
            std::size_t probCoarsenFullAggregates = 0;
            std::uint64_t probCoarsenAggregateMs = 0;
            double probCoarsenTotalGain = 0.0;
            std::size_t cbawCoarsenCandidates = 0;
            std::size_t cbawCoarsenEvaluated = 0;
            std::size_t cbawCoarsenMerges = 0;
            std::size_t cbawCoarsenRejectedNoGain = 0;
            std::size_t cbawCoarsenRejectedCycle = 0;
            std::size_t cbawCoarsenRejectedResource = 0;
            std::size_t cbawCoarsenStale = 0;
            std::uint64_t cbawCoarsenEvaluateMs = 0;
            std::uint64_t cbawCoarsenTopoMs = 0;
            KindCountMap cbawCoarsenGeneratedByKind;
            KindCountMap cbawCoarsenDedupSelectedByKind;
            KindCountMap cbawCoarsenDedupLostTagByKind;
            KindCountMap cbawCoarsenSelectedReason;
            KindCountMap cbawCoarsenEvaluatedByKind;
            KindCountMap cbawCoarsenAcceptedByKind;
            KindCountMap cbawCoarsenAcceptedByTag;
            KindCountMap cbawCoarsenRejectedNoGainByKind;
            KindCountMap cbawCoarsenRejectedNoGainByTag;
            KindCountMap cbawCoarsenRejectedCycleByKind;
            KindCountMap cbawCoarsenRejectedCycleByTag;
            KindCountMap cbawCoarsenRejectedResourceByKind;
            KindCountMap cbawCoarsenRejectedResourceByTag;
            KindCountMap cbawCoarsenStaleByKind;
            KindCountMap cbawCoarsenStaleByTag;
            CbawStageStats cbawAfterCoarsen;
            CbawStageStats cbawAfterDp;
            CbawStageStats cbawAfterFm;
            std::vector<CbawRootStageEntry> cbawAfterCoarsenRootStages;
            std::vector<CbawRootStageEntry> cbawAfterDpRootStages;
            std::vector<CbawRootStageEntry> cbawAfterFmRootStages;
            std::size_t fmRefineRounds = 0;
            std::size_t fmRefineCandidates = 0;
            std::size_t fmRefineMoves = 0;
            std::size_t fmRefineRejectedFootprint = 0;
            std::size_t fmRefineRejectedPhi = 0;
            std::size_t fmRefineRejectedWeight = 0;
            std::size_t fmRefineRejectedSize = 0;
            std::size_t fmRefineRejectedCycle = 0;
            KindCountMap fmRefineRejectedSizeFillBucket;
            KindCountMap fmRefineRejectedCycleRelation;
            double fmRefineTotalGain = 0.0;
            std::size_t segments = 0;
            std::size_t computeSupernodes = 0;
            std::size_t splitOversizeComputeNodes = 0;
            std::size_t splitOversizeComputeNodeSupernodes = 0;
            bool coarsenTailStopped = false;
            std::size_t coarsenTailIterations = 0;
            std::vector<CoarsenIteration> coarsenIterationStats;
        };

        struct StateReadTailAbsorbStats
        {
            std::size_t clonedOps = 0;
            std::size_t erasedOps = 0;
            std::size_t keptObservableReads = 0;
            std::size_t keptLocalReads = 0;
            std::size_t skippedTooManyTargets = 0;
        };

        ClusterView buildClusterView(const WorkingPartition &partition, const ActivityOpData &opData);
        std::vector<uint32_t> computeMffcRep(const ActivityOpData &data);

        std::vector<uint32_t> buildTopoOrderedClusterIds(const ClusterView &view)
        {
            if (view.members.empty())
            {
                return {};
            }

            wolvrix::lib::toposort::TopoDag<uint32_t> topoDag;
            topoDag.reserveNodes(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                topoDag.addNode(clusterId);
            }
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                for (const auto succ : view.succs[clusterId])
                {
                    topoDag.addEdge(clusterId, succ);
                }
            }

            std::vector<uint32_t> ordered;
            try
            {
                const auto layers = topoDag.toposort();
                ordered.reserve(view.members.size());
                for (const auto &layer : layers)
                {
                    std::vector<uint32_t> orderedLayer(layer.begin(), layer.end());
                    std::sort(orderedLayer.begin(),
                              orderedLayer.end(),
                              [&](uint32_t lhs, uint32_t rhs)
                              {
                                  const uint32_t lhsMin = view.members[lhs].empty() ? kInvalidActivitySupernodeId
                                                                                    : view.members[lhs].front();
                                  const uint32_t rhsMin = view.members[rhs].empty() ? kInvalidActivitySupernodeId
                                                                                    : view.members[rhs].front();
                                  if (lhsMin != rhsMin)
                                  {
                                      return lhsMin < rhsMin;
                                  }
                                  return lhs < rhs;
                              });
                    ordered.insert(ordered.end(), orderedLayer.begin(), orderedLayer.end());
                }
            }
            catch (const std::exception &)
            {
                return {};
            }
            if (ordered.size() != view.members.size())
            {
                return {};
            }
            return ordered;
        }

        ClusterView buildTopoOrderedClusterView(const ClusterView &view)
        {
            if (view.members.empty())
            {
                return view;
            }

            const std::vector<uint32_t> orderedClusterIds = buildTopoOrderedClusterIds(view);
            if (orderedClusterIds.size() != view.members.size())
            {
                return view;
            }

            ClusterView out;
            out.members.reserve(view.members.size());
            out.fixedBoundary.reserve(view.fixedBoundary.size());
            out.sinkOnly.reserve(view.sinkOnly.size());
            out.preds.resize(view.preds.size());
            out.succs.resize(view.succs.size());
            out.clusterOfTopoPos.assign(view.clusterOfTopoPos.size(), kInvalidActivitySupernodeId);

            std::vector<uint32_t> newIdByOldId(view.members.size(), kInvalidActivitySupernodeId);
            for (uint32_t newId = 0; newId < orderedClusterIds.size(); ++newId)
            {
                const uint32_t oldId = orderedClusterIds[newId];
                newIdByOldId[oldId] = newId;
                out.members.push_back(view.members[oldId]);
                out.fixedBoundary.push_back(view.fixedBoundary[oldId]);
                out.sinkOnly.push_back(oldId < view.sinkOnly.size() ? view.sinkOnly[oldId] : 0U);
                for (const auto topoPos : out.members.back())
                {
                    if (topoPos < out.clusterOfTopoPos.size())
                    {
                        out.clusterOfTopoPos[topoPos] = newId;
                    }
                }
            }

            for (uint32_t newId = 0; newId < orderedClusterIds.size(); ++newId)
            {
                const uint32_t oldId = orderedClusterIds[newId];
                for (const auto oldPred : view.preds[oldId])
                {
                    if (oldPred < newIdByOldId.size() && newIdByOldId[oldPred] != kInvalidActivitySupernodeId)
                    {
                        out.preds[newId].push_back(newIdByOldId[oldPred]);
                    }
                }
                for (const auto oldSucc : view.succs[oldId])
                {
                    if (oldSucc < newIdByOldId.size() && newIdByOldId[oldSucc] != kInvalidActivitySupernodeId)
                    {
                        out.succs[newId].push_back(newIdByOldId[oldSucc]);
                    }
                }
                std::sort(out.preds[newId].begin(), out.preds[newId].end());
                out.preds[newId].erase(std::unique(out.preds[newId].begin(), out.preds[newId].end()),
                                       out.preds[newId].end());
                std::sort(out.succs[newId].begin(), out.succs[newId].end());
                out.succs[newId].erase(std::unique(out.succs[newId].begin(), out.succs[newId].end()),
                                       out.succs[newId].end());
            }

            return out;
        }

        WorkingPartition canonicalizePartition(const WorkingPartition &partition,
                                               const ActivityOpData &opData)
        {
            if (partition.clusters.empty())
            {
                return partition;
            }

            const ClusterView orderedView = buildTopoOrderedClusterView(buildClusterView(partition, opData));
            WorkingPartition out;
            out.clusters = orderedView.members;
            out.fixedBoundary = orderedView.fixedBoundary;
            if (out.clusters.size() != partition.clusters.size())
            {
                return partition;
            }
            return out;
        }

        std::vector<uint32_t> findCyclePath(const std::vector<std::vector<uint32_t>> &dag)
        {
            std::vector<uint8_t> color(dag.size(), 0U);
            std::vector<uint32_t> stack;
            std::vector<uint32_t> stackIndex(dag.size(), kInvalidActivitySupernodeId);
            std::vector<uint32_t> cycle;

            const auto dfs = [&](auto &&self, uint32_t node) -> bool
            {
                color[node] = 1U;
                stackIndex[node] = static_cast<uint32_t>(stack.size());
                stack.push_back(node);
                for (const auto succ : dag[node])
                {
                    if (succ >= dag.size())
                    {
                        continue;
                    }
                    if (color[succ] == 0U)
                    {
                        if (self(self, succ))
                        {
                            return true;
                        }
                        continue;
                    }
                    if (color[succ] == 1U)
                    {
                        const uint32_t begin = stackIndex[succ];
                        cycle.assign(stack.begin() + begin, stack.end());
                        cycle.push_back(succ);
                        return true;
                    }
                }
                stackIndex[node] = kInvalidActivitySupernodeId;
                stack.pop_back();
                color[node] = 2U;
                return false;
            };

            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                if (color[node] == 0U && dfs(dfs, node))
                {
                    break;
                }
            }
            return cycle;
        }

        std::string describeCyclePath(const std::vector<uint32_t> &cycle,
                                      const std::vector<LiveCluster> &liveClusters)
        {
            if (cycle.empty())
            {
                return "cycle=<unavailable>";
            }

            std::ostringstream oss;
            oss << "cycle=";
            const std::size_t limit = std::min<std::size_t>(cycle.size(), 8);
            for (std::size_t i = 0; i < limit; ++i)
            {
                const uint32_t node = cycle[i];
                if (i != 0)
                {
                    oss << " -> ";
                }
                oss << node;
                if (node < liveClusters.size())
                {
                    const auto &cluster = liveClusters[node];
                    oss << "(min=" << cluster.minTopoPos
                        << ",max=" << cluster.maxTopoPos
                        << ",ops=" << cluster.topoPositions.size()
                        << ",sinkOnly=" << (cluster.sinkOnly ? "1" : "0") << ")";
                }
            }
            if (cycle.size() > limit)
            {
                oss << " -> ...";
            }
            return oss.str();
        }

        std::string describeWorkingPartitionCycle(const WorkingPartition &partition,
                                                  const ActivityOpData &opData)
        {
            if (partition.clusters.empty())
            {
                return {};
            }

            const ClusterView view = buildClusterView(partition, opData);
            const std::vector<uint32_t> cycle = findCyclePath(view.succs);
            if (cycle.empty())
            {
                return {};
            }

            std::ostringstream oss;
            oss << "cycle=";
            const std::size_t limit = std::min<std::size_t>(cycle.size(), 8);
            for (std::size_t i = 0; i < limit; ++i)
            {
                const uint32_t clusterId = cycle[i];
                if (i != 0)
                {
                    oss << " -> ";
                }
                oss << clusterId;
                if (clusterId < view.members.size() && !view.members[clusterId].empty())
                {
                    bool sinkOnly = true;
                    for (const auto topoPos : view.members[clusterId])
                    {
                        if (topoPos >= opData.topoSinkOnly.size() ||
                            opData.topoSinkOnly[topoPos] == 0)
                        {
                            sinkOnly = false;
                            break;
                        }
                    }
                    const uint32_t headTopo = view.members[clusterId].front();
                    const uint32_t tailTopo = view.members[clusterId].back();
                    oss << "(min=" << view.members[clusterId].front()
                        << ",max=" << view.members[clusterId].back()
                        << ",ops=" << view.members[clusterId].size()
                        << ",fixed=" << static_cast<uint32_t>(view.fixedBoundary[clusterId])
                        << ",sinkOnly=" << (sinkOnly ? "1" : "0")
                        << ",headKind=" << wolvrix::lib::grh::toString(opData.topoKinds[headTopo])
                        << ",tailKind=" << wolvrix::lib::grh::toString(opData.topoKinds[tailTopo]) << ")";
                }
            }
            if (cycle.size() > limit)
            {
                oss << " -> ...";
            }
            return oss.str();
        }

        constexpr std::size_t kCoarsenTailLargeClusterThreshold = 100000;
        constexpr std::size_t kCoarsenTailMaxClusterDeltaExclusive = 10;
        constexpr std::size_t kCoarsenTailMaxConsecutiveIters = 3;
        constexpr std::size_t kComputeNodeCoarsenTailLargeClusterThreshold = 100000;
        constexpr std::size_t kComputeNodeCoarsenTailMaxClusterDeltaExclusive = 1024;
        constexpr std::size_t kComputeNodeCoarsenTailMaxConsecutiveIters = 3;

        std::uint64_t elapsedMs(const std::chrono::steady_clock::time_point &start) noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }

        struct DisjointSet
        {
            explicit DisjointSet(std::size_t count)
                : parent(count), size(count, 1)
            {
                std::iota(parent.begin(), parent.end(), 0);
            }

            uint32_t find(uint32_t node)
            {
                uint32_t root = node;
                while (parent[root] != root)
                {
                    root = parent[root];
                }
                while (parent[node] != node)
                {
                    const uint32_t next = parent[node];
                    parent[node] = root;
                    node = next;
                }
                return root;
            }

            bool unite(uint32_t lhs, uint32_t rhs)
            {
                lhs = find(lhs);
                rhs = find(rhs);
                if (lhs == rhs)
                {
                    return false;
                }
                if (size[lhs] < size[rhs])
                {
                    std::swap(lhs, rhs);
                }
                parent[rhs] = lhs;
                size[lhs] += size[rhs];
                return true;
            }

            std::vector<uint32_t> parent;
            std::vector<uint32_t> size;
        };

        ActivityOpData buildActivityOpData(const wolvrix::lib::grh::Graph &graph,
                                           std::string &error)
        {
            ActivityOpData data;

            for (const auto opId : graph.operations())
            {
                data.maxOpIndex = std::max<std::size_t>(data.maxOpIndex, opId.index);
            }

            std::vector<wolvrix::lib::grh::OperationId> eligibleOps;
            eligibleOps.reserve(graph.operations().size());
            for (const auto opId : graph.operations())
            {
                if (isPartitionableOpKind(graph.opKind(opId)))
                {
                    eligibleOps.push_back(opId);
                }
            }

            data.topoPosByOpIndex.assign(data.maxOpIndex + 1, kInvalidActivitySupernodeId);
            if (eligibleOps.empty())
            {
                return data;
            }

            std::vector<uint8_t> eligibleByOpIndex(data.maxOpIndex + 1, 0);
            for (const auto opId : eligibleOps)
            {
                eligibleByOpIndex[opId.index] = 1;
            }

            wolvrix::lib::toposort::TopoDag<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> topoDag;
            topoDag.reserveNodes(eligibleOps.size());
            for (const auto opId : eligibleOps)
            {
                topoDag.addNode(opId);
            }

            std::vector<std::pair<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationId>> opEdges;
            opEdges.reserve(eligibleOps.size() * 2);
            for (const auto opId : eligibleOps)
            {
                for (const auto operand : graph.opOperands(opId))
                {
                    const auto defOp = graph.valueDef(operand);
                    if (!defOp.valid())
                    {
                        continue;
                    }
                    if (defOp.index >= eligibleByOpIndex.size() || eligibleByOpIndex[defOp.index] == 0)
                    {
                        continue;
                    }
                    topoDag.addEdge(defOp, opId);
                    opEdges.emplace_back(defOp, opId);
                }
            }

            try
            {
                const auto layers = topoDag.toposort();
                for (const auto &layer : layers)
                {
                    data.topoOps.insert(data.topoOps.end(), layer.begin(), layer.end());
                }
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule topo failed: ") + ex.what();
                return data;
            }

            if (data.topoOps.size() != eligibleOps.size())
            {
                error = "activity-schedule topo failed: combinational dependency cycle detected";
                data.topoOps.clear();
                return data;
            }

            data.topoSymbols.reserve(data.topoOps.size());
            data.topoKinds.reserve(data.topoOps.size());
            data.topoSinkOnly.reserve(data.topoOps.size());
            for (std::size_t pos = 0; pos < data.topoOps.size(); ++pos)
            {
                const auto opId = data.topoOps[pos];
                data.topoPosByOpIndex[opId.index] = static_cast<uint32_t>(pos);
                data.topoSymbols.push_back(graph.operationSymbol(opId));
                data.topoKinds.push_back(graph.opKind(opId));
                data.topoSinkOnly.push_back(isSinkPartitionOp(graph.getOperation(opId)) ? 1U : 0U);
            }

            data.topoEdges.reserve(opEdges.size());
            for (const auto &[srcOp, dstOp] : opEdges)
            {
                const uint32_t srcPos = data.topoPosByOpIndex[srcOp.index];
                const uint32_t dstPos = data.topoPosByOpIndex[dstOp.index];
                if (srcPos == kInvalidActivitySupernodeId || dstPos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                data.topoEdges.emplace_back(srcPos, dstPos);
            }
            std::sort(data.topoEdges.begin(), data.topoEdges.end());
            data.topoEdges.erase(std::unique(data.topoEdges.begin(), data.topoEdges.end()),
                                 data.topoEdges.end());

            return data;
        }

        WorkingPartition makeSeedPartition(const ActivityOpData &opData,
                                           const std::vector<uint8_t> *includeTopoPos = nullptr)
        {
            WorkingPartition partition;
            partition.clusters.reserve(opData.topoOps.size());
            partition.fixedBoundary.reserve(opData.topoOps.size());
            for (std::size_t pos = 0; pos < opData.topoOps.size(); ++pos)
            {
                if (includeTopoPos != nullptr &&
                    (pos >= includeTopoPos->size() || (*includeTopoPos)[pos] == 0))
                {
                    continue;
                }
                partition.clusters.push_back(std::vector<uint32_t>{static_cast<uint32_t>(pos)});
                partition.fixedBoundary.push_back(isSideEffectBoundaryKind(opData.topoKinds[pos]) ? 1U : 0U);
            }
            return partition;
        }

        std::vector<uint32_t> collectSinkTopoPositions(const wolvrix::lib::grh::Graph &graph,
                                                       const ActivityOpData &opData)
        {
            std::vector<uint32_t> sinks;
            sinks.reserve(opData.topoOps.size());
            for (std::size_t topoPos = 0; topoPos < opData.topoOps.size(); ++topoPos)
            {
                if (isSinkPartitionOp(graph.getOperation(opData.topoOps[topoPos])))
                {
                    sinks.push_back(static_cast<uint32_t>(topoPos));
                }
            }
            return sinks;
        }

        std::string normalizedSinkEventKey(const wolvrix::lib::grh::Graph &graph,
                                           const wolvrix::lib::grh::Operation &op,
                                           const ValueCanonicalMap *canonicalValues)
        {
            const auto edges =
                getAttrValue<std::vector<std::string>>(op, "eventEdge").value_or(std::vector<std::string>{});
            if (edges.empty())
            {
                return "none";
            }

            const auto operands = op.operands();
            std::size_t eventStart = operands.size();
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                eventStart = 3;
                break;
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                eventStart = 4;
                break;
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                eventStart = operands.size() >= edges.size() ? operands.size() - edges.size() : operands.size();
                break;
            default:
                return "opaque";
            }

            const std::size_t safeStart = std::min(eventStart, operands.size());
            const std::size_t eventCount = std::min(edges.size(), operands.size() - safeStart);
            if (eventCount == 0)
            {
                return "none";
            }

            std::vector<std::string> parts;
            parts.reserve(eventCount);
            for (std::size_t i = 0; i < eventCount; ++i)
            {
                std::string edge = edges[i];
                if (edge.empty())
                {
                    edge = "any";
                }
                const auto canonicalValue = canonicalActivityValue(operands[safeStart + i], canonicalValues);
                parts.push_back(edge + ":" + std::to_string(canonicalValue.index));
            }
            std::sort(parts.begin(), parts.end());
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());

            std::ostringstream key;
            key << "ev";
            for (const auto &part : parts)
            {
                key << "|" << part;
            }
            return key.str();
        }

        std::optional<wolvrix::lib::grh::ValueId> sinkUpdateCondValue(const wolvrix::lib::grh::Operation &op)
        {
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                break;
            default:
                return std::nullopt;
            }

            const auto operands = op.operands();
            if (operands.empty())
            {
                return std::nullopt;
            }
            return operands[0];
        }

        std::string normalizedSinkEventGuardKey(const wolvrix::lib::grh::Graph &graph,
                                                const wolvrix::lib::grh::Operation &op,
                                                wolvrix::lib::grh::OperationId opId,
                                                const ValueCanonicalMap *canonicalValues)
        {
            std::ostringstream key;
            key << normalizedSinkEventKey(graph, op, canonicalValues);
            const auto updateCond = sinkUpdateCondValue(op);
            if (!updateCond)
            {
                key << "|guard:opaque:" << static_cast<int>(op.kind());
                if (!op.symbolText().empty())
                {
                    key << ":" << op.symbolText();
                }
                key << ":" << opId.index;
                return key.str();
            }

            const auto canonicalCond = canonicalActivityValue(*updateCond, canonicalValues);
            key << "|guard:" << canonicalCond.index;
            return key.str();
        }

        WorkingPartition buildEventClusteredSinkPartition(const wolvrix::lib::grh::Graph &graph,
                                                          const ActivityOpData &opData,
                                                          const std::vector<uint32_t> &topoPositions,
                                                          std::size_t maxSize,
                                                          const ValueCanonicalMap *canonicalValues,
                                                          bool groupByGuard)
        {
            WorkingPartition partition;
            if (topoPositions.empty())
            {
                return partition;
            }

            const std::size_t chunkSize = maxSize == 0 ? topoPositions.size() : maxSize;

            if (groupByGuard)
            {
                constexpr std::size_t kMaxGuardEventMergeOps = 4096;
                const std::size_t mergeLimit =
                    maxSize == 0 ? kMaxGuardEventMergeOps : std::min(maxSize, kMaxGuardEventMergeOps);

                struct EventGuardBuckets
                {
                    std::vector<std::string> guardOrder;
                    std::unordered_map<std::string, std::vector<uint32_t>> positionsByGuard;
                };

                std::vector<std::string> eventOrder;
                std::unordered_map<std::string, EventGuardBuckets> bucketsByEvent;
                eventOrder.reserve(topoPositions.size());
                bucketsByEvent.reserve(topoPositions.size());
                for (const uint32_t topoPos : topoPositions)
                {
                    const auto opId = opData.topoOps[topoPos];
                    const auto op = graph.getOperation(opId);
                    const std::string eventKey = normalizedSinkEventKey(graph, op, canonicalValues);
                    const std::string guardKey = normalizedSinkEventGuardKey(graph, op, opId, canonicalValues);

                    auto [eventIt, eventInserted] = bucketsByEvent.try_emplace(eventKey);
                    if (eventInserted)
                    {
                        eventOrder.push_back(eventKey);
                        eventIt->second.positionsByGuard.reserve(4);
                    }

                    auto &eventBuckets = eventIt->second;
                    auto [guardIt, guardInserted] = eventBuckets.positionsByGuard.try_emplace(guardKey);
                    if (guardInserted)
                    {
                        eventBuckets.guardOrder.push_back(guardKey);
                    }
                    guardIt->second.push_back(topoPos);
                }

                partition.clusters.reserve(eventOrder.size());
                partition.fixedBoundary.reserve(eventOrder.size());
                for (const auto &eventKey : eventOrder)
                {
                    const auto eventIt = bucketsByEvent.find(eventKey);
                    if (eventIt == bucketsByEvent.end())
                    {
                        continue;
                    }
                    const auto &eventBuckets = eventIt->second;
                    std::vector<uint32_t> positions;
                    auto flushPositions = [&]()
                    {
                        if (positions.empty())
                        {
                            return;
                        }
                        partition.clusters.push_back(std::move(positions));
                        partition.fixedBoundary.push_back(0U);
                        positions = {};
                    };
                    for (const auto &guardKey : eventBuckets.guardOrder)
                    {
                        const auto guardIt = eventBuckets.positionsByGuard.find(guardKey);
                        if (guardIt == eventBuckets.positionsByGuard.end())
                        {
                            continue;
                        }
                        const auto &guardPositions = guardIt->second;
                        if (guardPositions.empty())
                        {
                            continue;
                        }
                        if (guardPositions.size() > mergeLimit)
                        {
                            flushPositions();
                            partition.clusters.emplace_back(guardPositions.begin(), guardPositions.end());
                            partition.fixedBoundary.push_back(0U);
                            continue;
                        }
                        if (!positions.empty() && positions.size() + guardPositions.size() > mergeLimit)
                        {
                            flushPositions();
                        }
                        positions.insert(positions.end(), guardPositions.begin(), guardPositions.end());
                    }
                    flushPositions();
                }

                return partition;
            }

            const std::size_t clusterReserve = (topoPositions.size() + chunkSize - 1) / chunkSize;
            partition.clusters.reserve(clusterReserve);
            partition.fixedBoundary.reserve(clusterReserve);

            std::vector<std::string> keyOrder;
            std::unordered_map<std::string, std::vector<uint32_t>> positionsByKey;
            keyOrder.reserve(topoPositions.size());
            positionsByKey.reserve(topoPositions.size());
            for (const uint32_t topoPos : topoPositions)
            {
                const auto opId = opData.topoOps[topoPos];
                const auto op = graph.getOperation(opId);
                const std::string key = normalizedSinkEventKey(graph, op, canonicalValues);
                auto [it, inserted] = positionsByKey.try_emplace(key);
                if (inserted)
                {
                    keyOrder.push_back(key);
                }
                it->second.push_back(topoPos);
            }

            for (const auto &key : keyOrder)
            {
                const auto it = positionsByKey.find(key);
                if (it == positionsByKey.end())
                {
                    continue;
                }
                const auto &positions = it->second;
                for (std::size_t offset = 0; offset < positions.size(); offset += chunkSize)
                {
                    const std::size_t end = std::min(offset + chunkSize, positions.size());
                    partition.clusters.emplace_back(positions.begin() + static_cast<std::ptrdiff_t>(offset),
                                                    positions.begin() + static_cast<std::ptrdiff_t>(end));
                    partition.fixedBoundary.push_back(0U);
                }
            }

            return partition;
        }

        bool clusterMembersAreSinkOnly(const std::vector<uint32_t> &members,
                                       const ActivityOpData &opData) noexcept
        {
            if (members.empty())
            {
                return false;
            }
            for (const auto topoPos : members)
            {
                if (topoPos >= opData.topoSinkOnly.size() || opData.topoSinkOnly[topoPos] == 0)
                {
                    return false;
                }
            }
            return true;
        }

        void markSinkOnlyClustersFixedBoundary(WorkingPartition &partition,
                                               const ActivityOpData &opData)
        {
            if (partition.fixedBoundary.size() < partition.clusters.size())
            {
                partition.fixedBoundary.resize(partition.clusters.size(), 0U);
            }
            for (std::size_t clusterId = 0; clusterId < partition.clusters.size(); ++clusterId)
            {
                if (clusterMembersAreSinkOnly(partition.clusters[clusterId], opData))
                {
                    partition.fixedBoundary[clusterId] = 1U;
                }
            }
        }

        ClusterView buildClusterView(const WorkingPartition &partition, const ActivityOpData &opData)
        {
            ClusterView view;
            view.members = partition.clusters;
            view.fixedBoundary = partition.fixedBoundary;
            view.sinkOnly.resize(view.members.size(), 0U);
            view.preds.resize(view.members.size());
            view.succs.resize(view.members.size());
            view.clusterOfTopoPos.assign(opData.topoOps.size(), kInvalidActivitySupernodeId);

            for (std::size_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                auto &members = view.members[clusterId];
                std::sort(members.begin(), members.end());
                members.erase(std::unique(members.begin(), members.end()), members.end());
                view.sinkOnly[clusterId] = clusterMembersAreSinkOnly(members, opData) ? 1U : 0U;
                for (const auto topoPos : members)
                {
                    view.clusterOfTopoPos[topoPos] = static_cast<uint32_t>(clusterId);
                }
            }

            for (const auto &[srcPos, dstPos] : opData.topoEdges)
            {
                const uint32_t srcCluster = view.clusterOfTopoPos[srcPos];
                const uint32_t dstCluster = view.clusterOfTopoPos[dstPos];
                if (srcCluster == kInvalidActivitySupernodeId ||
                    dstCluster == kInvalidActivitySupernodeId ||
                    srcCluster == dstCluster)
                {
                    continue;
                }
                view.succs[srcCluster].push_back(dstCluster);
                view.preds[dstCluster].push_back(srcCluster);
            }

            for (auto &preds : view.preds)
            {
                std::sort(preds.begin(), preds.end());
                preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
            }
            for (auto &succs : view.succs)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            return view;
        }

        WorkingPartition buildInitialPartition(const ActivityOpData &opData,
                                               const WorkingPartition &sinkPartition)
        {
            WorkingPartition partition;
            partition.clusters.reserve(opData.topoOps.size());
            partition.fixedBoundary.reserve(opData.topoOps.size());

            std::vector<uint32_t> sinkClusterOfTopoPos(opData.topoOps.size(), kInvalidActivitySupernodeId);
            for (std::size_t clusterId = 0; clusterId < sinkPartition.clusters.size(); ++clusterId)
            {
                for (const auto topoPos : sinkPartition.clusters[clusterId])
                {
                    if (topoPos < sinkClusterOfTopoPos.size())
                    {
                        sinkClusterOfTopoPos[topoPos] = static_cast<uint32_t>(clusterId);
                    }
                }
            }

            std::vector<uint8_t> emittedSink(sinkPartition.clusters.size(), 0U);
            for (std::size_t topoPos = 0; topoPos < opData.topoOps.size(); ++topoPos)
            {
                const uint32_t sinkClusterId =
                    topoPos < sinkClusterOfTopoPos.size() ? sinkClusterOfTopoPos[topoPos] : kInvalidActivitySupernodeId;
                if (sinkClusterId != kInvalidActivitySupernodeId)
                {
                    if (sinkClusterId < sinkPartition.clusters.size() && emittedSink[sinkClusterId] == 0)
                    {
                        partition.clusters.push_back(sinkPartition.clusters[sinkClusterId]);
                        partition.fixedBoundary.push_back(sinkPartition.fixedBoundary[sinkClusterId]);
                        emittedSink[sinkClusterId] = 1U;
                    }
                    continue;
                }

                partition.clusters.push_back(std::vector<uint32_t>{static_cast<uint32_t>(topoPos)});
                partition.fixedBoundary.push_back(isSideEffectBoundaryKind(opData.topoKinds[topoPos]) ? 1U : 0U);
            }
            return partition;
        }

        void markCoveredTopoPositions(const WorkingPartition &partition, std::vector<uint8_t> &coveredTopoMask)
        {
            for (const auto &cluster : partition.clusters)
            {
                for (const auto topoPos : cluster)
                {
                    if (topoPos < coveredTopoMask.size())
                    {
                        coveredTopoMask[topoPos] = 1U;
                    }
                }
            }
        }

        WorkingPartition rebuildPartitionFromDsu(const ClusterView &view,
                                                 DisjointSet &dsu,
                                                 const ActivityOpData &opData)
        {
            std::unordered_map<uint32_t, uint32_t> rootToNew;
            rootToNew.reserve(view.members.size());

            WorkingPartition out;
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                const uint32_t root = dsu.find(clusterId);
                auto [it, inserted] = rootToNew.emplace(root, static_cast<uint32_t>(out.clusters.size()));
                if (inserted)
                {
                    out.clusters.push_back({});
                    out.fixedBoundary.push_back(0);
                }
                auto &dstMembers = out.clusters[it->second];
                dstMembers.insert(dstMembers.end(), view.members[clusterId].begin(), view.members[clusterId].end());
                out.fixedBoundary[it->second] =
                    static_cast<uint8_t>(out.fixedBoundary[it->second] || view.fixedBoundary[clusterId]);
            }

            for (auto &members : out.clusters)
            {
                std::sort(members.begin(), members.end());
                members.erase(std::unique(members.begin(), members.end()), members.end());
            }
            return canonicalizePartition(out, opData);
        }

        bool tryMergeOut1(WorkingPartition &partition,
                          const ActivityOpData &opData,
                          std::size_t maxSize)
        {
            const ClusterView view = buildClusterView(partition, opData);
            DisjointSet dsu(view.members.size());
            std::vector<uint32_t> mergedSize(view.members.size(), 0);
            for (std::size_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                mergedSize[clusterId] = static_cast<uint32_t>(view.members[clusterId].size());
            }

            bool changed = false;
            for (std::size_t idx = view.members.size(); idx > 0; --idx)
            {
                const uint32_t clusterId = static_cast<uint32_t>(idx - 1);
                if (view.fixedBoundary[clusterId] != 0 || view.succs[clusterId].size() != 1)
                {
                    continue;
                }
                const uint32_t succId = view.succs[clusterId].front();
                if (view.fixedBoundary[succId] != 0 || view.sinkOnly[clusterId] != view.sinkOnly[succId])
                {
                    continue;
                }
                uint32_t lhs = dsu.find(clusterId);
                uint32_t rhs = dsu.find(succId);
                if (lhs == rhs)
                {
                    continue;
                }
                if (static_cast<std::size_t>(mergedSize[lhs] + mergedSize[rhs]) > maxSize)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    mergedSize[root] = mergedSize[lhs] + mergedSize[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            partition = rebuildPartitionFromDsu(view, dsu, opData);
            return true;
        }

        bool tryMergeIn1(WorkingPartition &partition,
                         const ActivityOpData &opData,
                         std::size_t maxSize)
        {
            const ClusterView view = buildClusterView(partition, opData);
            DisjointSet dsu(view.members.size());
            std::vector<uint32_t> mergedSize(view.members.size(), 0);
            for (std::size_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                mergedSize[clusterId] = static_cast<uint32_t>(view.members[clusterId].size());
            }

            bool changed = false;
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (view.fixedBoundary[clusterId] != 0 || view.preds[clusterId].size() != 1)
                {
                    continue;
                }
                const uint32_t predId = view.preds[clusterId].front();
                if (view.fixedBoundary[predId] != 0 || view.sinkOnly[clusterId] != view.sinkOnly[predId])
                {
                    continue;
                }
                uint32_t lhs = dsu.find(clusterId);
                uint32_t rhs = dsu.find(predId);
                if (lhs == rhs)
                {
                    continue;
                }
                if (static_cast<std::size_t>(mergedSize[lhs] + mergedSize[rhs]) > maxSize)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    mergedSize[root] = mergedSize[lhs] + mergedSize[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            partition = rebuildPartitionFromDsu(view, dsu, opData);
            return true;
        }

        bool tryMergeSiblings(WorkingPartition &partition,
                              const ActivityOpData &opData,
                              std::size_t maxSize)
        {
            const ClusterView view = buildClusterView(partition, opData);
            std::unordered_map<std::string, std::vector<uint32_t>> groups;
            groups.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (view.fixedBoundary[clusterId] != 0 || view.preds[clusterId].empty())
                {
                    continue;
                }
                std::ostringstream key;
                key << "p";
                for (const auto pred : view.preds[clusterId])
                {
                    key << '_' << pred;
                }
                groups[key.str()].push_back(clusterId);
            }

            DisjointSet dsu(view.members.size());
            std::vector<uint32_t> mergedSize(view.members.size(), 0);
            for (std::size_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                mergedSize[clusterId] = static_cast<uint32_t>(view.members[clusterId].size());
            }

            bool changed = false;
            for (auto &[_, siblings] : groups)
            {
                if (siblings.size() < 2)
                {
                    continue;
                }
                uint32_t anchor = siblings.front();
                for (std::size_t idx = 1; idx < siblings.size(); ++idx)
                {
                    const uint32_t candidate = siblings[idx];
                    uint32_t lhs = dsu.find(anchor);
                    uint32_t rhs = dsu.find(candidate);
                    if (lhs == rhs)
                    {
                        continue;
                    }
                    if (view.sinkOnly[lhs] != view.sinkOnly[rhs])
                    {
                        continue;
                    }
                    if (static_cast<std::size_t>(mergedSize[lhs] + mergedSize[rhs]) > maxSize)
                    {
                        continue;
                    }
                    if (dsu.unite(lhs, rhs))
                    {
                        const uint32_t root = dsu.find(lhs);
                        mergedSize[root] = mergedSize[lhs] + mergedSize[rhs];
                        anchor = root;
                        changed = true;
                    }
                }
            }
            if (!changed)
            {
                return false;
            }
            partition = rebuildPartitionFromDsu(view, dsu, opData);
            return true;
        }

        bool opGuaranteesOutputChangeForOperand(const wolvrix::lib::grh::Graph &graph,
                                                const wolvrix::lib::grh::Operation &op,
                                                std::size_t operandIndex,
                                                std::size_t trackedPredOperandCount) noexcept
        {
            const auto operands = op.operands();
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kAssign:
                return operands.size() == 1 && operandIndex == 0 && !op.results().empty() &&
                       graph.valueWidth(op.results().front()) >= graph.valueWidth(operands.front());
            case wolvrix::lib::grh::OperationKind::kNot:
                return operands.size() == 1 && operandIndex == 0;
            case wolvrix::lib::grh::OperationKind::kReplicate:
                return operands.size() == 2 && operandIndex == 0;
            case wolvrix::lib::grh::OperationKind::kAdd:
            case wolvrix::lib::grh::OperationKind::kSub:
            case wolvrix::lib::grh::OperationKind::kXor:
            case wolvrix::lib::grh::OperationKind::kXnor:
                return trackedPredOperandCount == 1 && operandIndex < operands.size();
            case wolvrix::lib::grh::OperationKind::kConcat:
                return operandIndex < operands.size();
            default:
                return false;
            }
        }

        bool isLegacyForwarderKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kAssign:
            case wolvrix::lib::grh::OperationKind::kConcat:
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
                return true;
            default:
                return false;
            }
        }

        std::optional<uint32_t> selectGuaranteedChangePredCluster(const wolvrix::lib::grh::Graph &graph,
                                                                  const ClusterView &view,
                                                                  const ActivityOpData &opData,
                                                                  uint32_t clusterId)
        {
            if (clusterId >= view.members.size() || view.members[clusterId].size() != 1)
            {
                return std::nullopt;
            }
            const uint32_t topoPos = view.members[clusterId].front();
            if (topoPos >= opData.topoOps.size())
            {
                return std::nullopt;
            }

            const auto opId = opData.topoOps[topoPos];
            const auto op = graph.getOperation(opId);
            const auto operands = op.operands();
            std::optional<uint32_t> primaryPred;
            std::size_t trackedPredOperandCount = 0;
            std::vector<uint32_t> predClusterByOperand(operands.size(), kInvalidActivitySupernodeId);

            for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
            {
                const auto defOpId = graph.valueDef(operands[operandIndex]);
                if (!defOpId.valid() || defOpId.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const uint32_t defTopoPos = opData.topoPosByOpIndex[defOpId.index];
                if (defTopoPos == kInvalidActivitySupernodeId || defTopoPos >= view.clusterOfTopoPos.size())
                {
                    continue;
                }
                const uint32_t defCluster = view.clusterOfTopoPos[defTopoPos];
                predClusterByOperand[operandIndex] = defCluster;
                if (defCluster == kInvalidActivitySupernodeId || defCluster == clusterId)
                {
                    continue;
                }
                const auto defKind = opData.topoKinds[defTopoPos];
                if (defKind == wolvrix::lib::grh::OperationKind::kConstant)
                {
                    continue;
                }
                if (!primaryPred.has_value())
                {
                    primaryPred = defCluster;
                    trackedPredOperandCount = 1;
                    continue;
                }
                if (*primaryPred != defCluster)
                {
                    return std::nullopt;
                }
                ++trackedPredOperandCount;
            }

            if (!primaryPred.has_value())
            {
                return std::nullopt;
            }

            bool sawGuaranteedOperand = false;
            for (std::size_t operandIndex = 0; operandIndex < predClusterByOperand.size(); ++operandIndex)
            {
                if (predClusterByOperand[operandIndex] != *primaryPred)
                {
                    continue;
                }
                if (!opGuaranteesOutputChangeForOperand(graph, op, operandIndex, trackedPredOperandCount))
                {
                    return std::nullopt;
                }
                sawGuaranteedOperand = true;
            }
            if (!sawGuaranteedOperand)
            {
                return std::nullopt;
            }
            return primaryPred;
        }

        bool tryMergeForwarders(WorkingPartition &partition,
                                const wolvrix::lib::grh::Graph &graph,
                                const ActivityOpData &opData,
                                std::size_t maxSize)
        {
            const ClusterView view = buildClusterView(partition, opData);
            DisjointSet dsu(view.members.size());
            std::vector<uint32_t> mergedSize(view.members.size(), 0);
            for (std::size_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                mergedSize[clusterId] = static_cast<uint32_t>(view.members[clusterId].size());
            }

            bool changed = false;
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (view.fixedBoundary[clusterId] != 0 || view.members[clusterId].size() != 1)
                {
                    continue;
                }
                const uint32_t topoPos = view.members[clusterId].front();
                std::optional<uint32_t> mergeTarget;
                if (const auto guaranteedPred = selectGuaranteedChangePredCluster(graph, view, opData, clusterId);
                    guaranteedPred.has_value() &&
                    *guaranteedPred < view.fixedBoundary.size() &&
                    view.fixedBoundary[*guaranteedPred] == 0)
                {
                    mergeTarget = *guaranteedPred;
                }
                else if (isLegacyForwarderKind(opData.topoKinds[topoPos]) &&
                         view.succs[clusterId].size() == 1 &&
                         view.fixedBoundary[view.succs[clusterId].front()] == 0)
                {
                    mergeTarget = view.succs[clusterId].front();
                }
                else if (isLegacyForwarderKind(opData.topoKinds[topoPos]) &&
                         view.preds[clusterId].size() == 1 &&
                         view.fixedBoundary[view.preds[clusterId].front()] == 0)
                {
                    mergeTarget = view.preds[clusterId].front();
                }
                if (!mergeTarget)
                {
                    continue;
                }
                if (view.sinkOnly[clusterId] != view.sinkOnly[*mergeTarget])
                {
                    continue;
                }

                uint32_t lhs = dsu.find(clusterId);
                uint32_t rhs = dsu.find(*mergeTarget);
                if (lhs == rhs)
                {
                    continue;
                }
                if (static_cast<std::size_t>(mergedSize[lhs] + mergedSize[rhs]) > maxSize)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    mergedSize[root] = mergedSize[lhs] + mergedSize[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            partition = rebuildPartitionFromDsu(view, dsu, opData);
            return true;
        }

        std::vector<std::vector<uint32_t>> buildDpSegments(const ClusterView &view,
                                                           std::size_t maxSize)
        {
            const std::size_t count = view.members.size();
            std::vector<int> bestCost(count + 1, std::numeric_limits<int>::max());
            std::vector<int> backtrace(count + 1, -1);
            bestCost[0] = 0;

            for (std::size_t begin = 0; begin < count; ++begin)
            {
                if (bestCost[begin] == std::numeric_limits<int>::max())
                {
                    continue;
                }

                std::size_t accumSize = 0;
                int cutCost = 0;
                bool fixedSingleton = false;
                std::optional<uint8_t> segmentSinkOnly;
                for (std::size_t end = begin + 1; end <= count; ++end)
                {
                    const std::size_t clusterId = end - 1;
                    const uint8_t clusterSinkOnly = clusterId < view.sinkOnly.size() ? view.sinkOnly[clusterId] : 0U;
                    if (!segmentSinkOnly.has_value())
                    {
                        segmentSinkOnly = clusterSinkOnly;
                    }
                    else if (*segmentSinkOnly != clusterSinkOnly)
                    {
                        break;
                    }
                    if (view.fixedBoundary[clusterId] != 0)
                    {
                        if (clusterId != begin)
                        {
                            break;
                        }
                        fixedSingleton = true;
                    }
                    else if (fixedSingleton)
                    {
                        break;
                    }

                    const std::size_t clusterSize = view.members[clusterId].size();
                    accumSize += clusterSize;
                    bool oversizedSingleton = false;
                    if (accumSize > maxSize)
                    {
                        if (clusterId != begin)
                        {
                            break;
                        }
                        oversizedSingleton = true;
                    }

                    cutCost += static_cast<int>(view.succs[clusterId].size());
                    for (const auto pred : view.preds[clusterId])
                    {
                        if (pred >= begin && pred < clusterId)
                        {
                            --cutCost;
                        }
                    }

                    const int newCost = bestCost[begin] + cutCost;
                    if (newCost < bestCost[end] ||
                        (newCost == bestCost[end] && (backtrace[end] < 0 || static_cast<int>(begin) < backtrace[end])))
                    {
                        bestCost[end] = newCost;
                        backtrace[end] = static_cast<int>(begin);
                    }
                    if (oversizedSingleton)
                    {
                        break;
                    }
                }
            }

            if (backtrace[count] < 0)
            {
                std::vector<std::vector<uint32_t>> fallback;
                fallback.reserve(count);
                for (uint32_t clusterId = 0; clusterId < count; ++clusterId)
                {
                    fallback.push_back(std::vector<uint32_t>{clusterId});
                }
                return fallback;
            }

            std::vector<std::vector<uint32_t>> segments;
            int end = static_cast<int>(count);
            while (end > 0)
            {
                const int begin = backtrace[end];
                std::vector<uint32_t> segment;
                segment.reserve(static_cast<std::size_t>(end - begin));
                for (int clusterId = begin; clusterId < end; ++clusterId)
                {
                    segment.push_back(static_cast<uint32_t>(clusterId));
                }
                segments.push_back(std::move(segment));
                end = begin;
            }
            std::reverse(segments.begin(), segments.end());
            return segments;
        }

        std::size_t segmentSize(const ClusterView &view,
                                const std::vector<uint32_t> &segment)
        {
            std::size_t size = 0;
            for (const auto clusterId : segment)
            {
                size += view.members[clusterId].size();
            }
            return size;
        }

        int computeMoveGain(const ClusterView &view,
                            const std::vector<uint32_t> &ownerByCluster,
                            uint32_t clusterId,
                            uint32_t oldSegment,
                            uint32_t newSegment)
        {
            int gain = 0;
            for (const auto pred : view.preds[clusterId])
            {
                const int oldCut = ownerByCluster[pred] == oldSegment ? 0 : 1;
                const int newCut = ownerByCluster[pred] == newSegment ? 0 : 1;
                gain += oldCut - newCut;
            }
            for (const auto succ : view.succs[clusterId])
            {
                const int oldCut = ownerByCluster[succ] == oldSegment ? 0 : 1;
                const int newCut = ownerByCluster[succ] == newSegment ? 0 : 1;
                gain += oldCut - newCut;
            }
            return gain;
        }

        std::vector<std::vector<uint32_t>> refineSegments(const ClusterView &view,
                                                          std::vector<std::vector<uint32_t>> segments,
                                                          std::size_t maxSize,
                                                          std::size_t maxIter)
        {
            if (segments.size() < 2)
            {
                return segments;
            }

            struct Move
            {
                int gain = 0;
                std::size_t boundary = 0;
                bool rightToLeft = false;
            };

            for (std::size_t iter = 0; iter < maxIter; ++iter)
            {
                std::vector<uint32_t> ownerByCluster(view.members.size(), kInvalidActivitySupernodeId);
                for (std::size_t segmentId = 0; segmentId < segments.size(); ++segmentId)
                {
                    for (const auto clusterId : segments[segmentId])
                    {
                        ownerByCluster[clusterId] = static_cast<uint32_t>(segmentId);
                    }
                }

                std::vector<std::size_t> segmentSizes(segments.size(), 0);
                for (std::size_t segmentId = 0; segmentId < segments.size(); ++segmentId)
                {
                    segmentSizes[segmentId] = segmentSize(view, segments[segmentId]);
                }

                Move bestMove;
                for (std::size_t boundary = 0; boundary + 1 < segments.size(); ++boundary)
                {
                    auto &left = segments[boundary];
                    auto &right = segments[boundary + 1];

                    if (left.size() > 1)
                    {
                        const uint32_t clusterId = left.back();
                        const std::size_t clusterSize = view.members[clusterId].size();
                        if (view.fixedBoundary[clusterId] == 0 &&
                            !right.empty() &&
                            view.sinkOnly[clusterId] == view.sinkOnly[right.front()] &&
                            segmentSizes[boundary + 1] + clusterSize <= maxSize)
                        {
                            const int gain = computeMoveGain(view,
                                                             ownerByCluster,
                                                             clusterId,
                                                             static_cast<uint32_t>(boundary),
                                                             static_cast<uint32_t>(boundary + 1));
                            if (gain > bestMove.gain)
                            {
                                bestMove = Move{gain, boundary, false};
                            }
                        }
                    }

                    if (right.size() > 1)
                    {
                        const uint32_t clusterId = right.front();
                        const std::size_t clusterSize = view.members[clusterId].size();
                        if (view.fixedBoundary[clusterId] == 0 &&
                            !left.empty() &&
                            view.sinkOnly[clusterId] == view.sinkOnly[left.back()] &&
                            segmentSizes[boundary] + clusterSize <= maxSize)
                        {
                            const int gain = computeMoveGain(view,
                                                             ownerByCluster,
                                                             clusterId,
                                                             static_cast<uint32_t>(boundary + 1),
                                                             static_cast<uint32_t>(boundary));
                            if (gain > bestMove.gain)
                            {
                                bestMove = Move{gain, boundary, true};
                            }
                        }
                    }
                }

                if (bestMove.gain <= 0)
                {
                    break;
                }

                auto &left = segments[bestMove.boundary];
                auto &right = segments[bestMove.boundary + 1];
                if (bestMove.rightToLeft)
                {
                    left.push_back(right.front());
                    right.erase(right.begin());
                }
                else
                {
                    right.insert(right.begin(), left.back());
                    left.pop_back();
                }
            }

            return segments;
        }

        WorkingPartition materializeSegments(const ClusterView &view,
                                            const std::vector<std::vector<uint32_t>> &segments)
        {
            WorkingPartition out;
            out.clusters.reserve(segments.size());
            out.fixedBoundary.reserve(segments.size());
            for (const auto &segment : segments)
            {
                std::vector<uint32_t> members;
                uint8_t fixed = 0;
                for (const auto clusterId : segment)
                {
                    members.insert(members.end(),
                                   view.members[clusterId].begin(),
                                   view.members[clusterId].end());
                    fixed = static_cast<uint8_t>(fixed || view.fixedBoundary[clusterId]);
                }
                std::sort(members.begin(), members.end());
                out.clusters.push_back(std::move(members));
                out.fixedBoundary.push_back(fixed);
            }
            return out;
        }

        SymbolPartition buildSymbolPartition(const WorkingPartition &partition,
                                             const ActivityOpData &opData)
        {
            SymbolPartition out;
            out.clusters.resize(partition.clusters.size());
            out.fixedBoundary = partition.fixedBoundary;
            for (std::size_t clusterId = 0; clusterId < partition.clusters.size(); ++clusterId)
            {
                auto &symbols = out.clusters[clusterId];
                symbols.reserve(partition.clusters[clusterId].size());
                for (const auto topoPos : partition.clusters[clusterId])
                {
                    symbols.push_back(opData.topoSymbols[topoPos]);
                }
            }
            return out;
        }

        WorkingPartition rebuildWorkingPartitionFromSymbolPartition(const SymbolPartition &partition,
                                                                   const ActivityOpData &opData)
        {
            WorkingPartition out;
            out.clusters.reserve(partition.clusters.size());
            out.fixedBoundary.reserve(partition.fixedBoundary.size());

            std::unordered_map<wolvrix::lib::grh::SymbolId, uint32_t, ActivityScheduleSymbolIdHash> topoPosBySymbol;
            topoPosBySymbol.reserve(opData.topoSymbols.size());
            for (std::size_t topoPos = 0; topoPos < opData.topoSymbols.size(); ++topoPos)
            {
                topoPosBySymbol.emplace(opData.topoSymbols[topoPos], static_cast<uint32_t>(topoPos));
            }

            for (std::size_t clusterId = 0; clusterId < partition.clusters.size(); ++clusterId)
            {
                std::vector<uint32_t> members;
                members.reserve(partition.clusters[clusterId].size());
                for (const auto symbol : partition.clusters[clusterId])
                {
                    const auto it = topoPosBySymbol.find(symbol);
                    if (it == topoPosBySymbol.end())
                    {
                        continue;
                    }
                    members.push_back(it->second);
                }
                std::sort(members.begin(), members.end());
                members.erase(std::unique(members.begin(), members.end()), members.end());
                if (members.empty())
                {
                    continue;
                }
                out.clusters.push_back(std::move(members));
                out.fixedBoundary.push_back(
                    clusterId < partition.fixedBoundary.size() ? partition.fixedBoundary[clusterId] : 0U);
            }

            return out;
        }

        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
        collectObservableBoundaryValues(const wolvrix::lib::grh::Graph &graph)
        {
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> values;
            values.reserve(graph.outputPorts().size() + graph.inoutPorts().size() * 2);
            for (const auto &port : graph.outputPorts())
            {
                values.insert(port.value);
            }
            for (const auto &port : graph.inoutPorts())
            {
                values.insert(port.out);
                values.insert(port.oe);
            }
            return values;
        }

        std::unordered_map<wolvrix::lib::grh::SymbolId, uint32_t, ActivityScheduleSymbolIdHash>
        buildSymbolToSupernodeMap(const SymbolPartition &partition)
        {
            std::unordered_map<wolvrix::lib::grh::SymbolId, uint32_t, ActivityScheduleSymbolIdHash> out;
            std::size_t total = 0;
            for (const auto &cluster : partition.clusters)
            {
                total += cluster.size();
            }
            out.reserve(total);
            for (std::size_t supernodeId = 0; supernodeId < partition.clusters.size(); ++supernodeId)
            {
                for (const auto symbol : partition.clusters[supernodeId])
                {
                    out[symbol] = static_cast<uint32_t>(supernodeId);
                }
            }
            return out;
        }

        bool isTailAbsorbableStateReadKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            return kind == wolvrix::lib::grh::OperationKind::kRegisterReadPort ||
                   kind == wolvrix::lib::grh::OperationKind::kLatchReadPort;
        }

        bool runStateReadTailAbsorbPhase(
            wolvrix::lib::grh::Graph &graph,
            SymbolPartition &partition,
            std::size_t maxTargets,
            StateReadTailAbsorbStats &stats,
            std::string &error)
        {
            if (partition.clusters.empty() || maxTargets == 0)
            {
                return false;
            }

            const auto observableValues = collectObservableBoundaryValues(graph);
            auto symbolToSupernode = buildSymbolToSupernodeMap(partition);
            std::vector<wolvrix::lib::grh::OperationId> candidates;
            candidates.reserve(graph.operations().size());
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (isTailAbsorbableStateReadKind(op.kind()))
                {
                    candidates.push_back(opId);
                }
            }

            bool changed = false;
            for (const auto opId : candidates)
            {
                const auto op = graph.getOperation(opId);
                if (!isTailAbsorbableStateReadKind(op.kind()) || op.results().size() != 1)
                {
                    continue;
                }

                const auto opSym = graph.operationSymbol(opId);
                const auto ownerIt = symbolToSupernode.find(opSym);
                if (ownerIt == symbolToSupernode.end())
                {
                    continue;
                }
                const uint32_t ownerSupernode = ownerIt->second;
                const auto result = op.results().front();

                bool mustKeepOriginal = observableValues.find(result) != observableValues.end();
                std::size_t localUserCount = 0;
                std::unordered_map<uint32_t, std::vector<wolvrix::lib::grh::ValueUser>> usersByTarget;
                std::vector<wolvrix::lib::grh::ValueUser> resultUsers;
                try
                {
                    const auto valueInfo = graph.getValue(result);
                    resultUsers.assign(valueInfo.users().begin(), valueInfo.users().end());
                }
                catch (const std::exception &ex)
                {
                    error = "activity-schedule state-read-tail-absorb getValue/users failed source=" +
                            describeOp(graph, opId) + ": " + ex.what();
                    return false;
                }

                for (const auto user : resultUsers)
                {
                    const auto userSym = graph.operationSymbol(user.operation);
                    const auto targetIt = symbolToSupernode.find(userSym);
                    if (targetIt == symbolToSupernode.end())
                    {
                        mustKeepOriginal = true;
                        continue;
                    }
                    if (targetIt->second == ownerSupernode)
                    {
                        ++localUserCount;
                        continue;
                    }
                    if (targetIt->second < partition.fixedBoundary.size() &&
                        partition.fixedBoundary[targetIt->second] != 0)
                    {
                        mustKeepOriginal = true;
                        continue;
                    }
                    usersByTarget[targetIt->second].push_back(user);
                }

                if (usersByTarget.empty())
                {
                    if (mustKeepOriginal)
                    {
                        ++stats.keptObservableReads;
                    }
                    else if (localUserCount != 0)
                    {
                        ++stats.keptLocalReads;
                    }
                    continue;
                }
                if (usersByTarget.size() > maxTargets)
                {
                    ++stats.skippedTooManyTargets;
                    continue;
                }

                std::optional<wolvrix::lib::grh::Value> resultInfo;
                try
                {
                    resultInfo = graph.getValue(result);
                }
                catch (const std::exception &ex)
                {
                    error = "activity-schedule state-read-tail-absorb getValue(resultInfo) failed source=" +
                            describeOp(graph, opId) + ": " + ex.what();
                    return false;
                }

                for (const auto &[targetSupernode, users] : usersByTarget)
                {
                    if (users.empty())
                    {
                        continue;
                    }

                    const auto cloneSym = graph.makeInternalOpSym();
                    const auto cloneOp = graph.createOperation(op.kind(), cloneSym);
                    if (op.srcLoc())
                    {
                        graph.setOpSrcLoc(cloneOp, *op.srcLoc());
                    }
                    for (const auto &attr : op.attrs())
                    {
                        graph.setAttr(cloneOp, attr.key, attr.value);
                    }
                    for (const auto operand : op.operands())
                    {
                        graph.addOperand(cloneOp, operand);
                    }

                    const auto cloneResult = graph.createValue(graph.makeInternalValSym(),
                                                               resultInfo->width(),
                                                               resultInfo->isSigned(),
                                                               resultInfo->type());
                    if (resultInfo->srcLoc())
                    {
                        graph.setValueSrcLoc(cloneResult, *resultInfo->srcLoc());
                    }
                    graph.addResult(cloneOp, cloneResult);

                    for (const auto user : users)
                    {
                        try
                        {
                            const auto userOp = graph.getOperation(user.operation);
                            const auto userOperands = userOp.operands();
                            if (user.operandIndex >= userOperands.size() || userOperands[user.operandIndex] != result)
                            {
                                std::ostringstream oss;
                                oss << "activity-schedule state-read-tail-absorb detected stale user edge: source="
                                    << describeOp(graph, opId) << " result=" << result.index
                                    << " targetSupernode=" << targetSupernode
                                    << " user=" << describeOp(graph, user.operation)
                                    << " operandIndex=" << user.operandIndex;
                                if (user.operandIndex < userOperands.size())
                                {
                                    oss << " currentOperand=" << userOperands[user.operandIndex].index;
                                }
                                error = oss.str();
                                return false;
                            }
                            graph.replaceOperand(user.operation, user.operandIndex, cloneResult);
                        }
                        catch (const std::exception &ex)
                        {
                            error = "activity-schedule state-read-tail-absorb replaceOperand failed source=" +
                                    describeOp(graph, opId) +
                                    " targetSupernode=" + std::to_string(targetSupernode) +
                                    " userOpIndex=" + std::to_string(user.operation.index) +
                                    " operandIndex=" + std::to_string(user.operandIndex) +
                                    ": " + ex.what();
                            return false;
                        }
                    }

                    partition.clusters[targetSupernode].push_back(cloneSym);
                    symbolToSupernode.emplace(cloneSym, targetSupernode);
                    ++stats.clonedOps;
                    changed = true;
                }

                if (mustKeepOriginal)
                {
                    ++stats.keptObservableReads;
                    continue;
                }
                if (localUserCount != 0)
                {
                    ++stats.keptLocalReads;
                    continue;
                }

                if (!graph.eraseOp(opId))
                {
                    error = "activity-schedule state-read-tail-absorb failed to erase source op: " +
                            describeOp(graph, opId);
                    return false;
                }
                auto &ownerCluster = partition.clusters[ownerSupernode];
                ownerCluster.erase(std::remove(ownerCluster.begin(), ownerCluster.end(), opSym),
                                   ownerCluster.end());
                symbolToSupernode.erase(opSym);
                ++stats.erasedOps;
                changed = true;
            }

            return changed;
        }

        WorkingPartition buildStateReadTailAbsorbTargetPartition(const wolvrix::lib::grh::Graph &graph,
                                                                 const WorkingPartition &seedPartition,
                                                                 const ActivityOpData &opData,
                                                                 const ActivityScheduleOptions &options)
        {
            WorkingPartition partition = seedPartition;
            if (partition.clusters.empty())
            {
                return partition;
            }

            if (options.enableCoarsen)
            {
                std::size_t tailIterations = 0;
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    const std::size_t clustersBeforeIter = partition.clusters.size();
                    if (options.enableChainMerge)
                    {
                        changed = tryMergeOut1(partition, opData, options.maxOpInComputeSupernode) || changed;
                        changed = tryMergeIn1(partition, opData, options.maxOpInComputeSupernode) || changed;
                    }
                    const std::size_t clustersAfterIter = partition.clusters.size();
                    const std::size_t clusterDelta =
                        clustersBeforeIter >= clustersAfterIter ? (clustersBeforeIter - clustersAfterIter) : 0;
                    const bool smallDeltaTail =
                        clustersBeforeIter >= kCoarsenTailLargeClusterThreshold &&
                        clusterDelta < kCoarsenTailMaxClusterDeltaExclusive;
                    if (smallDeltaTail)
                    {
                        ++tailIterations;
                    }
                    else
                    {
                        tailIterations = 0;
                    }
                    if (tailIterations >= kCoarsenTailMaxConsecutiveIters)
                    {
                        break;
                    }
                }
            }

            partition = canonicalizePartition(partition, opData);
            markSinkOnlyClustersFixedBoundary(partition, opData);
            const ClusterView coarseView = buildClusterView(partition, opData);
            const ClusterView dpView = buildTopoOrderedClusterView(coarseView);
            std::vector<std::vector<uint32_t>> segments =
                buildDpSegments(dpView, options.maxOpInComputeSupernode);
            partition = materializeSegments(dpView, segments);
            partition = canonicalizePartition(partition, opData);
            markSinkOnlyClustersFixedBoundary(partition, opData);
            return partition;
        }

        bool materializeFinalPartition(const wolvrix::lib::grh::Graph &graph,
                                       const SymbolPartition &partition,
                                       ActivityOpData &finalOpData,
                                       ActivityScheduleBuild &build,
                                       std::vector<uint32_t> &supernodeOfOp,
                                       FinalMaterializePerfStats *perf,
                                       std::string &error)
        {
            const auto rebuildOpDataStart = std::chrono::steady_clock::now();
            finalOpData = buildActivityOpData(graph, error);
            if (perf)
            {
                perf->rebuildOpDataMs = elapsedMs(rebuildOpDataStart);
            }
            if (!error.empty())
            {
                return false;
            }

            const auto mapLiveOpsStart = std::chrono::steady_clock::now();
            std::unordered_map<wolvrix::lib::grh::SymbolId, std::pair<wolvrix::lib::grh::OperationId, uint32_t>,
                               ActivityScheduleSymbolIdHash>
                liveOpsBySymbol;
            liveOpsBySymbol.reserve(finalOpData.topoOps.size());
            for (std::size_t topoPos = 0; topoPos < finalOpData.topoOps.size(); ++topoPos)
            {
                liveOpsBySymbol.emplace(finalOpData.topoSymbols[topoPos],
                                        std::make_pair(finalOpData.topoOps[topoPos], static_cast<uint32_t>(topoPos)));
            }
            if (perf)
            {
                perf->mapLiveOpsMs = elapsedMs(mapLiveOpsStart);
            }

            const auto collectLiveClustersStart = std::chrono::steady_clock::now();
            std::vector<LiveCluster> liveClusters;
            liveClusters.reserve(partition.clusters.size());
            for (const auto &cluster : partition.clusters)
            {
                LiveCluster live;
                live.topoPositions.reserve(cluster.size());
                for (const auto symbol : cluster)
                {
                    const auto it = liveOpsBySymbol.find(symbol);
                    if (it == liveOpsBySymbol.end())
                    {
                        continue;
                    }
                    live.topoPositions.push_back(it->second.second);
                    live.minTopoPos = std::min(live.minTopoPos, it->second.second);
                    live.maxTopoPos = std::max(live.maxTopoPos, it->second.second);
                    live.sinkOnly =
                        static_cast<bool>(live.sinkOnly && isSinkPartitionOp(graph.getOperation(it->second.first)));
                }
                if (live.topoPositions.empty())
                {
                    continue;
                }
                std::sort(live.topoPositions.begin(), live.topoPositions.end());
                live.topoPositions.erase(std::unique(live.topoPositions.begin(), live.topoPositions.end()),
                                         live.topoPositions.end());
                liveClusters.push_back(std::move(live));
            }

            std::sort(liveClusters.begin(), liveClusters.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.sinkOnly != rhs.sinkOnly)
                          {
                              return !lhs.sinkOnly && rhs.sinkOnly;
                          }
                          return lhs.minTopoPos < rhs.minTopoPos;
                      });
            if (perf)
            {
                perf->collectLiveClustersMs = elapsedMs(collectLiveClustersStart);
            }

            const auto buildSupernodeMapsStart = std::chrono::steady_clock::now();
            build = ActivityScheduleBuild{};
            build.supernodeToOps.resize(liveClusters.size());
            build.opToSupernode.assign(finalOpData.maxOpIndex, kInvalidActivitySupernodeId);
            build.dag.resize(liveClusters.size());
            if (!graph.values().empty())
            {
                build.valueFanout.resize(graph.values().back().index);
            }
            supernodeOfOp.assign(finalOpData.maxOpIndex + 1, kInvalidActivitySupernodeId);

            for (std::size_t supernodeId = 0; supernodeId < liveClusters.size(); ++supernodeId)
            {
                for (const auto topoPos : liveClusters[supernodeId].topoPositions)
                {
                    const auto opId = finalOpData.topoOps[topoPos];
                    build.supernodeToOps[supernodeId].push_back(opId);
                    build.opToSupernode[opId.index - 1] = static_cast<uint32_t>(supernodeId);
                    supernodeOfOp[opId.index] = static_cast<uint32_t>(supernodeId);
                }
            }
            if (perf)
            {
                perf->buildSupernodeMapsMs = elapsedMs(buildSupernodeMapsStart);
            }

            const auto buildDagStart = std::chrono::steady_clock::now();
            std::unordered_set<uint64_t> seenDagEdges;
            seenDagEdges.reserve(finalOpData.topoEdges.size());
            for (const auto &[fromPos, toPos] : finalOpData.topoEdges)
            {
                const auto fromOp = finalOpData.topoOps[fromPos];
                const auto toOp = finalOpData.topoOps[toPos];
                const uint32_t fromNode = supernodeOfOp[fromOp.index];
                const uint32_t toNode = supernodeOfOp[toOp.index];
                if (fromNode == kInvalidActivitySupernodeId || toNode == kInvalidActivitySupernodeId ||
                    fromNode == toNode)
                {
                    continue;
                }
                const uint64_t packed = (static_cast<uint64_t>(fromNode) << 32) | static_cast<uint64_t>(toNode);
                if (!seenDagEdges.insert(packed).second)
                {
                    continue;
                }
                build.dag[fromNode].push_back(toNode);
            }
            for (auto &succs : build.dag)
            {
                std::sort(succs.begin(), succs.end());
            }
            if (perf)
            {
                perf->buildDagMs = elapsedMs(buildDagStart);
            }

            const auto buildValueFanoutStart = std::chrono::steady_clock::now();
            for (wolvrix::lib::grh::OperationId toOpId : finalOpData.topoOps)
            {
                const wolvrix::lib::grh::Operation toOp = graph.getOperation(toOpId);
                const uint32_t toNode = supernodeOfOp[toOpId.index];
                if (toNode == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                for (wolvrix::lib::grh::ValueId operand : toOp.operands())
                {
                    const wolvrix::lib::grh::OperationId defOpId = graph.valueDef(operand);
                    if (!defOpId.valid())
                    {
                        continue;
                    }
                    const uint32_t fromNode = supernodeOfOp[defOpId.index];
                    if (fromNode == kInvalidActivitySupernodeId || fromNode == toNode || operand.index == 0 ||
                        operand.index > build.valueFanout.size())
                    {
                        continue;
                    }
                    build.valueFanout[operand.index - 1].push_back(toNode);
                }
            }
            for (auto &succs : build.valueFanout)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            if (perf)
            {
                perf->buildValueFanoutMs = elapsedMs(buildValueFanoutStart);
            }

            const auto buildStateReadSetsStart = std::chrono::steady_clock::now();
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    const auto stateSymbol = stateSymbolForReadOp(op);
                    if (stateSymbol && !stateSymbol->empty())
                    {
                        build.stateReadSupernodes[*stateSymbol].push_back(supernodeId);
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        for (const auto &storageSymbol : regToMemIntentSliceStorageReadSymbols(graph, op))
                        {
                            build.stateReadSupernodes[storageSymbol].push_back(supernodeId);
                        }
                    }
                }
            }
            for (auto &[stateSymbol, supernodes] : build.stateReadSupernodes)
            {
                (void)stateSymbol;
                std::sort(supernodes.begin(), supernodes.end());
                supernodes.erase(std::unique(supernodes.begin(), supernodes.end()), supernodes.end());
            }
            if (perf)
            {
                perf->buildStateReadSetsMs = elapsedMs(buildStateReadSetsStart);
            }

            const auto finalTopoStart = std::chrono::steady_clock::now();
            wolvrix::lib::toposort::TopoDag<uint32_t> finalTopoDag;
            finalTopoDag.reserveNodes(build.supernodeToOps.size());
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                finalTopoDag.addNode(supernodeId);
            }
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto succ : build.dag[supernodeId])
                {
                    finalTopoDag.addEdge(supernodeId, succ);
                }
            }

            try
            {
                const auto layers = finalTopoDag.toposort();
                for (const auto &layer : layers)
                {
                    build.topoOrder.insert(build.topoOrder.end(), layer.begin(), layer.end());
                }
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule final topo failed: ") + ex.what() + " " +
                        describeCyclePath(findCyclePath(build.dag), liveClusters);
                return false;
            }
            if (perf)
            {
                perf->finalTopoMs = elapsedMs(finalTopoStart);
            }
            return true;
        }

        struct ComputeNodeRewriteStats
        {
            using KindCountMap = ActivityScheduleSummaryStats::KindCountMap;

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
            KindCountMap computeNodeBoundaryExistingCommonOwnerByKind;
            KindCountMap computeNodeBoundaryExistingCommonOwnerByWidthBucket;
            KindCountMap computeNodeBoundaryExistingCommonOwnerByFanoutBucket;
        };

        struct ComputeNode
        {
            std::vector<wolvrix::lib::grh::OperationId> ops;
            std::vector<wolvrix::lib::grh::ValueId> boundaryInputs;
            bool commonExpr = false;
            bool indivisible = false;
            std::string intentGroup;
        };

        struct CommitNode
        {
            std::vector<wolvrix::lib::grh::OperationId> ops;
            std::vector<wolvrix::lib::grh::ValueId> inputValues;
        };

        struct ComputeRewriteBuild
        {
            std::vector<ComputeNode> computeNodes;
            std::vector<CommitNode> commitNodes;
            std::vector<std::vector<uint32_t>> computeDag;
            std::vector<uint32_t> computeTopoOrder;
            std::vector<uint32_t> computeNodeOfOp;
            ValueCanonicalMap canonicalValues;
            ComputeNodeRewriteStats stats;
        };

        const char *activitySupernodeKindName(ActivityScheduleSupernodeKind kind) noexcept
        {
            switch (kind)
            {
            case ActivityScheduleSupernodeKind::Compute:
                return "compute";
            case ActivityScheduleSupernodeKind::Commit:
                return "commit";
            }
            return "unknown";
        }

        void appendFinalSupernodeSummary(std::ostringstream &oss,
                                         const wolvrix::lib::grh::Graph &graph,
                                         const ComputeRewriteBuild &rewrite,
                                         const ActivityScheduleBuild &build,
                                         const std::vector<std::vector<uint32_t>> &computeNodesBySupernode,
                                         uint32_t supernodeId)
        {
            oss << "supernode=" << supernodeId;
            if (supernodeId < build.supernodeKinds.size())
            {
                oss << "(" << activitySupernodeKindName(build.supernodeKinds[supernodeId]) << ")";
            }
            if (supernodeId < computeNodesBySupernode.size() && !computeNodesBySupernode[supernodeId].empty())
            {
                oss << " computeNodes=[";
                const auto &nodes = computeNodesBySupernode[supernodeId];
                const std::size_t limit = std::min<std::size_t>(nodes.size(), 8);
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        oss << ",";
                    }
                    const uint32_t computeNodeId = nodes[i];
                    oss << computeNodeId;
                    if (computeNodeId < rewrite.computeNodes.size())
                    {
                        oss << ":ops=" << rewrite.computeNodes[computeNodeId].ops.size();
                if (rewrite.computeNodes[computeNodeId].commonExpr)
                {
                    oss << ":common";
                }
                if (rewrite.computeNodes[computeNodeId].indivisible)
                {
                    oss << ":indivisible";
                }
            }
                }
                if (nodes.size() > limit)
                {
                    oss << ",...";
                }
                oss << "]";
            }
            if (supernodeId < build.supernodeToOps.size())
            {
                const auto &ops = build.supernodeToOps[supernodeId];
                oss << " ops=[";
                const std::size_t limit = std::min<std::size_t>(ops.size(), 6);
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        oss << ",";
                    }
                    oss << describeOp(graph, ops[i]);
                }
                if (ops.size() > limit)
                {
                    oss << ",...";
                }
                oss << "]";
            }
        }

        void appendFinalEdgeReasons(std::ostringstream &oss,
                                    const wolvrix::lib::grh::Graph &graph,
                                    const ComputeRewriteBuild &rewrite,
                                    const ActivityScheduleBuild &build,
                                    const std::vector<uint32_t> &supernodeOfOp,
                                    uint32_t from,
                                    uint32_t to)
        {
            oss << " edge " << from << " -> " << to << " via";
            if (to >= build.supernodeToOps.size())
            {
                oss << " <invalid-target>";
                return;
            }
            std::size_t printed = 0;
            for (const auto toOpId : build.supernodeToOps[to])
            {
                const auto operands = graph.opOperands(toOpId);
                for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
                {
                    const auto operand = operands[operandIndex];
                    const auto defOp = graph.valueDef(operand);
                    if (!defOp.valid() || defOp.index >= supernodeOfOp.size() ||
                        supernodeOfOp[defOp.index] != from)
                    {
                        continue;
                    }
                    if (printed == 0)
                    {
                        oss << " ";
                    }
                    else
                    {
                        oss << "; ";
                    }
                    oss << describeValue(graph, operand)
                        << " def=" << describeOp(graph, defOp);
                    if (defOp.index < rewrite.computeNodeOfOp.size() &&
                        rewrite.computeNodeOfOp[defOp.index] != kInvalidActivitySupernodeId)
                    {
                        oss << "(computeNode=" << rewrite.computeNodeOfOp[defOp.index] << ")";
                    }
                    oss << " use=" << describeOp(graph, toOpId)
                        << "(operand=" << operandIndex;
                    if (toOpId.index < rewrite.computeNodeOfOp.size() &&
                        rewrite.computeNodeOfOp[toOpId.index] != kInvalidActivitySupernodeId)
                    {
                        oss << ",computeNode=" << rewrite.computeNodeOfOp[toOpId.index];
                    }
                    oss << ")";
                    ++printed;
                    if (printed >= 6)
                    {
                        oss << "; ...";
                        return;
                    }
                }
            }
            if (printed == 0)
            {
                oss << " <no matching operand found>";
            }
        }

        std::string describeFinalScheduleCycle(const wolvrix::lib::grh::Graph &graph,
                                               const ComputeRewriteBuild &rewrite,
                                               const ActivityScheduleBuild &build,
                                               const std::vector<std::vector<uint32_t>> &computeNodesBySupernode,
                                               const std::vector<uint32_t> &supernodeOfOp)
        {
            const std::vector<uint32_t> cycle = findCyclePath(build.dag);
            if (cycle.empty())
            {
                return "cycle=<unavailable>";
            }

            std::ostringstream oss;
            oss << "cycle_path=";
            const std::size_t nodeLimit = std::min<std::size_t>(cycle.size(), 16);
            for (std::size_t i = 0; i < nodeLimit; ++i)
            {
                if (i != 0)
                {
                    oss << " -> ";
                }
                oss << cycle[i];
            }
            if (cycle.size() > nodeLimit)
            {
                oss << " -> ...";
            }
            oss << " cycle_nodes={";
            const std::size_t summaryLimit = std::min<std::size_t>(cycle.size(), 12);
            for (std::size_t i = 0; i < summaryLimit; ++i)
            {
                if (i != 0)
                {
                    oss << " | ";
                }
                appendFinalSupernodeSummary(oss, graph, rewrite, build, computeNodesBySupernode, cycle[i]);
            }
            if (cycle.size() > summaryLimit)
            {
                oss << " | ...";
            }
            oss << "} cycle_edges={";
            const std::size_t edgeLimit = std::min<std::size_t>(cycle.size() - 1, 12);
            for (std::size_t i = 0; i < edgeLimit; ++i)
            {
                if (i != 0)
                {
                    oss << " | ";
                }
                appendFinalEdgeReasons(oss, graph, rewrite, build, supernodeOfOp, cycle[i], cycle[i + 1]);
            }
            if (cycle.size() - 1 > edgeLimit)
            {
                oss << " | ...";
            }
            oss << "}";
            return oss.str();
        }

        ActivityOpClass classifyActivityOp(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return ActivityOpClass::Source;
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
                return ActivityOpClass::Sink;
            default:
                break;
            }
            if (isStorageDeclOpKind(kind) || isHierLikeOpKind(kind))
            {
                return ActivityOpClass::Declaration;
            }
            return ActivityOpClass::Compute;
        }

        std::vector<ActivityOpClass> buildOpClasses(const wolvrix::lib::grh::Graph &graph,
                                                    std::size_t maxOpIndex)
        {
            std::vector<ActivityOpClass> out(maxOpIndex + 1, ActivityOpClass::Unsupported);
            for (const auto opId : graph.operations())
            {
                if (opId.index < out.size())
                {
                    out[opId.index] = classifyActivityOp(graph.opKind(opId));
                }
            }
            return out;
        }

        // NO0207 Phase A：静态变化概率（活动度）传播。在 PRE-clone 逻辑图的拓扑序上正向计算每个
        // op 的输出翻转概率 pi ∈ [0,1]。NO0208 §附录 A 修复：放弃「一律乘积补」（它度量「任一输入
        // 翻转」、随深度单调饱和），改用 transition-density 式的「转移函数」传播——让逻辑掩蔽点
        // （AND/OR、mux 选择、比较/归约的位宽收缩）衰减活动度，仅 XOR/算术/拼接这类「任一输入变化
        // 基本都传到输出」的 op 保留透传（乘积补）。信号概率固定取 p1=0.5（最大熵，prototype）。
        // 仍按「源代表」分组去相关（同源多输入取组内 max）。纯分析，不改划分；返回按 op index 的
        // pi（-1=非 eligible）；只在 PRE-clone 图上算（source clone 会复制 read op）。
        struct ActivityPiStats
        {
            std::size_t computeOps = 0;
            std::size_t highActivity = 0;
            std::size_t multiSource = 0;
            std::size_t histogram[6] = {0, 0, 0, 0, 0, 0};
            // NO0208 sanity：每 op-kind 与每逻辑深度的 pi 均值（核对转移函数与"是否随深度再饱和"）
            double kindSum[64] = {0};
            std::uint64_t kindCnt[64] = {0};
            double depthSum[7] = {0};
            std::uint64_t depthCnt[7] = {0};
        };

        std::vector<float> computeActivityPi(const wolvrix::lib::grh::Graph &graph,
                                             const ActivityScheduleOptions &options,
                                             const ActivityOpData &data,
                                             ActivityPiStats &stats)
        {
            namespace grh = wolvrix::lib::grh;
            stats = ActivityPiStats{};
            std::vector<float> piByOpIndex(data.maxOpIndex + 1, -1.0F);
            const std::size_t count = data.topoOps.size();
            if (count == 0)
            {
                return piByOpIndex;
            }

            const float piData = static_cast<float>(options.piDataInput);
            const float piReg = static_cast<float>(options.piRegRead);
            const float piHigh = static_cast<float>(options.piHighThreshold);
            constexpr uint32_t kMultiRep = kInvalidActivitySupernodeId;

            std::vector<float> topoPi(count, 0.0F);
            std::vector<uint32_t> rep(count, kMultiRep);
            std::vector<uint16_t> depth(count, 0); // 逻辑深度（max 输入深度 + 1；源/输入=0）

            // NO0207 Step 2 修复：源代表按状态符号（register/latch symbol）归并，使同一寄存器
            // 经不同 read port / slice 进入的输入被识别为同源去相关；外部输入按 value id 归并；
            // 常量/无符号读各取唯一 rep。rep 用稠密计数（远不会撞上 kMultiRep）。
            uint32_t nextRep = 0;
            std::unordered_map<std::string, uint32_t> symbolRep;
            std::unordered_map<uint32_t, uint32_t> extValueRep;
            auto internSymbolRep = [&](const std::string &s) -> uint32_t {
                auto [it, inserted] = symbolRep.emplace(s, nextRep);
                if (inserted)
                {
                    ++nextRep;
                }
                return it->second;
            };
            auto internExtRep = [&](uint32_t valueIndex) -> uint32_t {
                auto [it, inserted] = extValueRep.emplace(valueIndex, nextRep);
                if (inserted)
                {
                    ++nextRep;
                }
                return it->second;
            };
            auto freshRep = [&]() -> uint32_t { return nextRep++; };

            struct PiInput
            {
                float pi;
                uint32_t rep;
            };
            std::vector<PiInput> ins;

            // 输出源代表：所有输入共享同一真实 rep 且无多源/外部输入时继承之，否则多源。
            auto resolveRep = [&](const std::vector<PiInput> &items) -> uint32_t {
                uint32_t single = kMultiRep;
                for (const auto &it : items)
                {
                    if (it.rep == kMultiRep)
                    {
                        return kMultiRep;
                    }
                    if (single == kMultiRep)
                    {
                        single = it.rep;
                    }
                    else if (single != it.rep)
                    {
                        return kMultiRep;
                    }
                }
                return single;
            };
            // 源去相关：把输入按源代表分组，同组取组内 max（避免同源 reconvergence 被重复计），
            // 每个 kMultiRep 输入各自独立成组。返回每个独立源组的活动度。
            auto groupActs = [&](const std::vector<PiInput> &items) -> std::vector<float> {
                std::unordered_map<uint32_t, float> gm;
                std::vector<float> out;
                for (const auto &it : items)
                {
                    const float p = std::clamp(it.pi, 0.0F, 1.0F);
                    if (it.rep == kMultiRep)
                    {
                        out.push_back(p);
                    }
                    else
                    {
                        auto [iter, inserted] = gm.emplace(it.rep, p);
                        if (!inserted)
                        {
                            iter->second = std::max(iter->second, p);
                        }
                    }
                }
                for (const auto &e : gm)
                {
                    out.push_back(e.second);
                }
                return out;
            };
            // 透传（乘积补）：任一独立源变化基本都传到输出（XOR / 算术 / 拼接）。
            auto transparentCombine = [&](const std::vector<float> &g) -> float {
                double keep = 1.0;
                for (const float v : g)
                {
                    keep *= (1.0 - static_cast<double>(v));
                }
                return static_cast<float>(std::clamp(1.0 - keep, 0.0, 1.0));
            };
            // 逻辑掩蔽（AND/OR 系，p1=0.5）：n 输入门每输入观察概率 0.5^(n-1)，对宽门强衰减。
            auto maskedCombine = [&](const std::vector<float> &g) -> float {
                if (g.empty())
                {
                    return 0.0F;
                }
                double coef = 1.0;
                for (std::size_t k = 1; k < g.size(); ++k)
                {
                    coef *= 0.5;
                }
                double sum = 0.0;
                for (const float v : g)
                {
                    sum += static_cast<double>(v);
                }
                return static_cast<float>(std::clamp(coef * sum, 0.0, 1.0));
            };
            // 位宽收缩到 1 bit（比较 / 归约）：输出活动度按 0.5 衰减。
            auto narrowCombine = [&](const std::vector<float> &g) -> float {
                return 0.5F * transparentCombine(g);
            };
            auto maxPi = [&](const std::vector<PiInput> &items) -> float {
                float m = 0.0F;
                for (const auto &it : items)
                {
                    m = std::max(m, std::clamp(it.pi, 0.0F, 1.0F));
                }
                return m;
            };

            for (std::size_t pos = 0; pos < count; ++pos)
            {
                const auto opId = data.topoOps[pos];
                const auto kind = data.topoKinds[pos];

                ins.clear();
                uint16_t maxOpDepth = 0;
                bool anyEligibleOperand = false;
                for (const auto operand : graph.opOperands(opId))
                {
                    const auto defOp = graph.valueDef(operand);
                    uint32_t defPos = kMultiRep;
                    if (defOp.valid() && defOp.index < data.topoPosByOpIndex.size())
                    {
                        defPos = data.topoPosByOpIndex[defOp.index];
                    }
                    if (defPos != kMultiRep && static_cast<std::size_t>(defPos) < count)
                    {
                        ins.push_back(PiInput{topoPi[defPos], rep[defPos]});
                        maxOpDepth = std::max(maxOpDepth, depth[defPos]);
                        anyEligibleOperand = true;
                    }
                    else
                    {
                        // 外部输入 / 非 eligible 产生者：按数据输入先验；同一 value 归并为同源。
                        ins.push_back(PiInput{piData, internExtRep(operand.index)});
                    }
                }

                float pi = 0.0F;
                uint32_t outRep = kMultiRep;
                switch (kind)
                {
                case grh::OperationKind::kConstant:
                    pi = 0.0F;
                    outRep = freshRep(); // 常量恒不变，rep 唯一即可
                    break;
                case grh::OperationKind::kRegisterReadPort:
                case grh::OperationKind::kLatchReadPort:
                {
                    pi = piReg;
                    const auto sym = stateSymbolForReadOp(graph.getOperation(opId));
                    outRep = (sym && !sym->empty()) ? internSymbolRep(*sym) : freshRep();
                    break;
                }
                case grh::OperationKind::kMemoryReadPort:
                    pi = ins.empty() ? piReg : maxPi(ins); // π(memread) ≈ π(address)
                    outRep = resolveRep(ins);
                    break;
                // 单数据通路透传：活动度 = 输入活动度
                case grh::OperationKind::kAssign:
                case grh::OperationKind::kSliceStatic:
                case grh::OperationKind::kSliceDynamic:
                case grh::OperationKind::kSliceArray:
                case grh::OperationKind::kReplicate:
                case grh::OperationKind::kNot:
                case grh::OperationKind::kLogicNot:
                case grh::OperationKind::kShl:
                case grh::OperationKind::kLShr:
                case grh::OperationKind::kAShr:
                    pi = maxPi(ins);
                    outRep = resolveRep(ins);
                    break;
                // 位宽收缩到 1 bit 的归约：衰减
                case grh::OperationKind::kReduceAnd:
                case grh::OperationKind::kReduceOr:
                case grh::OperationKind::kReduceXor:
                case grh::OperationKind::kReduceNor:
                case grh::OperationKind::kReduceNand:
                case grh::OperationKind::kReduceXnor:
                    pi = 0.5F * maxPi(ins);
                    outRep = resolveRep(ins);
                    break;
                // 逻辑掩蔽门（AND/OR 系）：活动度被旁路信号掩蔽而衰减
                case grh::OperationKind::kAnd:
                case grh::OperationKind::kOr:
                case grh::OperationKind::kLogicAnd:
                case grh::OperationKind::kLogicOr:
                    pi = maskedCombine(groupActs(ins));
                    outRep = resolveRep(ins);
                    break;
                // XOR 系：对每个输入都完全敏感，透传
                case grh::OperationKind::kXor:
                case grh::OperationKind::kXnor:
                    pi = transparentCombine(groupActs(ins));
                    outRep = resolveRep(ins);
                    break;
                // 比较：宽输入压成 1 bit，衰减
                case grh::OperationKind::kEq:
                case grh::OperationKind::kNe:
                case grh::OperationKind::kCaseEq:
                case grh::OperationKind::kCaseNe:
                case grh::OperationKind::kWildcardEq:
                case grh::OperationKind::kWildcardNe:
                case grh::OperationKind::kLt:
                case grh::OperationKind::kLe:
                case grh::OperationKind::kGt:
                case grh::OperationKind::kGe:
                    pi = narrowCombine(groupActs(ins));
                    outRep = resolveRep(ins);
                    break;
                // 算术 / 拼接：任一输入变化基本传到输出，透传
                case grh::OperationKind::kAdd:
                case grh::OperationKind::kSub:
                case grh::OperationKind::kMul:
                case grh::OperationKind::kDiv:
                case grh::OperationKind::kMod:
                case grh::OperationKind::kConcat:
                    pi = transparentCombine(groupActs(ins));
                    outRep = resolveRep(ins);
                    break;
                case grh::OperationKind::kMux:
                {
                    const float sel = ins.empty() ? 0.0F : std::clamp(ins[0].pi, 0.0F, 1.0F);
                    double branchSum = 0.0;
                    std::size_t nb = 0;
                    for (std::size_t k = 1; k < ins.size(); ++k)
                    {
                        branchSum += static_cast<double>(std::clamp(ins[k].pi, 0.0F, 1.0F));
                        ++nb;
                    }
                    const float branchMean =
                        nb ? static_cast<float>(branchSum / static_cast<double>(nb)) : 0.0F;
                    // 选中分支活动透传 + sel 翻转贡献（p1=0.5 → 分支差异概率≈0.5）
                    pi = std::clamp(branchMean + 0.5F * sel, 0.0F, 1.0F);
                    outRep = kMultiRep;
                    break;
                }
                default:
                    // 其他（system func / dpic / xmr / sink 等）：保守透传
                    pi = transparentCombine(groupActs(ins));
                    outRep = resolveRep(ins);
                    break;
                }

                topoPi[pos] = pi;
                rep[pos] = outRep;
                const uint16_t opDepth =
                    anyEligibleOperand ? static_cast<uint16_t>(std::min<int>(maxOpDepth + 1, 65535)) : 0;
                depth[pos] = opDepth;
                if (opId.index < piByOpIndex.size())
                {
                    piByOpIndex[opId.index] = pi;
                }

                if (classifyActivityOp(kind) == ActivityOpClass::Compute)
                {
                    ++stats.computeOps;
                    if (outRep == kMultiRep && !ins.empty())
                    {
                        ++stats.multiSource;
                    }
                }
                if (pi >= piHigh)
                {
                    ++stats.highActivity;
                }
                const std::size_t bucket = pi < 0.05F   ? 0
                                           : pi < 0.2F  ? 1
                                           : pi < 0.5F  ? 2
                                           : pi < 0.8F  ? 3
                                           : pi < 0.95F ? 4
                                                        : 5;
                ++stats.histogram[bucket];

                const std::size_t kindIdx = static_cast<std::size_t>(kind);
                if (kindIdx < 64)
                {
                    stats.kindSum[kindIdx] += pi;
                    ++stats.kindCnt[kindIdx];
                }
                const std::size_t depthBucket = opDepth == 0    ? 0
                                                : opDepth <= 2  ? 1
                                                : opDepth <= 5  ? 2
                                                : opDepth <= 10 ? 3
                                                : opDepth <= 20 ? 4
                                                : opDepth <= 40 ? 5
                                                                : 6;
                stats.depthSum[depthBucket] += pi;
                ++stats.depthCnt[depthBucket];
            }

            return piByOpIndex;
        }

        enum class ActivityCostClass : uint8_t
        {
            Const,
            Src,
            Comp,
            Sink
        };

        struct ActivityCostStats
        {
            std::size_t eligibleOps = 0;
            std::size_t constOps = 0;
            std::size_t srcOps = 0;
            std::size_t compOps = 0;
            std::size_t sinkOps = 0;
            double totalComputeWeight = 0.0;
            double totalChangeWeight = 0.0;
            std::uint64_t totalFootprintBytes = 0;
            std::uint64_t widthUnitHistogram[6] = {0, 0, 0, 0, 0, 0};
            double kindWeightSum[64] = {0};
            std::uint64_t kindCnt[64] = {0};
        };

        struct ActivityCostModel
        {
            std::vector<double> computeWeightByOpIndex;
            std::vector<double> changeWeightByOpIndex;
            std::vector<std::uint64_t> footprintBytesByOpIndex;
        };

        ActivityCostClass classifyActivityCostClass(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
                return ActivityCostClass::Const;
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return ActivityCostClass::Src;
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                return ActivityCostClass::Sink;
            default:
                return ActivityCostClass::Comp;
            }
        }

        double activityCostClassUnit(ActivityCostClass costClass) noexcept
        {
            switch (costClass)
            {
            case ActivityCostClass::Const:
                return 0.125;
            case ActivityCostClass::Src:
                return 2.0;
            case ActivityCostClass::Comp:
                return 1.0;
            case ActivityCostClass::Sink:
                return 0.0;
            }
            return 1.0;
        }

        std::uint64_t activityComputeUnitsForWidth(int32_t width) noexcept
        {
            if (width <= 0)
            {
                return 1;
            }
            if (width <= 64)
            {
                return 1;
            }
            return (static_cast<std::uint64_t>(width) + 63ULL) / 64ULL;
        }

        std::uint64_t activityFootprintBytesForWidth(int32_t width) noexcept
        {
            if (width <= 1)
            {
                return 1;
            }
            if (width <= 8)
            {
                return 1;
            }
            if (width <= 16)
            {
                return 2;
            }
            if (width <= 32)
            {
                return 4;
            }
            if (width <= 64)
            {
                return 8;
            }
            return 8ULL * ((static_cast<std::uint64_t>(width) + 63ULL) / 64ULL);
        }

        int32_t activityOpResultWidth(const wolvrix::lib::grh::Graph &graph,
                                      wolvrix::lib::grh::OperationId opId)
        {
            const auto results = graph.opResults(opId);
            int32_t width = 0;
            for (const auto value : results)
            {
                width = std::max(width, graph.valueWidth(value));
            }
            return width > 0 ? width : 1;
        }

        ActivityCostModel computeActivityCostModel(const wolvrix::lib::grh::Graph &graph,
                                                   const ActivityOpData &data,
                                                   const std::vector<float> &piByOpIndex,
                                                   ActivityCostStats &stats)
        {
            stats = ActivityCostStats{};
            ActivityCostModel out;
            out.computeWeightByOpIndex.assign(data.maxOpIndex + 1, -1.0);
            out.changeWeightByOpIndex.assign(data.maxOpIndex + 1, -1.0);
            out.footprintBytesByOpIndex.assign(data.maxOpIndex + 1, 0);

            for (const auto opId : data.topoOps)
            {
                const auto kind = graph.opKind(opId);
                const ActivityCostClass costClass = classifyActivityCostClass(kind);
                const bool hasResult = !graph.opResults(opId).empty();
                const int32_t width = activityOpResultWidth(graph, opId);
                const std::uint64_t units = activityComputeUnitsForWidth(width);
                const std::uint64_t footprintBytes =
                    (costClass == ActivityCostClass::Sink || !hasResult) ? 0 : activityFootprintBytesForWidth(width);
                const double weight = activityCostClassUnit(costClass) * static_cast<double>(units);
                const double pi =
                    (opId.index < piByOpIndex.size() && piByOpIndex[opId.index] >= 0.0F)
                        ? static_cast<double>(piByOpIndex[opId.index])
                        : 0.0;
                const double changeWeight = weight * std::clamp(pi, 0.0, 1.0);

                if (opId.index < out.computeWeightByOpIndex.size())
                {
                    out.computeWeightByOpIndex[opId.index] = weight;
                    out.changeWeightByOpIndex[opId.index] = changeWeight;
                    out.footprintBytesByOpIndex[opId.index] = footprintBytes;
                }

                ++stats.eligibleOps;
                stats.totalComputeWeight += weight;
                stats.totalChangeWeight += changeWeight;
                stats.totalFootprintBytes += footprintBytes;
                switch (costClass)
                {
                case ActivityCostClass::Const: ++stats.constOps; break;
                case ActivityCostClass::Src: ++stats.srcOps; break;
                case ActivityCostClass::Comp: ++stats.compOps; break;
                case ActivityCostClass::Sink: ++stats.sinkOps; break;
                }

                const std::size_t unitBucket = units <= 1 ? 0
                                             : units <= 2 ? 1
                                             : units <= 4 ? 2
                                             : units <= 8 ? 3
                                             : units <= 16 ? 4
                                                           : 5;
                ++stats.widthUnitHistogram[unitBucket];

                const std::size_t kindIdx = static_cast<std::size_t>(kind);
                if (kindIdx < 64)
                {
                    stats.kindWeightSum[kindIdx] += weight;
                    ++stats.kindCnt[kindIdx];
                }
            }

            return out;
        }

        // NO0208 Phase D：测量 computeNode 对 MFFC 的忠实度。在同一 PRE-DP 图上算 MFFC 参考
        // （rep[u] 反向线性近似：只看 compute 消费者，全部同 rep 则继承、否则自成根），再与 builder
        // 的 computeNodeOfOp 对比 compute→compute 边——MFFC 认为同锥(rep 相同) vs builder 同
        // computeNode；差值即 builder 在 MFFC 之外引入的碎片化。只读，仅 prob 策略下调用。
        struct MffcCoverageStats
        {
            std::uint64_t computeEdges = 0;
            std::uint64_t mffcInternal = 0;        // rep[src]==rep[dst]
            std::uint64_t cnInternal = 0;          // computeNode 相同
            std::uint64_t mffcInternalCnSplit = 0; // MFFC 同锥但 builder 拆开
            std::uint64_t mffcGroups = 0;
        };

        MffcCoverageStats measureMffcCoverage(const ActivityOpData &data, const ComputeRewriteBuild &rewrite)
        {
            MffcCoverageStats s;
            const std::size_t count = data.topoOps.size();
            if (count == 0)
            {
                return s;
            }
            std::vector<uint8_t> isCompute(count, 0);
            for (std::size_t pos = 0; pos < count; ++pos)
            {
                isCompute[pos] =
                    (classifyActivityOp(data.topoKinds[pos]) == ActivityOpClass::Compute) ? 1U : 0U;
            }
            // compute→compute 消费者邻接（topo-pos 空间，CSR）
            std::vector<uint32_t> outDeg(count, 0);
            for (const auto &[src, dst] : data.topoEdges)
            {
                if (isCompute[src] && isCompute[dst])
                {
                    ++outDeg[src];
                }
            }
            std::vector<std::size_t> off(count + 1, 0);
            for (std::size_t i = 0; i < count; ++i)
            {
                off[i + 1] = off[i] + outDeg[i];
            }
            std::vector<uint32_t> cons(off[count]);
            std::vector<std::size_t> cur(off.begin(), off.end() - 1);
            for (const auto &[src, dst] : data.topoEdges)
            {
                if (isCompute[src] && isCompute[dst])
                {
                    cons[cur[src]++] = dst;
                }
            }
            // MFFC rep[]：反向拓扑；全部 compute 消费者同 rep 则继承，否则自成根（split / sink）
            constexpr uint32_t kNoRep = kInvalidActivitySupernodeId;
            std::vector<uint32_t> rep(count, kNoRep);
            for (std::size_t i = count; i-- > 0;)
            {
                if (!isCompute[i])
                {
                    continue;
                }
                uint32_t single = kNoRep;
                bool any = false;
                bool split = false;
                for (std::size_t e = off[i]; e < off[i + 1]; ++e)
                {
                    const uint32_t r = rep[cons[e]]; // dst > i，反向已处理
                    if (!any)
                    {
                        single = r;
                        any = true;
                    }
                    else if (single != r)
                    {
                        split = true;
                        break;
                    }
                }
                rep[i] = (any && !split) ? single : static_cast<uint32_t>(i);
            }
            for (std::size_t i = 0; i < count; ++i)
            {
                if (isCompute[i] && rep[i] == static_cast<uint32_t>(i))
                {
                    ++s.mffcGroups;
                }
            }
            for (const auto &[src, dst] : data.topoEdges)
            {
                if (!isCompute[src] || !isCompute[dst])
                {
                    continue;
                }
                ++s.computeEdges;
                const bool mffcSame = (rep[src] == rep[dst]);
                const auto srcOp = data.topoOps[src];
                const auto dstOp = data.topoOps[dst];
                const uint32_t cnSrc = srcOp.index < rewrite.computeNodeOfOp.size()
                                           ? rewrite.computeNodeOfOp[srcOp.index]
                                           : kInvalidActivitySupernodeId;
                const uint32_t cnDst = dstOp.index < rewrite.computeNodeOfOp.size()
                                           ? rewrite.computeNodeOfOp[dstOp.index]
                                           : kInvalidActivitySupernodeId;
                const bool cnSame = (cnSrc != kInvalidActivitySupernodeId && cnSrc == cnDst);
                if (mffcSame)
                {
                    ++s.mffcInternal;
                    if (!cnSame)
                    {
                        ++s.mffcInternalCnSplit;
                    }
                }
                if (cnSame)
                {
                    ++s.cnInternal;
                }
            }
            return s;
        }

        bool isObservableRootValue(const wolvrix::lib::grh::Graph &graph,
                                   wolvrix::lib::grh::ValueId value) noexcept
        {
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == value)
                {
                    return true;
                }
            }
            for (const auto &port : graph.inoutPorts())
            {
                if (port.out == value || port.oe == value)
                {
                    return true;
                }
            }
            return false;
        }

        bool vectorContainsValue(const std::vector<wolvrix::lib::grh::ValueId> &values,
                                 wolvrix::lib::grh::ValueId value)
        {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        std::vector<wolvrix::lib::grh::OperationId>
        uniqueOpsPreservingOrder(const std::vector<wolvrix::lib::grh::OperationId> &ops)
        {
            std::vector<wolvrix::lib::grh::OperationId> out;
            out.reserve(ops.size());
            std::unordered_set<wolvrix::lib::grh::OperationId,
                               wolvrix::lib::grh::OperationIdHash>
                seen;
            seen.reserve(ops.size());
            for (const auto opId : ops)
            {
                if (seen.insert(opId).second)
                {
                    out.push_back(opId);
                }
            }
            return out;
        }

        bool isDeclaredValue(const wolvrix::lib::grh::Graph &graph,
                             wolvrix::lib::grh::ValueId value) noexcept
        {
            if (!value.valid())
            {
                return false;
            }
            const auto symbol = graph.valueSymbol(value);
            return symbol.valid() && graph.isDeclaredSymbol(symbol);
        }

        wolvrix::lib::grh::OperationId cloneSingleResultSourceOp(wolvrix::lib::grh::Graph &graph,
                                                                 wolvrix::lib::grh::OperationId sourceOpId,
                                                                 wolvrix::lib::grh::ValueId sourceValue,
                                                                 wolvrix::lib::grh::ValueId &cloneValue,
                                                                 std::string &error)
        {
            try
            {
                const auto sourceOp = graph.getOperation(sourceOpId);
                const auto sourceInfo = graph.getValue(sourceValue);
                const auto cloneOp = graph.createOperation(sourceOp.kind(), graph.makeInternalOpSym());
                if (sourceOp.srcLoc())
                {
                    graph.setOpSrcLoc(cloneOp, *sourceOp.srcLoc());
                }
                for (const auto &attr : sourceOp.attrs())
                {
                    graph.setAttr(cloneOp, attr.key, attr.value);
                }
                for (const auto operand : sourceOp.operands())
                {
                    graph.addOperand(cloneOp, operand);
                }
                cloneValue = graph.createValue(graph.makeInternalValSym(),
                                               sourceInfo.width(),
                                               sourceInfo.isSigned(),
                                               sourceInfo.type());
                if (sourceInfo.srcLoc())
                {
                    graph.setValueSrcLoc(cloneValue, *sourceInfo.srcLoc());
                }
                graph.addResult(cloneOp, cloneValue);
                return cloneOp;
            }
            catch (const std::exception &ex)
            {
                error = "activity-schedule source clone failed source=" +
                        describeOp(graph, sourceOpId) + ": " + ex.what();
                return wolvrix::lib::grh::OperationId::invalid();
            }
        }

        bool cloneSourceUsesForCompute(wolvrix::lib::grh::Graph &graph,
                                       std::vector<ActivityOpClass> &opClasses,
                                       ComputeNodeRewriteStats &stats,
                                       ValueCanonicalMap &canonicalValues,
                                       bool &graphChanged,
                                       std::string &error)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;
            using wolvrix::lib::grh::ValueUser;

            struct Rewrite
            {
                OperationId sourceOp;
                ValueId sourceValue;
                OperationId userOp;
                uint32_t operandIndex = 0;
            };

            std::vector<Rewrite> rewrites;
            std::unordered_set<OperationId, OperationIdHash> originalSourceOps;
            for (const auto opId : graph.operations())
            {
                if (opId.index < opClasses.size() && opClasses[opId.index] == ActivityOpClass::Source)
                {
                    originalSourceOps.insert(opId);
                }
            }

            for (const auto sourceOp : originalSourceOps)
            {
                const auto results = graph.opResults(sourceOp);
                if (results.size() != 1)
                {
                    continue;
                }
                const ValueId sourceValue = results.front();
                const auto sourceValueInfo = graph.getValue(sourceValue);
                const std::vector<ValueUser> users(sourceValueInfo.users().begin(),
                                                   sourceValueInfo.users().end());
                for (const auto &user : users)
                {
                    if (!user.operation.valid() || user.operation.index >= opClasses.size())
                    {
                        continue;
                    }
                    if (opClasses[user.operation.index] != ActivityOpClass::Compute)
                    {
                        continue;
                    }
                    rewrites.push_back(Rewrite{sourceOp, sourceValue, user.operation, user.operandIndex});
                }
            }

            for (const auto &rewrite : rewrites)
            {
                ValueId cloneValue;
                const auto cloneOp =
                    cloneSingleResultSourceOp(graph, rewrite.sourceOp, rewrite.sourceValue, cloneValue, error);
                if (!cloneOp.valid())
                {
                    return false;
                }
                if (cloneOp.index >= opClasses.size())
                {
                    opClasses.resize(cloneOp.index + 1, ActivityOpClass::Unsupported);
                }
                opClasses[cloneOp.index] = ActivityOpClass::Source;
                canonicalValues[cloneValue] = rewrite.sourceValue;
                try
                {
                    graph.replaceOperand(rewrite.userOp, rewrite.operandIndex, cloneValue);
                }
                catch (const std::exception &ex)
                {
                    error = "activity-schedule source clone replaceOperand failed user=" +
                            describeOp(graph, rewrite.userOp) + ": " + ex.what();
                    return false;
                }
                ++stats.sourceClonesInComputeNodes;
                graphChanged = true;
            }
            return true;
        }

        bool sourceOpHasScheduleUse(const wolvrix::lib::grh::Graph &graph,
                                    wolvrix::lib::grh::OperationId opId,
                                    const std::vector<ActivityOpClass> &opClasses)
        {
            for (const auto result : graph.opResults(opId))
            {
                if (isObservableRootValue(graph, result))
                {
                    return true;
                }
                const auto value = graph.getValue(result);
                for (const auto &user : value.users())
                {
                    if (user.operation.index >= opClasses.size())
                    {
                        continue;
                    }
                    const ActivityOpClass userClass = opClasses[user.operation.index];
                    if (userClass == ActivityOpClass::Compute || userClass == ActivityOpClass::Sink)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        bool isLocalSharedComputeOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
            case wolvrix::lib::grh::OperationKind::kXMRRead:
            case wolvrix::lib::grh::OperationKind::kXMRWrite:
                return false;
            default:
                return true;
            }
        }

        bool opHasSideEffects(const wolvrix::lib::grh::Operation &op)
        {
            return getAttrValue<bool>(op, "hasSideEffects").value_or(false);
        }

        std::optional<uint64_t> parseSimpleConstUInt64(std::string_view literal)
        {
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (compact.empty() || compact.front() == '-')
            {
                return std::nullopt;
            }
            if (compact.front() == '+')
            {
                compact.erase(compact.begin());
            }
            if (compact.empty())
            {
                return std::nullopt;
            }

            int base = 10;
            std::string digits;
            const std::size_t tick = compact.find('\'');
            if (tick == std::string::npos)
            {
                digits = compact;
            }
            else
            {
                std::size_t pos = tick + 1;
                if (pos < compact.size() && compact[pos] == 's')
                {
                    ++pos;
                }
                if (pos >= compact.size())
                {
                    return std::nullopt;
                }
                switch (compact[pos++])
                {
                case 'b': base = 2; break;
                case 'o': base = 8; break;
                case 'd': base = 10; break;
                case 'h': base = 16; break;
                default: return std::nullopt;
                }
                digits = compact.substr(pos);
            }
            if (digits.empty())
            {
                return std::nullopt;
            }

            uint64_t value = 0;
            for (char ch : digits)
            {
                int digit = -1;
                if (ch >= '0' && ch <= '9')
                {
                    digit = ch - '0';
                }
                else if (ch >= 'a' && ch <= 'f')
                {
                    digit = 10 + (ch - 'a');
                }
                else
                {
                    return std::nullopt;
                }
                if (digit < 0 || digit >= base)
                {
                    return std::nullopt;
                }
                if (value > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(digit)) /
                                static_cast<uint64_t>(base))
                {
                    return std::nullopt;
                }
                value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
            }
            return value;
        }

        bool isRegToMemIntentSlice(const wolvrix::lib::grh::Operation &op)
        {
            using wolvrix::lib::grh::OperationKind;
            const auto group = getAttrString(op, "regToMem.intent.group");
            return (op.kind() == OperationKind::kSliceArray || op.kind() == OperationKind::kSliceDynamic) &&
                   getAttrString(op, "regToMem.intent.role").value_or(std::string()) == "slice" &&
                   getAttrString(op, "regToMem.intent.mode").value_or(std::string()) == "array-index" &&
                   group && !group->empty();
        }

        std::optional<wolvrix::lib::grh::ValueId>
        regToMemIntentSliceIndexValue(const wolvrix::lib::grh::Graph &graph,
                                      const wolvrix::lib::grh::Operation &op)
        {
            using wolvrix::lib::grh::OperationKind;
            if (!isRegToMemIntentSlice(op) || op.operands().size() != 2)
            {
                return std::nullopt;
            }
            if (op.kind() == OperationKind::kSliceArray)
            {
                return op.operands()[1];
            }

            const auto elementWidth = getAttrValue<int64_t>(op, "regToMem.intent.elementWidth");
            if (!elementWidth || *elementWidth <= 0)
            {
                return std::nullopt;
            }
            if (*elementWidth == 1)
            {
                return op.operands()[1];
            }
            const auto startDefId = graph.valueDef(op.operands()[1]);
            if (!startDefId.valid())
            {
                return std::nullopt;
            }
            const auto startDef = graph.getOperation(startDefId);
            if (startDef.kind() != OperationKind::kMul || startDef.operands().size() != 2)
            {
                return std::nullopt;
            }

            const auto constMatchesWidth = [&](wolvrix::lib::grh::ValueId value) {
                const auto defId = graph.valueDef(value);
                if (!defId.valid())
                {
                    return false;
                }
                const auto def = graph.getOperation(defId);
                if (def.kind() != OperationKind::kConstant)
                {
                    return false;
                }
                const auto literal = getAttrString(def, "constValue");
                if (!literal)
                {
                    return false;
                }
                const auto parsedValue = parseSimpleConstUInt64(*literal);
                return parsedValue && *parsedValue == static_cast<uint64_t>(*elementWidth);
            };

            const auto operands = startDef.operands();
            if (constMatchesWidth(operands[0]))
            {
                return operands[1];
            }
            if (constMatchesWidth(operands[1]))
            {
                return operands[0];
            }
            return std::nullopt;
        }

        std::vector<std::string>
        regToMemIntentSliceStorageReadSymbols(const wolvrix::lib::grh::Graph &graph,
                                              const wolvrix::lib::grh::Operation &op)
        {
            using wolvrix::lib::grh::OperationKind;

            std::vector<std::string> symbols;
            if (!isRegToMemIntentSlice(op) || op.operands().size() != 2)
            {
                return symbols;
            }
            const auto group = getAttrString(op, "regToMem.intent.group");
            const auto elementCount = getAttrValue<int64_t>(op, "regToMem.intent.elementCount");
            if (!group || group->empty() || !elementCount || *elementCount <= 0)
            {
                return symbols;
            }

            const auto concatOpId = graph.valueDef(op.operands().front());
            if (!concatOpId.valid())
            {
                return symbols;
            }
            const auto concatOp = graph.getOperation(concatOpId);
            if (concatOp.kind() != OperationKind::kConcat ||
                getAttrString(concatOp, "regToMem.intent.group").value_or(std::string()) != *group ||
                getAttrString(concatOp, "regToMem.intent.role").value_or(std::string()) != "concat" ||
                getAttrString(concatOp, "regToMem.intent.mode").value_or(std::string()) != "array-index")
            {
                return symbols;
            }

            const auto localRegSymbols =
                getAttrValue<std::vector<std::string>>(concatOp, "regToMem.intent.regSymbols");
            const auto storageRegSymbols =
                getAttrValue<std::vector<std::string>>(concatOp, "regToMem.intent.storageRegSymbols");
            const auto storageElementCount =
                getAttrValue<int64_t>(concatOp, "regToMem.intent.storageElementCount").value_or(*elementCount);
            const auto storageRowOffset =
                getAttrValue<int64_t>(concatOp, "regToMem.intent.storageRowOffset").value_or(0);
            if (!localRegSymbols ||
                localRegSymbols->size() != static_cast<std::size_t>(*elementCount) ||
                storageElementCount < *elementCount ||
                storageRowOffset < 0 ||
                storageRowOffset > storageElementCount ||
                storageElementCount - storageRowOffset < *elementCount)
            {
                return symbols;
            }

            const std::vector<std::string> &storageSymbols =
                storageRegSymbols ? *storageRegSymbols : *localRegSymbols;
            if (storageSymbols.size() != static_cast<std::size_t>(storageElementCount))
            {
                return symbols;
            }

            symbols.reserve(static_cast<std::size_t>(*elementCount));
            for (int64_t row = 0; row < *elementCount; ++row)
            {
                const auto storageRow = static_cast<std::size_t>(storageRowOffset + row);
                if (storageRow >= storageSymbols.size() || storageSymbols[storageRow].empty())
                {
                    symbols.clear();
                    return symbols;
                }
                symbols.push_back(storageSymbols[storageRow]);
            }
            return symbols;
        }

        std::size_t semanticConsumerCount(const wolvrix::lib::grh::Graph &graph,
                                          wolvrix::lib::grh::ValueId value,
                                          const std::vector<ActivityOpClass> &opClasses,
                                          uint32_t currentNode,
                                          const std::vector<uint32_t> &nodeOfOp)
        {
            std::unordered_set<uint64_t> consumers;
            const auto valueInfo = graph.getValue(value);
            for (const auto &user : valueInfo.users())
            {
                if (!user.operation.valid() || user.operation.index >= opClasses.size())
                {
                    continue;
                }
                const ActivityOpClass opClass = opClasses[user.operation.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Sink)
                {
                    continue;
                }
                if (user.operation.index < nodeOfOp.size() && nodeOfOp[user.operation.index] == currentNode)
                {
                    consumers.insert((static_cast<uint64_t>(currentNode) << 32) |
                                     static_cast<uint64_t>(user.operation.index));
                    continue;
                }
                consumers.insert(static_cast<uint64_t>(user.operation.index));
            }
            return consumers.size();
        }

        bool isEarliestSemanticConsumer(const wolvrix::lib::grh::Graph &graph,
                                        wolvrix::lib::grh::ValueId value,
                                        wolvrix::lib::grh::OperationId candidate,
                                        const std::vector<ActivityOpClass> &opClasses,
                                        const ActivityOpData &opData)
        {
            if (!candidate.valid() || candidate.index >= opData.topoPosByOpIndex.size())
            {
                return false;
            }
            const uint32_t candidatePos = opData.topoPosByOpIndex[candidate.index];
            if (candidatePos == kInvalidActivitySupernodeId)
            {
                return false;
            }
            uint32_t bestPos = kInvalidActivitySupernodeId;
            wolvrix::lib::grh::OperationId bestOp = wolvrix::lib::grh::OperationId::invalid();
            const auto valueInfo = graph.getValue(value);
            for (const auto &user : valueInfo.users())
            {
                if (!user.operation.valid() || user.operation.index >= opClasses.size() ||
                    user.operation.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const ActivityOpClass opClass = opClasses[user.operation.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Sink)
                {
                    continue;
                }
                const uint32_t userPos = opData.topoPosByOpIndex[user.operation.index];
                if (userPos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                if (bestOp == wolvrix::lib::grh::OperationId::invalid() ||
                    userPos < bestPos ||
                    (userPos == bestPos && user.operation.index < bestOp.index))
                {
                    bestPos = userPos;
                    bestOp = user.operation;
                }
            }
            return bestOp == candidate;
        }

        bool otherSemanticConsumerCanReachNode(const wolvrix::lib::grh::Graph &graph,
                                               wolvrix::lib::grh::ValueId value,
                                               wolvrix::lib::grh::OperationId currentConsumer,
                                               const std::vector<wolvrix::lib::grh::OperationId> &nodeOps,
                                               const std::vector<ActivityOpClass> &opClasses,
                                               const ActivityOpData &opData)
        {
            std::unordered_set<wolvrix::lib::grh::OperationId,
                               wolvrix::lib::grh::OperationIdHash>
                targets;
            targets.reserve(nodeOps.size());
            uint32_t maxTargetPos = 0;
            bool haveTarget = false;
            for (const auto opId : nodeOps)
            {
                if (!opId.valid() || opId.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                if (pos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                targets.insert(opId);
                maxTargetPos = haveTarget ? std::max(maxTargetPos, pos) : pos;
                haveTarget = true;
            }
            if (!haveTarget)
            {
                return false;
            }

            constexpr std::size_t kMaxReachabilityVisits = 4096;
            const auto canFollow = [&](wolvrix::lib::grh::OperationId opId)
            {
                if (!opId.valid() || opId.index >= opClasses.size() ||
                    opId.index >= opData.topoPosByOpIndex.size())
                {
                    return false;
                }
                const ActivityOpClass opClass = opClasses[opId.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Sink)
                {
                    return false;
                }
                const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                return pos != kInvalidActivitySupernodeId && pos <= maxTargetPos;
            };

            const auto valueInfo = graph.getValue(value);
            for (const auto &user : valueInfo.users())
            {
                const auto start = user.operation;
                if (start == currentConsumer || targets.find(start) != targets.end() ||
                    !canFollow(start))
                {
                    continue;
                }

                std::vector<wolvrix::lib::grh::OperationId> stack;
                std::unordered_set<wolvrix::lib::grh::OperationId,
                                   wolvrix::lib::grh::OperationIdHash>
                    seen;
                stack.push_back(start);
                seen.insert(start);
                std::size_t visits = 0;
                while (!stack.empty())
                {
                    const auto opId = stack.back();
                    stack.pop_back();
                    if (++visits > kMaxReachabilityVisits)
                    {
                        return true;
                    }
                    for (const auto result : graph.opResults(opId))
                    {
                        const auto resultInfo = graph.getValue(result);
                        for (const auto &nextUser : resultInfo.users())
                        {
                            const auto next = nextUser.operation;
                            if (targets.find(next) != targets.end())
                            {
                                return true;
                            }
                            if (!canFollow(next))
                            {
                                continue;
                            }
                            if (seen.insert(next).second)
                            {
                                stack.push_back(next);
                            }
                        }
                    }
                }
            }
            return false;
        }

        std::string widthBucket(std::size_t width)
        {
            if (width <= 1)
            {
                return "1";
            }
            if (width <= 8)
            {
                return "2-8";
            }
            if (width <= 32)
            {
                return "9-32";
            }
            if (width <= 64)
            {
                return "33-64";
            }
            if (width <= 256)
            {
                return "65-256";
            }
            return ">256";
        }

        std::string fanoutBucket(std::size_t fanout)
        {
            if (fanout <= 1)
            {
                return "1";
            }
            if (fanout <= 4)
            {
                return "2-4";
            }
            if (fanout <= 16)
            {
                return "5-16";
            }
            if (fanout <= 64)
            {
                return "17-64";
            }
            return ">64";
        }

        class ComputeNodeBuilder
        {
        public:
            ComputeNodeBuilder(wolvrix::lib::grh::Graph &graph,
                               const ActivityScheduleOptions &options,
                               const ActivityOpData &opData,
                               std::vector<ActivityOpClass> &opClasses,
                               ComputeRewriteBuild &build,
                               std::string &error)
                : graph_(graph),
                  options_(options),
                  opData_(opData),
                  opClasses_(opClasses),
                  build_(build),
                  error_(error)
            {
            }

            uint32_t ensureSourceOwnerNode(wolvrix::lib::grh::OperationId opId)
            {
                if (!opId.valid())
                {
                    return kInvalidActivitySupernodeId;
                }
                ensureOpCapacity(opId);
                uint32_t &owner = build_.computeNodeOfOp[opId.index];
                if (owner != kInvalidActivitySupernodeId)
                {
                    return owner;
                }
                owner = newNode(false);
                build_.computeNodes[owner].ops.push_back(opId);
                processOperandsBounded(owner, opId);
                return owner;
            }

            uint32_t ensureComputeNodeForOp(wolvrix::lib::grh::OperationId opId, bool commonExpr)
            {
                if (!opId.valid())
                {
                    return kInvalidActivitySupernodeId;
                }
                ensureOpCapacity(opId);
                uint32_t &owner = build_.computeNodeOfOp[opId.index];
                if (owner != kInvalidActivitySupernodeId)
                {
                    return owner;
                }
                owner = newNode(commonExpr);
                absorbOp(owner, opId);
                return owner;
            }

            std::optional<uint32_t> createIntentGroupNode(std::string group,
                                                          std::vector<wolvrix::lib::grh::OperationId> ops)
            {
                ops = uniqueOpsPreservingOrder(ops);
                if (group.empty() || ops.empty())
                {
                    return std::nullopt;
                }
                for (const auto opId : ops)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    ensureOpCapacity(opId);
                    const uint32_t existingOwner = build_.computeNodeOfOp[opId.index];
                    if (existingOwner != kInvalidActivitySupernodeId)
                    {
                        if (existingOwner < build_.computeNodes.size() &&
                            build_.computeNodes[existingOwner].intentGroup == group)
                        {
                            continue;
                        }
                        error_ = "activity-schedule reg-to-mem intent group overlaps existing compute node group=" +
                                 group + " op=" + describeOp(graph_, opId);
                        return std::nullopt;
                    }
                }

                const uint32_t nodeId = newNode(false);
                auto &node = build_.computeNodes[nodeId];
                node.indivisible = true;
                node.intentGroup = std::move(group);
                std::sort(ops.begin(), ops.end(), [&](const auto lhs, const auto rhs) {
                    return topoLessOp(opData_, lhs, rhs);
                });
                ops = uniqueOpsPreservingOrder(ops);
                for (const auto opId : ops)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    ensureOpCapacity(opId);
                    node.ops.push_back(opId);
                    build_.computeNodeOfOp[opId.index] = nodeId;
                }
                return nodeId;
            }

            bool processIntentGroupNode(uint32_t nodeId)
            {
                if (nodeId >= build_.computeNodes.size())
                {
                    return true;
                }
                const auto ops = build_.computeNodes[nodeId].ops;
                for (const auto opId : ops)
                {
                    processOperandsBounded(nodeId, opId);
                    if (!error_.empty())
                    {
                        return false;
                    }
                }
                return true;
            }

        private:
            void ensureOpCapacity(wolvrix::lib::grh::OperationId opId)
            {
                if (opId.index >= build_.computeNodeOfOp.size())
                {
                    build_.computeNodeOfOp.resize(opId.index + 1, kInvalidActivitySupernodeId);
                }
                if (opId.index >= opClasses_.size())
                {
                    opClasses_.resize(opId.index + 1, ActivityOpClass::Unsupported);
                }
            }

            uint32_t newNode(bool commonExpr)
            {
                const uint32_t nodeId = static_cast<uint32_t>(build_.computeNodes.size());
                ComputeNode node;
                node.commonExpr = commonExpr;
                build_.computeNodes.push_back(std::move(node));
                if (commonExpr)
                {
                    ++build_.stats.commonExprComputeNodes;
                }
                return nodeId;
            }

            bool canAddRawOp(uint32_t nodeId) const
            {
                const std::size_t maxOps =
                    options_.maxOpInComputeNode == 0 ? std::numeric_limits<std::size_t>::max()
                                                     : options_.maxOpInComputeNode;
                return nodeId < build_.computeNodes.size() && build_.computeNodes[nodeId].ops.size() < maxOps;
            }

            void addBoundary(uint32_t nodeId, wolvrix::lib::grh::ValueId value)
            {
                if (nodeId >= build_.computeNodes.size() || !value.valid())
                {
                    return;
                }
                auto &inputs = build_.computeNodes[nodeId].boundaryInputs;
                if (!vectorContainsValue(inputs, value))
                {
                    inputs.push_back(value);
                }
            }

            void noteExistingCommonOwner(wolvrix::lib::grh::OperationId defOp,
                                         wolvrix::lib::grh::ValueId value)
            {
                ++build_.stats.computeNodeBoundaryInputExistingCommonOwner;
                build_.stats.computeNodeBoundaryExistingCommonOwnerByKind[
                    std::string(wolvrix::lib::grh::toString(graph_.opKind(defOp)))]++;
                const auto valueInfo = graph_.getValue(value);
                build_.stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket[
                    widthBucket(valueInfo.width())]++;
                build_.stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket[
                    fanoutBucket(valueInfo.users().size())]++;
            }

            void absorbOp(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                if (nodeId >= build_.computeNodes.size() || !opId.valid() || !error_.empty())
                {
                    return;
                }
                auto &node = build_.computeNodes[nodeId];
                if (std::find(node.ops.begin(), node.ops.end(), opId) == node.ops.end())
                {
                    node.ops.push_back(opId);
                    ensureOpCapacity(opId);
                    build_.computeNodeOfOp[opId.index] = nodeId;
                }
                processOperandsBounded(nodeId, opId);
            }

            bool absorbSourceOp(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                if (nodeId >= build_.computeNodes.size() || !opId.valid())
                {
                    return false;
                }
                ensureOpCapacity(opId);
                const uint32_t existingOwner = build_.computeNodeOfOp[opId.index];
                if (existingOwner != kInvalidActivitySupernodeId)
                {
                    return existingOwner == nodeId;
                }
                auto &node = build_.computeNodes[nodeId];
                if (std::find(node.ops.begin(), node.ops.end(), opId) != node.ops.end())
                {
                    build_.computeNodeOfOp[opId.index] = nodeId;
                    return true;
                }
                node.ops.push_back(opId);
                build_.computeNodeOfOp[opId.index] = nodeId;
                processOperandsBounded(nodeId, opId);
                return true;
            }

            void processOperandsBounded(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                if (nodeId >= build_.computeNodes.size() || !opId.valid() || !error_.empty())
                {
                    return;
                }
                if (operandProcessDepth_ >= kMaxOperandProcessRecursion)
                {
                    pendingOperandProcesses_.push_back({nodeId, opId});
                    return;
                }
                ++operandProcessDepth_;
                processOperands(nodeId, opId);
                --operandProcessDepth_;
                if (operandProcessDepth_ == 0 && !drainingPendingOperands_)
                {
                    drainPendingOperandProcesses();
                }
            }

            void drainPendingOperandProcesses()
            {
                if (drainingPendingOperands_)
                {
                    return;
                }
                drainingPendingOperands_ = true;
                while (!pendingOperandProcesses_.empty() && error_.empty())
                {
                    const auto [nodeId, opId] = pendingOperandProcesses_.back();
                    pendingOperandProcesses_.pop_back();
                    processOperandsBounded(nodeId, opId);
                }
                drainingPendingOperands_ = false;
            }

            void processOperands(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                    const bool nodeIndivisible =
                        nodeId < build_.computeNodes.size() && build_.computeNodes[nodeId].indivisible;
                    const bool nodeIsIntentGroup =
                        nodeId < build_.computeNodes.size() && !build_.computeNodes[nodeId].intentGroup.empty();
                    const auto originalOperands = graph_.opOperands(opId);
                    std::vector<wolvrix::lib::grh::ValueId> operands(originalOperands.begin(), originalOperands.end());
                    if (nodeIsIntentGroup)
                    {
                        const auto op = graph_.getOperation(opId);
                        if (isRegToMemIntentSlice(op))
                        {
                            operands.clear();
                            if (const auto indexValue = regToMemIntentSliceIndexValue(graph_, op))
                            {
                                operands.push_back(*indexValue);
                            }
                        }
                    }
                    const std::size_t operandCount = operands.size();
                    for (std::size_t operandIndex = 0; operandIndex < operandCount; ++operandIndex)
                    {
                    if (operandIndex >= operands.size())
                    {
                        return;
                    }
                    const auto operand = operands[operandIndex];
                    const auto defOp = graph_.valueDef(operand);
                    if (!defOp.valid())
                    {
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputNoDef;
                        continue;
                    }
                    if (defOp.index >= opClasses_.size())
                    {
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputDefOutOfRange;
                        continue;
                    }
                    const ActivityOpClass defClass = opClasses_[defOp.index];
                    if (defClass == ActivityOpClass::Source)
                    {
                        ensureOpCapacity(defOp);
                        const uint32_t existingOwner = build_.computeNodeOfOp[defOp.index];
                        if (existingOwner == nodeId)
                        {
                            continue;
                        }
                        if (nodeIsIntentGroup)
                        {
                            ensureSourceOwnerNode(defOp);
                            if (!error_.empty())
                            {
                                return;
                            }
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputSourceSpill;
                            continue;
                        }
                        if (nodeIndivisible)
                        {
                            ensureSourceOwnerNode(defOp);
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputSourceSpill;
                            continue;
                        }
                        if (canAddRawOp(nodeId) && absorbSourceOp(nodeId, defOp))
                        {
                            continue;
                        }
                        ensureSourceOwnerNode(defOp);
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputSourceSpill;
                        continue;
                    }
                    if (defClass == ActivityOpClass::Sink)
                    {
                        error_ = "activity-schedule compute-supernode builder encountered sink predecessor source=" +
                                 describeOp(graph_, defOp) + " user=" + describeOp(graph_, opId);
                        return;
                    }
                    if (defClass != ActivityOpClass::Compute)
                    {
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputUnsupported;
                        continue;
                    }
                    if (!isLocalSharedComputeOpKind(graph_.opKind(defOp)))
                    {
                        if (opHasSideEffects(graph_.getOperation(defOp)))
                        {
                            ensureComputeNodeForOp(defOp, true);
                            if (!error_.empty())
                            {
                                return;
                            }
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputUnsupported;
                        continue;
                    }
                    ensureOpCapacity(defOp);
                    const uint32_t existingOwner = build_.computeNodeOfOp[defOp.index];
                    if (existingOwner != kInvalidActivitySupernodeId)
                    {
                        if (existingOwner != nodeId)
                        {
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputExistingOwner;
                            if (existingOwner < build_.computeNodes.size() &&
                                build_.computeNodes[existingOwner].commonExpr)
                            {
                                noteExistingCommonOwner(defOp, operand);
                            }
                        }
                        continue;
                    }
                    if (nodeIsIntentGroup)
                    {
                        const bool common =
                            semanticConsumerCount(graph_,
                                                  operand,
                                                  opClasses_,
                                                  kInvalidActivitySupernodeId,
                                                  build_.computeNodeOfOp) > 1;
                        ensureComputeNodeForOp(defOp, common);
                        if (!error_.empty())
                        {
                            return;
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputExistingOwner;
                        continue;
                    }
                    if (nodeIndivisible)
                    {
                        const bool common =
                            semanticConsumerCount(graph_,
                                                  operand,
                                                  opClasses_,
                                                  kInvalidActivitySupernodeId,
                                                  build_.computeNodeOfOp) > 1;
                        ensureComputeNodeForOp(defOp, common);
                        if (!error_.empty())
                        {
                            return;
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputExistingOwner;
                        continue;
                    }

                    const std::size_t consumers =
                        semanticConsumerCount(graph_, operand, opClasses_, nodeId, build_.computeNodeOfOp);
                    const bool shared = consumers > 1;
                    if (shared)
                    {
                        if (canAddRawOp(nodeId) &&
                            nodeId < build_.computeNodes.size() &&
                            !build_.computeNodes[nodeId].commonExpr &&
                            isEarliestSemanticConsumer(graph_, operand, opId, opClasses_, opData_) &&
                            !otherSemanticConsumerCanReachNode(graph_,
                                                               operand,
                                                               opId,
                                                               build_.computeNodes[nodeId].ops,
                                                               opClasses_,
                                                               opData_))
                        {
                            absorbOp(nodeId, defOp);
                            if (!error_.empty())
                            {
                                return;
                            }
                            continue;
                        }
                        ensureComputeNodeForOp(defOp, true);
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputShared;
                        continue;
                    }
                    if (!canAddRawOp(nodeId))
                    {
                        ensureComputeNodeForOp(defOp, false);
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputCapacity;
                        continue;
                    }
                    absorbOp(nodeId, defOp);
                    if (!error_.empty())
                    {
                        return;
                    }
                }
            }

            wolvrix::lib::grh::Graph &graph_;
            const ActivityScheduleOptions &options_;
            const ActivityOpData &opData_;
            std::vector<ActivityOpClass> &opClasses_;
            ComputeRewriteBuild &build_;
            std::string &error_;
            static constexpr std::size_t kMaxOperandProcessRecursion = 256;
            std::size_t operandProcessDepth_ = 0;
            bool drainingPendingOperands_ = false;
            std::vector<std::pair<uint32_t, wolvrix::lib::grh::OperationId>> pendingOperandProcesses_;
        };

        struct RegToMemIntentComputeGroup
        {
            std::string group;
            std::vector<wolvrix::lib::grh::OperationId> ops;
        };

        std::vector<RegToMemIntentComputeGroup>
        collectRegToMemIntentComputeGroups(const wolvrix::lib::grh::Graph &graph,
                                           const std::vector<ActivityOpClass> &opClasses)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationKind;
            using wolvrix::lib::grh::OperationIdHash;

            struct GroupBuild
            {
                std::string group;
                std::vector<OperationId> ops;
                std::unordered_set<OperationId, OperationIdHash> seen;
                std::optional<int64_t> elementWidth;
                std::optional<int64_t> elementCount;
                bool valid = true;
            };

            std::vector<GroupBuild> builders;
            std::unordered_map<std::string, std::size_t> indexByGroup;

            const auto addOp = [&](GroupBuild &builder, OperationId opId)
            {
                if (!opId.valid() || opId.index >= opClasses.size())
                {
                    return;
                }
                const ActivityOpClass opClass = opClasses[opId.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Source)
                {
                    return;
                }
                if (builder.seen.insert(opId).second)
                {
                    builder.ops.push_back(opId);
                }
            };

            const auto validateCommonAttrs = [](const wolvrix::lib::grh::Operation &op,
                                                std::string_view group,
                                                std::string_view role,
                                                GroupBuild &builder) {
                if (getAttrString(op, "regToMem.intent.group").value_or(std::string()) != group ||
                    getAttrString(op, "regToMem.intent.role").value_or(std::string()) != role ||
                    getAttrString(op, "regToMem.intent.mode").value_or(std::string()) != "array-index")
                {
                    return false;
                }
                const auto elementWidth = getAttrValue<int64_t>(op, "regToMem.intent.elementWidth");
                const auto elementCount = getAttrValue<int64_t>(op, "regToMem.intent.elementCount");
                if (!elementWidth || !elementCount || *elementWidth <= 0 || *elementCount <= 0)
                {
                    return false;
                }
                if (builder.elementWidth && *builder.elementWidth != *elementWidth)
                {
                    return false;
                }
                if (builder.elementCount && *builder.elementCount != *elementCount)
                {
                    return false;
                }
                builder.elementWidth = *elementWidth;
                builder.elementCount = *elementCount;
                return true;
            };

            for (const auto sliceOpId : graph.operations())
            {
                const auto sliceOp = graph.getOperation(sliceOpId);
                if (sliceOp.kind() != OperationKind::kSliceArray &&
                    sliceOp.kind() != OperationKind::kSliceDynamic)
                {
                    continue;
                }
                const auto group = getAttrString(sliceOp, "regToMem.intent.group");
                const auto role = getAttrString(sliceOp, "regToMem.intent.role");
                const auto mode = getAttrString(sliceOp, "regToMem.intent.mode");
                if (!group || group->empty() || role.value_or(std::string()) != "slice" ||
                    mode.value_or(std::string()) != "array-index")
                {
                    continue;
                }
                auto [it, inserted] = indexByGroup.emplace(*group, builders.size());
                if (inserted)
                {
                    GroupBuild build;
                    build.group = *group;
                    builders.push_back(std::move(build));
                }
                GroupBuild &build = builders[it->second];
                if (!build.valid)
                {
                    continue;
                }
                if (!validateCommonAttrs(sliceOp, *group, "slice", build))
                {
                    build.valid = false;
                    continue;
                }
                const auto sliceWidth = getAttrValue<int64_t>(sliceOp, "sliceWidth");
                if (!sliceWidth || *sliceWidth != *build.elementWidth ||
                    sliceOp.results().empty() ||
                    graph.valueWidth(sliceOp.results().front()) != *build.elementWidth ||
                    !regToMemIntentSliceIndexValue(graph, sliceOp))
                {
                    build.valid = false;
                    continue;
                }
                addOp(build, sliceOpId);
                const auto sliceOperands = sliceOp.operands();
                if (sliceOperands.size() != 2)
                {
                    build.valid = false;
                    continue;
                }
                const OperationId concatOpId = graph.valueDef(sliceOperands.front());
                if (!concatOpId.valid())
                {
                    build.valid = false;
                    continue;
                }
                const auto concatOp = graph.getOperation(concatOpId);
                if (concatOp.kind() != OperationKind::kConcat ||
                    concatOp.results().size() != 1 ||
                    !validateCommonAttrs(concatOp, *group, "concat", build))
                {
                    build.valid = false;
                    continue;
                }
                const auto regSymbols = getAttrValue<std::vector<std::string>>(concatOp, "regToMem.intent.regSymbols");
                const auto operandRows = getAttrValue<std::vector<int64_t>>(concatOp, "regToMem.intent.operandRows");
                const auto storageGroup =
                    getAttrString(concatOp, "regToMem.intent.storageGroup").value_or(*group);
                const auto storageRowOffset =
                    getAttrValue<int64_t>(concatOp, "regToMem.intent.storageRowOffset").value_or(0);
                const auto storageElementCount =
                    getAttrValue<int64_t>(concatOp, "regToMem.intent.storageElementCount")
                        .value_or(*build.elementCount);
                const auto concatOperands = concatOp.operands();
                if (!regSymbols || !operandRows ||
                    regSymbols->size() != static_cast<std::size_t>(*build.elementCount) ||
                    operandRows->size() != concatOperands.size() ||
                    concatOperands.size() != static_cast<std::size_t>(*build.elementCount) ||
                    storageGroup.empty() ||
                    storageElementCount < *build.elementCount ||
                    storageRowOffset < 0 ||
                    storageRowOffset > storageElementCount ||
                    storageElementCount - storageRowOffset < *build.elementCount)
                {
                    build.valid = false;
                    continue;
                }
                addOp(build, concatOpId);
                for (std::size_t operandIndex = 0; operandIndex < concatOperands.size(); ++operandIndex)
                {
                    const auto operand = concatOperands[operandIndex];
                    const OperationId readOpId = graph.valueDef(operand);
                    if (!readOpId.valid())
                    {
                        build.valid = false;
                        break;
                    }
                    const auto readOp = graph.getOperation(readOpId);
                    const int64_t row = (*operandRows)[operandIndex];
                    const auto readGroup = getAttrString(readOp, "regToMem.intent.group");
                    const auto readRow = getAttrValue<int64_t>(readOp, "regToMem.intent.row");
                    const auto readStorageGroup = getAttrString(readOp, "regToMem.intent.storageGroup");
                    const auto readStorageRow = getAttrValue<int64_t>(readOp, "regToMem.intent.storageRow");
                    const bool readLocalMatch = readGroup && *readGroup == *group && readRow && *readRow == row;
                    const bool readStorageMatch = readStorageGroup && *readStorageGroup == storageGroup &&
                                                  readStorageRow &&
                                                  *readStorageRow == row + storageRowOffset;
                    const auto readRegSymbol = getAttrString(readOp, "regSymbol");
                    if (readOp.kind() != OperationKind::kRegisterReadPort ||
                        getAttrString(readOp, "regToMem.intent.role").value_or(std::string()) != "read" ||
                        getAttrString(readOp, "regToMem.intent.mode").value_or(std::string()) != "array-index" ||
                        (!readLocalMatch && !readStorageMatch) ||
                        row < 0 || row >= *build.elementCount ||
                        !readRegSymbol ||
                        (*regSymbols)[static_cast<std::size_t>(row)] != *readRegSymbol ||
                        graph.valueWidth(operand) != *build.elementWidth)
                    {
                        build.valid = false;
                        break;
                    }
                    if (readLocalMatch)
                    {
                        addOp(build, readOpId);
                    }
                }
            }

            std::vector<RegToMemIntentComputeGroup> out;
            out.reserve(builders.size());
            for (auto &builder : builders)
            {
                if (!builder.valid || builder.ops.size() < 3)
                {
                    continue;
                }
                RegToMemIntentComputeGroup group;
                group.group = std::move(builder.group);
                group.ops = std::move(builder.ops);
                out.push_back(std::move(group));
            }
            return out;
        }

        struct ClusterValueEdges
        {
            struct ValueFanout
            {
                uint32_t sourceCluster = kInvalidActivitySupernodeId;
                std::vector<uint32_t> targetClusters;
            };

            std::unordered_map<uint64_t, std::size_t> weights;
            std::vector<std::vector<std::pair<uint32_t, std::size_t>>> outgoing;
            std::vector<ValueFanout> valueFanouts;
            std::vector<wolvrix::lib::grh::ValueId> fanoutValues;
            std::vector<std::vector<uint32_t>> sourceValuesByCluster;
            std::vector<std::vector<uint32_t>> targetValuesByCluster;
            std::vector<std::vector<uint32_t>> commitSuccsByCluster;
        };

        uint64_t packClusterPair(uint32_t from, uint32_t to) noexcept
        {
            return (static_cast<uint64_t>(from) << 32) | static_cast<uint64_t>(to);
        }

        std::size_t clusterEdgeWeight(const ClusterValueEdges &edges, uint32_t from, uint32_t to)
        {
            const auto it = edges.weights.find(packClusterPair(from, to));
            return it == edges.weights.end() ? 0 : it->second;
        }

        std::vector<uint32_t> computeNodeOpSizes(const ComputeRewriteBuild &rewrite)
        {
            std::vector<uint32_t> out;
            out.reserve(rewrite.computeNodes.size());
            for (const auto &node : rewrite.computeNodes)
            {
                out.push_back(static_cast<uint32_t>(node.ops.size()));
            }
            return out;
        }

        std::size_t clusterOpSize(const std::vector<uint32_t> &members,
                                  const std::vector<uint32_t> &nodeOpSizes)
        {
            std::size_t total = 0;
            for (const uint32_t node : members)
            {
                if (node < nodeOpSizes.size())
                {
                    total += nodeOpSizes[node];
                }
            }
            return total;
        }

        std::vector<uint32_t> topoOrderForDag(const std::vector<std::vector<uint32_t>> &dag)
        {
            wolvrix::lib::toposort::TopoDag<uint32_t> topoDag;
            topoDag.reserveNodes(dag.size());
            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                topoDag.addNode(node);
            }
            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                for (const auto succ : dag[node])
                {
                    topoDag.addEdge(node, succ);
                }
            }
            std::vector<uint32_t> out;
            const auto layers = topoDag.toposort();
            for (const auto &layer : layers)
            {
                std::vector<uint32_t> ordered(layer.begin(), layer.end());
                std::sort(ordered.begin(), ordered.end());
                out.insert(out.end(), ordered.begin(), ordered.end());
            }
            return out;
        }

        std::vector<uint32_t> topoOrderForDagValueLocal(const std::vector<std::vector<uint32_t>> &dag,
                                                        const ClusterValueEdges *valueEdges)
        {
            std::vector<uint32_t> indegree(dag.size(), 0);
            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                for (const auto succ : dag[node])
                {
                    if (succ < indegree.size())
                    {
                        ++indegree[succ];
                    }
                }
            }

            std::set<uint32_t> ready;
            for (uint32_t node = 0; node < indegree.size(); ++node)
            {
                if (indegree[node] == 0)
                {
                    ready.insert(node);
                }
            }

            std::vector<uint32_t> out;
            out.reserve(dag.size());
            uint32_t previous = kInvalidActivitySupernodeId;
            while (!ready.empty())
            {
                uint32_t node = *ready.begin();
                if (valueEdges != nullptr && previous != kInvalidActivitySupernodeId &&
                    previous < valueEdges->outgoing.size())
                {
                    std::size_t bestWeight = 0;
                    for (const auto &[candidate, weight] : valueEdges->outgoing[previous])
                    {
                        if (ready.find(candidate) == ready.end())
                        {
                            continue;
                        }
                        if (weight > bestWeight || (weight == bestWeight && candidate < node))
                        {
                            node = candidate;
                            bestWeight = weight;
                        }
                    }
                }

                ready.erase(node);
                out.push_back(node);
                previous = node;

                if (node >= dag.size())
                {
                    continue;
                }
                for (const auto succ : dag[node])
                {
                    if (succ >= indegree.size() || indegree[succ] == 0)
                    {
                        continue;
                    }
                    --indegree[succ];
                    if (indegree[succ] == 0)
                    {
                        ready.insert(succ);
                    }
                }
            }
            if (out.size() != dag.size())
            {
                throw std::runtime_error("toposort failed: graph contains cycle");
            }
            return out;
        }

        void buildComputeDag(ComputeRewriteBuild &build,
                             const std::vector<uint32_t> &nodeOfOpByIndex,
                             const wolvrix::lib::grh::Graph &graph)
        {
            build.computeDag.assign(build.computeNodes.size(), {});
            build.stats.computeNodeBoundaryValues = 0;
            std::unordered_set<uint64_t> seen;
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                for (const auto boundary : build.computeNodes[nodeId].boundaryInputs)
                {
                    const auto defOp = graph.valueDef(boundary);
                    if (!defOp.valid() || defOp.index >= nodeOfOpByIndex.size())
                    {
                        continue;
                    }
                    const uint32_t pred = nodeOfOpByIndex[defOp.index];
                    if (pred == kInvalidActivitySupernodeId || pred == nodeId)
                    {
                        continue;
                    }
                    const uint64_t packed = (static_cast<uint64_t>(pred) << 32) | nodeId;
                    if (seen.insert(packed).second)
                    {
                        build.computeDag[pred].push_back(nodeId);
                        ++build.stats.computeNodeBoundaryValues;
                    }
                }
            }
            for (auto &succs : build.computeDag)
            {
                std::sort(succs.begin(), succs.end());
            }
        }

        uint32_t topoPosForOp(const ActivityOpData &opData, wolvrix::lib::grh::OperationId opId)
        {
            if (!opId.valid() || opId.index >= opData.topoPosByOpIndex.size())
            {
                return kInvalidActivitySupernodeId;
            }
            return opData.topoPosByOpIndex[opId.index];
        }

        bool topoLessOp(const ActivityOpData &opData,
                        wolvrix::lib::grh::OperationId lhs,
                        wolvrix::lib::grh::OperationId rhs)
        {
            const uint32_t lhsPos = topoPosForOp(opData, lhs);
            const uint32_t rhsPos = topoPosForOp(opData, rhs);
            if (lhsPos != rhsPos)
            {
                if (lhsPos == kInvalidActivitySupernodeId)
                {
                    return false;
                }
                if (rhsPos == kInvalidActivitySupernodeId)
                {
                    return true;
                }
                return lhsPos < rhsPos;
            }
            return lhs.index < rhs.index;
        }

        void recomputeComputeNodeOwnersAndBoundaries(ComputeRewriteBuild &build,
                                                     const wolvrix::lib::grh::Graph &graph)
        {
            std::size_t mapSize = build.computeNodeOfOp.size();
            for (const auto &node : build.computeNodes)
            {
                for (const auto opId : node.ops)
                {
                    mapSize = std::max<std::size_t>(mapSize, opId.index + 1);
                }
            }
            build.computeNodeOfOp.assign(mapSize, kInvalidActivitySupernodeId);
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                auto &node = build.computeNodes[nodeId];
                node.boundaryInputs.clear();
                for (const auto opId : node.ops)
                {
                    if (opId.index >= build.computeNodeOfOp.size())
                    {
                        build.computeNodeOfOp.resize(opId.index + 1, kInvalidActivitySupernodeId);
                    }
                    build.computeNodeOfOp[opId.index] = nodeId;
                }
            }
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                auto &node = build.computeNodes[nodeId];
                for (const auto opId : node.ops)
                {
                    for (const auto operand : graph.opOperands(opId))
                    {
                        const auto defOp = graph.valueDef(operand);
                        if (!defOp.valid() || defOp.index >= build.computeNodeOfOp.size() ||
                            build.computeNodeOfOp[defOp.index] != nodeId)
                        {
                            if (!vectorContainsValue(node.boundaryInputs, operand))
                            {
                                node.boundaryInputs.push_back(operand);
                            }
                        }
                    }
                }
            }
        }

        bool splitCycleComputeNodesToSingletons(ComputeRewriteBuild &build,
                                                const wolvrix::lib::grh::Graph &graph,
                                                const ActivityOpData &opData,
                                                const std::vector<uint32_t> &cycle)
        {
            if (cycle.empty())
            {
                return false;
            }
            std::vector<uint8_t> splitNode(build.computeNodes.size(), 0U);
            bool anySplit = false;
            std::size_t extraNodes = 0;
            for (const uint32_t nodeId : cycle)
            {
                if (nodeId >= build.computeNodes.size())
                {
                    continue;
                }
                if (build.computeNodes[nodeId].ops.size() <= 1)
                {
                    continue;
                }
                if (splitNode[nodeId] == 0U)
                {
                    splitNode[nodeId] = 1U;
                    anySplit = true;
                    extraNodes += build.computeNodes[nodeId].ops.size() - 1;
                }
            }
            if (!anySplit)
            {
                return false;
            }

            std::vector<ComputeNode> nextNodes;
            nextNodes.reserve(build.computeNodes.size() + extraNodes);
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                const auto &node = build.computeNodes[nodeId];
                if (splitNode[nodeId] == 0U)
                {
                    ComputeNode copy;
                    copy.ops = node.ops;
                    copy.commonExpr = node.commonExpr;
                    nextNodes.push_back(std::move(copy));
                    continue;
                }
                std::vector<wolvrix::lib::grh::OperationId> orderedOps = node.ops;
                std::sort(orderedOps.begin(), orderedOps.end(), [&](const auto lhs, const auto rhs) {
                    return topoLessOp(opData, lhs, rhs);
                });
                for (const auto opId : orderedOps)
                {
                    ComputeNode split;
                    split.ops.push_back(opId);
                    split.commonExpr = node.commonExpr;
                    nextNodes.push_back(std::move(split));
                }
            }
            build.computeNodes = std::move(nextNodes);
            recomputeComputeNodeOwnersAndBoundaries(build, graph);
            return true;
        }

        void appendComputeNodeSummary(std::ostringstream &oss,
                                      const wolvrix::lib::grh::Graph &graph,
                                      const ComputeRewriteBuild &build,
                                      const ActivityOpData &opData,
                                      uint32_t nodeId)
        {
            oss << "node=" << nodeId;
            if (nodeId >= build.computeNodes.size())
            {
                return;
            }
            const auto &node = build.computeNodes[nodeId];
            oss << ":ops=" << node.ops.size()
                << ":boundaries=" << node.boundaryInputs.size();
            if (node.commonExpr)
            {
                oss << ":common";
            }
            uint32_t minTopo = kInvalidActivitySupernodeId;
            uint32_t maxTopo = 0;
            for (const auto opId : node.ops)
            {
                if (!opId.valid() || opId.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                if (pos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                minTopo = std::min(minTopo, pos);
                maxTopo = std::max(maxTopo, pos);
            }
            if (minTopo != kInvalidActivitySupernodeId)
            {
                oss << ":topo=[" << minTopo << "," << maxTopo << "]";
            }
            oss << "[";
            const std::size_t opLimit = std::min<std::size_t>(node.ops.size(), 6);
            for (std::size_t opIndex = 0; opIndex < opLimit; ++opIndex)
            {
                if (opIndex != 0)
                {
                    oss << ",";
                }
                const auto opId = node.ops[opIndex];
                oss << describeOp(graph, opId);
                if (opId.valid() && opId.index < opData.topoPosByOpIndex.size())
                {
                    const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                    if (pos != kInvalidActivitySupernodeId)
                    {
                        oss << "@topo" << pos;
                    }
                }
            }
            if (node.ops.size() > opLimit)
            {
                oss << ",...";
            }
            oss << "]";
        }

        void appendComputeNodeEdgeReasons(std::ostringstream &oss,
                                          const wolvrix::lib::grh::Graph &graph,
                                          const ComputeRewriteBuild &build,
                                          const ActivityOpData &opData,
                                          uint32_t from,
                                          uint32_t to)
        {
            oss << " edge " << from << " -> " << to << " via";
            if (to >= build.computeNodes.size())
            {
                oss << " <invalid-target>";
                return;
            }
            std::size_t printed = 0;
            for (const auto boundary : build.computeNodes[to].boundaryInputs)
            {
                const auto defOp = graph.valueDef(boundary);
                if (!defOp.valid() || defOp.index >= build.computeNodeOfOp.size() ||
                    build.computeNodeOfOp[defOp.index] != from)
                {
                    continue;
                }
                for (const auto useOp : build.computeNodes[to].ops)
                {
                    const auto operands = graph.opOperands(useOp);
                    for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
                    {
                        if (operands[operandIndex] != boundary)
                        {
                            continue;
                        }
                        if (printed == 0)
                        {
                            oss << " ";
                        }
                        else
                        {
                            oss << "; ";
                        }
                        oss << describeValue(graph, boundary)
                            << " def=" << describeOp(graph, defOp);
                        if (defOp.index < opData.topoPosByOpIndex.size())
                        {
                            const uint32_t pos = opData.topoPosByOpIndex[defOp.index];
                            if (pos != kInvalidActivitySupernodeId)
                            {
                                oss << "@topo" << pos;
                            }
                        }
                        oss << " use=" << describeOp(graph, useOp)
                            << "(operand=" << operandIndex;
                        if (useOp.index < opData.topoPosByOpIndex.size())
                        {
                            const uint32_t pos = opData.topoPosByOpIndex[useOp.index];
                            if (pos != kInvalidActivitySupernodeId)
                            {
                                oss << ",topo=" << pos;
                            }
                        }
                        oss << ")";
                        ++printed;
                        if (printed >= 6)
                        {
                            oss << "; ...";
                            return;
                        }
                    }
                }
            }
            if (printed == 0)
            {
                oss << " <no matching boundary found>";
            }
        }

        struct NodeClusterView
        {
            std::vector<std::vector<uint32_t>> members;
            std::vector<std::vector<uint32_t>> preds;
            std::vector<std::vector<uint32_t>> succs;
            std::vector<uint32_t> clusterOfNode;
        };

        NodeClusterView buildNodeClusterView(const std::vector<std::vector<uint32_t>> &clusters,
                                             const std::vector<std::vector<uint32_t>> &nodeDag,
                                             std::size_t nodeCount)
        {
            NodeClusterView view;
            view.members = clusters;
            view.preds.resize(clusters.size());
            view.succs.resize(clusters.size());
            view.clusterOfNode.assign(nodeCount, kInvalidActivitySupernodeId);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                auto &members = view.members[clusterId];
                std::sort(members.begin(), members.end());
                members.erase(std::unique(members.begin(), members.end()), members.end());
                for (const auto node : members)
                {
                    if (node < view.clusterOfNode.size())
                    {
                        view.clusterOfNode[node] = clusterId;
                    }
                }
            }
            for (uint32_t node = 0; node < nodeDag.size(); ++node)
            {
                const uint32_t from = node < view.clusterOfNode.size() ? view.clusterOfNode[node]
                                                                       : kInvalidActivitySupernodeId;
                if (from == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                for (const auto succNode : nodeDag[node])
                {
                    const uint32_t to = succNode < view.clusterOfNode.size() ? view.clusterOfNode[succNode]
                                                                             : kInvalidActivitySupernodeId;
                    if (to == kInvalidActivitySupernodeId || to == from)
                    {
                        continue;
                    }
                    view.succs[from].push_back(to);
                    view.preds[to].push_back(from);
                }
            }
            for (auto &preds : view.preds)
            {
                std::sort(preds.begin(), preds.end());
                preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
            }
            for (auto &succs : view.succs)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            return view;
        }

        ClusterValueEdges buildClusterValueEdges(const NodeClusterView &view,
                                                 const ComputeRewriteBuild &rewrite,
                                                 const wolvrix::lib::grh::Graph &graph)
        {
            ClusterValueEdges out;
            out.outgoing.resize(view.members.size());
            out.sourceValuesByCluster.resize(view.members.size());
            out.targetValuesByCluster.resize(view.members.size());
            out.commitSuccsByCluster.resize(view.members.size());
            std::unordered_map<wolvrix::lib::grh::ValueId,
                               uint32_t,
                               wolvrix::lib::grh::ValueIdHash>
                valueToFanout;
            for (uint32_t toCluster = 0; toCluster < view.members.size(); ++toCluster)
            {
                for (const auto nodeId : view.members[toCluster])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    for (const auto boundary : rewrite.computeNodes[nodeId].boundaryInputs)
                    {
                        const auto defOp = graph.valueDef(boundary);
                        if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                        {
                            continue;
                        }
                        const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                        if (predNode == kInvalidActivitySupernodeId || predNode >= view.clusterOfNode.size())
                        {
                            continue;
                        }
                        const uint32_t fromCluster = view.clusterOfNode[predNode];
                        if (fromCluster == kInvalidActivitySupernodeId || fromCluster == toCluster)
                        {
                            continue;
                        }
                        auto [it, inserted] =
                            valueToFanout.emplace(boundary, static_cast<uint32_t>(out.valueFanouts.size()));
                        if (inserted)
                        {
                            ClusterValueEdges::ValueFanout fanout;
                            fanout.sourceCluster = fromCluster;
                            out.valueFanouts.push_back(std::move(fanout));
                            out.fanoutValues.push_back(boundary);
                            if (fromCluster < out.sourceValuesByCluster.size())
                            {
                                out.sourceValuesByCluster[fromCluster].push_back(it->second);
                            }
                        }
                        auto &targets = out.valueFanouts[it->second].targetClusters;
                        if (std::find(targets.begin(), targets.end(), toCluster) == targets.end())
                        {
                            targets.push_back(toCluster);
                            ++out.weights[packClusterPair(fromCluster, toCluster)];
                            if (toCluster < out.targetValuesByCluster.size())
                            {
                                out.targetValuesByCluster[toCluster].push_back(it->second);
                            }
                        }
                    }
                }
            }
            const uint32_t commitBase = static_cast<uint32_t>(view.members.size());
            for (uint32_t commitId = 0; commitId < rewrite.commitNodes.size(); ++commitId)
            {
                const auto &commit = rewrite.commitNodes[commitId];
                for (const auto input : commit.inputValues)
                {
                    const auto defOp = graph.valueDef(input);
                    if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                    if (predNode == kInvalidActivitySupernodeId ||
                        predNode >= view.clusterOfNode.size())
                    {
                        continue;
                    }
                    const uint32_t fromCluster = view.clusterOfNode[predNode];
                    if (fromCluster == kInvalidActivitySupernodeId ||
                        fromCluster >= out.commitSuccsByCluster.size())
                    {
                        continue;
                    }
                    out.commitSuccsByCluster[fromCluster].push_back(commitBase + commitId);
                }
            }
            for (auto &fanout : out.valueFanouts)
            {
                std::sort(fanout.targetClusters.begin(), fanout.targetClusters.end());
            }
            for (auto &values : out.sourceValuesByCluster)
            {
                std::sort(values.begin(), values.end());
                values.erase(std::unique(values.begin(), values.end()), values.end());
            }
            for (auto &values : out.targetValuesByCluster)
            {
                std::sort(values.begin(), values.end());
                values.erase(std::unique(values.begin(), values.end()), values.end());
            }
            for (auto &succs : out.commitSuccsByCluster)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            for (const auto &[packed, weight] : out.weights)
            {
                const uint32_t from = static_cast<uint32_t>(packed >> 32);
                const uint32_t to = static_cast<uint32_t>(packed & 0xffffffffu);
                if (from < out.outgoing.size())
                {
                    out.outgoing[from].push_back({to, weight});
                }
            }
            for (auto &edges : out.outgoing)
            {
                std::sort(edges.begin(),
                          edges.end(),
                          [](const auto &lhs, const auto &rhs)
                          {
                              if (lhs.second != rhs.second)
                              {
                                  return lhs.second > rhs.second;
                              }
                              return lhs.first < rhs.first;
                          });
            }
            return out;
        }

        void recordInitialComputeSupernodeStats(const NodeClusterView &view,
                                                const ClusterValueEdges &valueEdges,
                                                ComputeRewriteBuild &rewrite,
                                                const wolvrix::lib::grh::Graph &graph,
                                                const std::vector<uint32_t> &nodeOpSizes)
        {
            auto &stats = rewrite.stats;
            stats.initialComputeSupernodes = view.members.size();
            stats.initialComputeSupernodeOpsTotal = 0;
            stats.initialComputeSupernodeDagEdges = 0;
            stats.initialBoundaryValues = 0;
            stats.initialBoundaryActivationEdges = 0;
            stats.initialComputeComputeValuePairs = 0;
            stats.initialComputeCommitValuePairs = 0;
            for (const auto &members : view.members)
            {
                stats.initialComputeSupernodeOpsTotal += clusterOpSize(members, nodeOpSizes);
            }

            std::unordered_set<uint64_t> dagEdges;
            dagEdges.reserve(valueEdges.weights.size() + rewrite.commitNodes.size());
            std::unordered_set<std::size_t> boundaryValues;
            boundaryValues.reserve(valueEdges.valueFanouts.size() + rewrite.commitNodes.size());

            auto noteComputeFanout = [&](wolvrix::lib::grh::ValueId value, uint32_t from, uint32_t to) {
                if (!value.valid() || from == kInvalidActivitySupernodeId ||
                    to == kInvalidActivitySupernodeId || from == to)
                {
                    return;
                }
                boundaryValues.insert(value.index);
                dagEdges.insert(packClusterPair(from, to));
                ++stats.initialBoundaryActivationEdges;
                ++stats.initialComputeComputeValuePairs;
            };

            for (std::size_t valueFanoutId = 0; valueFanoutId < valueEdges.valueFanouts.size(); ++valueFanoutId)
            {
                if (valueFanoutId >= valueEdges.fanoutValues.size())
                {
                    continue;
                }
                const auto value = valueEdges.fanoutValues[valueFanoutId];
                const auto &fanout = valueEdges.valueFanouts[valueFanoutId];
                for (const uint32_t to : fanout.targetClusters)
                {
                    noteComputeFanout(value, fanout.sourceCluster, to);
                }
            }

            const uint32_t commitBase = static_cast<uint32_t>(view.members.size());
            for (uint32_t commitId = 0; commitId < rewrite.commitNodes.size(); ++commitId)
            {
                const auto &commit = rewrite.commitNodes[commitId];
                for (const auto input : commit.inputValues)
                {
                    const auto defOp = graph.valueDef(input);
                    if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                    if (predNode == kInvalidActivitySupernodeId ||
                        predNode >= view.clusterOfNode.size())
                    {
                        continue;
                    }
                    const uint32_t from = view.clusterOfNode[predNode];
                    if (from == kInvalidActivitySupernodeId)
                    {
                        continue;
                    }
                    boundaryValues.insert(input.index);
                    dagEdges.insert(packClusterPair(from, commitBase + commitId));
                    ++stats.initialBoundaryActivationEdges;
                    ++stats.initialComputeCommitValuePairs;
                }
            }

            stats.initialComputeSupernodeDagEdges = dagEdges.size();
            stats.initialBoundaryValues = boundaryValues.size();
        }

        ComputeNodeMaterializePerfStats::CbawStageStats
        buildCbawStageStatsForView(const NodeClusterView &view,
                                   const ClusterValueEdges &valueEdges,
                                   const ComputeRewriteBuild &rewrite,
                                   const wolvrix::lib::grh::Graph &graph,
                                   const std::vector<uint32_t> &nodeOpSizes,
                                   std::size_t segmentCount)
        {
            ComputeNodeMaterializePerfStats::CbawStageStats stats;
            stats.clusterCount = view.members.size();
            stats.segmentCount = segmentCount;
            stats.computeSupernodeCount = view.members.size();

            std::vector<std::size_t> opCounts;
            opCounts.reserve(view.members.size());
            for (const auto &members : view.members)
            {
                opCounts.push_back(clusterOpSize(members, nodeOpSizes));
            }
            std::sort(opCounts.begin(), opCounts.end());
            stats.opCountP50 = percentileFromSorted(opCounts, 50, 100);
            stats.opCountP90 = percentileFromSorted(opCounts, 90, 100);
            stats.opCountP99 = percentileFromSorted(opCounts, 99, 100);
            stats.opCountMax = opCounts.empty() ? 0 : opCounts.back();

            std::unordered_set<std::uint64_t> dagEdges;
            dagEdges.reserve(valueEdges.weights.size() + rewrite.commitNodes.size());
            for (const auto &[packed, weight] : valueEdges.weights)
            {
                if (weight == 0)
                {
                    continue;
                }
                dagEdges.insert(packed);
                stats.boundaryActivationEdges += weight;
                stats.computeComputeValuePairs += weight;
            }

            const uint32_t commitBase = static_cast<uint32_t>(view.members.size());
            for (uint32_t commitId = 0; commitId < rewrite.commitNodes.size(); ++commitId)
            {
                const auto &commit = rewrite.commitNodes[commitId];
                for (const auto input : commit.inputValues)
                {
                    const auto defOp = graph.valueDef(input);
                    if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                    if (predNode == kInvalidActivitySupernodeId ||
                        predNode >= view.clusterOfNode.size())
                    {
                        continue;
                    }
                    const uint32_t from = view.clusterOfNode[predNode];
                    if (from == kInvalidActivitySupernodeId)
                    {
                        continue;
                    }
                    dagEdges.insert(packClusterPair(from, commitBase + commitId));
                    ++stats.boundaryActivationEdges;
                }
            }
            stats.dagEdges = dagEdges.size();
            return stats;
        }

        ComputeNodeMaterializePerfStats::CbawStageStats
        buildCbawStageStatsForComputeSupernodes(
            const std::vector<std::vector<uint32_t>> &computeSupernodes,
            const std::vector<std::vector<uint32_t>> &nodeDag,
            std::size_t nodeCount,
            const ComputeRewriteBuild &rewrite,
            const wolvrix::lib::grh::Graph &graph,
            const std::vector<uint32_t> &nodeOpSizes,
            std::size_t clusterCount,
            std::size_t segmentCount)
        {
            const NodeClusterView stageView =
                buildNodeClusterView(computeSupernodes, nodeDag, nodeCount);
            const ClusterValueEdges stageValueEdges =
                buildClusterValueEdges(stageView, rewrite, graph);
            auto stats = buildCbawStageStatsForView(stageView,
                                                    stageValueEdges,
                                                    rewrite,
                                                    graph,
                                                    nodeOpSizes,
                                                    segmentCount);
            stats.clusterCount = clusterCount;
            stats.computeSupernodeCount = computeSupernodes.size();
            return stats;
        }

        std::vector<uint32_t> cbawOwnerByClusterFromSegments(
            std::size_t clusterCount,
            const std::vector<std::vector<uint32_t>> &segments)
        {
            std::vector<uint32_t> owner(clusterCount, kInvalidActivitySupernodeId);
            for (uint32_t segmentId = 0; segmentId < segments.size(); ++segmentId)
            {
                for (const uint32_t clusterId : segments[segmentId])
                {
                    if (clusterId < owner.size())
                    {
                        owner[clusterId] = segmentId;
                    }
                }
            }
            return owner;
        }

        std::vector<uint32_t> cbawIdentityOwnerByCluster(std::size_t clusterCount)
        {
            std::vector<uint32_t> owner(clusterCount, kInvalidActivitySupernodeId);
            for (uint32_t clusterId = 0; clusterId < clusterCount; ++clusterId)
            {
                owner[clusterId] = clusterId;
            }
            return owner;
        }

        std::vector<ComputeNodeMaterializePerfStats::CbawRootStageEntry>
        buildCbawRootStageEntriesForOwners(
            const NodeClusterView &view,
            const ClusterValueEdges &valueEdges,
            const ComputeRewriteBuild &rewrite,
            const wolvrix::lib::grh::Graph &graph,
            const std::vector<uint32_t> &ownerByCluster)
        {
            using Entry = ComputeNodeMaterializePerfStats::CbawRootStageEntry;
            std::unordered_map<std::size_t, Entry> byValue;
            byValue.reserve(valueEdges.valueFanouts.size() + rewrite.commitNodes.size());

            const auto ownerOfCluster = [&](uint32_t clusterId) {
                return clusterId < ownerByCluster.size() ? ownerByCluster[clusterId]
                                                         : kInvalidActivitySupernodeId;
            };
            const auto ensureEntry = [&](wolvrix::lib::grh::ValueId value,
                                         uint32_t sourceCluster) -> Entry & {
                auto [it, inserted] = byValue.emplace(value.index, Entry{});
                auto &entry = it->second;
                if (inserted || entry.valueIndex == 0)
                {
                    entry.valueIndex = value.index;
                    entry.sourceCluster = sourceCluster;
                    entry.sourceSegment = ownerOfCluster(sourceCluster);
                }
                return entry;
            };

            std::vector<uint32_t> targetSegments;
            for (uint32_t valueFanoutId = 0; valueFanoutId < valueEdges.valueFanouts.size(); ++valueFanoutId)
            {
                if (valueFanoutId >= valueEdges.fanoutValues.size())
                {
                    continue;
                }
                const auto value = valueEdges.fanoutValues[valueFanoutId];
                if (!value.valid() || value.index == 0)
                {
                    continue;
                }
                const auto &fanout = valueEdges.valueFanouts[valueFanoutId];
                const uint32_t sourceSegment = ownerOfCluster(fanout.sourceCluster);
                if (sourceSegment == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                targetSegments.clear();
                targetSegments.reserve(fanout.targetClusters.size());
                for (const uint32_t targetCluster : fanout.targetClusters)
                {
                    const uint32_t targetSegment = ownerOfCluster(targetCluster);
                    if (targetSegment == kInvalidActivitySupernodeId ||
                        targetSegment == sourceSegment)
                    {
                        continue;
                    }
                    targetSegments.push_back(targetSegment);
                }
                if (targetSegments.empty())
                {
                    continue;
                }
                std::sort(targetSegments.begin(), targetSegments.end());
                targetSegments.erase(std::unique(targetSegments.begin(), targetSegments.end()),
                                     targetSegments.end());
                auto &entry = ensureEntry(value, fanout.sourceCluster);
                entry.computeTargetCount += targetSegments.size();
                entry.targetSegmentCount += targetSegments.size();
            }

            std::unordered_map<std::size_t, std::vector<uint32_t>> commitTargetsByValue;
            commitTargetsByValue.reserve(rewrite.commitNodes.size());
            for (uint32_t commitId = 0; commitId < rewrite.commitNodes.size(); ++commitId)
            {
                const auto &commit = rewrite.commitNodes[commitId];
                for (const auto input : commit.inputValues)
                {
                    if (!input.valid() || input.index == 0)
                    {
                        continue;
                    }
                    const auto defOp = graph.valueDef(input);
                    if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                    if (predNode == kInvalidActivitySupernodeId ||
                        predNode >= view.clusterOfNode.size())
                    {
                        continue;
                    }
                    const uint32_t sourceCluster = view.clusterOfNode[predNode];
                    if (sourceCluster == kInvalidActivitySupernodeId ||
                        ownerOfCluster(sourceCluster) == kInvalidActivitySupernodeId)
                    {
                        continue;
                    }
                    (void)ensureEntry(input, sourceCluster);
                    commitTargetsByValue[input.index].push_back(commitId);
                }
            }
            for (auto &[valueIndex, targets] : commitTargetsByValue)
            {
                auto it = byValue.find(valueIndex);
                if (it == byValue.end())
                {
                    continue;
                }
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                it->second.commitTargetCount += targets.size();
            }

            std::vector<Entry> out;
            out.reserve(byValue.size());
            for (auto &[_, entry] : byValue)
            {
                entry.targetCount = entry.computeTargetCount + entry.commitTargetCount;
                if (entry.targetCount != 0)
                {
                    out.push_back(entry);
                }
            }
            std::sort(out.begin(),
                      out.end(),
                      [](const Entry &lhs, const Entry &rhs)
                      {
                          return lhs.valueIndex < rhs.valueIndex;
                      });
            return out;
        }

        struct CbawDiagnosticAtomInfo
        {
            std::vector<uint32_t> opToAtom;
            std::vector<std::string> atomTags;
            std::vector<std::vector<uint32_t>> computeNodeToAtoms;
        };

        std::string joinCbawDiagnosticTags(std::vector<std::string> tags)
        {
            if (tags.empty())
            {
                return "plain";
            }
            std::sort(tags.begin(), tags.end());
            tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
            std::ostringstream oss;
            for (std::size_t i = 0; i < tags.size(); ++i)
            {
                if (i != 0)
                {
                    oss << "|";
                }
                oss << tags[i];
            }
            return oss.str();
        }

        CbawDiagnosticAtomInfo buildCbawDiagnosticAtomInfo(
            const ActivityScheduleBuild &build,
            const ComputeRewriteBuild &rewrite,
            const wolvrix::lib::grh::Graph &graph)
        {
            CbawDiagnosticAtomInfo info;
            info.computeNodeToAtoms.resize(rewrite.computeNodes.size());

            std::size_t maxOpIndex = build.opToSupernode.size();
            for (const auto opId : graph.operations())
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            info.opToAtom.assign(maxOpIndex + 1, kInvalidActivitySupernodeId);

            const auto addAtom = [&](const std::vector<wolvrix::lib::grh::OperationId> &ops,
                                     const std::vector<uint32_t> &computeNodes)
            {
                if (ops.empty())
                {
                    return;
                }
                bool rtmIntent = false;
                bool aggregate = false;
                bool guard = false;
                bool allPassthrough = true;
                for (const auto opId : ops)
                {
                    const auto op = graph.getOperation(opId);
                    const auto kind = op.kind();
                    rtmIntent = rtmIntent || activityScheduleHasRegToMemIntent(op);
                    aggregate = aggregate || activityScheduleIsAggregateShapeKind(kind);
                    guard = guard || activityScheduleIsGuardLikeKind(kind);
                    allPassthrough = allPassthrough && activityScheduleIsPassthroughKind(kind);
                }

                std::vector<std::string> tags;
                if (rtmIntent)
                {
                    tags.push_back("rtm_intent");
                }
                if (computeNodes.size() == 1)
                {
                    tags.push_back("mffc");
                }
                if (allPassthrough)
                {
                    tags.push_back("passthrough");
                }
                if (aggregate)
                {
                    tags.push_back("aggregate");
                }
                if (guard)
                {
                    tags.push_back("guard");
                }
                const uint32_t atomId = static_cast<uint32_t>(info.atomTags.size());
                info.atomTags.push_back(joinCbawDiagnosticTags(std::move(tags)));
                for (const auto opId : ops)
                {
                    if (opId.index < info.opToAtom.size())
                    {
                        info.opToAtom[opId.index] = atomId;
                    }
                }
                for (const uint32_t computeNodeId : computeNodes)
                {
                    if (computeNodeId < info.computeNodeToAtoms.size())
                    {
                        info.computeNodeToAtoms[computeNodeId].push_back(atomId);
                    }
                }
            };

            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId >= build.supernodeKinds.size() ||
                    build.supernodeKinds[supernodeId] != ActivityScheduleSupernodeKind::Compute)
                {
                    continue;
                }
                std::vector<uint8_t> assigned(build.supernodeToOps[supernodeId].size(), 0);
                const std::vector<uint32_t> memberComputeNodes =
                    supernodeId < build.computeNodesBySupernode.size()
                        ? build.computeNodesBySupernode[supernodeId]
                        : std::vector<uint32_t>{};
                if (memberComputeNodes.empty())
                {
                    addAtom(build.supernodeToOps[supernodeId], {});
                    continue;
                }
                for (const uint32_t computeNodeId : memberComputeNodes)
                {
                    if (computeNodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    std::vector<wolvrix::lib::grh::OperationId> atomOps;
                    for (const auto opId : rewrite.computeNodes[computeNodeId].ops)
                    {
                        if (opId.index == 0 || opId.index - 1 >= build.opToSupernode.size() ||
                            build.opToSupernode[opId.index - 1] != supernodeId)
                        {
                            continue;
                        }
                        atomOps.push_back(opId);
                        const auto posIt = std::find(build.supernodeToOps[supernodeId].begin(),
                                                     build.supernodeToOps[supernodeId].end(),
                                                     opId);
                        if (posIt != build.supernodeToOps[supernodeId].end())
                        {
                            assigned[static_cast<std::size_t>(
                                std::distance(build.supernodeToOps[supernodeId].begin(), posIt))] = 1;
                        }
                    }
                    addAtom(atomOps, {computeNodeId});
                }
                std::vector<wolvrix::lib::grh::OperationId> residualOps;
                for (std::size_t i = 0; i < build.supernodeToOps[supernodeId].size(); ++i)
                {
                    if (assigned[i] == 0)
                    {
                        residualOps.push_back(build.supernodeToOps[supernodeId][i]);
                    }
                }
                addAtom(residualOps, {});
            }

            for (auto &atoms : info.computeNodeToAtoms)
            {
                std::sort(atoms.begin(), atoms.end());
                atoms.erase(std::unique(atoms.begin(), atoms.end()), atoms.end());
            }
            return info;
        }

        struct CbawFinalRootEntry
        {
            std::size_t targetCount = 0;
            std::size_t computeTargetCount = 0;
            std::size_t commitTargetCount = 0;
            std::size_t consumerUseCount = 0;
        };

        std::unordered_map<std::size_t, CbawFinalRootEntry>
        buildCbawFinalRootEntries(const ActivityScheduleBuild &build,
                                  const ComputeRewriteBuild &rewrite,
                                  const wolvrix::lib::grh::Graph &graph)
        {
            std::unordered_map<std::size_t, CbawFinalRootEntry> roots;
            roots.reserve(build.valueFanout.size());
            for (std::size_t valueOffset = 0; valueOffset < build.valueFanout.size(); ++valueOffset)
            {
                const auto &targets = build.valueFanout[valueOffset];
                if (targets.empty())
                {
                    continue;
                }
                auto &entry = roots[valueOffset + 1];
                entry.targetCount = targets.size();
                for (const uint32_t target : targets)
                {
                    if (target < build.supernodeKinds.size() &&
                        build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++entry.computeTargetCount;
                    }
                    else if (target < build.supernodeKinds.size() &&
                             build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Commit)
                    {
                        ++entry.commitTargetCount;
                    }
                }
            }

            const auto noteUse = [&](wolvrix::lib::grh::ValueId value, uint32_t targetSupernode)
            {
                if (!value.valid() || value.index == 0 || targetSupernode >= build.supernodeToOps.size())
                {
                    return;
                }
                const auto canonical = canonicalActivityValue(value, &rewrite.canonicalValues);
                if (!canonical.valid())
                {
                    return;
                }
                const auto defOp = graph.valueDef(canonical);
                if (!defOp.valid() || defOp.index == 0 ||
                    defOp.index - 1 >= build.opToSupernode.size())
                {
                    return;
                }
                const uint32_t sourceSupernode = build.opToSupernode[defOp.index - 1];
                if (sourceSupernode == targetSupernode)
                {
                    return;
                }
                ++roots[canonical.index].consumerUseCount;
            };
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto op = graph.getOperation(opId);
                    for (const auto operand : op.operands())
                    {
                        noteUse(operand, supernodeId);
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            noteUse(*indexValue, supernodeId);
                        }
                    }
                }
            }
            return roots;
        }

        std::unordered_map<std::size_t, ComputeNodeMaterializePerfStats::CbawRootStageEntry>
        mapCbawRootStageEntries(
            const std::vector<ComputeNodeMaterializePerfStats::CbawRootStageEntry> &entries)
        {
            std::unordered_map<std::size_t, ComputeNodeMaterializePerfStats::CbawRootStageEntry> out;
            out.reserve(entries.size());
            for (const auto &entry : entries)
            {
                out[entry.valueIndex] = entry;
            }
            return out;
        }

        void attachCbawTopMultiplicityDiagnostics(
            ActivityScheduleCbawStats &stats,
            const ComputeNodeMaterializePerfStats &perf,
            const ActivityScheduleBuild &build,
            const ComputeRewriteBuild &rewrite,
            const wolvrix::lib::grh::Graph &graph)
        {
            using StageEntry = ComputeNodeMaterializePerfStats::CbawRootStageEntry;
            const auto afterP5 = mapCbawRootStageEntries(perf.cbawAfterCoarsenRootStages);
            const auto afterDp = mapCbawRootStageEntries(perf.cbawAfterDpRootStages);
            const auto afterFm = mapCbawRootStageEntries(perf.cbawAfterFmRootStages);
            const auto finalRoots = buildCbawFinalRootEntries(build, rewrite, graph);
            const CbawDiagnosticAtomInfo atomInfo =
                buildCbawDiagnosticAtomInfo(build, rewrite, graph);

            const auto signedDiff = [](std::size_t lhs, std::size_t rhs) {
                return static_cast<std::int64_t>(lhs) -
                       static_cast<std::int64_t>(rhs);
            };
            stats.topRootStageDeltas.push_back({
                "after_p5_to_after_dp",
                signedDiff(perf.cbawAfterDp.boundaryActivationEdges,
                           perf.cbawAfterCoarsen.boundaryActivationEdges),
                signedDiff(perf.cbawAfterDp.computeComputeValuePairs,
                           perf.cbawAfterCoarsen.computeComputeValuePairs),
            });
            stats.topRootStageDeltas.push_back({
                "after_dp_to_after_fm",
                signedDiff(perf.cbawAfterFm.boundaryActivationEdges,
                           perf.cbawAfterDp.boundaryActivationEdges),
                signedDiff(perf.cbawAfterFm.computeComputeValuePairs,
                           perf.cbawAfterDp.computeComputeValuePairs),
            });
            stats.topRootStageDeltas.push_back({
                "after_fm_to_final_replay",
                signedDiff(stats.crossBoundaryTargetCount,
                           perf.cbawAfterFm.boundaryActivationEdges),
                signedDiff(stats.computeMaterializedValueTargetCount,
                           perf.cbawAfterFm.computeComputeValuePairs),
            });

            std::unordered_map<std::size_t, std::string> rankReasons;
            const auto appendReason = [&](std::size_t valueIndex, std::string_view reason)
            {
                auto &slot = rankReasons[valueIndex];
                if (slot.find(reason) != std::string::npos)
                {
                    return;
                }
                if (!slot.empty())
                {
                    slot += "|";
                }
                slot += reason;
            };
            constexpr std::size_t kPerKeyLimit = 128;
            const auto selectTopFromStage =
                [&](const std::unordered_map<std::size_t, StageEntry> &entries,
                    std::string_view reason,
                    const auto &metric)
            {
                std::vector<const StageEntry *> ordered;
                ordered.reserve(entries.size());
                for (const auto &[_, entry] : entries)
                {
                    if (metric(entry) != 0)
                    {
                        ordered.push_back(&entry);
                    }
                }
                std::sort(ordered.begin(),
                          ordered.end(),
                          [&](const StageEntry *lhs, const StageEntry *rhs)
                          {
                              const std::size_t lhsMetric = metric(*lhs);
                              const std::size_t rhsMetric = metric(*rhs);
                              if (lhsMetric != rhsMetric)
                              {
                                  return lhsMetric > rhsMetric;
                              }
                              return lhs->valueIndex < rhs->valueIndex;
                          });
                const std::size_t n = std::min<std::size_t>(kPerKeyLimit, ordered.size());
                for (std::size_t i = 0; i < n; ++i)
                {
                    appendReason(ordered[i]->valueIndex, reason);
                }
            };
            selectTopFromStage(afterP5,
                               "after_p5",
                               [](const StageEntry &entry) { return entry.targetCount; });
            selectTopFromStage(afterDp,
                               "after_dp",
                               [](const StageEntry &entry) { return entry.targetCount; });
            selectTopFromStage(afterFm,
                               "after_fm",
                               [](const StageEntry &entry) { return entry.targetCount; });
            selectTopFromStage(afterDp,
                               "dp_to_fm_repair",
                               [&](const StageEntry &entry) {
                                   const auto fmIt = afterFm.find(entry.valueIndex);
                                   const std::size_t fmTargets =
                                       fmIt == afterFm.end() ? 0 : fmIt->second.targetCount;
                                   return entry.targetCount > fmTargets ? entry.targetCount - fmTargets
                                                                        : std::size_t{0};
                               });

            std::vector<std::size_t> computeCandidates;
            computeCandidates.reserve(afterP5.size() + finalRoots.size());
            std::unordered_set<std::size_t> seenCandidates;
            const auto addCandidateValue = [&](std::size_t valueIndex)
            {
                if (seenCandidates.insert(valueIndex).second)
                {
                    computeCandidates.push_back(valueIndex);
                }
            };
            for (const auto &[valueIndex, _] : afterP5)
            {
                addCandidateValue(valueIndex);
            }
            for (const auto &[valueIndex, _] : afterDp)
            {
                addCandidateValue(valueIndex);
            }
            for (const auto &[valueIndex, _] : afterFm)
            {
                addCandidateValue(valueIndex);
            }
            for (const auto &[valueIndex, _] : finalRoots)
            {
                addCandidateValue(valueIndex);
            }
            const auto stageComputeTargets =
                [&](const std::unordered_map<std::size_t, StageEntry> &entries,
                    std::size_t valueIndex) {
                    const auto it = entries.find(valueIndex);
                    return it == entries.end() ? std::size_t{0} : it->second.computeTargetCount;
                };
            const auto finalComputeTargets = [&](std::size_t valueIndex) {
                const auto it = finalRoots.find(valueIndex);
                return it == finalRoots.end() ? std::size_t{0} : it->second.computeTargetCount;
            };
            const auto computeMetric = [&](std::size_t valueIndex) {
                return std::max({stageComputeTargets(afterP5, valueIndex),
                                 stageComputeTargets(afterDp, valueIndex),
                                 stageComputeTargets(afterFm, valueIndex),
                                 finalComputeTargets(valueIndex)});
            };
            std::sort(computeCandidates.begin(),
                      computeCandidates.end(),
                      [&](std::size_t lhs, std::size_t rhs)
                      {
                          const std::size_t lhsMetric = computeMetric(lhs);
                          const std::size_t rhsMetric = computeMetric(rhs);
                          if (lhsMetric != rhsMetric)
                          {
                              return lhsMetric > rhsMetric;
                          }
                          return lhs < rhs;
                      });
            for (std::size_t i = 0;
                 i < computeCandidates.size() && i < kPerKeyLimit && computeMetric(computeCandidates[i]) != 0;
                 ++i)
            {
                appendReason(computeCandidates[i], "compute_targets");
            }

            std::vector<uint32_t> finalSegmentsByComputeNode(rewrite.computeNodes.size(), 0);
            for (const auto &nodes : build.computeNodesBySupernode)
            {
                for (const uint32_t computeNodeId : nodes)
                {
                    if (computeNodeId < finalSegmentsByComputeNode.size())
                    {
                        ++finalSegmentsByComputeNode[computeNodeId];
                    }
                }
            }

            const auto stageOrEmpty =
                [](const std::unordered_map<std::size_t, StageEntry> &entries,
                   std::size_t valueIndex) -> StageEntry {
                const auto it = entries.find(valueIndex);
                return it == entries.end() ? StageEntry{} : it->second;
            };
            const auto finalOrEmpty =
                [&](std::size_t valueIndex) -> CbawFinalRootEntry {
                const auto it = finalRoots.find(valueIndex);
                return it == finalRoots.end() ? CbawFinalRootEntry{} : it->second;
            };

            std::vector<ActivityScheduleCbawStats::TopRootStageReport> reports;
            reports.reserve(rankReasons.size());
            for (const auto &[valueIndex, reasons] : rankReasons)
            {
                const StageEntry p5 = stageOrEmpty(afterP5, valueIndex);
                const StageEntry dp = stageOrEmpty(afterDp, valueIndex);
                const StageEntry fm = stageOrEmpty(afterFm, valueIndex);
                const CbawFinalRootEntry finalEntry = finalOrEmpty(valueIndex);

                ActivityScheduleCbawStats::TopRootStageReport report;
                report.valueIndex = valueIndex;
                report.rankReasons = reasons;
                report.afterP5TargetCount = p5.targetCount;
                report.afterP5ComputeTargetCount = p5.computeTargetCount;
                report.afterP5CommitTargetCount = p5.commitTargetCount;
                report.afterP5SourceCluster = p5.sourceCluster;
                report.afterP5SourceSegment = p5.sourceSegment;
                report.afterP5TargetSegmentCount = p5.targetSegmentCount;
                report.afterDpTargetCount = dp.targetCount;
                report.afterDpComputeTargetCount = dp.computeTargetCount;
                report.afterDpCommitTargetCount = dp.commitTargetCount;
                report.afterDpSourceCluster = dp.sourceCluster;
                report.afterDpSourceSegment = dp.sourceSegment;
                report.afterDpTargetSegmentCount = dp.targetSegmentCount;
                report.afterFmTargetCount = fm.targetCount;
                report.afterFmComputeTargetCount = fm.computeTargetCount;
                report.afterFmCommitTargetCount = fm.commitTargetCount;
                report.afterFmSourceCluster = fm.sourceCluster;
                report.afterFmSourceSegment = fm.sourceSegment;
                report.afterFmTargetSegmentCount = fm.targetSegmentCount;
                report.finalTargetCount = finalEntry.targetCount;
                report.finalComputeTargetCount = finalEntry.computeTargetCount;
                report.finalCommitTargetCount = finalEntry.commitTargetCount;
                report.deltaP5ToDp = signedDiff(dp.targetCount, p5.targetCount);
                report.deltaDpToFm = signedDiff(fm.targetCount, dp.targetCount);
                report.finalReplayDelta = signedDiff(finalEntry.targetCount, fm.targetCount);
                report.dpToFmRepair =
                    dp.targetCount > fm.targetCount ? dp.targetCount - fm.targetCount : 0;
                report.computeUserCount = finalEntry.computeTargetCount;
                report.externalTargetCount =
                    finalEntry.targetCount != 0 ? finalEntry.targetCount : fm.targetCount;

                if (valueIndex != 0 && valueIndex <= graph.values().size())
                {
                    wolvrix::lib::grh::ValueId value;
                    value.graph = graph.id();
                    value.index = static_cast<uint32_t>(valueIndex);
                    const int32_t width = graph.valueWidth(value);
                    report.valueWidth = width > 0 ? static_cast<std::size_t>(width) : 0;
                    report.widthBucket = widthBucket(report.valueWidth);
                    report.valueBytes =
                        static_cast<std::uint64_t>(activityScheduleValueByteCost(graph, valueIndex));
                    const auto defOp = graph.valueDef(value);
                    if (defOp.valid())
                    {
                        const auto kind = graph.opKind(defOp);
                        report.definingOpKind = std::string(wolvrix::lib::grh::toString(kind));
                        report.sourceKind = activityScheduleCbawSourceKindName(kind);
                        if (defOp.index < rewrite.computeNodeOfOp.size())
                        {
                            report.producerComputeNodeId = rewrite.computeNodeOfOp[defOp.index];
                        }
                        if (defOp.index > 0 && defOp.index - 1 < build.opToSupernode.size())
                        {
                            report.producerSupernodeId = build.opToSupernode[defOp.index - 1];
                        }
                        if (defOp.index < atomInfo.opToAtom.size())
                        {
                            report.producerAtomId = atomInfo.opToAtom[defOp.index];
                        }
                    }
                }
                if (report.definingOpKind.empty())
                {
                    report.definingOpKind = "unknown";
                }
                if (report.sourceKind.empty())
                {
                    report.sourceKind = "unknown";
                }
                if (report.widthBucket.empty())
                {
                    report.widthBucket = "unknown";
                }
                if (report.producerAtomId != kInvalidActivitySupernodeId &&
                    report.producerAtomId < atomInfo.atomTags.size())
                {
                    report.semanticTags = atomInfo.atomTags[report.producerAtomId];
                }
                else
                {
                    report.semanticTags = "plain";
                }

                if (report.producerComputeNodeId != kInvalidActivitySupernodeId &&
                    report.producerComputeNodeId < rewrite.computeNodes.size())
                {
                    const auto &node = rewrite.computeNodes[report.producerComputeNodeId];
                    report.computeNodeOpCount = node.ops.size();
                    for (const auto opId : node.ops)
                    {
                        const auto kind = graph.opKind(opId);
                        if (activityScheduleCbawSourceKindName(kind) == "compute_like")
                        {
                            ++report.computeLikeOpCount;
                        }
                        else
                        {
                            ++report.sourceCloneCount;
                        }
                    }
                    if (report.producerComputeNodeId < atomInfo.computeNodeToAtoms.size())
                    {
                        report.splitAtomCount =
                            atomInfo.computeNodeToAtoms[report.producerComputeNodeId].size();
                    }
                    if (report.producerComputeNodeId < finalSegmentsByComputeNode.size())
                    {
                        report.splitSegmentCount =
                            finalSegmentsByComputeNode[report.producerComputeNodeId];
                    }
                }

                const std::size_t maxTargets =
                    std::max({report.afterP5TargetCount,
                              report.afterDpTargetCount,
                              report.afterFmTargetCount,
                              report.finalTargetCount});
                const std::size_t sharedUses =
                    std::max(finalEntry.consumerUseCount, maxTargets);
                report.fanoutBucket = fanoutBucket(maxTargets);
                report.sharedSourceBucket = fanoutBucket(sharedUses);
                report.highFanoutBucket = maxTargets > 16;
                report.sharedSourceBucketHit = finalEntry.consumerUseCount > maxTargets;
                reports.push_back(std::move(report));
            }

            std::sort(reports.begin(),
                      reports.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.afterDpTargetCount != rhs.afterDpTargetCount)
                          {
                              return lhs.afterDpTargetCount > rhs.afterDpTargetCount;
                          }
                          if (lhs.dpToFmRepair != rhs.dpToFmRepair)
                          {
                              return lhs.dpToFmRepair > rhs.dpToFmRepair;
                          }
                          if (lhs.afterP5TargetCount != rhs.afterP5TargetCount)
                          {
                              return lhs.afterP5TargetCount > rhs.afterP5TargetCount;
                          }
                          if (lhs.afterFmTargetCount != rhs.afterFmTargetCount)
                          {
                              return lhs.afterFmTargetCount > rhs.afterFmTargetCount;
                          }
                          if (lhs.finalComputeTargetCount != rhs.finalComputeTargetCount)
                          {
                              return lhs.finalComputeTargetCount > rhs.finalComputeTargetCount;
                          }
                          return lhs.valueIndex < rhs.valueIndex;
                      });
            constexpr std::size_t kReportLimit = 512;
            if (reports.size() > kReportLimit)
            {
                reports.resize(kReportLimit);
            }

            stats.cbawTopRootReportLimit = reports.size();
            for (const auto &[_, entry] : afterDp)
            {
                stats.cbawTopRootAfterDpTotalTargets += entry.targetCount;
            }
            for (const auto &report : reports)
            {
                stats.cbawTopRootAfterDpReportedTargets += report.afterDpTargetCount;
            }
            stats.cbawTopRootAfterDpCoveragePpm =
                activityScheduleRatioPpm(stats.cbawTopRootAfterDpReportedTargets,
                                         stats.cbawTopRootAfterDpTotalTargets);
            stats.topRootStageReports = std::move(reports);
        }

        // NO0207 Phase C：computeNode/cluster 层的概率超图聚合。这里只构建只读分析结构，
        // 不改变 plain/prob 当前 materialize 路径；Phase E/F 会复用这些 W/pi/edge 数据。
        struct ActivityHyperedge
        {
            std::uint64_t count = 0;
            double totalProb = 0.0;
        };

        struct ActivityHypergraphAggregate
        {
            std::vector<double> nodeWeight;
            std::vector<double> nodeChangeWeight;
            std::vector<std::uint64_t> nodeFootprintBytes;
            std::vector<double> nodeActiveProb;
            std::vector<uint32_t> nodeMinTopo;
            std::vector<uint32_t> nodeMaxTopo;
            std::vector<uint32_t> nodeOpCount;
            std::vector<uint32_t> edgeFrom;
            std::vector<uint32_t> edgeTo;
            std::vector<std::uint64_t> edgeCount;
            std::vector<double> edgeTotalProb;
            std::uint64_t boundaryValues = 0;
            std::uint64_t externalBoundaryValues = 0;
            std::uint64_t canonicalCloneValues = 0;
        };

        struct ProbCoarsenClusterAggregate
        {
            ActivityHypergraphAggregate aggregate;
            std::unordered_map<std::uint64_t, double> edgeProb;
        };

        struct ActivityPairValueKey
        {
            std::uint64_t pair = 0;
            wolvrix::lib::grh::ValueId value;

            bool operator==(const ActivityPairValueKey &other) const noexcept
            {
                return pair == other.pair && value == other.value;
            }
        };

        struct ActivityPairValueKeyHash
        {
            std::size_t operator()(const ActivityPairValueKey &key) const noexcept
            {
                const auto valueHash = wolvrix::lib::grh::ValueIdHash{}(key.value);
                return static_cast<std::size_t>(key.pair ^ (key.pair >> 32)) ^
                       (valueHash + 0x9e3779b97f4a7c15ull + (valueHash << 6) + (valueHash >> 2));
            }
        };

        struct ActivityTargetValueKey
        {
            uint32_t target = 0;
            wolvrix::lib::grh::ValueId value;

            bool operator==(const ActivityTargetValueKey &other) const noexcept
            {
                return target == other.target && value == other.value;
            }
        };

        struct ActivityTargetValueKeyHash
        {
            std::size_t operator()(const ActivityTargetValueKey &key) const noexcept
            {
                const auto valueHash = wolvrix::lib::grh::ValueIdHash{}(key.value);
                return static_cast<std::size_t>(key.target) ^
                       (valueHash + 0x9e3779b97f4a7c15ull + (valueHash << 6) + (valueHash >> 2));
            }
        };

        wolvrix::lib::grh::OperationId activityCanonicalDataOpForOp(
            const wolvrix::lib::grh::Graph &graph,
            wolvrix::lib::grh::OperationId opId,
            const ValueCanonicalMap &canonicalValues)
        {
            if (!opId.valid())
            {
                return opId;
            }
            for (const auto result : graph.opResults(opId))
            {
                const auto canonical = canonicalActivityValue(result, &canonicalValues);
                if (canonical.valid() && canonical != result)
                {
                    const auto canonicalDef = graph.valueDef(canonical);
                    if (canonicalDef.valid())
                    {
                        return canonicalDef;
                    }
                }
            }
            return opId;
        }

        double activityPiForOp(const wolvrix::lib::grh::Graph &graph,
                               const ActivityScheduleOptions &options,
                               const std::vector<float> &piByOpIndex,
                               wolvrix::lib::grh::OperationId opId)
        {
            if (opId.valid() && opId.index < piByOpIndex.size() && piByOpIndex[opId.index] >= 0.0F)
            {
                return std::clamp(static_cast<double>(piByOpIndex[opId.index]), 0.0, 1.0);
            }
            if (!opId.valid())
            {
                return std::clamp(options.piDataInput, 0.0, 1.0);
            }
            switch (graph.opKind(opId))
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
                return 0.0;
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                return std::clamp(options.piRegRead, 0.0, 1.0);
            default:
                return 0.0;
            }
        }

        double activityPiForValue(const wolvrix::lib::grh::Graph &graph,
                                  const ActivityScheduleOptions &options,
                                  const ValueCanonicalMap &canonicalValues,
                                  const std::vector<float> &piByOpIndex,
                                  wolvrix::lib::grh::ValueId value)
        {
            const auto canonical = canonicalActivityValue(value, &canonicalValues);
            const auto defOp = graph.valueDef(canonical);
            if (defOp.valid())
            {
                return activityPiForOp(graph, options, piByOpIndex, defOp);
            }
            return std::clamp(options.piDataInput, 0.0, 1.0);
        }

        double activityComputeWeightForOp(const wolvrix::lib::grh::Graph &graph,
                                          const ValueCanonicalMap &canonicalValues,
                                          const ActivityCostModel &costModel,
                                          wolvrix::lib::grh::OperationId opId)
        {
            const auto dataOp = activityCanonicalDataOpForOp(graph, opId, canonicalValues);
            if (dataOp.valid() &&
                dataOp.index < costModel.computeWeightByOpIndex.size() &&
                costModel.computeWeightByOpIndex[dataOp.index] >= 0.0)
            {
                return costModel.computeWeightByOpIndex[dataOp.index];
            }
            if (!dataOp.valid())
            {
                return 0.0;
            }
            const ActivityCostClass costClass = classifyActivityCostClass(graph.opKind(dataOp));
            return activityCostClassUnit(costClass) *
                   static_cast<double>(activityComputeUnitsForWidth(activityOpResultWidth(graph, dataOp)));
        }

        double activityChangeWeightForOp(const wolvrix::lib::grh::Graph &graph,
                                         const ActivityScheduleOptions &options,
                                         const ValueCanonicalMap &canonicalValues,
                                         const ActivityCostModel &costModel,
                                         const std::vector<float> &piByOpIndex,
                                         wolvrix::lib::grh::OperationId opId)
        {
            const auto dataOp = activityCanonicalDataOpForOp(graph, opId, canonicalValues);
            if (dataOp.valid() &&
                dataOp.index < costModel.changeWeightByOpIndex.size() &&
                costModel.changeWeightByOpIndex[dataOp.index] >= 0.0)
            {
                return costModel.changeWeightByOpIndex[dataOp.index];
            }
            return activityComputeWeightForOp(graph, canonicalValues, costModel, opId) *
                   activityPiForOp(graph, options, piByOpIndex, dataOp);
        }

        ActivityScheduleValueWeightStats buildActivityScheduleValueWeightStats(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ComputeRewriteBuild &rewrite,
            const ActivityCostModel &costModel,
            const std::vector<float> &piByOpIndex)
        {
            ActivityScheduleValueWeightStats out;
            if (graph.values().empty())
            {
                return out;
            }
            out.piByValueIndex.assign(graph.values().back().index + 1, 0.0);
            out.changeWeightByValueIndex.assign(graph.values().back().index + 1, 0.0);
            for (const auto valueId : graph.values())
            {
                if (!valueId.valid() || valueId.index >= out.piByValueIndex.size())
                {
                    continue;
                }
                const auto canonical = canonicalActivityValue(valueId, &rewrite.canonicalValues);
                const auto defOp = graph.valueDef(canonical);
                const double pi =
                    activityPiForValue(graph, options, rewrite.canonicalValues, piByOpIndex, valueId);
                double weight = 0.0;
                if (defOp.valid())
                {
                    weight = activityComputeWeightForOp(graph, rewrite.canonicalValues, costModel, defOp);
                }
                else
                {
                    weight = static_cast<double>(activityComputeUnitsForWidth(graph.valueWidth(canonical)));
                }
                out.piByValueIndex[valueId.index] = pi;
                out.changeWeightByValueIndex[valueId.index] = std::max(0.0, weight) * pi;
            }
            return out;
        }

        ActivityHypergraphAggregate buildActivityHypergraphAggregate(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const ComputeRewriteBuild &rewrite,
            const NodeClusterView &view,
            const ActivityCostModel &costModel,
            const std::vector<float> &piByOpIndex)
        {
            ActivityHypergraphAggregate out;
            const std::size_t clusters = view.members.size();
            out.nodeWeight.assign(clusters, 0.0);
            out.nodeChangeWeight.assign(clusters, 0.0);
            out.nodeFootprintBytes.assign(clusters, 0);
            out.nodeActiveProb.assign(clusters, 0.0);
            out.nodeMinTopo.assign(clusters, kInvalidActivitySupernodeId);
            out.nodeMaxTopo.assign(clusters, 0);
            out.nodeOpCount.assign(clusters, 0);

            std::vector<double> activeKeep(clusters, 1.0);
            std::unordered_set<ActivityTargetValueKey, ActivityTargetValueKeyHash> activeSeenValues;
            std::unordered_map<std::uint64_t, ActivityHyperedge> edgeMap;
            std::unordered_set<ActivityPairValueKey, ActivityPairValueKeyHash> seenEdgeValues;
            std::unordered_set<ActivityPairValueKey, ActivityPairValueKeyHash> seenEdgeProbValues;

            for (uint32_t clusterId = 0; clusterId < clusters; ++clusterId)
            {
                std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
                    footprintValues;
                for (const auto nodeId : view.members[clusterId])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    const auto &node = rewrite.computeNodes[nodeId];
                    for (const auto opId : node.ops)
                    {
                        ++out.nodeOpCount[clusterId];
                        const uint32_t topoPos = topoPosForOp(opData, opId);
                        if (topoPos != kInvalidActivitySupernodeId)
                        {
                            out.nodeMinTopo[clusterId] = std::min(out.nodeMinTopo[clusterId], topoPos);
                            out.nodeMaxTopo[clusterId] = std::max(out.nodeMaxTopo[clusterId], topoPos);
                        }

                        out.nodeWeight[clusterId] +=
                            activityComputeWeightForOp(graph, rewrite.canonicalValues, costModel, opId);
                        out.nodeChangeWeight[clusterId] +=
                            activityChangeWeightForOp(graph,
                                                      options,
                                                      rewrite.canonicalValues,
                                                      costModel,
                                                      piByOpIndex,
                                                      opId);

                        const auto dataOp = activityCanonicalDataOpForOp(graph, opId, rewrite.canonicalValues);
                        const double opPi = activityPiForOp(graph, options, piByOpIndex, dataOp);
                        if (dataOp.valid() &&
                            classifyActivityCostClass(graph.opKind(dataOp)) == ActivityCostClass::Src)
                        {
                            for (const auto result : graph.opResults(opId))
                            {
                                const auto canonical = canonicalActivityValue(result, &rewrite.canonicalValues);
                                ActivityTargetValueKey key{clusterId, canonical};
                                if (activeSeenValues.insert(key).second)
                                {
                                    activeKeep[clusterId] *= (1.0 - std::clamp(opPi, 0.0, 1.0));
                                }
                            }
                        }

                        const ActivityCostClass costClass =
                            dataOp.valid() ? classifyActivityCostClass(graph.opKind(dataOp))
                                           : ActivityCostClass::Sink;
                        if (costClass == ActivityCostClass::Sink)
                        {
                            continue;
                        }
                        for (const auto result : graph.opResults(opId))
                        {
                            const auto canonical = canonicalActivityValue(result, &rewrite.canonicalValues);
                            if (canonical != result)
                            {
                                ++out.canonicalCloneValues;
                            }
                            if (canonical.valid() && footprintValues.insert(canonical).second)
                            {
                                out.nodeFootprintBytes[clusterId] +=
                                    activityFootprintBytesForWidth(graph.valueWidth(canonical));
                            }
                        }
                    }
                }
            }

            for (uint32_t toCluster = 0; toCluster < clusters; ++toCluster)
            {
                for (const auto nodeId : view.members[toCluster])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    for (const auto boundary : rewrite.computeNodes[nodeId].boundaryInputs)
                    {
                        ++out.boundaryValues;
                        const auto canonicalBoundary = canonicalActivityValue(boundary, &rewrite.canonicalValues);
                        const auto defOp = graph.valueDef(boundary);
                        uint32_t fromCluster = kInvalidActivitySupernodeId;
                        if (defOp.valid() && defOp.index < rewrite.computeNodeOfOp.size())
                        {
                            const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                            if (predNode != kInvalidActivitySupernodeId &&
                                predNode < view.clusterOfNode.size())
                            {
                                fromCluster = view.clusterOfNode[predNode];
                            }
                        }
                        if (fromCluster == toCluster)
                        {
                            continue;
                        }

                        ActivityTargetValueKey activeKey{toCluster, canonicalBoundary};
                        if (activeSeenValues.insert(activeKey).second)
                        {
                            const double p = activityPiForValue(graph,
                                                                options,
                                                                rewrite.canonicalValues,
                                                                piByOpIndex,
                                                                boundary);
                            activeKeep[toCluster] *= (1.0 - std::clamp(p, 0.0, 1.0));
                        }

                        if (fromCluster == kInvalidActivitySupernodeId)
                        {
                            continue;
                        }
                        ++out.externalBoundaryValues;
                        const std::uint64_t pair = packClusterPair(fromCluster, toCluster);
                        auto &edge = edgeMap[pair];
                        ActivityPairValueKey countKey{pair, boundary};
                        if (seenEdgeValues.insert(countKey).second)
                        {
                            ++edge.count;
                        }
                        ActivityPairValueKey probKey{pair, canonicalBoundary};
                        if (seenEdgeProbValues.insert(probKey).second)
                        {
                            edge.totalProb += activityPiForValue(graph,
                                                                 options,
                                                                 rewrite.canonicalValues,
                                                                 piByOpIndex,
                                                                 boundary);
                        }
                    }
                }
            }

            for (uint32_t clusterId = 0; clusterId < clusters; ++clusterId)
            {
                out.nodeActiveProb[clusterId] = std::clamp(1.0 - activeKeep[clusterId], 0.0, 1.0);
                if (out.nodeMinTopo[clusterId] == kInvalidActivitySupernodeId)
                {
                    out.nodeMaxTopo[clusterId] = kInvalidActivitySupernodeId;
                }
            }

            std::vector<std::pair<std::uint64_t, ActivityHyperedge>> orderedEdges(edgeMap.begin(),
                                                                                  edgeMap.end());
            std::sort(orderedEdges.begin(),
                      orderedEdges.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          return lhs.first < rhs.first;
                      });
            out.edgeFrom.reserve(orderedEdges.size());
            out.edgeTo.reserve(orderedEdges.size());
            out.edgeCount.reserve(orderedEdges.size());
            out.edgeTotalProb.reserve(orderedEdges.size());
            for (const auto &[packed, edge] : orderedEdges)
            {
                out.edgeFrom.push_back(static_cast<uint32_t>(packed >> 32));
                out.edgeTo.push_back(static_cast<uint32_t>(packed & 0xffffffffu));
                out.edgeCount.push_back(edge.count);
                out.edgeTotalProb.push_back(edge.totalProb);
            }
            return out;
        }

        ProbCoarsenClusterAggregate buildProbCoarsenClusterAggregateFromSeed(
            const ActivityHypergraphAggregate &seed,
            const NodeClusterView &view)
        {
            ProbCoarsenClusterAggregate out;
            auto &agg = out.aggregate;
            const std::size_t clusters = view.members.size();
            agg.nodeWeight.assign(clusters, 0.0);
            agg.nodeChangeWeight.assign(clusters, 0.0);
            agg.nodeFootprintBytes.assign(clusters, 0);
            agg.nodeActiveProb.assign(clusters, 0.0);
            agg.nodeMinTopo.assign(clusters, kInvalidActivitySupernodeId);
            agg.nodeMaxTopo.assign(clusters, kInvalidActivitySupernodeId);
            agg.nodeOpCount.assign(clusters, 0);
            agg.boundaryValues = seed.boundaryValues;
            agg.canonicalCloneValues = seed.canonicalCloneValues;

            for (uint32_t clusterId = 0; clusterId < clusters; ++clusterId)
            {
                uint32_t minTopo = kInvalidActivitySupernodeId;
                uint32_t maxTopo = 0;
                bool anyTopo = false;
                for (const auto nodeId : view.members[clusterId])
                {
                    if (nodeId < seed.nodeWeight.size())
                    {
                        agg.nodeWeight[clusterId] += seed.nodeWeight[nodeId];
                    }
                    if (nodeId < seed.nodeChangeWeight.size())
                    {
                        agg.nodeChangeWeight[clusterId] += seed.nodeChangeWeight[nodeId];
                    }
                    if (nodeId < seed.nodeFootprintBytes.size())
                    {
                        agg.nodeFootprintBytes[clusterId] += seed.nodeFootprintBytes[nodeId];
                    }
                    if (nodeId < seed.nodeActiveProb.size())
                    {
                        agg.nodeActiveProb[clusterId] =
                            std::max(agg.nodeActiveProb[clusterId],
                                     std::clamp(seed.nodeActiveProb[nodeId], 0.0, 1.0));
                    }
                    if (nodeId < seed.nodeOpCount.size())
                    {
                        agg.nodeOpCount[clusterId] += seed.nodeOpCount[nodeId];
                    }
                    if (nodeId < seed.nodeMinTopo.size())
                    {
                        const uint32_t nodeMin = seed.nodeMinTopo[nodeId];
                        if (nodeMin != kInvalidActivitySupernodeId)
                        {
                            minTopo = std::min(minTopo, nodeMin);
                            anyTopo = true;
                        }
                    }
                    if (nodeId < seed.nodeMaxTopo.size())
                    {
                        const uint32_t nodeMax = seed.nodeMaxTopo[nodeId];
                        if (nodeMax != kInvalidActivitySupernodeId)
                        {
                            maxTopo = std::max(maxTopo, nodeMax);
                            anyTopo = true;
                        }
                    }
                }
                if (anyTopo)
                {
                    agg.nodeMinTopo[clusterId] = minTopo;
                    agg.nodeMaxTopo[clusterId] = maxTopo;
                }
            }

            out.edgeProb.reserve(seed.edgeFrom.size() * 2 + 1);
            for (std::size_t i = 0; i < seed.edgeFrom.size(); ++i)
            {
                if (i >= seed.edgeTo.size() || i >= seed.edgeTotalProb.size())
                {
                    continue;
                }
                const uint32_t fromNode = seed.edgeFrom[i];
                const uint32_t toNode = seed.edgeTo[i];
                if (fromNode >= view.clusterOfNode.size() || toNode >= view.clusterOfNode.size())
                {
                    continue;
                }
                const uint32_t fromCluster = view.clusterOfNode[fromNode];
                const uint32_t toCluster = view.clusterOfNode[toNode];
                if (fromCluster == kInvalidActivitySupernodeId ||
                    toCluster == kInvalidActivitySupernodeId ||
                    fromCluster == toCluster)
                {
                    continue;
                }
                out.edgeProb[packClusterPair(fromCluster, toCluster)] += seed.edgeTotalProb[i];
                if (i < seed.edgeCount.size())
                {
                    agg.externalBoundaryValues += seed.edgeCount[i];
                }
            }
            return out;
        }

        ProbCoarsenClusterAggregate buildProbCoarsenClusterAggregateFromGraph(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const ComputeRewriteBuild &rewrite,
            const NodeClusterView &view,
            const ActivityCostModel &costModel,
            const std::vector<float> &piByOpIndex)
        {
            ProbCoarsenClusterAggregate out;
            out.aggregate =
                buildActivityHypergraphAggregate(graph, options, opData, rewrite, view, costModel, piByOpIndex);
            out.edgeProb.reserve(out.aggregate.edgeFrom.size() * 2 + 1);
            for (std::size_t i = 0; i < out.aggregate.edgeFrom.size(); ++i)
            {
                if (i < out.aggregate.edgeTo.size() && i < out.aggregate.edgeTotalProb.size())
                {
                    out.edgeProb[packClusterPair(out.aggregate.edgeFrom[i], out.aggregate.edgeTo[i])] +=
                        out.aggregate.edgeTotalProb[i];
                }
            }
            return out;
        }

        template <typename T>
        T percentileOfSortedGeneric(const std::vector<T> &values, std::size_t pct)
        {
            if (values.empty())
            {
                return T{};
            }
            const std::size_t index =
                std::min(values.size() - 1, (values.size() - 1) * pct / static_cast<std::size_t>(100));
            return values[index];
        }

        std::string summarizeActivityHypergraphAggregate(const ActivityHypergraphAggregate &agg)
        {
            double totalWeight = 0.0;
            double totalChangeWeight = 0.0;
            double totalActiveProb = 0.0;
            std::uint64_t totalFootprint = 0;
            std::vector<double> weights = agg.nodeWeight;
            std::vector<double> changeWeights = agg.nodeChangeWeight;
            std::vector<double> active = agg.nodeActiveProb;
            std::vector<std::uint64_t> footprints = agg.nodeFootprintBytes;
            for (std::size_t i = 0; i < agg.nodeWeight.size(); ++i)
            {
                totalWeight += agg.nodeWeight[i];
                totalChangeWeight += i < agg.nodeChangeWeight.size() ? agg.nodeChangeWeight[i] : 0.0;
                totalActiveProb += i < agg.nodeActiveProb.size() ? agg.nodeActiveProb[i] : 0.0;
                totalFootprint += i < agg.nodeFootprintBytes.size() ? agg.nodeFootprintBytes[i] : 0;
            }
            std::sort(weights.begin(), weights.end());
            std::sort(changeWeights.begin(), changeWeights.end());
            std::sort(active.begin(), active.end());
            std::sort(footprints.begin(), footprints.end());

            std::uint64_t totalEdgeCount = 0;
            double totalEdgeProb = 0.0;
            std::vector<std::uint64_t> edgeCounts = agg.edgeCount;
            std::vector<double> edgeProbs = agg.edgeTotalProb;
            for (std::size_t i = 0; i < agg.edgeCount.size(); ++i)
            {
                totalEdgeCount += agg.edgeCount[i];
                totalEdgeProb += i < agg.edgeTotalProb.size() ? agg.edgeTotalProb[i] : 0.0;
            }
            std::sort(edgeCounts.begin(), edgeCounts.end());
            std::sort(edgeProbs.begin(), edgeProbs.end());

            const double meanWeight =
                agg.nodeWeight.empty() ? 0.0 : totalWeight / static_cast<double>(agg.nodeWeight.size());
            const double meanChange =
                agg.nodeChangeWeight.empty() ? 0.0
                                             : totalChangeWeight / static_cast<double>(agg.nodeChangeWeight.size());
            const double meanActive =
                agg.nodeActiveProb.empty() ? 0.0 : totalActiveProb / static_cast<double>(agg.nodeActiveProb.size());

            std::ostringstream oss;
            oss << "nodes=" << agg.nodeWeight.size()
                << " edges=" << agg.edgeFrom.size()
                << " boundary_values=" << agg.boundaryValues
                << " inter_node_boundary_values=" << agg.externalBoundaryValues
                << " canonical_clone_values=" << agg.canonicalCloneValues
                << " total_weight=" << totalWeight
                << " mean_weight=" << meanWeight
                << " weight_p50=" << percentileOfSortedGeneric(weights, 50)
                << " weight_p90=" << percentileOfSortedGeneric(weights, 90)
                << " weight_p99=" << percentileOfSortedGeneric(weights, 99)
                << " weight_max=" << (weights.empty() ? 0.0 : weights.back())
                << " total_change_weight=" << totalChangeWeight
                << " mean_change_weight=" << meanChange
                << " change_p90=" << percentileOfSortedGeneric(changeWeights, 90)
                << " total_footprint_bytes=" << totalFootprint
                << " footprint_p50=" << percentileOfSortedGeneric(footprints, 50)
                << " footprint_p90=" << percentileOfSortedGeneric(footprints, 90)
                << " footprint_p99=" << percentileOfSortedGeneric(footprints, 99)
                << " footprint_max=" << (footprints.empty() ? 0 : footprints.back())
                << " mean_active_prob=" << meanActive
                << " active_p50=" << percentileOfSortedGeneric(active, 50)
                << " active_p90=" << percentileOfSortedGeneric(active, 90)
                << " active_p99=" << percentileOfSortedGeneric(active, 99)
                << " active_max=" << (active.empty() ? 0.0 : active.back())
                << " edge_count_total=" << totalEdgeCount
                << " edge_count_p90=" << percentileOfSortedGeneric(edgeCounts, 90)
                << " edge_count_max=" << (edgeCounts.empty() ? 0 : edgeCounts.back())
                << " edge_total_prob=" << totalEdgeProb
                << " edge_prob_p90=" << percentileOfSortedGeneric(edgeProbs, 90)
                << " edge_prob_max=" << (edgeProbs.empty() ? 0.0 : edgeProbs.back());
            return oss.str();
        }

        std::string summarizeTopActivityHypergraphNodes(const ActivityHypergraphAggregate &agg,
                                                        std::size_t limit)
        {
            struct Entry
            {
                uint32_t node = 0;
                double weight = 0.0;
                double changeWeight = 0.0;
                std::uint64_t footprint = 0;
                double activeProb = 0.0;
                uint32_t minTopo = kInvalidActivitySupernodeId;
                uint32_t maxTopo = kInvalidActivitySupernodeId;
                uint32_t ops = 0;
            };
            std::vector<Entry> entries;
            entries.reserve(agg.nodeWeight.size());
            for (uint32_t i = 0; i < agg.nodeWeight.size(); ++i)
            {
                entries.push_back(Entry{
                    i,
                    agg.nodeWeight[i],
                    i < agg.nodeChangeWeight.size() ? agg.nodeChangeWeight[i] : 0.0,
                    i < agg.nodeFootprintBytes.size() ? agg.nodeFootprintBytes[i] : 0,
                    i < agg.nodeActiveProb.size() ? agg.nodeActiveProb[i] : 0.0,
                    i < agg.nodeMinTopo.size() ? agg.nodeMinTopo[i] : kInvalidActivitySupernodeId,
                    i < agg.nodeMaxTopo.size() ? agg.nodeMaxTopo[i] : kInvalidActivitySupernodeId,
                    i < agg.nodeOpCount.size() ? agg.nodeOpCount[i] : 0,
                });
            }
            std::sort(entries.begin(),
                      entries.end(),
                      [](const Entry &lhs, const Entry &rhs)
                      {
                          if (lhs.footprint != rhs.footprint)
                          {
                              return lhs.footprint > rhs.footprint;
                          }
                          if (lhs.weight != rhs.weight)
                          {
                              return lhs.weight > rhs.weight;
                          }
                          return lhs.node < rhs.node;
                      });
            std::ostringstream oss;
            const std::size_t n = std::min(limit, entries.size());
            for (std::size_t i = 0; i < n; ++i)
            {
                if (i != 0)
                {
                    oss << " ";
                }
                const auto &e = entries[i];
                oss << "#" << e.node
                    << "{ops=" << e.ops
                    << ",W=" << e.weight
                    << ",change=" << e.changeWeight
                    << ",footprint=" << e.footprint
                    << ",active=" << e.activeProb;
                if (e.minTopo != kInvalidActivitySupernodeId)
                {
                    oss << ",topo=[" << e.minTopo << "," << e.maxTopo << "]";
                }
                oss << "}";
            }
            return oss.str();
        }

        std::vector<std::vector<uint32_t>> canonicalizeNodeClusters(std::vector<std::vector<uint32_t>> clusters,
                                                                    const std::vector<uint32_t> &nodeTopoPos)
        {
            for (auto &members : clusters)
            {
                std::sort(members.begin(),
                          members.end(),
                          [&](uint32_t lhs, uint32_t rhs)
                          {
                              const uint32_t lhsPos = lhs < nodeTopoPos.size() ? nodeTopoPos[lhs]
                                                                               : kInvalidActivitySupernodeId;
                              const uint32_t rhsPos = rhs < nodeTopoPos.size() ? nodeTopoPos[rhs]
                                                                               : kInvalidActivitySupernodeId;
                              if (lhsPos != rhsPos)
                              {
                                  return lhsPos < rhsPos;
                              }
                              return lhs < rhs;
                          });
            }
            std::sort(clusters.begin(),
                      clusters.end(),
                      [&](const auto &lhs, const auto &rhs)
                      {
                          const uint32_t lhsHead =
                              lhs.empty() || lhs.front() >= nodeTopoPos.size() ? kInvalidActivitySupernodeId
                                                                               : nodeTopoPos[lhs.front()];
                          const uint32_t rhsHead =
                              rhs.empty() || rhs.front() >= nodeTopoPos.size() ? kInvalidActivitySupernodeId
                                                                               : nodeTopoPos[rhs.front()];
                          if (lhsHead != rhsHead)
                          {
                              return lhsHead < rhsHead;
                          }
                          return lhs < rhs;
                      });
            return clusters;
        }

        std::string formatTopCounts(const ActivityScheduleSummaryStats::KindCountMap &counts,
                                    std::size_t limit)
        {
            std::vector<std::pair<std::string, std::size_t>> ordered(counts.begin(), counts.end());
            std::sort(ordered.begin(),
                      ordered.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.second != rhs.second)
                          {
                              return lhs.second > rhs.second;
                          }
                          return lhs.first < rhs.first;
                      });
            std::ostringstream oss;
            for (std::size_t i = 0; i < ordered.size() && i < limit; ++i)
            {
                if (i != 0)
                {
                    oss << ",";
                }
                oss << ordered[i].first << ":" << ordered[i].second;
            }
            return oss.str();
        }

        std::size_t percentileOfSorted(const std::vector<std::size_t> &values, std::size_t pct)
        {
            if (values.empty())
            {
                return 0;
            }
            const std::size_t index =
                std::min(values.size() - 1, (values.size() - 1) * pct / static_cast<std::size_t>(100));
            return values[index];
        }

        std::string summarizeCoarsenClusterShape(const NodeClusterView &view,
                                                 const ComputeRewriteBuild &rewrite,
                                                 const wolvrix::lib::grh::Graph &graph,
                                                 const std::vector<uint32_t> &nodeOpSizes)
        {
            std::size_t isolated = 0;
            std::size_t sources = 0;
            std::size_t sinks = 0;
            std::size_t linear = 0;
            std::size_t forks = 0;
            std::size_t joins = 0;
            std::size_t maxPred = 0;
            std::size_t maxSucc = 0;
            std::vector<std::size_t> opSizes;
            opSizes.reserve(view.members.size());
            ActivityScheduleSummaryStats::KindCountMap boundaryDefKinds;
            ActivityScheduleSummaryStats::KindCountMap boundaryUseKinds;

            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                const std::size_t predCount = clusterId < view.preds.size() ? view.preds[clusterId].size() : 0;
                const std::size_t succCount = clusterId < view.succs.size() ? view.succs[clusterId].size() : 0;
                maxPred = std::max(maxPred, predCount);
                maxSucc = std::max(maxSucc, succCount);
                if (predCount == 0 && succCount == 0)
                {
                    ++isolated;
                }
                if (predCount == 0 && succCount != 0)
                {
                    ++sources;
                }
                if (succCount == 0 && predCount != 0)
                {
                    ++sinks;
                }
                if (predCount == 1 && succCount == 1)
                {
                    ++linear;
                }
                if (succCount > 1)
                {
                    ++forks;
                }
                if (predCount > 1)
                {
                    ++joins;
                }
                opSizes.push_back(clusterOpSize(view.members[clusterId], nodeOpSizes));

                for (const auto nodeId : view.members[clusterId])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    const auto &computeNode = rewrite.computeNodes[nodeId];
                    for (const auto boundary : computeNode.boundaryInputs)
                    {
                        const auto defOp = graph.valueDef(boundary);
                        if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                        {
                            continue;
                        }
                        const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                        if (predNode == kInvalidActivitySupernodeId ||
                            predNode >= view.clusterOfNode.size() ||
                            view.clusterOfNode[predNode] == clusterId)
                        {
                            continue;
                        }
                        ++boundaryDefKinds[std::string(wolvrix::lib::grh::toString(graph.opKind(defOp)))];
                        if (!computeNode.ops.empty())
                        {
                            ++boundaryUseKinds[
                                std::string(wolvrix::lib::grh::toString(graph.opKind(computeNode.ops.front())))];
                        }
                    }
                }
            }

            std::sort(opSizes.begin(), opSizes.end());
            const std::size_t totalOps =
                std::accumulate(opSizes.begin(), opSizes.end(), static_cast<std::size_t>(0));
            const std::size_t meanOps = opSizes.empty() ? 0 : totalOps / opSizes.size();

            std::ostringstream oss;
            oss << "clusters=" << view.members.size()
                << " isolated=" << isolated
                << " sources=" << sources
                << " sinks=" << sinks
                << " linear=" << linear
                << " forks=" << forks
                << " joins=" << joins
                << " max_pred=" << maxPred
                << " max_succ=" << maxSucc
                << " op_size_min=" << (opSizes.empty() ? 0 : opSizes.front())
                << " op_size_mean=" << meanOps
                << " op_size_p50=" << percentileOfSorted(opSizes, 50)
                << " op_size_p90=" << percentileOfSorted(opSizes, 90)
                << " op_size_max=" << (opSizes.empty() ? 0 : opSizes.back())
                << " boundary_def_kinds=" << formatTopCounts(boundaryDefKinds, 12)
                << " boundary_use_kinds=" << formatTopCounts(boundaryUseKinds, 12);
            return oss.str();
        }

        bool orderNodeClustersTopologically(std::vector<std::vector<uint32_t>> &clusters,
                                            const std::vector<std::vector<uint32_t>> &nodeDag,
                                            std::size_t nodeCount,
                                            const ComputeRewriteBuild *rewrite,
                                            const wolvrix::lib::grh::Graph *graph)
        {
            if (clusters.empty())
            {
                return true;
            }
            const NodeClusterView view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            std::vector<uint32_t> order;
            try
            {
                (void)rewrite;
                (void)graph;
                order = topoOrderForDagValueLocal(view.succs, nullptr);
            }
            catch (const std::exception &)
            {
                return false;
            }
            if (order.size() != view.members.size())
            {
                return false;
            }
            std::vector<std::vector<uint32_t>> out;
            out.reserve(order.size());
            for (const auto clusterId : order)
            {
                if (clusterId >= view.members.size())
                {
                    return false;
                }
                out.push_back(view.members[clusterId]);
            }
            clusters = std::move(out);
            return true;
        }

        std::uint64_t nodeSiblingPredHash(const std::vector<uint32_t> &preds)
        {
            std::uint64_t hash = static_cast<std::uint64_t>(preds.size()) * 0x9e3779b185ebca87ull;
            for (uint32_t pred : preds)
            {
                hash ^= static_cast<std::uint64_t>(pred) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            }
            return hash;
        }

        bool tryMergeNodeSiblings(std::vector<std::vector<uint32_t>> &clusters,
                                  const std::vector<std::vector<uint32_t>> &nodeDag,
                                  std::size_t nodeCount,
                                  const std::vector<uint32_t> &nodeTopoPos,
                                  const std::vector<uint32_t> &nodeOpSizes,
                                  std::size_t maxOps,
                                  const ComputeRewriteBuild &rewrite,
                                  const wolvrix::lib::grh::Graph &graph)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            std::unordered_map<std::uint64_t, std::vector<std::vector<uint32_t>>> buckets;
            buckets.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (view.preds[clusterId].empty())
                {
                    continue;
                }
                auto &bucket = buckets[nodeSiblingPredHash(view.preds[clusterId])];
                auto groupIt = std::find_if(bucket.begin(),
                                            bucket.end(),
                                            [&](const auto &group)
                                            {
                                                return !group.empty() &&
                                                       view.preds[group.front()] == view.preds[clusterId];
                                            });
                if (groupIt == bucket.end())
                {
                    bucket.push_back(std::vector<uint32_t>{clusterId});
                }
                else
                {
                    groupIt->push_back(clusterId);
                }
            }

            DisjointSet dsu(view.members.size());
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                sizes[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
            }

            bool changed = false;
            for (auto &[_, bucket] : buckets)
            {
                for (auto &siblings : bucket)
                {
                    if (siblings.size() < 2)
                    {
                        continue;
                    }
                    std::sort(siblings.begin(), siblings.end());
                    uint32_t anchor = dsu.find(siblings.front());
                    for (std::size_t index = 1; index < siblings.size(); ++index)
                    {
                        uint32_t lhs = dsu.find(anchor);
                        uint32_t rhs = dsu.find(siblings[index]);
                        if (lhs == rhs)
                        {
                            continue;
                        }
                        if (sizes[lhs] + sizes[rhs] > maxOps)
                        {
                            anchor = rhs;
                            continue;
                        }
                        if (dsu.unite(lhs, rhs))
                        {
                            const uint32_t root = dsu.find(lhs);
                            sizes[root] = sizes[lhs] + sizes[rhs];
                            anchor = root;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed)
            {
                return false;
            }

            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            out.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                const uint32_t root = dsu.find(clusterId);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(),
                                       view.members[clusterId].begin(),
                                       view.members[clusterId].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                return false;
            }
            clusters = std::move(out);
            return true;
        }

        bool tryMergeNodeOut1(std::vector<std::vector<uint32_t>> &clusters,
                              const std::vector<std::vector<uint32_t>> &nodeDag,
                              std::size_t nodeCount,
                              const std::vector<uint32_t> &nodeTopoPos,
                              const std::vector<uint32_t> &nodeOpSizes,
                              std::size_t maxOps,
                              const ComputeRewriteBuild &rewrite,
                              const wolvrix::lib::grh::Graph &graph)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            const auto valueEdges = buildClusterValueEdges(view, rewrite, graph);
            struct Candidate
            {
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t weight = 0;
            };
            std::vector<Candidate> candidates;
            candidates.reserve(view.members.size());
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                if (view.succs[id].size() != 1)
                {
                    continue;
                }
                const uint32_t succ = view.succs[id].front();
                const std::size_t weight = clusterEdgeWeight(valueEdges, id, succ);
                if (weight == 0)
                {
                    continue;
                }
                candidates.push_back(Candidate{id, succ, weight});
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.weight != rhs.weight)
                          {
                              return lhs.weight > rhs.weight;
                          }
                          if (lhs.from != rhs.from)
                          {
                              return lhs.from < rhs.from;
                          }
                          return lhs.to < rhs.to;
                      });
            DisjointSet dsu(view.members.size());
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                sizes[id] = clusterOpSize(view.members[id], nodeOpSizes);
            }
            bool changed = false;
            for (const auto &candidate : candidates)
            {
                uint32_t lhs = dsu.find(candidate.from);
                uint32_t rhs = dsu.find(candidate.to);
                if (lhs == rhs || sizes[lhs] + sizes[rhs] > maxOps)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    sizes[root] = sizes[lhs] + sizes[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                const uint32_t root = dsu.find(id);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(), view.members[id].begin(), view.members[id].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                return false;
            }
            clusters = std::move(out);
            return true;
        }

        bool tryMergeNodeIn1(std::vector<std::vector<uint32_t>> &clusters,
                             const std::vector<std::vector<uint32_t>> &nodeDag,
                             std::size_t nodeCount,
                             const std::vector<uint32_t> &nodeTopoPos,
                             const std::vector<uint32_t> &nodeOpSizes,
                             std::size_t maxOps,
                             const ComputeRewriteBuild &rewrite,
                             const wolvrix::lib::grh::Graph &graph)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            const auto valueEdges = buildClusterValueEdges(view, rewrite, graph);
            struct Candidate
            {
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t weight = 0;
            };
            std::vector<Candidate> candidates;
            candidates.reserve(view.members.size());
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                if (view.preds[id].size() != 1)
                {
                    continue;
                }
                const uint32_t pred = view.preds[id].front();
                const std::size_t weight = clusterEdgeWeight(valueEdges, pred, id);
                if (weight == 0)
                {
                    continue;
                }
                candidates.push_back(Candidate{pred, id, weight});
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.weight != rhs.weight)
                          {
                              return lhs.weight > rhs.weight;
                          }
                          if (lhs.from != rhs.from)
                          {
                              return lhs.from < rhs.from;
                          }
                          return lhs.to < rhs.to;
                      });
            DisjointSet dsu(view.members.size());
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                sizes[id] = clusterOpSize(view.members[id], nodeOpSizes);
            }
            bool changed = false;
            for (const auto &candidate : candidates)
            {
                uint32_t lhs = dsu.find(candidate.to);
                uint32_t rhs = dsu.find(candidate.from);
                if (lhs == rhs || sizes[lhs] + sizes[rhs] > maxOps)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    sizes[root] = sizes[lhs] + sizes[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                const uint32_t root = dsu.find(id);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(), view.members[id].begin(), view.members[id].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                return false;
            }
            clusters = std::move(out);
            return true;
        }

        constexpr uint32_t kCbawTagHeavyValueUse = 1u << 0;
        constexpr uint32_t kCbawTagPlainOut1 = 1u << 1;
        constexpr uint32_t kCbawTagPlainIn1 = 1u << 2;
        constexpr uint32_t kCbawTagPlainSiblings = 1u << 3;
        constexpr uint32_t kCbawTagAggregateHint = 1u << 4;
        constexpr uint32_t kCbawTagMffcDominance = 1u << 5;
        constexpr uint32_t kCbawTagGuardHint = 1u << 6;
        constexpr uint32_t kCbawTagSinkCone = 1u << 7;
        constexpr uint32_t kCbawTagPassthrough = 1u << 8;

        std::string_view cbawTagName(uint32_t tag) noexcept
        {
            switch (tag)
            {
            case kCbawTagHeavyValueUse: return "heavy_value_use";
            case kCbawTagPlainOut1: return "plain_out1";
            case kCbawTagPlainIn1: return "plain_in1";
            case kCbawTagPlainSiblings: return "plain_siblings";
            case kCbawTagAggregateHint: return "aggregate_hint";
            case kCbawTagMffcDominance: return "mffc_dominance";
            case kCbawTagGuardHint: return "guard_hint";
            case kCbawTagSinkCone: return "sink_cone";
            case kCbawTagPassthrough: return "passthrough";
            default: return "unknown";
            }
        }

        template <typename Fn>
        void forEachCbawTag(uint32_t tags, Fn &&fn)
        {
            constexpr std::array<uint32_t, 9> kTags = {
                kCbawTagHeavyValueUse,
                kCbawTagPlainOut1,
                kCbawTagPlainIn1,
                kCbawTagPlainSiblings,
                kCbawTagAggregateHint,
                kCbawTagMffcDominance,
                kCbawTagGuardHint,
                kCbawTagSinkCone,
                kCbawTagPassthrough,
            };
            for (const uint32_t tag : kTags)
            {
                if ((tags & tag) != 0)
                {
                    fn(tag);
                }
            }
        }

        int cbawPrimaryKindPriority(uint32_t tag) noexcept
        {
            switch (tag)
            {
            case kCbawTagPlainOut1:
            case kCbawTagPlainIn1:
                return 0;
            case kCbawTagPlainSiblings:
                return 1;
            case kCbawTagAggregateHint:
            case kCbawTagMffcDominance:
            case kCbawTagPassthrough:
                return 2;
            case kCbawTagGuardHint:
            case kCbawTagSinkCone:
                return 3;
            case kCbawTagHeavyValueUse:
            default:
                return 4;
            }
        }

        uint32_t cbawChoosePrimaryTag(uint32_t tags) noexcept
        {
            uint32_t best = kCbawTagHeavyValueUse;
            int bestPriority = std::numeric_limits<int>::max();
            forEachCbawTag(tags,
                           [&](uint32_t tag)
                           {
                               const int priority = cbawPrimaryKindPriority(tag);
                               if (priority < bestPriority ||
                                   (priority == bestPriority && tag < best))
                               {
                                   best = tag;
                                   bestPriority = priority;
                               }
                           });
            return best;
        }

        int cbawSemanticTieBreakScore(uint32_t tags) noexcept
        {
            int score = 0;
            if ((tags & kCbawTagAggregateHint) != 0)
            {
                score += 8;
            }
            if ((tags & kCbawTagMffcDominance) != 0)
            {
                score += 8;
            }
            if ((tags & kCbawTagPassthrough) != 0)
            {
                score += 4;
            }
            if ((tags & kCbawTagGuardHint) != 0)
            {
                score += 2;
            }
            if ((tags & kCbawTagSinkCone) != 0)
            {
                score += 2;
            }
            if ((tags & kCbawTagPlainSiblings) != 0)
            {
                score += 1;
            }
            return score;
        }

        bool sortedStringVectorsIntersect(const std::vector<std::string> &lhs,
                                          const std::vector<std::string> &rhs)
        {
            auto l = lhs.begin();
            auto r = rhs.begin();
            while (l != lhs.end() && r != rhs.end())
            {
                if (*l == *r)
                {
                    return true;
                }
                if (*l < *r)
                {
                    ++l;
                }
                else
                {
                    ++r;
                }
            }
            return false;
        }

        std::size_t countSortedIntersection(const std::vector<uint32_t> &lhs,
                                            const std::vector<uint32_t> &rhs,
                                            uint32_t excludedA = kInvalidActivitySupernodeId,
                                            uint32_t excludedB = kInvalidActivitySupernodeId)
        {
            std::size_t count = 0;
            auto l = lhs.begin();
            auto r = rhs.begin();
            while (l != lhs.end() && r != rhs.end())
            {
                if (*l == *r)
                {
                    if (*l != excludedA && *l != excludedB)
                    {
                        ++count;
                    }
                    ++l;
                    ++r;
                }
                else if (*l < *r)
                {
                    ++l;
                }
                else
                {
                    ++r;
                }
            }
            return count;
        }

        struct CbawExactDelta
        {
            std::size_t boundaryTargets = 0;
            std::size_t dagEdges = 0;
            std::size_t computeComputePairs = 0;
            std::size_t directValueTargets = 0;
            std::size_t sharedIncomingFanouts = 0;
        };

        CbawExactDelta computeCbawExactDelta(const NodeClusterView &view,
                                             const ClusterValueEdges &valueEdges,
                                             uint32_t lhs,
                                             uint32_t rhs)
        {
            CbawExactDelta delta;
            if (lhs == rhs ||
                lhs >= view.members.size() ||
                rhs >= view.members.size())
            {
                return delta;
            }
            const std::size_t lhsToRhs = clusterEdgeWeight(valueEdges, lhs, rhs);
            const std::size_t rhsToLhs = clusterEdgeWeight(valueEdges, rhs, lhs);
            delta.directValueTargets = lhsToRhs + rhsToLhs;

            if (lhs < valueEdges.targetValuesByCluster.size() &&
                rhs < valueEdges.targetValuesByCluster.size())
            {
                const auto &lhsTargets = valueEdges.targetValuesByCluster[lhs];
                const auto &rhsTargets = valueEdges.targetValuesByCluster[rhs];
                auto l = lhsTargets.begin();
                auto r = rhsTargets.begin();
                while (l != lhsTargets.end() && r != rhsTargets.end())
                {
                    if (*l == *r)
                    {
                        const uint32_t valueFanoutId = *l;
                        if (valueFanoutId < valueEdges.valueFanouts.size())
                        {
                            const uint32_t source = valueEdges.valueFanouts[valueFanoutId].sourceCluster;
                            if (source != lhs && source != rhs)
                            {
                                ++delta.sharedIncomingFanouts;
                            }
                        }
                        ++l;
                        ++r;
                    }
                    else if (*l < *r)
                    {
                        ++l;
                    }
                    else
                    {
                        ++r;
                    }
                }
            }

            delta.computeComputePairs = delta.directValueTargets + delta.sharedIncomingFanouts;
            delta.boundaryTargets = delta.computeComputePairs;

            const auto hasDagEdge = [&](uint32_t from, uint32_t to)
            {
                return from < view.succs.size() &&
                       std::binary_search(view.succs[from].begin(), view.succs[from].end(), to);
            };
            if (hasDagEdge(lhs, rhs))
            {
                ++delta.dagEdges;
            }
            if (hasDagEdge(rhs, lhs))
            {
                ++delta.dagEdges;
            }
            if (lhs < view.preds.size() && rhs < view.preds.size())
            {
                delta.dagEdges += countSortedIntersection(view.preds[lhs],
                                                          view.preds[rhs],
                                                          lhs,
                                                          rhs);
            }
            if (lhs < view.succs.size() && rhs < view.succs.size())
            {
                delta.dagEdges += countSortedIntersection(view.succs[lhs],
                                                          view.succs[rhs],
                                                          lhs,
                                                          rhs);
            }
            if (lhs < valueEdges.commitSuccsByCluster.size() &&
                rhs < valueEdges.commitSuccsByCluster.size())
            {
                delta.dagEdges += countSortedIntersection(valueEdges.commitSuccsByCluster[lhs],
                                                          valueEdges.commitSuccsByCluster[rhs]);
            }
            return delta;
        }

        bool tryMergeNodeCbaw(std::vector<std::vector<uint32_t>> &clusters,
                              const std::vector<std::vector<uint32_t>> &nodeDag,
                              std::size_t nodeCount,
                              const std::vector<uint32_t> &nodeTopoPos,
                              const std::vector<uint32_t> &nodeOpSizes,
                              std::size_t maxOps,
                              const ComputeRewriteBuild &rewrite,
                              const wolvrix::lib::grh::Graph &graph,
                              const ActivityOpData *opData,
                              ComputeNodeMaterializePerfStats *perf)
        {
            const auto evaluateStart = std::chrono::steady_clock::now();
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            if (view.members.size() < 2)
            {
                return false;
            }
            const auto valueEdges = buildClusterValueEdges(view, rewrite, graph);

            struct ClusterLabels
            {
                std::vector<std::string> aggregateFamilies;
                std::vector<std::string> guardDomains;
                std::vector<std::string> sinkLabels;
                uint32_t mffcRep = kInvalidActivitySupernodeId;
                bool passthrough = false;
                bool multiSink = false;
            };
            std::vector<ClusterLabels> labels(view.members.size());
            std::vector<uint32_t> mffcRepByTopo;
            if (opData != nullptr)
            {
                mffcRepByTopo = computeMffcRep(*opData);
            }
            const auto addUniqueString = [](std::vector<std::string> &values, std::string value)
            {
                if (!value.empty())
                {
                    values.push_back(std::move(value));
                }
            };
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                bool anyOp = false;
                bool allPassthrough = true;
                uint32_t clusterMffcRep = kInvalidActivitySupernodeId;
                bool haveMffcRep = false;
                bool splitMffcRep = false;
                for (const uint32_t nodeId : view.members[clusterId])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    if (!rewrite.computeNodes[nodeId].intentGroup.empty())
                    {
                        addUniqueString(labels[clusterId].aggregateFamilies,
                                        "rtm:" + rewrite.computeNodes[nodeId].intentGroup);
                    }
                    for (const auto opId : rewrite.computeNodes[nodeId].ops)
                    {
                        anyOp = true;
                        const auto op = graph.getOperation(opId);
                        const auto kind = op.kind();
                        if (opData != nullptr &&
                            opId.index < opData->topoPosByOpIndex.size())
                        {
                            const uint32_t topoPos = opData->topoPosByOpIndex[opId.index];
                            if (topoPos != kInvalidActivitySupernodeId &&
                                topoPos < mffcRepByTopo.size() &&
                                mffcRepByTopo[topoPos] != kInvalidActivitySupernodeId)
                            {
                                if (!haveMffcRep)
                                {
                                    clusterMffcRep = mffcRepByTopo[topoPos];
                                    haveMffcRep = true;
                                }
                                else if (clusterMffcRep != mffcRepByTopo[topoPos])
                                {
                                    splitMffcRep = true;
                                }
                            }
                        }
                        if (activityScheduleHasRegToMemIntent(op))
                        {
                            if (const auto group = getAttrString(op, "regToMem.intent.group"))
                            {
                                addUniqueString(labels[clusterId].aggregateFamilies, "rtm:" + *group);
                            }
                        }
                        if (activityScheduleIsAggregateShapeKind(kind))
                        {
                            if (const auto group = getAttrString(op, "regToMem.intent.group"))
                            {
                                addUniqueString(labels[clusterId].aggregateFamilies, "rtm:" + *group);
                            }
                            else if (!op.operands().empty())
                            {
                                const auto canonical = canonicalActivityValue(op.operands().front(),
                                                                              &rewrite.canonicalValues);
                                addUniqueString(labels[clusterId].aggregateFamilies,
                                                canonical.valid()
                                                    ? "value:" + std::to_string(canonical.index)
                                                    : "op:" + std::to_string(opId.index));
                            }
                            else
                            {
                                addUniqueString(labels[clusterId].aggregateFamilies,
                                                "op:" + std::to_string(opId.index));
                            }
                        }
                        if (activityScheduleIsGuardLikeKind(kind))
                        {
                            std::string guardKey;
                            if (kind == wolvrix::lib::grh::OperationKind::kMux &&
                                !op.operands().empty())
                            {
                                const auto canonical = canonicalActivityValue(op.operands().front(),
                                                                              &rewrite.canonicalValues);
                                guardKey = canonical.valid() ? "mux:" + std::to_string(canonical.index)
                                                             : std::string();
                            }
                            else if ((kind == wolvrix::lib::grh::OperationKind::kAnd ||
                                      kind == wolvrix::lib::grh::OperationKind::kLogicAnd ||
                                      kind == wolvrix::lib::grh::OperationKind::kOr ||
                                      kind == wolvrix::lib::grh::OperationKind::kLogicOr) &&
                                     !op.operands().empty())
                            {
                                std::vector<std::size_t> oneBitInputs;
                                for (const auto operand : op.operands())
                                {
                                    const auto canonical = canonicalActivityValue(operand,
                                                                                  &rewrite.canonicalValues);
                                    if (canonical.valid() && graph.valueWidth(canonical) == 1)
                                    {
                                        oneBitInputs.push_back(canonical.index);
                                    }
                                }
                                if (!oneBitInputs.empty())
                                {
                                    std::sort(oneBitInputs.begin(), oneBitInputs.end());
                                    guardKey = "logic:" + std::to_string(oneBitInputs.front());
                                }
                            }
                            addUniqueString(labels[clusterId].guardDomains, std::move(guardKey));
                        }
                        std::unordered_set<std::string> opSinkLabels;
                        for (const auto result : op.results())
                        {
                            if (!result.valid())
                            {
                                continue;
                            }
                            const auto value = graph.getValue(result);
                            for (const auto &user : value.users())
                            {
                                const auto userOp = graph.getOperation(user.operation);
                                if (!isSinkPartitionOp(userOp))
                                {
                                    continue;
                                }
                                std::string label =
                                    std::string(wolvrix::lib::grh::toString(userOp.kind()));
                                if (const auto reg = getAttrString(userOp, "regSymbol"))
                                {
                                    label += ":" + *reg;
                                }
                                else if (const auto latch = getAttrString(userOp, "latchSymbol"))
                                {
                                    label += ":" + *latch;
                                }
                                else if (const auto mem = getAttrString(userOp, "memSymbol"))
                                {
                                    label += ":" + *mem;
                                }
                                label += ":operand" + std::to_string(user.operandIndex);
                                opSinkLabels.insert(std::move(label));
                            }
                        }
                        if (opSinkLabels.size() > 1)
                        {
                            labels[clusterId].multiSink = true;
                        }
                        for (auto &label : opSinkLabels)
                        {
                            addUniqueString(labels[clusterId].sinkLabels, std::move(label));
                        }
                        allPassthrough = allPassthrough && activityScheduleIsPassthroughKind(kind);
                    }
                }
                labels[clusterId].passthrough = anyOp && allPassthrough;
                if (haveMffcRep && !splitMffcRep)
                {
                    labels[clusterId].mffcRep = clusterMffcRep;
                }
                auto normalizeStrings = [](std::vector<std::string> &values)
                {
                    std::sort(values.begin(), values.end());
                    values.erase(std::unique(values.begin(), values.end()), values.end());
                };
                normalizeStrings(labels[clusterId].aggregateFamilies);
                normalizeStrings(labels[clusterId].guardDomains);
                normalizeStrings(labels[clusterId].sinkLabels);
            }

            struct Candidate
            {
                uint32_t lhs = 0;
                uint32_t rhs = 0;
                CbawExactDelta delta;
                std::size_t directWeight = 0;
                std::size_t mergedOps = 0;
                std::size_t resourceSlack = 0;
                uint32_t tags = 0;
                uint32_t primaryTag = kCbawTagHeavyValueUse;
                const char *selectedReason = "exact_delta";
            };
            std::map<std::uint64_t, Candidate> bestByPair;
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                sizes[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
            }
            const auto betterCandidate = [](const Candidate &lhs, const Candidate &rhs) {
                if (lhs.delta.boundaryTargets != rhs.delta.boundaryTargets)
                {
                    return lhs.delta.boundaryTargets > rhs.delta.boundaryTargets;
                }
                if (lhs.delta.dagEdges != rhs.delta.dagEdges)
                {
                    return lhs.delta.dagEdges > rhs.delta.dagEdges;
                }
                if (lhs.delta.computeComputePairs != rhs.delta.computeComputePairs)
                {
                    return lhs.delta.computeComputePairs > rhs.delta.computeComputePairs;
                }
                const int lhsSemantic = cbawSemanticTieBreakScore(lhs.tags);
                const int rhsSemantic = cbawSemanticTieBreakScore(rhs.tags);
                if (lhsSemantic != rhsSemantic)
                {
                    return lhsSemantic > rhsSemantic;
                }
                if (lhs.directWeight != rhs.directWeight)
                {
                    return lhs.directWeight > rhs.directWeight;
                }
                if (lhs.resourceSlack != rhs.resourceSlack)
                {
                    return lhs.resourceSlack > rhs.resourceSlack;
                }
                if (lhs.lhs != rhs.lhs)
                {
                    return lhs.lhs < rhs.lhs;
                }
                return lhs.rhs < rhs.rhs;
            };
            const auto noteMap = [&](auto &map, uint32_t tag)
            {
                ++map[std::string(cbawTagName(tag))];
            };
            const auto addCandidateTag = [&](uint32_t lhs, uint32_t rhs, uint32_t tag)
            {
                if (perf != nullptr)
                {
                    noteMap(perf->cbawCoarsenGeneratedByKind, tag);
                    ++perf->cbawCoarsenCandidates;
                }
                if (lhs == rhs ||
                    lhs >= view.members.size() ||
                    rhs >= view.members.size())
                {
                    if (perf != nullptr)
                    {
                        ++perf->cbawCoarsenRejectedNoGain;
                        noteMap(perf->cbawCoarsenRejectedNoGainByKind, tag);
                        noteMap(perf->cbawCoarsenRejectedNoGainByTag, tag);
                    }
                    return;
                }
                if (rhs < lhs)
                {
                    std::swap(lhs, rhs);
                }
                const CbawExactDelta delta = computeCbawExactDelta(view, valueEdges, lhs, rhs);
                if (delta.boundaryTargets == 0 &&
                    delta.dagEdges == 0 &&
                    delta.computeComputePairs == 0)
                {
                    if (perf != nullptr)
                    {
                        ++perf->cbawCoarsenRejectedNoGain;
                        noteMap(perf->cbawCoarsenRejectedNoGainByKind, tag);
                        noteMap(perf->cbawCoarsenRejectedNoGainByTag, tag);
                    }
                    return;
                }
                const std::size_t lhsSize = lhs < sizes.size() ? sizes[lhs] : 0;
                const std::size_t rhsSize = rhs < sizes.size() ? sizes[rhs] : 0;
                const std::size_t mergedOps = lhsSize + rhsSize;
                Candidate candidate{
                    lhs,
                    rhs,
                    delta,
                    clusterEdgeWeight(valueEdges, lhs, rhs) + clusterEdgeWeight(valueEdges, rhs, lhs),
                    mergedOps,
                    maxOps == std::numeric_limits<std::size_t>::max() || mergedOps >= maxOps
                        ? std::size_t{0}
                        : maxOps - mergedOps,
                    tag,
                    cbawChoosePrimaryTag(tag),
                    "exact_delta",
                };
                const std::uint64_t packed = packClusterPair(lhs, rhs);
                auto it = bestByPair.find(packed);
                if (it == bestByPair.end())
                {
                    bestByPair[packed] = candidate;
                    return;
                }
                it->second.tags |= tag;
                it->second.primaryTag = cbawChoosePrimaryTag(it->second.tags);
                it->second.selectedReason =
                    cbawSemanticTieBreakScore(it->second.tags) != 0 ? "exact_delta_semantic_tie"
                                                                    : "exact_delta";
            };

            for (const auto &[packed, weight] : valueEdges.weights)
            {
                const uint32_t from = static_cast<uint32_t>(packed >> 32);
                const uint32_t to = static_cast<uint32_t>(packed & 0xffffffffu);
                if (weight == 0)
                {
                    continue;
                }
                addCandidateTag(from, to, kCbawTagHeavyValueUse);
                if (from < view.succs.size() && view.succs[from].size() == 1)
                {
                    addCandidateTag(from, to, kCbawTagPlainOut1);
                }
                if (to < view.preds.size() && view.preds[to].size() == 1)
                {
                    addCandidateTag(from, to, kCbawTagPlainIn1);
                }
                if (from < labels.size() && to < labels.size())
                {
                    if (sortedStringVectorsIntersect(labels[from].aggregateFamilies,
                                                     labels[to].aggregateFamilies))
                    {
                        addCandidateTag(from, to, kCbawTagAggregateHint);
                    }
                    if (labels[from].mffcRep != kInvalidActivitySupernodeId &&
                        labels[from].mffcRep == labels[to].mffcRep)
                    {
                        addCandidateTag(from, to, kCbawTagMffcDominance);
                    }
                    if (sortedStringVectorsIntersect(labels[from].guardDomains,
                                                     labels[to].guardDomains))
                    {
                        addCandidateTag(from, to, kCbawTagGuardHint);
                    }
                    if (!labels[from].multiSink &&
                        !labels[to].multiSink &&
                        sortedStringVectorsIntersect(labels[from].sinkLabels,
                                                     labels[to].sinkLabels))
                    {
                        addCandidateTag(from, to, kCbawTagSinkCone);
                    }
                    if (labels[from].passthrough && labels[to].passthrough)
                    {
                        addCandidateTag(from, to, kCbawTagPassthrough);
                    }
                }
            }

            std::unordered_map<std::uint64_t, std::vector<std::vector<uint32_t>>> siblingBuckets;
            siblingBuckets.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (clusterId >= view.preds.size() || view.preds[clusterId].empty())
                {
                    continue;
                }
                auto &bucket = siblingBuckets[nodeSiblingPredHash(view.preds[clusterId])];
                auto groupIt = std::find_if(bucket.begin(),
                                            bucket.end(),
                                            [&](const auto &group)
                                            {
                                                return !group.empty() &&
                                                       view.preds[group.front()] == view.preds[clusterId];
                                            });
                if (groupIt == bucket.end())
                {
                    bucket.push_back(std::vector<uint32_t>{clusterId});
                }
                else
                {
                    groupIt->push_back(clusterId);
                }
            }
            for (auto &[_, bucket] : siblingBuckets)
            {
                for (auto &siblings : bucket)
                {
                    if (siblings.size() < 2)
                    {
                        continue;
                    }
                    std::sort(siblings.begin(), siblings.end());
                    for (std::size_t i = 1; i < siblings.size(); ++i)
                    {
                        addCandidateTag(siblings[i - 1], siblings[i], kCbawTagPlainSiblings);
                    }
                }
            }

            std::vector<Candidate> candidates;
            candidates.reserve(bestByPair.size());
            for (auto &[_, candidate] : bestByPair)
            {
                candidate.primaryTag = cbawChoosePrimaryTag(candidate.tags);
                if (perf != nullptr)
                {
                    noteMap(perf->cbawCoarsenDedupSelectedByKind, candidate.primaryTag);
                    ++perf->cbawCoarsenSelectedReason[std::string(candidate.selectedReason)];
                    forEachCbawTag(candidate.tags,
                                   [&](uint32_t tag)
                                   {
                                       if (tag != candidate.primaryTag)
                                       {
                                           noteMap(perf->cbawCoarsenDedupLostTagByKind, tag);
                                       }
                                   });
                }
                candidates.push_back(candidate);
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [&](const Candidate &lhs, const Candidate &rhs)
                      {
                          return betterCandidate(lhs, rhs);
                      });
            if (perf != nullptr)
            {
                perf->cbawCoarsenEvaluateMs += elapsedMs(evaluateStart);
            }
            if (candidates.empty())
            {
                return false;
            }

            std::vector<uint8_t> used(view.members.size(), 0);
            std::vector<Candidate> acceptedCandidates;
            acceptedCandidates.reserve(candidates.size());
            constexpr std::size_t kMaxCbawMergesPerRound = 65536;
            std::vector<uint32_t> clusterRank(view.members.size(), kInvalidActivitySupernodeId);
            for (uint32_t rank = 0; rank < view.members.size(); ++rank)
            {
                clusterRank[rank] = rank;
            }
            struct DirectEdgeInfo
            {
                bool oneWay = false;
                uint32_t source = kInvalidActivitySupernodeId;
                uint32_t target = kInvalidActivitySupernodeId;
            };
            const auto directEdgeInfo = [&](uint32_t lhs, uint32_t rhs) {
                const bool lhsToRhs = lhs < view.succs.size() &&
                                      std::binary_search(view.succs[lhs].begin(),
                                                         view.succs[lhs].end(),
                                                         rhs);
                const bool rhsToLhs = rhs < view.succs.size() &&
                                      std::binary_search(view.succs[rhs].begin(),
                                                         view.succs[rhs].end(),
                                                         lhs);
                if (lhsToRhs == rhsToLhs)
                {
                    return DirectEdgeInfo{};
                }
                return DirectEdgeInfo{
                    .oneWay = true,
                    .source = lhsToRhs ? lhs : rhs,
                    .target = lhsToRhs ? rhs : lhs,
                };
            };
            const auto adjacentInClusterTopo = [&](uint32_t lhs, uint32_t rhs) {
                if (lhs >= clusterRank.size() || rhs >= clusterRank.size())
                {
                    return false;
                }
                const uint32_t lhsRank = clusterRank[lhs];
                const uint32_t rhsRank = clusterRank[rhs];
                if (lhsRank == kInvalidActivitySupernodeId ||
                    rhsRank == kInvalidActivitySupernodeId)
                {
                    return false;
                }
                return lhsRank + 1 == rhsRank || rhsRank + 1 == lhsRank;
            };
            const auto locallySafeDirectContraction = [&](const DirectEdgeInfo &edge) {
                if (!edge.oneWay ||
                    edge.source >= view.succs.size() ||
                    edge.target >= view.preds.size())
                {
                    return false;
                }
                return view.succs[edge.source].size() == 1 ||
                       view.preds[edge.target].size() == 1;
            };
            const auto locallySafeNonDirectContraction = [&](const Candidate &candidate) {
                const uint32_t lhs = candidate.lhs;
                const uint32_t rhs = candidate.rhs;
                if ((candidate.tags & kCbawTagPlainSiblings) == 0 ||
                    lhs >= view.preds.size() ||
                    rhs >= view.preds.size())
                {
                    return false;
                }
                return view.preds[lhs] == view.preds[rhs];
            };
            const auto noteCandidateTags = [&](auto &map, const Candidate &candidate)
            {
                forEachCbawTag(candidate.tags, [&](uint32_t tag) { noteMap(map, tag); });
            };
            for (const auto &candidate : candidates)
            {
                if (perf != nullptr)
                {
                    ++perf->cbawCoarsenEvaluated;
                    noteMap(perf->cbawCoarsenEvaluatedByKind, candidate.primaryTag);
                }
                const uint32_t lhs = candidate.lhs;
                const uint32_t rhs = candidate.rhs;
                if (lhs >= view.members.size() ||
                    rhs >= view.members.size() ||
                    used[lhs] != 0 ||
                    used[rhs] != 0)
                {
                    if (perf != nullptr)
                    {
                        ++perf->cbawCoarsenStale;
                        noteMap(perf->cbawCoarsenStaleByKind, candidate.primaryTag);
                        noteCandidateTags(perf->cbawCoarsenStaleByTag, candidate);
                    }
                    continue;
                }
                const std::size_t lhsSize = lhs < sizes.size() ? sizes[lhs] : 0;
                const std::size_t rhsSize = rhs < sizes.size() ? sizes[rhs] : 0;
                if (maxOps != std::numeric_limits<std::size_t>::max() &&
                    lhsSize + rhsSize > maxOps)
                {
                    if (perf != nullptr)
                    {
                        ++perf->cbawCoarsenRejectedResource;
                        noteMap(perf->cbawCoarsenRejectedResourceByKind, candidate.primaryTag);
                        noteCandidateTags(perf->cbawCoarsenRejectedResourceByTag, candidate);
                    }
                    continue;
                }
                const DirectEdgeInfo edge = directEdgeInfo(lhs, rhs);
                const bool locallyCycleSafe =
                    edge.oneWay
                        ? (locallySafeDirectContraction(edge) || adjacentInClusterTopo(lhs, rhs))
                        : locallySafeNonDirectContraction(candidate);
                if (!locallyCycleSafe)
                {
                    if (perf != nullptr)
                    {
                        ++perf->cbawCoarsenRejectedCycle;
                        noteMap(perf->cbawCoarsenRejectedCycleByKind, candidate.primaryTag);
                        noteCandidateTags(perf->cbawCoarsenRejectedCycleByTag, candidate);
                    }
                    continue;
                }
                used[lhs] = 1;
                used[rhs] = 1;
                acceptedCandidates.push_back(candidate);
                if (acceptedCandidates.size() >= kMaxCbawMergesPerRound)
                {
                    break;
                }
            }
            if (acceptedCandidates.empty())
            {
                return false;
            }

            const auto buildTrialForPrefix = [&](std::size_t acceptedCount) {
                DisjointSet dsu(view.members.size());
                for (std::size_t i = 0; i < acceptedCount; ++i)
                {
                    dsu.unite(acceptedCandidates[i].lhs, acceptedCandidates[i].rhs);
                }

                std::unordered_map<uint32_t, uint32_t> rootToCluster;
                std::vector<std::vector<uint32_t>> trial;
                trial.reserve(view.members.size() - acceptedCount);
                for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
                {
                    const uint32_t root = dsu.find(clusterId);
                    auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(trial.size()));
                    if (inserted)
                    {
                        trial.push_back({});
                    }
                    trial[it->second].insert(trial[it->second].end(),
                                             view.members[clusterId].begin(),
                                             view.members[clusterId].end());
                }
                return canonicalizeNodeClusters(std::move(trial), nodeTopoPos);
            };

            const auto topoStart = std::chrono::steady_clock::now();
            std::vector<std::vector<uint32_t>> trial;
            std::size_t acceptedCount = acceptedCandidates.size();
            while (acceptedCount != 0)
            {
                trial = buildTrialForPrefix(acceptedCount);
                if (orderNodeClustersTopologically(trial, nodeDag, nodeCount, &rewrite, &graph))
                {
                    break;
                }
                if (perf != nullptr)
                {
                    for (std::size_t i = acceptedCount / 2; i < acceptedCount; ++i)
                    {
                        ++perf->cbawCoarsenRejectedCycle;
                        noteMap(perf->cbawCoarsenRejectedCycleByKind,
                                acceptedCandidates[i].primaryTag);
                        noteCandidateTags(perf->cbawCoarsenRejectedCycleByTag,
                                          acceptedCandidates[i]);
                    }
                }
                acceptedCount /= 2;
            }
            if (acceptedCount == 0)
            {
                if (perf != nullptr)
                {
                    perf->cbawCoarsenTopoMs += elapsedMs(topoStart);
                }
                return false;
            }
            if (perf != nullptr)
            {
                perf->cbawCoarsenTopoMs += elapsedMs(topoStart);
                for (std::size_t i = 0; i < acceptedCount; ++i)
                {
                    ++perf->cbawCoarsenMerges;
                    noteMap(perf->cbawCoarsenAcceptedByKind,
                            acceptedCandidates[i].primaryTag);
                    noteCandidateTags(perf->cbawCoarsenAcceptedByTag,
                                      acceptedCandidates[i]);
                }
            }
            clusters = std::move(trial);
            return true;
        }

        double probCoarsenCheckCost(double activeProb, const ActivityScheduleOptions &options)
        {
            const double p = std::clamp(activeProb, 0.0, 1.0);
            if (p <= 0.0 || p >= 1.0)
            {
                return 0.0;
            }
            double cost = 1.0;
            if (p >= 0.2 && p <= 0.8)
            {
                cost += std::max(0.0, options.cBpMiss);
            }
            return cost;
        }

        double probCoarsenPhi(double weight, double changeWeight, double activeProb)
        {
            const double denom = weight * activeProb;
            if (denom <= 1e-12)
            {
                return changeWeight <= 1e-12 ? 1.0 : 0.0;
            }
            return changeWeight / denom;
        }

        bool tryMergeNodeProb(std::vector<std::vector<uint32_t>> &clusters,
                              const std::vector<std::vector<uint32_t>> &nodeDag,
                              std::size_t nodeCount,
                              const std::vector<uint32_t> &nodeTopoPos,
                              const std::vector<uint32_t> &nodeOpSizes,
                              std::size_t maxOps,
                              const ComputeRewriteBuild &rewrite,
                              const wolvrix::lib::grh::Graph &graph,
                              const ActivityScheduleOptions &options,
                              const ActivityOpData &opData,
                              const ActivityCostModel &costModel,
                              const std::vector<float> &piByOpIndex,
                              const ActivityHypergraphAggregate *probSeedHypergraph,
                              ComputeNodeMaterializePerfStats *perf)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            if (view.members.size() < 2)
            {
                return false;
            }

            const bool canUseSeed =
                probSeedHypergraph != nullptr &&
                probSeedHypergraph->nodeWeight.size() >= nodeCount &&
                probSeedHypergraph->nodeChangeWeight.size() >= nodeCount &&
                probSeedHypergraph->nodeFootprintBytes.size() >= nodeCount &&
                probSeedHypergraph->nodeActiveProb.size() >= nodeCount &&
                probSeedHypergraph->nodeOpCount.size() >= nodeCount;
            const auto aggregateStart = std::chrono::steady_clock::now();
            const ProbCoarsenClusterAggregate probAgg =
                canUseSeed
                    ? buildProbCoarsenClusterAggregateFromSeed(*probSeedHypergraph, view)
                    : buildProbCoarsenClusterAggregateFromGraph(graph,
                                                               options,
                                                               opData,
                                                               rewrite,
                                                               view,
                                                               costModel,
                                                               piByOpIndex);
            if (perf)
            {
                perf->probCoarsenAggregateMs += elapsedMs(aggregateStart);
                if (canUseSeed)
                {
                    ++perf->probCoarsenSeedAggregates;
                }
                else
                {
                    ++perf->probCoarsenFullAggregates;
                }
            }
            const auto &agg = probAgg.aggregate;
            const auto &edgeProb = probAgg.edgeProb;
            const auto edgeTotalProb = [&](uint32_t from, uint32_t to) -> double {
                const auto it = edgeProb.find(packClusterPair(from, to));
                return it == edgeProb.end() ? 0.0 : it->second;
            };

            struct Candidate
            {
                uint32_t lhs = 0;
                uint32_t rhs = 0;
                double gain = 0.0;
                double phi = 0.0;
                double active = 0.0;
                std::uint64_t footprint = 0;
                std::size_t ops = 0;
                bool sibling = false;
            };

            std::vector<Candidate> candidates;
            candidates.reserve(view.members.size());

            const auto rejectSize = [&]() {
                if (perf)
                {
                    ++perf->probCoarsenRejectedSize;
                }
            };
            const auto rejectFootprint = [&]() {
                if (perf)
                {
                    ++perf->probCoarsenRejectedFootprint;
                }
            };
            const auto rejectPhi = [&]() {
                if (perf)
                {
                    ++perf->probCoarsenRejectedPhi;
                }
            };
            const auto rejectWeight = [&]() {
                if (perf)
                {
                    ++perf->probCoarsenRejectedWeight;
                }
            };

            const auto considerCandidate = [&](uint32_t lhs, uint32_t rhs, bool sibling) {
                if (lhs == rhs || lhs >= view.members.size() || rhs >= view.members.size())
                {
                    return;
                }
                if (rhs < lhs)
                {
                    std::swap(lhs, rhs);
                }
                const std::size_t lhsOps = clusterOpSize(view.members[lhs], nodeOpSizes);
                const std::size_t rhsOps = clusterOpSize(view.members[rhs], nodeOpSizes);
                const std::size_t mergedOps = lhsOps + rhsOps;
                if (maxOps != std::numeric_limits<std::size_t>::max() && mergedOps > maxOps)
                {
                    rejectSize();
                    return;
                }

                const double lhsWeight = lhs < agg.nodeWeight.size() ? agg.nodeWeight[lhs] : 0.0;
                const double rhsWeight = rhs < agg.nodeWeight.size() ? agg.nodeWeight[rhs] : 0.0;
                const double lhsChange = lhs < agg.nodeChangeWeight.size() ? agg.nodeChangeWeight[lhs] : 0.0;
                const double rhsChange = rhs < agg.nodeChangeWeight.size() ? agg.nodeChangeWeight[rhs] : 0.0;
                const double lhsActive = lhs < agg.nodeActiveProb.size() ? agg.nodeActiveProb[lhs] : 0.0;
                const double rhsActive = rhs < agg.nodeActiveProb.size() ? agg.nodeActiveProb[rhs] : 0.0;
                const std::uint64_t lhsFootprint =
                    lhs < agg.nodeFootprintBytes.size() ? agg.nodeFootprintBytes[lhs] : 0;
                const std::uint64_t rhsFootprint =
                    rhs < agg.nodeFootprintBytes.size() ? agg.nodeFootprintBytes[rhs] : 0;
                const std::uint64_t mergedFootprint = lhsFootprint + rhsFootprint;
                if (options.footprintMaxBytes != 0 && mergedFootprint > options.footprintMaxBytes)
                {
                    rejectFootprint();
                    return;
                }

                const double mergedWeight = lhsWeight + rhsWeight;
                if (maxOps != std::numeric_limits<std::size_t>::max() &&
                    mergedWeight > static_cast<double>(maxOps))
                {
                    rejectWeight();
                    return;
                }
                const double mergedChange = lhsChange + rhsChange;
                const double mergedActive = std::max(std::clamp(lhsActive, 0.0, 1.0),
                                                     std::clamp(rhsActive, 0.0, 1.0));
                const double phi = probCoarsenPhi(mergedWeight, mergedChange, mergedActive);
                const double phiThreshold = mergedActive >= std::clamp(options.piHighThreshold, 0.0, 1.0)
                                                ? std::max(0.0, options.phiMin * 0.5)
                                                : std::max(0.0, options.phiMin);
                if (phi + 1e-12 < phiThreshold)
                {
                    rejectPhi();
                    return;
                }

                const double directSaved =
                    edgeTotalProb(lhs, rhs) * rhsWeight + edgeTotalProb(rhs, lhs) * lhsWeight;
                const double checkSaved =
                    probCoarsenCheckCost(lhsActive, options) +
                    probCoarsenCheckCost(rhsActive, options) -
                    probCoarsenCheckCost(mergedActive, options);
                const double beforeWork = lhsActive * lhsWeight + rhsActive * rhsWeight;
                const double afterWork = mergedActive * mergedWeight;
                const double gain = directSaved + checkSaved - std::max(0.0, afterWork - beforeWork);
                const bool highActivity = mergedActive >= std::clamp(options.piHighThreshold, 0.0, 1.0);
                if (gain < (highActivity ? 0.0 : 1e-12))
                {
                    return;
                }
                candidates.push_back(Candidate{
                    lhs,
                    rhs,
                    gain,
                    phi,
                    mergedActive,
                    mergedFootprint,
                    mergedOps,
                    sibling,
                });
            };

            if (options.enableChainMerge)
            {
                for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
                {
                    if (clusterId >= view.succs.size() || view.succs[clusterId].size() != 1)
                    {
                        continue;
                    }
                    const uint32_t succ = view.succs[clusterId].front();
                    if (succ < view.preds.size() && view.preds[succ].size() == 1 &&
                        view.preds[succ].front() == clusterId)
                    {
                        considerCandidate(clusterId, succ, false);
                    }
                }
            }

            std::unordered_map<std::uint64_t, std::vector<std::vector<uint32_t>>> buckets;
            buckets.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (view.preds[clusterId].empty())
                {
                    continue;
                }
                auto &bucket = buckets[nodeSiblingPredHash(view.preds[clusterId])];
                auto groupIt = std::find_if(bucket.begin(),
                                            bucket.end(),
                                            [&](const auto &group)
                                            {
                                                return !group.empty() &&
                                                       view.preds[group.front()] == view.preds[clusterId];
                                            });
                if (groupIt == bucket.end())
                {
                    bucket.push_back(std::vector<uint32_t>{clusterId});
                }
                else
                {
                    groupIt->push_back(clusterId);
                }
            }
            for (auto &[_, bucket] : buckets)
            {
                for (auto &siblings : bucket)
                {
                    if (siblings.size() < 2)
                    {
                        continue;
                    }
                    std::sort(siblings.begin(), siblings.end());
                    for (std::size_t i = 1; i < siblings.size(); ++i)
                    {
                        considerCandidate(siblings[i - 1], siblings[i], true);
                    }
                }
            }

            if (perf)
            {
                perf->probCoarsenCandidates += candidates.size();
            }
            if (candidates.empty())
            {
                return false;
            }

            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const Candidate &lhs, const Candidate &rhs)
                      {
                          if (lhs.gain != rhs.gain)
                          {
                              return lhs.gain > rhs.gain;
                          }
                          if (lhs.sibling != rhs.sibling)
                          {
                              return lhs.sibling > rhs.sibling;
                          }
                          if (lhs.phi != rhs.phi)
                          {
                              return lhs.phi > rhs.phi;
                          }
                          if (lhs.footprint != rhs.footprint)
                          {
                              return lhs.footprint < rhs.footprint;
                          }
                          if (lhs.lhs != rhs.lhs)
                          {
                              return lhs.lhs < rhs.lhs;
                          }
                          return lhs.rhs < rhs.rhs;
                      });

            DisjointSet dsu(view.members.size());
            std::vector<uint8_t> used(view.members.size(), 0);
            bool changed = false;
            std::size_t acceptedMerges = 0;
            std::size_t acceptedSiblingMerges = 0;
            std::size_t acceptedChainMerges = 0;
            double acceptedGain = 0.0;
            for (const auto &candidate : candidates)
            {
                uint32_t lhs = dsu.find(candidate.lhs);
                uint32_t rhs = dsu.find(candidate.rhs);
                if (lhs == rhs || used[candidate.lhs] != 0 || used[candidate.rhs] != 0)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    used[candidate.lhs] = 1;
                    used[candidate.rhs] = 1;
                    changed = true;
                    ++acceptedMerges;
                    acceptedGain += candidate.gain;
                    if (candidate.sibling)
                    {
                        ++acceptedSiblingMerges;
                    }
                    else
                    {
                        ++acceptedChainMerges;
                    }
                }
            }
            if (!changed)
            {
                return false;
            }

            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            out.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                const uint32_t root = dsu.find(clusterId);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(),
                                       view.members[clusterId].begin(),
                                       view.members[clusterId].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                if (perf)
                {
                    ++perf->probCoarsenRejectedCycle;
                }
                return false;
            }
            if (perf)
            {
                perf->probCoarsenMerges += acceptedMerges;
                perf->probCoarsenTotalGain += acceptedGain;
                perf->coarsenSiblingMerges += acceptedSiblingMerges;
                perf->coarsenOut1Merges += acceptedChainMerges;
            }
            clusters = std::move(out);
            return true;
        }

        std::vector<std::vector<uint32_t>> buildComputeSupernodeSegments(const NodeClusterView &view,
                                                                         const ClusterValueEdges &valueEdges,
                                                                         const std::vector<uint32_t> &nodeOpSizes,
                                                                         std::size_t maxNodes,
                                                                         const std::vector<double> *valueWeights,
                                                                         double segmentPenalty)
        {
            const std::size_t count = view.members.size();
            if (count == 0)
            {
                return {};
            }

            std::vector<std::size_t> prefixSize(count + 1, 0);
            for (std::size_t i = 0; i < count; ++i)
            {
                prefixSize[i + 1] = prefixSize[i] + clusterOpSize(view.members[i], nodeOpSizes);
            }

            auto segmentSize = [&](std::size_t begin, std::size_t end) {
                return prefixSize[end] - prefixSize[begin];
            };

            constexpr double kInf = std::numeric_limits<double>::infinity();
            std::vector<double> dp(count + 1, kInf);
            std::vector<std::size_t> prev(count + 1, 0);
            std::vector<uint32_t> targetSeen(valueEdges.valueFanouts.size(), 0);
            std::vector<uint32_t> countedIncoming(valueEdges.valueFanouts.size(), 0);
            const auto fanoutWeight = [&](uint32_t valueId) -> double {
                if (valueWeights != nullptr && valueId < valueWeights->size())
                {
                    return std::max(0.0, (*valueWeights)[valueId]);
                }
                return 1.0;
            };
            dp[0] = 0;
            for (std::size_t end = 1; end <= count; ++end)
            {
                const uint32_t stamp = static_cast<uint32_t>(end);
                double incomingActivationCost = 0.0;
                for (std::size_t begin = end; begin > 0; --begin)
                {
                    const std::size_t start = begin - 1;
                    const std::size_t size = segmentSize(start, end);
                    if (size > maxNodes && start + 1 < end)
                    {
                        break;
                    }
                    if (size > maxNodes && start + 1 == end)
                    {
                        continue;
                    }
                    if (start < valueEdges.targetValuesByCluster.size())
                    {
                        for (const auto valueId : valueEdges.targetValuesByCluster[start])
                        {
                            if (valueId >= valueEdges.valueFanouts.size() || targetSeen[valueId] == stamp)
                            {
                                continue;
                            }
                            targetSeen[valueId] = stamp;
                            if (valueEdges.valueFanouts[valueId].sourceCluster < start)
                            {
                                countedIncoming[valueId] = stamp;
                                incomingActivationCost += fanoutWeight(valueId);
                            }
                        }
                    }
                    if (start < valueEdges.sourceValuesByCluster.size())
                    {
                        for (const auto valueId : valueEdges.sourceValuesByCluster[start])
                        {
                            if (valueId < countedIncoming.size() && countedIncoming[valueId] == stamp)
                            {
                                countedIncoming[valueId] = 0;
                                incomingActivationCost -= fanoutWeight(valueId);
                            }
                        }
                    }
                    if (dp[start] == kInf)
                    {
                        continue;
                    }
                    const double candidate = dp[start] + incomingActivationCost + segmentPenalty;
                    if (candidate + 1e-12 < dp[end] ||
                        (std::fabs(candidate - dp[end]) <= 1e-12 && (end - start) > (end - prev[end])))
                    {
                        dp[end] = candidate;
                        prev[end] = start;
                    }
                }
                if (dp[end] == kInf)
                {
                    dp[end] = dp[end - 1] + 1;
                    prev[end] = end - 1;
                }
            }

            std::vector<std::pair<std::size_t, std::size_t>> ranges;
            for (std::size_t end = count; end > 0;)
            {
                const std::size_t begin = prev[end];
                ranges.emplace_back(begin, end);
                end = begin;
            }
            std::reverse(ranges.begin(), ranges.end());

            std::vector<std::vector<uint32_t>> segments;
            segments.reserve(ranges.size());
            for (const auto &[begin, end] : ranges)
            {
                std::vector<uint32_t> segment;
                segment.reserve(end - begin);
                for (std::size_t cluster = begin; cluster < end; ++cluster)
                {
                    segment.push_back(static_cast<uint32_t>(cluster));
                }
                segments.push_back(std::move(segment));
            }
            return segments;
        }

        std::vector<double> buildProbSegmentValueWeights(const wolvrix::lib::grh::Graph &graph,
                                                         const ActivityScheduleOptions &options,
                                                         const ComputeRewriteBuild &rewrite,
                                                         const ActivityCostModel &costModel,
                                                         const ClusterValueEdges &valueEdges,
                                                         const std::vector<float> &piByOpIndex)
        {
            std::vector<double> weights(valueEdges.valueFanouts.size(), 1.0);
            for (uint32_t valueId = 0; valueId < valueEdges.fanoutValues.size(); ++valueId)
            {
                const auto value = valueEdges.fanoutValues[valueId];
                const double pi = std::max(0.0,
                                           activityPiForValue(graph,
                                                              options,
                                                              rewrite.canonicalValues,
                                                              piByOpIndex,
                                                              value));
                const auto canonical = canonicalActivityValue(value, &rewrite.canonicalValues);
                const auto defOp = graph.valueDef(canonical);
                const double sourceWeight =
                    defOp.valid() ? activityComputeWeightForOp(graph, rewrite.canonicalValues, costModel, defOp)
                                  : static_cast<double>(activityComputeUnitsForWidth(graph.valueWidth(canonical)));
                const double changeWeight = std::max(0.0, sourceWeight) * pi;
                if (options.probDpCostMode == "pi")
                {
                    weights[valueId] = pi;
                }
                else if (options.probDpCostMode == "change")
                {
                    weights[valueId] = changeWeight;
                }
                else if (options.probDpCostMode == "mixed-change")
                {
                    weights[valueId] = 1.0 + std::max(0.0, options.probDpAlpha) * changeWeight;
                }
                else
                {
                    weights[valueId] = 1.0 + std::max(0.0, options.probDpAlpha) * pi;
                }
            }
            return weights;
        }

        struct FmSegmentStats
        {
            std::size_t ops = 0;
            std::uint64_t footprint = 0;
            double weight = 0.0;
            double changeWeight = 0.0;
            double activeProb = 0.0;
        };

        bool probFmSegmentPhiOk(const FmSegmentStats &before,
                                const FmSegmentStats &after,
                                const ActivityScheduleOptions &options)
        {
            if (after.ops == 0)
            {
                return false;
            }
            const double threshold = after.activeProb >= std::clamp(options.piHighThreshold, 0.0, 1.0)
                                         ? std::max(0.0, options.phiMin * 0.5)
                                         : std::max(0.0, options.phiMin);
            const double beforePhi = probCoarsenPhi(before.weight, before.changeWeight, before.activeProb);
            const double afterPhi = probCoarsenPhi(after.weight, after.changeWeight, after.activeProb);
            if (beforePhi + 1e-12 < threshold)
            {
                return afterPhi + 1e-12 >= beforePhi;
            }
            return afterPhi + 1e-12 >= threshold;
        }

        bool probFmMovePreservesSegmentTopo(const NodeClusterView &view,
                                            const std::vector<uint32_t> &ownerByCluster,
                                            uint32_t clusterId,
                                            uint32_t newSegment)
        {
            if (clusterId >= ownerByCluster.size())
            {
                return false;
            }
            for (const uint32_t pred : view.preds[clusterId])
            {
                if (pred >= ownerByCluster.size())
                {
                    continue;
                }
                const uint32_t predSegment = ownerByCluster[pred];
                if (predSegment == kInvalidActivitySupernodeId || predSegment == newSegment)
                {
                    continue;
                }
                if (predSegment >= newSegment)
                {
                    return false;
                }
            }
            for (const uint32_t succ : view.succs[clusterId])
            {
                if (succ >= ownerByCluster.size())
                {
                    continue;
                }
                const uint32_t succSegment = ownerByCluster[succ];
                if (succSegment == kInvalidActivitySupernodeId || succSegment == newSegment)
                {
                    continue;
                }
                if (newSegment >= succSegment)
                {
                    return false;
                }
            }
            return true;
        }

        double probFmMoveGain(const ClusterValueEdges &valueEdges,
                              const std::vector<double> &valueWeights,
                              const std::vector<uint32_t> &ownerByCluster,
                              uint32_t clusterId,
                              uint32_t oldSegment,
                              uint32_t newSegment)
        {
            double gain = 0.0;
            const auto valueWeight = [&](uint32_t valueId) -> double {
                if (valueId < valueWeights.size())
                {
                    return std::max(0.0, valueWeights[valueId]);
                }
                return 1.0;
            };
            const auto owner = [&](uint32_t cluster) -> uint32_t {
                return cluster < ownerByCluster.size() ? ownerByCluster[cluster] : kInvalidActivitySupernodeId;
            };
            if (clusterId < valueEdges.sourceValuesByCluster.size())
            {
                for (const uint32_t valueId : valueEdges.sourceValuesByCluster[clusterId])
                {
                    if (valueId >= valueEdges.valueFanouts.size())
                    {
                        continue;
                    }
                    const double weight = valueWeight(valueId);
                    for (const uint32_t targetCluster : valueEdges.valueFanouts[valueId].targetClusters)
                    {
                        const uint32_t targetSegment = owner(targetCluster);
                        if (targetSegment == kInvalidActivitySupernodeId)
                        {
                            continue;
                        }
                        const double oldCut = targetSegment == oldSegment ? 0.0 : weight;
                        const double newCut = targetSegment == newSegment ? 0.0 : weight;
                        gain += oldCut - newCut;
                    }
                }
            }
            if (clusterId < valueEdges.targetValuesByCluster.size())
            {
                for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                {
                    if (valueId >= valueEdges.valueFanouts.size())
                    {
                        continue;
                    }
                    const uint32_t sourceSegment = owner(valueEdges.valueFanouts[valueId].sourceCluster);
                    if (sourceSegment == kInvalidActivitySupernodeId)
                    {
                        continue;
                    }
                    const double weight = valueWeight(valueId);
                    const double oldCut = sourceSegment == oldSegment ? 0.0 : weight;
                    const double newCut = sourceSegment == newSegment ? 0.0 : weight;
                    gain += oldCut - newCut;
                }
            }
            return gain;
        }

        std::vector<std::vector<uint32_t>> refineComputeSupernodeSegmentsProb(
            const NodeClusterView &view,
            const ClusterValueEdges &valueEdges,
            const std::vector<uint32_t> &nodeOpSizes,
            std::vector<std::vector<uint32_t>> segments,
            const ActivityHypergraphAggregate &clusterAgg,
            const std::vector<double> &valueWeights,
            const ActivityScheduleOptions &options,
            std::size_t maxOps,
            ComputeNodeMaterializePerfStats *perf)
        {
            if (segments.size() < 2 || options.fmRefineMaxRounds == 0)
            {
                return segments;
            }
            std::vector<uint32_t> ownerByCluster(view.members.size(), kInvalidActivitySupernodeId);
            auto rebuildOwners = [&]() {
                std::fill(ownerByCluster.begin(), ownerByCluster.end(), kInvalidActivitySupernodeId);
                for (uint32_t segmentId = 0; segmentId < segments.size(); ++segmentId)
                {
                    for (const uint32_t clusterId : segments[segmentId])
                    {
                        if (clusterId < ownerByCluster.size())
                        {
                            ownerByCluster[clusterId] = segmentId;
                        }
                    }
                }
            };
            rebuildOwners();

            std::vector<std::size_t> clusterOps(view.members.size(), 0);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                clusterOps[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
            }

            std::vector<FmSegmentStats> segmentStats(segments.size());
            auto addClusterToStats = [&](FmSegmentStats &stats, uint32_t clusterId) {
                if (clusterId >= view.members.size())
                {
                    return;
                }
                stats.ops += clusterOps[clusterId];
                if (clusterId < clusterAgg.nodeFootprintBytes.size())
                {
                    stats.footprint += clusterAgg.nodeFootprintBytes[clusterId];
                }
                if (clusterId < clusterAgg.nodeWeight.size())
                {
                    stats.weight += clusterAgg.nodeWeight[clusterId];
                }
                if (clusterId < clusterAgg.nodeChangeWeight.size())
                {
                    stats.changeWeight += clusterAgg.nodeChangeWeight[clusterId];
                }
                if (clusterId < clusterAgg.nodeActiveProb.size())
                {
                    stats.activeProb =
                        std::max(stats.activeProb, std::clamp(clusterAgg.nodeActiveProb[clusterId], 0.0, 1.0));
                }
            };
            auto removeClusterFromStats = [&](FmSegmentStats stats, uint32_t clusterId) {
                if (clusterId < clusterOps.size())
                {
                    stats.ops = stats.ops >= clusterOps[clusterId] ? stats.ops - clusterOps[clusterId] : 0;
                }
                if (clusterId < clusterAgg.nodeFootprintBytes.size())
                {
                    stats.footprint = stats.footprint >= clusterAgg.nodeFootprintBytes[clusterId]
                                          ? stats.footprint - clusterAgg.nodeFootprintBytes[clusterId]
                                          : 0;
                }
                if (clusterId < clusterAgg.nodeWeight.size())
                {
                    stats.weight -= clusterAgg.nodeWeight[clusterId];
                }
                if (clusterId < clusterAgg.nodeChangeWeight.size())
                {
                    stats.changeWeight -= clusterAgg.nodeChangeWeight[clusterId];
                }
                stats.weight = std::max(0.0, stats.weight);
                stats.changeWeight = std::max(0.0, stats.changeWeight);
                // Keep the old segment active probability as a conservative upper bound. Recomputing the
                // exact max after each tentative move would require scanning the whole segment.
                return stats;
            };
            for (uint32_t segmentId = 0; segmentId < segments.size(); ++segmentId)
            {
                for (const uint32_t clusterId : segments[segmentId])
                {
                    addClusterToStats(segmentStats[segmentId], clusterId);
                }
            }

            struct Candidate
            {
                uint32_t cluster = 0;
                uint32_t from = 0;
                uint32_t to = 0;
                double gain = 0.0;
            };

            for (std::size_t round = 0; round < options.fmRefineMaxRounds; ++round)
            {
                std::vector<Candidate> candidates;
                std::size_t boundaryClusters = 0;
                for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
                {
                    const uint32_t from = ownerByCluster[clusterId];
                    if (from == kInvalidActivitySupernodeId || from >= segments.size())
                    {
                        continue;
                    }
                    std::vector<uint32_t> dests;
                    dests.reserve(view.preds[clusterId].size() + view.succs[clusterId].size());
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        if (pred < ownerByCluster.size() && ownerByCluster[pred] != from &&
                            ownerByCluster[pred] != kInvalidActivitySupernodeId)
                        {
                            dests.push_back(ownerByCluster[pred]);
                        }
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        if (succ < ownerByCluster.size() && ownerByCluster[succ] != from &&
                            ownerByCluster[succ] != kInvalidActivitySupernodeId)
                        {
                            dests.push_back(ownerByCluster[succ]);
                        }
                    }
                    if (dests.empty())
                    {
                        continue;
                    }
                    ++boundaryClusters;
                    std::sort(dests.begin(), dests.end());
                    dests.erase(std::unique(dests.begin(), dests.end()), dests.end());
                    for (const uint32_t to : dests)
                    {
                        const double gain =
                            probFmMoveGain(valueEdges, valueWeights, ownerByCluster, clusterId, from, to);
                        if (gain > 1e-12)
                        {
                            candidates.push_back(Candidate{clusterId, from, to, gain});
                        }
                    }
                }
                if (perf)
                {
                    perf->fmRefineCandidates += candidates.size();
                }
                if (candidates.empty())
                {
                    break;
                }
                std::sort(candidates.begin(),
                          candidates.end(),
                          [](const Candidate &lhs, const Candidate &rhs)
                          {
                              if (lhs.gain != rhs.gain)
                              {
                                  return lhs.gain > rhs.gain;
                              }
                              if (lhs.cluster != rhs.cluster)
                              {
                                  return lhs.cluster < rhs.cluster;
                              }
                              return lhs.to < rhs.to;
                          });

                std::vector<uint8_t> movedThisRound(view.members.size(), 0);
                std::size_t roundMoves = 0;
                double roundGain = 0.0;
                for (const Candidate &candidate : candidates)
                {
                    const uint32_t clusterId = candidate.cluster;
                    if (clusterId >= ownerByCluster.size() || movedThisRound[clusterId] != 0)
                    {
                        continue;
                    }
                    const uint32_t from = ownerByCluster[clusterId];
                    const uint32_t to = candidate.to;
                    if (from == kInvalidActivitySupernodeId || to == kInvalidActivitySupernodeId ||
                        from == to || from >= segments.size() || to >= segments.size())
                    {
                        continue;
                    }
                    if (segments[from].size() <= 1)
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedSize;
                        }
                        continue;
                    }
                    const double gain =
                        probFmMoveGain(valueEdges, valueWeights, ownerByCluster, clusterId, from, to);
                    if (gain <= 1e-12)
                    {
                        continue;
                    }
                    const std::size_t ops = clusterId < clusterOps.size() ? clusterOps[clusterId] : 0;
                    if (maxOps != 0 && segmentStats[to].ops + ops > maxOps)
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedSize;
                        }
                        continue;
                    }
                    FmSegmentStats nextFrom = removeClusterFromStats(segmentStats[from], clusterId);
                    FmSegmentStats nextTo = segmentStats[to];
                    addClusterToStats(nextTo, clusterId);
                    if (options.footprintMaxBytes != 0 && nextTo.footprint > options.footprintMaxBytes)
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedFootprint;
                        }
                        continue;
                    }
                    if (maxOps != 0 && nextTo.weight > static_cast<double>(maxOps))
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedWeight;
                        }
                        continue;
                    }
                    if (!probFmSegmentPhiOk(segmentStats[to], nextTo, options) ||
                        !probFmSegmentPhiOk(segmentStats[from], nextFrom, options))
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedPhi;
                        }
                        continue;
                    }
                    if (!probFmMovePreservesSegmentTopo(view, ownerByCluster, clusterId, to))
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedCycle;
                        }
                        continue;
                    }

                    auto &fromMembers = segments[from];
                    const auto eraseIt = std::find(fromMembers.begin(), fromMembers.end(), clusterId);
                    if (eraseIt == fromMembers.end())
                    {
                        continue;
                    }
                    fromMembers.erase(eraseIt);
                    segments[to].push_back(clusterId);
                    std::sort(segments[to].begin(), segments[to].end());
                    ownerByCluster[clusterId] = to;
                    segmentStats[from] = nextFrom;
                    segmentStats[to] = nextTo;
                    movedThisRound[clusterId] = 1;
                    ++roundMoves;
                    roundGain += gain;
                }
                if (roundMoves == 0)
                {
                    break;
                }
                if (perf)
                {
                    ++perf->fmRefineRounds;
                    perf->fmRefineMoves += roundMoves;
                    perf->fmRefineTotalGain += roundGain;
                }
                if (boundaryClusters > 100 && roundMoves * 100 < boundaryClusters)
                {
                    break;
                }
            }

            return segments;
        }

        std::vector<std::vector<uint32_t>> refineComputeSupernodeSegmentsCbaw(
            const NodeClusterView &view,
            const ClusterValueEdges &valueEdges,
            const std::vector<uint32_t> &nodeOpSizes,
            std::vector<std::vector<uint32_t>> segments,
            const ActivityScheduleOptions &options,
            std::size_t maxOps,
            ComputeNodeMaterializePerfStats *perf)
        {
            if (segments.size() < 2 || options.fmRefineMaxRounds == 0)
            {
                return segments;
            }

            std::vector<uint32_t> ownerByCluster(view.members.size(), kInvalidActivitySupernodeId);
            auto rebuildOwners = [&]() {
                std::fill(ownerByCluster.begin(), ownerByCluster.end(), kInvalidActivitySupernodeId);
                for (uint32_t segmentId = 0; segmentId < segments.size(); ++segmentId)
                {
                    for (const uint32_t clusterId : segments[segmentId])
                    {
                        if (clusterId < ownerByCluster.size())
                        {
                            ownerByCluster[clusterId] = segmentId;
                        }
                    }
                }
            };
            rebuildOwners();

            std::vector<std::size_t> clusterOps(view.members.size(), 0);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                clusterOps[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
            }
            std::vector<std::size_t> segmentOps(segments.size(), 0);
            for (uint32_t segmentId = 0; segmentId < segments.size(); ++segmentId)
            {
                for (const uint32_t clusterId : segments[segmentId])
                {
                    if (clusterId < clusterOps.size())
                    {
                        segmentOps[segmentId] += clusterOps[clusterId];
                    }
                }
            }

            std::vector<uint32_t> segmentSeen(segments.size(), 0);
            std::vector<uint32_t> valueSeen(valueEdges.valueFanouts.size(), 0);
            uint32_t segmentStamp = 1;
            uint32_t valueStamp = 1;
            const auto nextStamp = [](std::vector<uint32_t> &seen, uint32_t &stamp) {
                ++stamp;
                if (stamp == 0)
                {
                    std::fill(seen.begin(), seen.end(), 0);
                    stamp = 1;
                }
                return stamp;
            };
            const auto ownerWithMove = [&](uint32_t clusterId,
                                           uint32_t movingCluster,
                                           uint32_t movingTo) {
                if (clusterId == movingCluster)
                {
                    return movingTo;
                }
                return clusterId < ownerByCluster.size() ? ownerByCluster[clusterId]
                                                         : kInvalidActivitySupernodeId;
            };
            const auto valueTargetCountWithMove = [&](uint32_t valueId,
                                                      uint32_t movingCluster,
                                                      uint32_t movingTo) {
                if (valueId >= valueEdges.valueFanouts.size())
                {
                    return std::size_t{0};
                }
                const auto &fanout = valueEdges.valueFanouts[valueId];
                const uint32_t sourceSegment =
                    ownerWithMove(fanout.sourceCluster, movingCluster, movingTo);
                if (sourceSegment == kInvalidActivitySupernodeId)
                {
                    return std::size_t{0};
                }
                const uint32_t stamp = nextStamp(segmentSeen, segmentStamp);
                std::size_t count = 0;
                for (const uint32_t targetCluster : fanout.targetClusters)
                {
                    const uint32_t targetSegment =
                        ownerWithMove(targetCluster, movingCluster, movingTo);
                    if (targetSegment == kInvalidActivitySupernodeId ||
                        targetSegment == sourceSegment ||
                        targetSegment >= segmentSeen.size())
                    {
                        continue;
                    }
                    if (segmentSeen[targetSegment] == stamp)
                    {
                        continue;
                    }
                    segmentSeen[targetSegment] = stamp;
                    ++count;
                }
                return count;
            };
            const auto moveGain = [&](uint32_t clusterId, uint32_t from, uint32_t to) {
                if (clusterId >= ownerByCluster.size() ||
                    from == to ||
                    ownerByCluster[clusterId] != from ||
                    to >= segments.size())
                {
                    return std::int64_t{0};
                }
                const uint32_t stamp = nextStamp(valueSeen, valueStamp);
                std::int64_t gain = 0;
                const auto visitValue = [&](uint32_t valueId) {
                    if (valueId >= valueSeen.size() || valueSeen[valueId] == stamp)
                    {
                        return;
                    }
                    valueSeen[valueId] = stamp;
                    const std::size_t before =
                        valueTargetCountWithMove(valueId,
                                                 kInvalidActivitySupernodeId,
                                                 kInvalidActivitySupernodeId);
                    const std::size_t after =
                        valueTargetCountWithMove(valueId, clusterId, to);
                    gain += static_cast<std::int64_t>(before) -
                            static_cast<std::int64_t>(after);
                };
                if (clusterId < valueEdges.sourceValuesByCluster.size())
                {
                    for (const uint32_t valueId : valueEdges.sourceValuesByCluster[clusterId])
                    {
                        visitValue(valueId);
                    }
                }
                if (clusterId < valueEdges.targetValuesByCluster.size())
                {
                    for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                    {
                        visitValue(valueId);
                    }
                }
                return gain;
            };
            const auto ownerWithSwap = [&](uint32_t clusterId,
                                           uint32_t lhs,
                                           uint32_t lhsTo,
                                           uint32_t rhs,
                                           uint32_t rhsTo) {
                if (clusterId == lhs)
                {
                    return lhsTo;
                }
                if (clusterId == rhs)
                {
                    return rhsTo;
                }
                return clusterId < ownerByCluster.size() ? ownerByCluster[clusterId]
                                                         : kInvalidActivitySupernodeId;
            };
            const auto valueTargetCountWithSwap = [&](uint32_t valueId,
                                                      uint32_t lhs,
                                                      uint32_t lhsTo,
                                                      uint32_t rhs,
                                                      uint32_t rhsTo) {
                if (valueId >= valueEdges.valueFanouts.size())
                {
                    return std::size_t{0};
                }
                const auto &fanout = valueEdges.valueFanouts[valueId];
                const uint32_t sourceSegment =
                    ownerWithSwap(fanout.sourceCluster, lhs, lhsTo, rhs, rhsTo);
                if (sourceSegment == kInvalidActivitySupernodeId)
                {
                    return std::size_t{0};
                }
                const uint32_t stamp = nextStamp(segmentSeen, segmentStamp);
                std::size_t count = 0;
                for (const uint32_t targetCluster : fanout.targetClusters)
                {
                    const uint32_t targetSegment =
                        ownerWithSwap(targetCluster, lhs, lhsTo, rhs, rhsTo);
                    if (targetSegment == kInvalidActivitySupernodeId ||
                        targetSegment == sourceSegment ||
                        targetSegment >= segmentSeen.size())
                    {
                        continue;
                    }
                    if (segmentSeen[targetSegment] == stamp)
                    {
                        continue;
                    }
                    segmentSeen[targetSegment] = stamp;
                    ++count;
                }
                return count;
            };
            const auto swapPreservesSegmentTopo = [&](uint32_t lhs,
                                                      uint32_t lhsFrom,
                                                      uint32_t lhsTo,
                                                      uint32_t rhs,
                                                      uint32_t rhsFrom,
                                                      uint32_t rhsTo) {
                const auto clusterOk = [&](uint32_t clusterId) {
                    const uint32_t clusterSegment =
                        ownerWithSwap(clusterId, lhs, lhsTo, rhs, rhsTo);
                    if (clusterSegment == kInvalidActivitySupernodeId)
                    {
                        return false;
                    }
                    if (clusterId >= view.preds.size() || clusterId >= view.succs.size())
                    {
                        return false;
                    }
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        const uint32_t predSegment =
                            ownerWithSwap(pred, lhs, lhsTo, rhs, rhsTo);
                        if (predSegment == kInvalidActivitySupernodeId ||
                            predSegment == clusterSegment)
                        {
                            continue;
                        }
                        if (predSegment >= clusterSegment)
                        {
                            return false;
                        }
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        const uint32_t succSegment =
                            ownerWithSwap(succ, lhs, lhsTo, rhs, rhsTo);
                        if (succSegment == kInvalidActivitySupernodeId ||
                            succSegment == clusterSegment)
                        {
                            continue;
                        }
                        if (clusterSegment >= succSegment)
                        {
                            return false;
                        }
                    }
                    return true;
                };
                return lhs != rhs &&
                       lhs < ownerByCluster.size() &&
                       rhs < ownerByCluster.size() &&
                       ownerByCluster[lhs] == lhsFrom &&
                       ownerByCluster[rhs] == rhsFrom &&
                       lhsFrom != lhsTo &&
                       rhsFrom != rhsTo &&
                       lhsFrom == rhsTo &&
                       rhsFrom == lhsTo &&
                       clusterOk(lhs) &&
                       clusterOk(rhs);
            };
            const auto swapGain = [&](uint32_t lhs,
                                      uint32_t lhsFrom,
                                      uint32_t lhsTo,
                                      uint32_t rhs,
                                      uint32_t rhsFrom,
                                      uint32_t rhsTo) {
                if (lhs >= ownerByCluster.size() ||
                    rhs >= ownerByCluster.size() ||
                    lhs == rhs ||
                    ownerByCluster[lhs] != lhsFrom ||
                    ownerByCluster[rhs] != rhsFrom ||
                    lhsFrom == lhsTo ||
                    rhsFrom == rhsTo)
                {
                    return std::int64_t{0};
                }
                const uint32_t stamp = nextStamp(valueSeen, valueStamp);
                std::int64_t gain = 0;
                const auto visitValue = [&](uint32_t valueId) {
                    if (valueId >= valueSeen.size() || valueSeen[valueId] == stamp)
                    {
                        return;
                    }
                    valueSeen[valueId] = stamp;
                    const std::size_t before =
                        valueTargetCountWithSwap(valueId,
                                                 kInvalidActivitySupernodeId,
                                                 kInvalidActivitySupernodeId,
                                                 kInvalidActivitySupernodeId,
                                                 kInvalidActivitySupernodeId);
                    const std::size_t after =
                        valueTargetCountWithSwap(valueId, lhs, lhsTo, rhs, rhsTo);
                    gain += static_cast<std::int64_t>(before) -
                            static_cast<std::int64_t>(after);
                };
                const auto visitClusterValues = [&](uint32_t clusterId) {
                    if (clusterId < valueEdges.sourceValuesByCluster.size())
                    {
                        for (const uint32_t valueId : valueEdges.sourceValuesByCluster[clusterId])
                        {
                            visitValue(valueId);
                        }
                    }
                    if (clusterId < valueEdges.targetValuesByCluster.size())
                    {
                        for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                        {
                            visitValue(valueId);
                        }
                    }
                };
                visitClusterValues(lhs);
                visitClusterValues(rhs);
                return gain;
            };

            struct ScoredDests
            {
                std::vector<std::pair<uint32_t, std::size_t>> small;
                std::unordered_map<uint32_t, std::size_t> large;

                void add(uint32_t dest, std::size_t amount)
                {
                    if (dest == kInvalidActivitySupernodeId || amount == 0)
                    {
                        return;
                    }
                    if (large.empty() && small.size() < 64)
                    {
                        for (auto &entry : small)
                        {
                            if (entry.first == dest)
                            {
                                entry.second += amount;
                                return;
                            }
                        }
                        small.push_back({dest, amount});
                        return;
                    }
                    if (large.empty())
                    {
                        large.reserve(small.size() * 2 + 1);
                        for (const auto &[key, value] : small)
                        {
                            large[key] += value;
                        }
                        small.clear();
                    }
                    large[dest] += amount;
                }

                std::vector<std::pair<uint32_t, std::size_t>> take()
                {
                    if (!large.empty())
                    {
                        std::vector<std::pair<uint32_t, std::size_t>> out;
                        out.reserve(large.size());
                        for (const auto &[key, value] : large)
                        {
                            out.push_back({key, value});
                        }
                        return out;
                    }
                    return small;
                }
            };

            struct Candidate
            {
                uint32_t cluster = 0;
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t gain = 0;
                std::size_t approximateGain = 0;
            };
            struct BlockedMove
            {
                uint32_t cluster = 0;
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t approximateGain = 0;
            };
            struct SwapCandidate
            {
                uint32_t lhs = 0;
                uint32_t rhs = 0;
                uint32_t lhsFrom = 0;
                uint32_t lhsTo = 0;
                std::size_t gain = 0;
                std::size_t approximateGain = 0;
            };

            constexpr std::size_t kTopDestsPerCluster = 32;
            constexpr std::size_t kMaxMovesPerRound = 65536;
            constexpr std::size_t kMaxBlockedMovesPerRound = 32768;
            constexpr std::size_t kMaxSwapCandidatesPerRound = 65536;
            constexpr std::size_t kBlockedMoveTrimMultiple = 4;
            const auto trimBlockedMoves = [](std::vector<BlockedMove> &blockedMoves) {
                if (blockedMoves.size() <= kMaxBlockedMovesPerRound)
                {
                    return;
                }
                std::nth_element(blockedMoves.begin(),
                                 blockedMoves.begin() + kMaxBlockedMovesPerRound,
                                 blockedMoves.end(),
                                 [](const BlockedMove &lhs, const BlockedMove &rhs)
                                 {
                                     if (lhs.approximateGain != rhs.approximateGain)
                                     {
                                         return lhs.approximateGain > rhs.approximateGain;
                                     }
                                     if (lhs.cluster != rhs.cluster)
                                     {
                                         return lhs.cluster < rhs.cluster;
                                     }
                                     return lhs.to < rhs.to;
                                 });
                blockedMoves.resize(kMaxBlockedMovesPerRound);
            };
            const auto trimSwapCandidates = [](std::vector<SwapCandidate> &swapCandidates) {
                if (swapCandidates.size() <= kMaxSwapCandidatesPerRound)
                {
                    return;
                }
                std::nth_element(swapCandidates.begin(),
                                 swapCandidates.begin() + kMaxSwapCandidatesPerRound,
                                 swapCandidates.end(),
                                 [](const SwapCandidate &lhs, const SwapCandidate &rhs)
                                 {
                                     if (lhs.gain != rhs.gain)
                                     {
                                         return lhs.gain > rhs.gain;
                                     }
                                     if (lhs.approximateGain != rhs.approximateGain)
                                     {
                                         return lhs.approximateGain > rhs.approximateGain;
                                     }
                                     if (lhs.lhs != rhs.lhs)
                                     {
                                         return lhs.lhs < rhs.lhs;
                                     }
                                     return lhs.rhs < rhs.rhs;
                                 });
                swapCandidates.resize(kMaxSwapCandidatesPerRound);
            };
            const auto recordSizeBlock = [&](uint32_t targetSegment)
            {
                if (perf == nullptr || maxOps == 0 || targetSegment >= segmentOps.size())
                {
                    return;
                }
                const std::size_t fillPpm =
                    static_cast<std::size_t>(
                        (static_cast<std::uint64_t>(segmentOps[targetSegment]) * 1000000ull) /
                        static_cast<std::uint64_t>(maxOps));
                const char *bucket = "lt50";
                if (fillPpm >= 1000000)
                {
                    bucket = "ge100";
                }
                else if (fillPpm >= 950000)
                {
                    bucket = "95_100";
                }
                else if (fillPpm >= 900000)
                {
                    bucket = "90_95";
                }
                else if (fillPpm >= 750000)
                {
                    bucket = "75_90";
                }
                else if (fillPpm >= 500000)
                {
                    bucket = "50_75";
                }
                ++perf->fmRefineRejectedSizeFillBucket[bucket];
            };
            const auto recordCycleBlock = [&](uint32_t clusterId, uint32_t targetSegment)
            {
                if (perf == nullptr ||
                    clusterId >= view.preds.size() ||
                    clusterId >= view.succs.size() ||
                    targetSegment == kInvalidActivitySupernodeId)
                {
                    return;
                }
                bool predAfterOrSame = false;
                bool succBeforeOrSame = false;
                for (const uint32_t pred : view.preds[clusterId])
                {
                    const uint32_t predSegment =
                        pred < ownerByCluster.size() ? ownerByCluster[pred]
                                                     : kInvalidActivitySupernodeId;
                    if (predSegment != kInvalidActivitySupernodeId &&
                        predSegment != targetSegment &&
                        predSegment >= targetSegment)
                    {
                        predAfterOrSame = true;
                        break;
                    }
                }
                for (const uint32_t succ : view.succs[clusterId])
                {
                    const uint32_t succSegment =
                        succ < ownerByCluster.size() ? ownerByCluster[succ]
                                                     : kInvalidActivitySupernodeId;
                    if (succSegment != kInvalidActivitySupernodeId &&
                        succSegment != targetSegment &&
                        targetSegment >= succSegment)
                    {
                        succBeforeOrSame = true;
                        break;
                    }
                }
                const char *relation = "unknown";
                if (predAfterOrSame && succBeforeOrSame)
                {
                    relation = "pred_after_and_succ_before";
                }
                else if (predAfterOrSame)
                {
                    relation = "pred_after";
                }
                else if (succBeforeOrSame)
                {
                    relation = "succ_before";
                }
                ++perf->fmRefineRejectedCycleRelation[relation];
            };
            for (std::size_t round = 0; round < options.fmRefineMaxRounds; ++round)
            {
                std::vector<Candidate> candidates;
                std::vector<BlockedMove> blockedMoves;
                std::size_t boundaryClusters = 0;
                for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
                {
                    const uint32_t from = clusterId < ownerByCluster.size()
                                              ? ownerByCluster[clusterId]
                                              : kInvalidActivitySupernodeId;
                    if (from == kInvalidActivitySupernodeId || from >= segments.size())
                    {
                        continue;
                    }
                    ScoredDests scoredDests;
                    if (clusterId < valueEdges.sourceValuesByCluster.size())
                    {
                        for (const uint32_t valueId : valueEdges.sourceValuesByCluster[clusterId])
                        {
                            if (valueId >= valueEdges.valueFanouts.size())
                            {
                                continue;
                            }
                            for (const uint32_t targetCluster :
                                 valueEdges.valueFanouts[valueId].targetClusters)
                            {
                                const uint32_t targetSegment =
                                    targetCluster < ownerByCluster.size()
                                        ? ownerByCluster[targetCluster]
                                        : kInvalidActivitySupernodeId;
                                if (targetSegment != from)
                                {
                                    scoredDests.add(targetSegment, 1);
                                }
                            }
                        }
                    }
                    if (clusterId < valueEdges.targetValuesByCluster.size())
                    {
                        for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                        {
                            if (valueId >= valueEdges.valueFanouts.size())
                            {
                                continue;
                            }
                            const uint32_t sourceSegment =
                                valueEdges.valueFanouts[valueId].sourceCluster < ownerByCluster.size()
                                    ? ownerByCluster[valueEdges.valueFanouts[valueId].sourceCluster]
                                    : kInvalidActivitySupernodeId;
                            if (sourceSegment != from)
                            {
                                scoredDests.add(sourceSegment, 4);
                            }
                        }
                    }
                    auto dests = scoredDests.take();
                    if (dests.empty())
                    {
                        continue;
                    }
                    ++boundaryClusters;
                    std::sort(dests.begin(),
                              dests.end(),
                              [](const auto &lhs, const auto &rhs)
                              {
                                  if (lhs.second != rhs.second)
                                  {
                                      return lhs.second > rhs.second;
                                  }
                                  return lhs.first < rhs.first;
                              });
                    if (dests.size() > kTopDestsPerCluster)
                    {
                        dests.resize(kTopDestsPerCluster);
                    }
                    for (const auto &[to, approximateGain] : dests)
                    {
                        if (to == from ||
                            to >= segments.size() ||
                            segments[from].size() <= 1)
                        {
                            continue;
                        }
                        const std::size_t ops = clusterId < clusterOps.size() ? clusterOps[clusterId] : 0;
                        if (maxOps != 0 && segmentOps[to] + ops > maxOps)
                        {
                            if (perf)
                            {
                                ++perf->fmRefineRejectedSize;
                                recordSizeBlock(to);
                            }
                            if (approximateGain != 0)
                            {
                                blockedMoves.push_back(BlockedMove{
                                    clusterId,
                                    from,
                                    to,
                                    approximateGain,
                                });
                                if (blockedMoves.size() >
                                    kMaxBlockedMovesPerRound * kBlockedMoveTrimMultiple)
                                {
                                    trimBlockedMoves(blockedMoves);
                                }
                            }
                            continue;
                        }
                        if (!probFmMovePreservesSegmentTopo(view, ownerByCluster, clusterId, to))
                        {
                            if (perf)
                            {
                                ++perf->fmRefineRejectedCycle;
                                recordCycleBlock(clusterId, to);
                            }
                            continue;
                        }
                        const std::int64_t gain = moveGain(clusterId, from, to);
                        if (gain <= 0)
                        {
                            continue;
                        }
                        candidates.push_back(Candidate{
                            clusterId,
                            from,
                            to,
                            static_cast<std::size_t>(gain),
                            approximateGain,
                        });
                    }
                }
                if (perf)
                {
                    perf->fmRefineCandidates += candidates.size();
                }
                if (candidates.empty())
                {
                    trimBlockedMoves(blockedMoves);
                }
                else
                {
                    std::sort(candidates.begin(),
                              candidates.end(),
                              [](const Candidate &lhs, const Candidate &rhs)
                              {
                                  if (lhs.gain != rhs.gain)
                                  {
                                      return lhs.gain > rhs.gain;
                                  }
                                  if (lhs.approximateGain != rhs.approximateGain)
                                  {
                                      return lhs.approximateGain > rhs.approximateGain;
                                  }
                                  if (lhs.cluster != rhs.cluster)
                                  {
                                      return lhs.cluster < rhs.cluster;
                                  }
                                  return lhs.to < rhs.to;
                              });
                    trimBlockedMoves(blockedMoves);
                }
                if (candidates.empty() && blockedMoves.empty())
                {
                    break;
                }
                std::sort(blockedMoves.begin(),
                          blockedMoves.end(),
                          [](const BlockedMove &lhs, const BlockedMove &rhs)
                          {
                              if (lhs.approximateGain != rhs.approximateGain)
                              {
                                  return lhs.approximateGain > rhs.approximateGain;
                              }
                              if (lhs.cluster != rhs.cluster)
                              {
                                  return lhs.cluster < rhs.cluster;
                              }
                              return lhs.to < rhs.to;
                          });

                std::vector<uint8_t> movedThisRound(view.members.size(), 0);
                std::size_t roundMoves = 0;
                std::uint64_t roundGain = 0;
                for (const auto &candidate : candidates)
                {
                    const uint32_t clusterId = candidate.cluster;
                    if (clusterId >= ownerByCluster.size() || movedThisRound[clusterId] != 0)
                    {
                        continue;
                    }
                    const uint32_t from = ownerByCluster[clusterId];
                    const uint32_t to = candidate.to;
                    if (from != candidate.from ||
                        from == kInvalidActivitySupernodeId ||
                        to == kInvalidActivitySupernodeId ||
                        from == to ||
                        from >= segments.size() ||
                        to >= segments.size() ||
                        segments[from].size() <= 1)
                    {
                        continue;
                    }
                    const std::size_t ops = clusterId < clusterOps.size() ? clusterOps[clusterId] : 0;
                    if (maxOps != 0 && segmentOps[to] + ops > maxOps)
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedSize;
                            recordSizeBlock(to);
                        }
                        continue;
                    }
                    if (!probFmMovePreservesSegmentTopo(view, ownerByCluster, clusterId, to))
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedCycle;
                            recordCycleBlock(clusterId, to);
                        }
                        continue;
                    }
                    const std::int64_t gain = moveGain(clusterId, from, to);
                    if (gain <= 0)
                    {
                        continue;
                    }

                    auto &fromMembers = segments[from];
                    const auto eraseIt = std::find(fromMembers.begin(), fromMembers.end(), clusterId);
                    if (eraseIt == fromMembers.end())
                    {
                        continue;
                    }
                    fromMembers.erase(eraseIt);
                    segments[to].push_back(clusterId);
                    std::sort(segments[to].begin(), segments[to].end());
                    ownerByCluster[clusterId] = to;
                    segmentOps[from] = segmentOps[from] >= ops ? segmentOps[from] - ops : 0;
                    segmentOps[to] += ops;
                    movedThisRound[clusterId] = 1;
                    ++roundMoves;
                    roundGain += static_cast<std::uint64_t>(gain);
                    if (roundMoves >= kMaxMovesPerRound)
                    {
                        break;
                    }
                }
                std::vector<SwapCandidate> swapCandidates;
                for (const auto &blocked : blockedMoves)
                {
                    const uint32_t lhs = blocked.cluster;
                    if (lhs >= ownerByCluster.size() ||
                        movedThisRound[lhs] != 0 ||
                        ownerByCluster[lhs] != blocked.from ||
                        blocked.from == kInvalidActivitySupernodeId ||
                        blocked.to == kInvalidActivitySupernodeId ||
                        blocked.from == blocked.to ||
                        blocked.from >= segments.size() ||
                        blocked.to >= segments.size())
                    {
                        continue;
                    }
                    const std::size_t lhsOps = lhs < clusterOps.size() ? clusterOps[lhs] : 0;
                    if (maxOps == 0 || segmentOps[blocked.to] + lhsOps <= maxOps)
                    {
                        continue;
                    }
                    if (!probFmMovePreservesSegmentTopo(view, ownerByCluster, lhs, blocked.to))
                    {
                        continue;
                    }
                    const std::size_t toOverflow = segmentOps[blocked.to] + lhsOps - maxOps;
                    const std::size_t fromAfterLhs =
                        segmentOps[blocked.from] >= lhsOps ? segmentOps[blocked.from] - lhsOps : 0;
                    for (const uint32_t rhs : segments[blocked.to])
                    {
                        if (rhs >= ownerByCluster.size() ||
                            rhs == lhs ||
                            movedThisRound[rhs] != 0 ||
                            ownerByCluster[rhs] != blocked.to)
                        {
                            continue;
                        }
                        const std::size_t rhsOps = rhs < clusterOps.size() ? clusterOps[rhs] : 0;
                        if (rhsOps < toOverflow || fromAfterLhs + rhsOps > maxOps)
                        {
                            continue;
                        }
                        if (!swapPreservesSegmentTopo(lhs,
                                                      blocked.from,
                                                      blocked.to,
                                                      rhs,
                                                      blocked.to,
                                                      blocked.from))
                        {
                            if (perf)
                            {
                                ++perf->fmRefineRejectedCycle;
                                recordCycleBlock(lhs, blocked.to);
                            }
                            continue;
                        }
                        const std::int64_t gain =
                            swapGain(lhs,
                                     blocked.from,
                                     blocked.to,
                                     rhs,
                                     blocked.to,
                                     blocked.from);
                        if (gain <= 0)
                        {
                            continue;
                        }
                        swapCandidates.push_back(SwapCandidate{
                            lhs,
                            rhs,
                            blocked.from,
                            blocked.to,
                            static_cast<std::size_t>(gain),
                            blocked.approximateGain,
                        });
                        if (swapCandidates.size() >
                            kMaxSwapCandidatesPerRound * kBlockedMoveTrimMultiple)
                        {
                            trimSwapCandidates(swapCandidates);
                        }
                    }
                }
                trimSwapCandidates(swapCandidates);
                std::sort(swapCandidates.begin(),
                          swapCandidates.end(),
                          [](const SwapCandidate &lhs, const SwapCandidate &rhs)
                          {
                              if (lhs.gain != rhs.gain)
                              {
                                  return lhs.gain > rhs.gain;
                              }
                              if (lhs.approximateGain != rhs.approximateGain)
                              {
                                  return lhs.approximateGain > rhs.approximateGain;
                              }
                              if (lhs.lhs != rhs.lhs)
                              {
                                  return lhs.lhs < rhs.lhs;
                              }
                              return lhs.rhs < rhs.rhs;
                          });
                for (const auto &candidate : swapCandidates)
                {
                    const uint32_t lhs = candidate.lhs;
                    const uint32_t rhs = candidate.rhs;
                    const uint32_t lhsFrom = candidate.lhsFrom;
                    const uint32_t lhsTo = candidate.lhsTo;
                    if (lhs >= ownerByCluster.size() ||
                        rhs >= ownerByCluster.size() ||
                        movedThisRound[lhs] != 0 ||
                        movedThisRound[rhs] != 0 ||
                        ownerByCluster[lhs] != lhsFrom ||
                        ownerByCluster[rhs] != lhsTo ||
                        lhsFrom == lhsTo ||
                        lhsFrom >= segments.size() ||
                        lhsTo >= segments.size())
                    {
                        continue;
                    }
                    const std::size_t lhsOps = lhs < clusterOps.size() ? clusterOps[lhs] : 0;
                    const std::size_t rhsOps = rhs < clusterOps.size() ? clusterOps[rhs] : 0;
                    if (maxOps != 0 &&
                        (segmentOps[lhsTo] + lhsOps < rhsOps ||
                         segmentOps[lhsTo] + lhsOps - rhsOps > maxOps ||
                         segmentOps[lhsFrom] + rhsOps < lhsOps ||
                         segmentOps[lhsFrom] + rhsOps - lhsOps > maxOps))
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedSize;
                            recordSizeBlock(lhsTo);
                        }
                        continue;
                    }
                    if (!swapPreservesSegmentTopo(lhs, lhsFrom, lhsTo, rhs, lhsTo, lhsFrom))
                    {
                        if (perf)
                        {
                            ++perf->fmRefineRejectedCycle;
                            recordCycleBlock(lhs, lhsTo);
                            recordCycleBlock(rhs, lhsFrom);
                        }
                        continue;
                    }
                    const std::int64_t gain =
                        swapGain(lhs, lhsFrom, lhsTo, rhs, lhsTo, lhsFrom);
                    if (gain <= 0)
                    {
                        continue;
                    }

                    auto &fromMembers = segments[lhsFrom];
                    auto &toMembers = segments[lhsTo];
                    const auto lhsIt = std::find(fromMembers.begin(), fromMembers.end(), lhs);
                    const auto rhsIt = std::find(toMembers.begin(), toMembers.end(), rhs);
                    if (lhsIt == fromMembers.end() || rhsIt == toMembers.end())
                    {
                        continue;
                    }
                    fromMembers.erase(lhsIt);
                    toMembers.erase(rhsIt);
                    fromMembers.push_back(rhs);
                    toMembers.push_back(lhs);
                    std::sort(fromMembers.begin(), fromMembers.end());
                    std::sort(toMembers.begin(), toMembers.end());
                    ownerByCluster[lhs] = lhsTo;
                    ownerByCluster[rhs] = lhsFrom;
                    segmentOps[lhsFrom] = segmentOps[lhsFrom] >= lhsOps
                                              ? segmentOps[lhsFrom] - lhsOps + rhsOps
                                              : rhsOps;
                    segmentOps[lhsTo] = segmentOps[lhsTo] >= rhsOps
                                            ? segmentOps[lhsTo] - rhsOps + lhsOps
                                            : lhsOps;
                    movedThisRound[lhs] = 1;
                    movedThisRound[rhs] = 1;
                    roundMoves += 2;
                    roundGain += static_cast<std::uint64_t>(gain);
                    if (roundMoves >= kMaxMovesPerRound)
                    {
                        break;
                    }
                }
                if (roundMoves == 0)
                {
                    break;
                }
                if (perf)
                {
                    ++perf->fmRefineRounds;
                    perf->fmRefineMoves += roundMoves;
                    perf->fmRefineTotalGain += static_cast<double>(roundGain);
                }
                (void)boundaryClusters;
            }

            return segments;
        }

        std::vector<std::vector<uint32_t>> flattenNodeSegments(const NodeClusterView &view,
                                                               const std::vector<std::vector<uint32_t>> &segments,
                                                               const std::vector<uint32_t> &nodeTopoPos)
        {
            std::vector<std::vector<uint32_t>> out;
            out.reserve(segments.size());
            for (const auto &segment : segments)
            {
                std::vector<uint32_t> nodes;
                for (const auto clusterId : segment)
                {
                    if (clusterId < view.members.size())
                    {
                        nodes.insert(nodes.end(), view.members[clusterId].begin(), view.members[clusterId].end());
                    }
                }
                out.push_back(std::move(nodes));
            }
            for (auto &nodes : out)
            {
                std::sort(nodes.begin(),
                          nodes.end(),
                          [&](uint32_t lhs, uint32_t rhs)
                          {
                              const uint32_t lhsPos = lhs < nodeTopoPos.size() ? nodeTopoPos[lhs]
                                                                               : kInvalidActivitySupernodeId;
                              const uint32_t rhsPos = rhs < nodeTopoPos.size() ? nodeTopoPos[rhs]
                                                                               : kInvalidActivitySupernodeId;
                              if (lhsPos != rhsPos)
                              {
                                  return lhsPos < rhsPos;
                              }
                              return lhs < rhs;
                          });
            }
            return out;
        }

        bool topoSortLocalOps(const wolvrix::lib::grh::Graph &graph,
                              const std::vector<wolvrix::lib::grh::OperationId> &ops,
                              std::vector<wolvrix::lib::grh::OperationId> &out,
                              std::string &error)
        {
            out.clear();
            const std::vector<wolvrix::lib::grh::OperationId> uniqueOps =
                uniqueOpsPreservingOrder(ops);
            if (uniqueOps.size() < 2)
            {
                out = uniqueOps;
                return true;
            }

            std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> local;
            local.reserve(uniqueOps.size());
            for (const auto opId : uniqueOps)
            {
                local.insert(opId);
            }

            wolvrix::lib::toposort::TopoDag<wolvrix::lib::grh::OperationId,
                                            wolvrix::lib::grh::OperationIdHash>
                dag;
            dag.reserveNodes(uniqueOps.size());
            for (const auto opId : uniqueOps)
            {
                dag.addNode(opId);
            }
            for (const auto opId : uniqueOps)
            {
                for (const auto operand : graph.opOperands(opId))
                {
                    const auto defOp = graph.valueDef(operand);
                    if (defOp.valid() && local.find(defOp) != local.end() && defOp != opId)
                    {
                        dag.addEdge(defOp, opId);
                    }
                }
            }

            try
            {
                const auto layers = dag.toposort();
                out.reserve(uniqueOps.size());
                for (const auto &layer : layers)
                {
                    std::vector<wolvrix::lib::grh::OperationId> ordered(layer.begin(), layer.end());
                    std::sort(ordered.begin(),
                              ordered.end(),
                              [](const auto lhs, const auto rhs)
                              {
                                  return lhs.index < rhs.index;
                    });
                    out.insert(out.end(), ordered.begin(), ordered.end());
                }
                if (out.size() == uniqueOps.size())
                {
                    return true;
                }
                error = "activity-schedule local op topo failed: missing ops";
                return false;
            }
            catch (const std::exception &ex)
            {
                std::ostringstream oss;
                oss << "activity-schedule local op topo failed: " << ex.what()
                    << " ops=" << uniqueOps.size();
                const std::size_t limit = std::min<std::size_t>(uniqueOps.size(), 12);
                oss << " sample=[";
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        oss << ",";
                    }
                    oss << describeOp(graph, uniqueOps[i]);
                }
                if (uniqueOps.size() > limit)
                {
                    oss << ",...";
                }
                oss << "]";
                error = oss.str();
                return false;
            }
        }

        // NO0208 Phase D：MFFC rep[]（topo-pos 空间）。反向拓扑；compute 节点的全部 compute 消费者
        // 同 rep 则继承、否则自成根。非 compute 节点 rep 保持 kInvalid。
        std::vector<uint32_t> computeMffcRep(const ActivityOpData &data)
        {
            const std::size_t count = data.topoOps.size();
            std::vector<uint32_t> rep(count, kInvalidActivitySupernodeId);
            if (count == 0)
            {
                return rep;
            }
            std::vector<uint8_t> isCompute(count, 0);
            for (std::size_t pos = 0; pos < count; ++pos)
            {
                isCompute[pos] =
                    (classifyActivityOp(data.topoKinds[pos]) == ActivityOpClass::Compute) ? 1U : 0U;
            }
            std::vector<uint32_t> outDeg(count, 0);
            for (const auto &[s, d] : data.topoEdges)
            {
                if (isCompute[s] && isCompute[d])
                {
                    ++outDeg[s];
                }
            }
            std::vector<std::size_t> off(count + 1, 0);
            for (std::size_t i = 0; i < count; ++i)
            {
                off[i + 1] = off[i] + outDeg[i];
            }
            std::vector<uint32_t> cons(off[count]);
            std::vector<std::size_t> cur(off.begin(), off.end() - 1);
            for (const auto &[s, d] : data.topoEdges)
            {
                if (isCompute[s] && isCompute[d])
                {
                    cons[cur[s]++] = d;
                }
            }
            for (std::size_t i = count; i-- > 0;)
            {
                if (!isCompute[i])
                {
                    continue;
                }
                uint32_t single = kInvalidActivitySupernodeId;
                bool any = false;
                bool split = false;
                for (std::size_t e = off[i]; e < off[i + 1]; ++e)
                {
                    const uint32_t r = rep[cons[e]];
                    if (!any)
                    {
                        single = r;
                        any = true;
                    }
                    else if (single != r)
                    {
                        split = true;
                        break;
                    }
                }
                rep[i] = (any && !split) ? single : static_cast<uint32_t>(i);
            }
            return rep;
        }

        // NO0208 Phase D：把 builder 因保守 local reconvergent-absorb 而 over-split 的 computeNode
        // 合并到理想 MFFC 锥（仅 prob 策略）。用 rep[] 把同锥的 compute→compute 边对应的 computeNode
        // 用并查集合并，受 maxOpInComputeNode cap 约束；再 recompute owners/boundaries。DAG 重建与
        // cap 可能引发的 quotient 环修复，交给调用点后面已有的 cycle-split 循环。
        void mergeComputeNodesToMffc(ComputeRewriteBuild &build,
                                     const ActivityOpData &data,
                                     const wolvrix::lib::grh::Graph &graph,
                                     std::size_t maxOpsPerNode)
        {
            const std::size_t count = data.topoOps.size();
            const std::size_t n = build.computeNodes.size();
            if (count == 0 || n == 0)
            {
                return;
            }
            const std::vector<uint32_t> rep = computeMffcRep(data);
            // 每个 builder 节点的代表锥 rep（取节点内最大 topo 的 compute op）与 topo 排序键。
            // intent / indivisible 节点不参与合并（保留 reg-to-mem 语义）。
            std::vector<uint32_t> nodeRep(n, kInvalidActivitySupernodeId);
            std::vector<uint32_t> nodeTopo(n, 0);
            for (uint32_t i = 0; i < n; ++i)
            {
                const ComputeNode &node = build.computeNodes[i];
                if (node.indivisible || !node.intentGroup.empty())
                {
                    continue;
                }
                bool found = false;
                uint32_t bestTopo = 0;
                uint32_t r = kInvalidActivitySupernodeId;
                for (const auto opId : node.ops)
                {
                    const uint32_t pos = (opId.index < data.topoPosByOpIndex.size())
                                             ? data.topoPosByOpIndex[opId.index]
                                             : kInvalidActivitySupernodeId;
                    if (pos == kInvalidActivitySupernodeId || pos >= count)
                    {
                        continue;
                    }
                    if (classifyActivityOp(data.topoKinds[pos]) != ActivityOpClass::Compute)
                    {
                        continue;
                    }
                    if (!found || pos > bestTopo)
                    {
                        bestTopo = pos;
                        r = rep[pos];
                        found = true;
                    }
                }
                if (found)
                {
                    nodeRep[i] = r;
                    nodeTopo[i] = bestTopo;
                }
            }

            std::unordered_map<uint32_t, std::vector<uint32_t>> coneNodes;
            for (uint32_t i = 0; i < n; ++i)
            {
                if (nodeRep[i] != kInvalidActivitySupernodeId)
                {
                    coneNodes[nodeRep[i]].push_back(i);
                }
            }

            std::vector<ComputeNode> merged;
            merged.reserve(n);
            std::vector<uint8_t> consumed(n, 0);
            // 同锥节点按 topo 排序后做「topo 连续分块」合并（受 cap 约束）——连续分块保证 quotient
            // 无环，避免任意并查集合并造成的跨 topo straddle 环（曾导致 cycle-split storm）。
            for (auto &entry : coneNodes)
            {
                auto &ids = entry.second;
                std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
                    if (nodeTopo[a] != nodeTopo[b])
                    {
                        return nodeTopo[a] < nodeTopo[b];
                    }
                    return a < b;
                });
                std::size_t curIdx = SIZE_MAX;
                std::size_t chunkOps = 0;
                for (const uint32_t id : ids)
                {
                    const std::size_t sz = build.computeNodes[id].ops.size();
                    if (curIdx == SIZE_MAX || (maxOpsPerNode != 0 && chunkOps + sz > maxOpsPerNode))
                    {
                        curIdx = merged.size();
                        merged.emplace_back();
                        chunkOps = 0;
                    }
                    const ComputeNode &src = build.computeNodes[id];
                    merged[curIdx].ops.insert(merged[curIdx].ops.end(), src.ops.begin(), src.ops.end());
                    chunkOps += sz;
                    consumed[id] = 1;
                }
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!consumed[i])
                {
                    merged.push_back(std::move(build.computeNodes[i]));
                }
            }
            std::size_t commonExprCount = 0;
            for (auto &node : merged)
            {
                std::sort(node.ops.begin(),
                          node.ops.end(),
                          [&](wolvrix::lib::grh::OperationId a, wolvrix::lib::grh::OperationId b) {
                              const uint32_t pa = a.index < data.topoPosByOpIndex.size()
                                                      ? data.topoPosByOpIndex[a.index]
                                                      : kInvalidActivitySupernodeId;
                              const uint32_t pb = b.index < data.topoPosByOpIndex.size()
                                                      ? data.topoPosByOpIndex[b.index]
                                                      : kInvalidActivitySupernodeId;
                              if (pa != pb)
                              {
                                  return pa < pb;
                              }
                              return a.index < b.index;
                          });
                node.commonExpr = (node.ops.size() == 1);
                if (node.commonExpr)
                {
                    ++commonExprCount;
                }
            }
            build.computeNodes = std::move(merged);
            build.stats.commonExprComputeNodes = commonExprCount;
            recomputeComputeNodeOwnersAndBoundaries(build, graph);
        }

        bool buildComputeNodeRewrite(wolvrix::lib::grh::Graph &graph,
                                     const ActivityScheduleOptions &options,
                                     const ActivityOpData &opData,
                                     std::vector<ActivityOpClass> &opClasses,
                                     const ValueCanonicalMap &canonicalValues,
            ComputeRewriteBuild &out,
                                     std::string &error)
        {
            out = ComputeRewriteBuild{};
            out.canonicalValues = canonicalValues;
            out.computeNodeOfOp.assign(opClasses.size(), kInvalidActivitySupernodeId);
            ComputeNodeBuilder builder(graph, options, opData, opClasses, out, error);

            std::vector<uint32_t> intentNodeIds;
            for (auto &intentGroup : collectRegToMemIntentComputeGroups(graph, opClasses))
            {
                auto nodeId = builder.createIntentGroupNode(std::move(intentGroup.group), std::move(intentGroup.ops));
                if (!error.empty())
                {
                    return false;
                }
                if (nodeId)
                {
                    intentNodeIds.push_back(*nodeId);
                }
            }
            for (uint32_t nodeId : intentNodeIds)
            {
                if (!builder.processIntentGroupNode(nodeId))
                {
                    return false;
                }
            }

            const std::size_t maxCommitOps = options.maxOpInCommitSupernode;
            std::vector<uint32_t> sinkTopoPositions;
            sinkTopoPositions.reserve(opData.topoOps.size());
            for (uint32_t topoPos = 0; topoPos < opData.topoOps.size(); ++topoPos)
            {
                const auto opId = opData.topoOps[topoPos];
                if (opId.index < opClasses.size() && opClasses[opId.index] == ActivityOpClass::Sink)
                {
                    sinkTopoPositions.push_back(topoPos);
                }
            }
            WorkingPartition sinkPartition =
                buildEventClusteredSinkPartition(graph,
                                                 opData,
                                                 sinkTopoPositions,
                                                 maxCommitOps,
                                                 &canonicalValues,
                                                 options.commitGuardEventBuckets);
            out.stats.commitSinkOps = sinkTopoPositions.size();
            out.stats.commitEventKeyRuns = sinkPartition.clusters.size();
            {
                std::unordered_set<std::string> uniqueEventKeys;
                uniqueEventKeys.reserve(sinkTopoPositions.size());
                for (const auto topoPos : sinkTopoPositions)
                {
                    const auto op = graph.getOperation(opData.topoOps[topoPos]);
                    uniqueEventKeys.insert(normalizedSinkEventKey(graph, op, &canonicalValues));
                }
                out.stats.commitEventKeys = uniqueEventKeys.size();
            }
            for (const auto &cluster : sinkPartition.clusters)
            {
                CommitNode commit;
                for (const auto topoPos : cluster)
                {
                    const auto sinkOp = opData.topoOps[topoPos];
                    commit.ops.push_back(sinkOp);
                    for (const auto operand : graph.opOperands(sinkOp))
                    {
                        if (!vectorContainsValue(commit.inputValues, operand))
                        {
                            commit.inputValues.push_back(operand);
                            ++out.stats.commitInputRootValues;
                        }
                    }
                }
                out.commitNodes.push_back(std::move(commit));
            }

            auto ensureRootValue = [&](wolvrix::lib::grh::ValueId value, bool commitRoot) {
                if (!value.valid() || value.graph != graph.id())
                {
                    if (commitRoot)
                    {
                        error = "activity-schedule commit root value ownership mismatch";
                    }
                    return;
                }
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    return;
                }
                if (defOp.index >= opClasses.size())
                {
                    return;
                }
                const ActivityOpClass defClass = opClasses[defOp.index];
                if (defClass == ActivityOpClass::Source)
                {
                    if (commitRoot)
                    {
                        ++out.stats.directSourceInputsToCommitSupernodes;
                    }
                    builder.ensureSourceOwnerNode(defOp);
                    return;
                }
                if (defClass == ActivityOpClass::Sink)
                {
                    error = "activity-schedule compute root is defined by sink op value=" +
                            std::to_string(value.index) + " def=" + describeOp(graph, defOp);
                    return;
                }
                if (defClass == ActivityOpClass::Compute)
                {
                    const bool common =
                        semanticConsumerCount(graph,
                                              value,
                                              opClasses,
                                              kInvalidActivitySupernodeId,
                                              out.computeNodeOfOp) > 1;
                    builder.ensureComputeNodeForOp(defOp, common);
                }
            };

            for (const auto &commit : out.commitNodes)
            {
                for (const auto input : commit.inputValues)
                {
                    ensureRootValue(input, true);
                    if (!error.empty())
                    {
                        return false;
                    }
                }
            }
            for (const auto &port : graph.outputPorts())
            {
                ensureRootValue(port.value, false);
                if (!error.empty())
                {
                    return false;
                }
            }
            for (const auto &port : graph.inoutPorts())
            {
                ensureRootValue(port.out, false);
                if (!error.empty())
                {
                    return false;
                }
                ensureRootValue(port.oe, false);
                if (!error.empty())
                {
                    return false;
                }
            }
            for (const auto opId : opData.topoOps)
            {
                if (opId.index >= opClasses.size() || opClasses[opId.index] != ActivityOpClass::Compute)
                {
                    continue;
                }
                if (!graph.opResults(opId).empty())
                {
                    continue;
                }
                builder.ensureComputeNodeForOp(opId, false);
                if (!error.empty())
                {
                    return false;
                }
            }

            // NO0208 Phase D：把 over-split 的 computeNode 合并到理想 MFFC 锥（仅 prob）。CBAW
            // 从 P3 atom 层开始，不能把 prob/plain 的预合并结果作为初始解。
            if (options.partitionPolicy == "prob")
            {
                mergeComputeNodesToMffc(out, opData, graph, options.maxOpInComputeNode);
            }

            constexpr std::size_t kMaxComputeNodeCycleSplitIters = 1024;
            std::size_t cycleSplitIters = 0;
            while (true)
            {
                buildComputeDag(out, out.computeNodeOfOp, graph);
                try
                {
                    out.computeTopoOrder = topoOrderForDag(out.computeDag);
                    break;
                }
                catch (const std::exception &ex)
                {
                    const auto cycle = findCyclePath(out.computeDag);
                    if (cycleSplitIters < kMaxComputeNodeCycleSplitIters &&
                        splitCycleComputeNodesToSingletons(out, graph, opData, cycle))
                    {
                        ++cycleSplitIters;
                        continue;
                    }

                    std::ostringstream oss;
                    oss << "activity-schedule compute-node topo failed: " << ex.what();
                    if (!cycle.empty())
                    {
                        oss << " cycle_path=";
                        const std::size_t nodeLimit = std::min<std::size_t>(cycle.size(), 12);
                        for (std::size_t i = 0; i < nodeLimit; ++i)
                        {
                            if (i != 0)
                            {
                                oss << " -> ";
                            }
                            oss << cycle[i];
                        }
                        if (cycle.size() > nodeLimit)
                        {
                            oss << " -> ...";
                        }
                        oss << " cycle_nodes={";
                        for (std::size_t i = 0; i < nodeLimit; ++i)
                        {
                            if (i != 0)
                            {
                                oss << " | ";
                            }
                            appendComputeNodeSummary(oss, graph, out, opData, cycle[i]);
                        }
                        oss << "}";
                        oss << " cycle_edges={";
                        const std::size_t edgeLimit =
                            cycle.empty() ? std::size_t{0} : std::min<std::size_t>(cycle.size() - 1, 12);
                        for (std::size_t i = 0; i < edgeLimit; ++i)
                        {
                            if (i != 0)
                            {
                                oss << " | ";
                            }
                            appendComputeNodeEdgeReasons(oss, graph, out, opData, cycle[i], cycle[i + 1]);
                        }
                        if (!cycle.empty() && cycle.size() - 1 > edgeLimit)
                        {
                            oss << " | ...";
                        }
                        oss << "}";
                    }
                    if (cycleSplitIters >= kMaxComputeNodeCycleSplitIters)
                    {
                        oss << " cycle_split_iters=" << cycleSplitIters;
                    }
                    error = oss.str();
                    return false;
                }
            }
            out.stats.computeNodes = out.computeNodes.size();
            out.stats.computeNodeCycleSplitIters = cycleSplitIters;
            out.stats.computeNodeOpsTotal = 0;
            for (const auto &node : out.computeNodes)
            {
                out.stats.computeNodeOpsTotal += node.ops.size();
            }
            return true;
        }

        bool exportComputeDagJson(const wolvrix::lib::grh::Graph &graph,
                                  const ActivityScheduleOptions &options,
                                  const ComputeRewriteBuild &rewrite,
                                  std::string &error)
        {
            if (options.exportComputeDagPath.empty())
            {
                return true;
            }

            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;

            std::vector<OperationId> ops;
            std::unordered_set<OperationId, OperationIdHash> seenOps;
            for (const uint32_t computeNodeId : rewrite.computeTopoOrder)
            {
                if (computeNodeId >= rewrite.computeNodes.size())
                {
                    continue;
                }
                for (const OperationId opId : rewrite.computeNodes[computeNodeId].ops)
                {
                    if (opId.valid() && seenOps.insert(opId).second)
                    {
                        ops.push_back(opId);
                    }
                }
            }

            std::size_t maxOpIndex = 0;
            for (const OperationId opId : ops)
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            std::vector<uint32_t> nodeOfOp(maxOpIndex + 1, kInvalidActivitySupernodeId);
            for (uint32_t nodeId = 0; nodeId < ops.size(); ++nodeId)
            {
                if (ops[nodeId].index < nodeOfOp.size())
                {
                    nodeOfOp[ops[nodeId].index] = nodeId;
                }
            }

            std::map<std::pair<uint32_t, uint32_t>, std::vector<ValueId>> edgeValues;
            for (uint32_t dstNode = 0; dstNode < ops.size(); ++dstNode)
            {
                const OperationId dstOp = ops[dstNode];
                for (const ValueId operand : graph.opOperands(dstOp))
                {
                    const OperationId srcOp = graph.valueDef(operand);
                    if (!srcOp.valid() || srcOp.index >= nodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t srcNode = nodeOfOp[srcOp.index];
                    if (srcNode == kInvalidActivitySupernodeId || srcNode == dstNode)
                    {
                        continue;
                    }
                    auto &values = edgeValues[{srcNode, dstNode}];
                    if (std::find(values.begin(), values.end(), operand) == values.end())
                    {
                        values.push_back(operand);
                    }
                }
            }

            std::vector<std::vector<uint32_t>> opDag(ops.size());
            for (const auto &[pair, values] : edgeValues)
            {
                (void)values;
                if (pair.first < opDag.size() && pair.second < opDag.size())
                {
                    opDag[pair.first].push_back(pair.second);
                }
            }
            for (auto &succs : opDag)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            std::vector<uint32_t> opTopoOrder;
            try
            {
                opTopoOrder = topoOrderForDag(opDag);
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule op-level compute DAG topo failed: ") + ex.what();
                return false;
            }
            if (opTopoOrder.size() != ops.size())
            {
                error = "activity-schedule op-level compute DAG topo failed: missing ops";
                return false;
            }
            std::vector<uint32_t> oldToNew(ops.size(), kInvalidActivitySupernodeId);
            for (uint32_t newNode = 0; newNode < opTopoOrder.size(); ++newNode)
            {
                oldToNew[opTopoOrder[newNode]] = newNode;
            }
            std::map<std::pair<uint32_t, uint32_t>, std::vector<ValueId>> topoEdgeValues;
            for (const auto &[pair, values] : edgeValues)
            {
                const uint32_t src = pair.first < oldToNew.size() ? oldToNew[pair.first]
                                                                  : kInvalidActivitySupernodeId;
                const uint32_t dst = pair.second < oldToNew.size() ? oldToNew[pair.second]
                                                                   : kInvalidActivitySupernodeId;
                if (src == kInvalidActivitySupernodeId || dst == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                topoEdgeValues[{src, dst}] = values;
            }
            edgeValues = std::move(topoEdgeValues);

            const auto valueBitWidth = [](int32_t width) -> std::size_t {
                return width > 0 ? static_cast<std::size_t>(width) : std::size_t{1};
            };
            const auto edgeWeightForValues = [&](const std::vector<ValueId> &values) -> std::size_t {
                std::size_t bits = 0;
                for (const ValueId value : values)
                {
                    bits += valueBitWidth(graph.getValue(value).width());
                }
                return std::max<std::size_t>(std::size_t{1}, (bits + 63) / 64);
            };

            std::size_t edgeWeightTotal = 0;
            for (const auto &[pair, values] : edgeValues)
            {
                (void)pair;
                edgeWeightTotal += edgeWeightForValues(values);
            }

            std::ostringstream out;
            out << "{\n";
            out << "  \"format\":\"wolvrix.compute-op-dag.v1\",\n";
            out << "  \"graph_id\":\"" << escapeJsonString(std::string(graph.symbol()) + ".activity_compute") << "\",\n";
            out << "  \"source\":{\"pass\":\"activity-schedule\",\"path\":\""
                << escapeJsonString(options.path) << "\"},\n";
            out << "  \"options\":{\"node_granularity\":\"op\",\"edge_weight\":\"value_bitwidth_words\"},\n";
            out << "  \"stats\":{\"nodes\":" << ops.size()
                << ",\"edges\":" << edgeValues.size()
                << ",\"edge_weight_total\":" << edgeWeightTotal << "},\n";
            out << "  \"nodes\":[\n";
            for (uint32_t nodeId = 0; nodeId < ops.size(); ++nodeId)
            {
                const OperationId opId = ops[opTopoOrder[nodeId]];
                const auto op = graph.getOperation(opId);
                out << "    {\"id\":" << nodeId
                    << ",\"op_id\":" << opId.index
                    << ",\"kind\":\"" << escapeJsonString(std::string(wolvrix::lib::grh::toString(op.kind())))
                    << "\",\"symbol\":\"" << escapeJsonString(std::string(op.symbolText()))
                    << "\",\"topo_pos\":" << nodeId
                    << ",\"attrs\":{\"granularity\":\"op\""
                    << "}}";
                if (nodeId + 1 != ops.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ],\n";
            out << "  \"edges\":[\n";
            std::size_t edgeIndex = 0;
            for (const auto &[pair, values] : edgeValues)
            {
                const std::size_t edgeWeight = edgeWeightForValues(values);
                out << "    {\"src\":" << pair.first
                    << ",\"dst\":" << pair.second
                    << ",\"weight\":" << edgeWeight
                    << ",\"values\":[";
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    if (i != 0)
                    {
                        out << ",";
                    }
                    const auto valueInfo = graph.getValue(values[i]);
                    const std::size_t width = valueBitWidth(valueInfo.width());
                    out << "{\"id\":" << values[i].index
                        << ",\"width\":" << width << "}";
                }
                out << "]}";
                if (++edgeIndex != edgeValues.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ]\n";
            out << "}\n";

            try
            {
                const std::filesystem::path path(options.exportComputeDagPath);
                if (path.has_parent_path())
                {
                    std::filesystem::create_directories(path.parent_path());
                }
                std::ofstream file(options.exportComputeDagPath);
                if (!file)
                {
                    error = "activity-schedule failed to open compute DAG export path: " +
                            options.exportComputeDagPath;
                    return false;
                }
                file << out.str();
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule compute DAG export failed: ") + ex.what();
                return false;
            }
            return true;
        }

        bool materializeComputeNodeSchedule(const wolvrix::lib::grh::Graph &graph,
                                            const ActivityScheduleOptions &options,
                                            const ActivityOpData &opData,
                                            const ActivityCostModel *costModel,
                                            const std::vector<float> *piByOpIndex,
                                            const ActivityHypergraphAggregate *probSeedHypergraph,
                                            ComputeRewriteBuild &rewrite,
                                            ActivityScheduleBuild &build,
                                            ComputeNodeMaterializePerfStats *perf,
                                            std::string &error)
        {
            ComputeNodeMaterializePerfStats fallbackPerf;
            if (perf == nullptr)
            {
                perf = &fallbackPerf;
            }
            const std::size_t maxOpsPerComputeSupernode = options.maxOpInComputeSupernode;
            const std::size_t maxOpsPerSplitComputeNode =
                options.splitOversizeComputeNodeMaxOps != 0
                    ? options.splitOversizeComputeNodeMaxOps
                    : maxOpsPerComputeSupernode;
            const std::vector<uint32_t> nodeOpSizes = computeNodeOpSizes(rewrite);
            const auto initClustersStart = std::chrono::steady_clock::now();
            std::vector<uint32_t> nodeTopoPos(rewrite.computeNodes.size(), kInvalidActivitySupernodeId);
            for (uint32_t pos = 0; pos < rewrite.computeTopoOrder.size(); ++pos)
            {
                const uint32_t node = rewrite.computeTopoOrder[pos];
                if (node < nodeTopoPos.size())
                {
                    nodeTopoPos[node] = pos;
                }
            }
            std::vector<std::vector<uint32_t>> clusters;
            clusters.reserve(rewrite.computeNodes.size());
            for (const auto node : rewrite.computeTopoOrder)
            {
                clusters.push_back(std::vector<uint32_t>{node});
            }
            clusters = canonicalizeNodeClusters(std::move(clusters), nodeTopoPos);
            if (perf)
            {
                perf->initClustersMs = elapsedMs(initClustersStart);
                perf->clustersBeforeCoarsen = clusters.size();
            }

            const auto topoBeforeStart = std::chrono::steady_clock::now();
            if (!orderNodeClustersTopologically(clusters, rewrite.computeDag, rewrite.computeNodes.size(), &rewrite, &graph))
            {
                error = "activity-schedule compute-node cluster topo failed before coarsen";
                return false;
            }
            if (perf)
            {
                perf->topoBeforeCoarsenMs = elapsedMs(topoBeforeStart);
            }

            const auto coarsenStart = std::chrono::steady_clock::now();
            if (options.enableCoarsen)
            {
                const std::size_t coarsenMaxOps =
                    maxOpsPerComputeSupernode == 0 ? std::numeric_limits<std::size_t>::max()
                                                   : maxOpsPerComputeSupernode;
                bool changed = true;
                std::size_t tailIterations = 0;
                std::size_t cbawIterations = 0;
                constexpr std::size_t kMaxCbawCoarsenIterations = 8;
                while (changed)
                {
                    const auto iterStart = std::chrono::steady_clock::now();
                    const std::size_t clustersBeforeIter = clusters.size();
                    changed = false;
                    bool out1Changed = false;
                    bool in1Changed = false;
                    bool siblingsChanged = false;
                    bool probChanged = false;
                    bool cbawChanged = false;
                    if (options.partitionPolicy == "prob")
                    {
                        if (costModel == nullptr || piByOpIndex == nullptr)
                        {
                            error = "activity-schedule prob coarsen missing probability cost model";
                            return false;
                        }
                        probChanged = tryMergeNodeProb(clusters,
                                                       rewrite.computeDag,
                                                       rewrite.computeNodes.size(),
                                                       nodeTopoPos,
                                                       nodeOpSizes,
                                                       coarsenMaxOps,
                                                       rewrite,
                                                       graph,
                                                       options,
                                                       opData,
                                                       *costModel,
                                                       *piByOpIndex,
                                                       probSeedHypergraph,
                                                       perf);
                        changed = probChanged || changed;
                    }
                    else if (options.partitionPolicy == "cbaw")
                    {
                        cbawChanged = tryMergeNodeCbaw(clusters,
                                                       rewrite.computeDag,
                                                       rewrite.computeNodes.size(),
                                                       nodeTopoPos,
                                                       nodeOpSizes,
                                                       coarsenMaxOps,
                                                       rewrite,
                                                       graph,
                                                       &opData,
                                                       perf);
                        changed = cbawChanged || changed;
                    }
                    else
                    {
                        if (options.enableChainMerge)
                        {
                            const std::size_t clustersBeforeOut1 = clusters.size();
                            out1Changed = tryMergeNodeOut1(clusters,
                                                           rewrite.computeDag,
                                                           rewrite.computeNodes.size(),
                                                           nodeTopoPos,
                                                           nodeOpSizes,
                                                           coarsenMaxOps,
                                                           rewrite,
                                                           graph);
                            if (out1Changed && perf)
                            {
                                perf->coarsenOut1Merges += clustersBeforeOut1 >= clusters.size()
                                                               ? clustersBeforeOut1 - clusters.size()
                                                               : 0;
                            }
                            changed = out1Changed || changed;

                            const std::size_t clustersBeforeIn1 = clusters.size();
                            in1Changed = tryMergeNodeIn1(clusters,
                                                         rewrite.computeDag,
                                                         rewrite.computeNodes.size(),
                                                         nodeTopoPos,
                                                         nodeOpSizes,
                                                         coarsenMaxOps,
                                                         rewrite,
                                                         graph);
                            if (in1Changed && perf)
                            {
                                perf->coarsenIn1Merges += clustersBeforeIn1 >= clusters.size()
                                                              ? clustersBeforeIn1 - clusters.size()
                                                              : 0;
                            }
                            changed = in1Changed || changed;
                        }
                        const std::size_t clustersBeforeSiblings = clusters.size();
                        siblingsChanged = tryMergeNodeSiblings(clusters,
                                                               rewrite.computeDag,
                                                               rewrite.computeNodes.size(),
                                                               nodeTopoPos,
                                                               nodeOpSizes,
                                                               coarsenMaxOps,
                                                               rewrite,
                                                               graph);
                        if (siblingsChanged && perf)
                        {
                            perf->coarsenSiblingMerges += clustersBeforeSiblings >= clusters.size()
                                                              ? clustersBeforeSiblings - clusters.size()
                                                              : 0;
                        }
                        changed = siblingsChanged || changed;
                    }
                    if (perf)
                    {
                        const std::size_t clustersAfterIter = clusters.size();
                        const std::size_t clusterDelta =
                            clustersBeforeIter >= clustersAfterIter ? (clustersBeforeIter - clustersAfterIter) : 0;
                        if (options.partitionPolicy == "cbaw")
                        {
                            ++cbawIterations;
                        }
                        const bool cbawRoundLimitStop =
                            options.partitionPolicy == "cbaw" &&
                            cbawIterations >= kMaxCbawCoarsenIterations &&
                            changed;
                        const bool smallDeltaTail =
                            clustersBeforeIter >= kComputeNodeCoarsenTailLargeClusterThreshold &&
                            clusterDelta < kComputeNodeCoarsenTailMaxClusterDeltaExclusive;
                        if (changed && smallDeltaTail)
                        {
                            ++tailIterations;
                        }
                        else
                        {
                            tailIterations = 0;
                        }
                        const bool tailStopped =
                            cbawRoundLimitStop ||
                            tailIterations >= kComputeNodeCoarsenTailMaxConsecutiveIters;
                        if (tailStopped)
                        {
                            changed = false;
                            perf->coarsenTailStopped = true;
                            perf->coarsenTailIterations = tailIterations;
                        }
                        ++perf->coarsenIterations;
                        perf->coarsenIterationStats.push_back({
                            .iteration = perf->coarsenIterations,
                            .clusters = clustersAfterIter,
                            .clusterDelta = clusterDelta,
                            .changed = changed,
                            .out1Changed = out1Changed,
                            .in1Changed = in1Changed,
                            .siblingsChanged = siblingsChanged,
                            .probChanged = probChanged || cbawChanged,
                            .tailStopped = tailStopped,
                            .elapsedMs = elapsedMs(iterStart),
                        });
                    }
                    else
                    {
                        const std::size_t clustersAfterIter = clusters.size();
                        const std::size_t clusterDelta =
                            clustersBeforeIter >= clustersAfterIter ? (clustersBeforeIter - clustersAfterIter) : 0;
                        if (options.partitionPolicy == "cbaw")
                        {
                            ++cbawIterations;
                        }
                        const bool cbawRoundLimitStop =
                            options.partitionPolicy == "cbaw" &&
                            cbawIterations >= kMaxCbawCoarsenIterations &&
                            changed;
                        const bool smallDeltaTail =
                            clustersBeforeIter >= kComputeNodeCoarsenTailLargeClusterThreshold &&
                            clusterDelta < kComputeNodeCoarsenTailMaxClusterDeltaExclusive;
                        if (changed && smallDeltaTail)
                        {
                            ++tailIterations;
                        }
                        else
                        {
                            tailIterations = 0;
                        }
                        if (cbawRoundLimitStop ||
                            tailIterations >= kComputeNodeCoarsenTailMaxConsecutiveIters)
                        {
                            changed = false;
                        }
                    }
                }
            }
            if (perf)
            {
                perf->coarsenMs = elapsedMs(coarsenStart);
                perf->clustersAfterCoarsen = clusters.size();
            }

            const auto topoAfterStart = std::chrono::steady_clock::now();
            if (!orderNodeClustersTopologically(clusters, rewrite.computeDag, rewrite.computeNodes.size(), &rewrite, &graph))
            {
                error = "activity-schedule compute-node cluster topo failed after coarsen";
                return false;
            }
            if (perf)
            {
                perf->topoAfterCoarsenMs = elapsedMs(topoAfterStart);
            }

            const auto buildClusterViewStart = std::chrono::steady_clock::now();
            const NodeClusterView clusterView =
                buildNodeClusterView(clusters, rewrite.computeDag, rewrite.computeNodes.size());
            if (perf)
            {
                perf->buildClusterViewMs = elapsedMs(buildClusterViewStart);
            }
            const std::string coarsenShape =
                summarizeCoarsenClusterShape(clusterView, rewrite, graph, nodeOpSizes);
            std::fprintf(stderr,
                         "activity-schedule: activity-schedule compute-node coarsen shape: %s\n",
                         coarsenShape.c_str());

            const auto dpSegmentStart = std::chrono::steady_clock::now();
            const auto clusterValueEdges = buildClusterValueEdges(clusterView, rewrite, graph);
            recordInitialComputeSupernodeStats(clusterView,
                                               clusterValueEdges,
                                               rewrite,
                                               graph,
                                               nodeOpSizes);
            if (perf && options.partitionPolicy == "cbaw")
            {
                perf->cbawAfterCoarsen =
                    buildCbawStageStatsForView(clusterView,
                                               clusterValueEdges,
                                               rewrite,
                                               graph,
                                               nodeOpSizes,
                                               0);
                perf->cbawAfterCoarsenRootStages =
                    buildCbawRootStageEntriesForOwners(
                        clusterView,
                        clusterValueEdges,
                        rewrite,
                        graph,
                        cbawIdentityOwnerByCluster(clusterView.members.size()));
            }
            std::vector<double> probSegmentValueWeights;
            const std::vector<double> *segmentValueWeights = nullptr;
            if (options.partitionPolicy == "prob" && options.probDpCost && piByOpIndex != nullptr)
            {
                probSegmentValueWeights =
                    buildProbSegmentValueWeights(graph,
                                                 options,
                                                 rewrite,
                                                 *costModel,
                                                 clusterValueEdges,
                                                 *piByOpIndex);
                segmentValueWeights = &probSegmentValueWeights;
            }
            const double dpSegmentPenalty =
                (options.partitionPolicy == "prob" && options.probDpCost)
                    ? std::max(0.0, options.probDpSegmentPenalty)
                    : 1.0;
            std::vector<std::vector<uint32_t>> segments =
                buildComputeSupernodeSegments(clusterView,
                                              clusterValueEdges,
                                              nodeOpSizes,
                                              maxOpsPerComputeSupernode,
                                              segmentValueWeights,
                                              dpSegmentPenalty);
            if (perf)
            {
                perf->dpSegmentMs = elapsedMs(dpSegmentStart);
                perf->segments = segments.size();
                if (options.partitionPolicy == "cbaw")
                {
                    const auto dpComputeSupernodes =
                        flattenNodeSegments(clusterView, segments, nodeTopoPos);
                    perf->cbawAfterDp =
                        buildCbawStageStatsForComputeSupernodes(dpComputeSupernodes,
                                                                rewrite.computeDag,
                                                                rewrite.computeNodes.size(),
                                                                rewrite,
                                                                graph,
                                                                nodeOpSizes,
                                                                clusterView.members.size(),
                                                                segments.size());
                    perf->cbawAfterDpRootStages =
                        buildCbawRootStageEntriesForOwners(
                            clusterView,
                            clusterValueEdges,
                            rewrite,
                            graph,
                            cbawOwnerByClusterFromSegments(clusterView.members.size(), segments));
                }
            }

            const auto fmRefineStart = std::chrono::steady_clock::now();
            if (options.partitionPolicy == "prob" &&
                options.fmRefineMaxRounds != 0 &&
                costModel != nullptr &&
                piByOpIndex != nullptr)
            {
                std::vector<double> fmValueWeights =
                    !probSegmentValueWeights.empty()
                        ? probSegmentValueWeights
                        : buildProbSegmentValueWeights(graph,
                                                       options,
                                                       rewrite,
                                                       *costModel,
                                                       clusterValueEdges,
                                                       *piByOpIndex);
                const bool canUseSeed =
                    probSeedHypergraph != nullptr &&
                    probSeedHypergraph->nodeWeight.size() >= rewrite.computeNodes.size() &&
                    probSeedHypergraph->nodeChangeWeight.size() >= rewrite.computeNodes.size() &&
                    probSeedHypergraph->nodeFootprintBytes.size() >= rewrite.computeNodes.size() &&
                    probSeedHypergraph->nodeActiveProb.size() >= rewrite.computeNodes.size() &&
                    probSeedHypergraph->nodeOpCount.size() >= rewrite.computeNodes.size();
                const ProbCoarsenClusterAggregate fmAgg =
                    canUseSeed
                        ? buildProbCoarsenClusterAggregateFromSeed(*probSeedHypergraph, clusterView)
                        : buildProbCoarsenClusterAggregateFromGraph(graph,
                                                                   options,
                                                                   opData,
                                                                   rewrite,
                                                                   clusterView,
                                                                   *costModel,
                                                                   *piByOpIndex);
                segments = refineComputeSupernodeSegmentsProb(clusterView,
                                                              clusterValueEdges,
                                                              nodeOpSizes,
                                                              std::move(segments),
                                                              fmAgg.aggregate,
                                                              fmValueWeights,
                                                              options,
                                                              maxOpsPerComputeSupernode,
                                                              perf);
            }
            else if (options.partitionPolicy == "cbaw" &&
                     options.fmRefineMaxRounds != 0)
            {
                segments = refineComputeSupernodeSegmentsCbaw(clusterView,
                                                              clusterValueEdges,
                                                              nodeOpSizes,
                                                              std::move(segments),
                                                              options,
                                                              maxOpsPerComputeSupernode,
                                                              perf);
            }
            if (perf)
            {
                perf->fmRefineMs = elapsedMs(fmRefineStart);
                perf->segments = segments.size();
                if (options.partitionPolicy == "cbaw")
                {
                    const auto fmComputeSupernodes =
                        flattenNodeSegments(clusterView, segments, nodeTopoPos);
                    perf->cbawAfterFm =
                        buildCbawStageStatsForComputeSupernodes(fmComputeSupernodes,
                                                                rewrite.computeDag,
                                                                rewrite.computeNodes.size(),
                                                                rewrite,
                                                                graph,
                                                                nodeOpSizes,
                                                                clusterView.members.size(),
                                                                segments.size());
                    perf->cbawAfterFmRootStages =
                        buildCbawRootStageEntriesForOwners(
                            clusterView,
                            clusterValueEdges,
                            rewrite,
                            graph,
                            cbawOwnerByClusterFromSegments(clusterView.members.size(), segments));
                }
            }

            const auto flattenSegmentsStart = std::chrono::steady_clock::now();
            const auto computeSupernodes = flattenNodeSegments(clusterView, segments, nodeTopoPos);
            if (perf)
            {
                perf->flattenSegmentsMs = elapsedMs(flattenSegmentsStart);
                perf->computeSupernodes = computeSupernodes.size();
            }

            const auto buildFinalSupernodesStart = std::chrono::steady_clock::now();
            build = ActivityScheduleBuild{};
            build.supernodeToOps.reserve(computeSupernodes.size() + rewrite.commitNodes.size());
            build.supernodeKinds.reserve(computeSupernodes.size() + rewrite.commitNodes.size());
            build.computeNodesBySupernode.reserve(computeSupernodes.size() + rewrite.commitNodes.size());
            std::vector<uint32_t> splitOwnerComputeNodeBySupernode;
            std::vector<uint32_t> splitOrdinalBySupernode;
            std::vector<uint32_t> splitCountByComputeNode(rewrite.computeNodes.size(), 0);
            auto noteNonSplitSupernode = [&]()
            {
                splitOwnerComputeNodeBySupernode.push_back(kInvalidActivitySupernodeId);
                splitOrdinalBySupernode.push_back(kInvalidActivitySupernodeId);
            };
            for (uint32_t segmentId = 0; segmentId < computeSupernodes.size(); ++segmentId)
            {
                std::vector<wolvrix::lib::grh::OperationId> ops;
                std::vector<uint32_t> supernodeComputeNodes;
                auto flushComputeSupernode = [&]() -> bool
                {
                    if (ops.empty())
                    {
                        return true;
                    }
                    std::vector<wolvrix::lib::grh::OperationId> orderedOps;
                    if (!topoSortLocalOps(graph, ops, orderedOps, error))
                    {
                        return false;
                    }
                    build.supernodeToOps.push_back(std::move(orderedOps));
                    build.supernodeKinds.push_back(ActivityScheduleSupernodeKind::Compute);
                    build.computeNodesBySupernode.push_back(supernodeComputeNodes);
                    noteNonSplitSupernode();
                    ops.clear();
                    supernodeComputeNodes.clear();
                    return true;
                };

                for (const auto computeNodeId : computeSupernodes[segmentId])
                {
                    if (computeNodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    const auto &nodeOps = rewrite.computeNodes[computeNodeId].ops;
                    if (options.splitOversizeComputeNodes &&
                        maxOpsPerSplitComputeNode != 0 &&
                        nodeOps.size() > maxOpsPerSplitComputeNode)
                    {
                        std::vector<wolvrix::lib::grh::OperationId> orderedNodeOps;
                        if (!topoSortLocalOps(graph, nodeOps, orderedNodeOps, error))
                        {
                            return false;
                        }
                        ++perf->splitOversizeComputeNodes;
                        for (std::size_t begin = 0; begin < orderedNodeOps.size(); begin += maxOpsPerSplitComputeNode)
                        {
                            if (!flushComputeSupernode())
                            {
                                return false;
                            }
                            const std::size_t end =
                                std::min(orderedNodeOps.size(), begin + maxOpsPerSplitComputeNode);
                            std::vector<wolvrix::lib::grh::OperationId> chunkOps(
                                orderedNodeOps.begin() + static_cast<std::ptrdiff_t>(begin),
                                orderedNodeOps.begin() + static_cast<std::ptrdiff_t>(end));
                            build.supernodeToOps.push_back(std::move(chunkOps));
                            build.supernodeKinds.push_back(ActivityScheduleSupernodeKind::Compute);
                            build.computeNodesBySupernode.push_back({computeNodeId});
                            splitOwnerComputeNodeBySupernode.push_back(computeNodeId);
                            splitOrdinalBySupernode.push_back(splitCountByComputeNode[computeNodeId]++);
                            ++perf->splitOversizeComputeNodeSupernodes;
                        }
                        continue;
                    }
                    ops.insert(ops.end(), nodeOps.begin(), nodeOps.end());
                    supernodeComputeNodes.push_back(computeNodeId);
                }
                if (!flushComputeSupernode())
                {
                    return false;
                }
            }
            const uint32_t commitBase = static_cast<uint32_t>(build.supernodeToOps.size());
            for (const auto &commit : rewrite.commitNodes)
            {
                build.supernodeToOps.push_back(commit.ops);
                build.supernodeKinds.push_back(ActivityScheduleSupernodeKind::Commit);
                build.computeNodesBySupernode.push_back({});
                noteNonSplitSupernode();
            }
            if (perf)
            {
                perf->buildFinalSupernodesMs = elapsedMs(buildFinalSupernodesStart);
            }

            const auto buildFinalDagStart = std::chrono::steady_clock::now();
            std::size_t maxOpIndex = 0;
            for (const auto opId : graph.operations())
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            build.opToSupernode.assign(maxOpIndex, kInvalidActivitySupernodeId);
            std::vector<uint32_t> supernodeOfOp(maxOpIndex + 1, kInvalidActivitySupernodeId);
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    if (opId.index == 0 || opId.index > maxOpIndex)
                    {
                        continue;
                    }
                    build.opToSupernode[opId.index - 1] = supernodeId;
                    supernodeOfOp[opId.index] = supernodeId;
                }
            }

            build.dag.assign(build.supernodeToOps.size(), {});
            if (!graph.values().empty())
            {
                build.valueFanout.assign(graph.values().back().index, {});
                build.valueSourceKind.assign(graph.values().back().index + 1,
                                             wolvrix::lib::grh::OperationKind::kConstant);
                build.valueSourceSupernode.assign(graph.values().back().index + 1, kInvalidActivitySupernodeId);
                for (const auto valueId : graph.values())
                {
                    if (!valueId.valid() || valueId.index >= build.valueSourceKind.size())
                    {
                        continue;
                    }
                    const auto defOpId = graph.valueDef(valueId);
                    if (defOpId.valid())
                    {
                        build.valueSourceKind[valueId.index] = graph.opKind(defOpId);
                        if (defOpId.index > 0 && defOpId.index - 1 < build.opToSupernode.size())
                        {
                            build.valueSourceSupernode[valueId.index] = build.opToSupernode[defOpId.index - 1];
                        }
                    }
                }
            }
            std::unordered_set<uint64_t> seenEdges;
            const auto addValueDependency = [&](wolvrix::lib::grh::ValueId value,
                                                uint32_t to,
                                                bool skipDagEdge = false) {
                if (!value.valid() || to >= build.supernodeToOps.size())
                {
                    return;
                }
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    if (!skipDagEdge && value.index > 0 && value.index <= build.valueFanout.size())
                    {
                        build.valueFanout[value.index - 1].push_back(to);
                    }
                    return;
                }
                if (defOp.index >= supernodeOfOp.size())
                {
                    return;
                }
                const uint32_t from = supernodeOfOp[defOp.index];
                if (from == kInvalidActivitySupernodeId || from == to)
                {
                    return;
                }
                if (from < build.supernodeKinds.size() &&
                    build.supernodeKinds[from] == ActivityScheduleSupernodeKind::Commit)
                {
                    return;
                }
                if (!skipDagEdge)
                {
                    const uint64_t packed = (static_cast<uint64_t>(from) << 32) | to;
                    if (seenEdges.insert(packed).second)
                    {
                        build.dag[from].push_back(to);
                    }
                }
                if (!skipDagEdge && value.index > 0 && value.index <= build.valueFanout.size())
                {
                    build.valueFanout[value.index - 1].push_back(to);
                }
            };
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto toOpId : build.supernodeToOps[supernodeId])
                {
                    const auto toOp = graph.getOperation(toOpId);
                    for (const auto operand : toOp.operands())
                    {
                        const auto defOp = graph.valueDef(operand);
                        if (!defOp.valid() || defOp.index >= supernodeOfOp.size())
                        {
                            continue;
                        }
                        const uint32_t from = supernodeOfOp[defOp.index];
                        const uint32_t to = supernodeId;
                        bool skipDagEdge = false;
                        if (defOp.index < rewrite.computeNodeOfOp.size() &&
                            toOpId.index < rewrite.computeNodeOfOp.size())
                        {
                            const uint32_t defComputeNode = rewrite.computeNodeOfOp[defOp.index];
                            const uint32_t useComputeNode = rewrite.computeNodeOfOp[toOpId.index];
                            if (defComputeNode != kInvalidActivitySupernodeId &&
                                defComputeNode == useComputeNode &&
                                from != to)
                            {
                                const bool splitForward =
                                    from < splitOwnerComputeNodeBySupernode.size() &&
                                    to < splitOwnerComputeNodeBySupernode.size() &&
                                    splitOwnerComputeNodeBySupernode[from] == defComputeNode &&
                                    splitOwnerComputeNodeBySupernode[to] == defComputeNode &&
                                    from < splitOrdinalBySupernode.size() &&
                                    to < splitOrdinalBySupernode.size() &&
                                    splitOrdinalBySupernode[from] < splitOrdinalBySupernode[to];
                                skipDagEdge = !splitForward;
                            }
                        }
                        if (from == kInvalidActivitySupernodeId || from == to)
                        {
                            continue;
                        }
                        if (from < build.supernodeKinds.size() &&
                            build.supernodeKinds[from] == ActivityScheduleSupernodeKind::Commit)
                        {
                            continue;
                        }
                        if (!skipDagEdge)
                        {
                            const uint64_t packed = (static_cast<uint64_t>(from) << 32) | to;
                            if (seenEdges.insert(packed).second)
                            {
                                build.dag[from].push_back(to);
                            }
                        }
                        if (!skipDagEdge && operand.index > 0 && operand.index <= build.valueFanout.size())
                        {
                            build.valueFanout[operand.index - 1].push_back(to);
                        }
                    }
                    if (isRegToMemIntentSlice(toOp))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, toOp))
                        {
                            addValueDependency(*indexValue, supernodeId);
                        }
                    }
                }
            }
            for (auto &succs : build.dag)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            for (auto &fanout : build.valueFanout)
            {
                std::sort(fanout.begin(), fanout.end());
                fanout.erase(std::unique(fanout.begin(), fanout.end()), fanout.end());
            }
            if (perf)
            {
                perf->buildFinalDagMs = elapsedMs(buildFinalDagStart);
            }

            const auto buildStateReadSetsStart = std::chrono::steady_clock::now();
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId < build.supernodeKinds.size() &&
                    build.supernodeKinds[supernodeId] == ActivityScheduleSupernodeKind::Commit)
                {
                    continue;
                }
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto stateSymbol = stateSymbolForReadOp(graph.getOperation(opId));
                    if (stateSymbol && !stateSymbol->empty())
                    {
                        build.stateReadSupernodes[*stateSymbol].push_back(supernodeId);
                    }
                    const auto op = graph.getOperation(opId);
                    if (isRegToMemIntentSlice(op))
                    {
                        for (const auto &storageSymbol : regToMemIntentSliceStorageReadSymbols(graph, op))
                        {
                            build.stateReadSupernodes[storageSymbol].push_back(supernodeId);
                        }
                    }
                }
            }
            for (auto &[_, supernodes] : build.stateReadSupernodes)
            {
                std::sort(supernodes.begin(), supernodes.end());
                supernodes.erase(std::unique(supernodes.begin(), supernodes.end()), supernodes.end());
            }
            if (perf)
            {
                perf->buildStateReadSetsMs = elapsedMs(buildStateReadSetsStart);
            }

            const auto finalTopoStart = std::chrono::steady_clock::now();
            try
            {
                build.topoOrder = topoOrderForDag(build.dag);
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule final topo failed: ") + ex.what() + " " +
                        describeFinalScheduleCycle(graph,
                                                   rewrite,
                                                   build,
                                                   build.computeNodesBySupernode,
                                                   supernodeOfOp);
                return false;
            }
            if (build.topoOrder.size() != build.supernodeToOps.size())
            {
                error = "activity-schedule final topo failed: missing supernodes";
                return false;
            }
            if (perf)
            {
                perf->finalTopoMs = elapsedMs(finalTopoStart);
            }
            (void)commitBase;
            return true;
        }

    } // namespace

    ActivitySchedulePass::ActivitySchedulePass()
        : Pass("activity-schedule",
               "activity-schedule",
               "Build activity-schedule hypergraph for a single graph"),
          options_({})
    {
    }

    ActivitySchedulePass::ActivitySchedulePass(ActivityScheduleOptions options)
        : Pass("activity-schedule",
               "activity-schedule",
               "Build activity-schedule hypergraph for a single graph"),
          options_(std::move(options))
    {
    }

    PassResult ActivitySchedulePass::run()
    {
        PassResult result;
        const auto totalStart = std::chrono::steady_clock::now();
        if (options_.path.empty())
        {
            error("activity-schedule requires -path");
            result.failed = true;
            return result;
        }

        const std::size_t maxOpsPerComputeSupernode = options_.maxOpInComputeSupernode;
        const std::size_t maxCommitOps = options_.maxOpInCommitSupernode;
        if (maxOpsPerComputeSupernode == 0)
        {
            error("activity-schedule max_op_in_compute_supernode must be >= 1");
            result.failed = true;
            return result;
        }
        if (maxCommitOps == 0)
        {
            error("activity-schedule max_op_in_commit_supernode must be >= 1");
            result.failed = true;
            return result;
        }
        options_.maxOpInComputeSupernode = maxOpsPerComputeSupernode;
        options_.maxOpInCommitSupernode = maxCommitOps;
        if (options_.costModel != "edge-cut")
        {
            error("activity-schedule only supports -cost-model=edge-cut");
            result.failed = true;
            return result;
        }
        if (options_.partitionPolicy != "plain" &&
            options_.partitionPolicy != "prob" &&
            options_.partitionPolicy != "cbaw")
        {
            error("activity-schedule -partition-policy must be \"plain\", \"prob\", or \"cbaw\"");
            result.failed = true;
            return result;
        }
        if (options_.probDpCostMode != "pi" &&
            options_.probDpCostMode != "mixed-pi" &&
            options_.probDpCostMode != "change" &&
            options_.probDpCostMode != "mixed-change")
        {
            error("activity-schedule -prob-dp-cost-mode must be pi, mixed-pi, change, or mixed-change");
            result.failed = true;
            return result;
        }
        logInfo("activity-schedule partition policy: " + options_.partitionPolicy);
        if (options_.partitionPolicy == "prob")
        {
            logInfo("activity-schedule prob dp: enabled=" +
                    std::string(options_.probDpCost ? "true" : "false") +
                    " mode=" + options_.probDpCostMode +
                    " alpha=" + std::to_string(options_.probDpAlpha) +
                    " segment_penalty=" + std::to_string(options_.probDpSegmentPenalty) +
                    " fm_rounds=" + std::to_string(options_.fmRefineMaxRounds));
        }

        std::string resolveError;
        const std::optional<std::string> targetGraphName =
            resolveTargetGraphName(design(), options_.path, resolveError);
        if (!targetGraphName)
        {
            error(resolveError);
            result.failed = true;
            return result;
        }

        auto *graph = design().findGraph(*targetGraphName);
        if (graph == nullptr)
        {
            error("activity-schedule target graph not found: " + *targetGraphName);
            result.failed = true;
            return result;
        }

        bool graphChanged = false;
        std::vector<wolvrix::lib::grh::OperationId> opsNeedingSymbol;
        opsNeedingSymbol.reserve(graph->operations().size());
        for (const auto opId : graph->operations())
        {
            const auto op = graph->getOperation(opId);
            if (isHierLikeOpKind(op.kind()))
            {
                error(*graph,
                      op,
                      "activity-schedule guard: target graph must not contain hierarchical ops kind=" +
                          std::string(wolvrix::lib::grh::toString(op.kind())));
                result.failed = true;
            }
            if (!graph->operationSymbol(opId).valid() && isPartitionableOpKind(op.kind()))
            {
                opsNeedingSymbol.push_back(opId);
            }
        }
        if (result.failed)
        {
            return result;
        }

        for (const auto opId : opsNeedingSymbol)
        {
            graph->setOpSymbol(opId, graph->makeInternalOpSym());
            graphChanged = true;
        }

        const auto buildOpDataStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: build_op_data start graph=" + *targetGraphName);
        graph->freeze();
        std::string buildError;
        ActivityOpData opData = buildActivityOpData(*graph, buildError);
        const std::uint64_t buildOpDataMs = elapsedMs(buildOpDataStart);
        if (!buildError.empty())
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        logInfo("activity-schedule progress: build_op_data done ops=" +
                std::to_string(opData.topoOps.size()) +
                " topo_edges=" + std::to_string(opData.topoEdges.size()) +
                " elapsed_ms=" + std::to_string(buildOpDataMs));

        // NO0207 Phase A：在 PRE-clone 逻辑图上计算静态变化概率（仅 prob 策略；纯分析，
        // 不改 plain 路径与调度输出）。piByOpIndex 在 source clone 后的 opData 重建中仍有效，
        // 用于检视 pi 模型；后续 Phase B/C/E 的成本/增益再消费它。
        std::vector<float> piByOpIndex;
        ActivityCostModel costModel;
        ActivityHypergraphAggregate hypergraph;
        if (options_.partitionPolicy == "prob")
        {
            ActivityPiStats piStats;
            piByOpIndex = computeActivityPi(*graph, options_, opData, piStats);
            logInfo("activity-schedule probability(pi): ops=" +
                    std::to_string(opData.topoOps.size()) +
                    " compute_ops=" + std::to_string(piStats.computeOps) +
                    " high_activity=" + std::to_string(piStats.highActivity) +
                    " multi_source=" + std::to_string(piStats.multiSource) +
                    " hist[0,.05)=" + std::to_string(piStats.histogram[0]) +
                    " [.05,.2)=" + std::to_string(piStats.histogram[1]) +
                    " [.2,.5)=" + std::to_string(piStats.histogram[2]) +
                    " [.5,.8)=" + std::to_string(piStats.histogram[3]) +
                    " [.8,.95)=" + std::to_string(piStats.histogram[4]) +
                    " [.95,1]=" + std::to_string(piStats.histogram[5]));

            std::string piKindLine = "activity-schedule pi mean by kind:";
            for (std::size_t k = 0; k < 64; ++k)
            {
                if (piStats.kindCnt[k] == 0)
                {
                    continue;
                }
                const double mean = piStats.kindSum[k] / static_cast<double>(piStats.kindCnt[k]);
                piKindLine +=
                    " " +
                    std::string(wolvrix::lib::grh::toString(static_cast<wolvrix::lib::grh::OperationKind>(k))) +
                    "=" + std::to_string(mean) + "(" + std::to_string(piStats.kindCnt[k]) + ")";
            }
            logInfo(piKindLine);

            static const char *const kDepthLabels[7] = {"d0", "d1-2", "d3-5", "d6-10",
                                                        "d11-20", "d21-40", "d41+"};
            std::string piDepthLine = "activity-schedule pi mean by depth:";
            for (std::size_t d = 0; d < 7; ++d)
            {
                if (piStats.depthCnt[d] == 0)
                {
                    continue;
                }
                const double mean = piStats.depthSum[d] / static_cast<double>(piStats.depthCnt[d]);
                piDepthLine += " " + std::string(kDepthLabels[d]) + "=" + std::to_string(mean) + "(" +
                               std::to_string(piStats.depthCnt[d]) + ")";
            }
            logInfo(piDepthLine);

            ActivityCostStats costStats;
            costModel = computeActivityCostModel(*graph, opData, piByOpIndex, costStats);
            logInfo("activity-schedule cost-model: ops=" +
                    std::to_string(costStats.eligibleOps) +
                    " const=" + std::to_string(costStats.constOps) +
                    " src=" + std::to_string(costStats.srcOps) +
                    " comp=" + std::to_string(costStats.compOps) +
                    " sink=" + std::to_string(costStats.sinkOps) +
                    " total_weight=" + std::to_string(costStats.totalComputeWeight) +
                    " total_change_weight=" + std::to_string(costStats.totalChangeWeight) +
                    " total_footprint_bytes=" + std::to_string(costStats.totalFootprintBytes) +
                    " units[1]=" + std::to_string(costStats.widthUnitHistogram[0]) +
                    " [2]=" + std::to_string(costStats.widthUnitHistogram[1]) +
                    " [3,4]=" + std::to_string(costStats.widthUnitHistogram[2]) +
                    " [5,8]=" + std::to_string(costStats.widthUnitHistogram[3]) +
                    " [9,16]=" + std::to_string(costStats.widthUnitHistogram[4]) +
                    " [17+]=" + std::to_string(costStats.widthUnitHistogram[5]));

            std::string costKindLine = "activity-schedule cost weight mean by kind:";
            for (std::size_t k = 0; k < 64; ++k)
            {
                if (costStats.kindCnt[k] == 0)
                {
                    continue;
                }
                const double mean = costStats.kindWeightSum[k] / static_cast<double>(costStats.kindCnt[k]);
                costKindLine +=
                    " " +
                    std::string(wolvrix::lib::grh::toString(static_cast<wolvrix::lib::grh::OperationKind>(k))) +
                    "=" + std::to_string(mean) + "(" + std::to_string(costStats.kindCnt[k]) + ")";
            }
            logInfo(costKindLine);
        }

        std::vector<ActivityOpClass> opClasses = buildOpClasses(*graph, opData.maxOpIndex);
        ComputeNodeRewriteStats precloneStats;
        ValueCanonicalMap canonicalValues;
        bool sourceCloneGraphChanged = false;
        const auto sourceCloneStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: source_clone start");
        if (!cloneSourceUsesForCompute(*graph,
                                       opClasses,
                                       precloneStats,
                                       canonicalValues,
                                       sourceCloneGraphChanged,
                                       buildError))
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        logInfo("activity-schedule progress: source_clone done clones=" +
                std::to_string(precloneStats.sourceClonesInComputeNodes) +
                " graph_changed=" + std::string(sourceCloneGraphChanged ? "true" : "false") +
                " elapsed_ms=" + std::to_string(elapsedMs(sourceCloneStart)));
        if (sourceCloneGraphChanged)
        {
            graphChanged = true;
            const auto refreezeStart = std::chrono::steady_clock::now();
            logInfo("activity-schedule progress: source_clone_refreeze start");
            graph->freeze();
            opData = buildActivityOpData(*graph, buildError);
            if (!buildError.empty())
            {
                error(*graph, buildError);
                result.failed = true;
                return result;
            }
            opClasses = buildOpClasses(*graph, opData.maxOpIndex);
            logInfo("activity-schedule progress: source_clone_refreeze done ops=" +
                    std::to_string(opData.topoOps.size()) +
                    " topo_edges=" + std::to_string(opData.topoEdges.size()) +
                    " elapsed_ms=" + std::to_string(elapsedMs(refreezeStart)));
        }
        ComputeRewriteBuild rewrite;
        const auto computeNodeStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: compute_node_build start mode=default");
        const bool computeNodeBuildOk = buildComputeNodeRewrite(*graph,
                                                               options_,
                                                               opData,
                                                               opClasses,
                                                               canonicalValues,
                                                               rewrite,
                                                               buildError);
        if (!computeNodeBuildOk)
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        rewrite.stats.sourceClonesInComputeNodes = precloneStats.sourceClonesInComputeNodes;
        const std::uint64_t computeNodeMs = elapsedMs(computeNodeStart);
        logInfo("activity-schedule progress: compute_node_build done compute_nodes=" +
                std::to_string(rewrite.computeNodes.size()) +
                " commit_nodes=" + std::to_string(rewrite.commitNodes.size()) +
                " cycle_split_iters=" + std::to_string(rewrite.stats.computeNodeCycleSplitIters) +
                " elapsed_ms=" + std::to_string(computeNodeMs));
        if (options_.partitionPolicy == "prob")
        {
            // NO0208 Phase D：核对 computeNode 是否忠实 MFFC（只读）。
            const MffcCoverageStats mffc = measureMffcCoverage(opData, rewrite);
            const double etaEdge = mffc.mffcInternal ? static_cast<double>(mffc.cnInternal) /
                                                           static_cast<double>(mffc.mffcInternal)
                                                     : 0.0;
            const double splitFrac = mffc.mffcInternal
                                         ? static_cast<double>(mffc.mffcInternalCnSplit) /
                                               static_cast<double>(mffc.mffcInternal)
                                         : 0.0;
            logInfo("activity-schedule mffc-coverage: compute_edges=" + std::to_string(mffc.computeEdges) +
                    " mffc_internal=" + std::to_string(mffc.mffcInternal) +
                    " cn_internal=" + std::to_string(mffc.cnInternal) +
                    " mffc_but_cn_split=" + std::to_string(mffc.mffcInternalCnSplit) +
                    " eta_edge=" + std::to_string(etaEdge) +
                    " split_frac=" + std::to_string(splitFrac) +
                    " mffc_groups=" + std::to_string(mffc.mffcGroups) +
                    " compute_nodes=" + std::to_string(rewrite.computeNodes.size()));

            // NO0207 Phase C：以 computeNode=MFFC seed 为节点，构建概率超图聚合（只读）。
            const auto hyperStart = std::chrono::steady_clock::now();
            std::vector<std::vector<uint32_t>> singletonClusters(rewrite.computeNodes.size());
            for (uint32_t nodeId = 0; nodeId < rewrite.computeNodes.size(); ++nodeId)
            {
                singletonClusters[nodeId].push_back(nodeId);
            }
            const NodeClusterView hyperView =
                buildNodeClusterView(singletonClusters, rewrite.computeDag, rewrite.computeNodes.size());
            hypergraph = buildActivityHypergraphAggregate(*graph,
                                                          options_,
                                                          opData,
                                                          rewrite,
                                                          hyperView,
                                                          costModel,
                                                          piByOpIndex);
            logInfo("activity-schedule probability-hypergraph: " +
                    summarizeActivityHypergraphAggregate(hypergraph) +
                    " elapsed_ms=" + std::to_string(elapsedMs(hyperStart)));
            logInfo("activity-schedule probability-hypergraph top-footprint: " +
                    summarizeTopActivityHypergraphNodes(hypergraph, 8));
        }
        if (!exportComputeDagJson(*graph, options_, rewrite, buildError))
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        if (!options_.exportComputeDagPath.empty())
        {
            logInfo("activity-schedule compute DAG exported: path=" + options_.exportComputeDagPath);
        }
        if (rewrite.stats.sourceClonesInComputeNodes != 0)
        {
            graphChanged = true;
        }

        const auto freezeStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: freeze_after_compute_node start");
        graph->freeze();
        const std::uint64_t freezeMs = elapsedMs(freezeStart);
        logInfo("activity-schedule progress: freeze_after_compute_node done elapsed_ms=" +
                std::to_string(freezeMs));

        ActivityScheduleBuild build;
        ComputeNodeMaterializePerfStats materializePerf;
        std::optional<ActivityScheduleSummaryStats> cbawPlainBaselineStats;
        ComputeNodeMaterializePerfStats cbawPlainBaselinePerf;
        const bool hasExternalCbawPlainBaseline =
            options_.cbawPlainBoundaryBaseline != 0 &&
            options_.cbawPlainDagBaseline != 0 &&
            options_.cbawPlainComputeComputeBaseline != 0;
        if (options_.partitionPolicy == "cbaw" && hasExternalCbawPlainBaseline)
        {
            ActivityScheduleSummaryStats externalBaseline;
            externalBaseline.boundaryActivationEdges = options_.cbawPlainBoundaryBaseline;
            externalBaseline.dagEdges = options_.cbawPlainDagBaseline;
            externalBaseline.computeComputeValuePairs = options_.cbawPlainComputeComputeBaseline;
            cbawPlainBaselineStats = externalBaseline;
            logInfo("activity-schedule progress: cbaw_plain_gate_baseline external boundary=" +
                    std::to_string(cbawPlainBaselineStats->boundaryActivationEdges) +
                    " dag=" + std::to_string(cbawPlainBaselineStats->dagEdges) +
                    " compute_compute=" +
                    std::to_string(cbawPlainBaselineStats->computeComputeValuePairs));
        }
        constexpr std::size_t kCbawInternalPlainBaselineMaxOps = 200000;
        if (options_.partitionPolicy == "cbaw" &&
            !hasExternalCbawPlainBaseline &&
            opData.topoOps.size() <= kCbawInternalPlainBaselineMaxOps)
        {
            ActivityScheduleOptions plainBaselineOptions = options_;
            plainBaselineOptions.partitionPolicy = "plain";
            ActivityScheduleBuild plainBaselineBuild;
            logInfo("activity-schedule progress: cbaw_plain_gate_baseline start");
            if (!materializeComputeNodeSchedule(*graph,
                                                plainBaselineOptions,
                                                opData,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                rewrite,
                                                plainBaselineBuild,
                                                &cbawPlainBaselinePerf,
                                                buildError))
            {
                error(*graph, buildError);
                result.failed = true;
                return result;
            }
            cbawPlainBaselineStats =
                buildActivityScheduleSummaryStats(plainBaselineBuild,
                                                  rewrite,
                                                  opData,
                                                  *graph,
                                                  nullptr);
            logInfo("activity-schedule progress: cbaw_plain_gate_baseline done supernodes=" +
                    std::to_string(plainBaselineBuild.supernodeToOps.size()) +
                    " boundary=" +
                    std::to_string(cbawPlainBaselineStats->boundaryActivationEdges) +
                    " dag=" +
                    std::to_string(cbawPlainBaselineStats->dagEdges) +
                    " compute_compute=" +
                    std::to_string(
                        cbawPlainBaselineStats->computeComputeValuePairs));
        }
        else if (options_.partitionPolicy == "cbaw" && !hasExternalCbawPlainBaseline)
        {
            logInfo("activity-schedule progress: cbaw_plain_gate_baseline skipped reason=large_graph ops=" +
                    std::to_string(opData.topoOps.size()) +
                    " max_internal_ops=" +
                    std::to_string(kCbawInternalPlainBaselineMaxOps));
        }
        const auto materializeStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: final_materialize start");
        if (options_.partitionPolicy == "prob")
        {
            logInfo("activity-schedule partition_policy=prob: using probability-driven compute coarsen");
        }
        if (!materializeComputeNodeSchedule(*graph,
                                            options_,
                                            opData,
                                            options_.partitionPolicy == "prob" ? &costModel : nullptr,
                                            options_.partitionPolicy == "prob" ? &piByOpIndex : nullptr,
                                            options_.partitionPolicy == "prob" ? &hypergraph : nullptr,
                                            rewrite,
                                            build,
                                            &materializePerf,
                                            buildError))
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        const std::uint64_t materializeMs = elapsedMs(materializeStart);
        logInfo("activity-schedule progress: final_materialize done supernodes=" +
                std::to_string(build.supernodeToOps.size()) +
                " elapsed_ms=" + std::to_string(materializeMs));

        const std::string keyPrefix = options_.path + ".activity_schedule.";
        const auto exportStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: export_session start");
        setSessionValue(keyPrefix + "supernode_to_ops",
                        build.supernodeToOps,
                        "activity-schedule.supernode-to-ops");
        setSessionValue(keyPrefix + "op_to_supernode",
                        build.opToSupernode,
                        "activity-schedule.op-to-supernode");
        setSessionValue(keyPrefix + "dag", build.dag, "activity-schedule.dag");
        setSessionValue(keyPrefix + "supernode_kind",
                        build.supernodeKinds,
                        "activity-schedule.supernode-kind");
        setSessionValue(keyPrefix + "compute_nodes_by_supernode",
                        build.computeNodesBySupernode,
                        "activity-schedule.compute-nodes-by-supernode");
        setSessionValue(keyPrefix + "value_fanout", build.valueFanout, "activity-schedule.value-fanout");
        setSessionValue(keyPrefix + "topo_order", build.topoOrder, "activity-schedule.topo-order");
        setSessionValue(keyPrefix + "state_read_supernodes",
                        build.stateReadSupernodes,
                        "activity-schedule.state-read-supernodes");
        if (!piByOpIndex.empty())
        {
            // NO0207 Phase A：导出 PRE-clone 静态变化概率，供检视/校验（按 op index，-1=非 eligible）。
            setSessionValue(keyPrefix + "op_pi", piByOpIndex, "activity-schedule.op-pi");
        }
        if (!costModel.computeWeightByOpIndex.empty())
        {
            // NO0207 Phase B：导出 PRE-clone op 级成本模型。compute_weight 是执行槽位成本，
            // change_weight=compute_weight*pi，footprint_bytes 是宿主 L1D 工作集估计。
            setSessionValue(keyPrefix + "op_compute_weight",
                            costModel.computeWeightByOpIndex,
                            "activity-schedule.op-compute-weight");
            setSessionValue(keyPrefix + "op_change_weight",
                            costModel.changeWeightByOpIndex,
                            "activity-schedule.op-change-weight");
            setSessionValue(keyPrefix + "op_footprint_bytes",
                            costModel.footprintBytesByOpIndex,
                            "activity-schedule.op-footprint-bytes");
        }
        if (!hypergraph.nodeWeight.empty())
        {
            // NO0207 Phase C：导出 computeNode 层概率超图聚合。当前仅分析/供后续 Phase E/F 消费，
            // 不影响 materialize 输出。
            setSessionValue(keyPrefix + "compute_node_weight",
                            hypergraph.nodeWeight,
                            "activity-schedule.compute-node-weight");
            setSessionValue(keyPrefix + "compute_node_change_weight",
                            hypergraph.nodeChangeWeight,
                            "activity-schedule.compute-node-change-weight");
            setSessionValue(keyPrefix + "compute_node_footprint_bytes",
                            hypergraph.nodeFootprintBytes,
                            "activity-schedule.compute-node-footprint-bytes");
            setSessionValue(keyPrefix + "compute_node_active_prob",
                            hypergraph.nodeActiveProb,
                            "activity-schedule.compute-node-active-prob");
            setSessionValue(keyPrefix + "compute_node_min_topo",
                            hypergraph.nodeMinTopo,
                            "activity-schedule.compute-node-min-topo");
            setSessionValue(keyPrefix + "compute_node_max_topo",
                            hypergraph.nodeMaxTopo,
                            "activity-schedule.compute-node-max-topo");
            setSessionValue(keyPrefix + "compute_node_op_count",
                            hypergraph.nodeOpCount,
                            "activity-schedule.compute-node-op-count");
            setSessionValue(keyPrefix + "compute_node_edge_from",
                            hypergraph.edgeFrom,
                            "activity-schedule.compute-node-edge-from");
            setSessionValue(keyPrefix + "compute_node_edge_to",
                            hypergraph.edgeTo,
                            "activity-schedule.compute-node-edge-to");
            setSessionValue(keyPrefix + "compute_node_edge_count",
                            hypergraph.edgeCount,
                            "activity-schedule.compute-node-edge-count");
            setSessionValue(keyPrefix + "compute_node_edge_total_prob",
                            hypergraph.edgeTotalProb,
                            "activity-schedule.compute-node-edge-total-prob");
        }
        if (options_.partitionPolicy == "prob")
        {
            std::ostringstream probStats;
            probStats << "candidates=" << materializePerf.probCoarsenCandidates
                      << " merges=" << materializePerf.probCoarsenMerges
                      << " gain=" << materializePerf.probCoarsenTotalGain
                      << " reject_size=" << materializePerf.probCoarsenRejectedSize
                      << " reject_footprint=" << materializePerf.probCoarsenRejectedFootprint
                      << " reject_phi=" << materializePerf.probCoarsenRejectedPhi
                      << " reject_weight=" << materializePerf.probCoarsenRejectedWeight
                      << " reject_cycle=" << materializePerf.probCoarsenRejectedCycle
                      << " seed_aggregates=" << materializePerf.probCoarsenSeedAggregates
                      << " full_aggregates=" << materializePerf.probCoarsenFullAggregates
                      << " aggregate_ms=" << materializePerf.probCoarsenAggregateMs
                      << " fm_rounds=" << materializePerf.fmRefineRounds
                      << " fm_candidates=" << materializePerf.fmRefineCandidates
                      << " fm_moves=" << materializePerf.fmRefineMoves
                      << " fm_gain=" << materializePerf.fmRefineTotalGain
                      << " fm_reject_size=" << materializePerf.fmRefineRejectedSize
                      << " fm_reject_footprint=" << materializePerf.fmRefineRejectedFootprint
                      << " fm_reject_phi=" << materializePerf.fmRefineRejectedPhi
                      << " fm_reject_weight=" << materializePerf.fmRefineRejectedWeight
                      << " fm_reject_cycle=" << materializePerf.fmRefineRejectedCycle
                      << " fm_ms=" << materializePerf.fmRefineMs;
            setSessionValue(keyPrefix + "prob_coarsen_stats",
                            probStats.str(),
                            "activity-schedule.prob-coarsen-stats");
        }
        if (options_.partitionPolicy == "cbaw")
        {
            const auto formatPerfCounts =
                [](const ComputeNodeMaterializePerfStats::KindCountMap &counts)
            {
                std::ostringstream oss;
                bool first = true;
                for (const auto &[kind, count] : counts)
                {
                    if (!first)
                    {
                        oss << ",";
                    }
                    first = false;
                    oss << kind << ":" << count;
                }
                return first ? std::string("-") : oss.str();
            };
            std::ostringstream cbawCoarsenStats;
            cbawCoarsenStats
                << "candidates=" << materializePerf.cbawCoarsenCandidates
                << " evaluated=" << materializePerf.cbawCoarsenEvaluated
                << " accepted=" << materializePerf.cbawCoarsenMerges
                << " reject_no_gain=" << materializePerf.cbawCoarsenRejectedNoGain
                << " reject_resource=" << materializePerf.cbawCoarsenRejectedResource
                << " reject_cycle=" << materializePerf.cbawCoarsenRejectedCycle
                << " stale=" << materializePerf.cbawCoarsenStale
                << " generated_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenGeneratedByKind)
                << " dedup_selected_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenDedupSelectedByKind)
                << " dedup_lost_tag_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenDedupLostTagByKind)
                << " selected_reason="
                << formatPerfCounts(materializePerf.cbawCoarsenSelectedReason)
                << " evaluated_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenEvaluatedByKind)
                << " accepted_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenAcceptedByKind)
                << " accepted_by_tag="
                << formatPerfCounts(materializePerf.cbawCoarsenAcceptedByTag)
                << " reject_no_gain_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenRejectedNoGainByKind)
                << " reject_resource_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenRejectedResourceByKind)
                << " reject_cycle_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenRejectedCycleByKind)
                << " stale_by_kind="
                << formatPerfCounts(materializePerf.cbawCoarsenStaleByKind)
                << " reject_no_gain_by_tag="
                << formatPerfCounts(materializePerf.cbawCoarsenRejectedNoGainByTag)
                << " reject_resource_by_tag="
                << formatPerfCounts(materializePerf.cbawCoarsenRejectedResourceByTag)
                << " reject_cycle_by_tag="
                << formatPerfCounts(materializePerf.cbawCoarsenRejectedCycleByTag)
                << " stale_by_tag="
                << formatPerfCounts(materializePerf.cbawCoarsenStaleByTag)
                << " after_p5_boundary="
                << materializePerf.cbawAfterCoarsen.boundaryActivationEdges
                << " after_p5_dag=" << materializePerf.cbawAfterCoarsen.dagEdges
                << " after_p5_compute_compute="
                << materializePerf.cbawAfterCoarsen.computeComputeValuePairs
                << " after_dp_boundary="
                << materializePerf.cbawAfterDp.boundaryActivationEdges
                << " after_dp_dag=" << materializePerf.cbawAfterDp.dagEdges
                << " after_dp_compute_compute="
                << materializePerf.cbawAfterDp.computeComputeValuePairs
                << " after_fm_boundary="
                << materializePerf.cbawAfterFm.boundaryActivationEdges
                << " after_fm_dag=" << materializePerf.cbawAfterFm.dagEdges
                << " after_fm_compute_compute="
                << materializePerf.cbawAfterFm.computeComputeValuePairs
                << " fm_reject_size_fill="
                << formatPerfCounts(materializePerf.fmRefineRejectedSizeFillBucket)
                << " fm_reject_cycle_relation="
                << formatPerfCounts(materializePerf.fmRefineRejectedCycleRelation);
            setSessionValue(keyPrefix + "cbaw_coarsen_stats",
                            cbawCoarsenStats.str(),
                            "activity-schedule.cbaw-coarsen-stats");
        }
        ActivityScheduleValueWeightStats valueWeightStats;
        const ActivityScheduleValueWeightStats *summaryValueWeights = nullptr;
        if (options_.partitionPolicy == "prob" && !piByOpIndex.empty())
        {
            valueWeightStats = buildActivityScheduleValueWeightStats(*graph,
                                                                     options_,
                                                                     rewrite,
                                                                     costModel,
                                                                     piByOpIndex);
            summaryValueWeights = &valueWeightStats;
        }
        const ActivityScheduleSummaryStats summaryStats =
            buildActivityScheduleSummaryStats(build, rewrite, opData, *graph, summaryValueWeights);
        setSessionValue(keyPrefix + "summary_stats",
                        encodeActivityScheduleSummaryStatsJson(summaryStats),
                        "stats");
        ActivityScheduleCbawStats cbawStats =
            buildActivityScheduleCbawStats(build, rewrite, opData, *graph, summaryStats);
        if (options_.partitionPolicy == "cbaw")
        {
            attachCbawTopMultiplicityDiagnostics(cbawStats,
                                                 materializePerf,
                                                 build,
                                                 rewrite,
                                                 *graph);
        }
        setSessionValue(keyPrefix + "cbaw_stats",
                        encodeActivityScheduleCbawStatsJson(cbawStats),
                        "activity-schedule.cbaw-stats");
        const CbawStructureGateReport cbawGate =
            buildCbawStructureGateReport(cbawPlainBaselineStats ? &*cbawPlainBaselineStats : nullptr,
                                         cbawStats);
        if (options_.partitionPolicy == "cbaw")
        {
            std::ostringstream gateStats;
            gateStats << "runtime_allowed=" << (cbawGate.runtimeAllowed ? 1 : 0)
                      << " reason=" << cbawGate.reason
                      << " structural_pass=" << (cbawGate.structuralPass ? 1 : 0)
                      << " trigger_pass=" << (cbawGate.triggerPass ? 1 : 0)
                      << " resource_pass=" << (cbawGate.resourcePass ? 1 : 0)
                      << " dag_pass=" << (cbawGate.dagPass ? 1 : 0)
                      << " plain_boundary=" << cbawGate.plainCrossBoundaryTargets
                      << " cbaw_boundary=" << cbawGate.cbawCrossBoundaryTargets
                      << " plain_dag=" << cbawGate.plainDagEdges
                      << " cbaw_dag=" << cbawGate.cbawDagEdges
                      << " plain_compute_compute="
                      << cbawGate.plainComputeMaterializedTargets
                      << " cbaw_compute_compute="
                      << cbawGate.cbawComputeMaterializedTargets
                      << " cbaw_trigger_p99="
                      << cbawGate.cbawTriggerEstimatedP99
                      << " resource_op_exceptions="
                      << cbawGate.cbawResourceOpCountExceptions;
            setSessionValue(keyPrefix + "cbaw_gate_stats",
                            gateStats.str(),
                            "activity-schedule.cbaw-gate-stats");
        }
        const std::uint64_t exportMs = elapsedMs(exportStart);
        logInfo("activity-schedule progress: export_session done elapsed_ms=" +
                std::to_string(exportMs));
        logInfo("activity-schedule cbaw p0 replay: cross_boundary_target_count=" +
                std::to_string(cbawStats.crossBoundaryTargetCount) +
                " supernode_dependency_edge_count=" +
                std::to_string(cbawStats.supernodeDependencyEdgeCount) +
                " compute_materialized_value_target_count=" +
                std::to_string(cbawStats.computeMaterializedValueTargetCount) +
                " boundary_delta=" +
                std::to_string(cbawStats.replayBoundaryActivationDelta) +
                " dag_delta=" + std::to_string(cbawStats.replayDagEdgeDelta) +
                " compute_compute_delta=" +
                std::to_string(cbawStats.replayComputeComputeDelta) +
                " op_count_p99=" +
                std::to_string(cbawStats.computeSupernodeOpCountP99) +
                " op_count_p995=" +
                std::to_string(cbawStats.computeSupernodeOpCountP995) +
                " op_count_cap=" + std::to_string(cbawStats.resourceOpCountCap) +
                " op_count_exceptions=" +
                std::to_string(cbawStats.resourceOpCountBaselineExceptions));
        logInfo("activity-schedule cbaw p1 trigger: volatile_sources=" +
                std::to_string(cbawStats.triggerVolatileSourceValues) +
                " popcount_p50=" +
                std::to_string(cbawStats.triggerSignaturePopcountP50) +
                " popcount_p99=" +
                std::to_string(cbawStats.triggerSignaturePopcountP99) +
                " saturated_ratio_ppm=" +
                std::to_string(cbawStats.triggerSignatureSaturatedRatioPpm) +
                " non_empty_equal_bucket_covered_ppm=" +
                std::to_string(cbawStats.triggerNonEmptyEqualBucketCoveredSupernodeRatioPpm) +
                " non_empty_internalizable_compute_targets=" +
                std::to_string(cbawStats.triggerNonEmptyEqualBucketInternalizableComputeTargets) +
                " ate_equal_merge_recommended=" +
                std::to_string(cbawStats.triggerAteEqualMergeRecommended) +
                " no_go_reason=" + cbawStats.triggerAteNoGoReason);
        logInfo("activity-schedule cbaw p2 semantic: seed_groups=" +
                std::to_string(cbawStats.semanticSeedGroups) +
                " merge_hint_groups=" + std::to_string(cbawStats.semanticMergeHintGroups) +
                " debug_labels=" + std::to_string(cbawStats.semanticDebugLabels) +
                " rtm_groups=" + std::to_string(cbawStats.semanticRtmIntentGroups) +
                " mffc_groups=" + std::to_string(cbawStats.semanticMffcGroups) +
                " plain_out1=" + std::to_string(cbawStats.semanticPlainOut1Hints) +
                " plain_in1=" + std::to_string(cbawStats.semanticPlainIn1Hints) +
                " aggregate_families=" + std::to_string(cbawStats.semanticAggregateFamilies) +
                " guard_domains=" + std::to_string(cbawStats.semanticGuardDomains) +
                " sink_labels=" + std::to_string(cbawStats.semanticSinkConeLabels) +
                " passthrough_chains=" + std::to_string(cbawStats.semanticPassthroughChains) +
                " top_root_attributed=" +
                std::to_string(cbawStats.semanticTopRootAttributedCount));
        logInfo("activity-schedule cbaw p3 atom: atom_count=" +
                std::to_string(cbawStats.cbawAtomCount) +
                " quotient_edges=" + std::to_string(cbawStats.cbawAtomQuotientEdges) +
                " quotient_cycle=" + std::to_string(cbawStats.cbawAtomQuotientCycleDetected) +
                " op_count_p99=" + std::to_string(cbawStats.cbawAtomOpCountP99) +
                " op_count_p995=" + std::to_string(cbawStats.cbawAtomOpCountP995) +
                " op_count_cap=" + std::to_string(cbawStats.cbawAtomResourceOpCountCap) +
                " plain_replay_supernodes=" +
                std::to_string(cbawStats.cbawAtomPlainReplaySupernodes) +
                " plain_replay_boundary_delta=" +
                std::to_string(cbawStats.cbawAtomPlainReplayBoundaryDelta) +
                " plain_replay_dag_delta=" +
                std::to_string(cbawStats.cbawAtomPlainReplayDagDelta) +
                " plain_replay_compute_compute_delta=" +
                std::to_string(cbawStats.cbawAtomPlainReplayComputeComputeDelta));
        if (options_.partitionPolicy == "cbaw")
        {
            const auto cbawKindCount =
                [](const ComputeNodeMaterializePerfStats::KindCountMap &counts,
                   std::string_view kind)
            {
                const auto it = counts.find(std::string(kind));
                return it == counts.end() ? std::size_t{0} : it->second;
            };
            const auto formatCounts =
                [](const ComputeNodeMaterializePerfStats::KindCountMap &counts)
            {
                std::ostringstream oss;
                bool first = true;
                for (const auto &[kind, count] : counts)
                {
                    if (!first)
                    {
                        oss << ",";
                    }
                    first = false;
                    oss << kind << ":" << count;
                }
                return first ? std::string("-") : oss.str();
            };
            const auto logStage =
                [&](std::string_view stage,
                    const ComputeNodeMaterializePerfStats::CbawStageStats &stats)
            {
                logInfo("activity-schedule cbaw stage stats: stage=" + std::string(stage) +
                        " boundary_activation_edges=" +
                        std::to_string(stats.boundaryActivationEdges) +
                        " dag_edges=" + std::to_string(stats.dagEdges) +
                        " compute_compute_value_pairs=" +
                        std::to_string(stats.computeComputeValuePairs) +
                        " cluster_count=" + std::to_string(stats.clusterCount) +
                        " segment_count=" + std::to_string(stats.segmentCount) +
                        " compute_supernode_count=" +
                        std::to_string(stats.computeSupernodeCount) +
                        " op_count_p50=" + std::to_string(stats.opCountP50) +
                        " op_count_p90=" + std::to_string(stats.opCountP90) +
                        " op_count_p99=" + std::to_string(stats.opCountP99) +
                        " op_count_max=" + std::to_string(stats.opCountMax));
            };
            logStage("after_p5_coarsen", materializePerf.cbawAfterCoarsen);
            logStage("after_dp_before_fm", materializePerf.cbawAfterDp);
            logStage("after_fm", materializePerf.cbawAfterFm);
            logInfo("activity-schedule cbaw p5 coarsen: candidates=" +
                    std::to_string(materializePerf.cbawCoarsenCandidates) +
                    " evaluated=" +
                    std::to_string(materializePerf.cbawCoarsenEvaluated) +
                    " accepted=" +
                    std::to_string(materializePerf.cbawCoarsenMerges) +
                    " reject_no_gain=" +
                    std::to_string(materializePerf.cbawCoarsenRejectedNoGain) +
                    " reject_resource=" +
                    std::to_string(materializePerf.cbawCoarsenRejectedResource) +
                    " reject_cycle=" +
                    std::to_string(materializePerf.cbawCoarsenRejectedCycle) +
                    " stale=" + std::to_string(materializePerf.cbawCoarsenStale) +
                    " clusters_before=" +
                    std::to_string(materializePerf.clustersBeforeCoarsen) +
                    " clusters_after=" +
                    std::to_string(materializePerf.clustersAfterCoarsen) +
                    " quotient_cycle=" +
                    std::to_string(cbawStats.quotientDagCycleDetected) +
                    " generated_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenGeneratedByKind) +
                    " dedup_selected_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenDedupSelectedByKind) +
                    " dedup_lost_tag_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenDedupLostTagByKind) +
                    " selected_reason=" +
                    formatCounts(materializePerf.cbawCoarsenSelectedReason) +
                    " evaluated_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenEvaluatedByKind) +
                    " accepted_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenAcceptedByKind) +
                    " accepted_by_tag=" +
                    formatCounts(materializePerf.cbawCoarsenAcceptedByTag) +
                    " reject_no_gain_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenRejectedNoGainByKind) +
                    " reject_resource_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenRejectedResourceByKind) +
                    " reject_cycle_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenRejectedCycleByKind) +
                    " stale_by_kind=" +
                    formatCounts(materializePerf.cbawCoarsenStaleByKind) +
                    " reject_no_gain_by_tag=" +
                    formatCounts(materializePerf.cbawCoarsenRejectedNoGainByTag) +
                    " reject_resource_by_tag=" +
                    formatCounts(materializePerf.cbawCoarsenRejectedResourceByTag) +
                    " reject_cycle_by_tag=" +
                    formatCounts(materializePerf.cbawCoarsenRejectedCycleByTag) +
                    " stale_by_tag=" +
                    formatCounts(materializePerf.cbawCoarsenStaleByTag));
            logInfo("activity-schedule cbaw p6 guard-sink: guard_candidates=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenGeneratedByKind,
                                                "guard_hint")) +
                    " guard_accepted=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenAcceptedByTag,
                                                "guard_hint")) +
                    " guard_reject_no_gain=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenRejectedNoGainByTag,
                                                "guard_hint")) +
                    " guard_reject_resource=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenRejectedResourceByTag,
                                                "guard_hint")) +
                    " guard_reject_cycle=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenRejectedCycleByTag,
                                                "guard_hint")) +
                    " guard_stale=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenStaleByTag,
                                                "guard_hint")) +
                    " sink_candidates=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenGeneratedByKind,
                                                "sink_cone")) +
                    " sink_accepted=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenAcceptedByTag,
                                                "sink_cone")) +
                    " sink_reject_no_gain=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenRejectedNoGainByTag,
                                                "sink_cone")) +
                    " sink_reject_resource=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenRejectedResourceByTag,
                                                "sink_cone")) +
                    " sink_reject_cycle=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenRejectedCycleByTag,
                                                "sink_cone")) +
                    " sink_stale=" +
                    std::to_string(cbawKindCount(materializePerf.cbawCoarsenStaleByTag,
                                                "sink_cone")) +
                    " semantic_guard_domains=" +
                    std::to_string(cbawStats.semanticGuardDomains) +
                    " semantic_sink_labels=" +
                    std::to_string(cbawStats.semanticSinkConeLabels));
            logInfo("activity-schedule cbaw p7 refine: enabled=1 mode=cbaw_boundary_fm fm_moves=" +
                    std::to_string(materializePerf.fmRefineMoves) +
                    " fm_rounds=" + std::to_string(materializePerf.fmRefineRounds) +
                    " fm_gain=" + std::to_string(materializePerf.fmRefineTotalGain) +
                    " fm_candidates=" + std::to_string(materializePerf.fmRefineCandidates) +
                    " fm_reject_size=" + std::to_string(materializePerf.fmRefineRejectedSize) +
                    " fm_reject_cycle=" + std::to_string(materializePerf.fmRefineRejectedCycle) +
                    " fm_reject_size_fill=" +
                    formatCounts(materializePerf.fmRefineRejectedSizeFillBucket) +
                    " fm_reject_cycle_relation=" +
                    formatCounts(materializePerf.fmRefineRejectedCycleRelation) +
                    " local_exact_rois=0");
            if (cbawPlainBaselineStats)
            {
                const auto signedDiff = [](std::size_t lhs, std::size_t rhs)
                {
                    return static_cast<std::int64_t>(lhs) -
                           static_cast<std::int64_t>(rhs);
                };
                const auto absI64 = [](std::int64_t value)
                {
                    return value < 0 ? -value : value;
                };
                logInfo("activity-schedule cbaw p7 fm_required_estimate: after_dp_boundary_gap=" +
                        std::to_string(signedDiff(materializePerf.cbawAfterDp.boundaryActivationEdges,
                                                  cbawPlainBaselineStats->boundaryActivationEdges)) +
                        " p7_actual_boundary_gain=" +
                        std::to_string(signedDiff(materializePerf.cbawAfterDp.boundaryActivationEdges,
                                                  materializePerf.cbawAfterFm.boundaryActivationEdges)) +
                        " final_margin=" +
                        std::to_string(signedDiff(cbawPlainBaselineStats->boundaryActivationEdges,
                                                  materializePerf.cbawAfterFm.boundaryActivationEdges)) +
                        " final_margin_ppm_of_p7_gain=" +
                        std::to_string(
                            materializePerf.cbawAfterDp.boundaryActivationEdges >
                                    materializePerf.cbawAfterFm.boundaryActivationEdges
                                ? (absI64(signedDiff(cbawPlainBaselineStats->boundaryActivationEdges,
                                                     materializePerf.cbawAfterFm.boundaryActivationEdges)) *
                                   1000000ll) /
                                      std::max<std::int64_t>(
                                          1,
                                          signedDiff(materializePerf.cbawAfterDp.boundaryActivationEdges,
                                                     materializePerf.cbawAfterFm.boundaryActivationEdges))
                                : 0));
            }
            logInfo("activity-schedule cbaw p8 gate: runtime_allowed=" +
                    std::to_string(cbawGate.runtimeAllowed ? 1 : 0) +
                    " reason=" + cbawGate.reason +
                    " structural_pass=" +
                    std::to_string(cbawGate.structuralPass ? 1 : 0) +
                    " trigger_pass=" +
                    std::to_string(cbawGate.triggerPass ? 1 : 0) +
                    " resource_pass=" +
                    std::to_string(cbawGate.resourcePass ? 1 : 0) +
                    " dag_pass=" + std::to_string(cbawGate.dagPass ? 1 : 0) +
                    " plain_boundary=" +
                    std::to_string(cbawGate.plainCrossBoundaryTargets) +
                    " cbaw_boundary=" +
                    std::to_string(cbawGate.cbawCrossBoundaryTargets) +
                    " plain_dag=" + std::to_string(cbawGate.plainDagEdges) +
                    " cbaw_dag=" + std::to_string(cbawGate.cbawDagEdges) +
                    " plain_compute_compute=" +
                    std::to_string(cbawGate.plainComputeMaterializedTargets) +
                    " cbaw_compute_compute=" +
                    std::to_string(cbawGate.cbawComputeMaterializedTargets) +
                    " cbaw_trigger_p99=" +
                    std::to_string(cbawGate.cbawTriggerEstimatedP99));
        }

        const std::size_t computeSupernodes =
            std::count(build.supernodeKinds.begin(),
                       build.supernodeKinds.end(),
                       ActivityScheduleSupernodeKind::Compute);
        const std::size_t commitSupernodes =
            std::count(build.supernodeKinds.begin(),
                       build.supernodeKinds.end(),
                       ActivityScheduleSupernodeKind::Commit);

        logInfo("activity-schedule timing(ms): build_op_data=" + std::to_string(buildOpDataMs) +
                " compute_node_build=" + std::to_string(computeNodeMs) +
                " freeze_after_compute_node=" + std::to_string(freezeMs) +
                " final_materialize=" + std::to_string(materializeMs) +
                " export_session=" + std::to_string(exportMs) +
                " total=" + std::to_string(elapsedMs(totalStart)));
        logInfo("activity-schedule compute-node materialize timing(ms): init_clusters=" +
                std::to_string(materializePerf.initClustersMs) +
                " topo_before_coarsen=" + std::to_string(materializePerf.topoBeforeCoarsenMs) +
                " coarsen=" + std::to_string(materializePerf.coarsenMs) +
                " topo_after_coarsen=" + std::to_string(materializePerf.topoAfterCoarsenMs) +
                " build_cluster_view=" + std::to_string(materializePerf.buildClusterViewMs) +
                " dp_segment=" + std::to_string(materializePerf.dpSegmentMs) +
                " fm_refine=" + std::to_string(materializePerf.fmRefineMs) +
                " flatten_segments=" + std::to_string(materializePerf.flattenSegmentsMs) +
                " build_final_supernodes=" + std::to_string(materializePerf.buildFinalSupernodesMs) +
                " build_final_dag=" + std::to_string(materializePerf.buildFinalDagMs) +
                " build_state_read_sets=" + std::to_string(materializePerf.buildStateReadSetsMs) +
                " final_topo=" + std::to_string(materializePerf.finalTopoMs));
        logInfo("activity-schedule compute-node final split detail: oversize_compute_nodes=" +
                std::to_string(materializePerf.splitOversizeComputeNodes) +
                " split_supernodes=" +
                std::to_string(materializePerf.splitOversizeComputeNodeSupernodes));
        const auto formatKindCounts =
            [](const ComputeNodeMaterializePerfStats::KindCountMap &counts)
        {
            std::ostringstream oss;
            bool first = true;
            for (const auto &[kind, count] : counts)
            {
                if (!first)
                {
                    oss << ",";
                }
                first = false;
                oss << kind << ":" << count;
            }
            return first ? std::string("-") : oss.str();
        };
        logInfo("activity-schedule compute-node coarsen detail: enabled=" +
                std::string(options_.enableCoarsen ? "true" : "false") +
                " chain_merge=" + std::string(options_.enableChainMerge ? "true" : "false") +
                " iterations=" + std::to_string(materializePerf.coarsenIterations) +
                " out1_merges=" + std::to_string(materializePerf.coarsenOut1Merges) +
                " in1_merges=" + std::to_string(materializePerf.coarsenIn1Merges) +
                " sibling_merges=" + std::to_string(materializePerf.coarsenSiblingMerges) +
                " prob_candidates=" + std::to_string(materializePerf.probCoarsenCandidates) +
                " prob_merges=" + std::to_string(materializePerf.probCoarsenMerges) +
                " prob_gain=" + std::to_string(materializePerf.probCoarsenTotalGain) +
                " prob_reject_size=" + std::to_string(materializePerf.probCoarsenRejectedSize) +
                " prob_reject_footprint=" + std::to_string(materializePerf.probCoarsenRejectedFootprint) +
                " prob_reject_phi=" + std::to_string(materializePerf.probCoarsenRejectedPhi) +
                " prob_reject_weight=" + std::to_string(materializePerf.probCoarsenRejectedWeight) +
                " prob_reject_cycle=" + std::to_string(materializePerf.probCoarsenRejectedCycle) +
                " prob_seed_aggregates=" + std::to_string(materializePerf.probCoarsenSeedAggregates) +
                " prob_full_aggregates=" + std::to_string(materializePerf.probCoarsenFullAggregates) +
                " prob_aggregate_ms=" + std::to_string(materializePerf.probCoarsenAggregateMs) +
                " cbaw_candidates=" + std::to_string(materializePerf.cbawCoarsenCandidates) +
                " cbaw_evaluated=" + std::to_string(materializePerf.cbawCoarsenEvaluated) +
                " cbaw_merges=" + std::to_string(materializePerf.cbawCoarsenMerges) +
                " cbaw_reject_no_gain=" +
                std::to_string(materializePerf.cbawCoarsenRejectedNoGain) +
                " cbaw_reject_resource=" +
                std::to_string(materializePerf.cbawCoarsenRejectedResource) +
                " cbaw_reject_cycle=" + std::to_string(materializePerf.cbawCoarsenRejectedCycle) +
                " cbaw_stale=" + std::to_string(materializePerf.cbawCoarsenStale) +
                " cbaw_eval_ms=" + std::to_string(materializePerf.cbawCoarsenEvaluateMs) +
                " cbaw_topo_ms=" + std::to_string(materializePerf.cbawCoarsenTopoMs) +
                " cbaw_generated_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenGeneratedByKind) +
                " cbaw_dedup_selected_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenDedupSelectedByKind) +
                " cbaw_dedup_lost_tag_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenDedupLostTagByKind) +
                " cbaw_selected_reason=" +
                formatKindCounts(materializePerf.cbawCoarsenSelectedReason) +
                " cbaw_evaluated_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenEvaluatedByKind) +
                " cbaw_accepted_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenAcceptedByKind) +
                " cbaw_accepted_by_tag=" +
                formatKindCounts(materializePerf.cbawCoarsenAcceptedByTag) +
                " cbaw_reject_no_gain_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenRejectedNoGainByKind) +
                " cbaw_reject_resource_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenRejectedResourceByKind) +
                " cbaw_reject_cycle_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenRejectedCycleByKind) +
                " cbaw_stale_by_kind=" +
                formatKindCounts(materializePerf.cbawCoarsenStaleByKind) +
                " cbaw_reject_no_gain_by_tag=" +
                formatKindCounts(materializePerf.cbawCoarsenRejectedNoGainByTag) +
                " cbaw_reject_resource_by_tag=" +
                formatKindCounts(materializePerf.cbawCoarsenRejectedResourceByTag) +
                " cbaw_reject_cycle_by_tag=" +
                formatKindCounts(materializePerf.cbawCoarsenRejectedCycleByTag) +
                " cbaw_stale_by_tag=" +
                formatKindCounts(materializePerf.cbawCoarsenStaleByTag) +
                " fm_rounds=" + std::to_string(materializePerf.fmRefineRounds) +
                " fm_candidates=" + std::to_string(materializePerf.fmRefineCandidates) +
                " fm_moves=" + std::to_string(materializePerf.fmRefineMoves) +
                " fm_gain=" + std::to_string(materializePerf.fmRefineTotalGain) +
                " fm_reject_size=" + std::to_string(materializePerf.fmRefineRejectedSize) +
                " fm_reject_footprint=" + std::to_string(materializePerf.fmRefineRejectedFootprint) +
                " fm_reject_phi=" + std::to_string(materializePerf.fmRefineRejectedPhi) +
                " fm_reject_weight=" + std::to_string(materializePerf.fmRefineRejectedWeight) +
                " fm_reject_cycle=" + std::to_string(materializePerf.fmRefineRejectedCycle) +
                " fm_reject_size_fill=" +
                formatKindCounts(materializePerf.fmRefineRejectedSizeFillBucket) +
                " fm_reject_cycle_relation=" +
                formatKindCounts(materializePerf.fmRefineRejectedCycleRelation) +
                " cbaw_after_p5_boundary=" +
                std::to_string(materializePerf.cbawAfterCoarsen.boundaryActivationEdges) +
                " cbaw_after_p5_dag=" +
                std::to_string(materializePerf.cbawAfterCoarsen.dagEdges) +
                " cbaw_after_p5_compute_compute=" +
                std::to_string(materializePerf.cbawAfterCoarsen.computeComputeValuePairs) +
                " cbaw_after_dp_boundary=" +
                std::to_string(materializePerf.cbawAfterDp.boundaryActivationEdges) +
                " cbaw_after_dp_dag=" +
                std::to_string(materializePerf.cbawAfterDp.dagEdges) +
                " cbaw_after_dp_compute_compute=" +
                std::to_string(materializePerf.cbawAfterDp.computeComputeValuePairs) +
                " cbaw_after_fm_boundary=" +
                std::to_string(materializePerf.cbawAfterFm.boundaryActivationEdges) +
                " cbaw_after_fm_dag=" +
                std::to_string(materializePerf.cbawAfterFm.dagEdges) +
                " cbaw_after_fm_compute_compute=" +
                std::to_string(materializePerf.cbawAfterFm.computeComputeValuePairs) +
                " clusters_before=" + std::to_string(materializePerf.clustersBeforeCoarsen) +
                " clusters_after=" + std::to_string(materializePerf.clustersAfterCoarsen) +
                " tail_stopped=" + std::string(materializePerf.coarsenTailStopped ? "true" : "false") +
                " tail_iterations=" + std::to_string(materializePerf.coarsenTailIterations) +
                " segments=" + std::to_string(materializePerf.segments) +
                " compute_supernodes=" + std::to_string(materializePerf.computeSupernodes));
        for (const auto &iter : materializePerf.coarsenIterationStats)
        {
            logInfo("activity-schedule timing: compute_node_coarsen_iter=" +
                    std::to_string(iter.iteration) +
                    " clusters=" + std::to_string(iter.clusters) +
                    " cluster_delta=" + std::to_string(iter.clusterDelta) +
                    " changed=" + (iter.changed ? std::string("true") : std::string("false")) +
                    " out1=" + (iter.out1Changed ? std::string("1") : std::string("0")) +
                    " in1=" + (iter.in1Changed ? std::string("1") : std::string("0")) +
                    " siblings=" + (iter.siblingsChanged ? std::string("1") : std::string("0")) +
                    " prob=" + (iter.probChanged ? std::string("1") : std::string("0")) +
                    " tail_stop=" + (iter.tailStopped ? std::string("1") : std::string("0")) +
                    " elapsed_ms=" + std::to_string(iter.elapsedMs));
        }
        logInfo("activity-schedule timing detail: compute_nodes=" +
                std::to_string(rewrite.stats.computeNodes) +
                " compute_node_ops_total=" + std::to_string(rewrite.stats.computeNodeOpsTotal) +
                " compute_node_cycle_split_iters=" +
                std::to_string(rewrite.stats.computeNodeCycleSplitIters) +
                " initial_compute_supernodes=" +
                std::to_string(rewrite.stats.initialComputeSupernodes) +
                " initial_compute_supernode_ops_total=" +
                std::to_string(rewrite.stats.initialComputeSupernodeOpsTotal) +
                " initial_compute_supernode_dag_edges=" +
                std::to_string(rewrite.stats.initialComputeSupernodeDagEdges) +
                " initial_boundary_values=" + std::to_string(rewrite.stats.initialBoundaryValues) +
                " initial_boundary_activation_edges=" +
                std::to_string(rewrite.stats.initialBoundaryActivationEdges) +
                " initial_compute_compute_value_pairs=" +
                std::to_string(rewrite.stats.initialComputeComputeValuePairs) +
                " initial_compute_commit_value_pairs=" +
                std::to_string(rewrite.stats.initialComputeCommitValuePairs) +
                " source_clones_in_compute_nodes=" +
                std::to_string(rewrite.stats.sourceClonesInComputeNodes) +
                " local_shared_compute_clones_in_compute_nodes=" +
                std::to_string(rewrite.stats.localSharedComputeClonesInComputeNodes) +
                " direct_source_inputs_to_commit_supernodes=" +
                std::to_string(rewrite.stats.directSourceInputsToCommitSupernodes) +
                " common_expr_compute_nodes=" + std::to_string(rewrite.stats.commonExprComputeNodes) +
                " compute_node_boundary_inputs_total=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputsTotal) +
                " boundary_no_def=" + std::to_string(rewrite.stats.computeNodeBoundaryInputNoDef) +
                " boundary_def_out_of_range=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputDefOutOfRange) +
                " boundary_declared=" + std::to_string(rewrite.stats.computeNodeBoundaryInputDeclared) +
                " boundary_source_spill=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputSourceSpill) +
                " boundary_unsupported=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputUnsupported) +
                " boundary_existing_owner=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputExistingOwner) +
                " boundary_existing_common_owner=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputExistingCommonOwner) +
                " boundary_shared=" + std::to_string(rewrite.stats.computeNodeBoundaryInputShared) +
                " boundary_capacity=" + std::to_string(rewrite.stats.computeNodeBoundaryInputCapacity) +
                " compute_node_boundary_values=" +
                std::to_string(rewrite.stats.computeNodeBoundaryValues) +
                " commit_input_root_values=" + std::to_string(rewrite.stats.commitInputRootValues) +
                " commit_sink_ops=" + std::to_string(rewrite.stats.commitSinkOps) +
                " commit_event_key_runs=" + std::to_string(rewrite.stats.commitEventKeyRuns) +
                " commit_event_keys=" + std::to_string(rewrite.stats.commitEventKeys) +
                " compute_supernodes=" + std::to_string(computeSupernodes) +
                " commit_supernodes=" + std::to_string(commitSupernodes) +
                " topo_edges=" + std::to_string(opData.topoEdges.size()) +
                " graph_ops=" + std::to_string(graph->operations().size()) +
                " graph_values=" + std::to_string(graph->values().size()));
        logInfo("activity-schedule compute-node existing common owner detail: by_kind_top=" +
                formatTopCounts(rewrite.stats.computeNodeBoundaryExistingCommonOwnerByKind, 10) +
                " by_width=" +
                formatTopCounts(rewrite.stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket, 10) +
                " by_fanout=" +
                formatTopCounts(rewrite.stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket, 10));

        std::ostringstream summary;
        summary << "activity-schedule: path=" << options_.path
                << " graph=" << graph->symbol()
                << " supernodes=" << build.supernodeToOps.size()
                << " compute_supernodes=" << computeSupernodes
                << " commit_supernodes=" << commitSupernodes
                << " compute_nodes=" << rewrite.stats.computeNodes
                << " source_clones=" << rewrite.stats.sourceClonesInComputeNodes
                << " local_shared_compute_clones=" << rewrite.stats.localSharedComputeClonesInComputeNodes
                << " eligible_ops=" << opData.topoOps.size()
                << " state_read_sets=" << build.stateReadSupernodes.size()
                << " graph_changed=" << (graphChanged ? "true" : "false");
        logInfo(summary.str());

        result.changed = graphChanged;
        result.failed = false;
        return result;
    }

} // namespace wolvrix::lib::transform
