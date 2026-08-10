// NO0017 fixture: wide concat/slice/shift bit-order semantics probe.
// Verifies that the AM interpreter's concat operand order and dynamic
// slice/shift index semantics match Verilog/FIRRTL conventions, isolating
// the wide-vector arbitration-state failure seen in the gsim exec-GRH
// import (MSHR/priority one-hot vectors never one-hot).
#include "grhsim/am/grhsim_am_program_interpreter.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

int fail(const std::string &message) {
    std::cerr << "[concat-slice-semantics] " << message << '\n';
    return 1;
}

int run() {
    LinearProgramBuilder linear;
    const TypeId u1Type = linear.addType(Type::bitVector(1));
    const TypeId u3Type = linear.addType(Type::bitVector(3));
    const TypeId u6Type = linear.addType(Type::bitVector(6));
    const TypeId u32Type = linear.addType(Type::bitVector(32));
    const TypeId u115Type = linear.addType(Type::bitVector(115));

    std::vector<InstructionId> instructions;
    const auto add = [&](Opcode opcode, std::initializer_list<VariableId> results,
                         std::initializer_list<VariableId> operands) {
        const InstructionId instruction = linear.addInstruction(
            opcode, std::span<const VariableId>(results.begin(), results.size()),
            std::span<const VariableId>(operands.begin(), operands.size()));
        instructions.push_back(instruction);
        return instruction;
    };
    const auto constant = [&](TypeId type, std::span<const uint64_t> words) {
        const LiteralId literal = linear.addBitLiteral(type, words);
        return linear.addVariable(type, linear.addConstantInit(literal));
    };

    // {3'b101, 3'b010} per Verilog cat (first operand occupies the high bits)
    // => 6'b101010 = 0x2A. sel[1]=1, sel[4]=0.
    const VariableId kA = constant(u3Type, std::array<uint64_t, 1>{5});
    const VariableId kB = constant(u3Type, std::array<uint64_t, 1>{2});
    const VariableId cat = linear.addVariable(u6Type, linear.undefInit());
    add(Opcode::Concat, {cat}, {kA, kB});
    const VariableId idx1 = constant(u32Type, std::array<uint64_t, 1>{1});
    const VariableId idx4 = constant(u32Type, std::array<uint64_t, 1>{4});
    const VariableId sel1 = linear.addVariable(u1Type, linear.undefInit());
    const VariableId sel4 = linear.addVariable(u1Type, linear.undefInit());
    add(Opcode::SliceDynamic, {sel1}, {cat, idx1});
    add(Opcode::SliceDynamic, {sel4}, {cat, idx4});

    // ROB-style wide vector: bit 23 set; sel[23]=1, sel[22]=0, sel[24]=0.
    const std::array<uint64_t, 2> vecWords = {UINT64_C(1) << 23, 0};
    const VariableId vec = constant(u115Type, vecWords);
    const VariableId idx22 = constant(u32Type, std::array<uint64_t, 1>{22});
    const VariableId idx23 = constant(u32Type, std::array<uint64_t, 1>{23});
    const VariableId idx24 = constant(u32Type, std::array<uint64_t, 1>{24});
    const VariableId out22 = linear.addVariable(u1Type, linear.undefInit());
    const VariableId out23 = linear.addVariable(u1Type, linear.undefInit());
    const VariableId out24 = linear.addVariable(u1Type, linear.undefInit());
    add(Opcode::SliceDynamic, {out22}, {vec, idx22});
    add(Opcode::SliceDynamic, {out23}, {vec, idx23});
    add(Opcode::SliceDynamic, {out24}, {vec, idx24});

    // Wide logical right shift by 22: result bit 1 must be 1.
    const VariableId shifted = linear.addVariable(u115Type, linear.undefInit());
    add(Opcode::LogicalShr, {shifted}, {vec, idx22});
    const VariableId shiftedBit1 = linear.addVariable(u1Type, linear.undefInit());
    add(Opcode::SliceDynamic, {shiftedBit1}, {shifted, idx1});

    ScheduledProgramBuilder scheduled(linear.finish());
    scheduled.addBlock(std::span<const InstructionId>(instructions.begin(),
                                                      instructions.size()));
    const ExecutableModel model{
        .program = scheduled.finish(),
        .interface = {},
        .commitBlockBegin = 0,
        .commitBlockEnd = 0,
    };

    Interpreter interpreter(model);
    if (!interpreter.ready()) {
        return fail("interpreter failed to initialize the probe model");
    }
    const InterpreterResult result = interpreter.eval();
    if (!result.success()) {
        return fail("probe eval failed: " +
                    (result.diagnostic ? result.diagnostic->message : ""));
    }
    const auto bitOf = [&](VariableId v) { return interpreter.value(v).lowWord(); };
    if (bitOf(cat) != 0x2A) {
        return fail("concat order wrong: cat=0x" + std::to_string(bitOf(cat)) +
                    " expected 0x2A (Verilog MSB-first)");
    }
    if (bitOf(sel1) != 1 || bitOf(sel4) != 0) {
        return fail("slice_dynamic index semantics wrong: sel1=" +
                    std::to_string(bitOf(sel1)) + " sel4=" + std::to_string(bitOf(sel4)));
    }
    if (bitOf(out23) != 1 || bitOf(out22) != 0 || bitOf(out24) != 0) {
        return fail("wide slice_dynamic bit order wrong: out22=" +
                    std::to_string(bitOf(out22)) + " out23=" + std::to_string(bitOf(out23)) +
                    " out24=" + std::to_string(bitOf(out24)));
    }
    if (bitOf(shiftedBit1) != 1) {
        return fail("wide logical shift direction wrong: shiftedBit1=" +
                    std::to_string(bitOf(shiftedBit1)));
    }
    std::cerr << "[concat-slice-semantics] all wide concat/slice/shift checks pass\n";
    return 0;
}

} // namespace

int main() { return run(); }
