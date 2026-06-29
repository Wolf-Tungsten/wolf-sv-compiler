#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[activity-schedule-tests] " << message << '\n';
        return 1;
    }

    template <typename T>
    const T *getSessionValue(const SessionStore &session, std::string_view key)
    {
        auto it = session.find(std::string(key));
        if (it == session.end())
        {
            return nullptr;
        }
        auto *typed = dynamic_cast<const SessionSlotValue<T> *>(it->second.get());
        return typed == nullptr ? nullptr : &typed->value;
    }

    wolvrix::lib::grh::ValueId makeValue(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width,
                                         bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(name), width, isSigned);
    }

    wolvrix::lib::grh::ValueId makeConstant(wolvrix::lib::grh::Graph &graph,
                                            const std::string &opName,
                                            const std::string &valueName,
                                            int32_t width,
                                            std::string literal)
    {
        const auto value = makeValue(graph, valueName, width);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.internSymbol(opName));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    bool isCommitPhaseOp(const wolvrix::lib::grh::Operation &op)
    {
        using wolvrix::lib::grh::OperationKind;
        switch (op.kind())
        {
        case OperationKind::kRegisterWritePort:
        case OperationKind::kLatchWritePort:
        case OperationKind::kMemoryWritePort:
        case OperationKind::kMemoryFillPort:
            return true;
        default:
            return false;
        }
    }

    struct ScheduleView
    {
        const ActivityScheduleSupernodeToOps *supernodeToOps = nullptr;
        const ActivityScheduleOpToSupernode *opToSupernode = nullptr;
        const ActivityScheduleDag *dag = nullptr;
        const ActivityScheduleValueFanout *valueFanout = nullptr;
        const ActivityScheduleTopoOrder *topoOrder = nullptr;
        const ActivityScheduleStateReadSupernodes *stateReadSupernodes = nullptr;
        const ActivityScheduleSupernodeKinds *supernodeKinds = nullptr;
        const ActivityScheduleComputeNodesBySupernode *computeNodesBySupernode = nullptr;
        const std::string *summaryStats = nullptr;
    };

    ScheduleView loadSchedule(const SessionStore &session, const std::string &graphName)
    {
        const std::string prefix = graphName + ".activity_schedule.";
        return ScheduleView{
            getSessionValue<ActivityScheduleSupernodeToOps>(session, prefix + "supernode_to_ops"),
            getSessionValue<ActivityScheduleOpToSupernode>(session, prefix + "op_to_supernode"),
            getSessionValue<ActivityScheduleDag>(session, prefix + "dag"),
            getSessionValue<ActivityScheduleValueFanout>(session, prefix + "value_fanout"),
            getSessionValue<ActivityScheduleTopoOrder>(session, prefix + "topo_order"),
            getSessionValue<ActivityScheduleStateReadSupernodes>(session, prefix + "state_read_supernodes"),
            getSessionValue<ActivityScheduleSupernodeKinds>(session, prefix + "supernode_kind"),
            getSessionValue<ActivityScheduleComputeNodesBySupernode>(session, prefix + "compute_nodes_by_supernode"),
            getSessionValue<std::string>(session, prefix + "summary_stats"),
        };
    }

    bool hasFanoutTo(const ActivityScheduleValueFanout &fanout,
                     wolvrix::lib::grh::ValueId value,
                     uint32_t supernode)
    {
        if (value.index == 0 || value.index > fanout.size())
        {
            return false;
        }
        const auto &succs = fanout[value.index - 1];
        return std::find(succs.begin(), succs.end(), supernode) != succs.end();
    }

    bool supernodeContains(const ActivityScheduleSupernodeToOps &supernodeToOps,
                           uint32_t supernode,
                           wolvrix::lib::grh::OperationId opId)
    {
        if (supernode == kInvalidActivitySupernodeId || supernode >= supernodeToOps.size())
        {
            return false;
        }
        const auto &ops = supernodeToOps[supernode];
        return std::find(ops.begin(), ops.end(), opId) != ops.end();
    }

    bool stateReadHasSupernode(const ActivityScheduleStateReadSupernodes &stateReadSupernodes,
                               const std::string &stateSymbol,
                               uint32_t supernode)
    {
        const auto it = stateReadSupernodes.find(stateSymbol);
        if (it == stateReadSupernodes.end())
        {
            return false;
        }
        const auto &supernodes = it->second;
        return std::find(supernodes.begin(), supernodes.end(), supernode) != supernodes.end();
    }

    std::size_t parseStatField(const std::string &stats, const std::string &name)
    {
        const std::string needle = name + "=";
        const std::size_t pos = stats.find(needle);
        if (pos == std::string::npos)
        {
            return 0;
        }
        std::size_t end = pos + needle.size();
        while (end < stats.size() && stats[end] >= '0' && stats[end] <= '9')
        {
            ++end;
        }
        return static_cast<std::size_t>(std::stoull(stats.substr(pos + needle.size(), end - pos - needle.size())));
    }

    double parseJsonDoubleField(const std::string &json, const std::string &name)
    {
        const std::string needle = "\"" + name + "\":";
        const std::size_t pos = json.find(needle);
        if (pos == std::string::npos)
        {
            return -1.0;
        }
        std::size_t end = pos + needle.size();
        while (end < json.size() &&
               (json[end] == '-' || json[end] == '+' || json[end] == '.' ||
                json[end] == 'e' || json[end] == 'E' ||
                (json[end] >= '0' && json[end] <= '9')))
        {
            ++end;
        }
        return std::stod(json.substr(pos + needle.size(), end - pos - needle.size()));
    }

    void setIntentShape(wolvrix::lib::grh::Graph &graph,
                        wolvrix::lib::grh::OperationId opId,
                        const std::string &group,
                        const std::string &role,
                        int64_t elementWidth,
                        int64_t elementCount)
    {
        graph.setAttr(opId, "regToMem.intent.group", group);
        graph.setAttr(opId, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(opId, "regToMem.intent.role", role);
        graph.setAttr(opId, "regToMem.intent.elementWidth", elementWidth);
        graph.setAttr(opId, "regToMem.intent.elementCount", elementCount);
    }

    int validateCommonScheduleShape(const wolvrix::lib::grh::Graph &graph,
                                    const ScheduleView &schedule)
    {
        if (schedule.supernodeToOps == nullptr || schedule.opToSupernode == nullptr ||
            schedule.dag == nullptr || schedule.valueFanout == nullptr ||
            schedule.topoOrder == nullptr || schedule.stateReadSupernodes == nullptr ||
            schedule.supernodeKinds == nullptr || schedule.computeNodesBySupernode == nullptr ||
            schedule.summaryStats == nullptr)
        {
            return fail("Expected all activity-schedule session outputs to exist");
        }
        if (schedule.supernodeToOps->size() != schedule.supernodeKinds->size() ||
            schedule.supernodeToOps->size() != schedule.topoOrder->size())
        {
            return fail("Expected supernode outputs to have matching sizes");
        }
        for (uint32_t supernodeId = 0; supernodeId < schedule.supernodeToOps->size(); ++supernodeId)
        {
            const bool commitKind =
                (*schedule.supernodeKinds)[supernodeId] == ActivityScheduleSupernodeKind::Commit;
            for (const auto opId : (*schedule.supernodeToOps)[supernodeId])
            {
                const bool commitOp = isCommitPhaseOp(graph.getOperation(opId));
                if (commitKind != commitOp)
                {
                    return fail("Expected explicit supernode_kind to match contained ops");
                }
            }
        }
        return 0;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

} // namespace

int main()
{
    std::string currentCase;
    try
    {
    {
        currentCase = "reg-to-mem intent group";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("intent_top");
        design.markAsTop("intent_top");

        const auto idxReg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                  graph.internSymbol("idx_reg"));
        graph.setAttr(idxReg, "width", int64_t{2});
        graph.setAttr(idxReg, "isSigned", false);

        const auto idx = makeValue(graph, "idx", 2);
        const auto idxRead = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                   graph.internSymbol("idx_read_op"));
        graph.addResult(idxRead, idx);
        graph.setAttr(idxRead, "regSymbol", std::string("idx_reg"));

        std::vector<wolvrix::lib::grh::OperationId> reads;
        std::vector<wolvrix::lib::grh::ValueId> readValues;
        reads.reserve(4);
        readValues.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            const auto regOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                     graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            graph.setAttr(regOp, "regToMem.intent.group", std::string("rtm_intent_test"));
            graph.setAttr(regOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(regOp, "regToMem.intent.role", std::string("register"));
            graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));
            graph.setAttr(regOp, "regToMem.intent.elementWidth", int64_t{8});
            graph.setAttr(regOp, "regToMem.intent.elementCount", int64_t{4});

            const auto readValue = makeValue(graph, reg + "_read", 8);
            const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                      graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, readValue);
            graph.setAttr(readOp, "regSymbol", reg);
            graph.setAttr(readOp, "regToMem.intent.group", std::string("rtm_intent_test"));
            graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
            graph.setAttr(readOp, "regToMem.intent.row", static_cast<int64_t>(row));
            reads.push_back(readOp);
            readValues.push_back(readValue);
        }

        const auto packed = makeValue(graph, "packed", 32);
        const auto concat = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        graph.setAttr(concat, "regToMem.intent.group", std::string("rtm_intent_test"));
        graph.setAttr(concat, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(concat, "regToMem.intent.role", std::string("concat"));
        graph.setAttr(concat, "regToMem.intent.elementWidth", int64_t{8});
        graph.setAttr(concat, "regToMem.intent.elementCount", int64_t{4});
        graph.setAttr(concat, "regToMem.intent.regSymbols",
                      std::vector<std::string>{"r0", "r1", "r2", "r3"});
        graph.setAttr(concat, "regToMem.intent.operandRows",
                      std::vector<int64_t>{3, 2, 1, 0});

        const auto selected = makeValue(graph, "selected", 8);
        const auto slice = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceArray,
                                                 graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, idx);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        graph.setAttr(slice, "regToMem.intent.group", std::string("rtm_intent_test"));
        graph.setAttr(slice, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(slice, "regToMem.intent.role", std::string("slice"));
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-array"));
        graph.setAttr(slice, "regToMem.intent.elementWidth", int64_t{8});
        graph.setAttr(slice, "regToMem.intent.elementCount", int64_t{4});
        graph.bindOutputPort("selected", selected);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "intent_top",
                                    .maxOpInComputeSupernode = 64,
                                    .maxOpInComputeNode = 2,
                                    .enableCoarsen = false}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed for reg-to-mem intent group");
        }
        const auto schedule = loadSchedule(session, "intent_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if (schedule.opToSupernode == nullptr || schedule.supernodeToOps == nullptr)
        {
            return fail("Missing activity-schedule outputs for reg-to-mem intent group");
        }
        if (slice.index == 0 || slice.index > schedule.opToSupernode->size())
        {
            return fail("slice op missing from op-to-supernode map");
        }
        const uint32_t owner = (*schedule.opToSupernode)[slice.index - 1];
        if (owner == kInvalidActivitySupernodeId || owner >= schedule.supernodeToOps->size())
        {
            return fail("slice op has invalid supernode owner");
        }
        const auto &ownerOps = (*schedule.supernodeToOps)[owner];
        if (std::find(ownerOps.begin(), ownerOps.end(), concat) == ownerOps.end())
        {
            return fail("reg-to-mem intent concat was split from slice");
        }
        const auto scheduledConcatOperands = graph.opOperands(concat);
        for (const auto readValue : scheduledConcatOperands)
        {
            const auto read = graph.valueDef(readValue);
            if (std::find(ownerOps.begin(), ownerOps.end(), read) == ownerOps.end())
            {
                return fail("reg-to-mem intent read was split from slice");
            }
        }
        const auto scheduledSliceOperands = graph.opOperands(slice);
        if (scheduledSliceOperands.size() != 2)
        {
            return fail("reg-to-mem intent slice operands were rewritten unexpectedly");
        }
        const auto scheduledIndex = scheduledSliceOperands[1];
        const auto scheduledIndexRead = graph.valueDef(scheduledIndex);
        if (!scheduledIndexRead.valid() ||
            graph.opKind(scheduledIndexRead) != wolvrix::lib::grh::OperationKind::kRegisterReadPort)
        {
            return fail("reg-to-mem intent index should still be defined by a register read");
        }
        if (scheduledIndexRead.index == 0 || scheduledIndexRead.index > schedule.opToSupernode->size())
        {
            return fail("reg-to-mem intent index read missing from op-to-supernode map");
        }
        const uint32_t indexOwner = (*schedule.opToSupernode)[scheduledIndexRead.index - 1];
        if (indexOwner == kInvalidActivitySupernodeId || indexOwner >= schedule.supernodeToOps->size())
        {
            return fail("reg-to-mem intent index read has invalid supernode owner");
        }
        if (indexOwner == owner)
        {
            const auto &mergedOps = (*schedule.supernodeToOps)[owner];
            if (std::find(mergedOps.begin(), mergedOps.end(), scheduledIndexRead) == mergedOps.end())
            {
                return fail("reg-to-mem intent index read owner does not contain the read op");
            }
        }
        else if (!hasFanoutTo(*schedule.valueFanout, scheduledIndex, owner))
        {
            return fail("reg-to-mem intent index boundary value is not scheduled into the intent group");
        }
        for (int row = 0; row < 4; ++row)
        {
            if (!stateReadHasSupernode(*schedule.stateReadSupernodes, "r" + std::to_string(row), owner))
            {
                return fail("reg-to-mem intent slice missing storage-register activation mapping");
            }
        }
    }

    {
        currentCase = "reg-to-mem dynamic intent index";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("intent_dynamic");
        design.markAsTop("intent_dynamic");

        const std::string group = "rtm_intent_dyn";
        const auto idx = makeValue(graph, "idx", 2);
        const auto one = makeConstant(graph, "one_const", "one", 2, "2'd1");
        graph.bindInputPort("idx", idx);

        const auto idxPlus = makeValue(graph, "idx_plus", 2);
        const auto idxAdd = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                  graph.internSymbol("idx_plus_add"));
        graph.addOperand(idxAdd, idx);
        graph.addOperand(idxAdd, one);
        graph.addResult(idxAdd, idxPlus);

        std::vector<wolvrix::lib::grh::ValueId> readValues;
        readValues.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            const auto regOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                     graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            setIntentShape(graph, regOp, group, "register", 8, 4);
            graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));

            const auto readValue = makeValue(graph, reg + "_read", 8);
            const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                      graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, readValue);
            graph.setAttr(readOp, "regSymbol", reg);
            graph.setAttr(readOp, "regToMem.intent.group", group);
            graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
            graph.setAttr(readOp, "regToMem.intent.row", static_cast<int64_t>(row));
            readValues.push_back(readValue);
        }

        const auto packed = makeValue(graph, "packed", 32);
        const auto concat = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        setIntentShape(graph, concat, group, "concat", 8, 4);
        graph.setAttr(concat, "regToMem.intent.regSymbols",
                      std::vector<std::string>{"r0", "r1", "r2", "r3"});
        graph.setAttr(concat, "regToMem.intent.operandRows",
                      std::vector<int64_t>{3, 2, 1, 0});

        const auto elemWidth = makeConstant(graph, "elem_width_const", "elem_width", 4, "4'd8");
        const auto start = makeValue(graph, "start", 4);
        const auto mul = graph.createOperation(wolvrix::lib::grh::OperationKind::kMul,
                                               graph.internSymbol("start_mul"));
        graph.addOperand(mul, idxPlus);
        graph.addOperand(mul, elemWidth);
        graph.addResult(mul, start);

        const auto selected = makeValue(graph, "selected", 8);
        const auto slice = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                                 graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        setIntentShape(graph, slice, group, "slice", 8, 4);
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-dynamic"));
        graph.bindOutputPort("selected", selected);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "intent_dynamic",
                                    .maxOpInComputeSupernode = 6,
                                    .maxOpInComputeNode = 2,
                                    .enableCoarsen = false}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed for dynamic reg-to-mem intent group");
        }
        const auto schedule = loadSchedule(session, "intent_dynamic");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t sliceOwner = (*schedule.opToSupernode)[slice.index - 1];
        const uint32_t addOwner = (*schedule.opToSupernode)[idxAdd.index - 1];
        const uint32_t mulOwner = (*schedule.opToSupernode)[mul.index - 1];
        if (sliceOwner == kInvalidActivitySupernodeId || addOwner == kInvalidActivitySupernodeId)
        {
            return fail("dynamic intent slice or canonical index producer missing from schedule");
        }
        if (!supernodeContains(*schedule.supernodeToOps, sliceOwner, concat))
        {
            return fail("dynamic reg-to-mem intent concat was split from slice");
        }
        if (supernodeContains(*schedule.supernodeToOps, sliceOwner, idxAdd))
        {
            return fail("dynamic reg-to-mem intent should treat canonical index producer as boundary");
        }
        if (mulOwner != kInvalidActivitySupernodeId)
        {
            return fail("dynamic reg-to-mem intent should not schedule start-mul as the semantic index dependency");
        }
        if (!hasFanoutTo(*schedule.valueFanout, idxPlus, sliceOwner))
        {
            return fail("dynamic reg-to-mem intent missing canonical index fanout into intent group");
        }
        for (int row = 0; row < 4; ++row)
        {
            if (!stateReadHasSupernode(*schedule.stateReadSupernodes, "r" + std::to_string(row), sliceOwner))
            {
                return fail("dynamic reg-to-mem intent slice missing storage-register activation mapping");
            }
        }
    }

    {
        currentCase = "reg-to-mem dynamic input intent index";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("intent_dynamic_input");
        design.markAsTop("intent_dynamic_input");

        const std::string group = "rtm_intent_dyn_input";
        const auto idx = makeValue(graph, "idx", 2);
        graph.bindInputPort("idx", idx);

        std::vector<wolvrix::lib::grh::ValueId> readValues;
        readValues.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            const auto regOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                     graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            setIntentShape(graph, regOp, group, "register", 8, 4);
            graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));

            const auto readValue = makeValue(graph, reg + "_read", 8);
            const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                      graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, readValue);
            graph.setAttr(readOp, "regSymbol", reg);
            graph.setAttr(readOp, "regToMem.intent.group", group);
            graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
            graph.setAttr(readOp, "regToMem.intent.row", static_cast<int64_t>(row));
            readValues.push_back(readValue);
        }

        const auto packed = makeValue(graph, "packed", 32);
        const auto concat = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        setIntentShape(graph, concat, group, "concat", 8, 4);
        graph.setAttr(concat, "regToMem.intent.regSymbols",
                      std::vector<std::string>{"r0", "r1", "r2", "r3"});
        graph.setAttr(concat, "regToMem.intent.operandRows",
                      std::vector<int64_t>{3, 2, 1, 0});

        const auto elemWidth = makeConstant(graph, "elem_width_const", "elem_width", 4, "4'd8");
        const auto start = makeValue(graph, "start", 5);
        const auto mul = graph.createOperation(wolvrix::lib::grh::OperationKind::kMul,
                                               graph.internSymbol("start_mul"));
        graph.addOperand(mul, idx);
        graph.addOperand(mul, elemWidth);
        graph.addResult(mul, start);

        const auto selected = makeValue(graph, "selected", 8);
        const auto slice = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                                 graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        setIntentShape(graph, slice, group, "slice", 8, 4);
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-dynamic"));
        graph.bindOutputPort("selected", selected);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "intent_dynamic_input",
                                    .maxOpInComputeSupernode = 6,
                                    .maxOpInComputeNode = 2,
                                    .enableCoarsen = false}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed for dynamic input reg-to-mem intent group");
        }
        const auto schedule = loadSchedule(session, "intent_dynamic_input");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t sliceOwner = (*schedule.opToSupernode)[slice.index - 1];
        const uint32_t mulOwner = (*schedule.opToSupernode)[mul.index - 1];
        if (sliceOwner == kInvalidActivitySupernodeId)
        {
            return fail("dynamic input reg-to-mem intent slice missing from schedule");
        }
        if (!supernodeContains(*schedule.supernodeToOps, sliceOwner, concat))
        {
            return fail("dynamic input reg-to-mem intent concat was split from slice");
        }
        if (mulOwner != kInvalidActivitySupernodeId)
        {
            return fail("dynamic input reg-to-mem intent should not schedule start-mul as dependency");
        }
        if (!hasFanoutTo(*schedule.valueFanout, idx, sliceOwner))
        {
            return fail("dynamic input reg-to-mem intent missing input index fanout into intent group");
        }
        for (int row = 0; row < 4; ++row)
        {
            if (!stateReadHasSupernode(*schedule.stateReadSupernodes, "r" + std::to_string(row), sliceOwner))
            {
                return fail("dynamic input reg-to-mem intent slice missing storage-register activation mapping");
            }
        }
    }

    {
        currentCase = "top";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("top");
        design.markAsTop("top");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("a", a);

        const auto regDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol("q"));
        graph.setAttr(regDecl, "width", static_cast<int64_t>(8));
        graph.setAttr(regDecl, "isSigned", false);

        const auto qReadValue = makeValue(graph, "q_read", 8);
        const auto qReadOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                   graph.internSymbol("q_read_op"));
        graph.addResult(qReadOp, qReadValue);
        graph.setAttr(qReadOp, "regSymbol", std::string("q"));

        const auto maskValue = makeValue(graph, "mask_all", 8);
        const auto maskOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                  graph.internSymbol("mask_const"));
        graph.addResult(maskOp, maskValue);
        graph.setAttr(maskOp, "constValue", std::string("8'hFF"));

        const auto sumValue = makeValue(graph, "sum", 8);
        const auto addOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                 graph.internSymbol("sum_add"));
        graph.addOperand(addOp, qReadValue);
        graph.addOperand(addOp, a);
        graph.addResult(addOp, sumValue);
        graph.bindOutputPort("y", sumValue);

        const auto writeOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                   graph.internSymbol("q_write"));
        graph.addOperand(writeOp, en);
        graph.addOperand(writeOp, sumValue);
        graph.addOperand(writeOp, maskValue);
        graph.addOperand(writeOp, clk);
        graph.setAttr(writeOp, "regSymbol", std::string("q"));
        graph.setAttr(writeOp, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        const std::filesystem::path exportPath =
            std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "activity_schedule_top_compute_dag.json";
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "top",
                                    .maxOpInComputeSupernode = 4,
                                    .exportComputeDagPath = exportPath.string()}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed");
        }
        const auto schedule = loadSchedule(session, "top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if (!std::filesystem::exists(exportPath))
        {
            return fail("Expected activity-schedule compute DAG export file to exist");
        }
        const std::string exportedDag = readFile(exportPath);
        if (exportedDag.find("\"format\":\"wolvrix.compute-op-dag.v1\"") == std::string::npos ||
            exportedDag.find("\"graph_id\":\"top.activity_compute\"") == std::string::npos ||
            exportedDag.find("\"nodes\"") == std::string::npos ||
            exportedDag.find("\"edges\"") == std::string::npos)
        {
            return fail("Expected compute DAG export to contain harness JSON fields");
        }

        const uint32_t addSupernode = (*schedule.opToSupernode)[addOp.index - 1];
        const uint32_t writeSupernode = (*schedule.opToSupernode)[writeOp.index - 1];
        if (addSupernode == kInvalidActivitySupernodeId || writeSupernode == kInvalidActivitySupernodeId ||
            addSupernode == writeSupernode)
        {
            return fail("Expected compute and commit ops to map to distinct supernodes");
        }
        if ((*schedule.opToSupernode)[regDecl.index - 1] != kInvalidActivitySupernodeId)
        {
            return fail("Expected declaration op to stay out of schedule");
        }
        if (!hasFanoutTo(*schedule.valueFanout, sumValue, writeSupernode))
        {
            return fail("Expected compute value fanout into commit supernode");
        }
        if (!hasFanoutTo(*schedule.valueFanout, maskValue, writeSupernode))
        {
            return fail("Expected direct source value dependency into commit supernode");
        }
        if (schedule.summaryStats->find("\"compute_commit_value_pairs\":2") == std::string::npos)
        {
            return fail("Expected summary_stats to report two compute->commit value pairs in top case");
        }
        if (schedule.summaryStats->find("\"compute_compute_value_pairs\":0") == std::string::npos)
        {
            return fail("Expected summary_stats to report zero compute->compute value pairs in top case");
        }
        if (schedule.summaryStats->find("\"state_read_activation_edges\":0") == std::string::npos)
        {
            return fail("Expected top case to avoid cross-supernode state-read propagation");
        }
        if (schedule.summaryStats->find("\"memory_read_activation_edges\":0") == std::string::npos)
        {
            return fail("Expected top case to report zero memory-read propagation");
        }
        if (schedule.summaryStats->find("\"constant_activation_edges\":1") == std::string::npos)
        {
            return fail("Expected top case to report one constant propagation edge");
        }
        if (schedule.summaryStats->find("\"other_compute_activation_edges\":1") == std::string::npos)
        {
            return fail("Expected top case to report one compute propagation edge");
        }
        const auto readersIt = schedule.stateReadSupernodes->find("q");
        if (readersIt == schedule.stateReadSupernodes->end() || readersIt->second.empty())
        {
            return fail("Expected register read state mapping to compute supernode");
        }
    }

    {
        currentCase = "source_compute";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("source_compute");
        design.markAsTop("source_compute");

        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("a", a);
        const auto c = makeValue(graph, "c", 8);
        const auto cOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                               graph.internSymbol("const_source"));
        graph.addResult(cOp, c);
        graph.setAttr(cOp, "constValue", std::string("8'h01"));
        const auto y = makeValue(graph, "y", 8);
        const auto addOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                 graph.internSymbol("add"));
        graph.addOperand(addOp, a);
        graph.addOperand(addOp, c);
        graph.addResult(addOp, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "source_compute",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected source-to-compute schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "source_compute");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t addSupernode = (*schedule.opToSupernode)[addOp.index - 1];
        if (addSupernode == kInvalidActivitySupernodeId)
        {
            return fail("Expected compute op to map to a supernode");
        }
        bool foundLocalConstClone = false;
        wolvrix::lib::grh::OperationId clonedConstOp = wolvrix::lib::grh::OperationId::invalid();
        for (const auto opId : (*schedule.supernodeToOps)[addSupernode])
        {
            if (opId != cOp && graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kConstant)
            {
                foundLocalConstClone = true;
                clonedConstOp = opId;
            }
        }
        if (!foundLocalConstClone)
        {
            for (const auto opId : graph.operations())
            {
                if (opId != cOp && graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kConstant)
                {
                    const auto supernode = (*schedule.opToSupernode)[opId.index - 1];
                    if (supernode != kInvalidActivitySupernodeId)
                    {
                        foundLocalConstClone = true;
                        clonedConstOp = opId;
                        break;
                    }
                }
            }
        }
        if (!foundLocalConstClone || !clonedConstOp.valid())
        {
            return fail("Expected source constant clone to enter compute scheduling");
        }
        if (graph.opOperands(addOp).size() < 2 ||
            graph.valueDef(graph.opOperands(addOp)[1]) != clonedConstOp)
        {
            return fail("Expected compute op operand to be rewritten to source clone");
        }
    }

    {
        currentCase = "mem_read";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("mem_read");
        design.markAsTop("mem_read");

        const auto addr = makeValue(graph, "addr", 2);
        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("a", a);
        const auto memDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kMemory,
                                                   graph.internSymbol("m"));
        graph.setAttr(memDecl, "width", static_cast<int64_t>(8));
        graph.setAttr(memDecl, "row", static_cast<int64_t>(4));
        graph.setAttr(memDecl, "isSigned", false);
        const auto readValue = makeValue(graph, "r", 8);
        const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kMemoryReadPort,
                                                  graph.internSymbol("read"));
        graph.setAttr(readOp, "memSymbol", std::string("m"));
        graph.addOperand(readOp, addr);
        graph.addResult(readOp, readValue);
        const auto y = makeValue(graph, "y", 8);
        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor"));
        graph.addOperand(xorOp, readValue);
        graph.addOperand(xorOp, a);
        graph.addResult(xorOp, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "mem_read",
            .maxOpInComputeSupernode = 2,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected memory-read schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "mem_read");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        std::size_t memoryReadCount = 0;
        std::size_t scheduledMemoryReadCount = 0;
        for (const auto opId : graph.operations())
        {
            if (graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
            {
                ++memoryReadCount;
                if (opId.index > 0 &&
                    opId.index - 1 < schedule.opToSupernode->size() &&
                    (*schedule.opToSupernode)[opId.index - 1] != kInvalidActivitySupernodeId)
                {
                    ++scheduledMemoryReadCount;
                }
            }
        }
        if (memoryReadCount != 2)
        {
            return fail("Expected memory read to be cloned as source");
        }
        if (scheduledMemoryReadCount == 0)
        {
            return fail("Expected a memory read clone to be scheduled as compute op");
        }
        if (schedule.summaryStats->find("\"memory_read_activation_edges\":0") == std::string::npos)
        {
            return fail("Expected mem_read case to avoid cross-supernode memory-read propagation");
        }
    }

    {
        currentCase = "common_expr";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("common_expr");
        design.markAsTop("common_expr");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        const auto c = makeValue(graph, "c", 8);
        const auto d = makeValue(graph, "d", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);
        graph.bindInputPort("d", d);
        const auto shared = makeValue(graph, "shared", 8);
        const auto sharedOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                    graph.internSymbol("shared_op"));
        graph.addOperand(sharedOp, a);
        graph.addOperand(sharedOp, b);
        graph.addResult(sharedOp, shared);
        const auto y0 = makeValue(graph, "y0", 8);
        const auto andOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd,
                                                 graph.internSymbol("and"));
        graph.addOperand(andOp, shared);
        graph.addOperand(andOp, c);
        graph.addResult(andOp, y0);
        graph.bindOutputPort("y0", y0);
        const auto y1 = makeValue(graph, "y1", 8);
        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor"));
        graph.addOperand(xorOp, shared);
        graph.addOperand(xorOp, d);
        graph.addResult(xorOp, y1);
        graph.bindOutputPort("y1", y1);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "common_expr",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected common expr schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "common_expr");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t sharedSupernode = (*schedule.opToSupernode)[sharedOp.index - 1];
        const uint32_t andSupernode = (*schedule.opToSupernode)[andOp.index - 1];
        const uint32_t xorSupernode = (*schedule.opToSupernode)[xorOp.index - 1];
        if (sharedSupernode != andSupernode)
        {
            return fail("Expected safe shared expression to be owned by its earliest consumer");
        }
        if (!hasFanoutTo(*schedule.valueFanout, shared, xorSupernode))
        {
            return fail("Expected shared expression fanout to later consumer");
        }
        if (schedule.summaryStats->find("\"other_compute_activation_edges\":1") == std::string::npos)
        {
            return fail("Expected common_expr case to report one remaining compute propagation edge");
        }
        if (schedule.summaryStats->find("\"other_compute_multi_target_values\":0") == std::string::npos)
        {
            return fail("Expected common_expr case to avoid multi-target compute value after ownership selection");
        }
    }

    {
        currentCase = "shared_condition_feedback_cycle";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("shared_condition_feedback_cycle");
        design.markAsTop("shared_condition_feedback_cycle");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto req = makeValue(graph, "req", 2);
        const auto flag = makeValue(graph, "flag", 1);
        const auto a = makeValue(graph, "a", 27);
        const auto b = makeValue(graph, "b", 27);
        const auto c = makeValue(graph, "c", 27);
        const auto fallback = makeValue(graph, "fallback", 27);
        const auto idx = makeValue(graph, "idx", 2);
        const auto dummy = makeValue(graph, "dummy", 27);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("req", req);
        graph.bindInputPort("flag", flag);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);
        graph.bindInputPort("fallback", fallback);
        graph.bindInputPort("idx", idx);
        graph.bindInputPort("dummy", dummy);

        const auto regDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol("q"));
        graph.setAttr(regDecl, "width", static_cast<int64_t>(27));
        graph.setAttr(regDecl, "isSigned", false);

        const auto two = makeValue(graph, "two", 2);
        const auto twoOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                 graph.internSymbol("two_const"));
        graph.addResult(twoOp, two);
        graph.setAttr(twoOp, "constValue", std::string("2'h2"));

        const auto mask = makeValue(graph, "mask", 27);
        const auto maskOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                  graph.internSymbol("mask_const"));
        graph.addResult(maskOp, mask);
        graph.setAttr(maskOp, "constValue", std::string("27'h7ffffff"));

        const auto onlyS2 = makeValue(graph, "only_s2", 1);
        const auto onlyS2Op = graph.createOperation(wolvrix::lib::grh::OperationKind::kEq,
                                                    graph.internSymbol("only_s2_eq"));
        graph.addOperand(onlyS2Op, req);
        graph.addOperand(onlyS2Op, two);
        graph.addResult(onlyS2Op, onlyS2);

        const auto cond = makeValue(graph, "write_cond", 1);
        const auto condOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kOr,
                                                  graph.internSymbol("write_cond_or"));
        graph.addOperand(condOp, onlyS2);
        graph.addOperand(condOp, flag);
        graph.addResult(condOp, cond);

        const auto gvpn = makeValue(graph, "gvpn", 27);
        const auto gvpnOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kMux,
                                                  graph.internSymbol("gvpn_mux"));
        graph.addOperand(gvpnOp, onlyS2);
        graph.addOperand(gvpnOp, a);
        graph.addOperand(gvpnOp, b);
        graph.addResult(gvpnOp, gvpn);

        const auto earlyUse = makeValue(graph, "early_use", 27);
        const auto earlyUseOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                      graph.internSymbol("early_gvpn_use"));
        graph.addOperand(earlyUseOp, gvpn);
        graph.addOperand(earlyUseOp, dummy);
        graph.addResult(earlyUseOp, earlyUse);
        graph.bindOutputPort("early_use", earlyUse);

        const auto packed = makeValue(graph, "packed", 54);
        const auto packedOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                    graph.internSymbol("packed_concat"));
        graph.addOperand(packedOp, c);
        graph.addOperand(packedOp, gvpn);
        graph.addResult(packedOp, packed);

        const auto selected = makeValue(graph, "selected", 27);
        const auto selectedOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                                      graph.internSymbol("selected_dynamic_slice"));
        graph.addOperand(selectedOp, packed);
        graph.addOperand(selectedOp, idx);
        graph.addResult(selectedOp, selected);
        graph.setAttr(selectedOp, "sliceWidth", static_cast<int64_t>(27));

        const auto rhs = makeValue(graph, "rhs", 27);
        const auto rhsOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kMux,
                                                 graph.internSymbol("rhs_mux"));
        graph.addOperand(rhsOp, cond);
        graph.addOperand(rhsOp, selected);
        graph.addOperand(rhsOp, fallback);
        graph.addResult(rhsOp, rhs);

        const auto writeOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                   graph.internSymbol("q_write"));
        graph.addOperand(writeOp, en);
        graph.addOperand(writeOp, rhs);
        graph.addOperand(writeOp, mask);
        graph.addOperand(writeOp, clk);
        graph.setAttr(writeOp, "regSymbol", std::string("q"));
        graph.setAttr(writeOp, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "shared_condition_feedback_cycle",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected shared-condition feedback case to schedule without compute-node cycle");
        }
        const auto schedule = loadSchedule(session, "shared_condition_feedback_cycle");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if (!hasFanoutTo(*schedule.valueFanout, onlyS2, (*schedule.opToSupernode)[gvpnOp.index - 1]))
        {
            return fail("Expected shared condition to remain an explicit dependency of gvpn");
        }
        if (!hasFanoutTo(*schedule.valueFanout, gvpn, (*schedule.opToSupernode)[packedOp.index - 1]))
        {
            return fail("Expected gvpn to remain an explicit dependency of packed write path");
        }
    }

    {
        currentCase = "declared_value_local_compute";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("declared_value_local_compute");
        design.markAsTop("declared_value_local_compute");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        const auto c = makeValue(graph, "c", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);

        const auto wireSym = graph.internSymbol("declared_wire");
        graph.addDeclaredSymbol(wireSym);
        const auto declaredWire = graph.createValue(wireSym, 8, false);
        const auto declaredProducer = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                            graph.internSymbol("declared_producer"));
        graph.addOperand(declaredProducer, a);
        graph.addOperand(declaredProducer, b);
        graph.addResult(declaredProducer, declaredWire);

        const auto y = makeValue(graph, "y", 8);
        const auto consumer = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                    graph.internSymbol("declared_consumer"));
        graph.addOperand(consumer, declaredWire);
        graph.addOperand(consumer, c);
        graph.addResult(consumer, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "declared_value_local_compute",
            .maxOpInComputeSupernode = 2,
            .enableCoarsen = false,
            .enableChainMerge = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected declared-value-local-compute schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "declared_value_local_compute");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t producerSupernode = (*schedule.opToSupernode)[declaredProducer.index - 1];
        const uint32_t consumerSupernode = (*schedule.opToSupernode)[consumer.index - 1];
        if (producerSupernode == kInvalidActivitySupernodeId ||
            consumerSupernode == kInvalidActivitySupernodeId ||
            producerSupernode != consumerSupernode)
        {
            return fail("Expected declared compute value producer and single consumer to stay local");
        }
        if (hasFanoutTo(*schedule.valueFanout, declaredWire, consumerSupernode))
        {
            return fail("Expected local declared compute value to avoid cross-supernode fanout");
        }
        if (schedule.summaryStats->find("\"compute_compute_value_pairs\":0") == std::string::npos)
        {
            return fail("Expected declared_value_local_compute case to report zero compute->compute value pairs");
        }
    }

    {
        currentCase = "coarsen_respects_compute_supernode_op_limit";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("coarsen_respects_compute_supernode_op_limit");
        design.markAsTop("coarsen_respects_compute_supernode_op_limit");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        std::vector<wolvrix::lib::grh::ValueId> leaves;
        leaves.reserve(10);
        for (int i = 0; i < 10; ++i)
        {
            const auto result = makeValue(graph, "leaf_" + std::to_string(i), 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                  graph.internSymbol("leaf_op_" + std::to_string(i)));
            graph.addOperand(op, a);
            graph.addOperand(op, b);
            graph.addResult(op, result);
            leaves.push_back(result);
        }

        wolvrix::lib::grh::ValueId cursor = leaves.front();
        for (std::size_t i = 1; i < leaves.size(); ++i)
        {
            const auto result = makeValue(graph, "reduce_" + std::to_string(i), 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kOr,
                                                  graph.internSymbol("reduce_op_" + std::to_string(i)));
            graph.addOperand(op, cursor);
            graph.addOperand(op, leaves[i]);
            graph.addResult(op, result);
            cursor = result;
        }
        graph.bindOutputPort("y", cursor);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "coarsen_respects_compute_supernode_op_limit",
            .maxOpInComputeSupernode = 3,
            .maxOpInComputeNode = 1,
            .enableCoarsen = true,
            .enableChainMerge = true,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected coarsen op-limit schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "coarsen_respects_compute_supernode_op_limit");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        for (uint32_t supernodeId = 0; supernodeId < schedule.supernodeToOps->size(); ++supernodeId)
        {
            if ((*schedule.supernodeKinds)[supernodeId] == ActivityScheduleSupernodeKind::Compute &&
                (*schedule.supernodeToOps)[supernodeId].size() > 3)
            {
                return fail("Expected coarsened compute supernodes to obey maxOpInComputeSupernode");
            }
        }
    }

    {
        currentCase = "split_oversize_compute_node";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("split_oversize_compute_node");
        design.markAsTop("split_oversize_compute_node");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        std::vector<wolvrix::lib::grh::OperationId> ops;
        wolvrix::lib::grh::ValueId cursor = a;
        for (int i = 0; i < 5; ++i)
        {
            const auto result = makeValue(graph, "chain_" + std::to_string(i), 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                  graph.internSymbol("chain_op_" + std::to_string(i)));
            graph.addOperand(op, cursor);
            graph.addOperand(op, b);
            graph.addResult(op, result);
            ops.push_back(op);
            cursor = result;
        }
        graph.bindOutputPort("y", cursor);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "split_oversize_compute_node",
            .maxOpInComputeSupernode = 2,
            .maxOpInComputeNode = 16,
            .enableCoarsen = false,
            .splitOversizeComputeNodes = true,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected oversize compute-node split schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "split_oversize_compute_node");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t firstSupernode = (*schedule.opToSupernode)[ops.front().index - 1];
        const uint32_t lastSupernode = (*schedule.opToSupernode)[ops.back().index - 1];
        if (firstSupernode == kInvalidActivitySupernodeId ||
            lastSupernode == kInvalidActivitySupernodeId ||
            firstSupernode == lastSupernode)
        {
            return fail("Expected oversize compute node to split into multiple final supernodes");
        }
        bool splitReachable = false;
        if (firstSupernode < schedule.dag->size())
        {
            std::vector<uint32_t> stack{firstSupernode};
            std::vector<uint8_t> seen(schedule.dag->size(), 0);
            seen[firstSupernode] = 1;
            while (!stack.empty())
            {
                const uint32_t node = stack.back();
                stack.pop_back();
                if (node == lastSupernode)
                {
                    splitReachable = true;
                    break;
                }
                for (const uint32_t succ : (*schedule.dag)[node])
                {
                    if (succ < seen.size() && seen[succ] == 0)
                    {
                        seen[succ] = 1;
                        stack.push_back(succ);
                    }
                }
            }
        }
        if (!splitReachable)
        {
            return fail("Expected split chunks from the same MFFC compute node to stay reachable in DAG");
        }
        for (const auto &supernodeOps : *schedule.supernodeToOps)
        {
            if (supernodeOps.size() > 2)
            {
                return fail("Expected split final compute supernodes to obey maxOpInComputeSupernode");
            }
        }
    }

    {
        currentCase = "commit_chunk";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("commit_chunk");
        design.markAsTop("commit_chunk");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto mask = makeValue(graph, "mask", 8);
        const auto d0 = makeValue(graph, "d0", 8);
        const auto d1 = makeValue(graph, "d1", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        for (const char *name : {"q0", "q1"})
        {
            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
        }
        const auto write0 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w0"));
        graph.addOperand(write0, en);
        graph.addOperand(write0, d0);
        graph.addOperand(write0, mask);
        graph.addOperand(write0, clk);
        graph.setAttr(write0, "regSymbol", std::string("q0"));
        graph.setAttr(write0, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write1 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w1"));
        graph.addOperand(write1, en);
        graph.addOperand(write1, d1);
        graph.addOperand(write1, mask);
        graph.addOperand(write1, clk);
        graph.setAttr(write1, "regSymbol", std::string("q1"));
        graph.setAttr(write1, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "commit_chunk",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 1,
            .enableCoarsen = false,
            .commitGuardEventBuckets = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected commit chunk schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "commit_chunk");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if ((*schedule.opToSupernode)[write0.index - 1] == (*schedule.opToSupernode)[write1.index - 1])
        {
            return fail("Expected maxOpInCommitSupernode to split commit supernodes");
        }
    }

    {
        currentCase = "commit_guard_event_bucket";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("commit_guard_event_bucket");
        design.markAsTop("commit_guard_event_bucket");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto otherEn = makeValue(graph, "other_en", 1);
        const auto thirdEn = makeValue(graph, "third_en", 1);
        const auto mask = makeValue(graph, "mask", 8);
        const auto d0 = makeValue(graph, "d0", 8);
        const auto d1 = makeValue(graph, "d1", 8);
        const auto d2 = makeValue(graph, "d2", 8);
        const auto d3 = makeValue(graph, "d3", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("other_en", otherEn);
        graph.bindInputPort("third_en", thirdEn);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        graph.bindInputPort("d2", d2);
        graph.bindInputPort("d3", d3);
        for (const char *name : {"q0", "q1", "q2", "q3"})
        {
            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
        }

        const auto write0 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w0"));
        graph.addOperand(write0, en);
        graph.addOperand(write0, d0);
        graph.addOperand(write0, mask);
        graph.addOperand(write0, clk);
        graph.setAttr(write0, "regSymbol", std::string("q0"));
        graph.setAttr(write0, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write1 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w1"));
        graph.addOperand(write1, otherEn);
        graph.addOperand(write1, d1);
        graph.addOperand(write1, mask);
        graph.addOperand(write1, clk);
        graph.setAttr(write1, "regSymbol", std::string("q1"));
        graph.setAttr(write1, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write2 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w2"));
        graph.addOperand(write2, en);
        graph.addOperand(write2, d2);
        graph.addOperand(write2, mask);
        graph.addOperand(write2, clk);
        graph.setAttr(write2, "regSymbol", std::string("q2"));
        graph.setAttr(write2, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write3 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w3"));
        graph.addOperand(write3, thirdEn);
        graph.addOperand(write3, d3);
        graph.addOperand(write3, mask);
        graph.addOperand(write3, clk);
        graph.setAttr(write3, "regSymbol", std::string("q3"));
        graph.setAttr(write3, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "commit_guard_event_bucket",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 3,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected commit guard event bucket schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "commit_guard_event_bucket");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t write0Supernode = (*schedule.opToSupernode)[write0.index - 1];
        const uint32_t write1Supernode = (*schedule.opToSupernode)[write1.index - 1];
        const uint32_t write2Supernode = (*schedule.opToSupernode)[write2.index - 1];
        const uint32_t write3Supernode = (*schedule.opToSupernode)[write3.index - 1];
        if (write0Supernode != write1Supernode || write0Supernode != write2Supernode)
        {
            return fail("Expected guard buckets to merge while total commit ops stay under cap");
        }
        if (write0Supernode == write3Supernode)
        {
            return fail("Expected guard bucket packing to split before exceeding commit op cap");
        }
        const auto &commitOps = (*schedule.supernodeToOps)[write0Supernode];
        const auto write0It = std::find(commitOps.begin(), commitOps.end(), write0);
        const auto write1It = std::find(commitOps.begin(), commitOps.end(), write1);
        const auto write2It = std::find(commitOps.begin(), commitOps.end(), write2);
        if (write0It == commitOps.end() || write1It == commitOps.end() || write2It == commitOps.end())
        {
            return fail("Expected commit supernode to contain all test writes");
        }
        const auto write0Pos = std::distance(commitOps.begin(), write0It);
        const auto write1Pos = std::distance(commitOps.begin(), write1It);
        const auto write2Pos = std::distance(commitOps.begin(), write2It);
        if (write2Pos != write0Pos + 1 || write1Pos == write0Pos + 1)
        {
            return fail("Expected same-guard writes to stay adjacent inside the event commit supernode");
        }
    }

    {
        currentCase = "commit_guard_event_oversize_bucket";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("commit_guard_event_oversize_bucket");
        design.markAsTop("commit_guard_event_oversize_bucket");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto otherEn = makeValue(graph, "other_en", 1);
        const auto mask = makeValue(graph, "mask", 8);
        const auto d0 = makeValue(graph, "d0", 8);
        const auto d1 = makeValue(graph, "d1", 8);
        const auto d2 = makeValue(graph, "d2", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("other_en", otherEn);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        graph.bindInputPort("d2", d2);
        for (const char *name : {"q0", "q1", "q2"})
        {
            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
        }

        const auto write0 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w0"));
        graph.addOperand(write0, en);
        graph.addOperand(write0, d0);
        graph.addOperand(write0, mask);
        graph.addOperand(write0, clk);
        graph.setAttr(write0, "regSymbol", std::string("q0"));
        graph.setAttr(write0, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write1 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w1"));
        graph.addOperand(write1, otherEn);
        graph.addOperand(write1, d1);
        graph.addOperand(write1, mask);
        graph.addOperand(write1, clk);
        graph.setAttr(write1, "regSymbol", std::string("q1"));
        graph.setAttr(write1, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write2 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w2"));
        graph.addOperand(write2, en);
        graph.addOperand(write2, d2);
        graph.addOperand(write2, mask);
        graph.addOperand(write2, clk);
        graph.setAttr(write2, "regSymbol", std::string("q2"));
        graph.setAttr(write2, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "commit_guard_event_oversize_bucket",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected commit guard event oversized bucket schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "commit_guard_event_oversize_bucket");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t write0Supernode = (*schedule.opToSupernode)[write0.index - 1];
        const uint32_t write1Supernode = (*schedule.opToSupernode)[write1.index - 1];
        const uint32_t write2Supernode = (*schedule.opToSupernode)[write2.index - 1];
        if (write0Supernode != write2Supernode)
        {
            return fail("Expected oversized guard bucket to remain a single commit supernode");
        }
        if (write0Supernode == write1Supernode)
        {
            return fail("Expected oversized guard bucket not to merge with following guard bucket");
        }
        const auto &commitOps = (*schedule.supernodeToOps)[write0Supernode];
        if (commitOps.size() != 2 ||
            std::find(commitOps.begin(), commitOps.end(), write0) == commitOps.end() ||
            std::find(commitOps.begin(), commitOps.end(), write2) == commitOps.end())
        {
            return fail("Expected oversized guard bucket supernode to contain exactly the same-guard writes");
        }
    }

    {
        // NO0207 Phase A: static change-probability propagation (-partition-policy=prob).
        currentCase = "Phase A static probability propagation";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("pi_top");
        design.markAsTop("pi_top");
        using K = wolvrix::lib::grh::OperationKind;

        auto makeRegRead = [&](const std::string &reg)
            -> std::pair<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::ValueId> {
            const auto regOp = graph.createOperation(K::kRegister, graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            const auto val = makeValue(graph, reg + "_read", 8);
            const auto readOp =
                graph.createOperation(K::kRegisterReadPort, graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, val);
            graph.setAttr(readOp, "regSymbol", reg);
            return {readOp, val};
        };

        const auto [aReadOp, aRead] = makeRegRead("pa");
        const auto [bReadOp, bRead] = makeRegRead("pb");
        (void)bReadOp;

        const auto c0Val = makeValue(graph, "pi_c0", 8);
        const auto c0Op = graph.createOperation(K::kConstant, graph.internSymbol("pi_c0_op"));
        graph.addResult(c0Op, c0Val);
        graph.setAttr(c0Op, "constValue", std::string("0"));

        auto makeAssign = [&](const std::string &name, wolvrix::lib::grh::ValueId in) {
            const auto v = makeValue(graph, name, 8);
            const auto op = graph.createOperation(K::kAssign, graph.internSymbol(name + "_op"));
            graph.addOperand(op, in);
            graph.addResult(op, v);
            return v;
        };
        const auto a1 = makeAssign("pi_a1", aRead);
        const auto a2 = makeAssign("pi_a2", aRead);

        const auto csVal = makeValue(graph, "pi_cs", 16);
        const auto concatSame = graph.createOperation(K::kConcat, graph.internSymbol("pi_concat_same"));
        graph.addOperand(concatSame, a1);
        graph.addOperand(concatSame, a2);
        graph.addResult(concatSame, csVal);

        const auto cdVal = makeValue(graph, "pi_cd", 16);
        const auto concatDiff = graph.createOperation(K::kConcat, graph.internSymbol("pi_concat_diff"));
        graph.addOperand(concatDiff, aRead);
        graph.addOperand(concatDiff, bRead);
        graph.addResult(concatDiff, cdVal);

        const auto mxVal = makeValue(graph, "pi_mx", 8);
        const auto muxOp = graph.createOperation(K::kMux, graph.internSymbol("pi_mux"));
        graph.addOperand(muxOp, aRead); // sel
        graph.addOperand(muxOp, bRead); // a
        graph.addOperand(muxOp, c0Val); // b
        graph.addResult(muxOp, mxVal);

        // NO0208 §附录 A: 逻辑掩蔽 vs 透传 —— AND 衰减、XOR 透传
        const auto andVal = makeValue(graph, "pi_and", 8);
        const auto andOp = graph.createOperation(K::kAnd, graph.internSymbol("pi_and_op"));
        graph.addOperand(andOp, aRead);
        graph.addOperand(andOp, bRead);
        graph.addResult(andOp, andVal);

        const auto xorVal = makeValue(graph, "pi_xor", 8);
        const auto xorOp = graph.createOperation(K::kXor, graph.internSymbol("pi_xor_op"));
        graph.addOperand(xorOp, aRead);
        graph.addOperand(xorOp, bRead);
        graph.addResult(xorOp, xorVal);

        graph.bindOutputPort("o_cs", csVal);
        graph.bindOutputPort("o_cd", cdVal);
        graph.bindOutputPort("o_mx", mxVal);
        graph.bindOutputPort("o_and", andVal);
        graph.bindOutputPort("o_xor", xorVal);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "pi_top", .partitionPolicy = "prob"}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule (prob) to succeed for pi propagation");
        }

        const auto *pi = getSessionValue<std::vector<float>>(session, "pi_top.activity_schedule.op_pi");
        if (pi == nullptr)
        {
            return fail("Expected op_pi session value under prob policy");
        }
        auto piOf = [&](wolvrix::lib::grh::OperationId op) -> float {
            return op.index < pi->size() ? (*pi)[op.index] : -1.0F;
        };
        auto near = [](float got, float want) { return std::fabs(got - want) < 1e-3F; };

        if (!near(piOf(c0Op), 0.0F))
        {
            return fail("Expected constant pi=0");
        }
        if (!near(piOf(aReadOp), 0.2F))
        {
            return fail("Expected register-read pi=piRegRead(0.2)");
        }
        if (!near(piOf(concatSame), 0.2F))
        {
            return fail("Expected same-source concat pi=0.2 (source de-correlated)");
        }
        if (!near(piOf(concatDiff), 0.36F))
        {
            return fail("Expected independent concat pi=0.36 (transparent product-complement)");
        }
        if (!near(piOf(andOp), 0.2F))
        {
            return fail("Expected AND pi=0.2 (logical-masking attenuation, p1=0.5)");
        }
        if (!near(piOf(xorOp), 0.36F))
        {
            return fail("Expected XOR pi=0.36 (transparent)");
        }
        if (!near(piOf(muxOp), 0.2F))
        {
            return fail("Expected mux pi=0.2 (branch mean + 0.5*sel)");
        }
    }

    {
        // NO0207 Phase B: cost uses GRHSIM execution slots, not raw bit width.
        currentCase = "Phase B activity cost model";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("cost_top");
        design.markAsTop("cost_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto reg = graph.createOperation(K::kRegister, graph.internSymbol("cost_reg"));
        graph.setAttr(reg, "width", int64_t{64});
        graph.setAttr(reg, "isSigned", false);

        auto makeRegRead = [&](const std::string &name, int32_t width)
            -> std::pair<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::ValueId> {
            const auto val = makeValue(graph, name + "_read", width);
            const auto readOp =
                graph.createOperation(K::kRegisterReadPort, graph.internSymbol(name + "_read_op"));
            graph.addResult(readOp, val);
            graph.setAttr(readOp, "regSymbol", std::string("cost_reg"));
            return {readOp, val};
        };
        auto makeConst = [&](const std::string &name, int32_t width)
            -> std::pair<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::ValueId> {
            const auto val = makeValue(graph, name + "_const", width);
            const auto op = graph.createOperation(K::kConstant, graph.internSymbol(name + "_const_op"));
            graph.addResult(op, val);
            graph.setAttr(op, "constValue", std::string("0"));
            return {op, val};
        };
        auto makeAssign = [&](const std::string &name, wolvrix::lib::grh::ValueId in, int32_t width) {
            const auto val = makeValue(graph, name, width);
            const auto op = graph.createOperation(K::kAssign, graph.internSymbol(name + "_op"));
            graph.addOperand(op, in);
            graph.addResult(op, val);
            graph.bindOutputPort(name, val);
            return op;
        };

        const auto [read8Op, read8] = makeRegRead("cost_w8", 8);
        const auto [const8Op, const8] = makeConst("cost_w8", 8);
        const auto assign8Op = makeAssign("cost_assign8", read8, 8);
        (void)const8;

        const auto [read9Op, read9] = makeRegRead("cost_w9", 9);
        (void)read9Op;
        const auto assign9Op = makeAssign("cost_assign9", read9, 9);

        const auto [read31Op, read31] = makeRegRead("cost_w31", 31);
        (void)read31Op;
        const auto assign31Op = makeAssign("cost_assign31", read31, 31);

        const auto [read64Op, read64] = makeRegRead("cost_w64", 64);
        (void)read64Op;
        const auto assign64Op = makeAssign("cost_assign64", read64, 64);

        const auto [read65Op, read65] = makeRegRead("cost_w65", 65);
        const auto assign65Op = makeAssign("cost_assign65", read65, 65);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "cost_top", .partitionPolicy = "prob"}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule (prob) to succeed for activity cost model");
        }

        const auto *weight =
            getSessionValue<std::vector<double>>(session, "cost_top.activity_schedule.op_compute_weight");
        const auto *change =
            getSessionValue<std::vector<double>>(session, "cost_top.activity_schedule.op_change_weight");
        const auto *footprint =
            getSessionValue<std::vector<std::uint64_t>>(session, "cost_top.activity_schedule.op_footprint_bytes");
        const auto *pi = getSessionValue<std::vector<float>>(session, "cost_top.activity_schedule.op_pi");
        if (weight == nullptr || change == nullptr || footprint == nullptr || pi == nullptr)
        {
            return fail("Expected Phase B cost model session values under prob policy");
        }
        auto wOf = [&](wolvrix::lib::grh::OperationId op) -> double {
            return op.index < weight->size() ? (*weight)[op.index] : -1.0;
        };
        auto cwOf = [&](wolvrix::lib::grh::OperationId op) -> double {
            return op.index < change->size() ? (*change)[op.index] : -1.0;
        };
        auto fpOf = [&](wolvrix::lib::grh::OperationId op) -> std::uint64_t {
            return op.index < footprint->size() ? (*footprint)[op.index] : 0;
        };
        auto piOf = [&](wolvrix::lib::grh::OperationId op) -> double {
            return op.index < pi->size() ? static_cast<double>((*pi)[op.index]) : -1.0;
        };
        auto nearD = [](double got, double want) { return std::fabs(got - want) < 1e-9; };
        auto nearDLoose = [](double got, double want) { return std::fabs(got - want) < 1e-6; };

        if (!nearD(wOf(assign8Op), 1.0) ||
            !nearD(wOf(assign9Op), 1.0) ||
            !nearD(wOf(assign31Op), 1.0) ||
            !nearD(wOf(assign64Op), 1.0))
        {
            return fail("Expected scalar widths 8/9/31/64 to have one compute unit");
        }
        if (!nearD(wOf(assign65Op), 2.0))
        {
            return fail("Expected width 65 to round up to two compute units");
        }
        if (!nearD(wOf(read8Op), 2.0) || !nearD(wOf(read65Op), 4.0))
        {
            return fail("Expected source read cost to use src class multiplier over compute units");
        }
        if (!nearD(wOf(const8Op), 0.125))
        {
            return fail("Expected constant cost to use lightweight const multiplier");
        }
        if (fpOf(assign8Op) != 1 || fpOf(assign9Op) != 2 ||
            fpOf(assign31Op) != 4 || fpOf(assign64Op) != 8 ||
            fpOf(assign65Op) != 16)
        {
            return fail("Expected footprint bytes to follow GRHSIM storage buckets");
        }
        if (!nearDLoose(cwOf(assign8Op), wOf(assign8Op) * piOf(assign8Op)))
        {
            return fail("Expected change weight to equal compute weight times propagated pi");
        }
    }

    {
        // NO0207 Phase C: aggregate probability/cost data at computeNode granularity.
        currentCase = "Phase C activity hypergraph aggregation";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("hyper_top");
        design.markAsTop("hyper_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto addr = makeValue(graph, "hyper_addr", 8);
        const auto data = makeValue(graph, "hyper_data", 8);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("data", data);

        const auto memDecl = graph.createOperation(K::kMemory, graph.internSymbol("hyper_mem"));
        graph.setAttr(memDecl, "width", int64_t{8});
        graph.setAttr(memDecl, "row", int64_t{4});
        graph.setAttr(memDecl, "isSigned", false);

        const auto readValue = makeValue(graph, "hyper_read", 8);
        const auto readOp = graph.createOperation(K::kMemoryReadPort, graph.internSymbol("hyper_read_op"));
        graph.setAttr(readOp, "memSymbol", std::string("hyper_mem"));
        graph.addOperand(readOp, addr);
        graph.addResult(readOp, readValue);

        const auto andValue = makeValue(graph, "hyper_and", 8);
        const auto andOp = graph.createOperation(K::kAnd, graph.internSymbol("hyper_and_op"));
        graph.addOperand(andOp, readValue);
        graph.addOperand(andOp, data);
        graph.addResult(andOp, andValue);

        const auto xorValue = makeValue(graph, "hyper_xor", 8);
        const auto xorOp = graph.createOperation(K::kXor, graph.internSymbol("hyper_xor_op"));
        graph.addOperand(xorOp, andValue);
        graph.addOperand(xorOp, data);
        graph.addResult(xorOp, xorValue);
        graph.bindOutputPort("y", xorValue);

        const auto orValue = makeValue(graph, "hyper_or", 8);
        const auto orOp = graph.createOperation(K::kOr, graph.internSymbol("hyper_or_op"));
        graph.addOperand(orOp, andValue);
        graph.addOperand(orOp, data);
        graph.addResult(orOp, orValue);
        graph.bindOutputPort("z", orValue);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "hyper_top";
        options.partitionPolicy = "prob";
        options.enableCoarsen = false;
        options.maxOpInComputeSupernode = 2;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule (prob) to succeed for Phase C aggregate");
        }

        const auto *nodeWeight =
            getSessionValue<std::vector<double>>(session, "hyper_top.activity_schedule.compute_node_weight");
        const auto *nodeChange =
            getSessionValue<std::vector<double>>(session, "hyper_top.activity_schedule.compute_node_change_weight");
        const auto *nodeFootprint = getSessionValue<std::vector<std::uint64_t>>(
            session,
            "hyper_top.activity_schedule.compute_node_footprint_bytes");
        const auto *nodeActive =
            getSessionValue<std::vector<double>>(session, "hyper_top.activity_schedule.compute_node_active_prob");
        const auto *nodeOps =
            getSessionValue<std::vector<uint32_t>>(session, "hyper_top.activity_schedule.compute_node_op_count");
        const auto *edgeFrom =
            getSessionValue<std::vector<uint32_t>>(session, "hyper_top.activity_schedule.compute_node_edge_from");
        const auto *edgeTo =
            getSessionValue<std::vector<uint32_t>>(session, "hyper_top.activity_schedule.compute_node_edge_to");
        const auto *edgeCount =
            getSessionValue<std::vector<std::uint64_t>>(session, "hyper_top.activity_schedule.compute_node_edge_count");
        const auto *edgeProb =
            getSessionValue<std::vector<double>>(session, "hyper_top.activity_schedule.compute_node_edge_total_prob");
        if (nodeWeight == nullptr || nodeChange == nullptr || nodeFootprint == nullptr ||
            nodeActive == nullptr || nodeOps == nullptr || edgeFrom == nullptr || edgeTo == nullptr ||
            edgeCount == nullptr || edgeProb == nullptr)
        {
            return fail("Expected Phase C computeNode aggregate session values under prob policy");
        }

        auto nearD = [](double got, double want) { return std::fabs(got - want) < 1e-6; };
        uint32_t andNode = kInvalidActivitySupernodeId;
        uint32_t orNode = kInvalidActivitySupernodeId;
        for (uint32_t i = 0; i < nodeWeight->size(); ++i)
        {
            if (i < nodeOps->size() && (*nodeOps)[i] == 3 && nearD((*nodeWeight)[i], 4.0))
            {
                andNode = i;
            }
            else if (i < nodeOps->size() && (*nodeOps)[i] == 1 && nearD((*nodeWeight)[i], 1.0))
            {
                orNode = i;
            }
        }
        if (andNode == kInvalidActivitySupernodeId || orNode == kInvalidActivitySupernodeId ||
            andNode == orNode)
        {
            return fail("Expected Phase C aggregate to expose AND+XOR node and distinct OR node");
        }
        if ((*nodeOps)[andNode] != 3)
        {
            return fail("Expected AND+XOR computeNode to include source clone plus two compute ops");
        }
        if (!nearD((*nodeWeight)[andNode], 4.0))
        {
            return fail("Expected canonical memory-read clone to contribute source weight to computeNode W");
        }
        if (!nearD((*nodeChange)[andNode], 0.49))
        {
            return fail("Expected canonical memory-read clone pi to contribute to computeNode change_weight");
        }
        if ((*nodeFootprint)[andNode] != 3)
        {
            return fail("Expected computeNode footprint to deduplicate canonical source/result values");
        }
        if (!nearD((*nodeActive)[andNode], 0.271))
        {
            return fail("Expected computeNode active_prob to include source/output and boundary pi estimates");
        }

        bool foundAndToOr = false;
        for (std::size_t i = 0; i < edgeFrom->size(); ++i)
        {
            if ((*edgeFrom)[i] == andNode && (*edgeTo)[i] == orNode)
            {
                foundAndToOr = true;
                if ((*edgeCount)[i] != 1 || !nearD((*edgeProb)[i], 0.1))
                {
                    return fail("Expected AND+XOR -> OR hyperedge count=1 and total_prob=pi(and)=0.1");
                }
            }
        }
        if (!foundAndToOr)
        {
            return fail("Expected Phase C hyperedge from AND+XOR computeNode to OR computeNode");
        }
    }

    {
        currentCase = "Phase F probability DP cuts low-activity boundary";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("prob_dp_top");
        design.markAsTop("prob_dp_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto a = makeValue(graph, "prob_dp_a", 8);
        const auto b = makeValue(graph, "prob_dp_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto highValue = makeValue(graph, "prob_dp_high", 8);
        const auto highOp = graph.createOperation(K::kXor, graph.internSymbol("prob_dp_high_op"));
        graph.addOperand(highOp, a);
        graph.addOperand(highOp, b);
        graph.addResult(highOp, highValue);

        const auto lowValue = makeValue(graph, "prob_dp_low", 1);
        const auto lowOp = graph.createOperation(K::kReduceAnd, graph.internSymbol("prob_dp_low_op"));
        graph.addOperand(lowOp, highValue);
        graph.addResult(lowOp, lowValue);

        const auto outValue = makeValue(graph, "prob_dp_out", 1);
        const auto outOp = graph.createOperation(K::kLogicNot, graph.internSymbol("prob_dp_out_op"));
        graph.addOperand(outOp, lowValue);
        graph.addResult(outOp, outValue);
        graph.bindOutputPort("y", outValue);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "prob_dp_top";
        options.partitionPolicy = "prob";
        options.probDpCost = true;
        options.probDpCostMode = "pi";
        options.fmRefineMaxRounds = 0;
        options.enableCoarsen = false;
        options.maxOpInComputeNode = 1;
        options.maxOpInComputeSupernode = 2;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected probability DP segment schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "prob_dp_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t highSupernode = (*schedule.opToSupernode)[highOp.index - 1];
        const uint32_t lowSupernode = (*schedule.opToSupernode)[lowOp.index - 1];
        const uint32_t outSupernode = (*schedule.opToSupernode)[outOp.index - 1];
        if (highSupernode == kInvalidActivitySupernodeId ||
            lowSupernode == kInvalidActivitySupernodeId ||
            outSupernode == kInvalidActivitySupernodeId)
        {
            return fail("Expected probability DP test ops to be scheduled");
        }
        if (highSupernode != lowSupernode)
        {
            return fail("Expected probability DP to keep high-activity boundary inside the segment");
        }
        if (lowSupernode == outSupernode)
        {
            return fail("Expected probability DP to cut the lower-activity boundary under capacity=2");
        }
        const double edgePi = parseJsonDoubleField(*schedule.summaryStats, "compute_compute_edge_pi_sum");
        const double edgeChangeWeight =
            parseJsonDoubleField(*schedule.summaryStats, "compute_compute_edge_change_weight_sum");
        if (std::fabs(edgePi - 0.095) > 1e-6)
        {
            return fail("Expected weighted summary stats to report only the low-activity cut edge pi");
        }
        if (std::fabs(edgeChangeWeight - 0.095) > 1e-6)
        {
            return fail("Expected weighted summary stats to report only the low-activity cut edge change weight");
        }
        if (parseJsonDoubleField(*schedule.summaryStats, "boundary_edge_pi_sum") < edgePi)
        {
            return fail("Expected total weighted boundary pi to include compute->compute edge pi");
        }
    }

    {
        currentCase = "Phase F probability DP cost can be disabled";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("prob_dp_off_top");
        design.markAsTop("prob_dp_off_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto a = makeValue(graph, "prob_dp_off_a", 8);
        const auto b = makeValue(graph, "prob_dp_off_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto highValue = makeValue(graph, "prob_dp_off_high", 8);
        const auto highOp = graph.createOperation(K::kXor, graph.internSymbol("prob_dp_off_high_op"));
        graph.addOperand(highOp, a);
        graph.addOperand(highOp, b);
        graph.addResult(highOp, highValue);

        const auto lowValue = makeValue(graph, "prob_dp_off_low", 1);
        const auto lowOp = graph.createOperation(K::kReduceAnd, graph.internSymbol("prob_dp_off_low_op"));
        graph.addOperand(lowOp, highValue);
        graph.addResult(lowOp, lowValue);

        const auto outValue = makeValue(graph, "prob_dp_off_out", 1);
        const auto outOp = graph.createOperation(K::kLogicNot, graph.internSymbol("prob_dp_off_out_op"));
        graph.addOperand(outOp, lowValue);
        graph.addResult(outOp, outValue);
        graph.bindOutputPort("y", outValue);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "prob_dp_off_top";
        options.partitionPolicy = "prob";
        options.probDpCost = false;
        options.fmRefineMaxRounds = 0;
        options.enableCoarsen = false;
        options.maxOpInComputeNode = 1;
        options.maxOpInComputeSupernode = 2;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected disabled probability DP cost schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "prob_dp_off_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t highSupernode = (*schedule.opToSupernode)[highOp.index - 1];
        const uint32_t lowSupernode = (*schedule.opToSupernode)[lowOp.index - 1];
        const uint32_t outSupernode = (*schedule.opToSupernode)[outOp.index - 1];
        if (highSupernode == kInvalidActivitySupernodeId ||
            lowSupernode == kInvalidActivitySupernodeId ||
            outSupernode == kInvalidActivitySupernodeId)
        {
            return fail("Expected disabled probability DP test ops to be scheduled");
        }
        if (highSupernode == lowSupernode)
        {
            return fail("Expected disabled probability DP cost to cut the first boundary");
        }
        if (lowSupernode != outSupernode)
        {
            return fail("Expected disabled probability DP cost to keep the later unweighted segment");
        }
        const double edgePi = parseJsonDoubleField(*schedule.summaryStats, "compute_compute_edge_pi_sum");
        if (std::fabs(edgePi - 0.19) > 1e-6)
        {
            return fail("Expected disabled probability DP cost to expose the higher-pi cut edge in stats");
        }
    }

    {
        currentCase = "Phase G FM refines probability boundary";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("prob_fm_top");
        design.markAsTop("prob_fm_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto a = makeValue(graph, "prob_fm_a", 8);
        const auto b = makeValue(graph, "prob_fm_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto highValue = makeValue(graph, "prob_fm_high", 8);
        const auto highOp = graph.createOperation(K::kXor, graph.internSymbol("prob_fm_high_op"));
        graph.addOperand(highOp, a);
        graph.addOperand(highOp, b);
        graph.addResult(highOp, highValue);

        const auto lowValue = makeValue(graph, "prob_fm_low", 1);
        const auto lowOp = graph.createOperation(K::kReduceAnd, graph.internSymbol("prob_fm_low_op"));
        graph.addOperand(lowOp, highValue);
        graph.addResult(lowOp, lowValue);

        const auto outValue = makeValue(graph, "prob_fm_out", 1);
        const auto outOp = graph.createOperation(K::kLogicNot, graph.internSymbol("prob_fm_out_op"));
        graph.addOperand(outOp, lowValue);
        graph.addResult(outOp, outValue);
        graph.bindOutputPort("y", outValue);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "prob_fm_top";
        options.partitionPolicy = "prob";
        options.probDpCost = false;
        options.enableCoarsen = false;
        options.maxOpInComputeNode = 1;
        options.maxOpInComputeSupernode = 2;
        options.phiMin = 0.0;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected FM probability refinement schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "prob_fm_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t highSupernode = (*schedule.opToSupernode)[highOp.index - 1];
        const uint32_t lowSupernode = (*schedule.opToSupernode)[lowOp.index - 1];
        const uint32_t outSupernode = (*schedule.opToSupernode)[outOp.index - 1];
        if (highSupernode == kInvalidActivitySupernodeId ||
            lowSupernode == kInvalidActivitySupernodeId ||
            outSupernode == kInvalidActivitySupernodeId)
        {
            return fail("Expected FM probability test ops to be scheduled");
        }
        if (highSupernode != lowSupernode)
        {
            return fail("Expected FM to move the middle node across the high-pi boundary");
        }
        if (lowSupernode == outSupernode)
        {
            return fail("Expected FM to leave the lower-pi boundary cut under capacity=2");
        }
        const double edgePi = parseJsonDoubleField(*schedule.summaryStats, "compute_compute_edge_pi_sum");
        if (std::fabs(edgePi - 0.095) > 1e-6)
        {
            return fail("Expected FM to reduce compute->compute edge pi to the low-pi boundary");
        }
        const auto *stats =
            getSessionValue<std::string>(session, "prob_fm_top.activity_schedule.prob_coarsen_stats");
        if (stats == nullptr || parseStatField(*stats, "fm_moves") == 0)
        {
            return fail("Expected FM stats to report at least one accepted move");
        }
    }

    {
        currentCase = "Phase F mixed probability DP cost tie-breaks by low pi";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("prob_dp_mixed_top");
        design.markAsTop("prob_dp_mixed_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto a = makeValue(graph, "prob_dp_mixed_a", 8);
        const auto b = makeValue(graph, "prob_dp_mixed_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto highValue = makeValue(graph, "prob_dp_mixed_high", 8);
        const auto highOp = graph.createOperation(K::kXor, graph.internSymbol("prob_dp_mixed_high_op"));
        graph.addOperand(highOp, a);
        graph.addOperand(highOp, b);
        graph.addResult(highOp, highValue);

        const auto lowValue = makeValue(graph, "prob_dp_mixed_low", 1);
        const auto lowOp = graph.createOperation(K::kReduceAnd, graph.internSymbol("prob_dp_mixed_low_op"));
        graph.addOperand(lowOp, highValue);
        graph.addResult(lowOp, lowValue);

        const auto outValue = makeValue(graph, "prob_dp_mixed_out", 1);
        const auto outOp = graph.createOperation(K::kLogicNot, graph.internSymbol("prob_dp_mixed_out_op"));
        graph.addOperand(outOp, lowValue);
        graph.addResult(outOp, outValue);
        graph.bindOutputPort("y", outValue);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "prob_dp_mixed_top";
        options.partitionPolicy = "prob";
        options.probDpCost = true;
        options.probDpCostMode = "mixed-pi";
        options.probDpAlpha = 1.0;
        options.fmRefineMaxRounds = 0;
        options.enableCoarsen = false;
        options.maxOpInComputeNode = 1;
        options.maxOpInComputeSupernode = 2;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected mixed probability DP cost schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "prob_dp_mixed_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t highSupernode = (*schedule.opToSupernode)[highOp.index - 1];
        const uint32_t lowSupernode = (*schedule.opToSupernode)[lowOp.index - 1];
        const uint32_t outSupernode = (*schedule.opToSupernode)[outOp.index - 1];
        if (highSupernode == kInvalidActivitySupernodeId ||
            lowSupernode == kInvalidActivitySupernodeId ||
            outSupernode == kInvalidActivitySupernodeId)
        {
            return fail("Expected mixed probability DP test ops to be scheduled");
        }
        if (highSupernode != lowSupernode)
        {
            return fail("Expected mixed probability DP cost to keep high-pi boundary inside the segment");
        }
        if (lowSupernode == outSupernode)
        {
            return fail("Expected mixed probability DP cost to cut the lower-pi boundary under capacity=2");
        }
    }

    {
        currentCase = "Phase E probability coarsen merges sibling candidates";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("prob_coarsen_top");
        design.markAsTop("prob_coarsen_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto a = makeValue(graph, "prob_a", 8);
        const auto b = makeValue(graph, "prob_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto shared = makeValue(graph, "prob_shared", 8);
        const auto sharedOp = graph.createOperation(K::kXor, graph.internSymbol("prob_shared_op"));
        graph.addOperand(sharedOp, a);
        graph.addOperand(sharedOp, b);
        graph.addResult(sharedOp, shared);

        const auto y = makeValue(graph, "prob_y", 8);
        const auto yOp = graph.createOperation(K::kXor, graph.internSymbol("prob_y_op"));
        graph.addOperand(yOp, shared);
        graph.addOperand(yOp, a);
        graph.addResult(yOp, y);
        graph.bindOutputPort("y", y);

        const auto z = makeValue(graph, "prob_z", 8);
        const auto zOp = graph.createOperation(K::kXor, graph.internSymbol("prob_z_op"));
        graph.addOperand(zOp, shared);
        graph.addOperand(zOp, b);
        graph.addResult(zOp, z);
        graph.bindOutputPort("z", z);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "prob_coarsen_top";
        options.partitionPolicy = "prob";
        options.maxOpInComputeNode = 1;
        options.maxOpInComputeSupernode = 8;
        options.enableChainMerge = false;
        options.phiMin = 0.0;
        options.footprintMaxBytes = 1024;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected probability coarsen sibling merge schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "prob_coarsen_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const auto *stats =
            getSessionValue<std::string>(session, "prob_coarsen_top.activity_schedule.prob_coarsen_stats");
        if (stats == nullptr)
        {
            return fail("Expected probability coarsen stats session value");
        }
        if (parseStatField(*stats, "merges") == 0)
        {
            return fail("Expected probability coarsen to merge sibling candidates");
        }
    }

    {
        currentCase = "Phase E probability coarsen respects footprint cap";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("prob_coarsen_footprint_top");
        design.markAsTop("prob_coarsen_footprint_top");
        using K = wolvrix::lib::grh::OperationKind;

        const auto a = makeValue(graph, "prob_fp_a", 8);
        const auto b = makeValue(graph, "prob_fp_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto shared = makeValue(graph, "prob_fp_shared", 8);
        const auto sharedOp = graph.createOperation(K::kXor, graph.internSymbol("prob_fp_shared_op"));
        graph.addOperand(sharedOp, a);
        graph.addOperand(sharedOp, b);
        graph.addResult(sharedOp, shared);

        const auto y = makeValue(graph, "prob_fp_y", 8);
        const auto yOp = graph.createOperation(K::kXor, graph.internSymbol("prob_fp_y_op"));
        graph.addOperand(yOp, shared);
        graph.addOperand(yOp, a);
        graph.addResult(yOp, y);
        graph.bindOutputPort("y", y);

        const auto z = makeValue(graph, "prob_fp_z", 8);
        const auto zOp = graph.createOperation(K::kXor, graph.internSymbol("prob_fp_z_op"));
        graph.addOperand(zOp, shared);
        graph.addOperand(zOp, b);
        graph.addResult(zOp, z);
        graph.bindOutputPort("z", z);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "prob_coarsen_footprint_top";
        options.partitionPolicy = "prob";
        options.maxOpInComputeNode = 1;
        options.maxOpInComputeSupernode = 8;
        options.enableChainMerge = false;
        options.phiMin = 0.0;
        options.footprintMaxBytes = 1;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected probability coarsen footprint cap schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "prob_coarsen_footprint_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const auto *stats = getSessionValue<std::string>(
            session,
            "prob_coarsen_footprint_top.activity_schedule.prob_coarsen_stats");
        if (stats == nullptr)
        {
            return fail("Expected probability coarsen footprint stats session value");
        }
        if (parseStatField(*stats, "merges") != 0)
        {
            return fail("Expected footprint cap to block probability coarsen merge");
        }
        if (parseStatField(*stats, "reject_footprint") == 0)
        {
            return fail("Expected probability coarsen to report footprint rejection");
        }
    }

    return 0;
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception in ") + currentCase + ": " + ex.what());
    }
}
