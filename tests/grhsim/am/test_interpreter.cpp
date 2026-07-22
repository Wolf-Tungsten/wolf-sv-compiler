#include "grhsim/am/builder.hpp"
#include "grhsim/am/interpreter.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>

using namespace wolvrix::lib::grhsim::am;

namespace {

    int fail(const std::string &message) {
        std::cerr << "[grhsim_am_interpreter] " << message << '\n';
        return 1;
    }

    InstructionId addInstruction(LinearProgramBuilder &builder, Opcode opcode,
                                 std::initializer_list<VariableId> results,
                                 std::initializer_list<VariableId> operands) {
        return builder.addInstruction(
            opcode,
            std::span<const VariableId>(results.begin(), results.size()),
            std::span<const VariableId>(operands.begin(), operands.size()));
    }

    InstructionId addInstruction(ScheduledProgramBuilder &builder,
                                 Opcode opcode,
                                 std::initializer_list<VariableId> results,
                                 std::initializer_list<VariableId> operands) {
        return builder.addInstruction(
            opcode,
            std::span<const VariableId>(results.begin(), results.size()),
            std::span<const VariableId>(operands.begin(), operands.size()));
    }

    void addBlock(ScheduledProgramBuilder &builder,
                  std::initializer_list<InstructionId> instructions) {
        builder.addBlock(std::span<const InstructionId>(instructions.begin(),
                                                        instructions.size()));
    }

    void setTargets(ScheduledProgramBuilder &builder, InstructionId instruction,
                    std::initializer_list<BlockId> targets) {
        builder.setActivationTargets(
            instruction,
            std::span<const BlockId>(targets.begin(), targets.size()));
    }

    VariableId addConstant(LinearProgramBuilder &builder, TypeId type,
                           uint64_t value) {
        const std::array<uint64_t, 1> words = {value};
        const LiteralId literal = builder.addBitLiteral(type, words);
        return builder.addVariable(type, builder.addConstantInit(literal));
    }

    VariableId addConstant(LinearProgramBuilder &builder, TypeId type,
                           std::span<const uint64_t> words) {
        const LiteralId literal = builder.addBitLiteral(type, words);
        return builder.addVariable(type, builder.addConstantInit(literal));
    }

    InterpreterValue u1(uint64_t value) {
        const std::array<uint64_t, 1> words = {value};
        return InterpreterValue::bitVector(1, Signedness::Unsigned, words);
    }

    InterpreterValue u8(uint64_t value) {
        const std::array<uint64_t, 1> words = {value};
        return InterpreterValue::bitVector(8, Signedness::Unsigned, words);
    }

    struct FeedbackFixture {
        ExecutableModel model;
        VariableId input;
        VariableId entryEvent;
        VariableId midEvent;
        VariableId output;
    };

    FeedbackFixture makeFeedbackModel() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId input = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId inputOld =
            linear.addVariable(u8Type, linear.undefInit());
        const VariableId entryEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId middle = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId middleOld =
            linear.addVariable(u8Type, linear.undefInit());
        const VariableId middleEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId output = linear.addVariable(u8Type, linear.zeroInit());

        const InstructionId inputChanged = addInstruction(
            linear, Opcode::ChangedAny, {entryEvent}, {input, inputOld});
        const InstructionId assignMiddle =
            addInstruction(linear, Opcode::Assign, {middle}, {input});
        const InstructionId middleChanged = addInstruction(
            linear, Opcode::ChangedAny, {middleEvent}, {middle, middleOld});
        const InstructionId assignOutput =
            addInstruction(linear, Opcode::Assign, {output}, {middle});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activateMiddle =
            addInstruction(scheduled, Opcode::ActForward, {}, {entryEvent});
        const InstructionId activateOutput =
            addInstruction(scheduled, Opcode::ActBackward, {}, {middleEvent});
        setTargets(scheduled, activateMiddle, {BlockId{1}});
        setTargets(scheduled, activateOutput, {BlockId{2}});
        addBlock(scheduled, {inputChanged, activateMiddle});
        addBlock(scheduled, {assignMiddle, middleChanged, activateOutput});
        addBlock(scheduled, {assignOutput});

