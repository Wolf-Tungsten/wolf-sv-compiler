#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
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

    int validateCommonScheduleShape(const wolvrix::lib::grh::Graph &graph,
                                    const ScheduleView &schedule)
    {
        if (schedule.supernodeToOps == nullptr || schedule.opToSupernode == nullptr ||
            schedule.dag == nullptr || schedule.valueFanout == nullptr ||
            schedule.topoOrder == nullptr || schedule.stateReadSupernodes == nullptr ||
            schedule.supernodeKinds == nullptr || schedule.summaryStats == nullptr)
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

} // namespace

int main()
{
    std::string currentCase;
    try
    {
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
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "top", .maxOpInComputeSupernode = 4}));

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
            return fail("Expected shared expression to be owned by its earliest consumer");
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

    return 0;
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception in ") + currentCase + ": " + ex.what());
    }
}
