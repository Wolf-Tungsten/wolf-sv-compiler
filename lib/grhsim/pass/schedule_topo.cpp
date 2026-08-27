#include "grhsim/pass/schedule_topo.hpp"

#include "grhsim/ir/generic.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        struct DfsFrame
        {
            OpId op;
            std::size_t resultIndex = 0;
            std::size_t useIndex = 0;
            std::size_t virtualIndex = 0;
        };

        // Event states are represented by output sinks rather than ordinary
        // def-use edges. Keep those sparse, synthetic dependencies alongside
        // the real graph while computing SCCs and the final order.
        struct VirtualDependencies
        {
            std::unordered_map<uint32_t, std::vector<OpId>> forward;
            std::unordered_map<uint32_t, std::vector<OpId>> reverse;
        };

        VirtualDependencies findVirtualDependencies(const Module &module,
                                                     std::span<const OpId> liveOps)
        {
            VirtualDependencies result;
            std::vector<std::vector<OpId>> writersByState(module.states().size());
            for (OpId op : liveOps)
            {
                if (module.kind(op) != genericOp(GenericOpcode::OutWrite))
                {
                    continue;
                }
                const AttrValue *marker = module.attr(op, "eventState");
                const bool marked = marker && std::get_if<bool>(marker) &&
                                    *std::get_if<bool>(marker);
                if (!marked)
                {
                    continue;
                }
                const AttrValue *portAttr = module.attr(op, "port");
                const SymbolId *portName =
                    portAttr ? std::get_if<SymbolId>(portAttr) : nullptr;
                const StateId state = portName
                                          ? module.findState(module.symbol(*portName))
                                          : StateId::invalid();
                if (state.valid() && state.raw < writersByState.size())
                {
                    writersByState[state.raw].push_back(op);
                }
            }

            const auto add = [&](OpId producer, OpId consumer) {
                if (!producer.valid() || !consumer.valid() || producer == consumer)
                {
                    return;
                }
                result.forward[producer.raw].push_back(consumer);
                result.reverse[consumer.raw].push_back(producer);
            };
            for (OpId consumer : liveOps)
            {
                const AttrValue *eventsAttr = module.attr(consumer, "events");
                const auto *events = eventsAttr
                                         ? std::get_if<std::vector<SymbolId>>(eventsAttr)
                                         : nullptr;
                if (!events)
                {
                    continue;
                }
                for (SymbolId event : *events)
                {
                    const StateId state = module.findState(module.symbol(event));
                    if (!state.valid() || state.raw >= writersByState.size())
                    {
                        continue;
                    }
                    for (OpId producer : writersByState[state.raw])
                    {
                        add(producer, consumer);
                    }
                }
            }
            const auto normalize = [](auto &adjacency) {
                for (auto &[raw, targets] : adjacency)
                {
                    (void)raw;
                    std::sort(targets.begin(), targets.end());
                    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                }
            };
            normalize(result.forward);
            normalize(result.reverse);
            return result;
        }

        std::vector<OpId> scheduleTopologically(const Module &module)
        {
            const auto liveOps = module.ops();
            if (liveOps.empty())
            {
                return {};
            }

            const std::size_t nodeCount =
                static_cast<std::size_t>(liveOps.back().raw) + 1U;
            const VirtualDependencies virtualDependencies =
                findVirtualDependencies(module, liveOps);
            std::vector<uint8_t> visited(nodeCount, 0);
            std::vector<OpId> postorder;
            postorder.reserve(liveOps.size());
            std::vector<DfsFrame> dfs;

            // First Kosaraju walk follows def-use edges. Iterative traversal keeps
            // scheduling viable for the very deep flattened graphs this IR targets.
            for (OpId start : liveOps)
            {
                if (visited[start.raw])
                {
                    continue;
                }
                visited[start.raw] = 1;
                dfs.push_back(DfsFrame{.op = start});
                while (!dfs.empty())
                {
                    DfsFrame &frame = dfs.back();
                    const auto results = module.results(frame.op);
                    bool descended = false;
                    while (frame.resultIndex < results.size())
                    {
                        const auto users = module.users(results[frame.resultIndex]);
                        if (frame.useIndex >= users.size())
                        {
                            ++frame.resultIndex;
                            frame.useIndex = 0;
                            continue;
                        }
                        const OpId next = users[frame.useIndex++].user;
                        if (module.valid(next) && !visited[next.raw])
                        {
                            visited[next.raw] = 1;
                            dfs.push_back(DfsFrame{.op = next});
                            descended = true;
                            break;
                        }
                    }
                    if (!descended)
                    {
                        const auto virtualIt = virtualDependencies.forward.find(frame.op.raw);
                        if (virtualIt != virtualDependencies.forward.end())
                        {
                            while (frame.virtualIndex < virtualIt->second.size())
                            {
                                const OpId next = virtualIt->second[frame.virtualIndex++];
                                if (module.valid(next) && !visited[next.raw])
                                {
                                    visited[next.raw] = 1;
                                    dfs.push_back(DfsFrame{.op = next});
                                    descended = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!descended)
                    {
                        postorder.push_back(frame.op);
                        dfs.pop_back();
                    }
                }
            }

            std::vector<uint32_t> componentOf(nodeCount, OpId::kInvalid);
            std::vector<std::vector<OpId>> components;
            std::vector<OpId> worklist;
            for (auto iterator = postorder.rbegin(); iterator != postorder.rend(); ++iterator)
            {
                const OpId start = *iterator;
                if (componentOf[start.raw] != OpId::kInvalid)
                {
                    continue;
                }
                const uint32_t component = static_cast<uint32_t>(components.size());
                components.emplace_back();
                componentOf[start.raw] = component;
                worklist.push_back(start);
                while (!worklist.empty())
                {
                    const OpId current = worklist.back();
                    worklist.pop_back();
                    components.back().push_back(current);
                    for (EdgeId operand : module.operands(current))
                    {
                        const OpId producer = module.def(operand);
                        if (module.valid(producer) &&
                            componentOf[producer.raw] == OpId::kInvalid)
                        {
                            componentOf[producer.raw] = component;
                            worklist.push_back(producer);
                        }
                    }
                    const auto virtualIt = virtualDependencies.reverse.find(current.raw);
                    if (virtualIt != virtualDependencies.reverse.end())
                    {
                        for (OpId producer : virtualIt->second)
                        {
                            if (module.valid(producer) &&
                                componentOf[producer.raw] == OpId::kInvalid)
                            {
                                componentOf[producer.raw] = component;
                                worklist.push_back(producer);
                            }
                        }
                    }
                }
            }

            std::vector<std::vector<uint32_t>> successors(components.size());
            std::vector<uint32_t> indegree(components.size(), 0);
            std::vector<uint32_t> firstOp(components.size(), OpId::kInvalid);
            for (uint32_t component = 0; component < components.size(); ++component)
            {
                auto &members = components[component];
                std::sort(members.begin(), members.end());
                firstOp[component] = members.front().raw;
            }
            for (OpId producer : liveOps)
            {
                const uint32_t source = componentOf[producer.raw];
                for (EdgeId result : module.results(producer))
                {
                    for (Use use : module.users(result))
                    {
                        const uint32_t destination = componentOf[use.user.raw];
                        if (source != destination)
                        {
                            successors[source].push_back(destination);
                        }
                    }
                }
            }
            for (const auto &[producerRaw, consumers] : virtualDependencies.forward)
            {
                const uint32_t source = componentOf[producerRaw];
                for (OpId consumer : consumers)
                {
                    const uint32_t destination = componentOf[consumer.raw];
                    if (source != destination)
                    {
                        successors[source].push_back(destination);
                    }
                }
            }
            for (auto &targets : successors)
            {
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                for (uint32_t target : targets)
                {
                    ++indegree[target];
                }
            }

            using ReadyEntry = std::pair<uint32_t, uint32_t>;
            std::priority_queue<ReadyEntry, std::vector<ReadyEntry>,
                                std::greater<ReadyEntry>> ready;
            for (uint32_t component = 0; component < components.size(); ++component)
            {
                if (indegree[component] == 0)
                {
                    ready.emplace(firstOp[component], component);
                }
            }

            std::vector<OpId> order;
            order.reserve(liveOps.size());
            while (!ready.empty())
            {
                const uint32_t component = ready.top().second;
                ready.pop();
                order.insert(order.end(), components[component].begin(),
                             components[component].end());
                for (uint32_t target : successors[component])
                {
                    if (--indegree[target] == 0)
                    {
                        ready.emplace(firstOp[target], target);
                    }
                }
            }
            return order;
        }

        bool alreadyHasMinimalSchedule(const Module &module,
                                       std::span<const OpId> order)
        {
            const auto regions = module.regions();
            if (regions.size() != 1)
            {
                return false;
            }
            const RegionRec *region = module.region(regions.front());
            if (!region || region->activation.kind != ActivationKind::Always ||
                region->activation.state.valid() || !module.regionDeps(regions.front()).empty())
            {
                return false;
            }
            const auto current = module.regionOps(regions.front());
            return current.size() == order.size() &&
                   std::equal(current.begin(), current.end(), order.begin());
        }

        class ScheduleTopoPass final : public SimPass
        {
        public:
            ScheduleTopoPass()
                : SimPass("schedule-topo", "schedule-topo",
                          "Build one always-active region in deterministic topological order",
                          SimPassEffects{.mutatesGraph = true, .preservesSchedule = true})
            {
            }

            SimPassResult run(Module &module, SimPassContext &context) override
            {
                const std::vector<OpId> order = scheduleTopologically(module);
                if (order.size() != module.opCount())
                {
                    context.diagnostics.error(id(),
                                              "failed to topologically order the SCC graph",
                                              "module=" + module.name());
                    return SimPassResult{.failed = true};
                }
                if (alreadyHasMinimalSchedule(module, order))
                {
                    return {};
                }

                module.clearSchedule();
                const RegionId region = module.createRegion();
                if (!region.valid() || !module.setRegion(order, region) ||
                    !module.setRegionOrder(region, order))
                {
                    context.diagnostics.error(id(), "failed to materialize Schedule",
                                              "module=" + module.name());
                    return SimPassResult{.failed = true};
                }
                return SimPassResult{.changed = true};
            }
        };
    } // namespace

    std::unique_ptr<SimPass> makeScheduleTopoPass()
    {
        return std::make_unique<ScheduleTopoPass>();
    }

} // namespace wolvrix::lib::grhsim
