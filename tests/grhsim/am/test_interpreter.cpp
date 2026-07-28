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
        VariableId stateEvent;
        VariableId output;
    };

    // input -> (B1) middle -> commit write -> state -> (B2) output.  The
    // commit Block's change detector fires act.b to re-activate the reader
    // compute Block on the next round.
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
        const VariableId state = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId stateOld =
            linear.addVariable(u8Type, linear.undefInit());
        const VariableId stateEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId output = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId one = addConstant(linear, u1Type, 1);
        const VariableId mask = addConstant(linear, u8Type, 0xff);

        const InstructionId inputChanged = addInstruction(
            linear, Opcode::ChangedAny, {entryEvent}, {input, inputOld});
        const InstructionId assignMiddle =
            addInstruction(linear, Opcode::Assign, {middle}, {input});
        const InstructionId assignOutput =
            addInstruction(linear, Opcode::Assign, {output}, {state});
        const InstructionId writeState =
            addInstruction(linear, Opcode::RegisterWrite, {},
                           {one, mask, middle, state, one});
        const InstructionId detectState = addInstruction(
            linear, Opcode::ChangedAny, {stateEvent}, {state, stateOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activateMiddle =
            addInstruction(scheduled, Opcode::ActForward, {}, {entryEvent});
        const InstructionId activateOutput =
            addInstruction(scheduled, Opcode::ActBackward, {}, {stateEvent});
        setTargets(scheduled, activateMiddle, {BlockId{1}});
        setTargets(scheduled, activateOutput, {BlockId{2}});
        addBlock(scheduled, {inputChanged, activateMiddle});
        addBlock(scheduled, {assignMiddle});
        addBlock(scheduled, {assignOutput});
        addBlock(scheduled, {writeState, detectState, activateOutput});

        return FeedbackFixture{
            .model =
                ExecutableModel{
                    .program = scheduled.finish(),
                    .interface = {},
                    .commitBlockBegin = 3,
                    .commitBlockEnd = 4,
                },
            .input = input,
            .entryEvent = entryEvent,
            .stateEvent = stateEvent,
            .output = output,
        };
    }

    int testFeedbackAndRoundLifecycle() {
        FeedbackFixture fixture = makeFeedbackModel();
        Interpreter interpreter(fixture.model);
        if (!interpreter.ready()) {
            return fail("feedback model failed interpreter initialization");
        }
        const InterpreterResult initial = interpreter.eval();
        if (!initial.success() || interpreter.firstEval() ||
            interpreter.value(fixture.output).lowWord() != 0) {
            return fail("first eval did not execute every compute Block");
        }

        if (!interpreter.write(fixture.input, u8(0x35)).success()) {
            return fail("external input write failed");
        }
        const InterpreterResult changed = interpreter.eval();
        if (!changed.success() || changed.roundsExecuted != 2 ||
            interpreter.roundCounter() != 2 ||
            interpreter.value(fixture.output).lowWord() != 0x35) {
            return fail("act.b feedback did not execute in the next round");
        }
        // The commit detector rewrites its event on every round; the B0 event
        // is only consumed inside B0, so it persists until the next eval
        // re-runs B0.
        if (interpreter.value(fixture.entryEvent).lowWord() != 1 ||
            interpreter.value(fixture.stateEvent).lowWord() != 0) {
            return fail("round-local changed results have the wrong lifetime");
        }

        const InterpreterResult stable = interpreter.eval();
        if (!stable.success() || stable.roundsExecuted != 1 ||
            interpreter.value(fixture.output).lowWord() != 0x35 ||
            interpreter.value(fixture.entryEvent).lowWord() != 0) {
            return fail("stable input unexpectedly activated a compute Block");
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
        addBlock(scheduled, {detect});
        addBlock(scheduled, {write});
        return RegisterFixture{
            .model =
                ExecutableModel{
                    .program = scheduled.finish(),
                    .interface = {},
                    .commitBlockBegin = 1,
                    .commitBlockEnd = 2,
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
        addBlock(scheduled, {detect});
        addBlock(scheduled, {write, read});
        return MemoryFixture{
            .model =
                ExecutableModel{
                    .program = scheduled.finish(),
                    .interface = {},
                    .commitBlockBegin = 1,
                    .commitBlockEnd = 2,
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

    class EventTaskCountingHost final : public HostEnvironment {
    public:
        bool resolveSystemTask(ProgramView, InstructionId,
                               std::string &) override {
            return true;
        }

        bool invokeSystemTask(ProgramView, InstructionId,
                              std::span<const InterpreterValue> arguments,
                              std::string &error) override {
            if (!arguments.empty()) {
                error = "eventful task unexpectedly received arguments";
                return false;
            }
            ++calls;
            return true;
        }

        uint64_t calls = 0;
    };

    struct EventfulTaskFixture {
        ExecutableModel model;
        VariableId clock;
        VariableId event;
        VariableId guard;
    };

    // The commit Block raises the guard state one round after the edge and
    // its change detector re-activates the compute Block carrying the task.
    EventfulTaskFixture makeEventfulTaskModel(bool guardStartsLow,
                                              HostEventMode eventMode) {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const StringId taskName = linear.addString("eventful_task");
        const VariableId clock = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId event = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId one = addConstant(linear, u1Type, 1);
        const VariableId guardState =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId guardStateOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId guardEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId guard = guardStartsLow ? guardState : one;

        const InstructionId changed = addInstruction(
            linear, Opcode::ChangedPos, {event}, {clock, clockOld});
        const InstructionId task =
            addInstruction(linear, Opcode::SystemTask, {}, {guard, event});
        linear.setSystemTaskAttributes(
            task, SystemTaskAttributes{
                     .name = taskName,
                     .eventCount = 1,
                     .schedule = CallSchedule::Normal,
                     .eventMode = eventMode,
                 });
        const InstructionId writeGuard =
            addInstruction(linear, Opcode::RegisterWrite, {},
                           {one, one, one, guardState, one});
        const InstructionId detectGuard =
            addInstruction(linear, Opcode::ChangedAny, {guardEvent},
                           {guardState, guardStateOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId reactivate =
            addInstruction(scheduled, Opcode::ActBackward, {}, {guardEvent});
        setTargets(scheduled, reactivate, {BlockId{1}});
        addBlock(scheduled, {});
        addBlock(scheduled, {changed, task});
        addBlock(scheduled, {writeGuard, detectGuard, reactivate});

        return EventfulTaskFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = {},
                .commitBlockBegin = 2,
                .commitBlockEnd = 3,
            },
            .clock = clock,
            .event = event,
            .guard = guard,
        };
    }

    int testEventfulHostPendingAcrossRounds() {
        // The first case raises the guard only after the eventful task has
        // observed the edge.  The task must retain the event into round 1.
        {
            EventfulTaskFixture fixture =
                makeEventfulTaskModel(true, HostEventMode::Pending);
            EventTaskCountingHost host;
            Interpreter interpreter(fixture.model, &host);
            if (!interpreter.ready()) {
                return fail("eventful task fixture initialization failed: " +
                            (interpreter.initializationDiagnostic()
                                 ? interpreter.initializationDiagnostic()->message
                                 : std::string("unknown error")));
            }
            if (!interpreter.write(fixture.clock, u1(1)).success()) {
                return fail("eventful task fixture clock write failed");
            }
            const InterpreterResult result = interpreter.eval();
            if (!result.success() || result.roundsExecuted != 2 ||
                interpreter.roundCounter() != 2 || host.calls != 1 ||
                interpreter.value(fixture.event).lowWord() != 0 ||
                interpreter.value(fixture.guard).lowWord() != 1) {
                return fail("eventful task did not carry an edge across rounds");
            }
        }

        // If the guard is already true, the round-0 invocation must consume
        // the pending event and not fire again when the block is reactivated.
        {
            EventfulTaskFixture fixture =
                makeEventfulTaskModel(false, HostEventMode::Pending);
            EventTaskCountingHost host;
            Interpreter interpreter(fixture.model, &host);
            if (!interpreter.ready()) {
                return fail("eventful task repeat fixture initialization failed: " +
                            (interpreter.initializationDiagnostic()
                                 ? interpreter.initializationDiagnostic()->message
                                 : std::string("unknown error")));
            }
            if (!interpreter.write(fixture.clock, u1(1)).success()) {
                return fail("eventful task repeat fixture clock write failed");
            }
            const InterpreterResult result = interpreter.eval();
            if (!result.success() || result.roundsExecuted != 2 ||
                host.calls != 1 || interpreter.value(fixture.event).lowWord() != 0) {
                return fail("eventful task repeated after consuming its edge");
            }
        }
        return 0;
    }

    int testImmediateHostEventIsNotReplayed() {
        EventfulTaskFixture fixture =
            makeEventfulTaskModel(true, HostEventMode::Immediate);
        EventTaskCountingHost host;
        Interpreter interpreter(fixture.model, &host);
        if (!interpreter.ready() ||
            !interpreter.write(fixture.clock, u1(1)).success()) {
            return fail("immediate event fixture initialization failed");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.roundsExecuted != 2 ||
            host.calls != 0 || interpreter.value(fixture.event).lowWord() != 0 ||
            interpreter.value(fixture.guard).lowWord() != 1) {
            return fail("immediate host event was replayed after its round");
        }
        return 0;
    }

    struct MultiEventTaskFixture {
        ExecutableModel model;
        std::array<VariableId, 2> clocks;
        std::array<VariableId, 2> events;
    };

    MultiEventTaskFixture makeMultiEventTaskModel() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const StringId taskName = linear.addString("multi_event_task");
        const VariableId clock0 =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clock0Old =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId event0 =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clock1 =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clock1Old =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId event1 =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId one = addConstant(linear, u1Type, 1);

        const InstructionId changed0 = addInstruction(
            linear, Opcode::ChangedPos, {event0}, {clock0, clock0Old});
        const InstructionId changed1 = addInstruction(
            linear, Opcode::ChangedPos, {event1}, {clock1, clock1Old});
        const InstructionId task = addInstruction(
            linear, Opcode::SystemTask, {}, {one, event0, event1});
        linear.setSystemTaskAttributes(
            task, SystemTaskAttributes{
                      .name = taskName,
                      .eventCount = 2,
                      .schedule = CallSchedule::Normal,
                  });

        ScheduledProgramBuilder scheduled(linear.finish());
        addBlock(scheduled, {});
        addBlock(scheduled, {changed0, changed1, task});
        return MultiEventTaskFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = {},
            },
            .clocks = {clock0, clock1},
            .events = {event0, event1},
        };
    }

    int testEventfulHostMultipleEvents() {
        {
            MultiEventTaskFixture fixture = makeMultiEventTaskModel();
            EventTaskCountingHost host;
            Interpreter interpreter(fixture.model, &host);
            if (!interpreter.ready() ||
                !interpreter.write(fixture.clocks[1], u1(1)).success()) {
                return fail("multi-event second-edge fixture initialization failed");
            }
            const InterpreterResult result = interpreter.eval();
            if (!result.success() || host.calls != 1 ||
                interpreter.value(fixture.events[0]).lowWord() != 0 ||
                interpreter.value(fixture.events[1]).lowWord() != 1) {
                return fail("second event did not trigger an eventful task");
            }
        }

        {
            MultiEventTaskFixture fixture = makeMultiEventTaskModel();
            EventTaskCountingHost host;
            Interpreter interpreter(fixture.model, &host);
            if (!interpreter.ready() ||
                !interpreter.write(fixture.clocks[0], u1(1)).success() ||
                !interpreter.write(fixture.clocks[1], u1(1)).success()) {
                return fail("multi-event simultaneous fixture initialization failed");
            }
            const InterpreterResult result = interpreter.eval();
            if (!result.success() || host.calls != 1 ||
                interpreter.value(fixture.events[0]).lowWord() != 1 ||
                interpreter.value(fixture.events[1]).lowWord() != 1) {
                return fail("simultaneous events invoked a task more than once");
            }
        }
        return 0;
    }

    class EventDpiHost final : public HostEnvironment {
    public:
        bool resolveDpiCall(ProgramView, InstructionId,
                            std::string &) override {
            return true;
        }

        bool invokeDpiCall(ProgramView, InstructionId,
                           std::span<const InterpreterValue> arguments,
                           std::vector<InterpreterValue> &results,
                           std::string &error) override {
            if (arguments.size() != 1) {
                error = "eventful DPI call received the wrong argument count";
                return false;
            }
            ++calls;
            lastArgument = arguments[0].lowWord();
            results.push_back(u8(lastArgument + 1));
            return true;
        }

        uint64_t calls = 0;
        uint64_t lastArgument = 0;
    };

    struct EventfulDpiFixture {
        ExecutableModel model;
        VariableId clock;
        VariableId event;
        VariableId guard;
        VariableId argument;
        VariableId result;
    };

    // The commit Block publishes the argument and raises the guard one round
    // after the edge; its change detector re-activates the compute Block.
    EventfulDpiFixture makeEventfulDpiModel() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const StringId symbol = linear.addString("eventful_dpi");
        const StringId argumentName = linear.addString("argument");
        const std::array<DpiParameter, 1> parameters = {
            DpiParameter{
                .name = argumentName,
                .type = u8Type,
                .direction = DpiDirection::Input,
                .abi = DpiAbiKind::Integral,
            },
        };
        linear.addDpiImport(symbol, parameters,
                            DpiReturn{
                                .type = u8Type,
                                .abi = DpiAbiKind::Integral,
                                .present = true,
                            });

        const VariableId clock = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId event = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId guard = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId guardOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId guardEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId argument =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId result = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId one = addConstant(linear, u1Type, 1);
        const VariableId mask = addConstant(linear, u8Type, 0xff);
        const VariableId nextArgument = addConstant(linear, u8Type, 0x2a);

        const InstructionId changed = addInstruction(
            linear, Opcode::ChangedPos, {event}, {clock, clockOld});
        const InstructionId call = addInstruction(
            linear, Opcode::DpiCall, {result}, {guard, argument, event});
        linear.setDpiCallAttributes(
            call, DpiCallAttributes{
                      .importSymbol = symbol,
                      .eventCount = 1,
                      .eventMode = HostEventMode::Pending,
                  });
        const InstructionId setArgument =
            addInstruction(linear, Opcode::RegisterWrite, {},
                           {one, mask, nextArgument, argument, one});
        const InstructionId setGuard =
            addInstruction(linear, Opcode::RegisterWrite, {},
                           {one, one, one, guard, one});
        const InstructionId detectGuard = addInstruction(
            linear, Opcode::ChangedAny, {guardEvent}, {guard, guardOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId reactivate =
            addInstruction(scheduled, Opcode::ActBackward, {}, {guardEvent});
        setTargets(scheduled, reactivate, {BlockId{1}});
        addBlock(scheduled, {});
        addBlock(scheduled, {changed, call});
        addBlock(scheduled, {setArgument, setGuard, detectGuard, reactivate});
        return EventfulDpiFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = {},
                .commitBlockBegin = 2,
                .commitBlockEnd = 3,
            },
            .clock = clock,
            .event = event,
            .guard = guard,
            .argument = argument,
            .result = result,
        };
    }

    int testEventfulDpiPendingUsesCurrentOperands() {
        EventfulDpiFixture fixture = makeEventfulDpiModel();
        EventDpiHost host;
        Interpreter interpreter(fixture.model, &host);
        if (!interpreter.ready() ||
            !interpreter.write(fixture.clock, u1(1)).success()) {
            return fail("eventful DPI fixture initialization failed");
        }
        const InterpreterResult eval = interpreter.eval();
        if (!eval.success() || eval.roundsExecuted != 2 || host.calls != 1 ||
            host.lastArgument != 0x2a ||
            interpreter.value(fixture.event).lowWord() != 0 ||
            interpreter.value(fixture.guard).lowWord() != 1 ||
            interpreter.value(fixture.argument).lowWord() != 0x2a ||
            interpreter.value(fixture.result).lowWord() != 0x2b) {
            return fail(
                "eventful DPI call did not consume current-round operands");
        }
        return 0;
    }

    struct EvalBoundaryTaskFixture {
        ExecutableModel model;
        VariableId eventClock;
        VariableId event;
        VariableId wake;
        VariableId wakeEvent;
        VariableId guard;
    };

    EvalBoundaryTaskFixture makeEvalBoundaryTaskModel() {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const StringId taskName = linear.addString("eval_boundary_task");
        const VariableId eventClock =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId eventClockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId event = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId wake = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId wakeOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId wakeEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId guard = linear.addVariable(u1Type, linear.zeroInit());

        const InstructionId changedEvent = addInstruction(
            linear, Opcode::ChangedPos, {event}, {eventClock, eventClockOld});
        const InstructionId changedWake = addInstruction(
            linear, Opcode::ChangedPos, {wakeEvent}, {wake, wakeOld});
        const InstructionId task =
            addInstruction(linear, Opcode::SystemTask, {}, {guard, event});
        linear.setSystemTaskAttributes(
            task, SystemTaskAttributes{
                      .name = taskName,
                      .eventCount = 1,
                      .schedule = CallSchedule::Normal,
                      .eventMode = HostEventMode::Pending,
                  });

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activateEvent =
            addInstruction(scheduled, Opcode::ActForward, {}, {event});
        const InstructionId activateWake =
            addInstruction(scheduled, Opcode::ActForward, {}, {wakeEvent});
        setTargets(scheduled, activateEvent, {BlockId{1}});
        setTargets(scheduled, activateWake, {BlockId{1}});
        addBlock(scheduled,
                 {changedEvent, activateEvent, changedWake, activateWake});
        addBlock(scheduled, {task});
        return EvalBoundaryTaskFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = {},
            },
            .eventClock = eventClock,
            .event = event,
            .wake = wake,
            .wakeEvent = wakeEvent,
            .guard = guard,
        };
    }

    int testPendingHostEventClearedBetweenEvals() {
        EvalBoundaryTaskFixture fixture = makeEvalBoundaryTaskModel();
        EventTaskCountingHost host;
        Interpreter interpreter(fixture.model, &host);
        if (!interpreter.ready() ||
            !interpreter.write(fixture.eventClock, u1(1)).success()) {
            return fail("eval-boundary task fixture initialization failed");
        }
        const InterpreterResult first = interpreter.eval();
        // The cross-Block event is cleared at round end; the pending host
        // event is retained internally and not observable after the eval.
        if (!first.success() || host.calls != 0 ||
            interpreter.value(fixture.event).lowWord() != 0) {
            return fail("guard-false eval did not retain its pending event");
        }
        if (!interpreter.write(fixture.guard, u1(1)).success() ||
            !interpreter.write(fixture.wake, u1(1)).success()) {
            return fail("eval-boundary task fixture wake write failed");
        }
        const InterpreterResult second = interpreter.eval();
        if (!second.success() || second.roundsExecuted != 1 ||
            host.calls != 0 ||
            interpreter.value(fixture.event).lowWord() != 0 ||
            interpreter.value(fixture.wakeEvent).lowWord() != 1) {
            return fail("pending host event leaked across top-level eval calls");
        }
        return 0;
    }

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
    if (testFeedbackAndRoundLifecycle() != 0)
        return 1;
    if (testEventDrivenRegister() != 0)
        return 1;
    if (testMemoryWriteRead() != 0)
        return 1;
    if (testPureBitVectorOperations() != 0)
        return 1;
    if (testWideAndSignedArithmetic() != 0)
        return 1;
    if (testEventfulHostPendingAcrossRounds() != 0)
        return 1;
    if (testImmediateHostEventIsNotReplayed() != 0)
        return 1;
    if (testEventfulHostMultipleEvents() != 0)
        return 1;
    if (testEventfulDpiPendingUsesCurrentOperands() != 0)
        return 1;
    if (testPendingHostEventClearedBetweenEvals() != 0)
        return 1;
    if (testInjectedHostAndOnceSchedule() != 0)
        return 1;
    if (testMissingHostBindingDiagnostic() != 0)
        return 1;
    return 0;
}
