#include "grhsim/emit/cpu_single_thread.hpp"
#include "grhsim/ir.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace wolvrix::lib::grhsim;

    int fail(std::string_view message)
    {
        std::cerr << "[grhsim-cpu-single-thread] " << message << '\n';
        return 1;
    }

    std::string diagnosticsText(const wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        std::string result;
        for (const wolvrix::lib::diag::Diagnostic &message : diagnostics.messages())
        {
            if (!result.empty())
            {
                result += "; ";
            }
            result += message.message;
            if (!message.context.empty())
            {
                result += " [" + message.context + "]";
            }
        }
        return result;
    }

    std::string shellQuote(std::string_view value)
    {
        std::string result("'");
        for (char ch : value)
        {
            if (ch == '\'')
            {
                result += "'\\''";
            }
            else
            {
                result.push_back(ch);
            }
        }
        result.push_back('\'');
        return result;
    }

    bool writeFile(const std::filesystem::path &path, std::string_view contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        return static_cast<bool>(output);
    }

    EdgeId addConstant(Module &module, TypeId type, std::string literal,
                       std::string_view name = {})
    {
        const OpId op = module.createOp(genericOp(GenericOpcode::Const),
                                        name.empty() ? SymbolId::invalid() : module.intern(name));
        const EdgeId result = module.addResult(op, type);
        if (!op.valid() || !result.valid() ||
            !module.setAttr(op, "value", module.intern(literal)))
        {
            return EdgeId::invalid();
        }
        return result;
    }

    EdgeId addStateRead(Module &module, GenericOpcode opcode, StateId state,
                        std::string_view attrName)
    {
        const StateEntry *entry = module.state(state);
        if (!entry)
        {
            return EdgeId::invalid();
        }
        const OpId op = module.createOp(genericOp(opcode));
        const EdgeId result = module.addResult(op, entry->genType);
        if (!op.valid() || !result.valid() ||
            !module.setAttr(op, attrName, entry->name))
        {
            return EdgeId::invalid();
        }
        return result;
    }

    bool addOutputWrite(Module &module, StateId output, EdgeId value)
    {
        const StateEntry *entry = module.state(output);
        const OpId op = module.createOp(genericOp(GenericOpcode::OutWrite));
        const std::array<EdgeId, 1> operands{value};
        return entry && op.valid() && module.setOperands(op, operands) &&
               module.setAttr(op, "port", entry->name);
    }

    bool setStateWrite(Module &module, OpId op, StateId state,
                       std::span<const EdgeId> operands)
    {
        const StateEntry *entry = module.state(state);
        return entry && module.setOperands(op, operands) &&
               module.setAttr(op, "state", entry->name);
    }

    bool setPosedge(Module &module, OpId op, StateId clock)
    {
        const StateEntry *entry = module.state(clock);
        return entry &&
               module.setAttr(op, "events", std::vector<SymbolId>{entry->name}) &&
               module.setAttr(op, "eventEdge",
                              std::vector<SymbolId>{module.intern("posedge")});
    }

    bool schedule(Module &module, std::string &error)
    {
        auto pass = makeSimPass("schedule-topo", {}, error);
        if (!pass || !error.empty())
        {
            return false;
        }
        SimPassManager manager;
        manager.addPass(std::move(pass));
        wolvrix::lib::transform::PassDiagnostics diagnostics;
        const SimPipelineResult result = manager.run(module, diagnostics);
        if (!result.success || diagnostics.hasError())
        {
            error = diagnosticsText(diagnostics);
            return false;
        }
        return module.hasSchedule() && module.linearize().size() == module.opCount();
    }

    bool buildMainModule(Module &module, std::string &error)
    {
        const TypeId logic1 = module.internLogicType(1, false);
        const TypeId logic2 = module.internLogicType(2, false);
        const TypeId logic8 = module.internLogicType(8, false);
        const TypeId logic32 = module.internLogicType(32, false);
        const TypeId logic130 = module.internLogicType(130, false);
        const TypeId memoryType = module.internArrayType(4, logic8);

        const StateId clock = module.addState("clk", StateKind::Input, logic1);
        const StateId wideInput = module.addState("wide_in", StateKind::Input, logic130);
        const StateId latchEnable = module.addState("latch_en", StateKind::Input, logic1);
        const StateId byteInput = module.addState("byte_in", StateKind::Input, logic8);
        const StateId memoryWriteEnable = module.addState("mem_we", StateKind::Input, logic1);
        const StateId memoryAddress = module.addState("mem_addr", StateKind::Input, logic2);
        const StateId dpiInput = module.addState("dpi_in", StateKind::Input, logic32);
        const StateId wideOutput = module.addState("wide_q", StateKind::Output, logic130);
        const StateId latchOutput = module.addState("latch_q", StateKind::Output, logic8);
        const StateId memoryOutput = module.addState("mem_q", StateKind::Output, logic8);
        const StateId dpiOutput = module.addState("dpi_q", StateKind::Output, logic32);
        const StateId wideRegister = module.addState("wide_reg", StateKind::State, logic130);
        const StateId latch = module.addState("latch", StateKind::State, logic8);
        const StateId memory = module.addState("memory", StateKind::State, memoryType);

        const EdgeId clockValue = addStateRead(module, GenericOpcode::InRead, clock, "port");
        const EdgeId wideValue = addStateRead(module, GenericOpcode::InRead, wideInput, "port");
        const EdgeId latchEnableValue =
            addStateRead(module, GenericOpcode::InRead, latchEnable, "port");
        const EdgeId byteValue = addStateRead(module, GenericOpcode::InRead, byteInput, "port");
        const EdgeId memoryWriteEnableValue =
            addStateRead(module, GenericOpcode::InRead, memoryWriteEnable, "port");
        const EdgeId addressValue =
            addStateRead(module, GenericOpcode::InRead, memoryAddress, "port");
        const EdgeId dpiValue = addStateRead(module, GenericOpcode::InRead, dpiInput, "port");
        (void)clockValue;

        const EdgeId one = addConstant(module, logic1, "1'b1", "one");
        const EdgeId mask8 = addConstant(module, logic8, "8'hff", "mask8");
        const EdgeId mask130 = addConstant(
            module, logic130, "130'b" + std::string(130, '1'), "mask130");
        const EdgeId wideState =
            addStateRead(module, GenericOpcode::RegRead, wideRegister, "state");
        const EdgeId latchState =
            addStateRead(module, GenericOpcode::LatchRead, latch, "state");

        const OpId registerWrite = module.createOp(genericOp(GenericOpcode::RegWrite));
        const std::array<EdgeId, 3> registerOperands{one, wideValue, mask130};
        const OpId latchWrite = module.createOp(genericOp(GenericOpcode::LatchWrite));
        const std::array<EdgeId, 3> latchOperands{latchEnableValue, byteValue, mask8};
        const OpId memoryRead = module.createOp(genericOp(GenericOpcode::MemRead));
        const std::array<EdgeId, 1> memoryReadOperands{addressValue};
        const EdgeId memoryData = module.addResult(memoryRead, logic8);
        const OpId memoryWrite = module.createOp(genericOp(GenericOpcode::MemWrite));
        const std::array<EdgeId, 4> memoryWriteOperands{
            memoryWriteEnableValue, addressValue, byteValue, mask8};

        const std::array<HostParam, 2> signature{
            HostParam{module.intern("value"), logic32, HostParamDirection::Input},
            HostParam{module.intern("result"), logic32, HostParamDirection::Return},
        };
        const std::array<AttrKV, 8> hostAttrs{
            AttrKV{module.intern("argsDirection"),
                   std::vector<SymbolId>{module.intern("input")}},
            AttrKV{module.intern("argsWidth"), std::vector<int64_t>{32}},
            AttrKV{module.intern("argsName"),
                   std::vector<SymbolId>{module.intern("value")}},
            AttrKV{module.intern("argsSigned"), std::vector<bool>{false}},
            AttrKV{module.intern("argsType"),
                   std::vector<SymbolId>{module.intern("int")}},
            AttrKV{module.intern("hasReturn"), true},
            AttrKV{module.intern("returnWidth"), int64_t{32}},
            AttrKV{module.intern("returnType"), module.intern("int")},
        };
        const HostId dpiHost = module.addHost("dpi_transform", HostKind::Query, signature,
                                              "dpi_transform", hostAttrs);
        const OpId dpiCall = module.createOp(genericOp(GenericOpcode::HostCall));
        const std::array<EdgeId, 1> dpiOperands{dpiValue};
        const EdgeId dpiResult = module.addResult(dpiCall, logic32);

        if (!clock.valid() || !wideInput.valid() || !latchEnable.valid() ||
            !byteInput.valid() || !memoryWriteEnable.valid() || !memoryAddress.valid() ||
            !dpiInput.valid() || !wideOutput.valid() || !latchOutput.valid() ||
            !memoryOutput.valid() || !dpiOutput.valid() || !wideRegister.valid() ||
            !latch.valid() || !memory.valid() || !wideValue.valid() ||
            !latchEnableValue.valid() || !byteValue.valid() ||
            !memoryWriteEnableValue.valid() || !addressValue.valid() || !dpiValue.valid() ||
            !one.valid() || !mask8.valid() || !mask130.valid() || !wideState.valid() ||
            !latchState.valid() || !memoryRead.valid() || !memoryData.valid() ||
            !memoryWrite.valid() || !dpiHost.valid() || !dpiCall.valid() ||
            !dpiResult.valid() ||
            !setStateWrite(module, registerWrite, wideRegister, registerOperands) ||
            !setPosedge(module, registerWrite, clock) ||
            !setStateWrite(module, latchWrite, latch, latchOperands) ||
            !module.setOperands(memoryRead, memoryReadOperands) ||
            !module.setAttr(memoryRead, "state", module.state(memory)->name) ||
            !module.setOperands(memoryWrite, memoryWriteOperands) ||
            !module.setAttr(memoryWrite, "state", module.state(memory)->name) ||
            !setPosedge(module, memoryWrite, clock) ||
            !module.setOperands(dpiCall, dpiOperands) ||
            !module.setAttr(dpiCall, "entry", module.host(dpiHost)->entry) ||
            !addOutputWrite(module, wideOutput, wideState) ||
            !addOutputWrite(module, latchOutput, latchState) ||
            !addOutputWrite(module, memoryOutput, memoryData) ||
            !addOutputWrite(module, dpiOutput, dpiResult))
        {
            error = "failed to construct main CPU emitter fixture";
            return false;
        }
        return schedule(module, error);
    }

    bool buildOscillatorModule(Module &module, std::string &error)
    {
        const TypeId logic1 = module.internLogicType(1, false);
        const StateId output = module.addState("q", StateKind::Output, logic1);
        const StateId latch = module.addState("oscillator", StateKind::State, logic1);
        const EdgeId one = addConstant(module, logic1, "1'b1");
        const EdgeId current = addStateRead(module, GenericOpcode::LatchRead, latch, "state");
        const OpId invert = module.createOp(genericOp(GenericOpcode::Not));
        const std::array<EdgeId, 1> invertOperands{current};
        const EdgeId next = module.addResult(invert, logic1);
        const OpId write = module.createOp(genericOp(GenericOpcode::LatchWrite));
        const std::array<EdgeId, 3> writeOperands{one, next, one};
        if (!output.valid() || !latch.valid() || !one.valid() || !current.valid() ||
            !invert.valid() || !next.valid() || !write.valid() ||
            !module.setOperands(invert, invertOperands) ||
            !setStateWrite(module, write, latch, writeOperands) ||
            !addOutputWrite(module, output, current))
        {
            error = "failed to construct oscillator fixture";
            return false;
        }
        return schedule(module, error);
    }

    bool buildSyntheticEventModule(Module &module, std::string &error)
    {
        const TypeId logic1 = module.internLogicType(1, false);
        const TypeId logic8 = module.internLogicType(8, false);
        const StateId eventInput = module.addState("event_in", StateKind::Input, logic1);
        const StateId eventState = module.addState("derived$event", StateKind::Output, logic1);
        const StateId positiveOutput = module.addState("positive_q", StateKind::Output, logic8);
        const StateId negativeOutput = module.addState("negative_q", StateKind::Output, logic8);
        const StateId positiveState = module.addState("positive_state", StateKind::State, logic8);
        const StateId negativeState = module.addState("negative_state", StateKind::State, logic8);

        const EdgeId eventValue = addStateRead(module, GenericOpcode::InRead, eventInput, "port");
        const OpId derive = module.createOp(genericOp(GenericOpcode::Assign),
                                            module.intern("derive_event"));
        const EdgeId derivedValue = module.addResult(derive, logic1);
        const OpId eventWriter = module.createOp(genericOp(GenericOpcode::OutWrite),
                                                 module.intern("derived_event_writer"));
        const OpId positiveRead = module.createOp(genericOp(GenericOpcode::RegRead));
        const EdgeId positiveCurrent = module.addResult(positiveRead, logic8);
        const OpId negativeRead = module.createOp(genericOp(GenericOpcode::RegRead));
        const EdgeId negativeCurrent = module.addResult(negativeRead, logic8);
        const EdgeId one = addConstant(module, logic8, "8'h01", "one");
        const EdgeId mask = addConstant(module, logic8, "8'hff", "mask");
        const EdgeId condition = addConstant(module, logic1, "1'b1", "condition");
        const OpId positiveAdd = module.createOp(genericOp(GenericOpcode::Add),
                                                 module.intern("positive_add"));
        const EdgeId positiveNext = module.addResult(positiveAdd, logic8);
        const OpId negativeAdd = module.createOp(genericOp(GenericOpcode::Add),
                                                 module.intern("negative_add"));
        const EdgeId negativeNext = module.addResult(negativeAdd, logic8);
        const OpId positiveWrite = module.createOp(genericOp(GenericOpcode::RegWrite),
                                                   module.intern("positive_write"));
        const OpId negativeWrite = module.createOp(genericOp(GenericOpcode::RegWrite),
                                                   module.intern("negative_write"));
        const OpId positiveOutputWrite =
            module.createOp(genericOp(GenericOpcode::OutWrite), module.intern("positive_output"));
        const OpId negativeOutputWrite =
            module.createOp(genericOp(GenericOpcode::OutWrite), module.intern("negative_output"));

        const std::array<EdgeId, 1> deriveOperands{eventValue};
        const std::array<EdgeId, 1> eventWriterOperands{derivedValue};
        const std::array<EdgeId, 0> positiveReadOperands{};
        const std::array<EdgeId, 0> negativeReadOperands{};
        const std::array<EdgeId, 2> positiveAddOperands{positiveCurrent, one};
        const std::array<EdgeId, 2> negativeAddOperands{negativeCurrent, one};
        const std::array<EdgeId, 3> positiveWriteOperands{condition, positiveNext, mask};
        const std::array<EdgeId, 3> negativeWriteOperands{condition, negativeNext, mask};
        const std::array<EdgeId, 1> positiveOutputOperands{positiveCurrent};
        const std::array<EdgeId, 1> negativeOutputOperands{negativeCurrent};
        if (!eventInput.valid() || !eventState.valid() || !positiveOutput.valid() ||
            !negativeOutput.valid() || !positiveState.valid() || !negativeState.valid() ||
            !eventValue.valid() || !derive.valid() || !derivedValue.valid() ||
            !eventWriter.valid() || !positiveRead.valid() || !positiveCurrent.valid() ||
            !negativeRead.valid() || !negativeCurrent.valid() || !one.valid() || !mask.valid() ||
            !condition.valid() || !positiveAdd.valid() || !positiveNext.valid() ||
            !negativeAdd.valid() || !negativeNext.valid() || !positiveWrite.valid() ||
            !negativeWrite.valid() || !positiveOutputWrite.valid() ||
            !negativeOutputWrite.valid() || !module.setOperands(derive, deriveOperands) ||
            !module.setOperands(eventWriter, eventWriterOperands) ||
            !module.setAttr(eventWriter, "port", module.state(eventState)->name) ||
            !module.setAttr(eventWriter, "eventState", true) ||
            !module.setOperands(positiveRead, positiveReadOperands) ||
            !module.setAttr(positiveRead, "state", module.state(positiveState)->name) ||
            !module.setOperands(negativeRead, negativeReadOperands) ||
            !module.setAttr(negativeRead, "state", module.state(negativeState)->name) ||
            !module.setOperands(positiveAdd, positiveAddOperands) ||
            !module.setOperands(negativeAdd, negativeAddOperands) ||
            !setStateWrite(module, positiveWrite, positiveState, positiveWriteOperands) ||
            !setStateWrite(module, negativeWrite, negativeState, negativeWriteOperands) ||
            !setPosedge(module, positiveWrite, eventState) ||
            !module.setAttr(negativeWrite, "events",
                            std::vector<SymbolId>{module.state(eventState)->name}) ||
            !module.setAttr(negativeWrite, "eventEdge",
                            std::vector<SymbolId>{module.intern("negedge")}) ||
            !module.setOperands(positiveOutputWrite, positiveOutputOperands) ||
            !module.setAttr(positiveOutputWrite, "port", module.state(positiveOutput)->name) ||
            !module.setOperands(negativeOutputWrite, negativeOutputOperands) ||
            !module.setAttr(negativeOutputWrite, "port", module.state(negativeOutput)->name))
        {
            error = "failed to construct synthetic event emitter fixture";
            return false;
        }
        return schedule(module, error);
    }

    bool emitModule(const Module &module, const std::filesystem::path &directory,
                    uint32_t iterationLimit, std::string &error)
    {
        wolvrix::lib::diag::Diagnostics diagnostics;
        const CpuSingleThreadEmitResult result = emitCpuSingleThread(
            module,
            CpuSingleThreadEmitOptions{
                .outputDirectory = directory,
                .opsPerSourceFile = 4,
                .fixedPointIterationLimit = iterationLimit,
            },
            diagnostics);
        if (!result.success || diagnostics.hasError())
        {
            error = diagnosticsText(diagnostics);
            return false;
        }
        return result.artifacts.size() >= 6;
    }

    bool compileAndRun(const std::filesystem::path &directory,
                       std::string_view stem, std::string_view harness)
    {
        const std::filesystem::path harnessPath = directory / "harness.cpp";
        if (!writeFile(harnessPath, harness))
        {
            return false;
        }
        const std::string compiler = WOLVRIX_TEST_CXX_COMPILER;
        const std::string buildCommand =
            "make -C " + shellQuote(directory.string()) + " CXX=" + shellQuote(compiler) +
            " CXXFLAGS='-std=c++20 -O2'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return false;
        }
        const std::filesystem::path executable = directory / "harness";
        const std::filesystem::path archive =
            directory / ("libgrhsim_" + std::string(stem) + ".a");
        const std::string compileCommand =
            shellQuote(compiler) + " -std=c++20 -O2 -I" + shellQuote(directory.string()) +
            " " + shellQuote(harnessPath.string()) + " " + shellQuote(archive.string()) +
            " -o " + shellQuote(executable.string());
        if (std::system(compileCommand.c_str()) != 0)
        {
            return false;
        }
        return std::system(shellQuote(executable.string()).c_str()) == 0;
    }

    bool compilePublicHeaderWithExistingHostDeclaration(
        const std::filesystem::path &directory)
    {
        const std::filesystem::path source = directory / "public-header-smoke.cpp";
        constexpr std::string_view contents = R"CPP(
#include <cstdint>

extern "C" std::int32_t dpi_transform(std::int32_t);

#include "grhsim_cpu_emitter_top.hpp"
)CPP";
        if (!writeFile(source, contents))
        {
            return false;
        }
        const std::filesystem::path object = directory / "public-header-smoke.o";
        const std::string command =
            shellQuote(WOLVRIX_TEST_CXX_COMPILER) + " -std=c++20 -I" +
            shellQuote(directory.string()) + " -c " + shellQuote(source.string()) +
            " -o " + shellQuote(object.string());
        return std::system(command.c_str()) == 0;
    }
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::path(WOLVRIX_GRHSIM_CPU_EMIT_ARTIFACT_DIR) /
        "cpu-single-thread";
    std::error_code errorCode;
    std::filesystem::remove_all(root, errorCode);
    std::filesystem::create_directories(root, errorCode);
    if (errorCode)
    {
        return fail("failed to prepare emitter artifact directory");
    }

    std::string error;
    Module mainModule("cpu_emitter_top");
    if (!buildMainModule(mainModule, error))
    {
        return fail("main fixture schedule failed: " + error);
    }
    const std::filesystem::path mainDirectory = root / "main";
    if (!emitModule(mainModule, mainDirectory, 16, error))
    {
        return fail("main fixture emit failed: " + error);
    }
    if (!compilePublicHeaderWithExistingHostDeclaration(mainDirectory))
    {
        return fail("public model header conflicts with an existing host declaration");
    }
    constexpr std::string_view mainHarness = R"CPP(
