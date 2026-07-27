#include "core/grh.hpp"
#include "grhsim/am/interpreter.hpp"
#include "grhsim/am/lowering.hpp"
#include "grhsim/am/production_activity_schedule.hpp"
#include "grhsim/am/validate.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace wolvrix::lib;
    using namespace wolvrix::lib::grhsim::am;

    int fail(std::string_view message)
    {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    grh::ValueId logic(grh::Graph &graph, std::string_view name, int32_t width,
                       bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(name), width, isSigned,
                                 grh::ValueType::Logic);
    }

    grh::ValueId constant(grh::Graph &graph, std::string_view opName,
                          std::string_view valueName, int32_t width,
                          std::string literal, bool isSigned = false)
    {
        const auto value = logic(graph, valueName, width, isSigned);
        const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                              graph.internSymbol(opName));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    std::size_t countOpcode(ProgramView program, Opcode opcode)
    {
        std::size_t count = 0;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            count += program.opcode(InstructionId{index}) == opcode;
        }
        return count;
    }

    int testRepresentativeLowering()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("am_lowering");
        design.markAsTop(graph.symbol());

        const auto clk = logic(graph, "clk", 1);
        const auto en = logic(graph, "en", 1);
        const auto a = logic(graph, "a", 8);
        const auto address = logic(graph, "address", 2);
        const auto ioIn = logic(graph, "io_in", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("a", a);
        graph.bindInputPort("address", address);

        const auto one = constant(graph, "one_op", "one", 8, "8'h1");
        const auto mask = constant(graph, "mask_op", "mask", 8, "8'hff");
        const auto packed = constant(graph, "packed_op", "packed", 32,
                                     "32'h04030201");

        const auto sum = logic(graph, "sum", 8, true);
        const auto add = graph.createOperation(grh::OperationKind::kAdd,
                                               graph.internSymbol("add"));
        graph.addOperand(add, a);
        graph.addOperand(add, one);
        graph.addResult(add, sum);

        const auto reg = graph.createOperation(grh::OperationKind::kRegister,
                                               graph.internSymbol("q"));
        graph.setAttr(reg, "width", int64_t{8});
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h12"));
        graph.addDeclaredSymbol(graph.operationSymbol(reg));

        const auto q = logic(graph, "q_read", 8);
        const auto regRead = graph.createOperation(grh::OperationKind::kRegisterReadPort,
                                                   graph.internSymbol("q_read_op"));
        graph.addResult(regRead, q);
        graph.setAttr(regRead, "regSymbol", std::string("q"));

        const auto regWrite = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                    graph.internSymbol("q_write"));
        graph.addOperand(regWrite, en);
        graph.addOperand(regWrite, sum);   // GRH: cond, next, mask, events...
        graph.addOperand(regWrite, mask);
        graph.addOperand(regWrite, clk);
        graph.setAttr(regWrite, "regSymbol", std::string("q"));
        graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});

        const auto memory = graph.createOperation(grh::OperationKind::kMemory,
                                                  graph.internSymbol("mem"));
        graph.setAttr(memory, "width", int64_t{8});
        graph.setAttr(memory, "row", int64_t{4});
        graph.setAttr(memory, "isSigned", false);
        graph.setAttr(memory, "initKind", std::vector<std::string>{"literal"});
        graph.setAttr(memory, "initFile", std::vector<std::string>{""});
        graph.setAttr(memory, "initValue", std::vector<std::string>{"8'h3c"});
        graph.setAttr(memory, "initStart", std::vector<int64_t>{0});
        graph.setAttr(memory, "initLen", std::vector<int64_t>{4});

        const auto memData = logic(graph, "mem_data", 8);
        const auto memRead = graph.createOperation(grh::OperationKind::kMemoryReadPort,
                                                   graph.internSymbol("mem_read"));
        graph.addOperand(memRead, address);
        graph.addResult(memRead, memData);
        graph.setAttr(memRead, "memSymbol", std::string("mem"));

        const auto memWrite = graph.createOperation(grh::OperationKind::kMemoryWritePort,
                                                    graph.internSymbol("mem_write"));
        graph.addOperand(memWrite, en);
        graph.addOperand(memWrite, address);
        graph.addOperand(memWrite, a);     // GRH: cond, addr, data, mask, events...
        graph.addOperand(memWrite, mask);
        graph.addOperand(memWrite, clk);
        graph.setAttr(memWrite, "memSymbol", std::string("mem"));
        graph.setAttr(memWrite, "eventEdge", std::vector<std::string>{"posedge"});

        const auto memFill = graph.createOperation(grh::OperationKind::kMemoryFillPort,
                                                   graph.internSymbol("mem_fill"));
        graph.addOperand(memFill, en);
        graph.addOperand(memFill, packed);
        graph.addOperand(memFill, clk);
        graph.setAttr(memFill, "memSymbol", std::string("mem"));
        graph.setAttr(memFill, "eventEdge", std::vector<std::string>{"negedge"});

        const auto latch = graph.createOperation(grh::OperationKind::kLatch,
                                                 graph.internSymbol("lat"));
        graph.setAttr(latch, "width", int64_t{8});
        graph.setAttr(latch, "isSigned", false);
        const auto latValue = logic(graph, "lat_read", 8);
        const auto latchRead = graph.createOperation(grh::OperationKind::kLatchReadPort,
                                                     graph.internSymbol("lat_read_op"));
        graph.addResult(latchRead, latValue);
        graph.setAttr(latchRead, "latchSymbol", std::string("lat"));
        const auto latchWrite = graph.createOperation(grh::OperationKind::kLatchWritePort,
                                                      graph.internSymbol("lat_write"));
        graph.addOperand(latchWrite, en);
        graph.addOperand(latchWrite, a);
        graph.addOperand(latchWrite, mask);
        graph.setAttr(latchWrite, "latchSymbol", std::string("lat"));

        const auto timeValue = logic(graph, "time_value", 64);
        const auto systemFunction =
            graph.createOperation(grh::OperationKind::kSystemFunction,
                                  graph.internSymbol("time_call"));
        graph.addResult(systemFunction, timeValue);
        graph.setAttr(systemFunction, "name", std::string("$time"));
        graph.setAttr(systemFunction, "hasSideEffects", false);

        const auto secondTimeValue = logic(graph, "second_time_value", 64);
        const auto secondSystemFunction =
            graph.createOperation(grh::OperationKind::kSystemFunction,
                                  graph.internSymbol("second_time_call"));
        graph.addResult(secondSystemFunction, secondTimeValue);
        graph.setAttr(secondSystemFunction, "name", std::string("$time"));
        graph.setAttr(secondSystemFunction, "hasSideEffects", false);

        const auto text = graph.createValue(graph.internSymbol("text"), 0, false,
                                            grh::ValueType::String);
        const auto textOp = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("text_op"));
        graph.addResult(textOp, text);
        graph.setAttr(textOp, "constValue", std::string("q=%h\n"));
        const auto task = graph.createOperation(grh::OperationKind::kSystemTask,
                                                graph.internSymbol("display"));
        graph.addOperand(task, en);
        graph.addOperand(task, text);
        graph.addOperand(task, q);
        graph.addOperand(task, clk);
        graph.setAttr(task, "name", std::string("$display"));
        graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(task, "procKind", std::string("always"));
        graph.setAttr(task, "hasTiming", true);

        const auto dpiImport = graph.createOperation(grh::OperationKind::kDpicImport,
                                                     graph.internSymbol("dpi_echo"));
        graph.setAttr(dpiImport, "argsDirection",
                      std::vector<std::string>{"input", "output"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8, 8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"x", "y"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic", "logic"});
        graph.setAttr(dpiImport, "hasReturn", false);

        const auto dpiOut = logic(graph, "dpi_out", 8);
        const auto dpiCall = graph.createOperation(grh::OperationKind::kDpicCall,
                                                   graph.internSymbol("dpi_call"));
        graph.addOperand(dpiCall, en);
        graph.addOperand(dpiCall, a);
        graph.addOperand(dpiCall, clk);
        graph.addResult(dpiCall, dpiOut);
        graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_echo"));
        graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"x"});
        graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"y"});
        graph.setAttr(dpiCall, "inoutArgName", std::vector<std::string>{});
        graph.setAttr(dpiCall, "hasReturn", false);
        graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});

        const auto ioOut = logic(graph, "io_out", 8);
        const auto ioAssign = graph.createOperation(grh::OperationKind::kAssign,
                                                    graph.internSymbol("io_assign"));
        graph.addOperand(ioAssign, a);
        graph.addResult(ioAssign, ioOut);
        const auto ioOe = logic(graph, "io_oe", 1);
        const auto oeAssign = graph.createOperation(grh::OperationKind::kAssign,
                                                    graph.internSymbol("oe_assign"));
        graph.addOperand(oeAssign, en);
        graph.addResult(oeAssign, ioOe);
        graph.bindInoutPort("io", ioIn, ioOut, ioOe);
        graph.bindOutputPort("q_out", q);
        graph.bindOutputPort("mem_out", memData);
        graph.bindOutputPort("dpi_out", dpiOut);

        graph.freeze();
        diag::Diagnostics diagnostics;
        GrhToAmLowering lowering;
        auto artifact = lowering.lower(graph, diagnostics);
        if (!artifact || diagnostics.hasError())
        {
            for (const auto &message : diagnostics.messages())
            {
                std::cerr << message.message << " [" << message.context << "]\n";
            }
            return fail("representative normalized graph did not lower");
        }
        const ValidationResult validation =
            validate(*artifact, ValidationOptions{.level = ValidationLevel::Semantic});
        if (!validation.success())
        {
            return fail(validation.errors.front());
        }

        const ProgramView program = artifact->program.view();
        if (program.dpiImportCount() != 1 ||
            countOpcode(program, Opcode::RegisterWrite) != 1 ||
            countOpcode(program, Opcode::MemoryRead) != 1 ||
            countOpcode(program, Opcode::MemoryWrite) != 1 ||
            countOpcode(program, Opcode::MemoryFill) != 1 ||
            countOpcode(program, Opcode::LatchWrite) != 1 ||
            countOpcode(program, Opcode::SystemFunction) != 2 ||
            countOpcode(program, Opcode::SystemTask) != 1 ||
            countOpcode(program, Opcode::DpiCall) != 1 ||
            countOpcode(program, Opcode::ChangedPos) != 1 ||
            countOpcode(program, Opcode::ChangedNeg) != 1)
        {
            return fail("lowered opcode inventory is incomplete");
        }

        // All posedge(clk) consumers share one lowered detector instance.
        VariableId sharedPosedge;
        bool checkedSharedPosedge = false;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            const Opcode opcode = program.opcode(instruction);
            if (opcode == Opcode::ChangedPos)
            {
                sharedPosedge = program.results(instruction).front();
                continue;
            }
            std::size_t eventIndex = 0;
            if (opcode == Opcode::RegisterWrite)
            {
                eventIndex = 4;
            }
            else if (opcode == Opcode::MemoryWrite)
            {
                eventIndex = 5;
            }
            else
            {
                continue;
            }
            if (program.operands(instruction)[eventIndex] != sharedPosedge)
            {
                return fail("posedge(clk) writes did not share one lowered detector");
            }
            checkedSharedPosedge = true;
        }
        if (!checkedSharedPosedge || !sharedPosedge.valid())
        {
            return fail("missing shared posedge detector");
        }
        if (artifact->interface.ports.size() != 8 ||
            artifact->schedulingFacts.orderedEffects.size() != 4)
        {
            return fail("interface or ordered-effect facts are incomplete");
        }

        std::vector<InstructionId> ordinaryHostInstructions;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            const Opcode opcode = program.opcode(instruction);
            if (opcode == Opcode::SystemFunction || opcode == Opcode::SystemTask ||
                opcode == Opcode::DpiCall)
            {
                ordinaryHostInstructions.push_back(instruction);
            }
            if (opcode == Opcode::SystemTask &&
                program.systemTaskAttributes(instruction)->eventMode !=
                    HostEventMode::Immediate)
            {
                return fail("lowered system task must use immediate event consumption");
            }
            if (opcode == Opcode::DpiCall &&
                program.dpiCallAttributes(instruction)->eventMode !=
                    HostEventMode::Pending)
            {
                return fail("value-producing DPI call must retain its event within eval");
            }
        }
        if (ordinaryHostInstructions.size() != 4)
        {
            return fail("representative lowering did not contain four ordinary host interactions");
        }

        for (const OrderedEffect &effect : artifact->schedulingFacts.orderedEffects)
        {
            const Opcode opcode = program.opcode(effect.instruction);
            if (std::find(ordinaryHostInstructions.begin(), ordinaryHostInstructions.end(),
                          effect.instruction) != ordinaryHostInstructions.end())
            {
                return fail("ordinary host interaction was recorded as an explicit ordered effect");
            }
            if (opcode != Opcode::RegisterWrite && opcode != Opcode::MemoryWrite &&
                opcode != Opcode::MemoryFill && opcode != Opcode::LatchWrite)
            {
                return fail("unexpected instruction was recorded as an ordered effect");
            }
        }

        bool checkedRegisterOrder = false;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            if (program.opcode(instruction) != Opcode::RegisterWrite)
            {
                continue;
            }
            const auto operands = program.operands(instruction);
            if (operands.size() != 5)
            {
                return fail("register write did not get canonical AM operands");
            }
            const Type &maskType = program.type(program.variable(operands[1]).type);
            const Type &nextType = program.type(program.variable(operands[2]).type);
            const Type &targetType = program.type(program.variable(operands[3]).type);
            if (maskType.bitWidth != 8 || nextType != targetType)
            {
                return fail("register mask/next/target order or coercion is wrong");
            }
            checkedRegisterOrder = true;
        }
        return checkedRegisterOrder ? 0 : fail("missing register write");
    }

    int testExplicitExternalDpiOrderIsPreserved()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("explicit_external_dpi_order");
        design.markAsTop(graph.symbol());

        const auto condition = logic(graph, "condition", 1);
        const auto input = logic(graph, "input", 8);
        graph.bindInputPort("condition", condition);
        graph.bindInputPort("input", input);

        const auto dpiImport = graph.createOperation(grh::OperationKind::kDpicImport,
                                                     graph.internSymbol("ordered_dpi"));
        graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"x"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
        graph.setAttr(dpiImport, "hasReturn", false);

        const auto addCall = [&](std::string_view name, int64_t ordinal) {
            const auto call = graph.createOperation(grh::OperationKind::kDpicCall,
                                                    graph.internSymbol(name));
            graph.addOperand(call, condition);
            graph.addOperand(call, input);
            graph.setAttr(call, "targetImportSymbol", std::string("ordered_dpi"));
            graph.setAttr(call, "inArgName", std::vector<std::string>{"x"});
            graph.setAttr(call, "outArgName", std::vector<std::string>{});
            graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
            graph.setAttr(call, "hasReturn", false);
            graph.setAttr(call, "gsim.external_instance_group", std::string("instance0"));
            graph.setAttr(call, "gsim.external_call_ordinal", ordinal);
        };

        addCall("ordinal_one", 1);
        addCall("ordinal_zero", 0);
        graph.freeze();

        diag::Diagnostics diagnostics;
        GrhToAmLowering lowering;
        auto artifact = lowering.lower(graph, diagnostics);
        if (!artifact || diagnostics.hasError())
        {
            return fail("explicit external DPI order did not lower");
        }

        const ProgramView program = artifact->program.view();
        std::vector<InstructionId> dpiCalls;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            if (program.opcode(instruction) == Opcode::DpiCall)
            {
                dpiCalls.push_back(instruction);
            }
        }
        const auto &ordered = artifact->schedulingFacts.orderedEffects;
        if (dpiCalls.size() != 2 || ordered.size() != 2 ||
            ordered[0].group != ordered[1].group || ordered[0].ordinal != 0 ||
            ordered[1].ordinal != 1 || ordered[0].instruction != dpiCalls[1] ||
            ordered[1].instruction != dpiCalls[0] ||
            program.dpiCallAttributes(dpiCalls[0])->eventMode !=
                HostEventMode::Immediate ||
            program.dpiCallAttributes(dpiCalls[1])->eventMode !=
                HostEventMode::Immediate)
        {
            return fail("explicit external DPI order was omitted or did not follow its ordinals");
        }
        const ValidationResult validation =
            validate(*artifact, ValidationOptions{.level = ValidationLevel::Semantic});
        return validation.success() ? 0 : fail(validation.errors.front());
    }

    int testMemoryFillAndPriorityWritesShareStateOrder()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("memory_fill_priority_writes");
        design.markAsTop(graph.symbol());

        const auto clock = logic(graph, "clock", 1);
        const auto enable = logic(graph, "enable", 1);
        const auto address = logic(graph, "address", 2);
        const auto highData = logic(graph, "high_data", 8);
        const auto lowData = logic(graph, "low_data", 8);
        graph.bindInputPort("clock", clock);
        graph.bindInputPort("enable", enable);
        graph.bindInputPort("address", address);
        graph.bindInputPort("high_data", highData);
        graph.bindInputPort("low_data", lowData);
        const auto mask = constant(graph, "mask_op", "mask", 8, "8'hff");
        const auto fillData =
            constant(graph, "fill_data_op", "fill_data", 32, "32'h44332211");
        const auto disabled = constant(graph, "disabled_op", "disabled", 1, "1'h0");

        const auto memory = graph.createOperation(grh::OperationKind::kMemory,
                                                  graph.internSymbol("mem"));
        graph.setAttr(memory, "width", int64_t{8});
        graph.setAttr(memory, "row", int64_t{4});
        graph.setAttr(memory, "isSigned", false);

        const auto fill = graph.createOperation(grh::OperationKind::kMemoryFillPort,
                                                graph.internSymbol("fill"));
        graph.addOperand(fill, enable);
        graph.addOperand(fill, fillData);
        graph.addOperand(fill, clock);
        graph.setAttr(fill, "memSymbol", std::string("mem"));
        graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});

        const auto addWrite = [&](std::string_view name, grh::ValueId data,
                                  int64_t priority) {
            const auto write = graph.createOperation(grh::OperationKind::kMemoryWritePort,
                                                     graph.internSymbol(name));
            graph.addOperand(write, enable);
            graph.addOperand(write, address);
            graph.addOperand(write, data);
            graph.addOperand(write, mask);
            graph.addOperand(write, clock);
            graph.setAttr(write, "memSymbol", std::string("mem"));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            graph.setAttr(write, grh::kMemoryWritePriorityGroupAttr,
                          std::string("mem_writes"));
            graph.setAttr(write, grh::kMemoryWritePriorityAttr, priority);
        };
        addWrite("high_write", highData, 0);

        const auto interleavedFill =
            graph.createOperation(grh::OperationKind::kMemoryFillPort,
                                  graph.internSymbol("interleaved_fill"));
        graph.addOperand(interleavedFill, disabled);
        graph.addOperand(interleavedFill, fillData);
        graph.addOperand(interleavedFill, clock);
        graph.setAttr(interleavedFill, "memSymbol", std::string("mem"));
        graph.setAttr(interleavedFill, "eventEdge",
                      std::vector<std::string>{"posedge"});

        addWrite("low_write", lowData, 1);
        graph.freeze();

        diag::Diagnostics diagnostics;
        GrhToAmLowering lowering;
        std::optional<LinearProgramArtifact> artifact = lowering.lower(graph, diagnostics);
        if (!artifact || diagnostics.hasError())
        {
            return fail("memory fill and priority writes did not lower");
        }

        const ProgramView program = artifact->program.view();
        VariableId memoryVariable = VariableId::invalid();
        std::vector<InstructionId> fills;
        std::vector<InstructionId> writes;
        for (uint32_t index = 0; index < program.variableCount(); ++index)
        {
            const VariableId variable{index};
            const std::optional<StringId> label = program.variableLabel(variable);
            if (label && program.string(*label) == "mem")
            {
                memoryVariable = variable;
                break;
            }
        }
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            if (program.opcode(instruction) == Opcode::MemoryFill)
            {
                fills.push_back(instruction);
            }
            else if (program.opcode(instruction) == Opcode::MemoryWrite)
            {
                writes.push_back(instruction);
            }
        }
        const std::vector<OrderedEffect> &ordered =
            artifact->schedulingFacts.orderedEffects;
        if (!memoryVariable.valid() || fills.size() != 2 || writes.size() != 2 ||
            ordered.size() != 4 ||
            ordered[0].group != ordered[1].group ||
            ordered[1].group != ordered[2].group ||
            ordered[2].group != ordered[3].group || ordered[0].ordinal != 0 ||
            ordered[1].ordinal != 1 || ordered[2].ordinal != 2 ||
            ordered[3].ordinal != 3 || ordered[0].instruction != fills[0] ||
            ordered[1].instruction != writes[1] ||
            ordered[2].instruction != writes[0] || ordered[3].instruction != fills[1])
        {
            return fail("memory fill and atomic priority writes did not form one state group");
        }

        const auto findPort = [&](std::string_view name) {
            for (const PortBinding &port : artifact->interface.ports)
            {
                if (port.direction == PortDirection::Input &&
                    program.string(port.name) == name)
                {
                    return port.input;
                }
            }
            return VariableId::invalid();
        };
        const VariableId clockVariable = findPort("clock");
        const VariableId enableVariable = findPort("enable");
        const VariableId addressVariable = findPort("address");
        const VariableId highDataVariable = findPort("high_data");
        const VariableId lowDataVariable = findPort("low_data");
        if (!clockVariable.valid() || !enableVariable.valid() ||
            !addressVariable.valid() || !highDataVariable.valid() ||
            !lowDataVariable.valid())
        {
            return fail("memory priority runtime fixture has an invalid interface");
        }

        ProductionActivityScheduleStage scheduler;
        std::optional<ExecutableModel> model = scheduler.schedule(
            std::move(*artifact),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .maxCommitInstructionsPerBlock = 1,
                .maxStateWritesPerBlock = 1,
                .enableCoarsening = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("scheduler rejected a complete fill/write state order group");
        }
        const auto bitVector = [](uint32_t width, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return InterpreterValue::bitVector(width, Signedness::Unsigned, words);
        };
        Interpreter interpreter(*model);
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter.write(enableVariable, bitVector(1, 1)).success() ||
            !interpreter.write(addressVariable, bitVector(2, 2)).success() ||
            !interpreter.write(lowDataVariable, bitVector(8, 0x5a)).success() ||
            !interpreter.write(highDataVariable, bitVector(8, 0xa5)).success() ||
            !interpreter.write(clockVariable, bitVector(1, 1)).success() ||
            !interpreter.eval().success())
        {
            return fail("memory fill/priority runtime fixture did not execute");
        }
        if (interpreter.value(memoryVariable).arrayElementWords(2).front() != 0xa5)
        {
            return fail("priority 0 memory write did not win after a same-edge fill");
        }
        return 0;
    }

    int testRegisterChainSamplesPreCommitState()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("register_chain_snapshot");
        design.markAsTop(graph.symbol());

        const auto clock = logic(graph, "clock", 1);
        const auto nextValue = logic(graph, "next_value", 8);
        graph.bindInputPort("clock", clock);
        graph.bindInputPort("next_value", nextValue);
        const auto enabled = constant(graph, "enabled_op", "enabled", 1, "1'h1");
        const auto mask = constant(graph, "mask_op", "mask", 8, "8'hff");

        const auto addRegister = [&](std::string_view name, std::string init,
                                     bool isSigned) {
            const auto reg = graph.createOperation(grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", int64_t{8});
            graph.setAttr(reg, "isSigned", isSigned);
            graph.setAttr(reg, "initValue", std::move(init));
            graph.addDeclaredSymbol(graph.operationSymbol(reg));
            return reg;
        };
        const auto firstReg = addRegister("first", "8'h12", true);
        const auto secondReg = addRegister("second", "8'h00", false);

        const auto addRead = [&](grh::OperationId reg, std::string_view valueName,
                                 std::string_view operationName, bool isSigned) {
            const auto value = logic(graph, valueName, 8, isSigned);
            const auto read = graph.createOperation(grh::OperationKind::kRegisterReadPort,
                                                    graph.internSymbol(operationName));
            graph.addResult(read, value);
            graph.setAttr(read, "regSymbol",
                          std::string(graph.symbolText(graph.operationSymbol(reg))));
            return value;
        };
        const auto first =
            addRead(firstReg, "first_read", "first_read_op", true);
        const auto second =
            addRead(secondReg, "second_read", "second_read_op", false);
        const auto firstPassthrough = logic(graph, "first_passthrough", 8, true);
        const auto passthrough = graph.createOperation(grh::OperationKind::kAssign,
                                                       graph.internSymbol("first_passthrough_op"));
        graph.addOperand(passthrough, first);
        graph.addResult(passthrough, firstPassthrough);

        const auto addWrite = [&](grh::OperationId reg, grh::ValueId next,
                                  std::string_view name) {
            const auto write = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                     graph.internSymbol(name));
            graph.addOperand(write, enabled);
            graph.addOperand(write, next);
            graph.addOperand(write, mask);
            graph.addOperand(write, clock);
            graph.setAttr(write, "regSymbol",
                          std::string(graph.symbolText(graph.operationSymbol(reg))));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        };
        addWrite(firstReg, nextValue, "first_write");
        addWrite(secondReg, firstPassthrough, "second_write");
        graph.bindOutputPort("first", first);
        graph.bindOutputPort("second", second);
        graph.freeze();

        diag::Diagnostics diagnostics;
        GrhToAmLowering lowering;
        std::optional<LinearProgramArtifact> artifact = lowering.lower(graph, diagnostics);
        if (!artifact || diagnostics.hasError())
        {
            return fail("register-chain graph did not lower");
        }
        const auto findPort = [&](std::string_view name, PortDirection direction) {
            const ProgramView program = artifact->program.view();
            for (const PortBinding &port : artifact->interface.ports)
            {
                if (port.direction == direction && program.string(port.name) == name)
                {
                    return direction == PortDirection::Input ? port.input : port.output;
                }
            }
            return VariableId::invalid();
        };
        const VariableId clockVariable = findPort("clock", PortDirection::Input);
        const VariableId nextVariable = findPort("next_value", PortDirection::Input);
        const VariableId firstVariable = findPort("first", PortDirection::Output);
        const VariableId secondVariable = findPort("second", PortDirection::Output);
        if (!clockVariable.valid() || !nextVariable.valid() || !firstVariable.valid() ||
            !secondVariable.valid() || !artifact->preCommitSnapshots.empty())
        {
            return fail("register-chain lowering produced invalid interface or entry snapshots");
        }

        ProductionActivityScheduleStage scheduler;
        std::optional<ExecutableModel> model = scheduler.schedule(
            std::move(*artifact),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .maxCommitInstructionsPerBlock = 1,
                .maxStateWritesPerBlock = 1,
                .enableCoarsening = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || !model->preCommitSnapshots.empty() ||
            model->commitOperandCaptures.size() < 2 ||
            model->commitOperandCaptureOffsets.empty())
        {
            return fail("register-chain AM program did not schedule");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> nextWords = {0x34};
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            interpreter.value(firstVariable).lowWord() != 0x12 ||
            interpreter.value(secondVariable).lowWord() != 0 ||
            !interpreter
                 .write(nextVariable,
                        InterpreterValue::bitVector(8, Signedness::Unsigned, nextWords))
                 .success() ||
            !interpreter
                 .write(clockVariable,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success() ||
            !interpreter.eval().success())
        {
            return fail("register-chain AM program could not execute its first posedge");
        }
        if (interpreter.value(firstVariable).lowWord() != 0x34 ||
            interpreter.value(secondVariable).lowWord() != 0x12)
        {
            return fail("register chain observed a same-edge committed value");
        }

        const std::array<uint64_t, 1> lowWords = {0};
        const std::array<uint64_t, 1> laterWords = {0x56};
        if (!interpreter
                 .write(clockVariable,
                        InterpreterValue::bitVector(1, Signedness::Unsigned,
                                                    lowWords))
                 .success() ||
            !interpreter.eval().success() ||
            !interpreter
                 .write(nextVariable,
                        InterpreterValue::bitVector(8, Signedness::Unsigned,
                                                    laterWords))
                 .success() ||
            !interpreter
                 .write(clockVariable,
                        InterpreterValue::bitVector(1, Signedness::Unsigned,
                                                    highWords))
                 .success() ||
            !interpreter.eval().success() ||
            interpreter.value(firstVariable).lowWord() != 0x56 ||
            interpreter.value(secondVariable).lowWord() != 0x34)
        {
            return fail("pre-commit snapshot conversion was stale on a later edge");
        }
        return 0;
    }

    int testShiftWidensOperandBeforeExecution()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("wide_shift_operand");
        design.markAsTop(graph.symbol());

        const auto input = logic(graph, "input", 1);
        const auto signedInput = logic(graph, "signed_input", 8, true);
        graph.bindInputPort("input", input);
        graph.bindInputPort("signed_input", signedInput);
        const auto amount =
            constant(graph, "amount_op", "amount", 32, "32'd65");
        const auto shifted = logic(graph, "shifted", 130);
        const auto shift = graph.createOperation(grh::OperationKind::kShl,
                                                 graph.internSymbol("shift"));
        graph.addOperand(shift, input);
        graph.addOperand(shift, amount);
        graph.addResult(shift, shifted);
        graph.bindOutputPort("shifted", shifted);
        const auto logicalShifted = logic(graph, "logical_shifted", 130);
        const auto logicalShift =
            graph.createOperation(grh::OperationKind::kLShr,
                                  graph.internSymbol("logical_shift"));
        graph.addOperand(logicalShift, signedInput);
        graph.addOperand(logicalShift, amount);
        graph.addResult(logicalShift, logicalShifted);
        graph.bindOutputPort("logical_shifted", logicalShifted);
        const auto arithmeticShifted = logic(graph, "arithmetic_shifted", 130);
        const auto arithmeticShift =
            graph.createOperation(grh::OperationKind::kAShr,
                                  graph.internSymbol("arithmetic_shift"));
        graph.addOperand(arithmeticShift, signedInput);
        graph.addOperand(arithmeticShift, amount);
        graph.addResult(arithmeticShift, arithmeticShifted);
        graph.bindOutputPort("arithmetic_shifted", arithmeticShifted);
        graph.freeze();

        diag::Diagnostics diagnostics;
        GrhToAmLowering lowering;
        std::optional<LinearProgramArtifact> artifact =
            lowering.lower(graph, diagnostics);
        if (!artifact || diagnostics.hasError())
        {
            return fail("wide-result shift graph did not lower");
        }

        const ProgramView program = artifact->program.view();
        VariableId inputVariable = VariableId::invalid();
        VariableId signedInputVariable = VariableId::invalid();
        VariableId shiftedVariable = VariableId::invalid();
        VariableId logicalShiftedVariable = VariableId::invalid();
        VariableId arithmeticShiftedVariable = VariableId::invalid();
        for (const PortBinding &port : artifact->interface.ports)
        {
            const std::string_view name = program.string(port.name);
            if (port.direction == PortDirection::Input && name == "input")
            {
                inputVariable = port.input;
            }
            else if (port.direction == PortDirection::Input &&
                     name == "signed_input")
            {
                signedInputVariable = port.input;
            }
            else if (port.direction == PortDirection::Output && name == "shifted")
            {
                shiftedVariable = port.output;
            }
            else if (port.direction == PortDirection::Output &&
                     name == "logical_shifted")
            {
                logicalShiftedVariable = port.output;
            }
            else if (port.direction == PortDirection::Output &&
                     name == "arithmetic_shifted")
            {
                arithmeticShiftedVariable = port.output;
            }
        }

        bool checkedShiftTypes = false;
        bool checkedLogicalShiftTypes = false;
        bool checkedArithmeticShiftTypes = false;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            const Opcode opcode = program.opcode(instruction);
            if (opcode != Opcode::Shl && opcode != Opcode::LogicalShr &&
                opcode != Opcode::ArithmeticShr)
            {
                continue;
            }
            const auto operands = program.operands(instruction);
            const auto results = program.results(instruction);
            if (operands.size() != 2 || results.size() != 1)
            {
                return fail("wide-result shift has invalid AM arity");
            }
            const Type &operandType = program.type(program.variable(operands[0]).type);
            const Type &resultType = program.type(program.variable(results[0]).type);
            if (operandType != resultType)
            {
                return fail("shift lhs/result AM types do not match");
            }
            if (opcode == Opcode::Shl)
            {
                if (operandType.bitWidth != 130 ||
                    operandType.signedness != Signedness::Unsigned)
                {
                    return fail("wide-result shift did not widen its lhs before execution");
                }
                checkedShiftTypes = true;
            }
            else if (opcode == Opcode::LogicalShr)
            {
                if (operandType.bitWidth != 130 ||
                    operandType.signedness != Signedness::Signed)
                {
                    return fail("logical shift did not resize its signed lhs");
                }
                checkedLogicalShiftTypes = true;
            }
            else
            {
                if (operandType.bitWidth != 130 ||
                    operandType.signedness != Signedness::Signed)
                {
                    return fail("arithmetic shift did not preserve lhs signedness");
                }
                checkedArithmeticShiftTypes = true;
            }
        }
        if (!inputVariable.valid() || !signedInputVariable.valid() ||
            !shiftedVariable.valid() || !logicalShiftedVariable.valid() ||
            !arithmeticShiftedVariable.valid() || !checkedShiftTypes ||
            !checkedLogicalShiftTypes || !checkedArithmeticShiftTypes)
        {
            return fail("wide-result shift fixture has an invalid interface or program");
        }

        ProductionActivityScheduleStage scheduler;
        std::optional<ExecutableModel> model = scheduler.schedule(
            std::move(*artifact),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .enableCoarsening = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("wide-result shift graph did not schedule");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> inputWords = {1};
        const std::array<uint64_t, 1> signedInputWords = {0x80};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(inputVariable,
                        InterpreterValue::bitVector(
                            1, Signedness::Unsigned, inputWords))
                 .success() ||
            !interpreter
                 .write(signedInputVariable,
                        InterpreterValue::bitVector(
                            8, Signedness::Signed, signedInputWords))
                 .success() ||
            !interpreter.eval().success())
        {
            return fail("wide-result shift graph did not execute");
        }
        const std::span<const uint64_t> resultWords =
            interpreter.value(shiftedVariable).words();
        if (resultWords.size() != 3 || resultWords[0] != 0 ||
            resultWords[1] != 2 || resultWords[2] != 0)
        {
            return fail("wide-result shift lost bits beyond the lhs width");
        }
        const std::span<const uint64_t> logicalResultWords =
            interpreter.value(logicalShiftedVariable).words();
        if (logicalResultWords.size() != 3 ||
            logicalResultWords[0] != ~uint64_t{0} ||
            logicalResultWords[1] != 1 || logicalResultWords[2] != 0)
        {
            return fail("logical shift did not use the resized signed lhs bits");
        }
        const std::span<const uint64_t> arithmeticResultWords =
            interpreter.value(arithmeticShiftedVariable).words();
        if (arithmeticResultWords.size() != 3 ||
            arithmeticResultWords[0] != ~uint64_t{0} ||
            arithmeticResultWords[1] != ~uint64_t{0} ||
            arithmeticResultWords[2] != 3)
        {
            return fail("arithmetic shift used result signedness instead of lhs signedness");
        }
        return 0;
    }

    int testRejectsXzAndHierarchy()
    {
        {
            grh::Design design;
            auto &graph = design.createGraph("xz");
            const auto value = logic(graph, "x", 4);
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("x_op"));
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::string("4'b10xz"));
            diag::Diagnostics diagnostics;
            GrhToAmLowering lowering;
            if (lowering.lower(graph, diagnostics) || !diagnostics.hasError())
            {
                return fail("X/Z Logic literal was not rejected");
            }
        }
        {
            grh::Design design;
            auto &graph = design.createGraph("xz_two_state");
            const auto value = logic(graph, "x", 4);
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("x_op"));
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::string("4'b10xz"));
            diag::Diagnostics diagnostics;
            GrhToAmLowering lowering(GrhToAmLoweringOptions{
                .unknownLogic = UnknownLogicPolicy::FlattenToZero,
            });
            auto artifact = lowering.lower(graph, diagnostics);
            if (!artifact || diagnostics.hasError())
            {
                return fail("explicit two-state X/Z policy did not lower");
            }
            const ProgramView program = artifact->program.view();
            const InitDescriptor &init =
                program.init(program.variable(VariableId{0}).init);
            const LiteralView literal = program.literal(LiteralId{init.payload});
            if (init.kind != InitKind::Constant || literal.words.size() != 1 ||
                literal.words.front() != 8)
            {
                return fail("two-state X/Z policy did not flatten unknown bits to zero");
            }
        }
        {
            grh::Design design;
            auto &graph = design.createGraph("hierarchy");
            const auto op = graph.createOperation(grh::OperationKind::kInstance,
                                                  graph.internSymbol("u_child"));
            graph.setAttr(op, "moduleName", std::string("child"));
            diag::Diagnostics diagnostics;
            GrhToAmLowering lowering;
            if (lowering.lower(graph, diagnostics) || !diagnostics.hasError())
            {
                return fail("residual hierarchy was not rejected");
            }
        }
        return 0;
    }
} // namespace

int main()
{
    if (const int result = testRepresentativeLowering(); result != 0)
    {
        return result;
    }
    if (const int result = testExplicitExternalDpiOrderIsPreserved(); result != 0)
    {
        return result;
    }
    if (const int result = testMemoryFillAndPriorityWritesShareStateOrder(); result != 0)
    {
        return result;
    }
    if (const int result = testRegisterChainSamplesPreCommitState(); result != 0)
    {
        return result;
    }
    if (const int result = testShiftWidensOperandBeforeExecution(); result != 0)
    {
        return result;
    }
    return testRejectsXzAndHierarchy();
}
