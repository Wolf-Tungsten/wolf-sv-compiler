#include "core/grh.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_graph.hpp"
#include "grhsim/am/grh_ir_to_grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_graph_split.hpp"
#include "grhsim/am/grhsim_am_program.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib;
using namespace wolvrix::lib::grhsim::am;

namespace
{

    int fail(std::string_view message)
    {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    grh::ValueId logic(grh::Graph &graph, std::string_view name, int32_t width)
    {
        return graph.createValue(graph.internSymbol(name), width, false,
                                 grh::ValueType::Logic);
    }

    grh::ValueId constant(grh::Graph &graph, std::string_view opName,
                          std::string_view valueName, int32_t width,
                          std::string literal)
    {
        const auto value = logic(graph, valueName, width);
        const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                              graph.internSymbol(opName));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    // NO0006 fixture: two compute ops sharing gsim.node_id 42 (and -> mux,
    // the mux is the node anchor), one lone compute node 43, a register
    // decl (node id 44, no instruction) and a commit write (node id 45).
    // The constant and the changed detector stay unowned.
    struct Fixture
    {
        grh::Design design;
        grh::Graph *graph = nullptr;
    };

    Fixture makeFixture()
    {
        Fixture fixture;
        grh::Graph &graph = fixture.design.createGraph("node_aligned");
        fixture.graph = &graph;

        const auto clk = logic(graph, "clk", 1);
        const auto sel = logic(graph, "sel", 1);
        const auto a = logic(graph, "a", 8);
        const auto b = logic(graph, "b", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("sel", sel);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        const auto one = constant(graph, "one_op", "one", 8, "8'h1");
        const auto mask = constant(graph, "mask_op", "mask", 8, "8'hff");

        const auto t = logic(graph, "t", 8);
        const auto andOp = graph.createOperation(grh::OperationKind::kAnd,
                                                 graph.internSymbol("and_op"));
        graph.addOperand(andOp, a);
        graph.addOperand(andOp, b);
        graph.addResult(andOp, t);
        graph.setAttr(andOp, "gsim.node_id", int64_t{42});

        const auto r = logic(graph, "r", 8);
        const auto muxOp = graph.createOperation(grh::OperationKind::kMux,
                                                 graph.internSymbol("mux_op"));
        graph.addOperand(muxOp, sel);
        graph.addOperand(muxOp, t);
        graph.addOperand(muxOp, one);
        graph.addResult(muxOp, r);
        graph.setAttr(muxOp, "gsim.node_id", int64_t{42});
        graph.setAttr(muxOp, "gsim.node_name", std::string("anchor$r"));

        const auto u = logic(graph, "u", 8);
        const auto orOp = graph.createOperation(grh::OperationKind::kOr,
                                                graph.internSymbol("or_op"));
        graph.addOperand(orOp, a);
        graph.addOperand(orOp, b);
        graph.addResult(orOp, u);
        graph.setAttr(orOp, "gsim.node_id", int64_t{43});

        const auto reg = graph.createOperation(grh::OperationKind::kRegister,
                                               graph.internSymbol("q"));
        graph.setAttr(reg, "width", int64_t{8});
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h0"));
        graph.setAttr(reg, "gsim.node_id", int64_t{44});
        graph.addDeclaredSymbol(graph.operationSymbol(reg));

        const auto q = logic(graph, "q_read", 8);
        const auto regRead = graph.createOperation(grh::OperationKind::kRegisterReadPort,
                                                   graph.internSymbol("q_read_op"));
        graph.addResult(regRead, q);
        graph.setAttr(regRead, "regSymbol", std::string("q"));

        const auto regWrite = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                    graph.internSymbol("q_write"));
        graph.addOperand(regWrite, sel);
        graph.addOperand(regWrite, r);
        graph.addOperand(regWrite, mask);
        graph.addOperand(regWrite, clk);
        graph.setAttr(regWrite, "regSymbol", std::string("q"));
        graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(regWrite, "gsim.node_id", int64_t{45});

        return fixture;
    }

    uint32_t findInstruction(ProgramView program, Opcode opcode)
    {
        uint32_t found = UINT32_MAX;
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            if (program.opcode(InstructionId{index}) == opcode)
            {
                if (found != UINT32_MAX)
                {
                    return UINT32_MAX; // not unique
                }
                found = index;
            }
        }
        return found;
    }

