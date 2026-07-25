#include "grhsim/am/activity_schedule.hpp"
#include "grhsim/am/builder.hpp"
#include "grhsim/am/cpp_emitter.hpp"
#include "grhsim/am/interpreter.hpp"
#include "grhsim/am/production_activity_schedule.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << message << '\n';
        return 1;
    }

    bool hasArtifact(const GrhSimAmCppResult &result, std::string_view filename)
    {
        for (const std::string &artifact : result.artifacts)
        {
            if (std::filesystem::path(artifact).filename() == filename)
            {
                return true;
            }
        }
        return false;
    }

    bool hasDiagnosticContaining(const wolvrix::lib::diag::Diagnostics &diagnostics,
                                 std::string_view needle)
    {
        for (const wolvrix::lib::diag::Diagnostic &diagnostic : diagnostics.messages())
        {
            if (diagnostic.message.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    bool hasExactMakefileSources(const GrhSimAmCppResult &result,
                                 const std::filesystem::path &outputDirectory,
                                 std::initializer_list<std::string_view> expectedSources)
    {
        std::ifstream makefile(outputDirectory / "Makefile");
        std::string line;
        while (std::getline(makefile, line) && !line.starts_with("SRCS := "))
        {
        }
        if (!makefile || !line.starts_with("SRCS := "))
        {
            return false;
        }

        std::istringstream sourceList(line.substr(std::string_view("SRCS := ").size()));
        std::vector<std::string> listedSources;
        for (std::string source; sourceList >> source;)
        {
            listedSources.push_back(std::move(source));
        }
        std::vector<std::string> artifactSources;
        for (const std::string &artifact : result.artifacts)
        {
            const std::filesystem::path path(artifact);
            if (path.extension() == ".cpp")
            {
                artifactSources.push_back(path.filename().string());
            }
        }
        std::vector<std::string> expected;
        expected.reserve(expectedSources.size());
        for (std::string_view source : expectedSources)
        {
            expected.emplace_back(source);
        }
        return listedSources == expected && artifactSources == expected;
    }

    std::optional<std::string> readTextFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return std::nullopt;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        if (!input && !input.eof())
        {
            return std::nullopt;
        }
        return contents.str();
    }

    std::size_t countOccurrences(std::string_view text, std::string_view needle)
    {
        if (needle.empty())
        {
            return 0;
        }
        std::size_t count = 0;
        std::size_t position = 0;
        while ((position = text.find(needle, position)) != std::string_view::npos)
        {
            ++count;
            position += needle.size();
        }
        return count;
    }

    LinearProgramArtifact makeAddProgram()
    {
        LinearProgramBuilder builder;
        const TypeId valueType = builder.addType(Type::bitVector(8));
        const TypeId wideType = builder.addType(Type::bitVector(130));
        const StringId inputName = builder.addString("input_value");
        const StringId outputName = builder.addString("output_value");
        const StringId initializedName = builder.addString("initialized_value");
        const StringId randomName = builder.addString("random_value");
        const StringId wideInputName = builder.addString("wide_input");
        const StringId wideOutputName = builder.addString("wide_output");
        const StringId wideInitializedName = builder.addString("wide_initialized");
        const VariableId input = builder.addVariable(valueType, builder.zeroInit());
        const std::array<uint64_t, 1> oneWords = {1};
        const LiteralId oneLiteral = builder.addBitLiteral(valueType, oneWords);
        const VariableId one =
            builder.addVariable(valueType, builder.addConstantInit(oneLiteral));
        const VariableId output = builder.addVariable(valueType, builder.zeroInit());
        const std::array<uint64_t, 1> initializedWords = {0x5a};
        const LiteralId initializedLiteral =
            builder.addBitLiteral(valueType, initializedWords);
        const InitAction initializedAction{
            .kind = InitActionKind::Set,
            .expression = InitExpr{
                .kind = InitExprKind::Literal,
                .literal = initializedLiteral,
            },
        };
        const VariableId initialized = builder.addVariable(
            valueType,
            builder.addActionsInit(std::span<const InitAction>(&initializedAction, 1)));
        const InitAction randomAction{
            .kind = InitActionKind::Set,
            .expression = InitExpr{
                .kind = InitExprKind::Random,
            },
        };
        const VariableId random = builder.addVariable(
            valueType,
            builder.addActionsInit(std::span<const InitAction>(&randomAction, 1)));
        const VariableId wideInput = builder.addVariable(wideType, builder.zeroInit());
        const VariableId wideOutput = builder.addVariable(wideType, builder.zeroInit());
        const std::array<uint64_t, 3> wideInitializedWords = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
            UINT64_C(0x2),
        };
        const LiteralId wideInitializedLiteral =
            builder.addBitLiteral(wideType, wideInitializedWords);
        const InitAction wideInitializedAction{
            .kind = InitActionKind::Set,
            .expression = InitExpr{
                .kind = InitExprKind::Literal,
                .literal = wideInitializedLiteral,
            },
        };
        const VariableId wideInitialized = builder.addVariable(
            wideType,
            builder.addActionsInit(std::span<const InitAction>(&wideInitializedAction, 1)));
        const std::array<VariableId, 1> results = {output};
        const std::array<VariableId, 2> operands = {input, one};
        builder.addInstruction(Opcode::Add, results, operands);
        const std::array<VariableId, 1> wideResults = {wideOutput};
        const std::array<VariableId, 1> wideOperands = {wideInput};
        builder.addInstruction(Opcode::Assign, wideResults, wideOperands);

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
            PortBinding{
                .name = initializedName,
                .direction = PortDirection::Output,
                .output = initialized,
            },
            PortBinding{
                .name = randomName,
                .direction = PortDirection::Output,
                .output = random,
            },
            PortBinding{
                .name = wideInputName,
                .direction = PortDirection::Input,
                .input = wideInput,
            },
            PortBinding{
                .name = wideOutputName,
                .direction = PortDirection::Output,
                .output = wideOutput,
            },
            PortBinding{
                .name = wideInitializedName,
                .direction = PortDirection::Output,
                .output = wideInitialized,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput,
            VariableRole::None,
            VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
            VariableRole::ExternalInput,
            VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::Pure,
        };
        return LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };
    }

    VariableId addBitConstant(LinearProgramBuilder &builder,
                              TypeId type,
                              std::span<const uint64_t> words)
    {
        return builder.addVariable(
            type, builder.addConstantInit(builder.addBitLiteral(type, words)));
    }

    struct WidePureFixture
    {
        LinearProgramArtifact artifact;
        std::vector<std::pair<std::string, VariableId>> outputs;
    };

    WidePureFixture makeWidePureProgram()
    {
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        const TypeId u65Type = builder.addType(Type::bitVector(65));
        const TypeId s65Type =
            builder.addType(Type::bitVector(65, Signedness::Signed));
        const TypeId u96Type = builder.addType(Type::bitVector(96));
        const TypeId s96Type =
            builder.addType(Type::bitVector(96, Signedness::Signed));
        const TypeId u130Type = builder.addType(Type::bitVector(130));

        const std::array<uint64_t, 1> one = {1};
        const std::array<uint64_t, 1> four = {4};
        const std::array<uint64_t, 1> sixtySeven = {67};
        const std::array<uint64_t, 2> indexOne = {1, 0};
        const std::array<uint64_t, 2> indexSixtyOne = {61, 0};
        const std::array<uint64_t, 2> u65A = {
            UINT64_C(0xfedcba9876543210), UINT64_C(1)};
        const std::array<uint64_t, 2> u65B = {
            UINT64_C(0x0123456789abcdef), UINT64_C(0)};
        const std::array<uint64_t, 2> s65Negative = {
            UINT64_C(0xfffffffffffffff9), UINT64_C(1)};
        const std::array<uint64_t, 2> s65Positive = {UINT64_C(5), UINT64_C(0)};
        const std::array<uint64_t, 2> u96A = {
            UINT64_C(0x1122334455667788), UINT64_C(0x00000002)};
        const std::array<uint64_t, 2> u96HighBit = {
            UINT64_C(0x0123456789abcdef), UINT64_C(0x80000000)};
        const std::array<uint64_t, 2> u96Ones = {
            UINT64_MAX, UINT64_C(0xffffffff)};
        const std::array<uint64_t, 2> s96Negative = {
            UINT64_C(0xfffffffffffffffd), UINT64_C(0xffffffff)};
        const std::array<uint64_t, 2> s96Positive = {
            UINT64_C(0x123456789abcdef0), UINT64_C(0)};
        const std::array<uint64_t, 3> u130Base = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
            UINT64_C(2),
        };
        const std::array<uint64_t, 3> u130Ones = {
            UINT64_MAX, UINT64_MAX, UINT64_C(3)};

        const VariableId condition = addBitConstant(builder, u1Type, one);
        const VariableId amountFour = addBitConstant(builder, u8Type, four);
        const VariableId amountSixtySeven =
            addBitConstant(builder, u8Type, sixtySeven);
        const VariableId dynamicIndex =
            addBitConstant(builder, u65Type, indexSixtyOne);
        const VariableId arrayIndex = addBitConstant(builder, u65Type, indexOne);
        const VariableId unsigned65A = addBitConstant(builder, u65Type, u65A);
        const VariableId unsigned65B = addBitConstant(builder, u65Type, u65B);
        const VariableId signed65Negative =
            addBitConstant(builder, s65Type, s65Negative);
        const VariableId signed65Positive =
            addBitConstant(builder, s65Type, s65Positive);
        const VariableId unsigned96A = addBitConstant(builder, u96Type, u96A);
        const VariableId unsigned96HighBit =
            addBitConstant(builder, u96Type, u96HighBit);
        const VariableId unsigned96Ones =
            addBitConstant(builder, u96Type, u96Ones);
        const VariableId signed96Negative =
            addBitConstant(builder, s96Type, s96Negative);
        const VariableId signed96Positive =
            addBitConstant(builder, s96Type, s96Positive);
        const VariableId unsigned130Base =
            addBitConstant(builder, u130Type, u130Base);
        const VariableId unsigned130Ones =
            addBitConstant(builder, u130Type, u130Ones);

        ProgramInterface interface;
        std::vector<std::pair<std::string, VariableId>> outputs;
        const auto addOutput = [&](TypeId type, std::string_view name) {
            const StringId nameId = builder.addString(name);
            const VariableId variable = builder.addVariable(type, builder.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = nameId,
                .direction = PortDirection::Output,
                .output = variable,
            });
            outputs.emplace_back(std::string(name), variable);
            return variable;
        };
        const auto addPure = [&](Opcode opcode,
                                 VariableId result,
                                 std::initializer_list<VariableId> operands) {
            const std::array<VariableId, 1> results = {result};
            return builder.addInstruction(
                opcode,
                results,
                std::span<const VariableId>(operands.begin(), operands.size()));
        };

        addPure(Opcode::Add, addOutput(u96Type, "unsigned_add"),
                {unsigned96A, unsigned65B});
        addPure(Opcode::Sub, addOutput(u96Type, "unsigned_sub"),
                {unsigned96A, unsigned65B});
        addPure(Opcode::Add, addOutput(s96Type, "signed_add"),
                {signed96Negative, signed65Positive});
        addPure(Opcode::Sub, addOutput(s96Type, "signed_sub"),
                {signed96Negative, signed65Negative});
        addPure(Opcode::Add, addOutput(u96Type, "mixed_add"),
                {signed65Negative, unsigned96A});
        addPure(Opcode::And, addOutput(u96Type, "mixed_and"),
                {signed65Negative, unsigned96Ones});
        addPure(Opcode::Xor, addOutput(u96Type, "unsigned_xor"),
                {unsigned96A, unsigned65A});
        addPure(Opcode::Not, addOutput(s96Type, "signed_not"),
                {signed96Negative});
        addPure(Opcode::Lt, addOutput(u1Type, "signed_lt"),
                {signed65Negative, signed96Positive});
        addPure(Opcode::Lt, addOutput(u1Type, "mixed_lt"),
                {signed65Negative, unsigned96A});
        addPure(Opcode::Ge, addOutput(u1Type, "unsigned_ge"),
                {unsigned96A, unsigned65A});
        addPure(Opcode::Mux, addOutput(u130Type, "mixed_mux"),
                {condition, signed65Negative, unsigned96A});
        addPure(Opcode::Shl, addOutput(u65Type, "shift_left_65"),
                {unsigned65A, amountFour});
        addPure(Opcode::LogicalShr, addOutput(u130Type, "logical_shift_130"),
                {unsigned130Base, amountSixtySeven});
        addPure(Opcode::ArithmeticShr,
                addOutput(u96Type, "unsigned_arithmetic_shift"),
                {unsigned96HighBit, amountFour});
        addPure(Opcode::ArithmeticShr,
                addOutput(s96Type, "signed_arithmetic_shift"),
                {signed96Negative, amountFour});
        addPure(Opcode::ReduceAnd, addOutput(u1Type, "reduce_and"),
                {unsigned130Ones});
        addPure(Opcode::ReduceOr, addOutput(u1Type, "reduce_or"),
                {unsigned130Base});
        addPure(Opcode::ReduceXor, addOutput(u1Type, "reduce_xor"),
                {unsigned130Base});
        addPure(Opcode::Mul, addOutput(u130Type, "mixed_mul"),
                {signed65Negative, unsigned65B});
        addPure(Opcode::Concat, addOutput(u130Type, "concat_130"),
                {unsigned65A, unsigned65B});
        addPure(Opcode::Replicate, addOutput(u130Type, "replicate_130"),
                {unsigned65B});
        const InstructionId staticSlice =
            addPure(Opcode::SliceStatic, addOutput(u96Type, "slice_static_96"),
                    {unsigned130Base});
        builder.setSliceStaticAttributes(staticSlice, 17);
        addPure(Opcode::SliceDynamic, addOutput(u65Type, "slice_dynamic_65"),
                {unsigned130Base, dynamicIndex});
        addPure(Opcode::SliceArray, addOutput(u65Type, "slice_array_65"),
                {unsigned130Base, arrayIndex});

        LinearProgram program = builder.finish();
        SchedulingFacts facts;
        facts.variableRoles.assign(program.view().variableCount(), VariableRole::None);
        for (const auto &[name, variable] : outputs)
        {
            (void)name;
            facts.variableRoles[variable.value] = VariableRole::ExternalOutput;
        }
        facts.instructionEffects.assign(program.view().instructionCount(),
                                        InstructionEffect::Pure);
        return WidePureFixture{
            .artifact = LinearProgramArtifact{
                .program = std::move(program),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            },
            .outputs = std::move(outputs),
        };
    }

    int testWidePureOperations(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        WidePureFixture fixture = makeWidePureProgram();
        BaselineActivityScheduleStage scheduler;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = scheduler.schedule(
            std::move(fixture.artifact), ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("failed to build the wide AM emitter fixture");
        }

        Interpreter reference(*model);
        if (!reference.ready() || !reference.eval().success())
        {
            return fail("AM reference executor failed the wide pure-op fixture");
        }

        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "WideTop",
                .maxOutputFileBytes = 4 * 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the wide pure-op model");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << "#include \"grhsim_WideTop.hpp\"\n"
                   "#include <array>\n"
                   "#include <cstdint>\n\n"
                   "int main()\n"
                   "{\n"
                   "    GrhSIM_WideTop model;\n"
                   "    model.init();\n"
                   "    model.eval();\n";
        const ProgramView program = model->program.view();
        int returnCode = 1;
        for (const auto &[name, variable] : fixture.outputs)
        {
            const Type &type = program.type(program.variable(variable).type);
            const std::span<const uint64_t> expected = reference.value(variable).words();
            if (type.bitWidth <= 64)
            {
                harness << "    if (static_cast<std::uint64_t>(model." << name
                        << ") != UINT64_C(" << expected.front() << ")) return "
                        << returnCode << ";\n";
            }
            else
            {
                harness << "    if (model." << name
                        << " != std::array<std::uint64_t, " << expected.size() << ">{";
                for (std::size_t word = 0; word < expected.size(); ++word)
                {
                    if (word != 0)
                    {
                        harness << ", ";
                    }
                    harness << "UINT64_C(" << expected[word] << ")";
                }
                harness << "}) return " << returnCode << ";\n";
            }
            ++returnCode;
        }
        harness << "    return 0;\n}\n";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the wide generated model harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated wide AM model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_WideTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated wide AM model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated wide AM model disagreed with the Interpreter");
        }
        return 0;
    }

    struct MemoryEmitterFixture
    {
        ExecutableModel model;
        VariableId fillEnable;
        VariableId writeEnable;
        VariableId address;
        VariableId writeMask;
        VariableId writeData;
        VariableId fillData;
        VariableId readData;
        VariableId memoryChanged;
    };

    MemoryEmitterFixture makeMemoryEmitterFixture()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId u32Type = linear.addType(Type::bitVector(32));
        const TypeId memoryType = linear.addType(Type::array(4, 8));

        ProgramInterface interface;
        const auto addInput = [&](TypeId type, std::string_view name) {
            const VariableId variable = linear.addVariable(type, linear.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = linear.addString(name),
                .direction = PortDirection::Input,
                .input = variable,
            });
            return variable;
        };
        const auto addOutput = [&](TypeId type, std::string_view name) {
            const VariableId variable = linear.addVariable(type, linear.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = linear.addString(name),
                .direction = PortDirection::Output,
                .output = variable,
            });
            return variable;
        };
        const auto addInstruction = [&](Opcode opcode,
                                        std::initializer_list<VariableId> results,
                                        std::initializer_list<VariableId> operands) {
            return linear.addInstruction(
                opcode,
                std::span<const VariableId>(results.begin(), results.size()),
                std::span<const VariableId>(operands.begin(), operands.size()));
        };

        const VariableId fillEnable = addInput(u1Type, "fill_enable");
        const VariableId writeEnable = addInput(u1Type, "write_enable");
        const VariableId address = addInput(u8Type, "address");
        const VariableId writeMask = addInput(u8Type, "write_mask");
        const VariableId writeData = addInput(u8Type, "write_data");
        const VariableId fillData = addInput(u32Type, "fill_data");
        const VariableId readData = addOutput(u8Type, "read_data");
        const VariableId memoryChanged = addOutput(u1Type, "memory_changed");

        const std::array<uint64_t, 1> oneWords = {1};
        const VariableId event = addBitConstant(linear, u1Type, oneWords);
        const VariableId memory = linear.addVariable(memoryType, linear.zeroInit());
        const VariableId memoryOld = linear.addVariable(memoryType, linear.undefInit());
        const VariableId changedEvent = linear.addVariable(u1Type, linear.zeroInit());

        const InstructionId fill = addInstruction(
            Opcode::MemoryFill, {}, {fillEnable, fillData, memory, event});
        const InstructionId write = addInstruction(
            Opcode::MemoryWrite,
            {},
            {writeEnable, address, writeMask, writeData, memory, event});
        const InstructionId read = addInstruction(
            Opcode::MemoryRead, {readData}, {memory, address});
        const InstructionId changed = addInstruction(
            Opcode::ChangedAny, {changedEvent}, {memory, memoryOld});
        const InstructionId exposeChanged = addInstruction(
            Opcode::Assign, {memoryChanged}, {changedEvent});

        ScheduledProgramBuilder scheduled(linear.finish());
        std::vector<InstructionId> entry;
        const std::array<VariableId, 6> inputs = {
            fillEnable, writeEnable, address, writeMask, writeData, fillData,
        };
        for (VariableId input : inputs)
        {
            const TypeId type = scheduled.view().variable(input).type;
            const VariableId oldValue = scheduled.addVariable(type, scheduled.undefInit());
            const VariableId inputChanged =
                scheduled.addVariable(u1Type, scheduled.zeroInit());
            const std::array<VariableId, 1> changedResults = {inputChanged};
            const std::array<VariableId, 2> changedOperands = {input, oldValue};
            const InstructionId detect = scheduled.addInstruction(
                Opcode::ChangedAny, changedResults, changedOperands);
            const std::array<VariableId, 1> activateOperands = {inputChanged};
            const InstructionId activate = scheduled.addInstruction(
                Opcode::ActForward, {}, activateOperands);
            const std::array<BlockId, 1> targets = {BlockId{1}};
            scheduled.setActivationTargets(activate, targets);
            entry.push_back(detect);
            entry.push_back(activate);
        }
        scheduled.addBlock(entry);
        const std::array<InstructionId, 5> body = {
            fill, write, read, changed, exposeChanged,
        };
        scheduled.addBlock(body);

        return MemoryEmitterFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = std::move(interface),
            },
            .fillEnable = fillEnable,
            .writeEnable = writeEnable,
            .address = address,
            .writeMask = writeMask,
            .writeData = writeData,
            .fillData = fillData,
            .readData = readData,
            .memoryChanged = memoryChanged,
        };
    }

    struct MemoryTransaction
    {
        uint64_t fillEnable;
        uint64_t writeEnable;
        uint64_t address;
        uint64_t writeMask;
        uint64_t writeData;
        uint64_t fillData;
        uint64_t expectedRead;
        uint64_t expectedChanged;
    };

    int testMemoryOperations(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        MemoryEmitterFixture fixture = makeMemoryEmitterFixture();
        Interpreter reference(fixture.model);
        if (!reference.ready())
        {
            return fail("failed to build the AM memory reference fixture");
        }

        const std::array<MemoryTransaction, 6> transactions = {
            MemoryTransaction{1, 0, 0, 0x00, 0x00, 0x44332211, 0x11, 1},
            MemoryTransaction{0, 0, 2, 0x00, 0x00, 0x44332211, 0x33, 0},
            MemoryTransaction{0, 1, 1, 0x0f, 0xab, 0x44332211, 0x2b, 1},
            MemoryTransaction{0, 1, 1, 0x00, 0xcd, 0x44332211, 0x2b, 0},
            MemoryTransaction{0, 1, 4, 0xff, 0x5a, 0x44332211, 0x00, 0},
            MemoryTransaction{0, 0, 1, 0xff, 0x5a, 0x44332211, 0x2b, 0},
        };
        std::vector<std::array<uint64_t, 2>> oracle;
        oracle.reserve(transactions.size());
        const auto writeValue = [&](VariableId variable, uint32_t width, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return reference
                .write(variable,
                       InterpreterValue::bitVector(width, Signedness::Unsigned, words))
                .success();
        };
        for (const MemoryTransaction &transaction : transactions)
        {
            if (!writeValue(fixture.fillEnable, 1, transaction.fillEnable) ||
                !writeValue(fixture.writeEnable, 1, transaction.writeEnable) ||
                !writeValue(fixture.address, 8, transaction.address) ||
                !writeValue(fixture.writeMask, 8, transaction.writeMask) ||
                !writeValue(fixture.writeData, 8, transaction.writeData) ||
                !writeValue(fixture.fillData, 32, transaction.fillData) ||
                !reference.eval().success())
            {
                return fail("AM Interpreter failed a memory transaction");
            }
            const uint64_t read = reference.value(fixture.readData).lowWord();
            const uint64_t changed = reference.value(fixture.memoryChanged).lowWord();
            if (read != transaction.expectedRead ||
                changed != transaction.expectedChanged)
            {
                return fail("AM Interpreter disagreed with the memory transaction oracle");
            }
            oracle.push_back({read, changed});
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            fixture.model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "MemoryTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the memory model");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << "#include \"grhsim_MemoryTop.hpp\"\n"
                   "#include <cstdint>\n\n"
                   "int main()\n"
                   "{\n"
                   "    GrhSIM_MemoryTop model;\n"
                   "    model.init();\n";
        int returnCode = 1;
        for (std::size_t index = 0; index < transactions.size(); ++index)
        {
            const MemoryTransaction &transaction = transactions[index];
            harness << "    model.fill_enable = " << transaction.fillEnable << ";\n"
                    << "    model.write_enable = " << transaction.writeEnable << ";\n"
                    << "    model.address = " << transaction.address << ";\n"
                    << "    model.write_mask = " << transaction.writeMask << ";\n"
                    << "    model.write_data = " << transaction.writeData << ";\n"
                    << "    model.fill_data = UINT32_C(" << transaction.fillData << ");\n"
                    << "    model.eval();\n"
                    << "    if (static_cast<std::uint64_t>(model.read_data) != UINT64_C("
                    << oracle[index][0] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.memory_changed) != UINT64_C("
                    << oracle[index][1] << ")) return " << returnCode++ << ";\n";
        }
        harness << "    return 0;\n}\n";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the generated memory model harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated memory AM model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_MemoryTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated memory AM model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated memory AM model disagreed with the Interpreter");
        }
        return 0;
    }

    struct ArrayInitFixture
    {
        LinearProgramArtifact artifact;
        std::array<VariableId, 5> outputs;
        VariableId narrowOutput;
    };

    ArrayInitFixture makeArrayInitFixture()
    {
        LinearProgramBuilder builder;
        const TypeId indexType = builder.addType(Type::bitVector(8));
        const TypeId elementType = builder.addType(Type::bitVector(70));
        const TypeId arrayType = builder.addType(Type::array(5, 70));
        const TypeId narrowElementType = builder.addType(Type::bitVector(64));
        const TypeId narrowArrayType = builder.addType(Type::array(64, 64));

        const std::array<uint64_t, 2> literalWords = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0x15),
        };
        const LiteralId literal = builder.addBitLiteral(elementType, literalWords);
        const std::array<InitAction, 2> actions = {
            InitAction{
                .kind = InitActionKind::Fill,
                .expression = InitExpr{
                    .kind = InitExprKind::Literal,
                    .literal = literal,
                },
                .start = 1,
                .count = 4,
            },
            InitAction{
                .kind = InitActionKind::Fill,
                .expression = InitExpr{
                    .kind = InitExprKind::RandomSeeded,
                    .seed = UINT64_C(0x123456789abcdef0),
                },
                .start = 2,
                .count = 2,
            },
        };
        const VariableId memory =
            builder.addVariable(arrayType, builder.addActionsInit(actions));
        std::vector<InitAction> adjacentLiteralActions;
        adjacentLiteralActions.reserve(64);
        for (uint64_t element = 0; element < 64; ++element)
        {
            const std::array<uint64_t, 1> narrowWords = {
                UINT64_C(0x5a5a5a5a5a5a5a5a),
            };
            const LiteralId distinctNarrowLiteral =
                builder.addBitLiteral(narrowElementType, narrowWords);
            adjacentLiteralActions.push_back(InitAction{
                .kind = InitActionKind::Fill,
                .expression = InitExpr{
                    .kind = InitExprKind::Literal,
                    .literal = distinctNarrowLiteral,
                },
                .start = element,
                .count = 1,
            });
        }
        const VariableId narrowMemory = builder.addVariable(
            narrowArrayType, builder.addActionsInit(adjacentLiteralActions));

        ProgramInterface interface;
        std::array<VariableId, 5> outputs;
        for (uint32_t index = 0; index < outputs.size(); ++index)
        {
            const std::array<uint64_t, 1> indexWords = {index};
            const VariableId address = addBitConstant(builder, indexType, indexWords);
            const VariableId output = builder.addVariable(elementType, builder.zeroInit());
            outputs[index] = output;
            interface.ports.push_back(PortBinding{
                .name = builder.addString("array_element_" + std::to_string(index)),
                .direction = PortDirection::Output,
                .output = output,
            });
            const std::array<VariableId, 1> results = {output};
            const std::array<VariableId, 2> operands = {memory, address};
            builder.addInstruction(Opcode::MemoryRead, results, operands);
        }
        const std::array<uint64_t, 1> lastIndexWords = {63};
        const VariableId lastIndex = addBitConstant(builder, indexType, lastIndexWords);
        const VariableId narrowOutput =
            builder.addVariable(narrowElementType, builder.zeroInit());
        interface.ports.push_back(PortBinding{
            .name = builder.addString("narrow_element_63"),
            .direction = PortDirection::Output,
            .output = narrowOutput,
        });
        const std::array<VariableId, 1> narrowResults = {narrowOutput};
        const std::array<VariableId, 2> narrowOperands = {narrowMemory, lastIndex};
        builder.addInstruction(Opcode::MemoryRead, narrowResults, narrowOperands);

        LinearProgram program = builder.finish();
        SchedulingFacts facts;
        facts.variableRoles.assign(program.view().variableCount(), VariableRole::None);
        for (VariableId output : outputs)
        {
            facts.variableRoles[output.value] = VariableRole::ExternalOutput;
        }
        facts.variableRoles[narrowOutput.value] = VariableRole::ExternalOutput;
        facts.instructionEffects.assign(program.view().instructionCount(),
                                        InstructionEffect::Pure);
        return ArrayInitFixture{
            .artifact = LinearProgramArtifact{
                .program = std::move(program),
                .interface = std::move(interface),
                .schedulingFacts = std::move(facts),
            },
            .outputs = outputs,
            .narrowOutput = narrowOutput,
        };
    }

    int testArrayFillInitialization(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ArrayInitFixture fixture = makeArrayInitFixture();
        BaselineActivityScheduleStage scheduler;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = scheduler.schedule(
            std::move(fixture.artifact), ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("failed to build the Array Fill initialization fixture");
        }

        Interpreter reference(*model, nullptr,
                              InterpreterOptions{.randomSeed = UINT64_C(0xfeedface)});
        if (!reference.ready() || !reference.eval().success())
        {
            return fail("AM Interpreter failed the Array Fill initialization fixture");
        }
        std::array<std::array<uint64_t, 2>, 5> expected;
        for (std::size_t index = 0; index < fixture.outputs.size(); ++index)
        {
            const std::span<const uint64_t> words = reference.value(fixture.outputs[index]).words();
            if (words.size() != expected[index].size())
            {
                return fail("AM Interpreter returned an invalid Array Fill element width");
            }
            std::copy(words.begin(), words.end(), expected[index].begin());
        }
        const std::array<uint64_t, 2> zero = {0, 0};
        const std::array<uint64_t, 2> literal = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0x15),
        };
        if (expected[0] != zero || expected[1] != literal || expected[4] != literal ||
            expected[2] == expected[3] || expected[2] == zero || expected[3] == zero)
        {
            return fail("AM Interpreter disagreed with the Array Fill initialization oracle");
        }
        const uint64_t expectedNarrow = reference.value(fixture.narrowOutput).lowWord();
        if (expectedNarrow != UINT64_C(0x5a5a5a5a5a5a5a5a))
        {
            return fail("AM Interpreter disagreed with the narrow Array Fill oracle");
        }

        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "ArrayInitTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the Array Fill initialization model");
        }
        const std::optional<std::string> runtimeText =
            readTextFile(outputDirectory / "grhsim_ArrayInitTop_runtime.cpp");
        if (!runtimeText || countOccurrences(*runtimeText, "std::fill_n(") != 1 ||
            runtimeText->find(", 64, (UINT64_C(") == std::string::npos ||
            countOccurrences(*runtimeText, "for (std::size_t initElement_") != 2)
        {
            return fail("AM C++ emitter did not coalesce adjacent literal Array Fill actions");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << "#include \"grhsim_ArrayInitTop.hpp\"\n"
                   "#include <array>\n"
                   "#include <cstdint>\n\n"
                   "int main()\n"
                   "{\n"
                   "    GrhSIM_ArrayInitTop model;\n";
        int returnCode = 1;
        for (uint64_t randomSeed : {UINT64_C(1), UINT64_C(0xdeadbeef)})
        {
            harness << "    model.set_random_seed(UINT64_C(" << randomSeed << "));\n"
                    << "    model.init();\n"
                    << "    model.eval();\n";
            for (std::size_t index = 0; index < expected.size(); ++index)
            {
                harness << "    if (model.array_element_" << index
                        << " != std::array<std::uint64_t, 2>{UINT64_C("
                        << expected[index][0] << "), UINT64_C(" << expected[index][1]
                        << ")}) return " << returnCode++ << ";\n";
            }
            harness << "    if (model.narrow_element_63 != UINT64_C(" << expectedNarrow
                    << ")) return " << returnCode++ << ";\n";
        }
        harness << "    return 0;\n}\n";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the Array Fill initialization harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated Array Fill initialization model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_ArrayInitTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated Array Fill initialization harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated Array Fill initialization model disagreed with the Interpreter");
        }
        return 0;
    }

    struct PackedActivityFixture
    {
        ExecutableModel model;
        VariableId input;
        VariableId forwardOutput;
        VariableId backwardOutput;
        VariableId inputChanged;
        VariableId backwardChanged;
    };

    PackedActivityFixture makePackedActivityFixture()
    {
        constexpr uint32_t kBoundaryBlock = 17;
        constexpr uint32_t kForwardBlock = 64;
        constexpr uint32_t kBackwardBlock = 65;
        constexpr uint32_t kFinalBlock = 130;

        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const StringId inputName = linear.addString("activity_input");
        const StringId forwardOutputName = linear.addString("forward_output");
        const StringId backwardOutputName = linear.addString("backward_output");
        const VariableId input = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId inputOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId inputChanged = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId forwardStage = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId forwardOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId backwardChanged = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId forwardOutput = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId backwardOutput = linear.addVariable(u1Type, linear.zeroInit());

        const auto addInstruction = [&](Opcode opcode,
                                        std::initializer_list<VariableId> results,
                                        std::initializer_list<VariableId> operands) {
            return linear.addInstruction(
                opcode,
                std::span<const VariableId>(results.begin(), results.size()),
                std::span<const VariableId>(operands.begin(), operands.size()));
        };
        const InstructionId detectInput =
            addInstruction(Opcode::ChangedAny, {inputChanged}, {input, inputOld});
        const InstructionId assignForwardStage =
            addInstruction(Opcode::Assign, {forwardStage}, {input});
        const InstructionId assignForwardOutput =
            addInstruction(Opcode::Assign, {forwardOutput}, {forwardStage});
        const InstructionId detectBackward = addInstruction(
            Opcode::ChangedAny, {backwardChanged}, {forwardStage, forwardOld});
        const InstructionId assignBackwardOutput =
            addInstruction(Opcode::Assign, {backwardOutput}, {forwardStage});

        ScheduledProgramBuilder scheduled(linear.finish());
        const auto addScheduledInstruction = [&](Opcode opcode,
                                                 std::initializer_list<VariableId> results,
                                                 std::initializer_list<VariableId> operands) {
            return scheduled.addInstruction(
                opcode,
                std::span<const VariableId>(results.begin(), results.size()),
                std::span<const VariableId>(operands.begin(), operands.size()));
        };
        const InstructionId activateForward =
            addScheduledInstruction(Opcode::ActForward, {}, {inputChanged});
        const InstructionId activateFinal =
            addScheduledInstruction(Opcode::ActForward, {}, {forwardStage});
        const InstructionId activateBackward =
            addScheduledInstruction(Opcode::ActBackward, {}, {backwardChanged});
        const std::array<BlockId, 2> entryTargets = {
            BlockId{kBoundaryBlock},
            BlockId{kForwardBlock},
        };
        const std::array<BlockId, 1> finalTargets = {BlockId{kFinalBlock}};
        const std::array<BlockId, 1> backwardTargets = {BlockId{kBackwardBlock}};
        scheduled.setActivationTargets(activateForward, entryTargets);
        scheduled.setActivationTargets(activateFinal, finalTargets);
        scheduled.setActivationTargets(activateBackward, backwardTargets);

        for (uint32_t block = 0; block <= kFinalBlock; ++block)
        {
            if (block == 0)
            {
                const std::array<InstructionId, 2> entry = {
                    detectInput,
                    activateForward,
                };
                scheduled.addBlock(entry);
            }
            else if (block == kForwardBlock)
            {
                const std::array<InstructionId, 2> forward = {
                    assignForwardStage,
                    activateFinal,
                };
                scheduled.addBlock(forward);
            }
            else if (block == kBackwardBlock)
            {
                const std::array<InstructionId, 1> backward = {
                    assignBackwardOutput,
                };
                scheduled.addBlock(backward);
            }
            else if (block == kFinalBlock)
            {
                const std::array<InstructionId, 3> final = {
                    assignForwardOutput,
                    detectBackward,
                    activateBackward,
                };
                scheduled.addBlock(final);
            }
            else
            {
                scheduled.addBlock(std::span<const InstructionId>{});
            }
        }

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = inputName,
                .direction = PortDirection::Input,
                .input = input,
            },
            PortBinding{
                .name = forwardOutputName,
                .direction = PortDirection::Output,
                .output = forwardOutput,
            },
            PortBinding{
                .name = backwardOutputName,
                .direction = PortDirection::Output,
                .output = backwardOutput,
            },
        };
        return PackedActivityFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = std::move(interface),
            },
            .input = input,
            .forwardOutput = forwardOutput,
            .backwardOutput = backwardOutput,
            .inputChanged = inputChanged,
            .backwardChanged = backwardChanged,
        };
    }

    int testPackedActivityRuntime(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        PackedActivityFixture fixture = makePackedActivityFixture();
        const ValidationResult validation =
            validate(fixture.model, ValidationOptions{.level = ValidationLevel::Semantic});
        if (!validation.success())
        {
            return fail("packed activity fixture failed AM semantic validation: " +
                        validation.errors.front());
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        wolvrix::lib::diag::Diagnostics invalidDiagnostics;
        const GrhSimAmCppResult invalidResult = emitter.emit(
            fixture.model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory / "invalid-block-count",
                .modelName = "InvalidBlockCountTop",
                .maxOutputFileBytes = 1024 * 1024,
                .attributes = {{"blocksPerSource", "0"}},
            },
            invalidDiagnostics);
        if (invalidResult.success || !invalidDiagnostics.hasError() ||
            !invalidResult.artifacts.empty())
        {
            return fail("AM C++ emitter accepted an invalid blocksPerSource attribute");
        }

        const GrhSimAmCppResult emitResult = emitter.emit(
            fixture.model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "PackedActivityTop",
                .maxOutputFileBytes = 1024 * 1024,
                .attributes = {{"blocksPerSource", "17"}},
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the packed activity model");
        }

        const std::optional<std::string> headerText =
            readTextFile(outputDirectory / "grhsim_PackedActivityTop.hpp");
        const std::optional<std::string> runtimeText =
            readTextFile(outputDirectory / "grhsim_PackedActivityTop_runtime.cpp");
        if (!headerText || !runtimeText)
        {
            return fail("packed activity model did not emit its header and runtime source");
        }
        if (emitResult.artifacts.size() != 12 ||
            !hasExactMakefileSources(
                emitResult,
                outputDirectory,
                {"grhsim_PackedActivityTop_runtime.cpp",
                 "grhsim_PackedActivityTop_blocks_0.cpp",
                 "grhsim_PackedActivityTop_blocks_1.cpp",
                 "grhsim_PackedActivityTop_blocks_2.cpp",
                 "grhsim_PackedActivityTop_blocks_3.cpp",
                 "grhsim_PackedActivityTop_blocks_4.cpp",
                 "grhsim_PackedActivityTop_blocks_5.cpp",
                 "grhsim_PackedActivityTop_blocks_6.cpp",
                 "grhsim_PackedActivityTop_blocks_7.cpp"}))
        {
            return fail("packed activity model emitted an invalid shard manifest");
        }
        const std::optional<std::string> firstShardText =
            readTextFile(outputDirectory / "grhsim_PackedActivityTop_blocks_0.cpp");
        const std::optional<std::string> secondShardText =
            readTextFile(outputDirectory / "grhsim_PackedActivityTop_blocks_1.cpp");
        const std::optional<std::string> lastShardText =
            readTextFile(outputDirectory / "grhsim_PackedActivityTop_blocks_7.cpp");
        if (!firstShardText || !secondShardText || !lastShardText ||
            firstShardText->find("    case 0: {") == std::string::npos ||
            firstShardText->find("    case 16: {") == std::string::npos ||
            firstShardText->find("    case 17: {") != std::string::npos ||
            secondShardText->find("    case 17: {") == std::string::npos ||
            secondShardText->find("    case 33: {") == std::string::npos ||
            secondShardText->find("    case 16: {") != std::string::npos ||
            secondShardText->find("    case 34: {") != std::string::npos ||
            lastShardText->find("    case 119: {") == std::string::npos ||
            lastShardText->find("    case 130: {") == std::string::npos ||
            lastShardText->find("    case 118: {") != std::string::npos ||
            lastShardText->find("    case 131: {") != std::string::npos)
        {
            return fail("packed activity model emitted an invalid shard Block range");
        }
        std::string generatedSourceText = *runtimeText;
        for (const std::string &artifact : emitResult.artifacts)
        {
            const std::filesystem::path sourcePath(artifact);
            if (sourcePath.extension() != ".cpp" ||
                sourcePath.filename() == "grhsim_PackedActivityTop_runtime.cpp")
            {
                continue;
            }
            const std::optional<std::string> sourceText = readTextFile(sourcePath);
            if (!sourceText)
            {
                return fail("packed activity model did not emit a readable block source");
            }
            generatedSourceText += *sourceText;
        }

        const std::size_t blockCount = fixture.model.program.blockCount();
        const std::size_t activityWordCount = (blockCount + 63U) / 64U;
        const std::size_t activitySummaryWordCount = (activityWordCount + 63U) / 64U;
        const std::string oldActiveArray =
            "std::array<bool, " + std::to_string(blockCount) + "> active_{};";
        const std::string oldNextActiveArray =
            "std::array<bool, " + std::to_string(blockCount) + "> nextActive_{};";
        if (headerText->find(oldActiveArray) != std::string::npos ||
            headerText->find(oldNextActiveArray) != std::string::npos ||
            headerText->find("kActivityWordCount = " +
                             std::to_string(activityWordCount)) == std::string::npos ||
            headerText->find("kActivitySummaryWordCount = " +
                             std::to_string(activitySummaryWordCount)) == std::string::npos ||
            headerText->find("std::array<std::uint64_t, kActivityWordCount> activeWords_{};") ==
                std::string::npos ||
            headerText->find("std::array<std::uint64_t, kActivityWordCount> nextActiveWords_{};") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kActivitySummaryWordCount> activeSummary_{};") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kActivitySummaryWordCount> nextActiveSummary_{};") ==
                std::string::npos ||
            headerText->find("dirtyChangedResults_") == std::string::npos ||
            headerText->find("dirtyChangedBits_") == std::string::npos)
        {
            return fail("generated activity runtime did not use packed activity and dirty-change tracking");
        }
        if (runtimeText->find("activate_forward(") == std::string::npos ||
            runtimeText->find("activate_backward(") == std::string::npos ||
            countOccurrences(generatedSourceText, "set_changed_result(") < 2 ||
            runtimeText->find("dirtyChangedResults_") == std::string::npos ||
            runtimeText->find("dirtyChangedBits_") == std::string::npos ||
            runtimeText->find("if (block >= kBlockCount)") == std::string::npos ||
            runtimeText->find("switch (block / 17U)") == std::string::npos ||
            runtimeText->find("case 0: execute_blocks_0(block); return;") ==
                std::string::npos ||
            runtimeText->find("if (block < 17)") != std::string::npos ||
            runtimeText->find("std::none_of(nextActive_.begin()") != std::string::npos ||
            runtimeText->find("for (std::size_t block = 1; block < active_.size();") !=
                std::string::npos)
        {
            return fail("generated activity runtime retained dense activation or changed-result paths");
        }
        for (std::size_t source = 0; source < 8; ++source)
        {
            const std::string dispatch = "case " + std::to_string(source) +
                                         ": execute_blocks_" + std::to_string(source) +
                                         "(block); return;";
            if (runtimeText->find(dispatch) == std::string::npos)
            {
                return fail("generated activity runtime omitted a shard dispatch case");
            }
        }
        const std::string legacyInputClear =
            "values_[" + std::to_string(fixture.inputChanged.value) + "] = 0;";
        const std::string legacyBackwardClear =
            "values_[" + std::to_string(fixture.backwardChanged.value) + "] = 0;";
        if (runtimeText->find(legacyInputClear) != std::string::npos ||
            runtimeText->find(legacyBackwardClear) != std::string::npos)
        {
            return fail("generated activity runtime statically clears every changed result");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_PackedActivityTop.hpp"
int main()
{
    GrhSIM_PackedActivityTop model;
    model.init();
    model.activity_input = 0;
    model.eval();
    if (model.forward_output != 0 || model.backward_output != 0)
        return 1;
    model.activity_input = 1;
    model.eval();
    if (model.forward_output != 1 || model.backward_output != 1)
        return 2;
    model.eval();
    if (model.forward_output != 1 || model.backward_output != 1)
        return 3;
    model.activity_input = 0;
    model.eval();
    if (model.forward_output != 1 || model.backward_output != 1)
        return 4;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the packed activity model harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated packed activity model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_PackedActivityTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated packed activity model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated packed activity model violated activation semantics");
        }
        return 0;
    }

    ExecutableModel makePhasedCommitModel()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId memoryType = linear.addType(Type::array(1, 8));

        ProgramInterface interface;
        const auto addInput = [&](TypeId type, std::string_view name) {
            const VariableId variable = linear.addVariable(type, linear.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = linear.addString(name),
                .direction = PortDirection::Input,
                .input = variable,
            });
            return variable;
        };
        const auto addOutput = [&](TypeId type, std::string_view name) {
            const VariableId variable = linear.addVariable(type, linear.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = linear.addString(name),
                .direction = PortDirection::Output,
                .output = variable,
            });
            return variable;
        };
        const auto addInstruction = [&](Opcode opcode,
                                        std::initializer_list<VariableId> results,
                                        std::initializer_list<VariableId> operands) {
            return linear.addInstruction(
                opcode,
                std::span<const VariableId>(results.begin(), results.size()),
                std::span<const VariableId>(operands.begin(), operands.size()));
        };

        const VariableId clock = addInput(u1Type, "clock");
        const VariableId payload = addInput(u8Type, "payload");
        const VariableId state = addOutput(u8Type, "state");
        const VariableId sampledState = addOutput(u8Type, "sampled_state");
        const VariableId commitCount = addOutput(u8Type, "commit_count");
        const VariableId captureCommitCount =
            addOutput(u8Type, "capture_commit_count");
        const VariableId memoryWriteValue =
            addOutput(u8Type, "memory_write_value");
        const VariableId memoryFillValue =
            addOutput(u8Type, "memory_fill_value");
        const VariableId commitEvent = addOutput(u1Type, "commit_event");
        const VariableId entryOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId entryEvent = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId payloadOld = linear.addVariable(u8Type, linear.undefInit());
        const VariableId payloadEvent = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId clockOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId posedge = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId capturedPayload =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId capturedOld = linear.addVariable(u8Type, linear.undefInit());
        const VariableId capturedChanged =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId lateData = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId lateOld = linear.addVariable(u8Type, linear.undefInit());
        const VariableId lateChanged = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId nextState = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId nextCommitCount =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId nextCaptureCommitCount =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId stateOld = linear.addVariable(u8Type, linear.undefInit());
        const VariableId stateChanged = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId writtenMemory =
            linear.addVariable(memoryType, linear.zeroInit());
        const VariableId filledMemory =
            linear.addVariable(memoryType, linear.zeroInit());

        const std::array<uint64_t, 1> oneWords = {1};
        const std::array<uint64_t, 1> zeroWords = {0};
        const std::array<uint64_t, 1> maskWords = {0xff};
        const VariableId one = addBitConstant(linear, u1Type, oneWords);
        const VariableId zero = addBitConstant(linear, u8Type, zeroWords);
        const VariableId oneValue = addBitConstant(linear, u8Type, oneWords);
        const VariableId mask = addBitConstant(linear, u8Type, maskWords);

        const InstructionId sampleCommitEvent =
            addInstruction(Opcode::Assign, {commitEvent}, {posedge});
        const InstructionId watchClock =
            addInstruction(Opcode::ChangedAny, {entryEvent}, {clock, entryOld});
        const InstructionId watchPayload =
            addInstruction(Opcode::ChangedAny, {payloadEvent}, {payload, payloadOld});
        const InstructionId detectPosedge =
            addInstruction(Opcode::ChangedPos, {posedge}, {clock, clockOld});
        const InstructionId updateLateData =
            addInstruction(Opcode::Assign, {lateData}, {capturedPayload});
        const InstructionId detectLateData =
            addInstruction(Opcode::ChangedAny, {lateChanged}, {lateData, lateOld});
        const InstructionId addState =
            addInstruction(Opcode::Add, {nextState}, {state, lateData});
        const InstructionId addCommitCount =
            addInstruction(Opcode::Add, {nextCommitCount}, {commitCount, oneValue});
        const InstructionId addCaptureCommitCount = addInstruction(
            Opcode::Add,
            {nextCaptureCommitCount},
            {captureCommitCount, oneValue});
        const InstructionId commit = addInstruction(
            Opcode::RegisterWrite, {}, {one, mask, nextState, state, posedge});
        const InstructionId countCommit = addInstruction(
            Opcode::RegisterWrite,
            {},
            {one, mask, nextCommitCount, commitCount, posedge});
        const InstructionId capturePayload = addInstruction(
            Opcode::RegisterWrite,
            {},
            {one, mask, payload, capturedPayload, posedge});
        const InstructionId countCaptureCommit = addInstruction(
            Opcode::RegisterWrite,
            {},
            {one, mask, nextCaptureCommitCount, captureCommitCount, posedge});
        const InstructionId writeMemory = addInstruction(
            Opcode::MemoryWrite,
            {},
            {one, zero, mask, nextCaptureCommitCount, writtenMemory, posedge});
        const InstructionId fillMemory = addInstruction(
            Opcode::MemoryFill,
            {},
            {one, nextCaptureCommitCount, filledMemory, posedge});
        const InstructionId readWrittenMemory = addInstruction(
            Opcode::MemoryRead, {memoryWriteValue}, {writtenMemory, zero});
        const InstructionId readFilledMemory = addInstruction(
            Opcode::MemoryRead, {memoryFillValue}, {filledMemory, zero});
        const InstructionId detectCaptured = addInstruction(
            Opcode::ChangedAny, {capturedChanged}, {capturedPayload, capturedOld});
        const InstructionId detectState =
            addInstruction(Opcode::ChangedAny, {stateChanged}, {state, stateOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        const auto addScheduledInstruction =
            [&](Opcode opcode, std::initializer_list<VariableId> operands) {
                return scheduled.addInstruction(
                    opcode,
                    {},
                    std::span<const VariableId>(operands.begin(), operands.size()));
            };
        const auto setTargets = [&](InstructionId instruction,
                                    std::initializer_list<BlockId> targets) {
            scheduled.setActivationTargets(
                instruction,
                std::span<const BlockId>(targets.begin(), targets.size()));
        };
        const auto addBlock = [&](std::initializer_list<InstructionId> instructions) {
            scheduled.addBlock(std::span<const InstructionId>(instructions.begin(),
                                                               instructions.size()));
        };

        const InstructionId enterEdge =
            addScheduledInstruction(Opcode::ActForward, {entryEvent});
        const InstructionId enterPayload =
            addScheduledInstruction(Opcode::ActForward, {payloadEvent});
        const InstructionId activateCommit =
            addScheduledInstruction(Opcode::ActBackward, {posedge});
        const InstructionId activateCapture =
            addScheduledInstruction(Opcode::ActBackward, {posedge});
        const InstructionId activateLateData =
            addScheduledInstruction(Opcode::ActBackward, {capturedChanged});
        const InstructionId reactivateCommit =
            addScheduledInstruction(Opcode::ActForward, {lateChanged});
        const InstructionId reactivateCapture =
            addScheduledInstruction(Opcode::ActBackward, {stateChanged});
        setTargets(enterEdge, {BlockId{1}});
        setTargets(enterPayload, {BlockId{1}});
        setTargets(activateCommit, {BlockId{3}});
        setTargets(activateCapture, {BlockId{4}});
        setTargets(activateLateData, {BlockId{2}});
        setTargets(reactivateCommit, {BlockId{3}});
        setTargets(reactivateCapture, {BlockId{4}});
        addBlock({sampleCommitEvent, watchClock, enterEdge, watchPayload, enterPayload});
        addBlock({detectPosedge, activateCommit, activateCapture});
        addBlock({updateLateData, detectLateData, reactivateCommit});
        addBlock({addState, addCommitCount, commit, countCommit, detectState,
                  reactivateCapture});
        addBlock({addCaptureCommitCount, capturePayload, countCaptureCommit,
                  writeMemory, fillMemory, readWrittenMemory, readFilledMemory,
                  detectCaptured, activateLateData});

        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = std::move(interface),
            .commitBlockBegin = 3,
            .commitBlockEnd = 5,
            .commitBlockOrder = {BlockId{4}, BlockId{3}},
            .commitGroupOffsets = {0, 1, 2},
            .preCommitSnapshots = {
                PreCommitSnapshot{.source = state, .target = sampledState},
            },
        };
    }

    int testPhasedCommitRuntime(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        const ExecutableModel model = makePhasedCommitModel();
        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "PhasedCommitTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            for (const wolvrix::lib::diag::Diagnostic &diagnostic : diagnostics.messages())
            {
                std::cerr << "[phased-commit] " << diagnostic.message;
                if (!diagnostic.context.empty())
                {
                    std::cerr << " [" << diagnostic.context << ']';
                }
                std::cerr << '\n';
            }
            return fail("AM C++ emitter failed to generate the phased commit model");
        }

        const std::optional<std::string> headerText =
            readTextFile(outputDirectory / "grhsim_PhasedCommitTop.hpp");
        const std::optional<std::string> runtimeText =
            readTextFile(outputDirectory / "grhsim_PhasedCommitTop_runtime.cpp");
        const std::optional<std::string> blockText =
            readTextFile(outputDirectory / "grhsim_PhasedCommitTop_blocks_0.cpp");
        if (!headerText || !runtimeText || !blockText)
        {
            return fail("AM C++ emitter produced unreadable phased commit artifacts");
        }
        const std::size_t evalBegin =
            runtimeText->find("void GrhSIM_PhasedCommitTop::eval() {");
        const std::size_t evalEnd = runtimeText->find(
            "\n}\n\nvoid GrhSIM_PhasedCommitTop::set_random_seed", evalBegin);
        if (evalBegin == std::string::npos || evalEnd == std::string::npos)
        {
            return fail("AM C++ emitter produced an invalid phased commit eval function");
        }
        const std::string_view evalText(*runtimeText);
        const std::string_view evalBody =
            evalText.substr(evalBegin, evalEnd - evalBegin);
        if (headerText->find("static constexpr std::size_t kCommitEventCount = 1;") ==
                std::string::npos ||
            headerText->find("static constexpr std::size_t kCommitEventWordCount = 1;") ==
                std::string::npos ||
            headerText->find(
                "static const std::array<std::uint32_t, kCommitEventCount> "
                "kCommitEventVariables_;") == std::string::npos ||
            headerText->find("std::vector<std::uint32_t> dirtyCommitEventSlots_;") ==
                std::string::npos ||
            headerText->find("std::vector<std::uint32_t> pendingCommitEventSlots_;") ==
                std::string::npos ||
            headerText->find(
                "void set_changed_result(std::size_t variable, bool event) {\n"
                "        values_[variable] = event ? 1 : 0;\n"
                "        if (event) mark_changed_result(variable);\n"
                "    }") == std::string::npos ||
            headerText->find(
                "void set_commit_changed_result(std::uint32_t commitEventSlot, "
                "bool event) {\n"
                "        const std::size_t variable = "
                "kCommitEventVariables_[commitEventSlot];\n"
                "        values_[variable] = event ? 1 : 0;\n"
                "        if (event) mark_commit_changed_result(variable, "
                "commitEventSlot);\n"
                "    }") == std::string::npos ||
            headerText->find(
                "void mark_changed_result(std::size_t variable);") ==
                std::string::npos ||
            headerText->find(
                "void mark_commit_changed_result(std::size_t variable, "
                "std::uint32_t commitEventSlot);") == std::string::npos ||
            headerText->find(
                "static constexpr std::uint64_t bit_mask(std::uint32_t width) {") ==
                std::string::npos ||
            headerText->find(
                "static constexpr std::uint64_t resize_value(") ==
                std::string::npos ||
            headerText->find(
                "static constexpr std::uint64_t concat_value(") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kCommitEventWordCount> "
                "pendingCommitEventBits_{};") == std::string::npos ||
            headerText->find("std::array<bool, kCommitEventCount>") !=
                std::string::npos ||
            runtimeText->find(
                "const std::array<std::uint32_t, 1> "
                "GrhSIM_PhasedCommitTop::kCommitEventVariables_ = {") ==
                std::string::npos ||
            runtimeText->find(
                "for (const std::uint32_t slot : dirtyCommitEventSlots_)") ==
                std::string::npos ||
            runtimeText->find("pendingCommitEventBits_[word] |= bit;") ==
                std::string::npos ||
            runtimeText->find(
                "for (const std::uint32_t slot : pendingCommitEventSlots_)") ==
                std::string::npos ||
            runtimeText->find(
                "set_commit_changed_result(slot, true);") ==
                std::string::npos ||
            runtimeText->find(
                "pendingCommitEventBits_[slot / 64U] &= "
                "~(UINT64_C(1) << (slot % 64U));") == std::string::npos ||
            runtimeText->find(
                "::mark_changed_result(std::size_t variable) {") ==
                std::string::npos ||
            runtimeText->find(
                "::mark_commit_changed_result(std::size_t variable, "
                "std::uint32_t commitEventSlot) {") == std::string::npos ||
            runtimeText->find("::set_changed_result(") != std::string::npos ||
            runtimeText->find("::set_commit_changed_result(") != std::string::npos ||
            runtimeText->find("::bit_mask(") != std::string::npos ||
            runtimeText->find("::resize_value(") != std::string::npos ||
            runtimeText->find("::concat_value(") != std::string::npos ||
            runtimeText->find(
                "for (std::size_t index = 0; index < kCommitEventCount; ++index)") !=
                std::string::npos ||
            blockText->find("set_changed_result(") == std::string::npos ||
            blockText->find("set_commit_changed_result(0, ") == std::string::npos ||
            blockText->find("kNoCommitEventSlot") != std::string::npos ||
            evalBody.find("pendingCommitEventBits_.fill") != std::string_view::npos ||
            evalBody.find("kCommitEventCount") != std::string_view::npos ||
            runtimeText->find("pendingCommitEvents_") !=
                std::string::npos ||
            runtimeText->find("dirtyCommitEventSlots_.clear();") ==
                std::string::npos)
        {
            return fail("AM C++ emitter did not generate sparse commit-event tracking");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_PhasedCommitTop.hpp"
int main()
{
    GrhSIM_PhasedCommitTop model;
    model.init();
    model.clock = 0;
    model.payload = 0x5a;
    model.eval();
    if (model.state != 0 || model.sampled_state != 0 || model.commit_count != 0 ||
        model.capture_commit_count != 0 || model.memory_write_value != 0 ||
        model.memory_fill_value != 0 || model.commit_event != 0)
        return 1;
    model.clock = 1;
    model.eval();
    if (model.state != 0x5a || model.sampled_state != 0 || model.commit_count != 1 ||
        model.capture_commit_count != 1 || model.memory_write_value != 1 ||
        model.memory_fill_value != 1 || model.commit_event != 0)
        return 2;
    model.eval();
    if (model.state != 0x5a || model.sampled_state != 0x5a || model.commit_count != 1 ||
        model.capture_commit_count != 1 || model.memory_write_value != 1 ||
        model.memory_fill_value != 1 || model.commit_event != 0)
        return 3;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the phased commit model harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated phased commit AM model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_PhasedCommitTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated phased commit AM model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated phased commit AM model lost or reused its edge");
        }
        return 0;
    }

    ExecutableModel makeDisabledCommitWriteConsumptionModel()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId memoryType = linear.addType(Type::array(1, 8));
        const VariableId guard = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId data = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId nextData = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId pass = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId nextPass = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId reactivate = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId firstPass = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId address = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId selectedMask = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId registerValue = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId latchValue = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId writtenMemory =
            linear.addVariable(memoryType, linear.zeroInit());
        const VariableId filledMemory =
            linear.addVariable(memoryType, linear.zeroInit());
        const VariableId addressedMemory =
            linear.addVariable(memoryType, linear.zeroInit());
        const VariableId zeroMaskMemory =
            linear.addVariable(memoryType, linear.zeroInit());
        const VariableId writtenValue = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId filledValue = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId addressedValue = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId zeroMaskValue = linear.addVariable(u8Type, linear.zeroInit());
        const std::array<uint64_t, 1> oneWords = {1};
        const std::array<uint64_t, 1> threeWords = {3};
        const std::array<uint64_t, 1> zeroWords = {0};
        const std::array<uint64_t, 1> maskWords = {0xff};
        const VariableId one = addBitConstant(linear, u1Type, oneWords);
        const VariableId zeroBit = addBitConstant(linear, u1Type, zeroWords);
        const VariableId oneValue = addBitConstant(linear, u8Type, oneWords);
        const VariableId three = addBitConstant(linear, u8Type, threeWords);
        const VariableId zero = addBitConstant(linear, u8Type, zeroWords);
        const VariableId mask = addBitConstant(linear, u8Type, maskWords);

        const auto addInstruction = [&](Opcode opcode,
                                        std::initializer_list<VariableId> results,
                                        std::initializer_list<VariableId> operands) {
            return linear.addInstruction(
                opcode,
                std::span<const VariableId>(results.begin(), results.size()),
                std::span<const VariableId>(operands.begin(), operands.size()));
        };
        const InstructionId detectFirstPass =
            addInstruction(Opcode::Eq, {firstPass}, {pass, zero});
        const InstructionId selectAddress = addInstruction(
            Opcode::Mux, {address}, {firstPass, oneValue, zero});
        const InstructionId selectMask = addInstruction(
            Opcode::Mux, {selectedMask}, {firstPass, zero, mask});
        const InstructionId writeRegister = addInstruction(
            Opcode::RegisterWrite, {}, {guard, mask, data, registerValue, one});
        const InstructionId writeLatch = addInstruction(
            Opcode::LatchWrite, {}, {guard, mask, data, latchValue});
        const InstructionId writeMemory = addInstruction(
            Opcode::MemoryWrite, {},
            {guard, zero, mask, data, writtenMemory, one});
        const InstructionId fillMemory = addInstruction(
            Opcode::MemoryFill, {}, {guard, data, filledMemory, one});
        const InstructionId writeAfterInvalidAddress = addInstruction(
            Opcode::MemoryWrite, {},
            {one, address, mask, data, addressedMemory, zeroBit, one});
        const InstructionId writeWithZeroMask = addInstruction(
            Opcode::MemoryWrite, {},
            {one, zero, selectedMask, data, zeroMaskMemory, zeroBit, one});
        const InstructionId readWritten = addInstruction(
            Opcode::MemoryRead, {writtenValue}, {writtenMemory, zero});
        const InstructionId readFilled = addInstruction(
            Opcode::MemoryRead, {filledValue}, {filledMemory, zero});
        const InstructionId readAddressed = addInstruction(
            Opcode::MemoryRead, {addressedValue}, {addressedMemory, zero});
        const InstructionId readZeroMask = addInstruction(
            Opcode::MemoryRead, {zeroMaskValue}, {zeroMaskMemory, zero});
        const InstructionId incrementData =
            addInstruction(Opcode::Add, {nextData}, {data, oneValue});
        const InstructionId storeData =
            addInstruction(Opcode::Assign, {data}, {nextData});
        const InstructionId incrementPass =
            addInstruction(Opcode::Add, {nextPass}, {pass, oneValue});
        const InstructionId storePass =
            addInstruction(Opcode::Assign, {pass}, {nextPass});
        const InstructionId enableWrites =
            addInstruction(Opcode::Assign, {guard}, {one});
        const InstructionId testPass =
            addInstruction(Opcode::Lt, {reactivate}, {pass, three});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activateSelf = scheduled.addInstruction(
            Opcode::ActBackward, {}, std::array{reactivate});
        scheduled.setActivationTargets(activateSelf, std::array{BlockId{1}});
        scheduled.addBlock({});
        const std::array commitInstructions = {
            detectFirstPass, selectAddress, selectMask, writeRegister,
            writeLatch, writeMemory, fillMemory, writeAfterInvalidAddress,
            writeWithZeroMask, readWritten, readFilled, readAddressed,
            readZeroMask, incrementData, storeData, incrementPass, storePass,
            enableWrites, testPass, activateSelf,
        };
        scheduled.addBlock(commitInstructions);

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = scheduled.addString("pass_count"),
                .direction = PortDirection::Output,
                .output = pass,
            },
            PortBinding{
                .name = scheduled.addString("data_value"),
                .direction = PortDirection::Output,
                .output = data,
            },
            PortBinding{
                .name = scheduled.addString("register_value"),
                .direction = PortDirection::Output,
                .output = registerValue,
            },
            PortBinding{
                .name = scheduled.addString("latch_value"),
                .direction = PortDirection::Output,
                .output = latchValue,
            },
            PortBinding{
                .name = scheduled.addString("memory_write_value"),
                .direction = PortDirection::Output,
                .output = writtenValue,
            },
            PortBinding{
                .name = scheduled.addString("memory_fill_value"),
                .direction = PortDirection::Output,
                .output = filledValue,
            },
            PortBinding{
                .name = scheduled.addString("addressed_memory_value"),
                .direction = PortDirection::Output,
                .output = addressedValue,
            },
            PortBinding{
                .name = scheduled.addString("zero_mask_memory_value"),
                .direction = PortDirection::Output,
                .output = zeroMaskValue,
            },
        };
        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = std::move(interface),
            .commitBlockBegin = 1,
            .commitBlockEnd = 2,
            .commitBlockOrder = {BlockId{1}},
            .commitGroupOffsets = {0, 1},
        };
    }

    int testDisabledCommitWriteConsumptionRuntime(
        const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        const ExecutableModel model = makeDisabledCommitWriteConsumptionModel();
        if (!validate(model, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("disabled commit-write reactivation fixture is invalid");
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "GuardReactivationTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail(
                "AM C++ emitter failed to generate the disabled commit-write fixture");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_GuardReactivationTop.hpp"
int main()
{
    GrhSIM_GuardReactivationTop model;
    model.init();
    model.eval();
    if (model.pass_count != 3 || model.data_value != 3 ||
        model.register_value != 0 || model.latch_value != 2 ||
        model.memory_write_value != 0 || model.memory_fill_value != 0 ||
        model.addressed_memory_value != 0 || model.zero_mask_memory_value != 0)
        return 1;
    model.eval();
    if (model.pass_count != 3 || model.data_value != 3 ||
        model.register_value != 0 || model.latch_value != 2 ||
        model.memory_write_value != 0 || model.memory_fill_value != 0 ||
        model.addressed_memory_value != 0 || model.zero_mask_memory_value != 0)
        return 2;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the disabled commit-write fixture harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated disabled commit-write fixture failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_GuardReactivationTop.a").string() +
            "' -o '" + harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail(
                "generated disabled commit-write fixture harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail(
                "commit event was reused after a disabled or invalid write");
        }
        return 0;
    }

    ExecutableModel makeSameGroupForwardCaptureModel()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const VariableId trigger = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId triggerOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId triggerChanged =
            linear.addVariable(u1Type, linear.zeroInit());
        const std::array<uint64_t, 1> oneWords = {1};
        const std::array<uint64_t, 1> maskWords = {0xff};
        const std::array<uint64_t, 1> nextAWords = {0x34};
        const VariableId one = addBitConstant(linear, u1Type, oneWords);
        const VariableId mask = addBitConstant(linear, u8Type, maskWords);
        const VariableId nextA = addBitConstant(linear, u8Type, nextAWords);
        const VariableId stateA = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId stateB = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId capturedA =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId activateBEvent =
            linear.addVariable(u1Type, linear.zeroInit());

        const auto addInstruction = [&](Opcode opcode,
                                        std::initializer_list<VariableId> results,
                                        std::initializer_list<VariableId> operands) {
            return linear.addInstruction(
                opcode,
                std::span<const VariableId>(results.begin(), results.size()),
                std::span<const VariableId>(operands.begin(), operands.size()));
        };
        const InstructionId detect = addInstruction(
            Opcode::ChangedAny, {triggerChanged}, {trigger, triggerOld});
        const InstructionId writeA = addInstruction(
            Opcode::RegisterWrite, {}, {one, mask, nextA, stateA, one});
        const InstructionId enableB =
            addInstruction(Opcode::Assign, {activateBEvent}, {trigger});
        const InstructionId writeB = addInstruction(
            Opcode::RegisterWrite, {}, {one, mask, capturedA, stateB, one});

        ScheduledProgramBuilder scheduled(linear.finish());
        const auto addScheduledInstruction =
            [&](Opcode opcode, std::initializer_list<VariableId> operands) {
                return scheduled.addInstruction(
                    opcode,
                    {},
                    std::span<const VariableId>(operands.begin(), operands.size()));
            };
        const InstructionId activateA =
            addScheduledInstruction(Opcode::ActForward, {triggerChanged});
        const InstructionId activateB =
            addScheduledInstruction(Opcode::ActForward, {activateBEvent});
        scheduled.setActivationTargets(activateA, std::array{BlockId{1}});
        scheduled.setActivationTargets(activateB, std::array{BlockId{2}});
        scheduled.addBlock(std::array{detect, activateA});
        scheduled.addBlock(std::array{writeA, enableB, activateB});
        scheduled.addBlock(std::array{writeB});

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = scheduled.addString("trigger"),
                .direction = PortDirection::Input,
                .input = trigger,
            },
            PortBinding{
                .name = scheduled.addString("state_a"),
                .direction = PortDirection::Output,
                .output = stateA,
            },
            PortBinding{
                .name = scheduled.addString("state_b"),
                .direction = PortDirection::Output,
                .output = stateB,
            },
        };
        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = std::move(interface),
            .commitBlockBegin = 1,
            .commitBlockEnd = 3,
            .commitBlockOrder = {BlockId{1}, BlockId{2}},
            .commitGroupOffsets = {0, 2},
            .commitOperandCaptures = {
                CommitOperandCapture{.source = stateA, .target = capturedA},
            },
            .commitOperandCaptureOffsets = {0, 0, 1},
        };
    }

    int testSameGroupForwardCaptureRuntime(
        const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        const ExecutableModel model = makeSameGroupForwardCaptureModel();
        if (!validate(model, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("same-group forward capture fixture is invalid");
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "SameGroupForwardCaptureTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the same-group capture fixture");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_SameGroupForwardCaptureTop.hpp"
int main()
{
    GrhSIM_SameGroupForwardCaptureTop model;
    model.init();
    model.trigger = 0;
    model.eval();
    if (model.state_a != 0x34 || model.state_b != 0)
        return 1;
    model.trigger = 1;
    model.eval();
    if (model.state_a != 0x34 || model.state_b != 0x34)
        return 2;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the same-group capture fixture harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated same-group capture fixture failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_SameGroupForwardCaptureTop.a").string() +
            "' -o '" + harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated same-group capture fixture harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("same-group forward activation reused a stale operand capture");
        }
        return 0;
    }

    int testCrossGroupFirstEvalCaptureRuntime(
        const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ExecutableModel model = makeSameGroupForwardCaptureModel();
        model.commitGroupOffsets = {0, 1, 2};
        if (!validate(model, ValidationOptions{.level = ValidationLevel::Semantic})
                 .success())
        {
            return fail("cross-group first-eval capture fixture is invalid");
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "CrossGroupFirstEvalCaptureTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail(
                "AM C++ emitter failed to generate the cross-group first-eval capture fixture");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_CrossGroupFirstEvalCaptureTop.hpp"
int main()
{
    GrhSIM_CrossGroupFirstEvalCaptureTop model;
    model.init();
    model.trigger = 1;
    model.eval();
    if (model.state_a != 0x34 || model.state_b != 0x34)
        return 1;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the cross-group first-eval capture harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated cross-group first-eval capture fixture failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_CrossGroupFirstEvalCaptureTop.a").string() +
            "' -o '" + harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail(
                "generated cross-group first-eval capture fixture harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail(
                "cross-group first-eval activation reused the forced operand capture");
        }
        return 0;
    }

    LinearProgramArtifact makeProductionCommitCycleProgram()
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
        const VariableId stateD = builder.addVariable(type, builder.zeroInit());
        const VariableId guardB = builder.addVariable(type, builder.undefInit());
        const VariableId guardC = builder.addVariable(type, builder.undefInit());
        const VariableId dataA = builder.addVariable(type, builder.undefInit());
        const VariableId outputA = builder.addVariable(type, builder.undefInit());
        const VariableId outputB = builder.addVariable(type, builder.undefInit());
        const VariableId outputC = builder.addVariable(type, builder.undefInit());
        const VariableId outputD = builder.addVariable(type, builder.undefInit());

        builder.addInstruction(Opcode::Assign, std::array{guardB}, std::array{stateA});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{guardB, one, one, stateB, one});
        builder.addInstruction(Opcode::Assign, std::array{guardC}, std::array{stateB});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{guardC, one, one, stateC, one});
        builder.addInstruction(Opcode::LogicOr, std::array{dataA},
                               std::array{stateC, one});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{start, one, dataA, stateA, one});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{start, one, guardB, stateD, one});
        builder.addInstruction(Opcode::Assign, std::array{outputA}, std::array{stateA});
        builder.addInstruction(Opcode::Assign, std::array{outputB}, std::array{stateB});
        builder.addInstruction(Opcode::Assign, std::array{outputC}, std::array{stateC});
        builder.addInstruction(Opcode::Assign, std::array{outputD}, std::array{stateD});

        ProgramInterface interface;
        interface.ports = {
            PortBinding{
                .name = builder.addString("start"),
                .direction = PortDirection::Input,
                .input = start,
            },
            PortBinding{
                .name = builder.addString("state_a"),
                .direction = PortDirection::Output,
                .output = outputA,
            },
            PortBinding{
                .name = builder.addString("state_b"),
                .direction = PortDirection::Output,
                .output = outputB,
            },
            PortBinding{
                .name = builder.addString("state_c"),
                .direction = PortDirection::Output,
                .output = outputC,
            },
            PortBinding{
                .name = builder.addString("state_d"),
                .direction = PortDirection::Output,
                .output = outputD,
            },
        };
        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput,
            VariableRole::None,
            VariableRole::State,
            VariableRole::State,
            VariableRole::State,
            VariableRole::State,
            VariableRole::None,
            VariableRole::None,
            VariableRole::None,
            VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
        };
        facts.instructionEffects = {
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::Pure,
            InstructionEffect::Pure,
            InstructionEffect::Pure,
        };
        return LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        };
    }

    int testProductionCommitCycleRuntime(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ProductionActivityScheduleStage scheduler;
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = scheduler.schedule(
            makeProductionCommitCycleProgram(),
            ActivityScheduleOptions{
                .maxInstructionsPerBlock = 1,
                .maxCommitInstructionsPerBlock = 1,
                .maxStateWritesPerBlock = 1,
                .enableCoarsening = false,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->commitBlockOrder.size() != 4 ||
            model->commitGroupOffsets != std::vector<uint32_t>{0, 3, 4} ||
            model->commitOperandCaptures.empty())
        {
            return fail("failed to schedule the generated cyclic commit fixture");
        }

        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "CommitCycleTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the cyclic commit fixture");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_CommitCycleTop.hpp"
int main()
{
    GrhSIM_CommitCycleTop model;
    model.init();
    model.start = 0;
    model.eval();
    if (model.state_a != 0 || model.state_b != 0 || model.state_c != 0 ||
        model.state_d != 0)
        return 1;
    model.start = 1;
    model.eval();
    if (model.state_a != 1 || model.state_b != 1 || model.state_c != 1 ||
        model.state_d != 0)
        return 2;
    model.eval();
    if (model.state_a != 1 || model.state_b != 1 || model.state_c != 1)
        return 3;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the cyclic commit fixture harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated cyclic commit fixture failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_CommitCycleTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated cyclic commit fixture harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated cyclic commit fixture skipped a compute frontier");
        }
        return 0;
    }

} // namespace

