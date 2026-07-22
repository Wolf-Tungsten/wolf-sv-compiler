#include "grhsim/am/builder.hpp"
#include "grhsim/am/interpreter.hpp"
#include "grhsim/am/production_activity_schedule.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grhsim::am;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[grhsim_am_production_activity_schedule] " << message << '\n';
        return 1;
    }

    std::optional<ExecutableModel> schedule(LinearProgramArtifact &&linear,
                                            const ActivityScheduleOptions &options,
                                            wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        ProductionActivityScheduleStage scheduler;
        return scheduler.schedule(std::move(linear), options, diagnostics);
    }

    std::optional<BlockId> findInstructionBlock(const ExecutableModel &model,
                                                InstructionId expected)
    {
        for (uint32_t block = 0; block < model.program.blockCount(); ++block) {
            for (std::size_t position = 0; position < model.program.blockSize(BlockId{block});
                 ++position) {
                if (model.program.blockInstruction(BlockId{block}, position) == expected) {
                    return BlockId{block};
                }
            }
        }
        return std::nullopt;
    }

    bool hasActivationTarget(const ExecutableModel &model, BlockId source, Opcode opcode,
                             BlockId expectedTarget)
    {
        const ProgramView view = model.program.view();
        for (std::size_t position = 0; position < model.program.blockSize(source); ++position) {
            const InstructionId instruction = model.program.blockInstruction(source, position);
            if (view.opcode(instruction) != opcode) {
                continue;
            }
            const auto attributes = view.activationAttributes(instruction);
            if (!attributes) {
                continue;
            }
            for (BlockId target : attributes->targets) {
                if (target == expectedTarget) {
                    return true;
                }
            }
        }
        return false;
    }

    std::size_t countChangedWatches(const ExecutableModel &model, VariableId watched)
    {
        const ProgramView view = model.program.view();
        std::size_t count = 0;
        for (uint32_t block = 0; block < model.program.blockCount(); ++block) {
            for (std::size_t position = 0; position < model.program.blockSize(BlockId{block});
                 ++position) {
                const InstructionId instruction =
                    model.program.blockInstruction(BlockId{block}, position);
                if (view.opcode(instruction) != Opcode::ChangedAny) {
                    continue;
                }
                const auto operands = view.operands(instruction);
                if (operands.size() == 2 && operands.front() == watched) {
                    ++count;
                }
            }
        }
        return count;
    }

    bool watchActivatesTarget(const ExecutableModel &model, BlockId source, VariableId watched,
                              Opcode activationOpcode, BlockId expectedTarget)
    {
        const ProgramView view = model.program.view();
        for (std::size_t watcherPosition = 0;
             watcherPosition < model.program.blockSize(source); ++watcherPosition) {
            const InstructionId watcher =
                model.program.blockInstruction(source, watcherPosition);
            if (view.opcode(watcher) != Opcode::ChangedAny) {
                continue;
            }
            const auto watchedOperands = view.operands(watcher);
            const auto watcherResults = view.results(watcher);
            if (watchedOperands.size() != 2 || watchedOperands.front() != watched ||
                watcherResults.size() != 1) {
                continue;
            }
            for (std::size_t activationPosition = 0;
                 activationPosition < model.program.blockSize(source); ++activationPosition) {
                const InstructionId activation =
                    model.program.blockInstruction(source, activationPosition);
                if (view.opcode(activation) != activationOpcode) {
                    continue;
                }
                const auto activationOperands = view.operands(activation);
                if (activationOperands.size() != 1 ||
                    activationOperands.front() != watcherResults.front()) {
                    continue;
                }
                const auto attributes = view.activationAttributes(activation);
                if (!attributes) {
                    continue;
                }
                for (BlockId target : attributes->targets) {
                    if (target == expectedTarget) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    int testForwardDefUseIsTopologicallyScheduled()
    {
        LinearProgramBuilder builder;
        const TypeId type = builder.addType(Type::bitVector(8));
        const StringId inputName = builder.addString("input");
        const StringId outputName = builder.addString("output");
        const VariableId input = builder.addVariable(type, builder.zeroInit());
        const VariableId temporary = builder.addVariable(type, builder.undefInit());
        const VariableId output = builder.addVariable(type, builder.undefInit());

        const std::array<VariableId, 1> outputResults = {output};
        const std::array<VariableId, 1> outputOperands = {temporary};
        const InstructionId consume =
            builder.addInstruction(Opcode::Assign, outputResults, outputOperands);
        const std::array<VariableId, 1> temporaryResults = {temporary};
        const std::array<VariableId, 1> temporaryOperands = {input};
        const InstructionId produce =
            builder.addInstruction(Opcode::Assign, temporaryResults, temporaryOperands);

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = inputName,
                .direction = PortDirection::Input,
                .input = input,
            },
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = output,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput,
            VariableRole::None,
            VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::Pure,
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(std::move(linear),
                                                        ActivityScheduleOptions{
                                                            .maxInstructionsPerBlock = 1,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3) {
            return fail("forward def-use input was not accepted as a two-block schedule");
        }
        if (model->program.blockInstruction(BlockId{1}, 0) != produce ||
            model->program.blockInstruction(BlockId{2}, 0) != consume) {
            return fail("forward def-use instructions were not placed in topological order");
        }
        const ProgramView view = model->program.view();
        bool sawProducerActivation = false;
        for (std::size_t position = 1; position < model->program.blockSize(BlockId{1});
             ++position) {
            const InstructionId instruction = model->program.blockInstruction(BlockId{1}, position);
            if (view.opcode(instruction) != Opcode::ActForward) {
                continue;
            }
            const auto attributes = view.activationAttributes(instruction);
            sawProducerActivation = attributes && attributes->targets.size() == 1 &&
                                    attributes->targets.front() == BlockId{2};
        }
        if (!sawProducerActivation ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("producer change did not precisely activate its consumer block");
        }
        return 0;
    }

    int testEffectfulChangedDetectorsCoarsenAsCompute()
    {
        constexpr uint32_t detectorCount = 12;
        constexpr uint32_t maxInstructionsPerBlock = 4;
        constexpr std::array<Opcode, 3> detectorOpcodes = {
            Opcode::ChangedPos,
            Opcode::ChangedNeg,
            Opcode::ChangedAny,
        };

        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        std::vector<InstructionId> detectors;
        std::vector<VariableId> events;
        detectors.reserve(detectorCount);
        events.reserve(detectorCount);
        for (uint32_t index = 0; index < detectorCount; ++index) {
            const VariableId current = builder.addVariable(eventType, builder.zeroInit());
            const VariableId oldValue = builder.addVariable(eventType, builder.undefInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            const std::array<VariableId, 1> results = {event};
            const std::array<VariableId, 2> operands = {current, oldValue};
            detectors.push_back(builder.addInstruction(
                detectorOpcodes[index % detectorOpcodes.size()], results, operands));
            events.push_back(event);
        }

        std::vector<InstructionId> consumers;
        consumers.reserve(detectorCount);
        for (VariableId event : events) {
            const VariableId consumed = builder.addVariable(eventType, builder.undefInit());
            const std::array<VariableId, 1> results = {consumed};
            const std::array<VariableId, 1> operands = {event};
            consumers.push_back(builder.addInstruction(Opcode::Assign, results, operands));
        }

        SchedulingFacts facts;
        facts.variableRoles.assign(detectorCount * 4, VariableRole::None);
        facts.instructionEffects.assign(detectorCount, InstructionEffect::StateReadWrite);
        facts.instructionEffects.insert(facts.instructionEffects.end(), detectorCount,
                                        InstructionEffect::Pure);
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = maxInstructionsPerBlock,
                .enableCoarsening = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 7) {
            return fail("effectful changed detectors did not coarsen into cap-bounded compute blocks");
        }

        std::vector<uint8_t> detectorBlocks(model->program.blockCount(), 0);
        std::size_t detectorBlockCount = 0;
        for (uint32_t index = 0; index < detectorCount; ++index) {
            const std::optional<BlockId> detectorBlock =
                findInstructionBlock(*model, detectors[index]);
            const std::optional<BlockId> consumerBlock =
                findInstructionBlock(*model, consumers[index]);
            if (!detectorBlock || !consumerBlock || *detectorBlock >= *consumerBlock ||
                !hasActivationTarget(*model, *detectorBlock, Opcode::ActForward,
                                     *consumerBlock)) {
                return fail("changed detector did not precede and activate its consumer");
            }
            if (!detectorBlocks[detectorBlock->value]) {
                detectorBlocks[detectorBlock->value] = 1;
                ++detectorBlockCount;
            }
        }
        if (detectorBlockCount != 3 ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("independent changed detectors retained isolated one-instruction blocks");
        }
        return 0;
    }

    int testHostEffectsAreAcceptedAndOrdered()
    {
        LinearProgramBuilder builder;
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const StringId firstName = builder.addString("first_host_read");
        const StringId secondName = builder.addString("second_host_read");
        const StringId taskName = builder.addString("host_effect");
        const VariableId firstResult = builder.addVariable(valueType, builder.undefInit());
        const VariableId secondResult = builder.addVariable(valueType, builder.undefInit());
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());

        const std::array<VariableId, 1> firstResults = {firstResult};
        const InstructionId first =
            builder.addInstruction(Opcode::SystemFunction, firstResults, {});
        builder.setSystemFunctionAttributes(first, SystemFunctionAttributes{
                                                       .name = firstName,
                                                       .schedule = CallSchedule::Normal,
                                                       .hasSideEffects = false,
                                                   });
        const std::array<VariableId, 1> secondResults = {secondResult};
        const InstructionId second =
            builder.addInstruction(Opcode::SystemFunction, secondResults, {});
        builder.setSystemFunctionAttributes(second, SystemFunctionAttributes{
                                                        .name = secondName,
                                                        .schedule = CallSchedule::Normal,
                                                        .hasSideEffects = false,
                                                    });
        const std::array<VariableId, 1> taskOperands = {condition};
        const InstructionId task = builder.addInstruction(Opcode::SystemTask, {}, taskOperands);
        builder.setSystemTaskAttributes(task, SystemTaskAttributes{
                                                  .name = taskName,
                                                  .eventCount = 0,
                                                  .schedule = CallSchedule::Normal,
                                              });

        SchedulingFacts facts;
        facts.variableRoles.assign(3, VariableRole::None);
        facts.instructionEffects = {
            InstructionEffect::HostRead,
            InstructionEffect::HostRead,
            InstructionEffect::HostEffect,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = second, .group = 7, .ordinal = 0},
            OrderedEffect{.instruction = first, .group = 7, .ordinal = 1},
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(std::move(linear),
                                                        ActivityScheduleOptions{
                                                            .maxInstructionsPerBlock = 8,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3) {
            return fail("production scheduler rejected a valid host interaction");
        }
        bool sawOrderedPair = false;
        bool sawTask = false;
        for (uint32_t block = 1; block < model->program.blockCount(); ++block) {
            if (model->program.blockSize(BlockId{block}) >= 2 &&
                model->program.blockInstruction(BlockId{block}, 0) == second &&
                model->program.blockInstruction(BlockId{block}, 1) == first) {
                sawOrderedPair = true;
            }
            if (model->program.blockSize(BlockId{block}) >= 1 &&
                model->program.blockInstruction(BlockId{block}, 0) == task) {
                sawTask = true;
            }
        }
        if (!sawOrderedPair || !sawTask ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("host ordered-effect atoms did not preserve ordinal order");
        }
        return 0;
    }

    int testLongImplicitHostOrderDoesNotFormOneAtom()
    {
        constexpr uint32_t hostCount = 64;
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const StringId taskName = builder.addString("implicit_host_effect");
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const std::array<VariableId, 1> operands = {condition};
        std::vector<InstructionId> tasks;
        tasks.reserve(hostCount);
        for (uint32_t index = 0; index < hostCount; ++index) {
            const InstructionId task = builder.addInstruction(Opcode::SystemTask, {}, operands);
            builder.setSystemTaskAttributes(task, SystemTaskAttributes{
                                                      .name = taskName,
                                                      .eventCount = 0,
                                                      .schedule = CallSchedule::Normal,
                                                  });
            tasks.push_back(task);
        }

        SchedulingFacts facts;
        facts.variableRoles = {VariableRole::None};
        facts.instructionEffects.assign(hostCount, InstructionEffect::HostEffect);
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .enableCoarsening = true,
                .collectStats = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != hostCount + 1) {
            return fail("implicit host order collapsed into an oversized scheduling atom");
        }

        BlockId previous = BlockId{0};
        for (InstructionId task : tasks) {
            const std::optional<BlockId> block = findInstructionBlock(*model, task);
            if (!block || *block <= previous || model->program.blockSize(*block) != 1) {
                return fail("implicit host execution order is not monotonic across blocks");
            }
            previous = *block;
        }
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("indivisible AM scheduling atoms") != std::string::npos) {
                return fail("implicit host chain was reported as an oversized atom");
            }
        }
        if (!validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("implicit host chain schedule is not semantically valid");
        }
        return 0;
    }

    class StateObservingHost final : public HostEnvironment
    {
    public:
        bool resolveSystemTask(ProgramView, InstructionId, std::string &) override
        {
            return true;
        }

        bool invokeSystemTask(ProgramView, InstructionId,
                              std::span<const InterpreterValue> arguments,
                              std::string &error) override
        {
            if (arguments.size() != 1) {
                error = "expected one state argument";
                return false;
            }
            ++calls;
            observedState = arguments.front().lowWord();
            return true;
        }

        uint64_t calls = 0;
        uint64_t observedState = 0;
    };

    int testPosedgeHostEffectPrecedesRegisterCommit()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const StringId clockName = builder.addString("clock");
        const StringId taskName = builder.addString("observe_state");
        const std::array<uint64_t, 1> oneWords = {1};
        const LiteralId oneLiteral = builder.addBitLiteral(eventType, oneWords);
        const VariableId enabled =
            builder.addVariable(eventType, builder.addConstantInit(oneLiteral));
        const VariableId mask =
            builder.addVariable(eventType, builder.addConstantInit(oneLiteral));
        const VariableId data = builder.addVariable(eventType, builder.zeroInit());
        const InitAction stateInitAction{
            .kind = InitActionKind::Set,
            .expression = InitExpr{
                .kind = InitExprKind::Literal,
                .literal = oneLiteral,
            },
        };
        const VariableId state = builder.addVariable(
            eventType,
            builder.addActionsInit(std::span<const InitAction>(&stateInitAction, 1)));
        const VariableId clock = builder.addVariable(eventType, builder.zeroInit());
        const VariableId clockOld = builder.addVariable(eventType, builder.undefInit());
        const VariableId posedge = builder.addVariable(eventType, builder.zeroInit());

        const std::array<VariableId, 1> edgeResults = {posedge};
        const std::array<VariableId, 2> edgeOperands = {clock, clockOld};
        builder.addInstruction(Opcode::ChangedPos, edgeResults, edgeOperands);
        const std::array<VariableId, 5> writeOperands = {
            enabled, mask, data, state, posedge,
        };
        const InstructionId writer =
            builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);
        const std::array<VariableId, 3> taskOperands = {enabled, state, posedge};
        const InstructionId task =
            builder.addInstruction(Opcode::SystemTask, {}, taskOperands);
        builder.setSystemTaskAttributes(task, SystemTaskAttributes{
                                                  .name = taskName,
                                                  .eventCount = 1,
                                                  .schedule = CallSchedule::Normal,
                                              });

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = clockName,
                .direction = PortDirection::Input,
                .input = clock,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None,          VariableRole::None,
            VariableRole::State, VariableRole::ExternalInput, VariableRole::None,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::HostEffect,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = writer, .group = 1, .ordinal = 0},
            OrderedEffect{.instruction = task, .group = 2, .ordinal = 0},
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 1,
                .enableCoarsening = false,
            },
            diagnostics);
        const std::optional<BlockId> writerBlock =
            model ? findInstructionBlock(*model, writer) : std::nullopt;
        const std::optional<BlockId> taskBlock =
            model ? findInstructionBlock(*model, task) : std::nullopt;
        if (!model || diagnostics.hasError()) {
            return fail("posedge host-before-commit fixture was rejected by the scheduler");
        }
        if (!writerBlock || !taskBlock || writer.value >= task.value ||
            *taskBlock >= *writerBlock ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("posedge host effect was not scheduled before register commit");
        }

        StateObservingHost host;
        Interpreter interpreter(*model, &host);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() || host.calls != 0 ||
            interpreter.value(state).lowWord() != 1 ||
            !interpreter
                 .write(clock,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("posedge host-before-commit fixture could not enter its edge epoch");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || host.calls != 1 || host.observedState != 1 ||
            interpreter.value(state).lowWord() != 0) {
            return fail("host effect did not observe old state before the posedge commit");
        }
        return 0;
    }

    int testImplicitCommitBeforeHostDependencyIsRejected()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const StringId taskName = builder.addString("host_effect");
        const VariableId enabled = builder.addVariable(eventType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());

        const std::array<VariableId, 5> writeOperands = {
            enabled, mask, data, state, event,
        };
        builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);
        const std::array<VariableId, 1> taskOperands = {enabled};
        const InstructionId task =
            builder.addInstruction(Opcode::SystemTask, {}, taskOperands);
        builder.setSystemTaskAttributes(task, SystemTaskAttributes{
                                                  .name = taskName,
                                                  .eventCount = 0,
                                                  .schedule = CallSchedule::Normal,
                                              });

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None,
            VariableRole::None,
            VariableRole::None,
            VariableRole::State,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::StateReadWrite,
            InstructionEffect::HostEffect,
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 1,
                .enableCoarsening = false,
            },
            diagnostics);
        bool sawPhaseDependencyError = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Error &&
                message.message ==
                    "AM dependency requires a state commit before pre-commit work") {
                sawPhaseDependencyError = true;
            }
        }
        if (model || !diagnostics.hasError() || !sawPhaseDependencyError) {
            return fail("implicit commit-before-host dependency was not rejected clearly");
        }
        return 0;
    }

    int testMemoryWritersShareOneOrderedAtom()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const std::array<VariableId, 6> operands = {
            condition, address, mask, data, memory, event,
        };
        const InstructionId first = builder.addInstruction(Opcode::MemoryWrite, {}, operands);
        const InstructionId second = builder.addInstruction(Opcode::MemoryWrite, {}, operands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None,  VariableRole::None,
            VariableRole::None, VariableRole::State, VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = second, .group = 3, .ordinal = 0},
            OrderedEffect{.instruction = first, .group = 3, .ordinal = 1},
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(std::move(linear),
                                                        ActivityScheduleOptions{
                                                            .maxInstructionsPerBlock = 8,
                                                            .maxStateWritesPerBlock = 8,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            model->program.blockSize(BlockId{1}) != 2 ||
            model->program.blockInstruction(BlockId{1}, 0) != second ||
            model->program.blockInstruction(BlockId{1}, 1) != first) {
            return fail("same-memory writers were not kept at one ordered final frontier");
        }
        return 0;
    }

    struct OrderedCommitFixture
    {
        LinearProgramArtifact linear;
        InstructionId memoryWrite;
        InstructionId registerWrite;
    };

    OrderedCommitFixture makeStateAndMemoryCommit(bool useExplicitOrder)
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId registerTarget = builder.addVariable(valueType, builder.zeroInit());

        const std::array<VariableId, 6> memoryOperands = {
            condition, address, mask, data, memory, event,
        };
        const InstructionId memoryWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, memoryOperands);
        const std::array<VariableId, 5> registerOperands = {
            condition, mask, data, registerTarget, event,
        };
        const InstructionId registerWrite =
            builder.addInstruction(Opcode::RegisterWrite, {}, registerOperands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None,  VariableRole::None,
            VariableRole::None, VariableRole::State, VariableRole::None,
            VariableRole::State,
        };
        facts.instructionEffects = {
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        if (useExplicitOrder) {
            facts.orderedEffects = {
                OrderedEffect{.instruction = registerWrite, .group = 13, .ordinal = 0},
                OrderedEffect{.instruction = memoryWrite, .group = 13, .ordinal = 1},
            };
        }
        return OrderedCommitFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            },
            .memoryWrite = memoryWrite,
            .registerWrite = registerWrite,
        };
    }

    int testOrderedStateAndMemoryWritesShareOneCommitBucket()
    {
        OrderedCommitFixture fixture = makeStateAndMemoryCommit(true);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .maxCommitInstructionsPerBlock = 8,
                .maxStateWritesPerBlock = 8,
                .enableCoarsening = false,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            model->program.blockSize(BlockId{1}) != 2 ||
            model->program.blockInstruction(BlockId{1}, 0) != fixture.registerWrite ||
            model->program.blockInstruction(BlockId{1}, 1) != fixture.memoryWrite ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("ordered state and memory writes did not remain in one commit bucket");
        }
        return 0;
    }

    int testIndependentCommitAtomsCoarsenWithinCommitCap()
    {
        OrderedCommitFixture coarsenedFixture = makeStateAndMemoryCommit(false);
        wolvrix::lib::diag::Diagnostics coarsenedDiagnostics;
        std::optional<ExecutableModel> coarsened = schedule(
            std::move(coarsenedFixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 2,
                .maxStateWritesPerBlock = 2,
                .enableCoarsening = true,
            },
            coarsenedDiagnostics);
        OrderedCommitFixture splitFixture = makeStateAndMemoryCommit(false);
        wolvrix::lib::diag::Diagnostics splitDiagnostics;
        std::optional<ExecutableModel> split = schedule(
            std::move(splitFixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 1,
                .maxStateWritesPerBlock = 2,
                .enableCoarsening = true,
            },
            splitDiagnostics);
        if (!coarsened || !split || coarsenedDiagnostics.hasError() ||
            splitDiagnostics.hasError() || coarsened->program.blockCount() != 2 ||
            coarsened->program.blockSize(BlockId{1}) != 2 ||
            coarsened->program.blockInstruction(BlockId{1}, 0) != coarsenedFixture.memoryWrite ||
            coarsened->program.blockInstruction(BlockId{1}, 1) != coarsenedFixture.registerWrite ||
            split->program.blockCount() != 3 || split->program.blockSize(BlockId{1}) != 1 ||
            split->program.blockSize(BlockId{2}) != 1 ||
            split->program.blockInstruction(BlockId{1}, 0) != splitFixture.memoryWrite ||
            split->program.blockInstruction(BlockId{2}, 0) != splitFixture.registerWrite ||
            !validate(*coarsened, ValidationOptions{.level = ValidationLevel::Semantic}).success() ||
            !validate(*split, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("independent commit atoms did not obey the commit instruction cap");
        }
        return 0;
    }

    enum class CommitFixtureEventPattern
    {
        Same,
        DifferentMiddle,
    };

    enum class CommitFixtureGuardPattern
    {
        Same,
        DifferentMiddle,
    };

    struct CommitEventGuardFixture
    {
        LinearProgramArtifact linear;
        std::array<InstructionId, 3> writes;
    };

    CommitEventGuardFixture makeCommitEventGuardFixture(
        CommitFixtureEventPattern eventPattern, CommitFixtureGuardPattern guardPattern)
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));

        const VariableId rawEvent = builder.addVariable(eventType, builder.zeroInit());
        const VariableId otherRawEvent = builder.addVariable(eventType, builder.zeroInit());
        const VariableId guard = builder.addVariable(eventType, builder.zeroInit());
        const VariableId otherGuard = builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId registerTarget = builder.addVariable(valueType, builder.zeroInit());
        const VariableId writeMemory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId fillMemory = builder.addVariable(memoryType, builder.zeroInit());

        std::array<VariableId, 3> events;
        std::array<InstructionId, 3> detectors;
        for (std::size_t index = 0; index < events.size(); ++index) {
            const VariableId oldValue = builder.addVariable(eventType, builder.undefInit());
            events[index] = builder.addVariable(eventType, builder.zeroInit());
            const VariableId current =
                index == 1 && eventPattern == CommitFixtureEventPattern::DifferentMiddle
                    ? otherRawEvent
                    : rawEvent;
            const std::array<VariableId, 1> results = {events[index]};
            const std::array<VariableId, 2> operands = {current, oldValue};
            detectors[index] =
                builder.addInstruction(Opcode::ChangedPos, results, operands);
        }

        const VariableId middleGuard =
            guardPattern == CommitFixtureGuardPattern::DifferentMiddle ? otherGuard : guard;
        const std::array<VariableId, 5> registerOperands = {
            guard, mask, data, registerTarget, events[0],
        };
        const InstructionId registerWrite =
            builder.addInstruction(Opcode::RegisterWrite, {}, registerOperands);
        const std::array<VariableId, 6> memoryWriteOperands = {
            middleGuard, address, mask, data, writeMemory, events[1],
        };
        const InstructionId memoryWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, memoryWriteOperands);
        const std::array<VariableId, 4> memoryFillOperands = {
            guard, data, fillMemory, events[2],
        };
        const InstructionId memoryFill =
            builder.addInstruction(Opcode::MemoryFill, {}, memoryFillOperands);

        SchedulingFacts facts;
        facts.variableRoles.assign(builder.view().variableCount(), VariableRole::None);
        facts.variableRoles[registerTarget.value] = VariableRole::State;
        facts.variableRoles[writeMemory.value] = VariableRole::State;
        facts.variableRoles[fillMemory.value] = VariableRole::State;
        facts.instructionEffects.assign(detectors.size(), InstructionEffect::StateReadWrite);
        facts.instructionEffects.insert(facts.instructionEffects.end(), 3,
                                        InstructionEffect::StateReadWrite);
        facts.orderedEffects = {
            OrderedEffect{.instruction = registerWrite, .group = 40, .ordinal = 0},
            OrderedEffect{.instruction = memoryWrite, .group = 41, .ordinal = 0},
            OrderedEffect{.instruction = memoryFill, .group = 42, .ordinal = 0},
        };
        return CommitEventGuardFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            },
            .writes = {registerWrite, memoryWrite, memoryFill},
        };
    }

    std::optional<ExecutableModel> scheduleCommitEventGuardFixture(
        CommitEventGuardFixture &&fixture, wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        return schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .maxCommitInstructionsPerBlock = 3,
                .maxStateWritesPerBlock = 3,
                .enableCoarsening = true,
            },
            diagnostics);
    }

    int testCommitEventsCanonicalizeChangedDetectors()
    {
        CommitEventGuardFixture fixture = makeCommitEventGuardFixture(
            CommitFixtureEventPattern::Same, CommitFixtureGuardPattern::Same);
        const std::array<InstructionId, 3> writes = fixture.writes;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            scheduleCommitEventGuardFixture(std::move(fixture), diagnostics);
        const std::optional<BlockId> first = model ? findInstructionBlock(*model, writes[0])
                                                    : std::nullopt;
        const std::optional<BlockId> second = model ? findInstructionBlock(*model, writes[1])
                                                     : std::nullopt;
        const std::optional<BlockId> third = model ? findInstructionBlock(*model, writes[2])
                                                    : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 || !first ||
            !second || !third || *first != *second || *first != *third ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("equivalent changed-event detectors did not share one commit Block");
        }
        return 0;
    }

    int testDifferentCommitEventsStartNewBlocks()
    {
        CommitEventGuardFixture fixture = makeCommitEventGuardFixture(
            CommitFixtureEventPattern::DifferentMiddle, CommitFixtureGuardPattern::Same);
        const std::array<InstructionId, 3> writes = fixture.writes;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            scheduleCommitEventGuardFixture(std::move(fixture), diagnostics);
        const std::optional<BlockId> first = model ? findInstructionBlock(*model, writes[0])
                                                    : std::nullopt;
        const std::optional<BlockId> second = model ? findInstructionBlock(*model, writes[1])
                                                     : std::nullopt;
        const std::optional<BlockId> third = model ? findInstructionBlock(*model, writes[2])
                                                    : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4 || !first ||
            !second || !third || *first != *third || *first == *second ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("different commit events were packed into the same Block");
        }
        return 0;
    }

    int testCommitGuardsGroupWithoutForcingBlockBoundary()
    {
        CommitEventGuardFixture fixture = makeCommitEventGuardFixture(
            CommitFixtureEventPattern::Same, CommitFixtureGuardPattern::DifferentMiddle);
        const std::array<InstructionId, 3> writes = fixture.writes;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            scheduleCommitEventGuardFixture(std::move(fixture), diagnostics);
        const std::optional<BlockId> block = model ? findInstructionBlock(*model, writes[0])
                                                   : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 || !block ||
            findInstructionBlock(*model, writes[1]) != block ||
            findInstructionBlock(*model, writes[2]) != block ||
            model->program.blockInstruction(*block, 0) != writes[0] ||
            model->program.blockInstruction(*block, 1) != writes[2] ||
            model->program.blockInstruction(*block, 2) != writes[1] ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("same-event commit guards were not grouped and packed deterministically");
        }
        return 0;
    }

    int testOversizedOrderedCommitAtomSurvivesCommitCap()
    {
        OrderedCommitFixture fixture = makeStateAndMemoryCommit(true);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 1,
                .maxStateWritesPerBlock = 8,
                .enableCoarsening = true,
                .collectStats = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            model->program.blockSize(BlockId{1}) != 2 ||
            model->program.blockInstruction(BlockId{1}, 0) != fixture.registerWrite ||
            model->program.blockInstruction(BlockId{1}, 1) != fixture.memoryWrite) {
            return fail("an indivisible ordered commit atom was split at the commit cap");
        }

        bool sawOversizedWarning = false;
        bool sawOversizedStats = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("count=1") != std::string::npos &&
                message.message.find("first_instructions=2") != std::string::npos) {
                sawOversizedWarning = true;
            }
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Info &&
                message.message.find("oversized_atoms=1") != std::string::npos) {
                sawOversizedStats = true;
            }
        }
        if (!sawOversizedWarning || !sawOversizedStats ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("oversized commit-cap atom reporting or semantics are incomplete");
        }
        return 0;
    }

    int testCommitBucketExceedsStateWriteCapWithoutSplitting()
    {
        OrderedCommitFixture fixture = makeStateAndMemoryCommit(true);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 8,
                .maxCommitInstructionsPerBlock = 8,
                .maxStateWritesPerBlock = 1,
                .enableCoarsening = true,
                .collectStats = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            model->program.blockSize(BlockId{1}) != 2 ||
            model->program.blockInstruction(BlockId{1}, 0) != fixture.registerWrite ||
            model->program.blockInstruction(BlockId{1}, 1) != fixture.memoryWrite) {
            return fail("an oversized ordered commit bucket was split or reordered");
        }

        bool sawOversizedWarning = false;
        bool sawOversizedStats = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("count=1") != std::string::npos &&
                message.message.find("first_state_writes=2") != std::string::npos &&
                message.message.find("max_atom_state_writes=2") != std::string::npos) {
                sawOversizedWarning = true;
            }
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Info &&
                message.message.find("oversized_atoms=1") != std::string::npos &&
                message.message.find("max_atom_state_writes=2") != std::string::npos) {
                sawOversizedStats = true;
            }
        }
        if (!sawOversizedWarning || !sawOversizedStats ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("oversized commit bucket reporting or semantics are incomplete");
        }
        return 0;
    }

    struct MemoryFrontierFixture
    {
        LinearProgramArtifact linear;
        VariableId memory;
        InstructionId reader;
        InstructionId firstWrite;
        InstructionId finalWrite;
    };

    MemoryFrontierFixture makeMemoryReaderWithOrderedWriters()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 2> readOperands = {memory, address};
        const std::array<VariableId, 1> readResults = {readData};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryRead, readResults, readOperands);
        const std::array<VariableId, 6> writeOperands = {
            condition, address, mask, data, memory, event,
        };
        const InstructionId firstWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, writeOperands);
        const InstructionId finalWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, writeOperands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None,  VariableRole::None,
            VariableRole::None, VariableRole::State, VariableRole::None,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::StateRead,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = finalWrite, .group = 31, .ordinal = 0},
            OrderedEffect{.instruction = firstWrite, .group = 31, .ordinal = 1},
        };
        return MemoryFrontierFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            },
            .memory = memory,
            .reader = reader,
            .firstWrite = firstWrite,
            .finalWrite = finalWrite,
        };
    }

    int testFinalMemoryWriteFrontierUsesOneWatcher()
    {
        MemoryFrontierFixture fixture = makeMemoryReaderWithOrderedWriters();
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxStateWritesPerBlock = 8,
                .enableCoarsening = false,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3) {
            return fail("memory reader and writers did not form a deterministic schedule");
        }
        const std::optional<BlockId> readerBlock = findInstructionBlock(*model, fixture.reader);
        const std::optional<BlockId> firstWriterBlock =
            findInstructionBlock(*model, fixture.firstWrite);
        const std::optional<BlockId> finalWriterBlock =
            findInstructionBlock(*model, fixture.finalWrite);
        if (!readerBlock || !firstWriterBlock || !finalWriterBlock ||
            *firstWriterBlock != *finalWriterBlock ||
            model->program.blockInstruction(*firstWriterBlock, 0) != fixture.finalWrite ||
            model->program.blockInstruction(*firstWriterBlock, 1) != fixture.firstWrite ||
            countChangedWatches(*model, fixture.memory) != 1 ||
            !watchActivatesTarget(*model, *firstWriterBlock, fixture.memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("only the final memory-write frontier must watch and reactivate readers");
        }
        return 0;
    }

    int testStateChangeUsesBackwardReaderActivation()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const StringId conditionName = builder.addString("condition");
        const StringId eventName = builder.addString("event");
        const StringId dataName = builder.addString("data");
        const StringId outputName = builder.addString("output");
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId output = builder.addVariable(valueType, builder.undefInit());
        const std::array<VariableId, 1> readResults = {output};
        const std::array<VariableId, 1> readOperands = {state};
        const InstructionId reader =
            builder.addInstruction(Opcode::Assign, readResults, readOperands);
        const std::array<VariableId, 5> writeOperands = {
            condition, mask, data, state, event,
        };
        const InstructionId writer =
            builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = conditionName,
                .direction = PortDirection::Input,
                .input = condition,
            },
            PortBinding{
                .name = eventName,
                .direction = PortDirection::Input,
                .input = event,
            },
            PortBinding{
                .name = dataName,
                .direction = PortDirection::Input,
                .input = data,
            },
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = output,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput, VariableRole::ExternalInput, VariableRole::ExternalInput,
            VariableRole::None,          VariableRole::State,         VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = writer, .group = 0, .ordinal = 0},
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(std::move(linear),
                                                        ActivityScheduleOptions{
                                                            .maxInstructionsPerBlock = 1,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            model->program.blockInstruction(BlockId{1}, 0) != reader ||
            model->program.blockInstruction(BlockId{2}, 0) != writer) {
            return fail("state reader/writer blocks were not built deterministically");
        }
        const ProgramView view = model->program.view();
        bool sawBackwardReader = false;
        for (std::size_t position = 1; position < model->program.blockSize(BlockId{2});
             ++position) {
            const InstructionId instruction = model->program.blockInstruction(BlockId{2}, position);
            if (view.opcode(instruction) != Opcode::ActBackward) {
                continue;
            }
            const auto attributes = view.activationAttributes(instruction);
            sawBackwardReader = attributes && attributes->targets.size() == 1 &&
                                attributes->targets.front() == BlockId{1};
        }
        if (!sawBackwardReader ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("final state-write frontier did not reactivate the exact reader");
        }
        return 0;
    }

    int testMergedCommitWritesReactivateMemoryReaderAcrossEpoch()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const auto addConstant = [&builder](TypeId type, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return builder.addVariable(
                type, builder.addConstantInit(builder.addBitLiteral(type, words)));
        };
        const VariableId enabled = addConstant(eventType, 1);
        const VariableId address = addConstant(addressType, 2);
        const VariableId mask = addConstant(valueType, 0xff);
        const VariableId writeEvent = builder.addVariable(eventType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId registerState = builder.addVariable(valueType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {readData};
        const std::array<VariableId, 2> readerOperands = {memory, address};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryRead, readerResults, readerOperands);
        const std::array<VariableId, 4> registerOperands = {
            enabled, mask, data, registerState,
        };
        const InstructionId registerWrite =
            builder.addInstruction(Opcode::LatchWrite, {}, registerOperands);
        const std::array<VariableId, 6> memoryOperands = {
            enabled, address, mask, data, memory, writeEvent,
        };
        const InstructionId memoryWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, memoryOperands);

        const StringId eventName = builder.addString("write_event");
        const StringId dataName = builder.addString("data");
        const StringId outputName = builder.addString("read_data");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = eventName,
                .direction = PortDirection::Input,
                .input = writeEvent,
            },
            PortBinding{
                .name = dataName,
                .direction = PortDirection::Input,
                .input = data,
            },
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = readData,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None,          VariableRole::None, VariableRole::None,
            VariableRole::ExternalInput, VariableRole::ExternalInput,
            VariableRole::State,         VariableRole::State,
            VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::StateRead,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(linear),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 2,
                .maxStateWritesPerBlock = 2,
                .enableCoarsening = true,
            },
            diagnostics);
        const std::optional<BlockId> readerBlock =
            model ? findInstructionBlock(*model, reader) : std::nullopt;
        const std::optional<BlockId> registerBlock =
            model ? findInstructionBlock(*model, registerWrite) : std::nullopt;
        const std::optional<BlockId> memoryBlock =
            model ? findInstructionBlock(*model, memoryWrite) : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4 ||
            !readerBlock || !registerBlock || !memoryBlock || *readerBlock != BlockId{1} ||
            *registerBlock != BlockId{2} || *memoryBlock != BlockId{3} ||
            !watchActivatesTarget(*model, *memoryBlock, memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("different-event commits did not retain separate reader-frontier Blocks");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> writeEventWords = {1};
        const std::array<uint64_t, 1> dataWords = {0x5a};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter.write(
                writeEvent,
                InterpreterValue::bitVector(1, Signedness::Unsigned, writeEventWords))
                 .success() ||
            !interpreter.write(data,
                               InterpreterValue::bitVector(8, Signedness::Unsigned, dataWords))
                 .success()) {
            return fail("scheduled commit fixture could not enter its write epoch");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.epochsExecuted != 2 ||
            interpreter.value(registerState).lowWord() != 0x5a ||
            interpreter.value(memory).arrayElementWords(2).front() != 0x5a ||
            interpreter.value(readData).lowWord() != 0x5a) {
            return fail("separate commit event buckets did not reactivate the memory reader");
        }
        return 0;
    }

    LinearProgramArtifact makePureCycle()
    {
        LinearProgramBuilder builder;
        const TypeId type = builder.addType(Type::bitVector(8));
        const VariableId lhs = builder.addVariable(type, builder.zeroInit());
        const VariableId rhs = builder.addVariable(type, builder.zeroInit());
        const std::array<VariableId, 1> lhsResults = {lhs};
        const std::array<VariableId, 1> lhsOperands = {rhs};
        builder.addInstruction(Opcode::Assign, lhsResults, lhsOperands);
        const std::array<VariableId, 1> rhsResults = {rhs};
        const std::array<VariableId, 1> rhsOperands = {lhs};
        builder.addInstruction(Opcode::Assign, rhsResults, rhsOperands);
        SchedulingFacts facts;
        facts.variableRoles.assign(2, VariableRole::None);
        facts.instructionEffects.assign(2, InstructionEffect::Pure);
        return LinearProgramArtifact{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };
    }

    LinearProgramArtifact makePureChain(std::size_t instructionCount)
    {
        LinearProgramBuilder builder;
        const TypeId type = builder.addType(Type::bitVector(8));
        std::vector<VariableId> values;
        values.reserve(instructionCount + 1);
        values.push_back(builder.addVariable(type, builder.zeroInit()));
        for (std::size_t index = 0; index < instructionCount; ++index) {
            values.push_back(builder.addVariable(type, builder.undefInit()));
            const std::array<VariableId, 1> results = {values[index + 1]};
            const std::array<VariableId, 1> operands = {values[index]};
            builder.addInstruction(Opcode::Assign, results, operands);
        }
        SchedulingFacts facts;
        facts.variableRoles.assign(values.size(), VariableRole::None);
        facts.instructionEffects.assign(instructionCount, InstructionEffect::Pure);
        return LinearProgramArtifact{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };
    }

    std::vector<uint64_t> programShape(const ExecutableModel &model)
    {
        std::vector<uint64_t> shape;
        const ProgramView view = model.program.view();
        shape.push_back(model.program.blockCount());
        shape.push_back(view.instructionCount());
        for (uint32_t block = 0; block < model.program.blockCount(); ++block) {
            shape.push_back(model.program.blockSize(BlockId{block}));
            for (std::size_t position = 0; position < model.program.blockSize(BlockId{block});
                 ++position) {
                const InstructionId instruction =
                    model.program.blockInstruction(BlockId{block}, position);
                shape.push_back(instruction.value);
                shape.push_back(static_cast<uint8_t>(view.opcode(instruction)));
                const auto activation = view.activationAttributes(instruction);
                shape.push_back(activation ? activation->targets.size() : 0);
                if (activation) {
                    for (BlockId target : activation->targets) {
                        shape.push_back(target.value);
                    }
                }
            }
        }
        return shape;
    }

    int testPureComputeCoarseningHonorsInstructionCapDeterministically()
    {
        constexpr uint32_t instructionCount = 5;
        const ActivityScheduleOptions coarsenedOptions{
            .maxInstructionsPerBlock = 2,
            .enableCoarsening = true,
        };
        wolvrix::lib::diag::Diagnostics firstDiagnostics;
        std::optional<ExecutableModel> first =
            schedule(makePureChain(instructionCount), coarsenedOptions, firstDiagnostics);
        wolvrix::lib::diag::Diagnostics secondDiagnostics;
        std::optional<ExecutableModel> second =
            schedule(makePureChain(instructionCount), coarsenedOptions, secondDiagnostics);
        wolvrix::lib::diag::Diagnostics uncoarsenedDiagnostics;
        std::optional<ExecutableModel> uncoarsened = schedule(
            makePureChain(instructionCount),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 2,
                .enableCoarsening = false,
            },
            uncoarsenedDiagnostics);
        if (!first || !second || !uncoarsened || firstDiagnostics.hasError() ||
            secondDiagnostics.hasError() || uncoarsenedDiagnostics.hasError() ||
            first->program.blockCount() != 4 || uncoarsened->program.blockCount() != 6) {
            return fail("pure compute chain did not coarsen into cap-bounded blocks");
        }
        if (first->program.blockSize(BlockId{1}) < 2 ||
            first->program.blockInstruction(BlockId{1}, 0) != InstructionId{0} ||
            first->program.blockInstruction(BlockId{1}, 1) != InstructionId{1} ||
            first->program.blockSize(BlockId{2}) < 2 ||
            first->program.blockInstruction(BlockId{2}, 0) != InstructionId{2} ||
            first->program.blockInstruction(BlockId{2}, 1) != InstructionId{3} ||
            first->program.blockSize(BlockId{3}) < 1 ||
            first->program.blockInstruction(BlockId{3}, 0) != InstructionId{4}) {
            return fail("pure compute coarsening did not preserve contiguous topological segments");
        }
        for (uint32_t block = 1; block < first->program.blockCount(); ++block) {
            std::size_t semanticInstructions = 0;
            for (std::size_t position = 0;
                 position < first->program.blockSize(BlockId{block}); ++position) {
                const InstructionId instruction =
                    first->program.blockInstruction(BlockId{block}, position);
                semanticInstructions += instruction.value < instructionCount ? 1U : 0U;
            }
            if (semanticInstructions > 2) {
                return fail("pure compute block exceeded the configured instruction cap");
            }
        }
        if (programShape(*first) != programShape(*second) ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("cap-bounded pure compute coarsening is not deterministic or semantic");
        }
        return 0;
    }

    struct MixedActivationFixture
    {
        LinearProgramArtifact linear;
        VariableId state;
        InstructionId reader;
        InstructionId writer;
        InstructionId producer;
        InstructionId consumer;
    };

    MixedActivationFixture makeMixedActivationProgram()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId stateRead = builder.addVariable(valueType, builder.undefInit());
        const VariableId temporary = builder.addVariable(valueType, builder.undefInit());
        const VariableId output = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readResults = {stateRead};
        const std::array<VariableId, 1> readOperands = {state};
        const InstructionId reader =
            builder.addInstruction(Opcode::Assign, readResults, readOperands);
        const std::array<VariableId, 5> writeOperands = {
            condition, mask, data, state, event,
        };
        const InstructionId writer =
            builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);
        const std::array<VariableId, 1> producerResults = {temporary};
        const std::array<VariableId, 1> producerOperands = {data};
        const InstructionId producer =
            builder.addInstruction(Opcode::Assign, producerResults, producerOperands);
        const std::array<VariableId, 1> consumerResults = {output};
        const std::array<VariableId, 1> consumerOperands = {temporary};
        const InstructionId consumer =
            builder.addInstruction(Opcode::Assign, consumerResults, consumerOperands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::State, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::Pure,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = writer, .group = 41, .ordinal = 0},
        };
        return MixedActivationFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            },
            .state = state,
            .reader = reader,
            .writer = writer,
            .producer = producer,
            .consumer = consumer,
        };
    }

    int testMixedForwardBackwardActivationIsDeterministic()
    {
        const ActivityScheduleOptions options{
            .maxInstructionsPerBlock = 1,
            .maxStateWritesPerBlock = 8,
            .enableCoarsening = false,
        };
        MixedActivationFixture firstFixture = makeMixedActivationProgram();
        wolvrix::lib::diag::Diagnostics firstDiagnostics;
        std::optional<ExecutableModel> first =
            schedule(std::move(firstFixture.linear), options, firstDiagnostics);
        MixedActivationFixture secondFixture = makeMixedActivationProgram();
        wolvrix::lib::diag::Diagnostics secondDiagnostics;
        std::optional<ExecutableModel> second =
            schedule(std::move(secondFixture.linear), options, secondDiagnostics);
        if (!first || !second || firstDiagnostics.hasError() || secondDiagnostics.hasError() ||
            first->program.blockCount() != 5) {
            return fail("mixed forward/backward program was not scheduled deterministically");
        }
        const std::optional<BlockId> readerBlock =
            findInstructionBlock(*first, firstFixture.reader);
        const std::optional<BlockId> writerBlock =
            findInstructionBlock(*first, firstFixture.writer);
        const std::optional<BlockId> producerBlock =
            findInstructionBlock(*first, firstFixture.producer);
        const std::optional<BlockId> consumerBlock =
            findInstructionBlock(*first, firstFixture.consumer);
        if (!readerBlock || !writerBlock || !producerBlock || !consumerBlock ||
            *readerBlock >= *writerBlock || *producerBlock >= *consumerBlock ||
            *consumerBlock >= *writerBlock ||
            !hasActivationTarget(*first, *producerBlock, Opcode::ActForward, *consumerBlock) ||
            !watchActivatesTarget(*first, *writerBlock, firstFixture.state, Opcode::ActBackward,
                                  *readerBlock) ||
            programShape(*first) != programShape(*second) ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("forward and backward activity edges lost their stable epoch placement");
        }
        return 0;
    }

    int testPureSccConvergesThroughBackwardActivationDeterministically()
    {
        const ActivityScheduleOptions options{
            .maxInstructionsPerBlock = 8,
            .enableCoarsening = false,
        };
        wolvrix::lib::diag::Diagnostics firstDiagnostics;
        std::optional<ExecutableModel> first = schedule(makePureCycle(), options, firstDiagnostics);
        wolvrix::lib::diag::Diagnostics secondDiagnostics;
        std::optional<ExecutableModel> second =
            schedule(makePureCycle(), options, secondDiagnostics);
        if (!first || !second || firstDiagnostics.hasError() || secondDiagnostics.hasError() ||
            first->program.blockCount() != 2) {
            return fail("pure def-use SCC was not scheduled as one fixed-point block");
        }
        const ProgramView view = first->program.view();
        bool sawSelfActivation = false;
        for (std::size_t position = 2; position < first->program.blockSize(BlockId{1});
             ++position) {
            const InstructionId instruction = first->program.blockInstruction(BlockId{1}, position);
            if (view.opcode(instruction) != Opcode::ActBackward) {
                continue;
            }
            const auto attributes = view.activationAttributes(instruction);
            sawSelfActivation = attributes && attributes->targets.size() == 1 &&
                                attributes->targets.front() == BlockId{1};
        }
        if (!sawSelfActivation || programShape(*first) != programShape(*second) ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("pure SCC schedule is not deterministic or lacks next-epoch feedback");
        }
        return 0;
    }

    int testOversizedIndivisibleAtomFormsOneBlock()
    {
        const ActivityScheduleOptions options{
            .maxInstructionsPerBlock = 1,
            .enableCoarsening = true,
            .collectStats = true,
        };
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(makePureCycle(), options, diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            model->program.blockSize(BlockId{1}) < 2 ||
            model->program.blockInstruction(BlockId{1}, 0) != InstructionId{0} ||
            model->program.blockInstruction(BlockId{1}, 1) != InstructionId{1}) {
            return fail("oversized indivisible atom was rejected or split across blocks");
        }

        bool sawWarning = false;
        bool sawStats = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("count=1") != std::string::npos &&
                message.message.find("first_instructions=2") != std::string::npos) {
                sawWarning = true;
            }
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Info &&
                message.message.find("oversized_atoms=1") != std::string::npos &&
                message.message.find("max_atom_instructions=2") != std::string::npos) {
                sawStats = true;
            }
        }
        if (!sawWarning || !sawStats ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("oversized atom warning, statistics, or semantics are incomplete");
        }
        return 0;
    }
} // namespace

