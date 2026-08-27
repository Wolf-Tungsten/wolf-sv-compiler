#include "core/grh.hpp"
#include "grhsim/ir.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace wolvrix::lib;
    using namespace wolvrix::lib::grhsim;

    int fail(std::string_view message)
    {
        std::cerr << "[grhsim-ir-roundtrip] " << message << '\n';
        return 1;
    }

    std::string diagnosticsText(const diag::Diagnostics &diagnostics)
    {
        std::string result;
        for (const diag::Diagnostic &message : diagnostics.messages())
        {
            if (!result.empty())
            {
                result += "; ";
            }
            result += message.message;
            if (!message.context.empty())
            {
                result += " [" + message.context + "]";
            }
        }
        return result;
    }

    bool hasDiagnostic(const diag::Diagnostics &diagnostics, std::string_view needle)
    {
        return std::any_of(
            diagnostics.messages().begin(), diagnostics.messages().end(),
            [&](const diag::Diagnostic &message) {
                return message.message.find(needle) != std::string::npos;
            });
    }

    OpId findOp(const Module &module, std::string_view symbol)
    {
        for (OpId op : module.ops())
        {
            if (module.opSymbol(op).valid() && module.symbol(module.opSymbol(op)) == symbol)
            {
                return op;
            }
        }
        return OpId::invalid();
    }

    std::size_t countOpcode(const Module &module, GenericOpcode opcode)
    {
        return static_cast<std::size_t>(std::count_if(
            module.ops().begin(), module.ops().end(),
            [&](OpId op) { return module.kind(op) == genericOp(opcode); }));
    }

    bool replaceOnce(std::string &text, std::string_view from, std::string_view to)
    {
        const std::size_t offset = text.find(from);
        if (offset == std::string::npos)
        {
            return false;
        }
        text.replace(offset, from.size(), to);
        return true;
    }

    EdgeId irConstant(Module &module, TypeId type, std::string_view literal)
    {
        const OpId op = module.createOp(genericOp(GenericOpcode::Const));
        const EdgeId edge = module.addResult(op, type);
        if (!op.valid() || !edge.valid() ||
            !module.setAttr(op, "value", module.intern(literal)))
        {
            return EdgeId::invalid();
        }
        return edge;
    }

    grh::ValueId logic(grh::Graph &graph, std::string_view name, int32_t width,
                       bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(name), width, isSigned,
                                 grh::ValueType::Logic);
    }

    grh::ValueId constant(grh::Graph &graph, std::string_view opName,
                          std::string_view valueName, int32_t width,
                          std::string literal)
    {
        const grh::ValueId value = logic(graph, valueName, width);
        const grh::OperationId op = graph.createOperation(
            grh::OperationKind::kConstant, graph.internSymbol(opName));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    class InvalidateSchedulePass final : public SimPass
    {
    public:
        InvalidateSchedulePass()
            : SimPass("test-invalidate-schedule", "test-invalidate-schedule", "test pass",
                      SimPassEffects{.mutatesGraph = true, .preservesSchedule = false})
        {
        }

        SimPassResult run(Module &, SimPassContext &) override
        {
            return SimPassResult{.changed = true, .artifacts = {"schedule-invalidated"}};
        }
    };

    int testModuleStorageAndPasses()
    {
        Module module("storage_and_schedule");
        const TypeId logic1 = module.internLogicType(1, false);
        const TypeId logic8 = module.internLogicType(8, false);
        if (!logic1.valid() || !logic8.valid() ||
            module.internLogicType(8, false) != logic8)
        {
            return fail("generic Type interning failed");
        }
        const DialectRegistry &registry = dialectRegistry();
        const auto availableOps = registry.availableOps();
        const std::size_t genericOpcodeCount =
            static_cast<std::size_t>(GenericOpcode::HostCall) + 1U;
        if (availableOps.size() != genericOpcodeCount)
        {
            return fail("generic dialect registry does not contain every opcode");
        }
        for (std::size_t raw = 0; raw < genericOpcodeCount; ++raw)
        {
            const GenericOpcode opcode = static_cast<GenericOpcode>(raw);
            const std::string qualified = "generic." + std::string(genericOpcodeName(opcode));
            if (genericOpcodeName(opcode).empty() ||
                registry.find(qualified) != genericOp(opcode) ||
                registry.opName(genericOp(opcode)) != qualified)
            {
                return fail("generic dialect opcode name registration is incomplete");
            }
        }

        const std::array<AttrKV, 1> initAttrs{
            AttrKV{module.intern("value"), module.intern("8'h2a")},
        };
        const StateId clock = module.addState("clk", StateKind::Input, logic1);
        const StateId state = module.addState("q", StateKind::State, logic8, initAttrs);
        const StateId output = module.addState("out", StateKind::Output, logic8);
        const std::array<uint32_t, 2> backendParameters{8, 4};
        const TypeId backend = module.internBackendType(1, 7, backendParameters, logic8);
        if (!clock.valid() || !state.valid() || !output.valid() || !backend.valid() ||
            !module.setBackendType(state, backend))
        {
            return fail("StateDecl or backend Type construction failed");
        }

        const std::array<HostParam, 2> signature{
            HostParam{module.intern("arg"), logic8, HostParamDirection::Input},
            HostParam{module.intern("result"), logic8, HostParamDirection::Return},
        };
        const std::array<AttrKV, 1> hostAttrs{
            AttrKV{module.intern("pure"), true},
        };
        if (!module.addHost("identity", HostKind::Query, signature,
                            "host_identity", hostAttrs)
                 .valid())
        {
            return fail("HostTable construction failed");
        }

        const auto makeConst = [&](std::string_view name, std::string_view literal) {
            const OpId op = module.createOp(genericOp(GenericOpcode::Const), module.intern(name));
            const EdgeId edge = module.addResult(op, logic8, module.intern(std::string(name) + ".v"));
            if (!op.valid() || !edge.valid() ||
                !module.setAttr(op, "value", module.intern(literal)))
            {
                return std::pair{OpId::invalid(), EdgeId::invalid()};
            }
            return std::pair{op, edge};
        };

        const auto [constA, edgeA] = makeConst("const_a", "8'h11");
        const auto [constB, edgeB] = makeConst("const_b", "8'h22");
        const auto [deadConst, deadEdge] = makeConst("dead", "8'hff");
        const OpId add = module.createOp(genericOp(GenericOpcode::Add), module.intern("add"));
        const std::array<EdgeId, 2> addOperands{edgeA, edgeB};
        const EdgeId sum = module.addResult(add, logic8, module.intern("sum"));
        const OpId write = module.createOp(genericOp(GenericOpcode::OutWrite),
                                           module.intern("write"));
        const std::array<EdgeId, 1> writeOperands{sum};
        if (!constA.valid() || !constB.valid() || !deadConst.valid() || !deadEdge.valid() ||
            !add.valid() || !sum.valid() || !write.valid() ||
            !module.setOperands(add, addOperands) ||
            !module.setOperands(write, writeOperands) ||
            !module.setAttr(write, "port", module.state(output)->name))
        {
            return fail("generic graph construction failed");
        }
        if (module.users(sum).size() != 1 || !module.replaceAllUses(sum, edgeA) ||
            !module.users(sum).empty() || module.users(edgeA).size() != 2 ||
            !module.eraseOp(add) || !module.eraseOp(deadConst) || !module.hasTombstones())
        {
            return fail("use replacement or tombstone mutation failed");
        }

        module.compact();
        if (module.hasTombstones() || module.opCount() != 3 || module.edgeCount() != 2)
        {
            return fail("compact did not reclaim tombstones");
        }
        const OpId compactA = findOp(module, "const_a");
        const OpId compactB = findOp(module, "const_b");
        const OpId compactWrite = findOp(module, "write");
        if (!compactA.valid() || !compactB.valid() || !compactWrite.valid() ||
            module.operands(compactWrite).size() != 1 ||
            module.def(module.operands(compactWrite)[0]) != compactA)
        {
            return fail("compact ID remapping is inconsistent");
        }

        const RegionId compute = module.createRegion(
            Activation{.kind = ActivationKind::Posedge, .state = clock});
        const RegionId commit = module.createRegion();
        const std::array<OpId, 2> computeOps{compactB, compactA};
        const std::array<OpId, 1> commitOps{compactWrite};
        if (!compute.valid() || !commit.valid() ||
            !module.setRegion(computeOps, compute) || !module.setRegion(commitOps, commit) ||
            !module.setRegionOrder(compute, computeOps) ||
            !module.setRegionOrder(commit, commitOps) ||
            !module.addRegionDep(compute, commit))
        {
            return fail("Schedule construction failed");
        }
        if (module.mergeRegions(compute, commit))
        {
            return fail("mergeRegions accepted incompatible activation conditions");
        }
        diag::Diagnostics validation;
        if (!module.validate(validation))
        {
            std::cerr << diagnosticsText(validation) << '\n';
            return fail("valid scheduled module was rejected");
        }
        const std::vector<OpId> linear = module.linearize();
        const std::vector<StateId> trackSet = module.deriveTrackSet();
        if (linear.size() != module.opCount() || trackSet != std::vector<StateId>{clock})
        {
            return fail("Schedule linearization or TrackSet derivation failed");
        }

        module.freeze();
        if (!module.frozen())
        {
            return fail("freeze did not enter read-only mode");
        }
        if (module.ops().empty() ||
            module.symbol(module.opSymbol(module.ops().front())) != "const_b")
        {
            return fail("freeze did not compact operations in Schedule order");
        }
        module.intern("new_after_freeze");
        if (module.frozen())
        {
            return fail("symbol interning did not thaw the module");
        }
        module.freeze();
        if (!module.setAttr(findOp(module, "const_a"), "value", module.intern("8'h11")) ||
            module.frozen())
        {
            return fail("a mutation did not automatically thaw the module");
        }
        module.freeze();

        const std::string json = storeJson(module, true);
        Module restored = loadJson(json);
        if (!restored.frozen() || !structurallyEquivalent(module, restored) ||
            restored.states().size() != 3 || restored.hosts().size() != 1 ||
            restored.regions().size() != 2 || restored.linearize().size() != 3 ||
            restored.stateInitAttrs(state).size() != 1 ||
            restored.hostSignature(HostId{0}).size() != 2 ||
            restored.hostAttrs(HostId{0}).size() != 1)
        {
            return fail("GRHSIM JSON round trip lost structure");
        }

        const auto passes = availableSimPasses();
        std::string error;
        auto validatePass = makeSimPass("analyze_validate", {}, error);
        transform::PassDiagnostics passDiagnostics;
        SimPassManager manager;
        manager.addPass(std::move(validatePass));
        restored.setName(restored.name());
        if (restored.frozen())
        {
            return fail("test setup did not thaw the restored module");
        }
        const SimPipelineResult validationResult = manager.run(restored, passDiagnostics);
        if (passes != std::vector<std::string>{"analyze-validate", "schedule-topo"} ||
            !error.empty() || !validationResult.success || validationResult.changed ||
            passDiagnostics.hasError() || !restored.frozen())
        {
            return fail("SimPass registry or analyze-validate execution failed");
        }

        SimPassManager invalidatingManager;
        invalidatingManager.addPass(std::make_unique<InvalidateSchedulePass>());
        transform::PassDiagnostics invalidatingDiagnostics;
        const SimPipelineResult invalidatingResult =
            invalidatingManager.run(restored, invalidatingDiagnostics);
        if (!invalidatingResult.success || !invalidatingResult.changed || restored.hasSchedule() ||
            invalidatingResult.artifacts != std::vector<std::string>{"schedule-invalidated"})
        {
            return fail("SimPass Schedule invalidation contract failed");
        }
        return 0;
    }

    int testLoweringRoundTrip()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("phase0_top");
        design.markAsTop(graph.symbol());

        const grh::ValueId clk = logic(graph, "clk", 1);
        const grh::ValueId enable = logic(graph, "enable", 1);
        const grh::ValueId input = logic(graph, "input", 8);
        const grh::ValueId address = logic(graph, "address", 2);
        const grh::ValueId laneMask = logic(graph, "lane_mask", 4);
        const grh::ValueId image = logic(graph, "image", 32);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("enable", enable);
        graph.bindInputPort("input", input);
        graph.bindInputPort("address", address);
        graph.bindInputPort("lane_mask", laneMask);
        graph.bindInputPort("image", image);

        const grh::ValueId one = constant(graph, "one_op", "one", 8, "8'h1");
        const grh::ValueId mask = constant(graph, "mask_op", "mask", 8, "8'hff");
        const grh::ValueId ioOe = constant(graph, "io_oe_op", "io_oe", 1, "1'b1");
        const grh::ValueId sum = logic(graph, "sum", 8);
        const grh::OperationId add = graph.createOperation(
            grh::OperationKind::kAdd, graph.internSymbol("add"));
        graph.addOperand(add, input);
        graph.addOperand(add, one);
        graph.addResult(add, sum);

        const grh::OperationId reg = graph.createOperation(
            grh::OperationKind::kRegister, graph.internSymbol("q"));
        graph.setAttr(reg, "width", int64_t{8});
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h12"));
        const grh::ValueId q = logic(graph, "q_read", 8);
        const grh::OperationId regRead = graph.createOperation(
            grh::OperationKind::kRegisterReadPort, graph.internSymbol("q_read_op"));
        graph.addResult(regRead, q);
        graph.setAttr(regRead, "regSymbol", std::string("q"));
        const grh::OperationId regWrite = graph.createOperation(
            grh::OperationKind::kRegisterWritePort, graph.internSymbol("q_write"));
        graph.addOperand(regWrite, enable);
        graph.addOperand(regWrite, sum);
        graph.addOperand(regWrite, mask);
        graph.addOperand(regWrite, clk);
        graph.setAttr(regWrite, "regSymbol", std::string("q"));
        graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});

        const grh::OperationId memory = graph.createOperation(
            grh::OperationKind::kMemory, graph.internSymbol("mem"));
        graph.setAttr(memory, "width", int64_t{8});
        graph.setAttr(memory, "row", int64_t{4});
        graph.setAttr(memory, "isSigned", false);
        const grh::ValueId memoryData = logic(graph, "mem_data", 8);
        const grh::OperationId memoryRead = graph.createOperation(
            grh::OperationKind::kMemoryReadPort, graph.internSymbol("mem_read"));
        graph.addOperand(memoryRead, address);
        graph.addResult(memoryRead, memoryData);
        graph.setAttr(memoryRead, "memSymbol", std::string("mem"));
        const grh::OperationId memoryWrite = graph.createOperation(
            grh::OperationKind::kMemoryWritePort, graph.internSymbol("mem_write"));
        graph.addOperand(memoryWrite, enable);
        graph.addOperand(memoryWrite, address);
        graph.addOperand(memoryWrite, input);
        graph.addOperand(memoryWrite, mask);
        graph.addOperand(memoryWrite, clk);
        graph.setAttr(memoryWrite, "memSymbol", std::string("mem"));
        graph.setAttr(memoryWrite, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(memoryWrite, "memoryWrite.priorityGroup", std::string("writes"));
        graph.setAttr(memoryWrite, "memoryWrite.priority", int64_t{0});

        const grh::ValueId memoryImage = logic(graph, "memory_image", 32);
        const grh::OperationId memoryReadAll = graph.createOperation(
            grh::OperationKind::kMemoryReadAllPort, graph.internSymbol("mem_read_all"));
        graph.addResult(memoryReadAll, memoryImage);
        graph.setAttr(memoryReadAll, "memSymbol", std::string("mem"));
        const grh::OperationId memoryWriteLanes = graph.createOperation(
            grh::OperationKind::kMemoryWriteLanesPort,
            graph.internSymbol("mem_write_lanes"));
        graph.addOperand(memoryWriteLanes, laneMask);
        graph.addOperand(memoryWriteLanes, image);
        graph.addOperand(memoryWriteLanes, clk);
        graph.setAttr(memoryWriteLanes, "memSymbol", std::string("mem"));
        graph.setAttr(memoryWriteLanes, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(memoryWriteLanes, "memoryWrite.priorityGroup", std::string("writes"));
        graph.setAttr(memoryWriteLanes, "memoryWrite.priority", int64_t{1});
        const grh::OperationId memoryFill = graph.createOperation(
            grh::OperationKind::kMemoryFillPort, graph.internSymbol("mem_fill"));
        graph.addOperand(memoryFill, enable);
        graph.addOperand(memoryFill, image);
        graph.addOperand(memoryFill, clk);
        graph.setAttr(memoryFill, "memSymbol", std::string("mem"));
        graph.setAttr(memoryFill, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(memoryFill, "memoryWrite.priorityGroup", std::string("writes"));
        graph.setAttr(memoryFill, "memoryWrite.priority", int64_t{2});

        const grh::OperationId latch = graph.createOperation(
            grh::OperationKind::kLatch, graph.internSymbol("hold"));
        graph.setAttr(latch, "width", int64_t{8});
        graph.setAttr(latch, "isSigned", false);
        const grh::ValueId hold = logic(graph, "hold_read", 8);
        const grh::OperationId latchRead = graph.createOperation(
            grh::OperationKind::kLatchReadPort, graph.internSymbol("hold_read_op"));
        graph.addResult(latchRead, hold);
        graph.setAttr(latchRead, "latchSymbol", std::string("hold"));
        const grh::OperationId latchWrite = graph.createOperation(
            grh::OperationKind::kLatchWritePort, graph.internSymbol("hold_write"));
        graph.addOperand(latchWrite, enable);
        graph.addOperand(latchWrite, input);
        graph.addOperand(latchWrite, mask);
        graph.setAttr(latchWrite, "latchSymbol", std::string("hold"));

        const grh::ValueId internalEvent = logic(graph, "internal_event", 1);
        const grh::OperationId internalEventOp = graph.createOperation(
            grh::OperationKind::kLogicNot, graph.internSymbol("internal_event_op"));
        graph.addOperand(internalEventOp, enable);
        graph.addResult(internalEventOp, internalEvent);
        const grh::OperationId eventRegister = graph.createOperation(
            grh::OperationKind::kRegister, graph.internSymbol("event_q"));
        graph.setAttr(eventRegister, "width", int64_t{8});
        graph.setAttr(eventRegister, "isSigned", false);
        const grh::OperationId eventWrite = graph.createOperation(
            grh::OperationKind::kRegisterWritePort, graph.internSymbol("event_q_write"));
        graph.addOperand(eventWrite, enable);
        graph.addOperand(eventWrite, input);
        graph.addOperand(eventWrite, mask);
        graph.addOperand(eventWrite, internalEvent);
        graph.setAttr(eventWrite, "regSymbol", std::string("event_q"));
        graph.setAttr(eventWrite, "eventEdge", std::vector<std::string>{"posedge"});

        const grh::ValueId time = logic(graph, "time", 64);
        const grh::OperationId timeCall = graph.createOperation(
            grh::OperationKind::kSystemFunction, graph.internSymbol("time_call"));
        graph.addResult(timeCall, time);
        graph.setAttr(timeCall, "name", std::string("$time"));
        graph.setAttr(timeCall, "hasSideEffects", false);

        const grh::OperationId display = graph.createOperation(
            grh::OperationKind::kSystemTask, graph.internSymbol("display_clocked"));
        graph.addOperand(display, enable);
        graph.addOperand(display, input);
        graph.addOperand(display, clk);
        graph.setAttr(display, "name", std::string("$display"));
        graph.setAttr(display, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(display, "procKind", std::string("always_ff"));
        graph.setAttr(display, "hasTiming", false);
        const grh::OperationId displayInitial = graph.createOperation(
            grh::OperationKind::kSystemTask, graph.internSymbol("display_initial"));
        graph.addOperand(displayInitial, enable);
        graph.addOperand(displayInitial, input);
        graph.setAttr(displayInitial, "name", std::string("$display"));
        graph.setAttr(displayInitial, "eventEdge", std::vector<std::string>{});
        graph.setAttr(displayInitial, "procKind", std::string("initial"));
        graph.setAttr(displayInitial, "hasTiming", false);

        const grh::OperationId dpiImport = graph.createOperation(
            grh::OperationKind::kDpicImport, graph.internSymbol("dpi_echo"));
        graph.setAttr(dpiImport, "argsDirection",
                      std::vector<std::string>{"input", "output"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8, 8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"x", "y"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic", "logic"});
        graph.setAttr(dpiImport, "hasReturn", false);
        const grh::ValueId dpiOutput = logic(graph, "dpi_output", 8);
        const grh::OperationId dpiCall = graph.createOperation(
            grh::OperationKind::kDpicCall, graph.internSymbol("dpi_call"));
        graph.addOperand(dpiCall, enable);
        graph.addOperand(dpiCall, input);
        graph.addOperand(dpiCall, clk);
        graph.addResult(dpiCall, dpiOutput);
        graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_echo"));
        graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"x"});
        graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"y"});
        graph.setAttr(dpiCall, "inoutArgName", std::vector<std::string>{});
        graph.setAttr(dpiCall, "hasReturn", false);
        graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
        graph.bindOutputPort("q_out", q);
        graph.bindOutputPort("mem_out", memoryData);
        graph.bindOutputPort("mem_all_out", memoryImage);
        graph.bindOutputPort("hold_out", hold);
        graph.bindOutputPort("dpi_out", dpiOutput);
        const grh::ValueId ioIn = logic(graph, "io_in", 8);
        graph.bindInoutPort("io", ioIn, one, ioOe);
        graph.freeze();

        diag::Diagnostics diagnostics;
        auto lowered = lowerGrhToGrhsim(design, LowerGrhsimOptions{}, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("representative GRH graph did not lower");
        }
        const Module &module = *lowered;
        const StateEntry *clock = module.state(module.findState("clk"));
        const StateEntry *regState = module.state(module.findState("q"));
        const StateEntry *memoryState = module.state(module.findState("mem"));
        const HostEntry *timeHost = module.host(module.findHost("time"));
        const HostEntry *dpiHost = module.host(module.findHost("dpi_echo"));
        const HostEntry *displayHost = module.host(module.findHost("display"));
        const HostEntry *secondDisplayHost = module.host(module.findHost("display#1"));
        if (module.states().size() != 19 || module.hosts().size() != 4 ||
            !clock || clock->kind != StateKind::Input ||
            !regState || regState->kind != StateKind::State ||
            !memoryState || memoryState->kind != StateKind::State ||
            !timeHost || timeHost->kind != HostKind::Query ||
            !dpiHost || dpiHost->kind != HostKind::Effect ||
            !displayHost || displayHost->kind != HostKind::Effect ||
            !secondDisplayHost || secondDisplayHost->kind != HostKind::Effect ||
            countOpcode(module, GenericOpcode::InRead) != 7 ||
            countOpcode(module, GenericOpcode::Const) != 3 ||
            countOpcode(module, GenericOpcode::Add) != 1 ||
            countOpcode(module, GenericOpcode::LogicNot) != 1 ||
            countOpcode(module, GenericOpcode::RegRead) != 1 ||
            countOpcode(module, GenericOpcode::RegWrite) != 2 ||
            countOpcode(module, GenericOpcode::LatchRead) != 1 ||
            countOpcode(module, GenericOpcode::LatchWrite) != 1 ||
            countOpcode(module, GenericOpcode::MemRead) != 1 ||
            countOpcode(module, GenericOpcode::MemReadAll) != 1 ||
            countOpcode(module, GenericOpcode::MemWrite) != 1 ||
            countOpcode(module, GenericOpcode::MemWriteLanes) != 1 ||
            countOpcode(module, GenericOpcode::MemFill) != 1 ||
            countOpcode(module, GenericOpcode::HostCall) != 4 ||
            countOpcode(module, GenericOpcode::OutWrite) != 8)
        {
            return fail("lowered StateDecl, HostTable, or opcode mapping is incomplete");
        }
        const std::vector<StateId> trackSet = module.deriveTrackSet();
        std::vector<StateId> expectedTrackSet{
            module.findState("clk"),
            module.findState("internal_event$event"),
        };
        std::sort(expectedTrackSet.begin(), expectedTrackSet.end());
        if (trackSet != expectedTrackSet)
        {
            return fail("lowered event attributes did not derive the expected TrackSet");
        }
        Module restored = loadJson(storeJson(module, false));
        if (!structurallyEquivalent(module, restored))
        {
            return fail("lowered GRHSIM module failed its JSON round trip");
        }
        return 0;
    }

    int testComputeOpcodeLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("opcode_mapping");
        design.markAsTop(graph.symbol());

        const auto input = [&](std::string_view name, int32_t width) {
            const grh::ValueId value = logic(graph, name, width);
            graph.bindInputPort(name, value);
            return value;
        };
        const grh::ValueId bit = input("bit", 1);
        const grh::ValueId index = input("index", 2);
        const grh::ValueId lanes = input("lanes", 4);
        const grh::ValueId lhs = input("lhs", 8);
        const grh::ValueId rhs = input("rhs", 8);
        const grh::ValueId wide = input("wide", 32);

        std::vector<std::pair<grh::OperationKind, GenericOpcode>> expected;
        uint32_t ordinal = 0;
        const auto addMapped = [&](grh::OperationKind sourceKind, GenericOpcode targetOpcode,
                                   std::initializer_list<grh::ValueId> operands,
                                   int32_t resultWidth) {
            const std::string suffix = std::to_string(ordinal++);
            const grh::OperationId op = graph.createOperation(
                sourceKind, graph.internSymbol("mapped_op_" + suffix));
            for (grh::ValueId operand : operands)
            {
                graph.addOperand(op, operand);
            }
            const grh::ValueId result = logic(graph, "mapped_value_" + suffix, resultWidth);
            graph.addResult(op, result);
            expected.emplace_back(sourceKind, targetOpcode);
            return std::pair{op, result};
        };

        for (const auto [source, target] : std::array{
                 std::pair{grh::OperationKind::kAdd, GenericOpcode::Add},
                 std::pair{grh::OperationKind::kSub, GenericOpcode::Sub},
                 std::pair{grh::OperationKind::kDiv, GenericOpcode::Div},
                 std::pair{grh::OperationKind::kMod, GenericOpcode::Mod},
                 std::pair{grh::OperationKind::kAnd, GenericOpcode::And},
                 std::pair{grh::OperationKind::kOr, GenericOpcode::Or},
                 std::pair{grh::OperationKind::kXor, GenericOpcode::Xor},
                 std::pair{grh::OperationKind::kXnor, GenericOpcode::Xnor},
                 std::pair{grh::OperationKind::kShl, GenericOpcode::Shl},
                 std::pair{grh::OperationKind::kLShr, GenericOpcode::LShr},
                 std::pair{grh::OperationKind::kAShr, GenericOpcode::AShr},
             })
        {
            addMapped(source, target, {lhs, rhs}, 8);
        }
        addMapped(grh::OperationKind::kMul, GenericOpcode::Mul, {lhs, rhs}, 16);
        for (const auto [source, target] : std::array{
                 std::pair{grh::OperationKind::kLt, GenericOpcode::Lt},
                 std::pair{grh::OperationKind::kLe, GenericOpcode::Le},
                 std::pair{grh::OperationKind::kGt, GenericOpcode::Gt},
                 std::pair{grh::OperationKind::kGe, GenericOpcode::Ge},
                 std::pair{grh::OperationKind::kEq, GenericOpcode::Eq},
                 std::pair{grh::OperationKind::kNe, GenericOpcode::Ne},
                 std::pair{grh::OperationKind::kCaseEq, GenericOpcode::CaseEq},
                 std::pair{grh::OperationKind::kCaseNe, GenericOpcode::CaseNe},
                 std::pair{grh::OperationKind::kWildcardEq, GenericOpcode::WildEq},
                 std::pair{grh::OperationKind::kWildcardNe, GenericOpcode::WildNe},
                 std::pair{grh::OperationKind::kLogicAnd, GenericOpcode::LogicAnd},
                 std::pair{grh::OperationKind::kLogicOr, GenericOpcode::LogicOr},
             })
        {
            addMapped(source, target, {lhs, rhs}, 1);
        }
        addMapped(grh::OperationKind::kNot, GenericOpcode::Not, {lhs}, 8);
        for (const auto [source, target] : std::array{
                 std::pair{grh::OperationKind::kLogicNot, GenericOpcode::LogicNot},
                 std::pair{grh::OperationKind::kReduceAnd, GenericOpcode::ReduceAnd},
                 std::pair{grh::OperationKind::kReduceNand, GenericOpcode::ReduceNand},
                 std::pair{grh::OperationKind::kReduceOr, GenericOpcode::ReduceOr},
                 std::pair{grh::OperationKind::kReduceNor, GenericOpcode::ReduceNor},
                 std::pair{grh::OperationKind::kReduceXor, GenericOpcode::ReduceXor},
                 std::pair{grh::OperationKind::kReduceXnor, GenericOpcode::ReduceXnor},
             })
        {
            addMapped(source, target, {lhs}, 1);
        }
        addMapped(grh::OperationKind::kAssign, GenericOpcode::Assign, {lhs}, 8);
        addMapped(grh::OperationKind::kMux, GenericOpcode::Mux, {bit, lhs, rhs}, 8);
        addMapped(grh::OperationKind::kConcat, GenericOpcode::Concat, {lhs, rhs}, 16);

        const auto [replicate, replicated] =
            addMapped(grh::OperationKind::kReplicate, GenericOpcode::Replicate, {lhs}, 16);
        (void)replicated;
        graph.setAttr(replicate, "rep", int64_t{2});
        const auto [sliceStatic, staticSlice] =
            addMapped(grh::OperationKind::kSliceStatic, GenericOpcode::SliceStatic, {lhs}, 4);
        (void)staticSlice;
        graph.setAttr(sliceStatic, "sliceStart", int64_t{2});
        graph.setAttr(sliceStatic, "sliceEnd", int64_t{5});
        const auto [sliceDynamic, dynamicSlice] = addMapped(
            grh::OperationKind::kSliceDynamic, GenericOpcode::SliceDynamic, {lhs, index}, 4);
        (void)dynamicSlice;
        graph.setAttr(sliceDynamic, "sliceWidth", int64_t{4});
        const auto [sliceArray, arraySlice] = addMapped(
            grh::OperationKind::kSliceArray, GenericOpcode::SliceArray, {wide, index}, 8);
        (void)arraySlice;
        graph.setAttr(sliceArray, "sliceWidth", int64_t{8});

        const auto [laneConst, laneConstValue] = addMapped(
            grh::OperationKind::kArrayLaneConst, GenericOpcode::ArrayLaneConst, {}, 32);
        (void)laneConstValue;
        graph.setAttr(laneConst, "elemWidth", int64_t{8});
        graph.setAttr(laneConst, "rows", int64_t{4});
        graph.setAttr(laneConst, "values", std::vector<int64_t>{1, 2, 3, 4});
        addMapped(grh::OperationKind::kArrayMux, GenericOpcode::ArrayMux,
                  {lanes, wide, wide}, 32);
        const auto [onehot, onehotValue] = addMapped(
            grh::OperationKind::kArrayOnehot, GenericOpcode::ArrayOnehot, {index}, 4);
        (void)onehotValue;
        graph.setAttr(onehot, "rows", int64_t{4});
        for (const auto [source, target] : std::array{
                 std::pair{grh::OperationKind::kArrayReduceOr, GenericOpcode::ArrayReduceOr},
                 std::pair{grh::OperationKind::kArrayReduceAnd, GenericOpcode::ArrayReduceAnd},
                 std::pair{grh::OperationKind::kArrayReduceXor, GenericOpcode::ArrayReduceXor},
             })
        {
            const auto [op, value] = addMapped(source, target, {wide}, 1);
            (void)value;
            graph.setAttr(op, "elemWidth", int64_t{8});
        }
        for (const auto [source, target] : std::array{
                 std::pair{grh::OperationKind::kArrayReduceLanesOr,
                           GenericOpcode::ArrayReduceLanesOr},
                 std::pair{grh::OperationKind::kArrayReduceLanesAnd,
                           GenericOpcode::ArrayReduceLanesAnd},
                 std::pair{grh::OperationKind::kArrayReduceLanesXor,
                           GenericOpcode::ArrayReduceLanesXor},
             })
        {
            const auto [op, value] = addMapped(source, target, {wide}, 4);
            (void)value;
            graph.setAttr(op, "elemWidth", int64_t{8});
        }
        const auto [broadcast, broadcastValue] = addMapped(
            grh::OperationKind::kArrayBroadcast, GenericOpcode::ArrayBroadcast, {lhs}, 32);
        (void)broadcastValue;
        graph.setAttr(broadcast, "rows", int64_t{4});
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(design, LowerGrhsimOptions{}, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("documented compute opcode mapping did not lower");
        }
        for (const auto &[source, target] : expected)
        {
            (void)source;
            if (countOpcode(*lowered, target) != 1)
            {
                return fail("documented GRH compute opcode mapping is incomplete");
            }
        }
        if (!structurallyEquivalent(*lowered, loadJson(storeJson(*lowered, false))))
        {
            return fail("complete compute opcode mapping did not survive JSON round trip");
        }
        return 0;
    }

    int testSourceLessValueLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("source_less_value");
        design.markAsTop(graph.symbol());

        const grh::ValueId bare = logic(graph, "bare_wire", 8);
        const grh::ValueId copied = logic(graph, "copied", 8);
        const grh::OperationId assign = graph.createOperation(
            grh::OperationKind::kAssign, graph.internSymbol("copy"));
        graph.addOperand(assign, bare);
        graph.addResult(assign, copied);
        graph.bindOutputPort("out", copied);
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("source-less GRH Value did not lower");
        }

        const Module &module = *lowered;
        OpId zero = OpId::invalid();
        for (OpId op : module.ops())
        {
            if (module.kind(op) != genericOp(GenericOpcode::Const))
            {
                continue;
            }
            const auto results = module.results(op);
            if (results.size() == 1 && module.edgeSymbol(results[0]).valid() &&
                module.symbol(module.edgeSymbol(results[0])) == "bare_wire")
            {
                zero = op;
                break;
            }
        }
        const OpId copy = findOp(module, "copy");
        const AttrValue *valueAttr = zero.valid() ? module.attr(zero, "value") : nullptr;
        const SymbolId *literal = valueAttr ? std::get_if<SymbolId>(valueAttr) : nullptr;
        if (!zero.valid() || !copy.valid() || !literal || module.symbol(*literal) != "8'b0" ||
            module.results(zero).size() != 1 || module.operands(copy).size() != 1 ||
            module.operands(copy)[0] != module.results(zero)[0])
        {
            return fail("source-less GRH Value was not materialized as a wired zero constant");
        }
        return 0;
    }

    int testTruncatedMultiplyLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("truncated_multiply");
        design.markAsTop(graph.symbol());

        const grh::ValueId lhs = logic(graph, "lhs", 32, true);
        const grh::ValueId rhs = logic(graph, "rhs", 32, true);
        const grh::ValueId product = logic(graph, "product", 32, true);
        graph.bindInputPort("lhs", lhs);
        graph.bindInputPort("rhs", rhs);
        graph.bindOutputPort("product", product);
        const grh::OperationId multiply = graph.createOperation(
            grh::OperationKind::kMul, graph.internSymbol("truncated_mul"));
        graph.addOperand(multiply, lhs);
        graph.addOperand(multiply, rhs);
        graph.addResult(multiply, product);
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(design, LowerGrhsimOptions{}, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("truncated multiply did not lower");
        }

        const OpId mul = findOp(*lowered, "truncated_mul");
        if (!mul.valid())
        {
            return fail("truncated multiply lost its native operation or result");
        }
        const auto mulResults = lowered->results(mul);
        if (mulResults.size() != 1)
        {
            return fail("truncated multiply lost its native operation or result");
        }
        const TypeRec *nativeType = lowered->type(lowered->edgeType(mulResults[0]));
        if (!nativeType || nativeType->width != 64 || !nativeType->isSigned)
        {
            return fail("truncated multiply did not preserve its full-width native result");
        }

        EdgeId adapted = EdgeId::invalid();
        for (OpId op : lowered->ops())
        {
            if (lowered->kind(op) != genericOp(GenericOpcode::Assign))
            {
                continue;
            }
            const auto operands = lowered->operands(op);
            const auto results = lowered->results(op);
            if (operands.size() == 1 && operands[0] == mulResults[0] && results.size() == 1)
            {
                adapted = results[0];
                break;
            }
        }
        if (!adapted.valid())
        {
            return fail("truncated multiply did not adapt its result to the GRH Type");
        }
        const TypeRec *adaptedType = lowered->type(lowered->edgeType(adapted));
        if (!adaptedType || adaptedType->width != 32 ||
            !adaptedType->isSigned)
        {
            return fail("truncated multiply did not adapt its result to the GRH Type");
        }
        return 0;
    }

    int testContextSizedMuxLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("context_sized_mux");
        design.markAsTop(graph.symbol());

        const grh::ValueId condition = logic(graph, "condition", 1);
        const grh::ValueId wide = logic(graph, "wide", 32);
        const grh::ValueId narrow = logic(graph, "narrow", 4);
        const grh::ValueId selected = logic(graph, "selected", 4);
        graph.bindInputPort("condition", condition);
        graph.bindInputPort("wide", wide);
        graph.bindInputPort("narrow", narrow);
        graph.bindOutputPort("selected", selected);
        const grh::OperationId mux = graph.createOperation(
            grh::OperationKind::kMux, graph.internSymbol("context_mux"));
        graph.addOperand(mux, condition);
        graph.addOperand(mux, wide);
        graph.addOperand(mux, narrow);
        graph.addResult(mux, selected);
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(design, LowerGrhsimOptions{}, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("context-sized mux did not lower");
        }
        const OpId loweredMux = findOp(*lowered, "context_mux");
        if (!loweredMux.valid())
        {
            return fail("context-sized mux lost its native operation");
        }
        const auto operands = lowered->operands(loweredMux);
        const auto results = lowered->results(loweredMux);
        if (operands.size() != 3 || results.size() != 1)
        {
            return fail("context-sized mux has an invalid lowered shape");
        }
        for (EdgeId edge : {operands[1], operands[2], results[0]})
        {
            const TypeRec *type = lowered->type(lowered->edgeType(edge));
            if (!type || type->width != 4)
            {
                return fail("context-sized mux data was not adapted to its result width");
            }
        }
        return 0;
    }

    int testWideEffectConditionLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("wide_effect_condition");
        design.markAsTop(graph.symbol());

        const grh::ValueId condition = logic(graph, "condition", 8);
        graph.bindInputPort("condition", condition);
        const grh::OperationId task = graph.createOperation(
            grh::OperationKind::kSystemTask, graph.internSymbol("wide_condition_task"));
        graph.addOperand(task, condition);
        graph.setAttr(task, "name", std::string("$display"));
        graph.setAttr(task, "eventEdge", std::vector<std::string>{});
        graph.setAttr(task, "procKind", std::string("initial"));
        graph.setAttr(task, "hasTiming", false);
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("wide host effect condition did not lower");
        }

        const OpId hostCall = findOp(*lowered, "wide_condition_task");
        const auto hostOperands = hostCall.valid() ? lowered->operands(hostCall)
                                                   : std::span<const EdgeId>{};
        const OpId compare = hostOperands.size() == 1
                                 ? lowered->def(hostOperands[0])
                                 : OpId::invalid();
        const auto compareOperands = compare.valid() ? lowered->operands(compare)
                                                     : std::span<const EdgeId>{};
        const TypeRec *conditionType = hostOperands.size() == 1
                                           ? lowered->type(lowered->edgeType(hostOperands[0]))
                                           : nullptr;
        if (!hostCall.valid() || lowered->kind(hostCall) != genericOp(GenericOpcode::HostCall) ||
            hostOperands.size() != 1 || !conditionType || conditionType->width != 1 ||
            !compare.valid() || lowered->kind(compare) != genericOp(GenericOpcode::Ne) ||
            compareOperands.size() != 2)
        {
            return fail("wide host effect condition was not reduced through generic.ne");
        }
        const OpId zero = lowered->def(compareOperands[1]);
        const AttrValue *valueAttr = zero.valid() ? lowered->attr(zero, "value") : nullptr;
        const SymbolId *literal = valueAttr ? std::get_if<SymbolId>(valueAttr) : nullptr;
        const TypeRec *sourceType = lowered->type(lowered->edgeType(compareOperands[0]));
        if (!zero.valid() || lowered->kind(zero) != genericOp(GenericOpcode::Const) ||
            !literal || lowered->symbol(*literal) != "8'b0" || !sourceType ||
            sourceType->width != 8)
        {
            return fail("wide host effect condition reduction lost its typed zero operand");
        }
        return 0;
    }

    int testContextSizedBitwiseLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("context_sized_bitwise");
        design.markAsTop(graph.symbol());

        const grh::ValueId lhs = logic(graph, "lhs", 8);
        const grh::ValueId rhs = logic(graph, "rhs", 32);
        const grh::ValueId result = logic(graph, "result", 16);
        graph.bindInputPort("lhs", lhs);
        graph.bindInputPort("rhs", rhs);
        graph.bindOutputPort("result", result);
        const grh::OperationId bitwise = graph.createOperation(
            grh::OperationKind::kAnd, graph.internSymbol("context_and"));
        graph.addOperand(bitwise, lhs);
        graph.addOperand(bitwise, rhs);
        graph.addResult(bitwise, result);
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("context-sized bitwise operation did not lower");
        }

        const OpId native = findOp(*lowered, "context_and");
        const auto nativeResults = native.valid() ? lowered->results(native)
                                                  : std::span<const EdgeId>{};
        const TypeRec *nativeType = nativeResults.size() == 1
                                        ? lowered->type(lowered->edgeType(nativeResults[0]))
                                        : nullptr;
        EdgeId adapted = EdgeId::invalid();
        for (OpId op : lowered->ops())
        {
            if (lowered->kind(op) != genericOp(GenericOpcode::Assign))
            {
                continue;
            }
            const auto operands = lowered->operands(op);
            const auto results = lowered->results(op);
            if (nativeResults.size() == 1 && operands.size() == 1 &&
                operands[0] == nativeResults[0] && results.size() == 1)
            {
                adapted = results[0];
                break;
            }
        }
        const TypeRec *adaptedType = adapted.valid()
                                         ? lowered->type(lowered->edgeType(adapted))
                                         : nullptr;
        if (!native.valid() || lowered->kind(native) != genericOp(GenericOpcode::And) ||
            !nativeType || nativeType->width != 32 || !adaptedType ||
            adaptedType->width != 16)
        {
            return fail("bitwise lowering did not preserve native width and context conversion");
        }
        return 0;
    }

    int testMemoryFillAndSynthesizedPriorities()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("memory_write_contracts");
        design.markAsTop(graph.symbol());

        const grh::ValueId condition = logic(graph, "condition", 1);
        const grh::ValueId address = logic(graph, "address", 2);
        const grh::ValueId data = logic(graph, "data", 8);
        const grh::ValueId mask = logic(graph, "mask", 8);
        const grh::ValueId clock = logic(graph, "clock", 1);
        graph.bindInputPort("condition", condition);
        graph.bindInputPort("address", address);
        graph.bindInputPort("data", data);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("clock", clock);

        const grh::OperationId memory = graph.createOperation(
            grh::OperationKind::kMemory, graph.internSymbol("mem"));
        graph.setAttr(memory, "width", int64_t{8});
        graph.setAttr(memory, "row", int64_t{4});
        graph.setAttr(memory, "isSigned", false);
        const auto addWrite = [&](std::string_view name) {
            const grh::OperationId write = graph.createOperation(
                grh::OperationKind::kMemoryWritePort, graph.internSymbol(name));
            for (grh::ValueId operand : {condition, address, data, mask, clock})
            {
                graph.addOperand(write, operand);
            }
            graph.setAttr(write, "memSymbol", std::string("mem"));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            return write;
        };
        const grh::OperationId firstWrite = addWrite("first_write");
        const grh::OperationId secondWrite = addWrite("second_write");
        const grh::OperationId fill = graph.createOperation(
            grh::OperationKind::kMemoryFillPort, graph.internSymbol("element_fill"));
        graph.addOperand(fill, condition);
        graph.addOperand(fill, data);
        graph.addOperand(fill, clock);
        graph.setAttr(fill, "memSymbol", std::string("mem"));
        graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});
        if (!firstWrite.valid() || !secondWrite.valid() || !fill.valid())
        {
            return fail("failed to construct memory lowering fixture");
        }
        graph.freeze();

        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            std::cerr << diagnosticsText(diagnostics) << '\n';
            return fail("memory fill or synthesized priorities did not lower");
        }

        const OpId loweredFill = findOp(*lowered, "element_fill");
        const auto fillOperands = loweredFill.valid() ? lowered->operands(loweredFill)
                                                     : std::span<const EdgeId>{};
        const OpId broadcast = fillOperands.size() == 2
                                   ? lowered->def(fillOperands[1])
                                   : OpId::invalid();
        const auto broadcastResults = broadcast.valid() ? lowered->results(broadcast)
                                                        : std::span<const EdgeId>{};
        const TypeRec *broadcastType = broadcastResults.size() == 1
                                           ? lowered->type(lowered->edgeType(broadcastResults[0]))
                                           : nullptr;
        if (!loweredFill.valid() || lowered->kind(loweredFill) != genericOp(GenericOpcode::MemFill) ||
            !broadcast.valid() ||
            lowered->kind(broadcast) != genericOp(GenericOpcode::ArrayBroadcast) ||
            !broadcastType || broadcastType->width != 32)
        {
            return fail("element-width memory fill was not broadcast to the packed image");
        }

        const std::array<std::pair<std::string_view, int64_t>, 3> expected{{
            {"first_write", 2},
            {"second_write", 1},
            {"element_fill", 0},
        }};
        SymbolId group = SymbolId::invalid();
        for (const auto &[name, expectedPriority] : expected)
        {
            const OpId op = findOp(*lowered, name);
            const AttrValue *groupAttr = op.valid()
                                             ? lowered->attr(op, "memoryWrite.priorityGroup")
                                             : nullptr;
            const AttrValue *priorityAttr = op.valid()
                                                ? lowered->attr(op, "memoryWrite.priority")
                                                : nullptr;
            const SymbolId *actualGroup = groupAttr ? std::get_if<SymbolId>(groupAttr) : nullptr;
            const int64_t *actualPriority = priorityAttr
                                                ? std::get_if<int64_t>(priorityAttr)
                                                : nullptr;
            if (!actualGroup || !actualPriority || *actualPriority != expectedPriority ||
                (group.valid() && *actualGroup != group))
            {
                return fail("memory writers did not receive one contiguous priority group");
            }
            group = *actualGroup;
        }
        return 0;
    }

    int testValidationRejectsInvalidContracts()
    {
        {
            Module module("missing_required_attribute");
            const TypeId logic1 = module.internLogicType(1, false);
            const OpId constantOp = module.createOp(genericOp(GenericOpcode::Const));
            module.addResult(constantOp, logic1);
            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            const bool found = std::any_of(
                diagnostics.messages().begin(), diagnostics.messages().end(),
                [](const diag::Diagnostic &message) {
                    return message.message.find("required attribute 'value' is missing") !=
                           std::string::npos;
                });
            if (!found)
            {
                return fail("generic schema validation accepted a const without value");
            }
        }
        {
            Module module("missing_output_writer");
            const TypeId logic8 = module.internLogicType(8, false);
            module.addState("out", StateKind::Output, logic8);
            diag::Diagnostics diagnostics;
            if (module.validate(diagnostics))
            {
                return fail("validation accepted an output without an out_write");
            }
        }
        {
            Module module("host_type_mismatch");
            const TypeId logic1 = module.internLogicType(1, false);
            const TypeId logic8 = module.internLogicType(8, false);
            const std::array<HostParam, 2> signature{
                HostParam{module.intern("arg"), logic8, HostParamDirection::Input},
                HostParam{module.intern("result"), logic8, HostParamDirection::Return},
            };
            module.addHost("identity", HostKind::Query, signature, "host_identity");
            const OpId constantOp = module.createOp(genericOp(GenericOpcode::Const));
            const EdgeId constantEdge = module.addResult(constantOp, logic1);
            module.setAttr(constantOp, "value", module.intern("1'b0"));
            const OpId call = module.createOp(genericOp(GenericOpcode::HostCall));
            const std::array<EdgeId, 1> operands{constantEdge};
            module.setOperands(call, operands);
            module.addResult(call, logic8);
            module.setAttr(call, "entry", module.intern("identity"));
            diag::Diagnostics diagnostics;
            if (module.validate(diagnostics))
            {
                return fail("validation accepted a host_call argument Type mismatch");
            }
        }
        {
            Module module("split_scc");
            const TypeId logic1 = module.internLogicType(1, false);
            const OpId first = module.createOp(genericOp(GenericOpcode::Assign));
            const OpId second = module.createOp(genericOp(GenericOpcode::Assign));
            const EdgeId firstEdge = module.addResult(first, logic1);
            const EdgeId secondEdge = module.addResult(second, logic1);
            const std::array<EdgeId, 1> firstOperands{secondEdge};
            const std::array<EdgeId, 1> secondOperands{firstEdge};
            module.setOperands(first, firstOperands);
            module.setOperands(second, secondOperands);
            const RegionId firstRegion = module.createRegion();
            const RegionId secondRegion = module.createRegion();
            const std::array<OpId, 1> firstOrder{first};
            const std::array<OpId, 1> secondOrder{second};
            module.setRegion(firstOrder, firstRegion);
            module.setRegion(secondOrder, secondRegion);
            module.setRegionOrder(firstRegion, firstOrder);
            module.setRegionOrder(secondRegion, secondOrder);
            module.addRegionDep(firstRegion, secondRegion);
            module.addRegionDep(secondRegion, firstRegion);
            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            const bool found = std::any_of(
                diagnostics.messages().begin(), diagnostics.messages().end(),
                [](const diag::Diagnostic &message) {
                    return message.message.find("strongly connected component") !=
                           std::string::npos;
                });
            if (!found)
            {
                return fail("validation did not diagnose an SCC split across regions");
            }
        }
        {
            Module module("reversed_region_order");
            const TypeId logic1 = module.internLogicType(1, false);
            const OpId constantOp = module.createOp(genericOp(GenericOpcode::Const));
            const EdgeId constantEdge = module.addResult(constantOp, logic1);
            module.setAttr(constantOp, "value", module.intern("1'b0"));
            const OpId assign = module.createOp(genericOp(GenericOpcode::Assign));
            const std::array<EdgeId, 1> operands{constantEdge};
            module.setOperands(assign, operands);
            module.addResult(assign, logic1);
            const RegionId region = module.createRegion();
            const std::array<OpId, 2> reversed{assign, constantOp};
            module.setRegion(reversed, region);
            module.setRegionOrder(region, reversed);
            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            const bool found = std::any_of(
                diagnostics.messages().begin(), diagnostics.messages().end(),
                [](const diag::Diagnostic &message) {
                    return message.message.find("operation order violates") != std::string::npos;
                });
            if (!found)
            {
                return fail("validation did not diagnose reversed region dependency order");
            }
        }
        {
            Module module("host_call_forms");
            const TypeId logic1 = module.internLogicType(1, false);
            const TypeId logic8 = module.internLogicType(8, false);
            const OpId condition = module.createOp(genericOp(GenericOpcode::Const));
            const EdgeId conditionEdge = module.addResult(condition, logic1);
            module.setAttr(condition, "value", module.intern("1'b1"));

            const std::array<HostParam, 1> querySignature{
                HostParam{module.intern("result"), logic8, HostParamDirection::Return},
            };
            module.addHost("query", HostKind::Query, querySignature, "host_query");
            const OpId query = module.createOp(genericOp(GenericOpcode::HostCall));
            module.addResult(query, logic8);
            module.setAttr(query, "entry", module.intern("query"));
            module.setAttr(query, "events", std::vector<SymbolId>{});
            module.setAttr(query, "eventEdge", std::vector<SymbolId>{});

            module.addHost("effect", HostKind::Effect, {}, "host_effect");
            const OpId effect = module.createOp(genericOp(GenericOpcode::HostCall));
            const std::array<EdgeId, 1> effectOperands{conditionEdge};
            module.setOperands(effect, effectOperands);
            module.setAttr(effect, "entry", module.intern("effect"));

            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            if (!hasDiagnostic(diagnostics, "query host_call must not carry event attributes") ||
                !hasDiagnostic(diagnostics, "effect host_call requires events"))
            {
                return fail("validation accepted an invalid query/effect host_call form");
            }
        }
        {
            Module module("host_signature_type");
            const TypeId logic8 = module.internLogicType(8, false);
            const std::array<uint32_t, 1> parameters{8};
            const TypeId backend = module.internBackendType(1, 0, parameters, logic8);
            const std::array<HostParam, 1> signature{
                HostParam{module.intern("arg"), backend, HostParamDirection::Input},
            };
            module.addHost("backend_arg", HostKind::Query, signature, "backend_arg");
            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            if (!hasDiagnostic(diagnostics, "non-array generic Type"))
            {
                return fail("validation accepted a backend Type in a HostTable signature");
            }
        }
        {
            Module module("const_type");
            irConstant(module, module.internRealType(), "1.0");
            irConstant(module, module.internStringType(), "message");
            diag::Diagnostics diagnostics;
            if (!module.validate(diagnostics))
            {
                std::cerr << diagnosticsText(diagnostics) << '\n';
                return fail("validation rejected real or string generic.const results");
            }
        }
        {
            Module module("array_const_type");
            const TypeId logic8 = module.internLogicType(8, false);
            const TypeId array = module.internArrayType(4, logic8);
            irConstant(module, array, "array");
            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            if (!hasDiagnostic(diagnostics, "non-array generic Type"))
            {
                return fail("validation accepted an array generic.const result");
            }
        }
        {
            Module module("multiple_scalar_writers");
            const TypeId logic1 = module.internLogicType(1, false);
            const TypeId logic8 = module.internLogicType(8, false);
            module.addState("clk", StateKind::Input, logic1);
            module.addState("q", StateKind::State, logic8);

            const auto makeConst = [&](TypeId type, std::string_view literal) {
                const OpId op = module.createOp(genericOp(GenericOpcode::Const));
                const EdgeId edge = module.addResult(op, type);
                module.setAttr(op, "value", module.intern(literal));
                return edge;
            };
            const EdgeId condition = makeConst(logic1, "1'b1");
            const EdgeId value = makeConst(logic8, "8'h2a");
            const EdgeId mask = makeConst(logic8, "8'hff");
            const std::array<EdgeId, 3> operands{condition, value, mask};
            for (int index = 0; index < 2; ++index)
            {
                const OpId write = module.createOp(genericOp(GenericOpcode::RegWrite));
                module.setOperands(write, operands);
                module.setAttr(write, "state", module.intern("q"));
                module.setAttr(write, "events", std::vector<SymbolId>{module.intern("clk")});
                module.setAttr(write, "eventEdge",
                               std::vector<SymbolId>{module.intern("posedge")});
            }
            diag::Diagnostics diagnostics;
            module.validate(diagnostics);
            if (!hasDiagnostic(diagnostics, "multiple register/latch writers"))
            {
                return fail("validation accepted multiple scalar state writers");
            }
        }
        {
            Module module("mutually_exclusive_scalar_writers");
            const TypeId logic1 = module.internLogicType(1, false);
            const TypeId logic8 = module.internLogicType(8, false);
            module.addState("clk", StateKind::Input, logic1);
            module.addState("q", StateKind::State, logic8);

            const auto makeConst = [&](TypeId type, std::string_view literal) {
                const OpId op = module.createOp(genericOp(GenericOpcode::Const));
                const EdgeId edge = module.addResult(op, type);
                module.setAttr(op, "value", module.intern(literal));
                return edge;
            };
            const EdgeId condition = makeConst(logic1, "1'b1");
            const EdgeId value = makeConst(logic8, "8'h2a");
            const EdgeId mask = makeConst(logic8, "8'hff");
            const std::array<EdgeId, 3> operands{condition, value, mask};
            for (std::string_view edge : {"posedge", "negedge"})
            {
                const OpId write = module.createOp(genericOp(GenericOpcode::RegWrite));
                module.setOperands(write, operands);
                module.setAttr(write, "state", module.intern("q"));
                module.setAttr(write, "events", std::vector<SymbolId>{module.intern("clk")});
                module.setAttr(write, "eventEdge",
                               std::vector<SymbolId>{module.intern(edge)});
            }
            diag::Diagnostics diagnostics;
            if (!module.validate(diagnostics))
            {
                std::cerr << diagnosticsText(diagnostics) << '\n';
                return fail("validation rejected mutually exclusive scalar state writers");
            }
        }
        {
            Module module("memory_write_priorities");
            const TypeId logic1 = module.internLogicType(1, false);
            const TypeId logic2 = module.internLogicType(2, false);
            const TypeId logic8 = module.internLogicType(8, false);
            const TypeId memoryType = module.internArrayType(4, logic8);
            module.addState("clk", StateKind::Input, logic1);
            module.addState("mem", StateKind::State, memoryType);

            const auto makeConst = [&](TypeId type, std::string_view literal) {
                const OpId op = module.createOp(genericOp(GenericOpcode::Const));
                const EdgeId edge = module.addResult(op, type);
                module.setAttr(op, "value", module.intern(literal));
                return edge;
            };
            const std::array<EdgeId, 4> operands{
                makeConst(logic1, "1'b1"),
                makeConst(logic2, "2'b00"),
                makeConst(logic8, "8'h2a"),
                makeConst(logic8, "8'hff"),
            };
            std::array<OpId, 2> writes{};
            for (std::size_t index = 0; index < writes.size(); ++index)
            {
                writes[index] = module.createOp(genericOp(GenericOpcode::MemWrite));
                module.setOperands(writes[index], operands);
                module.setAttr(writes[index], "state", module.intern("mem"));
                module.setAttr(writes[index], "events",
                               std::vector<SymbolId>{module.intern("clk")});
                module.setAttr(writes[index], "eventEdge",
                               std::vector<SymbolId>{module.intern("posedge")});
            }
            diag::Diagnostics unorderedDiagnostics;
            module.validate(unorderedDiagnostics);
            if (!hasDiagnostic(unorderedDiagnostics, "require one explicit priority group"))
            {
                return fail("validation accepted ambiguous memory writers");
            }

            for (std::size_t index = 0; index < writes.size(); ++index)
            {
                module.setAttr(writes[index], "memoryWrite.priorityGroup",
                               module.intern("writes"));
                module.setAttr(writes[index], "memoryWrite.priority",
                               static_cast<int64_t>(index * 2));
            }
            diag::Diagnostics gappedDiagnostics;
            module.validate(gappedDiagnostics);
            if (!hasDiagnostic(gappedDiagnostics, "unique and contiguous from zero"))
            {
                return fail("validation accepted a gapped memory write priority group");
            }
            module.setAttr(writes[1], "memoryWrite.priority", int64_t{1});
            diag::Diagnostics orderedDiagnostics;
            if (!module.validate(orderedDiagnostics))
            {
                std::cerr << diagnosticsText(orderedDiagnostics) << '\n';
                return fail("validation rejected a contiguous memory write priority group");
            }
        }
        return 0;
    }

    int testComputeValidationContracts()
    {
        Module module("invalid_compute_contracts");
        const TypeId logic1 = module.internLogicType(1, false);
        const TypeId logic2 = module.internLogicType(2, false);
        const TypeId logic3 = module.internLogicType(3, false);
        const TypeId logic4 = module.internLogicType(4, false);
        const TypeId logic8 = module.internLogicType(8, false);
        const TypeId logic16 = module.internLogicType(16, false);
        const EdgeId one = irConstant(module, logic1, "1'b0");
        const EdgeId two = irConstant(module, logic2, "2'b00");
        const EdgeId three = irConstant(module, logic3, "3'b000");
        const EdgeId four = irConstant(module, logic4, "4'b0000");
        const EdgeId eight = irConstant(module, logic8, "8'b0");
        const EdgeId sixteen = irConstant(module, logic16, "16'b0");

        const auto addOp = [&](GenericOpcode opcode,
                               std::initializer_list<EdgeId> operands,
                               TypeId resultType) {
            const OpId op = module.createOp(genericOp(opcode));
            module.setOperands(op, std::span<const EdgeId>(operands.begin(), operands.size()));
            module.addResult(op, resultType);
            return op;
        };

        addOp(GenericOpcode::Add, {eight, sixteen}, logic8);
        addOp(GenericOpcode::Eq, {eight, sixteen}, logic2);
        addOp(GenericOpcode::Mux, {two, eight, sixteen}, logic8);
        addOp(GenericOpcode::Concat, {eight, eight}, logic8);
        const OpId replicate = addOp(GenericOpcode::Replicate, {eight}, logic8);
        module.setAttr(replicate, "count", int64_t{2});
        const OpId sliceStatic = addOp(GenericOpcode::SliceStatic, {eight}, logic4);
        module.setAttr(sliceStatic, "lsb", int64_t{6});
        addOp(GenericOpcode::SliceDynamic, {four, two}, logic8);
        addOp(GenericOpcode::SliceArray, {eight, two}, logic16);

        const OpId laneConst = addOp(GenericOpcode::ArrayLaneConst, {}, logic16);
        module.setAttr(laneConst, "elem_width", int64_t{8});
        module.setAttr(laneConst, "rows", int64_t{4});
        module.setAttr(laneConst, "values", std::vector<int64_t>{1, 2});
        const OpId negativeLaneConst =
            addOp(GenericOpcode::ArrayLaneConst, {}, logic8);
        module.setAttr(negativeLaneConst, "elem_width", int64_t{-2});
        module.setAttr(negativeLaneConst, "rows", int64_t{-1});
        module.setAttr(negativeLaneConst, "values", std::vector<int64_t>{});

        addOp(GenericOpcode::ArrayMux, {three, eight, sixteen}, logic8);
        const OpId onehot = addOp(GenericOpcode::ArrayOnehot, {two}, logic2);
        module.setAttr(onehot, "rows", int64_t{4});
        const OpId reduce = addOp(GenericOpcode::ArrayReduceOr, {eight}, logic2);
        module.setAttr(reduce, "elem_width", int64_t{2});
        const OpId reduceLanes =
            addOp(GenericOpcode::ArrayReduceLanesOr, {eight}, logic8);
        module.setAttr(reduceLanes, "elem_width", int64_t{2});
        const OpId unevenReduce =
            addOp(GenericOpcode::ArrayReduceXor, {eight}, logic1);
        module.setAttr(unevenReduce, "elem_width", int64_t{3});
        const OpId broadcast = addOp(GenericOpcode::ArrayBroadcast, {eight}, logic16);
        module.setAttr(broadcast, "rows", int64_t{4});

        diag::Diagnostics diagnostics;
        if (module.validate(diagnostics))
        {
            return fail("validation accepted invalid generic compute contracts");
        }
        for (std::string_view expected : {
                 "result width does not match its operands",
                 "result must be one bit",
                 "mux condition must be one bit",
                 "mux data operands and result must have identical widths",
                 "concat result width",
                 "replicate result width",
                 "slice_static range exceeds",
                 "dynamic slice result width exceeds",
                 "slice_array result width must divide",
                 "array_lane_const values count",
                 "array_lane_const result width",
                 "array_lane_const rows must be positive",
                 "array_lane_const elem_width must be positive",
                 "array_mux data operands and result",
                 "array_mux packed data width",
                 "array_onehot result width",
                 "array reduction result width",
                 "array reduction elem_width must divide",
                 "array_broadcast result width",
             })
        {
            if (!hasDiagnostic(diagnostics, expected))
            {
                std::cerr << diagnosticsText(diagnostics) << '\n';
                return fail("generic compute validation missed an invalid contract");
            }
        }

        Module assignModule("assign_conversion");
        const TypeId unsigned8 = assignModule.internLogicType(8, false);
        const TypeId signed32 = assignModule.internLogicType(32, true);
        const EdgeId input = irConstant(assignModule, unsigned8, "8'hff");
        const OpId assign = assignModule.createOp(genericOp(GenericOpcode::Assign));
        const std::array<EdgeId, 1> assignOperands{input};
        assignModule.setOperands(assign, assignOperands);
        assignModule.addResult(assign, signed32);
        diag::Diagnostics assignDiagnostics;
        if (!assignModule.validate(assignDiagnostics))
        {
            std::cerr << diagnosticsText(assignDiagnostics) << '\n';
            return fail("validation rejected a logic assign Type conversion");
        }
        return 0;
    }

    int testJsonBoundaryValidation()
    {
        const std::array<AttrValue, 2> nonFiniteValues{
            AttrValue{std::numeric_limits<double>::quiet_NaN()},
            AttrValue{std::vector<double>{std::numeric_limits<double>::infinity()}},
        };
        for (std::size_t index = 0; index < nonFiniteValues.size(); ++index)
        {
            Module module("non_finite_attribute");
            const std::array<AttrKV, 1> init{
                AttrKV{module.intern("value"), nonFiniteValues[index]},
            };
            const bool constructed = index == 0
                                         ? module.addState("in", StateKind::Input,
                                                           module.internLogicType(1, false), init)
                                               .valid()
                                         : module.addHost("host", HostKind::Query, {}, "host", init)
                                               .valid();
            if (!constructed)
            {
                return fail("failed to construct a non-finite attribute test module");
            }
            diag::Diagnostics diagnostics;
            if (module.validate(diagnostics) ||
                !hasDiagnostic(diagnostics, "non-finite floating-point value"))
            {
                return fail("validation accepted a non-finite floating-point attribute");
            }
            bool rejected = false;
            try
            {
                (void)storeJson(module, false);
            }
            catch (const std::runtime_error &)
            {
                rejected = true;
            }
            if (!rejected)
            {
                return fail("JSON store accepted a non-finite floating-point attribute");
            }
        }

        Module module("json_integer_boundaries");
        const TypeId logic1 = module.internLogicType(1, false);
        const TypeId backend = module.internBackendType(1, 7, {}, logic1);
        const StateId input = module.addState("in", StateKind::Input, logic1);
        module.setBackendType(input, backend);
        const OpId constantOp = module.createOp(genericOp(GenericOpcode::Const));
        module.addResult(constantOp, logic1);
        module.setAttr(constantOp, "value", module.intern("1'b0"));
        const RegionId region = module.createRegion();
        const std::array<OpId, 1> order{constantOp};
        module.setRegion(order, region);
        module.setRegionOrder(region, order);
        const std::string canonical = storeJson(module, false);
        if (!structurallyEquivalent(module, loadJson(canonical)))
        {
            return fail("JSON boundary fixture failed its canonical round trip");
        }

        for (const auto &[from, to] : std::array{
                 std::pair{std::string_view{"\"dialect\":1"},
                           std::string_view{"\"dialect\":65537"}},
                 std::pair{std::string_view{"\"kind_id\":7"},
                           std::string_view{"\"kind_id\":263"}},
                 std::pair{std::string_view{"\"element_type\":-1"},
                           std::string_view{"\"element_type\":4294967295"}},
                 std::pair{std::string_view{"\"refines\":-1"},
                           std::string_view{"\"refines\":4294967295"}},
                 std::pair{std::string_view{"\"backend_type\":1"},
                           std::string_view{"\"backend_type\":4294967295"}},
                 std::pair{std::string_view{"\"region\":0"},
                           std::string_view{"\"region\":4294967295"}},
                 std::pair{std::string_view{"\"activation_state\":-1"},
                           std::string_view{"\"activation_state\":-2"}},
             })
        {
            std::string malformed = canonical;
            if (!replaceOnce(malformed, from, to))
            {
                return fail("JSON boundary test could not locate its mutation target");
            }
            bool rejected = false;
            try
            {
                (void)loadJson(malformed);
            }
            catch (const std::runtime_error &)
            {
                rejected = true;
            }
            if (!rejected)
            {
                return fail("JSON load accepted an out-of-range integer or nullable ID");
            }
        }
        return 0;
    }

    int testLoweringRejectsResidualHierarchy()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("hierarchical");
        design.markAsTop(graph.symbol());
        graph.createOperation(grh::OperationKind::kInstance,
                              graph.internSymbol("unresolved_instance"));
        diag::Diagnostics diagnostics;
        const auto lowered = lowerGrhToGrhsim(graph, diagnostics);
        if (lowered || !diagnostics.hasError())
        {
            return fail("lower_grhsim accepted residual hierarchy");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testModuleStorageAndPasses(); result != 0)
    {
        return result;
    }
    if (const int result = testLoweringRoundTrip(); result != 0)
    {
        return result;
    }
    if (const int result = testComputeOpcodeLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testSourceLessValueLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testTruncatedMultiplyLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testContextSizedMuxLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testWideEffectConditionLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testContextSizedBitwiseLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testMemoryFillAndSynthesizedPriorities(); result != 0)
    {
        return result;
    }
    if (const int result = testValidationRejectsInvalidContracts(); result != 0)
    {
        return result;
    }
    if (const int result = testComputeValidationContracts(); result != 0)
    {
        return result;
    }
    if (const int result = testJsonBoundaryValidation(); result != 0)
    {
        return result;
    }
    return testLoweringRejectsResidualHierarchy();
}
