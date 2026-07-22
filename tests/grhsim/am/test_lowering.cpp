#include "core/grh.hpp"
#include "grhsim/am/lowering.hpp"
#include "grhsim/am/validate.hpp"

#include <algorithm>
#include <iostream>
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
            countOpcode(program, Opcode::ChangedPos) != 4 ||
            countOpcode(program, Opcode::ChangedNeg) != 1)
        {
            return fail("lowered opcode inventory is incomplete");
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
            ordered[1].instruction != dpiCalls[0])
        {
            return fail("explicit external DPI order was omitted or did not follow its ordinals");
        }
        const ValidationResult validation =
            validate(*artifact, ValidationOptions{.level = ValidationLevel::Semantic});
        return validation.success() ? 0 : fail(validation.errors.front());
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
    return testRejectsXzAndHierarchy();
}