int main()
{
    if (const int result = testForwardDefUseIsTopologicallyScheduled(); result != 0) {
        return result;
    }
    if (const int result = testEffectfulChangedDetectorsCoarsenAsCompute(); result != 0) {
        return result;
    }
    if (const int result = testHostEffectsAreAcceptedAndOrdered(); result != 0) {
        return result;
    }
    if (const int result = testLongImplicitHostOrderDoesNotFormOneAtom(); result != 0) {
        return result;
    }
    if (const int result = testPosedgeHostEffectPrecedesRegisterCommit(); result != 0) {
        return result;
    }
    if (const int result = testImplicitCommitBeforeHostDependencyIsRejected(); result != 0) {
        return result;
    }
    if (const int result = testMemoryWritersShareOneOrderedAtom(); result != 0) {
        return result;
    }
    if (const int result = testOrderedStateAndMemoryWritesShareOneCommitBucket(); result != 0) {
        return result;
    }
    if (const int result = testIndependentCommitAtomsCoarsenWithinCommitCap(); result != 0) {
        return result;
    }
    if (const int result = testCommitEventsCanonicalizeChangedDetectors(); result != 0) {
        return result;
    }
    if (const int result = testDifferentCommitEventsStartNewBlocks(); result != 0) {
        return result;
    }
    if (const int result = testCommitGuardsGroupWithoutForcingBlockBoundary(); result != 0) {
        return result;
    }
    if (const int result = testOversizedOrderedCommitAtomSurvivesCommitCap(); result != 0) {
        return result;
    }
    if (const int result = testCommitBucketExceedsStateWriteCapWithoutSplitting(); result != 0) {
        return result;
    }
    if (const int result = testFinalMemoryWriteFrontierUsesOneWatcher(); result != 0) {
        return result;
    }
    if (const int result = testStateChangeUsesBackwardReaderActivation(); result != 0) {
        return result;
    }
    if (const int result = testMergedCommitWritesReactivateMemoryReaderAcrossEpoch(); result != 0) {
        return result;
    }
    if (const int result = testPureComputeCoarseningHonorsInstructionCapDeterministically();
        result != 0) {
        return result;
    }
    if (const int result = testMixedForwardBackwardActivationIsDeterministic(); result != 0) {
        return result;
    }
    if (const int result = testPureSccConvergesThroughBackwardActivationDeterministically();
        result != 0) {
        return result;
    }
    if (const int result = testOversizedIndivisibleAtomFormsOneBlock(); result != 0) {
        return result;
    }
    return 0;
}