#include "grhsim_cpu_emitter_top.hpp"

#include <array>
#include <cstdint>

extern "C" std::uint32_t dpi_transform(std::uint32_t value)
{
    return value ^ UINT32_C(0x5a5aa5a5);
}

int main()
{
    GrhSIM_cpu_emitter_top sim;
    sim.init();
    const std::array<std::uint64_t, 3> wide{
        UINT64_C(0x0123456789abcdef),
        UINT64_C(0xfedcba9876543210),
        UINT64_C(0x3),
    };
    sim.clk = false;
    sim.wide_in = wide;
    sim.latch_en = false;
    sim.byte_in = UINT8_C(0x12);
    sim.mem_we = false;
    sim.mem_addr = UINT8_C(2);
    sim.dpi_in = UINT32_C(0x12345678);
    sim.eval();
    if (sim.wide_q != std::array<std::uint64_t, 3>{} || sim.latch_q != 0 ||
        sim.mem_q != 0 || sim.dpi_q != UINT32_C(0x486ef3dd))
        return 1;

    sim.latch_en = true;
    sim.byte_in = UINT8_C(0x5a);
    sim.eval();
    if (sim.latch_q != UINT8_C(0x5a))
        return 2;
    sim.latch_en = false;
    sim.byte_in = UINT8_C(0xa5);
    sim.eval();
    if (sim.latch_q != UINT8_C(0x5a))
        return 3;

    sim.mem_we = true;
    sim.clk = true;
    sim.eval();
    if (sim.wide_q != wide || sim.mem_q != UINT8_C(0xa5))
        return 4;
    sim.clk = false;
    sim.mem_we = false;
    sim.wide_in = {};
    sim.byte_in = 0;
    sim.eval();
    if (sim.wide_q != wide || sim.mem_q != UINT8_C(0xa5) ||
        sim.latch_q != UINT8_C(0x5a))
        return 5;
    return 0;
}
)CPP";
    if (!compileAndRun(mainDirectory, "cpu_emitter_top", mainHarness))
    {
        return fail("generated main model failed to compile or run");
    }

    Module oscillator("cpu_emitter_oscillator");
    if (!buildOscillatorModule(oscillator, error))
    {
        return fail("oscillator schedule failed: " + error);
    }
    const std::filesystem::path oscillatorDirectory = root / "oscillator";
    if (!emitModule(oscillator, oscillatorDirectory, 4, error))
    {
        return fail("oscillator emit failed: " + error);
    }
    constexpr std::string_view oscillatorHarness = R"CPP(
