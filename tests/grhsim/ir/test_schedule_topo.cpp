#include "grhsim/ir.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using namespace wolvrix::lib::grhsim;

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim-schedule-topo] " << message << '\n';
        return 1;
    }

    EdgeId addAssign(Module &module, OpId op, TypeId type, EdgeId operand)
    {
        const std::array<EdgeId, 1> operands{operand};
        module.setOperands(op, operands);
        return module.addResult(op, type);
    }

    std::vector<std::string> orderNames(const Module &module)
    {
        std::vector<std::string> names;
        for (OpId op : module.linearize())
        {
            names.emplace_back(module.symbol(module.opSymbol(op)));
        }
        return names;
    }

    int testDagOrderAndIdempotence()
    {
        Module module("dag");
        const TypeId logic1 = module.internLogicType(1, false);
        const OpId sink = module.createOp(genericOp(GenericOpcode::Assign), module.intern("sink"));
        const OpId middle =
            module.createOp(genericOp(GenericOpcode::Assign), module.intern("middle"));
        const OpId source =
            module.createOp(genericOp(GenericOpcode::Const), module.intern("source"));
        const EdgeId sourceEdge = module.addResult(source, logic1);
        module.setAttr(source, "value", module.intern("1'b1"));
        const EdgeId middleEdge = addAssign(module, middle, logic1, sourceEdge);
        (void)addAssign(module, sink, logic1, middleEdge);

        std::string error;
        auto pass = makeSimPass("schedule_topo", {}, error);
        if (!pass || !error.empty())
        {
            return fail("schedule-topo is not registered: " + error);
        }
        wolvrix::lib::transform::PassDiagnostics diagnostics;
        SimPassManager manager;
        manager.addPass(std::move(pass));
        const SimPipelineResult first = manager.run(module, diagnostics);
        const std::vector<std::string> expected{"source", "middle", "sink"};
        if (!first.success || !first.changed || diagnostics.hasError() ||
            orderNames(module) != expected || !module.validate())
        {
            return fail("schedule-topo did not order a reverse-created DAG");
        }

        error.clear();
        SimPassManager secondManager;
        secondManager.addPass(makeSimPass("schedule-topo", {}, error));
        wolvrix::lib::transform::PassDiagnostics secondDiagnostics;
        const SimPipelineResult second = secondManager.run(module, secondDiagnostics);
        if (!second.success || second.changed || secondDiagnostics.hasError())
        {
            return fail("schedule-topo is not idempotent");
        }
        return 0;
    }

    int testCycleOrder()
    {
        Module module("cycle");
        const TypeId logic1 = module.internLogicType(1, false);
        const OpId cycleHigh =
            module.createOp(genericOp(GenericOpcode::Assign), module.intern("cycle_high"));
        const OpId sink =
            module.createOp(genericOp(GenericOpcode::Assign), module.intern("sink"));
        const OpId cycleLow =
            module.createOp(genericOp(GenericOpcode::Assign), module.intern("cycle_low"));
        const EdgeId highEdge = module.addResult(cycleHigh, logic1);
        const EdgeId lowEdge = module.addResult(cycleLow, logic1);
        const std::array<EdgeId, 1> highOperands{lowEdge};
        const std::array<EdgeId, 1> lowOperands{highEdge};
        module.setOperands(cycleHigh, highOperands);
        module.setOperands(cycleLow, lowOperands);
        (void)addAssign(module, sink, logic1, highEdge);

        std::string error;
        SimPassManager manager;
        manager.addPass(makeSimPass("schedule-topo", {}, error));
        wolvrix::lib::transform::PassDiagnostics diagnostics;
        const SimPipelineResult result = manager.run(module, diagnostics);
        const std::vector<std::string> expected{"cycle_high", "cycle_low", "sink"};
        if (!result.success || diagnostics.hasError() || orderNames(module) != expected ||
            module.regions().size() != 1 || !module.validate())
        {
            return fail("schedule-topo did not keep an SCC contiguous and deterministic");
        }
        return 0;
    }

    int testSyntheticEventDependency()
    {
        Module module("synthetic_event_order");
        const TypeId logic1 = module.internLogicType(1, false);
        const StateId eventState = module.addState("derived$event", StateKind::Output, logic1);
        const StateId registerState = module.addState("q", StateKind::State, logic1);
        const OpId constant = module.createOp(genericOp(GenericOpcode::Const));
        const EdgeId one = module.addResult(constant, logic1);
        const OpId consumer = module.createOp(genericOp(GenericOpcode::RegWrite),
                                              module.intern("consumer"));
        const std::array<EdgeId, 3> consumerOperands{one, one, one};
        const OpId writer = module.createOp(genericOp(GenericOpcode::OutWrite),
                                            module.intern("event_writer"));
        const std::array<EdgeId, 1> writerOperands{one};
        if (!eventState.valid() || !registerState.valid() || !constant.valid() || !one.valid() ||
            !consumer.valid() || !writer.valid() || !module.setAttr(constant, "value",
                                                                     module.intern("1'b1")) ||
            !module.setOperands(consumer, consumerOperands) ||
            !module.setAttr(consumer, "state", module.state(registerState)->name) ||
            !module.setAttr(consumer, "events",
                            std::vector<SymbolId>{module.state(eventState)->name}) ||
            !module.setAttr(consumer, "eventEdge",
                            std::vector<SymbolId>{module.intern("posedge")}) ||
            !module.setOperands(writer, writerOperands) ||
            !module.setAttr(writer, "port", module.state(eventState)->name) ||
            !module.setAttr(writer, "eventState", true))
        {
            return fail("failed to construct synthetic event dependency fixture");
        }
        std::string error;
        auto pass = makeSimPass("schedule-topo", {}, error);
        if (!pass || !error.empty())
        {
            return fail("schedule-topo is not registered for synthetic event fixture: " + error);
        }
        SimPassManager manager;
        manager.addPass(std::move(pass));
        wolvrix::lib::transform::PassDiagnostics diagnostics;
        const SimPipelineResult result = manager.run(module, diagnostics);
        const auto order = module.linearize();
        const auto findPosition = [&](std::string_view name) {
            return std::find_if(order.begin(), order.end(), [&](OpId op) {
                return module.symbol(module.opSymbol(op)) == name;
            });
        };
        const auto writerPosition = findPosition("event_writer");
        const auto consumerPosition = findPosition("consumer");
        if (!result.success || diagnostics.hasError() || !module.validate() ||
            writerPosition == order.end() || consumerPosition == order.end() ||
            writerPosition >= consumerPosition)
        {
            return fail("schedule-topo did not place a synthetic event writer before its consumer");
        }
        return 0;
    }
} // namespace

int main()
{
    if (const int result = testDagOrderAndIdempotence(); result != 0)
    {
        return result;
    }
    if (const int result = testCycleOrder(); result != 0)
    {
        return result;
    }
    return testSyntheticEventDependency();
}
