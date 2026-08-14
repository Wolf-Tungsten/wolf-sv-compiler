#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_program_cpp_emitter.hpp"
#include "grhsim/am/grhsim_am_program_interpreter.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

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
        // NO0004: wide Insert must overwrite the window (not OR into it) —
        // unsigned65A overlaps bits already set in unsigned130Base.
        const InstructionId insertWide =
            addPure(Opcode::Insert, addOutput(u130Type, "insert_130"),
                    {unsigned130Base, unsigned65A});
        builder.setSliceStaticAttributes(insertWide, 17);

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
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            AmGraph::fromLinearProgram(fixture.artifact), ActivityScheduleOptions{},
            diagnostics);
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

        const VariableId memory = linear.addVariable(memoryType, linear.zeroInit());
        const VariableId memoryOld = linear.addVariable(memoryType, linear.undefInit());
        const VariableId changedEvent = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId commitMemoryOld =
            linear.addVariable(memoryType, linear.undefInit());
        const VariableId commitChangedEvent =
            linear.addVariable(u1Type, linear.zeroInit());

        // Commit gate detectors: one head changed.any per input the commit
        // Block reads, each with a private old baseline. Their results form
        // the Block's single event gate and serve as the writes' event
        // operands (the scheduler's re-pointed clones in hand-built form).
        const std::array<VariableId, 6> gateWatched = {
            fillEnable, writeEnable, address, writeMask, writeData, fillData,
        };
        const std::array<TypeId, 6> gateTypes = {
            u1Type, u1Type, u8Type, u8Type, u8Type, u32Type,
        };
        std::array<VariableId, 6> gateEvents{};
        std::array<InstructionId, 6> gateDetectors{};
        for (std::size_t index = 0; index < gateWatched.size(); ++index)
        {
            const VariableId gateOld =
                linear.addVariable(gateTypes[index], linear.undefInit());
            gateEvents[index] = linear.addVariable(u1Type, linear.zeroInit());
            gateDetectors[index] = addInstruction(
                Opcode::ChangedAny, {gateEvents[index]},
                {gateWatched[index], gateOld});
        }

        // The update conditions: the fill keeps the current packed image
        // unless fill_enable; the element write keeps cond/mask as operands
        // (reverted from the merged self-mux form), so the masked write is
        // gated on write_enable directly and no read-old helper chain is
        // needed.
        const VariableId memoryPacked = linear.addVariable(u32Type, linear.zeroInit());
        const VariableId fillNext = linear.addVariable(u32Type, linear.zeroInit());

        const InstructionId readPacked = addInstruction(
            Opcode::MemoryReadAll, {memoryPacked}, {memory});
        const InstructionId selectFill = addInstruction(
            Opcode::Mux, {fillNext}, {fillEnable, fillData, memoryPacked});
        const InstructionId fill = addInstruction(
            Opcode::MemoryFill, {},
            {fillNext, memory, gateEvents[0], gateEvents[5]});
        const InstructionId write = addInstruction(
            Opcode::MemoryWriteCondMask,
            {},
            {writeEnable, address, writeMask, writeData, memory, gateEvents[1],
             gateEvents[2], gateEvents[3], gateEvents[4]});
        const InstructionId read = addInstruction(
            Opcode::MemoryRead, {readData}, {memory, address});
        const InstructionId changed = addInstruction(
            Opcode::ChangedAny, {changedEvent}, {memory, memoryOld});
        const InstructionId exposeChanged = addInstruction(
            Opcode::Assign, {memoryChanged}, {changedEvent});
        const InstructionId detectCommitChanged = addInstruction(
            Opcode::ChangedAny, {commitChangedEvent}, {memory, commitMemoryOld});

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
            // Every watched input activates the compute readers and, through
            // the commit Block's gate-detector watch, the commit Block too.
            const std::array<BlockId, 2> targets = {BlockId{1}, BlockId{2}};
            scheduled.setActivationTargets(activate, targets);
            entry.push_back(detect);
            entry.push_back(activate);
        }
        scheduled.addBlock(entry);
        // Compute Block: state readers. The commit Block reactivates it via
        // act.b whenever a write actually changes the memory.
        const std::array<InstructionId, 3> readers = {
            read, changed, exposeChanged,
        };
        scheduled.addBlock(readers);
        // Commit Block: the head gate detectors run on every activation and
        // their OR gates everything else — the nextValue computation, the
        // state writes, and the tail watch detector feeding act.b.
        const InstructionId activateReaders = scheduled.addInstruction(
            Opcode::ActBackward, {}, std::array{commitChangedEvent});
        scheduled.setActivationTargets(activateReaders, std::array{BlockId{1}});
        std::vector<InstructionId> commit(gateDetectors.begin(), gateDetectors.end());
        commit.insert(commit.end(),
                      {readPacked, selectFill, fill, write, detectCommitChanged,
                       activateReaders});
        scheduled.addBlock(commit);

        return MemoryEmitterFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = std::move(interface),
                .commitBlockBegin = 2,
                .commitBlockEnd = 3,
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

        // ST00011 array write-point activation: the commit Block's tail
        // changed.any on the memory is replaced by change flags accumulated at
        // the Block's own write sites (mem.write element masked-write-detect —
        // cond/mask are operands again, so the write site emits
        // masked_write_words_detect — and mem.fill per-element detect in the
        // fill loop); the whole-array compare and baseline copy survive only
        // for the compute-Block detector, which has no same-Block write site.
        const std::optional<std::string> memoryBlocksText =
            readTextFile(outputDirectory / "grhsim_MemoryTop_blocks_0.cpp");
        if (!memoryBlocksText ||
            countOccurrences(*memoryBlocksText, "assign_words_detect(") != 0 ||
            countOccurrences(*memoryBlocksText, "masked_write_words_detect(") != 1 ||
            countOccurrences(*memoryBlocksText, "slice_words_detect(") != 1 ||
            countOccurrences(*memoryBlocksText, "bool arrChg_0 = false;") != 1 ||
            countOccurrences(*memoryBlocksText, "= (arrChg_0);") != 1 ||
            countOccurrences(*memoryBlocksText, "std::equal(") != 1 ||
            countOccurrences(*memoryBlocksText, "std::copy_n(") != 1)
        {
            return fail("AM C++ emitter did not fold the commit memory detector "
                        "into write-point change flags (ST00011)");
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

    struct ArrayEmitterFixture
    {
        ExecutableModel model;
        VariableId laneMask;
        VariableId data;
        VariableId scalar;
        VariableId index;
        VariableId clock;
        VariableId sel1;
        VariableId t1;
        VariableId f1;
        VariableId sel5;
        VariableId t13;
        VariableId f13;
        VariableId scalar13;
        VariableId bit1;
        VariableId all;
        VariableId muxed;
        VariableId broadcast;
        VariableId onehot;
        VariableId redOr;
        VariableId redAnd;
        VariableId redXor;
        VariableId lanesOr;
        VariableId lanesAnd;
        VariableId lanesXor;
        VariableId muxed1;
        VariableId muxed13;
        VariableId bcast13;
        VariableId bcast1;
    };

    // 8-lane x 8-bit array loopback: the entry Block's clock watch activates
    // the commit Block, whose head changed.pos gate drives a clocked
    // mem.write_lanes that scatters packed lanes into the memory; the commit
    // Block's tail changed.any reactivates the reader Block, which packs the
    // array with mem.read_all and applies the pure array ops.
    ArrayEmitterFixture makeArrayEmitterFixture()
    {
        LinearProgramBuilder linear;
        ProgramInterface interface;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u4Type = linear.addType(Type::bitVector(4));
        const TypeId u5Type = linear.addType(Type::bitVector(5));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId u13Type = linear.addType(Type::bitVector(13));
        const TypeId u16Type = linear.addType(Type::bitVector(16));
        const TypeId u64Type = linear.addType(Type::bitVector(64));
        const TypeId u65Type = linear.addType(Type::bitVector(65));
        const TypeId arrayType = linear.addType(Type::array(8, 8));
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

        const VariableId laneMask = addInput(u8Type, "lane_mask");
        const VariableId data = addInput(u64Type, "data");
        const VariableId scalar = addInput(u8Type, "scalar");
        const VariableId index = addInput(u4Type, "index");
        const VariableId clock = addInput(u1Type, "clock");
        // Width-class coverage for the array word helpers: elemWidth 1, a
        // power of two (8 above), and a non-power-of-two width whose lanes
        // cross word boundaries (13).
        const VariableId sel1 = addInput(u16Type, "sel1");
        const VariableId t1 = addInput(u16Type, "t1");
        const VariableId f1 = addInput(u16Type, "f1");
        const VariableId sel5 = addInput(u5Type, "sel5");
        const VariableId t13 = addInput(u65Type, "t13");
        const VariableId f13 = addInput(u65Type, "f13");
        const VariableId scalar13 = addInput(u13Type, "scalar13");
        const VariableId bit1 = addInput(u1Type, "bit1");
        const VariableId all = addOutput(u64Type, "all");
        const VariableId muxed = addOutput(u64Type, "muxed");
        const VariableId broadcast = addOutput(u64Type, "broadcast");
        const VariableId onehot = addOutput(u8Type, "onehot");
        const VariableId redOr = addOutput(u1Type, "red_or");
        const VariableId redAnd = addOutput(u1Type, "red_and");
        const VariableId redXor = addOutput(u1Type, "red_xor");
        const VariableId lanesOr = addOutput(u8Type, "lanes_or");
        const VariableId lanesAnd = addOutput(u8Type, "lanes_and");
        const VariableId lanesXor = addOutput(u8Type, "lanes_xor");
        const VariableId muxed1 = addOutput(u16Type, "muxed1");
        const VariableId muxed13 = addOutput(u65Type, "muxed13");
        const VariableId bcast13 = addOutput(u65Type, "bcast13");
        const VariableId bcast1 = addOutput(u64Type, "bcast1");

        const VariableId clockOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId clockEvent = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId commitClockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId clockPos = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId memory = linear.addVariable(arrayType, linear.zeroInit());
        const VariableId memoryOld =
            linear.addVariable(arrayType, linear.undefInit());
        const VariableId memoryEvent =
            linear.addVariable(u1Type, linear.zeroInit());

        // The entry Block's changed.any clock watch activates the commit
        // Block on every edge; the head changed.pos clone inside the commit
        // Block forms the write gate.
        const InstructionId watchClock = addInstruction(
            Opcode::ChangedAny, {clockEvent}, {clock, clockOld});
        const InstructionId detectClock = addInstruction(
            Opcode::ChangedPos, {clockPos}, {clock, commitClockOld});
        const InstructionId write = addInstruction(
            Opcode::MemoryWriteLanes, {}, {laneMask, data, memory, clockPos});
        const InstructionId readAll = addInstruction(
            Opcode::MemoryReadAll, {all}, {memory});
        const InstructionId bcast = addInstruction(
            Opcode::ArrayBroadcast, {broadcast}, {scalar});
        const InstructionId one = addInstruction(
            Opcode::ArrayOnehot, {onehot}, {index});
        const InstructionId mux = addInstruction(
            Opcode::ArrayMux, {muxed}, {onehot, broadcast, all});
        const InstructionId orReduce = addInstruction(
            Opcode::ArrayReduceOr, {redOr}, {all});
        const InstructionId andReduce = addInstruction(
            Opcode::ArrayReduceAnd, {redAnd}, {all});
        const InstructionId xorReduce = addInstruction(
            Opcode::ArrayReduceXor, {redXor}, {all});
        const InstructionId orLanes = addInstruction(
            Opcode::ArrayReduceLanesOr, {lanesOr}, {muxed});
        const InstructionId andLanes = addInstruction(
            Opcode::ArrayReduceLanesAnd, {lanesAnd}, {muxed});
        const InstructionId xorLanes = addInstruction(
            Opcode::ArrayReduceLanesXor, {lanesXor}, {muxed});
        const InstructionId muxOne = addInstruction(
            Opcode::ArrayMux, {muxed1}, {sel1, t1, f1});
        const InstructionId muxThirteen = addInstruction(
            Opcode::ArrayMux, {muxed13}, {sel5, t13, f13});
        const InstructionId bcastThirteen = addInstruction(
            Opcode::ArrayBroadcast, {bcast13}, {scalar13});
        const InstructionId bcastOne = addInstruction(
            Opcode::ArrayBroadcast, {bcast1}, {bit1});
        const InstructionId changed = addInstruction(
            Opcode::ChangedAny, {memoryEvent}, {memory, memoryOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        const InstructionId activateCommit = scheduled.addInstruction(
            Opcode::ActForward, {}, std::array{clockEvent});
        scheduled.setActivationTargets(activateCommit, std::array{BlockId{2}});
        std::vector<InstructionId> entry{watchClock, activateCommit};
        const std::array<VariableId, 12> computeInputs = {
            laneMask, data, scalar, index, sel1, t1,
            f1, sel5, t13, f13, scalar13, bit1,
        };
        for (VariableId input : computeInputs)
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
        const std::array<InstructionId, 14> readers = {
            readAll, bcast, one, mux, orReduce, andReduce, xorReduce,
            orLanes, andLanes, xorLanes, muxOne, muxThirteen, bcastThirteen,
            bcastOne,
        };
        scheduled.addBlock(readers);
        const InstructionId activateReaders = scheduled.addInstruction(
            Opcode::ActBackward, {}, std::array{memoryEvent});
        scheduled.setActivationTargets(activateReaders, std::array{BlockId{1}});
        const std::array<InstructionId, 4> commit = {
            detectClock, write, changed, activateReaders,
        };
        scheduled.addBlock(commit);

        return ArrayEmitterFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = std::move(interface),
                .commitBlockBegin = 2,
                .commitBlockEnd = 3,
            },
            .laneMask = laneMask,
            .data = data,
            .scalar = scalar,
            .index = index,
            .clock = clock,
            .sel1 = sel1,
            .t1 = t1,
            .f1 = f1,
            .sel5 = sel5,
            .t13 = t13,
            .f13 = f13,
            .scalar13 = scalar13,
            .bit1 = bit1,
            .all = all,
            .muxed = muxed,
            .broadcast = broadcast,
            .onehot = onehot,
            .redOr = redOr,
            .redAnd = redAnd,
            .redXor = redXor,
            .lanesOr = lanesOr,
            .lanesAnd = lanesAnd,
            .lanesXor = lanesXor,
            .muxed1 = muxed1,
            .muxed13 = muxed13,
            .bcast13 = bcast13,
            .bcast1 = bcast1,
        };
    }

    struct ArrayTransaction
    {
        uint64_t laneMask;
        uint64_t data;
        uint64_t scalar;
        uint64_t index;
        uint64_t clock;
        uint64_t sel1;
        uint64_t t1;
        uint64_t f1;
        uint64_t sel5;
        std::array<uint64_t, 2> t13;
        std::array<uint64_t, 2> f13;
        uint64_t scalar13;
        uint64_t bit1;
    };

    int testArrayOperations(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ArrayEmitterFixture fixture = makeArrayEmitterFixture();
        Interpreter reference(fixture.model);
        if (!reference.ready() || !reference.eval().success())
        {
            return fail("failed to build the AM array reference fixture");
        }

        const std::array<ArrayTransaction, 5> transactions = {
            ArrayTransaction{0xa5, UINT64_C(0x1122314455667788), 0x5c, 3, 1,
                             0xa5a5, 0x1234, 0xabcd, 0x16,
                             {UINT64_C(0x0100040010004001), 1},
                             {UINT64_C(0x7fbc3e001fff), 0}, 0x0abc, 1},
            ArrayTransaction{0xa5, UINT64_C(0x1122314455667788), 0x5c, 3, 0,
                             0xa5a5, 0x1234, 0xabcd, 0x16,
                             {UINT64_C(0x0100040010004001), 1},
                             {UINT64_C(0x7fbc3e001fff), 0}, 0x0abc, 1},
            ArrayTransaction{0xff, UINT64_MAX, 0x5c, 3, 1,
                             0x5a5a, 0xcdef, 0x4321, 0x0b,
                             {UINT64_C(0x0100040010004001), 1},
                             {UINT64_C(0x7fbc3e001fff), 0}, 0x1357, 0},
            ArrayTransaction{0x00, UINT64_C(0), 0x31, 5, 0,
                             0x5a5a, 0xcdef, 0x4321, 0x0b,
                             {UINT64_C(0x0100040010004001), 1},
                             {UINT64_C(0x7fbc3e001fff), 0}, 0x1357, 0},
            ArrayTransaction{0x03, UINT64_C(0x1122314455667788), 0x31, 5, 1,
                             0x5a5a, 0xcdef, 0x4321, 0x0b,
                             {UINT64_C(0x0100040010004001), 1},
                             {UINT64_C(0x7fbc3e001fff), 0}, 0x1357, 0},
        };
        std::vector<std::array<uint64_t, 16>> oracle;
        oracle.reserve(transactions.size());
        const auto writeValue = [&](VariableId variable, uint32_t width, uint64_t value) {
            const std::array<uint64_t, 1> words = {value};
            return reference
                .write(variable,
                       InterpreterValue::bitVector(width, Signedness::Unsigned, words))
                .success();
        };
        const auto writeWide = [&](VariableId variable, uint32_t width,
                                   std::array<uint64_t, 2> words) {
            return reference
                .write(variable,
                       InterpreterValue::bitVector(width, Signedness::Unsigned, words))
                .success();
        };
        for (const ArrayTransaction &transaction : transactions)
        {
            if (!writeValue(fixture.laneMask, 8, transaction.laneMask) ||
                !writeValue(fixture.data, 64, transaction.data) ||
                !writeValue(fixture.scalar, 8, transaction.scalar) ||
                !writeValue(fixture.index, 4, transaction.index) ||
                !writeValue(fixture.clock, 1, transaction.clock) ||
                !writeValue(fixture.sel1, 16, transaction.sel1) ||
                !writeValue(fixture.t1, 16, transaction.t1) ||
                !writeValue(fixture.f1, 16, transaction.f1) ||
                !writeValue(fixture.sel5, 5, transaction.sel5) ||
                !writeWide(fixture.t13, 65, transaction.t13) ||
                !writeWide(fixture.f13, 65, transaction.f13) ||
                !writeValue(fixture.scalar13, 13, transaction.scalar13) ||
                !writeValue(fixture.bit1, 1, transaction.bit1) ||
                !reference.eval().success())
            {
                return fail("AM Interpreter failed an array transaction");
            }
            oracle.push_back({reference.value(fixture.all).lowWord(),
                              reference.value(fixture.muxed).lowWord(),
                              reference.value(fixture.broadcast).lowWord(),
                              reference.value(fixture.onehot).lowWord(),
                              reference.value(fixture.redOr).lowWord(),
                              reference.value(fixture.redAnd).lowWord(),
                              reference.value(fixture.redXor).lowWord(),
                              reference.value(fixture.lanesOr).lowWord(),
                              reference.value(fixture.lanesAnd).lowWord(),
                              reference.value(fixture.lanesXor).lowWord(),
                              reference.value(fixture.muxed1).lowWord(),
                              reference.value(fixture.muxed13).words()[0],
                              reference.value(fixture.muxed13).words()[1],
                              reference.value(fixture.bcast13).words()[0],
                              reference.value(fixture.bcast13).words()[1],
                              reference.value(fixture.bcast1).lowWord()});
        }
        // Hand-computed expectations for the first scatter round: lanes
        // 0/2/5/7 take the packed data lanes, the rest stay zero; the
        // per-lane reduces see muxed = 0x110031005c660088 (lane 3 picked
        // from the broadcast by the onehot select). The width-class ops
        // check elemWidth 1 (muxed1), a non-power-of-two 13 (muxed13,
        // bcast13) and a one-bit broadcast (bcast1).
        if (oracle[0] != std::array<uint64_t, 16>{UINT64_C(0x1100310000660088),
                                                  UINT64_C(0x110031005c660088),
                                                  UINT64_C(0x5c5c5c5c5c5c5c5c),
                                                  0x08, 1, 0, 1,
                                                  0xad, 0x00, 0x20,
                                                  0x0a6c,
                                                  UINT64_C(0x01007f8010005fff), 1,
                                                  UINT64_C(0xabc55e2af1578abc), 0,
                                                  UINT64_MAX})
        {
            return fail("AM Interpreter disagreed with the array transaction oracle");
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            fixture.model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "ArrayTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the array model");
        }

        // ST00011: the commit Block's tail changed.any on the memory is
        // replaced by the mem.write_lanes site's lane-granular change detection.
        const std::optional<std::string> arrayBlocksText =
            readTextFile(outputDirectory / "grhsim_ArrayTop_blocks_0.cpp");
        if (!arrayBlocksText ||
            countOccurrences(*arrayBlocksText, "array_write_scatter_detect(") != 1 ||
            countOccurrences(*arrayBlocksText, "array_readall_pack(") != 1 ||
            countOccurrences(*arrayBlocksText, "array_mux_words(") != 3 ||
            countOccurrences(*arrayBlocksText, "array_broadcast_words(") != 3 ||
            countOccurrences(*arrayBlocksText, "array_onehot_words(") != 1 ||
            countOccurrences(*arrayBlocksText, "array_reduce_lanes_words(") != 3 ||
            countOccurrences(*arrayBlocksText, "bool arrChg_0 = false;") != 1 ||
            countOccurrences(*arrayBlocksText, "= (arrChg_0);") != 1)
        {
            return fail("AM C++ emitter did not lower the array ops to their "
                        "word helpers with write-point detection (ST00011)");
        }

        // The mux helper must not build its lane mask bit by bit (the
        // historical per-bit loop with a division per bit); the power-of-two
        // spread and the per-lane field loop have replaced it.
        const std::optional<std::string> arrayRuntimeText =
            readTextFile(outputDirectory / "grhsim_ArrayTop_runtime.cpp");
        if (!arrayRuntimeText ||
            countOccurrences(*arrayRuntimeText, "(index * 64U + bit) / elemWidth") != 0 ||
            countOccurrences(*arrayRuntimeText, "UINT64_C(0x5555555555555555)") < 1)
        {
            return fail("AM C++ emitter still emits the per-bit array_mux_words "
                        "lane-mask loop");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << "#include \"grhsim_ArrayTop.hpp\"\n"
                   "#include <array>\n"
                   "#include <cstdint>\n\n"
                   "int main()\n"
                   "{\n"
                   "    GrhSIM_ArrayTop model;\n"
                   "    model.init();\n";
        int returnCode = 1;
        for (std::size_t index = 0; index < transactions.size(); ++index)
        {
            const ArrayTransaction &transaction = transactions[index];
            harness << "    model.lane_mask = " << transaction.laneMask << ";\n"
                    << "    model.data = UINT64_C(" << transaction.data << ");\n"
                    << "    model.scalar = " << transaction.scalar << ";\n"
                    << "    model.index = " << transaction.index << ";\n"
                    << "    model.clock = " << transaction.clock << ";\n"
                    << "    model.sel1 = " << transaction.sel1 << ";\n"
                    << "    model.t1 = " << transaction.t1 << ";\n"
                    << "    model.f1 = " << transaction.f1 << ";\n"
                    << "    model.sel5 = " << transaction.sel5 << ";\n"
                    << "    model.t13 = std::array<std::uint64_t, 2>{UINT64_C("
                    << transaction.t13[0] << "), UINT64_C(" << transaction.t13[1] << ")};\n"
                    << "    model.f13 = std::array<std::uint64_t, 2>{UINT64_C("
                    << transaction.f13[0] << "), UINT64_C(" << transaction.f13[1] << ")};\n"
                    << "    model.scalar13 = " << transaction.scalar13 << ";\n"
                    << "    model.bit1 = " << transaction.bit1 << ";\n"
                    << "    model.eval();\n"
                    << "    if (static_cast<std::uint64_t>(model.all) != UINT64_C("
                    << oracle[index][0] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.muxed) != UINT64_C("
                    << oracle[index][1] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.broadcast) != UINT64_C("
                    << oracle[index][2] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.onehot) != UINT64_C("
                    << oracle[index][3] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.red_or) != UINT64_C("
                    << oracle[index][4] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.red_and) != UINT64_C("
                    << oracle[index][5] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.red_xor) != UINT64_C("
                    << oracle[index][6] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.lanes_or) != UINT64_C("
                    << oracle[index][7] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.lanes_and) != UINT64_C("
                    << oracle[index][8] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.lanes_xor) != UINT64_C("
                    << oracle[index][9] << ")) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.muxed1) != UINT64_C("
                    << oracle[index][10] << ")) return " << returnCode++ << ";\n"
                    << "    if (model.muxed13 != std::array<std::uint64_t, 2>{UINT64_C("
                    << oracle[index][11] << "), UINT64_C(" << oracle[index][12]
                    << ")}) return " << returnCode++ << ";\n"
                    << "    if (model.bcast13 != std::array<std::uint64_t, 2>{UINT64_C("
                    << oracle[index][13] << "), UINT64_C(" << oracle[index][14]
                    << ")}) return " << returnCode++ << ";\n"
                    << "    if (static_cast<std::uint64_t>(model.bcast1) != UINT64_C("
                    << oracle[index][15] << ")) return " << returnCode++ << ";\n";
        }
        harness << "    return 0;\n}\n";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the generated array model harness");
        }

        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated array AM model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_ArrayTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated array AM model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated array AM model disagreed with the Interpreter");
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
        facts.variableRoles[memory.value] = VariableRole::State;
        facts.variableRoles[narrowMemory.value] = VariableRole::State;
        for (VariableId output : outputs)
        {
            facts.variableRoles[output.value] = VariableRole::ExternalOutput;
        }
        facts.variableRoles[narrowOutput.value] = VariableRole::ExternalOutput;
        facts.instructionEffects.assign(program.view().instructionCount(),
                                        InstructionEffect::StateRead);
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
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            AmGraph::fromLinearProgram(fixture.artifact), ActivityScheduleOptions{},
            diagnostics);
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
        if (!runtimeText || countOccurrences(*runtimeText, "std::fill_n(wideValues_.data()") != 1 ||
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
        VariableId inputOld;
        VariableId backwardState;
        VariableId backwardStateOld;
        VariableId forwardOutput;
        VariableId backwardOutput;
        VariableId inputChanged;
        VariableId forwardChanged;
        VariableId backwardChanged;
        VariableId commitForwardEvent;
    };

    PackedActivityFixture makePackedActivityFixture()
    {
        constexpr uint32_t kBoundaryBlock = 17;
        constexpr uint32_t kForwardBlock = 64;
        constexpr uint32_t kBackwardBlock = 65;
        constexpr uint32_t kFinalBlock = 129;
        constexpr uint32_t kCommitBlock = 130;

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
        const VariableId forwardChanged =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId backwardState = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId backwardStateOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId backwardChanged =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId forwardOutput = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId backwardOutput =
            linear.addVariable(u1Type, linear.zeroInit());
        const std::array<uint64_t, 1> oneWords = {1};
        const VariableId one = addBitConstant(linear, u1Type, oneWords);
        const VariableId commitForwardOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId commitForwardEvent =
            linear.addVariable(u1Type, linear.zeroInit());
        const VariableId writeNext = linear.addVariable(u1Type, linear.zeroInit());

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
        const InstructionId detectForward = addInstruction(
            Opcode::ChangedAny, {forwardChanged}, {forwardStage, forwardOld});
        const InstructionId assignForwardOutput =
            addInstruction(Opcode::Assign, {forwardOutput}, {forwardStage});
        const InstructionId assignBackwardOutput =
            addInstruction(Opcode::Assign, {backwardOutput}, {backwardState});
        // Commit head gate detector: a changed.any clone on the write's raw
        // event source (forwardStage). The write's update condition folds
        // into nextValue: mux(forwardChanged, 1, backwardState).
        const InstructionId detectCommitForward =
            addInstruction(Opcode::ChangedAny, {commitForwardEvent},
                           {forwardStage, commitForwardOld});
        const InstructionId blendWriteNext = addInstruction(
            Opcode::Mux, {writeNext}, {forwardChanged, one, backwardState});
        const InstructionId writeBackward =
            addInstruction(Opcode::RegisterWrite,
                           {},
                           {writeNext, backwardState, commitForwardEvent});
        const InstructionId detectBackward = addInstruction(
            Opcode::ChangedAny, {backwardChanged}, {backwardState, backwardStateOld});

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
        const InstructionId activateMany =
            addScheduledInstruction(Opcode::ActForward, {}, {forwardStage});
        const InstructionId activateCommit =
            addScheduledInstruction(Opcode::ActForward, {}, {forwardChanged});
        const InstructionId activateBackward =
            addScheduledInstruction(Opcode::ActBackward, {}, {backwardChanged});
        const std::array<BlockId, 2> entryTargets = {
            BlockId{kBoundaryBlock},
            BlockId{kForwardBlock},
        };
        const std::array<BlockId, 1> finalTargets = {BlockId{kFinalBlock}};
        const std::array<BlockId, 1> commitTargets = {BlockId{kCommitBlock}};
        const std::array<BlockId, 1> backwardTargets = {BlockId{kBackwardBlock}};
        // Eight empty Blocks in one activity word: high enough fanout to select
        // the constant-mask emission form, and executing them is a no-op.
        const std::array<BlockId, 8> manyTargets = {
            BlockId{66}, BlockId{67}, BlockId{68}, BlockId{69},
            BlockId{70}, BlockId{71}, BlockId{72}, BlockId{73},
        };
        scheduled.setActivationTargets(activateForward, entryTargets);
        scheduled.setActivationTargets(activateFinal, finalTargets);
        scheduled.setActivationTargets(activateMany, manyTargets);
        scheduled.setActivationTargets(activateCommit, commitTargets);
        scheduled.setActivationTargets(activateBackward, backwardTargets);

        for (uint32_t block = 0; block <= kCommitBlock; ++block)
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
                const std::array<InstructionId, 5> forward = {
                    assignForwardStage,
                    detectForward,
                    activateFinal,
                    activateMany,
                    activateCommit,
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
                const std::array<InstructionId, 1> final = {
                    assignForwardOutput,
                };
                scheduled.addBlock(final);
            }
            else if (block == kCommitBlock)
            {
                // The commit Block is activation-driven: the forward Block
                // act.f's it through forwardChanged (the gate detector's
                // watched event), and inside the Block the head changed.any
                // clone on forwardStage gates the write plus the tail
                // detector that drives act.b back to the reader compute
                // Block.
                const std::array<InstructionId, 5> commit = {
                    detectCommitForward,
                    blendWriteNext,
                    writeBackward,
                    detectBackward,
                    activateBackward,
                };
                scheduled.addBlock(commit);
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
                .commitBlockBegin = kCommitBlock,
                .commitBlockEnd = kCommitBlock + 1,
            },
            .input = input,
            .inputOld = inputOld,
            .backwardState = backwardState,
            .backwardStateOld = backwardStateOld,
            .forwardOutput = forwardOutput,
            .backwardOutput = backwardOutput,
            .inputChanged = inputChanged,
            .forwardChanged = forwardChanged,
            .backwardChanged = backwardChanged,
            .commitForwardEvent = commitForwardEvent,
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
                .attributes = {{"blocksPerSource", "17"}, {"runtimeProfile", "true"}},
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
            firstShardText->find(
                "void GrhSIM_PackedActivityTop::execute_block_0() {") ==
                std::string::npos ||
            firstShardText->find(
                "void GrhSIM_PackedActivityTop::eval_scan_0() {") ==
                std::string::npos ||
            firstShardText->find("active_byte_ref(0) & UINT8_C(0xfe)") ==
                std::string::npos ||
            firstShardText->find("active_byte_ref(0) &= UINT8_C(0x1)") ==
                std::string::npos ||
            firstShardText->find("active_byte_ref(2) & UINT8_C(0x1)") ==
                std::string::npos ||
            firstShardText->find("active_byte_ref(3") != std::string::npos ||
            firstShardText->find("eval_commit_") != std::string::npos ||
            secondShardText->find(
                "void GrhSIM_PackedActivityTop::eval_scan_1() {") ==
                std::string::npos ||
            secondShardText->find("execute_block_0") != std::string::npos ||
            secondShardText->find("eval_commit_") != std::string::npos ||
            secondShardText->find("active_byte_ref(2) & UINT8_C(0xfe)") ==
                std::string::npos ||
            secondShardText->find("active_byte_ref(4) & UINT8_C(0x3)") ==
                std::string::npos ||
            secondShardText->find("active_byte_ref(5") != std::string::npos ||
            lastShardText->find(
                "void GrhSIM_PackedActivityTop::eval_scan_7() {") ==
                std::string::npos ||
            lastShardText->find(
                "void GrhSIM_PackedActivityTop::eval_commit_7() {") ==
                std::string::npos ||
            lastShardText->find("active_byte_ref(14) & UINT8_C(0x80)") ==
                std::string::npos ||
            lastShardText->find("active_byte_ref(16) & UINT8_C(0x3)") ==
                std::string::npos ||
            // The commit phase scans the same way: byte 16's commit-owned
            // bit (Block 130) snapshots, clears, and bit-tests like any
            // compute chunk.
            lastShardText->find("active_byte_ref(16) & UINT8_C(0x4);") ==
                std::string::npos ||
            lastShardText->find("active_byte_ref(16) &= UINT8_C(0xfb);") ==
                std::string::npos ||
            lastShardText->find("active_byte_ref(17") != std::string::npos)
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
        if (headerText->find("kActivityWordCount = " +
                             std::to_string(activityWordCount)) == std::string::npos ||
            headerText->find("kCommitBlockBegin = " +
                             std::to_string(blockCount - 1) + ";") ==
                std::string::npos ||
            headerText->find("kCommitBlockEnd = " + std::to_string(blockCount) + ";") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kActivityWordCount> activeWords_{};") ==
                std::string::npos ||
            headerText->find("bool backwardFired_ = false;") == std::string::npos ||
            headerText->find("std::uint64_t roundCounter_ = 0;") == std::string::npos ||
            headerText->find("dirtyChangedResults_") == std::string::npos ||
            headerText->find("dirtyChangedBits_") == std::string::npos ||
            headerText->find("void execute_block_0();") == std::string::npos ||
            headerText->find("void eval_scan_0();") == std::string::npos ||
            headerText->find("void eval_scan_7();") == std::string::npos ||
            headerText->find("void eval_commit_7();") == std::string::npos ||
            headerText->find("void eval_commit_0();") != std::string::npos ||
            headerText->find("active_byte_ref(std::size_t byte)") ==
                std::string::npos ||
            headerText->find("void execute_block(") != std::string::npos ||
            headerText->find("nextActiveWords_") != std::string::npos ||
            headerText->find("activeSummary_") != std::string::npos ||
            headerText->find("SummaryWordCount") != std::string::npos ||
            headerText->find("activeWordBuffers_") != std::string::npos)
        {
            return fail("generated activity runtime did not use the single-bitmap round activity state");
        }
        // Persistent narrow values are independent v<VariableId> members (the
        // GSIM form), declared without initializers and zeroed by init()'s
        // member-region memset; only the cross-block changed results keep a
        // dense runtime-indexed array. In this fixture forwardChanged is the
        // sole cross-block changed result (dense id 0), so the dirty bitmap
        // shrinks to one word and v<forwardChanged> has no member declaration.
        // ST00010: the same-block detector events (inputChanged in B0,
        // backwardChanged in the commit Block) fold into block-local detector
        // group flags, so their event members disappear as well.
        const std::string forwardChangedMember =
            "std::uint64_t v" + std::to_string(fixture.forwardChanged.value) + ";";
        if (headerText->find(
                "static constexpr std::size_t kChangedResultCount = 1;") ==
                std::string::npos ||
            headerText->find(
                "static constexpr std::size_t kDirtyChangedWordCount = 1;") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kChangedResultCount> changedResults_{};") ==
                std::string::npos ||
            headerText->find("std::uint64_t v" +
                             std::to_string(fixture.input.value) + ";") ==
                std::string::npos ||
            headerText->find("std::uint64_t v" +
                             std::to_string(fixture.inputChanged.value) + ";") !=
                std::string::npos ||
            headerText->find("std::uint64_t v" +
                             std::to_string(fixture.backwardChanged.value) + ";") !=
                std::string::npos ||
            headerText->find(forwardChangedMember) != std::string::npos ||
            headerText->find("values_[") != std::string::npos ||
            headerText->find("> values_{};") != std::string::npos ||
            generatedSourceText.find("set_changed_result(0, ") == std::string::npos ||
            runtimeText->find("std::memset(&v" +
                              std::to_string(fixture.input.value) +
                              ", 0, sizeof(v") == std::string::npos ||
            runtimeText->find("    v" + std::to_string(fixture.input.value) +
                              " = static_cast<std::uint64_t>(activity_input) & ") ==
                std::string::npos ||
            runtimeText->find("forward_output = static_cast<bool>(v" +
                              std::to_string(fixture.forwardOutput.value) + ");") ==
                std::string::npos)
        {
            return fail("generated activity runtime did not use member-variable narrow value storage");
        }
        // The Makefile precompiles the member-heavy model header once (PCH)
        // and compiles every translation unit against it; the support header
        // stays a textual include dependency.
        const std::optional<std::string> makefileText =
            readTextFile(outputDirectory / "Makefile");
        if (!makefileText ||
            makefileText->find("PCH_HEADER := grhsim_PackedActivityTop.hpp") ==
                std::string::npos ||
            makefileText->find("PCH_FILE := $(PCH_HEADER).pch") == std::string::npos ||
            makefileText->find("$(PCH_FILE): $(PCH_HEADER)") == std::string::npos ||
            makefileText->find("-x c++-header") == std::string::npos ||
            makefileText->find("-include-pch $(PCH_FILE)") == std::string::npos ||
            makefileText->find("rm -f $(OBJS) $(LIB) $(PCH_FILE)") ==
                std::string::npos)
        {
            return fail("generated activity runtime Makefile did not precompile the model header");
        }
        if (runtimeText->find("activate_forward(") != std::string::npos ||
            runtimeText->find("activate_backward(") != std::string::npos ||
            countOccurrences(generatedSourceText, "set_changed_result(") < 1 ||
            runtimeText->find("dirtyChangedResults_") == std::string::npos ||
            runtimeText->find("dirtyChangedBits_") == std::string::npos ||
            runtimeText->find("execute_block(") != std::string::npos ||
            runtimeText->find("switch (block") != std::string::npos ||
            runtimeText->find("std::countr_zero") != std::string::npos ||
            runtimeText->find("if (block < 17)") != std::string::npos ||
            runtimeText->find("std::none_of(nextActive_.begin()") != std::string::npos ||
            runtimeText->find("for (std::size_t block = 1; block < active_.size();") !=
                std::string::npos)
        {
            return fail("generated activity runtime retained dense activation or changed-result paths");
        }
        // The eval loop is a static straight-line dispatch: B0 runs once per
        // eval, then each round calls the per-part compute scans in ascending
        // (source, part) order followed by the commit scans; the
        // double-buffered epoch advance and the commit activity channels must
        // not come back.
        const std::size_t firstScanCall =
            runtimeText->find("        eval_scan_0();\n");
        const std::size_t commitCall =
            runtimeText->find("        eval_commit_7();\n");
        if (runtimeText->find("activeWords_.fill(0);") == std::string::npos ||
            runtimeText->find("execute_block_0();") == std::string::npos ||
            runtimeText->find("backwardFired_ = false;") == std::string::npos ||
            firstScanCall == std::string::npos ||
            runtimeText->find("        eval_scan_7();\n") == std::string::npos ||
            commitCall == std::string::npos || commitCall < firstScanCall ||
            runtimeText->find("++roundCounter_;") == std::string::npos ||
            runtimeText->find("if (!backwardFired_) break;") == std::string::npos ||
            runtimeText->find("std::swap(activeWords_") != std::string::npos ||
            runtimeText->find("nextActiveWords_") != std::string::npos ||
            runtimeText->find("drain_next_active_activity") != std::string::npos ||
            runtimeText->find("drain_next_commit_activity") != std::string::npos ||
            runtimeText->find("nextCommitWords_") != std::string::npos ||
            runtimeText->find("execute_next_commit_group") != std::string::npos)
        {
            return fail("generated activity runtime did not use the two-phase round loop");
        }
        // Every act is emitted as constant-mask writes into the single active
        // bitmap, except forward targets in the same activity byte owned by
        // the emitting chunk, which relay into the scan-local byteFlags (see
        // the fixture's manyTargets: 66 and 67 relay, 68..73 stay global); an
        // act.b also raises backwardFired_. The forward Block activates the
        // commit Block through the gate detector's watched event
        // (forwardChanged, dense changed result 0 -> Block 130's mask 0x4).
        // This fixture opts into the compile-time runtime profile, so the
        // mask path also carries its counted profile statement (6 masked + 2
        // relayed = 8).
        if (headerText->find("static constexpr bool kRuntimeProfileCompiled = true;") ==
                std::string::npos ||
            headerText->find("profilePerBlockExecs_") == std::string::npos ||
            generatedSourceText.find("activeWords_[0] |= UINT64_C(0x20000);") ==
                std::string::npos ||
            generatedSourceText.find("activeWords_[1] |= UINT64_C(0x1);") ==
                std::string::npos ||
            generatedSourceText.find("activeWords_[2] |= UINT64_C(0x2);") ==
                std::string::npos ||
            generatedSourceText.find("byteFlags |= UINT8_C(0xc);") ==
                std::string::npos ||
            generatedSourceText.find("activeWords_[1] |= UINT64_C(0x3f0);") ==
                std::string::npos ||
            generatedSourceText.find("activeWords_[1] |= UINT64_C(0x3fc);") !=
                std::string::npos ||
            generatedSourceText.find("activeWords_[1] |= UINT64_C(0x2);") ==
                std::string::npos ||
            generatedSourceText.find("if ((changedResults_[0] != 0)) {\n"
                                     "                if (runtimeProfileEnabled_) profileActivateForward_ += 1;\n"
                                     "                activeWords_[2] |= UINT64_C(0x4);") ==
                std::string::npos ||
            generatedSourceText.find("backwardFired_ = true;") == std::string::npos ||
            generatedSourceText.find("profileActivateForward_ += 8;") ==
                std::string::npos ||
            generatedSourceText.find("profileActivateBackward_ += 1;") ==
                std::string::npos)
        {
            return fail("generated activity runtime did not use constant-mask activation writes");
        }
        // ST00010 detector-group folding: the B0 input detector accumulates
        // branchlessly into a block-local detGrp_0 flag (updating its private
        // old baseline as before) and merges once; the folded event variables
        // keep no member and no assignment. The commit merge still raises
        // backwardFired_.
        const std::string entryAccumulate =
            "bool detGrp_0 = (v" + std::to_string(fixture.input.value) + " != v" +
            std::to_string(fixture.inputOld.value) + ");";
        // Commit Block event gating: the head changed.any clone on the raw
        // event source runs on every activation and refreshes its baseline;
        // its result forms the single gate wrapping the write and the tail
        // watch traffic.
        const std::string commitGate =
            "if ((v" + std::to_string(fixture.commitForwardEvent.value) + " != 0)) {";
        // ST00013: the commit Block's state detector is fused into the
        // RegisterWrite site (write-point compare, store and raise only on a
        // real change); its group accumulator reads the write-point flag
        // instead of a tail compare, and the old baseline is gone.
        const std::string commitAccumulate = "bool detGrp_0 = wrChg_0;";
        if (generatedSourceText.find(entryAccumulate) == std::string::npos ||
            generatedSourceText.find(commitGate) == std::string::npos ||
            generatedSourceText.find(commitAccumulate) == std::string::npos ||
            generatedSourceText.find("bool wrChg_0 = false;") == std::string::npos ||
            generatedSourceText.find("wrChg_0 = true;") == std::string::npos ||
            generatedSourceText.find("if (detGrp_0) {") == std::string::npos ||
            generatedSourceText.find("profileActivateForward_ += 2;") ==
                std::string::npos ||
            generatedSourceText.find("v" + std::to_string(fixture.inputOld.value) +
                                     " = v" + std::to_string(fixture.input.value) +
                                     ";") == std::string::npos)
        {
            return fail("generated activity runtime did not fold detector groups");
        }
        // Per-Block profile counters move into the dispatch form: scan Blocks
        // count at the top of their bit-test branch, the commit Block counts
        // inside its own activity bit test (commit Blocks are
        // activation-filtered like every other Block), and B0 counts only its
        // per-Block entry. NO0010: each counted site also wraps the fired body
        // in an rdtsc pair (profileBlockT0 local + profilePerBlockCycles_
        // accumulation). The scan itself is straight-line: no switch,
        // countr_zero, or do-while.
        if (generatedSourceText.find(
                "if ((byteFlags & UINT8_C(0x1)) != 0) {\n"
                "                std::uint64_t profileBlockT0 = 0;\n"
                "                if (runtimeProfileEnabled_) { profilePerBlockExecs_[64] += 1; ++profileBlockExecs_; profileBlockT0 = wolvrixAmRdtsc(); }") ==
                std::string::npos ||
            generatedSourceText.find(
                "if (runtimeProfileEnabled_) { profilePerBlockCycles_[64] += wolvrixAmRdtsc() - profileBlockT0; }") ==
                std::string::npos ||
            generatedSourceText.find(
                "if ((byteFlags & UINT8_C(0x4)) != 0) {\n"
                "                std::uint64_t profileBlockT0 = 0;\n"
                "                if (runtimeProfileEnabled_) { profilePerBlockExecs_[130] += 1; ++profileCommitBlockExecs_; profileBlockT0 = wolvrixAmRdtsc(); }") ==
                std::string::npos ||
            generatedSourceText.find(
                "::execute_block_0() {\n"
                "    std::uint64_t profileBlockT0 = 0;\n"
                "    if (runtimeProfileEnabled_) { profilePerBlockExecs_[0] += 1; profileBlockT0 = wolvrixAmRdtsc(); }") ==
                std::string::npos ||
            generatedSourceText.find("std::countr_zero") != std::string::npos ||
            generatedSourceText.find("do {") != std::string::npos ||
            generatedSourceText.find("switch (block") != std::string::npos)
        {
            return fail("generated activity runtime misplaced the per-Block profile counters");
        }
        for (std::size_t source = 0; source < 8; ++source)
        {
            const std::string call =
                "        eval_scan_" + std::to_string(source) + "();\n";
            if (runtimeText->find(call) == std::string::npos)
            {
                return fail("generated activity runtime omitted a shard scan call");
            }
        }
        // Changed results are never statically cleared: same-Block ones are
        // plain members rewritten before every read, and the cross-Block one
        // (forwardChanged, dense id 0) is cleared only through the round-end
        // dirty list, whose clear loop indexes changedResults_ by the runtime
        // dirty id rather than a compile-time constant.
        const std::string legacyInputClear =
            "v" + std::to_string(fixture.inputChanged.value) + " = 0;";
        const std::string legacyBackwardClear =
            "v" + std::to_string(fixture.backwardChanged.value) + " = 0;";
        if (runtimeText->find(legacyInputClear) != std::string::npos ||
            runtimeText->find(legacyBackwardClear) != std::string::npos ||
            runtimeText->find("changedResults_[0] = 0;") != std::string::npos)
        {
            return fail("generated activity runtime statically clears every changed result");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_PackedActivityTop.hpp"
int main()
{
    static_assert(GrhSIM_PackedActivityTop::kRuntimeProfileCompiled);
    GrhSIM_PackedActivityTop model;
    model.set_runtime_profile_enabled(true);
    if (!model.runtime_profile_enabled())
        return 5;
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
    model.dump_runtime_profile();
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
        const VariableId commitEvent = addOutput(u1Type, "commit_event");
        const VariableId clockOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId entryEvent = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId posedgeOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId posedge = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId nextCommitCount =
            linear.addVariable(u8Type, linear.zeroInit());
        const VariableId stateOld = linear.addVariable(u8Type, linear.undefInit());
        const VariableId stateChanged = linear.addVariable(u1Type, linear.zeroInit());

        const std::array<uint64_t, 1> oneWords = {1};
        const VariableId oneValue = addBitConstant(linear, u8Type, oneWords);
        const VariableId commitClockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId commitPosedge =
            linear.addVariable(u1Type, linear.zeroInit());

        const InstructionId watchClock =
            addInstruction(Opcode::ChangedAny, {entryEvent}, {clock, clockOld});
        const InstructionId detectPosedge =
            addInstruction(Opcode::ChangedPos, {posedge}, {clock, posedgeOld});
        const InstructionId addCommitCount =
            addInstruction(Opcode::Add, {nextCommitCount}, {commitCount, oneValue});
        const InstructionId sampleState =
            addInstruction(Opcode::Assign, {sampledState}, {state});
        const InstructionId sampleCommitEvent =
            addInstruction(Opcode::Assign, {commitEvent}, {posedge});
        // Commit head gate detector: the changed.pos clone on the clock. Both
        // writes are unconditional in their nextValue (constant cond, full
        // mask), so nextValue is the data operand directly.
        const InstructionId gateDetect = addInstruction(
            Opcode::ChangedPos, {commitPosedge}, {clock, commitClockOld});
        const InstructionId writeState = addInstruction(
            Opcode::RegisterWrite, {}, {payload, state, commitPosedge});
        const InstructionId writeCount = addInstruction(
            Opcode::RegisterWrite, {}, {nextCommitCount, commitCount, commitPosedge});
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
        const auto addBlock = [&](std::initializer_list<InstructionId> instructions) {
            scheduled.addBlock(std::span<const InstructionId>(instructions.begin(),
                                                               instructions.size()));
        };

        const InstructionId enterClock =
            addScheduledInstruction(Opcode::ActForward, {entryEvent});
        const InstructionId activateReaders =
            addScheduledInstruction(Opcode::ActBackward, {stateChanged});
        // The entry clock watch activates both the compute chain and the
        // commit Block (through its gate detector's watched clock source).
        scheduled.setActivationTargets(enterClock, std::array{BlockId{1}, BlockId{3}});
        scheduled.setActivationTargets(activateReaders, std::array{BlockId{2}});
        addBlock({watchClock, enterClock});
        addBlock({detectPosedge, addCommitCount});
        addBlock({sampleState, sampleCommitEvent});
        // Commit Block: activation-driven like every Block; the head
        // changed.pos clone gates the writes, and the tail same-Block
        // detector reactivates the reader compute Block through act.b.
        addBlock({gateDetect, writeState, writeCount, detectState, activateReaders});

        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = std::move(interface),
            .commitBlockBegin = 3,
            .commitBlockEnd = 4,
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
        if (headerText->find("static constexpr std::size_t kCommitBlockBegin = 3;") ==
                std::string::npos ||
            headerText->find("static constexpr std::size_t kCommitBlockEnd = 4;") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kActivityWordCount> activeWords_{};") ==
                std::string::npos ||
            headerText->find("bool backwardFired_ = false;") == std::string::npos ||
            headerText->find(
                "void set_changed_result(std::size_t variable, bool event) {\n"
                "        changedResults_[variable] = event ? 1 : 0;\n"
                "        if (event) mark_changed_result(variable);\n"
                "    }") == std::string::npos ||
            headerText->find(
                "void mark_changed_result(std::size_t variable);") ==
                std::string::npos ||
            headerText->find("void clear_changed_results();") ==
                std::string::npos ||
            headerText->find(
                "static constexpr std::uint64_t bit_mask(std::uint32_t width) {") ==
                std::string::npos ||
            headerText->find(
                "static constexpr std::uint64_t resize_value(") ==
                std::string::npos ||
            headerText->find(
                "static constexpr std::uint64_t concat_value(") ==
                std::string::npos ||
            headerText->find("CommitEvent") != std::string::npos ||
            headerText->find("commitGroup") != std::string::npos ||
            headerText->find("commitBlockOrder") != std::string::npos ||
            headerText->find("preCommitSnapshot") != std::string::npos ||
            runtimeText->find(
                "::mark_changed_result(std::size_t variable) {") ==
                std::string::npos ||
            runtimeText->find("::clear_changed_results() {") ==
                std::string::npos ||
            runtimeText->find("::set_changed_result(") != std::string::npos ||
            runtimeText->find("::bit_mask(") != std::string::npos ||
            runtimeText->find("::resize_value(") != std::string::npos ||
            runtimeText->find("::concat_value(") != std::string::npos ||
            evalBody.find("execute_block_0();") == std::string_view::npos ||
            evalBody.find("        eval_scan_0();\n        eval_commit_0();\n") ==
                std::string_view::npos ||
            evalBody.find("execute_block(") != std::string_view::npos ||
            evalBody.find("switch (block") != std::string_view::npos ||
            evalBody.find("std::countr_zero") != std::string_view::npos ||
            evalBody.find("++roundCounter_;") == std::string_view::npos ||
            evalBody.find("if (!backwardFired_) break;") ==
                std::string_view::npos ||
            evalBody.find("execute_next_commit_group") != std::string_view::npos ||
            evalBody.find("capture_pending_commit_operands") !=
                std::string_view::npos ||
            runtimeText->find("pendingCommitWords_") != std::string::npos ||
            runtimeText->find("forcedCommitWords_") != std::string::npos ||
            runtimeText->find("set_commit_changed_result") != std::string::npos ||
            blockText->find("set_changed_result(0, ") == std::string::npos)
        {
            return fail("AM C++ emitter did not generate the two-phase round commit runtime");
        }

        // Narrow-value storage: persistent scalars are independent
        // v<VariableId> members without initializers (zeroed by init()'s
        // member-region memset); the sole cross-Block changed result
        // (posedge, VariableId 9) moves into the dense runtime-indexed
        // changedResults_ array, so v9 has no member.
        if (headerText->find(
                "static constexpr std::size_t kChangedResultCount = 1;") ==
                std::string::npos ||
            headerText->find(
                "std::array<std::uint64_t, kChangedResultCount> changedResults_{};") ==
                std::string::npos ||
            headerText->find("std::uint64_t v0;") == std::string::npos ||
            headerText->find("std::uint64_t v9;") != std::string::npos ||
            headerText->find("values_[") != std::string::npos ||
            headerText->find("> values_{};") != std::string::npos ||
            blockText->find("resize_value(changedResults_[0], ") == std::string::npos ||
            runtimeText->find("std::memset(&v0, 0, sizeof(v0) * ") ==
                std::string::npos ||
            runtimeText->find("changedResults_[variable] = 0;") == std::string::npos ||
            runtimeText->find(
                "    v0 = static_cast<std::uint64_t>(clock) & ") == std::string::npos)
        {
            return fail("AM C++ emitter did not use member-variable narrow value storage");
        }

        // The runtime profile is a compile-time switch and defaults to off: no
        // counters, no hot-path profile branches, and the host profile API
        // degrades to no-op stubs (see testPackedActivityRuntime for the on state).
        if (headerText->find("static constexpr bool kRuntimeProfileCompiled = false;") ==
                std::string::npos ||
            headerText->find("runtimeProfileEnabled_") != std::string::npos ||
            headerText->find("profilePerBlockExecs_") != std::string::npos ||
            runtimeText->find("runtimeProfileEnabled_") != std::string::npos ||
            blockText->find("runtimeProfileEnabled_") != std::string::npos ||
            runtimeText->find(
                "::set_runtime_profile_enabled(bool enabled) { (void)enabled; }") ==
                std::string::npos ||
            runtimeText->find("::runtime_profile_enabled() const { return false; }") ==
                std::string::npos ||
            runtimeText->find("::dump_runtime_profile() const {}") ==
                std::string::npos)
        {
            return fail("AM C++ emitter kept runtime profile code with the switch off");
        }

        // The static straight-line dispatch carries the compute Blocks in
        // eval_scan_0 (one byte chunk owning Blocks 1 and 2), the entry Block
        // in execute_block_0, and the commit Block in eval_commit_0, which is
        // activation-scanned over byte 0's commit-owned bit (Block 3, 0x8)
        // exactly like a compute chunk.
        if (blockText->find("void GrhSIM_PhasedCommitTop::execute_block_0() {") ==
                std::string::npos ||
            blockText->find("void GrhSIM_PhasedCommitTop::eval_scan_0() {") ==
                std::string::npos ||
            blockText->find("active_byte_ref(0) & UINT8_C(0x6)") ==
                std::string::npos ||
            blockText->find("active_byte_ref(0) &= UINT8_C(0xf9)") ==
                std::string::npos ||
            blockText->find(
                "            if ((byteFlags & UINT8_C(0x2)) != 0) {") ==
                std::string::npos ||
            blockText->find(
                "            if ((byteFlags & UINT8_C(0x4)) != 0) {") ==
                std::string::npos ||
            blockText->find("void GrhSIM_PhasedCommitTop::eval_commit_0() {") ==
                std::string::npos ||
            blockText->find("active_byte_ref(0) & UINT8_C(0x8);") ==
                std::string::npos ||
            blockText->find("active_byte_ref(0) &= UINT8_C(0xf7);") ==
                std::string::npos ||
            blockText->find(
                "            if ((byteFlags & UINT8_C(0x8)) != 0) {") ==
                std::string::npos ||
            blockText->find("switch") != std::string::npos ||
            blockText->find("    case ") != std::string::npos)
        {
            return fail("AM C++ emitter did not split the static scan and commit dispatch");
        }

        // Commit event gating: the head changed.pos clone on the clock runs
        // on every activation (refreshing its baseline), and its result is
        // the single gate wrapping both state writes and the tail watch; the
        // entry clock watch activates the commit Block (mask 0xa = Blocks 1
        // and 3) because the gate detector watches its clock source. ST00013
        // still fuses the tail state detector into the RegisterWrite site.
        if (blockText->find("bool detGrp_0 = (v0 != v6);") == std::string::npos ||
            blockText->find("activeWords_[0] |= UINT64_C(0xa);") ==
                std::string::npos ||
            blockText->find("if ((v15 != 0)) {") == std::string::npos ||
            blockText->find("bool wrChg_0 = false;") == std::string::npos ||
            blockText->find("bool detGrp_0 = wrChg_0;") == std::string::npos)
        {
            return fail("AM C++ emitter did not gate the commit Block on its head detector");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_PhasedCommitTop.hpp"
int main()
{
    static_assert(!GrhSIM_PhasedCommitTop::kRuntimeProfileCompiled);
    GrhSIM_PhasedCommitTop model;
    model.set_runtime_profile_enabled(true);
    if (model.runtime_profile_enabled())
        return 6;
    model.dump_runtime_profile();
    model.init();
    model.clock = 0;
    model.payload = 0x5a;
    model.eval();
    if (model.state != 0 || model.sampled_state != 0 || model.commit_count != 0 ||
        model.commit_event != 0)
        return 1;
    model.clock = 1;
    model.eval();
    // The posedge gates both writes in the same round; the reader Block
    // re-samples through act.b in the following round.
    if (model.state != 0x5a || model.sampled_state != 0x5a ||
        model.commit_count != 1 || model.commit_event != 0)
        return 2;
    model.eval();
    // No fresh edge: the round-local event was cleared, nothing re-fires.
    if (model.state != 0x5a || model.sampled_state != 0x5a ||
        model.commit_count != 1 || model.commit_event != 0)
        return 3;
    model.clock = 0;
    model.eval();
    if (model.state != 0x5a || model.sampled_state != 0x5a ||
        model.commit_count != 1 || model.commit_event != 0)
        return 4;
    model.payload = 0x33;
    model.clock = 1;
    model.eval();
    if (model.state != 0x33 || model.sampled_state != 0x33 ||
        model.commit_count != 2 || model.commit_event != 0)
        return 5;
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
        const VariableId nextA = builder.addVariable(type, builder.undefInit());
        const VariableId nextB = builder.addVariable(type, builder.undefInit());
        const VariableId nextC = builder.addVariable(type, builder.undefInit());
        const VariableId nextD = builder.addVariable(type, builder.undefInit());
        const VariableId outputA = builder.addVariable(type, builder.undefInit());
        const VariableId outputB = builder.addVariable(type, builder.undefInit());
        const VariableId outputC = builder.addVariable(type, builder.undefInit());
        const VariableId outputD = builder.addVariable(type, builder.undefInit());

        // Merged nextValue form: each write's update condition is folded into
        // a mux against the current state, and the write's event operands
        // list exactly the sources whose change can require a re-evaluation
        // (they become the commit Block's gate detectors).
        builder.addInstruction(Opcode::Assign, std::array{guardB}, std::array{stateA});
        builder.addInstruction(Opcode::Mux, std::array{nextB},
                               std::array{guardB, one, stateB});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{nextB, stateB, guardB});
        builder.addInstruction(Opcode::Assign, std::array{guardC}, std::array{stateB});
        builder.addInstruction(Opcode::Mux, std::array{nextC},
                               std::array{guardC, one, stateC});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{nextC, stateC, guardC});
        builder.addInstruction(Opcode::LogicOr, std::array{dataA},
                               std::array{stateC, one});
        builder.addInstruction(Opcode::Mux, std::array{nextA},
                               std::array{start, dataA, stateA});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{nextA, stateA, start, dataA});
        builder.addInstruction(Opcode::Mux, std::array{nextD},
                               std::array{start, guardB, stateD});
        builder.addInstruction(Opcode::RegisterWrite, {},
                               std::array{nextD, stateD, start, guardB});
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
            VariableRole::None,
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
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
            InstructionEffect::Pure,
            InstructionEffect::StateReadWrite,
            InstructionEffect::Pure,
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
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            AmGraph::fromLinearProgram(makeProductionCommitCycleProgram()),
            ActivityScheduleOptions{
                .maxAtomsPerBlock = 1,
                .maxCommitAtomsPerBlock = 1,
                .enableCoarsening = false,
            },
            diagnostics);
        if (!model || diagnostics.hasError() || model->commitBlockBegin == 0 ||
            model->commitBlockEnd != model->program.blockCount() ||
            model->commitBlockEnd - model->commitBlockBegin != 4)
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
    // The guard chain propagates through act.b reactivation until the eval
    // reaches a fixed point, so state_d settles within this eval.
    if (model.state_a != 1 || model.state_b != 1 || model.state_c != 1 ||
        model.state_d != 1)
        return 2;
    model.eval();
    if (model.state_a != 1 || model.state_b != 1 || model.state_c != 1 ||
        model.state_d != 1)
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
            return fail("generated cyclic commit fixture did not reach the chained fixed point");
        }
        return 0;
    }

    // reg.write.c / mem.write.c / mem.write.cm in one commit Block: the cond
    // variants must emit an `if (cond)` gate around the write, the no-mask
    // mem variant a plain whole-element assign_words, and the constant-mask
    // variant a single-word read-modify-write inline (NO0018 unrolls constant
    // masks touching few words instead of calling masked_write_words).
    ExecutableModel makeCondGateEmitterModel()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
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

        const VariableId clock = addInput(u1Type, "clock");
        const VariableId cond = addInput(u1Type, "cond");
        const VariableId data = addInput(u8Type, "data");
        const VariableId state = addOutput(u8Type, "state");
        const VariableId address = linear.addVariable(u8Type, linear.zeroInit());
        const VariableId memory = linear.addVariable(memoryType, linear.zeroInit());
        const VariableId clockOld = linear.addVariable(u1Type, linear.undefInit());
        const VariableId clockEvent = linear.addVariable(u1Type, linear.zeroInit());
        const VariableId commitClockOld =
            linear.addVariable(u1Type, linear.undefInit());
        const VariableId clockPos = linear.addVariable(u1Type, linear.zeroInit());
        const std::array<uint64_t, 1> maskWords = {0xf0};
        const VariableId maskConst = linear.addVariable(
            u8Type,
            linear.addConstantInit(linear.addBitLiteral(u8Type, maskWords)));

        const InstructionId watchClock =
            addInstruction(Opcode::ChangedAny, {clockEvent}, {clock, clockOld});
        const InstructionId gateDetect =
            addInstruction(Opcode::ChangedPos, {clockPos}, {clock, commitClockOld});
        const InstructionId writeReg = addInstruction(
            Opcode::RegisterWriteCond, {}, {cond, data, state, clockPos});
        const InstructionId writeMem = addInstruction(
            Opcode::MemoryWriteCond, {}, {cond, address, data, memory, clockPos});
        const InstructionId writeMemCm =
            addInstruction(Opcode::MemoryWriteCondMask, {},
                           {cond, address, maskConst, data, memory, clockPos});

        ScheduledProgramBuilder scheduled(linear.finish());
        const auto addScheduled = [&](Opcode opcode,
                                      std::initializer_list<VariableId> operands) {
            return scheduled.addInstruction(
                opcode, {},
                std::span<const VariableId>(operands.begin(), operands.size()));
        };
        const InstructionId enterClock = addScheduled(Opcode::ActForward, {clockEvent});
        scheduled.setActivationTargets(enterClock, std::array{BlockId{1}});
        scheduled.addBlock(std::array{watchClock, enterClock});
        scheduled.addBlock(std::array{gateDetect, writeReg, writeMem, writeMemCm});

        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = std::move(interface),
            .commitBlockBegin = 1,
            .commitBlockEnd = 2,
        };
    }

    int testCondGatedWriteEmission(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ExecutableModel model = makeCondGateEmitterModel();
        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "CondGateTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the cond-gated model");
        }
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_CondGateTop_blocks_0.cpp");
        if (!blocksText)
        {
            return fail("AM C++ emitter produced no cond-gated blocks source");
        }
        const ProgramView program = model.program.view();
        VariableId cond;
        VariableId data;
        VariableId state;
        for (const PortBinding &port : model.interface.ports)
        {
            const std::string_view name = program.string(port.name);
            if (port.direction == PortDirection::Input && name == "cond")
            {
                cond = port.input;
            }
            else if (port.direction == PortDirection::Input && name == "data")
            {
                data = port.input;
            }
            else if (port.direction == PortDirection::Output && name == "state")
            {
                state = port.output;
            }
        }
        if (!cond.valid() || !data.valid() || !state.valid())
        {
            return fail("cond-gated model lost its interface bindings");
        }
        // reg.write.c: the plain store is wrapped in an if (cond) gate.
        const std::string gatedStore = "if ((v" + std::to_string(cond.value) +
                                       " != 0)) { v" + std::to_string(state.value) +
                                       " = v" + std::to_string(data.value) + " & ";
        if (blocksText->find(gatedStore) == std::string::npos ||
            countOccurrences(*blocksText, "assign_words(") != 1 ||
            countOccurrences(*blocksText, "masked_write_words(") != 0 ||
            blocksText->find("& ~UINT64_C(0xf0)") == std::string::npos ||
            blocksText->find("UINT64_C(0xf0)") == std::string::npos)
        {
            return fail("AM C++ emitter did not emit the cond-gated write forms");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_CondGateTop.hpp"
int main()
{
    GrhSIM_CondGateTop model;
    model.init();
    model.clock = 0;
    model.cond = 0;
    model.data = 0xa5;
    model.eval();
    if (model.state != 0) return 1;
    model.clock = 1;
    model.eval();
    // posedge with cond=0: the gated write stays dormant.
    if (model.state != 0) return 2;
    model.cond = 1;
    model.clock = 0;
    model.eval();
    // negedge: no commit.
    if (model.state != 0) return 3;
    model.clock = 1;
    model.eval();
    if (model.state != 0xa5) return 4;
    model.data = 0x5a;
    model.eval();
    // No fresh edge: nothing re-fires.
    if (model.state != 0xa5) return 5;
    model.clock = 0;
    model.eval();
    model.clock = 1;
    model.eval();
    if (model.state != 0x5a) return 6;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the cond-gated model harness");
        }
        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated cond-gated model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_CondGateTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated cond-gated model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated cond-gated model violated the cond gating semantics");
        }
        return 0;
    }

    // NO0008 block-level same-select mux fusion: one compute Block holds a
    // Tree atom (cone member + mux root) followed by two singleton muxes on
    // the same select (one chained), a singleton assign separator, two wide
    // singleton muxes on the same select, and a lone different-select mux.
    ExecutableModel makeMuxRunEmitterModel()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId u72Type = linear.addType(Type::bitVector(72));

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
            const VariableId variable = linear.addVariable(type, linear.undefInit());
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

        const VariableId sel = addInput(u1Type, "sel");
        const VariableId sel2 = addInput(u1Type, "sel2");
        const VariableId a = addInput(u8Type, "a");
        const VariableId b = addInput(u8Type, "b");
        const VariableId c = addInput(u8Type, "c");
        const VariableId d = addInput(u8Type, "d");
        const VariableId wideA = addInput(u72Type, "wide_a");
        const VariableId wideB = addInput(u72Type, "wide_b");
        const VariableId r1 = addOutput(u8Type, "r1");
        const VariableId r2 = addOutput(u8Type, "r2");
        const VariableId r3 = addOutput(u8Type, "r3");
        const VariableId w2 = addOutput(u72Type, "w2");
        const VariableId mo = addOutput(u8Type, "mo");
        const VariableId tmp = linear.addVariable(u8Type, linear.undefInit());
        const VariableId w1 = linear.addVariable(u72Type, linear.undefInit());
        const VariableId cone = linear.addVariable(u8Type, linear.undefInit());

        const InstructionId coneAnd = addInstruction(Opcode::And, {cone}, {a, d});
        const InstructionId mux1 = addInstruction(Opcode::Mux, {r1}, {sel, cone, b});
        const InstructionId mux2 = addInstruction(Opcode::Mux, {r2}, {sel, c, r1});
        const InstructionId mux3 = addInstruction(Opcode::Mux, {r3}, {sel, a, d});
        const InstructionId separator = addInstruction(Opcode::Assign, {tmp}, {a});
        const InstructionId wide1 = addInstruction(Opcode::Mux, {w1}, {sel, wideA, wideB});
        const InstructionId wide2 = addInstruction(Opcode::Mux, {w2}, {sel, wideB, wideA});
        const InstructionId other = addInstruction(Opcode::Mux, {mo}, {sel2, a, b});

        const std::array<VariableId, 8> watched = {sel, sel2, a, b, c, d, wideA, wideB};
        ScheduledProgramBuilder scheduled(linear.finish());
        std::vector<InstructionId> entry;
        for (VariableId input : watched)
        {
            const TypeId type = scheduled.view().variable(input).type;
            const VariableId oldValue = scheduled.addVariable(type, scheduled.undefInit());
            const VariableId inputChanged =
                scheduled.addVariable(u1Type, scheduled.zeroInit());
            entry.push_back(scheduled.addInstruction(
                Opcode::ChangedAny, std::array{inputChanged}, std::array{input, oldValue}));
            const InstructionId activate = scheduled.addInstruction(
                Opcode::ActForward, {}, std::array{inputChanged});
            scheduled.setActivationTargets(activate, std::array{BlockId{1}});
            entry.push_back(activate);
        }
        scheduled.addBlock(std::span<const InstructionId>(entry.data(), entry.size()));
        // One Tree atom (cone first, mux root last) followed by two
        // singleton muxes on the same select, a singleton assign separator,
        // two wide singleton muxes, and a singleton different-select mux at
        // the tail. Run fusion derives the select from each root
        // instruction, so implicit singleton atoms are eligible.
        scheduled.beginBlock();
        scheduled.beginAtom(AmAtomKind::Tree, sel.value, 42);
        scheduled.appendBlockInstruction(coneAnd);
        scheduled.appendBlockInstruction(mux1);
        scheduled.endAtom();
        scheduled.appendBlockInstruction(mux2);
        scheduled.appendBlockInstruction(mux3);
        scheduled.appendBlockInstruction(separator);
        scheduled.appendBlockInstruction(wide1);
        scheduled.appendBlockInstruction(wide2);
        scheduled.appendBlockInstruction(other);
        scheduled.endBlock();

        return ExecutableModel{
            .program = scheduled.finish(),
            .interface = std::move(interface),
            .commitBlockBegin = 0,
            .commitBlockEnd = 0,
        };
    }

    int testMuxRunFusion(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ExecutableModel model = makeMuxRunEmitterModel();
        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "MuxRunTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the mux-run model");
        }
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_MuxRunTop_blocks_0.cpp");
        if (!blocksText)
        {
            return fail("AM C++ emitter produced no mux-run blocks source");
        }
        const ProgramView program = model.program.view();
        const auto findPort = [&](std::string_view name) {
            for (const PortBinding &port : model.interface.ports)
            {
                if (program.string(port.name) == name)
                {
                    return port.direction == PortDirection::Input ? port.input : port.output;
                }
            }
            return VariableId::invalid();
        };
        const VariableId sel = findPort("sel");
        const VariableId sel2 = findPort("sel2");
        const VariableId a = findPort("a");
        const VariableId c = findPort("c");
        const VariableId d = findPort("d");
        const VariableId r1 = findPort("r1");
        const VariableId r2 = findPort("r2");
        if (!sel.valid() || !sel2.valid() || !a.valid() || !c.valid() || !d.valid() ||
            !r1.valid() || !r2.valid())
        {
            return fail("mux-run model lost its interface bindings");
        }
        // Two fused if/else structures share one select evaluation each: one
        // for the narrow run, one for the wide run. The trailing space
        // after '{' keeps activation-merge ifs out of the count. The cone
        // member (a & d) must be emitted before the fused gate it feeds, and
        // the singleton different-select mux keeps the plain ternary form
        // (a single-arm run never fuses).
        const std::string fusedGate = "if ((v" + std::to_string(sel.value) + " != 0)) { ";
        const std::string singletonGate = "if ((v" + std::to_string(sel2.value) + " != 0)) { ";
        const std::string chainTrue = "v" + std::to_string(r2.value) +
                                      " = (resize_value(v" + std::to_string(c.value) +
                                      ", 8, false, 8))";
        const std::string chainFalse = "v" + std::to_string(r2.value) +
                                       " = (resize_value(v" + std::to_string(r1.value) +
                                       ", 8, false, 8))";
        const std::string coneAssign = "resize_value(v" + std::to_string(a.value) +
                                       ", 8, false, 8) & resize_value(v" +
                                       std::to_string(d.value) + ", 8, false, 8)";
        const std::string loneTernary = "= ((v" + std::to_string(sel2.value) + " != 0) ? ";
        const std::size_t firstGate = blocksText->find(fusedGate);
        const std::size_t conePosition = blocksText->find(coneAssign);
        if (firstGate == std::string::npos ||
            countOccurrences(*blocksText, fusedGate) != 2 ||
            blocksText->find(chainTrue) == std::string::npos ||
            blocksText->find(chainFalse) == std::string::npos ||
            conePosition == std::string::npos || conePosition > firstGate ||
            countOccurrences(*blocksText, singletonGate) != 0 ||
            countOccurrences(*blocksText, "assign_words(") < 4 ||
            blocksText->find(loneTernary) == std::string::npos)
        {
            return fail("AM C++ emitter did not fuse the mux-rooted atom runs");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_MuxRunTop.hpp"
int main()
{
    GrhSIM_MuxRunTop model;
    model.init();
    model.sel = 0;
    model.sel2 = 0;
    model.a = 0x11;
    model.b = 0x22;
    model.c = 0x33;
    model.d = 0x44;
    model.wide_a = {UINT64_C(0xaaaabbbbccccdddd), UINT64_C(0x12)};
    model.wide_b = {UINT64_C(0x1111222233334444), UINT64_C(0x56)};
    model.eval();
    if (model.r1 != 0x22 || model.r2 != 0x22 || model.r3 != 0x44 || model.mo != 0x22)
        return 1;
    if (model.w2[0] != UINT64_C(0xaaaabbbbccccdddd) || model.w2[1] != UINT64_C(0x12))
        return 2;
    model.sel = 1;
    model.sel2 = 1;
    model.eval();
    // cone = a & d = 0x00 feeds r1's true arm inside the fused if/else.
    if (model.r1 != 0x00 || model.r2 != 0x33 || model.r3 != 0x11 || model.mo != 0x11)
        return 3;
    if (model.w2[0] != UINT64_C(0x1111222233334444) || model.w2[1] != UINT64_C(0x56))
        return 4;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the mux-run model harness");
        }
        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated mux-run model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_MuxRunTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated mux-run model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated mux-run model violated the fused select semantics");
        }
        return 0;
    }

    // NO0006 trace comments: the emitter annotates block sources with
    // per-block banners and per-atom provenance comments by default;
    // GrhSimAmCppOptions::traceComments = false turns them off.
    int testTraceComments(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ExecutableModel model = makeMuxRunEmitterModel();
        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "TraceTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the trace-comment model");
        }
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_TraceTop_blocks_0.cpp");
        if (!blocksText)
        {
            return fail("AM C++ emitter produced no trace-comment blocks source");
        }
        if (blocksText->find("// ===== block 0 role=entry atoms=") == std::string::npos ||
            blocksText->find("// ===== block 1 role=compute atoms=") == std::string::npos ||
            blocksText->find("// --- atom ") == std::string::npos ||
            blocksText->find("kind=Tree gsim_node=42 ---") == std::string::npos ||
            blocksText->find("kind=Singleton gsim_node=-1 ---") == std::string::npos)
        {
            return fail("AM C++ emitter did not emit the default trace comments");
        }

        std::filesystem::remove_all(outputDirectory);
        wolvrix::lib::diag::Diagnostics offDiagnostics;
        const GrhSimAmCppResult offResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "TraceTop",
                .maxOutputFileBytes = 1024 * 1024,
                .traceComments = false,
            },
            offDiagnostics);
        if (!offResult.success || offDiagnostics.hasError())
        {
            return fail("AM C++ emitter failed with trace comments disabled");
        }
        const std::optional<std::string> plainText =
            readTextFile(outputDirectory / "grhsim_TraceTop_blocks_0.cpp");
        if (!plainText)
        {
            return fail("AM C++ emitter produced no blocks source with comments disabled");
        }
        if (plainText->find("// ===== block ") != std::string::npos ||
            plainText->find("// --- atom ") != std::string::npos)
        {
            return fail("AM C++ emitter emitted trace comments despite traceComments=false");
        }
        return 0;
    }

    // B2 branchy-mux emission (emit-cost NO0001): with the "branchyMux"
    // attribute on, every scalar Mux emits an if/else arm assignment instead
    // of a ternary, so block bodies split into small basic blocks. Same model
    // and semantics as testMuxRunFusion; the fused runs keep their if/else
    // form, the different-select singleton mux joins them, and no narrow
    // ternary may remain in the file.
    int testBranchyMux(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ExecutableModel model = makeMuxRunEmitterModel();
        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "BranchyTop",
                .maxOutputFileBytes = 1024 * 1024,
                .attributes = {{"branchyMux", "true"}},
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the branchy-mux model");
        }
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_BranchyTop_blocks_0.cpp");
        if (!blocksText)
        {
            return fail("AM C++ emitter produced no branchy-mux blocks source");
        }
        const ProgramView program = model.program.view();
        const auto findPort = [&](std::string_view name) {
            for (const PortBinding &port : model.interface.ports)
            {
                if (program.string(port.name) == name)
                {
                    return port.direction == PortDirection::Input ? port.input : port.output;
                }
            }
            return VariableId::invalid();
        };
        const VariableId sel2 = findPort("sel2");
        if (!sel2.valid())
        {
            return fail("branchy-mux model lost its interface bindings");
        }
        // The fused runs contribute two if/else gates on sel; the
        // different-select singleton mux must add one more gate on sel2, and
        // no narrow ternary may remain anywhere in the file.
        const std::string singletonGate = "if ((v" + std::to_string(sel2.value) + " != 0)) { ";
        if (countOccurrences(*blocksText, singletonGate) != 1 ||
            countOccurrences(*blocksText, " != 0) ? ") != 0)
        {
            return fail("AM C++ emitter did not branch the scalar mux form");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_BranchyTop.hpp"
int main()
{
    GrhSIM_BranchyTop model;
    model.init();
    model.sel = 0;
    model.sel2 = 0;
    model.a = 0x11;
    model.b = 0x22;
    model.c = 0x33;
    model.d = 0x44;
    model.wide_a = {UINT64_C(0xaaaabbbbccccdddd), UINT64_C(0x12)};
    model.wide_b = {UINT64_C(0x1111222233334444), UINT64_C(0x56)};
    model.eval();
    if (model.r1 != 0x22 || model.r2 != 0x22 || model.r3 != 0x44 || model.mo != 0x22)
        return 1;
    if (model.w2[0] != UINT64_C(0xaaaabbbbccccdddd) || model.w2[1] != UINT64_C(0x12))
        return 2;
    model.sel = 1;
    model.sel2 = 1;
    model.eval();
    if (model.r1 != 0x00 || model.r2 != 0x33 || model.r3 != 0x11 || model.mo != 0x11)
        return 3;
    if (model.w2[0] != UINT64_C(0x1111222233334444) || model.w2[1] != UINT64_C(0x56))
        return 4;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the branchy-mux model harness");
        }
        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated branchy-mux model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_BranchyTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated branchy-mux model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated branchy-mux model violated the scalar mux semantics");
        }
        return 0;
    }

    // NO0008 production-form fusion: same-select muxes lowered into an
    // AmGraph, scheduled through graphToProgram (split -> tree-atom fold ->
    // partition -> materialize), then emitted. The pinned outputs keep each
    // mux a select-carrying singleton atom; adjacent in one Block they form
    // a single fusion run emitted as one fused if/else segment.
    int testMuxRunPipelineFusion(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        ProgramInterface interface;
        const auto addInput = [&](std::string_view name) {
            const VariableId variable = builder.addVariable(u8Type, builder.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Input,
                .input = variable,
            });
            return variable;
        };
        const auto addOutput = [&](std::string_view name) {
            const VariableId variable = builder.addVariable(u8Type, builder.undefInit());
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Output,
                .output = variable,
            });
            return variable;
        };
        const VariableId sel = builder.addVariable(u1Type, builder.zeroInit());
        interface.ports.push_back(PortBinding{
            .name = builder.addString("sel"),
            .direction = PortDirection::Input,
            .input = sel,
        });
        const VariableId a = addInput("a");
        const VariableId b = addInput("b");
        const VariableId c = addInput("c");
        const VariableId d = addInput("d");
        const VariableId e = addInput("e");
        const VariableId r1 = addOutput("r1");
        const VariableId r2 = addOutput("r2");
        const VariableId r3 = addOutput("r3");
        const auto mux = [&](VariableId result, VariableId whenTrue,
                             VariableId whenFalse) {
            builder.addInstruction(Opcode::Mux, std::array{result},
                                   std::array{sel, whenTrue, whenFalse});
        };
        mux(r1, a, b);
        mux(r2, c, d);
        mux(r3, a, e);

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput,  // sel
            VariableRole::ExternalInput,  // a
            VariableRole::ExternalInput,  // b
            VariableRole::ExternalInput,  // c
            VariableRole::ExternalInput,  // d
            VariableRole::ExternalInput,  // e
            VariableRole::ExternalOutput, // r1
            VariableRole::ExternalOutput, // r2
            VariableRole::ExternalOutput, // r3
        };

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        });
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model =
            GrhIRToGrhSimAMProgram::graphToProgram(std::move(graph),
                                                   ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            for (const auto &message : diagnostics.messages())
            {
                std::cerr << message.message << " [" << message.context << "]\n";
            }
            return fail("same-select mux graph did not schedule");
        }
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "MuxPipeTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the mux-pipeline model");
        }
        if (emitResult.muxAtomFused != 3)
        {
            return fail("mux-rooted run did not surface as one fused run of three");
        }
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_MuxPipeTop_blocks_0.cpp");
        if (!blocksText)
        {
            return fail("AM C++ emitter produced no mux-pipeline blocks source");
        }
        const std::string fusedHead =
            "if ((v" + std::to_string(sel.value) + " != 0)) { v" +
            std::to_string(r1.value) + " = (resize_value(v" + std::to_string(a.value) +
            ", 8, false, 8)) & ((UINT64_C(1) << 8) - UINT64_C(1));";
        const std::string secondAssign =
            "v" + std::to_string(r2.value) + " = (resize_value(v" +
            std::to_string(c.value) + ", 8, false, 8)) & ((UINT64_C(1) << 8) - UINT64_C(1));";
        if (blocksText->find(fusedHead) == std::string::npos ||
            blocksText->find(secondAssign) == std::string::npos)
        {
            return fail("scheduled same-select muxes were not emitted as one if/else run");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_MuxPipeTop.hpp"
int main()
{
    GrhSIM_MuxPipeTop model;
    model.init();
    model.sel = 0;
    model.a = 0x11;
    model.b = 0x22;
    model.c = 0x33;
    model.d = 0x44;
    model.e = 0x55;
    model.eval();
    if (model.r1 != 0x22 || model.r2 != 0x44 || model.r3 != 0x55) return 1;
    model.sel = 1;
    model.eval();
    if (model.r1 != 0x11 || model.r2 != 0x33 || model.r3 != 0x11) return 2;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the mux-pipeline model harness");
        }
        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated mux-pipeline model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_MuxPipeTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated mux-pipeline model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated mux-pipeline model violated the fused select semantics");
        }
        return 0;
    }

    // NO0018 block-local regrouping, pinned: same-select mux atoms landing in
    // one block are pulled adjacent by the block-local fusion anchors and fuse
    // into one run even when another select's mux sits between them in program
    // order; the different-select mux still emits as a plain ternary.
    // (mergeWhenMinGroup stays at the default 5, so the two-member same-select
    // set is not coarsen-clustered either.)
    int testSelectChangeRegroupsMuxRun(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        LinearProgramBuilder builder;
        const TypeId u1Type = builder.addType(Type::bitVector(1));
        const TypeId u8Type = builder.addType(Type::bitVector(8));
        ProgramInterface interface;
        const auto addInput = [&](std::string_view name) {
            const VariableId variable = builder.addVariable(u8Type, builder.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Input,
                .input = variable,
            });
            return variable;
        };
        const auto addOutput = [&](std::string_view name) {
            const VariableId variable = builder.addVariable(u8Type, builder.undefInit());
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Output,
                .output = variable,
            });
            return variable;
        };
        const auto addSelect = [&](std::string_view name) {
            const VariableId variable = builder.addVariable(u1Type, builder.zeroInit());
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Input,
                .input = variable,
            });
            return variable;
        };
        const VariableId sel = addSelect("sel");
        const VariableId sel2 = addSelect("sel2");
        const VariableId a = addInput("a");
        const VariableId b = addInput("b");
        const VariableId c = addInput("c");
        const VariableId d = addInput("d");
        const VariableId e = addInput("e");
        const VariableId r1 = addOutput("r1");
        const VariableId r2 = addOutput("r2");
        const VariableId r3 = addOutput("r3");
        builder.addInstruction(Opcode::Mux, std::array{r1}, std::array{sel, a, b});
        builder.addInstruction(Opcode::Mux, std::array{r2}, std::array{sel2, c, d});
        builder.addInstruction(Opcode::Mux, std::array{r3}, std::array{sel, a, e});

        SchedulingFacts facts;
        facts.variableRoles = {
            VariableRole::ExternalInput, VariableRole::ExternalInput,
            VariableRole::ExternalInput, VariableRole::ExternalInput,
            VariableRole::ExternalInput, VariableRole::ExternalInput,
            VariableRole::ExternalInput,
            VariableRole::ExternalOutput, VariableRole::ExternalOutput,
            VariableRole::ExternalOutput,
        };

        AmGraph graph = AmGraph::fromLinearProgram(LinearProgramArtifact{
            .program = builder.finish(),
            .interface = std::move(interface),
            .schedulingFacts = std::move(facts),
        });
        wolvrix::lib::diag::Diagnostics diagnostics;
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            std::move(graph), ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("select-change mux graph did not schedule");
        }
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            *model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "MuxBreakTop",
                .maxOutputFileBytes = 1024 * 1024,
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the select-change model");
        }
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_MuxBreakTop_blocks_0.cpp");
        if (!blocksText)
        {
            return fail("AM C++ emitter produced no select-change blocks source");
        }
        const std::string fusedGate =
            "if ((v" + std::to_string(sel.value) + " != 0)) { ";
        const std::string ternaryHead =
            " = ((v" + std::to_string(sel.value) + " != 0) ? ";
        const std::string ternaryHead2 =
            " = ((v" + std::to_string(sel2.value) + " != 0) ? ";
        if (emitResult.muxAtomFused != 2 ||
            countOccurrences(*blocksText, fusedGate) != 1 ||
            countOccurrences(*blocksText, ternaryHead) != 0 ||
            countOccurrences(*blocksText, ternaryHead2) != 1)
        {
            return fail("same-select mux atoms were not regrouped into one fused run");
        }
        return 0;
    }

    // NO0017 §5 wide-state scalar explode fixture: a 128-bit state bank of
    // 32 4-bit elements, read only through aligned constant slices
    // (4-bit and 8-bit sites -> element width gcd 4), written by one
    // constant-mask write (element 7) and one full-width write, with the
    // commit tail changed.any fused into the write-point flags by ST00013.
    // Literal constant init. This is the shape of the XiangShan BPU ABTB
    // counter bank the rule was built for.
    struct ExplodeEmitterFixture
    {
        ExecutableModel model;
        VariableId bank;
        VariableId writeEnable;
        VariableId writeElement;
        VariableId fullEnable;
        VariableId fullData;
        VariableId dynOffset;
    };

    ExplodeEmitterFixture makeExplodeEmitterFixture()
    {
        LinearProgramBuilder linear;
        const TypeId u1Type = linear.addType(Type::bitVector(1));
        const TypeId u4Type = linear.addType(Type::bitVector(4));
        const TypeId u8Type = linear.addType(Type::bitVector(8));
        const TypeId u28Type = linear.addType(Type::bitVector(28));
        const TypeId u96Type = linear.addType(Type::bitVector(96));
        const TypeId u128Type = linear.addType(Type::bitVector(128));

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

        const VariableId writeEnable = addInput(u1Type, "write_enable");
        const VariableId writeElement = addInput(u4Type, "write_element");
        const VariableId fullEnable = addInput(u1Type, "full_enable");
        const VariableId fullData = addInput(u8Type, "full_data");
        const VariableId dynOffset = addInput(u8Type, "dyn_offset");
        const VariableId elem0 = addOutput(u4Type, "elem0");
        const VariableId elem7 = addOutput(u4Type, "elem7");
        const VariableId pair34 = addOutput(u8Type, "pair34");
        const VariableId dynElem = addOutput(u4Type, "dyn_elem");

        // The bank: element e initializes to e & 0xf (words 0xfedcba9876543210).
        // Actions Set-literal init: a RegisterWrite target must not be
        // Constant-init (isMutable), and the explode rule takes exactly
        // this all-literal form.
        const std::array<uint64_t, 2> bankInitWords = {
            UINT64_C(0xfedcba9876543210), UINT64_C(0xfedcba9876543210)};
        const LiteralId bankInitLiteral =
            linear.addBitLiteral(u128Type, bankInitWords);
        const InitAction bankInitAction{
            .kind = InitActionKind::Set,
            .expression = InitExpr{
                .kind = InitExprKind::Literal,
                .literal = bankInitLiteral,
            },
        };
        const VariableId bank = linear.addVariable(
            u128Type,
            linear.addActionsInit(std::span<const InitAction>(&bankInitAction, 1)));
        const VariableId bankOld = linear.addVariable(u128Type, linear.undefInit());
        const VariableId bankChangedEvent = linear.addVariable(u1Type, linear.zeroInit());

        // Write machinery: mask selecting element 7, the masked next image
        // (element 7 <- write_element, other bits irrelevant), and the
        // full-width next image (every byte <- full_data).
        const std::array<uint64_t, 2> maskWords = {UINT64_C(0xf0000000), 0};
        const VariableId maskConstant = linear.addVariable(
            u128Type,
            linear.addConstantInit(linear.addBitLiteral(u128Type, maskWords)));
        const std::array<uint64_t, 2> zeroWords = {0, 0};
        const VariableId zero96 = linear.addVariable(
            u96Type, linear.addConstantInit(linear.addBitLiteral(u96Type, zeroWords)));
        const std::array<uint64_t, 1> zero28Words = {0};
        const VariableId zero28 = linear.addVariable(
            u28Type, linear.addConstantInit(linear.addBitLiteral(u28Type, zero28Words)));
        const VariableId maskedNext = linear.addVariable(u128Type, linear.zeroInit());
        const VariableId fullNext = linear.addVariable(u128Type, linear.zeroInit());

        // Commit gate detectors: one head changed.any per commit-read input,
        // each with a private old baseline.
        const std::array<VariableId, 4> gateWatched = {
            writeEnable, writeElement, fullEnable, fullData,
        };
        const std::array<TypeId, 4> gateTypes = {
            u1Type, u4Type, u1Type, u8Type,
        };
        std::array<VariableId, 4> gateEvents{};
        std::array<InstructionId, 4> gateDetectors{};
        for (std::size_t index = 0; index < gateWatched.size(); ++index)
        {
            const VariableId gateOld = linear.addVariable(gateTypes[index],
                                                          linear.undefInit());
            gateEvents[index] = linear.addVariable(u1Type, linear.zeroInit());
            gateDetectors[index] = addInstruction(
                Opcode::ChangedAny, {gateEvents[index]},
                {gateWatched[index], gateOld});
        }

        // Compute (readers) block: aligned constant slices plus one
        // dynamic-offset element read.
        const InstructionId readElem0 = addInstruction(Opcode::SliceStatic,
                                                       {elem0}, {bank});
        linear.setSliceStaticAttributes(readElem0, 0);
        const InstructionId readElem7 = addInstruction(Opcode::SliceStatic,
                                                       {elem7}, {bank});
        linear.setSliceStaticAttributes(readElem7, 28);
        const InstructionId readPair34 = addInstruction(Opcode::SliceStatic,
                                                        {pair34}, {bank});
        linear.setSliceStaticAttributes(readPair34, 12);
        const InstructionId readDyn = addInstruction(Opcode::SliceDynamic,
                                                     {dynElem}, {bank, dynOffset});

        // Commit block: next-image builders, the masked and full-width
        // writes, and the ST00013-fused tail detector reactivating readers.
        const InstructionId buildMaskedNext =
            addInstruction(Opcode::Concat, {maskedNext}, {zero96, writeElement, zero28});
        const InstructionId buildFullNext =
            addInstruction(Opcode::Replicate, {fullNext}, {fullData});
        const InstructionId maskedWrite = addInstruction(
            Opcode::RegisterWriteCondMask, {},
            {writeEnable, maskConstant, maskedNext, bank, gateEvents[0], gateEvents[1]});
        const InstructionId fullWrite = addInstruction(
            Opcode::RegisterWriteCond, {},
            {fullEnable, fullNext, bank, gateEvents[2], gateEvents[3]});
        const InstructionId detectBankChanged = addInstruction(
            Opcode::ChangedAny, {bankChangedEvent}, {bank, bankOld});

        ScheduledProgramBuilder scheduled(linear.finish());
        std::vector<InstructionId> entry;
        const auto addEntryDetector = [&](VariableId input,
                                          std::span<const BlockId> targets) {
            const TypeId type = scheduled.view().variable(input).type;
            const VariableId oldValue = scheduled.addVariable(type, scheduled.undefInit());
            const VariableId inputChanged = scheduled.addVariable(u1Type, scheduled.zeroInit());
            const std::array<VariableId, 1> changedResults = {inputChanged};
            const std::array<VariableId, 2> changedOperands = {input, oldValue};
            const InstructionId detect = scheduled.addInstruction(
                Opcode::ChangedAny, changedResults, changedOperands);
            const std::array<VariableId, 1> activateOperands = {inputChanged};
            const InstructionId activate = scheduled.addInstruction(
                Opcode::ActForward, {}, activateOperands);
            scheduled.setActivationTargets(activate, targets);
            entry.push_back(detect);
            entry.push_back(activate);
        };
        for (VariableId input : gateWatched)
        {
            const std::array<BlockId, 2> targets = {BlockId{1}, BlockId{2}};
            addEntryDetector(input, targets);
        }
        // dyn_offset only feeds the readers block.
        const std::array<BlockId, 1> readersTarget = {BlockId{1}};
        addEntryDetector(dynOffset, readersTarget);
        scheduled.addBlock(entry);
        const std::array<InstructionId, 4> readers = {
            readElem0, readElem7, readPair34, readDyn,
        };
        scheduled.addBlock(readers);
        const InstructionId activateReaders = scheduled.addInstruction(
            Opcode::ActBackward, {}, std::array{bankChangedEvent});
        scheduled.setActivationTargets(activateReaders, std::array{BlockId{1}});
        std::vector<InstructionId> commit(gateDetectors.begin(), gateDetectors.end());
        commit.insert(commit.end(),
                      {buildMaskedNext, buildFullNext, maskedWrite, fullWrite,
                       detectBankChanged, activateReaders});
        scheduled.addBlock(commit);

        return ExplodeEmitterFixture{
            .model = ExecutableModel{
                .program = scheduled.finish(),
                .interface = std::move(interface),
                .commitBlockBegin = 2,
                .commitBlockEnd = 3,
            },
            .bank = bank,
            .writeEnable = writeEnable,
            .writeElement = writeElement,
            .fullEnable = fullEnable,
            .fullData = fullData,
            .dynOffset = dynOffset,
        };
    }

    // NO0017 §5: with "wideStateExplode" on, the bank leaves the word pool
    // for a per-element scalar array (slices -> element loads, masked write
    // -> single-element RMW, full write -> per-element loop); with the
    // attribute off the emission keeps the baseline wideValues_ form. The
    // generated explode model is compiled and checked against the
    // Interpreter oracle over masked/full write transactions.
    int testWideStateExplode(const std::filesystem::path &outputDirectory)
    {
        std::filesystem::remove_all(outputDirectory);
        ExplodeEmitterFixture fixture = makeExplodeEmitterFixture();

        struct Transaction
        {
            uint64_t writeEnable;
            uint64_t writeElement;
            uint64_t fullEnable;
            uint64_t fullData;
            uint64_t dynOffset;
            uint64_t expectElem0;
            uint64_t expectElem7;
            uint64_t expectPair34;
            uint64_t expectDynElem;
        };
        const std::array<Transaction, 6> transactions = {
            Transaction{0, 0, 0, 0, 8, 0x0, 0x7, 0x43, 0x2},
            Transaction{1, 0xa, 0, 0, 6, 0x0, 0xa, 0x43, 0x8},
            Transaction{0, 0x5, 0, 0, 200, 0x0, 0xa, 0x43, 0x0},
            Transaction{0, 0x5, 1, 0xab, 124, 0xb, 0xa, 0xba, 0xa},
            Transaction{0, 0x5, 0, 0, 8, 0xb, 0xa, 0xba, 0xb},
            Transaction{1, 0x3, 0, 0, 0, 0xb, 0x3, 0xba, 0xb},
        };
        // Interpreter oracle: the model itself must produce the expected
        // element values before any emission mode is trusted.
        {
            Interpreter reference(fixture.model);
            if (!reference.ready())
            {
                std::string detail = "failed to build the AM explode reference fixture";
                if (reference.initializationDiagnostic())
                {
                    detail += ": " + reference.initializationDiagnostic()->message;
                }
                return fail(detail);
            }
            const auto writeInput = [&](VariableId variable, uint32_t width,
                                        uint64_t value) {
                const std::array<uint64_t, 1> words = {value};
                return reference
                    .write(variable,
                           InterpreterValue::bitVector(width, Signedness::Unsigned, words))
                    .success();
            };
            for (const Transaction &transaction : transactions)
            {
                if (!writeInput(fixture.writeEnable, 1, transaction.writeEnable) ||
                    !writeInput(fixture.writeElement, 4, transaction.writeElement) ||
                    !writeInput(fixture.fullEnable, 1, transaction.fullEnable) ||
                    !writeInput(fixture.fullData, 8, transaction.fullData) ||
                    !writeInput(fixture.dynOffset, 8, transaction.dynOffset) ||
                    !reference.eval().success())
                {
                    return fail("AM Interpreter failed an explode transaction");
                }
                const ProgramView program = fixture.model.program.view();
                const auto output = [&](std::string_view name) {
                    for (const PortBinding &port : fixture.model.interface.ports)
                    {
                        if (port.direction == PortDirection::Output &&
                            program.string(port.name) == name)
                        {
                            return reference.value(port.output).lowWord();
                        }
                    }
                    return ~uint64_t{0};
                };
                if (output("elem0") != transaction.expectElem0 ||
                    output("elem7") != transaction.expectElem7 ||
                    output("pair34") != transaction.expectPair34 ||
                    output("dyn_elem") != transaction.expectDynElem)
                {
                    return fail("AM Interpreter disagreed with the explode oracle");
                }
            }
        }

        // Off mode: the attribute defaults off and the emission keeps the
        // baseline wideValues_ form (no element-array member, word-extract
        // slice reads).
        {
            wolvrix::lib::diag::Diagnostics diagnostics;
            GrhSimAmCppEmitter emitter;
            const GrhSimAmCppResult emitResult = emitter.emit(
                fixture.model,
                GrhSimAmCppOptions{
                    .outputDirectory = outputDirectory / "off",
                    .modelName = "ExplodeOffTop",
                    .maxOutputFileBytes = 1024 * 1024,
                },
                diagnostics);
            if (!emitResult.success || diagnostics.hasError())
            {
                return fail("AM C++ emitter failed to generate the explode-off model");
            }
            const std::optional<std::string> headerText =
                readTextFile(outputDirectory / "off" / "grhsim_ExplodeOffTop.hpp");
            const std::optional<std::string> blocksText =
                readTextFile(outputDirectory / "off" / "grhsim_ExplodeOffTop_blocks_0.cpp");
            if (!headerText || !blocksText)
            {
                return fail("AM C++ emitter produced no explode-off sources");
            }
            const std::string member =
                "wv" + std::to_string(fixture.bank.value) + "_";
            if (headerText->find(member) != std::string::npos ||
                blocksText->find(member) != std::string::npos ||
                blocksText->find("extract_word(wideValues_.data()") == std::string::npos ||
                emitResult.wideStateExploded != 0)
            {
                return fail("AM C++ emitter changed the baseline form without "
                            "wideStateExplode");
            }
        }

        // On mode: the bank explodes into a per-element uint8 array.
        wolvrix::lib::diag::Diagnostics diagnostics;
        GrhSimAmCppEmitter emitter;
        const GrhSimAmCppResult emitResult = emitter.emit(
            fixture.model,
            GrhSimAmCppOptions{
                .outputDirectory = outputDirectory,
                .modelName = "ExplodeTop",
                .maxOutputFileBytes = 1024 * 1024,
                .attributes = {{"wideStateExplode", "true"}},
            },
            diagnostics);
        if (!emitResult.success || diagnostics.hasError())
        {
            return fail("AM C++ emitter failed to generate the explode model");
        }
        const std::string member = "wv" + std::to_string(fixture.bank.value) + "_";
        if (emitResult.wideStateExploded != 1 ||
            emitResult.wideStateExplodedElements != 32)
        {
            return fail("AM C++ emitter did not explode exactly the bank state");
        }
        const std::optional<std::string> headerText =
            readTextFile(outputDirectory / "grhsim_ExplodeTop.hpp");
        const std::optional<std::string> blocksText =
            readTextFile(outputDirectory / "grhsim_ExplodeTop_blocks_0.cpp");
        const std::optional<std::string> runtimeText =
            readTextFile(outputDirectory / "grhsim_ExplodeTop_runtime.cpp");
        if (!headerText || !blocksText || !runtimeText)
        {
            return fail("AM C++ emitter produced no explode sources");
        }
        if (headerText->find("std::array<std::uint8_t, 32> " + member + "{};") ==
                std::string::npos ||
            runtimeText->find(member + ".fill(0);") == std::string::npos ||
            runtimeText->find(member + "[7] = (UINT64_C(7))") == std::string::npos)
        {
            return fail("AM C++ emitter did not declare/init the exploded element array");
        }
        // Element reads for the 4-bit sites (elem0/elem7) and the 2-element
        // 8-bit site (pair34 assembles elements 3 and 4); the dynamic read
        // blends two elements; the masked write updates element 7 in place
        // and the full write loops per element.
        if (blocksText->find("= (" + member + "[0]) &") == std::string::npos ||
            blocksText->find("= (" + member + "[7]) &") == std::string::npos ||
            blocksText->find("(std::uint64_t(" + member + "[4]) << 4)") ==
                std::string::npos ||
            blocksText->find("wvE_") == std::string::npos ||
            blocksText->find(member + "[wvE_") == std::string::npos ||
            blocksText->find("wnext = ((" + member + "[7] & ~UINT64_C(0xf))") ==
                std::string::npos ||
            blocksText->find(member + "[7] = wnext;") == std::string::npos ||
            blocksText->find("wvIdx_") == std::string::npos)
        {
            return fail("AM C++ emitter did not rewrite the bank accesses to element form");
        }

        const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
        std::ofstream harness(harnessPath);
        harness << R"CPP(#include "grhsim_ExplodeTop.hpp"
int main()
{
    GrhSIM_ExplodeTop model;
    model.init();
    model.write_enable = 0; model.write_element = 0; model.full_enable = 0; model.full_data = 0;
    model.dyn_offset = 8;
    model.eval();
    if (model.elem0 != 0x0 || model.elem7 != 0x7 || model.pair34 != 0x43 || model.dyn_elem != 0x2) return 1;
    model.write_enable = 1; model.write_element = 0xa; model.dyn_offset = 6;
    model.eval();
    if (model.elem0 != 0x0 || model.elem7 != 0xa || model.pair34 != 0x43 || model.dyn_elem != 0x8) return 2;
    model.write_enable = 0; model.write_element = 0x5; model.dyn_offset = 200;
    model.eval();
    if (model.elem7 != 0xa || model.dyn_elem != 0x0) return 3;
    model.full_enable = 1; model.full_data = 0xab; model.dyn_offset = 124;
    model.eval();
    if (model.elem0 != 0xb || model.elem7 != 0xa || model.pair34 != 0xba || model.dyn_elem != 0xa) return 4;
    model.full_enable = 0; model.dyn_offset = 8;
    model.eval();
    if (model.elem0 != 0xb || model.elem7 != 0xa || model.pair34 != 0xba || model.dyn_elem != 0xb) return 5;
    model.write_enable = 1; model.write_element = 0x3; model.dyn_offset = 0;
    model.eval();
    if (model.elem7 != 0x3 || model.pair34 != 0xba || model.dyn_elem != 0xb) return 6;
    return 0;
}
)CPP";
        harness.close();
        if (!harness)
        {
            return fail("failed to write the explode model harness");
        }
        const std::string buildCommand =
            "make -C '" + outputDirectory.string() +
            "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("generated explode AM model failed to compile");
        }
        const std::filesystem::path harnessExecutable = outputDirectory / "harness";
        const std::string harnessCompileCommand =
            "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
            harnessPath.string() + "' '" +
            (outputDirectory / "libgrhsim_ExplodeTop.a").string() + "' -o '" +
            harnessExecutable.string() + "'";
        if (std::system(harnessCompileCommand.c_str()) != 0)
        {
            return fail("generated explode AM model harness failed to compile");
        }
        const std::string runCommand = "'" + harnessExecutable.string() + "'";
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("generated explode AM model disagreed with the oracle");
        }
        return 0;
    }

} // namespace

