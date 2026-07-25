#include "grhsim/am/builder.hpp"
#include "grhsim/am/interpreter.hpp"
#include "grhsim/am/production_activity_schedule.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
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

    InstructionId addInstruction(ScheduledProgramBuilder &builder, Opcode opcode,
                                 std::initializer_list<VariableId> results,
                                 std::initializer_list<VariableId> operands)
    {
        return builder.addInstruction(
            opcode, std::span<const VariableId>(results.begin(), results.size()),
            std::span<const VariableId>(operands.begin(), operands.size()));
    }

    void addBlock(ScheduledProgramBuilder &builder,
                  std::initializer_list<InstructionId> instructions)
    {
        builder.addBlock(
            std::span<const InstructionId>(instructions.begin(), instructions.size()));
    }

    void setTargets(ScheduledProgramBuilder &builder, InstructionId instruction,
                    std::initializer_list<BlockId> targets)
    {
        builder.setActivationTargets(
            instruction, std::span<const BlockId>(targets.begin(), targets.size()));
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

    bool hasLocalGuardedForwardActivation(const ExecutableModel &model, BlockId block,
                                          InstructionId writer, BlockId expectedTarget)
    {
        const ProgramView view = model.program.view();
        for (std::size_t position = 2; position < model.program.blockSize(block); ++position) {
            if (model.program.blockInstruction(block, position) != writer) {
                continue;
            }
            const InstructionId snapshot =
                model.program.blockInstruction(block, position - 2);
            const InstructionId activation =
                model.program.blockInstruction(block, position - 1);
            if (view.opcode(snapshot) != Opcode::ReduceOr ||
                view.opcode(activation) != Opcode::ActForward) {
                return false;
            }
            const auto snapshotOperands = view.operands(snapshot);
            const auto snapshotResults = view.results(snapshot);
            const auto writerOperands = view.operands(writer);
            const auto activationOperands = view.operands(activation);
            const auto activationAttributes = view.activationAttributes(activation);
            return snapshotOperands.size() == 1 && !writerOperands.empty() &&
                   snapshotOperands.front() == writerOperands.front() &&
                   snapshotResults.size() == 1 && activationOperands.size() == 1 &&
                   activationOperands.front() == snapshotResults.front() &&
                   activationAttributes && activationAttributes->targets.size() == 1 &&
                   activationAttributes->targets.front() == expectedTarget;
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
            return fail("independent changed detectors did not coarsen into cap-bounded Blocks");
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
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4) {
            return fail("production scheduler rejected a valid host interaction");
        }
        const std::optional<BlockId> firstBlock = findInstructionBlock(*model, first);
        const std::optional<BlockId> secondBlock = findInstructionBlock(*model, second);
        const std::optional<BlockId> taskBlock = findInstructionBlock(*model, task);
        if (!firstBlock || !secondBlock || !taskBlock || *secondBlock >= *firstBlock ||
            *taskBlock == *firstBlock || *taskBlock == *secondBlock ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("host ordered-effect edges did not preserve ordinal order across Blocks");
        }
        return 0;
    }

    int testInstructionCannotJoinMultipleExplicitOrderGroups()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const StringId taskName = builder.addString("multi_group_host_effect");
        const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
        const std::array<VariableId, 1> operands = {condition};
        const InstructionId repeated =
            builder.addInstruction(Opcode::SystemTask, {}, operands);
        builder.setSystemTaskAttributes(repeated, SystemTaskAttributes{
                                                       .name = taskName,
                                                       .eventCount = 0,
                                                       .schedule = CallSchedule::Normal,
                                                   });
        const InstructionId intervening =
            builder.addInstruction(Opcode::SystemTask, {}, operands);
        builder.setSystemTaskAttributes(intervening, SystemTaskAttributes{
                                                          .name = taskName,
                                                          .eventCount = 0,
                                                          .schedule = CallSchedule::Normal,
                                                      });

        SchedulingFacts facts;
        facts.variableRoles = {VariableRole::None};
        facts.instructionEffects = {
            InstructionEffect::HostEffect,
            InstructionEffect::HostEffect,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = repeated, .group = 5, .ordinal = 0},
            OrderedEffect{.instruction = intervening, .group = 6, .ordinal = 0},
            OrderedEffect{.instruction = repeated, .group = 7, .ordinal = 0},
        };
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
            },
            diagnostics);
        bool sawMultipleGroupError = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Error &&
                message.message ==
                    "AM instruction appears in multiple explicit ordered-effect groups: "
                    "instruction=0 first_group=5 second_group=7") {
                sawMultipleGroupError = true;
            }
        }
        if (model || !diagnostics.hasError() || !sawMultipleGroupError) {
            return fail("one instruction was accepted in multiple explicit order groups");
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
        constexpr uint32_t expectedHostBlocks = hostCount / 8;
        if (!model || diagnostics.hasError() ||
            model->program.blockCount() != expectedHostBlocks + 1) {
            return fail("implicit host order did not coarsen into cap-bounded compute Blocks");
        }

        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const BlockId expectedBlock{index / 8 + 1};
            const std::optional<BlockId> block = findInstructionBlock(*model, tasks[index]);
            if (!block || *block != expectedBlock ||
                model->program.blockInstruction(expectedBlock, index % 8) != tasks[index]) {
                return fail("implicit host execution order was not preserved while coarsening");
            }
        }
        bool sawZeroOversizedAtoms = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("indivisible AM scheduling atoms") != std::string::npos) {
                return fail("implicit host chain was reported as an oversized atom");
            }
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Info &&
                message.message.find("oversized_atoms=0") != std::string::npos) {
                sawZeroOversizedAtoms = true;
            }
        }
        if (!sawZeroOversizedAtoms ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("implicit host chain schedule is not semantically valid");
        }
        return 0;
    }

    class EchoSystemFunctionHost final : public HostEnvironment
    {
    public:
        bool resolveSystemFunction(ProgramView, InstructionId, std::string &) override
        {
            return true;
        }

        bool invokeSystemFunction(ProgramView, InstructionId,
                                  std::span<const InterpreterValue> arguments,
                                  InterpreterValue &result, std::string &error) override
        {
            if (arguments.size() != 1) {
                error = "expected one system-function argument";
                return false;
            }
            ++calls;
            lastArgument = arguments.front().lowWord();
            const std::array<uint64_t, 1> words = {lastArgument};
            result = InterpreterValue::bitVector(8, Signedness::Unsigned, words);
            return true;
        }

        uint64_t calls = 0;
        uint64_t lastArgument = 0;
    };

    int testSystemFunctionCoarsensWithProducerAndReschedules()
    {
        LinearProgramBuilder builder;
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const StringId inputName = builder.addString("input");
        const StringId functionName = builder.addString("echo");
        const VariableId input = builder.addVariable(valueType, builder.zeroInit());
        const VariableId produced = builder.addVariable(valueType, builder.undefInit());
        const VariableId result = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> producerResults = {produced};
        const std::array<VariableId, 1> producerOperands = {input};
        const InstructionId producer =
            builder.addInstruction(Opcode::Assign, producerResults, producerOperands);
        const std::array<VariableId, 1> functionResults = {result};
        const std::array<VariableId, 1> functionOperands = {produced};
        const InstructionId function =
            builder.addInstruction(Opcode::SystemFunction, functionResults, functionOperands);
        builder.setSystemFunctionAttributes(function, SystemFunctionAttributes{
                                                          .name = functionName,
                                                          .schedule = CallSchedule::Normal,
                                                          .hasSideEffects = false,
                                                      });

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = inputName,
                .direction = PortDirection::Input,
                .input = input,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput,
            VariableRole::None,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::HostRead,
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
                .maxInstructionsPerBlock = 2,
                .enableCoarsening = true,
            },
            diagnostics);
        const std::optional<BlockId> producerBlock =
            model ? findInstructionBlock(*model, producer) : std::nullopt;
        const std::optional<BlockId> functionBlock =
            model ? findInstructionBlock(*model, function) : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            !producerBlock || !functionBlock || *producerBlock != *functionBlock ||
            model->program.blockInstruction(*producerBlock, 0) != producer ||
            model->program.blockInstruction(*producerBlock, 1) != function ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("SystemFunction did not coarsen with its ordinary compute producer");
        }

        EchoSystemFunctionHost host;
        Interpreter interpreter(*model, &host);
        if (!interpreter.ready() || !interpreter.eval().success() || host.calls != 1 ||
            host.lastArgument != 0 || interpreter.value(result).lowWord() != 0) {
            return fail("coarsened SystemFunction did not run during the initial eval");
        }
        const std::array<uint64_t, 1> changedWords = {0x5a};
        if (!interpreter
                 .write(input,
                        InterpreterValue::bitVector(8, Signedness::Unsigned, changedWords))
                 .success() ||
            !interpreter.eval().success() || host.calls != 2 || host.lastArgument != 0x5a ||
            interpreter.value(result).lowWord() != 0x5a) {
            return fail("reactivated compute Block did not rerun and update SystemFunction");
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
        const InstructionId detector =
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
                .maxInstructionsPerBlock = 2,
                .maxCommitInstructionsPerBlock = 1,
                .enableCoarsening = true,
            },
            diagnostics);
        const std::optional<BlockId> detectorBlock =
            model ? findInstructionBlock(*model, detector) : std::nullopt;
        const std::optional<BlockId> writerBlock =
            model ? findInstructionBlock(*model, writer) : std::nullopt;
        const std::optional<BlockId> taskBlock =
            model ? findInstructionBlock(*model, task) : std::nullopt;
        if (!model || diagnostics.hasError()) {
            return fail("posedge host-before-commit fixture was rejected by the scheduler");
        }
        if (!detectorBlock || !writerBlock || !taskBlock || *detectorBlock != *taskBlock ||
            *taskBlock >= *writerBlock || model->commitBlockBegin != writerBlock->value ||
            model->commitBlockEnd != writerBlock->value + 1U ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("posedge detector and host effect did not coarsen before register commit");
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

    int testCommitWaitsForBackwardComputeAndConsumesEdgeOnce()
    {
        LinearProgramBuilder linear;
        const TypeId eventType = linear.addType(Type::bitVector(1));
        const TypeId valueType = linear.addType(Type::bitVector(8));
        const auto addConstant = [&linear](TypeId type, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return linear.addVariable(
                type, linear.addConstantInit(linear.addBitLiteral(type, words)));
        };

        const VariableId clock = linear.addVariable(eventType, linear.zeroInit());
        const VariableId entryOld = linear.addVariable(eventType, linear.undefInit());
        const VariableId entryEvent = linear.addVariable(eventType, linear.zeroInit());
        const VariableId clockOld = linear.addVariable(eventType, linear.undefInit());
        const VariableId posedge = linear.addVariable(eventType, linear.zeroInit());
        const VariableId payload = linear.addVariable(valueType, linear.zeroInit());
        const VariableId capturedPayload = linear.addVariable(valueType, linear.zeroInit());
        const VariableId capturedOld = linear.addVariable(valueType, linear.undefInit());
        const VariableId capturedChanged = linear.addVariable(eventType, linear.zeroInit());
        const VariableId lateData = linear.addVariable(valueType, linear.zeroInit());
        const VariableId lateOld = linear.addVariable(valueType, linear.undefInit());
        const VariableId lateChanged = linear.addVariable(eventType, linear.zeroInit());
        const VariableId state = linear.addVariable(valueType, linear.zeroInit());
        const VariableId nextState = linear.addVariable(valueType, linear.zeroInit());
        const VariableId stateOld = linear.addVariable(valueType, linear.undefInit());
        const VariableId stateChanged = linear.addVariable(eventType, linear.zeroInit());
        const VariableId commitCount = linear.addVariable(valueType, linear.zeroInit());
        const VariableId nextCommitCount = linear.addVariable(valueType, linear.zeroInit());
        const VariableId captureCommitCount =
            linear.addVariable(valueType, linear.zeroInit());
        const VariableId nextCaptureCommitCount =
            linear.addVariable(valueType, linear.zeroInit());
        const VariableId one = addConstant(eventType, 1);
        const VariableId oneValue = addConstant(valueType, 1);
        const VariableId mask = addConstant(valueType, 0xff);

        const InstructionId watchClock =
            linear.addInstruction(Opcode::ChangedAny, std::array{entryEvent},
                                  std::array{clock, entryOld});
        const InstructionId detectPosedge =
            linear.addInstruction(Opcode::ChangedPos, std::array{posedge},
                                  std::array{clock, clockOld});
        const InstructionId updateLateData =
            linear.addInstruction(Opcode::Assign, std::array{lateData},
                                  std::array{capturedPayload});
        const InstructionId detectLateData =
            linear.addInstruction(Opcode::ChangedAny, std::array{lateChanged},
                                  std::array{lateData, lateOld});
        const InstructionId addState =
            linear.addInstruction(Opcode::Add, std::array{nextState},
                                  std::array{state, lateData});
        const InstructionId addCommitCount =
            linear.addInstruction(Opcode::Add, std::array{nextCommitCount},
                                  std::array{commitCount, oneValue});
        const InstructionId addCaptureCommitCount =
            linear.addInstruction(Opcode::Add, std::array{nextCaptureCommitCount},
                                  std::array{captureCommitCount, oneValue});
        const InstructionId commit =
            linear.addInstruction(Opcode::RegisterWrite, {},
                                  std::array{one, mask, nextState, state, posedge});
        const InstructionId countCommit =
            linear.addInstruction(Opcode::RegisterWrite, {},
                                  std::array{one, mask, nextCommitCount, commitCount, posedge});
        const InstructionId countCaptureCommit =
            linear.addInstruction(
                Opcode::RegisterWrite, {},
                std::array{one, mask, nextCaptureCommitCount,
                           captureCommitCount, posedge});
        const InstructionId capturePayload =
            linear.addInstruction(Opcode::RegisterWrite, {},
                                  std::array{one, mask, payload, capturedPayload, posedge});
        const InstructionId detectCaptured =
            linear.addInstruction(Opcode::ChangedAny, std::array{capturedChanged},
                                  std::array{capturedPayload, capturedOld});
        const InstructionId detectState =
            linear.addInstruction(Opcode::ChangedAny, std::array{stateChanged},
                                  std::array{state, stateOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId enterEdge =
            addInstruction(scheduled, Opcode::ActForward, {}, {entryEvent});
        const InstructionId activateCommit =
            addInstruction(scheduled, Opcode::ActForward, {}, {posedge});
        const InstructionId activateCapture =
            addInstruction(scheduled, Opcode::ActForward, {}, {posedge});
        const InstructionId activateLateData =
            addInstruction(scheduled, Opcode::ActBackward, {}, {capturedChanged});
        const InstructionId reactivateCommit =
            addInstruction(scheduled, Opcode::ActForward, {}, {lateChanged});
        const InstructionId reactivateCapture =
            addInstruction(scheduled, Opcode::ActBackward, {}, {stateChanged});
        setTargets(scheduled, enterEdge, {BlockId{1}});
        setTargets(scheduled, activateCommit, {BlockId{3}});
        setTargets(scheduled, activateCapture, {BlockId{4}});
        setTargets(scheduled, activateLateData, {BlockId{2}});
        setTargets(scheduled, reactivateCommit, {BlockId{3}});
        setTargets(scheduled, reactivateCapture, {BlockId{4}});
        addBlock(scheduled, {watchClock, enterEdge});
        addBlock(scheduled, {detectPosedge, activateCommit, activateCapture});
        addBlock(scheduled, {updateLateData, detectLateData, reactivateCommit});
        addBlock(scheduled, {addState, addCommitCount, commit, countCommit,
                             detectState, reactivateCapture});
        addBlock(scheduled, {addCaptureCommitCount, capturePayload,
                             countCaptureCommit, detectCaptured,
                             activateLateData});

        ExecutableModel model{
            .program = scheduled.finish(),
            .interface = {},
            .commitBlockBegin = 3,
            .commitBlockEnd = 5,
            .commitBlockOrder = {BlockId{4}, BlockId{3}},
            .commitGroupOffsets = {0, 1, 2},
        };
        if (!validate(model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("late-data commit regression model is invalid");
        }

        Interpreter interpreter(model);
        const std::array<uint64_t, 1> high = {1};
        const std::array<uint64_t, 1> payloadWords = {0x5a};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(payload,
                        InterpreterValue::bitVector(8, Signedness::Unsigned, payloadWords))
                 .success() ||
            !interpreter
                 .write(clock,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, high))
                 .success()) {
            return fail("late-data commit regression could not enter its edge eval");
        }

        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.epochsExecuted != 3 ||
            interpreter.epochCounter() != 2 ||
            interpreter.value(lateData).lowWord() != 0x5a ||
            interpreter.value(state).lowWord() != 0x5a ||
            interpreter.value(commitCount).lowWord() != 1 ||
            interpreter.value(captureCommitCount).lowWord() != 1 ||
            interpreter.value(posedge).lowWord() != 0) {
            return fail("commit did not wait for backward compute or consumed its edge repeatedly");
        }
        const InterpreterResult stable = interpreter.eval();
        if (!stable.success() || stable.epochsExecuted != 0 ||
            interpreter.value(state).lowWord() != 0x5a ||
            interpreter.value(commitCount).lowWord() != 1) {
            return fail("deferred commit edge leaked into a later eval");
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

    int testMemoryWritersRemainOrderedWithoutAtomContraction()
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
        const std::optional<BlockId> secondBlock =
            model ? findInstructionBlock(*model, second) : std::nullopt;
        const std::optional<BlockId> firstBlock =
            model ? findInstructionBlock(*model, first) : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            !secondBlock || !firstBlock || *secondBlock != BlockId{1} ||
            *firstBlock != BlockId{2} ||
            !hasActivationTarget(*model, *secondBlock, Opcode::ActForward, *firstBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("same-memory writer dependency did not remain ordered across Blocks");
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

    int testOrderedStateAndMemoryWritesRemainSeparateWithoutCoarsening()
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
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            model->program.blockInstruction(BlockId{1}, 0) != fixture.registerWrite ||
            model->program.blockInstruction(BlockId{2}, 0) != fixture.memoryWrite ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("ordered state and memory writes did not remain separate ordered atoms");
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

    int testOrderedCommitChainSplitsAtCommitCap()
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
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            model->program.blockSize(BlockId{1}) != 1 ||
            model->program.blockSize(BlockId{2}) != 1 ||
            model->program.blockInstruction(BlockId{1}, 0) != fixture.registerWrite ||
            model->program.blockInstruction(BlockId{2}, 0) != fixture.memoryWrite) {
            return fail("ordered commit edges did not split cleanly at the commit cap");
        }

        bool sawZeroOversizedStats = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("indivisible AM scheduling atoms") != std::string::npos) {
                return fail("ordered commit chain was still reported as one oversized atom");
            }
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Info &&
                message.message.find("oversized_atoms=0") != std::string::npos &&
                message.message.find("max_atom_instructions=1") != std::string::npos) {
                sawZeroOversizedStats = true;
            }
        }
        if (!sawZeroOversizedStats ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("commit-cap split statistics or semantics are incomplete");
        }
        return 0;
    }

    int testOrderedCommitChainSplitsAtStateWriteCap()
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
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            model->program.blockSize(BlockId{1}) != 1 ||
            model->program.blockSize(BlockId{2}) != 1 ||
            model->program.blockInstruction(BlockId{1}, 0) != fixture.registerWrite ||
            model->program.blockInstruction(BlockId{2}, 0) != fixture.memoryWrite) {
            return fail("ordered commit edges did not split cleanly at the state-write cap");
        }

        bool sawZeroOversizedStats = false;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("indivisible AM scheduling atoms") != std::string::npos) {
                return fail("ordered state-write chain was still reported as one oversized atom");
            }
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Info &&
                message.message.find("oversized_atoms=0") != std::string::npos &&
                message.message.find("max_atom_state_writes=1") != std::string::npos) {
                sawZeroOversizedStats = true;
            }
        }
        if (!sawZeroOversizedStats ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("state-write-cap split statistics or semantics are incomplete");
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
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4) {
            return fail("memory reader and writers did not form a deterministic schedule");
        }
        const std::optional<BlockId> readerBlock = findInstructionBlock(*model, fixture.reader);
        const std::optional<BlockId> firstWriterBlock =
            findInstructionBlock(*model, fixture.firstWrite);
        const std::optional<BlockId> finalWriterBlock =
            findInstructionBlock(*model, fixture.finalWrite);
        if (!readerBlock || !firstWriterBlock || !finalWriterBlock ||
            *finalWriterBlock >= *firstWriterBlock ||
            !hasActivationTarget(*model, *finalWriterBlock, Opcode::ActForward,
                                 *firstWriterBlock) ||
            countChangedWatches(*model, fixture.memory) != 1 ||
            !watchActivatesTarget(*model, *firstWriterBlock, fixture.memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("only the final memory-write frontier must watch and reactivate readers");
        }
        return 0;
    }

    int testEarlierWriterActivityReachesFinalFrontier()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId signedConditionType =
            builder.addType(Type::bitVector(1, Signedness::Signed));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const auto addConstant = [&builder](TypeId type, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return builder.addVariable(
                type, builder.addConstantInit(builder.addBitLiteral(type, words)));
        };

        const VariableId earlierEnabled =
            builder.addVariable(signedConditionType, builder.zeroInit());
        const VariableId disabled = addConstant(eventType, 0);
        const VariableId companionTrigger =
            builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = addConstant(addressType, 2);
        const VariableId mask = addConstant(valueType, 0xff);
        const VariableId data = addConstant(valueType, 0x5a);
        const VariableId event = addConstant(eventType, 1);
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId companionMemory =
            builder.addVariable(memoryType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {readData};
        const std::array<VariableId, 2> readerOperands = {memory, address};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryRead, readerResults, readerOperands);
        const std::array<VariableId, 6> companionOperands = {
            companionTrigger, address, mask, data, companionMemory, event,
        };
        const InstructionId companionWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, companionOperands);
        const std::array<VariableId, 6> earlierOperands = {
            earlierEnabled, address, mask, data, memory, event,
        };
        const InstructionId earlierWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, earlierOperands);
        const std::array<VariableId, 6> finalOperands = {
            disabled, address, mask, data, memory, event,
        };
        const InstructionId finalWrite =
            builder.addInstruction(Opcode::MemoryWrite, {}, finalOperands);

        const StringId triggerName = builder.addString("companion_trigger");
        const StringId outputName = builder.addString("read_data");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = triggerName,
                .direction = PortDirection::Input,
                .input = companionTrigger,
            },
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = readData,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None,          VariableRole::ExternalInput,
            VariableRole::None, VariableRole::None,          VariableRole::None,
            VariableRole::None, VariableRole::State,         VariableRole::State,
            VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::StateRead,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = earlierWrite, .group = 32, .ordinal = 0},
            OrderedEffect{.instruction = finalWrite, .group = 32, .ordinal = 1},
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
        const std::optional<BlockId> earlierBlock =
            model ? findInstructionBlock(*model, earlierWrite) : std::nullopt;
        const std::optional<BlockId> companionBlock =
            model ? findInstructionBlock(*model, companionWrite) : std::nullopt;
        const std::optional<BlockId> finalBlock =
            model ? findInstructionBlock(*model, finalWrite) : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4 ||
            !readerBlock || !earlierBlock || !companionBlock || !finalBlock ||
            *companionBlock != *earlierBlock || *earlierBlock >= *finalBlock ||
            !hasActivationTarget(*model, BlockId{0}, Opcode::ActForward, *companionBlock) ||
            !hasActivationTarget(*model, *earlierBlock, Opcode::ActForward, *finalBlock) ||
            countChangedWatches(*model, memory) != 1 ||
            !watchActivatesTarget(*model, *finalBlock, memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("earlier writer activity did not reach the unique final-write frontier");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> highWords = {1};
        // Only the companion input supplies scheduled activity to their shared commit Block.
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(earlierEnabled,
                        InterpreterValue::bitVector(1, Signedness::Signed, highWords))
                 .success() ||
            !interpreter
                 .write(companionTrigger,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("earlier-writer frontier fixture could not enter its write epoch");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.epochsExecuted != 2 ||
            interpreter.value(memory).arrayElementWords(2).front() != 0x5a ||
            interpreter.value(readData).lowWord() != 0x5a) {
            return fail("final writer frontier did not reactivate the reader after an earlier write");
        }
        return 0;
    }

    int testBackwardActivatedEarlierWriterReachesFinalFrontierInSameEpoch()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const auto addConstant = [&builder](TypeId type, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return builder.addVariable(
                type, builder.addConstantInit(builder.addBitLiteral(type, words)));
        };

        const VariableId sourceTrigger = builder.addVariable(eventType, builder.zeroInit());
        const VariableId disabled = addConstant(eventType, 0);
        const VariableId one = addConstant(eventType, 1);
        const VariableId mask = addConstant(valueType, 0xff);
        const VariableId mainData = addConstant(valueType, 0x5a);
        const VariableId mainState = builder.addVariable(valueType, builder.zeroInit());
        const VariableId triggerState = builder.addVariable(eventType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {readData};
        const std::array<VariableId, 1> readerOperands = {mainState};
        const InstructionId reader =
            builder.addInstruction(Opcode::Assign, readerResults, readerOperands);
        const std::array<VariableId, 5> earlierOperands = {
            triggerState, mask, mainData, mainState, one,
        };
        const InstructionId earlierWriter =
            builder.addInstruction(Opcode::RegisterWrite, {}, earlierOperands);
        const std::array<VariableId, 5> finalOperands = {
            disabled, mask, mainData, mainState, one,
        };
        const InstructionId finalWriter =
            builder.addInstruction(Opcode::RegisterWrite, {}, finalOperands);
        const std::array<VariableId, 5> triggerOperands = {
            sourceTrigger, one, one, triggerState, one,
        };
        const InstructionId triggerWriter =
            builder.addInstruction(Opcode::RegisterWrite, {}, triggerOperands);

        const StringId triggerName = builder.addString("source_trigger");
        const StringId outputName = builder.addString("read_data");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = triggerName,
                .direction = PortDirection::Input,
                .input = sourceTrigger,
            },
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = readData,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput, VariableRole::None, VariableRole::None,
            VariableRole::None,          VariableRole::None, VariableRole::State,
            VariableRole::State,         VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = earlierWriter, .group = 90, .ordinal = 0},
            OrderedEffect{.instruction = finalWriter, .group = 90, .ordinal = 1},
            OrderedEffect{.instruction = triggerWriter, .group = 90, .ordinal = 2},
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
                .maxStateWritesPerBlock = 1,
                .enableCoarsening = true,
            },
            diagnostics);
        const std::optional<BlockId> readerBlock =
            model ? findInstructionBlock(*model, reader) : std::nullopt;
        const std::optional<BlockId> earlierBlock =
            model ? findInstructionBlock(*model, earlierWriter) : std::nullopt;
        const std::optional<BlockId> finalBlock =
            model ? findInstructionBlock(*model, finalWriter) : std::nullopt;
        const std::optional<BlockId> triggerBlock =
            model ? findInstructionBlock(*model, triggerWriter) : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 5 ||
            !readerBlock || !earlierBlock || !finalBlock || !triggerBlock ||
            !(*readerBlock < *earlierBlock && *earlierBlock < *finalBlock &&
              *finalBlock < *triggerBlock) ||
            !hasActivationTarget(*model, BlockId{0}, Opcode::ActForward, *triggerBlock) ||
            !watchActivatesTarget(*model, *triggerBlock, triggerState, Opcode::ActBackward,
                                  *earlierBlock) ||
            !hasLocalGuardedForwardActivation(*model, *earlierBlock, earlierWriter,
                                              *finalBlock) ||
            countChangedWatches(*model, triggerState) != 1 ||
            countChangedWatches(*model, mainState) != 1 ||
            !watchActivatesTarget(*model, *finalBlock, mainState, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("backward-activated earlier writer lost its same-epoch final frontier");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(sourceTrigger,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("backward frontier fixture could not enter its trigger epoch");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.epochsExecuted != 3 ||
            interpreter.value(triggerState).lowWord() != 1 ||
            interpreter.value(mainState).lowWord() != 0x5a ||
            interpreter.value(readData).lowWord() != 0x5a) {
            return fail("backward activity did not reach the reader through the final frontier");
        }
        return 0;
    }

    int testRuntimeWriterFrontierChainPropagatesWithoutStaticClosure()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const auto addConstant = [&builder](TypeId type, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return builder.addVariable(
                type, builder.addConstantInit(builder.addBitLiteral(type, words)));
        };

        const VariableId start = builder.addVariable(eventType, builder.zeroInit());
        const VariableId disabled = addConstant(eventType, 0);
        const VariableId one = addConstant(eventType, 1);
        const VariableId eventA = addConstant(eventType, 1);
        const VariableId eventB = addConstant(eventType, 1);
        const VariableId eventC = addConstant(eventType, 1);
        const VariableId mask = addConstant(valueType, 0xff);
        const VariableId data = addConstant(valueType, 0x5a);
        const VariableId stateX = builder.addVariable(eventType, builder.zeroInit());
        const VariableId stateY = builder.addVariable(valueType, builder.zeroInit());
        const VariableId output = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {output};
        const std::array<VariableId, 1> readerOperands = {stateY};
        const InstructionId reader =
            builder.addInstruction(Opcode::Assign, readerResults, readerOperands);
        const std::array<VariableId, 5> earlierXOperands = {
            start, one, one, stateX, eventA,
        };
        const InstructionId earlierX =
            builder.addInstruction(Opcode::RegisterWrite, {}, earlierXOperands);
        const std::array<VariableId, 5> finalXOperands = {
            disabled, one, one, stateX, eventB,
        };
        const InstructionId finalX =
            builder.addInstruction(Opcode::RegisterWrite, {}, finalXOperands);
        const std::array<VariableId, 5> earlierYOperands = {
            stateX, mask, data, stateY, eventB,
        };
        const InstructionId earlierY =
            builder.addInstruction(Opcode::RegisterWrite, {}, earlierYOperands);
        const std::array<VariableId, 5> finalYOperands = {
            disabled, mask, data, stateY, eventC,
        };
        const InstructionId finalY =
            builder.addInstruction(Opcode::RegisterWrite, {}, finalYOperands);

        const StringId startName = builder.addString("start");
        const StringId outputName = builder.addString("output");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = startName,
                .direction = PortDirection::Input,
                .input = start,
            },
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = output,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput, VariableRole::None, VariableRole::None,
            VariableRole::None,          VariableRole::None, VariableRole::None,
            VariableRole::None,          VariableRole::None, VariableRole::State,
            VariableRole::State,         VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = earlierX, .group = 91, .ordinal = 0},
            OrderedEffect{.instruction = finalX, .group = 91, .ordinal = 1},
            OrderedEffect{.instruction = earlierY, .group = 91, .ordinal = 2},
            OrderedEffect{.instruction = finalY, .group = 91, .ordinal = 3},
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
        const std::optional<BlockId> blockA =
            model ? findInstructionBlock(*model, earlierX) : std::nullopt;
        const std::optional<BlockId> finalXBlock =
            model ? findInstructionBlock(*model, finalX) : std::nullopt;
        const std::optional<BlockId> blockB =
            model ? findInstructionBlock(*model, earlierY) : std::nullopt;
        const std::optional<BlockId> blockC =
            model ? findInstructionBlock(*model, finalY) : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 5 ||
            !readerBlock || !blockA || !finalXBlock || !blockB || !blockC ||
            *finalXBlock != *blockB ||
            !(*readerBlock < *blockA && *blockA < *blockB && *blockB < *blockC) ||
            !hasLocalGuardedForwardActivation(*model, *blockA, earlierX, *blockB) ||
            !hasLocalGuardedForwardActivation(*model, *blockB, earlierY, *blockC) ||
            hasActivationTarget(*model, *blockA, Opcode::ActForward, *blockC) ||
            hasActivationTarget(*model, BlockId{0}, Opcode::ActForward, *blockB) ||
            hasActivationTarget(*model, BlockId{0}, Opcode::ActForward, *blockC) ||
            countChangedWatches(*model, stateY) != 1 ||
            !watchActivatesTarget(*model, *blockC, stateY, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("runtime writer-frontier chain was flattened or materialized incorrectly");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(start, InterpreterValue::bitVector(
                                   1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("runtime writer-frontier chain could not enter its start epoch");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.epochsExecuted != 2 ||
            interpreter.value(stateX).lowWord() != 1 ||
            interpreter.value(stateY).lowWord() != 0x5a ||
            interpreter.value(output).lowWord() != 0x5a) {
            return fail("runtime writer-frontier chain did not reach C in the source epoch");
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

    struct CommitCycleFixture
    {
        LinearProgramArtifact linear;
        VariableId start;
        VariableId stateA;
        VariableId stateB;
        VariableId stateC;
        InstructionId writerA;
        InstructionId writerB;
        InstructionId writerC;
    };

    CommitCycleFixture makeCommitCycleFixture()
    {
        LinearProgramBuilder builder;
        const TypeId type = builder.addType(Type::bitVector(1));
        const std::array<uint64_t, 1> oneWords = {1};
        const auto oneInit =
            builder.addConstantInit(builder.addBitLiteral(type, oneWords));
        const VariableId start = builder.addVariable(type, builder.zeroInit());
        const VariableId one = builder.addVariable(type, oneInit);
        const VariableId stateA = builder.addVariable(type, builder.zeroInit());
        const VariableId stateB = builder.addVariable(type, builder.zeroInit());
        const VariableId stateC = builder.addVariable(type, builder.zeroInit());
        const VariableId guardB = builder.addVariable(type, builder.undefInit());
        const VariableId guardC = builder.addVariable(type, builder.undefInit());
        const VariableId dataA = builder.addVariable(type, builder.undefInit());

        builder.addInstruction(Opcode::Assign, std::array{guardB}, std::array{stateA});
        const InstructionId writerB = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{guardB, one, one, stateB, one});
        builder.addInstruction(Opcode::Assign, std::array{guardC}, std::array{stateB});
        const InstructionId writerC = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{guardC, one, one, stateC, one});
        builder.addInstruction(Opcode::LogicOr, std::array{dataA}, std::array{stateC, one});
        const InstructionId writerA = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{start, one, dataA, stateA, one});

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = builder.addString("start"),
                .direction = PortDirection::Input,
                .input = start,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput,
            VariableRole::None,
            VariableRole::State,
            VariableRole::State,
            VariableRole::State,
            VariableRole::None,
            VariableRole::None,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
        };
        return CommitCycleFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            },
            .start = start,
            .stateA = stateA,
            .stateB = stateB,
            .stateC = stateC,
            .writerA = writerA,
            .writerB = writerB,
            .writerC = writerC,
        };
    }

    int testCommitCycleUsesActivationOrderAndSccGroups()
    {
        const ActivityScheduleOptions options{
            .maxInstructionsPerBlock = 1,
            .maxCommitInstructionsPerBlock = 1,
            .maxStateWritesPerBlock = 1,
            .enableCoarsening = false,
        };
        CommitCycleFixture firstFixture = makeCommitCycleFixture();
        wolvrix::lib::diag::Diagnostics firstDiagnostics;
        std::optional<ExecutableModel> first =
            schedule(std::move(firstFixture.linear), options, firstDiagnostics);
        CommitCycleFixture secondFixture = makeCommitCycleFixture();
        wolvrix::lib::diag::Diagnostics secondDiagnostics;
        std::optional<ExecutableModel> second =
            schedule(std::move(secondFixture.linear), options, secondDiagnostics);

        const std::optional<BlockId> blockA =
            first ? findInstructionBlock(*first, firstFixture.writerA) : std::nullopt;
        const std::optional<BlockId> blockB =
            first ? findInstructionBlock(*first, firstFixture.writerB) : std::nullopt;
        const std::optional<BlockId> blockC =
            first ? findInstructionBlock(*first, firstFixture.writerC) : std::nullopt;
        const std::vector<uint32_t> expectedOffsets = {0, 3};
        if (!first || !second || firstDiagnostics.hasError() || secondDiagnostics.hasError() ||
            !blockA || !blockB || !blockC || !(*blockB < *blockC && *blockC < *blockA) ||
            first->commitBlockOrder != std::vector<BlockId>{*blockA, *blockB, *blockC} ||
            first->commitGroupOffsets != expectedOffsets ||
            first->commitBlockOrder != second->commitBlockOrder ||
            first->commitGroupOffsets != second->commitGroupOffsets ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            std::cerr << "[commit-cycle] blocks="
                      << (blockA ? std::to_string(blockA->value) : "missing") << ','
                      << (blockB ? std::to_string(blockB->value) : "missing") << ','
                      << (blockC ? std::to_string(blockC->value) : "missing") << " order=";
            if (first) {
                for (BlockId block : first->commitBlockOrder) {
                    std::cerr << block.value << ',';
                }
                std::cerr << " offsets=";
                for (uint32_t offset : first->commitGroupOffsets) {
                    std::cerr << offset << ',';
                }
            }
            std::cerr << '\n';
            for (const wolvrix::lib::diag::Diagnostic &message : firstDiagnostics.messages()) {
                std::cerr << "[commit-cycle] " << message.message << '\n';
            }
            return fail("cyclic commits did not use deterministic activation order and groups");
        }

        Interpreter interpreter(*first);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(firstFixture.start,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("cyclic commit fixture could not enter its source eval");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || interpreter.value(firstFixture.stateA).lowWord() != 1 ||
            interpreter.value(firstFixture.stateB).lowWord() != 1 ||
            interpreter.value(firstFixture.stateC).lowWord() != 1) {
            return fail("cyclic commit execution skipped an intervening compute frontier");
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
    if (const int result = testInstructionCannotJoinMultipleExplicitOrderGroups(); result != 0) {
        return result;
    }
    if (const int result = testLongImplicitHostOrderDoesNotFormOneAtom(); result != 0) {
        return result;
    }
    if (const int result = testSystemFunctionCoarsensWithProducerAndReschedules(); result != 0) {
        return result;
    }
    if (const int result = testPosedgeHostEffectPrecedesRegisterCommit(); result != 0) {
        return result;
    }
    if (const int result = testCommitWaitsForBackwardComputeAndConsumesEdgeOnce(); result != 0) {
        return result;
    }
    if (const int result = testImplicitCommitBeforeHostDependencyIsRejected(); result != 0) {
        return result;
    }
    if (const int result = testMemoryWritersRemainOrderedWithoutAtomContraction(); result != 0) {
        return result;
    }
    if (const int result = testOrderedStateAndMemoryWritesRemainSeparateWithoutCoarsening();
        result != 0) {
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
    if (const int result = testOrderedCommitChainSplitsAtCommitCap(); result != 0) {
        return result;
    }
    if (const int result = testOrderedCommitChainSplitsAtStateWriteCap(); result != 0) {
        return result;
    }
    if (const int result = testFinalMemoryWriteFrontierUsesOneWatcher(); result != 0) {
        return result;
    }
    if (const int result = testEarlierWriterActivityReachesFinalFrontier(); result != 0) {
        return result;
    }
    if (const int result = testBackwardActivatedEarlierWriterReachesFinalFrontierInSameEpoch();
        result != 0) {
        return result;
    }
    if (const int result = testRuntimeWriterFrontierChainPropagatesWithoutStaticClosure();
        result != 0) {
        return result;
    }
    if (const int result = testStateChangeUsesBackwardReaderActivation(); result != 0) {
        return result;
    }
    if (const int result = testMergedCommitWritesReactivateMemoryReaderAcrossEpoch(); result != 0) {
        return result;
    }
    if (const int result = testCommitCycleUsesActivationOrderAndSccGroups(); result != 0) {
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
