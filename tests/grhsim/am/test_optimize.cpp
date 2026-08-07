#include "core/diagnostics.hpp"
#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_opcode_traits.hpp"
#include "grhsim/am/grhsim_am_graph_optimize.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using namespace wolvrix::lib;
    using namespace wolvrix::lib::grhsim::am;

    int fail(std::string_view message) {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    int failValidation(std::string_view message, const ValidationResult &result) {
        std::cerr << "FAIL: " << message << '\n';
        for (const std::string &error : result.errors) {
            std::cerr << "  " << error << '\n';
        }
        return 1;
    }

    ValidationResult validateSemantic(const AmGraph &graph) {
        return validate(graph, ValidationOptions{.level = ValidationLevel::Semantic});
    }

    // Mirrors the mechanical effect-table fill in lowering.cpp.
    InstructionEffect effectFor(Opcode opcode) {
        switch (opcodeTraits(opcode).effect) {
        case OpcodeEffect::Pure:
            return InstructionEffect::Pure;
        case OpcodeEffect::ChangeDetector:
        case OpcodeEffect::StateReadWrite:
            return InstructionEffect::StateReadWrite;
        case OpcodeEffect::StateRead:
            return InstructionEffect::StateRead;
        case OpcodeEffect::HostRead:
            return InstructionEffect::HostRead;
        case OpcodeEffect::HostEffect:
        case OpcodeEffect::Activation:
            return InstructionEffect::HostEffect;
        }
        return InstructionEffect::HostEffect;
    }

    struct Fixture {
        VariableId constant(TypeId type, uint64_t value) {
            const LiteralId literal =
                builder.addBitLiteral(type, std::array<uint64_t, 1>{value});
            const VariableId variable =
                builder.addVariable(type, builder.addConstantInit(literal));
            roles.push_back(VariableRole::None);
            return variable;
        }

        VariableId variable(TypeId type, VariableRole role = VariableRole::None,
                            std::optional<StringId> label = std::nullopt) {
            const VariableId variable = builder.addVariable(type, builder.zeroInit(), label);
            roles.push_back(role);
            return variable;
        }

        InstructionId emit(Opcode opcode, std::vector<VariableId> results,
                           std::vector<VariableId> operands) {
            const InstructionId instruction = builder.addInstruction(opcode, results, operands);
            effects.push_back(effectFor(opcode));
            return instruction;
        }

        void addInputPort(std::string_view name, VariableId variable) {
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Input,
                .input = variable,
            });
        }

        void addOutputPort(std::string_view name, VariableId variable) {
            interface.ports.push_back(PortBinding{
                .name = builder.addString(name),
                .direction = PortDirection::Output,
                .output = variable,
            });
        }

        void declareVariable(std::string_view name, VariableId variable) {
            interface.declaredVariables.push_back(VariableLabel{
                .variable = variable,
                .label = builder.addString(name),
            });
        }

        void addOrderedEffect(InstructionId instruction, uint32_t group, uint32_t ordinal) {
            orderedEffects.push_back(OrderedEffect{
                .instruction = instruction,
                .group = group,
                .ordinal = ordinal,
            });
        }

        LinearProgramArtifact finish() {
            return LinearProgramArtifact{
                .program = builder.finish(),
                .interface = std::move(interface),
                .schedulingFacts =
                    SchedulingFacts{
                        .variableRoles = std::move(roles),
                        .instructionEffects = std::move(effects),
                        .orderedEffects = std::move(orderedEffects),
                    },
            };
        }

        LinearProgramBuilder builder;
        std::vector<VariableRole> roles;
        std::vector<InstructionEffect> effects;
        std::vector<OrderedEffect> orderedEffects;
        ProgramInterface interface;
    };

    std::size_t countOpcode(ProgramView program, Opcode opcode) {
        std::size_t count = 0;
        for (uint32_t index = 0; index < program.instructionCount(); ++index) {
            count += program.opcode(InstructionId{index}) == opcode;
        }
        return count;
    }

    std::optional<uint64_t> constantValue(ProgramView program, VariableId variable) {
        if (!variable.valid() || variable.value >= program.variableCount()) {
            return std::nullopt;
        }
        const VariableRecord &record = program.variable(variable);
        if (!record.init.valid() || record.init.value >= program.initCount()) {
            return std::nullopt;
        }
        const InitDescriptor &init = program.init(record.init);
        if (init.kind != InitKind::Constant) {
            return std::nullopt;
        }
        const LiteralId literalId{init.payload};
        if (!literalId.valid() || literalId.value >= program.literalCount()) {
            return std::nullopt;
        }
        const LiteralView literal = program.literal(literalId);
        if (literal.words.empty()) {
            return std::nullopt;
        }
        return literal.words.front();
    }

    std::optional<uint32_t> findOpcode(ProgramView program, Opcode opcode) {
        for (uint32_t index = 0; index < program.instructionCount(); ++index) {
            if (program.opcode(InstructionId{index}) == opcode) {
                return index;
            }
        }
        return std::nullopt;
    }

    int testDeadConeElimination() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const VariableId input = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId output = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId live = fixture.variable(bv32);
        const VariableId deadA = fixture.variable(bv32);
        const VariableId deadB = fixture.variable(bv32);
        fixture.addInputPort("in", input);
        fixture.addOutputPort("out", output);
        fixture.emit(Opcode::And, {live}, {input, input});
        fixture.emit(Opcode::Add, {output}, {input, live});
        fixture.emit(Opcode::Xor, {deadA}, {live, input});
        fixture.emit(Opcode::Or, {deadB}, {deadA, input});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("dead-cone fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("dead-cone optimize reported failure");
        }
        const ProgramView program = graph.program();
        if (program.instructionCount() != 2) {
            return fail("dead cone was not removed");
        }
        if (program.opcode(InstructionId{0}) != Opcode::And ||
            program.opcode(InstructionId{1}) != Opcode::Add) {
            return fail("dead-cone compaction did not preserve instruction order");
        }
        const auto addOperands = program.operands(InstructionId{1});
        if (addOperands.size() != 2 || addOperands[0] != input || addOperands[1] != live) {
            return fail("live instruction operands changed during dead-cone removal");
        }
        const auto addResults = program.results(InstructionId{1});
        if (addResults.size() != 1 || addResults[0] != output) {
            return fail("output producer lost its result variable");
        }
        if (graph.instructionEffects().size() != 2 ||
            graph.instructionEffects()[0] != InstructionEffect::Pure ||
            graph.instructionEffects()[1] != InstructionEffect::Pure) {
            return fail("instruction effects were not compacted with the program");
        }
        if (graph.variableRoles().size() != program.variableCount()) {
            return fail("variable roles lost dense alignment");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("dead-cone optimized artifact is invalid", result);
        }
        return 0;
    }

    int testCommutativeCse() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const VariableId in0 = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId in1 = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId out0 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId first = fixture.variable(bv32);
        const VariableId duplicate = fixture.variable(bv32);
        const VariableId sub0 = fixture.variable(bv32);
        const VariableId sub1 = fixture.variable(bv32);
        fixture.addInputPort("in0", in0);
        fixture.addInputPort("in1", in1);
        fixture.addOutputPort("out0", out0);
        fixture.addOutputPort("out1", out1);
        fixture.emit(Opcode::And, {first}, {in0, in1});
        fixture.emit(Opcode::And, {duplicate}, {in1, in0});
        fixture.emit(Opcode::Sub, {sub0}, {in0, in1});
        fixture.emit(Opcode::Sub, {sub1}, {in1, in0});
        fixture.emit(Opcode::Or, {out0}, {first, duplicate});
        fixture.emit(Opcode::Or, {out1}, {sub0, sub1});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("CSE fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("CSE optimize reported failure");
        }
        const ProgramView program = graph.program();
        if (program.instructionCount() != 5 || countOpcode(program, Opcode::And) != 1 ||
            countOpcode(program, Opcode::Sub) != 2 || countOpcode(program, Opcode::Or) != 2) {
            return fail("commutative CSE removed the wrong instructions");
        }
        bool checkedAlias = false;
        for (uint32_t index = 0; index < program.instructionCount(); ++index) {
            const InstructionId instruction{index};
            if (program.opcode(instruction) != Opcode::Or) {
                continue;
            }
            const auto results = program.results(instruction);
            if (results.size() == 1 && results[0] == out0) {
                const auto operands = program.operands(instruction);
                if (operands.size() != 2 || operands[0] != first || operands[1] != first) {
                    return fail("CSE did not alias the duplicate result variable");
                }
                checkedAlias = true;
            }
        }
        if (!checkedAlias) {
            return fail("CSE output producer is missing");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("CSE optimized artifact is invalid", result);
        }
        return 0;
    }

    int testConstantFoldCascade() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const TypeId bv64 = fixture.builder.addType(Type::bitVector(64));
        const VariableId two = fixture.constant(bv32, 2);
        const VariableId three = fixture.constant(bv32, 3);
        const VariableId sum = fixture.variable(bv32);
        const VariableId product = fixture.variable(bv64);
        const VariableId output = fixture.variable(bv64, VariableRole::ExternalOutput);
        fixture.addOutputPort("out", output);
        fixture.emit(Opcode::Add, {sum}, {two, three});
        fixture.emit(Opcode::Mul, {product}, {sum, two});
        fixture.emit(Opcode::Assign, {output}, {product});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("fold fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            for (const auto &message : diagnostics.messages()) {
                std::cerr << "  diag: " << message.message << '\n';
            }
            return fail("fold optimize reported failure");
        }
        const ProgramView program = graph.program();
        if (program.instructionCount() != 0) {
            return fail("fold cascade did not collapse the constant chain");
        }
        if (graph.interface().ports.size() != 1 ||
            graph.interface().ports[0].direction != PortDirection::Output) {
            return fail("fold cascade lost the output port");
        }
        const VariableId portOutput = graph.interface().ports[0].output;
        const std::optional<uint64_t> folded = constantValue(program, portOutput);
        if (!folded || *folded != 10) {
            return fail("fold cascade produced the wrong constant value");
        }
        const Type &constantType =
            program.type(program.variable(portOutput).type);
        if (constantType.kind != TypeKind::BitVector || constantType.bitWidth != 64) {
            return fail("folded multiply did not keep the widened result type");
        }
        if (!hasRole(graph.valueFacts(portOutput).roles,
                     VariableRole::ExternalOutput)) {
            return fail("fold cascade did not transfer the external-output role");
        }
        if (program.variableCount() != 7) {
            return fail("fold did not intern exactly the two new constants");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("fold optimized artifact is invalid", result);
        }
        return 0;
    }

    int testOrderedEffectsAndFactsRemap() {
        Fixture fixture;
        const TypeId bv1 = fixture.builder.addType(Type::bitVector(1));
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const TypeId memoryType = fixture.builder.addType(Type::array(4, 32));
        const VariableId one = fixture.constant(bv32, 1);
        const VariableId dataIn = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId event = fixture.variable(bv1, VariableRole::ExternalInput);
        const StringId memoryLabel = fixture.builder.addString("mem");
        const VariableId memory =
            fixture.variable(memoryType, VariableRole::State | VariableRole::Observable,
                             memoryLabel);
        const VariableId address = fixture.variable(bv32);
        const VariableId keptRead = fixture.variable(bv32);
        const VariableId deadRead = fixture.variable(bv32);
        fixture.addInputPort("din", dataIn);
        fixture.addInputPort("ev", event);
        fixture.declareVariable("mem", memory);
        fixture.emit(Opcode::Add, {address}, {one, one});
        fixture.emit(Opcode::MemoryRead, {deadRead}, {memory, address});
        const InstructionId orderedRead =
            fixture.emit(Opcode::MemoryRead, {keptRead}, {memory, address});
        // mem.write.cm layout [cond, addr, mask, data, target,
        // events...]: cond=true with a full mask writes dataIn directly.
        const VariableId oneBit = fixture.constant(bv1, 1);
        const VariableId fullMask = fixture.constant(bv32, 0xffffffff);
        const InstructionId write = fixture.emit(
            Opcode::MemoryWriteCondMask, {}, {oneBit, address, fullMask, dataIn, memory, event});
        fixture.addOrderedEffect(orderedRead, 0, 0);
        fixture.addOrderedEffect(write, 0, 1);
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("ordered-effects fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("ordered-effects optimize reported failure");
        }
        const ProgramView program = graph.program();
        if (program.instructionCount() != 2) {
            return fail("unordered dead MemoryRead was not removed");
        }
        if (program.opcode(InstructionId{0}) != Opcode::MemoryRead ||
            program.opcode(InstructionId{1}) != Opcode::MemoryWriteCondMask) {
            return fail("ordered MemoryRead was dropped or reordered");
        }
        const std::vector<InstructionEffect> &instructionEffects = graph.instructionEffects();
        if (instructionEffects.size() != 2 ||
            instructionEffects[0] != InstructionEffect::StateRead ||
            instructionEffects[1] != InstructionEffect::StateReadWrite) {
            return fail("instruction effects were not remapped to the dense ids");
        }
        const std::vector<OrderedEffect> &orderedEffects = graph.orderedEffects();
        if (orderedEffects.size() != 2 ||
            orderedEffects[0].instruction != InstructionId{0} ||
            orderedEffects[0].group != 0 || orderedEffects[0].ordinal != 0 ||
            orderedEffects[1].instruction != InstructionId{1} ||
            orderedEffects[1].group != 0 || orderedEffects[1].ordinal != 1) {
            return fail("ordered effects were not remapped to the dense ids");
        }
        const auto readOperands = program.operands(InstructionId{0});
        if (readOperands.size() != 2 || readOperands[0] != memory ||
            constantValue(program, readOperands[1]) != std::optional<uint64_t>(2)) {
            return fail("ordered MemoryRead operands were not remapped");
        }
        const auto writeOperands = program.operands(InstructionId{1});
        if (writeOperands.size() != 6 ||
            constantValue(program, writeOperands[0]) != std::optional<uint64_t>(1) ||
            constantValue(program, writeOperands[1]) != std::optional<uint64_t>(2) ||
            constantValue(program, writeOperands[2]) != std::optional<uint64_t>(0xffffffff) ||
            writeOperands[3] != dataIn || writeOperands[4] != memory ||
            writeOperands[5] != event) {
            return fail("MemoryWrite operands were not remapped");
        }
        const std::vector<VariableRole> roles = graph.variableRoles();
        if (roles.size() != program.variableCount() ||
            !hasRole(roles[memory.value], VariableRole::State) ||
            !hasRole(roles[memory.value], VariableRole::Observable) ||
            roles[dataIn.value] != VariableRole::ExternalInput) {
            return fail("variable roles were not preserved by compaction");
        }
        if (graph.interface().ports.size() != 2 ||
            graph.interface().declaredVariables.size() != 1 ||
            graph.interface().declaredVariables[0].variable != memory) {
            return fail("interface bindings were not preserved by compaction");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("ordered-effects optimized artifact is invalid", result);
        }
        return 0;
    }

    int testMemoryReadNeverCse() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const TypeId memoryType = fixture.builder.addType(Type::array(4, 32));
        const VariableId address = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId memory = fixture.variable(memoryType, VariableRole::State);
        const VariableId read0 = fixture.variable(bv32);
        const VariableId read1 = fixture.variable(bv32);
        const VariableId output = fixture.variable(bv32, VariableRole::ExternalOutput);
        fixture.addInputPort("addr", address);
        fixture.addOutputPort("out", output);
        fixture.emit(Opcode::MemoryRead, {read0}, {memory, address});
        fixture.emit(Opcode::MemoryRead, {read1}, {memory, address});
        fixture.emit(Opcode::Or, {output}, {read0, read1});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("memory-CSE fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("memory-CSE optimize reported failure");
        }
        const ProgramView program = graph.program();
        if (program.instructionCount() != 3 || countOpcode(program, Opcode::MemoryRead) != 2) {
            return fail("MemoryRead instructions must never be CSE'd or folded");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("memory-CSE optimized artifact is invalid", result);
        }
        return 0;
    }

    int testObservableAliasRepointsInterface() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const VariableId one = fixture.constant(bv32, 1);
        const VariableId two = fixture.constant(bv32, 2);
        const VariableId observed = fixture.variable(bv32, VariableRole::Observable);
        const VariableId output = fixture.variable(bv32, VariableRole::ExternalOutput);
        fixture.declareVariable("observed", observed);
        fixture.addOutputPort("out", output);
        fixture.emit(Opcode::Add, {observed}, {one, two});
        fixture.emit(Opcode::Assign, {output}, {observed});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("observable fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("observable optimize reported failure");
        }
        // The observable producer may be folded/aliased away as long as the
        // interface is re-pointed to a variable carrying the same value and
        // the visibility roles move with it.
        const ProgramView program = graph.program();
        if (graph.interface().declaredVariables.size() != 1) {
            return fail("declared variable binding was not preserved");
        }
        const VariableId declared = graph.interface().declaredVariables[0].variable;
        const std::optional<uint64_t> declaredValue = constantValue(program, declared);
        if (!declaredValue || *declaredValue != 3) {
            return fail("declared variable does not observe the folded value");
        }
        if (!hasRole(graph.valueFacts(declared).roles,
                     VariableRole::Observable)) {
            return fail("observable role was not transferred to the representative");
        }
        if (graph.interface().ports.size() != 1 ||
            constantValue(program, graph.interface().ports[0].output) !=
                std::optional<uint64_t>(3)) {
            return fail("output port does not observe the folded value");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("observable optimized artifact is invalid", result);
        }
        return 0;
    }

    int testAssignAliasKeepsStateSnapshot() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const TypeId bv1 = fixture.builder.addType(Type::bitVector(1));
        const VariableId state = fixture.variable(bv32, VariableRole::State);
        const VariableId input = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId clock = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId snapshot = fixture.variable(bv32);
        const VariableId wire = fixture.variable(bv32);
        const VariableId plain = fixture.variable(bv32);
        const VariableId computed = fixture.variable(bv32);
        const VariableId computedWire = fixture.variable(bv32);
        const VariableId out0 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out2 = fixture.variable(bv32, VariableRole::ExternalOutput);
        fixture.addInputPort("in", input);
        fixture.addInputPort("clk", clock);
        fixture.addOutputPort("out0", out0);
        fixture.addOutputPort("out1", out1);
        fixture.addOutputPort("out2", out2);
        fixture.emit(Opcode::Assign, {snapshot}, {state});
        fixture.emit(Opcode::Assign, {wire}, {state});
        fixture.emit(Opcode::Assign, {plain}, {input});
        fixture.emit(Opcode::Or, {computed}, {plain, plain});
        fixture.emit(Opcode::Or, {computedWire}, {wire, plain});
        fixture.emit(Opcode::Assign, {out0}, {snapshot});
        fixture.emit(Opcode::Assign, {out1}, {computed});
        fixture.emit(Opcode::Assign, {out2}, {computedWire});
        // The snapshot feeds a commit-side state write, so it is a genuine
        // read-old snapshot and must be kept; the compute-only wire assign
        // reading the same state is bypassed.
        const InstructionId write =
            fixture.emit(Opcode::RegisterWrite, {}, {snapshot, state, clock});
        fixture.addOrderedEffect(write, 0, 0);
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("assign-alias fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("assign-alias optimize reported failure");
        }
        const ProgramView program = graph.program();
        // Kept: the snapshot Assign (commit read-old), the write, and the two
        // Ors. Bypassed: the plain/wire/output assigns.
        if (countOpcode(program, Opcode::Assign) != 1 ||
            countOpcode(program, Opcode::RegisterWrite) != 1 ||
            countOpcode(program, Opcode::Or) != 2) {
            return fail("assign-alias bypassed the wrong instructions");
        }
        const std::optional<uint32_t> assignIndex = findOpcode(program, Opcode::Assign);
        const auto operands = program.operands(InstructionId{*assignIndex});
        if (operands.size() != 1 || operands[0] != state) {
            return fail("state snapshot assign did not keep its state operand");
        }
        // The commit write still reads the snapshot, not the state directly.
        const std::optional<uint32_t> writeIndex =
            findOpcode(program, Opcode::RegisterWrite);
        const auto writeOperands = program.operands(InstructionId{*writeIndex});
        if (writeOperands.size() != 3 || writeOperands[0] != snapshot ||
            writeOperands[1] != state) {
            return fail("commit write does not read the snapshot");
        }
        // The compute-only wire assign was bypassed: its Or consumer reads the
        // state variable directly now.
        bool sawStateReaderOr = false;
        for (uint32_t index = 0; index < program.instructionCount(); ++index) {
            if (program.opcode(InstructionId{index}) != Opcode::Or) {
                continue;
            }
            const auto orOperands = program.operands(InstructionId{index});
            if (std::find(orOperands.begin(), orOperands.end(), state) !=
                orOperands.end()) {
                sawStateReaderOr = true;
            }
        }
        if (!sawStateReaderOr) {
            return fail("compute-only state-read assign was not bypassed");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("assign-alias optimized artifact is invalid", result);
        }
        return 0;
    }

    int testLogicUnify() {
        Fixture fixture;
        const TypeId bv1 = fixture.builder.addType(Type::bitVector(1));
        const TypeId bv2 = fixture.builder.addType(Type::bitVector(2));
        const VariableId a = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId b = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId wide = fixture.variable(bv2, VariableRole::ExternalInput);
        const VariableId andOut = fixture.variable(bv1);
        const VariableId logicOut = fixture.variable(bv1);
        const VariableId wideOut = fixture.variable(bv1);
        const VariableId out0 = fixture.variable(bv1, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv1, VariableRole::ExternalOutput);
        const VariableId out2 = fixture.variable(bv1, VariableRole::ExternalOutput);
        fixture.addInputPort("a", a);
        fixture.addInputPort("b", b);
        fixture.addInputPort("w", wide);
        fixture.addOutputPort("o0", out0);
        fixture.addOutputPort("o1", out1);
        fixture.addOutputPort("o2", out2);
        fixture.emit(Opcode::And, {andOut}, {a, b});
        fixture.emit(Opcode::LogicAnd, {logicOut}, {a, b});
        // 2-bit operand: truth(wide) is a reduction, not a bitwise op, so
        // this LogicAnd must keep its form.
        fixture.emit(Opcode::LogicAnd, {wideOut}, {wide, wide});
        fixture.emit(Opcode::Assign, {out0}, {andOut});
        fixture.emit(Opcode::Assign, {out1}, {logicOut});
        fixture.emit(Opcode::Assign, {out2}, {wideOut});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("logic-unify fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("logic-unify optimize reported failure");
        }
        const ProgramView program = graph.program();
        // The 1-bit And/LogicAnd pair unifies and CSEs into a single And;
        // the 2-bit LogicAnd survives. Output ports: ports[3..5] (three
        // input ports precede them); o0 and o1 must observe the unified And.
        if (countOpcode(program, Opcode::And) != 1 ||
            countOpcode(program, Opcode::LogicAnd) != 1) {
            return fail("logic-unify did not merge the 1-bit copies");
        }
        if (graph.interface().ports[4].output != andOut ||
            graph.interface().ports[3].output != andOut) {
            return fail("logic-unify outputs diverged");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("logic-unify optimized artifact is invalid", result);
        }
        return 0;
    }

    int testMuxNotAbsorb() {
        Fixture fixture;
        const TypeId bv1 = fixture.builder.addType(Type::bitVector(1));
        const TypeId bv8 = fixture.builder.addType(Type::bitVector(8));
        const VariableId c = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId c2 = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId c3 = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId d = fixture.variable(bv8, VariableRole::ExternalInput);
        const VariableId a = fixture.variable(bv8, VariableRole::ExternalInput);
        const VariableId b = fixture.variable(bv8, VariableRole::ExternalInput);
        const VariableId inverted = fixture.variable(bv1);
        const VariableId wideInverted = fixture.variable(bv1);
        const VariableId shared = fixture.variable(bv1);
        const VariableId mixed = fixture.variable(bv1);
        const VariableId selected = fixture.variable(bv8);
        const VariableId wideA = fixture.variable(bv8);
        const VariableId wideB = fixture.variable(bv8);
        const VariableId sharedA = fixture.variable(bv8);
        const VariableId sharedB = fixture.variable(bv8);
        const VariableId mixedSel = fixture.variable(bv8);
        const VariableId mixedAnd = fixture.variable(bv1);
        const VariableId out0 = fixture.variable(bv8, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv8, VariableRole::ExternalOutput);
        const VariableId out2 = fixture.variable(bv8, VariableRole::ExternalOutput);
        const VariableId out3 = fixture.variable(bv8, VariableRole::ExternalOutput);
        const VariableId out4 = fixture.variable(bv8, VariableRole::ExternalOutput);
        const VariableId out5 = fixture.variable(bv8, VariableRole::ExternalOutput);
        const VariableId out6 = fixture.variable(bv1, VariableRole::ExternalOutput);
        fixture.addInputPort("c", c);
        fixture.addInputPort("c2", c2);
        fixture.addInputPort("c3", c3);
        fixture.addInputPort("d", d);
        fixture.addInputPort("a", a);
        fixture.addInputPort("b", b);
        fixture.addOutputPort("o0", out0);
        fixture.addOutputPort("o1", out1);
        fixture.addOutputPort("o2", out2);
        fixture.addOutputPort("o3", out3);
        fixture.addOutputPort("o4", out4);
        fixture.addOutputPort("o5", out5);
        fixture.addOutputPort("o6", out6);
        // Single-use bitwise Not on a 1-bit select: absorbed, arms swapped.
        fixture.emit(Opcode::Not, {inverted}, {c});
        fixture.emit(Opcode::Mux, {selected}, {inverted, a, b});
        // LogicNot of a wide operand shared by Mux selects: kept (a wide
        // select would violate the 1-bit select rule).
        fixture.emit(Opcode::LogicNot, {wideInverted}, {d});
        fixture.emit(Opcode::Mux, {wideA}, {wideInverted, a, b});
        fixture.emit(Opcode::Mux, {wideB}, {wideInverted, b, a});
        // 1-bit LogicNot shared by two Mux selects: absorbed from both.
        fixture.emit(Opcode::LogicNot, {shared}, {c3});
        fixture.emit(Opcode::Mux, {sharedA}, {shared, a, b});
        fixture.emit(Opcode::Mux, {sharedB}, {shared, b, a});
        // Not feeding a Mux select AND a non-Mux consumer: must stay, and
        // its Mux keeps the inverted select.
        fixture.emit(Opcode::Not, {mixed}, {c2});
        fixture.emit(Opcode::Mux, {mixedSel}, {mixed, a, b});
        fixture.emit(Opcode::And, {mixedAnd}, {mixed, c});
        fixture.emit(Opcode::Assign, {out0}, {selected});
        fixture.emit(Opcode::Assign, {out1}, {wideA});
        fixture.emit(Opcode::Assign, {out2}, {wideB});
        fixture.emit(Opcode::Assign, {out3}, {sharedA});
        fixture.emit(Opcode::Assign, {out4}, {sharedB});
        fixture.emit(Opcode::Assign, {out5}, {mixedSel});
        fixture.emit(Opcode::Assign, {out6}, {mixedAnd});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("mux-not-absorb fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("mux-not-absorb optimize reported failure");
        }
        const ProgramView program = graph.program();
        // The mixed-consumer Not and the wide LogicNot survive; the six
        // Muxes remain, three with swapped arms.
        if (countOpcode(program, Opcode::Not) != 1 ||
            countOpcode(program, Opcode::LogicNot) != 1 ||
            countOpcode(program, Opcode::Mux) != 6) {
            return fail("mux-not-absorb rewrote the wrong instructions");
        }
        bool sawSwapped = false;
        bool sawSharedSwapped = false;
        bool sawWideKept = false;
        bool sawMixedKept = false;
        for (uint32_t index = 0; index < program.instructionCount(); ++index) {
            if (program.opcode(InstructionId{index}) != Opcode::Mux) {
                continue;
            }
            const auto operands = program.operands(InstructionId{index});
            if (operands.size() != 3) {
                continue;
            }
            if (operands[0] == c && operands[1] == b && operands[2] == a) {
                sawSwapped = true;
            }
            if (operands[0] == c3 && operands[1] == b && operands[2] == a) {
                sawSharedSwapped = true;
            }
            if (operands[0] == wideInverted && operands[1] == a && operands[2] == b) {
                sawWideKept = true;
            }
            if (operands[0] == mixed && operands[1] == a && operands[2] == b) {
                sawMixedKept = true;
            }
        }
        if (!sawSwapped || !sawSharedSwapped) {
            return fail("mux-not-absorb did not swap the data arms");
        }
        if (!sawWideKept || !sawMixedKept) {
            return fail("mux-not-absorb touched a mux it must keep");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("mux-not-absorb optimized artifact is invalid", result);
        }
        return 0;
    }

    int testNotUnify() {
        Fixture fixture;
        const TypeId bv1 = fixture.builder.addType(Type::bitVector(1));
        const VariableId x = fixture.variable(bv1, VariableRole::ExternalInput);
        const VariableId zero = fixture.constant(bv1, 0);
        const VariableId notOut = fixture.variable(bv1);
        const VariableId eqOut = fixture.variable(bv1);
        const VariableId andUse = fixture.variable(bv1);
        const VariableId out0 = fixture.variable(bv1, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv1, VariableRole::ExternalOutput);
        const VariableId out2 = fixture.variable(bv1, VariableRole::ExternalOutput);
        fixture.addInputPort("x", x);
        fixture.addOutputPort("o0", out0);
        fixture.addOutputPort("o1", out1);
        fixture.addOutputPort("o2", out2);
        // not(x) feeds a non-Mux consumer (kept as a value) while an explicit
        // x == 0 exists: unification turns the Not into Eq(x, 0) and CSE
        // merges the two.
        fixture.emit(Opcode::Not, {notOut}, {x});
        fixture.emit(Opcode::Eq, {eqOut}, {x, zero});
        fixture.emit(Opcode::And, {andUse}, {notOut, x});
        fixture.emit(Opcode::Assign, {out0}, {notOut});
        fixture.emit(Opcode::Assign, {out1}, {eqOut});
        fixture.emit(Opcode::Assign, {out2}, {andUse});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("not-unify fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        AmOptimizeOptions options;
        options.notUnify = true;
        if (!optimizeAmGraph(graph, options, diagnostics)) {
            return fail("not-unify optimize reported failure");
        }
        const ProgramView program = graph.program();
        if (countOpcode(program, Opcode::Not) != 0 ||
            countOpcode(program, Opcode::Eq) != 1 ||
            countOpcode(program, Opcode::And) != 1) {
            return fail("not-unify kept the wrong instructions");
        }
        // Both outputs observe the single merged Eq.
        if (graph.interface().ports[1].output != graph.interface().ports[2].output) {
            return fail("not-unify outputs diverged");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("not-unify optimized artifact is invalid", result);
        }
        return 0;
    }

    int testSliceFuse() {
        Fixture fixture;
        const TypeId bv16 = fixture.builder.addType(Type::bitVector(16));
        const TypeId bv8 = fixture.builder.addType(Type::bitVector(8));
        const TypeId bv4 = fixture.builder.addType(Type::bitVector(4));
        const VariableId x = fixture.variable(bv16, VariableRole::ExternalInput);
        const VariableId mid = fixture.variable(bv8);
        const VariableId outer = fixture.variable(bv4);
        const VariableId identity = fixture.variable(bv16);
        const VariableId out0 = fixture.variable(bv4, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv16, VariableRole::ExternalOutput);
        fixture.addInputPort("x", x);
        fixture.addOutputPort("o0", out0);
        fixture.addOutputPort("o1", out1);
        const InstructionId midSlice = fixture.emit(Opcode::SliceStatic, {mid}, {x});
        fixture.builder.setSliceStaticAttributes(midSlice, 4);
        const InstructionId outerSlice =
            fixture.emit(Opcode::SliceStatic, {outer}, {mid});
        fixture.builder.setSliceStaticAttributes(outerSlice, 2);
        const InstructionId idSlice =
            fixture.emit(Opcode::SliceStatic, {identity}, {x});
        fixture.builder.setSliceStaticAttributes(idSlice, 0);
        fixture.emit(Opcode::Assign, {out0}, {outer});
        fixture.emit(Opcode::Assign, {out1}, {identity});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("slice-fuse fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("slice-fuse optimize reported failure");
        }
        const ProgramView program = graph.program();
        // The chain fuses into a single slice(x, 6); the identity slice is
        // bypassed entirely.
        if (program.instructionCount() != 1 ||
            countOpcode(program, Opcode::SliceStatic) != 1) {
            return fail("slice-fuse kept the wrong instructions");
        }
        const auto operands = program.operands(InstructionId{0});
        if (operands.size() != 1 || operands[0] != x) {
            return fail("slice-fuse did not re-point to the chain base");
        }
        const auto attributes =
            program.sliceStaticAttributes(InstructionId{0});
        if (!attributes || attributes->lsb != 6) {
            return fail("slice-fuse produced a wrong lsb");
        }
        if (graph.interface().ports[2].output != x) {
            return fail("identity slice was not bypassed");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("slice-fuse optimized artifact is invalid", result);
        }
        return 0;
    }

    int testConstMemFold() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const TypeId memoryType = fixture.builder.addType(Type::array(4, 32));
        // Memory variables may not carry a Constant init (validation), so the
        // foldable cases are Zero and Undef init on never-written memories.
        const VariableId zeroMem = fixture.variable(memoryType, VariableRole::State);
        const VariableId undefMem = fixture.builder.addVariable(
            memoryType, fixture.builder.undefInit());
        fixture.roles.push_back(VariableRole::State);
        const VariableId ram = fixture.variable(memoryType, VariableRole::State);
        const TypeId bv1 = fixture.builder.addType(Type::bitVector(1));
        const VariableId one1 = fixture.constant(bv1, 1);
        const VariableId dataIn = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId addr1 = fixture.constant(bv32, 1);
        const VariableId addr9 = fixture.constant(bv32, 9);
        const VariableId readZero = fixture.variable(bv32);
        const VariableId readUndef = fixture.variable(bv32);
        const VariableId readRange = fixture.variable(bv32);
        const VariableId readRam = fixture.variable(bv32);
        const VariableId out0 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out2 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out3 = fixture.variable(bv32, VariableRole::ExternalOutput);
        fixture.addInputPort("din", dataIn);
        fixture.addOutputPort("o0", out0);
        fixture.addOutputPort("o1", out1);
        fixture.addOutputPort("o2", out2);
        fixture.addOutputPort("o3", out3);
        fixture.emit(Opcode::MemoryRead, {readZero}, {zeroMem, addr1});
        fixture.emit(Opcode::MemoryRead, {readUndef}, {undefMem, addr1});
        fixture.emit(Opcode::MemoryRead, {readRange}, {zeroMem, addr9});
        fixture.emit(Opcode::MemoryRead, {readRam}, {ram, addr1});
        // mem.write.cm layout [cond, addr, mask, data, target,
        // events...]: cond=true with a full mask writes dataIn directly.
        const VariableId fullMask = fixture.constant(bv32, 0xffffffff);
        fixture.emit(Opcode::MemoryWriteCondMask, {}, {one1, addr1, fullMask, dataIn, ram, one1});
        fixture.emit(Opcode::Assign, {out0}, {readZero});
        fixture.emit(Opcode::Assign, {out1}, {readUndef});
        fixture.emit(Opcode::Assign, {out2}, {readRange});
        fixture.emit(Opcode::Assign, {out3}, {readRam});
        AmGraph graph = AmGraph::fromLinearProgram(fixture.finish());
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("const-mem-fold fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeAmGraph(graph, AmOptimizeOptions{}, diagnostics)) {
            return fail("const-mem-fold optimize reported failure");
        }
        const ProgramView program = graph.program();
        // Zero/Undef-init never-written reads fold to zero (storage is
        // zero-initialized); out-of-range reads fold to zero. Only the read
        // of the written memory and the write itself survive.
        if (countOpcode(program, Opcode::MemoryRead) != 1 ||
            countOpcode(program, Opcode::MemoryWriteCondMask) != 1) {
            return fail("const-mem-fold eliminated the wrong instructions");
        }
        const std::optional<uint64_t> foldedZero =
            constantValue(program, graph.interface().ports[1].output);
        const std::optional<uint64_t> foldedUndef =
            constantValue(program, graph.interface().ports[2].output);
        const std::optional<uint64_t> foldedRange =
            constantValue(program, graph.interface().ports[3].output);
        if (!foldedZero || *foldedZero != 0) {
            return fail("const-mem-fold did not zero never-written reads");
        }
        if (!foldedUndef || *foldedUndef != 0) {
            return fail("const-mem-fold did not zero undef-init reads");
        }
        if (!foldedRange || *foldedRange != 0) {
            return fail("const-mem-fold did not zero out-of-range reads");
        }
        if (const ValidationResult result = validateSemantic(graph); !result.success()) {
            return failValidation("const-mem-fold optimized artifact is invalid", result);
        }
        return 0;
    }
} // namespace

int main() {
    if (const int result = testDeadConeElimination(); result != 0) {
        return result;
    }
    if (const int result = testCommutativeCse(); result != 0) {
        return result;
    }
    if (const int result = testConstantFoldCascade(); result != 0) {
        return result;
    }
    if (const int result = testOrderedEffectsAndFactsRemap(); result != 0) {
        return result;
    }
    if (const int result = testMemoryReadNeverCse(); result != 0) {
        return result;
    }
    if (const int result = testObservableAliasRepointsInterface(); result != 0) {
        return result;
    }
    if (const int result = testAssignAliasKeepsStateSnapshot(); result != 0) {
        return result;
    }
    if (const int result = testLogicUnify(); result != 0) {
        return result;
    }
    if (const int result = testMuxNotAbsorb(); result != 0) {
        return result;
    }
    if (const int result = testNotUnify(); result != 0) {
        return result;
    }
    if (const int result = testSliceFuse(); result != 0) {
        return result;
    }
    return testConstMemFold();
}
