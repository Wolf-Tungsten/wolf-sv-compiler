#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_program_validate.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace wolvrix::lib::grhsim::am;

static_assert(!std::is_copy_constructible_v<LinearProgram>);
static_assert(!std::is_copy_assignable_v<LinearProgram>);
static_assert(std::is_nothrow_move_constructible_v<LinearProgram>);
static_assert(std::is_nothrow_move_assignable_v<LinearProgram>);
static_assert(!std::is_copy_constructible_v<ScheduledProgram>);
static_assert(!std::is_copy_assignable_v<ScheduledProgram>);
static_assert(std::is_nothrow_move_constructible_v<ScheduledProgram>);
static_assert(std::is_nothrow_move_assignable_v<ScheduledProgram>);

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim_am_program] " << message << '\n';
        return 1;
    }

    template <typename Builder>
    InstructionId addInstruction(Builder &builder,
                                 Opcode opcode,
                                 std::initializer_list<VariableId> results,
                                 std::initializer_list<VariableId> operands)
    {
        return builder.addInstruction(
            opcode,
            std::span<const VariableId>(results.begin(), results.size()),
            std::span<const VariableId>(operands.begin(), operands.size()));
    }

    void addBlock(ScheduledProgramBuilder &builder,
                  std::initializer_list<InstructionId> instructions)
    {
        builder.addBlock(std::span<const InstructionId>(instructions.begin(), instructions.size()));
    }

    void setActivationTargets(ScheduledProgramBuilder &builder,
                              InstructionId instruction,
                              std::initializer_list<BlockId> targets)
    {
        builder.setActivationTargets(
            instruction,
            std::span<const BlockId>(targets.begin(), targets.size()));
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

    template <typename Program>
    bool rejects(const Program &program,
                 ValidationLevel level,
                 std::string_view errorFragment)
    {
        const ValidationResult result = validate(program, ValidationOptions{.level = level});
        return !result.success() && containsError(result, errorFragment);
    }

    bool hasCompleteArenaTelemetry(const ProgramStorageStats &stats)
    {
        constexpr std::array<uint64_t, static_cast<std::size_t>(ProgramArena::Count)>
            expectedElementBytes = {
                12, 4, 1, 12, 48, 20, 8, 1, 8, 8, 1, 4, 4,
                4, 4, 8, 12, 16, 16, 12, 4, 20, 12, 4, 4,
            };
        uint64_t sizeBytes = 0;
        uint64_t capacityBytes = 0;
        for (std::size_t index = 0; index < expectedElementBytes.size(); ++index)
        {
            const ArenaStorageStats &arena = stats.arenas[index];
            if (arena.elementBytes != expectedElementBytes[index] ||
                arena.capacity < arena.elements)
            {
                return false;
            }
            sizeBytes += arena.sizeBytes();
            capacityBytes += arena.capacityBytes();
        }
        return sizeBytes == stats.estimatedBytes && capacityBytes == stats.reservedBytes;
    }

    int testLinearProgramDataAndValidation()
    {
        LinearProgramBuilder builder;
        builder.reserve(ProgramReserve{
            .types = 6,
            .strings = 7,
            .stringBytes = 64,
            .initDescriptors = 4,
            .initActions = 1,
            .literals = 2,
            .literalWords = 2,
            .literalBytes = 3,
            .variables = 9,
            .variableLabels = 2,
            .instructions = 5,
            .operands = 7,
            .results = 5,
            .sliceStaticAttributes = 1,
            .systemFunctionAttributes = 1,
            .systemTaskAttributes = 1,
            .dpiCallAttributes = 1,
            .dpiImports = 1,
            .dpiParameters = 2,
        });

        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const TypeId wideType = builder.addType(Type::bitVector(65, Signedness::Signed));
        const TypeId realType = builder.addType(Type::real());
        const TypeId stringType = builder.addType(Type::string());
        const TypeId arrayType = builder.addType(Type::array(16, 32));
        const TypeId eventType = builder.addType(Type::bitVector(1));
        if (u8Type.value != 0 || wideType.value != 1 || realType.value != 2 ||
            stringType.value != 3 || arrayType.value != 4 || eventType.value != 5)
        {
            return fail("TypeId values must be dense and zero-based");
        }

        const StringId signalLabel = builder.addString("top.signal");
        const StringId functionName = builder.addString("clog2");
        const StringId taskName = builder.addString("display");
        const StringId dpiSymbol = builder.addString("host_call");
        const StringId inputName = builder.addString("input_value");
        const StringId outputName = builder.addString("output_value");
        const StringId actionLabel = builder.addString("top.action_value");
        if (signalLabel.value != 0 || functionName.value != 1 || taskName.value != 2 ||
            dpiSymbol.value != 3 || inputName.value != 4 || outputName.value != 5 ||
            actionLabel.value != 6)
        {
            return fail("StringId values must be dense and zero-based");
        }

        const std::array<uint64_t, 2> wideWords = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(1),
        };
        const LiteralId wideLiteral = builder.addBitLiteral(wideType, wideWords);
        constexpr char embeddedNulBytes[] = {'A', '\0', 'B'};
        const LiteralId stringLiteral = builder.addStringLiteral(
            stringType,
            std::string_view(embeddedNulBytes, sizeof(embeddedNulBytes)));
        if (wideLiteral.value != 0 || stringLiteral.value != 1)
        {
            return fail("LiteralId values must be dense and zero-based");
        }

        const InitId constantInit = builder.addConstantInit(wideLiteral);
        const std::array<InitAction, 1> initActions = {
            InitAction{
                .kind = InitActionKind::Set,
                .rangeKind = InitRangeKind::All,
                .expression = InitExpr{
                    .kind = InitExprKind::Literal,
                    .literal = wideLiteral,
                },
                .start = 0,
                .count = 0,
                .path = StringId::invalid(),
            },
        };
        const InitId actionsInit = builder.addActionsInit(initActions);
        if (builder.undefInit().value != 0 || builder.zeroInit().value != 1 ||
            constantInit.value != 2 || actionsInit.value != 3)
        {
            return fail("InitId values must include dense built-in and appended records");
        }

        const VariableId constant = builder.addVariable(wideType, constantInit, signalLabel);
        const VariableId source = builder.addVariable(wideType, builder.zeroInit());
        const VariableId assigned = builder.addVariable(wideType, builder.undefInit());
        const VariableId sliced = builder.addVariable(u8Type, builder.undefInit());
        const VariableId functionResult = builder.addVariable(wideType, builder.undefInit());
        const VariableId event = builder.addVariable(eventType, builder.zeroInit());
        const VariableId dpiReturn = builder.addVariable(stringType, builder.undefInit());
        const VariableId dpiOutput = builder.addVariable(realType, builder.undefInit());
        const VariableId actionValue = builder.addVariable(wideType, actionsInit, actionLabel);
        const std::array<VariableId, 9> variables = {
            constant,
            source,
            assigned,
            sliced,
            functionResult,
            event,
            dpiReturn,
            dpiOutput,
            actionValue,
        };
        for (uint32_t index = 0; index < variables.size(); ++index)
        {
            if (variables[index].value != index)
            {
                return fail("VariableId values must be dense and zero-based");
            }
        }

        const InstructionId assign =
            addInstruction(builder, Opcode::Assign, {assigned}, {source});
        const InstructionId slice =
            addInstruction(builder, Opcode::SliceStatic, {sliced}, {assigned});
        builder.setSliceStaticAttributes(slice, 7);
        const InstructionId systemFunction =
            addInstruction(builder, Opcode::SystemFunction, {functionResult}, {source});
        builder.setSystemFunctionAttributes(
            systemFunction,
            SystemFunctionAttributes{
                .name = functionName,
                .schedule = CallSchedule::Once,
                .hasSideEffects = false,
            });
        const InstructionId systemTask =
            addInstruction(builder, Opcode::SystemTask, {}, {event, source});
        builder.setSystemTaskAttributes(
            systemTask,
            SystemTaskAttributes{
                .name = taskName,
                .eventCount = 0,
                .schedule = CallSchedule::Final,
                .eventMode = HostEventMode::Immediate,
            });
        const InstructionId dpiCall =
            addInstruction(builder, Opcode::DpiCall, {dpiReturn, dpiOutput}, {event, source});
        builder.setDpiCallAttributes(
            dpiCall,
            DpiCallAttributes{
                .importSymbol = dpiSymbol,
                .eventCount = 0,
                .eventMode = HostEventMode::Pending,
            });
        const std::array<InstructionId, 5> instructions = {
            assign,
            slice,
            systemFunction,
            systemTask,
            dpiCall,
        };
        for (uint32_t index = 0; index < instructions.size(); ++index)
        {
            if (instructions[index].value != index)
            {
                return fail("InstructionId values must be dense and zero-based");
            }
        }

        const std::array<DpiParameter, 2> parameters = {
            DpiParameter{
                .name = inputName,
                .type = wideType,
                .direction = DpiDirection::Input,
                .abi = DpiAbiKind::Integral,
            },
            DpiParameter{
                .name = outputName,
                .type = realType,
                .direction = DpiDirection::Output,
                .abi = DpiAbiKind::Real32,
            },
        };
        const DpiImportId import = builder.addDpiImport(
            dpiSymbol,
            parameters,
            DpiReturn{
                .type = stringType,
                .abi = DpiAbiKind::String,
                .present = true,
            });
        if (import.value != 0)
        {
            return fail("DpiImportId values must be dense and zero-based");
        }

        LinearProgram program = builder.finish();
        if (builder.view().valid())
        {
            return fail("finished builder must no longer expose a ProgramView");
        }
        if (!program.valid())
        {
            return fail("finished LinearProgram must be valid");
        }

        const ProgramView view = program.view();
        if (view.typeCount() != 6 || view.stringCount() != 7 || view.initCount() != 4 ||
            view.literalCount() != 2 || view.variableCount() != 9 ||
            view.instructionCount() != 5 || view.dpiImportCount() != 1)
        {
            return fail("LinearProgram table counts do not match the appended records");
        }
        if (view.type(wideType) != Type::bitVector(65, Signedness::Signed) ||
            view.type(realType) != Type::real() || view.type(stringType) != Type::string() ||
            view.type(arrayType) != Type::array(16, 32))
        {
            return fail("type records did not round-trip through ProgramView");
        }
        if (view.string(signalLabel) != "top.signal" || view.string(dpiSymbol) != "host_call")
        {
            return fail("string records did not round-trip through ProgramView");
        }
        const LiteralView readWideLiteral = view.literal(wideLiteral);
        const LiteralView readStringLiteral = view.literal(stringLiteral);
        if (readWideLiteral.type != wideType || readWideLiteral.words.size() != 2 ||
            readWideLiteral.words[0] != wideWords[0] || readWideLiteral.words[1] != wideWords[1] ||
            readStringLiteral.type != stringType ||
            readStringLiteral.bytes != std::string_view(embeddedNulBytes, sizeof(embeddedNulBytes)))
        {
            return fail("literal payloads did not round-trip through ProgramView");
        }
        if (view.init(constantInit).kind != InitKind::Constant ||
            view.init(constantInit).payload != wideLiteral.value ||
            view.init(actionsInit).kind != InitKind::Actions ||
            view.initActions(actionsInit).size() != 1 ||
            view.initActions(actionsInit)[0].expression.literal != wideLiteral)
        {
            return fail("init descriptors and action arena did not round-trip");
        }
        if (view.variable(constant).type != wideType ||
            view.variable(constant).init != constantInit ||
            view.variable(actionValue).init != actionsInit ||
            !view.variableLabel(constant) || *view.variableLabel(constant) != signalLabel ||
            !view.variableLabel(actionValue) || *view.variableLabel(actionValue) != actionLabel ||
            view.variableLabel(source).has_value())
        {
            return fail("variable type/init or sparse label metadata did not round-trip");
        }

        if (view.results(assign).size() != 1 || view.results(assign)[0] != assigned ||
            view.operands(assign).size() != 1 || view.operands(assign)[0] != source ||
            view.results(dpiCall).size() != 2 || view.results(dpiCall)[0] != dpiReturn ||
            view.results(dpiCall)[1] != dpiOutput ||
            view.operands(dpiCall).size() != 2 || view.operands(dpiCall)[0] != event ||
            view.operands(dpiCall)[1] != source)
        {
            return fail("instruction result/operand CSR spans did not round-trip");
        }
        const auto sliceAttributes = view.sliceStaticAttributes(slice);
        const auto functionAttributes = view.systemFunctionAttributes(systemFunction);
        const auto taskAttributes = view.systemTaskAttributes(systemTask);
        const auto callAttributes = view.dpiCallAttributes(dpiCall);
        if (!sliceAttributes || sliceAttributes->lsb != 7 || !functionAttributes ||
            functionAttributes->name != functionName ||
            functionAttributes->schedule != CallSchedule::Once || !taskAttributes ||
            taskAttributes->name != taskName || taskAttributes->eventCount != 0 ||
            taskAttributes->schedule != CallSchedule::Final ||
            taskAttributes->eventMode != HostEventMode::Immediate || !callAttributes ||
            callAttributes->importSymbol != dpiSymbol || callAttributes->eventCount != 0 ||
            callAttributes->eventMode != HostEventMode::Pending)
        {
            return fail("typed instruction attributes did not round-trip");
        }
        if (view.systemTaskAttributes(assign).has_value() ||
            view.activationAttributes(dpiCall).has_value())
        {
            return fail("typed attribute lookup must remain sparse and opcode-specific");
        }

        const DpiImportView importView = view.dpiImport(import);
        if (importView.symbol != dpiSymbol || importView.parameters.size() != 2 ||
            importView.parameters[0].name != inputName ||
            importView.parameters[0].direction != DpiDirection::Input ||
            importView.parameters[1].type != realType ||
            importView.parameters[1].abi != DpiAbiKind::Real32 ||
            !importView.returnValue.present || importView.returnValue.type != stringType ||
            importView.returnValue.abi != DpiAbiKind::String)
        {
            return fail("DPI import signature did not round-trip");
        }
        const ProgramStorageStats stats = view.storageStats();
        const uint64_t expectedInstructionBytes =
            5 * sizeof(Opcode) + 2 * 6 * sizeof(uint32_t) +
            7 * sizeof(VariableId) + 5 * sizeof(VariableId);
        if (stats.types != 6 || stats.strings != 7 || stats.variables != 9 ||
            stats.instructions != 5 || stats.operands != 7 || stats.results != 5 ||
            stats.blocks != 0 || stats.blockInstructionIds != 0 || stats.estimatedBytes == 0 ||
            stats.reservedBytes < stats.estimatedBytes ||
            stats.instructionBytes != expectedInstructionBytes ||
            stats.arena(ProgramArena::Opcodes).elementBytes != sizeof(uint8_t) ||
            stats.arena(ProgramArena::OperandOffsets).elementBytes != sizeof(uint32_t) ||
            stats.arena(ProgramArena::Operands).elementBytes != sizeof(VariableId) ||
            stats.arena(ProgramArena::ResultOffsets).elementBytes != sizeof(uint32_t) ||
            stats.arena(ProgramArena::Results).elementBytes != sizeof(VariableId) ||
            stats.arena(ProgramArena::Variables).elementBytes != sizeof(VariableRecord) ||
            stats.arena(ProgramArena::BlockOffsets).elementBytes != sizeof(uint32_t) ||
            !hasCompleteArenaTelemetry(stats))
        {
            return fail("LinearProgram storage statistics are inconsistent");
        }
        constexpr std::array<uint64_t, static_cast<std::size_t>(ProgramArena::Count)>
            expectedElements = {
                6, 8, 70, 4, 1, 2, 2, 3, 9, 2, 5, 6, 7,
                6, 5, 1, 1, 1, 1, 0, 0, 1, 2, 0, 0,
            };
        constexpr std::array<uint64_t, static_cast<std::size_t>(ProgramArena::Count)>
            minimumCapacities = {
                6, 8, 64, 4, 1, 2, 2, 3, 9, 2, 5, 6, 7,
                6, 5, 1, 1, 1, 1, 0, 0, 1, 2, 0, 0,
            };
        for (std::size_t index = 0; index < expectedElements.size(); ++index)
        {
            if (stats.arenas[index].elements != expectedElements[index] ||
                stats.arenas[index].capacity < minimumCapacities[index])
            {
                return fail("LinearProgram reserve or per-arena telemetry is inconsistent");
            }
        }

        const ValidationResult structural = validate(
            program,
            ValidationOptions{.level = ValidationLevel::Structural});
        const ValidationResult semantic = validate(
            program,
            ValidationOptions{.level = ValidationLevel::Semantic});
        if (!structural.success() || !semantic.success())
        {
            return fail("valid LinearProgram must pass structural and semantic validation");
        }

        LinearProgram moved = std::move(program);
        if (program.valid() || !moved.valid() || moved.view().instructionCount() != 5)
        {
            return fail("LinearProgram move must transfer ownership and invalidate the source");
        }
        return 0;
    }

    int testBuilderClonesSelfReferentialSpans()
    {
        LinearProgramBuilder builder;
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId stringType = builder.addType(Type::string());

        const StringId originalText = builder.addString("self-span");
        const StringId copiedText = builder.addString(builder.view().string(originalText));
        const StringId firstSymbol = builder.addString("first_import");
        const StringId secondSymbol = builder.addString("second_import");
        const StringId parameterName = builder.addString("value");

        const std::array<uint64_t, 1> words = {UINT64_C(0xa5)};
        const LiteralId originalBits = builder.addBitLiteral(valueType, words);
        const LiteralId copiedBits =
            builder.addBitLiteral(valueType, builder.view().literal(originalBits).words);
        const LiteralId originalString = builder.addStringLiteral(stringType, "payload");
        const LiteralId copiedString = builder.addStringLiteral(
            stringType,
            builder.view().literal(originalString).bytes);

        const std::array<InitAction, 1> actions = {
            InitAction{
                .kind = InitActionKind::Set,
                .rangeKind = InitRangeKind::All,
                .expression = InitExpr{
                    .kind = InitExprKind::Literal,
                    .literal = originalBits,
                },
            },
        };
        const InitId originalActions = builder.addActionsInit(actions);
        const InitId copiedActions =
            builder.addActionsInit(builder.view().initActions(originalActions));

        const std::array<DpiParameter, 1> parameters = {
            DpiParameter{
                .name = parameterName,
                .type = valueType,
                .direction = DpiDirection::Input,
                .abi = DpiAbiKind::Integral,
            },
        };
        const DpiImportId firstImport = builder.addDpiImport(firstSymbol, parameters);
        const DpiImportId secondImport = builder.addDpiImport(
            secondSymbol,
            builder.view().dpiImport(firstImport).parameters);

        const VariableId source = builder.addVariable(valueType, builder.zeroInit());
        const VariableId operand = builder.addVariable(valueType, builder.zeroInit());
        const VariableId firstResult = builder.addVariable(valueType, builder.undefInit());
        const VariableId thirdResult = builder.addVariable(valueType, builder.undefInit());
        builder.addVariable(valueType, copiedActions);
        const InstructionId first = addInstruction(
            builder,
            Opcode::Assign,
            {firstResult},
            {source});

        const std::span<const VariableId> aliasedResults = builder.view().operands(first);
        const std::array<VariableId, 1> secondOperands = {operand};
        const InstructionId second =
            builder.addInstruction(Opcode::Assign, aliasedResults, secondOperands);

        const std::array<VariableId, 1> thirdResults = {thirdResult};
        const std::span<const VariableId> aliasedOperands = builder.view().results(second);
        const InstructionId third =
            builder.addInstruction(Opcode::Assign, thirdResults, aliasedOperands);

        LinearProgram program = builder.finish();
        const ProgramView view = program.view();
        if (view.string(copiedText) != "self-span" ||
            view.literal(copiedBits).words.size() != 1 ||
            view.literal(copiedBits).words.front() != words.front() ||
            view.literal(copiedString).bytes != "payload" ||
            view.initActions(copiedActions).size() != 1 ||
            view.initActions(copiedActions).front().expression.literal != originalBits ||
            view.dpiImport(secondImport).parameters.size() != 1 ||
            view.dpiImport(secondImport).parameters.front().name != parameterName ||
            view.results(second).size() != 1 || view.results(second).front() != source ||
            view.operands(third).size() != 1 || view.operands(third).front() != source)
        {
            return fail("builder did not preserve self-referential append spans");
        }
        if (!validate(program, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("self-referential builder appends must produce a valid LinearProgram");
        }
        return 0;
    }

    int testScheduledReserveAndArenaTelemetry()
    {
        LinearProgramBuilder linearBuilder;
        const TypeId type = linearBuilder.addType(Type::bitVector(8));
        const VariableId firstSource =
            linearBuilder.addVariable(type, linearBuilder.zeroInit());
        const VariableId secondSource =
            linearBuilder.addVariable(type, linearBuilder.zeroInit());
        const VariableId firstResult =
            linearBuilder.addVariable(type, linearBuilder.undefInit());
        const VariableId secondResult =
            linearBuilder.addVariable(type, linearBuilder.undefInit());
        const InstructionId first = addInstruction(
            linearBuilder,
            Opcode::Assign,
            {firstResult},
            {firstSource});
        const InstructionId second = addInstruction(
            linearBuilder,
            Opcode::Assign,
            {secondResult},
            {secondSource});

        ScheduledProgramBuilder builder(linearBuilder.finish());
        const ProgramStorageStats before = builder.view().storageStats();
        builder.reserve(ScheduledProgramReserve{
            .additionalTypes = 2,
            .additionalStrings = 2,
            .additionalStringBytes = 16,
            .additionalVariables = 3,
            .additionalVariableLabels = 2,
            .additionalInstructions = 4,
            .additionalOperands = 5,
            .additionalResults = 6,
            .additionalSliceStaticAttributes = 1,
            .additionalSystemFunctionAttributes = 1,
            .additionalSystemTaskAttributes = 1,
            .additionalDpiCallAttributes = 1,
            .blocks = 2,
            .blockInstructionIds = 2,
            .activationInstructions = 3,
            .activationTargets = 4,
        });
        const ProgramStorageStats reserved = builder.view().storageStats();
        if (!hasCompleteArenaTelemetry(reserved) ||
            reserved.arena(ProgramArena::Types).capacity < before.arena(ProgramArena::Types).elements + 2 ||
            reserved.arena(ProgramArena::StringOffsets).capacity < before.arena(ProgramArena::StringOffsets).elements + 2 ||
            reserved.arena(ProgramArena::StringBytes).capacity < before.arena(ProgramArena::StringBytes).elements + 16 ||
            reserved.arena(ProgramArena::Variables).capacity < before.arena(ProgramArena::Variables).elements + 3 ||
            reserved.arena(ProgramArena::VariableLabels).capacity < before.arena(ProgramArena::VariableLabels).elements + 2 ||
            reserved.arena(ProgramArena::Opcodes).capacity < before.arena(ProgramArena::Opcodes).elements + 4 ||
            reserved.arena(ProgramArena::OperandOffsets).capacity < before.arena(ProgramArena::OperandOffsets).elements + 4 ||
            reserved.arena(ProgramArena::Operands).capacity < before.arena(ProgramArena::Operands).elements + 5 ||
            reserved.arena(ProgramArena::ResultOffsets).capacity < before.arena(ProgramArena::ResultOffsets).elements + 4 ||
            reserved.arena(ProgramArena::Results).capacity < before.arena(ProgramArena::Results).elements + 6 ||
            reserved.arena(ProgramArena::SliceStaticAttributes).capacity < 1 ||
            reserved.arena(ProgramArena::SystemFunctionAttributes).capacity < 1 ||
            reserved.arena(ProgramArena::SystemTaskAttributes).capacity < 1 ||
            reserved.arena(ProgramArena::DpiCallAttributes).capacity < 1 ||
            reserved.arena(ProgramArena::BlockOffsets).capacity < before.arena(ProgramArena::BlockOffsets).elements + 2 ||
            reserved.arena(ProgramArena::ActivationAttributes).capacity < 3 ||
            reserved.arena(ProgramArena::ActivationTargets).capacity < 4)
        {
            return fail("ScheduledProgramReserve did not cover every appendable arena");
        }

        addBlock(builder, {second});
        addBlock(builder, {first});
        const ScheduledProgram program = builder.finish();
        const ProgramStorageStats finalStats = program.view().storageStats();
        if (!hasCompleteArenaTelemetry(finalStats) ||
            finalStats.arena(ProgramArena::BlockInstructions).elements != 2 ||
            finalStats.arena(ProgramArena::BlockInstructions).capacity < 2)
        {
            return fail("scheduled block-permutation reserve was not retained when materialized");
        }
        return 0;
    }

    int testReserveRejects32BitOverflowWithoutAllocation()
    {
        if constexpr (sizeof(std::size_t) > sizeof(uint32_t))
        {
            const std::size_t tooMany =
                static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) + 1;
            LinearProgramBuilder builder;
            const ProgramStorageStats before = builder.view().storageStats();
            bool rejected = false;
            try
            {
                builder.reserve(ProgramReserve{.types = tooMany});
            }
            catch (const std::overflow_error &)
            {
                rejected = true;
            }
            const ProgramStorageStats after = builder.view().storageStats();
            if (!rejected)
            {
                return fail("ProgramReserve must reject an arena larger than UINT32_MAX");
            }
            for (std::size_t index = 0; index < before.arenas.size(); ++index)
            {
                if (before.arenas[index].capacity != after.arenas[index].capacity)
                {
                    return fail("rejected ProgramReserve must not allocate any arena");
                }
            }
        }

        {
            LinearProgramBuilder builder;
            const ProgramStorageStats before = builder.view().storageStats();
            bool rejected = false;
            try
            {
                builder.reserve(ProgramReserve{
                    .strings = std::numeric_limits<uint32_t>::max(),
                });
            }
            catch (const std::overflow_error &)
            {
                rejected = true;
            }
            const ProgramStorageStats after = builder.view().storageStats();
            if (!rejected)
            {
                return fail("offset-table reserve must leave room for its trailing sentinel");
            }
            for (std::size_t index = 0; index < before.arenas.size(); ++index)
            {
                if (before.arenas[index].capacity != after.arenas[index].capacity)
                {
                    return fail("rejected offset-table reserve must not allocate any arena");
                }
            }
        }

        {
            LinearProgramBuilder linearBuilder;
            linearBuilder.addType(Type::bitVector(1));
            ScheduledProgramBuilder builder(linearBuilder.finish());
            const ProgramStorageStats before = builder.view().storageStats();
            bool rejected = false;
            try
            {
                builder.reserve(ScheduledProgramReserve{
                    .additionalTypes = std::numeric_limits<uint32_t>::max(),
                });
            }
            catch (const std::overflow_error &)
            {
                rejected = true;
            }
            const ProgramStorageStats after = builder.view().storageStats();
            if (!rejected)
            {
                return fail("ScheduledProgramReserve must reject current-plus-additional overflow");
            }
            for (std::size_t index = 0; index < before.arenas.size(); ++index)
            {
                if (before.arenas[index].capacity != after.arenas[index].capacity)
                {
                    return fail("rejected ScheduledProgramReserve must not allocate any arena");
                }
            }
        }
        return 0;
    }

    int testScheduledBuilderPreservesLinearStorageOwnership()
    {
        LinearProgramBuilder builder;
        builder.reserve(ProgramReserve{
            .types = 2,
            .strings = 3,
            .stringBytes = 32,
            .initDescriptors = 3,
            .initActions = 1,
            .literals = 2,
            .literalWords = 1,
            .literalBytes = 4,
            .variables = 3,
            .instructions = 1,
            .operands = 1,
            .results = 1,
            .dpiImports = 1,
            .dpiParameters = 1,
        });
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId stringType = builder.addType(Type::string());
        const StringId text = builder.addString("storage");
        const StringId symbol = builder.addString("dpi_symbol");
        const StringId parameterName = builder.addString("value");
        const std::array<uint64_t, 1> words = {UINT64_C(0x5a)};
        const LiteralId bits = builder.addBitLiteral(valueType, words);
        const LiteralId bytes = builder.addStringLiteral(stringType, "text");
        const std::array<InitAction, 1> actions = {
            InitAction{
                .kind = InitActionKind::Set,
                .expression = InitExpr{
                    .kind = InitExprKind::Literal,
                    .literal = bits,
                },
            },
        };
        const InitId actionInit = builder.addActionsInit(actions);
        const VariableId source = builder.addVariable(valueType, actionInit);
        const VariableId result = builder.addVariable(valueType, builder.undefInit());
        builder.addVariable(stringType, builder.zeroInit());
        const InstructionId assign = addInstruction(
            builder,
            Opcode::Assign,
            {result},
            {source});
        const std::array<DpiParameter, 1> parameters = {
            DpiParameter{
                .name = parameterName,
                .type = valueType,
                .direction = DpiDirection::Input,
                .abi = DpiAbiKind::Integral,
            },
        };
        const DpiImportId import = builder.addDpiImport(symbol, parameters);
        LinearProgram linear = builder.finish();
        const ProgramView linearView = linear.view();
        const ProgramStorageStats linearStats = linearView.storageStats();
        const Type *typeAddress = &linearView.type(valueType);
        const char *stringAddress = linearView.string(text).data();
        const InitAction *actionAddress = linearView.initActions(actionInit).data();
        const uint64_t *wordAddress = linearView.literal(bits).words.data();
        const char *byteAddress = linearView.literal(bytes).bytes.data();
        const VariableRecord *variableAddress = &linearView.variable(source);
        const VariableId *operandAddress = linearView.operands(assign).data();
        const VariableId *resultAddress = linearView.results(assign).data();
        const DpiParameter *parameterAddress = linearView.dpiImport(import).parameters.data();

        ScheduledProgramBuilder scheduledBuilder(std::move(linear));
        const ProgramView scheduledView = scheduledBuilder.view();
        const ProgramStorageStats scheduledStats = scheduledView.storageStats();
        if (linear.valid() || &scheduledView.type(valueType) != typeAddress ||
            scheduledView.string(text).data() != stringAddress ||
            scheduledView.initActions(actionInit).data() != actionAddress ||
            scheduledView.literal(bits).words.data() != wordAddress ||
            scheduledView.literal(bytes).bytes.data() != byteAddress ||
            &scheduledView.variable(source) != variableAddress ||
            scheduledView.operands(assign).data() != operandAddress ||
            scheduledView.results(assign).data() != resultAddress ||
            scheduledView.dpiImport(import).parameters.data() != parameterAddress)
        {
            return fail("ScheduledProgramBuilder must take ownership without copying prefix arenas");
        }
        for (std::size_t index = 0;
             index < static_cast<std::size_t>(ProgramArena::BlockOffsets);
             ++index)
        {
            if (linearStats.arenas[index].capacity != scheduledStats.arenas[index].capacity)
            {
                return fail("ScheduledProgramBuilder must preserve prefix arena capacities");
            }
        }
        return 0;
    }

    int testSemanticValidationIsStricterThanStructuralValidation()
    {
        LinearProgramBuilder builder;
        const TypeId type = builder.addType(Type::bitVector(8));
        const VariableId source = builder.addVariable(type, builder.zeroInit());
        const VariableId result = builder.addVariable(type, builder.undefInit());
        const InstructionId first = addInstruction(builder, Opcode::Assign, {result}, {source});
        const InstructionId second = addInstruction(builder, Opcode::Assign, {result}, {source});
        LinearProgram program = builder.finish();

        const ValidationResult structural = validate(
            program,
            ValidationOptions{.level = ValidationLevel::Structural});
        const ValidationResult semantic = validate(
            program,
            ValidationOptions{.level = ValidationLevel::Semantic});
        if (!structural.success())
        {
            return fail("multiple writers are structurally representable");
        }
        if (semantic.success() || !containsError(semantic, "multiple result writers"))
        {
            return fail("semantic validation must reject multiple writers in Linear normal form");
        }

        ScheduledProgramBuilder scheduledBuilder(std::move(program));
        addBlock(scheduledBuilder, {first, second});
        ScheduledProgram scheduled = scheduledBuilder.finish();
        if (!validate(scheduled, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("ScheduledProgram must accept legal repeated result writers");
        }

        LinearProgramBuilder aliasBuilder;
        const TypeId aliasType = aliasBuilder.addType(Type::bitVector(8));
        const VariableId aliased = aliasBuilder.addVariable(aliasType, aliasBuilder.zeroInit());
        const InstructionId alias =
            addInstruction(aliasBuilder, Opcode::Assign, {aliased}, {aliased});
        LinearProgram aliasProgram = aliasBuilder.finish();
        const ValidationResult aliasStructural = validate(
            aliasProgram,
            ValidationOptions{.level = ValidationLevel::Structural});
        const ValidationResult aliasSemantic = validate(
            aliasProgram,
            ValidationOptions{.level = ValidationLevel::Semantic});
        if (!aliasStructural.success() || aliasSemantic.success())
        {
            return fail("Linear normal form must reject Result/Operand aliases semantically only");
        }

        ScheduledProgramBuilder aliasScheduledBuilder(std::move(aliasProgram));
        addBlock(aliasScheduledBuilder, {alias});
        ScheduledProgram aliasScheduled = aliasScheduledBuilder.finish();
        if (!validate(aliasScheduled, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("ScheduledProgram must accept legal Result/Operand aliases");
        }
        return 0;
    }

    int testValidatorRejectsInvalidEnumsAndInitRanges()
    {
        {
            LinearProgramBuilder builder;
            Type invalidType = Type::bitVector(8);
            invalidType.kind = static_cast<TypeKind>(255);
            const TypeId type = builder.addType(invalidType);
            builder.addVariable(type, builder.zeroInit());
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "invalid AM type record"))
            {
                return fail("validator must reject an invalid TypeKind");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(
                Type::bitVector(8, static_cast<Signedness>(255)));
            builder.addVariable(type, builder.zeroInit());
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "invalid AM type signedness"))
            {
                return fail("validator must reject an invalid Signedness");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::bitVector(8));
            const std::array<InitAction, 1> actions = {
                InitAction{
                    .kind = static_cast<InitActionKind>(255),
                    .expression = InitExpr{.kind = InitExprKind::Random},
                },
            };
            const InitId init = builder.addActionsInit(actions);
            builder.addVariable(type, init);
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "invalid enum value"))
            {
                return fail("validator must reject an invalid init-action enum");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::array(4, 8));
            const StringId path = builder.addString("init.hex");
            const std::array<InitAction, 1> actions = {
                InitAction{
                    .kind = InitActionKind::Load,
                    .format = LoadFormat::Hex,
                    .rangeKind = InitRangeKind::Span,
                    .start = std::numeric_limits<uint64_t>::max(),
                    .count = 1,
                    .path = path,
                },
            };
            const InitId init = builder.addActionsInit(actions);
            builder.addVariable(type, init);
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "load init is invalid"))
            {
                return fail("validator must reject a wrapping load span");
            }
        }
        {
            LinearProgramBuilder builder;
            builder.addInstruction(static_cast<Opcode>(255), {}, {});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "invalid opcode"))
            {
                return fail("validator must reject an invalid Opcode");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const StringId name = builder.addString("task");
            const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
            const InstructionId task = addInstruction(
                builder,
                Opcode::SystemTask,
                {},
                {condition});
            builder.setSystemTaskAttributes(
                task,
                SystemTaskAttributes{
                    .name = name,
                    .eventCount = 0,
                    .schedule = static_cast<CallSchedule>(255),
                });
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "typed attributes"))
            {
                return fail("validator must reject an invalid CallSchedule");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const StringId name = builder.addString("task");
            const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
            const InstructionId task = addInstruction(
                builder,
                Opcode::SystemTask,
                {},
                {condition});
            builder.setSystemTaskAttributes(
                task,
                SystemTaskAttributes{
                    .name = name,
                    .eventCount = 0,
                    .schedule = CallSchedule::Normal,
                    .eventMode = static_cast<HostEventMode>(255),
                });
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "typed attributes"))
            {
                return fail("validator must reject an invalid HostEventMode");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::bitVector(8));
            const StringId symbol = builder.addString("dpi_invalid_direction");
            const StringId name = builder.addString("value");
            const std::array<DpiParameter, 1> parameters = {
                DpiParameter{
                    .name = name,
                    .type = type,
                    .direction = static_cast<DpiDirection>(255),
                    .abi = DpiAbiKind::Integral,
                },
            };
            builder.addDpiImport(symbol, parameters);
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Structural, "parameter declaration is invalid"))
            {
                return fail("validator must reject an invalid DPI direction");
            }
        }
        return 0;
    }

    int testValidatorRejectsInvalidInstructionSemantics()
    {
        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::real());
            const VariableId lhs = builder.addVariable(type, builder.zeroInit());
            const VariableId rhs = builder.addVariable(type, builder.zeroInit());
            const VariableId result = builder.addVariable(type, builder.undefInit());
            addInstruction(builder, Opcode::Add, {result}, {lhs, rhs});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "bit-vector variables"))
            {
                return fail("combinational instructions must reject non-BV values");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const std::array<uint64_t, 1> words = {0};
            const LiteralId literal = builder.addBitLiteral(valueType, words);
            const InitId constant = builder.addConstantInit(literal);
            const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
            const VariableId mask = builder.addVariable(valueType, builder.zeroInit());
            const VariableId next = builder.addVariable(valueType, builder.zeroInit());
            const VariableId target = builder.addVariable(valueType, constant);
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            addInstruction(
                builder,
                Opcode::RegisterWriteCondMask,
                {},
                {condition, mask, next, target, event});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "invalid Type or target"))
            {
                return fail("state writes must reject constant targets");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const TypeId addressType = builder.addType(Type::bitVector(2));
            const VariableId target = builder.addVariable(valueType, builder.zeroInit());
            const VariableId address = builder.addVariable(addressType, builder.zeroInit());
            const VariableId result = builder.addVariable(valueType, builder.undefInit());
            addInstruction(builder, Opcode::MemoryRead, {result}, {target, address});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "invalid Type or target"))
            {
                return fail("memory reads must require an Array target");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const TypeId oldType = builder.addType(Type::bitVector(16));
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const VariableId value = builder.addVariable(valueType, builder.zeroInit());
            const VariableId oldValue = builder.addVariable(oldType, builder.undefInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            addInstruction(builder, Opcode::ChangedAny, {event}, {value, oldValue});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "changed instruction"))
            {
                return fail("changed must require identical new and old Types");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const VariableId value = builder.addVariable(valueType, builder.zeroInit());
            const VariableId oldValue = builder.addVariable(valueType, builder.zeroInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            addInstruction(builder, Opcode::ChangedAny, {event}, {value, oldValue});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "changed instruction"))
            {
                return fail("changed old storage must use undef initialization");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const VariableId value = builder.addVariable(valueType, builder.zeroInit());
            const VariableId oldValue = builder.addVariable(valueType, builder.undefInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            const VariableId copy = builder.addVariable(valueType, builder.undefInit());
            addInstruction(builder, Opcode::ChangedAny, {event}, {value, oldValue});
            addInstruction(builder, Opcode::Assign, {copy}, {oldValue});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "used outside its detector"))
            {
                return fail("changed old storage must be detector-exclusive");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId type = builder.addType(Type::bitVector(8));
            const VariableId base = builder.addVariable(type, builder.zeroInit());
            const VariableId result = builder.addVariable(type, builder.undefInit());
            const InstructionId slice = addInstruction(
                builder,
                Opcode::SliceStatic,
                {result},
                {base});
            builder.setSliceStaticAttributes(slice, 1);
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "invalid Type signature"))
            {
                return fail("slice_static must reject an out-of-range result");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const StringId name = builder.addString("final_task");
            const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
            const VariableId event = builder.addVariable(eventType, builder.zeroInit());
            const InstructionId task = addInstruction(
                builder,
                Opcode::SystemTask,
                {},
                {condition, event});
            builder.setSystemTaskAttributes(
                task,
                SystemTaskAttributes{
                    .name = name,
                    .eventCount = 1,
                    .schedule = CallSchedule::Final,
                });
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "final schedule"))
            {
                return fail("final system tasks must reject event operands");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const StringId missing = builder.addString("missing_import");
            const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
            const InstructionId call = addInstruction(
                builder,
                Opcode::DpiCall,
                {},
                {condition});
            builder.setDpiCallAttributes(
                call,
                DpiCallAttributes{.importSymbol = missing, .eventCount = 0});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "unknown import"))
            {
                return fail("DPI calls must resolve their import symbol");
            }
        }
        {
            LinearProgramBuilder builder;
            const TypeId eventType = builder.addType(Type::bitVector(1));
            const TypeId valueType = builder.addType(Type::bitVector(8));
            const StringId symbol = builder.addString("takes_input");
            const StringId name = builder.addString("value");
            const VariableId condition = builder.addVariable(eventType, builder.zeroInit());
            const std::array<DpiParameter, 1> parameters = {
                DpiParameter{
                    .name = name,
                    .type = valueType,
                    .direction = DpiDirection::Input,
                    .abi = DpiAbiKind::Integral,
                },
            };
            builder.addDpiImport(symbol, parameters);
            const InstructionId call = addInstruction(
                builder,
                Opcode::DpiCall,
                {},
                {condition});
            builder.setDpiCallAttributes(
                call,
                DpiCallAttributes{.importSymbol = symbol, .eventCount = 0});
            const LinearProgram program = builder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "invalid operand or result count"))
            {
                return fail("DPI calls must match import parameter groups");
            }
        }
        {
            LinearProgramBuilder linearBuilder;
            const TypeId eventType =
                linearBuilder.addType(Type::bitVector(1, Signedness::Signed));
            const VariableId source =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.undefInit());
            const InstructionId assign = addInstruction(
                linearBuilder,
                Opcode::Assign,
                {event},
                {source});
            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const InstructionId activate = addInstruction(
                scheduledBuilder,
                Opcode::ActForward,
                {},
                {event});
            setActivationTargets(scheduledBuilder, activate, {BlockId{1}});
            addBlock(scheduledBuilder, {assign, activate});
            addBlock(scheduledBuilder, {});
            const ScheduledProgram program = scheduledBuilder.finish();
            if (!rejects(program, ValidationLevel::Semantic, "unsigned one-bit"))
            {
                return fail("activation events must be unsigned BV1 values");
            }
        }
        return 0;
    }

    int testScheduledBuilderRecoversFromRejectedAppends()
    {
        LinearProgramBuilder linearBuilder;
        const TypeId valueType = linearBuilder.addType(Type::bitVector(8));
        const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
        const VariableId source = linearBuilder.addVariable(valueType, linearBuilder.zeroInit());
        const VariableId result = linearBuilder.addVariable(valueType, linearBuilder.undefInit());
        const VariableId eventSource =
            linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
        const VariableId event = linearBuilder.addVariable(eventType, linearBuilder.undefInit());
        const InstructionId assign = addInstruction(
            linearBuilder,
            Opcode::Assign,
            {result},
            {source});
        const InstructionId defineEvent = addInstruction(
            linearBuilder,
            Opcode::Assign,
            {event},
            {eventSource});
        ScheduledProgramBuilder builder(linearBuilder.finish());
        const InstructionId activate = addInstruction(
            builder,
            Opcode::ActForward,
            {},
            {event});
        setActivationTargets(builder, activate, {BlockId{1}});

        bool rejectedInvalidBlock = false;
        const std::array<InstructionId, 2> invalidBlock = {
            assign,
            InstructionId::invalid(),
        };
        try
        {
            builder.addBlock(invalidBlock);
        }
        catch (const std::invalid_argument &)
        {
            rejectedInvalidBlock = true;
        }
        if (!rejectedInvalidBlock)
        {
            return fail("ScheduledProgramBuilder must reject an invalid Block instruction");
        }

        bool rejectedDuplicateTargets = false;
        try
        {
            setActivationTargets(builder, activate, {BlockId{1}});
        }
        catch (const std::invalid_argument &)
        {
            rejectedDuplicateTargets = true;
        }
        if (!rejectedDuplicateTargets)
        {
            return fail("ScheduledProgramBuilder must reject duplicate activation attributes");
        }

        addBlock(builder, {assign, defineEvent, activate});
        addBlock(builder, {});
        const ScheduledProgram program = builder.finish();
        const ProgramStorageStats stats = program.view().storageStats();
        if (stats.arena(ProgramArena::ActivationTargets).elements != 1 ||
            !validate(program, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("rejected activation attributes must not leak target arena entries");
        }
        return 0;
    }

    int testScheduledProgramLayoutsAndActivation()
    {
        LinearProgramBuilder linearBuilder;
        const TypeId valueType = linearBuilder.addType(Type::bitVector(8));
        const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
        const VariableId oldValue = linearBuilder.addVariable(valueType, linearBuilder.undefInit());
        const VariableId newValue = linearBuilder.addVariable(valueType, linearBuilder.zeroInit());
        const VariableId event = linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
        const VariableId output = linearBuilder.addVariable(valueType, linearBuilder.undefInit());
        const InstructionId changed = addInstruction(
            linearBuilder,
            Opcode::ChangedAny,
            {event},
            {newValue, oldValue});

        LinearProgram linear = linearBuilder.finish();
        ScheduledProgramBuilder scheduledBuilder(std::move(linear));
        scheduledBuilder.reserve(ScheduledProgramReserve{
            .additionalInstructions = 2,
            .additionalOperands = 2,
            .additionalResults = 1,
            .blocks = 2,
            .blockInstructionIds = 3,
            .activationInstructions = 1,
            .activationTargets = 1,
        });
        if (linear.valid())
        {
            return fail("ScheduledProgramBuilder must consume its LinearProgram");
        }
        const InstructionId activate = addInstruction(
            scheduledBuilder,
            Opcode::ActForward,
            {},
            {event});
        setActivationTargets(scheduledBuilder, activate, {BlockId{1}});
        const InstructionId assign = addInstruction(
            scheduledBuilder,
            Opcode::Assign,
            {output},
            {newValue});
        addBlock(scheduledBuilder, {changed, activate});
        addBlock(scheduledBuilder, {assign});
        if (scheduledBuilder.pendingBlockCount() != 2)
        {
            return fail("ScheduledProgramBuilder must expose its pending dense block count");
        }

        ScheduledProgram scheduled = scheduledBuilder.finish();
        if (!scheduled.valid() || scheduled.blockCount() != 2 ||
            scheduled.blockSize(BlockId{0}) != 2 || scheduled.blockSize(BlockId{1}) != 1 ||
            scheduled.blockInstruction(BlockId{0}, 0) != changed ||
            scheduled.blockInstruction(BlockId{0}, 1) != activate ||
            scheduled.blockInstruction(BlockId{1}, 0) != assign)
        {
            return fail("ScheduledProgram block layout did not round-trip");
        }
        const ProgramStorageStats identityStats = scheduled.view().storageStats();
        if (identityStats.blocks != 2 || identityStats.blockInstructionIds != 0)
        {
            return fail("identity block instruction order must elide the explicit ID arena");
        }
        const auto activation = scheduled.view().activationAttributes(activate);
        if (!activation || activation->targets.size() != 1 ||
            activation->targets[0] != BlockId{1})
        {
            return fail("activation target CSR span did not round-trip");
        }
        const ValidationResult semantic = validate(
            scheduled,
            ValidationOptions{.level = ValidationLevel::Semantic});
        if (!semantic.success())
        {
            return fail("valid ScheduledProgram must pass semantic validation");
        }

        ScheduledProgram moved = std::move(scheduled);
        if (scheduled.valid() || !moved.valid() || moved.blockCount() != 2)
        {
            return fail("ScheduledProgram move must transfer ownership and invalidate the source");
        }

        LinearProgramBuilder reorderedLinearBuilder;
        const TypeId reorderedType = reorderedLinearBuilder.addType(Type::bitVector(8));
        const VariableId firstSource =
            reorderedLinearBuilder.addVariable(reorderedType, reorderedLinearBuilder.zeroInit());
        const VariableId secondSource =
            reorderedLinearBuilder.addVariable(reorderedType, reorderedLinearBuilder.zeroInit());
        const VariableId firstResult =
            reorderedLinearBuilder.addVariable(reorderedType, reorderedLinearBuilder.undefInit());
        const VariableId secondResult =
            reorderedLinearBuilder.addVariable(reorderedType, reorderedLinearBuilder.undefInit());
        const InstructionId first = addInstruction(
            reorderedLinearBuilder,
            Opcode::Assign,
            {firstResult},
            {firstSource});
        const InstructionId second = addInstruction(
            reorderedLinearBuilder,
            Opcode::Assign,
            {secondResult},
            {secondSource});
        ScheduledProgramBuilder reorderedBuilder(reorderedLinearBuilder.finish());
        addBlock(reorderedBuilder, {second});
        addBlock(reorderedBuilder, {first});
        ScheduledProgram reordered = reorderedBuilder.finish();
        if (reordered.view().storageStats().blockInstructionIds != 2 ||
            reordered.blockInstruction(BlockId{0}, 0) != second ||
            reordered.blockInstruction(BlockId{1}, 0) != first)
        {
            return fail("non-identity block order must retain and use the explicit ID arena");
        }
        if (!validate(reordered, ValidationOptions{.level = ValidationLevel::Semantic}).success())
        {
            return fail("valid non-identity ScheduledProgram layout must pass validation");
        }
        return 0;
    }

    int testScheduledValidatorRejectsIllegalEntryBlock()
    {
        LinearProgramBuilder linearBuilder;
        const TypeId type = linearBuilder.addType(Type::bitVector(8));
        std::array<VariableId, 5> operands;
        for (VariableId &operand : operands)
        {
            operand = linearBuilder.addVariable(type, linearBuilder.zeroInit());
        }
        const InstructionId stateWrite = linearBuilder.addInstruction(
            Opcode::RegisterWrite,
            {},
            operands);
        ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
        addBlock(scheduledBuilder, {stateWrite});
        ScheduledProgram scheduled = scheduledBuilder.finish();

        const ValidationResult result = validate(
            scheduled,
            ValidationOptions{.level = ValidationLevel::Structural});
        if (result.success() || !containsError(result, "EntryBlock contains a forbidden opcode"))
        {
            return fail("validator must reject stateful instructions in B0");
        }
        return 0;
    }

    int testScheduledValidatorRejectsIllegalActivationTargets()
    {
        {
            LinearProgramBuilder linearBuilder;
            const TypeId valueType = linearBuilder.addType(Type::bitVector(8));
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const VariableId oldValue =
                linearBuilder.addVariable(valueType, linearBuilder.undefInit());
            const VariableId newValue =
                linearBuilder.addVariable(valueType, linearBuilder.zeroInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const InstructionId changed = addInstruction(
                linearBuilder,
                Opcode::ChangedAny,
                {event},
                {newValue, oldValue});
            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const InstructionId activate = addInstruction(
                scheduledBuilder,
                Opcode::ActForward,
                {},
                {event});
            setActivationTargets(scheduledBuilder, activate, {BlockId{2}});
            addBlock(scheduledBuilder, {changed, activate});
            addBlock(scheduledBuilder, {});
            ScheduledProgram scheduled = scheduledBuilder.finish();
            const ValidationResult result = validate(
                scheduled,
                ValidationOptions{.level = ValidationLevel::Structural});
            if (result.success() || !containsError(result, "outside the Program"))
            {
                return fail("validator must reject an out-of-range activation target");
            }
        }

        {
            LinearProgramBuilder linearBuilder;
            const TypeId valueType = linearBuilder.addType(Type::bitVector(8));
            const TypeId eventType = linearBuilder.addType(Type::bitVector(1));
            const VariableId oldValue =
                linearBuilder.addVariable(valueType, linearBuilder.undefInit());
            const VariableId newValue =
                linearBuilder.addVariable(valueType, linearBuilder.zeroInit());
            const VariableId event =
                linearBuilder.addVariable(eventType, linearBuilder.zeroInit());
            const InstructionId changed = addInstruction(
                linearBuilder,
                Opcode::ChangedAny,
                {event},
                {newValue, oldValue});
            ScheduledProgramBuilder scheduledBuilder(linearBuilder.finish());
            const InstructionId activate = addInstruction(
                scheduledBuilder,
                Opcode::ActBackward,
                {},
                {event});
            setActivationTargets(scheduledBuilder, activate, {BlockId{0}});
            addBlock(scheduledBuilder, {});
            addBlock(scheduledBuilder, {changed, activate});
            ScheduledProgram scheduled = scheduledBuilder.finish();
            const ValidationResult result = validate(
                scheduled,
                ValidationOptions{.level = ValidationLevel::Structural});
            if (result.success() || !containsError(result, "act.b targets EntryBlock"))
            {
                return fail("validator must reject act.b targeting B0");
            }
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testLinearProgramDataAndValidation(); result != 0)
    {
        return result;
    }
    if (const int result = testBuilderClonesSelfReferentialSpans(); result != 0)
    {
        return result;
    }
    if (const int result = testScheduledReserveAndArenaTelemetry(); result != 0)
    {
        return result;
    }
    if (const int result = testReserveRejects32BitOverflowWithoutAllocation(); result != 0)
    {
        return result;
    }
    if (const int result = testScheduledBuilderPreservesLinearStorageOwnership(); result != 0)
    {
        return result;
    }
    if (const int result = testSemanticValidationIsStricterThanStructuralValidation(); result != 0)
    {
        return result;
    }
    if (const int result = testValidatorRejectsInvalidEnumsAndInitRanges(); result != 0)
    {
        return result;
    }
    if (const int result = testValidatorRejectsInvalidInstructionSemantics(); result != 0)
    {
        return result;
    }
    if (const int result = testScheduledBuilderRecoversFromRejectedAppends(); result != 0)
    {
        return result;
    }
    if (const int result = testScheduledProgramLayoutsAndActivation(); result != 0)
    {
        return result;
    }
    if (const int result = testScheduledValidatorRejectsIllegalEntryBlock(); result != 0)
    {
        return result;
    }
    if (const int result = testScheduledValidatorRejectsIllegalActivationTargets(); result != 0)
    {
        return result;
    }
    return 0;
}