    int testNodeAlignedSplit()
    {
        Fixture fixture = makeFixture();
        diag::Diagnostics diagnostics;
        GrhIRToGrhSimAMGraphLowering lowering;
        std::optional<AmGraph> lowered = lowering.lower(*fixture.graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            return fail("lowering failed for the node-aligned fixture");
        }
        AmGraph &graph = *lowered;
        if (!graph.hasGsimNodeProvenance())
        {
            return fail("gsim node provenance flag not set");
        }
        const ProgramView program = graph.program();
        const uint32_t andInstr = findInstruction(program, Opcode::And);
        const uint32_t muxInstr = findInstruction(program, Opcode::Mux);
        const uint32_t orInstr = findInstruction(program, Opcode::Or);
        const uint32_t detectorInstr = findInstruction(program, Opcode::ChangedPos);
        const uint32_t writeInstr = findInstruction(program, Opcode::RegisterWriteCond);
        if (andInstr == UINT32_MAX || muxInstr == UINT32_MAX || orInstr == UINT32_MAX ||
            detectorInstr == UINT32_MAX || writeInstr == UINT32_MAX)
        {
            return fail("fixture instructions not found or not unique");
        }
        // Per-instruction provenance: node-owned ops carry their node id,
        // the AM clock-domain detector stays unowned, and the materialized
        // commit instruction inherits its source write op's node id.
        if (graph.gsimNodeId(InstructionId{andInstr}) != 42 ||
            graph.gsimNodeId(InstructionId{muxInstr}) != 42 ||
            graph.gsimNodeId(InstructionId{orInstr}) != 43 ||
            graph.gsimNodeId(InstructionId{detectorInstr}) != -1 ||
            graph.gsimNodeId(InstructionId{writeInstr}) != 45)
        {
            return fail("instruction gsim node ids not stamped as expected");
        }

        std::optional<AmGraphSplitContext> context =
            splitAmGraphStage(graph, ActivityScheduleOptions{}, diagnostics);
        if (!context || diagnostics.hasError())
        {
            return fail("node-aligned split failed");
        }
        if (context->atomCount != 4)
        {
            return fail("expected 4 atoms (tree + singleton + detector + commit)");
        }
        const uint32_t treeAtom = context->instructionAtom[andInstr];
        if (context->instructionAtom[muxInstr] != treeAtom)
        {
            return fail("same-node instructions did not group into one atom");
        }
        if (context->atomInstructions[treeAtom] != 2 ||
            context->atomKinds[treeAtom] != static_cast<uint8_t>(AmAtomKind::Tree))
        {
            return fail("node group is not a two-instruction Tree atom");
        }
        const uint32_t treeBegin = context->atomMemberOffsets[treeAtom];
        if (context->atomMembers[treeBegin] != andInstr ||
            context->atomMembers[treeBegin + 1] != muxInstr)
        {
            return fail("tree atom member order is not (and, mux-anchor-last)");
        }
        // The mux root records its select variable as the atom signature.
        const auto muxOperands = program.operands(InstructionId{muxInstr});
        if (context->atomSignatures[treeAtom] != muxOperands[0].value)
        {
            return fail("tree atom signature is not the mux select variable");
        }
        const uint32_t orAtom = context->instructionAtom[orInstr];
        if (orAtom == treeAtom ||
            context->atomKinds[orAtom] != static_cast<uint8_t>(AmAtomKind::Singleton) ||
            context->atomInstructions[orAtom] != 1)
        {
            return fail("lone node did not form a Singleton atom");
        }
        const uint32_t detectorAtom = context->instructionAtom[detectorInstr];
        if (detectorAtom == treeAtom || detectorAtom == orAtom ||
            context->atomKinds[detectorAtom] != static_cast<uint8_t>(AmAtomKind::Singleton))
        {
            return fail("unowned detector did not stay a singleton atom");
        }
        const uint32_t writeAtom = context->instructionAtom[writeInstr];
        if (context->atomIsCommit[writeAtom] != 1 ||
            context->atomKinds[writeAtom] != static_cast<uint8_t>(AmAtomKind::CommitEvent) ||
            context->atomInstructions[writeAtom] != 1)
        {
            return fail("commit write did not stay a singleton CommitEvent atom");
        }
        return 0;
    }

