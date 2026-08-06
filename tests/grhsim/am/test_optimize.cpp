#include "core/diagnostics.hpp"
#include "grhsim/am/builder.hpp"
#include "grhsim/am/opcode_traits.hpp"
#include "grhsim/am/optimize.hpp"
#include "grhsim/am/pipeline.hpp"

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

    ValidationResult validateSemantic(const LinearProgramArtifact &artifact) {
        return validate(artifact, ValidationOptions{.level = ValidationLevel::Semantic});
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
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("dead-cone fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("dead-cone optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
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
        if (artifact.schedulingFacts.instructionEffects.size() != 2 ||
            artifact.schedulingFacts.instructionEffects[0] != InstructionEffect::Pure ||
            artifact.schedulingFacts.instructionEffects[1] != InstructionEffect::Pure) {
            return fail("instruction effects were not compacted with the program");
        }
        if (artifact.schedulingFacts.variableRoles.size() != program.variableCount()) {
            return fail("variable roles lost dense alignment");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
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
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("CSE fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("CSE optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
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
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
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
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("fold fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            for (const auto &message : diagnostics.messages()) {
                std::cerr << "  diag: " << message.message << '\n';
            }
            return fail("fold optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
        if (program.instructionCount() != 0) {
            return fail("fold cascade did not collapse the constant chain");
        }
        if (artifact.interface.ports.size() != 1 ||
            artifact.interface.ports[0].direction != PortDirection::Output) {
            return fail("fold cascade lost the output port");
        }
        const VariableId portOutput = artifact.interface.ports[0].output;
        const std::optional<uint64_t> folded = constantValue(program, portOutput);
        if (!folded || *folded != 10) {
            return fail("fold cascade produced the wrong constant value");
        }
        const Type &constantType =
            program.type(program.variable(portOutput).type);
        if (constantType.kind != TypeKind::BitVector || constantType.bitWidth != 64) {
            return fail("folded multiply did not keep the widened result type");
        }
        if (!hasRole(artifact.schedulingFacts.variableRoles[portOutput.value],
                     VariableRole::ExternalOutput)) {
            return fail("fold cascade did not transfer the external-output role");
        }
        if (program.variableCount() != 7) {
            return fail("fold did not intern exactly the two new constants");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
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
        // Reverted mem.write layout [cond, addr, mask, data, target,
        // events...]: cond=true with a full mask writes dataIn directly.
        const VariableId oneBit = fixture.constant(bv1, 1);
        const VariableId fullMask = fixture.constant(bv32, 0xffffffff);
        const InstructionId write = fixture.emit(
            Opcode::MemoryWrite, {}, {oneBit, address, fullMask, dataIn, memory, event});
        fixture.addOrderedEffect(orderedRead, 0, 0);
        fixture.addOrderedEffect(write, 0, 1);
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("ordered-effects fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("ordered-effects optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
        if (program.instructionCount() != 2) {
            return fail("unordered dead MemoryRead was not removed");
        }
        if (program.opcode(InstructionId{0}) != Opcode::MemoryRead ||
            program.opcode(InstructionId{1}) != Opcode::MemoryWrite) {
            return fail("ordered MemoryRead was dropped or reordered");
        }
        const SchedulingFacts &facts = artifact.schedulingFacts;
        if (facts.instructionEffects.size() != 2 ||
            facts.instructionEffects[0] != InstructionEffect::StateRead ||
            facts.instructionEffects[1] != InstructionEffect::StateReadWrite) {
            return fail("instruction effects were not remapped to the dense ids");
        }
        if (facts.orderedEffects.size() != 2 ||
            facts.orderedEffects[0].instruction != InstructionId{0} ||
            facts.orderedEffects[0].group != 0 || facts.orderedEffects[0].ordinal != 0 ||
            facts.orderedEffects[1].instruction != InstructionId{1} ||
            facts.orderedEffects[1].group != 0 || facts.orderedEffects[1].ordinal != 1) {
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
        if (facts.variableRoles.size() != program.variableCount() ||
            !hasRole(facts.variableRoles[memory.value], VariableRole::State) ||
            !hasRole(facts.variableRoles[memory.value], VariableRole::Observable) ||
            facts.variableRoles[dataIn.value] != VariableRole::ExternalInput) {
            return fail("variable roles were not preserved by compaction");
        }
        if (artifact.interface.ports.size() != 2 ||
            artifact.interface.declaredVariables.size() != 1 ||
            artifact.interface.declaredVariables[0].variable != memory) {
            return fail("interface bindings were not preserved by compaction");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
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
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("memory-CSE fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("memory-CSE optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
        if (program.instructionCount() != 3 || countOpcode(program, Opcode::MemoryRead) != 2) {
            return fail("MemoryRead instructions must never be CSE'd or folded");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
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
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("observable fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("observable optimize reported failure");
        }
        // The observable producer may be folded/aliased away as long as the
        // interface is re-pointed to a variable carrying the same value and
        // the visibility roles move with it.
        const ProgramView program = artifact.program.view();
        if (artifact.interface.declaredVariables.size() != 1) {
            return fail("declared variable binding was not preserved");
        }
        const VariableId declared = artifact.interface.declaredVariables[0].variable;
        const std::optional<uint64_t> declaredValue = constantValue(program, declared);
        if (!declaredValue || *declaredValue != 3) {
            return fail("declared variable does not observe the folded value");
        }
        if (!hasRole(artifact.schedulingFacts.variableRoles[declared.value],
                     VariableRole::Observable)) {
            return fail("observable role was not transferred to the representative");
        }
        if (artifact.interface.ports.size() != 1 ||
            constantValue(program, artifact.interface.ports[0].output) !=
                std::optional<uint64_t>(3)) {
            return fail("output port does not observe the folded value");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("observable optimized artifact is invalid", result);
        }
        return 0;
    }

    int testAssignAliasKeepsStateSnapshot() {
        Fixture fixture;
        const TypeId bv32 = fixture.builder.addType(Type::bitVector(32));
        const VariableId state = fixture.variable(bv32, VariableRole::State);
        const VariableId input = fixture.variable(bv32, VariableRole::ExternalInput);
        const VariableId snapshot = fixture.variable(bv32);
        const VariableId plain = fixture.variable(bv32);
        const VariableId computed = fixture.variable(bv32);
        const VariableId out0 = fixture.variable(bv32, VariableRole::ExternalOutput);
        const VariableId out1 = fixture.variable(bv32, VariableRole::ExternalOutput);
        fixture.addInputPort("in", input);
        fixture.addOutputPort("out0", out0);
        fixture.addOutputPort("out1", out1);
        fixture.emit(Opcode::Assign, {snapshot}, {state});
        fixture.emit(Opcode::Assign, {plain}, {input});
        fixture.emit(Opcode::Or, {computed}, {plain, plain});
        fixture.emit(Opcode::Assign, {out0}, {snapshot});
        fixture.emit(Opcode::Assign, {out1}, {computed});
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("assign-alias fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("assign-alias optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
        // The state snapshot Assign must survive; the plain value assigns are
        // bypassed, so only the snapshot Assign and the Or remain.
        if (program.instructionCount() != 2 ||
            countOpcode(program, Opcode::Assign) != 1 ||
            countOpcode(program, Opcode::Or) != 1) {
            return fail("assign-alias bypassed the wrong instructions");
        }
        const std::optional<uint32_t> assignIndex = findOpcode(program, Opcode::Assign);
        const auto operands = program.operands(InstructionId{*assignIndex});
        if (operands.size() != 1 || operands[0] != state) {
            return fail("state snapshot assign did not keep its state operand");
        }
        if (artifact.interface.ports[2].output != snapshot &&
            artifact.interface.ports[1].output != snapshot) {
            return fail("snapshot consumer was not preserved");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("assign-alias optimized artifact is invalid", result);
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
        // Reverted mem.write layout [cond, addr, mask, data, target,
        // events...]: cond=true with a full mask writes dataIn directly.
        const VariableId fullMask = fixture.constant(bv32, 0xffffffff);
        fixture.emit(Opcode::MemoryWrite, {}, {one1, addr1, fullMask, dataIn, ram, one1});
        fixture.emit(Opcode::Assign, {out0}, {readZero});
        fixture.emit(Opcode::Assign, {out1}, {readUndef});
        fixture.emit(Opcode::Assign, {out2}, {readRange});
        fixture.emit(Opcode::Assign, {out3}, {readRam});
        LinearProgramArtifact artifact = fixture.finish();
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("const-mem-fold fixture is invalid", result);
        }

        diag::Diagnostics diagnostics;
        if (!optimizeLinearProgram(artifact, AmOptimizeOptions{}, diagnostics)) {
            return fail("const-mem-fold optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
        // Zero/Undef-init never-written reads fold to zero (storage is
        // zero-initialized); out-of-range reads fold to zero. Only the read
        // of the written memory and the write itself survive.
        if (countOpcode(program, Opcode::MemoryRead) != 1 ||
            countOpcode(program, Opcode::MemoryWrite) != 1) {
            return fail("const-mem-fold eliminated the wrong instructions");
        }
        const std::optional<uint64_t> foldedZero =
            constantValue(program, artifact.interface.ports[1].output);
        const std::optional<uint64_t> foldedUndef =
            constantValue(program, artifact.interface.ports[2].output);
        const std::optional<uint64_t> foldedRange =
            constantValue(program, artifact.interface.ports[3].output);
        if (!foldedZero || *foldedZero != 0) {
            return fail("const-mem-fold did not zero never-written reads");
        }
        if (!foldedUndef || *foldedUndef != 0) {
            return fail("const-mem-fold did not zero undef-init reads");
        }
        if (!foldedRange || *foldedRange != 0) {
            return fail("const-mem-fold did not zero out-of-range reads");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
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
    return testConstMemFold();
}