int main()
{
    const std::filesystem::path outputDirectory =
        std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "cpp-emitter";
    std::filesystem::remove_all(outputDirectory);

    BaselineActivityScheduleStage scheduler;
    wolvrix::lib::diag::Diagnostics diagnostics;
    std::optional<ExecutableModel> model =
        scheduler.schedule(makeAddProgram(), ActivityScheduleOptions{}, diagnostics);
    if (!model || diagnostics.hasError())
    {
        return fail("failed to build the scalar AM emitter fixture");
    }
    const VariableId input = model->interface.ports[0].input;
    const VariableId output = model->interface.ports[1].output;
    const VariableId initialized = model->interface.ports[2].output;
    const VariableId random = model->interface.ports[3].output;
    const VariableId wideInput = model->interface.ports[4].input;
    const VariableId wideOutput = model->interface.ports[5].output;
    const VariableId wideInitialized = model->interface.ports[6].output;
    Interpreter reference(*model);
    const std::array<uint64_t, 1> firstInput = {41};
    const std::array<uint64_t, 3> firstWideInput = {
        UINT64_C(0x1111222233334444),
        UINT64_C(0xaaaabbbbccccdddd),
        UINT64_C(0x7),
    };
    if (!reference.ready() ||
        !reference.write(
                      input,
                      InterpreterValue::bitVector(
                          8, Signedness::Unsigned, firstInput))
             .success() ||
        !reference.write(
                      wideInput,
                      InterpreterValue::bitVector(
                          130, Signedness::Unsigned, firstWideInput))
             .success() ||
        !reference.eval().success() || reference.value(output).lowWord() != 42 ||
        reference.value(initialized).lowWord() != 0x5a ||
        reference.value(random).lowWord() != 0xaf ||
        reference.value(wideOutput).words()[2] != 0x3 ||
        reference.value(wideInitialized).words()[2] != 0x2)
    {
        return fail("AM reference executor disagreed on the first scalar eval");
    }
    const std::array<uint64_t, 1> secondInput = {5};
    if (!reference.write(
                      input,
                      InterpreterValue::bitVector(
                          8, Signedness::Unsigned, secondInput))
             .success() ||
        !reference.eval().success() || reference.value(output).lowWord() != 6)
    {
        return fail("AM reference executor disagreed after an input change");
    }

    GrhSimAmCppEmitter emitter;
    for (const std::string_view invalidValue : {std::string_view{"0"},
                                                std::string_view{"not-a-number"}})
    {
        wolvrix::lib::diag::Diagnostics invalidSourceBytesDiagnostics;
        const GrhSimAmCppResult invalidSourceBytesResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory /
                                   ("invalid-source-bytes-" + std::string(invalidValue)),
                .modelName = "InvalidSourceBytesTop",
                .maxOutputFileBytes = 1024 * 1024,
                .attributes = {{"maxSourceBytes", std::string(invalidValue)}},
            },
            invalidSourceBytesDiagnostics);
        if (invalidSourceBytesResult.success ||
            !invalidSourceBytesDiagnostics.hasError() ||
            !invalidSourceBytesResult.artifacts.empty() ||
            !hasDiagnosticContaining(invalidSourceBytesDiagnostics, "maxSourceBytes"))
        {
            return fail("AM C++ emitter accepted an invalid maxSourceBytes attribute");
        }
    }
    const GrhSimAmCppResult emitResult = emitter.emit(
        *model,
        GrhSimAmCppOptions{
            .outputDirectory = outputDirectory,
            .modelName = "TestTop",
            .maxOutputFileBytes = 1024 * 1024,
            .attributes = {{"blocksPerSource", "16"}, {"maxSourceBytes", "1"}},
        },
        diagnostics);
    if (!emitResult.success || diagnostics.hasError())
    {
        return fail("AM C++ emitter failed to generate the scalar model");
    }
    if (!hasArtifact(emitResult, "grhsim_TestTop.hpp") ||
        !hasArtifact(emitResult, "grhsim_TestTop_support.hpp") ||
        !hasArtifact(emitResult, "grhsim_TestTop_runtime.cpp") ||
        !hasArtifact(emitResult, "grhsim_TestTop_blocks_0.cpp") ||
        !hasArtifact(emitResult, "grhsim_TestTop_blocks_0_part_1.cpp") ||
        !hasArtifact(emitResult, "Makefile") ||
        !hasExactMakefileSources(
            emitResult,
            outputDirectory,
            {"grhsim_TestTop_runtime.cpp",
             "grhsim_TestTop_blocks_0.cpp",
             "grhsim_TestTop_blocks_0_part_1.cpp"}))
    {
        return fail("AM C++ emitter produced an incomplete multi-source artifact manifest");
    }
    const std::optional<std::string> splitHeaderText =
        readTextFile(outputDirectory / "grhsim_TestTop.hpp");
    const std::optional<std::string> splitRuntimeText =
        readTextFile(outputDirectory / "grhsim_TestTop_runtime.cpp");
    const std::optional<std::string> splitFirstPartText =
        readTextFile(outputDirectory / "grhsim_TestTop_blocks_0.cpp");
    const std::optional<std::string> splitSecondPartText =
        readTextFile(outputDirectory / "grhsim_TestTop_blocks_0_part_1.cpp");
    if (!splitHeaderText || !splitRuntimeText || !splitFirstPartText ||
        !splitSecondPartText ||
        splitHeaderText->find("void execute_blocks_0(std::size_t block);") ==
            std::string::npos ||
        splitHeaderText->find("void execute_blocks_0_part_1(std::size_t block);") ==
            std::string::npos ||
        splitHeaderText->find("void execute_blocks_1(std::size_t block);") !=
            std::string::npos ||
        splitRuntimeText->find("switch (block / 16U)") == std::string::npos ||
        splitRuntimeText->find(
            "case 0:\n        if (block < 1U) { execute_blocks_0(block); return; }\n"
            "        execute_blocks_0_part_1(block); return;") == std::string::npos ||
        splitRuntimeText->find("\n    case 1:") != std::string::npos ||
        splitFirstPartText->find("void GrhSIM_TestTop::execute_blocks_0(") ==
            std::string::npos ||
        splitFirstPartText->find("    case 0: {") == std::string::npos ||
        splitFirstPartText->find("    case 1: {") != std::string::npos ||
        splitSecondPartText->find(
            "void GrhSIM_TestTop::execute_blocks_0_part_1(") == std::string::npos ||
        splitSecondPartText->find("    case 0: {") != std::string::npos ||
        splitSecondPartText->find("    case 1: {") == std::string::npos)
    {
        return fail("AM C++ emitter produced an inconsistent physical source split");
    }

    const std::filesystem::path stagingOutputDirectory =
        std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "cpp-emitter-staging";
    std::filesystem::remove_all(stagingOutputDirectory);
    wolvrix::lib::diag::Diagnostics stagingDiagnostics;
    const GrhSimAmCppResult stagingResult = emitter.emit(
        *model,
        GrhSimAmCppOptions{
            .outputDirectory = stagingOutputDirectory,
            .modelName = "StagingTop",
            .maxOutputFileBytes = 1,
            .attributes = {{"blocksPerSource", "1"}},
        },
        stagingDiagnostics);
    if (stagingResult.success || !stagingDiagnostics.hasError() ||
        !stagingResult.artifacts.empty() ||
        std::filesystem::exists(stagingOutputDirectory / "grhsim_StagingTop.hpp") ||
        std::filesystem::exists(stagingOutputDirectory / "grhsim_StagingTop_support.hpp") ||
        std::filesystem::exists(stagingOutputDirectory / "grhsim_StagingTop_runtime.cpp") ||
        !std::filesystem::is_empty(stagingOutputDirectory))
    {
        return fail("AM C++ emitter published a partial artifact after a staging failure");
    }

    const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
    std::ofstream harness(harnessPath);
    harness << R"CPP(#include "grhsim_TestTop.hpp"
