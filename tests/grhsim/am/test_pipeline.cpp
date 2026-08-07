#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_opcode_traits.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim_am_pipeline] " << message << '\n';
        return 1;
    }

    bool containsError(const ValidationResult &result, std::string_view needle)
    {
        for (const std::string &error : result.errors)
        {
            if (error.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    enum class LowerBehavior
    {
        Success,
        ReturnFailure,
        InvalidInterface,
        InvalidSchedulingFacts,
    };

    enum class EmitBehavior
    {
        Success,
        ReturnFailure,
    };

    struct MockState
    {
        std::vector<std::string> calls;
        bool lowerSawGraph = false;
        bool emitterSawScheduledProgram = false;
        bool emitterSawInterface = false;
        bool emitterSawOptions = false;
    };

    LinearProgramArtifact makeLinearArtifact()
    {
        LinearProgramBuilder builder;
        const TypeId type = builder.addType(Type::bitVector(1));
        const StringId portName = builder.addString("clock");
        const StringId variableName = builder.addString("top.clock");
        const VariableId input = builder.addVariable(type, builder.zeroInit(), variableName);

        ProgramInterface interface;
        interface.ports.push_back(PortBinding{
            .name = portName,
            .direction = PortDirection::Input,
            .input = input,
            .output = VariableId::invalid(),
            .outputEnable = VariableId::invalid(),
        });
        interface.declaredVariables.push_back(VariableLabel{
            .variable = input,
            .label = variableName,
        });

        SchedulingFacts facts;
        facts.variableRoles.push_back(VariableRole::ExternalInput | VariableRole::Observable);

        return LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };
    }

    class MockLowering final : public GrhIRToGrhSimAMGraphLoweringStage
    {
    public:
        MockLowering(MockState &state, LowerBehavior behavior)
            : state_(state), behavior_(behavior)
        {
        }

        std::optional<AmGraph>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics) override
        {
            state_.calls.emplace_back("lower");
            state_.lowerSawGraph = graph.symbol() == "pipeline_top";
            if (behavior_ == LowerBehavior::ReturnFailure)
            {
                diagnostics.error("mock lowering failed", "mock-lower");
                return std::nullopt;
            }

            AmGraph lowered = AmGraph::fromLinearProgram(makeLinearArtifact());
            if (behavior_ == LowerBehavior::InvalidInterface)
            {
                lowered.mutableInterface().ports.front().output =
                    lowered.mutableInterface().ports.front().input;
            }
            else if (behavior_ == LowerBehavior::InvalidSchedulingFacts)
            {
                AmValueFacts facts = lowered.valueFacts(VariableId{0});
                facts.roles = VariableRole::None;
                lowered.setValueFacts(VariableId{0}, facts);
            }
            return std::optional<AmGraph>(std::move(lowered));
        }

    private:
        MockState &state_;
        LowerBehavior behavior_;
    };

    class MockEmitter final : public GrhSimAmCppEmitStage
    {
    public:
        MockEmitter(MockState &state, EmitBehavior behavior)
            : state_(state), behavior_(behavior)
        {
        }

        GrhSimAmCppResult
        emit(const ExecutableModel &model,
             const GrhSimAmCppOptions &options,
             wolvrix::lib::diag::Diagnostics &diagnostics) override
        {
            state_.calls.emplace_back("emit");
            state_.emitterSawScheduledProgram =
                model.program.valid() && model.program.blockCount() == 2;
            state_.emitterSawInterface =
                model.interface.ports.size() == 1 &&
                model.interface.ports.front().input == VariableId{0};
            state_.emitterSawOptions =
                options.outputDirectory == std::filesystem::path("pipeline-output") &&
                options.maxOutputFileBytes == 4096 &&
                options.attributes.size() == 1 &&
                options.attributes.at("mode") == "test";

            if (behavior_ == EmitBehavior::ReturnFailure)
            {
                diagnostics.error("mock emit failed", "mock-emit");
                return GrhSimAmCppResult{
                    .success = false,
                    .artifacts = {"partial.cpp"},
                };
            }
            return GrhSimAmCppResult{
                .success = true,
                .artifacts = {"model.cpp", "support.cpp"},
            };
        }

    private:
        MockState &state_;
        EmitBehavior behavior_;
    };

    struct CaseOutcome
    {
        bool success = false;
        bool hasModel = false;
        bool diagnosticsHaveError = false;
        std::size_t modelBlockCount = 0;
        std::vector<std::string> artifacts;
        MockState state;
    };

    CaseOutcome runCase(LowerBehavior lowerBehavior,
                        EmitBehavior emitBehavior)
    {
        MockState state;
        MockLowering lowering(state, lowerBehavior);
        MockEmitter emitter(state, emitBehavior);
        GrhIRToGrhSimAMProgram pipeline(lowering, emitter);

        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("pipeline_top");
        wolvrix::lib::diag::Diagnostics diagnostics;
        const ActivityScheduleOptions scheduleOptions{
            .maxAtomsPerBlock = 17,
            .maxCommitAtomsPerBlock = 23,
            .enableCoarsening = false,
            .collectStats = true,
        };
        GrhSimAmCppOptions emitOptions{
            .outputDirectory = "pipeline-output",
            .maxOutputFileBytes = 4096,
            .attributes = {},
        };
        emitOptions.attributes.emplace("mode", "test");

        GrhIRToGrhSimAMProgramResult result = pipeline.run(
            graph,
            scheduleOptions,
            emitOptions,
            diagnostics);
        const std::size_t blockCount =
            result.model ? result.model->program.blockCount() : 0;
        return CaseOutcome{
            .success = result.success,
            .hasModel = result.model.has_value(),
            .diagnosticsHaveError = diagnostics.hasError(),
            .modelBlockCount = blockCount,
            .artifacts = std::move(result.artifacts),
            .state = std::move(state),
        };
    }

    int testStrictSuccessOrder()
    {
        const CaseOutcome outcome = runCase(
            LowerBehavior::Success,
            EmitBehavior::Success);
        if (!outcome.success || !outcome.hasModel || outcome.diagnosticsHaveError ||
            outcome.modelBlockCount != 2 ||
            outcome.artifacts != std::vector<std::string>{"model.cpp", "support.cpp"})
        {
            return fail("successful pipeline must return its ScheduledProgram and emitted artifacts");
        }
        if (outcome.state.calls != std::vector<std::string>{"lower", "emit"})
        {
            return fail("pipeline stages must run strictly as Linear -> Scheduled -> emit");
        }
        if (!outcome.state.lowerSawGraph ||
            !outcome.state.emitterSawScheduledProgram ||
            !outcome.state.emitterSawInterface || !outcome.state.emitterSawOptions)
        {
            return fail("pipeline must preserve stage inputs, metadata, and options");
        }
        return 0;
    }

    int testStagedPipelineDoesNotRetainGraph()
    {
        MockState state;
        MockLowering lowering(state, LowerBehavior::Success);
        MockEmitter emitter(state, EmitBehavior::Success);
        GrhIRToGrhSimAMProgram pipeline(lowering, emitter);
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<AmGraph> linear;
        {
            wolvrix::lib::grh::Design design;
            wolvrix::lib::grh::Graph &graph = design.createGraph("pipeline_top");
            linear = pipeline.lower(graph, diagnostics);
        }
        if (!linear || diagnostics.hasError() ||
            state.calls != std::vector<std::string>{"lower"})
        {
            return fail("staged pipeline lowering must return an owned artifact");
        }

        const ActivityScheduleOptions scheduleOptions{
            .maxAtomsPerBlock = 17,
            .maxCommitAtomsPerBlock = 23,
            .enableCoarsening = false,
            .collectStats = true,
        };
        GrhSimAmCppOptions emitOptions{
            .outputDirectory = "pipeline-output",
            .maxOutputFileBytes = 4096,
            .attributes = {},
        };
        emitOptions.attributes.emplace("mode", "test");
        GrhIRToGrhSimAMProgramResult result = pipeline.run(
            std::move(*linear),
            scheduleOptions,
            emitOptions,
            diagnostics);
        if (!result.success || !result.model || diagnostics.hasError() ||
            result.artifacts != std::vector<std::string>{"model.cpp", "support.cpp"} ||
            state.calls != std::vector<std::string>{"lower", "emit"})
        {
            return fail("staged pipeline must run after its source Graph has been destroyed");
        }
        return 0;
    }

    int testExistingDiagnosticsDoNotConsumeArtifact()
    {
        MockState state;
        MockLowering lowering(state, LowerBehavior::Success);
        MockEmitter emitter(state, EmitBehavior::Success);
        GrhIRToGrhSimAMProgram pipeline(lowering, emitter);
        AmGraph linear = AmGraph::fromLinearProgram(makeLinearArtifact());
        wolvrix::lib::diag::Diagnostics diagnostics;
        diagnostics.error("existing failure", "test");
        const GrhIRToGrhSimAMProgramResult result = pipeline.run(
            std::move(linear),
            ActivityScheduleOptions{},
            GrhSimAmCppOptions{},
            diagnostics);
        if (result.success || result.model || !result.artifacts.empty() ||
            !linear.program().valid() || !state.calls.empty())
        {
            return fail("an existing diagnostic error must not consume the staged artifact");
        }
        return 0;
    }

    int testLoweringInterfaceGate()
    {
        const CaseOutcome outcome = runCase(
            LowerBehavior::InvalidInterface,
            EmitBehavior::Success);
        if (outcome.success || outcome.hasModel || !outcome.diagnosticsHaveError ||
            outcome.state.calls != std::vector<std::string>{"lower"})
        {
            return fail("invalid lowering ProgramInterface must gate scheduler and emitter");
        }
        return 0;
    }

    int testSchedulingFactsGate()
    {
        const CaseOutcome outcome = runCase(
            LowerBehavior::InvalidSchedulingFacts,
            EmitBehavior::Success);
        if (outcome.success || outcome.hasModel || !outcome.diagnosticsHaveError ||
            outcome.state.calls != std::vector<std::string>{"lower"})
        {
            return fail("invalid SchedulingFacts must gate scheduler and emitter");
        }
        return 0;
    }

    int testExternalInputRoleMustMatchInterface()
    {
        LinearProgramArtifact artifact = makeLinearArtifact();
        artifact.schedulingFacts.variableRoles.front() = VariableRole::None;
        const ValidationResult validation = validate(
            artifact,
            ValidationOptions{.level = ValidationLevel::Semantic});
        if (validation.success() ||
            !containsError(validation, "external-input roles do not match ProgramInterface"))
        {
            return fail("ProgramInterface inputs must carry the ExternalInput scheduling role");
        }
        return 0;
    }

    int testArtifactRolesAndInterfaceExposureContracts()
    {
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const VariableId nextValue = builder.addVariable(valueType, builder.zeroInit());
            const VariableId target = builder.addVariable(valueType, builder.zeroInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            // reg.write [nextValue, target, events...]: the write reaches
            // `target`, which lacks the State scheduling role.
            const std::array<VariableId, 3> operands = {
                nextValue,
                target,
                event,
            };
            const InstructionId write =
                builder.addInstruction(Opcode::RegisterWrite, {}, operands);
            SchedulingFacts facts;
            facts.variableRoles.assign(3, VariableRole::None);
            facts.instructionEffects = {InstructionEffect::StateReadWrite};
            facts.orderedEffects = {
                OrderedEffect{.instruction = write, .group = 0, .ordinal = 0},
            };
            LinearProgramArtifact artifact{
                .program = builder.finish(),
                .schedulingFacts = std::move(facts),
            };
            const ValidationResult validation = validate(
                artifact,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "missing the State variable role"))
            {
                return fail("state instruction targets must carry the State scheduling role");
            }
        }

        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::bitVector(8));
            const StringId outputName = builder.addString("output");
            const VariableId output = builder.addVariable(type, builder.zeroInit());
            ProgramInterface interface;
            interface.ports = {
                PortBinding{
                    .name = outputName,
                    .direction = PortDirection::Output,
                    .output = output,
                },
            };
            SchedulingFacts facts;
            facts.variableRoles = {VariableRole::None};
            LinearProgramArtifact artifact{
                .program = builder.finish(),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            };
            if (validate(artifact, ValidationOptions{.level = ValidationLevel::Semantic}).success())
            {
                return fail("ProgramInterface outputs must carry the ExternalOutput role");
            }
        }

        {
            LinearProgramBuilder builder;
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const StringId portName = builder.addString("old_value");
            const VariableId value = builder.addVariable(valueType, builder.zeroInit());
            const VariableId oldValue = builder.addVariable(valueType, builder.undefInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            const std::array<VariableId, 1> results = {event};
            const std::array<VariableId, 2> operands = {value, oldValue};
            builder.addInstruction(Opcode::ChangedAny, results, operands);
            ProgramInterface interface;
            interface.ports = {
                PortBinding{
                    .name = portName,
                    .direction = PortDirection::Output,
                    .output = oldValue,
                },
            };
            SchedulingFacts facts;
            facts.variableRoles = {
                VariableRole::None,
                VariableRole::ExternalOutput,
                VariableRole::None,
            };
            facts.instructionEffects = {InstructionEffect::StateReadWrite};
            LinearProgramArtifact artifact{
                .program = builder.finish(),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            };
            if (validate(artifact, ValidationOptions{.level = ValidationLevel::Semantic}).success())
            {
                return fail("ProgramInterface ports must not expose changed old storage");
            }
        }

        {
            LinearProgramBuilder builder;
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const StringId eventName = builder.addString("event");
            const VariableId value = builder.addVariable(valueType, builder.zeroInit());
            const VariableId oldValue = builder.addVariable(valueType, builder.undefInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            const std::array<VariableId, 1> results = {event};
            const std::array<VariableId, 2> operands = {value, oldValue};
            builder.addInstruction(Opcode::ChangedAny, results, operands);
            ProgramInterface interface;
            interface.declaredVariables = {
                VariableLabel{.variable = event, .label = eventName},
            };
            SchedulingFacts facts;
            facts.variableRoles = {
                VariableRole::None,
                VariableRole::None,
                VariableRole::Observable,
            };
            facts.instructionEffects = {InstructionEffect::StateReadWrite};
            LinearProgramArtifact artifact{
                .program = builder.finish(),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            };
            if (validate(artifact, ValidationOptions{.level = ValidationLevel::Semantic}).success())
            {
                return fail("declared-variable mappings must not expose changed results");
            }
        }

        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::bitVector(8));
            const StringId inputName = builder.addString("input");
            const VariableId input = builder.addVariable(type, builder.zeroInit());
            const VariableId replacement = builder.addVariable(type, builder.zeroInit());
            const std::array<VariableId, 1> results = {input};
            const std::array<VariableId, 1> operands = {replacement};
            builder.addInstruction(Opcode::Assign, results, operands);
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
            };
            facts.instructionEffects = {InstructionEffect::Pure};
            LinearProgramArtifact artifact{
                .program = builder.finish(),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            };
            const ValidationResult validation = validate(
                artifact,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "writes a ProgramInterface input"))
            {
                return fail("LinearProgram instructions must not write interface inputs");
            }
        }
        return 0;
    }

    int testInoutAcceptsEquivalentLogicalTypes()
    {
        LinearProgramBuilder builder;
        const TypeId inputType = builder.addType(Type::bitVector(8));
        const TypeId outputType = builder.addType(Type::bitVector(8));
        const TypeId enableType = builder.addType(Type::bitVector(1));
        const StringId portName = builder.addString("bus");
        const VariableId input = builder.addVariable(inputType, builder.zeroInit());
        const VariableId output = builder.addVariable(outputType, builder.zeroInit());
        const VariableId outputEnable = builder.addVariable(enableType, builder.zeroInit());
        const LinearProgram program = builder.finish();
        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = portName,
                .direction = PortDirection::Inout,
                .input = input,
                .output = output,
                .outputEnable = outputEnable,
            },
        };
        if (!validate(interface,
                      program.view(),
                      ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("inout must accept equal logical Types with distinct TypeIds");
        }
        return 0;
    }

    int testExecutableRequiresChangedAnyInputCoverage()
    {
        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const StringId inputName = linearBuilder.addString("input");
            const VariableId input =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId output =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const std::array<VariableId, 1> assignResults = {output};
            const std::array<VariableId, 1> assignOperands = {input};
            const InstructionId assign = scheduledBuilder.addInstruction(
                Opcode::Assign,
                assignResults,
                assignOperands);
            const std::array<InstructionId, 0> noInstructions = {};
            const std::array<InstructionId, 1> computeInstructions = {assign};
            scheduledBuilder.addBlock(noInstructions);
            scheduledBuilder.addBlock(computeInstructions);
            ExecutableModel model{
                .program = scheduledBuilder.finish(),
                .interface = ProgramInterface{
                    .ports = {
                        PortBinding{
                            .name = inputName,
                            .direction = PortDirection::Input,
                            .input = input,
                        },
                    },
                },
            };
            const ValidationResult validation = validate(
                model,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "does not activate from every ProgramInterface input"))
            {
                return fail("ExecutableModel must reject an input with no EntryBlock watch");
            }
        }

        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const StringId inputName = linearBuilder.addString("input");
            const VariableId input =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId oldValue =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const std::array<VariableId, 1> changedResults = {event};
            const std::array<VariableId, 2> changedOperands = {input, oldValue};
            const InstructionId changed = linearBuilder.addInstruction(
                Opcode::ChangedPos,
                changedResults,
                changedOperands);

            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const std::array<VariableId, 1> activationOperands = {event};
            const InstructionId activate = scheduledBuilder.addInstruction(
                Opcode::ActForward,
                {},
                activationOperands);
            const std::array<BlockId, 1> targets = {BlockId{1}};
            scheduledBuilder.setActivationTargets(activate, targets);
            const std::array<InstructionId, 2> entryInstructions = {changed, activate};
            const std::array<InstructionId, 0> noInstructions = {};
            scheduledBuilder.addBlock(entryInstructions);
            scheduledBuilder.addBlock(noInstructions);
            ExecutableModel model{
                .program = scheduledBuilder.finish(),
                .interface = ProgramInterface{
                    .ports = {
                        PortBinding{
                            .name = inputName,
                            .direction = PortDirection::Input,
                            .input = input,
                        },
                    },
                },
            };
            const ValidationResult validation = validate(
                model,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "does not activate from every ProgramInterface input"))
            {
                return fail("edge-only EntryBlock watches must not count as full input coverage");
            }
        }
        return 0;
    }

    int testExecutableInputCoverageTracksEntryBlockDataflow()
    {
        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const StringId firstName = linearBuilder.addString("first");
            const StringId secondName = linearBuilder.addString("second");
            const VariableId firstInput =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId secondInput =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId firstOld =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const VariableId secondOld =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const VariableId firstEvent =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId secondEvent =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId combinedEvent =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const std::array<VariableId, 1> firstResults = {firstEvent};
            const std::array<VariableId, 2> firstOperands = {firstInput, firstOld};
            const InstructionId firstChanged = linearBuilder.addInstruction(
                Opcode::ChangedAny,
                firstResults,
                firstOperands);
            const std::array<VariableId, 1> secondResults = {secondEvent};
            const std::array<VariableId, 2> secondOperands = {secondInput, secondOld};
            const InstructionId secondChanged = linearBuilder.addInstruction(
                Opcode::ChangedAny,
                secondResults,
                secondOperands);
            const std::array<VariableId, 1> combinedResults = {combinedEvent};
            const std::array<VariableId, 2> combinedOperands = {firstEvent, secondEvent};
            const InstructionId combine = linearBuilder.addInstruction(
                Opcode::LogicOr,
                combinedResults,
                combinedOperands);

            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const std::array<VariableId, 1> activationOperands = {combinedEvent};
            const InstructionId activate = scheduledBuilder.addInstruction(
                Opcode::ActForward,
                {},
                activationOperands);
            const std::array<BlockId, 1> targets = {BlockId{1}};
            scheduledBuilder.setActivationTargets(activate, targets);
            const std::array<InstructionId, 4> entryInstructions = {
                firstChanged,
                secondChanged,
                combine,
                activate,
            };
            const std::array<InstructionId, 0> noInstructions = {};
            scheduledBuilder.addBlock(entryInstructions);
            scheduledBuilder.addBlock(noInstructions);
            ExecutableModel model{
                .program = scheduledBuilder.finish(),
                .interface = ProgramInterface{
                    .ports = {
                        PortBinding{
                            .name = firstName,
                            .direction = PortDirection::Input,
                            .input = firstInput,
                        },
                        PortBinding{
                            .name = secondName,
                            .direction = PortDirection::Input,
                            .input = secondInput,
                        },
                    },
                },
            };
            if (!validate(model, ValidationOptions{.level = ValidationLevel::Semantic}).success())
            {
                return fail("an OR of direct B0 ChangedAny events must cover both inputs");
            }
        }

        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const StringId inputName = linearBuilder.addString("input");
            const VariableId input =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId oldValue =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId replacement =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const std::array<VariableId, 1> changedResults = {event};
            const std::array<VariableId, 2> changedOperands = {input, oldValue};
            const InstructionId changed = linearBuilder.addInstruction(
                Opcode::ChangedAny,
                changedResults,
                changedOperands);

            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const std::array<VariableId, 1> overwriteResults = {event};
            const std::array<VariableId, 1> overwriteOperands = {replacement};
            const InstructionId overwrite = scheduledBuilder.addInstruction(
                Opcode::Assign,
                overwriteResults,
                overwriteOperands);
            const std::array<VariableId, 1> activationOperands = {event};
            const InstructionId activate = scheduledBuilder.addInstruction(
                Opcode::ActForward,
                {},
                activationOperands);
            const std::array<BlockId, 1> targets = {BlockId{1}};
            scheduledBuilder.setActivationTargets(activate, targets);
            const std::array<InstructionId, 3> entryInstructions = {
                changed,
                overwrite,
                activate,
            };
            const std::array<InstructionId, 0> noInstructions = {};
            scheduledBuilder.addBlock(entryInstructions);
            scheduledBuilder.addBlock(noInstructions);
            ExecutableModel model{
                .program = scheduledBuilder.finish(),
                .interface = ProgramInterface{
                    .ports = {
                        PortBinding{
                            .name = inputName,
                            .direction = PortDirection::Input,
                            .input = input,
                        },
                    },
                },
            };
            const ValidationResult validation = validate(
                model,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "does not activate from every ProgramInterface input"))
            {
                return fail("a ChangedAny event overwritten before act must not cover its input");
            }
        }

        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const StringId inputName = linearBuilder.addString("input");
            const VariableId input =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId replacement =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId oldValue =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const std::array<VariableId, 1> overwriteResults = {input};
            const std::array<VariableId, 1> overwriteOperands = {replacement};
            const InstructionId overwrite = linearBuilder.addInstruction(
                Opcode::Assign,
                overwriteResults,
                overwriteOperands);
            const std::array<VariableId, 1> changedResults = {event};
            const std::array<VariableId, 2> changedOperands = {input, oldValue};
            const InstructionId changed = linearBuilder.addInstruction(
                Opcode::ChangedAny,
                changedResults,
                changedOperands);

            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const std::array<VariableId, 1> activationOperands = {event};
            const InstructionId activate = scheduledBuilder.addInstruction(
                Opcode::ActForward,
                {},
                activationOperands);
            const std::array<BlockId, 1> targets = {BlockId{1}};
            scheduledBuilder.setActivationTargets(activate, targets);
            const std::array<InstructionId, 3> entryInstructions = {
                overwrite,
                changed,
                activate,
            };
            const std::array<InstructionId, 0> noInstructions = {};
            scheduledBuilder.addBlock(entryInstructions);
            scheduledBuilder.addBlock(noInstructions);
            ExecutableModel model{
                .program = scheduledBuilder.finish(),
                .interface = ProgramInterface{
                    .ports = {
                        PortBinding{
                            .name = inputName,
                            .direction = PortDirection::Input,
                            .input = input,
                        },
                    },
                },
            };
            const ValidationResult validation = validate(
                model,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "does not activate from every ProgramInterface input") ||
                !containsError(validation, "writes a ProgramInterface input"))
            {
                return fail("a ChangedAny after an EntryBlock input overwrite must not prove coverage");
            }
        }

        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const StringId inputName = linearBuilder.addString("input");
            const VariableId input =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId replacement =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId oldValue =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const std::array<VariableId, 1> changedResults = {event};
            const std::array<VariableId, 2> changedOperands = {input, oldValue};
            const InstructionId changed = linearBuilder.addInstruction(
                Opcode::ChangedAny,
                changedResults,
                changedOperands);
            const std::array<VariableId, 1> overwriteResults = {input};
            const std::array<VariableId, 1> overwriteOperands = {replacement};
            const InstructionId overwrite = linearBuilder.addInstruction(
                Opcode::Assign,
                overwriteResults,
                overwriteOperands);

            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const std::array<VariableId, 1> activationOperands = {event};
            const InstructionId activate = scheduledBuilder.addInstruction(
                Opcode::ActForward,
                {},
                activationOperands);
            const std::array<BlockId, 1> targets = {BlockId{1}};
            scheduledBuilder.setActivationTargets(activate, targets);
            const std::array<InstructionId, 2> entryInstructions = {changed, activate};
            const std::array<InstructionId, 1> bodyInstructions = {overwrite};
            scheduledBuilder.addBlock(entryInstructions);
            scheduledBuilder.addBlock(bodyInstructions);
            ExecutableModel model{
                .program = scheduledBuilder.finish(),
                .interface = ProgramInterface{
                    .ports = {
                        PortBinding{
                            .name = inputName,
                            .direction = PortDirection::Input,
                            .input = input,
                        },
                    },
                },
            };
            const ValidationResult validation = validate(
                model,
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation, "writes a ProgramInterface input"))
            {
                return fail("normal Blocks must not overwrite interface inputs");
            }
        }
        return 0;
    }

    int testFailureShortCircuiting()
    {
        const CaseOutcome lowerFailure = runCase(
            LowerBehavior::ReturnFailure,
            EmitBehavior::Success);
        if (lowerFailure.success || lowerFailure.hasModel ||
            !lowerFailure.diagnosticsHaveError ||
            lowerFailure.state.calls != std::vector<std::string>{"lower"})
        {
            return fail("lowering failure must short-circuit scheduler and emitter");
        }

        // The schedule stage is GrhIRToGrhSimAMProgram::graphToProgram itself, so a
        // schedule-stage failure is injected through invalid scheduling
        // limits: schedule() rejects them before the emitter can run.
        {
            MockState state;
            MockLowering lowering(state, LowerBehavior::Success);
            MockEmitter emitter(state, EmitBehavior::Success);
            GrhIRToGrhSimAMProgram pipeline(lowering, emitter);
            wolvrix::lib::grh::Design design;
            wolvrix::lib::grh::Graph &graph = design.createGraph("pipeline_top");
            wolvrix::lib::diag::Diagnostics diagnostics;
            const ActivityScheduleOptions scheduleOptions{
                .maxAtomsPerBlock = 0,
            };
            const GrhIRToGrhSimAMProgramResult result = pipeline.run(
                graph,
                scheduleOptions,
                GrhSimAmCppOptions{},
                diagnostics);
            if (result.success || result.model || !diagnostics.hasError() ||
                state.calls != std::vector<std::string>{"lower"})
            {
                return fail("scheduler failure must short-circuit the emitter");
            }
        }

        const CaseOutcome emitFailure = runCase(
            LowerBehavior::Success,
            EmitBehavior::ReturnFailure);
        if (emitFailure.success || !emitFailure.hasModel ||
            !emitFailure.diagnosticsHaveError ||
            emitFailure.state.calls != std::vector<std::string>{"lower", "emit"} ||
            emitFailure.artifacts != std::vector<std::string>{"partial.cpp"})
        {
            return fail("emitter failure must preserve the scheduled model and partial artifact list");
        }
        return 0;
    }

    int testProductionWatchesOtherwiseUnusedInputs()
    {
        LinearProgramArtifact linear = makeLinearArtifact();
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            GrhIRToGrhSimAMProgram::graphToProgram(AmGraph::fromLinearProgram(linear),
                                           ActivityScheduleOptions{},
                                           diagnostics);
        if (!model || diagnostics.hasError() || model->program.blockCount() != 2 ||
            model->program.blockSize(BlockId{0}) != 2 ||
            model->program.blockSize(BlockId{1}) != 0)
        {
            return fail("production scheduler must materialize a watch Block for an otherwise unused input");
        }
        const InstructionId changed = model->program.blockInstruction(BlockId{0}, 0);
        const InstructionId activate = model->program.blockInstruction(BlockId{0}, 1);
        if (model->program.view().opcode(changed) != Opcode::ChangedAny ||
            model->program.view().opcode(activate) != Opcode::ActForward ||
            !validate(*model, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("otherwise unused inputs must satisfy ExecutableModel B0 coverage");
        }
        return 0;
    }

    int testOpcodeTraitsExposeMemoryAccessAndOrdering()
    {
        const OpcodeTraits readTraits = opcodeTraits(Opcode::MemoryRead);
        const OpcodeTraits writeTraits = opcodeTraits(Opcode::MemoryWrite);
        const OpcodeTraits registerTraits = opcodeTraits(Opcode::RegisterWrite);
        const OpcodeTraits fillTraits = opcodeTraits(Opcode::MemoryFill);
        const OpcodeTraits lanesTraits = opcodeTraits(Opcode::MemoryWriteLanes);
        // Write operand layout is [cond?, addr?(mem), mask?, data, target,
        // events...]: the state target is the last fixed operand -- 1 for
        // reg/fill, 2 for mem.write and mem.write_lanes, 4 for mem.write.cm
        // ([cond, addr, mask, data, target, events...]).
        const OpcodeTraits writeCmTraits = opcodeTraits(Opcode::MemoryWriteCondMask);
        if (!readTraits.memoryAccess || readTraits.effect != OpcodeEffect::StateRead ||
            readTraits.stateTargetOperand != 0 || !writeTraits.memoryAccess ||
            writeTraits.effect != OpcodeEffect::StateReadWrite ||
            writeTraits.stateTargetOperand != 2 || !writeTraits.hasOrderedEffect ||
            !writeCmTraits.memoryAccess || writeCmTraits.stateTargetOperand != 4 ||
            registerTraits.stateTargetOperand != 1 ||
            fillTraits.stateTargetOperand != 1 || lanesTraits.stateTargetOperand != 2)
        {
            return fail("opcode traits must expose memory access and ordering semantics");
        }
        // The layout helper derives the same fixed-operand positions for all
        // 12 reg/latch/mem write variants.
        const StateWriteLayout regCm = stateWriteLayout(Opcode::RegisterWriteCondMask);
        const StateWriteLayout latchC = stateWriteLayout(Opcode::LatchWriteCond);
        const StateWriteLayout memM = stateWriteLayout(Opcode::MemoryWriteMask);
        if (!regCm.isStateWrite || regCm.memory || !regCm.hasCond || !regCm.hasMask ||
            regCm.fixedCount != 4 || regCm.dataIndex != 2 || regCm.targetIndex != 3 ||
            !latchC.isStateWrite || latchC.memory || !latchC.hasCond || latchC.hasMask ||
            latchC.fixedCount != 3 || latchC.dataIndex != 1 || latchC.targetIndex != 2 ||
            !memM.isStateWrite || !memM.memory || memM.hasCond || !memM.hasMask ||
            memM.fixedCount != 4 || memM.dataIndex != 2 || memM.targetIndex != 3 ||
            !isStateWriteOpcode(Opcode::MemoryFill) ||
            !isStateWriteOpcode(Opcode::MemoryWriteLanes) ||
            isStateWriteOpcode(Opcode::MemoryRead))
        {
            return fail("state-write layout helper disagrees with the operand layouts");
        }
        return 0;
    }

    enum class CommitStructureViolation
    {
        None,
        NonContiguousCommitRange,
        StateWriteOutsideCommit,
        MissingGateDetector,
        ActForwardTargetsEarlierBlock,
        ActBackwardTargetsEntry,
        ChangedResultFlowsBackward,
    };

    // B0 watches a source event, B1 reads the state, B2 commits the state
    // write. A commit Block is [head gate detector] + [state writes] +
    // [tail watch + act]: the head changed.* clone gates the whole Block and
    // the write's event operand points at the clone's result. act.f/act.b
    // may live inside commit Blocks and may target other commit Blocks; only
    // the forward/entry target rules remain. Each violation breaks exactly
    // one structural rule.
    ExecutableModel makeCommitStructureModel(CommitStructureViolation violation)
    {
        LinearProgramBuilder linear;
        const TypeId type = linear.addType(Type::bitVector(1));
        const VariableId source = linear.addVariable(type, linear.zeroInit());
        const VariableId oldValue = linear.addVariable(type, linear.undefInit());
        const VariableId event = linear.addVariable(type, linear.zeroInit());
        const VariableId state = linear.addVariable(type, linear.zeroInit());
        const VariableId stateOld = linear.addVariable(type, linear.undefInit());
        const VariableId stateEvent = linear.addVariable(type, linear.zeroInit());
        const VariableId gateOld = linear.addVariable(type, linear.undefInit());
        const VariableId gateEvent = linear.addVariable(type, linear.zeroInit());
        const VariableId reader = linear.addVariable(type, linear.zeroInit());
        const std::array<VariableId, 1> changedResults = {event};
        const std::array<VariableId, 2> changedOperands = {source, oldValue};
        const InstructionId inputChanged =
            linear.addInstruction(Opcode::ChangedAny, changedResults, changedOperands);
        const bool missingGate =
            violation == CommitStructureViolation::MissingGateDetector;
        const bool writeOutside =
            violation == CommitStructureViolation::StateWriteOutsideCommit;
        InstructionId gateChanged = InstructionId::invalid();
        if (!missingGate)
        {
            const std::array<VariableId, 1> gateResults = {gateEvent};
            const std::array<VariableId, 2> gateOperands = {source, gateOld};
            gateChanged =
                linear.addInstruction(Opcode::ChangedAny, gateResults, gateOperands);
        }
        // reg.write [nextValue, target, events...]: the event operand is
        // re-pointed at the Block-local gate detector result.
        InstructionId write = InstructionId::invalid();
        if (!missingGate && !writeOutside)
        {
            const std::array<VariableId, 3> writeOperands = {source, state, gateEvent};
            write = linear.addInstruction(Opcode::RegisterWrite, {}, writeOperands);
        }
        else
        {
            const std::array<VariableId, 2> writeOperands = {source, state};
            write = linear.addInstruction(Opcode::RegisterWrite, {}, writeOperands);
        }
        const std::array<VariableId, 1> stateChangedResults = {stateEvent};
        const std::array<VariableId, 2> stateChangedOperands = {state, stateOld};
        const InstructionId stateChanged = linear.addInstruction(
            Opcode::ChangedAny, stateChangedResults, stateChangedOperands);
        const bool backwardFlow =
            violation == CommitStructureViolation::ChangedResultFlowsBackward;
        const std::array<VariableId, 1> readerResults = {reader};
        const std::array<VariableId, 1> readerOperands = {backwardFlow ? stateEvent : state};
        const InstructionId read =
            linear.addInstruction(Opcode::Assign, readerResults, readerOperands);

        ScheduledProgramBuilder scheduled(linear.finish());
        InstructionId reactivate = InstructionId::invalid();
        if (violation != CommitStructureViolation::ActForwardTargetsEarlierBlock)
        {
            const std::array<VariableId, 1> stateEventOperands = {stateEvent};
            reactivate =
                scheduled.addInstruction(Opcode::ActBackward, {}, stateEventOperands);
            const std::array<BlockId, 1> reactivateTargets = {
                violation == CommitStructureViolation::ActBackwardTargetsEntry ? BlockId{0}
                                                                             : BlockId{1},
            };
            scheduled.setActivationTargets(reactivate, reactivateTargets);
        }
        InstructionId forward = InstructionId::invalid();
        if (violation == CommitStructureViolation::ActForwardTargetsEarlierBlock)
        {
            const std::array<VariableId, 1> forwardOperands = {stateEvent};
            forward = scheduled.addInstruction(Opcode::ActForward, {}, forwardOperands);
            const std::array<BlockId, 1> forwardTargets = {BlockId{1}};
            scheduled.setActivationTargets(forward, forwardTargets);
        }

        const std::array<InstructionId, 1> entry = {inputChanged};
        scheduled.addBlock(entry);
        if (writeOutside)
        {
            const std::array<InstructionId, 2> compute = {write, read};
            scheduled.addBlock(compute);
        }
        else
        {
            const std::array<InstructionId, 1> compute = {read};
            scheduled.addBlock(compute);
        }
        if (writeOutside)
        {
            const std::array<InstructionId, 3> commit = {
                gateChanged,
                stateChanged,
                reactivate,
            };
            scheduled.addBlock(commit);
        }
        else if (missingGate)
        {
            const std::array<InstructionId, 3> commit = {write, stateChanged, reactivate};
            scheduled.addBlock(commit);
        }
        else if (violation == CommitStructureViolation::ActForwardTargetsEarlierBlock)
        {
            const std::array<InstructionId, 4> commit = {
                gateChanged,
                write,
                stateChanged,
                forward,
            };
            scheduled.addBlock(commit);
        }
        else
        {
            const std::array<InstructionId, 4> commit = {
                gateChanged,
                write,
                stateChanged,
                reactivate,
            };
            scheduled.addBlock(commit);
        }

        const bool badRange =
            violation == CommitStructureViolation::NonContiguousCommitRange;
        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = {},
            .commitBlockBegin = badRange ? 1U : 2U,
            .commitBlockEnd = badRange ? 2U : 3U,
        };
    }

    // Two commit Blocks chained through their tail watches: B2 ends with an
    // act.f into the later commit Block B3 (same round), B3 ends with an
    // act.b back into B2 (next round). Both directions are legal now.
    ExecutableModel makeCommitChainModel()
    {
        LinearProgramBuilder linear;
        const TypeId type = linear.addType(Type::bitVector(1));
        const VariableId source = linear.addVariable(type, linear.zeroInit());
        const VariableId sourceOld = linear.addVariable(type, linear.undefInit());
        const VariableId sourceEvent = linear.addVariable(type, linear.zeroInit());
        const VariableId stateA = linear.addVariable(type, linear.zeroInit());
        const VariableId gateOldA = linear.addVariable(type, linear.undefInit());
        const VariableId gateEventA = linear.addVariable(type, linear.zeroInit());
        const VariableId stateOldA = linear.addVariable(type, linear.undefInit());
        const VariableId stateEventA = linear.addVariable(type, linear.zeroInit());
        const VariableId stateB = linear.addVariable(type, linear.zeroInit());
        const VariableId gateOldB = linear.addVariable(type, linear.undefInit());
        const VariableId gateEventB = linear.addVariable(type, linear.zeroInit());
        const VariableId stateOldB = linear.addVariable(type, linear.undefInit());
        const VariableId stateEventB = linear.addVariable(type, linear.zeroInit());
        const VariableId reader = linear.addVariable(type, linear.zeroInit());
        const std::array<VariableId, 1> inputResults = {sourceEvent};
        const std::array<VariableId, 2> inputOperands = {source, sourceOld};
        const InstructionId inputChanged =
            linear.addInstruction(Opcode::ChangedAny, inputResults, inputOperands);
        const std::array<VariableId, 1> gateResultsA = {gateEventA};
        const std::array<VariableId, 2> gateOperandsA = {source, gateOldA};
        const InstructionId gateA =
            linear.addInstruction(Opcode::ChangedAny, gateResultsA, gateOperandsA);
        const std::array<VariableId, 3> writeOperandsA = {source, stateA, gateEventA};
        const InstructionId writeA =
            linear.addInstruction(Opcode::RegisterWrite, {}, writeOperandsA);
        const std::array<VariableId, 1> tailResultsA = {stateEventA};
        const std::array<VariableId, 2> tailOperandsA = {stateA, stateOldA};
        const InstructionId tailA =
            linear.addInstruction(Opcode::ChangedAny, tailResultsA, tailOperandsA);
        const std::array<VariableId, 1> gateResultsB = {gateEventB};
        const std::array<VariableId, 2> gateOperandsB = {source, gateOldB};
        const InstructionId gateB =
            linear.addInstruction(Opcode::ChangedAny, gateResultsB, gateOperandsB);
        const std::array<VariableId, 3> writeOperandsB = {source, stateB, gateEventB};
        const InstructionId writeB =
            linear.addInstruction(Opcode::RegisterWrite, {}, writeOperandsB);
        const std::array<VariableId, 1> tailResultsB = {stateEventB};
        const std::array<VariableId, 2> tailOperandsB = {stateB, stateOldB};
        const InstructionId tailB =
            linear.addInstruction(Opcode::ChangedAny, tailResultsB, tailOperandsB);
        const std::array<VariableId, 1> readerResults = {reader};
        const std::array<VariableId, 1> readerOperands = {stateA};
        const InstructionId read =
            linear.addInstruction(Opcode::Assign, readerResults, readerOperands);

        ScheduledProgramBuilder scheduled(linear.finish());
        const std::array<VariableId, 1> forwardOperands = {stateEventA};
        const InstructionId forward =
            scheduled.addInstruction(Opcode::ActForward, {}, forwardOperands);
        const std::array<BlockId, 1> forwardTargets = {BlockId{3}};
        scheduled.setActivationTargets(forward, forwardTargets);
        const std::array<VariableId, 1> backwardOperands = {stateEventB};
        const InstructionId backward =
            scheduled.addInstruction(Opcode::ActBackward, {}, backwardOperands);
        const std::array<BlockId, 1> backwardTargets = {BlockId{2}};
        scheduled.setActivationTargets(backward, backwardTargets);

        const std::array<InstructionId, 1> entry = {inputChanged};
        scheduled.addBlock(entry);
        const std::array<InstructionId, 1> compute = {read};
        scheduled.addBlock(compute);
        const std::array<InstructionId, 4> commitA = {gateA, writeA, tailA, forward};
        scheduled.addBlock(commitA);
        const std::array<InstructionId, 4> commitB = {gateB, writeB, tailB, backward};
        scheduled.addBlock(commitB);
        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = {},
            .commitBlockBegin = 2,
            .commitBlockEnd = 4,
        };
    }

    int testExecutableCommitStructureValidation()
    {
        if (!validate(makeCommitStructureModel(CommitStructureViolation::None),
                      ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("well-formed commit Block structure was rejected");
        }
        if (!validate(makeCommitChainModel(),
                      ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("commit-to-commit act.f/act.b chains must be accepted");
        }
        {
            const ValidationResult validation = validate(
                makeCommitStructureModel(CommitStructureViolation::NonContiguousCommitRange),
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation,
                               "commit Blocks must form a contiguous suffix"))
            {
                return fail("commit Blocks outside one contiguous suffix must be rejected");
            }
        }
        {
            const ValidationResult validation = validate(
                makeCommitStructureModel(CommitStructureViolation::StateWriteOutsideCommit),
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation,
                               "state write instruction outside a commit Block"))
            {
                return fail("state writes outside commit Blocks must be rejected");
            }
        }
        {
            const ValidationResult validation = validate(
                makeCommitStructureModel(CommitStructureViolation::MissingGateDetector),
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation,
                               "commit Block state write is not preceded by a gate detector"))
            {
                return fail("commit state writes without a head gate detector must be rejected");
            }
        }
        {
            const ValidationResult validation = validate(
                makeCommitStructureModel(CommitStructureViolation::ActForwardTargetsEarlierBlock),
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation,
                               "ActForward target is not a later Block"))
            {
                return fail("act.f that does not target a later Block must be rejected");
            }
        }
        {
            const ValidationResult validation = validate(
                makeCommitStructureModel(CommitStructureViolation::ActBackwardTargetsEntry),
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation,
                               "ActBackward target is not a non-entry Block"))
            {
                return fail("act.b targeting the entry Block must be rejected");
            }
        }
        {
            const ValidationResult validation = validate(
                makeCommitStructureModel(CommitStructureViolation::ChangedResultFlowsBackward),
                ValidationOptions{.level = ValidationLevel::Semantic});
            if (validation.success() ||
                !containsError(validation,
                               "Changed result is consumed by an earlier Block"))
            {
                return fail("cross-Block changed results must flow strictly forward");
            }
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testStrictSuccessOrder(); result != 0)
    {
        return result;
    }
    if (const int result = testStagedPipelineDoesNotRetainGraph(); result != 0)
    {
        return result;
    }
    if (const int result = testExistingDiagnosticsDoNotConsumeArtifact(); result != 0)
    {
        return result;
    }
    if (const int result = testLoweringInterfaceGate(); result != 0)
    {
        return result;
    }
    if (const int result = testSchedulingFactsGate(); result != 0)
    {
        return result;
    }
    if (const int result = testExternalInputRoleMustMatchInterface(); result != 0)
    {
        return result;
    }
    if (const int result = testArtifactRolesAndInterfaceExposureContracts(); result != 0)
    {
        return result;
    }
    if (const int result = testInoutAcceptsEquivalentLogicalTypes(); result != 0)
    {
        return result;
    }
    if (const int result = testExecutableRequiresChangedAnyInputCoverage(); result != 0)
    {
        return result;
    }
    if (const int result = testExecutableInputCoverageTracksEntryBlockDataflow(); result != 0)
    {
        return result;
    }
    if (const int result = testFailureShortCircuiting(); result != 0)
    {
        return result;
    }
    if (const int result = testProductionWatchesOtherwiseUnusedInputs(); result != 0)
    {
        return result;
    }
    if (const int result = testOpcodeTraitsExposeMemoryAccessAndOrdering(); result != 0)
    {
        return result;
    }
    if (const int result = testExecutableCommitStructureValidation(); result != 0)
    {
        return result;
    }
    return 0;
}