int main()
{
    const std::filesystem::path outputDirectory =
        std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) / "cpp-emitter";
    std::filesystem::remove_all(outputDirectory);

    wolvrix::lib::diag::Diagnostics diagnostics;
    std::optional<ExecutableModel> model =
        GrhIRToGrhSimAMProgram::graphToProgram(AmGraph::fromLinearProgram(makeAddProgram()),
                           ActivityScheduleOptions{}, diagnostics);
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
        splitHeaderText->find("void execute_block_0();") == std::string::npos ||
        splitHeaderText->find("void eval_scan_0_part_1();") ==
            std::string::npos ||
        splitHeaderText->find("void eval_scan_0();") != std::string::npos ||
        splitHeaderText->find("void eval_commit_") != std::string::npos ||
        splitHeaderText->find("void execute_block(std::size_t block);") !=
            std::string::npos ||
        splitHeaderText->find("active_byte_ref(std::size_t byte)") ==
            std::string::npos ||
        splitRuntimeText->find("execute_block_0();") == std::string::npos ||
        splitRuntimeText->find("        eval_scan_0_part_1();\n") ==
            std::string::npos ||
        splitRuntimeText->find("switch (block /") != std::string::npos ||
        splitRuntimeText->find("execute_block(") != std::string::npos ||
        splitFirstPartText->find("void GrhSIM_TestTop::execute_block_0() {") ==
            std::string::npos ||
        splitFirstPartText->find("eval_scan_") != std::string::npos ||
        splitFirstPartText->find("eval_commit_") != std::string::npos ||
        splitFirstPartText->find("switch") != std::string::npos ||
        splitSecondPartText->find(
            "void GrhSIM_TestTop::eval_scan_0_part_1() {") ==
            std::string::npos ||
        splitSecondPartText->find(
            "std::uint8_t byteFlags = active_byte_ref(0) & UINT8_C(0x2);") ==
            std::string::npos ||
        splitSecondPartText->find("active_byte_ref(0) &= UINT8_C(0xfd);") ==
            std::string::npos ||
        splitSecondPartText->find(
            "            if ((byteFlags & UINT8_C(0x2)) != 0) {") ==
            std::string::npos ||
        splitSecondPartText->find("execute_block_0") != std::string::npos ||
        splitSecondPartText->find("switch") != std::string::npos)
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
    if (const int result = testArrayOperations(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-array-ops");
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
    if (const int result = testCondGatedWriteEmission(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-cond-gate");
        result != 0)
    {
        return result;
    }
    if (const int result = testMuxRunFusion(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-mux-run");
        result != 0)
    {
        return result;
    }
    if (const int result = testTraceComments(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-trace-comments");
        result != 0)
    {
        return result;
    }
    if (const int result = testBranchyMux(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-branchy-mux");
        result != 0)
    {
        return result;
    }
    if (const int result = testMuxRunPipelineFusion(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-mux-pipeline");
        result != 0)
    {
        return result;
    }
    if (const int result = testSelectChangeRegroupsMuxRun(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-mux-run-break");
        result != 0)
    {
        return result;
    }
    if (const int result = testWideStateExplode(
            std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
            "cpp-emitter-wide-state-explode");
        result != 0)
    {
        return result;
    }
    return testProductionCommitCycleRuntime(
        std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
        "cpp-emitter-production-commit-cycle");
}