int main()
{
    GrhSIM_TestTop model;
    model.init();
    model.input_value = 41;
    model.eval();
    if (model.output_value != 42)
        return 1;
    if (model.initialized_value != 0x5a)
        return 4;
    if (model.random_value != 0xaf)
        return 5;
    model.wide_input = {
        UINT64_C(0x1111222233334444),
        UINT64_C(0xaaaabbbbccccdddd),
        UINT64_C(0x7),
    };
    model.eval();
    if (model.wide_output != std::array<std::uint64_t, 3>{
                                 UINT64_C(0x1111222233334444),
                                 UINT64_C(0xaaaabbbbccccdddd),
                                 UINT64_C(0x3),
                             })
        return 6;
    if (model.wide_initialized != std::array<std::uint64_t, 3>{
                                      UINT64_C(0x0123456789abcdef),
                                      UINT64_C(0xfedcba9876543210),
                                      UINT64_C(0x2),
                                  })
        return 7;
    model.input_value = 5;
    model.eval();
    if (model.output_value != 6)
        return 2;
    model.eval();
    if (model.output_value != 6)
        return 3;
    return 0;
}
)CPP";
    harness.close();
    if (!harness)
    {
        return fail("failed to write the generated model harness");
    }

    const std::string buildCommand =
        "make -C '" + outputDirectory.string() +
        "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
    if (std::system(buildCommand.c_str()) != 0)
    {
        return fail("generated AM model failed to compile");
    }
    const std::filesystem::path harnessExecutable = outputDirectory / "harness";
    const std::string harnessCompileCommand =
        "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
        harnessPath.string() + "' '" +
        (outputDirectory / "libgrhsim_TestTop.a").string() + "' -o '" +
        harnessExecutable.string() + "'";
    if (std::system(harnessCompileCommand.c_str()) != 0)
    {
        return fail("generated AM model harness failed to compile");
    }
    const std::string runCommand = "'" + harnessExecutable.string() + "'";
    if (std::system(runCommand.c_str()) != 0)
    {
        return fail("generated AM model produced the wrong eval result");
    }
    if (const int result = testWidePureOperations(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-wide");
        result != 0)
    {
        return result;
    }
    if (const int result = testMemoryOperations(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-memory");
        result != 0)
    {
        return result;
    }
    if (const int result = testArrayFillInitialization(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-array-init");
        result != 0)
    {
        return result;
    }
    if (const int result = testPackedActivityRuntime(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-packed-activity");
        result != 0)
    {
        return result;
    }
    if (const int result = testPhasedCommitRuntime(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-phased-commit");
        result != 0)
    {
        return result;
    }
    if (const int result = testDisabledCommitWriteConsumptionRuntime(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-guard-reactivation");
        result != 0)
    {
        return result;
    }
    if (const int result = testSameGroupForwardCaptureRuntime(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-same-group-forward-capture");
        result != 0)
    {
        return result;
    }
    if (const int result = testCrossGroupFirstEvalCaptureRuntime(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-cross-group-first-eval-capture");
        result != 0)
    {
        return result;
    }
    return testProductionCommitCycleRuntime(
        std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
        "cpp-emitter-production-commit-cycle");
}
