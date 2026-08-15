#ifndef WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_PROGRAM_HPP
#define WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_PROGRAM_HPP

#include "core/diagnostics.hpp"
#include "core/grh.hpp"
#include "grhsim/am/grhsim_am_graph.hpp"
#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_program_validate.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    ValidationResult validate(const ProgramInterface &interface,
                              ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const SchedulingFacts &facts,
                              ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const LinearProgramArtifact &artifact,
                              const ValidationOptions &options = {});
    ValidationResult validate(const AmGraph &graph,
                              const ValidationOptions &options = {});
    ValidationResult validate(const ExecutableModel &model,
                              const ValidationOptions &options = {});

    // gsim node-aligned atomization mode (NO0006). When active, the split
    // stage packs scheduling atoms by the gsim.node_id provenance stamped
    // during lowering (one compute atom per gsim node) instead of the
    // heuristic SCC + tree-atom-fold atomization, and the AM graph
    // optimize/fold passes are skipped (the gsim flatten graph is already
    // optimized; the AM passes would erase node anchors and break the 1:1
    // mapping). Auto activates iff the graph carries gsim node provenance.
    enum class GsimNodeAlignedMode : uint8_t
    {
        Auto = 0,
        On,
        Off,
    };

    struct ActivityScheduleOptions
    {
        // Compute 块容量上限，按 atom 计（NO0007 P3：atom 是分区大小的
        // 单位；单超 cap 的 atom 自成一块）。NO0008 树化后 atom=单输出
        // 表达式树（均值 ~2.7 指令），默认 48 是香山实测的性能中性点
        // （host 306.6s ≈ NO0007 基线 309.7s；cap128 形态最优但 +5.2%
        // host，见 am-graph NO0008 §11）。
        std::size_t maxAtomsPerBlock = 48;
        // Commit Blocks may carry a longer ordered state-write chain than a
        // compute block, but never split an indivisible effect atom.
        // 按 commit atom 计数；另有指令数护栏 kMaxGuardEventMergeOps
        // （commit 分区内部）。对齐 legacy maxOpInCommitSupernode。
        std::size_t maxCommitAtomsPerBlock = 4096;
        // Aligns with the legacy enableCoarsen/enableChainMerge switches; the
        // out1/in1 and sibling merge stages are both built in.
        bool enableCoarsening = true;
        bool collectStats = false;
        // DP fixed cost per segment boundary; legacy hard-codes +1 per segment.
        double dpSegmentPenalty = 1.0;
        // Merge host member limit for the out1/in1/sibling merge sweeps, in
        // atoms (P3: formerly an instruction count). 0 selects the default
        // 256: larger values cut fewer cross-block values but grow an
        // oversized-block tail that re-executes wholesale on activation
        // (XiangShan: 7000 -> +6% host time, 256 -> -3% host time vs the
        // legacy partitioner; see compute-partition NO0004).
        std::size_t dpCoarsenAtomBudget = 0;
        // Merge host member limit in member instructions for the out1/in1
        // sweeps (gsim MAX_NODES_PER_SUPER corrected to emitted-instruction
        // terms: tree-atom fold makes an atom ~25 instructions on XiangShan
        // vs ~6.5 emitted lines per gsim node, so the atom budget lets
        // single blocks grow past what the host C++ backend can optimize).
        // 0 = off (atom budget applies); nonzero replaces the atom budget in
        // every merge sweep (out1/in1 and mergeWhen group formation).
        std::size_t dpCoarsenInstrBudget = 0;
        // Post-DP local-move refinement rounds for the compute partition
        // (0 = off). Deterministic, monotone cost decrease; see
        // grhsim_am_compute_graph_partition.cpp.
        std::size_t dpRefinementRounds = 10;
        // mergeWhen coarsen sweep (partition-am-compute-graph, gsim
        // mergeWhenNodes analogue): same-select mux-rooted compute atoms
        // merge into one coarsen cluster when the ready-at-one-wavefront
        // group out-sizes this threshold (gsim MergeWhenSize). Values < 2
        // disable the sweep.
        std::size_t mergeWhenMinGroup = 5;
        // State-anchor sweeps (NO0018, partition-am-compute-graph): rebuild
        // gsim's value-graph register edges as virtual anchors. 0 = off;
        // 1 = read-anchor grouping only; 2 = full (write anchors + degree
        // guards). See grhsim_am_compute_graph_partition.cpp.
        std::size_t stateAnchorMode = 0;
        // Fanout absorption (NO0015): absorb compute atoms with >= 2
        // consumer atoms and at most fanoutAbsorbMaxInstructions member
        // instructions into every consumer atom (pre-partition replication,
        // gsim replication analogue pushed to the read-port boundary).
        // Lab evidence on the corrected anchor graph: cross 394,306 ->
        // 180,213 (1.011x gsim) at cost cap 2 / budget x1.0; production
        // anchor reaches 1.078x at budget x2.0. DEFAULT OFF: the metric win
        // costs host time roughly linear in the duplicated instructions
        // (cap48 x1.0: +164% host) until the orphan overhead is fixed.
        std::size_t fanoutAbsorbMaxInstructions = 0;
        // Global duplication budget: fanoutAbsorbBudgetMult x total compute
        // instructions. Absorption stops when the budget is exhausted.
        double fanoutAbsorbBudgetMult = 1.0;
        // Per-atom consumer-count cap (hubs above it stay shared).
        std::size_t fanoutAbsorbMaxConsumers = 256;
        // Tree-atom fold set size cap (NO0002 L2 alignment): fold trees are
        // partitioned into connected sub-trees of at most this many member
        // instructions. 0 = uncapped (legacy behavior). The
        // WOLVRIX_GRHSIM_AM_TREE_ATOM_FOLD_MAX_INSTR env var overrides when
        // this is 0. Value 2 aligns the AM atom count with the gsim node
        // count on the XiangShan exec-GRH (0.95x).
        std::size_t treeAtomFoldMaxInstr = 0;
        // NO0006 gsim node-aligned atomization (see GsimNodeAlignedMode).
        // The WOLVRIX_GRHSIM_AM_NODE_ALIGNED env var (0|1) overrides this
        // option when set.
        GsimNodeAlignedMode gsimNodeAligned = GsimNodeAlignedMode::Auto;
        // 实验开关（默认 off）：移除 EntryBlock（block 0）detector 组的
        // preset 激活边中、目标 compute Block 同时被 commit Block act.b
        // 反向激活的那些边——这类 Block 不再由钟/输入变化 preset 激活，
        // 只能在 round-1 末尾由 commit act.b 于 round-2 触发。护栏：若某个
        // watch 变量的全部 preset 目标都会被移除，则保留该组（输入不得
        // 失去 EntryBlock watch）。eval() 首次求值的全块激活路径不受影响。
        // off 时输出与既往逐字节一致。
        bool skipPresetActivation = false;
    };

    // Resolves the effective node-aligned mode (NO0006): the
    // WOLVRIX_GRHSIM_AM_NODE_ALIGNED env var (0|1) wins over the option;
    // Auto activates iff the graph carries gsim node provenance.
    bool gsimNodeAlignedScheduling(const AmGraph &graph,
                                   const ActivityScheduleOptions &options);

    // Escape hatch (NO0006): WOLVRIX_GRHSIM_AM_NODE_ALIGNED_OPTIMIZE=1
    // force-runs the AM graph optimize passes even when node-aligned
    // scheduling is active (A/B bisection; erases the 1:1 node mapping).
    bool gsimNodeAlignedOptimizeForced();

    struct AmOptimizeOptions
    {
        bool dce = true;
        bool constFold = true;
        bool cse = true;
        // Bypass single-operand Assign instructions (alias the result to the
        // operand). Assigns reading a state variable are kept by default:
        // commit-side instructions must observe the pre-commit value through
        // the snapshot (lowering preCommitValue). See stateReadAlias for the
        // refined rule.
        bool assignAlias = true;
        // Refined form of assignAlias: an Assign reading a state variable
        // may still be bypassed when no commit-side instruction (state
        // write, change detector, host effect; transitively through
        // single-operand Assign chains) references its result. Only the
        // preCommit snapshots that actually feed commit operands are kept.
        bool stateReadAlias = true;
        // Rewrite 1-bit LogicAnd/LogicOr/LogicNot to the bitwise And/Or/Not
        // (identical semantics on 1-bit operands) so CSE can merge the two
        // forms instead of keeping parallel guard-logic copies.
        bool logicUnify = true;
        // Absorb a Not/LogicNot that feeds only Mux selects (single-use or
        // shared): mux(!c, a, b) == mux(c, b, a) with the arms swapped, and
        // the Not instruction is removed. Restricted to 1-bit inverted
        // operands because the IR requires a 1-bit Mux select.
        bool muxNotAbsorb = true;
        // Fuse SliceStatic chains (slice(slice(x, l1), l2) -> slice(x,
        // l1+l2)) and bypass identity slices (lsb 0, identical type).
        bool sliceFuse = true;
        // Fuse packed-vector element-write insert patterns
        // (concat(slice(row, hi), data, slice(row, lo)) and its two-operand
        // boundary forms) into a single Insert instruction (NO0004).
        bool insertFuse = true;
        // Canonicalize 1-bit Not to Eq(x, 0): matches the reference flow's
        // negation form and lets CSE merge it with an explicit x == 0.
        // Measured on XiangShan (compute-partition NO0005): pure bucket
        // shift, no ge2/sum_outdeg gain (CSE merged 6 pairs) — default off.
        bool notUnify = false;
        // Fold MemoryRead instructions with a constant address on memories
        // that are never written. Emitted/interpreted storage is
        // zero-initialized, so Undef/Zero init reads as zero and Constant
        // init carries the packed lane literal; both are compile-time known.
        bool constMemFold = true;
        // Allow fold/CSE/alias to eliminate results that are referenced by
        // the ProgramInterface (output ports, declared observables): the
        // interface entries are re-pointed to the alias representative and
        // the visibility roles move with them. false restores the legacy
        // policy (any roled variable is untouchable).
        bool interfaceAlias = true;
    };

    struct GrhSimAmCppOptions
    {
        std::filesystem::path outputDirectory;
        std::string modelName;
        uint64_t maxOutputFileBytes = UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);
        // NO0006 trace comments: annotate the generated block sources with
        // per-block banners and per-atom provenance comments
        // (// ===== block ... ===== / // --- atom ... gsim_node=<id> ---).
        // Comment-only; no semantic effect on the emitted model.
        bool traceComments = true;
        std::map<std::string, std::string, std::less<>> attributes;
    };

    struct GrhSimAmCppResult
    {
        bool success = true;
        std::vector<std::string> artifacts;
        // Same-select mux fusion (NO0008, block-level mux-run fusion over
        // mux-rooted atoms): number of mux assignments covered by fused
        // if/else segments in the emitted sources.
        uint64_t muxAtomFused = 0;
        // NO0013 windowed emission of lane-build concat cones (F1 chains,
        // F2 standalone concats): planned/rewritten instruction counts.
        uint64_t windowedChains = 0;
        uint64_t windowedSteps = 0;
        uint64_t windowedConcatsF2 = 0;
        uint64_t windowedSkippedSlices = 0;
        uint64_t windowedRemappedSlices = 0;
        uint64_t windowedMaterialized = 0;
        uint64_t windowedBailedChains = 0;
        // NO0014 dynamic bit-field functional-update cone collapse.
        uint64_t dynBlendChains = 0;
        uint64_t dynBlendCones = 0;
        uint64_t dynBlendSkipped = 0;
        uint64_t dynBlendRemapped = 0;
        uint64_t dynBlendMaterialized = 0;
        uint64_t dynBlendBailed = 0;
    };

    // The pipeline currency is the AmGraph: lowering builds the graph
    // natively from GRH IR, graph passes rewrite it in place, and the linear
    // AM Program is materialized from the graph only at schedule finalize.
    class GrhIRToGrhSimAMGraphLoweringStage
    {
    public:
        virtual ~GrhIRToGrhSimAMGraphLoweringStage() = default;

        virtual std::optional<AmGraph>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics) = 0;
    };

    class GrhSimAmCppEmitStage
    {
    public:
        virtual ~GrhSimAmCppEmitStage() = default;

        virtual GrhSimAmCppResult
        emit(const ExecutableModel &model,
             const GrhSimAmCppOptions &options,
             wolvrix::lib::diag::Diagnostics &diagnostics) = 0;
    };

    struct GrhIRToGrhSimAMProgramResult
    {
        bool success = false;
        std::vector<std::string> artifacts;
        std::optional<ExecutableModel> model;
    };

    class GrhIRToGrhSimAMProgram
    {
    public:
        GrhIRToGrhSimAMProgram(GrhIRToGrhSimAMGraphLoweringStage &lowering,
                             GrhSimAmCppEmitStage &emitter);

        // Configures the AM optimization stage that runs between lowering and
        // scheduling. Defaults to all optimizations enabled.
        void setAmOptimizeOptions(AmOptimizeOptions options);

        // GRHSIM AM Graph -> GRHSIM AM Program：split-am-graph → opt-am-compute-graph →
        // partition-am-compute-graph → partition-am-commit-graph → materialize。
        static std::optional<ExecutableModel>
        graphToProgram(AmGraph &&graph,
                       const ActivityScheduleOptions &options,
                       wolvrix::lib::diag::Diagnostics &diagnostics);

        std::optional<AmGraph>
        lower(const wolvrix::lib::grh::Graph &graph,
              wolvrix::lib::diag::Diagnostics &diagnostics);
        GrhIRToGrhSimAMProgramResult run(AmGraph &&graph,
                                       const ActivityScheduleOptions &scheduleOptions,
                                       const GrhSimAmCppOptions &emitOptions,
                                       wolvrix::lib::diag::Diagnostics &diagnostics);
        GrhIRToGrhSimAMProgramResult run(const wolvrix::lib::grh::Graph &graph,
                                       const ActivityScheduleOptions &scheduleOptions,
                                       const GrhSimAmCppOptions &emitOptions,
                                       wolvrix::lib::diag::Diagnostics &diagnostics);

    private:
        GrhIRToGrhSimAMGraphLoweringStage &lowering_;
        GrhSimAmCppEmitStage &emitter_;
        AmOptimizeOptions optimizeOptions_;
    };

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRH_IR_TO_GRHSIM_AM_PROGRAM_HPP
