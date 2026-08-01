#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/array_select_recovery.hpp"
#include "transform/simplify.hpp"

#include "slang/numeric/SVInt.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[array-select-recovery-tests] " << message << '\n';
        return 1;
    }

    template <typename T>
    std::optional<T> getAttr(const Operation &op, std::string_view key)
    {
        auto attr = op.attr(key);
        if (!attr)
        {
            return std::nullopt;
        }
        if (const auto *value = std::get_if<T>(&*attr))
        {
            return *value;
        }
        return std::nullopt;
    }

    ValueId makeLogicValue(Graph &graph, std::string_view name, int32_t width)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), width, false, ValueType::Logic);
    }

    ValueId addConstant(Graph &graph,
                        std::string_view opName,
                        std::string_view valueName,
                        int32_t width,
                        std::string literal)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    OperationId addRegister(Graph &graph, std::string_view name, int32_t width, bool withInit = false)
    {
        const OperationId op = graph.createOperation(OperationKind::kRegister,
                                                     graph.internSymbol(std::string(name)));
        graph.setAttr(op, "width", static_cast<int64_t>(width));
        graph.setAttr(op, "isSigned", false);
        if (withInit)
        {
            graph.setAttr(op, "initValue", std::string("8'd0"));
        }
        return op;
    }

    ValueId addRegisterRead(Graph &graph,
                            std::string_view opName,
                            std::string_view valueName,
                            std::string regSymbol,
                            int32_t width)
    {
        ValueId result = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kRegisterReadPort,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, result);
        graph.setAttr(op, "regSymbol", std::move(regSymbol));
        return result;
    }

    OperationId addRegisterWrite(Graph &graph,
                                 std::string_view opName,
                                 std::string regSymbol,
                                 ValueId guard,
                                 ValueId data,
                                 ValueId mask,
                                 ValueId clk)
    {
        const OperationId op = graph.createOperation(OperationKind::kRegisterWritePort,
                                                     graph.internSymbol(std::string(opName)));
        graph.addOperand(op, guard);
        graph.addOperand(op, data);
        graph.addOperand(op, mask);
        graph.addOperand(op, clk);
        graph.setAttr(op, "regSymbol", std::move(regSymbol));
        graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
        return op;
    }

    ValueId addBinary(Graph &graph,
                      OperationKind kind,
                      std::string_view opName,
                      std::string_view valueName,
                      ValueId lhs,
                      ValueId rhs,
                      int32_t width = 1)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(kind, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, value);
        return value;
    }

    ValueId addMux(Graph &graph,
                   std::string_view opName,
                   std::string_view valueName,
                   ValueId cond,
                   ValueId trueValue,
                   ValueId falseValue,
                   int32_t width)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kMux,
                                                     graph.internSymbol(std::string(opName)));
        graph.addOperand(op, cond);
        graph.addOperand(op, trueValue);
        graph.addOperand(op, falseValue);
        graph.addResult(op, value);
        return value;
    }

    ValueId addSliceStatic(Graph &graph,
                           std::string_view opName,
                           std::string_view valueName,
                           ValueId base,
                           int64_t start,
                           int64_t end)
    {
        ValueId value = makeLogicValue(graph, valueName, static_cast<int32_t>(end - start + 1));
        const OperationId op = graph.createOperation(OperationKind::kSliceStatic,
                                                     graph.internSymbol(std::string(opName)));
        graph.addOperand(op, base);
        graph.addResult(op, value);
        graph.setAttr(op, "sliceStart", start);
        graph.setAttr(op, "sliceEnd", end);
        return value;
    }

    std::size_t countKind(const Graph &graph, OperationKind kind)
    {
        std::size_t count = 0;
        for (const OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    std::vector<OperationId> opsOfKind(const Graph &graph, OperationKind kind)
    {
        std::vector<OperationId> ops;
        for (const OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ops.push_back(opId);
            }
        }
        return ops;
    }

    std::optional<uint64_t> parseConstU64(const Graph &graph, ValueId value)
    {
        if (!value.valid())
        {
            return std::nullopt;
        }
        const OperationId defId = graph.valueDef(value);
        if (!defId.valid())
        {
            return std::nullopt;
        }
        const Operation def = graph.getOperation(defId);
        if (def.kind() != OperationKind::kConstant)
        {
            return std::nullopt;
        }
        const auto literal = getAttr<std::string>(def, "constValue");
        if (!literal)
        {
            return std::nullopt;
        }
        slang::SVInt parsed;
        try
        {
            parsed = slang::SVInt::fromString(*literal);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
        if (parsed.hasUnknown())
        {
            return std::nullopt;
        }
        parsed = parsed.resize(64);
        return *parsed.getRawPtr();
    }

    struct FamilyHandles
    {
        static constexpr std::size_t kRows = 4;
        static constexpr int32_t kWidth = 8;
        std::string regNames[kRows];
        ValueId index;
        ValueId enable;
        ValueId enableB;
        ValueId update;
        ValueId wdata;
        ValueId mask;
        ValueId clk;
        ValueId onehot;
        ValueId selects[kRows];
        ValueId treeReads[kRows];
        ValueId selfReads[kRows];
        ValueId nextValues[kRows];
        ValueId updateConds[kRows];
        ValueId treeRoot;
    };

    struct BuildVariant
    {
        bool constInputs = false;   // drive idx/en/upd/wdata with constants
        uint64_t constIndex = 0;
        bool constEnable = true;
        bool constUpdate = true;
        bool orReadTree = false;    // or-of-ands read tree instead of mux chain
        bool eqGuards = false;      // kEq(idx, C) write guards instead of shift-onehot
        bool wildRead = false;      // extra uncovered read of entries_2
        bool twoWrites = false;     // second write port on entries_1
        bool noRowSelect = false;   // entries_0 guard without a row select
        bool selfrefNonMux = false; // entries_1 nextValue = self + 1
        bool initAttr = false;      // entries_3 carries initValue
    };

    // Builds a 4-entry family: registers entries_0..3, a one-hot selected
    // read-out tree bound to output "out", and one write port per register
    // whose updateCond is an OR of AND terms gated by the row select and
    // whose nextValue is mux(upd, wdata, self).
    Design buildFamilyDesign(const BuildVariant &variant, FamilyHandles &handles)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr std::size_t rows = FamilyHandles::kRows;
        constexpr int32_t width = FamilyHandles::kWidth;
        for (std::size_t row = 0; row < rows; ++row)
        {
            handles.regNames[row] = "entries_" + std::to_string(row);
            addRegister(graph, handles.regNames[row], width, variant.initAttr && row == 3);
        }

        if (variant.constInputs)
        {
            handles.index = addConstant(graph, "idx_op", "idx", 2,
                                        "2'd" + std::to_string(variant.constIndex));
            handles.enable = addConstant(graph, "en_op", "en", 1,
                                         variant.constEnable ? "1'b1" : "1'b0");
            handles.enableB = addConstant(graph, "enb_op", "enb", 1, "1'b1");
            handles.update = addConstant(graph, "upd_op", "upd", 1,
                                         variant.constUpdate ? "1'b1" : "1'b0");
            handles.wdata = addConstant(graph, "wdata_op", "wdata", width, "8'hA5");
        }
        else
        {
            handles.index = makeLogicValue(graph, "idx", 2);
            handles.enable = makeLogicValue(graph, "en", 1);
            handles.enableB = makeLogicValue(graph, "enb", 1);
            handles.update = makeLogicValue(graph, "upd", 1);
            handles.wdata = makeLogicValue(graph, "wdata", width);
            graph.bindInputPort("idx", handles.index);
            graph.bindInputPort("en", handles.enable);
            graph.bindInputPort("enb", handles.enableB);
            graph.bindInputPort("upd", handles.update);
            graph.bindInputPort("wdata", handles.wdata);
        }
        handles.mask = addConstant(graph, "mask_op", "mask", width, "8'hff");
        handles.clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("clk", handles.clk);

        // onehot = 1 << idx (4-bit), sel_k = onehot[k].
        const ValueId one = addConstant(graph, "one_op", "one", 1, "1'b1");
        handles.onehot = addBinary(graph, OperationKind::kShl, "onehot_op", "onehot", one,
                                   handles.index, 4);
        for (std::size_t row = 0; row < rows; ++row)
        {
            handles.selects[row] = addSliceStatic(graph,
                                                  "sel_" + std::to_string(row) + "_op",
                                                  "sel_" + std::to_string(row),
                                                  handles.onehot,
                                                  static_cast<int64_t>(row),
                                                  static_cast<int64_t>(row));
        }

        // Read-out tree over per-entry read ports.
        for (std::size_t row = 0; row < rows; ++row)
        {
            handles.treeReads[row] = addRegisterRead(graph,
                                                     "t" + std::to_string(row) + "_op",
                                                     "t" + std::to_string(row),
                                                     handles.regNames[row],
                                                     width);
        }
        if (variant.orReadTree)
        {
            ValueId ands[rows];
            for (std::size_t row = 0; row < rows; ++row)
            {
                ands[row] = addBinary(graph, OperationKind::kAnd,
                                      "and_" + std::to_string(row) + "_op",
                                      "and_" + std::to_string(row),
                                      handles.selects[row], handles.treeReads[row], width);
            }
            ValueId acc = ands[0];
            for (std::size_t row = 1; row < rows; ++row)
            {
                acc = addBinary(graph, OperationKind::kOr,
                                "or_" + std::to_string(row) + "_op",
                                "or_" + std::to_string(row),
                                acc, ands[row], width);
            }
            handles.treeRoot = acc;
        }
        else
        {
            ValueId chain = handles.treeReads[0]; // default leaf: entries_0
            for (std::size_t row = 1; row < rows; ++row)
            {
                chain = addMux(graph,
                               "m" + std::to_string(row) + "_op",
                               "m" + std::to_string(row),
                               handles.selects[row],
                               handles.treeReads[row],
                               chain,
                               width);
            }
            handles.treeRoot = chain;
        }
        graph.bindOutputPort("out", handles.treeRoot);

        // Write side: one write port per register.
        for (std::size_t row = 0; row < rows; ++row)
        {
            handles.selfReads[row] = addRegisterRead(graph,
                                                     "s" + std::to_string(row) + "_op",
                                                     "s" + std::to_string(row),
                                                     handles.regNames[row],
                                                     width);
            ValueId rowSelect = handles.selects[row];
            if (variant.eqGuards)
            {
                const ValueId rowConst = addConstant(graph,
                                                     "rc_" + std::to_string(row) + "_op",
                                                     "rc_" + std::to_string(row),
                                                     2,
                                                     "2'd" + std::to_string(row));
                rowSelect = addBinary(graph, OperationKind::kEq,
                                      "eq_" + std::to_string(row) + "_op",
                                      "eq_" + std::to_string(row),
                                      handles.index, rowConst);
            }
            ValueId guard;
            if (variant.noRowSelect && row == 0)
            {
                guard = handles.enable;
            }
            else if (row == 2)
            {
                const ValueId termA = addBinary(graph, OperationKind::kLogicAnd,
                                                "g2a_op", "g2a", rowSelect, handles.enable);
                const ValueId termB = addBinary(graph, OperationKind::kLogicAnd,
                                                "g2b_op", "g2b", rowSelect, handles.enableB);
                guard = addBinary(graph, OperationKind::kLogicOr, "g2_op", "g2", termA, termB);
            }
            else
            {
                guard = addBinary(graph, OperationKind::kLogicAnd,
                                  "g" + std::to_string(row) + "_op",
                                  "g" + std::to_string(row),
                                  rowSelect, handles.enable);
            }
            handles.updateConds[row] = guard;

            ValueId nextValue;
            if (variant.selfrefNonMux && row == 1)
            {
                const ValueId oneC = addConstant(graph, "inc_op", "inc", width, "8'd1");
                nextValue = addBinary(graph, OperationKind::kAdd, "nv1_op", "nv1",
                                      handles.selfReads[row], oneC, width);
            }
            else
            {
                nextValue = addMux(graph,
                                   "nv" + std::to_string(row) + "_op",
                                   "nv" + std::to_string(row),
                                   handles.update,
                                   handles.wdata,
                                   handles.selfReads[row],
                                   width);
            }
            handles.nextValues[row] = nextValue;
            addRegisterWrite(graph,
                             "w" + std::to_string(row) + "_op",
                             handles.regNames[row],
                             guard,
                             nextValue,
                             handles.mask,
                             handles.clk);
        }

        if (variant.twoWrites)
        {
            addRegisterWrite(graph, "w1b_op", handles.regNames[1], handles.selects[1],
                             handles.nextValues[1], handles.mask, handles.clk);
        }
        if (variant.wildRead)
        {
            const ValueId wild = addRegisterRead(graph, "wild_op", "wild", handles.regNames[2], width);
            graph.bindOutputPort("wild", wild);
        }
        return design;
    }

    int runRecovery(Design &design, bool rewrite, std::vector<std::string> *logs = nullptr)
    {
        PassManagerOptions managerOptions;
        if (logs != nullptr)
        {
            managerOptions.logLevel = wolvrix::lib::LogLevel::Info;
            managerOptions.logSink = [logs](wolvrix::lib::LogLevel, std::string_view, std::string_view message) {
                logs->emplace_back(message);
            };
        }
        PassManager manager(managerOptions);
        ArraySelectRecoveryOptions options;
        options.rewrite = rewrite;
        manager.addPass(std::make_unique<ArraySelectRecoveryPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("array-select-recovery pass failed");
        }
        return 0;
    }

    int runSimplify(Design &design)
    {
        PassManager manager;
        SimplifyOptions options;
        options.semantics = ConstantFoldOptions::Semantics::TwoState;
        manager.addPass(std::make_unique<SimplifyPass>(options));
        manager.addPass(std::make_unique<SimplifyPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("simplify pipeline failed");
        }
        return 0;
    }

    // Constant folding only (no DCE): the original arm of the semantics test
    // keeps state ops alive even when nothing observes the registers.
    int runConstFold(Design &design)
    {
        PassManager manager;
        ConstantFoldOptions options;
        options.semantics = ConstantFoldOptions::Semantics::TwoState;
        manager.addPass(std::make_unique<ConstantFoldPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("const-fold pipeline failed");
        }
        return 0;
    }

    // Test 1: full 4-entry family is rewritten into kMemory + 1 read port +
    // 4 write ports with the expected operands; census mode leaves the graph
    // untouched and reports the match.
    int testBasicRewriteAndCensus()
    {
        FamilyHandles handles;
        Design design = buildFamilyDesign(BuildVariant{}, handles);
        if (runRecovery(design, true) != 0)
        {
            return 1;
        }
        const Graph &graph = *design.findGraph("top");

        if (countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("expected all register ops to be erased");
        }
        const std::vector<OperationId> mems = opsOfKind(graph, OperationKind::kMemory);
        if (mems.size() != 1)
        {
            return fail("expected exactly one kMemory, got " + std::to_string(mems.size()));
        }
        const Operation mem = graph.getOperation(mems.front());
        if (getAttr<int64_t>(mem, "width").value_or(0) != FamilyHandles::kWidth ||
            getAttr<int64_t>(mem, "row").value_or(0) != 4 ||
            getAttr<bool>(mem, "isSigned").value_or(true))
        {
            return fail("kMemory attributes mismatch");
        }
        const std::string memSymbol(mem.symbolText());
        if (memSymbol.rfind("asr_mem$entries_", 0) != 0)
        {
            return fail("unexpected kMemory symbol: " + memSymbol);
        }

        const std::vector<OperationId> reads = opsOfKind(graph, OperationKind::kMemoryReadPort);
        if (reads.size() != 1)
        {
            return fail("expected exactly one kMemoryReadPort, got " + std::to_string(reads.size()));
        }
        const Operation read = graph.getOperation(reads.front());
        if (read.operands().size() != 1 || read.operands()[0] != handles.index)
        {
            return fail("kMemoryReadPort address must be the tree index");
        }
        if (getAttr<std::string>(read, "memSymbol").value_or("") != memSymbol)
        {
            return fail("kMemoryReadPort memSymbol mismatch");
        }
        if (read.results().size() != 1 || graph.outputPortValue("out") != read.results().front())
        {
            return fail("output port must be rebound to the kMemoryReadPort result");
        }
        if (graph.valueWidth(read.results().front()) != FamilyHandles::kWidth)
        {
            return fail("kMemoryReadPort result width mismatch");
        }

        const std::vector<OperationId> writes = opsOfKind(graph, OperationKind::kMemoryWritePort);
        if (writes.size() != 4)
        {
            return fail("expected four kMemoryWritePort ops, got " + std::to_string(writes.size()));
        }
        bool sawOrGuard = false;
        for (const OperationId writeId : writes)
        {
            const Operation write = graph.getOperation(writeId);
            if (write.operands().size() != 5)
            {
                return fail("kMemoryWritePort must have 5 operands");
            }
            if (write.operands()[1] != handles.index)
            {
                return fail("kMemoryWritePort address must be the guard index");
            }
            if (write.operands()[2] != handles.wdata)
            {
                return fail("kMemoryWritePort data must be the mux new value");
            }
            if (write.operands()[3] != handles.mask || write.operands()[4] != handles.clk)
            {
                return fail("kMemoryWritePort mask/events must be preserved");
            }
            if (getAttr<std::string>(write, "memSymbol").value_or("") != memSymbol)
            {
                return fail("kMemoryWritePort memSymbol mismatch");
            }
            const auto edges = getAttr<std::vector<std::string>>(write, "eventEdge");
            if (!edges || edges->size() != 1 || edges->front() != "posedge")
            {
                return fail("kMemoryWritePort eventEdge mismatch");
            }
            // updateCond must be kLogicAnd(original updateCond, upd).
            const OperationId condDefId = graph.valueDef(write.operands()[0]);
            if (!condDefId.valid())
            {
                return fail("write updateCond must have a defining op");
            }
            const Operation condDef = graph.getOperation(condDefId);
            if (condDef.kind() != OperationKind::kLogicAnd || condDef.operands().size() != 2)
            {
                return fail("write updateCond must be kLogicAnd(cond, upd)");
            }
            const ValueId lhs = condDef.operands()[0];
            const ValueId rhs = condDef.operands()[1];
            const bool hasUpdate = lhs == handles.update || rhs == handles.update;
            const ValueId original = lhs == handles.update ? rhs : lhs;
            if (!hasUpdate)
            {
                return fail("write updateCond must include the mux sel");
            }
            bool originalKnown = false;
            for (std::size_t row = 0; row < FamilyHandles::kRows; ++row)
            {
                if (original == handles.updateConds[row])
                {
                    originalKnown = true;
                    if (row == 2)
                    {
                        sawOrGuard = true;
                    }
                }
            }
            if (!originalKnown)
            {
                return fail("write updateCond must reuse the original guard");
            }
        }
        if (!sawOrGuard)
        {
            return fail("expected the two-term OR guard to be rewritten as well");
        }

        // Census mode: no graph mutation, stats in the log.
        FamilyHandles censusHandles;
        Design censusDesign = buildFamilyDesign(BuildVariant{}, censusHandles);
        const Graph &censusGraph = *censusDesign.findGraph("top");
        const std::size_t opsBefore = censusGraph.operations().size();
        std::vector<std::string> logs;
        if (runRecovery(censusDesign, false, &logs) != 0)
        {
            return 1;
        }
        if (censusGraph.operations().size() != opsBefore ||
            countKind(censusGraph, OperationKind::kMemory) != 0 ||
            countKind(censusGraph, OperationKind::kRegister) != 4)
        {
            return fail("census mode must not modify the graph");
        }
        std::string summary;
        for (const std::string &line : logs)
        {
            if (line.find("array-select-recovery (census):") == 0)
            {
                summary = line;
            }
        }
        if (summary.empty())
        {
            return fail("missing census summary log");
        }
        for (const std::string_view needle : {"matched=1", "rows=4", "read_trees=1", "write_ports=4"})
        {
            if (summary.find(needle) == std::string::npos)
            {
                return fail("census summary missing " + std::string(needle) + ": " + summary);
            }
        }
        return 0;
    }

    // Test 2: 2-state semantics. For each idx/en/upd combination, compare
    // the original graph (fold only) against the rewritten graph (recovery
    // + fold). In the original arm the tree leaves are substituted by
    // distinct constants C_k first (modeling "entry k currently holds C_k"),
    // so the read tree must fold to C_idx; each register write enable must
    // fold to (idx==k)&&en. In the rewritten arm the memory read port
    // address must fold to idx and each write enable must fold to
    // (idx==k)&&en&&upd - the original enable times the mux-hold sel.
    int testFoldSemantics()
    {
        const uint64_t entryConst[4] = {0x11, 0x22, 0x44, 0x88};
        for (uint64_t index = 0; index < 4; ++index)
        {
            for (const bool enable : {false, true})
            {
                for (const bool update : {false, true})
                {
                    BuildVariant variant;
                    variant.constInputs = true;
                    variant.constIndex = index;
                    variant.constEnable = enable;
                    variant.constUpdate = update;

                    // Original arm: substitute leaves, then fold only.
                    FamilyHandles originalHandles;
                    Design original = buildFamilyDesign(variant, originalHandles);
                    {
                        Graph &graph = *original.findGraph("top");
                        for (std::size_t row = 0; row < FamilyHandles::kRows; ++row)
                        {
                            char literal[16];
                            std::snprintf(literal, sizeof(literal), "8'h%02llx",
                                          static_cast<unsigned long long>(entryConst[row]));
                            const ValueId entryValue = addConstant(graph,
                                                                   "ec" + std::to_string(row) + "_op",
                                                                   "ec" + std::to_string(row),
                                                                   FamilyHandles::kWidth,
                                                                   literal);
                            graph.replaceAllUses(originalHandles.treeReads[row], entryValue);
                        }
                    }
                    if (runConstFold(original) != 0)
                    {
                        return 1;
                    }
                    const Graph &originalGraph = *original.findGraph("top");
                    const auto outValue = parseConstU64(originalGraph, originalGraph.outputPortValue("out"));
                    if (!outValue || *outValue != entryConst[index])
                    {
                        return fail("original graph must fold the read tree to entry " +
                                    std::to_string(index));
                    }
                    for (std::size_t row = 0; row < FamilyHandles::kRows; ++row)
                    {
                        const OperationId writeId = originalGraph.findOperation("w" + std::to_string(row) + "_op");
                        if (!writeId.valid())
                        {
                            return fail("original register write port missing after simplify");
                        }
                        const Operation write = originalGraph.getOperation(writeId);
                        const auto cond = parseConstU64(originalGraph, write.operands()[0]);
                        // Rows 0/1/3: guard = sel_k && en. Row 2 has the
                        // two-term guard sel_2 && (en || enB) with enB=1.
                        const uint64_t expected =
                            row == 2 ? (index == 2 ? 1 : 0) : ((index == row && enable) ? 1 : 0);
                        if (!cond || *cond != expected)
                        {
                            return fail("original write enable mismatch for row " +
                                        std::to_string(row) + " idx=" + std::to_string(index) +
                                        " en=" + std::to_string(enable) +
                                        " upd=" + std::to_string(update) +
                                        " got=" + (cond ? std::to_string(*cond) : std::string("none")));
                        }
                    }

                    // Rewritten arm: recovery + fold.
                    FamilyHandles rewrittenHandles;
                    Design rewritten = buildFamilyDesign(variant, rewrittenHandles);
                    if (runRecovery(rewritten, true) != 0)
                    {
                        return 1;
                    }
                    if (runSimplify(rewritten) != 0)
                    {
                        return 1;
                    }
                    const Graph &rewrittenGraph = *rewritten.findGraph("top");
                    const std::vector<OperationId> reads =
                        opsOfKind(rewrittenGraph, OperationKind::kMemoryReadPort);
                    if (reads.size() != 1)
                    {
                        return fail("rewritten graph must keep one kMemoryReadPort");
                    }
                    const Operation read = rewrittenGraph.getOperation(reads.front());
                    const auto addr = parseConstU64(rewrittenGraph, read.operands()[0]);
                    if (!addr || *addr != index)
                    {
                        return fail("rewritten read address must fold to " + std::to_string(index));
                    }
                    const std::vector<OperationId> writes =
                        opsOfKind(rewrittenGraph, OperationKind::kMemoryWritePort);
                    if (writes.size() != 4)
                    {
                        return fail("rewritten graph must keep four kMemoryWritePort ops");
                    }
                    unsigned enabledCount = 0;
                    for (const OperationId writeId : writes)
                    {
                        const Operation write = rewrittenGraph.getOperation(writeId);
                        const auto cond = parseConstU64(rewrittenGraph, write.operands()[0]);
                        if (!cond || *cond > 1)
                        {
                            return fail("write enable must fold to 0 or 1");
                        }
                        const auto writeAddr = parseConstU64(rewrittenGraph, write.operands()[1]);
                        if (!writeAddr || *writeAddr != index)
                        {
                            return fail("write address must fold to the constant index");
                        }
                        const auto data = parseConstU64(rewrittenGraph, write.operands()[2]);
                        if (!data || *data != 0xA5)
                        {
                            return fail("write data must fold to the wdata constant");
                        }
                        enabledCount += static_cast<unsigned>(*cond);
                    }
                    // The one-hot select admits at most the row idx, and only
                    // when the row guard and upd hold; row 2's guard also
                    // fires through enB=1 regardless of en.
                    const unsigned expectedEnabled =
                        (update && (index == 2 || enable)) ? 1u : 0u;
                    if (enabledCount != expectedEnabled)
                    {
                        return fail("expected exactly " + std::to_string(expectedEnabled) +
                                    " enabled write ports, got " + std::to_string(enabledCount));
                    }
                }
            }
        }
        return 0;
    }

    // Test 3: negative cases must not rewrite anything.
    int testNegative(std::string_view name, const BuildVariant &variant)
    {
        FamilyHandles handles;
        Design design = buildFamilyDesign(variant, handles);
        if (runRecovery(design, true) != 0)
        {
            return 1;
        }
        const Graph &graph = *design.findGraph("top");
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 0 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 0)
        {
            return fail(std::string(name) + ": unexpected memory ops created");
        }
        if (countKind(graph, OperationKind::kRegister) != 4)
        {
            return fail(std::string(name) + ": registers must be preserved");
        }
        return 0;
    }

    int testNegatives()
    {
        {
            BuildVariant variant;
            variant.wildRead = true;
            if (testNegative("wild_read", variant) != 0)
            {
                return 1;
            }
        }
        {
            BuildVariant variant;
            variant.twoWrites = true;
            if (testNegative("write_count", variant) != 0)
            {
                return 1;
            }
        }
        {
            BuildVariant variant;
            variant.noRowSelect = true;
            if (testNegative("guard_form", variant) != 0)
            {
                return 1;
            }
        }
        {
            BuildVariant variant;
            variant.selfrefNonMux = true;
            if (testNegative("selfref_form", variant) != 0)
            {
                return 1;
            }
        }
        {
            BuildVariant variant;
            variant.initAttr = true;
            if (testNegative("reset_attr", variant) != 0)
            {
                return 1;
            }
        }
        return 0;
    }

    // Test 4: kEq(idx, C) write guards and the or-of-ands read tree are
    // accepted forms.
    int testEqGuardsAndOrTree()
    {
        {
            FamilyHandles handles;
            BuildVariant variant;
            variant.eqGuards = true;
            Design design = buildFamilyDesign(variant, handles);
            if (runRecovery(design, true) != 0)
            {
                return 1;
            }
            const Graph &graph = *design.findGraph("top");
            if (countKind(graph, OperationKind::kMemory) != 1 ||
                countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
                countKind(graph, OperationKind::kMemoryWritePort) != 4)
            {
                return fail("eq-guard family must be fully rewritten");
            }
            for (const OperationId writeId : opsOfKind(graph, OperationKind::kMemoryWritePort))
            {
                const Operation write = graph.getOperation(writeId);
                if (write.operands().size() != 5 || write.operands()[1] != handles.index)
                {
                    return fail("eq-guard write port address must be the eq index");
                }
            }
        }
        {
            FamilyHandles handles;
            BuildVariant variant;
            variant.orReadTree = true;
            Design design = buildFamilyDesign(variant, handles);
            if (runRecovery(design, true) != 0)
            {
                return 1;
            }
            const Graph &graph = *design.findGraph("top");
            if (countKind(graph, OperationKind::kMemory) != 1 ||
                countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
                countKind(graph, OperationKind::kMemoryWritePort) != 4)
            {
                return fail("or-tree family must be fully rewritten");
            }
            const std::vector<OperationId> reads = opsOfKind(graph, OperationKind::kMemoryReadPort);
            const Operation read = graph.getOperation(reads.front());
            if (read.operands().size() != 1 || read.operands()[0] != handles.index ||
                graph.outputPortValue("out") != read.results().front())
            {
                return fail("or-tree read port must replace the tree root");
            }
        }
        return 0;
    }

    int testPassRegistration()
    {
        bool listed = false;
        for (const std::string &name : availableTransformPasses())
        {
            if (name == "array-select-recovery")
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            return fail("array-select-recovery must be listed by availableTransformPasses");
        }
        std::string error;
        const std::vector<std::string_view> noArgs;
        if (!makePass("array-select-recovery", noArgs, error))
        {
            return fail("makePass must create array-select-recovery: " + error);
        }
        const std::vector<std::string_view> inlineArg = {"-rewrite=false"};
        if (!makePass("array-select-recovery", inlineArg, error))
        {
            return fail("makePass must accept -rewrite=false: " + error);
        }
        const std::vector<std::string_view> splitArgs = {"-rewrite", "off"};
        if (!makePass("array-select-recovery", splitArgs, error))
        {
            return fail("makePass must accept -rewrite off: " + error);
        }
        const std::vector<std::string_view> badValue = {"-rewrite=maybe"};
        if (makePass("array-select-recovery", badValue, error))
        {
            return fail("makePass must reject an invalid -rewrite value");
        }
        const std::vector<std::string_view> unknownArg = {"-rows=4"};
        if (makePass("array-select-recovery", unknownArg, error))
        {
            return fail("makePass must reject unknown options");
        }
        return 0;
    }

    // Test 5: two families where family B's write data bypasses family A's
    // read tree result. After the rewrite B's write ports must reference A's
    // kMemoryReadPort result, not the dead tree root.
    int testTwoFamiliesBypass()
    {
        FamilyHandles a;
        Design design = buildFamilyDesign(BuildVariant{}, a);
        Graph &graph = *design.findGraph("top");

        // Family B: shadow_0..3 with its own select logic; every write's
        // nextValue is family A's read tree root (a bypass), which is not
        // self-referential for B, so data must be that value.
        constexpr std::size_t rows = FamilyHandles::kRows;
        constexpr int32_t width = FamilyHandles::kWidth;
        std::string bRegs[rows];
        for (std::size_t row = 0; row < rows; ++row)
        {
            bRegs[row] = "shadow_" + std::to_string(row);
            addRegister(graph, bRegs[row], width);
        }
        const ValueId idx2 = makeLogicValue(graph, "idx2", 2);
        const ValueId en2 = makeLogicValue(graph, "en2", 1);
        graph.bindInputPort("idx2", idx2);
        graph.bindInputPort("en2", en2);
        const ValueId onehot2 = addBinary(graph, OperationKind::kShl, "onehot2_op", "onehot2",
                                          graph.findValue("one"), idx2, 4);
        ValueId sel2[rows];
        for (std::size_t row = 0; row < rows; ++row)
        {
            sel2[row] = addSliceStatic(graph,
                                       "s2_" + std::to_string(row) + "_op",
                                       "s2_" + std::to_string(row),
                                       onehot2,
                                       static_cast<int64_t>(row),
                                       static_cast<int64_t>(row));
        }
        ValueId bReads[rows];
        for (std::size_t row = 0; row < rows; ++row)
        {
            bReads[row] = addRegisterRead(graph,
                                          "bt" + std::to_string(row) + "_op",
                                          "bt" + std::to_string(row),
                                          bRegs[row],
                                          width);
        }
        ValueId chain = bReads[0];
        for (std::size_t row = 1; row < rows; ++row)
        {
            chain = addMux(graph,
                           "bm" + std::to_string(row) + "_op",
                           "bm" + std::to_string(row),
                           sel2[row],
                           bReads[row],
                           chain,
                           width);
        }
        graph.bindOutputPort("out2", chain);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const ValueId guard = addBinary(graph, OperationKind::kLogicAnd,
                                            "bg" + std::to_string(row) + "_op",
                                            "bg" + std::to_string(row),
                                            sel2[row], en2);
            addRegisterWrite(graph,
                             "bw" + std::to_string(row) + "_op",
                             bRegs[row],
                             guard,
                             a.treeRoot, // bypass from family A's read tree
                             a.mask,
                             a.clk);
        }

        if (runRecovery(design, true) != 0)
        {
            return 1;
        }
        if (countKind(graph, OperationKind::kMemory) != 2 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 2 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 8 ||
            countKind(graph, OperationKind::kRegister) != 0)
        {
            return fail("both families must be fully rewritten");
        }
        // Identify the two memories by symbol and find A's read port result.
        std::string memSymbolA;
        std::string memSymbolB;
        for (const OperationId memId : opsOfKind(graph, OperationKind::kMemory))
        {
            const std::string sym(graph.getOperation(memId).symbolText());
            if (sym.find("entries") != std::string::npos)
            {
                memSymbolA = sym;
            }
            if (sym.find("shadow") != std::string::npos)
            {
                memSymbolB = sym;
            }
        }
        if (memSymbolA.empty() || memSymbolB.empty())
        {
            return fail("expected memories for both families");
        }
        ValueId readResultA;
        for (const OperationId readId : opsOfKind(graph, OperationKind::kMemoryReadPort))
        {
            const Operation read = graph.getOperation(readId);
            if (getAttr<std::string>(read, "memSymbol").value_or("") == memSymbolA)
            {
                readResultA = read.results().front();
            }
        }
        if (!readResultA.valid())
        {
            return fail("family A read port missing");
        }
        for (const OperationId writeId : opsOfKind(graph, OperationKind::kMemoryWritePort))
        {
            const Operation write = graph.getOperation(writeId);
            if (getAttr<std::string>(write, "memSymbol").value_or("") != memSymbolB)
            {
                continue;
            }
            if (write.operands().size() != 5 || write.operands()[1] != idx2)
            {
                return fail("family B write port address must be idx2");
            }
            if (write.operands()[2] != readResultA)
            {
                return fail("family B write data must bypass family A's read port result");
            }
        }
        return 0;
    }
} // namespace

int main()
{
    if (testBasicRewriteAndCensus() != 0)
    {
        return 1;
    }
    if (testFoldSemantics() != 0)
    {
        return 1;
    }
    if (testNegatives() != 0)
    {
        return 1;
    }
    if (testEqGuardsAndOrTree() != 0)
    {
        return 1;
    }
    if (testTwoFamiliesBypass() != 0)
    {
        return 1;
    }
    if (testPassRegistration() != 0)
    {
        return 1;
    }
    std::cout << "array-select-recovery tests passed\n";
    return 0;
}