        return FeedbackFixture{
            .model =
                ExecutableModel{
                    .program = scheduled.finish(),
                    .interface = {},
                },
            .input = input,
            .entryEvent = entryEvent,
            .midEvent = middleEvent,
            .output = output,
        };
    }

    int testFeedbackAndEpochLifecycle() {
        FeedbackFixture fixture = makeFeedbackModel();
        Interpreter interpreter(fixture.model);
        if (!interpreter.ready()) {
            return fail("feedback model failed interpreter initialization");
        }
        const InterpreterResult initial = interpreter.eval();
        if (!initial.success() || interpreter.firstEval() ||
            interpreter.value(fixture.output).lowWord() != 0) {
            return fail("first eval did not execute every normal Block");
        }

        if (!interpreter.write(fixture.input, u8(0x35)).success()) {
            return fail("external input write failed");
        }
        const InterpreterResult changed = interpreter.eval();
        if (!changed.success() || changed.epochsExecuted != 2 ||
            interpreter.epochCounter() != 1 ||
            interpreter.value(fixture.output).lowWord() != 0x35) {
            return fail("act.b feedback did not execute in the next epoch");
        }
        if (interpreter.value(fixture.entryEvent).lowWord() != 0 ||
            interpreter.value(fixture.midEvent).lowWord() != 0) {
            return fail("changed results were not cleared on next-epoch entry");
        }

        const InterpreterResult stable = interpreter.eval();
        if (!stable.success() || stable.epochsExecuted != 0 ||
            interpreter.value(fixture.output).lowWord() != 0x35) {
            return fail("stable input unexpectedly activated a normal Block");
        }
        if (interpreter.write(fixture.entryEvent, u1(1)).success()) {
            return fail("external write to a changed-owned event was accepted");
        }
        return 0;
    }

    struct RegisterFixture {
        ExecutableModel model;
        VariableId clock;
        VariableId data;
        VariableId state;
    };

    RegisterFixture makeRegisterModel() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId clock = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId posedge =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId enabled = addConstant(linear, u1Type, 1);
        const VariableId mask = addConstant(linear, u8Type, 0xff);
        const VariableId data = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId state = linear.addVariable(u8Type, linear.zeroInit());

        const InstructionId detect = addInstruction(
            linear, Opcode::ChangedPos, {posedge}, {clock, clockOld});
        const InstructionId write =
            addInstruction(linear, Opcode::RegisterWrite, {},
                           {enabled, mask, data, state, posedge});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activate =
            addInstruction(scheduled, Opcode::ActForward, {}, {posedge});
        setTargets(scheduled, activate, {BlockId{1}});
        addBlock(scheduled, {detect, activate});
        addBlock(scheduled, {write});
        return RegisterFixture{
            .model =
                ExecutableModel{
                    .program = scheduled.finish(),
                    .interface = {},
                },
            .clock = clock,
            .data = data,
            .state = state,
        };
    }

    int testEventDrivenRegister() {
        RegisterFixture fixture = makeRegisterModel();
        Interpreter interpreter(fixture.model);
        if (!interpreter.ready() || !interpreter.eval().success()) {
            return fail("register model failed its initial eval");
        }
        if (!interpreter.write(fixture.data, u8(0xa5)).success() ||
            !interpreter.write(fixture.clock, u1(1)).success() ||
            !interpreter.eval().success() ||
            interpreter.value(fixture.state).lowWord() != 0xa5) {
            return fail("posedge register write did not commit data");
        }
        if (!interpreter.write(fixture.data, u8(0x5a)).success() ||
            !interpreter.eval().success() ||
            interpreter.value(fixture.state).lowWord() != 0xa5) {
            return fail("register changed without a new posedge");
        }
        if (!interpreter.write(fixture.clock, u1(0)).success() ||
            !interpreter.eval().success() ||
            !interpreter.write(fixture.clock, u1(1)).success() ||
            !interpreter.eval().success() ||
            interpreter.value(fixture.state).lowWord() != 0x5a) {
            return fail("second posedge register write did not commit data");
        }
        return 0;
    }

    struct MemoryFixture {
        ExecutableModel model;
        VariableId clock;
        VariableId address;
        VariableId data;
        VariableId memory;
        VariableId readData;
    };

    MemoryFixture makeMemoryModel() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u2Type = linear.addType(Type::bitVector(2));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId memoryType = linear.addType(Type::array(4, 8));
        const VariableId clock = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId posedge =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId enabled = addConstant(linear, u1Type, 1);
        const VariableId address =
            linear.addVariable(u2Type, linear.zeroInit());
        const VariableId mask = addConstant(linear, u8Type, 0xff);
        const VariableId data = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId memory =
            linear.addVariable(memoryType, linear.zeroInit());
        const VariableId readData =
            linear.addVariable(u8Type, linear.zeroInit());

        const InstructionId detect = addInstruction(
            linear, Opcode::ChangedPos, {posedge}, {clock, clockOld});
        const InstructionId write =
            addInstruction(linear, Opcode::MemoryWrite, {},
                           {enabled, address, mask, data, memory, posedge});
        const InstructionId read = addInstruction(
            linear, Opcode::MemoryRead, {readData}, {memory, address});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activate =
            addInstruction(scheduled, Opcode::ActForward, {}, {posedge});
        setTargets(scheduled, activate, {BlockId{1}});
        addBlock(scheduled, {detect, activate});
        addBlock(scheduled, {write, read});
        return MemoryFixture{
            .model =
                ExecutableModel{
                    .program = scheduled.finish(),
                    .interface = {},
                },
            .clock = clock,
            .address = address,
            .data = data,
            .memory = memory,
            .readData = readData,
        };
    }

    int testMemoryWriteRead() {
        MemoryFixture fixture = makeMemoryModel();
        Interpreter interpreter(fixture.model);
        if (!interpreter.ready() || !interpreter.eval().success()) {
            return fail("memory model failed its initial eval");
        }
        const std::array<uint64_t, 1> addressWords = {2};
        const InterpreterValue address =
            InterpreterValue::bitVector(2, Signedness::Unsigned, addressWords);
        if (!interpreter.write(fixture.address, address).success() ||
            !interpreter.write(fixture.data, u8(0xc3)).success() ||
            !interpreter.write(fixture.clock, u1(1)).success() ||
            !interpreter.eval().success()) {
            return fail("clocked memory transaction failed");
        }
        if (interpreter.value(fixture.readData).lowWord() != 0xc3 ||
            interpreter.value(fixture.memory).arrayElementWords(2).front() !=
                0xc3) {
            return fail("memory write/read ordering is incorrect");
        }
        return 0;
    }

    int testPureBitVectorOperations() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId u16Type = linear.addType(Type::bitVector(16));
        const VariableId lhs = addConstant(linear, u8Type, 250);
        const VariableId rhs = addConstant(linear, u8Type, 10);
        const VariableId amount = addConstant(linear, u8Type, 2);
        const VariableId sum = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId product =
            linear.addVariable(u16Type, linear.zeroInit());
        const VariableId quotient =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId remainder =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId less = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId shifted =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId concatenated =
            linear.addVariable(u16Type, linear.zeroInit());
        const VariableId sliced = linear.addVariable(u8Type, linear.zeroInit());

        const InstructionId add =
            addInstruction(linear, Opcode::Add, {sum}, {lhs, rhs});
        const InstructionId multiply =
            addInstruction(linear, Opcode::Mul, {product}, {lhs, rhs});
        const InstructionId divide =
            addInstruction(linear, Opcode::Div, {quotient}, {lhs, rhs});
        const InstructionId modulo =
            addInstruction(linear, Opcode::Mod, {remainder}, {lhs, rhs});
        const InstructionId compare =
            addInstruction(linear, Opcode::Lt, {less}, {lhs, rhs});
        const InstructionId shift =
            addInstruction(linear, Opcode::Shl, {shifted}, {rhs, amount});
        const InstructionId concat =
            addInstruction(linear, Opcode::Concat, {concatenated}, {lhs, rhs});
        const InstructionId slice = addInstruction(linear, Opcode::SliceStatic,
                                                   {sliced}, {concatenated});
        linear.setSliceStaticAttributes(slice, 0);

        ScheduledProgramBuilder scheduled(linear.finish());
        addBlock(scheduled, {});
        addBlock(scheduled, {add, multiply, divide, modulo, compare, shift,
                             concat, slice});
        ExecutableModel model{
            .program = scheduled.finish(),
            .interface = {},
        };
        Interpreter interpreter(model);
        if (!interpreter.ready() || !interpreter.eval().success()) {
            return fail("pure bit-vector model failed its initial eval");
        }
        if (interpreter.value(sum).lowWord() != 4 ||
            interpreter.value(product).lowWord() != 2500 ||
            interpreter.value(quotient).lowWord() != 25 ||
            interpreter.value(remainder).lowWord() != 0 ||
            interpreter.value(less).lowWord() != 0 ||
            interpreter.value(shifted).lowWord() != 40 ||
            interpreter.value(concatenated).lowWord() != 0xfa0a ||
            interpreter.value(sliced).lowWord() != 10) {
            return fail("one or more pure bit-vector operations produced the "
                        "wrong value");
        }
        return 0;
    }

    int testWideAndSignedArithmetic() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId s8Type =
            linear.addType(Type::bitVector(8, Signedness::Signed));
        const TypeId u65Type = linear.addType(Type::bitVector(65));
        const TypeId u130Type = linear.addType(Type::bitVector(130));
        const std::array<uint64_t, 2> wideLhsWords = {3, 1};
        const std::array<uint64_t, 2> wideRhsWords = {5, 1};
        const VariableId wideLhs = addConstant(linear, u65Type, wideLhsWords);
        const VariableId wideRhs = addConstant(linear, u65Type, wideRhsWords);
        const VariableId negativeSeven = addConstant(linear, s8Type, 0xf9);
        const VariableId positiveThree = addConstant(linear, s8Type, 3);
        const VariableId product =
            linear.addVariable(u130Type, linear.zeroInit());
        const VariableId quotient =
            linear.addVariable(u130Type, linear.zeroInit());
        const VariableId signedQuotient =
            linear.addVariable(s8Type, linear.zeroInit());
        const VariableId signedRemainder =
            linear.addVariable(s8Type, linear.zeroInit());
        const VariableId signedLess =
            linear.addVariable(u1Type, linear.zeroInit());

        const InstructionId multiply =
            addInstruction(linear, Opcode::Mul, {product}, {wideLhs, wideRhs});
        const InstructionId divide =
            addInstruction(linear, Opcode::Div, {quotient}, {product, wideLhs});
        const InstructionId signedDivide =
            addInstruction(linear, Opcode::Div, {signedQuotient},
                           {negativeSeven, positiveThree});
        const InstructionId signedModulo =
            addInstruction(linear, Opcode::Mod, {signedRemainder},
                           {negativeSeven, positiveThree});
        const InstructionId signedCompare = addInstruction(
            linear, Opcode::Lt, {signedLess}, {negativeSeven, positiveThree});

        ScheduledProgramBuilder scheduled(linear.finish());
        addBlock(scheduled, {});
        addBlock(scheduled,
                 {multiply, divide, signedDivide, signedModulo, signedCompare});
        ExecutableModel model{
            .program = scheduled.finish(),
            .interface = {},
        };
        Interpreter interpreter(model);
        if (!interpreter.ready() || !interpreter.eval().success()) {
            return fail("wide arithmetic model failed its initial eval");
        }
        const auto productWords = interpreter.value(product).words();
        const auto quotientWords = interpreter.value(quotient).words();
        if (productWords.size() != 3 || productWords[0] != 15 ||
            productWords[1] != 8 || productWords[2] != 1 ||
            quotientWords.size() != 3 || quotientWords[0] != 5 ||
            quotientWords[1] != 1 || quotientWords[2] != 0 ||
            interpreter.value(signedQuotient).lowWord() != 0xfe ||
            interpreter.value(signedRemainder).lowWord() != 0xff ||
            interpreter.value(signedLess).lowWord() != 1) {
            return fail("wide or signed arithmetic produced the wrong value");
        }
        return 0;
    }

    class CountingHost final : public HostEnvironment {
    public:
        bool resolveSystemFunction(ProgramView, InstructionId,
                                   std::string &) override {
            return true;
        }

        bool invokeSystemFunction(ProgramView, InstructionId,
                                  std::span<const InterpreterValue>,
                                  InterpreterValue &result,
                                  std::string &) override {
            ++calls;
            result = u8(calls);
            return true;
        }

        uint64_t calls = 0;
    };

    int testInjectedHostAndOnceSchedule() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const StringId name = linear.addString("reference_only");
        const VariableId trigger =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId triggerOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId event = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId result = linear.addVariable(u8Type, linear.zeroInit());
        const InstructionId changed = addInstruction(
            linear, Opcode::ChangedAny, {event}, {trigger, triggerOld});
        const InstructionId call =
            addInstruction(linear, Opcode::SystemFunction, {result}, {});
        linear.setSystemFunctionAttributes(call,
                                           SystemFunctionAttributes{
                                               .name = name,
                                               .schedule = CallSchedule::Once,
                                               .hasSideEffects = false,
                                           });
        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activate =
            addInstruction(scheduled, Opcode::ActForward, {}, {event});
        setTargets(scheduled, activate, {BlockId{1}});
        addBlock(scheduled, {changed, activate});
        addBlock(scheduled, {call});
        ExecutableModel model{
            .program = scheduled.finish(),
            .interface = {},
        };
        CountingHost host;
        Interpreter interpreter(model, &host);
        if (!interpreter.ready() || !interpreter.eval().success() ||
            host.calls != 1 || interpreter.value(result).lowWord() != 1) {
            return fail(
                "injected HostEnvironment did not execute a system function");
        }
        if (!interpreter.write(trigger, u1(1)).success() ||
            !interpreter.eval().success() || host.calls != 1 ||
            interpreter.value(result).lowWord() != 1) {
            return fail(
                "schedule=once system function executed more than once");
        }
        return 0;
    }

    int testMissingHostBindingDiagnostic() {
        LinearProgramBuilder linear;
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const StringId name = linear.addString("reference_only");
        const VariableId result = linear.addVariable(u8Type, linear.zeroInit());
        const InstructionId call =
            addInstruction(linear, Opcode::SystemFunction, {result}, {});
        linear.setSystemFunctionAttributes(call,
                                           SystemFunctionAttributes{
                                               .name = name,
                                               .schedule = CallSchedule::Normal,
                                               .hasSideEffects = false,
                                           });
        ScheduledProgramBuilder scheduled(linear.finish());
        addBlock(scheduled, {});
        addBlock(scheduled, {call});
        ExecutableModel model{
            .program = scheduled.finish(),
            .interface = {},
        };
        Interpreter interpreter(model);
        if (interpreter.ready() || !interpreter.initializationDiagnostic() ||
            interpreter.initializationDiagnostic()->code !=
                InterpreterErrorCode::MissingHostBinding ||
            interpreter.initializationDiagnostic()->instruction != call) {
            return fail(
                "missing HostEnvironment binding did not produce a contextual "
                "diagnostic");
        }
        return 0;
    }

} // namespace

int main() {
    if (testFeedbackAndEpochLifecycle() != 0)
        return 1;
    if (testEventDrivenRegister() != 0)
        return 1;
    if (testMemoryWriteRead() != 0)
        return 1;
    if (testPureBitVectorOperations() != 0)
        return 1;
    if (testWideAndSignedArithmetic() != 0)
        return 1;
    if (testInjectedHostAndOnceSchedule() != 0)
        return 1;
    if (testMissingHostBindingDiagnostic() != 0)
        return 1;
    return 0;
}
