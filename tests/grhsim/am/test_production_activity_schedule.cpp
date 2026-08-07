#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_program_interpreter.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_graph_partition.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <charconv>
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
        return GrhIRToGrhSimAMProgram::graphToProgram(AmGraph::fromLinearProgram(linear), options, diagnostics);
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

    std::optional<std::size_t> blockInstructionPosition(const ExecutableModel &model,
                                                        BlockId block, InstructionId expected)
    {
        for (std::size_t position = 0; position < model.program.blockSize(block); ++position) {
            if (model.program.blockInstruction(block, position) == expected) {
                return position;
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

    std::size_t countHeadGateDetectors(const ExecutableModel &model, BlockId block)
    {
        const ProgramView view = model.program.view();
        std::size_t count = 0;
        for (std::size_t position = 0; position < model.program.blockSize(block); ++position) {
            const InstructionId instruction = model.program.blockInstruction(block, position);
            const Opcode opcode = view.opcode(instruction);
            if (opcode != Opcode::ChangedAny && opcode != Opcode::ChangedPos &&
                opcode != Opcode::ChangedNeg) {
                break;
            }
            ++count;
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
                                                            .maxAtomsPerBlock = 1,
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
        constexpr uint32_t maxAtomsPerBlock = 4;
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
                .maxAtomsPerBlock = maxAtomsPerBlock,
                .enableCoarsening = true,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 7) {
            return fail("effectful changed detectors did not coarsen into cap-bounded compute blocks");
        }

        // Coarsening contracts each detector->consumer chain, then the segment
        // DP packs two detector+consumer pairs into each cap-bounded Block.
        std::vector<uint8_t> detectorBlocks(model->program.blockCount(), 0);
        std::size_t detectorBlockCount = 0;
        for (uint32_t index = 0; index < detectorCount; ++index) {
            const std::optional<BlockId> detectorBlock =
                findInstructionBlock(*model, detectors[index]);
            const std::optional<BlockId> consumerBlock =
                findInstructionBlock(*model, consumers[index]);
            if (!detectorBlock || !consumerBlock || *detectorBlock != *consumerBlock) {
                return fail("changed detector did not coarsen into its consumer's Block");
            }
            const std::optional<std::size_t> detectorPosition =
                blockInstructionPosition(*model, *detectorBlock, detectors[index]);
            const std::optional<std::size_t> consumerPosition =
                blockInstructionPosition(*model, *consumerBlock, consumers[index]);
            if (!detectorPosition || !consumerPosition ||
                *detectorPosition >= *consumerPosition) {
                return fail("changed detector did not precede its consumer inside the Block");
            }
            if (!detectorBlocks[detectorBlock->value]) {
                detectorBlocks[detectorBlock->value] = 1;
                ++detectorBlockCount;
            }
        }
        if (detectorBlockCount != 6) {
            return fail("independent changed detectors did not pack into cap-bounded Blocks");
        }
        for (uint32_t block = 1; block < model->program.blockCount(); ++block) {
            if (model->program.blockSize(BlockId{block}) > maxAtomsPerBlock) {
                return fail("changed-detector Block exceeded the configured atom cap");
            }
        }
        if (!validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("changed-detector schedule is not semantically valid");
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
                                                            .maxAtomsPerBlock = 8,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2) {
            return fail("production scheduler rejected a valid host interaction");
        }
        const std::optional<BlockId> firstBlock = findInstructionBlock(*model, first);
        const std::optional<BlockId> secondBlock = findInstructionBlock(*model, second);
        const std::optional<BlockId> taskBlock = findInstructionBlock(*model, task);
        if (!firstBlock || !secondBlock || !taskBlock || *firstBlock != BlockId{1} ||
            *secondBlock != BlockId{1} || *taskBlock != BlockId{1}) {
            return fail("host instructions did not share one compute Block");
        }
        // The explicit ordinal edge (second before first) is preserved by the
        // in-Block instruction order.
        const std::optional<std::size_t> secondPosition =
            blockInstructionPosition(*model, BlockId{1}, second);
        const std::optional<std::size_t> firstPosition =
            blockInstructionPosition(*model, BlockId{1}, first);
        if (!secondPosition || !firstPosition || *secondPosition >= *firstPosition ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("host ordered-effect edges did not preserve ordinal order inside the Block");
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
                .maxAtomsPerBlock = 8,
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
                .maxAtomsPerBlock = 8,
                .enableCoarsening = true,
                .collectStats = true,
                // Explicit budget so this scenario does not depend on the
                // automatic coarsen-budget default.
                .dpCoarsenAtomBudget = 256,
            },
            diagnostics);
        // The chain stays 64 separate atoms, but the coarsen budget
        // (256) contracts it into one cluster, so the
        // segment DP emits a single compute Block preserving program order.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2) {
            return fail("implicit host order did not pack into one ordered compute Block");
        }

        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const std::optional<BlockId> block = findInstructionBlock(*model, tasks[index]);
            if (!block || *block != BlockId{1} ||
                model->program.blockInstruction(BlockId{1}, index) != tasks[index]) {
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
                .maxAtomsPerBlock = 2,
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
        // Merged nextValue form: the write itself is unconditional; the commit
        // Block's head gate (a ChangedPos clone watching the clock) decides
        // whether it runs.
        const std::array<VariableId, 3> writeOperands = {data, state, posedge};
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
            VariableRole::None, VariableRole::None, VariableRole::State,
            VariableRole::ExternalInput, VariableRole::None, VariableRole::None,
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
                .maxAtomsPerBlock = 2,
                .maxCommitAtomsPerBlock = 1,
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
            return fail("posedge host-before-commit fixture could not enter its edge eval");
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
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());

        // Neither effectful instruction carries an explicit ordered-effect
        // group, so the implicit ordered-effect chain forces the commit ahead
        // of the pre-commit host task.
        const std::array<VariableId, 3> writeOperands = {data, state, event};
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
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 1,
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

    int testOrderedMemoryWritersStayOrderedInSharedCommitBlock()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId enable = builder.addVariable(eventType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const std::array<VariableId, 6> operands = {enable, address, mask, data, memory, event};
        const InstructionId first = builder.addInstruction(Opcode::MemoryWriteCondMask, {}, operands);
        const InstructionId second = builder.addInstruction(Opcode::MemoryWriteCondMask, {}, operands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::State, VariableRole::None,
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
                                                            .maxAtomsPerBlock = 8,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        const std::optional<BlockId> secondBlock =
            model ? findInstructionBlock(*model, second) : std::nullopt;
        const std::optional<BlockId> firstBlock =
            model ? findInstructionBlock(*model, first) : std::nullopt;
        // Same event bucket: the ordered writers share one commit Block and
        // keep their explicit ordinal order behind the Block's head gate
        // detector.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            !secondBlock || !firstBlock || *secondBlock != BlockId{1} ||
            *firstBlock != BlockId{1}) {
            return fail("same-memory ordered writers did not share one commit Block");
        }
        const std::optional<std::size_t> secondPosition =
            blockInstructionPosition(*model, BlockId{1}, second);
        const std::optional<std::size_t> firstPosition =
            blockInstructionPosition(*model, BlockId{1}, first);
        if (!secondPosition || !firstPosition || *secondPosition >= *firstPosition ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("same-memory ordered writers did not stay ordered inside the commit Block");
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
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId enable = builder.addVariable(eventType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId registerTarget = builder.addVariable(valueType, builder.zeroInit());

        const std::array<VariableId, 6> memoryOperands = {enable, address, mask, data, memory, event};
        const InstructionId memoryWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, memoryOperands);
        const std::array<VariableId, 3> registerOperands = {data, registerTarget, event};
        const InstructionId registerWrite =
            builder.addInstruction(Opcode::RegisterWrite, {}, registerOperands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::State, VariableRole::None, VariableRole::State,
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

    int testOrderedStateAndMemoryWritesShareOneCommitBlock()
    {
        OrderedCommitFixture fixture = makeStateAndMemoryCommit(true);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxAtomsPerBlock = 8,
                .maxCommitAtomsPerBlock = 8,
                .enableCoarsening = false,
            },
            diagnostics);
        // Same event bucket: the ordered writes share one commit Block and
        // keep their explicit ordinal order behind the head gate detector.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2) {
            return fail("ordered state and memory writes did not share one commit Block");
        }
        const std::optional<std::size_t> registerPosition =
            blockInstructionPosition(*model, BlockId{1}, fixture.registerWrite);
        const std::optional<std::size_t> memoryPosition =
            blockInstructionPosition(*model, BlockId{1}, fixture.memoryWrite);
        if (!registerPosition || !memoryPosition || *registerPosition >= *memoryPosition ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("ordered state and memory writes lost their order inside the commit Block");
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
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 2,
                .enableCoarsening = true,
            },
            coarsenedDiagnostics);
        OrderedCommitFixture splitFixture = makeStateAndMemoryCommit(false);
        wolvrix::lib::diag::Diagnostics splitDiagnostics;
        std::optional<ExecutableModel> split = schedule(
            std::move(splitFixture.linear),
            ActivityScheduleOptions{
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 1,
                .enableCoarsening = true,
            },
            splitDiagnostics);
        // The cap-2 run packs both same-bucket writes into one commit Block
        // (in implicit program order, behind the head gate detector); the
        // cap-1 run splits the bucket into two commit Blocks in the same
        // order.
        const std::optional<std::size_t> coarsenedMemoryPosition =
            coarsened ? blockInstructionPosition(*coarsened, BlockId{1},
                                                 coarsenedFixture.memoryWrite)
                      : std::nullopt;
        const std::optional<std::size_t> coarsenedRegisterPosition =
            coarsened ? blockInstructionPosition(*coarsened, BlockId{1},
                                                 coarsenedFixture.registerWrite)
                      : std::nullopt;
        if (!coarsened || !split || coarsenedDiagnostics.hasError() ||
            splitDiagnostics.hasError() || coarsened->program.blockCount() != 2 ||
            !coarsenedMemoryPosition || !coarsenedRegisterPosition ||
            *coarsenedMemoryPosition >= *coarsenedRegisterPosition ||
            split->program.blockCount() != 3 ||
            findInstructionBlock(*split, splitFixture.memoryWrite) != BlockId{1} ||
            findInstructionBlock(*split, splitFixture.registerWrite) != BlockId{2} ||
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

    struct CommitEventFixture
    {
        LinearProgramArtifact linear;
        std::array<InstructionId, 3> writes;
    };

    CommitEventFixture makeCommitEventFixture(CommitFixtureEventPattern eventPattern)
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId packedType = builder.addType(Type::bitVector(32));
        const TypeId memoryType = builder.addType(Type::array(4, 8));

        const VariableId rawEvent = builder.addVariable(eventType, builder.zeroInit());
        const VariableId otherRawEvent = builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId enable = builder.addVariable(eventType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId packedData = builder.addVariable(packedType, builder.zeroInit());
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

        const std::array<VariableId, 3> registerOperands = {data, registerTarget, events[0]};
        const InstructionId registerWrite =
            builder.addInstruction(Opcode::RegisterWrite, {}, registerOperands);
        const std::array<VariableId, 6> memoryWriteOperands = {
            enable, address, mask, data, writeMemory, events[1],
        };
        const InstructionId memoryWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, memoryWriteOperands);
        const std::array<VariableId, 3> memoryFillOperands = {
            packedData, fillMemory, events[2],
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
        return CommitEventFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            },
            .writes = {registerWrite, memoryWrite, memoryFill},
        };
    }

    std::optional<ExecutableModel> scheduleCommitEventFixture(
        CommitEventFixture &&fixture, wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        return schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxAtomsPerBlock = 8,
                .maxCommitAtomsPerBlock = 3,
                .enableCoarsening = true,
            },
            diagnostics);
    }

    int testCommitEventsCanonicalizeChangedDetectors()
    {
        CommitEventFixture fixture =
            makeCommitEventFixture(CommitFixtureEventPattern::Same);
        const std::array<InstructionId, 3> writes = fixture.writes;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            scheduleCommitEventFixture(std::move(fixture), diagnostics);
        const std::optional<BlockId> first = model ? findInstructionBlock(*model, writes[0])
                                                    : std::nullopt;
        const std::optional<BlockId> second = model ? findInstructionBlock(*model, writes[1])
                                                     : std::nullopt;
        const std::optional<BlockId> third = model ? findInstructionBlock(*model, writes[2])
                                                    : std::nullopt;
        // Three distinct changed-event variables over one raw source
        // canonicalize to one event signature: a single commit Block gated by
        // a single head detector.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 || !first ||
            !second || !third || *first != *second || *first != *third ||
            countHeadGateDetectors(*model, *first) != 1 ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("equivalent changed-event detectors did not share one gated commit Block");
        }
        return 0;
    }

    int testDifferentCommitEventsStartNewBlocks()
    {
        CommitEventFixture fixture =
            makeCommitEventFixture(CommitFixtureEventPattern::DifferentMiddle);
        const std::array<InstructionId, 3> writes = fixture.writes;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            scheduleCommitEventFixture(std::move(fixture), diagnostics);
        const std::optional<BlockId> first = model ? findInstructionBlock(*model, writes[0])
                                                    : std::nullopt;
        const std::optional<BlockId> second = model ? findInstructionBlock(*model, writes[1])
                                                     : std::nullopt;
        const std::optional<BlockId> third = model ? findInstructionBlock(*model, writes[2])
                                                    : std::nullopt;
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4 || !first ||
            !second || !third || *first != *third || *first == *second ||
            countHeadGateDetectors(*model, *first) != 1 ||
            countHeadGateDetectors(*model, *second) != 1 ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("different commit events were packed into the same Block");
        }
        return 0;
    }

    int testCommitNextValuesDoNotSplitEventBuckets()
    {
        CommitEventFixture fixture =
            makeCommitEventFixture(CommitFixtureEventPattern::Same);
        const std::array<InstructionId, 3> writes = fixture.writes;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            scheduleCommitEventFixture(std::move(fixture), diagnostics);
        const std::optional<BlockId> block = model ? findInstructionBlock(*model, writes[0])
                                                   : std::nullopt;
        // The commit bucket key is the event signature alone: distinct
        // nextValue expressions (scalar write data vs packed fill data) still
        // share one commit Block, packed in deterministic instruction order
        // behind the single head gate detector.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 || !block ||
            findInstructionBlock(*model, writes[1]) != block ||
            findInstructionBlock(*model, writes[2]) != block ||
            countHeadGateDetectors(*model, *block) != 1) {
            return fail("same-event writes with different nextValues did not share one Block");
        }
        const std::optional<std::size_t> registerPosition =
            blockInstructionPosition(*model, *block, writes[0]);
        const std::optional<std::size_t> memoryPosition =
            blockInstructionPosition(*model, *block, writes[1]);
        const std::optional<std::size_t> fillPosition =
            blockInstructionPosition(*model, *block, writes[2]);
        if (!registerPosition || !memoryPosition || !fillPosition ||
            *registerPosition >= *memoryPosition || *memoryPosition >= *fillPosition ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("same-event commit writes were not packed deterministically");
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
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 1,
                .enableCoarsening = true,
                .collectStats = true,
            },
            diagnostics);
        // The explicit commit chain splits at the commit cap: one write per
        // commit Block (each behind its own head gate detector), ordered by
        // the ordinal edge.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            findInstructionBlock(*model, fixture.registerWrite) != BlockId{1} ||
            findInstructionBlock(*model, fixture.memoryWrite) != BlockId{2}) {
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

    struct MemoryReaderWritersFixture
    {
        LinearProgramArtifact linear;
        VariableId memory;
        InstructionId reader;
        InstructionId firstWrite;
        InstructionId finalWrite;
    };

    MemoryReaderWritersFixture makeMemoryReaderWithOrderedWriters()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId addressType = builder.addType(Type::bitVector(2));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const VariableId address = builder.addVariable(addressType, builder.zeroInit());
        const VariableId enable = builder.addVariable(eventType, builder.zeroInit());
        const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 2> readOperands = {memory, address};
        const std::array<VariableId, 1> readResults = {readData};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryRead, readResults, readOperands);
        const std::array<VariableId, 6> writeOperands = {enable, address, mask, data, memory, event};
        const InstructionId firstWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, writeOperands);
        const InstructionId finalWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, writeOperands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::State, VariableRole::None, VariableRole::None,
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
        return MemoryReaderWritersFixture{
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

    int testMergedMemoryWritersShareOneWatchAndReactivateReader()
    {
        MemoryReaderWritersFixture fixture = makeMemoryReaderWithOrderedWriters();
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(fixture.linear),
            ActivityScheduleOptions{
                .maxAtomsPerBlock = 1,
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
        // The ordered writers share one commit Block, so one tail ChangedAny
        // watch reactivates the reader through ActBackward; the writers keep
        // their ordinal order behind the Block's head gate detector.
        const std::optional<std::size_t> finalPosition =
            model ? blockInstructionPosition(*model, BlockId{2}, fixture.finalWrite)
                  : std::nullopt;
        const std::optional<std::size_t> firstPosition =
            model ? blockInstructionPosition(*model, BlockId{2}, fixture.firstWrite)
                  : std::nullopt;
        if (!readerBlock || !firstWriterBlock || !finalWriterBlock ||
            *readerBlock != BlockId{1} || *finalWriterBlock != BlockId{2} ||
            *firstWriterBlock != BlockId{2} || !finalPosition || !firstPosition ||
            *finalPosition >= *firstPosition ||
            countChangedWatches(*model, fixture.memory) != 1 ||
            !watchActivatesTarget(*model, *finalWriterBlock, fixture.memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("merged memory writers did not share one watch that reactivates the reader");
        }
        return 0;
    }

    // mem.write_lanes (operands [laneMask, data, mem, events...]) must schedule
    // like mem.write: ordered writers on the same array target share one
    // commit Block, one ChangedAny watch, and reactivate the mem.read_all
    // reader through ActBackward.
    int testMergedMemoryWriteLanesShareOneWatchAndReactivateReader()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId maskType = builder.addType(Type::bitVector(4));
        const TypeId dataType = builder.addType(Type::bitVector(32));
        const TypeId memoryType = builder.addType(Type::array(4, 8));
        const VariableId laneMask = builder.addVariable(maskType, builder.zeroInit());
        const VariableId data = builder.addVariable(dataType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId all = builder.addVariable(dataType, builder.undefInit());

        const std::array<VariableId, 1> readResults = {all};
        const std::array<VariableId, 1> readOperands = {memory};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryReadAll, readResults, readOperands);
        const std::array<VariableId, 4> writeOperands = {
            laneMask, data, memory, event,
        };
        const InstructionId firstWrite =
            builder.addInstruction(Opcode::MemoryWriteLanes, {}, writeOperands);
        const InstructionId finalWrite =
            builder.addInstruction(Opcode::MemoryWriteLanes, {}, writeOperands);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::State,
            VariableRole::None, VariableRole::None,
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
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = schedule(
            std::move(linear),
            ActivityScheduleOptions{
                .maxAtomsPerBlock = 1,
                .enableCoarsening = false,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3) {
            return fail("array reader and writers did not form a deterministic schedule");
        }
        const std::optional<BlockId> readerBlock = findInstructionBlock(*model, reader);
        const std::optional<BlockId> firstWriterBlock =
            findInstructionBlock(*model, firstWrite);
        const std::optional<BlockId> finalWriterBlock =
            findInstructionBlock(*model, finalWrite);
        const std::optional<std::size_t> finalPosition =
            model ? blockInstructionPosition(*model, BlockId{2}, finalWrite) : std::nullopt;
        const std::optional<std::size_t> firstPosition =
            model ? blockInstructionPosition(*model, BlockId{2}, firstWrite) : std::nullopt;
        if (!readerBlock || !firstWriterBlock || !finalWriterBlock ||
            *readerBlock != BlockId{1} || *finalWriterBlock != BlockId{2} ||
            *firstWriterBlock != BlockId{2} || !finalPosition || !firstPosition ||
            *finalPosition >= *firstPosition ||
            countChangedWatches(*model, memory) != 1 ||
            !watchActivatesTarget(*model, *finalWriterBlock, memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("merged array writers did not share one watch that reactivates the reader");
        }
        return 0;
    }

    int testSplitCommitBlocksEachWatchAndReactivateReader()
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

        // One mutable event variable gates every writer: the commit Blocks'
        // head detectors fire when the tick changes.
        const VariableId tick = builder.addVariable(eventType, builder.zeroInit());
        const VariableId address = addConstant(addressType, 2);
        const VariableId data = addConstant(valueType, 0x5a);
        const VariableId enable = addConstant(eventType, 1);
        const VariableId mask = addConstant(valueType, 0xff);
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId companionMemory =
            builder.addVariable(memoryType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {readData};
        const std::array<VariableId, 2> readerOperands = {memory, address};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryRead, readerResults, readerOperands);
        const std::array<VariableId, 6> companionOperands = {
            enable, address, mask, data, companionMemory, tick,
        };
        const InstructionId companionWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, companionOperands);
        const std::array<VariableId, 6> earlierOperands = {enable, address, mask, data, memory, tick};
        const InstructionId earlierWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, earlierOperands);
        const InstructionId finalWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, earlierOperands);

        const StringId outputName = builder.addString("read_data");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = readData,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::State, VariableRole::State,
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
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 2,
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
        // The commit cap splits the two memory writers across two commit
        // Blocks; each one watches the memory and reactivates the reader.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 4 ||
            !readerBlock || !earlierBlock || !companionBlock || !finalBlock ||
            *companionBlock != *earlierBlock || *earlierBlock >= *finalBlock ||
            countChangedWatches(*model, memory) != 2 ||
            !watchActivatesTarget(*model, *earlierBlock, memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !watchActivatesTarget(*model, *finalBlock, memory, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("split commit Blocks did not each watch and reactivate the reader");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(tick, InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("split-commit fixture could not enter its write eval");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.roundsExecuted != 2 ||
            interpreter.value(memory).arrayElementWords(2).front() != 0x5a ||
            interpreter.value(readData).lowWord() != 0x5a) {
            return fail("earlier commit Block did not reactivate the reader after its write");
        }
        return 0;
    }

    int testCommitWriteReadsLiveStateFromEarlierCommitBlock()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const auto addConstant = [&builder, &eventType](uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return builder.addVariable(
                eventType, builder.addConstantInit(builder.addBitLiteral(eventType, words)));
        };

        const VariableId tick = builder.addVariable(eventType, builder.zeroInit());
        const VariableId tick2 = builder.addVariable(eventType, builder.zeroInit());
        const VariableId dormant = builder.addVariable(eventType, builder.zeroInit());
        const VariableId one = addConstant(1);
        const VariableId zero = addConstant(0);
        const VariableId stateX = builder.addVariable(eventType, builder.zeroInit());
        const VariableId stateY = builder.addVariable(eventType, builder.zeroInit());
        const VariableId output = builder.addVariable(eventType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {output};
        const std::array<VariableId, 1> readerOperands = {stateY};
        const InstructionId reader =
            builder.addInstruction(Opcode::Assign, readerResults, readerOperands);
        // earlierY's nextValue reads stateX directly, so it observes the live
        // value committed by the earlier commit Block in the same round.
        const std::array<VariableId, 3> earlierXOperands = {one, stateX, tick};
        const InstructionId earlierX =
            builder.addInstruction(Opcode::RegisterWrite, {}, earlierXOperands);
        const std::array<VariableId, 3> finalXOperands = {zero, stateX, dormant};
        const InstructionId finalX =
            builder.addInstruction(Opcode::RegisterWrite, {}, finalXOperands);
        const std::array<VariableId, 3> earlierYOperands = {stateX, stateY, tick2};
        const InstructionId earlierY =
            builder.addInstruction(Opcode::RegisterWrite, {}, earlierYOperands);
        const std::array<VariableId, 3> finalYOperands = {zero, stateY, dormant};
        const InstructionId finalY =
            builder.addInstruction(Opcode::RegisterWrite, {}, finalYOperands);

        const StringId outputName = builder.addString("output");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = output,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::None, VariableRole::State,
            VariableRole::State, VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
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
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 2,
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
        // stateX has only commit-Block readers (the earlierY nextValue), so it
        // gets no tail watch; stateY has a compute reader, so both of its
        // writer Blocks watch it and reactivate the reader.
        if (!model || diagnostics.hasError() || model->program.blockCount() != 6 ||
            !readerBlock || !blockA || !finalXBlock || !blockB || !blockC ||
            !(*readerBlock < *blockA && *blockA < *finalXBlock && *finalXBlock < *blockB &&
              *blockB < *blockC) ||
            countChangedWatches(*model, stateX) != 0 ||
            countChangedWatches(*model, stateY) != 2 ||
            !watchActivatesTarget(*model, *blockB, stateY, Opcode::ActBackward,
                                  *readerBlock) ||
            !watchActivatesTarget(*model, *blockC, stateY, Opcode::ActBackward,
                                  *readerBlock) ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("commit Blocks did not watch their written states for compute readers");
        }

        Interpreter interpreter(*model);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(tick, InterpreterValue::bitVector(
                                   1, Signedness::Unsigned, highWords))
                 .success() ||
            !interpreter
                 .write(tick2, InterpreterValue::bitVector(
                                   1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("live-state commit fixture could not enter its start eval");
        }
        // earlierY reads the live stateX written by the earlier commit Block in
        // the same round; the reader reruns one round later through act.b.
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.roundsExecuted != 2 ||
            interpreter.value(stateX).lowWord() != 1 ||
            interpreter.value(stateY).lowWord() != 1 ||
            interpreter.value(output).lowWord() != 1) {
            return fail("commit write did not observe the live earlier-commit state");
        }
        return 0;
    }

    int testStateChangeUsesBackwardReaderActivation()
    {
        LinearProgramBuilder builder;
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const StringId outputName = builder.addString("output");
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId output = builder.addVariable(valueType, builder.undefInit());
        const std::array<VariableId, 1> readResults = {output};
        const std::array<VariableId, 1> readOperands = {state};
        const InstructionId reader =
            builder.addInstruction(Opcode::Assign, readResults, readOperands);
        const std::array<VariableId, 3> writeOperands = {data, state, event};
        const InstructionId writer =
            builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = output,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::State,
            VariableRole::ExternalOutput,
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
                                                            .maxAtomsPerBlock = 1,
                                                            .enableCoarsening = false,
                                                        },
                                                        diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 3 ||
            model->program.blockInstruction(BlockId{1}, 0) != reader ||
            findInstructionBlock(*model, writer) != BlockId{2}) {
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
            return fail("commit Block did not reactivate the exact reader through act.b");
        }
        return 0;
    }

    int testSeparateCommitEventBucketsReactivateMemoryReader()
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
        const VariableId address = addConstant(addressType, 2);
        const VariableId writeEvent = builder.addVariable(eventType, builder.zeroInit());
        const VariableId enable = addConstant(eventType, 1);
        const VariableId mask = addConstant(valueType, 0xff);
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId memory = builder.addVariable(memoryType, builder.zeroInit());
        const VariableId registerState = builder.addVariable(valueType, builder.zeroInit());
        const VariableId readData = builder.addVariable(valueType, builder.undefInit());

        const std::array<VariableId, 1> readerResults = {readData};
        const std::array<VariableId, 2> readerOperands = {memory, address};
        const InstructionId reader =
            builder.addInstruction(Opcode::MemoryRead, readerResults, readerOperands);
        // The latch write is eventless: its commit Block gates on a
        // ChangedAny watch over the data operand.
        const std::array<VariableId, 2> registerOperands = {data, registerState};
        const InstructionId registerWrite =
            builder.addInstruction(Opcode::LatchWrite, {}, registerOperands);
        const std::array<VariableId, 6> memoryOperands = {enable, address, mask, data, memory, writeEvent};
        const InstructionId memoryWrite =
            builder.addInstruction(Opcode::MemoryWriteCondMask, {}, memoryOperands);

        const StringId outputName = builder.addString("read_data");
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = outputName,
                .direction = PortDirection::Output,
                .output = readData,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None, VariableRole::None, VariableRole::None, VariableRole::None,
            VariableRole::None, VariableRole::State, VariableRole::State,
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
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 2,
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
            return fail("different-event commits did not form separate commit Blocks");
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
            return fail("scheduled commit fixture could not enter its write eval");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || result.roundsExecuted != 2 ||
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
        VariableId tick;
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
        const VariableId start = builder.addVariable(type, builder.zeroInit());
        const VariableId tick = builder.addVariable(type, builder.zeroInit());
        const VariableId stateA = builder.addVariable(type, builder.zeroInit());
        const VariableId stateB = builder.addVariable(type, builder.zeroInit());
        const VariableId stateC = builder.addVariable(type, builder.zeroInit());
        const VariableId oldA = builder.addVariable(type, builder.undefInit());
        const VariableId oldB = builder.addVariable(type, builder.undefInit());
        const VariableId eventA = builder.addVariable(type, builder.zeroInit());
        const VariableId eventB = builder.addVariable(type, builder.zeroInit());

        // Each writer's gate watches the previous commit's state: writerB
        // fires on a stateA change, writerC on a stateB change, so the chain
        // resolves in static Block order once writerA commits.
        builder.addInstruction(Opcode::ChangedAny, std::array{eventA},
                               std::array{stateA, oldA});
        builder.addInstruction(Opcode::ChangedAny, std::array{eventB},
                               std::array{stateB, oldB});
        const InstructionId writerA = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{start, stateA, tick});
        const InstructionId writerB = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{stateA, stateB, eventA});
        const InstructionId writerC = builder.addInstruction(
            Opcode::RegisterWrite, {}, std::array{stateB, stateC, eventB});

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::None,
            VariableRole::None,
            VariableRole::State,
            VariableRole::State,
            VariableRole::State,
            VariableRole::None,
            VariableRole::None,
            VariableRole::None,
            VariableRole::None,
        };
        facts.instructionEffects = {
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
        };
        return CommitCycleFixture{
            .linear = LinearProgramArtifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            },
            .start = start,
            .tick = tick,
            .stateA = stateA,
            .stateB = stateB,
            .stateC = stateC,
            .writerA = writerA,
            .writerB = writerB,
            .writerC = writerC,
        };
    }

    int testCommitCycleResolvesAcrossRoundsInStaticOrder()
    {
        const ActivityScheduleOptions options{
            .maxAtomsPerBlock = 1,
            .maxCommitAtomsPerBlock = 1,
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
        // One writer per commit Block at the cap, ascending in static order:
        // A (tick bucket) before B (stateA bucket) before C (stateB bucket).
        if (!first || !second || firstDiagnostics.hasError() || secondDiagnostics.hasError() ||
            !blockA || !blockB || !blockC || !(*blockA < *blockB && *blockB < *blockC) ||
            first->program.blockCount() != 6 ||
            first->commitBlockBegin != blockA->value ||
            first->commitBlockEnd != first->program.blockCount() ||
            programShape(*first) != programShape(*second) ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("cyclic commits did not form a deterministic ordered commit suffix");
        }

        Interpreter interpreter(*first);
        const std::array<uint64_t, 1> highWords = {1};
        if (!interpreter.ready() || !interpreter.eval().success() ||
            !interpreter
                 .write(firstFixture.start,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success() ||
            !interpreter
                 .write(firstFixture.tick,
                        InterpreterValue::bitVector(1, Signedness::Unsigned, highWords))
                 .success()) {
            return fail("cyclic commit fixture could not enter its source eval");
        }
        const InterpreterResult result = interpreter.eval();
        if (!result.success() || interpreter.value(firstFixture.stateA).lowWord() != 1 ||
            interpreter.value(firstFixture.stateB).lowWord() != 1 ||
            interpreter.value(firstFixture.stateC).lowWord() != 1) {
            return fail("cyclic commit execution did not converge to the expected state");
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

    int testPureComputeChainPacksDeterministically()
    {
        constexpr uint32_t instructionCount = 5;
        const ActivityScheduleOptions coarsenedOptions{
            .maxAtomsPerBlock = 2,
            .enableCoarsening = true,
            // Explicit budget so this scenario does not depend on the
            // automatic coarsen-budget default.
            .dpCoarsenAtomBudget = 64,
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
                .maxAtomsPerBlock = 2,
                .enableCoarsening = false,
            },
            uncoarsenedDiagnostics);
        // Coarsening contracts the whole chain into one cluster; without it the
        // segment DP packs the chain into cap-bounded contiguous segments.
        if (!first || !second || !uncoarsened || firstDiagnostics.hasError() ||
            secondDiagnostics.hasError() || uncoarsenedDiagnostics.hasError() ||
            first->program.blockCount() != 2 || uncoarsened->program.blockCount() != 4) {
            return fail("pure compute chain did not pack into deterministic blocks");
        }
        if (first->program.blockSize(BlockId{1}) < instructionCount) {
            return fail("coarsened pure compute chain was split across blocks");
        }
        for (uint32_t index = 0; index < instructionCount; ++index) {
            if (first->program.blockInstruction(BlockId{1}, index) != InstructionId{index}) {
                return fail("coarsened pure compute chain lost its topological order");
            }
        }
        if (uncoarsened->program.blockSize(BlockId{1}) < 1 ||
            uncoarsened->program.blockInstruction(BlockId{1}, 0) != InstructionId{0} ||
            uncoarsened->program.blockSize(BlockId{2}) < 2 ||
            uncoarsened->program.blockInstruction(BlockId{2}, 0) != InstructionId{1} ||
            uncoarsened->program.blockInstruction(BlockId{2}, 1) != InstructionId{2} ||
            uncoarsened->program.blockSize(BlockId{3}) < 2 ||
            uncoarsened->program.blockInstruction(BlockId{3}, 0) != InstructionId{3} ||
            uncoarsened->program.blockInstruction(BlockId{3}, 1) != InstructionId{4}) {
            return fail("pure compute packing did not preserve contiguous topological segments");
        }
        for (uint32_t block = 1; block < uncoarsened->program.blockCount(); ++block) {
            std::size_t semanticInstructions = 0;
            for (std::size_t position = 0;
                 position < uncoarsened->program.blockSize(BlockId{block}); ++position) {
                const InstructionId instruction =
                    uncoarsened->program.blockInstruction(BlockId{block}, position);
                semanticInstructions += instruction.value < instructionCount ? 1U : 0U;
            }
            if (semanticInstructions > 2) {
                return fail("pure compute block exceeded the configured instruction cap");
            }
        }
        if (programShape(*first) != programShape(*second) ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success() ||
            !validate(*uncoarsened, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success()) {
            return fail("pure compute packing is not deterministic or semantic");
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
        const std::array<VariableId, 3> writeOperands = {data, state, event};
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
            VariableRole::None, VariableRole::State, VariableRole::None,
            VariableRole::None, VariableRole::None, VariableRole::None,
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
            .maxAtomsPerBlock = 1,
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
            return fail("forward and backward activity edges lost their stable block placement");
        }
        return 0;
    }

    int testPureSccFormsOneDeterministicBlock()
    {
        const ActivityScheduleOptions options{
            .maxAtomsPerBlock = 8,
            .enableCoarsening = false,
        };
        wolvrix::lib::diag::Diagnostics firstDiagnostics;
        std::optional<ExecutableModel> first = schedule(makePureCycle(), options, firstDiagnostics);
        wolvrix::lib::diag::Diagnostics secondDiagnostics;
        std::optional<ExecutableModel> second =
            schedule(makePureCycle(), options, secondDiagnostics);
        // A pure def-use SCC stays one indivisible compute Block. Compute
        // Blocks never carry act.b; nothing reactivates the Block, so it runs
        // once per activation in the round model.
        if (!first || !second || firstDiagnostics.hasError() || secondDiagnostics.hasError() ||
            first->program.blockCount() != 2 ||
            first->program.blockInstruction(BlockId{1}, 0) != InstructionId{0} ||
            first->program.blockInstruction(BlockId{1}, 1) != InstructionId{1} ||
            first->program.blockSize(BlockId{1}) != 2) {
            return fail("pure def-use SCC was not scheduled as one deterministic block");
        }
        if (programShape(*first) != programShape(*second) ||
            !validate(*first, ValidationOptions{.level = ValidationLevel::Semantic}).success()) {
            return fail("pure SCC schedule is not deterministic or semantically valid");
        }
        return 0;
    }

    int testOversizedIndivisibleAtomFormsOneBlock()
    {
        const ActivityScheduleOptions options{
            .maxAtomsPerBlock = 1,
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

    bool lineMatches(const std::string &line, const std::string &prefix,
                     const std::string &suffix)
    {
        return line.size() >= prefix.size() + suffix.size() &&
               line.compare(0, prefix.size(), prefix) == 0 &&
               line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    int testInstructionGraphExportWritesJsonl()
    {
        namespace fs = std::filesystem;
        const fs::path exportDir =
            fs::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "instruction-graph-export";
        std::error_code fsError;
        fs::create_directories(exportDir, fsError);
        const fs::path exportPath = exportDir / "graph.jsonl";

        LinearProgramBuilder builder;
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const StringId firstName = builder.addString("first_host_read");
        const StringId secondName = builder.addString("second_host_read");
        const VariableId input = builder.addVariable(valueType, builder.zeroInit());
        const VariableId valueA = builder.addVariable(valueType, builder.undefInit());
        const VariableId valueB = builder.addVariable(valueType, builder.undefInit());
        const VariableId loopLhs = builder.addVariable(valueType, builder.zeroInit());
        const VariableId loopRhs = builder.addVariable(valueType, builder.zeroInit());
        const VariableId firstResult = builder.addVariable(valueType, builder.undefInit());
        const VariableId secondResult = builder.addVariable(valueType, builder.undefInit());
        const VariableId data = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());

        const std::array<VariableId, 1> readResults = {valueA};
        const std::array<VariableId, 1> readOperands = {input};
        const InstructionId readInput =
            builder.addInstruction(Opcode::Assign, readResults, readOperands);
        const std::array<VariableId, 1> passResults = {valueB};
        const std::array<VariableId, 1> passOperands = {valueA};
        const InstructionId passA =
            builder.addInstruction(Opcode::Assign, passResults, passOperands);
        const std::array<VariableId, 1> lhsResults = {loopLhs};
        const std::array<VariableId, 1> lhsOperands = {loopRhs};
        const InstructionId loopForward =
            builder.addInstruction(Opcode::Assign, lhsResults, lhsOperands);
        const std::array<VariableId, 1> rhsResults = {loopRhs};
        const std::array<VariableId, 1> rhsOperands = {loopLhs};
        const InstructionId loopBackward =
            builder.addInstruction(Opcode::Assign, rhsResults, rhsOperands);
        const std::array<VariableId, 1> firstResults = {firstResult};
        const InstructionId firstHostRead =
            builder.addInstruction(Opcode::SystemFunction, firstResults, {});
        builder.setSystemFunctionAttributes(firstHostRead,
                                            SystemFunctionAttributes{
                                                .name = firstName,
                                                .schedule = CallSchedule::Normal,
                                                .hasSideEffects = false,
                                            });
        const std::array<VariableId, 1> secondResults = {secondResult};
        const InstructionId secondHostRead =
            builder.addInstruction(Opcode::SystemFunction, secondResults, {});
        builder.setSystemFunctionAttributes(secondHostRead,
                                            SystemFunctionAttributes{
                                                .name = secondName,
                                                .schedule = CallSchedule::Normal,
                                                .hasSideEffects = false,
                                            });
        const std::array<VariableId, 3> writeOperands = {data, state, event};
        const InstructionId writer =
            builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);

        SchedulingFacts facts;
        facts.variableRoles.assign(10, VariableRole::None);
        facts.variableRoles[state.value] = VariableRole::State;
        facts.instructionEffects = {
            InstructionEffect::Pure,     InstructionEffect::Pure,
            InstructionEffect::Pure,     InstructionEffect::Pure,
            InstructionEffect::HostRead, InstructionEffect::HostRead,
            InstructionEffect::StateReadWrite,
        };
        facts.orderedEffects = {
            OrderedEffect{.instruction = firstHostRead, .group = 5, .ordinal = 0},
            OrderedEffect{.instruction = secondHostRead, .group = 5, .ordinal = 1},
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        setenv("WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL", exportPath.string().c_str(), 1);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            schedule(std::move(linear),
                     ActivityScheduleOptions{
                         .maxAtomsPerBlock = 8,
                         .enableCoarsening = false,
                     },
                     diagnostics);
        unsetenv("WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL");
        if (!model || diagnostics.hasError()) {
            return fail("instruction graph export run was not scheduled cleanly");
        }

        std::ifstream in(exportPath);
        if (!in) {
            return fail("instruction graph export did not create the JSONL file");
        }
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            lines.push_back(line);
        }
        // 1 header + 7 nodes + 3 def-use edges + 3 external reads + 1 order edge.
        if (lines.size() != 15) {
            return fail("instruction graph export wrote an unexpected line count: " +
                        std::to_string(lines.size()));
        }
        const std::string expectedHeader =
            "{\"record\":\"header\",\"format\":\"wolvrix.am-instruction-graph.v1\","
            "\"instructions\":7,\"variables\":10,\"atoms\":6,\"comb_loop_atoms\":1,"
            "\"def_use_edges\":3,\"external_reads\":3,\"order_edges\":1}";
        if (lines.front() != expectedHeader) {
            return fail("instruction graph export header mismatch: " + lines.front());
        }

        const auto nodeLineOk = [&](uint32_t id, Opcode opcode, uint64_t width,
                                    bool stateWrite, bool combLoop) {
            const std::string prefix =
                "{\"record\":\"node\",\"id\":" + std::to_string(id) +
                ",\"op\":" + std::to_string(static_cast<uint8_t>(opcode)) + ",\"opcode\":\"" +
                std::string(toString(opcode)) + "\",\"width\":" + std::to_string(width) +
                ",\"state_write\":" + (stateWrite ? "true" : "false") + ",\"atom\":";
            const std::string suffix = std::string(",\"comb_loop_atom\":") +
                                       (combLoop ? "true" : "false") + "}";
            return lineMatches(lines[1 + id], prefix, suffix);
        };
        if (!nodeLineOk(readInput.value, Opcode::Assign, 8, false, false) ||
            !nodeLineOk(passA.value, Opcode::Assign, 8, false, false) ||
            !nodeLineOk(loopForward.value, Opcode::Assign, 8, false, true) ||
            !nodeLineOk(loopBackward.value, Opcode::Assign, 8, false, true) ||
            !nodeLineOk(firstHostRead.value, Opcode::SystemFunction, 8, false, false) ||
            !nodeLineOk(secondHostRead.value, Opcode::SystemFunction, 8, false, false) ||
            !nodeLineOk(writer.value, Opcode::RegisterWrite, 0, true, false)) {
            return fail("instruction graph export node records are incomplete");
        }

        const auto defUseEdge = [&](VariableId variable, InstructionId source,
                                    InstructionId target, uint64_t width) {
            return std::string("{\"record\":\"edge\",\"kind\":\"def_use\",\"src\":") +
                   std::to_string(source.value) + ",\"dst\":" + std::to_string(target.value) +
                   ",\"var\":" + std::to_string(variable.value) +
                   ",\"width\":" + std::to_string(width) + "}";
        };
        const auto externalRead = [&](VariableId variable, InstructionId target,
                                      uint64_t width) {
            return std::string("{\"record\":\"edge\",\"kind\":\"external_read\",\"dst\":") +
                   std::to_string(target.value) + ",\"var\":" +
                   std::to_string(variable.value) + ",\"width\":" + std::to_string(width) + "}";
        };
        const std::set<std::string> edgeLines(lines.begin() + 8, lines.end());
        const std::string orderEdge = std::string("{\"record\":\"edge\",\"kind\":\"order\",\"src\":") +
                                      std::to_string(firstHostRead.value) +
                                      ",\"dst\":" + std::to_string(secondHostRead.value) + "}";
        if (edgeLines.count(defUseEdge(valueA, readInput, passA, 8)) != 1 ||
            edgeLines.count(defUseEdge(loopLhs, loopForward, loopBackward, 8)) != 1 ||
            edgeLines.count(defUseEdge(loopRhs, loopBackward, loopForward, 8)) != 1 ||
            edgeLines.count(externalRead(input, readInput, 8)) != 1 ||
            edgeLines.count(externalRead(data, writer, 8)) != 1 ||
            edgeLines.count(externalRead(event, writer, 1)) != 1 ||
            edgeLines.count(orderEdge) != 1) {
            return fail("instruction graph export edge records are incomplete");
        }
        return 0;
    }

    int testInstructionGraphExportFailureIsReported()
    {
        namespace fs = std::filesystem;
        const fs::path exportDir =
            fs::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "instruction-graph-export";
        std::error_code fsError;
        fs::create_directories(exportDir, fsError);
        const fs::path blocker = exportDir / "blocker";
        {
            std::ofstream out(blocker);
            out << "not a directory\n";
        }
        const fs::path badPath = blocker / "graph.jsonl";

        setenv("WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL", badPath.string().c_str(), 1);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            schedule(makePureCycle(),
                     ActivityScheduleOptions{
                         .maxAtomsPerBlock = 8,
                         .enableCoarsening = false,
                     },
                     diagnostics);
        unsetenv("WOLVRIX_GRHSIM_AM_INSTRUCTION_GRAPH_JSONL");
        if (model || !diagnostics.hasError()) {
            return fail("instruction graph export failure was not surfaced as an error");
        }
        return 0;
    }

    int testBlockAssignmentExportWritesJsonl()
    {
        namespace fs = std::filesystem;
        const fs::path exportDir =
            fs::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "instruction-graph-export";
        std::error_code fsError;
        fs::create_directories(exportDir, fsError);
        const fs::path exportPath = exportDir / "block_assignment.jsonl";

        LinearProgramBuilder builder;
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId eventType = builder.addType(Type::bitVector(1));
        const VariableId input = builder.addVariable(valueType, builder.zeroInit());
        const VariableId valueA = builder.addVariable(valueType, builder.undefInit());
        const VariableId valueB = builder.addVariable(valueType, builder.undefInit());
        const VariableId loopLhs = builder.addVariable(valueType, builder.zeroInit());
        const VariableId loopRhs = builder.addVariable(valueType, builder.zeroInit());
        const VariableId state = builder.addVariable(valueType, builder.zeroInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());

        const std::array<VariableId, 1> readResults = {valueA};
        const std::array<VariableId, 1> readOperands = {input};
        builder.addInstruction(Opcode::Assign, readResults, readOperands);
        const std::array<VariableId, 1> passResults = {valueB};
        const std::array<VariableId, 1> passOperands = {valueA};
        builder.addInstruction(Opcode::Assign, passResults, passOperands);
        const std::array<VariableId, 1> lhsResults = {loopLhs};
        const std::array<VariableId, 1> lhsOperands = {loopRhs};
        builder.addInstruction(Opcode::Assign, lhsResults, lhsOperands);
        const std::array<VariableId, 1> rhsResults = {loopRhs};
        const std::array<VariableId, 1> rhsOperands = {loopLhs};
        builder.addInstruction(Opcode::Assign, rhsResults, rhsOperands);
        const std::array<VariableId, 3> writeOperands = {valueB, state, event};
        builder.addInstruction(Opcode::RegisterWrite, {}, writeOperands);

        SchedulingFacts facts;
        facts.variableRoles.assign(7, VariableRole::None);
        facts.variableRoles[state.value] = VariableRole::State;
        facts.instructionEffects = {
            InstructionEffect::Pure, InstructionEffect::Pure,
            InstructionEffect::Pure, InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
        };
        LinearProgramArtifact linear{
            .program = builder.finish(),
            .schedulingFacts = std::move(facts),
        };

        setenv("WOLVRIX_GRHSIM_AM_BLOCK_ASSIGNMENT_JSONL", exportPath.string().c_str(), 1);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            schedule(std::move(linear),
                     ActivityScheduleOptions{
                         .maxAtomsPerBlock = 8,
                         .enableCoarsening = false,
                     },
                     diagnostics);
        unsetenv("WOLVRIX_GRHSIM_AM_BLOCK_ASSIGNMENT_JSONL");
        if (!model || diagnostics.hasError()) {
            return fail("block assignment export run was not scheduled cleanly");
        }

        std::ifstream in(exportPath);
        if (!in) {
            return fail("block assignment export did not create the JSONL file");
        }
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            lines.push_back(line);
        }
        // 1 header + 2 block records + 5 assign records.
        if (lines.size() != 8) {
            return fail("block assignment export wrote an unexpected line count: " +
                        std::to_string(lines.size()));
        }
        // One compute block (the comb-loop assign pair plus the two value
        // producers) and one commit block. Cone packing is gone: the
        // register write's fan-in stays in compute, so exactly one value
        // crosses the compute->commit boundary (dag_edges/pairs/copy = 1).
        const std::string expectedHeader =
            "{\"record\":\"header\",\"format\":\"wolvrix.am-block-assignment.v1\","
            "\"instructions\":5,\"variables\":7,\"blocks\":2,\"compute_blocks\":1,"
            "\"commit_blocks\":1,\"input_sink_block\":0,\"dag_edges\":1,"
            "\"compute_compute_value_pairs\":1,\"incoming_copy_cost\":1}";
        if (lines.front() != expectedHeader) {
            return fail("block assignment export header mismatch: " + lines.front());
        }
        // Block records carry the post-merge atom count: block 1 packs the
        // comb-loop assign pair as one CombLoopScc atom plus two singleton
        // producers (3 atoms); block 2 is the single CommitEvent write atom.
        if (lines[1] != "{\"record\":\"block\",\"id\":1,\"kind\":\"compute\",\"size\":4,\"atoms\":3}" ||
            lines[2] != "{\"record\":\"block\",\"id\":2,\"kind\":\"commit\",\"size\":1,\"atoms\":1}") {
            return fail("block assignment export block records mismatch");
        }
        const std::array<uint32_t, 5> expectedBlocks = {1, 1, 1, 1, 2};
        std::vector<uint32_t> recordAtoms(5, UINT32_MAX);
        for (uint32_t instruction = 0; instruction < 5; ++instruction) {
            const std::string &line = lines[3 + instruction];
            const std::string prefix = std::string("{\"record\":\"assign\",\"instr\":") +
                                       std::to_string(instruction) + ",\"block\":" +
                                       std::to_string(expectedBlocks[instruction]) +
                                       ",\"atom\":";
            if (!line.starts_with(prefix) || line.back() != '}') {
                return fail("block assignment export assign record mismatch: " + line);
            }
            const std::string atomText =
                line.substr(prefix.size(), line.size() - prefix.size() - 1);
            uint32_t atom = UINT32_MAX;
            const auto [end, error] =
                std::from_chars(atomText.data(), atomText.data() + atomText.size(), atom);
            if (error != std::errc{} || end != atomText.data() + atomText.size()) {
                return fail("block assignment export atom field is not a number: " + line);
            }
            recordAtoms[instruction] = atom;
        }
        // The comb-loop pair shares one atom; the two chain producers are
        // distinct singletons; the write is its own commit atom.
        if (recordAtoms[2] != recordAtoms[3] || recordAtoms[0] == recordAtoms[2] ||
            recordAtoms[1] == recordAtoms[2] || recordAtoms[0] == recordAtoms[1] ||
            recordAtoms[4] == recordAtoms[2]) {
            return fail("block assignment export atom fields do not reflect the SCC packing");
        }
        return 0;
    }

    // split-am-graph must isolate the two induced subgraphs (no cross-class
    // edges inside either subgraph), and the two partition passes bucket the
    // atoms as expected: direct assertions on the split and on both
    // partition results.
    int testComputeCommitGraphSplitAndPartitions()
    {
        constexpr uint32_t kAtoms = 9; // 0..5 compute, 6..8 commit
        // Edges: 0->1, 0->2, 1->3, 2->3, 3->4, 4->5 (compute DAG),
        //        4->6, 5->7, 5->8 (compute->commit boundary), 6->7 (commit chain).
        const std::vector<std::pair<uint32_t, uint32_t>> edges = {
            {0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4}, {4, 5}, {4, 6}, {5, 7}, {5, 8}, {6, 7},
        };
        std::vector<uint32_t> atomOffsets(kAtoms + 1, 0);
        for (const auto &[source, target] : edges) {
            ++atomOffsets[source + 1];
        }
        std::partial_sum(atomOffsets.begin(), atomOffsets.end(), atomOffsets.begin());
        std::vector<uint32_t> atomTargets(atomOffsets.back());
        {
            std::vector<uint32_t> cursor(atomOffsets.begin(), atomOffsets.end() - 1);
            for (const auto &[source, target] : edges) {
                atomTargets[cursor[source]++] = target;
            }
        }
        const std::vector<uint32_t> atomInstructions(kAtoms, 1);
        const std::vector<uint32_t> atomStateWrites = {0, 0, 0, 0, 0, 0, 1, 1, 1};
        const std::vector<uint8_t> atomIsCommit = {0, 0, 0, 0, 0, 0, 1, 1, 1};
        std::vector<uint32_t> atomMinInstruction(kAtoms);
        std::iota(atomMinInstruction.begin(), atomMinInstruction.end(), uint32_t{0});
        const std::vector<uint32_t> commitRanks = {0, 0, 0, 0, 0, 0, 0, 0, 1};
        const uint32_t none = std::numeric_limits<uint32_t>::max();
        const std::vector<uint32_t> definitions = {0, none};
        const std::vector<uint32_t> useOffsets = {0, 4, 5};
        const std::vector<uint32_t> uses = {1, 2, 3, 6, 7};
        std::vector<uint32_t> instructionAtom(kAtoms);
        std::iota(instructionAtom.begin(), instructionAtom.end(), uint32_t{0});

        AmGraphPartitionInput input{
            .atomCount = kAtoms,
            .atomOffsets = atomOffsets,
            .atomTargets = atomTargets,
            .atomInstructions = atomInstructions,
            .atomStateWrites = atomStateWrites,
            .atomIsCommit = atomIsCommit,
            .atomMinInstruction = atomMinInstruction,
            .commitEventRank = commitRanks,
            .variableCount = 2,
            .definitions = definitions,
            .useOffsets = useOffsets,
            .uses = uses,
            .instructionAtom = instructionAtom,
            .maxAtomsPerBlock = 60,
            .enableCoarsening = false,
            .segmentPenalty = 1.0,
        };

        std::string error;
        auto split = splitAmGraph(input, error);
        if (!split) {
            return fail("graph split failed: " + error);
        }
        if (split->computeGraph.atomCount != 6 || split->commitGraph.atomCount != 3) {
            return fail("split atom counts mismatch");
        }
        // Compute subgraph keeps only the 6 compute->compute edges; the commit
        // subgraph keeps only 6->7 (local 0->1). Boundary edges 4->6/5->7/5->8
        // belong to neither subgraph.
        if (split->computeGraph.targets.size() != 6 || split->commitGraph.targets.size() != 1) {
            return fail("split induced edge counts mismatch");
        }
        optAmComputeGraph(split->computeGraph, input);
        const auto compute = partitionAmComputeGraph(input, *split, error);
        if (!compute) {
            return fail("compute partition failed: " + error);
        }
        const auto commit = partitionAmCommitGraph(input, *split, error);
        if (!commit) {
            return fail("commit partition failed: " + error);
        }
        // Six single-instruction compute atoms under a capacity-60 segment DP
        // with unit penalty: one block holds them all.
        if (compute->blockCount != 1) {
            return fail("compute partition should form one block");
        }
        for (uint32_t local = 0; local < split->computeGraph.atomCount; ++local) {
            if (compute->atomBlock[local] != 1) {
                return fail("every compute atom should land in the single compute block");
            }
        }
        // Event clustering: same-rank commit atoms 6/7 (local 0/1) share one
        // block; rank 1 atom 8 (local 2) forms another.
        if (commit->blockCount != 2 || commit->atomBlock[0] != commit->atomBlock[1] ||
            commit->atomBlock[0] == commit->atomBlock[2]) {
            return fail("commit event clustering mismatch");
        }
        return 0;
    }

    // Post-DP local-move refinement: on a crafted graph the segment DP's
    // contiguous segmentation is uniquely suboptimal and the refinement must
    // make exactly one improving move, then converge -- without introducing
    // any backward (def-after-use across blocks) edge.
    //
    // Graph (instruction = atom, sizes {2,1,1,1,1,1}, cap 3, penalty 1.0):
    //   edges: 0->2, 0->3, 1->3, 1->4, 2->5, 3->5, 4->5
    //   vars:  v1..v7 defined by {0,0,1,1,2,3,4};
    //          v1->use{2}, v2->{3}, v3->{3}, v4->{4}, v5->{5}, v6->{5}, v7->{5}
    // The DP optimum is uniquely [0],[1,2],[3,4,5] (pairs 5, cost 5+3=8; next
    // best costs 9). Refinement moves instruction 2 from block 2 to block 1
    // (delta -1: v1 stops crossing; v5's consumer is in block 3 either way),
    // then converges: pairs 4, blocks [0,2],[1],[3,4,5].
    int testRefinementMovesClusterIntoNeighborBlock()
    {
        // The Kernighan DP segments a fixed topological sequence by edge
        // cuts, so it is blind to external variables (no defining
        // instruction, hence no DAG edge): the DP puts atom 1 with atom 2 in
        // block 2 although atom 1's only data input v0 is defined in block 1.
        // Refinement must move atom 1 into block 1, removing one
        // (variable, block) incoming pair; atom 2 stays behind because
        // joining block 1 as well would exceed the block cap.
        constexpr uint32_t kAtoms = 3;
        const std::vector<std::pair<uint32_t, uint32_t>> edges = {
            {0, 1}, {1, 2},
        };
        std::vector<uint32_t> atomOffsets(kAtoms + 1, 0);
        for (const auto &[source, target] : edges) {
            ++atomOffsets[source + 1];
        }
        std::partial_sum(atomOffsets.begin(), atomOffsets.end(), atomOffsets.begin());
        std::vector<uint32_t> atomTargets(atomOffsets.back());
        {
            std::vector<uint32_t> cursor(atomOffsets.begin(), atomOffsets.end() - 1);
            for (const auto &[source, target] : edges) {
                atomTargets[cursor[source]++] = target;
            }
        }
        const std::vector<uint32_t> atomInstructions(kAtoms, 1);
        const std::vector<uint32_t> atomStateWrites(kAtoms, 0);
        const std::vector<uint8_t> atomIsCommit(kAtoms, 0);
        std::vector<uint32_t> atomMinInstruction(kAtoms);
        std::iota(atomMinInstruction.begin(), atomMinInstruction.end(), uint32_t{0});
        const std::vector<uint32_t> commitRanks(kAtoms, 0);
        const uint32_t none = std::numeric_limits<uint32_t>::max();
        // v0: defined by instruction 0, used by instruction 1 -- the only
        // variable refinement can re-home. v1: external (no definition),
        // used by instruction 2.
        const std::vector<uint32_t> definitions = {0, none};
        const std::vector<uint32_t> useOffsets = {0, 1, 2};
        const std::vector<uint32_t> uses = {1, 2};
        std::vector<uint32_t> instructionAtom(kAtoms);
        std::iota(instructionAtom.begin(), instructionAtom.end(), uint32_t{0});

        const auto runCase = [&](std::size_t refinementRounds) {
            AmGraphPartitionInput input{
                .atomCount = kAtoms,
                .atomOffsets = atomOffsets,
                .atomTargets = atomTargets,
                .atomInstructions = atomInstructions,
                .atomStateWrites = atomStateWrites,
                .atomIsCommit = atomIsCommit,
                .atomMinInstruction = atomMinInstruction,
                .commitEventRank = commitRanks,
                .variableCount = 2,
                .definitions = definitions,
                .useOffsets = useOffsets,
                .uses = uses,
                .instructionAtom = instructionAtom,
                .maxAtomsPerBlock = 2,
                .enableCoarsening = false,
                .segmentPenalty = 1.0,
                .refinementRounds = refinementRounds,
            };
            std::string error;
            auto split = splitAmGraph(input, error);
            if (!split) {
                return std::optional<AmComputeActivityGraph>{};
            }
            optAmComputeGraph(split->computeGraph, input);
            return partitionAmComputeGraph(input, *split, error);
        };

        const auto baseline = runCase(0);
        if (!baseline) {
            return fail("baseline partition failed");
        }
        const std::vector<uint32_t> expectedBaseline = {1, 2, 2};
        if (baseline->atomBlock != expectedBaseline || baseline->refinementMoves != 0) {
            return fail("baseline DP assignment mismatch");
        }

        const auto refined = runCase(10);
        if (!refined) {
            return fail("refined partition failed");
        }
        if (refined->refinementMoves != 1) {
            return fail("expected exactly one refinement move, got " +
                        std::to_string(refined->refinementMoves));
        }
        if (refined->refinementCostBefore != 2.0 || refined->refinementCostAfter != 1.0) {
            return fail("refinement cost should drop 2 -> 1");
        }
        const std::vector<uint32_t> expectedRefined = {1, 1, 2};
        if (refined->atomBlock != expectedRefined) {
            return fail("refinement should move atom 1 into block 1");
        }
        if (refined->refinementRounds != 2) {
            return fail("refinement should converge after one idle round");
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
    if (const int result = testImplicitCommitBeforeHostDependencyIsRejected(); result != 0) {
        return result;
    }
    if (const int result = testOrderedMemoryWritersStayOrderedInSharedCommitBlock(); result != 0) {
        return result;
    }
    if (const int result = testOrderedStateAndMemoryWritesShareOneCommitBlock(); result != 0) {
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
    if (const int result = testCommitNextValuesDoNotSplitEventBuckets(); result != 0) {
        return result;
    }
    if (const int result = testOrderedCommitChainSplitsAtCommitCap(); result != 0) {
        return result;
    }
    if (const int result = testMergedMemoryWritersShareOneWatchAndReactivateReader(); result != 0) {
        return result;
    }
    if (const int result = testMergedMemoryWriteLanesShareOneWatchAndReactivateReader(); result != 0) {
        return result;
    }
    if (const int result = testSplitCommitBlocksEachWatchAndReactivateReader(); result != 0) {
        return result;
    }
    if (const int result = testCommitWriteReadsLiveStateFromEarlierCommitBlock(); result != 0) {
        return result;
    }
    if (const int result = testStateChangeUsesBackwardReaderActivation(); result != 0) {
        return result;
    }
    if (const int result = testSeparateCommitEventBucketsReactivateMemoryReader(); result != 0) {
        return result;
    }
    if (const int result = testCommitCycleResolvesAcrossRoundsInStaticOrder(); result != 0) {
        return result;
    }
    if (const int result = testPureComputeChainPacksDeterministically(); result != 0) {
        return result;
    }
    if (const int result = testMixedForwardBackwardActivationIsDeterministic(); result != 0) {
        return result;
    }
    if (const int result = testPureSccFormsOneDeterministicBlock(); result != 0) {
        return result;
    }
    if (const int result = testOversizedIndivisibleAtomFormsOneBlock(); result != 0) {
        return result;
    }
    if (const int result = testInstructionGraphExportWritesJsonl(); result != 0) {
        return result;
    }
    if (const int result = testInstructionGraphExportFailureIsReported(); result != 0) {
        return result;
    }
    if (const int result = testBlockAssignmentExportWritesJsonl(); result != 0) {
        return result;
    }
    if (const int result = testComputeCommitGraphSplitAndPartitions(); result != 0) {
        return result;
    }
    if (const int result = testRefinementMovesClusterIntoNeighborBlock(); result != 0) {
        return result;
    }
    return 0;
}
