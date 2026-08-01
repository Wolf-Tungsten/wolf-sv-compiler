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
            return fail("fold optimize reported failure");
        }
        const ProgramView program = artifact.program.view();
        if (program.instructionCount() != 1 ||
            program.opcode(InstructionId{0}) != Opcode::Assign) {
            return fail("fold cascade did not collapse the constant chain");
        }
        const auto operands = program.operands(InstructionId{0});
        if (operands.size() != 1) {
            return fail("folded assign lost its operand");
        }
        const std::optional<uint64_t> folded = constantValue(program, operands[0]);
        if (!folded || *folded != 10) {
            return fail("fold cascade produced the wrong constant value");
        }
        const Type &constantType =
            program.type(program.variable(operands[0]).type);
        if (constantType.kind != TypeKind::BitVector || constantType.bitWidth != 64) {
            return fail("folded multiply did not keep the widened result type");
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
        const VariableId enable = fixture.constant(bv1, 1);
        const VariableId mask = fixture.constant(bv32, UINT64_C(0xFFFFFFFF));
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
        const InstructionId write = fixture.emit(
            Opcode::MemoryWrite, {}, {enable, address, mask, dataIn, memory, event});
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
            constantValue(program, writeOperands[2]) !=
                std::optional<uint64_t>(UINT64_C(0xFFFFFFFF)) ||
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

    int testObservableProducerPreserved() {
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
        const ProgramView program = artifact.program.view();
        if (program.instructionCount() != 2 || countOpcode(program, Opcode::Add) != 1) {
            return fail("observable producer must not be folded away");
        }
        const std::optional<uint32_t> addIndex = findOpcode(program, Opcode::Add);
        if (!addIndex) {
            return fail("observable producer is missing");
        }
        const auto results = program.results(InstructionId{*addIndex});
        if (results.size() != 1 || results[0] != observed) {
            return fail("observable variable lost its producing instruction");
        }
        if (artifact.interface.declaredVariables.size() != 1 ||
            artifact.interface.declaredVariables[0].variable != observed) {
            return fail("declared variable binding was not preserved");
        }
        if (const ValidationResult result = validateSemantic(artifact); !result.success()) {
            return failValidation("observable optimized artifact is invalid", result);
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
    return testObservableProducerPreserved();
}
