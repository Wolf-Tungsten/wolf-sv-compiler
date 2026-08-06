#include "grhsim/am/builder.hpp"
#include "grhsim/am/graph.hpp"
#include "grhsim/am/pipeline.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim_am_graph] " << message << '\n';
        return 1;
    }

    bool programsEqual(ProgramView lhs, ProgramView rhs, std::string &where)
    {
        if (lhs.typeCount() != rhs.typeCount())
        {
            where = "type count";
            return false;
        }
        for (uint32_t index = 0; index < lhs.typeCount(); ++index)
        {
            if (!(lhs.type(TypeId{index}) == rhs.type(TypeId{index})))
            {
                where = "type " + std::to_string(index);
                return false;
            }
        }
        if (lhs.stringCount() != rhs.stringCount())
        {
            where = "string count";
            return false;
        }
        for (uint32_t index = 0; index < lhs.stringCount(); ++index)
        {
            if (lhs.string(StringId{index}) != rhs.string(StringId{index}))
            {
                where = "string " + std::to_string(index);
                return false;
            }
        }
        if (lhs.initCount() != rhs.initCount() || lhs.literalCount() != rhs.literalCount())
        {
            where = "init/literal count";
            return false;
        }
        for (uint32_t index = 0; index < lhs.literalCount(); ++index)
        {
            const LiteralView a = lhs.literal(LiteralId{index});
            const LiteralView b = rhs.literal(LiteralId{index});
            if (a.type != b.type || a.words.size() != b.words.size() ||
                a.bytes.size() != b.bytes.size())
            {
                where = "literal " + std::to_string(index);
                return false;
            }
            for (std::size_t word = 0; word < a.words.size(); ++word)
            {
                if (a.words[word] != b.words[word])
                {
                    where = "literal words " + std::to_string(index);
                    return false;
                }
            }
        }
        if (lhs.variableCount() != rhs.variableCount() ||
            lhs.instructionCount() != rhs.instructionCount())
        {
            where = "variable/instruction count";
            return false;
        }
        for (uint32_t index = 0; index < lhs.variableCount(); ++index)
        {
            const VariableRecord &a = lhs.variable(VariableId{index});
            const VariableRecord &b = rhs.variable(VariableId{index});
            if (a.type != b.type || a.init != b.init)
            {
                where = "variable " + std::to_string(index);
                return false;
            }
        }
        const auto lhsLabels = lhs.variableLabels();
        const auto rhsLabels = rhs.variableLabels();
        if (lhsLabels.size() != rhsLabels.size())
        {
            where = "variable label count";
            return false;
        }
        for (std::size_t index = 0; index < lhsLabels.size(); ++index)
        {
            if (lhsLabels[index].variable != rhsLabels[index].variable ||
                lhsLabels[index].label != rhsLabels[index].label)
            {
                where = "variable label " + std::to_string(index);
                return false;
            }
        }
        for (uint32_t index = 0; index < lhs.instructionCount(); ++index)
        {
            const InstructionId id{index};
            if (lhs.opcode(id) != rhs.opcode(id))
            {
                where = "opcode " + std::to_string(index);
                return false;
            }
            const auto aOps = lhs.operands(id);
            const auto bOps = rhs.operands(id);
            const auto aRes = lhs.results(id);
            const auto bRes = rhs.results(id);
            if (aOps.size() != bOps.size() || aRes.size() != bRes.size())
            {
                where = "arity " + std::to_string(index);
                return false;
            }
            for (std::size_t position = 0; position < aOps.size(); ++position)
            {
                if (aOps[position] != bOps[position])
                {
                    where = "operand " + std::to_string(index);
                    return false;
                }
            }
            for (std::size_t position = 0; position < aRes.size(); ++position)
            {
                if (aRes[position] != bRes[position])
                {
                    where = "result " + std::to_string(index);
                    return false;
                }
            }
            const auto aSlice = lhs.sliceStaticAttributes(id);
            const auto bSlice = rhs.sliceStaticAttributes(id);
            if (aSlice.has_value() != bSlice.has_value() ||
                (aSlice && aSlice->lsb != bSlice->lsb))
            {
                where = "slice attrs " + std::to_string(index);
                return false;
            }
            const auto aFn = lhs.systemFunctionAttributes(id);
            const auto bFn = rhs.systemFunctionAttributes(id);
            if (aFn.has_value() != bFn.has_value() ||
                (aFn && (aFn->name != bFn->name || aFn->schedule != bFn->schedule ||
                         aFn->hasSideEffects != bFn->hasSideEffects)))
            {
                where = "system-function attrs " + std::to_string(index);
                return false;
            }
            const auto aTask = lhs.systemTaskAttributes(id);
            const auto bTask = rhs.systemTaskAttributes(id);
            if (aTask.has_value() != bTask.has_value() ||
                (aTask && (aTask->name != bTask->name ||
                           aTask->eventCount != bTask->eventCount ||
                           aTask->schedule != bTask->schedule)))
            {
                where = "system-task attrs " + std::to_string(index);
                return false;
            }
            const auto aDpi = lhs.dpiCallAttributes(id);
            const auto bDpi = rhs.dpiCallAttributes(id);
            if (aDpi.has_value() != bDpi.has_value() ||
                (aDpi && (aDpi->importSymbol != bDpi->importSymbol ||
                          aDpi->eventCount != bDpi->eventCount)))
            {
                where = "dpi-call attrs " + std::to_string(index);
                return false;
            }
        }
        if (lhs.dpiImportCount() != rhs.dpiImportCount())
        {
            where = "dpi import count";
            return false;
        }
        for (uint32_t index = 0; index < lhs.dpiImportCount(); ++index)
        {
            const DpiImportView a = lhs.dpiImport(DpiImportId{index});
            const DpiImportView b = rhs.dpiImport(DpiImportId{index});
            if (a.symbol != b.symbol || a.parameters.size() != b.parameters.size() ||
                a.returnValue.present != b.returnValue.present)
            {
                where = "dpi import " + std::to_string(index);
                return false;
            }
        }
        return true;
    }

    bool factsEqual(const SchedulingFacts &lhs, const SchedulingFacts &rhs)
    {
        if (lhs.variableRoles != rhs.variableRoles ||
            lhs.instructionEffects != rhs.instructionEffects ||
            lhs.orderedEffects.size() != rhs.orderedEffects.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < lhs.orderedEffects.size(); ++index)
        {
            const OrderedEffect &a = lhs.orderedEffects[index];
            const OrderedEffect &b = rhs.orderedEffects[index];
            if (a.instruction != b.instruction || a.group != b.group || a.ordinal != b.ordinal)
            {
                return false;
            }
        }
        return true;
    }

    // Builds an artifact touching every table: two types, several strings, a
    // constant with a wide literal, input/state/observable variables, pure
    // and effectful instructions with each attribute flavor, and an ordered
    // effect group chaining two writes.
    LinearProgramArtifact makeRichArtifact()
    {
        LinearProgramBuilder builder;
        const TypeId bit1 = builder.addType(Type::bitVector(1));
        const TypeId bit64 = builder.addType(Type::bitVector(64));
        const TypeId memType = builder.addType(Type::array(16, 64));
        const TypeId stringType = builder.addType(Type::string());

        const StringId clockName = builder.addString("clock");
        const StringId resetName = builder.addString("reset");
        const StringId regLabel = builder.addString("top.acc");
        const StringId memLabel = builder.addString("top.ram");
        const StringId funcName = builder.addString("$display");
        const StringId dpiSymbol = builder.addString("my_dpi");

        const uint64_t words[] = {0xdeadbeef};
        const LiteralId literal = builder.addBitLiteral(bit64, words);
        const InitId constInit = builder.addConstantInit(literal);

        const VariableId clock = builder.addVariable(bit1, builder.zeroInit(), clockName);
        const VariableId reset = builder.addVariable(bit1, builder.zeroInit(), resetName);
        const VariableId constant = builder.addVariable(bit64, constInit, std::nullopt);
        const VariableId acc = builder.addVariable(bit64, builder.zeroInit(), regLabel);
        const VariableId ram = builder.addVariable(memType, builder.undefInit(), memLabel);
        const VariableId sum = builder.addVariable(bit64, builder.undefInit(), std::nullopt);
        const VariableId text = builder.addVariable(stringType, builder.undefInit(), std::nullopt);

        const InstructionId add =
            builder.addInstruction(Opcode::Add, {&sum, 1}, std::array{acc, constant});
        const InstructionId slice = builder.addInstruction(Opcode::SliceStatic, {&sum, 1},
                                                           std::array{sum});
        builder.setSliceStaticAttributes(slice, 3);
        const InstructionId regWrite = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{sum, acc, clock});
        const InstructionId memWrite = builder.addInstruction(
            Opcode::MemoryWrite, {}, std::array{constant, sum, ram, clock});
        const InstructionId display = builder.addInstruction(Opcode::SystemTask, {},
                                                             std::array{constant, clock});
        builder.setSystemTaskAttributes(display, SystemTaskAttributes{
                                                     .name = funcName,
                                                     .eventCount = 1,
                                                     .schedule = CallSchedule::Normal,
                                                 });

        const DpiParameter param{
            .name = funcName,
            .type = bit64,
            .direction = DpiDirection::Input,
            .abi = DpiAbiKind::Integral,
        };
        const DpiImportId import =
            builder.addDpiImport(dpiSymbol, {&param, 1}, DpiReturn{});
        const InstructionId call = builder.addInstruction(Opcode::DpiCall, {}, std::array{sum});
        builder.setDpiCallAttributes(call, DpiCallAttributes{
                                               .importSymbol = dpiSymbol,
                                               .eventCount = 0,
                                           });
        (void)add;
        (void)regWrite;
        (void)memWrite;
        (void)import;

        ProgramInterface interface;
        interface.ports.push_back(PortBinding{
            .name = clockName,
            .direction = PortDirection::Input,
            .input = clock,
        });
        interface.ports.push_back(PortBinding{
            .name = resetName,
            .direction = PortDirection::Input,
            .input = reset,
        });
        interface.declaredVariables.push_back(VariableLabel{.variable = acc, .label = regLabel});
        interface.declaredVariables.push_back(VariableLabel{.variable = ram, .label = memLabel});

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput | VariableRole::Observable,
            VariableRole::ExternalInput,
            VariableRole::None,
            VariableRole::State | VariableRole::Observable,
            VariableRole::State,
            VariableRole::None,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,          InstructionEffect::Pure,
            InstructionEffect::StateReadWrite, InstructionEffect::StateReadWrite,
            InstructionEffect::HostEffect,    InstructionEffect::HostEffect,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = regWrite, .group = 0, .ordinal = 0},
            OrderedEffect{.instruction = memWrite, .group = 0, .ordinal = 1},
        };

        return LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };
    }

    int testRoundTripLossless()
    {
        LinearProgramArtifact original = makeRichArtifact();
        AmGraph graph = AmGraph::fromLinearProgram(original);
        LinearProgramArtifact restored = graph.toLinearProgram();

        std::string where;
        if (!programsEqual(original.program.view(), restored.program.view(), where))
        {
            return fail("round trip mismatch at " + where);
        }
        if (!factsEqual(original.schedulingFacts, restored.schedulingFacts))
        {
            return fail("scheduling facts mismatch after round trip");
        }
        if (restored.interface.ports.size() != original.interface.ports.size() ||
            restored.interface.declaredVariables.size() !=
                original.interface.declaredVariables.size())
        {
            return fail("interface size mismatch after round trip");
        }

        // Value classification must survive: clock is an input, acc/ram are
        // state (register vs memory), the constant is a constant.
        if (graph.valueFacts(VariableId{0}).kind != AmValueKind::Input ||
            graph.valueFacts(VariableId{1}).kind != AmValueKind::Input ||
            graph.valueFacts(VariableId{2}).kind != AmValueKind::Constant ||
            graph.valueFacts(VariableId{3}).kind != AmValueKind::State ||
            graph.valueFacts(VariableId{4}).kind != AmValueKind::State)
        {
            return fail("value kind classification mismatch");
        }
        if (graph.valueFacts(VariableId{3}).stateKind != AmStateKind::Register ||
            graph.valueFacts(VariableId{4}).stateKind != AmStateKind::Memory)
        {
            return fail("state kind classification mismatch");
        }
        return 0;
    }

    int testTombstoneCompaction()
    {
        LinearProgramBuilder builder;
        const TypeId bit1 = builder.addType(Type::bitVector(1));
        const StringId aName = builder.addString("a");
        const VariableId input = builder.addVariable(bit1, builder.zeroInit(), aName);
        const VariableId state = builder.addVariable(bit1, builder.zeroInit(), std::nullopt);
        const VariableId net = builder.addVariable(bit1, builder.undefInit(), std::nullopt);
        const VariableId net2 = builder.addVariable(bit1, builder.undefInit(), std::nullopt);
        const InstructionId keep0 =
            builder.addInstruction(Opcode::Assign, {&net, 1}, std::array{input});
        const InstructionId drop =
            builder.addInstruction(Opcode::Not, {&net2, 1}, std::array{net});
        const InstructionId keep1 =
            builder.addInstruction(Opcode::RegisterWrite, {}, std::array{net2, state, input});

        SchedulingFacts facts;
        facts.variableRoles = {VariableRole::ExternalInput, VariableRole::State,
                               VariableRole::None, VariableRole::None};
        facts.instructionEffects = {InstructionEffect::Pure, InstructionEffect::Pure,
                                    InstructionEffect::StateReadWrite};
        facts.orderedEffects = {
            OrderedEffect{.instruction = keep1, .group = 0, .ordinal = 0},
        };
        LinearProgramArtifact original{
            .program = builder.finish(),
            .interface = ProgramInterface{},
            .schedulingFacts = std::move(facts),
        };
        (void)keep0;

        AmGraph graph = AmGraph::fromLinearProgram(original);
        graph.removeInstruction(drop);
        LinearProgramArtifact compacted = graph.toLinearProgram();
        ProgramView program = compacted.program.view();
        if (program.instructionCount() != 2)
        {
            return fail("tombstone compaction kept a removed instruction");
        }
        if (program.opcode(InstructionId{0}) != Opcode::Assign ||
            program.opcode(InstructionId{1}) != Opcode::RegisterWrite)
        {
            return fail("tombstone compaction changed instruction order");
        }
        if (compacted.schedulingFacts.instructionEffects.size() != 2 ||
            compacted.schedulingFacts.instructionEffects[1] !=
                InstructionEffect::StateReadWrite)
        {
            return fail("effects not compacted in lockstep");
        }
        if (compacted.schedulingFacts.orderedEffects.size() != 1 ||
            compacted.schedulingFacts.orderedEffects.front().instruction.value != 1)
        {
            return fail("ordered effect not remapped after compaction");
        }
        return 0;
    }

    int testMutationApi()
    {
        LinearProgramArtifact original = makeRichArtifact();
        AmGraph graph = AmGraph::fromLinearProgram(original);
        const VariableId fresh =
            graph.addVariable(graph.program().typeCount()
                                  ? TypeId{0}
                                  : TypeId{0},
                              graph.program().initCount() ? InitId{1} : InitId{1},
                              std::nullopt, AmGraph::ValueFacts{});
        if (!fresh.valid() || fresh.value != original.program.view().variableCount())
        {
            return fail("addVariable did not append densely");
        }
        const InstructionId added = graph.addInstruction(Opcode::Assign, {&fresh, 1},
                                                         std::array{VariableId{0}});
        if (!added.valid() || added.value != original.program.view().instructionCount())
        {
            return fail("addInstruction did not append densely");
        }
        graph.setInstructionOperand(added, 0, VariableId{1});
        if (graph.program().operands(added).front() != VariableId{1})
        {
            return fail("setInstructionOperand did not rewire");
        }
        if (graph.stateAccess(added, 0) != AmStateAccess::PreCommit)
        {
            return fail("state access default is not PreCommit");
        }
        graph.setStateAccess(added, 0, AmStateAccess::Live);
        if (graph.stateAccess(added, 0) != AmStateAccess::Live)
        {
            return fail("state access set/get mismatch");
        }
        graph.setStateAccess(added, 0, AmStateAccess::PreCommit);
        if (graph.stateAccess(added, 0) != AmStateAccess::PreCommit)
        {
            return fail("state access reset mismatch");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int rc = testRoundTripLossless())
    {
        return rc;
    }
    if (const int rc = testTombstoneCompaction())
    {
        return rc;
    }
    if (const int rc = testMutationApi())
    {
        return rc;
    }
    return 0;
}
