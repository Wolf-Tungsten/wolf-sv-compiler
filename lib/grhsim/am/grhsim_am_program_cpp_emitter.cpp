#include "grhsim/am/grhsim_am_program_cpp_emitter.hpp"

#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        constexpr std::string_view kContext = "grhsim-am-cpp-emit";

        bool isCppIdentifier(std::string_view text)
        {
            if (text.empty() ||
                !(std::isalpha(static_cast<unsigned char>(text.front())) || text.front() == '_'))
            {
                return false;
            }
            for (char ch : text.substr(1))
            {
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
                {
                    return false;
                }
            }
            static const std::unordered_set<std::string_view> keywords = {
                "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
                "bool", "break", "case", "catch", "char", "class", "compl", "concept",
                "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
                "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do",
                "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
                "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
                "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
                "operator", "or", "or_eq", "private", "protected", "public", "register",
                "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
                "static_assert", "static_cast", "struct", "switch", "template", "this",
                "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
                "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor",
                "xor_eq",
            };
            return !keywords.contains(text);
        }

        // Port names arriving through imported graphs (e.g. gsim executable
        // GRH) may contain non-identifier characters ("difftest$$perfCtrl$$clean").
        // Sanitize them exactly like the legacy GrhSIM emitter does, so the
        // difftest port ABI keeps matching its double-underscore alternatives.
        std::string sanitizeCppIdentifier(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            if (text.empty() || (!std::isalpha(static_cast<unsigned char>(text.front())) && text.front() != '_'))
            {
                out.push_back('_');
            }
            for (unsigned char ch : text)
            {
                if (std::isalnum(ch) || ch == '_')
                {
                    out.push_back(static_cast<char>(ch));
                }
                else
                {
                    out.push_back('_');
                }
            }
            return out;
        }

        std::string cppScalarType(uint32_t width)
        {
            if (width == 1)
            {
                return "bool";
            }
            if (width <= 8)
            {
                return "std::uint8_t";
            }
            if (width <= 16)
            {
                return "std::uint16_t";
            }
            if (width <= 32)
            {
                return "std::uint32_t";
            }
            return "std::uint64_t";
        }

        std::string cppPortType(const Type &type)
        {
            if (type.kind != TypeKind::BitVector)
            {
                return {};
            }
            if (type.bitWidth <= 64)
            {
                return cppScalarType(type.bitWidth);
            }
            return "std::array<std::uint64_t, " +
                   std::to_string((static_cast<uint64_t>(type.bitWidth) + 63U) / 64U) + ">";
        }

        std::string cppStringLiteral(std::string_view bytes)
        {
            std::string result = "\"";
            for (unsigned char byte : bytes)
            {
                if (byte == '\\' || byte == '"')
                {
                    result.push_back('\\');
                    result.push_back(static_cast<char>(byte));
                }
                else if (byte >= 0x20 && byte <= 0x7e)
                {
                    result.push_back(static_cast<char>(byte));
                }
                else
                {
                    result.push_back('\\');
                    result.push_back(static_cast<char>('0' + ((byte >> 6U) & 0x7U)));
                    result.push_back(static_cast<char>('0' + ((byte >> 3U) & 0x7U)));
                    result.push_back(static_cast<char>('0' + (byte & 0x7U)));
                }
            }
            result.push_back('"');
            return result;
        }

        std::string maskExpr(uint32_t width)
        {
            if (width >= 64)
            {
                return "UINT64_MAX";
            }
            return "((UINT64_C(1) << " + std::to_string(width) + ") - UINT64_C(1))";
        }

        std::string wordMaskLiteral(uint64_t mask)
        {
            char buffer[16];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), mask, 16);
            if (error != std::errc{})
            {
                return "UINT64_C(" + std::to_string(mask) + ")";
            }
            return "UINT64_C(0x" + std::string(buffer, end) + ")";
        }

        std::string byteMaskLiteral(uint8_t mask)
        {
            char buffer[2];
            const auto [end, error] =
                std::to_chars(buffer, buffer + sizeof(buffer), mask, 16);
            if (error != std::errc{})
            {
                return "UINT8_C(" + std::to_string(mask) + ")";
            }
            return "UINT8_C(0x" + std::string(buffer, end) + ")";
        }

        // NO0017 §5 explode guard-bail categories (EmitState::explodeBails
        // index); the first recorded reason wins per state.
        constexpr std::size_t kExplodeBailHostVisible = 0;   // port / declared label
        constexpr std::size_t kExplodeBailRandomInit = 1;    // random(-seeded) init
        constexpr std::size_t kExplodeBailResultDefined = 2; // instruction result
        constexpr std::size_t kExplodeBailPlanCaptured = 3;  // windowed/dynblend plan
        constexpr std::size_t kExplodeBailNonSliceRead = 4;  // whole-width read
        constexpr std::size_t kExplodeBailDynamicSlice = 5;  // SliceDynamic source
        constexpr std::size_t kExplodeBailWideSlice = 6;     // slice result > 64 bits
        constexpr std::size_t kExplodeBailSliceWidth = 7;    // no uniform element width
        constexpr std::size_t kExplodeBailArraySliceWidth = 8; // SliceArray width != K
        constexpr std::size_t kExplodeBailChangedDetector = 9; // unfused changed.*
        constexpr std::size_t kExplodeBailDynamicWrite = 10; // RegisterWriteDynLane
        constexpr std::size_t kExplodeBailNonconstantMask = 11;
        constexpr std::size_t kExplodeBailMaskElements = 12; // mask spans > 32 elements
        constexpr std::size_t kExplodeBailWriteData = 13;    // write data exploded, K mismatch
        constexpr std::size_t kExplodeBailCount = 14;
        // Cap on the per-element unrolling of one constant-mask write site.
        constexpr uint32_t kExplodeMaskElementLimit = 32;

        struct EmitState
        {
            struct Storage
            {
                uint64_t offset = 0;
                uint32_t wordCount = 0;
            };

            ProgramView program;
            uint32_t blockCount = 0;
            std::vector<Type> variableTypes;
            std::vector<Storage> variableStorage;
            uint64_t wideWords = 0;
            uint64_t realValues = 0;
            uint64_t stringValues = 0;
            // Compile-time runtime-profile switch (GrhSimAmCppOptions attribute
            // "runtimeProfile", default off). When false the generated model
            // carries no profile counters or hot-path profile branches, and the
            // host-facing profile API degrades to no-op stubs.
            bool runtimeProfile = false;
            // Compile-time full-evaluation switch (attribute "fullEvaluation",
            // default off). When true every scan/commit byte chunk executes its
            // blocks unconditionally, bypassing activity filtering. Block
            // bodies still perform their activation bookkeeping, so the mode
            // measures exactly the value of activity filtering itself.
            bool fullEvaluation = false;
            // Compile-time changed-trace switch (attribute "changedTrace",
            // default off). When true the model can stream per-(eval, round)
            // changed-variable records to the file named by
            // EMU_AM_CHANGED_TRACE (windowed by EMU_AM_TRACE_BEGIN_EVAL /
            // EMU_AM_TRACE_END_EVAL) for offline ideal-dynamic-work replay.
            bool changedTrace = false;
            // Compile-time branchy-mux switch (attribute "branchyMux", default
            // off). When true scalar Mux instructions emit if/else arm
            // assignments instead of a ternary expression, chopping block
            // bodies into many small basic blocks so the C++ backend never
            // faces one giant straight-line region (emit-cost NO0001 B2).
            bool branchyMux = false;
            // NO0006 trace comments (GrhSimAmCppOptions::traceComments,
            // default on): per-block banner and per-atom provenance comment
            // lines in the block sources. Comment-only.
            bool traceComments = false;
            std::unordered_map<uint32_t, uint32_t> onceSlotByInstruction;
            uint32_t onceSlotCount = 0;
            std::unordered_map<uint32_t, uint32_t> pendingEventSlotByInstruction;
            uint32_t pendingEventSlotCount = 0;
            // Changed results read from a block other than their definition:
            // only these join the round-cleared dirty list. They are also the
            // only persistent narrow values addressed by a runtime index
            // (set_changed_result / clear_changed_results), so they keep a
            // dense array storage (changedResults_[denseId]) while every other
            // persistent narrow value becomes an independent class member
            // (v<VariableId>). Non-CR variables map to ~0u.
            std::vector<bool> crossBlockChangedResults;
            std::vector<uint32_t> changedResultDenseIndex;
            uint32_t changedResultCount = 0;
            // First VariableId that gets a v<K> member and the total member
            // count, recorded while emitting the member declarations. Members
            // are declared without initializers (clang miscompiles the
            // implicit default constructor at multi-million {}-initialized
            // members); init() zeroes the contiguous member region with one
            // memset from the first member instead.
            uint32_t firstMemberVariable = std::numeric_limits<uint32_t>::max();
            uint32_t memberValueCount = 0;
            std::unordered_map<uint32_t, DpiImportId> dpiImportBySymbol;
            std::vector<bool> referencedDpiImports;
            std::vector<InstructionId> finalSystemTasks;
            // ST00009 block-local value localization: a narrow scalar value defined in
            // exactly one block and only read later in the same block is emitted as a
            // C++ local instead of a persistent member. variableEscapeFlags != 0 keeps
            // the persistent storage (kEscape* bits below).
            std::vector<uint32_t> variableDefBlock;
            std::vector<uint32_t> variableDefPosition;
            std::vector<uint8_t> variableEscapeFlags;
            std::vector<std::vector<uint32_t>> blockDefinedVariables;
            mutable std::vector<uint32_t> localValueStamps;
            mutable std::vector<uint32_t> localValueIndices;
            mutable uint32_t activeLocalityBlock = std::numeric_limits<uint32_t>::max();
            mutable std::vector<uint32_t> activeLocalityDeclarations;
            // NO0016 narrow-value storage classes: every localized value is
            // emitted with a C type sized to its bit width (class 0=uint8_t,
            // 1=uint16_t, 2=uint32_t, 3=uint64_t) instead of a uniform
            // uint64_t slot, shrinking chunked-Block shared arrays ~8x for
            // 1-bit-dominated networks. localValueClasses is per variable;
            // values whose emission can take the slot's address (word-level
            // helpers on wide instructions, windowed/dynblend plans, array or
            // host-visible ops) are pinned to class 3 by
            // classifyLocalValueStorage. Per-class indices (not flat) are
            // stored in localValueIndices; activeLocalityDeclClasses parallels
            // activeLocalityDeclarations.
            std::vector<uint8_t> localValueClasses;
            mutable std::vector<uint8_t> activeLocalityDeclClasses;
            mutable std::array<uint32_t, 4> activeLocalityClassCounts{};
            // NO0016 Stage B chunk-internal scalarization: a localized value
            // of an oversized (chunked) Block whose reads all stay inside its
            // defining chunk is emitted as a chunk-function-local typed
            // scalar (register candidate) instead of a shared parent-scope
            // array slot — the microbenchmarked 13x gap between array
            // round-trips and registers. Only cross-chunk values keep array
            // slots. chunkScalarIndex != kInvalidChunkScalar marks an
            // internal value and holds its dense per-(chunk, class) index;
            // blockChunkScalarCounts[block][chunk][class] gives the
            // declaration counts. Values pinned to class 3 (address-taken)
            // and operands/results of mux-fusion-covered instructions are
            // never internalized (their reads are emitted at the run head /
            // plan tail positions, not at their instruction position).
            std::vector<uint32_t> chunkScalarIndex;
            std::vector<std::vector<std::array<uint32_t, 4>>> blockChunkScalarCounts;
            uint64_t chunkLocalScalarCount = 0;
            // Oversized-Block chunking (blockChunkInstructions): while an
            // oversized Block's instruction stream is emitted as
            // block_<id>_chunk_<k>() member functions, this holds the chunked
            // Block's id. The Block's locals (localblk_<id>[k]), write-point
            // watch flags (wrChgblk_<id>[k] / arrChgblk_<id>[k]) and
            // detector-group flags (detGrpblk_<id>[k]) are addressed as
            // parent-scope arrays so every chunk function can share them
            // through pointer parameters.
            mutable uint32_t activeChunkedBlock = std::numeric_limits<uint32_t>::max();
            // Static-scan local-relay context. While a compute Block is emitted
            // into the byte-chunk scan form, forward activations whose target
            // bit is owned by the current byte chunk set the scan-local
            // byteFlags variable instead of the global activity words (the
            // legacy batch-local relay idiom). scanRelayByte < 0 disables the
            // relay (entry/commit Blocks and any non-scan emission context).
            mutable int32_t scanRelayByte = -1;
            mutable uint8_t scanRelayMask = 0;
            // ST00010 detector-group folding: block-tail runs of
            // scheduler-materialized (changed.*, act.f/act.b) watch groups are
            // re-grouped at emit time by activation target signature. All
            // detectors of one group accumulate branchlessly into a block-local
            // flag (detGrp_N) and the group merges once, replacing one branch +
            // one activation write per detector with one per group. Folded
            // event variables are never assigned, so they are dropped from the
            // v<K> member declarations. The equivalence argument: activity bits
            // are idempotent and order-free, so "any event of the group fired"
            // activates exactly the same targets as the per-detector branches;
            // each detector still owns and updates its private old baseline.
            struct DetectorGroupPlan
            {
                struct Group
                {
                    bool forward = true;
                    // Union of the member act target lists (identical by
                    // construction: the group key is the sorted target set).
                    std::vector<BlockId> targets;
                    // Sum of member act target counts; preserves the per-act
                    // profile counter semantics under one merged merge.
                    uint64_t originalTargetCount = 0;
                };
                std::vector<Group> groups;
                // All maps are keyed by block instruction position.
                // Folded changed instruction -> group ids it accumulates into
                // (one per consumed act direction, so 1 or 2 entries).
                std::unordered_map<uint32_t, std::vector<uint32_t>> accumGroups;
                // Member acts replaced by their group's merge.
                std::unordered_set<uint32_t> skippedActs;
                // Position after which each group's merge is emitted.
                std::unordered_map<uint32_t, std::vector<uint32_t>> mergesAfter;
            };
            std::vector<std::optional<DetectorGroupPlan>> blockDetectorPlans;
            std::vector<uint8_t> foldedDetectorEvents;
            uint64_t detectorFoldedCount = 0;
            uint64_t detectorGroupCount = 0;
            // ST00011 array write-point activation: a commit Block's tail
            // changed.any on an Array state target costs one whole-array
            // compare (std::equal -> memcmp) plus one whole-array baseline
            // copy (std::copy_n -> memcpy) every round, because the detector
            // executes unconditionally with its Block. The replacement moves
            // change detection to the Block's own write sites: mem.write
            // compares the touched element words while writing
            // (masked_write_words_detect), mem.fill tracks per-element change
            // inside its fill loop (assign_words_detect / slice_words_detect),
            // and the tail detector becomes a read of the accumulated flag.
            // Completeness: every Block that writes an Array state target owns
            // one detector per (Block, target) pair whose ActBackward targets
            // cover all reader Blocks, so detecting exactly this Block's own
            // writes still activates every reader of every actual change;
            // baseline-drift firings caused by other writer Blocks were
            // redundant duplicates (activation is idempotent). A write that
            // restores the previous value no longer re-triggers readers,
            // matching the legacy write-point suppression and keeping eval
            // convergence identical to the compare-based form.
            struct ArrayWatchPlan
            {
                // changed.any position -> accumulator id consumed instead of
                // the whole-array compare/copy.
                std::unordered_map<uint32_t, uint32_t> detectorAccum;
                // mem.write/mem.fill position -> accumulator id fed by the
                // write site's fused change detection (at most one detector
                // per (Block, Array) pair by construction).
                std::unordered_map<uint32_t, uint32_t> writeAccum;
                uint32_t accumCount = 0;
            };
            std::vector<std::optional<ArrayWatchPlan>> blockArrayWatchPlans;
            uint64_t arrayWatchReplacedCount = 0;
            // Mutable emission context for the current instruction position
            // (same pattern as scanRelayByte): -1 when the position is not a
            // planned write site / detector.
            mutable int32_t arrayWriteAccum = -1;
            mutable int32_t arrayDetectorAccum = -1;
            // Commit event gating: every commit Block opens with its
            // aggregated changed.* gate detectors (one per watched clock-domain
            // source, or one per latch nextValue), so the emitter evaluates the
            // deduplicated OR of their results once and wraps the rest of the
            // Block (all writes plus the tail watch/activation traffic) in a
            // single batch check. The gate replaces the legacy per-statement
            // event evaluation entirely.
            struct CommitGate
            {
                std::string expression;
                std::string preamble;
                uint32_t headCount = 0;
            };
            std::vector<CommitGate> blockCommitGate;
            uint64_t commitGateBlockCount = 0;
            // Commit-input gating (GrhSimAmCppOptions attribute
            // "commitInputGating", default off): backward-slice each commit
            // Block from its state writes. Trackable scalar state leaves
            // propagate a dirty byte from their already-fused write-point
            // comparisons; the small non-state remainder uses snapshots.
            bool commitInputGating = false;
            // Cost-aware refinement of commit-input gating (default off):
            // retain a gate only when its statically protected tail has at
            // least four instructions per dirty propagation edge. Rejected
            // blocks stay on the existing event-gate path.
            bool commitInputSparseGating = false;
            uint64_t commitInputSparseRejectedBlocks = 0;
            uint64_t commitInputSparseRejectedWrites = 0;
            uint64_t commitInputSparseRejectedEdges = 0;
            uint64_t commitInputSnapshotCount = 0;
            uint64_t commitInputGateCount = 0;
            uint64_t commitInputTrackedStateCount = 0;
            uint64_t commitInputProducerBlockCount = 0;
            uint64_t commitInputDirtyEdgeCount = 0;
            uint64_t commitInputGatedInstructions = 0;
            uint64_t commitInputGatedWrites = 0;
            std::unordered_map<uint32_t, std::vector<uint32_t>>
                commitInputDirtyGatesByInstruction;
            std::unordered_map<uint32_t, std::vector<uint32_t>>
                commitInputDirtyGatesByBlock;
            // Guard-event gating (GrhSimAmCppOptions attribute
            // "guardEventGating", default off): a pure guard Block is a
            // compute Block whose only observable effects are event-gated
            // host tasks (fatal/fwrite/finish with fire conditions of the
            // form `fire && (changedResults_ slot OR)`, fatal additionally
            // `!firstEval_`-masked); every other instruction is side-effect
            // free and every Block-written value stays Block-local. The
            // scan then wraps the whole Block body in the OR of the guard
            // tasks' changedResults_ slots: with the gate closed no guard
            // task can fire and nothing else is observable, so the Block
            // body is skipped outright (e.g. xiangshan's assertion Block on
            // the clock negedge). Eligibility is planned by
            // planGuardEventGates; an empty expression means no gate.
            struct GuardGate
            {
                std::string expression;
                uint64_t atoms = 0;
                uint64_t instructions = 0;
            };
            bool guardEventGating = false;
            std::vector<GuardGate> blockGuardGate;
            uint64_t guardGatedBlockCount = 0;
            uint64_t guardGatedAtoms = 0;
            uint64_t guardGatedInstructions = 0;
            // ST00013 scalar write-point detection fusion (P5): a commit Block
            // tail changed.any on a BitVector state target costs one compare
            // plus one old-baseline store per round even when nothing was
            // written. The replacement moves detection into the Block's own
            // RegisterWrite sites: the write computes its next value, and only
            // a real change stores and raises a block-local flag (wrChg_N,
            // the legacy "compare at write point, store on change" idiom).
            // The tail detector then reads the flag (into its ST00010 group
            // accumulator when folded, else directly into the event variable)
            // and its old baseline goes dead. Multi-write targets OR-accumulate
            // across their sites; a write restoring the previous value raises
            // nothing, preserving eval convergence. Eligibility: same-Block
            // RegisterWrite sites cover the target (LatchWrite/MemoryWrite are
            // not fused), event consumed only by same-Block act.f/act.b, no
            // cross-block changed result.
            struct ScalarWatchPlan
            {
                // RegisterWrite position -> flag id raised on real change.
                std::unordered_map<uint32_t, uint32_t> writeRaise;
                // changed.any position -> flag id replacing the compare+store.
                std::unordered_map<uint32_t, uint32_t> detectorRaise;
                uint32_t flagCount = 0;
            };
            std::vector<std::optional<ScalarWatchPlan>> blockScalarWatchPlans;
            uint64_t scalarWatchFusedCount = 0;
            // Mutable emission context (same pattern as arrayWriteAccum): the
            // current RegisterWrite position's raise flag, -1 when none.
            mutable int32_t scalarWriteRaise = -1;
            // Block-level same-select mux fusion (NO0008): mux-rooted atoms
            // (Singleton/Tree carrying a select signature) that sit adjacent
            // in a Block form a fusion run; the head instruction of the run
            // emits every run atom's cone members (in atom order) followed by
            // one fused if/else over the root muxes, and the other covered
            // instructions emit nothing. Runs are broken whenever a cone
            // member would read an earlier run root's result (the fused
            // if/else would otherwise be a use-before-def). instructionMuxRun
            // maps an instruction to its run plan id (-1 when not covered).
            struct MuxRunPlan
            {
                uint32_t head = 0; // first covered instruction value
                VariableId select;
                std::vector<InstructionId> preamble; // cone members, atom order
                std::vector<InstructionId> arms;     // root muxes, atom order
                // Covered atom range (dense AtomId values, inclusive): runs
                // cover whole contiguous atoms, so the trace comments of every
                // covered atom emit at the head position.
                uint32_t firstAtom = 0;
                uint32_t lastAtom = 0;
            };
            std::vector<int32_t> instructionMuxRun;
            std::vector<MuxRunPlan> muxRunPlans;
            uint64_t muxAtomFusedCount = 0;
            // Re-entrancy guard: emitMuxFusionRun emits the preamble through
            // emitInstruction, which must not re-enter the run hook.
            mutable bool muxRunEmissionActive = false;
            // NO0013 F1/F2 windowed emission of lane-build concat cones. A
            // lane-build chain is Concat C_0..C_n where every step C_j
            // (j >= 1) re-splices a few narrow element windows over the
            // previous chain value through identity-placed SliceStatic
            // operands. The stock emission rebuilds all W bits per step
            // (zero_words + full-width insert_words), so a 260-step chain on
            // a 1040-bit value moves ~9 MB of words per block activation.
            // The windowed form emits C_0 once into the final step's slot D
            // and each later step as element-window replaces into D; chain
            // intermediates are never materialized (escape fallback: a full
            // assign_words copy at the step's own position). SliceStatic
            // consumers of an intermediate are re-pointed at D when their
            // emission position precedes the first step that overwrites
            // their window, otherwise the intermediate is materialized.
            // (The F2 standalone-concat variant was reverted after
            // measurement: per-word replace-RMW is pricier than the stock
            // zero fill + OR-insert; see NO0013 §9.)
            struct WindowChainPlan
            {
                struct Step
                {
                    InstructionId instruction;
                    // (operand index, concat bit offset) of the element
                    // (non-backbone) operands spliced by this step.
                    std::vector<std::pair<uint32_t, uint64_t>> elems;
                };
                VariableId finalVar; // D: last chain concat's result
                uint32_t width = 0;  // chain bit width W
                std::vector<Step> steps;
                std::unordered_map<uint32_t, uint32_t> stepIndexByInstr;
            };
            // Per-instruction action (indexed by InstructionId value):
            // -1 none, 0 chain head, 1 chain step, 2 skip (backbone slice),
            // 3 slice re-pointed at the chain final var, 4 windowed concat.
            std::vector<int32_t> instructionWindowPlan;
            std::vector<int8_t> instructionWindowAction;
            // Chain member also materializes its own slot (assign copy from
            // the final var right after its window replaces).
            std::vector<uint8_t> instructionWindowMaterialize;
            std::vector<WindowChainPlan> windowChainPlans;
            uint64_t windowedChainCount = 0;
            uint64_t windowedStepCount = 0;
            uint64_t windowedConcatCount = 0;
            uint64_t windowedSkippedSlices = 0;
            uint64_t windowedRemappedSlices = 0;
            uint64_t windowedMaterialized = 0;
            uint64_t windowedBailedChains = 0;
            // NO0014 dynamic bit-field functional-update cone collapse
            // (dynblend). Cone signature: or(and(base, not(shl(ones, idx))),
            // shl(and(zext(elem), ones), idx)) with an optional
            // mux(cond, merged, base) tail, chaining where a cone's result
            // is the next cone's base (multi-port register-table updates,
            // e.g. intRat/vecRat/vlRat difftest_table). Stock emission
            // materializes every intermediate as full-width helper calls
            // (4-8 cross-TU calls per cone); the collapsed form copies the
            // chain base once into the last cone's slot D and emits one
            // conditional blend_window_dyn_words call per cone, matching
            // gsim's per-port `if (wen) next[addr] = data` scalar stores.
            struct DynBlendPlan
            {
                struct Cone
                {
                    InstructionId tail;   // mux (conditional) or or (blend site)
                    InstructionId internal[7]; // onehot,elemm,placed,notoh,cleared,merged[,zext]
                    uint32_t internalCount = 0;
                    VariableId result;    // cone result var
                    VariableId base;
                    VariableId idx;
                    VariableId ones;
                    VariableId elem;
                    uint32_t elemWidth = 0;
                    VariableId cond;      // invalid when unconditional
                };
                VariableId finalVar;      // accumulator D (last cone result)
                uint32_t width = 0;
                std::vector<Cone> cones;
            };
            // Actions: -1 none, 0 chain head (copy base + blend), 1 blend,
            // 2 skip (cone-internal), 3 slice consumer re-pointed at D.
            std::vector<int32_t> instructionDynBlendPlan;
            std::vector<int8_t> instructionDynBlendAction;
            std::vector<uint8_t> instructionDynBlendMaterialize;
            std::vector<DynBlendPlan> dynBlendPlans;
            uint64_t dynBlendChainCount = 0;
            uint64_t dynBlendConeCount = 0;
            uint64_t dynBlendSkipped = 0;
            uint64_t dynBlendRemapped = 0;
            uint64_t dynBlendMaterialized = 0;
            uint64_t dynBlendBailed = 0;
            // NO0017 §5 wide-state scalar explode (GrhSimAmCppOptions
            // attribute "wideStateExplode", default off). A wide BitVector
            // state whose accesses are all uniformly-sized aligned constant
            // slices (reads) plus aligned constant-mask / full-width state
            // writes leaves the wideValues_ word pool and becomes a
            // per-element scalar array member (wv<K>_, element width K =
            // gcd of every constant slice site's width and lsb): constant
            // SliceStatic reads become element loads, SliceArray reads
            // indexed element loads, constant-mask writes per-element RMWs,
            // and full-width writes per-element loops. Planning is purely an
            // emit-time representation decision (atom edge set untouched);
            // explodedElementWidth[variable] != 0 marks an exploded state
            // and holds K. Guards are deliberately conservative — any
            // access outside the recognized forms keeps the pool path.
            bool wideStateExplode = false;
            std::vector<uint32_t> explodedElementWidth;
            uint64_t explodedStateCount = 0;
            uint64_t explodedElementTotal = 0;
            uint64_t explodedReclaimedWords = 0;
            std::array<uint64_t, kExplodeBailCount> explodeBails{};
            // NO0016: values pinned to the uint64_t storage class by
            // classifyLocalValueStorage (address-taken or non-scalar).
            uint64_t narrowLocalPinned = 0;
            // NO0018 escape hatches (env-set, see classifyLocalValueStorage):
            // keep the pre-NO0018 outlined slice_words / masked-write helper
            // emission instead of the inline forms.
            bool disableWideSliceInline = false;
            bool disableMaskedWriteUnroll = false;
        };

        constexpr uint8_t kEscapeGlobal = 1U << 0U;
        constexpr uint8_t kEscapeCrossBlockUse = 1U << 1U;
        constexpr uint8_t kEscapeEarlyUse = 1U << 2U;
        constexpr uint32_t kInvalidChunkScalar = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t kInvalidLocalityBlock = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t kInvalidChangedResultIndex =
            std::numeric_limits<uint32_t>::max();
        // NO0013 windowed emission actions (EmitState::instructionWindowAction).
        constexpr int8_t kWindowActionChainHead = 0;
        constexpr int8_t kWindowActionChainStep = 1;
        constexpr int8_t kWindowActionSkip = 2;
        constexpr int8_t kWindowActionRemapSlice = 3;
        constexpr int8_t kWindowActionConcat = 4;
        // NO0014 dynblend cone actions (EmitState::instructionDynBlendAction).
        constexpr int8_t kDynBlendHead = 0;
        constexpr int8_t kDynBlendCone = 1;
        constexpr int8_t kDynBlendSkip = 2;
        constexpr int8_t kDynBlendRemapSlice = 3;

        bool isLocalValue(const EmitState &state, VariableId variable)
        {
            return state.activeLocalityBlock != kInvalidLocalityBlock &&
                   state.localValueStamps[variable.value] == state.activeLocalityBlock + 1;
        }

        // True while an oversized Block's chunks are emitted: its locals and
        // watch/detector flags are named as parent-scope array elements
        // (localblk_<id>[k] and friends) instead of plain block-function
        // locals, so the chunk functions can share them by pointer.
        bool chunkedBlockNaming(const EmitState &state)
        {
            return state.activeChunkedBlock != kInvalidLocalityBlock &&
                   state.activeChunkedBlock == state.activeLocalityBlock;
        }

        // NO0016 storage-class name infix: class 3 keeps the legacy
        // local_<k>/localblk_<id>[k] spelling (so the all-class-3 escape
        // hatch reproduces the pre-NO0016 output byte for byte), narrower
        // classes carry the width in the name.
        std::string localClassInfix(uint8_t storageClass)
        {
            switch (storageClass)
            {
                case 0: return "8";
                case 1: return "16";
                case 2: return "32";
                default: return "";
            }
        }

        std::string valueExpr(const EmitState &state, VariableId variable)
        {
            if (isLocalValue(state, variable))
            {
                const std::string infix =
                    localClassInfix(state.localValueClasses[variable.value]);
                if (chunkedBlockNaming(state))
                {
                    // NO0016 Stage B: chunk-internal values name the
                    // chunk-function-local scalar, not a shared array slot.
                    if (state.chunkScalarIndex[variable.value] != kInvalidChunkScalar)
                    {
                        return "local" + infix + "_" +
                               std::to_string(state.chunkScalarIndex[variable.value]);
                    }
                    return "localblk" + infix + "_" +
                           std::to_string(state.activeChunkedBlock) + "[" +
                           std::to_string(state.localValueIndices[variable.value]) + "]";
                }
                return "local" + infix + "_" +
                       std::to_string(state.localValueIndices[variable.value]);
            }
            const uint32_t denseIndex = state.changedResultDenseIndex[variable.value];
            if (denseIndex != kInvalidChangedResultIndex)
            {
                return "changedResults_[" + std::to_string(denseIndex) + "]";
            }
            return "v" + std::to_string(variable.value);
        }

        std::string boolExpr(const EmitState &state, VariableId variable)
        {
            return "(" + valueExpr(state, variable) + " != 0)";
        }

        // Block-local ST00013 scalar write-point flag: a plain local in the
        // inline form, a parent-scope array element in the chunked form.
        std::string scalarWatchFlagExpr(const EmitState &state, uint32_t flag)
        {
            if (chunkedBlockNaming(state))
            {
                return "wrChgblk_" + std::to_string(state.activeChunkedBlock) + "[" +
                       std::to_string(flag) + "]";
            }
            return "wrChg_" + std::to_string(flag);
        }

        std::string commitInputDirtyMarks(const EmitState &state,
                                          InstructionId instruction)
        {
            const auto entry =
                state.commitInputDirtyGatesByInstruction.find(instruction.value);
            if (entry == state.commitInputDirtyGatesByInstruction.end())
            {
                return {};
            }
            std::string code;
            for (const uint32_t gate : entry->second)
            {
                code += "commitInputDirty_[" + std::to_string(gate) + "] = 1; ";
            }
            return code;
        }

        std::string commitInputDirtyBlockMarks(const EmitState &state,
                                               uint32_t block)
        {
            const auto entry = state.commitInputDirtyGatesByBlock.find(block);
            if (entry == state.commitInputDirtyGatesByBlock.end())
            {
                return {};
            }
            std::string code;
            for (const uint32_t gate : entry->second)
            {
                code += "commitInputDirty_[" + std::to_string(gate) + "] = 1;\n";
            }
            return code;
        }

        // Same for the ST00011 array write-point accumulator flags.
        std::string arrayWatchAccumExpr(const EmitState &state, uint32_t accum)
        {
            if (chunkedBlockNaming(state))
            {
                return "arrChgblk_" + std::to_string(state.activeChunkedBlock) + "[" +
                       std::to_string(accum) + "]";
            }
            return "arrChg_" + std::to_string(accum);
        }

        // Same for the ST00010 detector-group accumulator flags. The chunked
        // form declares the zero-initialized array in the parent function, so
        // chunked references never carry the inline first-use declaration.
        std::string detectorGroupExpr(const EmitState &state, uint32_t group)
        {
            if (chunkedBlockNaming(state))
            {
                return "detGrpblk_" + std::to_string(state.activeChunkedBlock) + "[" +
                       std::to_string(group) + "]";
            }
            return "detGrp_" + std::to_string(group);
        }

        void beginLocalityBlock(const EmitState &state, uint32_t block)
        {
            state.activeLocalityBlock = block;
            state.activeLocalityDeclarations.clear();
            state.activeLocalityDeclClasses.clear();
            state.activeLocalityClassCounts.fill(0);
            for (const uint32_t variable : state.blockDefinedVariables[block])
            {
                if (state.variableEscapeFlags[variable] != 0)
                {
                    continue;
                }
                state.localValueStamps[variable] = block + 1;
                // NO0016 Stage B: a chunk-internal scalar is declared inside
                // its home chunk function; it gets no shared array slot.
                if (state.chunkScalarIndex[variable] != kInvalidChunkScalar)
                {
                    continue;
                }
                // NO0016: per-class indices (the class's own dense ordinal),
                // not the flat declaration order.
                const uint8_t storageClass = state.localValueClasses[variable];
                state.localValueIndices[variable] =
                    state.activeLocalityClassCounts[storageClass]++;
                state.activeLocalityDeclarations.push_back(
                    state.localValueIndices[variable]);
                state.activeLocalityDeclClasses.push_back(storageClass);
            }
        }

        void endLocalityBlock(const EmitState &state)
        {
            state.activeLocalityBlock = kInvalidLocalityBlock;
            state.activeLocalityDeclarations.clear();
            state.activeLocalityDeclClasses.clear();
            state.activeLocalityClassCounts.fill(0);
        }

        const char *localClassCppType(uint8_t storageClass)
        {
            switch (storageClass)
            {
                case 0: return "std::uint8_t";
                case 1: return "std::uint16_t";
                case 2: return "std::uint32_t";
                default: return "std::uint64_t";
            }
        }

        std::string localValueDeclarations(const EmitState &state)
        {
            if (state.activeLocalityDeclarations.empty())
            {
                return {};
            }
            // NO0016: one declaration per storage class, names carry the
            // class infix (local8_<k>, ..., class 3 stays local_<k>).
            std::string code;
            for (uint8_t storageClass = 0; storageClass < 4; ++storageClass)
            {
                if (state.activeLocalityClassCounts[storageClass] == 0)
                {
                    continue;
                }
                code += localClassCppType(storageClass);
                code += " ";
                const std::string infix = localClassInfix(storageClass);
                bool first = true;
                for (std::size_t i = 0; i < state.activeLocalityDeclarations.size(); ++i)
                {
                    if (state.activeLocalityDeclClasses[i] != storageClass)
                    {
                        continue;
                    }
                    if (!first)
                    {
                        code += ", ";
                    }
                    first = false;
                    code += "local" + infix + "_" +
                            std::to_string(state.activeLocalityDeclarations[i]);
                }
                code += ";\n";
            }
            return code;
        }

        const Type &variableType(const EmitState &state, VariableId variable)
        {
            return state.variableTypes.at(variable.value);
        }

        const EmitState::Storage &variableStorage(const EmitState &state, VariableId variable)
        {
            return state.variableStorage.at(variable.value);
        }

        std::string changedResultAssignPrefix(const EmitState &state, VariableId variable)
        {
            // A result read by another block is round-local state and goes
            // through the dirty list; a same-block result is rewritten before
            // every same-block read and is a plain assignment. The dirty list
            // is indexed by the dense changed-result id, not the VariableId.
            if (state.crossBlockChangedResults[variable.value])
            {
                return "set_changed_result(" +
                       std::to_string(state.changedResultDenseIndex[variable.value]) +
                       ", ";
            }
            return valueExpr(state, variable) + " = (";
        }

        bool isDetectorChangedOpcode(Opcode opcode)
        {
            return opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                   opcode == Opcode::ChangedNeg;
        }

        // Boolean change expression for a narrow scalar (BitVector <= 64 bits)
        // changed.* instruction; kept in lockstep with the scalar ChangedAny/
        // ChangedPos/ChangedNeg case in emitInstruction.
        std::string narrowChangedEventExpr(const EmitState &state, Opcode opcode,
                                           VariableId watched, VariableId old)
        {
            if (opcode == Opcode::ChangedAny)
            {
                return valueExpr(state, watched) + " != " + valueExpr(state, old);
            }
            if (opcode == Opcode::ChangedPos)
            {
                return "(" + valueExpr(state, old) + " == 0 && " +
                       valueExpr(state, watched) + " != 0)";
            }
            return "(" + valueExpr(state, old) + " != 0 && " +
                   valueExpr(state, watched) + " == 0)";
        }

        // Splits an activation target list into the scan-local relay mask
        // (same-byte forward targets owned by the current chunk) and the global
        // per-word masks. Returns false on an out-of-range target.
        bool splitActivationTargets(const EmitState &state, bool forward,
                                    std::span<const BlockId> targets, uint8_t &relayMask,
                                    std::map<uint32_t, uint64_t> &masks, std::string &error)
        {
            relayMask = 0;
            masks.clear();
            for (const BlockId target : targets)
            {
                if (target.value >= state.blockCount)
                {
                    error = "AM activation target BlockId out of range";
                    return false;
                }
                const uint32_t bit = target.value % 8U;
                if (forward && state.scanRelayByte >= 0 &&
                    target.value / 8U == static_cast<uint32_t>(state.scanRelayByte) &&
                    ((state.scanRelayMask >> bit) & 1U) != 0)
                {
                    relayMask = static_cast<uint8_t>(relayMask | (1U << bit));
                    continue;
                }
                masks[target.value / 64U] |= UINT64_C(1) << (target.value % 64U);
            }
            return true;
        }

        // Emits one conditional activation merge: the shared body of an
        // act.f/act.b instruction and of a folded detector-group merge
        // (ST00010). A pure same-byte relay can drop the branch entirely
        // (allowBranchlessRelay): OR-ing the sign-extended flag keeps the merge
        // in registers, the legacy deferred-group idiom. Global mask writes
        // stay conditional so a quiet group does not dirty the shared activity
        // words.
        std::string emitActivationMerge(const EmitState &state, bool forward,
                                        uint8_t relayMask,
                                        const std::map<uint32_t, uint64_t> &masks,
                                        uint64_t originalTargetCount,
                                        const std::string &condition,
                                        bool allowBranchlessRelay)
        {
            if (allowBranchlessRelay && forward && relayMask != 0 && masks.empty() &&
                !state.runtimeProfile)
            {
                return "byteFlags |= (std::uint8_t)((0U - (std::uint8_t)(" + condition +
                       ")) & " + byteMaskLiteral(relayMask) + ");\n";
            }
            std::string code = "if (" + condition + ") {\n";
            if (relayMask != 0)
            {
                code += "byteFlags |= " + byteMaskLiteral(relayMask) + ";\n";
            }
            if (state.runtimeProfile && originalTargetCount != 0)
            {
                code += "if (runtimeProfileEnabled_) ";
                code += forward ? "profileActivateForward_ += "
                                : "profileActivateBackward_ += ";
                code += std::to_string(originalTargetCount) + ";\n";
            }
            for (const auto &[word, mask] : masks)
            {
                code += "activeWords_[" + std::to_string(word) +
                        "] |= " + wordMaskLiteral(mask) + ";\n";
            }
            if (!forward)
            {
                code += "backwardFired_ = true;\n";
            }
            code += "}\n";
            return code;
        }

        bool isWideBitVector(const EmitState &state, VariableId variable)
        {
            const Type &type = variableType(state, variable);
            return type.kind == TypeKind::BitVector && type.bitWidth > 64;
        }

        std::string wideDataExpr(const EmitState &state, VariableId variable)
        {
            return "wideValues_.data() + " +
                   std::to_string(variableStorage(state, variable).offset);
        }

        std::string resizedExpr(const EmitState &state,
                                VariableId variable,
                                uint32_t width,
                                Signedness signedness)
        {
            const Type &source = variableType(state, variable);
            return "resize_value(" + valueExpr(state, variable) + ", " +
                   std::to_string(source.bitWidth) + ", " +
                   (signedness == Signedness::Signed ? "true" : "false") + ", " +
                   std::to_string(width) + ")";
        }

        std::string wordDataExpr(const EmitState &state, VariableId variable)
        {
            const Type &type = variableType(state, variable);
            return (isWideBitVector(state, variable) || type.kind == TypeKind::Array)
                       ? wideDataExpr(state, variable)
                       : "&" + valueExpr(state, variable);
        }

        // NO0018: assign a <=64-bit slice of a wide word-array value as an
        // inline expression. extract_word is header-constexpr, so a constant
        // start folds to 1-2 loads and the result needs no addressable slot
        // (unpinning in classifyLocalValueStorage mirrors this).
        std::string wideSliceAssign(const EmitState &state,
                                    VariableId result,
                                    const std::string &sourceWords,
                                    uint32_t sourceWidth,
                                    const std::string &startExpr)
        {
            const uint32_t width = variableType(state, result).bitWidth;
            return valueExpr(state, result) + " = (extract_word(" + sourceWords + ", " +
                   std::to_string(sourceWidth) + ", " + startExpr + ")) & " +
                   maskExpr(width) + ";\n";
        }

        // NO0017 §5 exploded-state accessors. An exploded wide state leaves
        // the wideValues_ word pool and becomes a per-element scalar array
        // member wv<VariableId>_; K is the uniform element width in bits.
        std::optional<std::vector<uint64_t>> constantWordsVector(const EmitState &state,
                                                                 VariableId variable,
                                                                 std::size_t words);

        bool isExplodedState(const EmitState &state, VariableId variable)
        {
            return variable.value < state.explodedElementWidth.size() &&
                   state.explodedElementWidth[variable.value] != 0;
        }

        const char *explodedElemCppType(uint32_t elemWidth)
        {
            if (elemWidth <= 8)
            {
                return "std::uint8_t";
            }
            if (elemWidth <= 16)
            {
                return "std::uint16_t";
            }
            if (elemWidth <= 32)
            {
                return "std::uint32_t";
            }
            return "std::uint64_t";
        }

        std::string explodedMemberName(VariableId variable)
        {
            return "wv" + std::to_string(variable.value) + "_";
        }

        // Constant aligned slice read of an exploded state: the site spans
        // whole elements (planner invariant: K divides both lsb and the
        // result width), so it assembles the covered elements directly.
        std::string explodedSliceAssign(const EmitState &state,
                                        VariableId result,
                                        VariableId source,
                                        uint64_t lsb)
        {
            const uint32_t elemWidth = state.explodedElementWidth[source.value];
            const uint32_t width = variableType(state, result).bitWidth;
            const uint64_t first = lsb / elemWidth;
            const uint32_t elems = width / elemWidth;
            const std::string member = explodedMemberName(source);
            std::string value;
            for (uint32_t elem = 0; elem < elems; ++elem)
            {
                std::string term = member + "[" + std::to_string(first + elem) + "]";
                if (elem != 0)
                {
                    term = "(std::uint64_t(" + term + ") << " +
                           std::to_string(elem * elemWidth) + ")";
                }
                value = value.empty() ? term : value + " | " + term;
            }
            return valueExpr(state, result) + " = (" + value + ") & " +
                   maskExpr(width) + ";\n";
        }

        // The element value of an exploded state's write data at one
        // element index: a direct element load when the data is itself
        // exploded (same K, planner-checked), else an inline word extract.
        std::string explodedWriteDataElem(const EmitState &state,
                                          VariableId data,
                                          uint32_t dataWidth,
                                          uint64_t elem,
                                          uint32_t elemWidth)
        {
            if (isExplodedState(state, data))
            {
                return explodedMemberName(data) + "[" + std::to_string(elem) + "]";
            }
            return "(extract_word(" + wordDataExpr(state, data) + ", " +
                   std::to_string(dataWidth) + ", UINT64_C(" +
                   std::to_string(elem * elemWidth) + ")))";
        }

        // Masked state write on an exploded target: one conditional RMW per
        // mask-touched element (the constant mask's set bits are grouped per
        // element), replacing the full-width masked_write_words word loop.
        std::optional<std::string> emitExplodedMaskedWrite(const EmitState &state,
                                                           VariableId target,
                                                           VariableId data,
                                                           VariableId mask,
                                                           const std::string &flagExpr,
                                                           std::string &error)
        {
            const uint32_t elemWidth = state.explodedElementWidth[target.value];
            const uint32_t width = variableType(state, target).bitWidth;
            const std::size_t words = (static_cast<std::size_t>(width) + 63U) / 64U;
            std::optional<std::vector<uint64_t>> maskWords =
                constantWordsVector(state, mask, words);
            if (!maskWords)
            {
                error = "exploded state write lost its constant mask";
                return std::nullopt;
            }
            std::map<uint64_t, uint64_t> elementMasks;
            for (std::size_t word = 0; word < words; ++word)
            {
                uint64_t value = (*maskWords)[word];
                if (word + 1U == words && width % 64U != 0)
                {
                    value &= (UINT64_C(1) << (width % 64U)) - UINT64_C(1);
                }
                while (value != 0)
                {
                    const uint32_t bit = static_cast<uint32_t>(std::countr_zero(value));
                    value &= value - UINT64_C(1);
                    const uint64_t global = word * 64U + bit;
                    elementMasks[global / elemWidth] |= UINT64_C(1) << (global % elemWidth);
                }
            }
            if (elementMasks.size() > kExplodeMaskElementLimit)
            {
                error = "exploded state write mask spans more elements than planned";
                return std::nullopt;
            }
            const std::string member = explodedMemberName(target);
            std::string code;
            for (const auto &[elem, elemMask] : elementMasks)
            {
                const std::string targetElem = member + "[" + std::to_string(elem) + "]";
                const std::string dataElem =
                    explodedWriteDataElem(state, data, width, elem, elemWidth);
                const std::string maskLit = wordMaskLiteral(elemMask);
                const std::string next =
                    "((" + targetElem + " & ~" + maskLit + ") | (" + dataElem + " & " +
                    maskLit + "))";
                if (flagExpr.empty())
                {
                    code += targetElem + " = " + next + ";\n";
                }
                else
                {
                    code += "{ const " + std::string(explodedElemCppType(elemWidth)) +
                            " wnext = " + next + "; " + flagExpr + " |= (wnext != " +
                            targetElem + "); " + targetElem + " = wnext; }\n";
                }
            }
            return code;
        }

        // Full-width (unmasked) state write on an exploded target: a
        // per-element store loop with optional change accumulation,
        // replacing the assign_words(/_detect) whole-width word loop.
        std::string emitExplodedFullWrite(const EmitState &state,
                                          InstructionId instruction,
                                          VariableId target,
                                          VariableId data,
                                          const std::string &flagExpr)
        {
            const uint32_t elemWidth = state.explodedElementWidth[target.value];
            const uint32_t width = variableType(state, target).bitWidth;
            const uint64_t elemCount = width / elemWidth;
            const std::string member = explodedMemberName(target);
            const std::string elemType = explodedElemCppType(elemWidth);
            const std::string suffix = std::to_string(instruction.value);
            const std::string changed = "wvChg_" + suffix;
            const std::string index = "wvIdx_" + suffix;
            const bool detect = !flagExpr.empty();
            std::string code = "{ ";
            if (detect)
            {
                code += "bool " + changed + " = false; ";
            }
            const auto store = [&](const std::string &indexExpr) {
                return (detect ? changed + " |= (wnext != " + member + "[" + indexExpr + "]); "
                               : "") +
                       member + "[" + indexExpr + "] = wnext; ";
            };
            if (isExplodedState(state, data))
            {
                const std::string dataMember = explodedMemberName(data);
                code += "for (std::size_t " + index + " = 0; " + index + " < " +
                        std::to_string(elemCount) + "; ++" + index + ") { const " + elemType +
                        " wnext = " + dataMember + "[" + index + "]; " +
                        store(index) + "} ";
            }
            else if (64U % elemWidth == 0)
            {
                // Word-parallel unpack: every data word carries 64/K
                // elements with constant shifts after the inner unroll.
                const uint32_t elemsPerWord = 64U / elemWidth;
                const std::size_t words = (static_cast<std::size_t>(width) + 63U) / 64U;
                const std::string epw = std::to_string(elemsPerWord);
                const std::string elemIndex = index + " * " + epw + " + wj";
                code += "for (std::size_t " + index + " = 0; " + index + " < " +
                        std::to_string(words) + "; ++" + index +
                        ") { const std::uint64_t wdata = (" + wordDataExpr(state, data) +
                        ")[" + index + "]; for (std::size_t wj = 0; wj < " + epw + " && " +
                        elemIndex + " < " + std::to_string(elemCount) +
                        "; ++wj) { const " + elemType + " wnext = static_cast<" + elemType +
                        ">((wdata >> (wj * " + std::to_string(elemWidth) + ")) & " +
                        maskExpr(elemWidth) + "); " + store(elemIndex) + "} } ";
            }
            else
            {
                code += "for (std::size_t " + index + " = 0; " + index + " < " +
                        std::to_string(elemCount) + "; ++" + index + ") { const " + elemType +
                        " wnext = static_cast<" + elemType + ">(extract_word(" +
                        wordDataExpr(state, data) + ", " + std::to_string(width) + ", " +
                        index + " * " + std::to_string(elemWidth) + ") & " +
                        maskExpr(elemWidth) + "); " + store(index) + "} ";
            }
            if (detect)
            {
                code += flagExpr + " |= " + changed + "; ";
            }
            code += "}\n";
            return code;
        }

        // Exploded-state write dispatch (NO0017 §5): constant-mask writes
        // become per-element RMWs, full-width writes per-element loops; a
        // present cond gates the whole statement and the ST00013 raise flag
        // (when fused) accumulates the per-element change exactly like the
        // word forms did.
        std::optional<std::string> emitExplodedStateWrite(const EmitState &state,
                                                          InstructionId instruction,
                                                          const StateWriteLayout &layout,
                                                          std::string &error)
        {
            const Opcode opcode = state.program.opcode(instruction);
            const auto operands = state.program.operands(instruction);
            const VariableId target = operands[layout.targetIndex];
            const VariableId data = operands[layout.dataIndex];
            std::string flag;
            if (isRegisterWriteOpcode(opcode) && state.scalarWriteRaise >= 0)
            {
                flag = scalarWatchFlagExpr(state,
                                           static_cast<uint32_t>(state.scalarWriteRaise));
            }
            std::string body;
            if (layout.hasMask)
            {
                const VariableId mask = operands[layout.hasCond ? 1 : 0];
                std::optional<std::string> masked =
                    emitExplodedMaskedWrite(state, target, data, mask, flag, error);
                if (!masked)
                {
                    return std::nullopt;
                }
                body = std::move(*masked);
            }
            else
            {
                body = emitExplodedFullWrite(state, instruction, target, data, flag);
            }
            if (layout.hasCond)
            {
                return "if (" + boolExpr(state, operands[0]) + ") { " + body + "}\n";
            }
            return body;
        }

        // The word value of a constant-initialized narrow bit-vector
        // variable, when it is one: lets a state-write mask fold to an
        // immediate instead of reading a state slot.
        std::optional<uint64_t> scalarConstantWord(const EmitState &state, VariableId variable)
        {
            const VariableRecord &record = state.program.variable(variable);
            if (!record.init.valid() || record.init.value >= state.program.initCount())
            {
                return std::nullopt;
            }
            const InitDescriptor &init = state.program.init(record.init);
            if (init.kind != InitKind::Constant)
            {
                return std::nullopt;
            }
            const LiteralView literal = state.program.literal(LiteralId{init.payload});
            const Type &literalType = state.program.type(literal.type);
            if (literalType.kind != TypeKind::BitVector || literalType.bitWidth > 64)
            {
                return std::nullopt;
            }
            return literal.words.empty() ? uint64_t{0} : literal.words.front();
        }

        // Stack-local word-array declaration rendering a constant bit-vector
        // variable ("const std::uint64_t <name>[N] = {...};"), so word-level
        // helpers can take a constant mask without a state-slot read.
        std::optional<std::string> constantWordsDecl(const EmitState &state, VariableId variable,
                                                     const std::string &name)
        {
            const VariableRecord &record = state.program.variable(variable);
            if (!record.init.valid() || record.init.value >= state.program.initCount())
            {
                return std::nullopt;
            }
            const InitDescriptor &init = state.program.init(record.init);
            if (init.kind != InitKind::Constant)
            {
                return std::nullopt;
            }
            const LiteralView literal = state.program.literal(LiteralId{init.payload});
            const Type &literalType = state.program.type(literal.type);
            if (literalType.kind != TypeKind::BitVector)
            {
                return std::nullopt;
            }
            const std::size_t words =
                (static_cast<std::size_t>(literalType.bitWidth) + 63U) / 64U;
            std::string decl = "const std::uint64_t " + name + "[" + std::to_string(words) +
                               "] = {";
            for (std::size_t word = 0; word < words; ++word)
            {
                if (word != 0)
                {
                    decl += ", ";
                }
                decl += wordMaskLiteral(word < literal.words.size() ? literal.words[word] : 0);
            }
            decl += "};";
            return decl;
        }

        uint64_t storedWordCount(const EmitState &state, VariableId variable)
        {
            const Type &type = variableType(state, variable);
            const uint64_t elements = type.kind == TypeKind::Array ? type.elementCount : 1U;
            return static_cast<uint64_t>(variableStorage(state, variable).wordCount) * elements;
        }

        std::string assignVariableStatement(const EmitState &state,
                                            VariableId target,
                                            VariableId source,
                                            Signedness extension)
        {
            const Type &targetType = variableType(state, target);
            const Type &sourceType = variableType(state, source);
            const std::string sign = extension == Signedness::Signed ? "true" : "false";
            if (targetType.bitWidth <= 64)
            {
                if (sourceType.bitWidth <= 64)
                {
                    return valueExpr(state, target) + " = resize_value(" + valueExpr(state, source) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " + sign + ", " +
                           std::to_string(targetType.bitWidth) + ");\n";
                }
                return valueExpr(state, target) + " = (" + wordDataExpr(state, source) + ")[0] & " +
                       maskExpr(targetType.bitWidth) + ";\n";
            }
            if (sourceType.bitWidth <= 64)
            {
                return "assign_words_from_scalar(" + wideDataExpr(state, target) + ", " +
                       std::to_string(targetType.bitWidth) + ", " + valueExpr(state, source) + ", " +
                       std::to_string(sourceType.bitWidth) + ", " + sign + ");\n";
            }
            return "assign_words(" + wideDataExpr(state, target) + ", " +
                   std::to_string(targetType.bitWidth) + ", " + wideDataExpr(state, source) +
                   ", " + std::to_string(sourceType.bitWidth) + ", " + sign + ");\n";
        }

        // NO0018 W3: the constant word values of a constant-initialized
        // bit-vector variable, zero-padded to `words`.
        std::optional<std::vector<uint64_t>> constantWordsVector(const EmitState &state,
                                                                 VariableId variable,
                                                                 std::size_t words)
        {
            const VariableRecord &record = state.program.variable(variable);
            if (!record.init.valid() || record.init.value >= state.program.initCount())
            {
                return std::nullopt;
            }
            const InitDescriptor &init = state.program.init(record.init);
            if (init.kind != InitKind::Constant)
            {
                return std::nullopt;
            }
            const LiteralView literal = state.program.literal(LiteralId{init.payload});
            const Type &literalType = state.program.type(literal.type);
            if (literalType.kind != TypeKind::BitVector)
            {
                return std::nullopt;
            }
            std::vector<uint64_t> out(words, 0);
            for (std::size_t word = 0; word < words && word < literal.words.size(); ++word)
            {
                out[word] = literal.words[word];
            }
            return out;
        }

        // NO0018 W3: masked wide write emission. A constant mask touching few
        // words unrolls to per-word RMW inline (the masked_write_words helper
        // loops over every word of the target width; e.g. 32 words for a
        // 2-bit counter update on a 2048-bit state). `flagExpr` empty selects
        // the plain (non-detect) form; otherwise the per-word change is
        // accumulated into it. Falls back to the helper call otherwise.
        std::string emitMaskedWriteWords(const EmitState &state,
                                         const std::string &targetWords,
                                         const std::string &dataWords,
                                         VariableId mask,
                                         uint32_t width,
                                         const std::string &flagExpr,
                                         const std::string &constName)
        {
            const std::size_t words = (static_cast<std::size_t>(width) + 63U) / 64U;
            if (!state.disableMaskedWriteUnroll)
            {
                if (std::optional<std::vector<uint64_t>> maskWords =
                        constantWordsVector(state, mask, words))
                {
                    std::vector<std::pair<std::size_t, uint64_t>> touched;
                    for (std::size_t word = 0; word < words; ++word)
                    {
                        uint64_t value = (*maskWords)[word];
                        const uint32_t bits =
                            word + 1U == words ? width - static_cast<uint32_t>(word * 64U) : 64U;
                        if (bits < 64U)
                        {
                            value &= (UINT64_C(1) << bits) - UINT64_C(1);
                        }
                        if (value != 0)
                        {
                            touched.emplace_back(word, value);
                        }
                    }
                    if (touched.size() <= 16)
                    {
                        std::string code;
                        for (const auto &[word, value] : touched)
                        {
                            const std::string target =
                                "(" + targetWords + ")[" + std::to_string(word) + "]";
                            const std::string maskLit = wordMaskLiteral(value);
                            const std::string next =
                                "((" + target + " & ~" + maskLit + ") | ((" + dataWords + ")[" +
                                std::to_string(word) + "] & " + maskLit + "))";
                            if (flagExpr.empty())
                            {
                                code += target + " = " + next + ";\n";
                            }
                            else
                            {
                                code += "{ const std::uint64_t wnext = " + next + "; " + flagExpr +
                                        " |= (wnext != " + target + "); " + target +
                                        " = wnext; }\n";
                            }
                        }
                        return code;
                    }
                }
            }
            std::string maskWords;
            std::string prefix;
            if (std::optional<std::string> decl = constantWordsDecl(state, mask, constName))
            {
                prefix = *decl + "\n";
                maskWords = constName;
            }
            else
            {
                maskWords = wordDataExpr(state, mask);
            }
            const std::string writeArgs = targetWords + ", " + dataWords + ", " + maskWords +
                                          ", " + std::to_string(width) + ")";
            if (flagExpr.empty())
            {
                return prefix + "masked_write_words(" + writeArgs + ";\n";
            }
            return prefix + flagExpr + " |= masked_write_words_detect(" + writeArgs + ";\n";
        }

        std::string dpiIntegralCppType(const Type &type)
        {
            if (type.bitWidth == 1)
            {
                return "std::uint8_t";
            }
            const bool isSigned = type.signedness == Signedness::Signed;
            if (type.bitWidth <= 8)
            {
                return isSigned ? "std::int8_t" : "std::uint8_t";
            }
            if (type.bitWidth <= 16)
            {
                return isSigned ? "std::int16_t" : "std::uint16_t";
            }
            if (type.bitWidth <= 32)
            {
                return isSigned ? "std::int32_t" : "std::uint32_t";
            }
            return isSigned ? "std::int64_t" : "std::uint64_t";
        }

        std::optional<std::string> dpiCppType(const Type &type,
                                              DpiAbiKind abi,
                                              std::string &error)
        {
            switch (abi)
            {
                case DpiAbiKind::Integral:
                    if (type.kind != TypeKind::BitVector || type.bitWidth == 0 ||
                        type.bitWidth > 64)
                    {
                        error = "AM C++ emitter supports DPI integral values only up to 64 bits";
                        return std::nullopt;
                    }
                    return dpiIntegralCppType(type);
                case DpiAbiKind::Real64:
                    if (type.kind != TypeKind::Real)
                    {
                        error = "DPI real64 ABI requires an AM Real type";
                        return std::nullopt;
                    }
                    return "double";
                case DpiAbiKind::Real32:
                    if (type.kind != TypeKind::Real)
                    {
                        error = "DPI real32 ABI requires an AM Real type";
                        return std::nullopt;
                    }
                    return "float";
                case DpiAbiKind::String:
                    if (type.kind != TypeKind::String)
                    {
                        error = "DPI string ABI requires an AM String type";
                        return std::nullopt;
                    }
                    return "const char *";
            }
            error = "unknown DPI ABI kind";
            return std::nullopt;
        }

        std::string taskArgumentExpr(const EmitState &state, VariableId variable)
        {
            const Type &type = variableType(state, variable);
            const EmitState::Storage &storage = variableStorage(state, variable);
            if (type.kind == TypeKind::BitVector && type.bitWidth <= 64)
            {
                return "TaskArgument::logic_scalar(" + valueExpr(state, variable) + ", " +
                       std::to_string(type.bitWidth) + ", " +
                       (type.signedness == Signedness::Signed ? "true" : "false") + ")";
            }
            if (type.kind == TypeKind::BitVector)
            {
                return "TaskArgument::logic_wide(" + wideDataExpr(state, variable) + ", " +
                       std::to_string(type.bitWidth) + ", " +
                       (type.signedness == Signedness::Signed ? "true" : "false") + ")";
            }
            if (type.kind == TypeKind::Real)
            {
                return "TaskArgument::real(std::bit_cast<double>(realValues_[" +
                       std::to_string(storage.offset) + "]))";
            }
            if (type.kind == TypeKind::String)
            {
                return "TaskArgument::string(stringValues_[" +
                       std::to_string(storage.offset) + "])";
            }
            return {};
        }

        std::string dpiReadExpr(const EmitState &state,
                                VariableId variable,
                                DpiAbiKind abi,
                                std::string_view cppType)
        {
            const Type &type = variableType(state, variable);
            const EmitState::Storage &storage = variableStorage(state, variable);
            switch (abi)
            {
                case DpiAbiKind::Integral:
                    return "static_cast<" + std::string(cppType) + ">(" +
                           valueExpr(state, variable) + " & " + maskExpr(type.bitWidth) + ")";
                case DpiAbiKind::Real64:
                    return "std::bit_cast<double>(realValues_[" +
                           std::to_string(storage.offset) + "])";
                case DpiAbiKind::Real32:
                    return "static_cast<float>(std::bit_cast<double>(realValues_[" +
                           std::to_string(storage.offset) + "]))";
                case DpiAbiKind::String:
                    return "stringValues_[" + std::to_string(storage.offset) + "]";
            }
            return {};
        }

        std::string dpiCommitStatement(const EmitState &state,
                                       VariableId target,
                                       DpiAbiKind abi,
                                       std::string_view temporary)
        {
            const Type &type = variableType(state, target);
            const EmitState::Storage &storage = variableStorage(state, target);
            switch (abi)
            {
                case DpiAbiKind::Integral:
                    return valueExpr(state, target) + " = static_cast<std::uint64_t>(" +
                           std::string(temporary) + ") & " + maskExpr(type.bitWidth) + ";\n";
                case DpiAbiKind::Real64:
                    return "realValues_[" + std::to_string(storage.offset) +
                           "] = std::bit_cast<std::uint64_t>(static_cast<double>(" +
                           std::string(temporary) + "));\n";
                case DpiAbiKind::Real32:
                    return "realValues_[" + std::to_string(storage.offset) +
                           "] = std::bit_cast<std::uint64_t>(static_cast<double>(static_cast<float>(" +
                           std::string(temporary) + ")));\n";
                case DpiAbiKind::String:
                    return "stringValues_[" + std::to_string(storage.offset) + "] = " +
                           std::string(temporary) + " == nullptr ? std::string{} : std::string(" +
                           std::string(temporary) + ");\n";
            }
            return {};
        }

        std::string eventFireExpr(const EmitState &state,
                                  std::span<const VariableId> operands,
                                  uint32_t eventCount,
                                  bool finalPhase)
        {
            std::string expression = boolExpr(state, operands.front());
            if (finalPhase || eventCount == 0)
            {
                return expression;
            }
            expression += " && (";
            const std::size_t eventBegin = operands.size() - eventCount;
            for (std::size_t index = eventBegin; index < operands.size(); ++index)
            {
                if (index != eventBegin)
                {
                    expression += " || ";
                }
                expression += boolExpr(state, operands[index]);
            }
            expression += ")";
            return expression;
        }

        std::string eventHitExpr(const EmitState &state,
                                 std::span<const VariableId> operands,
                                 uint32_t eventCount)
        {
            if (eventCount == 0)
            {
                return "true";
            }
            std::string expression = "(";
            const std::size_t eventBegin = operands.size() - eventCount;
            for (std::size_t index = eventBegin; index < operands.size(); ++index)
            {
                if (index != eventBegin)
                {
                    expression += " || ";
                }
                expression += boolExpr(state, operands[index]);
            }
            expression += ")";
            return expression;
        }

        std::optional<std::string> emitSystemTaskInstruction(const EmitState &state,
                                                             InstructionId instruction,
                                                             bool finalPhase,
                                                             std::string &error)
        {
            const auto attributes = state.program.systemTaskAttributes(instruction);
            if (!attributes)
            {
                error = "system.task is missing required attributes";
                return std::nullopt;
            }
            if ((attributes->schedule == CallSchedule::Final) != finalPhase)
            {
                return std::string{};
            }

            const auto operands = state.program.operands(instruction);
            if (operands.empty() || attributes->eventCount > operands.size() - 1U)
            {
                error = "system.task has an invalid operand/event layout";
                return std::nullopt;
            }
            const std::size_t argumentEnd = operands.size() - attributes->eventCount;
            const std::size_t argumentCount = argumentEnd - 1U;
            const std::string name(state.program.string(attributes->name));
            if (name != "fwrite" && name != "finish" && name != "fatal")
            {
                error = "unsupported system.task binding in the AM C++ emitter: " + name;
                return std::nullopt;
            }

            std::string preamble;
            std::optional<uint32_t> pendingEventSlot;
            std::string fire;
            if (attributes->eventCount != 0 &&
                attributes->eventMode == HostEventMode::Pending && !finalPhase)
            {
                const auto slot = state.pendingEventSlotByInstruction.find(instruction.value);
                if (slot == state.pendingEventSlotByInstruction.end())
                {
                    error = "eventful system.task is missing a pending-event slot";
                    return std::nullopt;
                }
                pendingEventSlot = slot->second;
                const std::string pending =
                    "pendingHostEvents_[" + std::to_string(*pendingEventSlot) + "]";
                preamble = "if (" + eventHitExpr(state, operands, attributes->eventCount) +
                           ") " + pending + " = true;\n";
                fire = boolExpr(state, operands.front()) + " && " + pending;
            }
            else
            {
                fire = eventFireExpr(state, operands, attributes->eventCount, finalPhase);
            }
            if (attributes->schedule == CallSchedule::Once)
            {
                const auto slot = state.onceSlotByInstruction.find(instruction.value);
                if (slot == state.onceSlotByInstruction.end())
                {
                    error = "system.task once schedule is missing a completed slot";
                    return std::nullopt;
                }
                fire = "!onceCompleted_[" + std::to_string(slot->second) + "] && (" + fire + ")";
            }

            std::string code = preamble + "if (" + fire + ") {\n";
            if (name == "fatal")
            {
                // The first eval is a baseline-sync pass: compute blocks read
                // registers before the commit scan applies their reset values,
                // so assertion conditions can be transiently violated. Skip
                // fatal firing during it (checks resume from eval 2 onward).
                code = preamble + "if (!firstEval_ && (" + fire + ")) {\n";
            }
            if (name == "fwrite")
            {
                if (argumentCount < 2)
                {
                    error = "fwrite system.task requires a handle and format argument";
                    return std::nullopt;
                }
                const VariableId handle = operands[1];
                const VariableId format = operands[2];
                const Type &handleType = variableType(state, handle);
                const Type &formatType = variableType(state, format);
                if (handleType.kind != TypeKind::BitVector || handleType.bitWidth > 64 ||
                    formatType.kind != TypeKind::String)
                {
                    error = "fwrite system.task requires a scalar logic handle and String format";
                    return std::nullopt;
                }
                const std::string suffix = std::to_string(instruction.value);
                const std::string handleName = "task_handle_" + suffix;
                const std::string formatterName = "task_formatter_" + suffix;
                code += "const std::uint64_t " + handleName + " = " + valueExpr(state, handle) +
                        " & " + maskExpr(handleType.bitWidth) + ";\n";
                code += "TaskFormatter " + formatterName + "(stringValues_[" +
                        std::to_string(variableStorage(state, format).offset) + "]);\n";
                for (std::size_t index = 3; index < argumentEnd; ++index)
                {
                    const std::string argument = taskArgumentExpr(state, operands[index]);
                    if (argument.empty())
                    {
                        error = "fwrite system.task encountered an unsupported argument type";
                        return std::nullopt;
                    }
                    code += formatterName + ".append(" + argument + ");\n";
                }
                code += "std::ostream &task_output_" + suffix + " = (" + handleName +
                        " == UINT64_C(2) || " + handleName +
                        " == UINT64_C(0x80000002)) ? std::cerr : std::cout;\n";
                code += "task_output_" + suffix + " << " + formatterName + ".finish();\n";
            }
            else
            {
                // "fatal" mirrors the legacy GrhSIM emitter semantics: an
                // optional leading scalar logic argument is the exit code
                // (default 1), remaining String arguments are printed to
                // std::cerr with a "[fatal] " prefix; both fatal and finish
                // request termination via the host-facing flags.
                const bool fatalTask = name == "fatal";
                if (fatalTask)
                {
                    code += "fatalRequested_ = true;\n";
                }
                code += "finishRequested_ = true;\n";
                std::size_t messageBegin = 1U;
                if (argumentCount != 0)
                {
                    const Type &exitType = variableType(state, operands[1]);
                    if (exitType.kind == TypeKind::BitVector && exitType.bitWidth <= 64)
                    {
                        code += "systemExitCode_ = static_cast<int>(" + valueExpr(state, operands[1]) +
                                " & " + maskExpr(exitType.bitWidth) + ");\n";
                        messageBegin = 2U;
                    }
                    else if (!fatalTask)
                    {
                        error = "finish system.task exit code must be scalar logic";
                        return std::nullopt;
                    }
                    else
                    {
                        code += "systemExitCode_ = 1;\n";
                    }
                }
                else
                {
                    code += std::string("systemExitCode_ = ") + (fatalTask ? "1" : "0") + ";\n";
                }
                if (fatalTask)
                {
                    for (std::size_t index = messageBegin; index < argumentEnd; ++index)
                    {
                        const Type &messageType = variableType(state, operands[index]);
                        if (messageType.kind != TypeKind::String)
                        {
                            continue;
                        }
                        code += "std::cerr << \"[fatal] \" << stringValues_[" +
                                std::to_string(variableStorage(state, operands[index]).offset) +
                                "] << \"\\n\";\n";
                    }
                }
            }
            if (attributes->schedule == CallSchedule::Once)
            {
                const auto slot = state.onceSlotByInstruction.find(instruction.value);
                if (slot == state.onceSlotByInstruction.end())
                {
                    error = "system.task once schedule is missing a completed slot";
                    return std::nullopt;
                }
                code += "onceCompleted_[" + std::to_string(slot->second) + "] = true;\n";
            }
            if (pendingEventSlot)
            {
                code += "pendingHostEvents_[" + std::to_string(*pendingEventSlot) +
                        "] = false;\n";
            }
            code += "}\n";
            return code;
        }

        std::optional<std::string> emitDpiCallInstruction(const EmitState &state,
                                                          InstructionId instruction,
                                                          std::string &error)
        {
            const auto attributes = state.program.dpiCallAttributes(instruction);
            if (!attributes)
            {
                error = "dpi.call is missing required attributes";
                return std::nullopt;
            }
            const auto importIt = state.dpiImportBySymbol.find(attributes->importSymbol.value);
            if (importIt == state.dpiImportBySymbol.end())
            {
                error = "dpi.call references an unknown import symbol";
                return std::nullopt;
            }
            const DpiImportView import = state.program.dpiImport(importIt->second);
            const std::string symbol(state.program.string(import.symbol));
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);

            std::size_t inputCount = 0;
            std::size_t outputCount = 0;
            std::size_t inoutCount = 0;
            for (const DpiParameter &parameter : import.parameters)
            {
                switch (parameter.direction)
                {
                    case DpiDirection::Input: ++inputCount; break;
                    case DpiDirection::Output: ++outputCount; break;
                    case DpiDirection::Inout: ++inoutCount; break;
                }
            }
            const std::size_t returnCount = import.returnValue.present ? 1U : 0U;
            if (operands.size() != 1U + inputCount + inoutCount + attributes->eventCount ||
                results.size() != returnCount + outputCount + inoutCount)
            {
                error = "dpi.call operand/result layout does not match its import";
                return std::nullopt;
            }

            const std::string suffix = std::to_string(instruction.value);
            std::vector<std::string> declarations;
            std::vector<std::string> callArguments;
            std::vector<std::string> commits;
            declarations.reserve(import.parameters.size());
            callArguments.reserve(import.parameters.size());
            commits.reserve(outputCount + inoutCount + returnCount);
            std::size_t nextInput = 0;
            std::size_t nextOutput = 0;
            std::size_t nextInoutInput = 0;
            std::size_t nextInoutOutput = 0;
            for (std::size_t index = 0; index < import.parameters.size(); ++index)
            {
                const DpiParameter &parameter = import.parameters[index];
                const Type &type = state.program.type(parameter.type);
                std::optional<std::string> cppType = dpiCppType(type, parameter.abi, error);
                if (!cppType)
                {
                    error += ": import=" + symbol + " parameter=" + std::to_string(index);
                    return std::nullopt;
                }
                if (parameter.abi == DpiAbiKind::String &&
                    parameter.direction != DpiDirection::Input)
                {
                    error = "AM C++ emitter does not support DPI output/inout String ABI: import=" +
                            symbol;
                    return std::nullopt;
                }

                const std::string temporary = "dpi_arg_" + suffix + "_" +
                                              std::to_string(index);
                if (parameter.direction == DpiDirection::Input)
                {
                    const VariableId source = operands[1U + nextInput++];
                    if (parameter.abi == DpiAbiKind::String)
                    {
                        declarations.push_back("const std::string " + temporary + " = " +
                                               dpiReadExpr(state, source, parameter.abi, *cppType) +
                                               ";\n");
                        callArguments.push_back(temporary + ".c_str()");
                    }
                    else
                    {
                        declarations.push_back("const " + *cppType + " " + temporary + " = " +
                                               dpiReadExpr(state, source, parameter.abi, *cppType) +
                                               ";\n");
                        callArguments.push_back(temporary);
                    }
                }
                else if (parameter.direction == DpiDirection::Output)
                {
                    declarations.push_back(*cppType + " " + temporary + "{};\n");
                    callArguments.push_back("&" + temporary);
                    const VariableId target = results[returnCount + nextOutput++];
                    commits.push_back(dpiCommitStatement(state, target, parameter.abi, temporary));
                }
                else
                {
                    const VariableId source =
                        operands[1U + inputCount + nextInoutInput++];
                    declarations.push_back(*cppType + " " + temporary + " = " +
                                           dpiReadExpr(state, source, parameter.abi, *cppType) +
                                           ";\n");
                    callArguments.push_back("&" + temporary);
                    const VariableId target =
                        results[returnCount + outputCount + nextInoutOutput++];
                    commits.push_back(dpiCommitStatement(state, target, parameter.abi, temporary));
                }
            }

            std::string call = symbol + "(";
            for (std::size_t index = 0; index < callArguments.size(); ++index)
            {
                if (index != 0)
                {
                    call += ", ";
                }
                call += callArguments[index];
            }
            call += ")";

            std::string preamble;
            std::optional<uint32_t> pendingEventSlot;
            std::string fire;
            if (attributes->eventCount != 0 &&
                attributes->eventMode == HostEventMode::Pending)
            {
                const auto slot = state.pendingEventSlotByInstruction.find(instruction.value);
                if (slot == state.pendingEventSlotByInstruction.end())
                {
                    error = "eventful dpi.call is missing a pending-event slot";
                    return std::nullopt;
                }
                pendingEventSlot = slot->second;
                const std::string pending =
                    "pendingHostEvents_[" + std::to_string(*pendingEventSlot) + "]";
                preamble = "if (" + eventHitExpr(state, operands, attributes->eventCount) +
                           ") " + pending + " = true;\n";
                fire = boolExpr(state, operands.front()) + " && " + pending;
            }
            else
            {
                fire = eventFireExpr(state, operands, attributes->eventCount, false);
            }

            std::string code = preamble + "if (" + fire + ") {\n";
            for (const std::string &declaration : declarations)
            {
                code += declaration;
            }
            if (import.returnValue.present)
            {
                const Type &type = state.program.type(import.returnValue.type);
                std::optional<std::string> cppType =
                    dpiCppType(type, import.returnValue.abi, error);
                if (!cppType)
                {
                    error += ": import=" + symbol + " return";
                    return std::nullopt;
                }
                if (import.returnValue.abi == DpiAbiKind::String)
                {
                    error = "AM C++ emitter does not support DPI String return ABI: import=" +
                            symbol;
                    return std::nullopt;
                }
                const std::string temporary = "dpi_return_" + suffix;
                code += *cppType + " " + temporary + " = " + call + ";\n";
                code += dpiCommitStatement(
                    state, results.front(), import.returnValue.abi, temporary);
            }
            else
            {
                code += call + ";\n";
            }
            for (const std::string &commit : commits)
            {
                code += commit;
            }
            if (pendingEventSlot)
            {
                code += "pendingHostEvents_[" + std::to_string(*pendingEventSlot) +
                        "] = false;\n";
            }
            code += "}\n";
            return code;
        }

        // NO0013 F1/F2 windowed emission (plan fields documented in
        // EmitState). Handles the per-instruction actions planned by
        // planWindowedChains; returns nullopt only on plan inconsistency.
        std::optional<std::string> emitWindowedInstruction(const EmitState &state,
                                                           InstructionId instruction,
                                                           std::string &error)
        {
            const int8_t action = state.instructionWindowAction[instruction.value];
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);
            if (action == kWindowActionSkip)
            {
                return std::string();
            }
            if (action == kWindowActionConcat)
            {
                // F2 (currently unplanned — reverted after measurement, see
                // NO0013 §9): operand windows tile the full result width, so
                // the stock zero_words preamble is dead; replaces suffice.
                const Type &resultType = variableType(state, results.front());
                const uint32_t width = resultType.bitWidth;
                const std::string target = wordDataExpr(state, results.front());
                std::string code;
                uint32_t remaining = width;
                for (VariableId operand : operands)
                {
                    const Type &type = variableType(state, operand);
                    remaining -= type.bitWidth;
                    code += "replace_window_words(" + target + ", " +
                            std::to_string(width) + ", " + std::to_string(remaining) + ", " +
                            wordDataExpr(state, operand) + ", " +
                            std::to_string(type.bitWidth) + ");\n";
                }
                // No top-word mask: every reader masks by width
                // (equal_words/any_words/extract_word/slice_words/...), so
                // bits above the concat width are unobservable; the extra
                // RMW statement per concat measured as a cold-line cost.
                return code;
            }
            const EmitState::WindowChainPlan &plan =
                state.windowChainPlans[static_cast<std::size_t>(
                    state.instructionWindowPlan[instruction.value])];
            const std::string chainTarget = wordDataExpr(state, plan.finalVar);
            if (action == kWindowActionChainHead)
            {
                // C_0 full build, straight into the chain final slot D.
                std::string code = "zero_words(" + chainTarget + ", " +
                                   std::to_string(plan.width) + ");\n";
                uint32_t remaining = plan.width;
                for (VariableId operand : operands)
                {
                    const Type &type = variableType(state, operand);
                    remaining -= type.bitWidth;
                    code += "insert_words(" + chainTarget + ", " +
                            std::to_string(plan.width) + ", " +
                            std::to_string(remaining) + ", " +
                            wordDataExpr(state, operand) + ", " +
                            std::to_string(type.bitWidth) + ");\n";
                }
                if (state.instructionWindowMaterialize[instruction.value] != 0)
                {
                    code += "assign_words(" + wordDataExpr(state, results.front()) + ", " +
                            std::to_string(plan.width) + ", " + chainTarget + ", " +
                            std::to_string(plan.width) + ", false);\n";
                }
                return code;
            }
            if (action == kWindowActionChainStep)
            {
                // Later steps only splice their element windows into D; the
                // backbone content is already there by construction.
                const auto stepIt = plan.stepIndexByInstr.find(instruction.value);
                if (stepIt == plan.stepIndexByInstr.end())
                {
                    error = "window chain step missing from plan";
                    return std::nullopt;
                }
                const EmitState::WindowChainPlan::Step &step = plan.steps[stepIt->second];
                std::string code;
                for (const auto &[operandIndex, offset] : step.elems)
                {
                    const VariableId operand = operands[operandIndex];
                    const Type &type = variableType(state, operand);
                    code += "replace_window_words(" + chainTarget + ", " +
                            std::to_string(plan.width) + ", " + std::to_string(offset) +
                            ", " + wordDataExpr(state, operand) + ", " +
                            std::to_string(type.bitWidth) + ");\n";
                }
                if (state.instructionWindowMaterialize[instruction.value] != 0)
                {
                    code += "assign_words(" + wordDataExpr(state, results.front()) + ", " +
                            std::to_string(plan.width) + ", " + chainTarget + ", " +
                            std::to_string(plan.width) + ", false);\n";
                }
                return code;
            }
            // kWindowActionRemapSlice: SliceStatic re-pointed at D (the
            // planner proved the read happens before any overwriting step).
            const Type &resultType = variableType(state, results.front());
            const auto attributes = state.program.sliceStaticAttributes(instruction);
            if (resultType.bitWidth <= 64 && !state.disableWideSliceInline)
            {
                return wideSliceAssign(state, results.front(), chainTarget, plan.width,
                                       "UINT64_C(" + std::to_string(attributes->lsb) + ")");
            }
            return "slice_words(" + wordDataExpr(state, results.front()) + ", " +
                   std::to_string(resultType.bitWidth) + ", " + chainTarget + ", " +
                   std::to_string(plan.width) + ", UINT64_C(" +
                   std::to_string(attributes->lsb) + "));\n";
        }

        // NO0014 dynblend cone collapse emission (plan documented in
        // EmitState). Emits one conditional blend_window_dyn_words per cone
        // into the chain accumulator, matching gsim's per-port conditional
        // dynamic scalar stores.
        std::optional<std::string> emitDynBlendInstruction(const EmitState &state,
                                                           InstructionId instruction,
                                                           std::string &error)
        {
            const int8_t action = state.instructionDynBlendAction[instruction.value];
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);
            if (action == kDynBlendSkip)
            {
                return std::string();
            }
            const EmitState::DynBlendPlan &plan =
                state.dynBlendPlans[static_cast<std::size_t>(
                    state.instructionDynBlendPlan[instruction.value])];
            const std::string accumulator = wordDataExpr(state, plan.finalVar);
            if (action == kDynBlendRemapSlice)
            {
                // Slice consumer of an intermediate result, re-pointed at D.
                const Type &resultType = variableType(state, results.front());
                if (state.program.opcode(instruction) == Opcode::SliceStatic)
                {
                    const auto attributes = state.program.sliceStaticAttributes(instruction);
                    if (resultType.bitWidth <= 64 && !state.disableWideSliceInline)
                    {
                        return wideSliceAssign(state, results.front(), accumulator,
                                               plan.width,
                                               "UINT64_C(" + std::to_string(attributes->lsb) + ")");
                    }
                    return "slice_words(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " + accumulator +
                           ", " + std::to_string(plan.width) + ", UINT64_C(" +
                           std::to_string(attributes->lsb) + "));\n";
                }
                const Type &indexType = variableType(state, operands[1]);
                if (resultType.bitWidth <= 64 && indexType.kind == TypeKind::BitVector &&
                    indexType.bitWidth <= 64 && !state.disableWideSliceInline)
                {
                    // Narrow index == index_words with zero high words;
                    // extract_word covers the out-of-range (>= width) case.
                    return wideSliceAssign(state, results.front(), accumulator,
                                           plan.width, valueExpr(state, operands[1]));
                }
                return "slice_dynamic_words(" + wordDataExpr(state, results.front()) +
                       ", " + std::to_string(resultType.bitWidth) + ", " + accumulator +
                       ", " + std::to_string(plan.width) + ", " +
                       wordDataExpr(state, operands[1]) + ", " +
                       std::to_string(indexType.bitWidth) + ");\n";
            }
            const EmitState::DynBlendPlan::Cone *cone = nullptr;
            for (const auto &candidate : plan.cones)
            {
                if (candidate.tail == instruction)
                {
                    cone = &candidate;
                    break;
                }
            }
            if (cone == nullptr)
            {
                error = "dynblend cone tail missing from plan";
                return std::nullopt;
            }
            std::string code;
            if (action == kDynBlendHead)
            {
                // Chain head: seed the accumulator with the external base.
                code += "assign_words(" + accumulator + ", " +
                        std::to_string(plan.width) + ", " +
                        wordDataExpr(state, cone->base) + ", " +
                        std::to_string(plan.width) + ", false);\n";
            }
            const Type &indexType = variableType(state, cone->idx);
            const std::string blend =
                "blend_window_dyn_words(" + accumulator + ", " +
                std::to_string(plan.width) + ", " + wordDataExpr(state, cone->idx) +
                ", " + std::to_string(indexType.bitWidth) + ", " +
                wordDataExpr(state, cone->ones) + ", " + wordDataExpr(state, cone->elem) +
                ", " + std::to_string(cone->elemWidth) + ");\n";
            if (cone->cond.valid())
            {
                code += "if (" + boolExpr(state, cone->cond) + ") { " + blend + "}\n";
            }
            else
            {
                code += blend;
            }
            if (state.instructionDynBlendMaterialize[instruction.value] != 0)
            {
                code += "assign_words(" + wordDataExpr(state, cone->result) + ", " +
                        std::to_string(plan.width) + ", " + accumulator + ", " +
                        std::to_string(plan.width) + ", false);\n";
            }
            return code;
        }

        std::optional<std::string> emitNonScalarInstruction(const EmitState &state,
                                                            InstructionId instruction,
                                                            std::string &error)
        {
            const Opcode opcode = state.program.opcode(instruction);
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);
            const auto isBitVector = [&](VariableId variable) {
                return variableType(state, variable).kind == TypeKind::BitVector;
            };
            if (!std::all_of(operands.begin(), operands.end(), isBitVector) ||
                !std::all_of(results.begin(), results.end(), isBitVector))
            {
                error = "non-scalar pure AM instruction requires bit-vector operands: " +
                        std::string(toString(opcode));
                return std::nullopt;
            }

            const auto binaryCall = [&](std::string_view helper, uint32_t operation) {
                const Type &resultType = variableType(state, results.front());
                const Type &leftType = variableType(state, operands[0]);
                const Type &rightType = variableType(state, operands[1]);
                const bool commonSigned = leftType.signedness == Signedness::Signed &&
                                          rightType.signedness == Signedness::Signed;
                return std::string(helper) + "(" + wordDataExpr(state, results.front()) + ", " +
                       std::to_string(resultType.bitWidth) + ", " +
                       wordDataExpr(state, operands[0]) + ", " +
                       std::to_string(leftType.bitWidth) + ", " +
                       (commonSigned ? "true" : "false") + ", " +
                       wordDataExpr(state, operands[1]) + ", " +
                       std::to_string(rightType.bitWidth) + ", " +
                       (commonSigned ? "true" : "false") + ", " +
                       std::to_string(operation) + ");\n";
            };
            const auto scalarResult = [&](std::string expression) {
                return valueExpr(state, results.front()) + " = (" + expression + ") ? 1 : 0;\n";
            };

            switch (opcode)
            {
                case Opcode::Add:
                    return binaryCall("arithmetic_words", 0);
                case Opcode::Sub:
                    return binaryCall("arithmetic_words", 1);
                case Opcode::Mul:
                    return binaryCall("arithmetic_words", 2);
                case Opcode::And:
                    return binaryCall("bitwise_words", 0);
                case Opcode::Or:
                    return binaryCall("bitwise_words", 1);
                case Opcode::Xor:
                    return binaryCall("bitwise_words", 2);
                case Opcode::Xnor:
                    return binaryCall("bitwise_words", 3);
                case Opcode::Not:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands.front());
                    return "not_words(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands.front()) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " +
                           (sourceType.signedness == Signedness::Signed ? "true" : "false") +
                           ");\n";
                }
                case Opcode::Eq:
                case Opcode::Ne:
                case Opcode::Lt:
                case Opcode::Le:
                case Opcode::Gt:
                case Opcode::Ge:
                {
                    const Type &leftType = variableType(state, operands[0]);
                    const Type &rightType = variableType(state, operands[1]);
                    const bool isSigned = leftType.signedness == Signedness::Signed &&
                                          rightType.signedness == Signedness::Signed;
                    std::string relation;
                    switch (opcode)
                    {
                        case Opcode::Eq: relation = " == 0"; break;
                        case Opcode::Ne: relation = " != 0"; break;
                        case Opcode::Lt: relation = " < 0"; break;
                        case Opcode::Le: relation = " <= 0"; break;
                        case Opcode::Gt: relation = " > 0"; break;
                        case Opcode::Ge: relation = " >= 0"; break;
                        default: break;
                    }
                    return scalarResult(
                        "compare_words(" + wordDataExpr(state, operands[0]) + ", " +
                        std::to_string(leftType.bitWidth) + ", " +
                        wordDataExpr(state, operands[1]) + ", " +
                        std::to_string(rightType.bitWidth) + ", " +
                        (isSigned ? "true" : "false") + ")" + relation);
                }
                case Opcode::LogicAnd:
                case Opcode::LogicOr:
                {
                    const Type &leftType = variableType(state, operands[0]);
                    const Type &rightType = variableType(state, operands[1]);
                    const std::string lhs = "any_words(" + wordDataExpr(state, operands[0]) +
                                            ", " + std::to_string(leftType.bitWidth) + ")";
                    const std::string rhs = "any_words(" + wordDataExpr(state, operands[1]) +
                                            ", " + std::to_string(rightType.bitWidth) + ")";
                    return scalarResult(lhs + (opcode == Opcode::LogicAnd ? " && " : " || ") + rhs);
                }
                case Opcode::LogicNot:
                {
                    const Type &type = variableType(state, operands.front());
                    return scalarResult("!any_words(" + wordDataExpr(state, operands.front()) +
                                        ", " + std::to_string(type.bitWidth) + ")");
                }
                case Opcode::ReduceAnd:
                case Opcode::ReduceNand:
                case Opcode::ReduceOr:
                case Opcode::ReduceNor:
                case Opcode::ReduceXor:
                case Opcode::ReduceXnor:
                {
                    const Type &type = variableType(state, operands.front());
                    const uint32_t operation = static_cast<uint32_t>(opcode) -
                                               static_cast<uint32_t>(Opcode::ReduceAnd);
                    return scalarResult("reduce_words(" + wordDataExpr(state, operands.front()) +
                                        ", " + std::to_string(type.bitWidth) + ", " +
                                        std::to_string(operation) + ")");
                }
                case Opcode::Shl:
                case Opcode::LogicalShr:
                case Opcode::ArithmeticShr:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands[0]);
                    const Type &amountType = variableType(state, operands[1]);
                    const uint32_t operation = opcode == Opcode::Shl
                                                   ? 0
                                                   : opcode == Opcode::LogicalShr ? 1 : 2;
                    return "shift_words(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands[0]) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " +
                           (sourceType.signedness == Signedness::Signed ? "true" : "false") +
                           ", " + wordDataExpr(state, operands[1]) + ", " +
                           std::to_string(amountType.bitWidth) + ", " +
                           std::to_string(operation) + ");\n";
                }
                case Opcode::Mux:
                {
                    const Type &trueType = variableType(state, operands[1]);
                    const Type &falseType = variableType(state, operands[2]);
                    const Signedness common =
                        trueType.signedness == Signedness::Signed &&
                                falseType.signedness == Signedness::Signed
                            ? Signedness::Signed
                            : Signedness::Unsigned;
                    return "if (" + boolExpr(state, operands[0]) + ") { " +
                           assignVariableStatement(state, results.front(), operands[1], common) +
                           "} else { " +
                           assignVariableStatement(state, results.front(), operands[2], common) +
                           "}\n";
                }
                case Opcode::Concat:
                {
                    const Type &resultType = variableType(state, results.front());
                    std::string code = "zero_words(" + wordDataExpr(state, results.front()) +
                                       ", " + std::to_string(resultType.bitWidth) + ");\n";
                    uint32_t remaining = resultType.bitWidth;
                    for (VariableId operand : operands)
                    {
                        const Type &type = variableType(state, operand);
                        remaining -= type.bitWidth;
                        code += "insert_words(" + wordDataExpr(state, results.front()) + ", " +
                                std::to_string(resultType.bitWidth) + ", " +
                                std::to_string(remaining) + ", " + wordDataExpr(state, operand) +
                                ", " + std::to_string(type.bitWidth) + ");\n";
                    }
                    return code;
                }
                case Opcode::Replicate:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands.front());
                    const uint32_t count = resultType.bitWidth / sourceType.bitWidth;
                    std::string code = "zero_words(" + wordDataExpr(state, results.front()) +
                                       ", " + std::to_string(resultType.bitWidth) + ");\n";
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        code += "insert_words(" + wordDataExpr(state, results.front()) + ", " +
                                std::to_string(resultType.bitWidth) + ", " +
                                std::to_string(index * sourceType.bitWidth) + ", " +
                                wordDataExpr(state, operands.front()) + ", " +
                                std::to_string(sourceType.bitWidth) + ");\n";
                    }
                    return code;
                }
                case Opcode::SliceStatic:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands.front());
                    const auto attributes = state.program.sliceStaticAttributes(instruction);
                    if (isExplodedState(state, operands.front()))
                    {
                        // NO0017 §5: the planner guarantees the site spans
                        // whole elements (K divides lsb and the width).
                        const uint32_t elemWidth =
                            state.explodedElementWidth[operands.front().value];
                        if (resultType.bitWidth > 64 ||
                            resultType.bitWidth % elemWidth != 0 ||
                            attributes->lsb % elemWidth != 0)
                        {
                            error = "exploded state slice is not element-aligned";
                            return std::nullopt;
                        }
                        return explodedSliceAssign(state, results.front(),
                                                   operands.front(), attributes->lsb);
                    }
                    if (resultType.bitWidth <= 64 && !state.disableWideSliceInline)
                    {
                        return wideSliceAssign(state, results.front(),
                                               wordDataExpr(state, operands.front()),
                                               sourceType.bitWidth,
                                               "UINT64_C(" + std::to_string(attributes->lsb) + ")");
                    }
                    return "slice_words(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands.front()) + ", " +
                           std::to_string(sourceType.bitWidth) + ", UINT64_C(" +
                           std::to_string(attributes->lsb) + "));\n";
                }
                case Opcode::SliceDynamic:
                case Opcode::SliceArray:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands[0]);
                    const Type &indexType = variableType(state, operands[1]);
                    if (opcode == Opcode::SliceArray && isExplodedState(state, operands[0]))
                    {
                        // NO0017 §5: element-indexed read; the planner
                        // guarantees result width == element width.
                        const uint32_t elemWidth =
                            state.explodedElementWidth[operands[0].value];
                        if (resultType.bitWidth != elemWidth)
                        {
                            error = "exploded state array-slice width mismatch";
                            return std::nullopt;
                        }
                        const uint64_t count = sourceType.bitWidth / elemWidth;
                        const std::string index = valueExpr(state, operands[1]);
                        return valueExpr(state, results.front()) + " = (" + index +
                               " >= " + std::to_string(count) + " ? 0 : " +
                               explodedMemberName(operands[0]) + "[" + index + "]) & " +
                               maskExpr(resultType.bitWidth) + ";\n";
                    }
                    if (opcode == Opcode::SliceDynamic && isExplodedState(state, operands[0]))
                    {
                        // NO0017 §5: dynamic bit-offset read of one element
                        // width. Any alignment is exact: the window spans
                        // elements e=floor(start/K) (bits r..K-1) and e+1
                        // (bits 0..r-1); out-of-range reads produce 0,
                        // matching extract_word.
                        const uint32_t elemWidth =
                            state.explodedElementWidth[operands[0].value];
                        if (resultType.bitWidth != elemWidth)
                        {
                            error = "exploded state dynamic-slice width mismatch";
                            return std::nullopt;
                        }
                        const uint64_t count = sourceType.bitWidth / elemWidth;
                        const std::string start = valueExpr(state, operands[1]);
                        const std::string suffix = std::to_string(instruction.value);
                        const std::string elemVar = "wvE_" + suffix;
                        const std::string remVar = "wvR_" + suffix;
                        const std::string member = explodedMemberName(operands[0]);
                        const std::string kText = std::to_string(elemWidth);
                        return "{ const std::uint64_t " + elemVar + " = (" + start +
                               ") / " + kText + "; const std::uint32_t " + remVar +
                               " = (std::uint32_t)((" + start + ") % " + kText + "); " +
                               valueExpr(state, results.front()) + " = (((" + elemVar +
                               " < " + std::to_string(count) + ") ? (" + member + "[" +
                               elemVar + "] >> " + remVar + ") : 0) | ((" + remVar +
                               " != 0 && " + elemVar + " < " +
                               std::to_string(count - 1) + ") ? (" + member + "[" +
                               elemVar + " + 1] << (" + kText + " - " + remVar +
                               ")) : 0)) & " + maskExpr(resultType.bitWidth) + ";\n}\n";
                    }
                    if (resultType.bitWidth <= 64 && indexType.kind == TypeKind::BitVector &&
                        indexType.bitWidth <= 64 && !state.disableWideSliceInline)
                    {
                        // Narrow index == index_words with zero high words.
                        std::string start = valueExpr(state, operands[1]);
                        if (opcode == Opcode::SliceArray)
                        {
                            // element == count maps to sourceWidth (-> 0).
                            const uint64_t count =
                                sourceType.bitWidth / resultType.bitWidth;
                            start = "(" + start + " >= " + std::to_string(count) +
                                    " ? " + std::to_string(sourceType.bitWidth) + " : " +
                                    start + " * " + std::to_string(resultType.bitWidth) + ")";
                        }
                        return wideSliceAssign(state, results.front(),
                                               wordDataExpr(state, operands[0]),
                                               sourceType.bitWidth, start);
                    }
                    const std::string helper =
                        opcode == Opcode::SliceDynamic ? "slice_dynamic_words" : "slice_array_words";
                    return helper + "(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands[0]) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " +
                           wordDataExpr(state, operands[1]) + ", " +
                           std::to_string(indexType.bitWidth) + ");\n";
                }
                case Opcode::Insert:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &rowType = variableType(state, operands[0]);
                    const Type &dataType = variableType(state, operands[1]);
                    const auto attributes = state.program.sliceStaticAttributes(instruction);
                    return "insert_replace_words(" + wordDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands[0]) + ", " +
                           std::to_string(rowType.bitWidth) + ", UINT64_C(" +
                           std::to_string(attributes->lsb) + "), " +
                           wordDataExpr(state, operands[1]) + ", " +
                           std::to_string(dataType.bitWidth) + ");\n";
                }
                case Opcode::Div:
                case Opcode::Mod:
                    error = "wide div/mod is not yet supported by the AM C++ emitter";
                    return std::nullopt;
                default:
                    error = "unsupported non-scalar opcode in the AM C++ emitter: " +
                            std::string(toString(opcode));
                    return std::nullopt;
            }
        }

        std::optional<std::string> emitArrayPureInstruction(const EmitState &state,
                                                            InstructionId instruction,
                                                            std::string &error)
        {
            const Opcode opcode = state.program.opcode(instruction);
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);
            const Type &resultType = variableType(state, results.front());
            switch (opcode)
            {
                case Opcode::ArrayMux:
                {
                    const uint32_t rows = variableType(state, operands[0]).bitWidth;
                    return "array_mux_words(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands[0]) + ", " +
                           wordDataExpr(state, operands[1]) + ", " +
                           wordDataExpr(state, operands[2]) + ", " +
                           std::to_string(resultType.bitWidth / rows) + ");\n";
                }
                case Opcode::ArrayReduceOr:
                case Opcode::ArrayReduceAnd:
                case Opcode::ArrayReduceXor:
                {
                    // Per-lane then cross-lane reduction is the plain
                    // full-width reduction (associativity), so reduce_words
                    // with the And/Or/Xor operation code is exact.
                    const Type &dataType = variableType(state, operands.front());
                    const uint32_t operation = opcode == Opcode::ArrayReduceAnd
                                                   ? 0
                                                   : opcode == Opcode::ArrayReduceOr ? 2 : 4;
                    return valueExpr(state, results.front()) + " = (reduce_words(" +
                           wordDataExpr(state, operands.front()) + ", " +
                           std::to_string(dataType.bitWidth) + ", " +
                           std::to_string(operation) + ")) ? 1 : 0;\n";
                }
                case Opcode::ArrayBroadcast:
                {
                    const Type &sourceType = variableType(state, operands.front());
                    return "array_broadcast_words(" + wordDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands.front()) + ", " +
                           std::to_string(sourceType.bitWidth) + ");\n";
                }
                case Opcode::ArrayOnehot:
                {
                    const Type &indexType = variableType(state, operands.front());
                    return "array_onehot_words(" + wordDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands.front()) + ", " +
                           std::to_string(indexType.bitWidth) + ");\n";
                }
                case Opcode::ArrayReduceLanesOr:
                case Opcode::ArrayReduceLanesAnd:
                case Opcode::ArrayReduceLanesXor:
                {
                    // Per-lane reduction into the rows-bit guard vector;
                    // elemWidth is implicit as dataWidth / rows.
                    const Type &dataType = variableType(state, operands.front());
                    const uint32_t operation = opcode == Opcode::ArrayReduceLanesAnd
                                                   ? 0
                                                   : opcode == Opcode::ArrayReduceLanesOr ? 2 : 4;
                    return "array_reduce_lanes_words(" + wordDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands.front()) + ", " +
                           std::to_string(dataType.bitWidth / resultType.bitWidth) + ", " +
                           std::to_string(operation) + ");\n";
                }
                default:
                    error = "unsupported array opcode in the AM C++ emitter: " +
                            std::string(toString(opcode));
                    return std::nullopt;
            }
        }

        // One fused if/else for a same-select mux arm list (NO0006/NO0007
        // P2): the select evaluates once per structure and each branch
        // carries every member's arm assignment in list order, so a chained
        // arm simply reads the earlier branch assignment. Arm rendering
        // mirrors the standalone Mux cases: narrow values use the masked
        // scalar assignment, wide values use the same assignVariableStatement
        // form as the wide Mux case.
        std::optional<std::string> emitMuxRun(const EmitState &state,
                                              const std::vector<InstructionId> &members,
                                              std::string &error)
        {
            (void)error;
            const VariableId select = state.program.operands(members.front())[0];
            std::string trueBody;
            std::string falseBody;
            for (const InstructionId member : members)
            {
                const auto operands = state.program.operands(member);
                const auto results = state.program.results(member);
                const auto isNonScalar = [&](VariableId variable) {
                    const Type &type = variableType(state, variable);
                    return type.kind != TypeKind::BitVector || type.bitWidth > 64;
                };
                const bool wide =
                    std::any_of(operands.begin(), operands.end(), isNonScalar) ||
                    std::any_of(results.begin(), results.end(), isNonScalar);
                if (wide)
                {
                    const Type &trueType = variableType(state, operands[1]);
                    const Type &falseType = variableType(state, operands[2]);
                    const Signedness common =
                        trueType.signedness == Signedness::Signed &&
                                falseType.signedness == Signedness::Signed
                            ? Signedness::Signed
                            : Signedness::Unsigned;
                    trueBody += assignVariableStatement(state, results.front(), operands[1],
                                                        common);
                    falseBody += assignVariableStatement(state, results.front(), operands[2],
                                                         common);
                }
                else
                {
                    const Type &resultType = variableType(state, results.front());
                    trueBody += valueExpr(state, results.front()) + " = (" +
                                resizedExpr(state, operands[1], resultType.bitWidth,
                                            resultType.signedness) +
                                ") & " + maskExpr(resultType.bitWidth) + ";\n";
                    falseBody += valueExpr(state, results.front()) + " = (" +
                                 resizedExpr(state, operands[2], resultType.bitWidth,
                                             resultType.signedness) +
                                 ") & " + maskExpr(resultType.bitWidth) + ";\n";
                }
            }
            return "if (" + boolExpr(state, select) + ") { " + trueBody + "} else { " +
                   falseBody + "}\n";
        }

        std::optional<std::string> emitInstruction(const EmitState &state,
                                                   InstructionId instruction,
                                                   std::string &error);

        // Mux-run two-phase emission (NO0008): the run atoms' cone members
        // emit normally in atom order, then one fused if/else covers the
        // root muxes. The run planner guarantees cones never read an
        // earlier run root's result, so this order is def-before-use.
        std::optional<std::string> emitMuxFusionRun(const EmitState &state,
                                                    const EmitState::MuxRunPlan &plan,
                                                    std::string &error)
        {
            state.muxRunEmissionActive = true;
            std::string code;
            for (const InstructionId member : plan.preamble)
            {
                std::optional<std::string> emitted =
                    emitInstruction(state, member, error);
                if (!emitted)
                {
                    state.muxRunEmissionActive = false;
                    return std::nullopt;
                }
                code += *emitted;
            }
            std::optional<std::string> fused = emitMuxRun(state, plan.arms, error);
            if (!fused)
            {
                state.muxRunEmissionActive = false;
                return std::nullopt;
            }
            code += *fused;
            state.muxRunEmissionActive = false;
            return code;
        }

        std::optional<std::string> emitInstruction(const EmitState &state,
                                                   InstructionId instruction,
                                                   std::string &error)
        {
            const Opcode opcode = state.program.opcode(instruction);
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);
            const auto resultAssign = [&](std::string expression) {
                const Type &resultType = variableType(state, results.front());
                return valueExpr(state, results.front()) + " = (" + expression + ") & " +
                       maskExpr(resultType.bitWidth) + ";\n";
            };
            const auto binaryOperands = [&](const Type &resultType) {
                const std::string lhs = resizedExpr(
                    state, operands[0], resultType.bitWidth, resultType.signedness);
                const std::string rhs = resizedExpr(
                    state, operands[1], resultType.bitWidth, resultType.signedness);
                return std::array<std::string, 2>{lhs, rhs};
            };

            if (!state.muxRunEmissionActive &&
                instruction.value < state.instructionMuxRun.size())
            {
                const int32_t muxRun = state.instructionMuxRun[instruction.value];
                if (muxRun >= 0)
                {
                    const EmitState::MuxRunPlan &plan = state.muxRunPlans[muxRun];
                    if (plan.head != instruction.value)
                    {
                        // The run head emitted the whole two-phase code.
                        return std::string();
                    }
                    return emitMuxFusionRun(state, plan, error);
                }
            }

            // NO0013/NO0014 planned emission actions: checked here (not in
            // emitNonScalarInstruction) because some planned instructions
            // (e.g. dynblend zext Assigns) route through the special paths
            // below instead of the non-scalar switch.
            if (instruction.value < state.instructionDynBlendAction.size() &&
                state.instructionDynBlendAction[instruction.value] >= 0)
            {
                return emitDynBlendInstruction(state, instruction, error);
            }
            if (instruction.value < state.instructionWindowAction.size() &&
                state.instructionWindowAction[instruction.value] >= 0)
            {
                return emitWindowedInstruction(state, instruction, error);
            }

            if (opcode == Opcode::Assign &&
                (isWideBitVector(state, results.front()) ||
                 isWideBitVector(state, operands.front())))
            {
                const Type &resultType = variableType(state, results.front());
                const Type &sourceType = variableType(state, operands.front());
                if (resultType.kind != TypeKind::BitVector ||
                    sourceType.kind != TypeKind::BitVector)
                {
                    error = "wide AM assign requires bit-vector operands";
                    return std::nullopt;
                }
                if (resultType.bitWidth <= 64)
                {
                    return resultAssign("(" + wideDataExpr(state, operands.front()) + ")[0]");
                }
                const std::string sign =
                    sourceType.signedness == Signedness::Signed ? "true" : "false";
                if (sourceType.bitWidth <= 64)
                {
                    return "assign_words_from_scalar(" + wideDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           valueExpr(state, operands.front()) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " + sign + ");\n";
                }
                return "assign_words(" + wideDataExpr(state, results.front()) + ", " +
                       std::to_string(resultType.bitWidth) + ", " +
                       wideDataExpr(state, operands.front()) + ", " +
                       std::to_string(sourceType.bitWidth) + ", " + sign + ");\n";
            }

            if (opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                opcode == Opcode::ChangedNeg)
            {
                const std::string setResult =
                    changedResultAssignPrefix(state, results.front());
                const Type &type = variableType(state, operands.front());
                if (type.kind == TypeKind::Array)
                {
                    if (opcode != Opcode::ChangedAny)
                    {
                        error = "edge changed opcode requires a bit-vector operand";
                        return std::nullopt;
                    }
                    if (state.arrayDetectorAccum >= 0)
                    {
                        // ST00011: the change flag was accumulated at this
                        // Block's own write sites; the whole-array compare and
                        // baseline copy are gone.
                        return setResult +
                               arrayWatchAccumExpr(
                                   state, static_cast<uint32_t>(state.arrayDetectorAccum)) +
                               ");\n";
                    }
                    const uint64_t words = storedWordCount(state, operands.front());
                    const std::string current = wordDataExpr(state, operands[0]);
                    const std::string previous = wordDataExpr(state, operands[1]);
                    return setResult + "!std::equal(" + current + ", " + current + " + " +
                           std::to_string(words) + ", " + previous + "));\n" +
                           "std::copy_n(" + current + ", " + std::to_string(words) + ", " +
                           previous + ");\n";
                }
                if (type.kind == TypeKind::Real)
                {
                    if (opcode != Opcode::ChangedAny)
                    {
                        error = "edge changed opcode requires a bit-vector operand";
                        return std::nullopt;
                    }
                    const uint64_t current = variableStorage(state, operands[0]).offset;
                    const uint64_t previous = variableStorage(state, operands[1]).offset;
                    return setResult + "realValues_[" + std::to_string(current) +
                           "] != realValues_[" + std::to_string(previous) + ");\nrealValues_[" +
                           std::to_string(previous) + "] = realValues_[" +
                           std::to_string(current) + "];\n";
                }
                if (type.kind == TypeKind::String)
                {
                    if (opcode != Opcode::ChangedAny)
                    {
                        error = "edge changed opcode requires a bit-vector operand";
                        return std::nullopt;
                    }
                    const uint64_t current = variableStorage(state, operands[0]).offset;
                    const uint64_t previous = variableStorage(state, operands[1]).offset;
                    return setResult + "stringValues_[" + std::to_string(current) +
                           "] != stringValues_[" + std::to_string(previous) +
                           ");\nstringValues_[" +
                           std::to_string(previous) + "] = stringValues_[" +
                           std::to_string(current) + "];\n";
                }
                if (!isWideBitVector(state, operands.front()))
                {
                    // Scalar bit-vector changed operations are emitted below.
                }
                else
                {
                    const uint32_t width = type.bitWidth;
                std::string event;
                if (opcode == Opcode::ChangedAny)
                {
                    event = "!equal_words(" + wideDataExpr(state, operands[0]) + ", " +
                            wideDataExpr(state, operands[1]) + ", " +
                            std::to_string(width) + ")";
                }
                else
                {
                    const std::string current = "any_words(" + wideDataExpr(state, operands[0]) +
                                                ", " + std::to_string(width) + ")";
                    const std::string previous = "any_words(" + wideDataExpr(state, operands[1]) +
                                                 ", " + std::to_string(width) + ")";
                    event = opcode == Opcode::ChangedPos
                                ? "(" + current + " && !" + previous + ")"
                                : "(!" + current + " && " + previous + ")";
                }
                return setResult + event + ");\n" +
                       "assign_words(" + wideDataExpr(state, operands[1]) + ", " +
                       std::to_string(width) + ", " + wideDataExpr(state, operands[0]) + ", " +
                       std::to_string(width) + ", false);\n";
                }
            }

            if (opcode == Opcode::RegisterWriteDynLane)
            {
                // Dynamic-lane register write (NO0012 Tier 3): replace bits
                // [offset, offset + width(data)) of the target when cond
                // holds; the helper has an aligned fast path for the
                // word-aligned lanes this opcode was built for.
                const VariableId target = operands[3];
                const VariableId offset = operands[1];
                const VariableId data = operands[2];
                const uint32_t targetWidth = variableType(state, target).bitWidth;
                const uint32_t dataWidth = variableType(state, data).bitWidth;
                const std::string writeArgs =
                    wordDataExpr(state, target) + ", " +
                    std::to_string(targetWidth) + ", " +
                    wordDataExpr(state, offset) + ", " +
                    wordDataExpr(state, data) + ", " +
                    std::to_string(dataWidth) + ")";
                std::string body;
                if (state.scalarWriteRaise >= 0)
                {
                    body = scalarWatchFlagExpr(
                               state, static_cast<uint32_t>(state.scalarWriteRaise)) +
                           " |= dynlane_write_words_detect(" + writeArgs + ";\n";
                }
                else
                {
                    body = "dynlane_write_words(" + writeArgs + ";\n";
                }
                return "if (" + boolExpr(state, operands[0]) + ") { " + body + "}\n";
            }

            const StateWriteLayout wideWriteLayout = stateWriteLayout(opcode);
            if (wideWriteLayout.isStateWrite && !wideWriteLayout.memory &&
                isWideBitVector(state, operands[wideWriteLayout.targetIndex]))
            {
                if (isExplodedState(state, operands[wideWriteLayout.targetIndex]))
                {
                    // NO0017 §5: exploded states write per element (constant
                    // mask) or per-element loops (full width); the commit
                    // word loops are gone.
                    return emitExplodedStateWrite(state, instruction, wideWriteLayout,
                                                  error);
                }
                // Wide scalar write: target/data(/mask) share one Type, so all
                // are wide word arrays. A present cond gates the whole
                // statement; a present mask turns the plain copy into a
                // read-modify-write mix at commit time.
                const VariableId target = operands[wideWriteLayout.targetIndex];
                const VariableId data = operands[wideWriteLayout.dataIndex];
                const uint32_t width = variableType(state, target).bitWidth;
                std::string body;
                std::string flag;
                if (isRegisterWriteOpcode(opcode) && state.scalarWriteRaise >= 0)
                {
                    flag = scalarWatchFlagExpr(
                        state, static_cast<uint32_t>(state.scalarWriteRaise));
                }
                if (wideWriteLayout.hasMask)
                {
                    const VariableId mask =
                        operands[wideWriteLayout.hasCond ? 1 : 0];
                    // ST00013: report a real change for the raise flag.
                    body += emitMaskedWriteWords(state, wideDataExpr(state, target),
                                                 wideDataExpr(state, data), mask, width, flag,
                                                 "write_mask_" + std::to_string(instruction.value));
                }
                else if (!flag.empty())
                {
                    // ST00013: report a real change for the raise flag.
                    body = flag + " |= assign_words_detect(" +
                           wideDataExpr(state, target) +
                           ", " + std::to_string(width) + ", " +
                           wideDataExpr(state, data) + ", " +
                           std::to_string(width) + ", false);\n";
                }
                else
                {
                    body = "assign_words(" + wideDataExpr(state, target) + ", " +
                           std::to_string(width) + ", " + wideDataExpr(state, data) +
                           ", " + std::to_string(width) + ", false);\n";
                }
                const std::string dirtyMarks =
                    commitInputDirtyMarks(state, instruction);
                if (!flag.empty() && !dirtyMarks.empty())
                {
                    body += "if (" + flag + ") { " + dirtyMarks + "}\n";
                }
                if (wideWriteLayout.hasCond)
                {
                    return "if (" + boolExpr(state, operands[0]) + ") { " + body + "}\n";
                }
                return body;
            }

            if (opcode == Opcode::ArrayMux || opcode == Opcode::ArrayReduceOr ||
                opcode == Opcode::ArrayReduceAnd || opcode == Opcode::ArrayReduceXor ||
                opcode == Opcode::ArrayBroadcast || opcode == Opcode::ArrayOnehot ||
                opcode == Opcode::ArrayReduceLanesOr || opcode == Opcode::ArrayReduceLanesAnd ||
                opcode == Opcode::ArrayReduceLanesXor)
            {
                return emitArrayPureInstruction(state, instruction, error);
            }

            const bool deferredUnsupported =
                opcode == Opcode::MemoryRead || opcode == Opcode::MemoryWrite ||
                opcode == Opcode::MemoryWriteCond || opcode == Opcode::MemoryWriteMask ||
                opcode == Opcode::MemoryWriteCondMask ||
                opcode == Opcode::MemoryFill || opcode == Opcode::MemoryReadAll ||
                opcode == Opcode::MemoryWriteLanes || opcode == Opcode::SystemFunction ||
                opcode == Opcode::SystemTask || opcode == Opcode::DpiCall;
            if (!deferredUnsupported)
            {
                const auto isNonScalar = [&](VariableId variable) {
                    const Type &type = variableType(state, variable);
                    return type.kind != TypeKind::BitVector || type.bitWidth > 64;
                };
                if (std::any_of(operands.begin(), operands.end(), isNonScalar) ||
                    std::any_of(results.begin(), results.end(), isNonScalar))
                {
                    return emitNonScalarInstruction(state, instruction, error);
                }
            }

            switch (opcode)
            {
                case Opcode::Assign:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands.front());
                    return resultAssign(resizedExpr(state,
                                                    operands.front(),
                                                    resultType.bitWidth,
                                                    sourceType.signedness));
                }
                case Opcode::Add:
                case Opcode::Sub:
                case Opcode::Mul:
                case Opcode::And:
                case Opcode::Or:
                case Opcode::Xor:
                case Opcode::Xnor:
                {
                    const Type &resultType = variableType(state, results.front());
                    const auto values = binaryOperands(resultType);
                    std::string token;
                    switch (opcode)
                    {
                        case Opcode::Add: token = "+"; break;
                        case Opcode::Sub: token = "-"; break;
                        case Opcode::Mul: token = "*"; break;
                        case Opcode::And: token = "&"; break;
                        case Opcode::Or: token = "|"; break;
                        case Opcode::Xor: token = "^"; break;
                        case Opcode::Xnor:
                            return resultAssign("~(" + values[0] + " ^ " + values[1] + ")");
                        default: break;
                    }
                    return resultAssign(values[0] + " " + token + " " + values[1]);
                }
                case Opcode::Div:
                case Opcode::Mod:
                {
                    const Type &resultType = variableType(state, results.front());
                    const auto values = binaryOperands(resultType);
                    const char *helper = opcode == Opcode::Div ? "divide_value" : "modulo_value";
                    return resultAssign(std::string(helper) + "(" + values[0] + ", " + values[1] +
                                        ", " + std::to_string(resultType.bitWidth) + ", " +
                                        (resultType.signedness == Signedness::Signed ? "true" : "false") + ")");
                }
                case Opcode::Not:
                    return resultAssign("~" + valueExpr(state, operands.front()));
                case Opcode::Eq:
                case Opcode::Ne:
                case Opcode::Lt:
                case Opcode::Le:
                case Opcode::Gt:
                case Opcode::Ge:
                {
                    const Type &leftType = variableType(state, operands[0]);
                    const Type &rightType = variableType(state, operands[1]);
                    const uint32_t width = std::max(leftType.bitWidth, rightType.bitWidth);
                    const Signedness sign = leftType.signedness == Signedness::Signed &&
                                                    rightType.signedness == Signedness::Signed
                                                ? Signedness::Signed
                                                : Signedness::Unsigned;
                    const std::string lhs = resizedExpr(state, operands[0], width, sign);
                    const std::string rhs = resizedExpr(state, operands[1], width, sign);
                    if (opcode == Opcode::Eq || opcode == Opcode::Ne)
                    {
                        return resultAssign(lhs + (opcode == Opcode::Eq ? " == " : " != ") + rhs);
                    }
                    std::string token;
                    switch (opcode)
                    {
                        case Opcode::Lt: token = "<"; break;
                        case Opcode::Le: token = "<="; break;
                        case Opcode::Gt: token = ">"; break;
                        case Opcode::Ge: token = ">="; break;
                        default: break;
                    }
                    const std::string compare = sign == Signedness::Signed
                                                    ? "signed_value(" + lhs + ", " +
                                                          std::to_string(width) + ") " + token +
                                                          " signed_value(" + rhs + ", " +
                                                          std::to_string(width) + ")"
                                                    : lhs + " " + token + " " + rhs;
                    return resultAssign(compare);
                }
                case Opcode::LogicAnd:
                    return resultAssign(boolExpr(state, operands[0]) + " && " + boolExpr(state, operands[1]));
                case Opcode::LogicOr:
                    return resultAssign(boolExpr(state, operands[0]) + " || " + boolExpr(state, operands[1]));
                case Opcode::LogicNot:
                    return resultAssign("!" + boolExpr(state, operands[0]));
                case Opcode::ReduceAnd:
                case Opcode::ReduceNand:
                {
                    const Type &sourceType = variableType(state, operands.front());
                    const std::string reduced = "((" + valueExpr(state, operands.front()) + " & " +
                                                maskExpr(sourceType.bitWidth) + ") == " +
                                                maskExpr(sourceType.bitWidth) + ")";
                    return resultAssign(opcode == Opcode::ReduceAnd ? reduced : "!(" + reduced + ")");
                }
                case Opcode::ReduceOr:
                case Opcode::ReduceNor:
                {
                    const std::string reduced = boolExpr(state, operands.front());
                    return resultAssign(opcode == Opcode::ReduceOr ? reduced : "!(" + reduced + ")");
                }
                case Opcode::ReduceXor:
                case Opcode::ReduceXnor:
                {
                    const std::string reduced = "(std::popcount(" + valueExpr(state, operands.front()) + ") & 1U)";
                    return resultAssign(opcode == Opcode::ReduceXor ? reduced : "!(" + reduced + ")");
                }
                case Opcode::Shl:
                case Opcode::LogicalShr:
                case Opcode::ArithmeticShr:
                {
                    const Type &resultType = variableType(state, results.front());
                    const char *helper = opcode == Opcode::Shl
                                             ? "shift_left"
                                             : opcode == Opcode::LogicalShr ? "shift_right"
                                                                             : "arithmetic_shift_right";
                    return resultAssign(std::string(helper) + "(" + valueExpr(state, operands[0]) + ", " +
                                        valueExpr(state, operands[1]) + ", " +
                                        std::to_string(resultType.bitWidth) + ", " +
                                        (resultType.signedness == Signedness::Signed ? "true" : "false") + ")");
                }
                case Opcode::Mux:
                {
                    const Type &resultType = variableType(state, results.front());
                    if (state.branchyMux)
                    {
                        // B2 branchy form: one if/else per mux so block bodies
                        // split into small basic blocks (ternary chains kept
                        // giant single-block bodies that clang could not
                        // optimize in acceptable time).
                        return "if (" + boolExpr(state, operands[0]) + ") { " +
                               valueExpr(state, results.front()) + " = (" +
                               resizedExpr(state, operands[1], resultType.bitWidth,
                                           resultType.signedness) +
                               ") & " + maskExpr(resultType.bitWidth) + "; } else { " +
                               valueExpr(state, results.front()) + " = (" +
                               resizedExpr(state, operands[2], resultType.bitWidth,
                                           resultType.signedness) +
                               ") & " + maskExpr(resultType.bitWidth) + "; }\n";
                    }
                    return resultAssign(boolExpr(state, operands[0]) + " ? " +
                                        resizedExpr(state, operands[1], resultType.bitWidth,
                                                    resultType.signedness) +
                                        " : " +
                                        resizedExpr(state, operands[2], resultType.bitWidth,
                                                    resultType.signedness));
                }
                case Opcode::Concat:
                {
                    const std::string suffix = std::to_string(instruction.value);
                    std::string code = "{ std::uint64_t concat_" + suffix + " = 0;\n";
                    uint32_t accumulated = 0;
                    for (VariableId operand : operands)
                    {
                        const uint32_t width = variableType(state, operand).bitWidth;
                        code += "concat_" + suffix + " = concat_value(concat_" + suffix + ", " +
                                std::to_string(accumulated) + ", " + valueExpr(state, operand) + ", " +
                                std::to_string(width) + ");\n";
                        accumulated += width;
                    }
                    code += resultAssign("concat_" + suffix);
                    code += "}\n";
                    return code;
                }
                case Opcode::Replicate:
                {
                    const Type &resultType = variableType(state, results.front());
                    const Type &sourceType = variableType(state, operands.front());
                    const uint32_t count = resultType.bitWidth / sourceType.bitWidth;
                    const std::string suffix = std::to_string(instruction.value);
                    std::string code = "{ std::uint64_t replicate_" + suffix + " = 0;\n";
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        code += "replicate_" + suffix + " = concat_value(replicate_" + suffix + ", " +
                                std::to_string(index * sourceType.bitWidth) + ", " +
                                valueExpr(state, operands.front()) + ", " +
                                std::to_string(sourceType.bitWidth) + ");\n";
                    }
                    code += resultAssign("replicate_" + suffix);
                    code += "}\n";
                    return code;
                }
                case Opcode::SliceStatic:
                {
                    const auto attributes = state.program.sliceStaticAttributes(instruction);
                    return resultAssign("slice_value(" + valueExpr(state, operands.front()) + ", " +
                                        std::to_string(attributes->lsb) + ", " +
                                        std::to_string(variableType(state, results.front()).bitWidth) + ")");
                }
                case Opcode::Insert:
                {
                    const auto attributes = state.program.sliceStaticAttributes(instruction);
                    const uint32_t dataWidth = variableType(state, operands[1]).bitWidth;
                    const std::string lsb = std::to_string(attributes->lsb);
                    const std::string window =
                        "(" + maskExpr(dataWidth) + " << " + lsb + ")";
                    return resultAssign("((" + valueExpr(state, operands[0]) + " & ~" +
                                        window + ") | ((" + valueExpr(state, operands[1]) +
                                        " & " + maskExpr(dataWidth) + ") << " + lsb + "))");
                }
                case Opcode::SliceDynamic:
                    return resultAssign("slice_value(" + valueExpr(state, operands[0]) + ", " +
                                        valueExpr(state, operands[1]) + ", " +
                                        std::to_string(variableType(state, results.front()).bitWidth) + ")");
                case Opcode::SliceArray:
                    return resultAssign("slice_array_value(" + valueExpr(state, operands[0]) + ", " +
                                        valueExpr(state, operands[1]) + ", " +
                                        std::to_string(variableType(state, results.front()).bitWidth) + ", " +
                                        std::to_string(variableType(state, operands[0]).bitWidth) + ")");
                case Opcode::ChangedAny:
                case Opcode::ChangedPos:
                case Opcode::ChangedNeg:
                {
                    return changedResultAssignPrefix(state, results.front()) +
                           narrowChangedEventExpr(state, opcode, operands[0], operands[1]) +
                           ");\n" +
                           valueExpr(state, operands[1]) + " = " +
                           valueExpr(state, operands[0]) + ";\n";
                }
                case Opcode::RegisterWrite:
                case Opcode::RegisterWriteCond:
                case Opcode::RegisterWriteMask:
                case Opcode::RegisterWriteCondMask:
                case Opcode::LatchWrite:
                case Opcode::LatchWriteCond:
                case Opcode::LatchWriteMask:
                case Opcode::LatchWriteCondMask:
                {
                    const StateWriteLayout layout = stateWriteLayout(opcode);
                    const VariableId target = operands[layout.targetIndex];
                    const VariableId data = operands[layout.dataIndex];
                    const uint32_t width = variableType(state, target).bitWidth;
                    std::string nextValue =
                        valueExpr(state, data) + " & " + maskExpr(width);
                    if (layout.hasMask)
                    {
                        // Commit-local bit mix: (cur & ~mask) | (data & mask);
                        // a constant mask folds to an immediate.
                        const VariableId mask = operands[layout.hasCond ? 1 : 0];
                        std::string maskText;
                        if (const std::optional<uint64_t> constant =
                                scalarConstantWord(state, mask))
                        {
                            maskText = wordMaskLiteral(*constant);
                        }
                        else
                        {
                            maskText = valueExpr(state, mask);
                        }
                        nextValue = "(" + valueExpr(state, target) + " & ~(" + maskText +
                                    ")) | ((" + nextValue + ") & (" + maskText + "))";
                    }
                    std::string body;
                    if (isRegisterWriteOpcode(opcode) && state.scalarWriteRaise >= 0)
                    {
                        // ST00013: compare at the write point and store only
                        // on a real change (the legacy write-point detection
                        // idiom); the tail detector reads the raise flag.
                        const std::string next =
                            "wrNext_" + std::to_string(instruction.value);
                        body = "{ const auto " + next + " = " + nextValue + ";\nif (" + next +
                               " != " + valueExpr(state, target) + ") { " +
                               valueExpr(state, target) + " = " + next + "; " +
                               scalarWatchFlagExpr(
                                   state, static_cast<uint32_t>(state.scalarWriteRaise)) +
                               " = true; " + commitInputDirtyMarks(state, instruction) +
                               "} }\n";
                    }
                    else
                    {
                        body = valueExpr(state, target) + " = " + nextValue + ";\n";
                    }
                    if (layout.hasCond)
                    {
                        return "if (" + boolExpr(state, operands[0]) + ") { " + body + "}\n";
                    }
                    return body;
                }
                case Opcode::MemoryRead:
                {
                    const Type &memoryType = variableType(state, operands[0]);
                    const Type &addressType = variableType(state, operands[1]);
                    const Type &resultType = variableType(state, results.front());
                    const uint32_t stride = variableStorage(state, operands[0]).wordCount;
                    const std::string suffix = std::to_string(instruction.value);
                    const std::string index = "memory_index_" + suffix;
                    const std::string row = wordDataExpr(state, operands[0]) + " + " + index +
                                            " * " + std::to_string(stride);
                    std::string code = "{ const std::size_t " + index + " = index_words(" +
                                       wordDataExpr(state, operands[1]) + ", " +
                                       std::to_string(addressType.bitWidth) + ", " +
                                       std::to_string(memoryType.elementCount) + ");\n";
                    code += "if (" + index + " == " +
                            std::to_string(memoryType.elementCount) + ") { ";
                    if (resultType.bitWidth <= 64)
                    {
                        code += valueExpr(state, results.front()) + " = 0; } else { " +
                                valueExpr(state, results.front()) + " = (" + row + ")[0] & " +
                                maskExpr(resultType.bitWidth) + "; }\n";
                    }
                    else
                    {
                        code += "zero_words(" + wordDataExpr(state, results.front()) + ", " +
                                std::to_string(resultType.bitWidth) + "); } else { assign_words(" +
                                wordDataExpr(state, results.front()) + ", " +
                                std::to_string(resultType.bitWidth) + ", " + row + ", " +
                                std::to_string(memoryType.bitWidth) + ", false); }\n";
                    }
                    code += "}\n";
                    return code;
                }
                case Opcode::MemoryWrite:
                case Opcode::MemoryWriteCond:
                case Opcode::MemoryWriteMask:
                case Opcode::MemoryWriteCondMask:
                {
                    const StateWriteLayout layout = stateWriteLayout(opcode);
                    const std::size_t addrIndex = layout.hasCond ? 1 : 0;
                    const VariableId target = operands[layout.targetIndex];
                    const VariableId data = operands[layout.dataIndex];
                    const Type &memoryType = variableType(state, target);
                    const Type &addressType = variableType(state, operands[addrIndex]);
                    const uint32_t stride = variableStorage(state, target).wordCount;
                    const std::string suffix = std::to_string(instruction.value);
                    const std::string index = "memory_index_" + suffix;
                    std::string code = "{ const std::size_t " + index + " = index_words(" +
                                       wordDataExpr(state, operands[addrIndex]) + ", " +
                                       std::to_string(addressType.bitWidth) + ", " +
                                       std::to_string(memoryType.elementCount) + ");\n";
                    std::string condition =
                        index + " != " + std::to_string(memoryType.elementCount);
                    if (layout.hasCond)
                    {
                        condition = boolExpr(state, operands[0]) + " && " + condition;
                    }
                    const std::string row = wordDataExpr(state, target) + " + " + index +
                                            " * " + std::to_string(stride);
                    std::string body;
                    if (layout.hasMask)
                    {
                        const VariableId mask = operands[addrIndex + 1];
                        std::string flag;
                        if (state.arrayWriteAccum >= 0)
                        {
                            // ST00011: fuse reader-activation change detection
                            // into the element write (exact: only a real element
                            // change raises the flag).
                            flag = arrayWatchAccumExpr(
                                state, static_cast<uint32_t>(state.arrayWriteAccum));
                        }
                        body += emitMaskedWriteWords(state, row, wordDataExpr(state, data), mask,
                                                     memoryType.bitWidth, flag,
                                                     "write_mask_" + suffix);
                    }
                    else
                    {
                        // No mask: overwrite the whole element without reading
                        // the old one.
                        const std::string writeArgs =
                            row + ", " + std::to_string(memoryType.bitWidth) + ", " +
                            wordDataExpr(state, data) + ", " +
                            std::to_string(memoryType.bitWidth) + ", false)";
                        if (state.arrayWriteAccum >= 0)
                        {
                            // ST00011: same write-point change fusion as the
                            // masked form, at whole-element granularity.
                            body = arrayWatchAccumExpr(
                                       state, static_cast<uint32_t>(state.arrayWriteAccum)) +
                                   " |= assign_words_detect(" + writeArgs + ";";
                        }
                        else
                        {
                            body = "assign_words(" + writeArgs + ";";
                        }
                    }
                    code += "if (" + condition + ") { " + body + " }\n";
                    code += "}\n";
                    return code;
                }
                case Opcode::MemoryReadAll:
                {
                    const Type &memoryType = variableType(state, operands[0]);
                    const Type &resultType = variableType(state, results.front());
                    return "array_readall_pack(" + wordDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands[0]) + ", " +
                           std::to_string(memoryType.bitWidth) + ", " +
                           std::to_string(memoryType.elementCount) + ");\n";
                }
                case Opcode::MemoryWriteLanes:
                {
                    const Type &memoryType = variableType(state, operands[2]);
                    const Type &laneMaskType = variableType(state, operands[0]);
                    const std::string scatterArgs =
                        wordDataExpr(state, operands[2]) + ", " +
                        wordDataExpr(state, operands[0]) + ", " +
                        wordDataExpr(state, operands[1]) + ", " +
                        std::to_string(memoryType.bitWidth) + ", " +
                        std::to_string(memoryType.elementCount) + ")";
                    std::string body;
                    if (state.arrayWriteAccum >= 0)
                    {
                        // ST00011: same write-point change fusion as
                        // mem.write, at whole-lane granularity.
                        body = arrayWatchAccumExpr(
                                   state, static_cast<uint32_t>(state.arrayWriteAccum)) +
                               " |= array_write_scatter_detect(" + scatterArgs + ";";
                    }
                    else
                    {
                        body = "array_write_scatter(" + scatterArgs + ";";
                    }
                    // The lane mask already carries the per-lane write
                    // enable; skip the scatter entirely when it is empty.
                    return "if (any_words(" + wordDataExpr(state, operands[0]) + ", " +
                           std::to_string(laneMaskType.bitWidth) + ")) { " + body + " }\n";
                }
                case Opcode::MemoryFill:
                {
                    // The data operand is the full packed image with the fill
                    // condition already folded in by the lowering.
                    const Type &memoryType = variableType(state, operands[1]);
                    const Type &dataType = variableType(state, operands[0]);
                    const uint32_t stride = variableStorage(state, operands[1]).wordCount;
                    const std::string suffix = std::to_string(instruction.value);
                    const std::string element = "memory_element_" + suffix;
                    std::string body = "for (std::size_t " + element + " = 0; " + element +
                                       " < " + std::to_string(memoryType.elementCount) + "; ++" +
                                       element + ") { ";
                    const std::string target = wordDataExpr(state, operands[1]) + " + " + element +
                                               " * " + std::to_string(stride);
                    std::string elementWrite;
                    if (state.arrayWriteAccum >= 0)
                    {
                        // ST00011: a re-fill of identical values must not
                        // re-activate readers (eval convergence), so the flag
                        // tracks real per-element change inside the loop.
                        const std::string accum = arrayWatchAccumExpr(
                            state, static_cast<uint32_t>(state.arrayWriteAccum));
                        elementWrite =
                            accum + " |= slice_words_detect(" + target + ", " +
                            std::to_string(memoryType.bitWidth) + ", " +
                            wordDataExpr(state, operands[0]) + ", " +
                            std::to_string(dataType.bitWidth) + ", " + element +
                            " * " + std::to_string(memoryType.bitWidth) + ");";
                    }
                    else
                    {
                        elementWrite = "slice_words(" + target + ", " +
                                       std::to_string(memoryType.bitWidth) + ", " +
                                       wordDataExpr(state, operands[0]) + ", " +
                                       std::to_string(dataType.bitWidth) + ", " + element +
                                       " * " + std::to_string(memoryType.bitWidth) + ");";
                    }
                    body += elementWrite + " }";
                    return body + "\n";
                }
                case Opcode::ActForward:
                case Opcode::ActBackward:
                {
                    const auto attributes = state.program.activationAttributes(instruction);
                    const bool forward = opcode == Opcode::ActForward;
                    // Act targets span compute and commit Blocks, so activation
                    // is a constant-mask OR into the single active bitmap. A
                    // fired ActBackward also flags that another round is
                    // needed. While emitting inside a byte-chunk scan,
                    // same-byte forward targets owned by the current chunk
                    // relay into the scan-local byteFlags instead (strictly
                    // forward, so the bit is still pending in this chunk's
                    // ascending test sequence).
                    std::map<uint32_t, uint64_t> masks;
                    uint8_t relayMask = 0;
                    if (!splitActivationTargets(state, forward, attributes->targets,
                                                relayMask, masks, error))
                    {
                        return std::nullopt;
                    }
                    return emitActivationMerge(state, forward, relayMask, masks,
                                               attributes->targets.size(),
                                               boolExpr(state, operands.front()),
                                               /*allowBranchlessRelay=*/false);
                }
                case Opcode::SystemFunction:
                    error = "unsupported opcode in the initial AM C++ emitter: " +
                            std::string(toString(opcode));
                    return std::nullopt;
                case Opcode::SystemTask:
                    return emitSystemTaskInstruction(state, instruction, false, error);
                case Opcode::DpiCall:
                    return emitDpiCallInstruction(state, instruction, error);
            }
            error = "unknown AM opcode";
            return std::nullopt;
        }

        bool writeFile(const std::filesystem::path &path,
                       std::string_view contents,
                       uint64_t limit,
                       wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            if (contents.size() > limit)
            {
                diagnostics.error("generated artifact exceeds maxOutputFileBytes: " + path.string(),
                                  std::string(kContext));
                return false;
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                diagnostics.error("failed to open generated artifact: " + path.string(),
                                  std::string(kContext));
                return false;
            }
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!output)
            {
                diagnostics.error("failed to write generated artifact: " + path.string(),
                                  std::string(kContext));
                return false;
            }
            return true;
        }

        const EmitState::DetectorGroupPlan *detectorPlanFor(const EmitState &state,
                                                            std::size_t blockIndex)
        {
            if (state.blockDetectorPlans.empty())
            {
                return nullptr;
            }
            const auto &slot = state.blockDetectorPlans[blockIndex];
            return slot ? &*slot : nullptr;
        }

        const EmitState::ArrayWatchPlan *arrayWatchPlanFor(const EmitState &state,
                                                           std::size_t blockIndex)
        {
            if (state.blockArrayWatchPlans.empty())
            {
                return nullptr;
            }
            const auto &slot = state.blockArrayWatchPlans[blockIndex];
            return slot ? &*slot : nullptr;
        }

        const EmitState::ScalarWatchPlan *scalarWatchPlanFor(const EmitState &state,
                                                             std::size_t blockIndex)
        {
            if (state.blockScalarWatchPlans.empty())
            {
                return nullptr;
            }
            const auto &slot = state.blockScalarWatchPlans[blockIndex];
            return slot ? &*slot : nullptr;
        }

        // ST00013: block-local write-point flag declarations (one per fused
        // scalar detector), reset at every Block execution entry.
        std::string scalarWatchDeclarations(const EmitState::ScalarWatchPlan *plan)
        {
            if (plan == nullptr || plan->flagCount == 0)
            {
                return {};
            }
            std::string code = "bool ";
            for (uint32_t flag = 0; flag < plan->flagCount; ++flag)
            {
                if (flag != 0)
                {
                    code += ", ";
                }
                code += "wrChg_" + std::to_string(flag) + " = false";
            }
            return code + ";\n";
        }

        // ST00011: block-local change-flag declarations (one per replaced
        // array detector), reset at every Block execution entry.
        std::string arrayWatchDeclarations(const EmitState::ArrayWatchPlan *plan)
        {
            if (plan == nullptr || plan->accumCount == 0)
            {
                return {};
            }
            std::string code = "bool ";
            for (uint32_t accum = 0; accum < plan->accumCount; ++accum)
            {
                if (accum != 0)
                {
                    code += ", ";
                }
                code += "arrChg_" + std::to_string(accum) + " = false";
            }
            return code + ";\n";
        }

        // Oversized-Block chunking: a chunked Block's local values live in
        // parent-scope arrays (uninitialized, exactly like the inline
        // per-value locals) shared with the chunk functions through pointer
        // parameters. NO0016: one array per non-empty storage class
        // (localblk8_<id>[...], ...; class 3 keeps the legacy localblk_<id>
        // spelling). Requires beginLocalityBlock + activeChunkedBlock.
        std::string chunkedLocalValueDeclarations(const EmitState &state)
        {
            if (state.activeLocalityDeclarations.empty())
            {
                return {};
            }
            std::string code;
            for (uint8_t storageClass = 0; storageClass < 4; ++storageClass)
            {
                const uint32_t count = state.activeLocalityClassCounts[storageClass];
                if (count == 0)
                {
                    continue;
                }
                code += localClassCppType(storageClass);
                code += " localblk";
                code += localClassInfix(storageClass);
                code += "_" + std::to_string(state.activeChunkedBlock) + "[" +
                        std::to_string(count) + "];\n";
            }
            return code;
        }

        // Chunked-form watch flags: same reset-at-Block-entry semantics as
        // the inline declarations, as a zero-initialized parent-scope array.
        std::string chunkedScalarWatchDeclarations(const EmitState &state,
                                                   const EmitState::ScalarWatchPlan *plan)
        {
            if (plan == nullptr || plan->flagCount == 0)
            {
                return {};
            }
            return "bool wrChgblk_" + std::to_string(state.activeChunkedBlock) + "[" +
                   std::to_string(plan->flagCount) + "] = {};\n";
        }

        std::string chunkedArrayWatchDeclarations(const EmitState &state,
                                                  const EmitState::ArrayWatchPlan *plan)
        {
            if (plan == nullptr || plan->accumCount == 0)
            {
                return {};
            }
            return "bool arrChgblk_" + std::to_string(state.activeChunkedBlock) + "[" +
                   std::to_string(plan->accumCount) + "] = {};\n";
        }

        // Chunked-form detector-group flags: the inline form declares each
        // flag at its first accumulate; the chunks share one zero-initialized
        // parent-scope array instead.
        std::string chunkedDetectorGroupDeclarations(const EmitState &state,
                                                     const EmitState::DetectorGroupPlan *plan)
        {
            if (plan == nullptr || plan->groups.empty())
            {
                return {};
            }
            return "bool detGrpblk_" + std::to_string(state.activeChunkedBlock) + "[" +
                   std::to_string(plan->groups.size()) + "] = {};\n";
        }

        // ST00010: emits the branchless accumulate for one folded detector:
        // the change comparison feeds the group flag(s) and the detector's
        // private old baseline is updated unconditionally, exactly as the
        // un-folded form did.
        std::optional<std::string>
        emitDetectorAccumulate(const EmitState &state, InstructionId instruction,
                               const std::vector<uint32_t> &groups,
                               std::vector<uint8_t> &groupDeclared, std::string &error)
        {
            const Opcode opcode = state.program.opcode(instruction);
            const auto operands = state.program.operands(instruction);
            if (!isDetectorChangedOpcode(opcode) || operands.size() < 2 || groups.empty())
            {
                error = "invalid folded detector instruction";
                return std::nullopt;
            }
            const std::string event =
                narrowChangedEventExpr(state, opcode, operands[0], operands[1]);
            std::string code;
            // First-use declaration state: the inline form declares the
            // block-local flag at its first accumulate; the chunked form
            // references the parent-scope array element instead (declared and
            // zero-initialized with the chunk calls).
            const auto groupRef = [&](uint32_t group, bool &declared) {
                declared = groupDeclared[group] != 0;
                std::string reference;
                if (!declared && !chunkedBlockNaming(state))
                {
                    reference = "bool ";
                }
                reference += detectorGroupExpr(state, group);
                groupDeclared[group] = 1;
                return reference;
            };
            if (groups.size() == 1)
            {
                bool declared = false;
                code += groupRef(groups.front(), declared);
                code += declared ? " |= (" : " = (";
                code += event + ");\n";
            }
            else
            {
                const std::string temporary = "detEv_" + std::to_string(instruction.value);
                code += "const bool " + temporary + " = (" + event + ");\n";
                for (const uint32_t group : groups)
                {
                    bool declared = false;
                    code += groupRef(group, declared);
                    code += declared ? " |= " : " = ";
                    code += temporary + ";\n";
                }
            }
            code += valueExpr(state, operands[1]) + " = " + valueExpr(state, operands[0]) +
                    ";\n";
            return code;
        }

        // ST00010: emits the single merged activation for one detector group.
        std::optional<std::string>
        emitDetectorGroupMerge(const EmitState &state,
                               const EmitState::DetectorGroupPlan &plan, uint32_t groupId,
                               std::string &error)
        {
            const EmitState::DetectorGroupPlan::Group &group = plan.groups[groupId];
            std::map<uint32_t, uint64_t> masks;
            uint8_t relayMask = 0;
            if (!splitActivationTargets(state, group.forward, group.targets, relayMask,
                                        masks, error))
            {
                return std::nullopt;
            }
            return emitActivationMerge(state, group.forward, relayMask, masks,
                                       group.originalTargetCount,
                                       detectorGroupExpr(state, groupId),
                                       /*allowBranchlessRelay=*/true);
        }

        // Emits all code hanging off one block instruction position: the
        // instruction itself (or its folded replacement), plus any
        // detector-group merges anchored after it (ST00010). arrayPlan
        // (ST00011) rewrites planned array write sites and tail detectors
        // through the mutable EmitState context consumed by emitInstruction.
        std::optional<std::string>
        emitBlockPositionCode(const EmitState &state,
                              const EmitState::DetectorGroupPlan *plan,
                              const EmitState::ArrayWatchPlan *arrayPlan,
                              const EmitState::ScalarWatchPlan *scalarPlan,
                              InstructionId instruction, uint32_t index,
                              std::vector<uint8_t> &groupDeclared, std::string &error)
        {
            std::string code;
            if (plan != nullptr && plan->skippedActs.count(index) != 0)
            {
                // Replaced by the group merge anchored at the run end.
            }
            else if (scalarPlan != nullptr && scalarPlan->detectorRaise.count(index) != 0)
            {
                // ST00013: the flag raised at the Block's own write sites
                // feeds the group accumulator (folded) or the event variable.
                const uint32_t flag = scalarPlan->detectorRaise.at(index);
                const std::string flagExpr = scalarWatchFlagExpr(state, flag);
                if (plan != nullptr && plan->accumGroups.count(index) != 0)
                {
                    for (const uint32_t group : plan->accumGroups.at(index))
                    {
                        const bool declared = groupDeclared[group] != 0;
                        if (!declared && !chunkedBlockNaming(state))
                        {
                            code += "bool ";
                        }
                        code += detectorGroupExpr(state, group);
                        code += declared ? " |= " : " = ";
                        code += flagExpr + ";\n";
                        groupDeclared[group] = 1;
                    }
                }
                else
                {
                    const auto results = state.program.results(instruction);
                    code += changedResultAssignPrefix(state, results.front()) + flagExpr +
                            ");\n";
                }
            }
            else if (plan != nullptr && plan->accumGroups.count(index) != 0)
            {
                std::optional<std::string> accum = emitDetectorAccumulate(
                    state, instruction, plan->accumGroups.at(index), groupDeclared, error);
                if (!accum)
                {
                    return std::nullopt;
                }
                code += *accum;
            }
            else
            {
                if (arrayPlan != nullptr)
                {
                    const auto write = arrayPlan->writeAccum.find(index);
                    if (write != arrayPlan->writeAccum.end())
                    {
                        state.arrayWriteAccum = static_cast<int32_t>(write->second);
                    }
                    const auto detector = arrayPlan->detectorAccum.find(index);
                    if (detector != arrayPlan->detectorAccum.end())
                    {
                        state.arrayDetectorAccum =
                            static_cast<int32_t>(detector->second);
                    }
                }
                if (scalarPlan != nullptr)
                {
                    const auto write = scalarPlan->writeRaise.find(index);
                    if (write != scalarPlan->writeRaise.end())
                    {
                        state.scalarWriteRaise = static_cast<int32_t>(write->second);
                    }
                }
                std::optional<std::string> emitted =
                    emitInstruction(state, instruction, error);
                state.arrayWriteAccum = -1;
                state.arrayDetectorAccum = -1;
                state.scalarWriteRaise = -1;
                if (!emitted)
                {
                    return std::nullopt;
                }
                code += *emitted;
            }
            if (plan != nullptr)
            {
                const auto merges = plan->mergesAfter.find(index);
                if (merges != plan->mergesAfter.end())
                {
                    for (const uint32_t groupId : merges->second)
                    {
                        std::optional<std::string> merge =
                            emitDetectorGroupMerge(state, *plan, groupId, error);
                        if (!merge)
                        {
                            return std::nullopt;
                        }
                        code += *merge;
                    }
                }
            }
            return code;
        }

        std::optional<std::size_t>
        parseBlocksPerSource(const GrhSimAmCppOptions &options,
                             wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            constexpr std::size_t kDefaultBlocksPerSource = 2048;
            const auto attribute = options.attributes.find("blocksPerSource");
            if (attribute == options.attributes.end())
            {
                return kDefaultBlocksPerSource;
            }

            std::size_t value = 0;
            const char *const begin = attribute->second.data();
            const char *const end = begin + attribute->second.size();
            const auto [parsedEnd, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsedEnd != end || value == 0)
            {
                diagnostics.error(
                    "AM C++ emitter blocksPerSource must be a positive integer: " +
                        attribute->second,
                    std::string(kContext));
                return std::nullopt;
            }
            return value;
        }

        std::optional<uint64_t>
        parseMaxSourceBytes(const GrhSimAmCppOptions &options,
                            wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            constexpr uint64_t kDefaultMaxSourceBytes =
                UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024);
            const auto attribute = options.attributes.find("maxSourceBytes");
            if (attribute == options.attributes.end())
            {
                return kDefaultMaxSourceBytes;
            }

            uint64_t value = 0;
            const char *const begin = attribute->second.data();
            const char *const end = begin + attribute->second.size();
            const auto [parsedEnd, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsedEnd != end || value == 0)
            {
                diagnostics.error(
                    "AM C++ emitter maxSourceBytes must be a positive integer: " +
                        attribute->second,
                    std::string(kContext));
                return std::nullopt;
            }
            return value;
        }

        // Commit Blocks are unconditionally inlined into their part's
        // eval_commit_* function and are individually dense (thousands of
        // masked writes each), so the 4 MiB source budget yields single
        // functions of tens of thousands of lines — deep in the superlinear
        // regime of the optimizer (SLP/GVN/JumpThreading; measured 470 s and
        // 13 GB RSS for one 58k-line function). The commit budget keeps each
        // eval_commit_* function small; every part is an independent TU, so
        // this also restores build parallelism across the commit shard.
        std::optional<uint64_t>
        parseMaxCommitSourceBytes(const GrhSimAmCppOptions &options,
                                  wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            constexpr uint64_t kDefaultMaxCommitSourceBytes =
                UINT64_C(512) * UINT64_C(1024);
            const auto attribute = options.attributes.find("maxCommitSourceBytes");
            if (attribute == options.attributes.end())
            {
                return kDefaultMaxCommitSourceBytes;
            }

            uint64_t value = 0;
            const char *const begin = attribute->second.data();
            const char *const end = begin + attribute->second.size();
            const auto [parsedEnd, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsedEnd != end || value == 0)
            {
                diagnostics.error(
                    "AM C++ emitter maxCommitSourceBytes must be a positive integer: " +
                        attribute->second,
                    std::string(kContext));
                return std::nullopt;
            }
            return value;
        }

        // Oversized-Block chunking threshold: a Block whose instruction count
        // exceeds this budget emits its stream as block_<id>_chunk_<k>()
        // member calls instead of inline code, keeping every generated
        // function small enough for the C++ optimizer (one 145k-instruction
        // XiangShan Block produced a single 106k-line function that neither
        // clang -O1 nor -O3 could compile).
        std::optional<std::size_t>
        parseBlockChunkInstructions(const GrhSimAmCppOptions &options,
                                    wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            constexpr std::size_t kDefaultBlockChunkInstructions = 3000;
            const auto attribute = options.attributes.find("blockChunkInstructions");
            if (attribute == options.attributes.end())
            {
                return kDefaultBlockChunkInstructions;
            }

            std::size_t value = 0;
            const char *const begin = attribute->second.data();
            const char *const end = begin + attribute->second.size();
            const auto [parsedEnd, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsedEnd != end || value == 0)
            {
                diagnostics.error(
                    "AM C++ emitter blockChunkInstructions must be a positive integer: " +
                        attribute->second,
                    std::string(kContext));
                return std::nullopt;
            }
            return value;
        }

        bool finishWrittenFile(std::ofstream &output,
                               const std::filesystem::path &path,
                               uint64_t limit,
                               wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            output.flush();
            const std::streamoff byteCount = output.tellp();
            if (!output || byteCount < 0)
            {
                diagnostics.error("failed to write generated artifact: " + path.string(),
                                  std::string(kContext));
                return false;
            }
            output.close();
            if (static_cast<uint64_t>(byteCount) > limit)
            {
                diagnostics.error("generated artifact exceeds maxOutputFileBytes: " +
                                      path.string(),
                                  std::string(kContext));
                return false;
            }
            return true;
        }

        void writeIndentedLines(std::ostream &output,
                                std::string_view contents,
                                std::string_view indentation)
        {
            std::istringstream lines{std::string(contents)};
            std::string line;
            while (std::getline(lines, line))
            {
                output << indentation << line << '\n';
            }
        }

        std::string scanSourceFunctionName(std::size_t sourceIndex,
                                           std::size_t partIndex)
        {
            std::string name = "eval_scan_" + std::to_string(sourceIndex);
            if (partIndex != 0)
            {
                name += "_part_" + std::to_string(partIndex);
            }
            return name;
        }

        std::string commitSourceFunctionName(std::size_t sourceIndex,
                                             std::size_t partIndex)
        {
            std::string name = "eval_commit_" + std::to_string(sourceIndex);
            if (partIndex != 0)
            {
                name += "_part_" + std::to_string(partIndex);
            }
            return name;
        }

        std::string blockSourceFilename(std::string_view prefix,
                                        std::size_t sourceIndex,
                                        std::size_t partIndex)
        {
            std::string name = std::string(prefix) + "_blocks_" +
                               std::to_string(sourceIndex);
            if (partIndex != 0)
            {
                name += "_part_" + std::to_string(partIndex);
            }
            return name + ".cpp";
        }

        // Compute Blocks are 1..computeEnd-1 with
        // computeEnd = commitBlockBegin != 0 ? commitBlockBegin : blockCount;
        // commit Blocks are the suffix [commitBlockBegin, commitBlockEnd) when
        // commitBlockBegin != 0. Both helpers clip a part's [firstBlock,
        // endBlock) span to the respective phase range.
        std::pair<std::size_t, std::size_t>
        computeBlockRange(std::size_t firstBlock,
                          std::size_t endBlock,
                          std::size_t blockCount,
                          uint32_t commitBlockBegin)
        {
            const std::size_t computeEnd =
                commitBlockBegin != 0 ? commitBlockBegin : blockCount;
            const std::size_t lo = std::max(firstBlock, static_cast<std::size_t>(1));
            const std::size_t hi = std::min(endBlock, computeEnd);
            return {lo, std::max(lo, hi)};
        }

        std::pair<std::size_t, std::size_t>
        commitBlockRange(std::size_t firstBlock,
                         std::size_t endBlock,
                         uint32_t commitBlockBegin,
                         uint32_t commitBlockEnd)
        {
            if (commitBlockBegin == 0)
            {
                return {0, 0};
            }
            const std::size_t lo =
                std::max(firstBlock, static_cast<std::size_t>(commitBlockBegin));
            const std::size_t hi =
                std::min(endBlock, static_cast<std::size_t>(commitBlockEnd));
            return {lo, std::max(lo, hi)};
        }

        // One 8-Block byte segment of a scan range owned by a single part.
        // Parts may split inside a byte (maxSourceBytes), so each chunk
        // carries the mask of byte bits it dispatches; bits owned by a
        // sibling chunk are left untouched in the global byte for that
        // chunk's later pass.
        struct ScanByteChunk
        {
            std::size_t byteIndex = 0;
            std::size_t firstBlock = 0;
            std::size_t endBlock = 0;
            uint8_t ownedMask = 0;
        };

        std::vector<ScanByteChunk> scanByteChunks(std::size_t rangeLo,
                                                  std::size_t rangeHi)
        {
            std::vector<ScanByteChunk> chunks;
            for (std::size_t byte = rangeLo / 8U; byte <= (rangeHi - 1U) / 8U; ++byte)
            {
                const std::size_t first = std::max(rangeLo, byte * 8U);
                const std::size_t end = std::min(rangeHi, byte * 8U + 8U);
                uint8_t ownedMask = 0;
                for (std::size_t block = first; block < end; ++block)
                {
                    ownedMask = static_cast<uint8_t>(ownedMask | (1U << (block % 8U)));
                }
                chunks.push_back(ScanByteChunk{
                    .byteIndex = byte,
                    .firstBlock = first,
                    .endBlock = end,
                    .ownedMask = ownedMask,
                });
            }
            return chunks;
        }

        std::string blockSourceIncludes(std::string_view prefix)
        {
            return "#include \"" + std::string(prefix) + ".hpp\"\n" +
                   "#include \"" + std::string(prefix) + "_support.hpp\"\n";
        }

        // NO0006 trace comments (GrhSimAmCppOptions::traceComments): block
        // banners and per-atom provenance comment lines. atomKindName is a
        // local equivalent of the split stage's helper (deliberately not a
        // new cross-file helper).
        std::string_view atomKindName(AmAtomKind kind)
        {
            switch (kind)
            {
            case AmAtomKind::Singleton:
                return "Singleton";
            case AmAtomKind::Tree:
                return "Tree";
            case AmAtomKind::CombLoopScc:
                return "CombLoopScc";
            case AmAtomKind::CommitEvent:
                return "CommitEvent";
            }
            return "Unknown";
        }

        std::string_view blockTraceRole(const ExecutableModel &model, uint32_t blockIndex)
        {
            if (blockIndex == 0)
            {
                return "entry";
            }
            if (model.commitBlockBegin != 0 && blockIndex >= model.commitBlockBegin &&
                blockIndex < model.commitBlockEnd)
            {
                return "commit";
            }
            return "compute";
        }

        std::string blockTraceBanner(const ExecutableModel &model, uint32_t blockIndex)
        {
            const BlockId block{blockIndex};
            return "// ===== block " + std::to_string(blockIndex) +
                   " role=" + std::string(blockTraceRole(model, blockIndex)) +
                   " atoms=" + std::to_string(model.program.blockAtomCount(block)) +
                   " instrs=" + std::to_string(model.program.blockSize(block)) +
                   " =====\n";
        }

        std::string atomTraceComment(const ScheduledProgram &program, AtomId atom)
        {
            return "// --- atom " + std::to_string(atom.value) +
                   " kind=" + std::string(atomKindName(program.atomKind(atom))) +
                   " gsim_node=" + std::to_string(program.atomGsimNodeId(atom)) +
                   " ---\n";
        }

        // Atoms tile their Block's flat instruction range contiguously: the
        // boundary list maps an atom's block-local start position to its id.
        struct AtomTraceBoundary
        {
            uint32_t position = 0;
            AtomId atom;
        };

        std::vector<AtomTraceBoundary> blockAtomTraceBoundaries(
            const ScheduledProgram &program, BlockId block)
        {
            std::vector<AtomTraceBoundary> boundaries;
            boundaries.reserve(program.blockAtomCount(block));
            uint32_t position = 0;
            for (std::size_t index = 0; index < program.blockAtomCount(block); ++index)
            {
                const AtomId atom = program.blockAtom(block, index);
                boundaries.push_back(AtomTraceBoundary{
                    .position = position,
                    .atom = atom,
                });
                position += static_cast<uint32_t>(program.atomInstructionCount(atom));
            }
            return boundaries;
        }

        // One boundary position of the per-atom trace comments. Atoms covered
        // by a fused mux run carry their code at the run head's position, so
        // the head emits every covered atom's comment and the later covered
        // boundaries emit nothing.
        std::string atomTraceCommentsAt(const EmitState &state,
                                        const ScheduledProgram &program, AtomId atom,
                                        InstructionId instruction)
        {
            const int32_t runId =
                instruction.value < state.instructionMuxRun.size()
                    ? state.instructionMuxRun[instruction.value]
                    : -1;
            if (runId < 0)
            {
                return atomTraceComment(program, atom);
            }
            const EmitState::MuxRunPlan &plan =
                state.muxRunPlans[static_cast<std::size_t>(runId)];
            if (plan.head != instruction.value)
            {
                return {};
            }
            std::string comments;
            for (uint32_t covered = plan.firstAtom; covered <= plan.lastAtom; ++covered)
            {
                comments += atomTraceComment(program, AtomId{covered});
            }
            return comments;
        }

        std::string entryBlockSourcePrologue(std::string_view className)
        {
            return "\nvoid " + std::string(className) + "::execute_block_0() {\n";
        }

        // Runtime-profile counter for the entry Block: per-Block count plus the
        // NO0010 cycle pair (t0 here, accumulation in entryBlockProfileEpilogue).
        std::string entryBlockProfileLine()
        {
            return "    std::uint64_t profileBlockT0 = 0;\n"
                   "    if (runtimeProfileEnabled_) { profilePerBlockExecs_[0] += 1; profileBlockT0 = wolvrixAmRdtsc(); }\n";
        }

        std::string entryBlockProfileEpilogue()
        {
            return "    if (runtimeProfileEnabled_) { profilePerBlockCycles_[0] += wolvrixAmRdtsc() - profileBlockT0; }\n";
        }

        std::string scanSourcePrologue(std::string_view className,
                                       std::size_t sourceIndex,
                                       std::size_t partIndex)
        {
            return "\nvoid " + std::string(className) + "::" +
                   scanSourceFunctionName(sourceIndex, partIndex) + "() {\n";
        }

        std::string commitSourcePrologue(std::string_view className,
                                         std::size_t sourceIndex,
                                         std::size_t partIndex)
        {
            return "\nvoid " + std::string(className) + "::" +
                   commitSourceFunctionName(sourceIndex, partIndex) + "() {\n";
        }

        constexpr std::string_view kBlockSourceFunctionEpilogue = "}\n";

        // One byte chunk of the static compute scan: snapshot the activity
        // byte into a local, clear the owned bits from the global byte, then
        // consume the snapshot with straight-line ascending bit tests (the
        // legacy/GSIM batch idiom). Same-byte forward activations relay into
        // the local byteFlags, so they are picked up later in the same pass.
        // In full-evaluation mode the snapshot is forced to the owned mask, so
        // every owned block test passes unconditionally.
        std::string scanByteChunkPrologue(std::size_t byteIndex, uint8_t ownedMask,
                                          bool fullEvaluation)
        {
            if (fullEvaluation)
            {
                return "    {\n        std::uint8_t byteFlags = " +
                       byteMaskLiteral(ownedMask) +
                       ";\n        if (byteFlags != 0) {\n";
            }
            std::string text = "    {\n        std::uint8_t byteFlags = active_byte_ref(" +
                               std::to_string(byteIndex) + ")";
            if (ownedMask == 0xffU)
            {
                text += ";\n        if (byteFlags != 0) {\n            active_byte_ref(" +
                        std::to_string(byteIndex) + ") = 0;\n";
            }
            else
            {
                text += " & " + byteMaskLiteral(ownedMask) +
                        ";\n        if (byteFlags != 0) {\n            active_byte_ref(" +
                        std::to_string(byteIndex) + ") &= " +
                        byteMaskLiteral(static_cast<uint8_t>(~ownedMask)) + ";\n";
            }
            return text;
        }

        constexpr std::string_view kScanByteChunkEpilogue =
            "        }\n"
            "    }\n";

        std::string scanBlockTestPrologue(std::size_t blockIndex,
                                          bool runtimeProfile)
        {
            std::string text =
                "            if ((byteFlags & " +
                byteMaskLiteral(static_cast<uint8_t>(1U << (blockIndex % 8U))) +
                ") != 0) {\n";
            if (runtimeProfile)
            {
                // NO0010: the t0 local lives in this Block's if-scope (sibling
                // Blocks redeclare it in their own scopes) and is consumed by
                // scanBlockTestEpilogue, so the timed region is exactly the fired
                // Block body (including chunk calls on the split path).
                text += "                std::uint64_t profileBlockT0 = 0;\n";
                text += "                if (runtimeProfileEnabled_) { profilePerBlockExecs_[" +
                        std::to_string(blockIndex) +
                        "] += 1; ++profileBlockExecs_; profileBlockT0 = wolvrixAmRdtsc(); }\n";
            }
            return text;
        }

        std::string scanBlockTestEpilogue(std::size_t blockIndex,
                                          bool runtimeProfile)
        {
            std::string text;
            if (runtimeProfile)
            {
                text += "                if (runtimeProfileEnabled_) { profilePerBlockCycles_[" +
                        std::to_string(blockIndex) +
                        "] += wolvrixAmRdtsc() - profileBlockT0; }\n";
            }
            text += "            }\n";
            return text;
        }

        std::string commitBlockTestPrologue(std::size_t blockIndex,
                                            bool runtimeProfile)
        {
            // Commit Blocks are activation-filtered exactly like compute
            // Blocks: the byte-chunk scan above owns the bit snapshot/clear.
            std::string text =
                "            if ((byteFlags & " +
                byteMaskLiteral(static_cast<uint8_t>(1U << (blockIndex % 8U))) +
                ") != 0) {\n";
            if (runtimeProfile)
            {
                text += "                std::uint64_t profileBlockT0 = 0;\n";
                text += "                if (runtimeProfileEnabled_) { profilePerBlockExecs_[" +
                        std::to_string(blockIndex) +
                        "] += 1; ++profileCommitBlockExecs_; profileBlockT0 = wolvrixAmRdtsc(); }\n";
            }
            return text;
        }

        std::string blockChunkFunctionName(std::size_t blockIndex, std::size_t chunkIndex)
        {
            return "block_" + std::to_string(blockIndex) + "_chunk_" +
                   std::to_string(chunkIndex);
        }

        // Instruction ranges [first, end) covered by each chunk of an
        // oversized Block: at most chunkInstructions per chunk, never
        // splitting one instruction, with a forced boundary at the commit
        // gate head so the gate's "if" can wrap whole chunk calls at the call
        // site. Empty when the Block fits in one function.
        std::vector<std::pair<std::size_t, std::size_t>>
        blockChunkRanges(std::size_t blockSize,
                         std::size_t chunkInstructions,
                         std::size_t gateBoundary)
        {
            std::vector<std::pair<std::size_t, std::size_t>> ranges;
            if (blockSize <= chunkInstructions)
            {
                return ranges;
            }
            const auto append = [&](std::size_t first, std::size_t end) {
                for (std::size_t at = first; at < end; at += chunkInstructions)
                {
                    ranges.emplace_back(at, std::min(at + chunkInstructions, end));
                }
            };
            append(0, gateBoundary);
            append(gateBoundary, blockSize);
            return ranges;
        }

        // Pointer/reference parameters a chunked Block's chunk functions
        // share: one per non-empty parent-scope array, plus the scan
        // byte-flags relay for Blocks emitted inside a byte-chunk scan
        // (every Block except the entry Block).
        struct BlockChunkParams
        {
            // NO0016: one shared array per non-empty local storage class.
            std::array<bool, 4> locals{};
            bool scalarWatch = false;
            bool arrayWatch = false;
            bool detectorGroups = false;
            bool byteFlags = false;
        };

        BlockChunkParams blockChunkParamsFor(const EmitState &state,
                                             std::size_t blockIndex)
        {
            BlockChunkParams params;
            beginLocalityBlock(state, static_cast<uint32_t>(blockIndex));
            for (uint8_t storageClass = 0; storageClass < 4; ++storageClass)
            {
                params.locals[storageClass] =
                    state.activeLocalityClassCounts[storageClass] != 0;
            }
            endLocalityBlock(state);
            const EmitState::ScalarWatchPlan *scalarPlan =
                scalarWatchPlanFor(state, blockIndex);
            params.scalarWatch = scalarPlan != nullptr && scalarPlan->flagCount != 0;
            const EmitState::ArrayWatchPlan *arrayPlan =
                arrayWatchPlanFor(state, blockIndex);
            params.arrayWatch = arrayPlan != nullptr && arrayPlan->accumCount != 0;
            const EmitState::DetectorGroupPlan *detectorPlan =
                detectorPlanFor(state, blockIndex);
            params.detectorGroups =
                detectorPlan != nullptr && !detectorPlan->groups.empty();
            params.byteFlags = blockIndex != 0;
            return params;
        }

        // Parameter names carry the Block id because the emitted instruction
        // code addresses the arrays as localblk_<id>[k] and friends.
        std::string blockChunkParameterList(std::size_t blockIndex,
                                            const BlockChunkParams &params)
        {
            const std::string suffix = "_" + std::to_string(blockIndex);
            std::string list;
            const auto append = [&](std::string parameter) {
                if (!list.empty())
                {
                    list += ", ";
                }
                list += parameter;
            };
            // __restrict__ on every shared-array parameter: without it each
            // store through these pointers may-alias every member-variable
            // access in the chunk body, which sends GVN's memory-dependence
            // queries quadratic on the oversized Blocks (measured: -O3 did
            // not finish a 62k-line TU in 40 minutes; the parent-scope
            // arrays never alias member storage, so restrict is valid).
            if (params.locals[0])
            {
                append("std::uint8_t *__restrict__ localblk8" + suffix);
            }
            if (params.locals[1])
            {
                append("std::uint16_t *__restrict__ localblk16" + suffix);
            }
            if (params.locals[2])
            {
                append("std::uint32_t *__restrict__ localblk32" + suffix);
            }
            if (params.locals[3])
            {
                append("std::uint64_t *__restrict__ localblk" + suffix);
            }
            if (params.scalarWatch)
            {
                append("bool *__restrict__ wrChgblk" + suffix);
            }
            if (params.arrayWatch)
            {
                append("bool *__restrict__ arrChgblk" + suffix);
            }
            if (params.detectorGroups)
            {
                append("bool *__restrict__ detGrpblk" + suffix);
            }
            if (params.byteFlags)
            {
                append("std::uint8_t &__restrict__ byteFlags");
            }
            return list;
        }

        // Call arguments matching blockChunkParameterList: the parent-scope
        // arrays by name (each decays to the pointer parameter).
        std::string blockChunkArgumentList(std::size_t blockIndex,
                                           const BlockChunkParams &params)
        {
            const std::string suffix = "_" + std::to_string(blockIndex);
            std::string list;
            const auto append = [&](std::string argument) {
                if (!list.empty())
                {
                    list += ", ";
                }
                list += argument;
            };
            if (params.locals[0])
            {
                append("localblk8" + suffix);
            }
            if (params.locals[1])
            {
                append("localblk16" + suffix);
            }
            if (params.locals[2])
            {
                append("localblk32" + suffix);
            }
            if (params.locals[3])
            {
                append("localblk" + suffix);
            }
            if (params.scalarWatch)
            {
                append("wrChgblk" + suffix);
            }
            if (params.arrayWatch)
            {
                append("arrChgblk" + suffix);
            }
            if (params.detectorGroups)
            {
                append("detGrpblk" + suffix);
            }
            if (params.byteFlags)
            {
                append("byteFlags");
            }
            return list;
        }

        bool addByteCount(uint64_t &total, uint64_t increment)
        {
            if (increment > std::numeric_limits<uint64_t>::max() - total)
            {
                return false;
            }
            total += increment;
            return true;
        }

        std::optional<uint64_t>
        indentedLineByteCount(std::string_view contents,
                              std::string_view indentation)
        {
            uint64_t byteCount = 0;
            std::size_t lineBegin = 0;
            while (lineBegin < contents.size())
            {
                const std::size_t lineEnd = contents.find('\n', lineBegin);
                const std::size_t contentBytes =
                    lineEnd == std::string_view::npos
                        ? contents.size() - lineBegin
                        : lineEnd - lineBegin;
                const uint64_t lineBytes = static_cast<uint64_t>(indentation.size()) +
                                           static_cast<uint64_t>(contentBytes) + 1U;
                if (!addByteCount(byteCount, lineBytes))
                {
                    return std::nullopt;
                }
                if (lineEnd == std::string_view::npos)
                {
                    break;
                }
                lineBegin = lineEnd + 1U;
            }
            return byteCount;
        }

        std::optional<uint64_t>
        measureBlockBody(const ExecutableModel &model,
                         const EmitState &state,
                         std::size_t blockIndex,
                         std::string_view indentation,
                         wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            uint64_t byteCount = 0;
            beginLocalityBlock(state, static_cast<uint32_t>(blockIndex));
            const std::optional<uint64_t> declarationBytes =
                indentedLineByteCount(localValueDeclarations(state), indentation);
            if (!declarationBytes || !addByteCount(byteCount, *declarationBytes))
            {
                endLocalityBlock(state);
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            const EmitState::ArrayWatchPlan *arrayPlan =
                arrayWatchPlanFor(state, blockIndex);
            const std::optional<uint64_t> arrayWatchBytes = indentedLineByteCount(
                arrayWatchDeclarations(arrayPlan), indentation);
            if (!arrayWatchBytes || !addByteCount(byteCount, *arrayWatchBytes))
            {
                endLocalityBlock(state);
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            const EmitState::ScalarWatchPlan *scalarPlan =
                scalarWatchPlanFor(state, blockIndex);
            const std::optional<uint64_t> scalarWatchBytes = indentedLineByteCount(
                scalarWatchDeclarations(scalarPlan), indentation);
            if (!scalarWatchBytes || !addByteCount(byteCount, *scalarWatchBytes))
            {
                endLocalityBlock(state);
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            const BlockId block{static_cast<uint32_t>(blockIndex)};
            const EmitState::DetectorGroupPlan *detectorPlan =
                detectorPlanFor(state, blockIndex);
            std::vector<uint8_t> detectorGroupDeclared(
                detectorPlan != nullptr ? detectorPlan->groups.size() : 0, 0);
            if (state.traceComments)
            {
                const std::optional<uint64_t> bannerBytes = indentedLineByteCount(
                    blockTraceBanner(model, static_cast<uint32_t>(blockIndex)),
                    indentation);
                if (!bannerBytes || !addByteCount(byteCount, *bannerBytes))
                {
                    endLocalityBlock(state);
                    diagnostics.error("AM C++ emitter source size overflow: block=" +
                                          std::to_string(blockIndex),
                                      std::string(kContext));
                    return std::nullopt;
                }
            }
            const std::optional<uint64_t> dirtyMarkBytes = indentedLineByteCount(
                commitInputDirtyBlockMarks(
                    state, static_cast<uint32_t>(blockIndex)),
                indentation);
            if (!dirtyMarkBytes || !addByteCount(byteCount, *dirtyMarkBytes))
            {
                endLocalityBlock(state);
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            const std::vector<AtomTraceBoundary> atomBoundaries =
                state.traceComments ? blockAtomTraceBoundaries(model.program, block)
                                    : std::vector<AtomTraceBoundary>{};
            std::size_t atomBoundaryCursor = 0;
            for (std::size_t index = 0; index < model.program.blockSize(block); ++index)
            {
                const InstructionId instruction =
                    model.program.blockInstruction(block, index);
                if (state.traceComments)
                {
                    while (atomBoundaryCursor < atomBoundaries.size() &&
                           atomBoundaries[atomBoundaryCursor].position ==
                               static_cast<uint32_t>(index))
                    {
                        const std::optional<uint64_t> commentBytes =
                            indentedLineByteCount(
                                atomTraceCommentsAt(
                                    state, model.program,
                                    atomBoundaries[atomBoundaryCursor].atom,
                                    instruction),
                                indentation);
                        if (!commentBytes || !addByteCount(byteCount, *commentBytes))
                        {
                            endLocalityBlock(state);
                            diagnostics.error("AM C++ emitter source size overflow: block=" +
                                                  std::to_string(blockIndex),
                                              std::string(kContext));
                            return std::nullopt;
                        }
                        ++atomBoundaryCursor;
                    }
                }
                std::string error;
                const std::optional<std::string> code =
                    emitBlockPositionCode(state, detectorPlan, arrayPlan, scalarPlan,
                                          instruction,
                                          static_cast<uint32_t>(index),
                                          detectorGroupDeclared, error);
                if (!code)
                {
                    endLocalityBlock(state);
                    diagnostics.error(error + ": instruction=" +
                                          std::to_string(instruction.value),
                                      std::string(kContext));
                    return std::nullopt;
                }
                const std::optional<uint64_t> codeBytes =
                    indentedLineByteCount(*code, indentation);
                if (!codeBytes || !addByteCount(byteCount, *codeBytes))
                {
                    endLocalityBlock(state);
                    diagnostics.error("AM C++ emitter source size overflow: block=" +
                                          std::to_string(blockIndex),
                                      std::string(kContext));
                    return std::nullopt;
                }
            }
            endLocalityBlock(state);
            return byteCount;
        }

        // Scan-form compute Block: a straight-line bit test inside its byte
        // chunk. Empty Blocks (no instructions and no local declarations)
        // emit no test at all; the chunk-level byte clear still consumes
        // their activity bit.
        std::optional<uint64_t>
        measureScanBlockCase(const ExecutableModel &model,
                             const EmitState &state,
                             std::size_t blockIndex,
                             wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            // Planning-time relay context: forward targets in the same byte
            // above the source bit (the writer recomputes the exact owned
            // mask from the final part layout; splits only shave relays, so
            // the byte error is negligible).
            state.scanRelayByte = static_cast<int32_t>(blockIndex / 8U);
            state.scanRelayMask =
                static_cast<uint8_t>(0xfeU << (blockIndex % 8U));
            const std::optional<uint64_t> bodyBytes =
                measureBlockBody(model, state, blockIndex, "                ", diagnostics);
            state.scanRelayByte = -1;
            state.scanRelayMask = 0;
            if (!bodyBytes)
            {
                return std::nullopt;
            }
            if (*bodyBytes == 0)
            {
                return static_cast<uint64_t>(0);
            }
            uint64_t byteCount = static_cast<uint64_t>(
                scanBlockTestPrologue(blockIndex, state.runtimeProfile).size());
            const std::string &guardGate = state.blockGuardGate[blockIndex].expression;
            if (!guardGate.empty() &&
                (!addByteCount(byteCount,
                               static_cast<uint64_t>(16 + 4 + guardGate.size() + 4)) ||
                 !addByteCount(byteCount, static_cast<uint64_t>(16 + 2))))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            if (!addByteCount(byteCount, *bodyBytes) ||
                !addByteCount(byteCount,
                              scanBlockTestEpilogue(blockIndex, state.runtimeProfile).size()))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            return byteCount;
        }

        std::optional<uint64_t>
        measureCommitBlockCase(const ExecutableModel &model,
                               const EmitState &state,
                               std::size_t blockIndex,
                               wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const std::optional<uint64_t> bodyBytes =
                measureBlockBody(model, state, blockIndex, "                ", diagnostics);
            if (!bodyBytes)
            {
                return std::nullopt;
            }
            uint64_t byteCount = static_cast<uint64_t>(
                commitBlockTestPrologue(blockIndex, state.runtimeProfile).size());
            const auto &commitGate = state.blockCommitGate[blockIndex];
            if (commitGate.headCount != 0 &&
                (!addByteCount(byteCount,
                               static_cast<uint64_t>(12 + commitGate.expression.size() + 4)) ||
                 !addByteCount(byteCount, static_cast<uint64_t>(10))))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            if (!commitGate.preamble.empty())
            {
                const std::optional<uint64_t> preambleBytes =
                    indentedLineByteCount(commitGate.preamble, "                    ");
                if (!preambleBytes || !addByteCount(byteCount, *preambleBytes))
                {
                    diagnostics.error("AM C++ emitter source size overflow: block=" +
                                          std::to_string(blockIndex),
                                      std::string(kContext));
                    return std::nullopt;
                }
            }
            if (!addByteCount(byteCount, *bodyBytes) ||
                !addByteCount(byteCount,
                              scanBlockTestEpilogue(blockIndex, state.runtimeProfile).size()))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
                                  std::string(kContext));
                return std::nullopt;
            }
            return byteCount;
        }

        std::optional<uint64_t>
        measureEntryBlockCase(const ExecutableModel &model,
                              const EmitState &state,
                              wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const std::optional<uint64_t> bodyBytes =
                measureBlockBody(model, state, 0, "    ", diagnostics);
            if (!bodyBytes)
            {
                return std::nullopt;
            }
            uint64_t byteCount = 0;
            if (state.runtimeProfile &&
                (!addByteCount(byteCount,
                               static_cast<uint64_t>(entryBlockProfileLine().size())) ||
                 !addByteCount(byteCount,
                               static_cast<uint64_t>(entryBlockProfileEpilogue().size()))))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=0",
                                  std::string(kContext));
                return std::nullopt;
            }
            if (!addByteCount(byteCount, *bodyBytes))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=0",
                                  std::string(kContext));
                return std::nullopt;
            }
            return byteCount;
        }

        struct BlockSourcePart
        {
            std::size_t sourceIndex = 0;
            std::size_t partIndex = 0;
            std::size_t firstBlock = 0;
            std::size_t endBlock = 0;
        };

        using BlockSourcePlan = std::vector<std::vector<BlockSourcePart>>;

        std::optional<BlockSourcePlan>
        planBlockSources(const ExecutableModel &model,
                         const EmitState &state,
                         std::size_t blocksPerSource,
                         std::size_t sourceCount,
                         uint64_t maxSourceBytes,
                         uint64_t maxCommitSourceBytes,
                         std::string_view prefix,
                         std::string_view className,
                         wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            BlockSourcePlan plan(sourceCount);
            const std::size_t blockCount = model.program.blockCount();
            for (std::size_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex)
            {
                const std::size_t firstBlock = sourceIndex * blocksPerSource;
                const std::size_t endBlock =
                    std::min(blockCount, firstBlock + blocksPerSource);
                std::size_t partIndex = 0;
                std::size_t partFirstBlock = firstBlock;
                bool partHasBlock = false;
                uint64_t partBytes = 0;
                const auto resetPartBytes = [&]() {
                    // Fixed per-part overhead: the shared includes plus the
                    // prologue/epilogue of every Block function form the part
                    // may emit (unused forms are a small constant
                    // overestimate).
                    partBytes =
                        static_cast<uint64_t>(blockSourceIncludes(prefix).size()) +
                        static_cast<uint64_t>(
                            entryBlockSourcePrologue(className).size()) +
                        static_cast<uint64_t>(
                            scanSourcePrologue(className, sourceIndex, partIndex)
                                .size()) +
                        static_cast<uint64_t>(
                            commitSourcePrologue(className, sourceIndex, partIndex)
                                .size()) +
                        3U * static_cast<uint64_t>(
                                 kBlockSourceFunctionEpilogue.size());
                };
                resetPartBytes();
                std::size_t lastScanByte = std::numeric_limits<std::size_t>::max();
                const auto scanChunkOverhead = [&](std::size_t blockIndex,
                                                   uint64_t &pendingBytes) {
                    // First scan Block of an activity byte in this part:
                    // account for the byte-chunk prologue/epilogue. The owned
                    // mask assumes the part keeps the rest of the byte; a
                    // mid-byte split only shaves bits off the mask.
                    const std::size_t scanByte = blockIndex / 8U;
                    const std::size_t byteEnd =
                        std::min(endBlock, scanByte * 8U + 8U);
                    uint8_t ownedMask = 0;
                    for (std::size_t block = blockIndex; block < byteEnd; ++block)
                    {
                        const bool commitBlock =
                            model.commitBlockBegin != 0 &&
                            block >= model.commitBlockBegin &&
                            block < model.commitBlockEnd;
                        if (!commitBlock)
                        {
                            ownedMask = static_cast<uint8_t>(
                                ownedMask | (1U << (block % 8U)));
                        }
                    }
                    return addByteCount(
                        pendingBytes,
                        static_cast<uint64_t>(
                            scanByteChunkPrologue(scanByte, ownedMask,
                                                  state.fullEvaluation)
                                .size()) +
                            static_cast<uint64_t>(kScanByteChunkEpilogue.size()));
                };

                for (std::size_t blockIndex = firstBlock;
                     blockIndex < endBlock;
                     ++blockIndex)
                {
                    const bool isEntry = blockIndex == 0;
                    const bool isCommit = model.commitBlockBegin != 0 &&
                                          blockIndex >= model.commitBlockBegin &&
                                          blockIndex < model.commitBlockEnd;
                    const bool isScan = !isEntry && !isCommit;
                    const std::optional<uint64_t> blockBytes =
                        isEntry
                            ? measureEntryBlockCase(model, state, diagnostics)
                            : isCommit
                                  ? measureCommitBlockCase(
                                        model, state, blockIndex, diagnostics)
                                  : measureScanBlockCase(
                                        model, state, blockIndex, diagnostics);
                    if (!blockBytes)
                    {
                        return std::nullopt;
                    }
                    uint64_t pendingBytes = *blockBytes;
                    const std::size_t scanByte = blockIndex / 8U;
                    if (isScan && scanByte != lastScanByte &&
                        !scanChunkOverhead(blockIndex, pendingBytes))
                    {
                        diagnostics.error(
                            "AM C++ emitter source size overflow: block=" +
                                std::to_string(blockIndex),
                            std::string(kContext));
                        return std::nullopt;
                    }
                    const uint64_t blockBudget =
                        isCommit ? maxCommitSourceBytes : maxSourceBytes;
                    const bool exceedsBudget =
                        partBytes > blockBudget ||
                        pendingBytes > blockBudget - partBytes;
                    if (partHasBlock && exceedsBudget)
                    {
                        plan[sourceIndex].push_back(BlockSourcePart{
                            .sourceIndex = sourceIndex,
                            .partIndex = partIndex,
                            .firstBlock = partFirstBlock,
                            .endBlock = blockIndex,
                        });
                        ++partIndex;
                        partFirstBlock = blockIndex;
                        partHasBlock = false;
                        resetPartBytes();
                        lastScanByte = std::numeric_limits<std::size_t>::max();
                        if (isScan)
                        {
                            pendingBytes = *blockBytes;
                            if (!scanChunkOverhead(blockIndex, pendingBytes))
                            {
                                diagnostics.error(
                                    "AM C++ emitter source size overflow: block=" +
                                        std::to_string(blockIndex),
                                    std::string(kContext));
                                return std::nullopt;
                            }
                        }
                    }
                    if (isScan)
                    {
                        lastScanByte = scanByte;
                    }
                    if (!addByteCount(partBytes, pendingBytes))
                    {
                        diagnostics.error("AM C++ emitter source size overflow: block=" +
                                              std::to_string(blockIndex),
                                          std::string(kContext));
                        return std::nullopt;
                    }
                    partHasBlock = true;
                }
                if (partHasBlock)
                {
                    plan[sourceIndex].push_back(BlockSourcePart{
                        .sourceIndex = sourceIndex,
                        .partIndex = partIndex,
                        .firstBlock = partFirstBlock,
                        .endBlock = endBlock,
                    });
                }
            }
            return plan;
        }

        struct StagedArtifact
        {
            std::filesystem::path staged;
            std::filesystem::path destination;
        };

        bool publishStagedArtifacts(const std::filesystem::path &stagingDirectory,
                                    const std::vector<StagedArtifact> &artifacts,
                                    wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            const std::filesystem::path backupDirectory = stagingDirectory / ".backup";
            std::error_code filesystemError;
            std::filesystem::create_directory(backupDirectory, filesystemError);
            if (filesystemError)
            {
                diagnostics.error("failed to prepare AM C++ artifact publication: " +
                                      filesystemError.message(),
                                  std::string(kContext));
                return false;
            }

            struct PublishedArtifact
            {
                const StagedArtifact *artifact = nullptr;
                std::filesystem::path backup;
                bool hadOriginal = false;
            };
            std::vector<PublishedArtifact> published;
            published.reserve(artifacts.size());

            const auto rollback = [&] {
                for (auto iterator = published.rbegin(); iterator != published.rend(); ++iterator)
                {
                    std::error_code rollbackError;
                    std::filesystem::remove(iterator->artifact->destination, rollbackError);
                    if (iterator->hadOriginal)
                    {
                        rollbackError.clear();
                        std::filesystem::rename(iterator->backup,
                                                iterator->artifact->destination,
                                                rollbackError);
                    }
                }
            };

            for (const StagedArtifact &artifact : artifacts)
            {
                filesystemError.clear();
                const bool exists = std::filesystem::exists(artifact.destination, filesystemError);
                if (filesystemError)
                {
                    diagnostics.error("failed to inspect AM C++ output artifact: " +
                                          filesystemError.message(),
                                      std::string(kContext));
                    rollback();
                    return false;
                }

                PublishedArtifact publication{
                    .artifact = &artifact,
                    .backup = backupDirectory / artifact.destination.filename(),
                    .hadOriginal = exists,
                };
                if (exists)
                {
                    filesystemError.clear();
                    if (!std::filesystem::is_regular_file(artifact.destination, filesystemError) ||
                        filesystemError)
                    {
                        diagnostics.error("AM C++ output artifact is not a regular file: " +
                                              artifact.destination.string(),
                                          std::string(kContext));
                        rollback();
                        return false;
                    }
                    std::filesystem::rename(artifact.destination,
                                            publication.backup,
                                            filesystemError);
                    if (filesystemError)
                    {
                        diagnostics.error("failed to stage existing AM C++ output artifact: " +
                                              filesystemError.message(),
                                          std::string(kContext));
                        rollback();
                        return false;
                    }
                }

                published.push_back(publication);
                filesystemError.clear();
                std::filesystem::rename(artifact.staged, artifact.destination, filesystemError);
                if (filesystemError)
                {
                    diagnostics.error("failed to publish AM C++ output artifact: " +
                                          filesystemError.message(),
                                      std::string(kContext));
                    rollback();
                    return false;
                }
            }
            return true;
        }

        // ST00010 detector-group folding analysis. For each Block, scans the
        // maximal runs of scheduler-materialized watch-group instructions
        // (changed.* detectors and act.f/act.b activations) and re-groups the
        // foldable detectors by activation target signature (direction +
        // sorted target set). A detector is foldable when:
        //   - it compares a narrow scalar (BitVector <= 64 bits);
        //   - its event result stays block-local (cross-block changed results
        //     go through the runtime-indexed dirty list and keep their form);
        //   - every use of the event is an act inside the same run;
        //   - neither operand is defined inside the run (re-ordering detectors
        //     and hoisting the merges to the run end cannot cross a def).
        // Activity bits are idempotent and order-free and each detector keeps
        // its private old baseline update, so the grouped form is equivalent.
        void planDetectorGroups(const ExecutableModel &model, EmitState &state)
        {
            state.blockDetectorPlans.clear();
            state.blockDetectorPlans.resize(state.blockCount);
            state.foldedDetectorEvents.assign(state.program.variableCount(), 0);
            state.detectorFoldedCount = 0;
            state.detectorGroupCount = 0;
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                if (blockSize < 2)
                {
                    continue;
                }
                const auto opcodeAt = [&](std::size_t position) {
                    return state.program.opcode(model.program.blockInstruction(
                        block, position));
                };
                const auto isRunInstruction = [&](std::size_t position) {
                    const Opcode opcode = opcodeAt(position);
                    return isDetectorChangedOpcode(opcode) ||
                           opcode == Opcode::ActForward || opcode == Opcode::ActBackward;
                };
                bool hasAct = false;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const Opcode opcode = opcodeAt(position);
                    if (opcode == Opcode::ActForward || opcode == Opcode::ActBackward)
                    {
                        hasAct = true;
                        break;
                    }
                }
                if (!hasAct)
                {
                    continue;
                }
                // Operand use positions within the block, in ascending order.
                std::unordered_map<uint32_t, std::vector<uint32_t>> uses;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    for (const VariableId operand : state.program.operands(instruction))
                    {
                        uses[operand.value].push_back(static_cast<uint32_t>(position));
                    }
                }

                EmitState::DetectorGroupPlan plan;
                bool anyFold = false;
                std::size_t position = 0;
                while (position < blockSize)
                {
                    if (!isRunInstruction(position))
                    {
                        ++position;
                        continue;
                    }
                    const std::size_t runBegin = position;
                    while (position < blockSize && isRunInstruction(position))
                    {
                        ++position;
                    }
                    const std::size_t runEnd = position;

                    // Foldable detectors in the run.
                    std::unordered_set<uint32_t> foldable;
                    for (std::size_t index = runBegin; index < runEnd; ++index)
                    {
                        const InstructionId instruction =
                            model.program.blockInstruction(block, index);
                        if (!isDetectorChangedOpcode(state.program.opcode(instruction)))
                        {
                            continue;
                        }
                        const auto operands = state.program.operands(instruction);
                        const auto results = state.program.results(instruction);
                        if (operands.size() < 2 || results.empty())
                        {
                            continue;
                        }
                        const VariableId watched = operands[0];
                        const VariableId old = operands[1];
                        const VariableId event = results.front();
                        const Type &watchedType = variableType(state, watched);
                        if (watchedType.kind != TypeKind::BitVector ||
                            watchedType.bitWidth > 64)
                        {
                            continue;
                        }
                        if (state.crossBlockChangedResults[event.value])
                        {
                            continue;
                        }
                        const auto useEntry = uses.find(event.value);
                        if (useEntry == uses.end())
                        {
                            continue;
                        }
                        bool eligible = true;
                        for (const uint32_t use : useEntry->second)
                        {
                            if (use < runBegin || use >= runEnd)
                            {
                                eligible = false;
                                break;
                            }
                            const Opcode useOpcode = opcodeAt(use);
                            if (useOpcode != Opcode::ActForward &&
                                useOpcode != Opcode::ActBackward)
                            {
                                eligible = false;
                                break;
                            }
                        }
                        if (!eligible)
                        {
                            continue;
                        }
                        for (const VariableId operand : {watched, old})
                        {
                            if (state.variableDefBlock[operand.value] == blockIndex &&
                                state.variableDefPosition[operand.value] >= runBegin &&
                                state.variableDefPosition[operand.value] <
                                    static_cast<uint32_t>(runEnd))
                            {
                                eligible = false;
                                break;
                            }
                        }
                        if (eligible)
                        {
                            foldable.insert(static_cast<uint32_t>(index));
                        }
                    }
                    if (foldable.empty())
                    {
                        continue;
                    }

                    // Group the member acts of each foldable detector by
                    // (direction, sorted targets). Groups never span runs, so
                    // each merge is anchored exactly once at the run end.
                    std::map<std::pair<bool, std::vector<uint32_t>>, uint32_t> groupByKey;
                    std::vector<uint32_t> runGroups;
                    for (std::size_t index = runBegin; index < runEnd; ++index)
                    {
                        if (foldable.count(static_cast<uint32_t>(index)) == 0)
                        {
                            continue;
                        }
                        const InstructionId instruction =
                            model.program.blockInstruction(block, index);
                        const VariableId event = state.program.results(instruction).front();
                        // Validate every consuming act before committing any
                        // group state, so a mid-list failure cannot leave an
                        // accumulator-less group behind.
                        bool eligible = true;
                        for (const uint32_t use : uses.at(event.value))
                        {
                            const InstructionId act =
                                model.program.blockInstruction(block, use);
                            if (!state.program.activationAttributes(act))
                            {
                                eligible = false;
                                break;
                            }
                        }
                        if (!eligible)
                        {
                            continue;
                        }
                        std::vector<uint32_t> groups;
                        for (const uint32_t use : uses.at(event.value))
                        {
                            const InstructionId act =
                                model.program.blockInstruction(block, use);
                            const auto attributes = state.program.activationAttributes(act);
                            const bool forward =
                                state.program.opcode(act) == Opcode::ActForward;
                            std::vector<uint32_t> targets;
                            targets.reserve(attributes->targets.size());
                            for (const BlockId target : attributes->targets)
                            {
                                targets.push_back(target.value);
                            }
                            std::sort(targets.begin(), targets.end());
                            const auto key = std::make_pair(forward, std::move(targets));
                            const auto [entry, inserted] = groupByKey.emplace(
                                key, static_cast<uint32_t>(plan.groups.size()));
                            if (inserted)
                            {
                                EmitState::DetectorGroupPlan::Group group;
                                group.forward = forward;
                                group.targets.reserve(entry->first.second.size());
                                for (const uint32_t target : entry->first.second)
                                {
                                    group.targets.push_back(BlockId{target});
                                }
                                plan.groups.push_back(std::move(group));
                                runGroups.push_back(entry->second);
                            }
                            plan.groups[entry->second].originalTargetCount +=
                                attributes->targets.size();
                            groups.push_back(entry->second);
                        }
                        if (groups.empty())
                        {
                            continue;
                        }
                        for (const uint32_t use : uses.at(event.value))
                        {
                            plan.skippedActs.insert(use);
                        }
                        plan.accumGroups.emplace(static_cast<uint32_t>(index),
                                                 std::move(groups));
                        state.foldedDetectorEvents[event.value] = 1;
                        ++state.detectorFoldedCount;
                        anyFold = true;
                    }
                    if (!runGroups.empty())
                    {
                        plan.mergesAfter.emplace(static_cast<uint32_t>(runEnd - 1),
                                                 std::move(runGroups));
                    }
                }
                if (anyFold)
                {
                    state.detectorGroupCount += plan.groups.size();
                    state.blockDetectorPlans[blockIndex] = std::move(plan);
                }
            }
        }

        // ST00011: plans the emit-time replacement of commit-Block tail
        // changed.any detectors on Array state targets by write-site change
        // flags (see EmitState::ArrayWatchPlan). Eligibility per detector:
        // Array watched type, at least one same-Block write site (mem.write /
        // mem.fill, the only array-writing opcodes), event consumed only by
        // same-Block act.f/act.b, and no cross-block changed result. Anything
        // else keeps the whole-array compare form.
        void planArrayWatchGroups(const ExecutableModel &model, EmitState &state)
        {
            state.blockArrayWatchPlans.clear();
            state.blockArrayWatchPlans.resize(state.blockCount);
            state.arrayWatchReplacedCount = 0;
            if (model.commitBlockBegin == 0)
            {
                return;
            }
            for (uint32_t blockIndex = model.commitBlockBegin;
                 blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                if (blockSize < 2)
                {
                    continue;
                }
                const auto opcodeAt = [&](std::size_t position) {
                    return state.program.opcode(model.program.blockInstruction(
                        block, position));
                };
                // Operand use positions within the block, in ascending order.
                std::unordered_map<uint32_t, std::vector<uint32_t>> uses;
                // Write positions per array target: mem.write / mem.fill /
                // mem.write_lanes are the only opcodes that write an Array
                // variable.
                std::unordered_map<uint32_t, std::vector<uint32_t>> arrayWrites;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const auto operands = state.program.operands(instruction);
                    const StateWriteLayout writeLayout = stateWriteLayout(opcode);
                    if (writeLayout.isStateWrite && writeLayout.memory)
                    {
                        arrayWrites[operands[writeLayout.targetIndex].value].push_back(
                            static_cast<uint32_t>(position));
                    }
                    else if (opcode == Opcode::MemoryFill)
                    {
                        arrayWrites[operands[1].value].push_back(
                            static_cast<uint32_t>(position));
                    }
                    else if (opcode == Opcode::MemoryWriteLanes)
                    {
                        arrayWrites[operands[2].value].push_back(
                            static_cast<uint32_t>(position));
                    }
                    for (const VariableId operand : operands)
                    {
                        uses[operand.value].push_back(static_cast<uint32_t>(position));
                    }
                }
                if (arrayWrites.empty())
                {
                    continue;
                }

                EmitState::ArrayWatchPlan plan;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    if (opcodeAt(position) != Opcode::ChangedAny)
                    {
                        continue;
                    }
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const auto operands = state.program.operands(instruction);
                    const auto results = state.program.results(instruction);
                    if (operands.size() < 2 || results.empty())
                    {
                        continue;
                    }
                    const VariableId watched = operands[0];
                    const VariableId event = results.front();
                    if (variableType(state, watched).kind != TypeKind::Array)
                    {
                        continue;
                    }
                    const auto writes = arrayWrites.find(watched.value);
                    if (writes == arrayWrites.end() || writes->second.empty())
                    {
                        continue;
                    }
                    // The detector must observe the Block's own writes, so
                    // every write site has to precede it.
                    if (writes->second.back() >= static_cast<uint32_t>(position))
                    {
                        continue;
                    }
                    if (state.crossBlockChangedResults[event.value])
                    {
                        continue;
                    }
                    const auto useEntry = uses.find(event.value);
                    if (useEntry == uses.end())
                    {
                        continue;
                    }
                    bool eligible = true;
                    for (const uint32_t use : useEntry->second)
                    {
                        const Opcode useOpcode = opcodeAt(use);
                        if (useOpcode != Opcode::ActForward &&
                            useOpcode != Opcode::ActBackward)
                        {
                            eligible = false;
                            break;
                        }
                    }
                    if (!eligible)
                    {
                        continue;
                    }
                    bool collision = false;
                    for (const uint32_t writePosition : writes->second)
                    {
                        if (plan.writeAccum.count(writePosition) != 0)
                        {
                            // Should not happen (one detector per (Block,
                            // array) by construction); keep both sites in the
                            // compare form rather than guessing.
                            collision = true;
                            break;
                        }
                    }
                    if (collision)
                    {
                        continue;
                    }
                    const uint32_t accum = plan.accumCount++;
                    plan.detectorAccum.emplace(static_cast<uint32_t>(position), accum);
                    for (const uint32_t writePosition : writes->second)
                    {
                        plan.writeAccum.emplace(writePosition, accum);
                    }
                    ++state.arrayWatchReplacedCount;
                }
                if (plan.accumCount != 0)
                {
                    state.blockArrayWatchPlans[blockIndex] = std::move(plan);
                }
            }
        }

        // Computes the per-Block batch event gate for commit Blocks (see
        // EmitState::blockCommitGate). The gate is the deduplicated OR of the
        // Block's leading changed.* gate detectors (the instructions before
        // the first state write); headCount records where the gated region
        // begins. Every commit Block carries its detectors by construction,
        // so the gate always exists when the Block has anything to gate.
        void planCommitEventGates(const ExecutableModel &model, EmitState &state)
        {
            state.blockCommitGate.clear();
            state.blockCommitGate.resize(state.blockCount);
            state.commitGateBlockCount = 0;
            if (model.commitBlockBegin == 0)
            {
                return;
            }
            for (uint32_t blockIndex = model.commitBlockBegin;
                 blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                std::string gate;
                uint32_t headCount = 0;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    if (!isDetectorChangedOpcode(state.program.opcode(instruction)))
                    {
                        break;
                    }
                    const auto results = state.program.results(instruction);
                    if (results.size() != 1)
                    {
                        break;
                    }
                    if (!gate.empty())
                    {
                        gate += " || ";
                    }
                    gate += boolExpr(state, results.front());
                    ++headCount;
                }
                if (headCount == 0 || headCount >= blockSize)
                {
                    continue;
                }
                state.blockCommitGate[blockIndex] =
                    EmitState::CommitGate{std::move(gate), {}, headCount};
                ++state.commitGateBlockCount;
            }
        }

        // Strengthens the existing commit event gate with exact input-dirty
        // tracking. The leaf set is a backward slice from every state write
        // through definitions in the gated body. A narrow state leaf is
        // tracked without a value comparison when every one of its writers
        // already has ST00013 write-point change detection: a real write sets
        // the dependent gate's dirty byte. Remaining narrow leaves use exact
        // snapshots. This turns large stable commit cones into one dirty-byte
        // check without adding a second comparison at their write stations.
        void planCommitInputGates(const ExecutableModel &model, EmitState &state)
        {
            state.commitInputSnapshotCount = 0;
            state.commitInputGateCount = 0;
            state.commitInputTrackedStateCount = 0;
            state.commitInputProducerBlockCount = 0;
            state.commitInputDirtyEdgeCount = 0;
            state.commitInputGatedInstructions = 0;
            state.commitInputGatedWrites = 0;
            state.commitInputDirtyGatesByInstruction.clear();
            state.commitInputDirtyGatesByBlock.clear();
            state.commitInputSparseRejectedBlocks = 0;
            state.commitInputSparseRejectedWrites = 0;
            state.commitInputSparseRejectedEdges = 0;
            if (!state.commitInputGating || model.commitBlockBegin == 0)
            {
                return;
            }
            uint64_t rejectedUnsafeBlocks = 0;
            uint64_t rejectedUnsafeWrites = 0;
            uint64_t rejectedSnapshotBlocks = 0;
            uint64_t rejectedSnapshotWrites = 0;
            uint64_t rejectedCostBlocks = 0;
            uint64_t rejectedCostWrites = 0;

            const std::size_t variableCount = state.program.variableCount();
            std::vector<uint8_t> mutableOrExternal(variableCount, 0);
            std::vector<uint8_t> stateTargetSeen(variableCount, 0);
            std::vector<uint8_t> stateTargetTrackable(variableCount, 1);
            std::unordered_map<uint32_t, std::vector<uint32_t>>
                writeInstructionsByTarget;
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const EmitState::ScalarWatchPlan *scalarPlan =
                    blockIndex < state.blockScalarWatchPlans.size() &&
                            state.blockScalarWatchPlans[blockIndex]
                        ? &*state.blockScalarWatchPlans[blockIndex]
                        : nullptr;
                for (std::size_t position = 0;
                     position < model.program.blockSize(block); ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const OpcodeTraits traits = opcodeTraits(opcode);
                    const auto operands = state.program.operands(instruction);
                    if (traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand ||
                        traits.stateTargetOperand >= operands.size())
                    {
                        continue;
                    }
                    const uint32_t target = operands[traits.stateTargetOperand].value;
                    mutableOrExternal[target] = 1;
                    if (!isStateWriteOpcode(opcode))
                    {
                        continue;
                    }
                    stateTargetSeen[target] = 1;
                    const Type &type = state.variableTypes[target];
                    const bool fusedScalarWrite =
                        type.kind == TypeKind::BitVector &&
                        (type.bitWidth <= 64 || !state.wideStateExplode) &&
                        isRegisterWriteOpcode(opcode) &&
                        opcode != Opcode::RegisterWriteDynLane &&
                        scalarPlan != nullptr &&
                        scalarPlan->writeRaise.count(static_cast<uint32_t>(position)) != 0;
                    if (!fusedScalarWrite)
                    {
                        stateTargetTrackable[target] = 0;
                    }
                    else
                    {
                        writeInstructionsByTarget[target].push_back(instruction.value);
                    }
                }
            }
            for (const PortBinding &port : model.interface.ports)
            {
                for (const VariableId variable :
                     {port.input, port.output, port.outputEnable})
                {
                    if (variable.valid())
                    {
                        mutableOrExternal[variable.value] = 1;
                    }
                }
            }
            for (const VariableLabel &declared : model.interface.declaredVariables)
            {
                mutableOrExternal[declared.variable.value] = 1;
            }

            for (uint32_t blockIndex = model.commitBlockBegin;
                 blockIndex < model.commitBlockEnd; ++blockIndex)
            {
                EmitState::CommitGate &gate = state.blockCommitGate[blockIndex];
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                if (gate.headCount == 0 || gate.headCount >= blockSize)
                {
                    continue;
                }

                bool safe = true;
                uint64_t writeCount = 0;
                std::vector<uint32_t> writePositions;
                for (std::size_t position = gate.headCount; position < blockSize; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const OpcodeEffect effect = opcodeTraits(opcode).effect;
                    if (effect == OpcodeEffect::HostRead ||
                        effect == OpcodeEffect::HostEffect)
                    {
                        safe = false;
                        break;
                    }
                    if (isStateWriteOpcode(opcode))
                    {
                        // MemoryFill / MemoryWriteLanes do not use the common
                        // fixed-operand layout; keep their Blocks on the
                        // baseline path rather than treating scheduler event
                        // operands as cone inputs.
                        if (!stateWriteLayout(opcode).isStateWrite)
                        {
                            safe = false;
                            break;
                        }
                        writePositions.push_back(static_cast<uint32_t>(position));
                        ++writeCount;
                    }
                }
                if (!safe || writePositions.empty())
                {
                    continue;
                }

                std::set<uint32_t> leaves;
                std::unordered_set<uint32_t> visitedDefinitions;
                std::function<void(VariableId, uint32_t)> visit =
                    [&](VariableId variable, uint32_t beforePosition) {
                        const uint32_t definitionBlock =
                            state.variableDefBlock[variable.value];
                        const uint32_t definitionPosition =
                            state.variableDefPosition[variable.value];
                        if (definitionBlock == blockIndex &&
                            definitionPosition >= gate.headCount &&
                            definitionPosition < beforePosition)
                        {
                            if (!visitedDefinitions.insert(definitionPosition).second)
                            {
                                return;
                            }
                            const InstructionId definition =
                                model.program.blockInstruction(block, definitionPosition);
                            for (const VariableId operand :
                                 state.program.operands(definition))
                            {
                                visit(operand, definitionPosition);
                            }
                            return;
                        }

                        const InitKind initKind = state.program.init(
                            state.program.variable(variable).init).kind;
                        const bool immutable = definitionBlock == kInvalidLocalityBlock &&
                                               mutableOrExternal[variable.value] == 0 &&
                                               (initKind == InitKind::Zero ||
                                                initKind == InitKind::Constant);
                        if (!immutable)
                        {
                            leaves.insert(variable.value);
                        }
                    };

                for (const uint32_t position : writePositions)
                {
                    const InstructionId write =
                        model.program.blockInstruction(block, position);
                    const StateWriteLayout layout =
                        stateWriteLayout(state.program.opcode(write));
                    const auto operands = state.program.operands(write);
                    for (uint32_t operandIndex = 0;
                         operandIndex < layout.fixedCount; ++operandIndex)
                    {
                        visit(operands[operandIndex], position);
                    }
                }
                if (leaves.empty())
                {
                    continue;
                }
                std::vector<uint32_t> trackedStateLeaves;
                std::vector<uint32_t> snapshotLeaves;
                std::set<uint32_t> producerBlocks;
                for (const uint32_t variable : leaves)
                {
                    if (stateTargetSeen[variable] != 0 &&
                        stateTargetTrackable[variable] != 0)
                    {
                        trackedStateLeaves.push_back(variable);
                        continue;
                    }
                    const uint32_t definitionBlock =
                        state.variableDefBlock[variable];
                    if (stateTargetSeen[variable] == 0 &&
                        definitionBlock != kInvalidLocalityBlock &&
                        definitionBlock != 0 &&
                        definitionBlock < model.commitBlockBegin)
                    {
                        producerBlocks.insert(definitionBlock);
                        continue;
                    }
                    const Type &type = state.variableTypes[variable];
                    if (type.kind != TypeKind::BitVector || type.bitWidth > 64)
                    {
                        safe = false;
                        continue;
                    }
                    snapshotLeaves.push_back(variable);
                }
                const uint64_t workInstructions = blockSize - gate.headCount;
                uint64_t estimatedDirtyEdges = producerBlocks.size();
                for (const uint32_t variable : trackedStateLeaves)
                {
                    const auto writers = writeInstructionsByTarget.find(variable);
                    if (writers != writeInstructionsByTarget.end())
                    {
                        estimatedDirtyEdges += writers->second.size();
                    }
                }
                const bool sparseCostRejected =
                    state.commitInputSparseGating && estimatedDirtyEdges != 0 &&
                    workInstructions < estimatedDirtyEdges * UINT64_C(4);
                if (!safe || snapshotLeaves.size() > 64U ||
                    sparseCostRejected ||
                    workInstructions < snapshotLeaves.size() * 2U)
                {
                    if (!safe)
                    {
                        ++rejectedUnsafeBlocks;
                        rejectedUnsafeWrites += writeCount;
                    }
                    else if (snapshotLeaves.size() > 64U)
                    {
                        ++rejectedSnapshotBlocks;
                        rejectedSnapshotWrites += writeCount;
                    }
                    else if (sparseCostRejected)
                    {
                        ++state.commitInputSparseRejectedBlocks;
                        state.commitInputSparseRejectedWrites += writeCount;
                        state.commitInputSparseRejectedEdges += estimatedDirtyEdges;
                    }
                    else
                    {
                        ++rejectedCostBlocks;
                        rejectedCostWrites += writeCount;
                    }
                    continue;
                }

                const uint64_t validIndex = state.commitInputGateCount;
                const uint64_t snapshotOffset = state.commitInputSnapshotCount;
                std::string changed = "commitInputValid_[" +
                                      std::to_string(validIndex) + "] == 0 || " +
                                      "commitInputDirty_[" +
                                      std::to_string(validIndex) + "] != 0";
                std::string refresh = "commitInputValid_[" +
                                      std::to_string(validIndex) + "] = 1;\n" +
                                      "commitInputDirty_[" +
                                      std::to_string(validIndex) + "] = 0;\n";
                uint64_t inputIndex = 0;
                for (const uint32_t variable : snapshotLeaves)
                {
                    const std::string value = valueExpr(state, VariableId{variable});
                    const std::string snapshot = "commitInputSnapshots_[" +
                                                 std::to_string(snapshotOffset + inputIndex) +
                                                 "]";
                    changed += " || " + snapshot + " != " + value;
                    refresh += snapshot + " = " + value + ";\n";
                    ++inputIndex;
                }
                gate.expression = "(" + gate.expression + ") && (" + changed + ")";
                gate.preamble = std::move(refresh);
                for (const uint32_t variable : trackedStateLeaves)
                {
                    const auto writers = writeInstructionsByTarget.find(variable);
                    if (writers == writeInstructionsByTarget.end())
                    {
                        continue;
                    }
                    for (const uint32_t instruction : writers->second)
                    {
                        state.commitInputDirtyGatesByInstruction[instruction].push_back(
                            static_cast<uint32_t>(validIndex));
                        ++state.commitInputDirtyEdgeCount;
                    }
                }
                for (const uint32_t producer : producerBlocks)
                {
                    state.commitInputDirtyGatesByBlock[producer].push_back(
                        static_cast<uint32_t>(validIndex));
                    ++state.commitInputDirtyEdgeCount;
                }
                state.commitInputSnapshotCount += snapshotLeaves.size();
                state.commitInputTrackedStateCount += trackedStateLeaves.size();
                state.commitInputProducerBlockCount += producerBlocks.size();
                ++state.commitInputGateCount;
                state.commitInputGatedInstructions += workInstructions;
                state.commitInputGatedWrites += writeCount;
            }
            std::cerr << "[commit-input-gating-bails] unsafe="
                      << rejectedUnsafeBlocks << '/' << rejectedUnsafeWrites
                      << " snapshots=" << rejectedSnapshotBlocks << '/'
                      << rejectedSnapshotWrites
                      << " cost=" << rejectedCostBlocks << '/'
                      << rejectedCostWrites << '\n';
            if (state.commitInputSparseGating)
            {
                std::cerr << "[commit-input-sparse] min_work_per_edge=4 rejected="
                          << state.commitInputSparseRejectedBlocks << '/'
                          << state.commitInputSparseRejectedWrites
                          << " dirty_edges=" << state.commitInputSparseRejectedEdges
                          << '\n';
            }
        }

        // ST00013: plans the emit-time fusion of commit-Block tail changed.any
        // detectors on BitVector state targets into the Block's own
        // RegisterWrite sites (see EmitState::ScalarWatchPlan). Eligibility
        // mirrors ST00011: same-Block write sites cover the target (no
        // LatchWrite to it), event consumed only by same-Block act.f/act.b,
        // no cross-block changed result.
        void planScalarWatchGroups(const ExecutableModel &model, EmitState &state)
        {
            state.blockScalarWatchPlans.clear();
            state.blockScalarWatchPlans.resize(state.blockCount);
            state.scalarWatchFusedCount = 0;
            if (model.commitBlockBegin == 0)
            {
                return;
            }
            for (uint32_t blockIndex = model.commitBlockBegin;
                 blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                if (blockSize < 2)
                {
                    continue;
                }
                const auto opcodeAt = [&](std::size_t position) {
                    return state.program.opcode(model.program.blockInstruction(
                        block, position));
                };
                std::unordered_map<uint32_t, std::vector<uint32_t>> uses;
                std::unordered_map<uint32_t, std::vector<uint32_t>> registerWrites;
                std::unordered_set<uint32_t> latchTargets;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const auto operands = state.program.operands(instruction);
                    const StateWriteLayout writeLayout = stateWriteLayout(opcode);
                    if (isRegisterWriteOpcode(opcode))
                    {
                        registerWrites[operands[writeLayout.targetIndex].value].push_back(
                            static_cast<uint32_t>(position));
                    }
                    else if (isLatchWriteOpcode(opcode))
                    {
                        latchTargets.insert(operands[writeLayout.targetIndex].value);
                    }
                    for (const VariableId operand : operands)
                    {
                        uses[operand.value].push_back(static_cast<uint32_t>(position));
                    }
                }
                if (registerWrites.empty())
                {
                    continue;
                }

                EmitState::ScalarWatchPlan plan;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    if (opcodeAt(position) != Opcode::ChangedAny)
                    {
                        continue;
                    }
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const auto operands = state.program.operands(instruction);
                    const auto results = state.program.results(instruction);
                    if (operands.size() < 2 || results.empty())
                    {
                        continue;
                    }
                    const VariableId watched = operands[0];
                    const VariableId event = results.front();
                    if (variableType(state, watched).kind != TypeKind::BitVector)
                    {
                        continue;
                    }
                    const auto writes = registerWrites.find(watched.value);
                    if (writes == registerWrites.end() || writes->second.empty() ||
                        latchTargets.count(watched.value) != 0)
                    {
                        continue;
                    }
                    if (writes->second.back() >= static_cast<uint32_t>(position))
                    {
                        continue;
                    }
                    if (state.crossBlockChangedResults[event.value])
                    {
                        continue;
                    }
                    const auto useEntry = uses.find(event.value);
                    if (useEntry == uses.end())
                    {
                        continue;
                    }
                    bool eligible = true;
                    for (const uint32_t use : useEntry->second)
                    {
                        const Opcode useOpcode = opcodeAt(use);
                        if (useOpcode != Opcode::ActForward &&
                            useOpcode != Opcode::ActBackward)
                        {
                            eligible = false;
                            break;
                        }
                    }
                    if (!eligible)
                    {
                        continue;
                    }
                    bool collision = false;
                    for (const uint32_t writePosition : writes->second)
                    {
                        if (plan.writeRaise.count(writePosition) != 0)
                        {
                            collision = true;
                            break;
                        }
                    }
                    if (collision)
                    {
                        continue;
                    }
                    const uint32_t flag = plan.flagCount++;
                    plan.detectorRaise.emplace(static_cast<uint32_t>(position), flag);
                    for (const uint32_t writePosition : writes->second)
                    {
                        plan.writeRaise.emplace(writePosition, flag);
                    }
                    ++state.scalarWatchFusedCount;
                }
                if (plan.flagCount != 0)
                {
                    state.blockScalarWatchPlans[blockIndex] = std::move(plan);
                }
            }
        }

        // Guard-event gating (attribute "guardEventGating", default off):
        // finds pure guard compute Blocks and plans their scan-site event
        // gate (EmitState::blockGuardGate). A Block qualifies when
        //   - it holds at least one gateable fatal: a system.task "fatal"
        //     with Normal schedule (no onceCompleted_ slot write),
        //     Immediate event mode (no pendingHostEvents_ latch) and at
        //     least one event operand, every event operand being a
        //     cross-block changed result (a changedResults_[] slot);
        //   - every other instruction is side-effect free: a Pure opcode
        //     per opcodeTraits, or a MemoryRead/MemoryReadAll state read
        //     (no state writes, no act.f/act.b, no changed detectors, no
        //     DPI calls); event-gated "fwrite"/"finish" system tasks under
        //     the same constraints as the fatal are also allowed — their
        //     host effect fires only when one of their event slots is set,
        //     which the gate covers;
        //   - every value the Block writes (instruction results) is defined
        //     exactly once inside the Block, is never read by another Block
        //     or before its own definition, and is no state target,
        //     interface port/declared variable, or init()-assigned slot.
        // The gate expression is the OR of the guard tasks' event slot
        // boolExprs: gate closed implies every guard task's
        // `fire && (events)` condition is false, so no observable effect of
        // the Block can trigger and skipping the whole body is
        // unobservable.
        void planGuardEventGates(const ExecutableModel &model, EmitState &state)
        {
            state.blockGuardGate.clear();
            state.blockGuardGate.resize(state.blockCount);
            state.guardGatedBlockCount = 0;
            state.guardGatedAtoms = 0;
            state.guardGatedInstructions = 0;
            if (!state.guardEventGating || model.commitBlockBegin == 0)
            {
                return;
            }
            const std::size_t variableCount = state.variableTypes.size();
            // Values a gate-safe Block may not define: state targets (any
            // state-write target or detector-watched / memory-read state
            // operand), interface ports and declared variables, and
            // init()-assigned slots.
            std::vector<bool> stateOrInterface(variableCount, false);
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const OpcodeTraits traits =
                        opcodeTraits(state.program.opcode(instruction));
                    const auto operands = state.program.operands(instruction);
                    if (traits.stateTargetOperand != OpcodeTraits::kNoTargetOperand &&
                        traits.stateTargetOperand < operands.size())
                    {
                        stateOrInterface[operands[traits.stateTargetOperand].value] = true;
                    }
                }
            }
            for (const PortBinding &port : model.interface.ports)
            {
                if (port.input.valid())
                {
                    stateOrInterface[port.input.value] = true;
                }
                if (port.output.valid())
                {
                    stateOrInterface[port.output.value] = true;
                }
                if (port.outputEnable.valid())
                {
                    stateOrInterface[port.outputEnable.value] = true;
                }
            }
            for (const VariableLabel &declared : model.interface.declaredVariables)
            {
                stateOrInterface[declared.variable.value] = true;
            }
            for (uint32_t index = 0; index < variableCount; ++index)
            {
                const InitKind kind =
                    state.program.init(state.program.variable(VariableId{index}).init).kind;
                if (kind == InitKind::Constant || kind == InitKind::Actions)
                {
                    stateOrInterface[index] = true;
                }
            }

            // Block 0 is the entry Block and never goes through the scan
            // wrapper, so only [1, commitBlockBegin) can be gated.
            for (uint32_t blockIndex = 1; blockIndex < model.commitBlockBegin; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                if (blockSize == 0)
                {
                    continue;
                }
                bool eligible = true;
                bool sawFatal = false;
                std::vector<uint32_t> gateVariables;
                std::unordered_set<uint32_t> gateVariableSet;
                for (std::size_t position = 0; position < blockSize && eligible; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const auto operands = state.program.operands(instruction);
                    const auto results = state.program.results(instruction);
                    const OpcodeEffect effect = opcodeTraits(opcode).effect;
                    const bool sideEffectFree =
                        effect == OpcodeEffect::Pure || opcode == Opcode::MemoryRead ||
                        opcode == Opcode::MemoryReadAll;
                    if (!sideEffectFree)
                    {
                        if (opcode != Opcode::SystemTask)
                        {
                            eligible = false;
                            break;
                        }
                        const auto attributes =
                            state.program.systemTaskAttributes(instruction);
                        // Gateable guard task: an event-gated host task
                        // (fatal/fwrite/finish are the only system.task
                        // bindings this emitter supports) whose firing
                        // requires one of its changedResults_ event slots —
                        // so closing the slot OR silences it. Normal
                        // schedule + Immediate mode exclude the
                        // onceCompleted_ / pendingHostEvents_ slot writes,
                        // which would be effects the gate cannot cover.
                        if (!attributes || attributes->schedule != CallSchedule::Normal ||
                            attributes->eventMode != HostEventMode::Immediate ||
                            attributes->eventCount == 0 || operands.empty() ||
                            attributes->eventCount > operands.size() - 1U)
                        {
                            eligible = false;
                            break;
                        }
                        const std::string_view taskName =
                            state.program.string(attributes->name);
                        const bool fatalTask = taskName == "fatal";
                        if (!fatalTask && taskName != "fwrite" && taskName != "finish")
                        {
                            eligible = false;
                            break;
                        }
                        const std::size_t eventBegin =
                            operands.size() - attributes->eventCount;
                        for (std::size_t index = eventBegin; index < operands.size(); ++index)
                        {
                            const uint32_t variable = operands[index].value;
                            if (!state.crossBlockChangedResults[variable])
                            {
                                eligible = false;
                                break;
                            }
                            if (gateVariableSet.insert(variable).second)
                            {
                                gateVariables.push_back(variable);
                            }
                        }
                        if (!eligible)
                        {
                            break;
                        }
                        sawFatal = sawFatal || fatalTask;
                    }
                    for (const VariableId result : results)
                    {
                        const uint32_t variable = result.value;
                        if (state.variableDefBlock[variable] != blockIndex ||
                            (state.variableEscapeFlags[variable] &
                             (kEscapeCrossBlockUse | kEscapeEarlyUse)) != 0 ||
                            stateOrInterface[variable])
                        {
                            eligible = false;
                            break;
                        }
                    }
                }
                if (!eligible || !sawFatal || gateVariables.empty())
                {
                    continue;
                }
                std::string gate;
                for (const uint32_t variable : gateVariables)
                {
                    if (!gate.empty())
                    {
                        gate += " || ";
                    }
                    gate += boolExpr(state, VariableId{variable});
                }
                EmitState::GuardGate plan;
                plan.expression = std::move(gate);
                plan.atoms = model.program.blockAtomCount(block);
                plan.instructions = blockSize;
                state.guardGatedAtoms += plan.atoms;
                state.guardGatedInstructions += plan.instructions;
                state.blockGuardGate[blockIndex] = std::move(plan);
                ++state.guardGatedBlockCount;
            }
        }

        // NO0008 block-level same-select mux fusion: mux-rooted atoms
        // (Singleton/Tree whose last member is a Mux) adjacent in a Block
        // form fusion runs; the select derives from the root instruction
        // itself, so the planner does not depend on atom signatures. The
        // first covered instruction emits the run's whole two-phase code
        // (cones in atom order, then one fused if/else over the root
        // muxes); the other covered instructions emit nothing. A run closes
        // on a non-eligible atom, on a select change, or when a cone member
        // would read an earlier run root's result (the fused if/else would
        // otherwise become a use-before-def); the atom then starts a fresh
        // run. Runs with a single arm emit plainly.
        // NO0014: plan dynamic bit-field functional-update cone collapse
        // (semantics and cone signature documented in EmitState). Emitter
        // side only; the scheduled program is untouched (atom connectivity
        // invariant by construction). Planned after planWindowedChains so
        // F1-claimed instructions can be excluded.
        void planDynBlendCones(const ExecutableModel &model, EmitState &state)
        {
            constexpr uint32_t kMinBlendWidth = 256;

            const std::size_t instructionCount = state.program.instructionCount();
            state.instructionDynBlendPlan.assign(instructionCount, -1);
            state.instructionDynBlendAction.assign(instructionCount, -1);
            state.instructionDynBlendMaterialize.assign(instructionCount, 0);
            state.dynBlendPlans.clear();
            state.dynBlendChainCount = 0;
            state.dynBlendConeCount = 0;
            state.dynBlendSkipped = 0;
            state.dynBlendRemapped = 0;
            state.dynBlendMaterialized = 0;
            state.dynBlendBailed = 0;
            if (std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_DYNBLEND") != nullptr)
            {
                return;
            }

            const uint32_t variableCount = state.program.variableCount();
            const uint32_t blockCount = state.blockCount;
            constexpr uint32_t kInvalidVar = std::numeric_limits<uint32_t>::max();

            std::vector<uint8_t> visible(variableCount, 0);
            for (const PortBinding &port : model.interface.ports)
            {
                if (port.input.valid())
                {
                    visible[port.input.value] = 1;
                }
                if (port.output.valid())
                {
                    visible[port.output.value] = 1;
                }
                if (port.outputEnable.valid())
                {
                    visible[port.outputEnable.value] = 1;
                }
            }
            for (const VariableLabel &declared : model.interface.declaredVariables)
            {
                visible[declared.variable.value] = 1;
            }

            // Consumer lists (CSR) over wide vars (cone internals/results).
            std::vector<uint32_t> instructionBlock(instructionCount, kInvalidVar);
            std::vector<uint32_t> instructionPos(instructionCount, 0);
            std::vector<uint8_t> candidate(variableCount, 0);
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    instructionBlock[instruction.value] = blockIndex;
                    instructionPos[instruction.value] = static_cast<uint32_t>(position);
                    const auto results = state.program.results(instruction);
                    if (!results.empty() &&
                        variableType(state, results.front()).bitWidth > 64)
                    {
                        candidate[results.front().value] = 1;
                    }
                }
            }
            std::vector<uint32_t> useOffsets(variableCount + 1, 0);
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    for (const VariableId operand : state.program.operands(instruction))
                    {
                        if (operand.valid() && candidate[operand.value])
                        {
                            ++useOffsets[operand.value + 1];
                        }
                    }
                }
            }
            for (uint32_t index = 0; index < variableCount; ++index)
            {
                useOffsets[index + 1] += useOffsets[index];
            }
            std::vector<uint32_t> useList(useOffsets[variableCount]);
            std::vector<uint32_t> useCursor(useOffsets.begin(), useOffsets.end() - 1);
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    for (const VariableId operand : state.program.operands(instruction))
                    {
                        if (operand.valid() && candidate[operand.value])
                        {
                            useList[useCursor[operand.value]++] = instruction.value;
                        }
                    }
                }
            }
            const auto usesOf = [&](VariableId variable) {
                return std::pair(useOffsets[variable.value], useOffsets[variable.value + 1]);
            };

            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                if (blockIndex >= model.commitBlockBegin && blockIndex < model.commitBlockEnd)
                {
                    continue;
                }
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                const auto blockInstr = [&](std::size_t position) {
                    return model.program.blockInstruction(block, position);
                };
                const auto defInstruction = [&](VariableId variable) {
                    return blockInstr(state.variableDefPosition[variable.value]);
                };
                const auto muxCovered = [&](InstructionId instruction) {
                    return instruction.value < state.instructionMuxRun.size() &&
                           state.instructionMuxRun[instruction.value] >= 0;
                };
                const auto windowClaimed = [&](InstructionId instruction) {
                    return instruction.value < state.instructionWindowAction.size() &&
                           state.instructionWindowAction[instruction.value] >= 0;
                };
                // Cone match anchored at an Or instruction.
                struct ConeMatch
                {
                    InstructionId onehot, notoh, cleared, elemm, placed, merged, zext;
                    bool hasZext = false;
                    InstructionId tail;
                    VariableId result, base, idx, ones, elem, cond;
                    uint32_t elemWidth = 0;
                };
                std::unordered_map<uint32_t, ConeMatch> coneByResult;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId orInstr = blockInstr(position);
                    if (state.program.opcode(orInstr) != Opcode::Or || muxCovered(orInstr) ||
                        windowClaimed(orInstr))
                    {
                        continue;
                    }
                    const auto orOperands = state.program.operands(orInstr);
                    const auto orResults = state.program.results(orInstr);
                    if (orOperands.size() != 2 || orResults.empty())
                    {
                        continue;
                    }
                    const uint32_t width =
                        variableType(state, orResults.front()).bitWidth;
                    if (width < kMinBlendWidth ||
                        variableType(state, orResults.front()).kind !=
                            TypeKind::BitVector)
                    {
                        continue;
                    }
                    // Match both operand orders of (cleared, placed).
                    ConeMatch match;
                    bool matched = false;
                    for (uint32_t order = 0; order < 2 && !matched; ++order)
                    {
                        const VariableId clearedVar = orOperands[order];
                        const VariableId placedVar = orOperands[1 - order];
                        if (!clearedVar.valid() || !placedVar.valid() ||
                            state.variableDefBlock[clearedVar.value] != blockIndex ||
                            state.variableDefBlock[placedVar.value] != blockIndex)
                        {
                            continue;
                        }
                        const InstructionId andInstr = defInstruction(clearedVar);
                        const InstructionId shlInstr = defInstruction(placedVar);
                        if (state.program.opcode(andInstr) != Opcode::And ||
                            state.program.opcode(shlInstr) != Opcode::Shl ||
                            muxCovered(andInstr) || muxCovered(shlInstr) ||
                            windowClaimed(andInstr) || windowClaimed(shlInstr))
                        {
                            continue;
                        }
                        // placed = shl(elemm, idx)
                        const auto shlOperands = state.program.operands(shlInstr);
                        const VariableId elemmVar = shlOperands[0];
                        const VariableId idxVar = shlOperands[1];
                        // cleared = and(base, notoh)
                        const auto andOperands = state.program.operands(andInstr);
                        VariableId baseVar, notohVar;
                        baseVar = notohVar = VariableId{};
                        for (uint32_t k = 0; k < 2; ++k)
                        {
                            const VariableId operand = andOperands[k];
                            if (operand.valid() &&
                                state.variableDefBlock[operand.value] == blockIndex &&
                                state.program.opcode(defInstruction(operand)) ==
                                    Opcode::Not)
                            {
                                notohVar = operand;
                            }
                            else
                            {
                                baseVar = operand;
                            }
                        }
                        if (!baseVar.valid() || !notohVar.valid() ||
                            variableType(state, baseVar).bitWidth != width ||
                            variableType(state, baseVar).kind != TypeKind::BitVector)
                        {
                            continue;
                        }
                        const InstructionId notInstr = defInstruction(notohVar);
                        if (muxCovered(notInstr) || windowClaimed(notInstr))
                        {
                            continue;
                        }
                        // notoh = not(onehot); onehot = shl(ones, idx)
                        const VariableId onehotVar =
                            state.program.operands(notInstr).front();
                        if (!onehotVar.valid() ||
                            state.variableDefBlock[onehotVar.value] != blockIndex)
                        {
                            continue;
                        }
                        const InstructionId onehotInstr = defInstruction(onehotVar);
                        if (state.program.opcode(onehotInstr) != Opcode::Shl ||
                            muxCovered(onehotInstr) || windowClaimed(onehotInstr))
                        {
                            continue;
                        }
                        const auto onehotOperands = state.program.operands(onehotInstr);
                        const VariableId onesVar = onehotOperands[0];
                        if (onehotOperands[1] != idxVar ||
                            variableType(state, onesVar).bitWidth != width ||
                            variableType(state, onesVar).kind != TypeKind::BitVector)
                        {
                            continue;
                        }
                        // elemm = and(elemSource, ones)
                        if (!elemmVar.valid() ||
                            state.variableDefBlock[elemmVar.value] != blockIndex)
                        {
                            continue;
                        }
                        const InstructionId elemmInstr = defInstruction(elemmVar);
                        if (state.program.opcode(elemmInstr) != Opcode::And ||
                            muxCovered(elemmInstr) || windowClaimed(elemmInstr))
                        {
                            continue;
                        }
                        const auto elemmOperands = state.program.operands(elemmInstr);
                        VariableId elemSource;
                        if (elemmOperands[0] == onesVar)
                        {
                            elemSource = elemmOperands[1];
                        }
                        else if (elemmOperands[1] == onesVar)
                        {
                            elemSource = elemmOperands[0];
                        }
                        else
                        {
                            continue;
                        }
                        // elem: zext assign (narrow -> wide) or direct narrow.
                        VariableId elemVar = elemSource;
                        uint32_t elemWidth =
                            variableType(state, elemSource).bitWidth;
                        InstructionId zextInstr{};
                        bool hasZext = false;
                        if (elemSource.valid() &&
                            state.variableDefBlock[elemSource.value] == blockIndex)
                        {
                            const InstructionId sourceDef = defInstruction(elemSource);
                            if (state.program.opcode(sourceDef) == Opcode::Assign &&
                                !state.program.operands(sourceDef).empty())
                            {
                                const VariableId narrow =
                                    state.program.operands(sourceDef).front();
                                const uint32_t narrowWidth =
                                    variableType(state, narrow).bitWidth;
                                if (narrowWidth <= 64)
                                {
                                    elemVar = narrow;
                                    elemWidth = narrowWidth;
                                    zextInstr = sourceDef;
                                    hasZext = true;
                                }
                            }
                        }
                        if (elemWidth == 0 || elemWidth > 64)
                        {
                            continue;
                        }
                        match.onehot = onehotInstr;
                        match.notoh = notInstr;
                        match.cleared = andInstr;
                        match.elemm = elemmInstr;
                        match.placed = shlInstr;
                        match.merged = orInstr;
                        match.zext = zextInstr;
                        match.hasZext = hasZext;
                        match.base = baseVar;
                        match.idx = idxVar;
                        match.ones = onesVar;
                        match.elem = elemVar;
                        match.elemWidth = elemWidth;
                        match.cond = VariableId{};
                        matched = true;
                    }
                    if (!matched)
                    {
                        continue;
                    }
                    // Conditional tail: mux(cond, merged, base) as the sole
                    // merged consumer turns the cone conditional.
                    match.tail = orInstr;
                    match.result = orResults.front();
                    const auto [ub, ue] = usesOf(match.result);
                    if (ue - ub == 1)
                    {
                        const InstructionId only{useList[ub]};
                        if (instructionBlock[only.value] == blockIndex &&
                            !muxCovered(only) && !windowClaimed(only) &&
                            state.program.opcode(only) == Opcode::Mux)
                        {
                            const auto muxOperands = state.program.operands(only);
                            if (muxOperands.size() == 3 &&
                                muxOperands[1] == match.result &&
                                muxOperands[2] == match.base)
                            {
                                match.tail = only;
                                match.result =
                                    state.program.results(only).front();
                                match.cond = muxOperands[0];
                            }
                        }
                    }
                    else if (ue - ub > 1)
                    {
                        continue; // merged fans out: cannot skip the or
                    }
                    coneByResult.emplace(match.result.value, std::move(match));
                }
                if (coneByResult.empty())
                {
                    continue;
                }
                // Internal closure: every internal result's consumers must
                // stay inside the cone.
                std::unordered_set<uint32_t> bailedResults;
                for (const auto &[resultVar, cone] : coneByResult)
                {
                    std::unordered_set<uint32_t> internalInstrs;
                    internalInstrs.insert(cone.onehot.value);
                    internalInstrs.insert(cone.notoh.value);
                    internalInstrs.insert(cone.cleared.value);
                    internalInstrs.insert(cone.elemm.value);
                    internalInstrs.insert(cone.placed.value);
                    internalInstrs.insert(cone.merged.value);
                    internalInstrs.insert(cone.tail.value);
                    if (cone.hasZext)
                    {
                        internalInstrs.insert(cone.zext.value);
                    }
                    bool closed = true;
                    const InstructionId internalResults[6] = {
                        cone.onehot, cone.notoh, cone.cleared,
                        cone.elemm, cone.placed, cone.merged};
                    for (const InstructionId internal : internalResults)
                    {
                        const VariableId internalResult =
                            state.program.results(internal).front();
                        if (internalResult == cone.result)
                        {
                            continue;
                        }
                        const auto [ub, ue] = usesOf(internalResult);
                        for (uint32_t use = ub; use < ue && closed; ++use)
                        {
                            if (internalInstrs.count(useList[use]) == 0)
                            {
                                closed = false;
                            }
                        }
                        if (!closed)
                        {
                            break;
                        }
                    }
                    if (cone.hasZext)
                    {
                        const VariableId zextResult =
                            state.program.results(cone.zext).front();
                        const auto [ub, ue] = usesOf(zextResult);
                        for (uint32_t use = ub; use < ue && closed; ++use)
                        {
                            if (internalInstrs.count(useList[use]) == 0)
                            {
                                closed = false;
                            }
                        }
                    }
                    if (!closed)
                    {
                        bailedResults.insert(resultVar);
                        ++state.dynBlendBailed;
                    }
                }
                // Chain linking: cone A.base == cone B.result.
                std::unordered_map<uint32_t, uint32_t> childOf;
                std::unordered_set<uint32_t> blocked;
                for (const auto &[resultVar, cone] : coneByResult)
                {
                    if (bailedResults.count(resultVar) != 0)
                    {
                        continue;
                    }
                    if (coneByResult.count(cone.base.value) == 0 ||
                        bailedResults.count(cone.base.value) != 0)
                    {
                        continue;
                    }
                    const auto [it, inserted] =
                        childOf.emplace(cone.base.value, resultVar);
                    if (!inserted)
                    {
                        blocked.insert(cone.base.value);
                        blocked.insert(it->second);
                        blocked.insert(resultVar);
                    }
                }
                for (const auto &[firstVar, firstCone] : coneByResult)
                {
                    if (bailedResults.count(firstVar) != 0 ||
                        blocked.count(firstVar) != 0)
                    {
                        continue;
                    }
                    // Chain start: its base is not another cone's result.
                    if (coneByResult.count(firstCone.base.value) != 0 &&
                        bailedResults.count(firstCone.base.value) == 0)
                    {
                        continue;
                    }
                    std::vector<uint32_t> members; // result vars, cone order
                    members.push_back(firstVar);
                    bool okChain = true;
                    while (okChain)
                    {
                        const auto childIt = childOf.find(members.back());
                        if (childIt == childOf.end())
                        {
                            break;
                        }
                        if (blocked.count(childIt->second) != 0 ||
                            bailedResults.count(childIt->second) != 0)
                        {
                            okChain = false;
                            break;
                        }
                        members.push_back(childIt->second);
                    }
                    if (!okChain)
                    {
                        ++state.dynBlendBailed;
                        continue;
                    }
                    // Validate intermediate result consumers.
                    bool bail = false;
                    std::vector<std::pair<uint32_t, int8_t>> pending;
                    std::vector<uint32_t> materializeTails;
                    for (std::size_t k = 0; k + 1 < members.size() && !bail; ++k)
                    {
                        const uint32_t resultVar = members[k];
                        const ConeMatch &cone = coneByResult[resultVar];
                        const uint32_t tailPos = instructionPos[cone.tail.value];
                        const uint32_t nextTailPos =
                            instructionPos[coneByResult[members[k + 1]].tail.value];
                        bool materialize = visible[resultVar] != 0;
                        const auto [ub, ue] = usesOf(VariableId{resultVar});
                        for (uint32_t use = ub; use < ue && !bail; ++use)
                        {
                            const uint32_t user = useList[use];
                            if (user == coneByResult[members[k + 1]].cleared.value ||
                                user == coneByResult[members[k + 1]].tail.value)
                            {
                                // Next cone's base read: realized by the
                                // accumulator (cleared is skipped; the
                                // conditional tail's false arm is "no blend").
                                continue;
                            }
                            if (instructionBlock[user] != blockIndex)
                            {
                                materialize = true;
                                continue;
                            }
                            if (muxCovered(InstructionId{user}) ||
                                windowClaimed(InstructionId{user}))
                            {
                                bail = true;
                                break;
                            }
                            const InstructionId userInstr{user};
                            const Opcode userOp = state.program.opcode(userInstr);
                            if (userOp != Opcode::SliceStatic &&
                                userOp != Opcode::SliceDynamic)
                            {
                                materialize = true;
                                continue;
                            }
                            const uint32_t userPos = instructionPos[user];
                            if (userPos <= tailPos)
                            {
                                materialize = true; // reads a previous round
                                continue;
                            }
                            if (userPos >= nextTailPos)
                            {
                                materialize = true; // reads a torn value
                                continue;
                            }
                            pending.emplace_back(user, kDynBlendRemapSlice);
                        }
                        if (materialize)
                        {
                            materializeTails.push_back(cone.tail.value);
                        }
                    }
                    // Final result: same-block early use or mux coverage
                    // would tear the accumulator.
                    {
                        const uint32_t lastVar = members.back();
                        const uint32_t lastPos =
                            instructionPos[coneByResult[lastVar].tail.value];
                        const auto [ub, ue] = usesOf(VariableId{lastVar});
                        for (uint32_t use = ub; use < ue && !bail; ++use)
                        {
                            const uint32_t user = useList[use];
                            if (muxCovered(InstructionId{user}) ||
                                windowClaimed(InstructionId{user}))
                            {
                                bail = true;
                                break;
                            }
                            if (instructionBlock[user] == blockIndex &&
                                instructionPos[user] <= lastPos)
                            {
                                bail = true;
                                break;
                            }
                        }
                    }
                    if (bail)
                    {
                        ++state.dynBlendBailed;
                        continue;
                    }
                    const int32_t planId =
                        static_cast<int32_t>(state.dynBlendPlans.size());
                    EmitState::DynBlendPlan plan;
                    plan.finalVar = VariableId{members.back()};
                    plan.width =
                        variableType(state, VariableId{members.back()}).bitWidth;
                    plan.cones.reserve(members.size());
                    for (const uint32_t memberVar : members)
                    {
                        const ConeMatch &match = coneByResult[memberVar];
                        EmitState::DynBlendPlan::Cone cone;
                        cone.tail = match.tail;
                        cone.result = match.result;
                        cone.base = match.base;
                        cone.idx = match.idx;
                        cone.ones = match.ones;
                        cone.elem = match.elem;
                        cone.elemWidth = match.elemWidth;
                        cone.cond = match.cond;
                        const InstructionId internalInstrs[7] = {
                            match.onehot, match.notoh, match.cleared,
                            match.elemm, match.placed, match.merged,
                            match.hasZext ? match.zext : match.merged};
                        cone.internalCount = match.hasZext ? 7 : 6;
                        for (uint32_t i = 0; i < cone.internalCount; ++i)
                        {
                            cone.internal[i] = internalInstrs[i];
                        }
                        plan.cones.push_back(cone);
                    }
                    for (std::size_t k = 0; k < plan.cones.size(); ++k)
                    {
                        const auto &cone = plan.cones[k];
                        state.instructionDynBlendPlan[cone.tail.value] = planId;
                        state.instructionDynBlendAction[cone.tail.value] =
                            k == 0 ? kDynBlendHead : kDynBlendCone;
                        for (uint32_t i = 0; i < cone.internalCount; ++i)
                        {
                            const uint32_t internal = cone.internal[i].value;
                            if (internal != cone.tail.value &&
                                state.instructionDynBlendAction[internal] < 0)
                            {
                                state.instructionDynBlendAction[internal] =
                                    kDynBlendSkip;
                                ++state.dynBlendSkipped;
                            }
                        }
                    }
                    for (const auto &[user, action] : pending)
                    {
                        state.instructionDynBlendPlan[user] = planId;
                        state.instructionDynBlendAction[user] = action;
                        ++state.dynBlendRemapped;
                    }
                    for (const uint32_t tail : materializeTails)
                    {
                        state.instructionDynBlendMaterialize[tail] = 1;
                    }
                    state.dynBlendChainCount += 1;
                    state.dynBlendConeCount += plan.cones.size();
                    state.dynBlendMaterialized += materializeTails.size();
                    state.dynBlendPlans.push_back(std::move(plan));
                }
            }
        }

        // NO0016 narrow-value storage classification: picks the C storage
        // class (0=uint8_t, 1=uint16_t, 2=uint32_t, 3=uint64_t) of every
        // localizable block value. A value may narrow only when no emission
        // path can take its slot's address or otherwise need a uint64_t
        // slot:
        //  - any instruction with a non-scalar (>64-bit or non-BitVector)
        //    operand/result routes to the word-level helpers
        //    (emitNonScalarInstruction / wide-assign path), which address
        //    narrow participants as uint64_t*;
        //  - NO0013 windowed / NO0014 dynblend planned instructions emit
        //    word-pointer helper calls (insert/replace/slice/blend); the
        //    dynblend cone's named fields (base/idx/ones/elem/cond/result)
        //    are addressed from the cone tail's emission even when they are
        //    not the tail instruction's own operands, so they are pinned
        //    from the plans directly;
        //  - memory/array/system/host opcodes leave the scalar switch
        //    entirely (their local-relevant values already escape; pinned
        //    regardless as belt and braces).
        // Remaining values are scalar-expression-only (valueExpr/resize_value
        // by value), where a narrower unsigned slot is semantics-preserving:
        // producers mask results to the value width (resultAssign) and every
        // expression read masks through resize_value. A missed address-taken
        // case fails the C++ compile (uint8_t* vs uint64_t*), never silently
        // miscompiles. Escape hatch: WOLVRIX_GRHSIM_AM_DISABLE_NARROW_LOCALS
        // pins every value to class 3, reproducing pre-NO0016 output.
        // NO0017 §5 wide-state scalar explode planning (attribute
        // "wideStateExplode", default off — off leaves the emission
        // byte-identical to the pre-NO0017 form). Purely an emit-time
        // representation decision: the scheduled program is untouched
        // (atom edge set invariant). A wide BitVector variable explodes
        // into a per-element scalar array member when every access to it
        // is one of the recognized forms:
        //   read:  constant SliceStatic with a <=64-bit result (its width
        //          and lsb join the element-width gcd), SliceArray with a
        //          narrow index and result width == element width, or
        //          SliceDynamic with a narrow index and result width ==
        //          element width (two-element blend, exact for any
        //          alignment);
        //   write: RegisterWrite*/LatchWrite* on the state — masked with
        //          a constant mask spanning <= 32 elements (per-element
        //          RMW), or unmasked full-width (per-element loop);
        //   watch: changed.* only when ST00013 fused it into the Block's
        //          write-point flags (planScalarWatchGroups);
        //   init:  Undef/Zero/Constant or all-literal Set actions (random
        //          init keeps the pool path so the shared initRandomState
        //          chain order never changes).
        // Any other touch (whole-width read, SliceDynamic, windowed/
        // dynblend plan capture, host-visible port/label, RegisterWrite-
        // DynLane, being an instruction result) keeps the pool path.
        // Element width K is the gcd of every constant slice site's width
        // and lsb; K must divide the state width and be <= 64. Planned
        // after the windowed/dynblend plans (capture is a guard) and
        // before classifyLocalValueStorage; exploded states release their
        // pool reservation (offsets compacted here).
        void planWideStateExplode(const ExecutableModel &model, EmitState &state)
        {
            state.explodedElementWidth.clear();
            if (!state.wideStateExplode)
            {
                return;
            }
            const ProgramView program = state.program;
            const std::size_t variableCount = program.variableCount();
            struct Candidate
            {
                uint64_t gcd = 0; // gcd of every constant slice width and lsb
                uint64_t sliceSites = 0;
                std::vector<uint32_t> arraySliceWidths;
                std::vector<uint32_t> dynamicSliceWidths;
                // (data, mask) variable pairs of the state-write sites;
                // mask invalid for unmasked full-width writes.
                std::vector<std::pair<VariableId, VariableId>> writeSites;
                std::size_t bail = kExplodeBailCount; // first reason (none when == count)
            };
            std::unordered_map<uint32_t, Candidate> candidates;
            const auto noteBail = [&candidates](uint32_t variable, std::size_t reason) {
                Candidate &candidate = candidates[variable];
                if (candidate.bail == kExplodeBailCount)
                {
                    candidate.bail = reason;
                }
            };
            const auto isWideBv = [&state](VariableId variable) {
                const Type &type = state.variableTypes[variable.value];
                return type.kind == TypeKind::BitVector && type.bitWidth > 64;
            };
            // Host-visible values (difftest ports, declared labels) never
            // leave the pool.
            for (const PortBinding &port : model.interface.ports)
            {
                if (port.input.valid())
                {
                    noteBail(port.input.value, kExplodeBailHostVisible);
                }
                if (port.output.valid())
                {
                    noteBail(port.output.value, kExplodeBailHostVisible);
                }
                if (port.outputEnable.valid())
                {
                    noteBail(port.outputEnable.value, kExplodeBailHostVisible);
                }
            }
            for (const VariableLabel &declared : model.interface.declaredVariables)
            {
                noteBail(declared.variable.value, kExplodeBailHostVisible);
            }
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const EmitState::ScalarWatchPlan *scalarPlan =
                    state.blockScalarWatchPlans[blockIndex]
                        ? &*state.blockScalarWatchPlans[blockIndex]
                        : nullptr;
                for (std::size_t position = 0;
                     position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = program.opcode(instruction);
                    const auto operands = program.operands(instruction);
                    const auto results = program.results(instruction);
                    const bool planCaptured =
                        (instruction.value < state.instructionWindowAction.size() &&
                         state.instructionWindowAction[instruction.value] >= 0) ||
                        (instruction.value < state.instructionDynBlendAction.size() &&
                         state.instructionDynBlendAction[instruction.value] >= 0);
                    if (planCaptured)
                    {
                        for (const VariableId operand : operands)
                        {
                            if (isWideBv(operand))
                            {
                                noteBail(operand.value, kExplodeBailPlanCaptured);
                            }
                        }
                        for (const VariableId result : results)
                        {
                            if (isWideBv(result))
                            {
                                noteBail(result.value, kExplodeBailPlanCaptured);
                            }
                        }
                        continue;
                    }
                    for (const VariableId result : results)
                    {
                        if (isWideBv(result))
                        {
                            noteBail(result.value, kExplodeBailResultDefined);
                        }
                    }
                    const StateWriteLayout layout = stateWriteLayout(opcode);
                    for (std::size_t index = 0; index < operands.size(); ++index)
                    {
                        const VariableId operand = operands[index];
                        if (!isWideBv(operand))
                        {
                            continue;
                        }
                        switch (opcode)
                        {
                            case Opcode::SliceStatic:
                            {
                                const uint32_t width =
                                    state.variableTypes[results.front().value].bitWidth;
                                if (width > 64)
                                {
                                    noteBail(operand.value, kExplodeBailWideSlice);
                                    break;
                                }
                                if (width == state.variableTypes[operand.value].bitWidth)
                                {
                                    noteBail(operand.value, kExplodeBailNonSliceRead);
                                    break;
                                }
                                const auto attributes =
                                    program.sliceStaticAttributes(instruction);
                                Candidate &candidate = candidates[operand.value];
                                candidate.gcd = std::gcd(candidate.gcd,
                                                         static_cast<uint64_t>(width));
                                candidate.gcd =
                                    std::gcd(candidate.gcd, attributes->lsb);
                                candidate.sliceSites += 1;
                                break;
                            }
                            case Opcode::SliceArray:
                                if (index == 0)
                                {
                                    const uint32_t width =
                                        state.variableTypes[results.front().value]
                                            .bitWidth;
                                    if (width > 64)
                                    {
                                        noteBail(operand.value, kExplodeBailWideSlice);
                                        break;
                                    }
                                    if (isWideBv(operands[1]))
                                    {
                                        // Element-indexed reads need a
                                        // narrow (scalar) index.
                                        noteBail(operand.value, kExplodeBailDynamicSlice);
                                        break;
                                    }
                                    Candidate &candidate = candidates[operand.value];
                                    candidate.gcd =
                                        std::gcd(candidate.gcd,
                                                 static_cast<uint64_t>(width));
                                    candidate.arraySliceWidths.push_back(width);
                                    candidate.sliceSites += 1;
                                }
                                else
                                {
                                    // The state as a slice index is a
                                    // whole-width read.
                                    noteBail(operand.value, kExplodeBailNonSliceRead);
                                }
                                break;
                            case Opcode::SliceDynamic:
                                if (index == 0 &&
                                    state.variableTypes[results.front().value]
                                            .bitWidth <= 64 &&
                                    !isWideBv(operands[1]))
                                {
                                    // Dynamic-offset reads of one element
                                    // width emit the two-element blend
                                    // form (exact for any alignment).
                                    const uint32_t width =
                                        state.variableTypes[results.front().value]
                                            .bitWidth;
                                    Candidate &candidate = candidates[operand.value];
                                    candidate.gcd =
                                        std::gcd(candidate.gcd,
                                                 static_cast<uint64_t>(width));
                                    candidate.dynamicSliceWidths.push_back(width);
                                    candidate.sliceSites += 1;
                                }
                                else
                                {
                                    noteBail(operand.value, kExplodeBailDynamicSlice);
                                }
                                break;
                            case Opcode::RegisterWriteDynLane:
                                noteBail(operand.value, kExplodeBailDynamicWrite);
                                break;
                            case Opcode::RegisterWrite:
                            case Opcode::RegisterWriteCond:
                            case Opcode::RegisterWriteMask:
                            case Opcode::RegisterWriteCondMask:
                            case Opcode::LatchWrite:
                            case Opcode::LatchWriteCond:
                            case Opcode::LatchWriteMask:
                            case Opcode::LatchWriteCondMask:
                                if (layout.isStateWrite && !layout.memory &&
                                    static_cast<std::size_t>(index) == layout.targetIndex)
                                {
                                    Candidate &candidate = candidates[operand.value];
                                    candidate.writeSites.emplace_back(
                                        operands[layout.dataIndex],
                                        layout.hasMask
                                            ? operands[layout.hasCond ? 1 : 0]
                                            : VariableId::invalid());
                                }
                                else
                                {
                                    // The state as write data/mask is a
                                    // whole-width read of it.
                                    noteBail(operand.value, kExplodeBailNonSliceRead);
                                }
                                break;
                            case Opcode::ChangedAny:
                                if (index == 0 && scalarPlan != nullptr &&
                                    scalarPlan->detectorRaise.count(
                                        static_cast<uint32_t>(position)) != 0)
                                {
                                    // ST00013: the write sites carry the
                                    // change detection; nothing wide is
                                    // emitted for this detector.
                                    break;
                                }
                                noteBail(operand.value, kExplodeBailChangedDetector);
                                break;
                            case Opcode::ChangedPos:
                            case Opcode::ChangedNeg:
                                noteBail(operand.value, kExplodeBailChangedDetector);
                                break;
                            default:
                                noteBail(operand.value, kExplodeBailNonSliceRead);
                                break;
                        }
                    }
                }
            }
            // Init eligibility: random(-seeded) init shares the
            // initRandomState chain, whose statement order is frozen.
            for (auto &[variableValue, candidate] : candidates)
            {
                if (candidate.bail != kExplodeBailCount)
                {
                    continue;
                }
                const VariableRecord &record =
                    program.variable(VariableId{variableValue});
                const InitDescriptor &init = program.init(record.init);
                if (init.kind != InitKind::Actions)
                {
                    continue;
                }
                for (const InitAction &action : program.initActions(record.init))
                {
                    if (action.kind != InitActionKind::Set ||
                        action.expression.kind != InitExprKind::Literal)
                    {
                        candidate.bail = kExplodeBailRandomInit;
                        break;
                    }
                }
            }
            // Decision pass: only states with at least one slice read site
            // are candidates (nothing to scalarize otherwise).
            std::vector<uint32_t> exploded;
            state.explodedElementWidth.assign(variableCount, 0);
            for (auto &[variableValue, candidate] : candidates)
            {
                if (candidate.sliceSites == 0)
                {
                    continue;
                }
                if (candidate.bail != kExplodeBailCount)
                {
                    ++state.explodeBails[candidate.bail];
                    continue;
                }
                const uint32_t width = state.variableTypes[variableValue].bitWidth;
                const uint64_t elemWidth = candidate.gcd;
                if (elemWidth == 0 || elemWidth > 64 ||
                    width % elemWidth != 0 || width / elemWidth < 2)
                {
                    ++state.explodeBails[kExplodeBailSliceWidth];
                    continue;
                }
                bool bailed = false;
                for (const uint32_t arrayWidth : candidate.arraySliceWidths)
                {
                    if (arrayWidth != elemWidth)
                    {
                        ++state.explodeBails[kExplodeBailArraySliceWidth];
                        bailed = true;
                        break;
                    }
                }
                if (bailed)
                {
                    continue;
                }
                for (const uint32_t dynamicWidth : candidate.dynamicSliceWidths)
                {
                    if (dynamicWidth != elemWidth)
                    {
                        ++state.explodeBails[kExplodeBailDynamicSlice];
                        bailed = true;
                        break;
                    }
                }
                if (bailed)
                {
                    continue;
                }
                for (const auto &[data, mask] : candidate.writeSites)
                {
                    (void)data;
                    if (!mask.valid())
                    {
                        continue;
                    }
                    const std::size_t words =
                        (static_cast<std::size_t>(width) + 63U) / 64U;
                    std::optional<std::vector<uint64_t>> maskWords =
                        constantWordsVector(state, mask, words);
                    if (!maskWords)
                    {
                        ++state.explodeBails[kExplodeBailNonconstantMask];
                        bailed = true;
                        break;
                    }
                    uint32_t touched = 0;
                    uint64_t lastElem = std::numeric_limits<uint64_t>::max();
                    for (std::size_t word = 0; word < words; ++word)
                    {
                        uint64_t value = (*maskWords)[word];
                        if (word + 1U == words && width % 64U != 0)
                        {
                            value &= (UINT64_C(1) << (width % 64U)) - UINT64_C(1);
                        }
                        while (value != 0)
                        {
                            const uint32_t bit =
                                static_cast<uint32_t>(std::countr_zero(value));
                            value &= value - UINT64_C(1);
                            const uint64_t elem = (word * 64U + bit) / elemWidth;
                            if (elem != lastElem)
                            {
                                ++touched;
                                lastElem = elem;
                            }
                        }
                    }
                    if (touched > kExplodeMaskElementLimit)
                    {
                        ++state.explodeBails[kExplodeBailMaskElements];
                        bailed = true;
                        break;
                    }
                }
                if (bailed)
                {
                    continue;
                }
                state.explodedElementWidth[variableValue] =
                    static_cast<uint32_t>(elemWidth);
                exploded.push_back(variableValue);
                ++state.explodedStateCount;
                state.explodedElementTotal += width / elemWidth;
            }
            // Cross-state write-data compatibility: a write whose data is
            // itself exploded needs matching element widths (element loads
            // replace the word extract).
            for (const uint32_t variableValue : exploded)
            {
                Candidate &candidate = candidates.at(variableValue);
                const uint32_t elemWidth = state.explodedElementWidth[variableValue];
                for (const auto &[data, mask] : candidate.writeSites)
                {
                    (void)mask;
                    if (isExplodedState(state, data) &&
                        state.explodedElementWidth[data.value] != elemWidth)
                    {
                        state.explodedElementWidth[variableValue] = 0;
                        state.explodedElementTotal -=
                            state.variableTypes[variableValue].bitWidth / elemWidth;
                        --state.explodedStateCount;
                        ++state.explodeBails[kExplodeBailWriteData];
                        break;
                    }
                }
            }
            // Exploded states release their pool reservation; compact the
            // remaining wide offsets so wideValues_ shrinks accordingly.
            if (state.explodedStateCount != 0)
            {
                uint64_t wideWords = 0;
                for (uint32_t index = 0; index < variableCount; ++index)
                {
                    const Type &type = state.variableTypes[index];
                    if ((type.kind != TypeKind::BitVector || type.bitWidth <= 64) &&
                        type.kind != TypeKind::Array)
                    {
                        continue;
                    }
                    EmitState::Storage &storage = state.variableStorage[index];
                    const uint64_t elements =
                        type.kind == TypeKind::Array ? type.elementCount : 1U;
                    if (state.explodedElementWidth[index] != 0)
                    {
                        state.explodedReclaimedWords += storage.wordCount * elements;
                        storage.offset = std::numeric_limits<uint64_t>::max();
                        continue;
                    }
                    storage.offset = wideWords;
                    wideWords += static_cast<uint64_t>(storage.wordCount) * elements;
                }
                state.wideWords = wideWords;
            }
        }

        void classifyLocalValueStorage(const ExecutableModel &model, EmitState &state)
        {
            const std::size_t variableCount = state.program.variableCount();
            state.localValueClasses.assign(variableCount, 3);
            state.narrowLocalPinned = 0;
            state.disableWideSliceInline =
                std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_WIDE_SLICE_INLINE") != nullptr;
            state.disableMaskedWriteUnroll =
                std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_MASKED_WRITE_UNROLL") != nullptr;
            if (std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_NARROW_LOCALS") != nullptr)
            {
                return;
            }
            std::vector<uint8_t> pinWide(variableCount, 0);
            const auto pinAll = [&](std::span<const VariableId> variables) {
                for (const VariableId variable : variables)
                {
                    pinWide[variable.value] = 1;
                }
            };
            const auto isNonScalar = [&](VariableId variable) {
                const Type &type = state.variableTypes[variable.value];
                return type.kind != TypeKind::BitVector || type.bitWidth > 64;
            };
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0;
                     position < model.program.blockSize(block); ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const auto operands = state.program.operands(instruction);
                    const auto results = state.program.results(instruction);
                    bool pin = opcode == Opcode::MemoryRead ||
                               opcode == Opcode::MemoryReadAll ||
                               opcode == Opcode::MemoryWrite ||
                               opcode == Opcode::MemoryWriteCond ||
                               opcode == Opcode::MemoryWriteMask ||
                               opcode == Opcode::MemoryWriteCondMask ||
                               opcode == Opcode::MemoryFill ||
                               opcode == Opcode::MemoryWriteLanes ||
                               opcode == Opcode::SystemFunction ||
                               opcode == Opcode::SystemTask ||
                               opcode == Opcode::DpiCall ||
                               opcode == Opcode::ArrayMux ||
                               opcode == Opcode::ArrayReduceOr ||
                               opcode == Opcode::ArrayReduceAnd ||
                               opcode == Opcode::ArrayReduceXor ||
                               opcode == Opcode::ArrayBroadcast ||
                               opcode == Opcode::ArrayOnehot ||
                               opcode == Opcode::ArrayReduceLanesOr ||
                               opcode == Opcode::ArrayReduceLanesAnd ||
                               opcode == Opcode::ArrayReduceLanesXor;
                    // NO0018: slice instructions emitted inline
                    // (wideSliceAssign) touch no address: the wide source
                    // stays class 3 by width, the narrow index and the
                    // <=64-bit results are plain by-value scalars. The same
                    // predicate gates the emission paths, so storage class
                    // and emission stay consistent.
                    const auto scalarSliceInline = [&] {
                        if (state.disableWideSliceInline)
                        {
                            return false;
                        }
                        if (opcode != Opcode::SliceStatic &&
                            opcode != Opcode::SliceDynamic &&
                            opcode != Opcode::SliceArray)
                        {
                            return false;
                        }
                        if (std::any_of(results.begin(), results.end(), isNonScalar))
                        {
                            return false;
                        }
                        if (opcode != Opcode::SliceStatic && isNonScalar(operands[1]))
                        {
                            return false;
                        }
                        return true;
                    };
                    const bool sliceInline = scalarSliceInline();
                    if (!pin && instruction.value < state.instructionWindowAction.size() &&
                        state.instructionWindowAction[instruction.value] >= 0 &&
                        !(sliceInline && state.instructionWindowAction[instruction.value] ==
                                             kWindowActionRemapSlice))
                    {
                        pin = true;
                    }
                    if (!pin && instruction.value < state.instructionDynBlendAction.size() &&
                        state.instructionDynBlendAction[instruction.value] >= 0 &&
                        !(sliceInline && state.instructionDynBlendAction[instruction.value] ==
                                             kDynBlendRemapSlice))
                    {
                        pin = true;
                    }
                    if (!pin && !sliceInline)
                    {
                        pin = std::any_of(operands.begin(), operands.end(), isNonScalar) ||
                              std::any_of(results.begin(), results.end(), isNonScalar);
                    }
                    if (pin)
                    {
                        pinAll(operands);
                        pinAll(results);
                    }
                }
            }
            // Dynblend cone named fields are addressed from the tail's
            // emission (blend_window_dyn_words/assign_words) even when not
            // operands of a pinned instruction.
            for (const EmitState::DynBlendPlan &plan : state.dynBlendPlans)
            {
                pinWide[plan.finalVar.value] = 1;
                for (const EmitState::DynBlendPlan::Cone &cone : plan.cones)
                {
                    pinWide[cone.base.value] = 1;
                    pinWide[cone.idx.value] = 1;
                    pinWide[cone.ones.value] = 1;
                    pinWide[cone.elem.value] = 1;
                    pinWide[cone.result.value] = 1;
                    if (cone.cond.valid())
                    {
                        pinWide[cone.cond.value] = 1;
                    }
                }
            }
            for (const EmitState::WindowChainPlan &plan : state.windowChainPlans)
            {
                pinWide[plan.finalVar.value] = 1;
            }
            for (std::size_t variable = 0; variable < variableCount; ++variable)
            {
                if (pinWide[variable] != 0)
                {
                    state.narrowLocalPinned += 1;
                    continue;
                }
                const Type &type = state.variableTypes[variable];
                if (type.kind != TypeKind::BitVector || type.bitWidth > 64)
                {
                    continue;
                }
                if (type.bitWidth <= 8)
                {
                    state.localValueClasses[variable] = 0;
                }
                else if (type.bitWidth <= 16)
                {
                    state.localValueClasses[variable] = 1;
                }
                else if (type.bitWidth <= 32)
                {
                    state.localValueClasses[variable] = 2;
                }
            }
        }

        // NO0016 Stage B chunk-internal scalarization planning: for every
        // oversized (chunked) Block, decide which localized values are read
        // only inside their defining chunk. Those become chunk-function-local
        // typed scalars (register candidates); values read across chunk
        // boundaries keep the shared parent-scope arrays. A value is never
        // internalized when its reads are not anchored at its own instruction
        // position: class-3 (address-taken, see classifyLocalValueStorage),
        // mux-fusion-covered instructions' operands/results (emitted at the
        // run head position), and never-read values (a scalar declaration
        // would trip -Wunused-variable, unlike an array slot). Misclassifying
        // an internal value fails the C++ compile (undeclared identifier in
        // the foreign chunk), never silently. Escape hatch:
        // WOLVRIX_GRHSIM_AM_DISABLE_CHUNK_LOCALS keeps everything shared.
        void planChunkLocalScalars(const ExecutableModel &model, EmitState &state,
                                   std::size_t chunkInstructions)
        {
            const std::size_t variableCount = state.program.variableCount();
            state.chunkScalarIndex.assign(variableCount, kInvalidChunkScalar);
            state.blockChunkScalarCounts.assign(state.blockCount, {});
            state.chunkLocalScalarCount = 0;
            if (std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_NARROW_LOCALS") != nullptr ||
                std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_CHUNK_LOCALS") != nullptr)
            {
                return;
            }
            // Scratch use-analysis arrays, reset per Block via a touched list.
            std::vector<uint32_t> firstUseChunk(variableCount, kInvalidChunkScalar);
            std::vector<uint8_t> useConflict(variableCount, 0);
            std::vector<uint32_t> touched;
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                const std::vector<std::pair<std::size_t, std::size_t>> ranges =
                    blockChunkRanges(blockSize, chunkInstructions,
                                     state.blockCommitGate[blockIndex].headCount);
                if (ranges.empty())
                {
                    continue;
                }
                const auto chunkOf = [&](std::size_t position) {
                    for (std::size_t chunk = 0; chunk < ranges.size(); ++chunk)
                    {
                        if (position < ranges[chunk].second)
                        {
                            return static_cast<uint32_t>(chunk);
                        }
                    }
                    return static_cast<uint32_t>(ranges.size() - 1);
                };
                touched.clear();
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    // Mux-fusion-covered instructions are emitted fused at
                    // the run head position: their operand reads (and result
                    // writes) are not anchored here, so those values must
                    // stay in the shared arrays.
                    const bool muxCovered =
                        instruction.value < state.instructionMuxRun.size() &&
                        state.instructionMuxRun[instruction.value] >= 0;
                    const auto markTouched = [&](VariableId variable) {
                        if (firstUseChunk[variable.value] == kInvalidChunkScalar &&
                            useConflict[variable.value] == 0)
                        {
                            touched.push_back(variable.value);
                        }
                    };
                    if (muxCovered)
                    {
                        for (const VariableId operand :
                             state.program.operands(instruction))
                        {
                            markTouched(operand);
                            useConflict[operand.value] = 1;
                        }
                        for (const VariableId result :
                             state.program.results(instruction))
                        {
                            markTouched(result);
                            useConflict[result.value] = 1;
                        }
                        continue;
                    }
                    for (const VariableId operand :
                         state.program.operands(instruction))
                    {
                        if (state.variableDefBlock[operand.value] != blockIndex ||
                            state.variableEscapeFlags[operand.value] != 0)
                        {
                            continue;
                        }
                        markTouched(operand);
                        if (useConflict[operand.value] != 0)
                        {
                            continue;
                        }
                        const uint32_t useChunk = chunkOf(position);
                        if (firstUseChunk[operand.value] == kInvalidChunkScalar)
                        {
                            firstUseChunk[operand.value] = useChunk;
                        }
                        else if (firstUseChunk[operand.value] != useChunk)
                        {
                            useConflict[operand.value] = 1;
                        }
                    }
                }
                std::vector<std::array<uint32_t, 4>> counts(ranges.size());
                for (const uint32_t variable : touched)
                {
                    const uint32_t firstUse = firstUseChunk[variable];
                    const uint8_t conflict = useConflict[variable];
                    firstUseChunk[variable] = kInvalidChunkScalar;
                    useConflict[variable] = 0;
                    if (conflict != 0 || firstUse == kInvalidChunkScalar ||
                        state.localValueClasses[variable] >= 3)
                    {
                        continue;
                    }
                    const uint32_t defChunk =
                        chunkOf(state.variableDefPosition[variable]);
                    if (firstUse != defChunk)
                    {
                        continue;
                    }
                    const uint8_t storageClass = state.localValueClasses[variable];
                    state.chunkScalarIndex[variable] =
                        counts[defChunk][storageClass]++;
                    state.chunkLocalScalarCount += 1;
                }
                state.blockChunkScalarCounts[blockIndex] = std::move(counts);
            }
        }

        // NO0013 F1/F2: plan windowed emission for lane-build concat chains
        // (F1) and standalone wide concats (F2). See EmitState's
        // WindowChainPlan comment for the semantics. Planning is purely an
        // emit-time code-shape decision: the scheduled program is untouched
        // (atom connectivity invariant by construction). Planned after
        // planMuxFusionRuns so mux-run-covered instructions can be excluded.
        void planWindowedChains(const ExecutableModel &model, EmitState &state)
        {
            constexpr uint32_t kMinChainWidth = 256;
            constexpr std::size_t kMinChainSteps = 3;

            const std::size_t instructionCount = state.program.instructionCount();
            state.instructionWindowPlan.assign(instructionCount, -1);
            state.instructionWindowAction.assign(instructionCount, -1);
            state.instructionWindowMaterialize.assign(instructionCount, 0);
            state.windowChainPlans.clear();
            state.windowedChainCount = 0;
            state.windowedStepCount = 0;
            state.windowedConcatCount = 0;
            state.windowedSkippedSlices = 0;
            state.windowedRemappedSlices = 0;
            state.windowedMaterialized = 0;
            state.windowedBailedChains = 0;
            if (std::getenv("WOLVRIX_GRHSIM_AM_DISABLE_WINDOWED_EMIT") != nullptr)
            {
                return;
            }

            const uint32_t variableCount = state.program.variableCount();
            const uint32_t blockCount = state.blockCount;
            constexpr uint32_t kInvalidVar = std::numeric_limits<uint32_t>::max();

            // Interface-visible variables must keep their own slot contents.
            std::vector<uint8_t> visible(variableCount, 0);
            for (const PortBinding &port : model.interface.ports)
            {
                if (port.input.valid())
                {
                    visible[port.input.value] = 1;
                }
                if (port.output.valid())
                {
                    visible[port.output.value] = 1;
                }
                if (port.outputEnable.valid())
                {
                    visible[port.outputEnable.value] = 1;
                }
            }
            for (const VariableLabel &declared : model.interface.declaredVariables)
            {
                visible[declared.variable.value] = 1;
            }

            // Per-instruction (block, position) plus consumer lists (CSR)
            // over candidate vars = results of SliceStatic or wide Concat.
            std::vector<uint32_t> instructionBlock(instructionCount, kInvalidVar);
            std::vector<uint32_t> instructionPos(instructionCount, 0);
            std::vector<uint8_t> candidate(variableCount, 0);
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    instructionBlock[instruction.value] = blockIndex;
                    instructionPos[instruction.value] = static_cast<uint32_t>(position);
                    const Opcode opcode = state.program.opcode(instruction);
                    const auto results = state.program.results(instruction);
                    if (results.empty())
                    {
                        continue;
                    }
                    if (opcode == Opcode::SliceStatic)
                    {
                        candidate[results.front().value] = 1;
                    }
                    else if (opcode == Opcode::Concat &&
                             variableType(state, results.front()).bitWidth >= kMinChainWidth)
                    {
                        candidate[results.front().value] = 1;
                    }
                }
            }
            std::vector<uint32_t> useOffsets(variableCount + 1, 0);
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    for (const VariableId operand : state.program.operands(instruction))
                    {
                        if (operand.valid() && candidate[operand.value])
                        {
                            ++useOffsets[operand.value + 1];
                        }
                    }
                }
            }
            for (uint32_t index = 0; index < variableCount; ++index)
            {
                useOffsets[index + 1] += useOffsets[index];
            }
            std::vector<uint32_t> useList(useOffsets[variableCount]);
            std::vector<uint32_t> useCursor(useOffsets.begin(), useOffsets.end() - 1);
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < model.program.blockSize(block);
                     ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    for (const VariableId operand : state.program.operands(instruction))
                    {
                        if (operand.valid() && candidate[operand.value])
                        {
                            useList[useCursor[operand.value]++] = instruction.value;
                        }
                    }
                }
            }

            struct StepInfo
            {
                uint32_t pred = kInvalidVar;
                std::vector<std::pair<uint32_t, uint64_t>> elems; // (operand index, offset)
            };
            for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
            {
                if (blockIndex >= model.commitBlockBegin && blockIndex < model.commitBlockEnd)
                {
                    continue;
                }
                const BlockId block{blockIndex};
                const std::size_t blockSize = model.program.blockSize(block);
                const auto blockInstr = [&](std::size_t position) {
                    return model.program.blockInstruction(block, position);
                };
                const auto defInstruction = [&](VariableId variable) {
                    return blockInstr(state.variableDefPosition[variable.value]);
                };
                // Classify wide concats: chain steps carry >= 1 backbone
                // operand (SliceStatic of one in-block concat predecessor,
                // identity-placed); everything else is an element operand.
                std::unordered_map<uint32_t, StepInfo> stepByVar;
                for (std::size_t position = 0; position < blockSize; ++position)
                {
                    const InstructionId instruction = blockInstr(position);
                    if (state.program.opcode(instruction) != Opcode::Concat)
                    {
                        continue;
                    }
                    const auto operands = state.program.operands(instruction);
                    const auto results = state.program.results(instruction);
                    const uint32_t width = variableType(state, results.front()).bitWidth;
                    if (width < kMinChainWidth)
                    {
                        continue;
                    }
                    if (instruction.value < state.instructionMuxRun.size() &&
                        state.instructionMuxRun[instruction.value] >= 0)
                    {
                        continue;
                    }
                    StepInfo info;
                    bool ok = true;
                    uint64_t offset = width;
                    for (uint32_t index = 0; index < operands.size() && ok; ++index)
                    {
                        const VariableId operand = operands[index];
                        const uint32_t operandWidth = variableType(state, operand).bitWidth;
                        offset -= operandWidth;
                        bool isBackbone = false;
                        if (operand.valid() &&
                            state.variableDefBlock[operand.value] == blockIndex &&
                            (state.variableEscapeFlags[operand.value] & kEscapeEarlyUse) == 0)
                        {
                            const InstructionId sliceInstr = defInstruction(operand);
                            if (state.program.opcode(sliceInstr) == Opcode::SliceStatic &&
                                (sliceInstr.value >= state.instructionMuxRun.size() ||
                                 state.instructionMuxRun[sliceInstr.value] < 0))
                            {
                                const auto attributes =
                                    state.program.sliceStaticAttributes(sliceInstr);
                                const VariableId source =
                                    state.program.operands(sliceInstr).front();
                                if (attributes && attributes->lsb == offset &&
                                    source.valid() &&
                                    variableType(state, source).bitWidth == width &&
                                    state.variableDefBlock[source.value] == blockIndex)
                                {
                                    const InstructionId predInstr = defInstruction(source);
                                    if (state.program.opcode(predInstr) == Opcode::Concat &&
                                        (predInstr.value >= state.instructionMuxRun.size() ||
                                         state.instructionMuxRun[predInstr.value] < 0))
                                    {
                                        isBackbone = true;
                                        if (info.pred != kInvalidVar &&
                                            info.pred != source.value)
                                        {
                                            ok = false; // two concat predecessors
                                        }
                                        info.pred = source.value;
                                    }
                                }
                            }
                        }
                        if (!isBackbone)
                        {
                            info.elems.emplace_back(index, offset);
                        }
                    }
                    if (ok && info.pred != kInvalidVar)
                    {
                        stepByVar.emplace(results.front().value, std::move(info));
                    }
                }
                if (!stepByVar.empty())
                {
                // Link steps into chains; branching predecessors block both
                // sides (kept conservative, windowing bails on such chains).
                std::unordered_map<uint32_t, uint32_t> childOf;
                std::unordered_set<uint32_t> blocked;
                for (const auto &[resultVar, info] : stepByVar)
                {
                    const auto [it, inserted] = childOf.emplace(info.pred, resultVar);
                    if (!inserted)
                    {
                        blocked.insert(info.pred);
                        blocked.insert(it->second);
                        blocked.insert(resultVar);
                    }
                }
                for (const auto &[firstVar, firstInfo] : stepByVar)
                {
                    if (stepByVar.count(firstInfo.pred) != 0)
                    {
                        continue; // mid-chain step; the head's walk covers it
                    }
                    std::vector<uint32_t> members;
                    members.push_back(firstInfo.pred);
                    bool okChain = blocked.count(firstInfo.pred) == 0;
                    while (okChain)
                    {
                        const auto childIt = childOf.find(members.back());
                        if (childIt == childOf.end())
                        {
                            break;
                        }
                        if (blocked.count(childIt->second) != 0)
                        {
                            okChain = false;
                            break;
                        }
                        members.push_back(childIt->second);
                    }
                    if (!okChain || members.size() - 1 < kMinChainSteps)
                    {
                        if (!okChain)
                        {
                            ++state.windowedBailedChains;
                        }
                        continue;
                    }
                    const uint32_t width =
                        variableType(state, VariableId{members.back()}).bitWidth;
                    // Validate consumers of every non-final member.
                    bool bail = false;
                    bool memberMaterialize = false;
                    std::vector<std::pair<uint32_t, int8_t>> pending;
                    std::vector<uint32_t> materializeInstrs;
                    for (std::size_t j = 0; j + 1 < members.size() && !bail; ++j)
                    {
                        const uint32_t memberVar = members[j];
                        const uint32_t memberPos = state.variableDefPosition[memberVar];
                        const uint32_t nextVar = members[j + 1];
                        const uint32_t nextInstr =
                            defInstruction(VariableId{nextVar}).value;
                        bool materialize = visible[memberVar] != 0;
                        for (uint32_t use = useOffsets[memberVar];
                             use < useOffsets[memberVar + 1] && !bail; ++use)
                        {
                            const uint32_t user = useList[use];
                            if (user == nextInstr)
                            {
                                continue;
                            }
                            if (instructionBlock[user] != blockIndex)
                            {
                                materialize = true;
                                continue;
                            }
                            if (user < state.instructionMuxRun.size() &&
                                state.instructionMuxRun[user] >= 0)
                            {
                                bail = true; // fused emission moves positions
                                break;
                            }
                            const InstructionId userInstr{user};
                            if (state.program.opcode(userInstr) != Opcode::SliceStatic)
                            {
                                materialize = true;
                                continue;
                            }
                            const uint32_t userResult =
                                state.program.results(userInstr).front().value;
                            const uint32_t userUsesBegin = useOffsets[userResult];
                            const uint32_t userUsesEnd = useOffsets[userResult + 1];
                            if (userUsesEnd - userUsesBegin == 1 &&
                                useList[userUsesBegin] == nextInstr)
                            {
                                pending.emplace_back(user, kWindowActionSkip);
                                continue;
                            }
                            const uint32_t userPos = instructionPos[user];
                            if (userPos <= memberPos)
                            {
                                materialize = true; // reads a previous round
                                continue;
                            }
                            const auto attributes =
                                state.program.sliceStaticAttributes(userInstr);
                            const uint64_t sliceLsb = attributes->lsb;
                            const uint64_t sliceWidth =
                                variableType(state, VariableId{userResult}).bitWidth;
                            bool remap = true;
                            for (std::size_t m = j + 1; m < members.size(); ++m)
                            {
                                const StepInfo &stepInfo = stepByVar[members[m]];
                                const uint32_t stepPos =
                                    state.variableDefPosition[members[m]];
                                const InstructionId stepInstr =
                                    defInstruction(VariableId{members[m]});
                                bool touches = false;
                                for (const auto &[operandIndex, elemOffset] :
                                     stepInfo.elems)
                                {
                                    const uint64_t elemWidth =
                                        variableType(state,
                                                     state.program.operands(
                                                         stepInstr)[operandIndex])
                                            .bitWidth;
                                    if (elemOffset < sliceLsb + sliceWidth &&
                                        sliceLsb < elemOffset + elemWidth)
                                    {
                                        touches = true;
                                        break;
                                    }
                                }
                                if (touches)
                                {
                                    remap = userPos < stepPos;
                                    break;
                                }
                            }
                            if (remap)
                            {
                                pending.emplace_back(user, kWindowActionRemapSlice);
                            }
                            else
                            {
                                materialize = true;
                            }
                        }
                        if (materialize)
                        {
                            memberMaterialize = true;
                            materializeInstrs.push_back(
                                defInstruction(VariableId{memberVar}).value);
                        }
                    }
                    // The final member's slot accumulates from the head's
                    // position on: a same-block read at or before its own
                    // definition (or a mux-run-moved read) would tear.
                    const uint32_t lastVar = members.back();
                    const uint32_t lastPos = state.variableDefPosition[lastVar];
                    for (uint32_t use = useOffsets[lastVar];
                         use < useOffsets[lastVar + 1] && !bail; ++use)
                    {
                        const uint32_t user = useList[use];
                        if (user < state.instructionMuxRun.size() &&
                            state.instructionMuxRun[user] >= 0)
                        {
                            bail = true;
                            break;
                        }
                        if (instructionBlock[user] == blockIndex &&
                            instructionPos[user] <= lastPos)
                        {
                            bail = true;
                            break;
                        }
                    }
                    if (bail)
                    {
                        ++state.windowedBailedChains;
                        continue;
                    }
                    const int32_t planId =
                        static_cast<int32_t>(state.windowChainPlans.size());
                    EmitState::WindowChainPlan plan;
                    plan.finalVar = VariableId{lastVar};
                    plan.width = width;
                    plan.steps.reserve(members.size() - 1);
                    for (std::size_t j = 1; j < members.size(); ++j)
                    {
                        const InstructionId stepInstr =
                            defInstruction(VariableId{members[j]});
                        EmitState::WindowChainPlan::Step step;
                        step.instruction = stepInstr;
                        step.elems = stepByVar[members[j]].elems;
                        plan.stepIndexByInstr.emplace(
                            stepInstr.value,
                            static_cast<uint32_t>(plan.steps.size()));
                        plan.steps.push_back(std::move(step));
                    }
                    const InstructionId headInstr =
                        defInstruction(VariableId{members.front()});
                    state.instructionWindowPlan[headInstr.value] = planId;
                    state.instructionWindowAction[headInstr.value] =
                        kWindowActionChainHead;
                    for (const auto &step : plan.steps)
                    {
                        state.instructionWindowPlan[step.instruction.value] = planId;
                        state.instructionWindowAction[step.instruction.value] =
                            kWindowActionChainStep;
                    }
                    for (const auto &[user, action] : pending)
                    {
                        state.instructionWindowPlan[user] = planId;
                        state.instructionWindowAction[user] = action;
                        if (action == kWindowActionSkip)
                        {
                            ++state.windowedSkippedSlices;
                        }
                        else
                        {
                            ++state.windowedRemappedSlices;
                        }
                    }
                    for (const uint32_t instr : materializeInstrs)
                    {
                        state.instructionWindowMaterialize[instr] = 1;
                    }
                    state.windowedChainCount += 1;
                    state.windowedStepCount += plan.steps.size();
                    state.windowedMaterialized += materializeInstrs.size();
                    (void)memberMaterialize;
                    state.windowChainPlans.push_back(std::move(plan));
                }
                }
                // F2 (standalone wide concat -> plain window replaces) was
                // REVERTED after measurement: replace-RMW costs more per
                // word than the stock zero_words fill + OR-insert
                // (b77703 +24.6%, b38653 +5.2%), so standalone concats keep
                // the stock emission. The kWindowConcat case remains in
                // emitWindowedInstruction for reference.
            }
        }

        void planMuxFusionRuns(const ExecutableModel &model, EmitState &state)
        {
            state.instructionMuxRun.assign(state.program.instructionCount(), -1);
            state.muxRunPlans.clear();
            state.muxAtomFusedCount = 0;
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const BlockId block{blockIndex};
                std::vector<InstructionId> runPreamble;
                std::vector<InstructionId> runArms;
                std::unordered_set<uint32_t> runRootResults;
                uint32_t runSelect = kInvalidAtomSignature;
                constexpr uint32_t kInvalidRunAtom =
                    std::numeric_limits<uint32_t>::max();
                uint32_t runFirstAtom = kInvalidRunAtom;
                uint32_t runLastAtom = kInvalidRunAtom;
                const auto flushRun = [&]() {
                    if (runArms.size() >= 2)
                    {
                        EmitState::MuxRunPlan plan;
                        plan.head = (runPreamble.empty() ? runArms.front()
                                                         : runPreamble.front())
                                        .value;
                        plan.select = VariableId{runSelect};
                        plan.preamble = runPreamble;
                        plan.arms = runArms;
                        plan.firstAtom = runFirstAtom;
                        plan.lastAtom = runLastAtom;
                        const int32_t planId =
                            static_cast<int32_t>(state.muxRunPlans.size());
                        for (const InstructionId member : plan.preamble)
                        {
                            state.instructionMuxRun[member.value] = planId;
                        }
                        for (const InstructionId member : plan.arms)
                        {
                            state.instructionMuxRun[member.value] = planId;
                        }
                        state.muxAtomFusedCount += plan.arms.size();
                        state.muxRunPlans.push_back(std::move(plan));
                    }
                    runPreamble.clear();
                    runArms.clear();
                    runRootResults.clear();
                    runSelect = kInvalidAtomSignature;
                    runFirstAtom = kInvalidRunAtom;
                    runLastAtom = kInvalidRunAtom;
                };
                for (std::size_t atomIndex = 0;
                     atomIndex < model.program.blockAtomCount(block); ++atomIndex)
                {
                    const AtomId atom = model.program.blockAtom(block, atomIndex);
                    const AmAtomKind kind = model.program.atomKind(atom);
                    const std::size_t memberCount = model.program.atomInstructionCount(atom);
                    bool eligible = (kind == AmAtomKind::Singleton ||
                                     kind == AmAtomKind::Tree) &&
                                    memberCount > 0;
                    InstructionId root;
                    uint32_t select = kInvalidAtomSignature;
                    if (eligible)
                    {
                        root = model.program.atomInstruction(atom, memberCount - 1);
                        const auto operands = state.program.operands(root);
                        eligible = state.program.opcode(root) == Opcode::Mux &&
                                   operands.size() == 3 && operands[0].valid();
                        if (eligible)
                        {
                            select = operands[0].value;
                        }
                    }
                    if (!eligible)
                    {
                        flushRun();
                        continue;
                    }
                    if (select != runSelect)
                    {
                        flushRun();
                        runSelect = select;
                    }
                    // Cone safety: a cone member reading an earlier run
                    // root's result would move its def after this use.
                    bool coneUnsafe = false;
                    for (std::size_t member = 0; member + 1 < memberCount && !coneUnsafe;
                         ++member)
                    {
                        const InstructionId coneInstruction =
                            model.program.atomInstruction(atom, member);
                        for (const VariableId operand :
                             state.program.operands(coneInstruction))
                        {
                            if (operand.valid() &&
                                runRootResults.count(operand.value) != 0)
                            {
                                coneUnsafe = true;
                                break;
                            }
                        }
                    }
                    if (coneUnsafe)
                    {
                        flushRun();
                        runSelect = select;
                    }
                    if (runFirstAtom == kInvalidRunAtom)
                    {
                        runFirstAtom = atom.value;
                    }
                    runLastAtom = atom.value;
                    for (std::size_t member = 0; member + 1 < memberCount; ++member)
                    {
                        runPreamble.push_back(
                            model.program.atomInstruction(atom, member));
                    }
                    runArms.push_back(root);
                    const auto rootResults = state.program.results(root);
                    if (!rootResults.empty() && rootResults.front().valid())
                    {
                        runRootResults.insert(rootResults.front().value);
                    }
                }
                flushRun();
            }
        }
    } // namespace

    GrhSimAmCppResult
    GrhSimAmCppEmitter::emit(const ExecutableModel &model,
                            const GrhSimAmCppOptions &options,
                            wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        GrhSimAmCppResult result{.success = false};
        if (diagnostics.hasError())
        {
            return result;
        }
        if (options.outputDirectory.empty())
        {
            diagnostics.error("AM C++ emitter requires a non-empty output directory",
                              std::string(kContext));
            return result;
        }
        if (!isCppIdentifier(options.modelName))
        {
            diagnostics.error("AM C++ emitter modelName must be a non-keyword C++ identifier",
                              std::string(kContext));
            return result;
        }
        const ValidationResult validation =
            validate(model, ValidationOptions{.level = ValidationLevel::Semantic});
        if (!validation.success())
        {
            for (const std::string &validationError : validation.errors)
            {
                diagnostics.error(validationError, std::string(kContext));
            }
            return result;
        }

        const ProgramView program = model.program.view();
        EmitState state{.program = program};
        state.blockCount = static_cast<uint32_t>(model.program.blockCount());
        const auto runtimeProfileAttribute = options.attributes.find("runtimeProfile");
        state.runtimeProfile = runtimeProfileAttribute != options.attributes.end() &&
                               runtimeProfileAttribute->second == "true";
        const auto fullEvaluationAttribute = options.attributes.find("fullEvaluation");
        state.fullEvaluation = fullEvaluationAttribute != options.attributes.end() &&
                               fullEvaluationAttribute->second == "true";
        const auto changedTraceAttribute = options.attributes.find("changedTrace");
        state.changedTrace = changedTraceAttribute != options.attributes.end() &&
                             changedTraceAttribute->second == "true";
        const auto branchyMuxAttribute = options.attributes.find("branchyMux");
        state.branchyMux = branchyMuxAttribute != options.attributes.end() &&
                           branchyMuxAttribute->second == "true";
        // NO0017 §5: wide-state scalar explode (default off).
        const auto wideStateExplodeAttribute = options.attributes.find("wideStateExplode");
        state.wideStateExplode = wideStateExplodeAttribute != options.attributes.end() &&
                                 wideStateExplodeAttribute->second == "true";
        // Guard-event gating of pure fatal-guard compute Blocks (default off).
        const auto guardEventGatingAttribute = options.attributes.find("guardEventGating");
        state.guardEventGating = guardEventGatingAttribute != options.attributes.end() &&
                                 guardEventGatingAttribute->second == "true";
        const auto commitInputGatingAttribute =
            options.attributes.find("commitInputGating");
        state.commitInputGating =
            commitInputGatingAttribute != options.attributes.end() &&
            commitInputGatingAttribute->second == "true";
        const auto commitInputSparseGatingAttribute =
            options.attributes.find("commitInputSparseGating");
        state.commitInputSparseGating =
            commitInputSparseGatingAttribute != options.attributes.end() &&
            commitInputSparseGatingAttribute->second == "true";
        state.traceComments = options.traceComments;
        state.variableTypes.reserve(program.variableCount());
        state.variableStorage.resize(program.variableCount());
        for (uint32_t index = 0; index < program.variableCount(); ++index)
        {
            const Type &type = program.type(program.variable(VariableId{index}).type);
            if ((type.kind == TypeKind::BitVector && type.bitWidth == 0) ||
                (type.kind == TypeKind::Array &&
                 (type.bitWidth == 0 || type.elementCount == 0)))
            {
                diagnostics.error(
                    "AM C++ emitter encountered an invalid zero-sized variable: variable=" +
                    std::to_string(index),
                    std::string(kContext));
                return result;
            }
            EmitState::Storage &storage = state.variableStorage[index];
            if ((type.kind == TypeKind::BitVector && type.bitWidth > 64) ||
                type.kind == TypeKind::Array)
            {
                const uint64_t words = (static_cast<uint64_t>(type.bitWidth) + 63U) / 64U;
                const uint64_t elements = type.kind == TypeKind::Array ? type.elementCount : 1U;
                if (words > std::numeric_limits<uint64_t>::max() / elements ||
                    state.wideWords > std::numeric_limits<uint64_t>::max() - words * elements)
                {
                    diagnostics.error("AM C++ emitter wide storage size overflow: variable=" +
                                          std::to_string(index),
                                      std::string(kContext));
                    return result;
                }
                storage.offset = state.wideWords;
                storage.wordCount = static_cast<uint32_t>(words);
                state.wideWords += words * elements;
            }
            else if (type.kind == TypeKind::Real)
            {
                storage.offset = state.realValues++;
                storage.wordCount = 1;
            }
            else if (type.kind == TypeKind::String)
            {
                storage.offset = state.stringValues++;
            }
            const InitDescriptor &init = program.init(program.variable(VariableId{index}).init);
            if (init.kind == InitKind::Actions)
            {
                for (const InitAction &action :
                     program.initActions(program.variable(VariableId{index}).init))
                {
                    if (action.kind == InitActionKind::Load)
                    {
                        diagnostics.error(
                            "AM C++ emitter does not support Array Load initialization: variable=" +
                                std::to_string(index),
                            std::string(kContext));
                        return result;
                    }
                    if ((type.kind == TypeKind::Array &&
                         action.kind != InitActionKind::Fill) ||
                        (type.kind != TypeKind::Array &&
                         action.kind != InitActionKind::Set))
                    {
                        diagnostics.error(
                            "AM C++ emitter encountered an init action incompatible with its target: variable=" +
                                std::to_string(index),
                            std::string(kContext));
                        return result;
                    }
                    if ((type.kind == TypeKind::Real || type.kind == TypeKind::String) &&
                        action.expression.kind != InitExprKind::Literal)
                    {
                        diagnostics.error(
                            "AM C++ emitter random initialization requires a bit-vector target: variable=" +
                                std::to_string(index),
                            std::string(kContext));
                        return result;
                    }
                }
            }
            state.variableTypes.push_back(type);
        }

        state.referencedDpiImports.assign(program.dpiImportCount(), false);
        for (uint32_t index = 0; index < program.dpiImportCount(); ++index)
        {
            const DpiImportId id{index};
            const DpiImportView import = program.dpiImport(id);
            state.dpiImportBySymbol.emplace(import.symbol.value, id);
        }
        for (uint32_t index = 0; index < program.instructionCount(); ++index)
        {
            const InstructionId instruction{index};
            if (program.opcode(instruction) == Opcode::SystemTask)
            {
                const auto attributes = program.systemTaskAttributes(instruction);
                if (attributes && attributes->schedule == CallSchedule::Once)
                {
                    state.onceSlotByInstruction.emplace(index, state.onceSlotCount++);
                }
                if (attributes && attributes->eventCount != 0 &&
                    attributes->eventMode == HostEventMode::Pending)
                {
                    state.pendingEventSlotByInstruction.emplace(
                        index, state.pendingEventSlotCount++);
                }
            }
            else if (program.opcode(instruction) == Opcode::DpiCall)
            {
                const auto attributes = program.dpiCallAttributes(instruction);
                if (!attributes)
                {
                    diagnostics.error("dpi.call is missing required attributes: instruction=" +
                                          std::to_string(index),
                                      std::string(kContext));
                    return result;
                }
                const auto importIt =
                    state.dpiImportBySymbol.find(attributes->importSymbol.value);
                if (importIt == state.dpiImportBySymbol.end())
                {
                    diagnostics.error("dpi.call references an unknown import: instruction=" +
                                          std::to_string(index),
                                      std::string(kContext));
                    return result;
                }
                if (attributes->eventCount != 0 &&
                    attributes->eventMode == HostEventMode::Pending)
                {
                    state.pendingEventSlotByInstruction.emplace(
                        index, state.pendingEventSlotCount++);
                }
                state.referencedDpiImports[importIt->second.value] = true;
            }
        }
        for (uint32_t blockIndex = 0; blockIndex < model.program.blockCount(); ++blockIndex)
        {
            const BlockId block{blockIndex};
            for (std::size_t position = 0; position < model.program.blockSize(block); ++position)
            {
                const InstructionId instruction =
                    model.program.blockInstruction(block, position);
                if (program.opcode(instruction) != Opcode::SystemTask)
                {
                    continue;
                }
                const auto attributes = program.systemTaskAttributes(instruction);
                if (attributes && attributes->schedule == CallSchedule::Final)
                {
                    state.finalSystemTasks.push_back(instruction);
                }
            }
        }

        for (uint32_t index = 0; index < program.dpiImportCount(); ++index)
        {
            if (!state.referencedDpiImports[index])
            {
                continue;
            }
            const DpiImportView import = program.dpiImport(DpiImportId{index});
            const std::string symbol(program.string(import.symbol));
            if (!isCppIdentifier(symbol))
            {
                diagnostics.error("AM C++ emitter DPI symbol is not a C++ identifier: " + symbol,
                                  std::string(kContext));
                return result;
            }
            for (std::size_t parameterIndex = 0;
                 parameterIndex < import.parameters.size();
                 ++parameterIndex)
            {
                const DpiParameter &parameter = import.parameters[parameterIndex];
                std::string error;
                if (!dpiCppType(program.type(parameter.type), parameter.abi, error))
                {
                    diagnostics.error(error + ": import=" + symbol + " parameter=" +
                                          std::to_string(parameterIndex),
                                      std::string(kContext));
                    return result;
                }
                if (parameter.abi == DpiAbiKind::String &&
                    parameter.direction != DpiDirection::Input)
                {
                    diagnostics.error(
                        "AM C++ emitter does not support DPI output/inout String ABI: import=" +
                            symbol,
                        std::string(kContext));
                    return result;
                }
            }
            if (import.returnValue.present)
            {
                std::string error;
                if (!dpiCppType(program.type(import.returnValue.type),
                                import.returnValue.abi,
                                error))
                {
                    diagnostics.error(error + ": import=" + symbol + " return",
                                      std::string(kContext));
                    return result;
                }
                if (import.returnValue.abi == DpiAbiKind::String)
                {
                    diagnostics.error(
                        "AM C++ emitter does not support DPI String return ABI: import=" + symbol,
                        std::string(kContext));
                    return result;
                }
            }
        }

        const auto statsAttribute = options.attributes.find("collectStats");
        if (statsAttribute != options.attributes.end() && statsAttribute->second == "true")
        {
            constexpr std::size_t opcodeCount =
                static_cast<std::size_t>(Opcode::RegisterWriteDynLane) + 1U;
            std::array<uint64_t, opcodeCount> nonScalarOpcodes{};
            for (uint32_t index = 0; index < program.instructionCount(); ++index)
            {
                const InstructionId instruction{index};
                const auto isNonScalar = [&](VariableId variable) {
                    const Type &type = variableType(state, variable);
                    return type.kind != TypeKind::BitVector || type.bitWidth > 64;
                };
                const auto operands = program.operands(instruction);
                const auto results = program.results(instruction);
                if (std::any_of(operands.begin(), operands.end(), isNonScalar) ||
                    std::any_of(results.begin(), results.end(), isNonScalar))
                {
                    ++nonScalarOpcodes[static_cast<std::size_t>(program.opcode(instruction))];
                }
            }
            std::string message =
                "AM C++ emitter storage stats: wide_words=" + std::to_string(state.wideWords) +
                " real_values=" + std::to_string(state.realValues) +
                " string_values=" + std::to_string(state.stringValues) +
                " non_scalar_opcodes=";
            bool first = true;
            for (std::size_t opcode = 0; opcode < nonScalarOpcodes.size(); ++opcode)
            {
                if (nonScalarOpcodes[opcode] == 0)
                {
                    continue;
                }
                if (!first)
                {
                    message += ',';
                }
                first = false;
                message += std::string(toString(static_cast<Opcode>(opcode))) + ':' +
                           std::to_string(nonScalarOpcodes[opcode]);
            }
            diagnostics.info(std::move(message), std::string(kContext));
        }

        std::unordered_set<std::string> portNames;
        for (const PortBinding &port : model.interface.ports)
        {
            const std::string name = sanitizeCppIdentifier(program.string(port.name));
            if (!isCppIdentifier(name) || !portNames.insert(name).second)
            {
                diagnostics.error(
                    "AM C++ emitter requires unique C++ identifier port names: " + name,
                    std::string(kContext));
                return result;
            }
            if (port.direction == PortDirection::Inout)
            {
                diagnostics.error("initial AM C++ emitter does not support inout ports: " + name,
                                  std::string(kContext));
                return result;
            }
            const VariableId variable =
                port.direction == PortDirection::Input ? port.input : port.output;
            if (variableType(state, variable).kind != TypeKind::BitVector)
            {
                diagnostics.error("initial AM C++ emitter supports only bit-vector ports: " + name,
                                  std::string(kContext));
                return result;
            }
        }

        const std::optional<std::size_t> blocksPerSource =
            parseBlocksPerSource(options, diagnostics);
        if (!blocksPerSource)
        {
            return result;
        }
        const std::optional<uint64_t> maxSourceBytes =
            parseMaxSourceBytes(options, diagnostics);
        if (!maxSourceBytes)
        {
            return result;
        }
        const std::optional<uint64_t> maxCommitSourceBytes =
            parseMaxCommitSourceBytes(options, diagnostics);
        if (!maxCommitSourceBytes)
        {
            return result;
        }
        const std::optional<std::size_t> blockChunkInstructions =
            parseBlockChunkInstructions(options, diagnostics);
        if (!blockChunkInstructions)
        {
            return result;
        }
        const std::size_t blockCount = model.program.blockCount();

        // ST00009: escape analysis for block-local value localization. A value keeps
        // its persistent storage (an independent v<VariableId> member, or a
        // changedResults_[] slot for cross-block changed results) unless it is a
        // narrow scalar defined by exactly one instruction and only read later in
        // the same block. Anything the runtime, the host, another block, or a
        // previous round can observe escapes.
        const std::size_t variableCount = program.variableCount();
        state.variableDefBlock.assign(variableCount, kInvalidLocalityBlock);
        state.variableDefPosition.assign(variableCount, 0);
        state.variableEscapeFlags.assign(variableCount, 0);
        state.blockDefinedVariables.assign(blockCount, {});
        state.localValueStamps.assign(variableCount, 0);
        state.localValueIndices.assign(variableCount, 0);
        state.crossBlockChangedResults.assign(variableCount, false);
        std::vector<uint8_t> variableDefCounts(variableCount, 0);
        std::vector<bool> changedResultVariables(variableCount, false);
        for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        {
            const BlockId block{blockIndex};
            for (std::size_t position = 0; position < model.program.blockSize(block); ++position)
            {
                const InstructionId instruction =
                    model.program.blockInstruction(block, position);
                const Opcode opcode = program.opcode(instruction);
                const auto operands = program.operands(instruction);
                const auto results = program.results(instruction);
                const auto escapeOperands = [&] {
                    for (const VariableId operand : operands)
                    {
                        state.variableEscapeFlags[operand.value] |= kEscapeGlobal;
                    }
                };
                const auto escapeResults = [&] {
                    for (const VariableId result : results)
                    {
                        state.variableEscapeFlags[result.value] |= kEscapeGlobal;
                    }
                };
                switch (opcode)
                {
                    case Opcode::ChangedAny:
                    case Opcode::ChangedPos:
                    case Opcode::ChangedNeg:
                        // Watched values persist across rounds; cross-block results
                        // are written by index through set_changed_result.
                        escapeOperands();
                        escapeResults();
                        changedResultVariables[results.front().value] = true;
                        break;
                    case Opcode::RegisterWrite:
                    case Opcode::RegisterWriteCond:
                    case Opcode::RegisterWriteMask:
                    case Opcode::RegisterWriteCondMask:
                    case Opcode::LatchWrite:
                    case Opcode::LatchWriteCond:
                    case Opcode::LatchWriteMask:
                    case Opcode::LatchWriteCondMask:
                    case Opcode::MemoryWrite:
                    case Opcode::MemoryWriteCond:
                    case Opcode::MemoryWriteMask:
                    case Opcode::MemoryWriteCondMask:
                    case Opcode::MemoryFill:
                    case Opcode::MemoryWriteLanes:
                        // State writes keep persistent slots for every operand
                        // (targets are state read again on later rounds).
                        escapeOperands();
                        break;
                    case Opcode::MemoryRead:
                    case Opcode::MemoryReadAll:
                        escapeOperands();
                        break;
                    case Opcode::SystemFunction:
                    case Opcode::SystemTask:
                    case Opcode::DpiCall:
                        // Host-visible values; Final tasks are also read by finalize().
                        escapeOperands();
                        escapeResults();
                        break;
                    default:
                        break;
                }
                // The emitter only assigns results.front(); keep any extra result
                // conservatively in persistent storage.
                for (std::size_t extra = 1; extra < results.size(); ++extra)
                {
                    state.variableEscapeFlags[results[extra].value] |= kEscapeGlobal;
                }
                for (const VariableId result : results)
                {
                    uint8_t &defCount = variableDefCounts[result.value];
                    if (defCount != 0)
                    {
                        defCount = 2;
                        state.variableDefBlock[result.value] = kInvalidLocalityBlock;
                        state.variableEscapeFlags[result.value] |= kEscapeGlobal;
                        continue;
                    }
                    defCount = 1;
                    state.variableDefBlock[result.value] = blockIndex;
                    state.variableDefPosition[result.value] = static_cast<uint32_t>(position);
                    state.blockDefinedVariables[blockIndex].push_back(result.value);
                }
            }
        }
        for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        {
            const BlockId block{blockIndex};
            for (std::size_t position = 0; position < model.program.blockSize(block); ++position)
            {
                const InstructionId instruction =
                    model.program.blockInstruction(block, position);
                for (const VariableId operand : program.operands(instruction))
                {
                    const uint32_t variable = operand.value;
                    if (state.variableDefBlock[variable] != blockIndex)
                    {
                        state.variableEscapeFlags[variable] |= kEscapeCrossBlockUse;
                        if (changedResultVariables[variable])
                        {
                            state.crossBlockChangedResults[variable] = true;
                        }
                    }
                    else if (static_cast<uint32_t>(position) <=
                             state.variableDefPosition[variable])
                    {
                        // A read at or before its definition observes a previous round.
                        state.variableEscapeFlags[variable] |= kEscapeEarlyUse;
                    }
                }
            }
        }
        // Dense id space for the cross-block changed results: these are the
        // only values written through a runtime index (set_changed_result and
        // the round-end dirty-list clear), so they share one dense array and
        // the dirty bitmap word count shrinks to their count.
        state.changedResultDenseIndex.assign(variableCount, kInvalidChangedResultIndex);
        state.changedResultCount = 0;
        for (uint32_t index = 0; index < variableCount; ++index)
        {
            if (state.crossBlockChangedResults[index])
            {
                state.changedResultDenseIndex[index] = state.changedResultCount++;
            }
        }
        for (uint32_t index = 0; index < variableCount; ++index)
        {
            const Type &type = state.variableTypes[index];
            if (type.kind != TypeKind::BitVector || type.bitWidth > 64)
            {
                state.variableEscapeFlags[index] |= kEscapeGlobal;
                continue;
            }
            const InitDescriptor &init = program.init(program.variable(VariableId{index}).init);
            if (init.kind == InitKind::Constant || init.kind == InitKind::Actions)
            {
                // init() assigns persistent storage slots by index.
                state.variableEscapeFlags[index] |= kEscapeGlobal;
            }
        }
        for (const PortBinding &port : model.interface.ports)
        {
            if (port.input.valid())
            {
                state.variableEscapeFlags[port.input.value] |= kEscapeGlobal;
            }
            if (port.output.valid())
            {
                state.variableEscapeFlags[port.output.value] |= kEscapeGlobal;
            }
            if (port.outputEnable.valid())
            {
                state.variableEscapeFlags[port.outputEnable.value] |= kEscapeGlobal;
            }
        }
        for (const VariableLabel &declared : model.interface.declaredVariables)
        {
            state.variableEscapeFlags[declared.variable.value] |= kEscapeGlobal;
        }

        // ST00010 detector-group folding: plan the emit-time re-grouping of
        // block-tail watch-group runs before the member declarations (folded
        // event variables are dropped from the v<K> region) and before the
        // block source measure/write passes (both consume the plan).
        planDetectorGroups(model, state);
        // ST00011 array write-point activation: replace commit-Block tail
        // whole-array changed.any detectors by write-site change flags.
        // Planned after the escape analysis (it reads crossBlockChangedResults)
        // and before the measure/write passes.
        planArrayWatchGroups(model, state);
        // ST00012 commit event batch gating: one event-union branch per
        // commit Block replaces per-statement event loads on quiet rounds.
        planCommitEventGates(model, state);
        // ST00013 scalar write-point detection fusion: move commit-Block
        // tail change detection on BitVector targets into the Block's own
        // RegisterWrite sites. Planned after ST00010 (emission consults its
        // group assignments).
        planScalarWatchGroups(model, state);
        // Optional commit-internal cone gating reuses ST00013 real-change
        // decisions to propagate exact state-input dirtiness.
        planCommitInputGates(model, state);
        if (state.commitInputGating)
        {
            std::cerr << "[commit-input-gating] gated="
                      << state.commitInputGateCount
                      << " tracked_state=" << state.commitInputTrackedStateCount
                      << " producer_blocks=" << state.commitInputProducerBlockCount
                      << " dirty_edges=" << state.commitInputDirtyEdgeCount
                      << " snapshots=" << state.commitInputSnapshotCount
                      << " instrs=" << state.commitInputGatedInstructions
                      << " writes=" << state.commitInputGatedWrites << '\n';
        }
        // Guard-event gating (attribute "guardEventGating", default off):
        // pure fatal-guard compute Blocks get a scan-site event gate over
        // their fatal instructions' changedResults_ slots. Reads the escape
        // analysis (def block, cross-block use, crossBlockChangedResults).
        planGuardEventGates(model, state);
        if (state.guardEventGating)
        {
            std::cerr << "[guard-event-gating] gated=" << state.guardGatedBlockCount
                      << " atoms=" << state.guardGatedAtoms
                      << " instrs=" << state.guardGatedInstructions << '\n';
            for (uint32_t blockIndex = 0; blockIndex < state.blockCount; ++blockIndex)
            {
                const EmitState::GuardGate &gate = state.blockGuardGate[blockIndex];
                if (gate.expression.empty())
                {
                    continue;
                }
                std::cerr << "[guard-event-gating] block=" << blockIndex
                          << " atoms=" << gate.atoms << " instrs=" << gate.instructions
                          << '\n';
            }
        }
        // NO0008 block-level same-select mux fusion: adjacent mux-rooted
        // atoms sharing one select emit one fused if/else per run.
        planMuxFusionRuns(model, state);
        // NO0013 F1/F2 windowed emission of lane-build concat cones:
        // planned after mux fusion (mux-covered instructions are excluded).
        planWindowedChains(model, state);
        // NO0014 dynamic bit-field functional-update cone collapse
        // (intRat/vecRat/vlRat-style multi-port table updates).
        planDynBlendCones(model, state);
        // NO0017 §5 wide-state scalar explode: planned after the windowed/
        // dynblend plans (plan capture is a guard) and after ST00013 (the
        // fused-detector check); compacts the pool offsets of the exploded
        // states out of wideValues_.
        planWideStateExplode(model, state);
        // NO0016 narrow-value storage classes: planned last because the
        // classification reads the windowed/dynblend plans.
        classifyLocalValueStorage(model, state);
        // NO0016 Stage B chunk-internal scalarization: reads the storage
        // classes and the mux-fusion coverage.
        planChunkLocalScalars(model, state, *blockChunkInstructions);

        if (statsAttribute != options.attributes.end() && statsAttribute->second == "true")
        {
            uint64_t localValues = 0;
            uint64_t narrowClassCounts[4] = {0, 0, 0, 0};
            for (const std::vector<uint32_t> &defined : state.blockDefinedVariables)
            {
                for (const uint32_t variable : defined)
                {
                    if (state.variableEscapeFlags[variable] == 0)
                    {
                        ++localValues;
                        narrowClassCounts[state.localValueClasses[variable]] += 1;
                    }
                }
            }
            diagnostics.info("AM C++ emitter locality stats: local_values=" +
                                 std::to_string(localValues) + " escaped_values=" +
                                 std::to_string(variableCount - localValues) +
                                 " narrow_local8=" +
                                 std::to_string(narrowClassCounts[0]) +
                                 " narrow_local16=" +
                                 std::to_string(narrowClassCounts[1]) +
                                 " narrow_local32=" +
                                 std::to_string(narrowClassCounts[2]) +
                                 " narrow_local64=" +
                                 std::to_string(narrowClassCounts[3]) +
                                 " narrow_local_pinned=" +
                                 std::to_string(state.narrowLocalPinned) +
                                 " chunk_local_scalars=" +
                                 std::to_string(state.chunkLocalScalarCount) +
                                 " folded_detectors=" +
                                 std::to_string(state.detectorFoldedCount) +
                                 " detector_groups=" +
                                 std::to_string(state.detectorGroupCount) +
                                 " array_watch_write_point=" +
                                 std::to_string(state.arrayWatchReplacedCount) +
                                 " commit_gated_blocks=" +
                                 std::to_string(state.commitGateBlockCount) +
                                 " scalar_watch_fused=" +
                                 std::to_string(state.scalarWatchFusedCount) +
                                 " mux_atom_fused=" +
                                 std::to_string(state.muxAtomFusedCount) +
                                 " windowed_chains=" +
                                 std::to_string(state.windowedChainCount) +
                                 " windowed_steps=" +
                                 std::to_string(state.windowedStepCount) +
                                 " windowed_concats_f2=" +
                                 std::to_string(state.windowedConcatCount) +
                                 " windowed_skipped_slices=" +
                                 std::to_string(state.windowedSkippedSlices) +
                                 " windowed_remapped_slices=" +
                                 std::to_string(state.windowedRemappedSlices) +
                                 " windowed_materialized=" +
                                 std::to_string(state.windowedMaterialized) +
                                 " windowed_bailed_chains=" +
                                 std::to_string(state.windowedBailedChains),
                             std::string(kContext));
        }

        if (state.wideStateExplode &&
            statsAttribute != options.attributes.end() && statsAttribute->second == "true")
        {
            static const std::array<std::string_view, kExplodeBailCount> bailNames = {
                "host_visible", "random_init", "result_defined", "plan_captured",
                "non_slice_read", "dynamic_slice", "wide_slice", "slice_width",
                "array_slice_width", "changed_detector", "dynamic_write",
                "nonconstant_mask", "mask_elements", "write_data",
            };
            std::string message =
                "AM C++ emitter wide-state explode stats: exploded_states=" +
                std::to_string(state.explodedStateCount) +
                " exploded_elements=" + std::to_string(state.explodedElementTotal) +
                " reclaimed_wide_words=" + std::to_string(state.explodedReclaimedWords) +
                " guard_bails[";
            bool first = true;
            for (std::size_t index = 0; index < kExplodeBailCount; ++index)
            {
                if (state.explodeBails[index] == 0)
                {
                    continue;
                }
                if (!first)
                {
                    message += ",";
                }
                first = false;
                message += std::string(bailNames[index]) + "=" +
                           std::to_string(state.explodeBails[index]);
            }
            message += "]";
            diagnostics.info(std::move(message), std::string(kContext));
        }

        const std::size_t blockSourceCount =
            blockCount / *blocksPerSource + (blockCount % *blocksPerSource == 0 ? 0 : 1);
        const std::size_t activityWordCount = (blockCount + 63U) / 64U;
        const std::size_t dirtyChangedWordCount =
            (static_cast<std::size_t>(state.changedResultCount) + 63U) / 64U;

        const std::string prefix = "grhsim_" + options.modelName;
        const std::string className = "GrhSIM_" + options.modelName;
        const std::optional<BlockSourcePlan> blockSourcePlan =
            planBlockSources(model,
                             state,
                             *blocksPerSource,
                             blockSourceCount,
                             *maxSourceBytes,
                             *maxCommitSourceBytes,
                             prefix,
                             className,
                             diagnostics);
        if (!blockSourcePlan)
        {
            return result;
        }
        std::size_t blockPartCount = 0;
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            blockPartCount += sourceParts.size();
        }
        std::ostringstream header;
        header << "#pragma once\n"
               << "#include <array>\n#include <chrono>\n#include <cstddef>\n#include <cstdint>\n#include <cstring>\n#include <string>\n#include <vector>\n\n";
        if (state.runtimeProfile || state.changedTrace)
        {
            header << "#include <cstdio>\n#include <cstdlib>\n\n";
        }
        if (state.runtimeProfile)
        {
            // NO0010: rdtsc helper for per-Block cycle accounting. Plain rdtsc with
            // a compiler barrier (no lfence): cluster-level aggregation plus
            // median-of-3 runs absorbs the reordering jitter.
            header << "static inline std::uint64_t wolvrixAmRdtsc()\n"
                   << "{\n"
                   << "    std::uint32_t lo = 0;\n"
                   << "    std::uint32_t hi = 0;\n"
                   << "    __asm__ __volatile__(\"rdtsc\" : \"=a\"(lo), \"=d\"(hi) :: \"memory\");\n"
                   << "    return (static_cast<std::uint64_t>(hi) << 32) | lo;\n"
                   << "}\n\n";
        }
        header << "#define WOLVRIX_GRHSIM_PERF 0\n\n"
               << "class " << className << " {\npublic:\n"
               << "    " << className << "();\n"
               << "    ~" << className << "();\n"
               // A model owns live simulation and host state; copying is disabled.
               << "    " << className << "(const " << className << " &) = delete;\n"
               << "    " << className << " &operator=(const " << className << " &) = delete;\n"
               << "    void init();\n"
               << "    void eval();\n"
               << "    void finalize();\n"
               << "    void set_random_seed(std::uint64_t seed);\n"
               << "    [[nodiscard]] bool had_register_write_conflict() const;\n"
               << "    void set_runtime_profile_enabled(bool enabled);\n"
               << "    [[nodiscard]] bool runtime_profile_enabled() const;\n"
               << "    void dump_runtime_profile() const;\n"
               << "    static constexpr bool kRuntimeProfileCompiled = "
               << (state.runtimeProfile ? "true" : "false") << ";\n"
               << "    [[nodiscard]] bool finish_requested() const;\n"
               << "    [[nodiscard]] bool stop_requested() const;\n"
               << "    [[nodiscard]] bool fatal_requested() const;\n"
               << "    [[nodiscard]] int system_exit_code() const;\n"
               << "    [[nodiscard]] const std::string &dumpfile_path() const;\n"
               << "    [[nodiscard]] bool dumpvars_enabled() const;\n";
        for (const PortBinding &port : model.interface.ports)
        {
            const VariableId variable =
                port.direction == PortDirection::Input ? port.input : port.output;
            const Type &type = variableType(state, variable);
            header << "    " << cppPortType(type) << " " << sanitizeCppIdentifier(program.string(port.name))
                   << "{};\n";
        }
        header << "\nprivate:\n";
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            for (const BlockSourcePart &part : sourceParts)
            {
                if (part.firstBlock == 0)
                {
                    header << "    void execute_block_0();\n";
                }
                const auto [scanLo, scanHi] =
                    computeBlockRange(part.firstBlock,
                                      part.endBlock,
                                      blockCount,
                                      model.commitBlockBegin);
                if (scanLo < scanHi)
                {
                    header << "    void "
                           << scanSourceFunctionName(part.sourceIndex,
                                                     part.partIndex)
                           << "();\n";
                }
                const auto [commitLo, commitHi] =
                    commitBlockRange(part.firstBlock,
                                     part.endBlock,
                                     model.commitBlockBegin,
                                     model.commitBlockEnd);
                if (commitLo < commitHi)
                {
                    header << "    void "
                           << commitSourceFunctionName(part.sourceIndex,
                                                       part.partIndex)
                           << "();\n";
                }
                // Chunk functions of this part's oversized Blocks (whose
                // instruction streams exceed blockChunkInstructions).
                for (std::size_t blockIndex = part.firstBlock;
                     blockIndex < part.endBlock;
                     ++blockIndex)
                {
                    const BlockId block{static_cast<uint32_t>(blockIndex)};
                    const std::size_t blockSize = model.program.blockSize(block);
                    const std::vector<std::pair<std::size_t, std::size_t>> chunkRanges =
                        blockChunkRanges(blockSize,
                                         *blockChunkInstructions,
                                         state.blockCommitGate[blockIndex].headCount);
                    if (chunkRanges.empty())
                    {
                        continue;
                    }
                    const BlockChunkParams chunkParams =
                        blockChunkParamsFor(state, blockIndex);
                    const std::string parameterList =
                        blockChunkParameterList(blockIndex, chunkParams);
                    for (std::size_t chunk = 0; chunk < chunkRanges.size(); ++chunk)
                    {
                        header << "    void "
                               << blockChunkFunctionName(blockIndex, chunk) << "("
                               << parameterList << ");\n";
                    }
                }
            }
        }
        header << "    // Byte view of the packed activity words for the static compute scan, which\n"
                  "    // consumes activity in 8-Block bytes (the legacy/GSIM batch granularity).\n"
                  "    // Relies on little-endian byte order (the generated models target x86_64 hosts).\n"
                  "    [[nodiscard]] std::uint8_t &active_byte_ref(std::size_t byte) {\n"
                  "        return reinterpret_cast<std::uint8_t *>(activeWords_.data())[byte];\n"
                  "    }\n"
               << "    [[nodiscard]] static bool is_commit_block(std::size_t block);\n"
               << "    void set_changed_result(std::size_t variable, bool event) {\n"
               << "        changedResults_[variable] = event ? 1 : 0;\n"
               << "        if (event) mark_changed_result(variable);\n"
               << "    }\n"
               << "    void mark_changed_result(std::size_t variable);\n"
               << "    void clear_changed_results();\n";
        if (state.changedTrace)
        {
            header << "    void trace_changed_init();\n";
        }
        header << "    static constexpr std::uint64_t bit_mask(std::uint32_t width) {\n"
               << "        return width >= 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1);\n"
               << "    }\n"
               << "    static constexpr std::size_t word_count(std::uint32_t width) { return (static_cast<std::size_t>(width) + 63U) / 64U; }\n"
               << "    static bool any_words(const std::uint64_t *value, std::uint32_t width);\n"
               << "    static bool equal_words(const std::uint64_t *lhs, const std::uint64_t *rhs, std::uint32_t width);\n"
               << "    static void assign_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend);\n"
               << "    static void assign_words_from_scalar(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t source, std::uint32_t sourceWidth, bool signExtend);\n"
               << "    static void masked_write_words(std::uint64_t *target, const std::uint64_t *data, const std::uint64_t *mask, std::uint32_t width);\n"
               << "    static bool masked_write_words_detect(std::uint64_t *target, const std::uint64_t *data, const std::uint64_t *mask, std::uint32_t width);\n"
               << "    static void dynlane_write_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *offset, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static bool dynlane_write_words_detect(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *offset, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static bool assign_words_detect(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend);\n"
               << "    static bool slice_words_detect(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, std::uint64_t start);\n"
               << "    static std::uint64_t resized_word(const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend, std::size_t index);\n"
               << "    static void zero_words(std::uint64_t *target, std::uint32_t width);\n"
               << "    static void bitwise_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *lhs, std::uint32_t lhsWidth, bool lhsSigned, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool rhsSigned, std::uint32_t operation);\n"
               << "    static void arithmetic_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *lhs, std::uint32_t lhsWidth, bool lhsSigned, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool rhsSigned, std::uint32_t operation);\n"
               << "    static void not_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool sourceSigned);\n"
               << "    static int compare_words(const std::uint64_t *lhs, std::uint32_t lhsWidth, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool isSigned);\n"
               << "    static bool reduce_words(const std::uint64_t *source, std::uint32_t width, std::uint32_t operation);\n"
               << "    static std::size_t index_words(const std::uint64_t *source, std::uint32_t width, std::size_t limit);\n"
               << "    static constexpr std::uint64_t extract_word(const std::uint64_t *source, std::uint32_t width, std::uint64_t start) {\n"
               << "        if (start >= width) return 0;\n"
               << "        const std::size_t sourceWord = static_cast<std::size_t>(start / 64U);\n"
               << "        const std::uint32_t shift = static_cast<std::uint32_t>(start % 64U);\n"
               << "        std::uint64_t value = source[sourceWord] >> shift;\n"
               << "        if (shift != 0 && sourceWord + 1U < word_count(width)) value |= source[sourceWord + 1U] << (64U - shift);\n"
               << "        const std::uint64_t remaining = static_cast<std::uint64_t>(width) - start;\n"
               << "        return value & bit_mask(remaining >= 64U ? 64U : static_cast<std::uint32_t>(remaining));\n"
               << "    }\n"
               << "    static void insert_words(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static void insert_replace_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *row, std::uint32_t rowWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static void replace_window_words(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static std::uint64_t shl_word(const std::uint64_t *source, std::uint32_t width, std::uint64_t shift, std::size_t word);\n"
               << "    static void blend_window_dyn_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *index, std::uint32_t indexWidth, const std::uint64_t *ones, const std::uint64_t *elem, std::uint32_t elemWidth);\n"
               << "    static void slice_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, std::uint64_t start);\n"
               << "    static void slice_dynamic_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, const std::uint64_t *index, std::uint32_t indexWidth);\n"
               << "    static void slice_array_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, const std::uint64_t *index, std::uint32_t indexWidth);\n"
               << "    static void array_readall_pack(std::uint64_t *target, std::uint32_t packedWidth, const std::uint64_t *source, std::uint32_t elemWidth, std::uint32_t rows);\n"
               << "    static void array_write_scatter(std::uint64_t *target, const std::uint64_t *laneMask, const std::uint64_t *data, std::uint32_t elemWidth, std::uint32_t rows);\n"
               << "    static bool array_write_scatter_detect(std::uint64_t *target, const std::uint64_t *laneMask, const std::uint64_t *data, std::uint32_t elemWidth, std::uint32_t rows);\n"
               << "    static void array_mux_words(std::uint64_t *target, std::uint32_t packedWidth, const std::uint64_t *sel, const std::uint64_t *t, const std::uint64_t *f, std::uint32_t elemWidth);\n"
               << "    static void array_broadcast_words(std::uint64_t *target, std::uint32_t packedWidth, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static void array_onehot_words(std::uint64_t *target, std::uint32_t rows, const std::uint64_t *index, std::uint32_t indexWidth);\n"
               << "    static void array_reduce_lanes_words(std::uint64_t *target, std::uint32_t rows, const std::uint64_t *source, std::uint32_t elemWidth, std::uint32_t operation);\n"
               << "    static void shift_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool sourceSigned, const std::uint64_t *amount, std::uint32_t amountWidth, std::uint32_t operation);\n"
               << "    static std::uint64_t split_mix64(std::uint64_t &state);\n"
               << "    static constexpr std::uint64_t resize_value(std::uint64_t value, std::uint32_t sourceWidth, bool signExtend, std::uint32_t targetWidth) {\n"
               << "        value &= bit_mask(sourceWidth);\n"
               << "        if (signExtend && sourceWidth < targetWidth && ((value >> (sourceWidth - 1)) & 1U)) value |= ~bit_mask(sourceWidth);\n"
               << "        return value & bit_mask(targetWidth);\n"
               << "    }\n"
               << "    static std::int64_t signed_value(std::uint64_t value, std::uint32_t width);\n"
               << "    static std::uint64_t divide_value(std::uint64_t lhs, std::uint64_t rhs, std::uint32_t width, bool isSigned);\n"
               << "    static std::uint64_t modulo_value(std::uint64_t lhs, std::uint64_t rhs, std::uint32_t width, bool isSigned);\n"
               << "    static std::uint64_t shift_left(std::uint64_t value, std::uint64_t amount, std::uint32_t width, bool);\n"
               << "    static std::uint64_t shift_right(std::uint64_t value, std::uint64_t amount, std::uint32_t width, bool);\n"
               << "    static std::uint64_t arithmetic_shift_right(std::uint64_t value, std::uint64_t amount, std::uint32_t width, bool isSigned);\n"
               << "    static constexpr std::uint64_t concat_value(std::uint64_t accumulated, std::uint32_t accumulatedWidth, std::uint64_t value, std::uint32_t valueWidth) {\n"
               << "        if (valueWidth >= 64) return value;\n"
               << "        return ((accumulated & bit_mask(accumulatedWidth)) << valueWidth) | (value & bit_mask(valueWidth));\n"
               << "    }\n"
               << "    static std::uint64_t slice_value(std::uint64_t value, std::uint64_t start, std::uint32_t width);\n"
               << "    static std::uint64_t slice_array_value(std::uint64_t value, std::uint64_t index, std::uint32_t width, std::uint32_t baseWidth);\n"
               << "    static constexpr std::size_t kBlockCount = " << blockCount << ";\n"
               << "    static constexpr std::size_t kCommitBlockBegin = "
               << model.commitBlockBegin << ";\n"
               << "    static constexpr std::size_t kCommitBlockEnd = "
               << model.commitBlockEnd << ";\n"
               << "    static constexpr std::size_t kCommitBlockCount = "
               << (model.commitBlockEnd - model.commitBlockBegin) << ";\n"
               // Compute Blocks are 1..kComputeBlockEnd-1 (B0 is the entry
               // Block); commit Blocks are the remaining suffix.
               << "    static constexpr std::size_t kComputeBlockEnd = kCommitBlockBegin != 0 ? kCommitBlockBegin : kBlockCount;\n"
               << "    static constexpr std::size_t kActivityWordCount = " << activityWordCount
               << ";\n"
               << "    static constexpr std::size_t kChangedResultCount = "
               << state.changedResultCount << ";\n"
               << "    static constexpr std::size_t kDirtyChangedWordCount = "
               << dirtyChangedWordCount << ";\n";
        // Persistent narrow values (BitVector <= 64 bits) become independent
        // members so the compiler lays out and register-allocates each value
        // on its own instead of indexing one big shared array (the GSIM
        // form). Block-local values (ST00009) stay C++ locals inside their
        // defining block; cross-block changed results stay in the dense
        // changedResults_ array because they are written through a runtime
        // index (set_changed_result and the dirty-list clear). Members are
        // Members are declared in ascending VariableId order without
        // initializers: clang miscompiles the implicit default constructor
        // once a class carries tens of thousands of {}-initialized members
        // (only a small prefix is actually initialized), so init() zeroes
        // the contiguous member region with a single memset instead. The
        // declaration burst is built in memory to keep emission fast on
        // multi-million-value models.
        {
            std::ostringstream memberDeclarations;
            for (uint32_t index = 0; index < variableCount; ++index)
            {
                const Type &type = state.variableTypes[index];
                if (type.kind != TypeKind::BitVector || type.bitWidth > 64 ||
                    state.crossBlockChangedResults[index])
                {
                    continue;
                }
                if (state.variableEscapeFlags[index] == 0 &&
                    state.variableDefBlock[index] != kInvalidLocalityBlock)
                {
                    continue;
                }
                // ST00010: a folded detector's event variable is never
                // assigned or read (the group flag replaces it), and its
                // zeroInit needs no explicit init write, so it gets no member.
                if (state.foldedDetectorEvents[index])
                {
                    continue;
                }
                if (state.memberValueCount == 0)
                {
                    state.firstMemberVariable = index;
                }
                ++state.memberValueCount;
                memberDeclarations << "    std::uint64_t v" << index << ";\n";
            }
            header << memberDeclarations.str();
        }
        header << "    std::array<std::uint64_t, kChangedResultCount> changedResults_{};\n";
        if (state.commitInputGating)
        {
            header << "    std::array<std::uint64_t, "
                   << state.commitInputSnapshotCount
                   << "> commitInputSnapshots_{};\n"
                   << "    std::array<std::uint8_t, "
                   << state.commitInputGateCount
                   << "> commitInputValid_{};\n"
                   << "    std::array<std::uint8_t, "
                   << state.commitInputGateCount
                   << "> commitInputDirty_{};\n";
        }
        // NO0017 §5: exploded wide states — one per-element scalar array
        // member per state (element width K), out of the wideValues_ pool.
        for (uint32_t index = 0; index < state.explodedElementWidth.size(); ++index)
        {
            const uint32_t elemWidth = state.explodedElementWidth[index];
            if (elemWidth == 0)
            {
                continue;
            }
            header << "    std::array<" << explodedElemCppType(elemWidth) << ", "
                   << (state.variableTypes[index].bitWidth / elemWidth) << "> "
                   << explodedMemberName(VariableId{index}) << "{};\n";
        }
        header << "    std::array<std::uint64_t, " << state.wideWords << "> wideValues_{};\n"
               << "    std::array<std::uint64_t, " << state.realValues << "> realValues_{};\n"
               << "    std::array<std::string, " << state.stringValues << "> stringValues_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> activeWords_{};\n"
               << "    bool backwardFired_ = false;\n"
               << "    std::uint64_t roundCounter_ = 0;\n"
               << "    std::array<std::uint64_t, kDirtyChangedWordCount> dirtyChangedBits_{};\n"
               << "    std::vector<std::uint32_t> dirtyChangedResults_;\n"
               << "    std::array<bool, " << state.onceSlotCount << "> onceCompleted_{};\n"
               << "    std::array<bool, " << state.pendingEventSlotCount
               << "> pendingHostEvents_{};\n"
               << "    bool firstEval_ = true;\n"
               << "    std::uint64_t randomSeed_ = 0;\n";
        if (state.runtimeProfile)
        {
            header << "    bool runtimeProfileEnabled_ = false;\n"
                   << "    std::uint64_t profileEvalCalls_ = 0;\n"
                   << "    std::uint64_t profileRounds_ = 0;\n"
                   << "    std::uint64_t profileBlockExecs_ = 0;\n"
                   << "    std::uint64_t profileCommitBlockExecs_ = 0;\n"
                   << "    std::uint64_t profileActivateForward_ = 0;\n"
                   << "    std::uint64_t profileActivateBackward_ = 0;\n"
                   << "    std::uint64_t profileChangedMarks_ = 0;\n"
                   << "    std::uint64_t profileChangedClears_ = 0;\n"
                   << "    std::uint64_t profileComputeNs_ = 0;\n"
                   << "    std::uint64_t profileCommitNs_ = 0;\n"
                   << "    std::uint64_t profileEvalNs_ = 0;\n"
                   << "    std::array<std::uint64_t, kBlockCount> profilePerBlockExecs_{};\n"
                   // NO0010: accumulated cycles per Block, same indexing as execs.
                   << "    std::array<std::uint64_t, kBlockCount> profilePerBlockCycles_{};\n";
        }
        if (state.changedTrace)
        {
            header << "    std::FILE *traceChangedFile_ = nullptr;\n"
                   << "    bool traceChangedInit_ = false;\n"
                   << "    std::uint64_t traceChangedEval_ = 0;\n"
                   << "    std::uint64_t traceChangedBegin_ = 0;\n"
                   << "    std::uint64_t traceChangedEnd_ = UINT64_MAX;\n";
        }
        header << "    bool finalized_ = false;\n"
               << "    bool finishRequested_ = false;\n"
               << "    bool stopRequested_ = false;\n"
               << "    bool fatalRequested_ = false;\n"
               << "    int systemExitCode_ = 0;\n"
               << "    std::string emptyPath_;\n"
               << "};\n";

        std::ostringstream support;
        support << "#pragma once\n"
                << "#include <algorithm>\n#include <bit>\n#include <cstddef>\n#include <cstdint>\n"
                << "#include <iomanip>\n#include <iostream>\n#include <limits>\n#include <sstream>\n"
                << "#include <stdexcept>\n#include <string>\n#include <string_view>\n#include <utility>\n"
                << "#include <vector>\n\n";
        for (uint32_t index = 0; index < program.dpiImportCount(); ++index)
        {
            if (!state.referencedDpiImports[index])
            {
                continue;
            }
            const DpiImportView import = program.dpiImport(DpiImportId{index});
            std::string error;
            std::string returnType = "void";
            if (import.returnValue.present)
            {
                returnType = *dpiCppType(program.type(import.returnValue.type),
                                         import.returnValue.abi,
                                         error);
            }
            support << "extern \"C\" " << returnType << " "
                    << program.string(import.symbol) << "(";
            for (std::size_t parameterIndex = 0;
                 parameterIndex < import.parameters.size();
                 ++parameterIndex)
            {
                if (parameterIndex != 0)
                {
                    support << ", ";
                }
                const DpiParameter &parameter = import.parameters[parameterIndex];
                std::string parameterType =
                    *dpiCppType(program.type(parameter.type), parameter.abi, error);
                if (parameter.direction != DpiDirection::Input)
                {
                    parameterType += " *";
                }
                support << parameterType;
            }
            support << ");\n";
        }
        support << R"CPP(

namespace
{
    enum class TaskArgumentKind
    {
        Logic,
        Real,
        String,
    };

    struct TaskArgument
    {
        TaskArgumentKind kind = TaskArgumentKind::Logic;
        std::size_t width = 0;
        bool isSigned = false;
        bool isWide = false;
        std::uint64_t scalarValue = 0;
        const std::uint64_t *wideValue = nullptr;
        double realValue = 0.0;
        std::string_view stringValue;

        static TaskArgument logic_scalar(std::uint64_t value,
                                         std::size_t width,
                                         bool isSigned)
        {
            TaskArgument argument;
            argument.width = width;
            argument.isSigned = isSigned;
            argument.scalarValue = value;
            return argument;
        }

        static TaskArgument logic_wide(const std::uint64_t *value,
                                       std::size_t width,
                                       bool isSigned)
        {
            TaskArgument argument;
            argument.width = width;
            argument.isSigned = isSigned;
            argument.isWide = true;
            argument.wideValue = value;
            return argument;
        }

        static TaskArgument real(double value)
        {
            TaskArgument argument;
            argument.kind = TaskArgumentKind::Real;
            argument.realValue = value;
            return argument;
        }

        static TaskArgument string(std::string_view value)
        {
            TaskArgument argument;
            argument.kind = TaskArgumentKind::String;
            argument.stringValue = value;
            return argument;
        }
    };

    std::uint64_t task_mask(std::size_t width)
    {
        return width >= 64U ? UINT64_MAX : (UINT64_C(1) << width) - UINT64_C(1);
    }

    std::vector<std::uint64_t> task_words(const TaskArgument &argument)
    {
        const std::size_t count = (argument.width + 63U) / 64U;
        std::vector<std::uint64_t> words(count, 0);
        if (count == 0)
        {
            return words;
        }
        if (argument.isWide)
        {
            std::copy_n(argument.wideValue, count, words.data());
        }
        else
        {
            words[0] = argument.scalarValue;
        }
        const std::size_t tailWidth = argument.width - (count - 1U) * 64U;
        words.back() &= task_mask(tailWidth);
        return words;
    }

    bool task_words_zero(const std::vector<std::uint64_t> &words)
    {
        return std::all_of(words.begin(), words.end(),
                           [](std::uint64_t word) { return word == 0; });
    }

    bool task_sign_bit(const std::vector<std::uint64_t> &words, std::size_t width)
    {
        return width != 0 &&
               ((words[(width - 1U) / 64U] >> ((width - 1U) % 64U)) & UINT64_C(1)) != 0;
    }

    void task_negate(std::vector<std::uint64_t> &words, std::size_t width)
    {
        for (std::uint64_t &word : words)
        {
            word = ~word;
        }
        std::uint64_t carry = 1;
        for (std::uint64_t &word : words)
        {
            const std::uint64_t next = word + carry;
            carry = next < word ? 1U : 0U;
            word = next;
            if (carry == 0)
            {
                break;
            }
        }
        if (!words.empty())
        {
            words.back() &= task_mask(width - (words.size() - 1U) * 64U);
        }
    }

    std::uint32_t task_divmod(std::vector<std::uint64_t> &words, std::uint32_t base)
    {
        unsigned __int128 remainder = 0;
        for (std::size_t index = words.size(); index-- > 0;)
        {
            const unsigned __int128 current = (remainder << 64U) | words[index];
            words[index] = static_cast<std::uint64_t>(current / base);
            remainder = current % base;
        }
        return static_cast<std::uint32_t>(remainder);
    }

    std::string task_unsigned_text(std::vector<std::uint64_t> words,
                                   std::uint32_t base,
                                   bool uppercase)
    {
        if (words.empty() || task_words_zero(words))
        {
            return "0";
        }
        const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string text;
        while (!task_words_zero(words))
        {
            text.push_back(digits[task_divmod(words, base)]);
        }
        std::reverse(text.begin(), text.end());
        return text;
    }

    std::string task_logic_text(const TaskArgument &argument,
                                std::uint32_t base,
                                bool uppercase,
                                bool signedDecimal)
    {
        std::vector<std::uint64_t> words = task_words(argument);
        const bool negative = signedDecimal && argument.isSigned &&
                              task_sign_bit(words, argument.width);
        if (negative)
        {
            task_negate(words, argument.width);
        }
        std::string text = task_unsigned_text(std::move(words), base, uppercase);
        if (negative && text != "0")
        {
            text.insert(text.begin(), '-');
        }
        return text;
    }

    std::uint64_t task_u64(const TaskArgument &argument)
    {
        if (argument.kind == TaskArgumentKind::Real)
        {
            return static_cast<std::uint64_t>(argument.realValue);
        }
        if (argument.kind == TaskArgumentKind::String)
        {
            return argument.stringValue.empty()
                       ? 0
                       : static_cast<unsigned char>(argument.stringValue.front());
        }
        if (argument.isWide)
        {
            return argument.wideValue == nullptr ? 0 : argument.wideValue[0];
        }
        return argument.scalarValue & task_mask(argument.width);
    }

    std::string task_default_text(const TaskArgument &argument)
    {
        if (argument.kind == TaskArgumentKind::String)
        {
            return std::string(argument.stringValue);
        }
        if (argument.kind == TaskArgumentKind::Real)
        {
            std::ostringstream stream;
            stream << std::defaultfloat << argument.realValue;
            return stream.str();
        }
        return task_logic_text(argument, 10U, false, argument.isSigned);
    }

    std::string task_apply_width(std::string text,
                                 int width,
                                 bool leftJustify,
                                 bool zeroPad)
    {
        if (width <= 0 || static_cast<int>(text.size()) >= width)
        {
            return text;
        }
        const std::size_t count = static_cast<std::size_t>(width) - text.size();
        const char fill = zeroPad && !leftJustify ? '0' : ' ';
        if (leftJustify)
        {
            text.append(count, fill);
            return text;
        }
        if (fill == '0' && !text.empty() && text.front() == '-')
        {
            return "-" + std::string(count, '0') + text.substr(1);
        }
        return std::string(count, fill) + text;
    }

    std::string task_format_one(const TaskArgument &argument,
                                char specifier,
                                int width,
                                int precision,
                                bool leftJustify,
                                bool zeroPad)
    {
        std::string text;
        switch (specifier)
        {
            case 'd':
            case 'i':
                text = argument.kind == TaskArgumentKind::Logic
                           ? task_logic_text(argument, 10U, false, argument.isSigned)
                           : argument.kind == TaskArgumentKind::Real
                                 ? std::to_string(static_cast<long long>(argument.realValue))
                                 : task_default_text(argument);
                break;
            case 'u':
                text = argument.kind == TaskArgumentKind::Logic
                           ? task_logic_text(argument, 10U, false, false)
                           : std::to_string(task_u64(argument));
                break;
            case 'h':
            case 'x':
                text = argument.kind == TaskArgumentKind::Logic
                           ? task_logic_text(argument, 16U, false, false)
                           : task_default_text(argument);
                break;
            case 'H':
            case 'X':
                text = argument.kind == TaskArgumentKind::Logic
                           ? task_logic_text(argument, 16U, true, false)
                           : task_default_text(argument);
                break;
            case 'b':
                text = argument.kind == TaskArgumentKind::Logic
                           ? task_logic_text(argument, 2U, false, false)
                           : task_default_text(argument);
                break;
            case 'o':
                text = argument.kind == TaskArgumentKind::Logic
                           ? task_logic_text(argument, 8U, false, false)
                           : task_default_text(argument);
                break;
            case 'c':
                text.assign(1, static_cast<char>(task_u64(argument) & UINT64_C(0xff)));
                break;
            case 's':
                text = argument.kind == TaskArgumentKind::String
                           ? std::string(argument.stringValue)
                           : task_default_text(argument);
                break;
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            {
                const double value = argument.kind == TaskArgumentKind::Real
                                         ? argument.realValue
                                         : static_cast<double>(task_u64(argument));
                std::ostringstream stream;
                if (precision >= 0)
                {
                    stream << std::setprecision(precision);
                }
                if (specifier == 'e' || specifier == 'E')
                {
                    stream << std::scientific;
                }
                else if (specifier == 'f' || specifier == 'F')
                {
                    stream << std::fixed;
                }
                if (specifier == 'E' || specifier == 'F' || specifier == 'G')
                {
                    stream << std::uppercase;
                }
                stream << value;
                text = stream.str();
                break;
            }
            case 't':
                text = std::to_string(task_u64(argument));
                break;
            case 'v':
            default:
                text = task_default_text(argument);
                break;
        }
        return task_apply_width(std::move(text), width, leftJustify, zeroPad);
    }

    class TaskFormatter
    {
    public:
        explicit TaskFormatter(std::string_view format) : format_(format) {}

        void append(const TaskArgument &argument)
        {
            emitUntilArgument(&argument);
        }

        std::string finish()
        {
            emitUntilArgument(nullptr);
            return std::move(output_);
        }

    private:
        bool emitUntilArgument(const TaskArgument *argument)
        {
            while (cursor_ < format_.size())
            {
                if (format_[cursor_] != '%')
                {
                    output_.push_back(format_[cursor_++]);
                    continue;
                }
                ++cursor_;
                if (cursor_ >= format_.size())
                {
                    output_.push_back('%');
                    return false;
                }
                if (format_[cursor_] == '%')
                {
                    output_.push_back('%');
                    ++cursor_;
                    continue;
                }
                bool leftJustify = false;
                bool zeroPad = false;
                while (cursor_ < format_.size())
                {
                    if (format_[cursor_] == '-')
                    {
                        leftJustify = true;
                        ++cursor_;
                    }
                    else if (format_[cursor_] == '0')
                    {
                        zeroPad = true;
                        ++cursor_;
                    }
                    else
                    {
                        break;
                    }
                }
                int width = 0;
                while (cursor_ < format_.size() && format_[cursor_] >= '0' &&
                       format_[cursor_] <= '9')
                {
                    width = width * 10 + static_cast<int>(format_[cursor_++] - '0');
                }
                int precision = -1;
                if (cursor_ < format_.size() && format_[cursor_] == '.')
                {
                    ++cursor_;
                    precision = 0;
                    while (cursor_ < format_.size() && format_[cursor_] >= '0' &&
                           format_[cursor_] <= '9')
                    {
                        precision = precision * 10 +
                                    static_cast<int>(format_[cursor_++] - '0');
                    }
                }
                while (cursor_ < format_.size() &&
                       (format_[cursor_] == 'l' || format_[cursor_] == 'L' ||
                        format_[cursor_] == 'z'))
                {
                    ++cursor_;
                }
                if (cursor_ >= format_.size())
                {
                    return false;
                }
                const char specifier = format_[cursor_++];
                if (specifier == 'm')
                {
                    output_ += "top";
                    continue;
                }
                if (argument == nullptr)
                {
                    output_.push_back('%');
                    output_.push_back(specifier);
                    continue;
                }
                output_ += task_format_one(
                    *argument, specifier, width, precision, leftJustify, zeroPad);
                return true;
            }
            return false;
        }

        std::string_view format_;
        std::size_t cursor_ = 0;
        std::string output_;
    };
} // namespace

)CPP";

        std::ostringstream runtime;
        runtime << "#include \"" << prefix << ".hpp\"\n"
                << "#include \"" << prefix << "_support.hpp\"\n\n";
        runtime << className << "::" << className << "() = default;\n"
               << className << "::~" << className << "() { finalize(); }\n\n"
               << "bool " << className
               << "::any_words(const std::uint64_t *value, std::uint32_t width) {\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == words ? width - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        if ((value[index] & bit_mask(bits)) != 0) return true;\n"
               << "    }\n"
               << "    return false;\n}\n"
               << "bool " << className
               << "::equal_words(const std::uint64_t *lhs, const std::uint64_t *rhs, std::uint32_t width) {\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == words ? width - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t mask = bit_mask(bits);\n"
               << "        if ((lhs[index] & mask) != (rhs[index] & mask)) return false;\n"
               << "    }\n"
               << "    return true;\n}\n"
               << "void " << className
               << "::assign_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend) {\n"
               << "    const std::size_t targetWords = word_count(targetWidth);\n"
               << "    const std::size_t sourceWords = word_count(sourceWidth);\n"
               << "    std::fill(target, target + targetWords, UINT64_C(0));\n"
               << "    for (std::size_t index = 0; index < std::min(targetWords, sourceWords); ++index) target[index] = source[index];\n"
               << "    if (signExtend && targetWidth > sourceWidth && ((source[(sourceWidth - 1U) / 64U] >> ((sourceWidth - 1U) % 64U)) & 1U)) {\n"
               << "        std::size_t index = sourceWidth / 64U;\n"
               << "        const std::uint32_t bit = sourceWidth % 64U;\n"
               << "        if (bit != 0) { target[index] |= ~bit_mask(bit); ++index; }\n"
               << "        for (; index < targetWords; ++index) target[index] = UINT64_MAX;\n"
               << "    }\n"
               << "    target[targetWords - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((targetWords - 1U) * 64U));\n"
               << "}\n"
               << "void " << className
               << "::assign_words_from_scalar(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t source, std::uint32_t sourceWidth, bool signExtend) {\n"
               << "    assign_words(target, targetWidth, &source, sourceWidth, signExtend);\n"
               << "}\n"
               << "void " << className
               << "::masked_write_words(std::uint64_t *target, const std::uint64_t *data, const std::uint64_t *mask, std::uint32_t width) {\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == words ? width - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t liveMask = bit_mask(bits);\n"
               << "        const std::uint64_t writeMask = mask[index] & liveMask;\n"
               << "        target[index] = ((target[index] & ~writeMask) | (data[index] & writeMask)) & liveMask;\n"
               << "    }\n"
               << "}\n"
               << "bool " << className
               << "::masked_write_words_detect(std::uint64_t *target, const std::uint64_t *data, const std::uint64_t *mask, std::uint32_t width) {\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    bool changed = false;\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == words ? width - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t liveMask = bit_mask(bits);\n"
               << "        const std::uint64_t writeMask = mask[index] & liveMask;\n"
               << "        const std::uint64_t next = ((target[index] & ~writeMask) | (data[index] & writeMask)) & liveMask;\n"
               << "        changed = changed || (next != target[index]);\n"
               << "        target[index] = next;\n"
               << "    }\n"
               << "    return changed;\n"
               << "}\n"
               << "bool " << className
               << "::dynlane_write_words_detect(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *offset, const std::uint64_t *source, std::uint32_t sourceWidth) {\n"
               << "    const std::uint64_t bitOffset = offset[0];\n"
               << "    if (bitOffset >= targetWidth) { return false; }\n"
               << "    const std::uint64_t limit = std::min<std::uint64_t>(static_cast<std::uint64_t>(sourceWidth), static_cast<std::uint64_t>(targetWidth) - bitOffset);\n"
               << "    bool changed = false;\n"
               << "    if ((bitOffset % 64U) == 0 && (sourceWidth % 64U) == 0) {\n"
               << "        const std::size_t firstWord = static_cast<std::size_t>(bitOffset / 64U);\n"
               << "        const std::size_t words = static_cast<std::size_t>(limit / 64U);\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            changed = changed || (target[firstWord + index] != source[index]);\n"
               << "            target[firstWord + index] = source[index];\n"
               << "        }\n"
               << "        return changed;\n"
               << "    }\n"
               << "    for (std::uint64_t bit = 0; bit < limit; ++bit) {\n"
               << "        const std::uint64_t sourceBit = (source[bit / 64U] >> (bit % 64U)) & UINT64_C(1);\n"
               << "        const std::uint64_t targetBit = bitOffset + bit;\n"
               << "        const std::uint64_t mask = UINT64_C(1) << (targetBit % 64U);\n"
               << "        changed = changed || (((target[targetBit / 64U] & mask) != 0) != (sourceBit != 0));\n"
               << "        target[targetBit / 64U] = (target[targetBit / 64U] & ~mask) | (sourceBit << (targetBit % 64U));\n"
               << "    }\n"
               << "    return changed;\n"
               << "}\n"
               << "void " << className
               << "::dynlane_write_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *offset, const std::uint64_t *source, std::uint32_t sourceWidth) {\n"
               << "    (void)dynlane_write_words_detect(target, targetWidth, offset, source, sourceWidth);\n"
               << "}\n"
               << "bool " << className
               << "::assign_words_detect(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend) {\n"
               << "    const std::size_t targetWords = word_count(targetWidth);\n"
               << "    const std::size_t sourceWords = word_count(sourceWidth);\n"
               << "    const bool negative = signExtend && targetWidth > sourceWidth && ((source[(sourceWidth - 1U) / 64U] >> ((sourceWidth - 1U) % 64U)) & 1U);\n"
               << "    const std::size_t signIndex = sourceWidth / 64U;\n"
               << "    const std::uint32_t signBit = sourceWidth % 64U;\n"
               << "    bool changed = false;\n"
               << "    for (std::size_t index = 0; index < targetWords; ++index) {\n"
               << "        std::uint64_t next = index < std::min(targetWords, sourceWords) ? source[index] : UINT64_C(0);\n"
               << "        if (negative) {\n"
               << "            if (signBit != 0) {\n"
               << "                if (index == signIndex) next |= ~bit_mask(signBit);\n"
               << "                else if (index > signIndex) next = UINT64_MAX;\n"
               << "            } else if (index >= signIndex) { next = UINT64_MAX; }\n"
               << "        }\n"
               << "        if (index + 1U == targetWords) next &= bit_mask(targetWidth - static_cast<std::uint32_t>(index * 64U));\n"
               << "        changed = changed || (next != target[index]);\n"
               << "        target[index] = next;\n"
               << "    }\n"
               << "    return changed;\n"
               << "}\n"
               << "bool " << className
               << "::slice_words_detect(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, std::uint64_t start) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    bool changed = false;\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        std::uint64_t next = extract_word(source, sourceWidth, start + index * 64U);\n"
               << "        if (index + 1U == words) next &= bit_mask(targetWidth - static_cast<std::uint32_t>(index * 64U));\n"
               << "        changed = changed || (next != target[index]);\n"
               << "        target[index] = next;\n"
               << "    }\n"
               << "    return changed;\n"
               << "}\n"
               << "std::uint64_t " << className
               << "::resized_word(const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend, std::size_t index) {\n"
               << "    const std::size_t words = word_count(sourceWidth);\n"
               << "    const bool negative = signExtend && ((source[(sourceWidth - 1U) / 64U] >> ((sourceWidth - 1U) % 64U)) & 1U);\n"
               << "    if (index >= words) return negative ? UINT64_MAX : UINT64_C(0);\n"
               << "    std::uint64_t value = source[index];\n"
               << "    if (index + 1U == words && sourceWidth % 64U != 0) {\n"
               << "        const std::uint64_t mask = bit_mask(sourceWidth % 64U);\n"
               << "        value &= mask;\n"
               << "        if (negative) value |= ~mask;\n"
               << "    }\n"
               << "    return value;\n}\n"
               << "void " << className
               << "::zero_words(std::uint64_t *target, std::uint32_t width) {\n"
               << "    std::fill(target, target + word_count(width), UINT64_C(0));\n"
               << "}\n"
               << "void " << className
               << "::bitwise_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *lhs, std::uint32_t lhsWidth, bool lhsSigned, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool rhsSigned, std::uint32_t operation) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint64_t left = resized_word(lhs, lhsWidth, lhsSigned, index);\n"
               << "        const std::uint64_t right = resized_word(rhs, rhsWidth, rhsSigned, index);\n"
               << "        switch (operation) {\n"
               << "        case 0: target[index] = left & right; break;\n"
               << "        case 1: target[index] = left | right; break;\n"
               << "        case 2: target[index] = left ^ right; break;\n"
               << "        default: target[index] = ~(left ^ right); break;\n"
               << "        }\n"
               << "    }\n"
               << "    target[words - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((words - 1U) * 64U));\n"
               << "}\n"
               << "void " << className
               << "::arithmetic_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *lhs, std::uint32_t lhsWidth, bool lhsSigned, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool rhsSigned, std::uint32_t operation) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    zero_words(target, targetWidth);\n"
               << "    if (operation == 2) {\n"
               << "        for (std::size_t lhsIndex = 0; lhsIndex < words; ++lhsIndex) {\n"
               << "            const std::uint64_t left = resized_word(lhs, lhsWidth, lhsSigned, lhsIndex);\n"
               << "            unsigned __int128 carry = 0;\n"
               << "            for (std::size_t rhsIndex = 0; lhsIndex + rhsIndex < words; ++rhsIndex) {\n"
               << "                const std::size_t targetIndex = lhsIndex + rhsIndex;\n"
               << "                const unsigned __int128 total = static_cast<unsigned __int128>(left) * resized_word(rhs, rhsWidth, rhsSigned, rhsIndex) + target[targetIndex] + carry;\n"
               << "                target[targetIndex] = static_cast<std::uint64_t>(total);\n"
               << "                carry = total >> 64U;\n"
               << "            }\n"
               << "        }\n"
               << "    } else {\n"
               << "        std::uint64_t carryOrBorrow = 0;\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint64_t left = resized_word(lhs, lhsWidth, lhsSigned, index);\n"
               << "            const std::uint64_t right = resized_word(rhs, rhsWidth, rhsSigned, index);\n"
               << "            if (operation == 0) {\n"
               << "                const unsigned __int128 total = static_cast<unsigned __int128>(left) + right + carryOrBorrow;\n"
               << "                target[index] = static_cast<std::uint64_t>(total);\n"
               << "                carryOrBorrow = static_cast<std::uint64_t>(total >> 64U);\n"
               << "            } else {\n"
               << "                const unsigned __int128 subtrahend = static_cast<unsigned __int128>(right) + carryOrBorrow;\n"
               << "                target[index] = left - static_cast<std::uint64_t>(subtrahend);\n"
               << "                carryOrBorrow = static_cast<unsigned __int128>(left) < subtrahend;\n"
               << "            }\n"
               << "        }\n"
               << "    }\n"
               << "    target[words - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((words - 1U) * 64U));\n"
               << "}\n"
               << "void " << className
               << "::not_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool sourceSigned) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    for (std::size_t index = 0; index < words; ++index) target[index] = ~resized_word(source, sourceWidth, sourceSigned, index);\n"
               << "    target[words - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((words - 1U) * 64U));\n"
               << "}\n"
               << "int " << className
               << "::compare_words(const std::uint64_t *lhs, std::uint32_t lhsWidth, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool isSigned) {\n"
               << "    const std::uint32_t width = std::max(lhsWidth, rhsWidth);\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    const bool lhsNegative = isSigned && ((resized_word(lhs, lhsWidth, true, words - 1U) >> ((width - 1U) % 64U)) & 1U);\n"
               << "    const bool rhsNegative = isSigned && ((resized_word(rhs, rhsWidth, true, words - 1U) >> ((width - 1U) % 64U)) & 1U);\n"
               << "    if (lhsNegative != rhsNegative) return lhsNegative ? -1 : 1;\n"
               << "    for (std::size_t index = words; index-- > 0;) {\n"
               << "        std::uint64_t left = resized_word(lhs, lhsWidth, isSigned, index);\n"
               << "        std::uint64_t right = resized_word(rhs, rhsWidth, isSigned, index);\n"
               << "        if (index + 1U == words) {\n"
               << "            const std::uint64_t mask = bit_mask(width - static_cast<std::uint32_t>(index * 64U));\n"
               << "            left &= mask; right &= mask;\n"
               << "        }\n"
               << "        if (left < right) return -1;\n"
               << "        if (left > right) return 1;\n"
               << "    }\n"
               << "    return 0;\n}\n"
               << "bool " << className
               << "::reduce_words(const std::uint64_t *source, std::uint32_t width, std::uint32_t operation) {\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    if (operation <= 1) {\n"
               << "        bool all = true;\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint32_t bits = index + 1U == words ? width - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "            const std::uint64_t mask = bit_mask(bits);\n"
               << "            all = all && ((source[index] & mask) == mask);\n"
               << "        }\n"
               << "        return operation == 0 ? all : !all;\n"
               << "    }\n"
               << "    if (operation <= 3) {\n"
               << "        const bool any = any_words(source, width);\n"
               << "        return operation == 2 ? any : !any;\n"
               << "    }\n"
               << "    unsigned parity = 0;\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == words ? width - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        parity ^= static_cast<unsigned>(std::popcount(source[index] & bit_mask(bits)) & 1U);\n"
               << "    }\n"
               << "    return operation == 4 ? parity != 0 : parity == 0;\n"
               << "}\n"
               << "std::size_t " << className
               << "::index_words(const std::uint64_t *source, std::uint32_t width, std::size_t limit) {\n"
               << "    const std::size_t words = word_count(width);\n"
               << "    for (std::size_t index = 1; index < words; ++index) if (source[index] != 0) return limit;\n"
               << "    return source[0] >= limit ? limit : static_cast<std::size_t>(source[0]);\n"
               << "}\n"
               << "void " << className
               << "::insert_words(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth) {\n"
               << "    if (targetLsb >= targetWidth) return;\n"
               << "    const std::size_t targetWords = word_count(targetWidth);\n"
               << "    const std::size_t sourceWords = word_count(sourceWidth);\n"
               << "    const std::size_t firstTargetWord = static_cast<std::size_t>(targetLsb / 64U);\n"
               << "    const std::uint32_t shift = static_cast<std::uint32_t>(targetLsb % 64U);\n"
               << "    for (std::size_t index = 0; index < sourceWords; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == sourceWords ? sourceWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t value = source[index] & bit_mask(bits);\n"
               << "        const std::size_t targetIndex = firstTargetWord + index;\n"
               << "        if (targetIndex < targetWords) target[targetIndex] |= value << shift;\n"
               << "        if (shift != 0 && targetIndex + 1U < targetWords) target[targetIndex + 1U] |= value >> (64U - shift);\n"
               << "    }\n"
               << "    target[targetWords - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((targetWords - 1U) * 64U));\n"
               << "}\n"
               << "void " << className
               << "::insert_replace_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *row, std::uint32_t rowWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth) {\n"
               << "    assign_words(target, targetWidth, row, rowWidth, false);\n"
               << "    if (targetLsb >= targetWidth) return;\n"
               << "    const std::size_t targetWords = word_count(targetWidth);\n"
               << "    const std::size_t sourceWords = word_count(sourceWidth);\n"
               << "    const std::size_t firstTargetWord = static_cast<std::size_t>(targetLsb / 64U);\n"
               << "    const std::uint32_t shift = static_cast<std::uint32_t>(targetLsb % 64U);\n"
               << "    for (std::size_t index = 0; index < sourceWords; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == sourceWords ? sourceWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t value = source[index] & bit_mask(bits);\n"
               << "        const std::size_t targetIndex = firstTargetWord + index;\n"
               << "        const std::uint32_t covered = bits < 64U - shift ? bits : 64U - shift;\n"
               << "        if (targetIndex < targetWords) {\n"
               << "            const std::uint64_t window = covered >= 64U ? UINT64_MAX : (bit_mask(covered) << shift);\n"
               << "            target[targetIndex] = (target[targetIndex] & ~window) | ((value << shift) & window);\n"
               << "        }\n"
               << "        if (shift != 0 && bits > covered && targetIndex + 1U < targetWords) {\n"
               << "            const std::uint32_t spill = bits - covered;\n"
               << "            const std::uint64_t window = bit_mask(spill);\n"
               << "            target[targetIndex + 1U] = (target[targetIndex + 1U] & ~window) | ((value >> (64U - shift)) & window);\n"
               << "        }\n"
               << "    }\n"
               << "}\n"
               << "void " << className
               << "::replace_window_words(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth) {\n"
               << "    if (targetLsb >= targetWidth) return;\n"
               << "    const std::size_t targetWords = word_count(targetWidth);\n"
               << "    const std::size_t sourceWords = word_count(sourceWidth);\n"
               << "    const std::size_t firstTargetWord = static_cast<std::size_t>(targetLsb / 64U);\n"
               << "    const std::uint32_t shift = static_cast<std::uint32_t>(targetLsb % 64U);\n"
               << "    for (std::size_t index = 0; index < sourceWords; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == sourceWords ? sourceWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t value = source[index] & bit_mask(bits);\n"
               << "        const std::size_t targetIndex = firstTargetWord + index;\n"
               << "        const std::uint32_t covered = bits < 64U - shift ? bits : 64U - shift;\n"
               << "        if (targetIndex < targetWords) {\n"
               << "            const std::uint64_t window = covered >= 64U ? UINT64_MAX : (bit_mask(covered) << shift);\n"
               << "            target[targetIndex] = (target[targetIndex] & ~window) | ((value << shift) & window);\n"
               << "        }\n"
               << "        if (shift != 0 && bits > covered && targetIndex + 1U < targetWords) {\n"
               << "            const std::uint32_t spill = bits - covered;\n"
               << "            const std::uint64_t window = bit_mask(spill);\n"
               << "            target[targetIndex + 1U] = (target[targetIndex + 1U] & ~window) | ((value >> (64U - shift)) & window);\n"
               << "        }\n"
               << "    }\n"
               << "}\n"
               << "std::uint64_t " << className
               << "::shl_word(const std::uint64_t *source, std::uint32_t width, std::uint64_t shift, std::size_t word) {\n"
               << "    const std::int64_t start = static_cast<std::int64_t>(word) * 64 - static_cast<std::int64_t>(shift);\n"
               << "    if (start >= 0) return extract_word(source, width, static_cast<std::uint64_t>(start));\n"
               << "    const std::uint32_t back = static_cast<std::uint32_t>(-start);\n"
               << "    if (back >= 64) return 0;\n"
               << "    return (extract_word(source, width, 0) & ((UINT64_C(1) << (64U - back)) - 1U)) << back;\n"
               << "}\n"
               << "void " << className
               << "::blend_window_dyn_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *index, std::uint32_t indexWidth, const std::uint64_t *ones, const std::uint64_t *elem, std::uint32_t elemWidth) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    const std::uint64_t idx = static_cast<std::uint64_t>(index_words(index, indexWidth, targetWidth));\n"
               << "    const std::uint64_t maskedElem = elem[0] & (ones[0] & bit_mask(elemWidth >= 64U ? 64U : elemWidth));\n"
               << "    if (elemWidth == 8U && (idx & 7U) == 0 && idx + 8U <= targetWidth && ones[0] == UINT64_C(0xFF)) {\n"
               << "        bool upperZero = true;\n"
               << "        for (std::size_t i = 1; i < words; ++i) if (ones[i] != 0) { upperZero = false; break; }\n"
               << "        if (upperZero) {\n"
               << "            reinterpret_cast<std::uint8_t *>(target)[idx >> 3U] = static_cast<std::uint8_t>(maskedElem);\n"
               << "            return;\n"
               << "        }\n"
               << "    }\n"
               << "    for (std::size_t i = 0; i < words; ++i) {\n"
               << "        const std::uint64_t mask = shl_word(ones, targetWidth, idx, i);\n"
               << "        const std::uint64_t ev = shl_word(&maskedElem, 64U, idx, i);\n"
               << "        target[i] = (target[i] & ~mask) | ev;\n"
               << "    }\n"
               << "    target[words - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((words - 1U) * 64U));\n"
               << "}\n"
               << "void " << className
               << "::slice_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, std::uint64_t start) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    for (std::size_t index = 0; index < words; ++index) target[index] = extract_word(source, sourceWidth, start + index * 64U);\n"
               << "    target[words - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((words - 1U) * 64U));\n"
               << "}\n"
               << "void " << className
               << "::slice_dynamic_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, const std::uint64_t *index, std::uint32_t indexWidth) {\n"
               << "    slice_words(target, targetWidth, source, sourceWidth, index_words(index, indexWidth, sourceWidth));\n"
               << "}\n"
               << "void " << className
               << "::slice_array_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, const std::uint64_t *index, std::uint32_t indexWidth) {\n"
               << "    const std::size_t count = (sourceWidth + targetWidth - 1U) / targetWidth;\n"
               << "    const std::size_t element = index_words(index, indexWidth, count);\n"
               << "    slice_words(target, targetWidth, source, sourceWidth, element == count ? sourceWidth : element * static_cast<std::size_t>(targetWidth));\n"
               << "}\n"
               << "void " << className
               << "::array_readall_pack(std::uint64_t *target, std::uint32_t packedWidth, const std::uint64_t *source, std::uint32_t elemWidth, std::uint32_t rows) {\n"
               << "    zero_words(target, packedWidth);\n"
               << "    const std::size_t stride = word_count(elemWidth);\n"
               << "    for (std::uint32_t row = 0; row < rows; ++row) {\n"
               << "        insert_words(target, packedWidth, static_cast<std::uint64_t>(row) * elemWidth, source + static_cast<std::size_t>(row) * stride, elemWidth);\n"
               << "    }\n"
               << "}\n"
               << "void " << className
               << "::array_write_scatter(std::uint64_t *target, const std::uint64_t *laneMask, const std::uint64_t *data, std::uint32_t elemWidth, std::uint32_t rows) {\n"
               << "    const std::size_t stride = word_count(elemWidth);\n"
               << "    const std::uint32_t dataWidth = elemWidth * rows;\n"
               << "    for (std::uint32_t row = 0; row < rows; ++row) {\n"
               << "        if (((laneMask[row / 64U] >> (row % 64U)) & 1U) == 0) continue;\n"
               << "        slice_words(target + static_cast<std::size_t>(row) * stride, elemWidth, data, dataWidth, static_cast<std::uint64_t>(row) * elemWidth);\n"
               << "    }\n"
               << "}\n"
               << "bool " << className
               << "::array_write_scatter_detect(std::uint64_t *target, const std::uint64_t *laneMask, const std::uint64_t *data, std::uint32_t elemWidth, std::uint32_t rows) {\n"
               << "    const std::size_t stride = word_count(elemWidth);\n"
               << "    const std::uint32_t dataWidth = elemWidth * rows;\n"
               << "    bool changed = false;\n"
               << "    for (std::uint32_t row = 0; row < rows; ++row) {\n"
               << "        if (((laneMask[row / 64U] >> (row % 64U)) & 1U) == 0) continue;\n"
               << "        changed = slice_words_detect(target + static_cast<std::size_t>(row) * stride, elemWidth, data, dataWidth, static_cast<std::uint64_t>(row) * elemWidth) || changed;\n"
               << "    }\n"
               << "    return changed;\n"
               << "}\n"
               << "void " << className
               << "::array_mux_words(std::uint64_t *target, std::uint32_t packedWidth, const std::uint64_t *sel, const std::uint64_t *t, const std::uint64_t *f, std::uint32_t elemWidth) {\n"
               << "    const std::size_t words = word_count(packedWidth);\n"
               << "    if (elemWidth == 1U) {\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint32_t bits = index + 1U == words ? packedWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "            const std::uint64_t liveMask = bit_mask(bits);\n"
               << "            const std::uint64_t select = sel[index] & liveMask;\n"
               << "            target[index] = (t[index] & select) | (f[index] & ~select & liveMask);\n"
               << "        }\n"
               << "        return;\n"
               << "    }\n"
               << "    if ((elemWidth & (elemWidth - 1U)) == 0 && elemWidth < 32U) {\n"
               << "        const std::uint32_t lanesPerWord = 64U / elemWidth;\n"
               << "        const std::uint64_t field = (UINT64_C(1) << elemWidth) - 1U;\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint32_t bits = index + 1U == words ? packedWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "            const std::uint64_t liveMask = bit_mask(bits);\n"
               << "            const std::uint64_t firstLane = static_cast<std::uint64_t>(index) * lanesPerWord;\n"
               << "            std::uint64_t spread = (sel[firstLane / 64U] >> (firstLane % 64U)) & ((UINT64_C(1) << lanesPerWord) - 1U);\n"
               << "            for (std::uint32_t spacing = 1U; spacing < elemWidth; spacing <<= 1U) {\n"
               << "                spread = (spread | (spread << 16U)) & UINT64_C(0x0000FFFF0000FFFF);\n"
               << "                spread = (spread | (spread << 8U)) & UINT64_C(0x00FF00FF00FF00FF);\n"
               << "                spread = (spread | (spread << 4U)) & UINT64_C(0x0F0F0F0F0F0F0F0F);\n"
               << "                spread = (spread | (spread << 2U)) & UINT64_C(0x3333333333333333);\n"
               << "                spread = (spread | (spread << 1U)) & UINT64_C(0x5555555555555555);\n"
               << "            }\n"
               << "            const std::uint64_t mask = (spread * field) & liveMask;\n"
               << "            target[index] = (t[index] & mask) | (f[index] & ~mask & liveMask);\n"
               << "        }\n"
               << "        return;\n"
               << "    }\n"
               << "    for (std::size_t index = 0; index < words; ++index) {\n"
               << "        const std::uint32_t bits = index + 1U == words ? packedWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "        const std::uint64_t liveMask = bit_mask(bits);\n"
               << "        const std::uint64_t wordStart = static_cast<std::uint64_t>(index) * 64U;\n"
               << "        std::uint64_t mask = UINT64_C(0);\n"
               << "        std::uint32_t lane = static_cast<std::uint32_t>(wordStart / elemWidth);\n"
               << "        for (;;) {\n"
               << "            const std::uint64_t laneStart = static_cast<std::uint64_t>(lane) * elemWidth;\n"
               << "            if (laneStart >= wordStart + bits) break;\n"
               << "            if (((sel[lane / 64U] >> (lane % 64U)) & 1U) != 0) {\n"
               << "                const std::uint32_t lo = laneStart > wordStart ? static_cast<std::uint32_t>(laneStart - wordStart) : 0U;\n"
               << "                const std::uint64_t end = laneStart + elemWidth - wordStart;\n"
               << "                const std::uint32_t hi = end >= bits ? bits : static_cast<std::uint32_t>(end);\n"
               << "                mask |= (hi == 64U ? UINT64_MAX : ((UINT64_C(1) << hi) - 1U)) & (UINT64_MAX << lo);\n"
               << "            }\n"
               << "            ++lane;\n"
               << "        }\n"
               << "        target[index] = (t[index] & mask) | (f[index] & ~mask & liveMask);\n"
               << "    }\n"
               << "}\n"
               << "void " << className
               << "::array_broadcast_words(std::uint64_t *target, std::uint32_t packedWidth, const std::uint64_t *source, std::uint32_t sourceWidth) {\n"
               << "    const std::size_t words = word_count(packedWidth);\n"
               << "    if (sourceWidth == 1U) {\n"
               << "        const std::uint64_t fill = (source[0] & 1U) != 0 ? UINT64_MAX : UINT64_C(0);\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint32_t bits = index + 1U == words ? packedWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "            target[index] = fill & bit_mask(bits);\n"
               << "        }\n"
               << "        return;\n"
               << "    }\n"
               << "    if (sourceWidth == 64U) {\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint32_t bits = index + 1U == words ? packedWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "            target[index] = source[0] & bit_mask(bits);\n"
               << "        }\n"
               << "        return;\n"
               << "    }\n"
               << "    if ((sourceWidth & (sourceWidth - 1U)) == 0) {\n"
               << "        std::uint64_t word = source[0] & bit_mask(sourceWidth);\n"
               << "        for (std::uint32_t shift = sourceWidth; shift < 64U; shift <<= 1U) word |= word << shift;\n"
               << "        for (std::size_t index = 0; index < words; ++index) {\n"
               << "            const std::uint32_t bits = index + 1U == words ? packedWidth - static_cast<std::uint32_t>(index * 64U) : 64U;\n"
               << "            target[index] = word & bit_mask(bits);\n"
               << "        }\n"
               << "        return;\n"
               << "    }\n"
               << "    zero_words(target, packedWidth);\n"
               << "    for (std::uint64_t lsb = 0; lsb < packedWidth; lsb += sourceWidth) {\n"
               << "        insert_words(target, packedWidth, lsb, source, sourceWidth);\n"
               << "    }\n"
               << "}\n"
               << "void " << className
               << "::array_onehot_words(std::uint64_t *target, std::uint32_t rows, const std::uint64_t *index, std::uint32_t indexWidth) {\n"
               << "    zero_words(target, rows);\n"
               << "    const std::size_t lane = index_words(index, indexWidth, rows);\n"
               << "    if (lane != rows) target[lane / 64U] |= UINT64_C(1) << (lane % 64U);\n"
               << "}\n"
               << "void " << className
               << "::array_reduce_lanes_words(std::uint64_t *target, std::uint32_t rows, const std::uint64_t *source, std::uint32_t elemWidth, std::uint32_t operation) {\n"
               << "    zero_words(target, rows);\n"
               << "    const std::uint32_t sourceWidth = rows * elemWidth;\n"
               << "    for (std::uint32_t lane = 0; lane < rows; ++lane) {\n"
               << "        std::uint64_t offset = static_cast<std::uint64_t>(lane) * elemWidth;\n"
               << "        std::uint32_t remaining = elemWidth;\n"
               << "        bool reduced = operation == 0;\n"
               << "        while (remaining != 0U) {\n"
               << "            const std::uint32_t chunk = remaining > 64U ? 64U : remaining;\n"
               << "            const std::uint64_t value = extract_word(source, sourceWidth, offset) & bit_mask(chunk);\n"
               << "            if (operation == 0) reduced = reduced && (value == bit_mask(chunk));\n"
               << "            else if (operation == 2) reduced = reduced || (value != 0);\n"
               << "            else reduced = reduced != (static_cast<unsigned>(std::popcount(value) & 1U) != 0U);\n"
               << "            remaining -= chunk;\n"
               << "            offset += chunk;\n"
               << "        }\n"
               << "        if (reduced) target[lane / 64U] |= UINT64_C(1) << (lane % 64U);\n"
               << "    }\n"
               << "}\n"
               << "void " << className
               << "::shift_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool sourceSigned, const std::uint64_t *amount, std::uint32_t amountWidth, std::uint32_t operation) {\n"
               << "    const std::size_t words = word_count(targetWidth);\n"
               << "    std::vector<std::uint64_t> resized(words);\n"
               << "    assign_words(resized.data(), targetWidth, source, sourceWidth, sourceSigned);\n"
               << "    const std::size_t shift = index_words(amount, amountWidth, targetWidth);\n"
               << "    const bool negative = sourceSigned && ((resized[(targetWidth - 1U) / 64U] >> ((targetWidth - 1U) % 64U)) & 1U) != 0;\n"
               << "    zero_words(target, targetWidth);\n"
               << "    if (operation == 0) { insert_words(target, targetWidth, shift, resized.data(), targetWidth); }\n"
               << "    else { slice_words(target, targetWidth, resized.data(), targetWidth, shift); }\n"
               << "    if (operation == 2 && negative) {\n"
               << "        const std::size_t first = shift >= targetWidth ? 0 : targetWidth - shift;\n"
               << "        for (std::size_t bit = first; bit < targetWidth; ++bit) target[bit / 64U] |= UINT64_C(1) << (bit % 64U);\n"
               << "    }\n"
               << "    target[words - 1U] &= bit_mask(targetWidth - static_cast<std::uint32_t>((words - 1U) * 64U));\n"
               << "}\n"
               << "std::uint64_t " << className
               << "::split_mix64(std::uint64_t &state) {\n"
               << "    state += UINT64_C(0x9e3779b97f4a7c15);\n"
               << "    std::uint64_t value = state;\n"
               << "    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);\n"
               << "    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);\n"
               << "    return value ^ (value >> 31U);\n}\n"
               << "std::int64_t " << className
               << "::signed_value(std::uint64_t value, std::uint32_t width) {\n"
               << "    value &= bit_mask(width);\n"
               << "    if (width < 64 && ((value >> (width - 1)) & 1U)) value |= ~bit_mask(width);\n"
               << "    return static_cast<std::int64_t>(value);\n}\n"
               << "std::uint64_t " << className
               << "::divide_value(std::uint64_t lhs, std::uint64_t rhs, std::uint32_t width, bool isSigned) {\n"
               << "    lhs &= bit_mask(width); rhs &= bit_mask(width); if (rhs == 0) return 0;\n"
               << "    if (!isSigned) return (lhs / rhs) & bit_mask(width);\n"
               << "    const std::int64_t a = signed_value(lhs, width), b = signed_value(rhs, width);\n"
               << "    if (width == 64 && a == std::numeric_limits<std::int64_t>::min() && b == -1) return lhs;\n"
               << "    return static_cast<std::uint64_t>(a / b) & bit_mask(width);\n}\n"
               << "std::uint64_t " << className
               << "::modulo_value(std::uint64_t lhs, std::uint64_t rhs, std::uint32_t width, bool isSigned) {\n"
               << "    lhs &= bit_mask(width); rhs &= bit_mask(width); if (rhs == 0) return 0;\n"
               << "    if (!isSigned) return (lhs % rhs) & bit_mask(width);\n"
               << "    const std::int64_t a = signed_value(lhs, width), b = signed_value(rhs, width);\n"
               << "    if (width == 64 && a == std::numeric_limits<std::int64_t>::min() && b == -1) return 0;\n"
               << "    return static_cast<std::uint64_t>(a % b) & bit_mask(width);\n}\n"
               << "std::uint64_t " << className
               << "::shift_left(std::uint64_t value, std::uint64_t amount, std::uint32_t width, bool) { return amount >= width ? 0 : (value << amount) & bit_mask(width); }\n"
               << "std::uint64_t " << className
               << "::shift_right(std::uint64_t value, std::uint64_t amount, std::uint32_t width, bool) { return amount >= width ? 0 : (value & bit_mask(width)) >> amount; }\n"
               << "std::uint64_t " << className
               << "::arithmetic_shift_right(std::uint64_t value, std::uint64_t amount, std::uint32_t width, bool isSigned) {\n"
               << "    if (!isSigned) return shift_right(value, amount, width, false);\n"
               << "    const bool negative = ((value >> (width - 1)) & 1U) != 0;\n"
               << "    if (amount >= width) return negative ? bit_mask(width) : 0;\n"
               << "    if (!negative || amount == 0) return shift_right(value, amount, width, false);\n"
               << "    const std::uint64_t fill = width == 64 ? (~UINT64_C(0) << (64 - amount)) : (bit_mask(static_cast<std::uint32_t>(amount)) << (width - amount));\n"
               << "    return (shift_right(value, amount, width, false) | fill) & bit_mask(width);\n}\n"
               << "std::uint64_t " << className
               << "::slice_value(std::uint64_t value, std::uint64_t start, std::uint32_t width) { return start >= 64 ? 0 : (value >> start) & bit_mask(width); }\n"
               << "std::uint64_t " << className
               << "::slice_array_value(std::uint64_t value, std::uint64_t index, std::uint32_t width, std::uint32_t baseWidth) {\n"
               << "    if (width == 0 || index >= (baseWidth + width - 1) / width) return 0;\n"
               << "    return slice_value(value, index * width, width);\n}\n\n"
               << "void " << className
               << "::init() {\n";
        // Zero the contiguous v<K> member region with one memset (members
        // are declared without initializers; see the header emission note).
        if (state.memberValueCount != 0)
        {
            runtime << "    std::memset(&v" << state.firstMemberVariable
                    << ", 0, sizeof(v" << state.firstMemberVariable << ") * "
                    << state.memberValueCount << "U);\n";
        }
        runtime << "    changedResults_.fill(0); wideValues_.fill(0); realValues_.fill(0);\n"
               << "    for (std::string &value : stringValues_) value.clear();\n"
               << "    activeWords_.fill(0); backwardFired_ = false;\n"
               << "    dirtyChangedBits_.fill(0); dirtyChangedResults_.clear(); onceCompleted_.fill(false);\n";
        if (state.commitInputGating)
        {
            runtime << "    commitInputSnapshots_.fill(0); commitInputValid_.fill(0); "
                       "commitInputDirty_.fill(0);\n";
        }
        // NO0017 §5: re-zero the exploded element arrays on (re-)init, same
        // contract as the pool fill above; literal init stores follow below.
        for (uint32_t index = 0; index < state.explodedElementWidth.size(); ++index)
        {
            if (state.explodedElementWidth[index] == 0)
            {
                continue;
            }
            runtime << "    " << explodedMemberName(VariableId{index}) << ".fill(0);\n";
        }
        runtime << "    firstEval_ = true; roundCounter_ = 0; finalized_ = false;\n"
               << "    finishRequested_ = false; stopRequested_ = false; fatalRequested_ = false; systemExitCode_ = 0;\n"
               << "    std::uint64_t initRandomState = randomSeed_;\n";
        const auto emitSetInitialization = [&](VariableId variable,
                                               const InitExpr &expression,
                                               std::size_t actionIndex) {
            const Type &type = variableType(state, variable);
            const EmitState::Storage &storage = variableStorage(state, variable);
            const bool seeded = expression.kind == InitExprKind::RandomSeeded;
            const std::string seedName = "seededInitState_" +
                                         std::to_string(variable.value) + "_" +
                                         std::to_string(actionIndex);
            if (seeded)
            {
                runtime << "    { std::uint64_t " << seedName << " = UINT64_C("
                        << expression.seed << ");\n";
            }
            const std::string randomState = seeded ? seedName : "initRandomState";

            if (type.kind == TypeKind::BitVector && type.bitWidth <= 64)
            {
                std::string value;
                if (expression.kind == InitExprKind::Literal)
                {
                    const LiteralView literal = program.literal(expression.literal);
                    const uint64_t word = literal.words.empty() ? 0 : literal.words.front();
                    value = "UINT64_C(" + std::to_string(word) + ")";
                }
                else
                {
                    value = "split_mix64(" + randomState + ")";
                }
                runtime << "    " << valueExpr(state, variable) << " = (" << value
                        << ") & " << maskExpr(type.bitWidth) << ";\n";
            }
            else if (type.kind == TypeKind::BitVector)
            {
                std::optional<LiteralView> literal;
                if (expression.kind == InitExprKind::Literal)
                {
                    literal = program.literal(expression.literal);
                }
                if (isExplodedState(state, variable))
                {
                    // NO0017 §5: exploded states take literal init only
                    // (random init keeps the pool path by the planner).
                    // Per-element stores in ascending order so the compiler
                    // can fold constant runs (fill(0) above covers zeros).
                    const uint32_t elemWidth = state.explodedElementWidth[variable.value];
                    const uint64_t elemCount = type.bitWidth / elemWidth;
                    for (uint64_t elem = 0; elem < elemCount; ++elem)
                    {
                        const uint64_t lo = elem * elemWidth;
                        const uint64_t wordIndex = lo / 64U;
                        const uint32_t shift = static_cast<uint32_t>(lo % 64U);
                        uint64_t value =
                            literal && wordIndex < literal->words.size()
                                ? literal->words[wordIndex] >> shift
                                : 0;
                        if (shift != 0 && shift + elemWidth > 64U)
                        {
                            const uint64_t next =
                                literal && wordIndex + 1U < literal->words.size()
                                    ? literal->words[wordIndex + 1U]
                                    : 0;
                            value |= next << (64U - shift);
                        }
                        value &= elemWidth >= 64
                                     ? UINT64_MAX
                                     : (UINT64_C(1) << elemWidth) - UINT64_C(1);
                        if (value == 0)
                        {
                            continue;
                        }
                        runtime << "    " << explodedMemberName(variable) << "[" << elem
                                << "] = (UINT64_C(" << value << ")) & "
                                << maskExpr(elemWidth) << ";\n";
                    }
                    return;
                }
                for (uint32_t word = 0; word < storage.wordCount; ++word)
                {
                    std::string value;
                    if (literal)
                    {
                        const uint64_t payload =
                            word < literal->words.size() ? literal->words[word] : 0;
                        value = "UINT64_C(" + std::to_string(payload) + ")";
                    }
                    else
                    {
                        value = "split_mix64(" + randomState + ")";
                    }
                    const uint32_t bits = word + 1U == storage.wordCount
                                              ? type.bitWidth - word * 64U
                                              : 64U;
                    runtime << "    wideValues_[" << storage.offset + word << "] = ("
                            << value << ") & " << maskExpr(bits) << ";\n";
                }
            }
            else if (type.kind == TypeKind::Real)
            {
                const LiteralView literal = program.literal(expression.literal);
                const uint64_t word = literal.words.empty() ? 0 : literal.words.front();
                runtime << "    realValues_[" << storage.offset << "] = UINT64_C("
                        << word << ");\n";
            }
            else if (type.kind == TypeKind::String)
            {
                const LiteralView literal = program.literal(expression.literal);
                runtime << "    stringValues_[" << storage.offset << "] = "
                        << cppStringLiteral(literal.bytes) << ";\n";
            }
            if (seeded)
            {
                runtime << "    }\n";
            }
        };
        const auto emitFillInitialization = [&](VariableId variable,
                                                const InitAction &action,
                                                std::size_t actionIndex) {
            const Type &type = variableType(state, variable);
            const EmitState::Storage &storage = variableStorage(state, variable);
            const bool seeded = action.expression.kind == InitExprKind::RandomSeeded;
            const std::string suffix = std::to_string(variable.value) + "_" +
                                       std::to_string(actionIndex);
            const std::string seedName = "seededInitState_" + suffix;
            const std::string elementName = "initElement_" + suffix;
            if (seeded)
            {
                runtime << "    { std::uint64_t " << seedName << " = UINT64_C("
                        << action.expression.seed << ");\n";
            }
            const std::string randomState = seeded ? seedName : "initRandomState";
            std::optional<LiteralView> literal;
            if (action.expression.kind == InitExprKind::Literal)
            {
                literal = program.literal(action.expression.literal);
            }
            if (literal && storage.wordCount == 1)
            {
                const uint64_t payload = literal->words.empty() ? 0 : literal->words.front();
                runtime << "    std::fill_n(wideValues_.data() + "
                        << storage.offset + action.start << ", " << action.count
                        << ", (UINT64_C(" << payload << ")) & "
                        << maskExpr(type.bitWidth) << ");\n";
                return;
            }
            runtime << "    for (std::size_t " << elementName << " = " << action.start
                    << "; " << elementName << " < " << action.start + action.count
                    << "; ++" << elementName << ") {\n";
            for (uint32_t word = 0; word < storage.wordCount; ++word)
            {
                std::string value;
                if (literal)
                {
                    const uint64_t payload =
                        word < literal->words.size() ? literal->words[word] : 0;
                    value = "UINT64_C(" + std::to_string(payload) + ")";
                }
                else
                {
                    value = "split_mix64(" + randomState + ")";
                }
                const uint32_t bits = word + 1U == storage.wordCount
                                          ? type.bitWidth - word * 64U
                                          : 64U;
                runtime << "        wideValues_[" << storage.offset << " + " << elementName
                        << " * " << storage.wordCount << " + " << word << "] = (" << value
                        << ") & " << maskExpr(bits) << ";\n";
            }
            runtime << "    }\n";
            if (seeded)
            {
                runtime << "    }\n";
            }
        };
        const auto sameLiteralExpression = [&](const InitExpr &lhs, const InitExpr &rhs) {
            if (lhs.kind != InitExprKind::Literal || rhs.kind != InitExprKind::Literal)
            {
                return false;
            }
            if (lhs.literal == rhs.literal)
            {
                return true;
            }
            const LiteralView lhsLiteral = program.literal(lhs.literal);
            const LiteralView rhsLiteral = program.literal(rhs.literal);
            return lhsLiteral.type == rhsLiteral.type &&
                   lhsLiteral.words.size() == rhsLiteral.words.size() &&
                   std::equal(lhsLiteral.words.begin(), lhsLiteral.words.end(),
                              rhsLiteral.words.begin()) &&
                   lhsLiteral.bytes == rhsLiteral.bytes;
        };
        for (uint32_t index = 0; index < program.variableCount(); ++index)
        {
            const VariableId variable{index};
            const InitDescriptor &init = program.init(program.variable(variable).init);
            if (init.kind == InitKind::Constant)
            {
                emitSetInitialization(variable,
                                      InitExpr{
                                          .kind = InitExprKind::Literal,
                                          .literal = LiteralId{init.payload},
                                      },
                                      0);
            }
            else if (init.kind == InitKind::Actions)
            {
                const std::span<const InitAction> actions =
                    program.initActions(program.variable(variable).init);
                std::size_t actionIndex = 0;
                while (actionIndex < actions.size())
                {
                    const InitAction &action = actions[actionIndex];
                    std::size_t nextActionIndex = actionIndex + 1;
                    if (action.kind == InitActionKind::Fill)
                    {
                        InitAction mergedAction = action;
                        while (mergedAction.expression.kind == InitExprKind::Literal &&
                               nextActionIndex < actions.size())
                        {
                            const InitAction &next = actions[nextActionIndex];
                            if (next.kind != InitActionKind::Fill ||
                                mergedAction.start + mergedAction.count != next.start ||
                                !sameLiteralExpression(mergedAction.expression,
                                                       next.expression))
                            {
                                break;
                            }
                            mergedAction.count += next.count;
                            ++nextActionIndex;
                        }
                        emitFillInitialization(variable, mergedAction, actionIndex);
                    }
                    else
                    {
                        emitSetInitialization(variable, action.expression, actionIndex);
                    }
                    actionIndex = nextActionIndex;
                }
            }
        }
        runtime << "}\n\nbool " << className
                << "::is_commit_block(std::size_t block) {\n"
                << "    return kCommitBlockBegin != 0 && block >= kCommitBlockBegin && block < kCommitBlockEnd;\n"
                << "}\n\nvoid " << className
                << "::mark_changed_result(std::size_t variable) {\n"
                << (state.runtimeProfile
                        ? "    if (runtimeProfileEnabled_) ++profileChangedMarks_;\n"
                        : "")
                << "    const std::size_t word = variable / 64U;\n"
                << "    const std::uint64_t bit = UINT64_C(1) << (variable % 64U);\n"
                << "    if ((dirtyChangedBits_[word] & bit) == 0) {\n"
                << "        dirtyChangedBits_[word] |= bit;\n"
                << "        dirtyChangedResults_.push_back(static_cast<std::uint32_t>(variable));\n"
                << "    }\n"
                << "}\n\nvoid " << className
                << "::clear_changed_results() {\n"
                << (state.runtimeProfile
                        ? "    if (runtimeProfileEnabled_) profileChangedClears_ += dirtyChangedResults_.size();\n"
                        : "")
                << (state.changedTrace
                        ? "    if (traceChangedFile_ != nullptr && !dirtyChangedResults_.empty() &&\n"
                          "        traceChangedEval_ >= traceChangedBegin_ && traceChangedEval_ <= traceChangedEnd_) {\n"
                          "        const std::uint64_t traceHeader[3] = { traceChangedEval_, roundCounter_,\n"
                          "                                               static_cast<std::uint64_t>(dirtyChangedResults_.size()) };\n"
                          "        std::fwrite(traceHeader, sizeof(traceHeader), 1, traceChangedFile_);\n"
                          "        std::fwrite(dirtyChangedResults_.data(), sizeof(std::uint32_t),\n"
                          "                    dirtyChangedResults_.size(), traceChangedFile_);\n"
                          "    }\n"
                        : "")
                << "    for (const std::uint32_t variable : dirtyChangedResults_) {\n"
                << "        changedResults_[variable] = 0;\n"
                << "        dirtyChangedBits_[variable / 64U] &= ~(UINT64_C(1) << (variable % 64U));\n"
                << "    }\n"
                << "    dirtyChangedResults_.clear();\n"
                << "}\n";
        if (state.changedTrace)
        {
            runtime << "\nvoid " << className
                    << "::trace_changed_init() {\n"
                    << "    traceChangedInit_ = true;\n"
                    << "    const char *path = std::getenv(\"EMU_AM_CHANGED_TRACE\");\n"
                    << "    if (path == nullptr || path[0] == '\\0') return;\n"
                    << "    if (const char *begin = std::getenv(\"EMU_AM_TRACE_BEGIN_EVAL\")) traceChangedBegin_ = std::strtoull(begin, nullptr, 10);\n"
                    << "    if (const char *end = std::getenv(\"EMU_AM_TRACE_END_EVAL\")) traceChangedEnd_ = std::strtoull(end, nullptr, 10);\n"
                    << "    traceChangedFile_ = std::fopen(path, \"wb\");\n"
                    << "}\n";
        }
        runtime << "\nvoid " << className << "::finalize() {\n"
                << "    if (finalized_) return;\n"
                << "    finalized_ = true;\n"
                << (state.changedTrace
                        ? "    if (traceChangedFile_ != nullptr) { std::fclose(traceChangedFile_); traceChangedFile_ = nullptr; }\n"
                        : "");
        for (InstructionId instruction : state.finalSystemTasks)
        {
            std::string error;
            std::optional<std::string> code =
                emitSystemTaskInstruction(state, instruction, true, error);
            if (!code)
            {
                diagnostics.error(error + ": instruction=" +
                                      std::to_string(instruction.value),
                                  std::string(kContext));
                return result;
            }
            writeIndentedLines(runtime, *code, "    ");
        }
        runtime << "    std::cout.flush();\n"
                << "    std::cerr.flush();\n"
                << "}\n\n"
                << "void " << className << "::eval() {\n"
                << "    if (finalized_) throw std::runtime_error(\"cannot eval a finalized AM model\");\n"
                << (state.runtimeProfile
                        ? "    if (runtimeProfileEnabled_) ++profileEvalCalls_;\n"
                          "    const auto profileEvalStart = std::chrono::steady_clock::now();\n"
                        : "")
                << (state.changedTrace
                        ? "    if (!traceChangedInit_) trace_changed_init();\n"
                          "    ++traceChangedEval_;\n"
                        : "");
        for (const PortBinding &port : model.interface.ports)
        {
            if (port.direction != PortDirection::Input)
            {
                continue;
            }
            const Type &type = variableType(state, port.input);
            if (type.bitWidth <= 64)
            {
                runtime << "    " << valueExpr(state, port.input)
                        << " = static_cast<std::uint64_t>(" << sanitizeCppIdentifier(program.string(port.name))
                        << ") & " << maskExpr(type.bitWidth) << ";\n";
            }
            else
            {
                const EmitState::Storage &storage = variableStorage(state, port.input);
                for (uint32_t word = 0; word < storage.wordCount; ++word)
                {
                    const uint32_t bits = word + 1U == storage.wordCount
                                              ? type.bitWidth - word * 64U
                                              : 64U;
                    runtime << "    wideValues_[" << storage.offset + word << "] = "
                            << sanitizeCppIdentifier(program.string(port.name)) << "[" << word << "] & "
                            << maskExpr(bits) << ";\n";
                }
            }
        }
        runtime << "    const bool initial = firstEval_;\n"
                << "    activeWords_.fill(0);\n"
                << "    clear_changed_results();\n"
                << "    pendingHostEvents_.fill(false);\n"
                << "    roundCounter_ = 0;\n"
                << "    execute_block_0();\n"
                << "    if (initial) {\n"
                << "        // First eval activates every Block: compute Blocks settle the\n"
                << "        // comb cloud and commit Blocks sync every gate-detector baseline\n"
                << "        // and evaluate every state write once.\n"
                << "        for (std::size_t block = 1; block < kBlockCount; ++block) {\n"
                << "            activeWords_[block / 64U] |= UINT64_C(1) << (block % 64U);\n"
                << "        }\n"
                << "    }\n"
                << "    while (true) {\n"
                << "        backwardFired_ = false;\n"
                << (state.runtimeProfile
                        ? "        const auto profileComputeStart = runtimeProfileEnabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};\n"
                        : "");
        // Compute phase: one static call per source part covering compute
        // Blocks, in ascending (source, part) order; each part consumes its
        // activity byte chunks with straight-line ascending bit tests.
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            for (const BlockSourcePart &part : sourceParts)
            {
                const auto [scanLo, scanHi] =
                    computeBlockRange(part.firstBlock,
                                      part.endBlock,
                                      blockCount,
                                      model.commitBlockBegin);
                if (scanLo >= scanHi)
                {
                    continue;
                }
                runtime << "        "
                        << scanSourceFunctionName(part.sourceIndex, part.partIndex)
                        << "();\n";
            }
        }
        runtime << (state.runtimeProfile
                        ? "        if (runtimeProfileEnabled_) profileComputeNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - profileComputeStart).count());\n"
                        : "")
                << (state.runtimeProfile
                        ? "        const auto profileCommitStart = runtimeProfileEnabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};\n"
                        : "");
        // Commit phase: one static call per source part covering commit
        // Blocks, in ascending (source, part) order; each part scans its
        // activity byte chunks exactly like the compute phase.
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            for (const BlockSourcePart &part : sourceParts)
            {
                const auto [commitLo, commitHi] =
                    commitBlockRange(part.firstBlock,
                                     part.endBlock,
                                     model.commitBlockBegin,
                                     model.commitBlockEnd);
                if (commitLo >= commitHi)
                {
                    continue;
                }
                runtime << "        "
                        << commitSourceFunctionName(part.sourceIndex, part.partIndex)
                        << "();\n";
            }
        }
        runtime << (state.runtimeProfile
                        ? "        if (runtimeProfileEnabled_) profileCommitNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - profileCommitStart).count());\n"
                        : "")
                << "        clear_changed_results();\n"
                << "        ++roundCounter_;\n"
                << "        if (roundCounter_ > UINT64_C(1000000)) throw std::runtime_error(\"AM eval did not converge\");\n"
                << "        if (!backwardFired_) break;\n"
                << "    }\n"
                << "    if (initial) firstEval_ = false;\n";
        // Debug change-trace (WOLVRIX_GRHSIM_AM_TRACE selects labeled variables
        // by substring at emit time; WOLVRIX_GRHSIM_AM_TRACE_RUN gates printing
        // at runtime). Prints each traced variable's value when it changes.
        if (const char *traceSpec = std::getenv("WOLVRIX_GRHSIM_AM_TRACE")) {
            std::vector<std::string> needles;
            std::string spec(traceSpec);
            std::size_t begin = 0;
            while (begin <= spec.size()) {
                const std::size_t comma = spec.find(',', begin);
                needles.push_back(comma == std::string::npos
                                      ? spec.substr(begin)
                                      : spec.substr(begin, comma - begin));
                if (comma == std::string::npos) {
                    break;
                }
                begin = comma + 1;
            }
            const ProgramView traceView = model.program.view();
            runtime << "    if (std::getenv(\"WOLVRIX_GRHSIM_AM_TRACE_RUN\") != nullptr) {\n"
                    << "        static std::uint64_t traceEval = 0; ++traceEval;\n";
            for (const VariableLabel &label : traceView.variableLabels()) {
                const std::string_view name = traceView.string(label.label);
                bool matches = false;
                for (const std::string &needle : needles) {
                    if (!needle.empty() && name.find(needle) != std::string_view::npos) {
                        matches = true;
                        break;
                    }
                }
                if (!matches) {
                    continue;
                }
                const Type &type = traceView.type(traceView.variable(label.variable).type);
                if (type.kind != TypeKind::BitVector || type.bitWidth > 64) {
                    continue;
                }
                const std::string field = "v" + std::to_string(label.variable.value);
                runtime << "        { static std::uint64_t shadow = ~UINT64_C(0);"
                        << " if (" << field << " != shadow) {"
                        << " std::fprintf(stderr, \"[tr] ev=%llu " << name << " %llu->%llu\\n\","
                        << " (unsigned long long)traceEval, (unsigned long long)shadow,"
                        << " (unsigned long long)" << field << "); shadow = " << field
                        << "; } }\n";
            }
            runtime << "    }\n";
        }
        runtime << (state.runtimeProfile
                        ? "    if (runtimeProfileEnabled_) {\n"
                          "        profileRounds_ += roundCounter_;\n"
                          "        profileEvalNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - profileEvalStart).count());\n"
                          "    }\n"
                        : "");
        for (const PortBinding &port : model.interface.ports)
        {
            if (port.direction != PortDirection::Output)
            {
                continue;
            }
            const Type &type = variableType(state, port.output);
            if (type.bitWidth <= 64)
            {
                runtime << "    " << sanitizeCppIdentifier(program.string(port.name)) << " = static_cast<"
                        << cppScalarType(type.bitWidth) << ">("
                        << valueExpr(state, port.output) << ");\n";
            }
            else
            {
                const EmitState::Storage &storage = variableStorage(state, port.output);
                for (uint32_t word = 0; word < storage.wordCount; ++word)
                {
                    runtime << "    " << sanitizeCppIdentifier(program.string(port.name)) << "[" << word
                            << "] = wideValues_[" << storage.offset + word << "];\n";
                }
            }
        }
        runtime << "}\n\n"
                << "void " << className
                << "::set_random_seed(std::uint64_t seed) { randomSeed_ = seed; }\n"
                << "bool " << className
                << "::had_register_write_conflict() const { return false; }\n";
        if (state.runtimeProfile)
        {
            runtime << "void " << className
                    << "::set_runtime_profile_enabled(bool enabled) { runtimeProfileEnabled_ = enabled; }\n"
                    << "bool " << className
                    << "::runtime_profile_enabled() const { return runtimeProfileEnabled_; }\n"
                    << "void " << className << "::dump_runtime_profile() const {\n"
                    // NO0010: one-time rdtsc calibration at dump time (same host,
                    // once per run): empty pair cost + TSC frequency, so offline
                    // analysis can subtract per-fire overhead and convert cycles.
                    << "    std::uint64_t profileRdtscOverhead = 0;\n"
                    << "    std::uint64_t profileTscHz = 0;\n"
                    << "    {\n"
                    << "        std::uint64_t best = ~0ull;\n"
                    << "        for (int i = 0; i < 100000; ++i) { const std::uint64_t a = wolvrixAmRdtsc(); const std::uint64_t b = wolvrixAmRdtsc(); if (b - a < best) best = b - a; }\n"
                    << "        profileRdtscOverhead = best;\n"
                    << "        const std::chrono::steady_clock::time_point calStart = std::chrono::steady_clock::now();\n"
                    << "        const std::uint64_t calT0 = wolvrixAmRdtsc();\n"
                    << "        while (std::chrono::steady_clock::now() - calStart < std::chrono::milliseconds(2)) {}\n"
                    << "        profileTscHz = (wolvrixAmRdtsc() - calT0) * 500;\n"
                    << "    }\n"
                    << "    std::cerr << \"[am-profile] tsc_hz: \" << profileTscHz << \", rdtsc_overhead: \" << profileRdtscOverhead << \"\\n\";\n"
                    << "    const std::uint64_t totalBlockExecs = profileBlockExecs_ + profileCommitBlockExecs_;\n"
                    << "    const double evalMs = static_cast<double>(profileEvalNs_) / 1.0e6;\n"
                    << "    const double computeMs = static_cast<double>(profileComputeNs_) / 1.0e6;\n"
                    << "    const double commitMs = static_cast<double>(profileCommitNs_) / 1.0e6;\n"
                    << "    std::cerr << \"[am-profile] eval calls: \" << profileEvalCalls_ << \", rounds: \" << profileRounds_\n"
                    << "              << \" (\" << (profileEvalCalls_ != 0 ? static_cast<double>(profileRounds_) / static_cast<double>(profileEvalCalls_) : 0.0) << \" per eval)\\n\";\n"
                    << "    std::cerr << \"[am-profile] block execs: \" << totalBlockExecs << \" (compute \" << profileBlockExecs_\n"
                    << "              << \", commit \" << profileCommitBlockExecs_ << \")\\n\";\n"
                    << "    std::cerr << \"[am-profile] activations: forward \" << profileActivateForward_ << \", backward \" << profileActivateBackward_ << \"\\n\";\n"
                    << "    std::cerr << \"[am-profile] changed marks: \" << profileChangedMarks_ << \", clears \" << profileChangedClears_ << \"\\n\";\n"
                    << "    std::cerr << \"[am-profile] time ms: eval \" << evalMs << \", compute \" << computeMs << \" (\"\n"
                    << "              << (evalMs > 0.0 ? 100.0 * computeMs / evalMs : 0.0) << \"%), commit \" << commitMs << \" (\"\n"
                    << "              << (evalMs > 0.0 ? 100.0 * commitMs / evalMs : 0.0) << \"%), other \" << (evalMs - computeMs - commitMs) << \"\\n\";\n"
                    << "    std::vector<std::size_t> order(kBlockCount);\n"
                    << "    for (std::size_t index = 0; index < kBlockCount; ++index) order[index] = index;\n"
                    << "    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) { return profilePerBlockExecs_[lhs] > profilePerBlockExecs_[rhs]; });\n"
                    << "    const std::size_t topCount = kBlockCount < 32 ? kBlockCount : 32;\n"
                    << "    std::cerr << \"[am-profile] top blocks by exec count:\\n\";\n"
                    << "    for (std::size_t rank = 0; rank < topCount; ++rank) {\n"
                    << "        const std::size_t block = order[rank];\n"
                    << "        if (profilePerBlockExecs_[block] == 0) break;\n"
                    << "        std::cerr << \"  block \" << block << (is_commit_block(block) ? \" (commit)\" : \"\") << \": \" << profilePerBlockExecs_[block]\n"
                    << "                  << \" (\" << (totalBlockExecs != 0 ? 100.0 * static_cast<double>(profilePerBlockExecs_[block]) / static_cast<double>(totalBlockExecs) : 0.0) << \"%)\\n\";\n"
                    << "    }\n"
                    // NO0017 B-layer: optionally stream the full per-block exec
                    // counts (one "block kind execs" line per block) to the file
                    // named by EMU_AM_BLOCK_EXECS, for offline dynamic
                    // instruction-count derivation.
                    << "    if (const char *blockExecsPath = std::getenv(\"EMU_AM_BLOCK_EXECS\")) {\n"
                    << "        if (std::FILE *blockExecsFile = std::fopen(blockExecsPath, \"w\")) {\n"
                    << "            for (std::size_t block = 0; block < kBlockCount; ++block) {\n"
                    << "                std::fprintf(blockExecsFile, \"%zu %c %llu %llu\\n\", block,\n"
                    << "                             is_commit_block(block) ? 'c' : 'w',\n"
                    << "                             static_cast<unsigned long long>(profilePerBlockExecs_[block]),\n"
                    << "                             static_cast<unsigned long long>(profilePerBlockCycles_[block]));\n"
                    << "            }\n"
                    << "            std::fclose(blockExecsFile);\n"
                    << "        }\n"
                    << "    }\n"
                    << "}\n";
        }
        else
        {
            runtime << "void " << className
                    << "::set_runtime_profile_enabled(bool enabled) { (void)enabled; }\n"
                    << "bool " << className
                    << "::runtime_profile_enabled() const { return false; }\n"
                    << "void " << className << "::dump_runtime_profile() const {}\n";
        }
        runtime << "bool " << className
                << "::finish_requested() const { return finishRequested_; }\n"
                << "bool " << className
                << "::stop_requested() const { return stopRequested_; }\n"
                << "bool " << className
                << "::fatal_requested() const { return fatalRequested_; }\n"
                << "int " << className << "::system_exit_code() const { return systemExitCode_; }\n"
                << "const std::string &" << className
                << "::dumpfile_path() const { return emptyPath_; }\n"
                << "bool " << className << "::dumpvars_enabled() const { return false; }\n";

        std::vector<std::string> sourceNames;
        sourceNames.reserve(blockPartCount + 1U);
        sourceNames.push_back(prefix + "_runtime.cpp");
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            for (const BlockSourcePart &part : sourceParts)
            {
                sourceNames.push_back(
                    blockSourceFilename(prefix, part.sourceIndex, part.partIndex));
            }
        }

        std::ostringstream makefile;
        // The GNU make built-in default CXX=g++ counts as defined for `?=`
        // but cannot consume the clang-style PCH flags below; only honor an
        // explicit environment/command-line compiler, otherwise pin clang++.
        makefile << "ifeq ($(origin CXX),default)\n"
                 << "CXX := clang++\n"
                 << "endif\n"
                 << "AR ?= ar\n"
                 << "ARFLAGS ?= rcs\n"
                 << "CXXFLAGS ?= -std=c++20 -O3\n"
                 << "LIB := lib" << prefix << ".a\n"
                 << "SRCS :=";
        for (const std::string &sourceName : sourceNames)
        {
            makefile << " " << sourceName;
        }
        // Precompile the model header (which carries one member declaration
        // per persistent narrow value) once per build, mirroring the legacy
        // GSIM emitter Makefile; the support header stays a textual include.
        makefile << "\n"
                 << "OBJS := $(SRCS:.cpp=.o)\n"
                 << "PCH_HEADER := " << prefix << ".hpp\n"
                 << "PCH_FILE := $(PCH_HEADER).pch\n\n"
                 << "all: $(LIB)\n\n"
                 << "$(LIB): $(OBJS)\n\t$(AR) $(ARFLAGS) $@ $^\n\n"
                 << "$(PCH_FILE): $(PCH_HEADER)\n"
                 << "\t$(CXX) $(CXXFLAGS) -I. -x c++-header $< -o $@\n\n"
                 << "%.o: %.cpp $(PCH_FILE) " << prefix << "_support.hpp\n"
                 << "\t$(CXX) $(CXXFLAGS) -I. -include-pch $(PCH_FILE) -c $< -o $@\n\n"
                 << "clean:\n\trm -f $(OBJS) $(LIB) $(PCH_FILE)\n";

        try
        {
            std::filesystem::create_directories(options.outputDirectory);
        }
        catch (const std::filesystem::filesystem_error &error)
        {
            diagnostics.error(
                "failed to create AM C++ output directory: " + std::string(error.what()),
                std::string(kContext));
            return result;
        }
        std::filesystem::path stagingDirectory;
        const std::string stagingPrefix =
            "." + prefix + ".staging-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::size_t attempt = 0; attempt != 1024; ++attempt)
        {
            const std::filesystem::path candidate =
                options.outputDirectory / (stagingPrefix + "-" + std::to_string(attempt));
            std::error_code filesystemError;
            if (std::filesystem::create_directory(candidate, filesystemError))
            {
                stagingDirectory = candidate;
                break;
            }
            if (filesystemError)
            {
                diagnostics.error("failed to create AM C++ staging directory: " +
                                      filesystemError.message(),
                                  std::string(kContext));
                return result;
            }
        }
        if (stagingDirectory.empty())
        {
            diagnostics.error("failed to allocate a unique AM C++ staging directory",
                              std::string(kContext));
            return result;
        }
        const auto discardStaging = [&] {
            std::error_code filesystemError;
            std::filesystem::remove_all(stagingDirectory, filesystemError);
        };

        const std::filesystem::path headerPath = options.outputDirectory / (prefix + ".hpp");
        const std::filesystem::path supportPath =
            options.outputDirectory / (prefix + "_support.hpp");
        const std::filesystem::path runtimePath =
            options.outputDirectory / (prefix + "_runtime.cpp");
        const std::filesystem::path makefilePath = options.outputDirectory / "Makefile";
        const std::filesystem::path stagedHeaderPath = stagingDirectory / headerPath.filename();
        const std::filesystem::path stagedSupportPath = stagingDirectory / supportPath.filename();
        const std::filesystem::path stagedRuntimePath = stagingDirectory / runtimePath.filename();
        const std::filesystem::path stagedMakefilePath = stagingDirectory / makefilePath.filename();
        if (!writeFile(stagedHeaderPath, header.str(), options.maxOutputFileBytes, diagnostics) ||
            !writeFile(stagedSupportPath, support.str(), options.maxOutputFileBytes, diagnostics) ||
            !writeFile(stagedRuntimePath, runtime.str(), options.maxOutputFileBytes, diagnostics) ||
            !writeFile(stagedMakefilePath, makefile.str(), options.maxOutputFileBytes, diagnostics))
        {
            discardStaging();
            return result;
        }

        std::vector<std::filesystem::path> blockPaths;
        blockPaths.reserve(blockPartCount);
        bool blocksGenerated = true;
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            for (const BlockSourcePart &part : sourceParts)
            {
                const std::filesystem::path blockPath =
                    stagingDirectory /
                    blockSourceFilename(prefix, part.sourceIndex, part.partIndex);
                blockPaths.push_back(blockPath);
                std::ofstream blockSource(blockPath, std::ios::binary | std::ios::trunc);
                if (!blockSource)
                {
                    diagnostics.error("failed to open generated artifact: " +
                                          blockPath.string(),
                                      std::string(kContext));
                    blocksGenerated = false;
                    break;
                }

                const auto writeBlockBody = [&](std::size_t blockIndex,
                                                std::string_view indentation,
                                                const EmitState::CommitGate *gate = nullptr) {
                    if (state.traceComments)
                    {
                        blockSource << indentation
                                    << blockTraceBanner(
                                           model, static_cast<uint32_t>(blockIndex));
                    }
                    writeIndentedLines(
                        blockSource,
                        commitInputDirtyBlockMarks(
                            state, static_cast<uint32_t>(blockIndex)),
                        indentation);
                    beginLocalityBlock(state, static_cast<uint32_t>(blockIndex));
                    writeIndentedLines(blockSource,
                                       localValueDeclarations(state),
                                       indentation);
                    const EmitState::ArrayWatchPlan *arrayPlan =
                        arrayWatchPlanFor(state, blockIndex);
                    writeIndentedLines(blockSource,
                                       arrayWatchDeclarations(arrayPlan),
                                       indentation);
                    const EmitState::ScalarWatchPlan *scalarPlan =
                        scalarWatchPlanFor(state, blockIndex);
                    writeIndentedLines(blockSource,
                                       scalarWatchDeclarations(scalarPlan),
                                       indentation);
                    const BlockId block{static_cast<uint32_t>(blockIndex)};
                    const std::size_t blockSize = model.program.blockSize(block);
                    const std::string gatedIndentation =
                        std::string(indentation) + "    ";
                    const EmitState::DetectorGroupPlan *detectorPlan =
                        detectorPlanFor(state, blockIndex);
                    std::vector<uint8_t> detectorGroupDeclared(
                        detectorPlan != nullptr ? detectorPlan->groups.size() : 0, 0);
                    const std::vector<AtomTraceBoundary> atomBoundaries =
                        state.traceComments ? blockAtomTraceBoundaries(model.program, block)
                                            : std::vector<AtomTraceBoundary>{};
                    std::size_t atomBoundaryCursor = 0;
                    for (std::size_t index = 0;
                         index < blockSize;
                         ++index)
                    {
                        if (gate != nullptr && index == gate->headCount)
                        {
                            // The Block's single merged event gate: everything
                            // from here on (writes and tail watch traffic)
                            // runs only when a gate detector fired.
                            blockSource << indentation << "if (" << gate->expression
                                        << ") {\n";
                            writeIndentedLines(blockSource, gate->preamble,
                                               gatedIndentation);
                        }
                        const InstructionId instruction =
                            model.program.blockInstruction(block, index);
                        if (state.traceComments)
                        {
                            while (atomBoundaryCursor < atomBoundaries.size() &&
                                   atomBoundaries[atomBoundaryCursor].position ==
                                       static_cast<uint32_t>(index))
                            {
                                writeIndentedLines(
                                    blockSource,
                                    atomTraceCommentsAt(
                                        state, model.program,
                                        atomBoundaries[atomBoundaryCursor].atom,
                                        instruction),
                                    gate != nullptr && index >= gate->headCount
                                        ? gatedIndentation
                                        : indentation);
                                ++atomBoundaryCursor;
                            }
                        }
                        std::string error;
                        const std::optional<std::string> code =
                            emitBlockPositionCode(state, detectorPlan, arrayPlan, scalarPlan,
                                                  instruction,
                                                  static_cast<uint32_t>(index),
                                                  detectorGroupDeclared, error);
                        if (!code)
                        {
                            endLocalityBlock(state);
                            diagnostics.error(error + ": instruction=" +
                                                  std::to_string(instruction.value),
                                              std::string(kContext));
                            return false;
                        }
                        writeIndentedLines(blockSource, *code,
                                           gate != nullptr && index >= gate->headCount
                                               ? gatedIndentation
                                               : indentation);
                    }
                    if (gate != nullptr && gate->headCount < blockSize)
                    {
                        blockSource << indentation << "}\n";
                    }
                    endLocalityBlock(state);
                    return true;
                };
                // Chunk definitions of this part's oversized Blocks: emitted
                // into a side buffer while the parent scan/commit/entry
                // function streams (so the byte-chunk relay context active at
                // the call site applies), then appended after the parent
                // function's epilogue — member-function definition order in
                // the TU is free.
                std::ostringstream chunkDefs;
                const auto flushChunkDefs = [&] {
                    blockSource << chunkDefs.str();
                    chunkDefs.str(std::string());
                };
                // Oversized-Block form of writeBlockBody: the Block's
                // instruction stream becomes a sequence of chunk-function
                // definitions (same per-position emission, array-named
                // locals/watch/detector flags), and the parent body keeps the
                // shared array declarations plus one call per chunk. The
                // commit gate wraps exactly the chunks past the gate head
                // boundary, preserving the inline gate's semantics.
                const auto writeChunkedBlockBody = [&](std::size_t blockIndex,
                                                       std::string_view indentation,
                                                       const EmitState::CommitGate *gate) {
                    const BlockId block{static_cast<uint32_t>(blockIndex)};
                    const std::size_t blockSize = model.program.blockSize(block);
                    const std::size_t gateBoundary =
                        gate != nullptr ? gate->headCount : 0;
                    const std::vector<std::pair<std::size_t, std::size_t>> chunkRanges =
                        blockChunkRanges(blockSize, *blockChunkInstructions, gateBoundary);
                    const BlockChunkParams chunkParams =
                        blockChunkParamsFor(state, blockIndex);
                    const std::string parameterList =
                        blockChunkParameterList(blockIndex, chunkParams);
                    const std::string argumentList =
                        blockChunkArgumentList(blockIndex, chunkParams);
                    beginLocalityBlock(state, static_cast<uint32_t>(blockIndex));
                    state.activeChunkedBlock = static_cast<uint32_t>(blockIndex);
                    const EmitState::ArrayWatchPlan *arrayPlan =
                        arrayWatchPlanFor(state, blockIndex);
                    const EmitState::ScalarWatchPlan *scalarPlan =
                        scalarWatchPlanFor(state, blockIndex);
                    const EmitState::DetectorGroupPlan *detectorPlan =
                        detectorPlanFor(state, blockIndex);
                    // First-use declaration state is continuous across the
                    // Block's chunks (chunked references never re-declare).
                    std::vector<uint8_t> detectorGroupDeclared(
                        detectorPlan != nullptr ? detectorPlan->groups.size() : 0, 0);
                    // Atom boundaries are also continuous across chunks: an
                    // atom's trace comment lands in the chunk holding its
                    // first instruction.
                    const std::vector<AtomTraceBoundary> atomBoundaries =
                        state.traceComments ? blockAtomTraceBoundaries(model.program, block)
                                            : std::vector<AtomTraceBoundary>{};
                    std::size_t atomBoundaryCursor = 0;
                    for (std::size_t chunk = 0; chunk < chunkRanges.size(); ++chunk)
                    {
                        chunkDefs << "\nvoid " << className << "::"
                                  << blockChunkFunctionName(blockIndex, chunk) << "("
                                  << parameterList << ") {\n";
                        if (state.traceComments)
                        {
                            chunkDefs << "    "
                                      << blockTraceBanner(
                                             model, static_cast<uint32_t>(blockIndex));
                        }
                        // NO0016 Stage B: declare this chunk's internal
                        // scalars (one group per non-empty storage class).
                        if (blockIndex < state.blockChunkScalarCounts.size() &&
                            chunk < state.blockChunkScalarCounts[blockIndex].size())
                        {
                            const auto &chunkCounts =
                                state.blockChunkScalarCounts[blockIndex][chunk];
                            for (uint8_t storageClass = 0; storageClass < 4;
                                 ++storageClass)
                            {
                                const uint32_t count = chunkCounts[storageClass];
                                if (count == 0)
                                {
                                    continue;
                                }
                                chunkDefs << "    " << localClassCppType(storageClass)
                                          << " ";
                                const std::string infix =
                                    localClassInfix(storageClass);
                                for (uint32_t index = 0; index < count; ++index)
                                {
                                    if (index != 0)
                                    {
                                        chunkDefs << ", ";
                                    }
                                    chunkDefs << "local" << infix << "_" << index;
                                }
                                chunkDefs << ";\n";
                            }
                        }
                        const auto [firstPosition, endPosition] = chunkRanges[chunk];
                        for (std::size_t index = firstPosition;
                             index < endPosition;
                             ++index)
                        {
                            const InstructionId instruction =
                                model.program.blockInstruction(block, index);
                            if (state.traceComments)
                            {
                                while (atomBoundaryCursor < atomBoundaries.size() &&
                                       atomBoundaries[atomBoundaryCursor].position ==
                                           static_cast<uint32_t>(index))
                                {
                                    writeIndentedLines(
                                        chunkDefs,
                                        atomTraceCommentsAt(
                                            state, model.program,
                                            atomBoundaries[atomBoundaryCursor].atom,
                                            instruction),
                                        "    ");
                                    ++atomBoundaryCursor;
                                }
                            }
                            std::string error;
                            const std::optional<std::string> code =
                                emitBlockPositionCode(state, detectorPlan, arrayPlan,
                                                      scalarPlan, instruction,
                                                      static_cast<uint32_t>(index),
                                                      detectorGroupDeclared, error);
                            if (!code)
                            {
                                state.activeChunkedBlock = kInvalidLocalityBlock;
                                endLocalityBlock(state);
                                diagnostics.error(error + ": instruction=" +
                                                      std::to_string(instruction.value),
                                                  std::string(kContext));
                                return false;
                            }
                            writeIndentedLines(chunkDefs, *code, "    ");
                        }
                        chunkDefs << "}\n";
                    }
                    if (state.traceComments)
                    {
                        blockSource << indentation
                                    << blockTraceBanner(
                                           model, static_cast<uint32_t>(blockIndex));
                    }
                    writeIndentedLines(
                        blockSource,
                        commitInputDirtyBlockMarks(
                            state, static_cast<uint32_t>(blockIndex)),
                        indentation);
                    writeIndentedLines(blockSource,
                                       chunkedLocalValueDeclarations(state),
                                       indentation);
                    writeIndentedLines(blockSource,
                                       chunkedArrayWatchDeclarations(state, arrayPlan),
                                       indentation);
                    writeIndentedLines(blockSource,
                                       chunkedScalarWatchDeclarations(state, scalarPlan),
                                       indentation);
                    writeIndentedLines(blockSource,
                                       chunkedDetectorGroupDeclarations(state, detectorPlan),
                                       indentation);
                    const std::size_t ungatedChunks =
                        gateBoundary == 0
                            ? chunkRanges.size()
                            : (gateBoundary + *blockChunkInstructions - 1U) /
                                  *blockChunkInstructions;
                    const std::string gatedIndentation = std::string(indentation) + "    ";
                    for (std::size_t chunk = 0; chunk < chunkRanges.size(); ++chunk)
                    {
                        if (gate != nullptr && chunk == ungatedChunks)
                        {
                            // The Block's single merged event gate: the chunks
                            // past the gate head run only when a gate detector
                            // fired (the inline form wraps the instructions).
                            blockSource << indentation << "if (" << gate->expression
                                        << ") {\n";
                            writeIndentedLines(blockSource, gate->preamble,
                                               gatedIndentation);
                        }
                        blockSource << (chunk < ungatedChunks ? indentation
                                                              : gatedIndentation)
                                    << blockChunkFunctionName(blockIndex, chunk) << "("
                                    << argumentList << ");\n";
                    }
                    if (gate != nullptr && ungatedChunks < chunkRanges.size())
                    {
                        blockSource << indentation << "}\n";
                    }
                    state.activeChunkedBlock = kInvalidLocalityBlock;
                    endLocalityBlock(state);
                    return true;
                };
                const auto writeBlockBodyOrChunks =
                    [&](std::size_t blockIndex,
                        std::string_view indentation,
                        const EmitState::CommitGate *gate = nullptr) {
                        const BlockId block{static_cast<uint32_t>(blockIndex)};
                        if (model.program.blockSize(block) > *blockChunkInstructions)
                        {
                            return writeChunkedBlockBody(blockIndex, indentation, gate);
                        }
                        return writeBlockBody(blockIndex, indentation, gate);
                    };
                // A scan Block with no instructions and no local declarations
                // emits no bit test; the byte-chunk clear still consumes its
                // activity bit.
                const auto scanBlockIsEmpty = [&](std::size_t blockIndex) {
                    beginLocalityBlock(state, static_cast<uint32_t>(blockIndex));
                    const bool empty =
                        localValueDeclarations(state).empty() &&
                        model.program.blockSize(
                            BlockId{static_cast<uint32_t>(blockIndex)}) == 0;
                    endLocalityBlock(state);
                    return empty;
                };

                blockSource << blockSourceIncludes(prefix);
                if (part.firstBlock == 0)
                {
                    blockSource << entryBlockSourcePrologue(className);
                    if (state.runtimeProfile)
                    {
                        blockSource << entryBlockProfileLine();
                    }
                    if (!writeBlockBodyOrChunks(0, "    "))
                    {
                        blocksGenerated = false;
                        break;
                    }
                    if (state.runtimeProfile)
                    {
                        blockSource << entryBlockProfileEpilogue();
                    }
                    blockSource << kBlockSourceFunctionEpilogue;
                    flushChunkDefs();
                }
                const auto [scanLo, scanHi] =
                    computeBlockRange(part.firstBlock,
                                      part.endBlock,
                                      blockCount,
                                      model.commitBlockBegin);
                if (scanLo < scanHi)
                {
                    blockSource << scanSourcePrologue(className,
                                                      part.sourceIndex,
                                                      part.partIndex);
                    for (const ScanByteChunk &chunk :
                         scanByteChunks(scanLo, scanHi))
                    {
                        blockSource << scanByteChunkPrologue(chunk.byteIndex,
                                                             chunk.ownedMask,
                                                             state.fullEvaluation);
                        state.scanRelayByte =
                            static_cast<int32_t>(chunk.byteIndex);
                        state.scanRelayMask = chunk.ownedMask;
                        for (std::size_t blockIndex = chunk.firstBlock;
                             blockIndex < chunk.endBlock;
                             ++blockIndex)
                        {
                            if (scanBlockIsEmpty(blockIndex))
                            {
                                continue;
                            }
                            blockSource << scanBlockTestPrologue(
                                blockIndex, state.runtimeProfile);
                            // Guard-event gating: the gate wraps the whole
                            // Block body (all chunk calls) after the byte
                            // snapshot/clear, so the activity bookkeeping is
                            // unchanged and a closed gate skips the body.
                            const std::string &guardGate =
                                state.blockGuardGate[blockIndex].expression;
                            if (!guardGate.empty())
                            {
                                blockSource << "                if (" << guardGate
                                            << ") {\n";
                            }
                            if (!writeBlockBodyOrChunks(blockIndex,
                                                        guardGate.empty()
                                                            ? "                "
                                                            : "                    "))
                            {
                                blocksGenerated = false;
                                break;
                            }
                            if (!guardGate.empty())
                            {
                                blockSource << "                }\n";
                            }
                            blockSource << scanBlockTestEpilogue(
                                blockIndex, state.runtimeProfile);
                        }
                        state.scanRelayByte = -1;
                        state.scanRelayMask = 0;
                        if (!blocksGenerated)
                        {
                            break;
                        }
                        blockSource << kScanByteChunkEpilogue;
                    }
                    if (!blocksGenerated)
                    {
                        break;
                    }
                    blockSource << kBlockSourceFunctionEpilogue;
                    flushChunkDefs();
                }
                const auto [commitLo, commitHi] =
                    commitBlockRange(part.firstBlock,
                                     part.endBlock,
                                     model.commitBlockBegin,
                                     model.commitBlockEnd);
                if (commitLo < commitHi)
                {
                    blockSource << commitSourcePrologue(className,
                                                        part.sourceIndex,
                                                        part.partIndex);
                    // Commit phase: the same byte-chunk activation scan as the
                    // compute phase, over the commit Block range. A commit
                    // Block runs only when a watched event source activated
                    // it; same-byte forward activations relay through
                    // byteFlags exactly like the compute scan.
                    for (const ScanByteChunk &chunk :
                         scanByteChunks(commitLo, commitHi))
                    {
                        blockSource << scanByteChunkPrologue(chunk.byteIndex,
                                                             chunk.ownedMask,
                                                             state.fullEvaluation);
                        state.scanRelayByte =
                            static_cast<int32_t>(chunk.byteIndex);
                        state.scanRelayMask = chunk.ownedMask;
                        for (std::size_t blockIndex = chunk.firstBlock;
                             blockIndex < chunk.endBlock;
                             ++blockIndex)
                        {
                            blockSource << commitBlockTestPrologue(
                                blockIndex, state.runtimeProfile);
                            const EmitState::CommitGate &commitGate =
                                state.blockCommitGate[blockIndex];
                            const EmitState::CommitGate *gate =
                                commitGate.headCount != 0 ? &commitGate : nullptr;
                            if (!writeBlockBodyOrChunks(blockIndex, "                ", gate))
                            {
                                blocksGenerated = false;
                                break;
                            }
                            blockSource << scanBlockTestEpilogue(
                                blockIndex, state.runtimeProfile);
                        }
                        state.scanRelayByte = -1;
                        state.scanRelayMask = 0;
                        if (!blocksGenerated)
                        {
                            break;
                        }
                        blockSource << kScanByteChunkEpilogue;
                    }
                    if (!blocksGenerated)
                    {
                        break;
                    }
                    blockSource << kBlockSourceFunctionEpilogue;
                    flushChunkDefs();
                }
                if (!finishWrittenFile(blockSource,
                                       blockPath,
                                       options.maxOutputFileBytes,
                                       diagnostics))
                {
                    blocksGenerated = false;
                    break;
                }
            }
            if (!blocksGenerated)
            {
                break;
            }
        }
        if (!blocksGenerated)
        {
            discardStaging();
            return result;
        }

        std::vector<StagedArtifact> stagedArtifacts;
        stagedArtifacts.reserve(blockPaths.size() + 4U);
        stagedArtifacts.push_back(StagedArtifact{.staged = stagedHeaderPath, .destination = headerPath});
        stagedArtifacts.push_back(StagedArtifact{.staged = stagedSupportPath, .destination = supportPath});
        stagedArtifacts.push_back(StagedArtifact{.staged = stagedRuntimePath, .destination = runtimePath});
        for (const std::filesystem::path &blockPath : blockPaths)
        {
            stagedArtifacts.push_back(
                StagedArtifact{.staged = blockPath,
                               .destination = options.outputDirectory / blockPath.filename()});
        }
        stagedArtifacts.push_back(
            StagedArtifact{.staged = stagedMakefilePath, .destination = makefilePath});
        if (!publishStagedArtifacts(stagingDirectory, stagedArtifacts, diagnostics))
        {
            discardStaging();
            return result;
        }
        discardStaging();

        result.success = true;
        result.muxAtomFused = state.muxAtomFusedCount;
        result.windowedChains = state.windowedChainCount;
        result.windowedSteps = state.windowedStepCount;
        result.windowedConcatsF2 = state.windowedConcatCount;
        result.windowedSkippedSlices = state.windowedSkippedSlices;
        result.windowedRemappedSlices = state.windowedRemappedSlices;
        result.windowedMaterialized = state.windowedMaterialized;
        result.windowedBailedChains = state.windowedBailedChains;
        result.dynBlendChains = state.dynBlendChainCount;
        result.dynBlendCones = state.dynBlendConeCount;
        result.dynBlendSkipped = state.dynBlendSkipped;
        result.dynBlendRemapped = state.dynBlendRemapped;
        result.dynBlendMaterialized = state.dynBlendMaterialized;
        result.dynBlendBailed = state.dynBlendBailed;
        result.wideStateExploded = state.explodedStateCount;
        result.wideStateExplodedElements = state.explodedElementTotal;
        result.wideStateExplodeBailed =
            std::accumulate(state.explodeBails.begin(), state.explodeBails.end(),
                            uint64_t{0});
        result.artifacts.reserve(stagedArtifacts.size());
        for (const StagedArtifact &artifact : stagedArtifacts)
        {
            result.artifacts.push_back(artifact.destination.string());
        }
        return result;
    }

} // namespace wolvrix::lib::grhsim::am