#include "grhsim_cpu_emitter_oscillator.hpp"

#include <stdexcept>
#include <string_view>

int main()
{
    GrhSIM_cpu_emitter_oscillator sim;
    sim.init();
    try {
        sim.eval();
    }
    catch (const std::runtime_error &error) {
        return std::string_view(error.what()) ==
                       "GRHSIM fixed-point iteration limit exceeded"
                   ? 0
                   : 2;
    }
    return 1;
}
)CPP";
    if (!compileAndRun(oscillatorDirectory, "cpu_emitter_oscillator",
                       oscillatorHarness))
    {
        return fail("generated oscillator did not enforce the iteration limit");
    }

    Module syntheticEvent("cpu_emitter_synthetic_event");
    if (!buildSyntheticEventModule(syntheticEvent, error))
    {
        return fail("synthetic event schedule failed: " + error);
    }
    const std::filesystem::path syntheticDirectory = root / "synthetic-event";
    if (!emitModule(syntheticEvent, syntheticDirectory, 16, error))
    {
        return fail("synthetic event emit failed: " + error);
    }
    constexpr std::string_view syntheticHarness = R"CPP(
#include "grhsim_cpu_emitter_synthetic_event.hpp"

#include <cstdint>

int main()
{
    GrhSIM_cpu_emitter_synthetic_event sim;
    sim.init();
    sim.event_in = false;
    sim.eval();
    if (sim.positive_q != 0 || sim.negative_q != 0)
        return 1;

    sim.event_in = true;
    sim.eval();
    if (sim.positive_q != 1 || sim.negative_q != 0)
        return 2;
    sim.eval();
    if (sim.positive_q != 1 || sim.negative_q != 0)
        return 3;

    sim.event_in = false;
    sim.eval();
    if (sim.positive_q != 1 || sim.negative_q != 1)
        return 4;
    sim.eval();
    if (sim.positive_q != 1 || sim.negative_q != 1)
        return 5;

    sim.event_in = true;
    sim.eval();
    if (sim.positive_q != 2 || sim.negative_q != 1)
        return 6;
    return 0;
}
)CPP";
    if (!compileAndRun(syntheticDirectory, "cpu_emitter_synthetic_event",
                       syntheticHarness))
    {
        return fail("generated synthetic event model failed to compile or run");
    }
    return 0;
}