    int testNodeAlignedFullSchedule()
    {
        Fixture fixture = makeFixture();
        diag::Diagnostics diagnostics;
        GrhIRToGrhSimAMGraphLowering lowering;
        std::optional<AmGraph> lowered = lowering.lower(*fixture.graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            return fail("lowering failed for the full-schedule fixture");
        }
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            std::move(*lowered), ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("node-aligned graphToProgram failed");
        }
        uint32_t treeAtoms = 0;
        uint32_t commitAtoms = 0;
        bool provenanceOk = true;
        const ProgramView scheduledView = model->program.view();
        for (uint32_t block = 0; block < model->program.blockCount(); ++block)
        {
            const BlockId blockId{block};
            for (std::size_t index = 0; index < model->program.blockAtomCount(blockId);
                 ++index)
            {
                const AtomId atom = model->program.blockAtom(blockId, index);
                const AmAtomKind kind = model->program.atomKind(atom);
                treeAtoms += kind == AmAtomKind::Tree ? 1 : 0;
                commitAtoms += kind == AmAtomKind::CommitEvent ? 1 : 0;
                // Per-atom provenance: the node-grouped atoms carry their
                // gsim node id (tree 42, lone node 43, commit write 45);
                // every helper atom (detectors, activation traffic) stays
                // unowned (-1).
                bool hasAnd = false;
                bool hasMux = false;
                bool hasOr = false;
                for (std::size_t member = 0;
                     member < model->program.atomInstructionCount(atom); ++member)
                {
                    const Opcode opcode = scheduledView.opcode(
                        model->program.atomInstruction(atom, member));
                    hasAnd = hasAnd || opcode == Opcode::And;
                    hasMux = hasMux || opcode == Opcode::Mux;
                    hasOr = hasOr || opcode == Opcode::Or;
                }
                const int64_t expected = (hasAnd && hasMux) ? 42
                                         : hasOr           ? 43
                                         : kind == AmAtomKind::CommitEvent ? 45
                                                                           : -1;
                if (model->program.atomGsimNodeId(atom) != expected)
                {
                    provenanceOk = false;
                }
            }
        }
        if (treeAtoms != 1 || commitAtoms != 1)
        {
            return fail("scheduled model does not carry the Tree/CommitEvent atoms");
        }
        if (!provenanceOk)
        {
            return fail("scheduled atom gsim node ids were not propagated as expected");
        }
        return 0;
    }

    int testLegacyPathUnchanged()
    {
        Fixture fixture = makeFixture();
        diag::Diagnostics diagnostics;
        GrhIRToGrhSimAMGraphLowering lowering;
        std::optional<AmGraph> lowered = lowering.lower(*fixture.graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            return fail("lowering failed for the legacy-path fixture");
        }
        // Mode Off keeps the SCC atomization even when provenance exists.
        ActivityScheduleOptions options;
        options.gsimNodeAligned = GsimNodeAlignedMode::Off;
        std::optional<AmGraphSplitContext> context =
            splitAmGraphStage(*lowered, options, diagnostics);
        if (!context || diagnostics.hasError())
        {
            return fail("legacy split failed");
        }
        const ProgramView program = lowered->program();
        if (context->atomCount != program.instructionCount())
        {
            return fail("legacy path no longer produces one atom per instruction");
        }
        for (uint32_t atom = 0; atom < context->atomCount; ++atom)
        {
            if (context->atomKinds[atom] == static_cast<uint8_t>(AmAtomKind::Tree))
            {
                return fail("legacy path produced a Tree atom");
            }
        }
        return 0;
    }

    int testNodeAlignedCombLoopUnion()
    {
        // Two compute ops in one combinational cycle, owned by different
        // gsim nodes: the SCC-union fallback packs them into one
        // CombLoopScc atom whose mixed provenance must surface as -2.
        grh::Design design;
        grh::Graph &graph = design.createGraph("node_aligned_loop");
        const auto a = logic(graph, "a", 8);
        const auto b = logic(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        const auto x = logic(graph, "x", 8);
        const auto y = logic(graph, "y", 8);
        const auto andOp = graph.createOperation(grh::OperationKind::kAnd,
                                                 graph.internSymbol("loop_and"));
        graph.addOperand(andOp, a);
        graph.addOperand(andOp, y);
        graph.addResult(andOp, x);
        graph.setAttr(andOp, "gsim.node_id", int64_t{60});
        const auto orOp = graph.createOperation(grh::OperationKind::kOr,
                                                graph.internSymbol("loop_or"));
        graph.addOperand(orOp, x);
        graph.addOperand(orOp, b);
        graph.addResult(orOp, y);
        graph.setAttr(orOp, "gsim.node_id", int64_t{61});

        diag::Diagnostics diagnostics;
        GrhIRToGrhSimAMGraphLowering lowering;
        std::optional<AmGraph> lowered = lowering.lower(graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            return fail("lowering failed for the comb-loop fixture");
        }
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            std::move(*lowered), ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("comb-loop graphToProgram failed");
        }
        uint32_t loopAtoms = 0;
        for (uint32_t block = 0; block < model->program.blockCount(); ++block)
        {
            const BlockId blockId{block};
            for (std::size_t index = 0; index < model->program.blockAtomCount(blockId);
                 ++index)
            {
                const AtomId atom = model->program.blockAtom(blockId, index);
                if (model->program.atomKind(atom) != AmAtomKind::CombLoopScc)
                {
                    continue;
                }
                ++loopAtoms;
                if (model->program.atomGsimNodeId(atom) != -2)
                {
                    return fail("comb-loop union atom did not get the mixed (-2) node id");
                }
            }
        }
        if (loopAtoms != 1)
        {
            return fail("expected exactly one CombLoopScc atom for the two-node cycle");
        }
        return 0;
    }

    int testBlockAtomExport()
    {
        Fixture fixture = makeFixture();
        diag::Diagnostics diagnostics;
        GrhIRToGrhSimAMGraphLowering lowering;
        std::optional<AmGraph> lowered = lowering.lower(*fixture.graph, diagnostics);
        if (!lowered || diagnostics.hasError())
        {
            return fail("lowering failed for the block/atom export fixture");
        }
        std::optional<ExecutableModel> model = GrhIRToGrhSimAMProgram::graphToProgram(
            std::move(*lowered), ActivityScheduleOptions{}, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return fail("block/atom export graphToProgram failed");
        }
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                           "wolvrix_grhsim_am_block_atom_test.jsonl";
        if (!exportGrhSimAmBlockAtomJsonl(*model, path, diagnostics) ||
            diagnostics.hasError())
        {
            return fail("block/atom export failed");
        }
        std::ifstream input(path);
        if (!input)
        {
            return fail("block/atom export did not produce a readable file");
        }
        std::string firstLine;
        std::getline(input, firstLine);
        bool sawTree = false;
        bool sawCommit = false;
        std::size_t blockLines = 0;
        std::size_t atomLines = 0;
        for (std::string line; std::getline(input, line);)
        {
            if (line.starts_with("{\"block\":"))
            {
                ++blockLines;
                continue;
            }
            if (!line.starts_with("{\"atom\":"))
            {
                return fail("block/atom export produced an unrecognized line");
            }
            ++atomLines;
            if (line.find("\"kind\":\"Tree\"") != std::string::npos)
            {
                sawTree = true;
                if (line.find("\"gsim_node\":42") == std::string::npos ||
                    line.find("\"instr_count\":2") == std::string::npos)
                {
                    return fail("tree atom export line lost its provenance");
                }
            }
            if (line.find("\"kind\":\"CommitEvent\"") != std::string::npos)
            {
                sawCommit = true;
                if (line.find("\"gsim_node\":45") == std::string::npos)
                {
                    return fail("commit atom export line lost its provenance");
                }
            }
        }
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        if (firstLine.find("\"block\":0") == std::string::npos ||
            firstLine.find("\"role\":\"entry\"") == std::string::npos || !sawTree ||
            !sawCommit || blockLines + 1 != model->program.blockCount() ||
            atomLines != model->program.atomCount())
        {
            return fail("block/atom export content is inconsistent");
        }
        return 0;
    }

} // namespace

int main()
{
    if (const int result = testNodeAlignedSplit())
    {
        return result;
    }
    if (const int result = testNodeAlignedFullSchedule())
    {
        return result;
    }
    if (const int result = testNodeAlignedCombLoopUnion())
    {
        return result;
    }
    if (const int result = testBlockAtomExport())
    {
        return result;
    }
    if (const int result = testLegacyPathUnchanged())
    {
        return result;
    }
    std::cout << "grhsim-am-node-aligned: all tests passed\n";
    return 0;
}
