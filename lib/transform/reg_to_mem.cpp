#include "transform/reg_to_mem.hpp"

#include "core/grh.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;

        constexpr std::string_view kPassId = "reg-to-mem";
        using ProfileClock = std::chrono::steady_clock;

        int64_t elapsedMs(ProfileClock::time_point start, ProfileClock::time_point end = ProfileClock::now())
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        }

        void profileLog(const std::string &message)
        {
            std::cerr << "reg-to-mem profile: " << message << '\n';
            std::cerr.flush();
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

        std::optional<std::string> getStringAttr(const Operation &op, std::string_view key)
        {
            return getAttr<std::string>(op, key);
        }

        struct AnchorCandidate
        {
            OperationId sliceOp;
            OperationId concatOp;
            std::vector<OperationId> readOps;
            std::vector<std::string> regSymbols;
            std::vector<int64_t> operandRows;
            int32_t elementWidth = 0;
            std::size_t elementCount = 0;
            ValueId indexValue;
            std::string sliceKind;
        };

        struct GroupCandidate
        {
            std::vector<std::string> regSymbols;
            std::vector<AnchorCandidate> anchors;
            int32_t elementWidth = 0;
            std::size_t elementCount = 0;
            std::string intentGroupName;
            std::string storageGroupName;
            std::vector<std::string> storageRegSymbols;
            std::size_t storageElementCount = 0;
            std::size_t storageRowOffset = 0;
            bool sharesStorage = false;
            bool overlapsOtherCandidate = false;
            bool trueOnlyStorage = false;
        };

        struct GroupAnchorsResult
        {
            std::vector<GroupCandidate> groups;
            std::size_t candidateGroups = 0;
            std::size_t candidateAnchors = 0;
            std::size_t candidateMembers = 0;
            std::size_t conflictGroups = 0;
            std::size_t conflictAnchors = 0;
            std::size_t conflictMembers = 0;
        };

        struct WritePortInfo
        {
            OperationId op;
            ValueId updateCond;
            ValueId nextValue;
            ValueId mask;
            std::vector<ValueId> events;
            std::vector<std::string> eventEdges;
        };

        struct EqualityTerm
        {
            OperationId op;
            ValueId addr;
            ValueId constant;
            uint64_t row = 0;
        };

        struct GuardMatch
        {
            ValueId addr;
            std::vector<ValueId> commonTerms;
            uint64_t row = 0;
        };

        struct PriorityGuardMatch
        {
            ValueId addr;
            std::vector<ValueId> commonTerms;
            std::vector<ValueId> conflictAddrs;
            uint64_t row = 0;
        };

        struct RegularWriteFamily
        {
            std::vector<WritePortInfo> writes;
            ValueId addr;
            ValueId data;
            ValueId mask;
            std::vector<ValueId> events;
            std::vector<std::string> eventEdges;
            std::vector<ValueId> commonTerms;
            std::vector<ValueId> conflictAddrs;
        };

        struct CompoundWriteMatch
        {
            GuardMatch regularGuard;
            ValueId regularData;
            ValueId resetGuard;
            ValueId resetData;
        };

        struct ResetWriteFamily
        {
            std::vector<WritePortInfo> writes;
            ValueId guard;
            std::vector<ValueId> guardTerms;
            ValueId data;
            std::vector<ValueId> rowData;
            ValueId mask;
            std::vector<ValueId> events;
            std::vector<std::string> eventEdges;
            bool packed = false;
        };

        struct RegularWriteMatch
        {
            RegularWriteFamily family;
            std::optional<ResetWriteFamily> splitReset;
        };

        struct ConsolidatedWriteMatch
        {
            std::vector<RegularWriteFamily> regulars;
            std::optional<ResetWriteFamily> reset;
        };

        struct MuxWriteBranch
        {
            ValueId guard;
            ValueId data;
        };

        struct MuxWriteChain
        {
            std::vector<MuxWriteBranch> branches;
            ValueId fallback;
        };

        struct TrueMergeCandidate
        {
            std::vector<RegularWriteFamily> regulars;
            std::optional<ResetWriteFamily> reset;
            std::vector<OperationId> regOps;
            std::vector<std::vector<OperationId>> readOpsByRow;
            std::vector<std::optional<std::string>> initValues;
        };

        struct RegToMemStats
        {
            std::size_t graphs = 0;
            std::size_t intentCandidateGroups = 0;
            std::size_t intentCandidateAnchors = 0;
            std::size_t intentCandidateMembers = 0;
            std::size_t intentConflictGroups = 0;
            std::size_t intentConflictAnchors = 0;
            std::size_t intentConflictMembers = 0;
            std::size_t intentGroups = 0;
            std::size_t intentAnchors = 0;
            std::size_t intentMembers = 0;
            std::size_t trueGroups = 0;
            std::size_t trueAnchors = 0;
            std::size_t trueMembers = 0;
            std::size_t skippedTrueCandidates = 0;
        };

        struct RegToMemProfile
        {
            int64_t buildUsesMs = 0;
            int64_t discoverAnchorsMs = 0;
            int64_t groupAnchorsMs = 0;
            int64_t buildReadIndexMs = 0;
            int64_t collectWritesMs = 0;
            int64_t trueClosureMs = 0;
            int64_t collectInitsMs = 0;
            int64_t regularWriteMatchMs = 0;
            int64_t resetWriteMatchMs = 0;
            int64_t finalizeTrueMatchMs = 0;
            int64_t rewriteTrueMs = 0;
            int64_t rewriteMemoryMs = 0;
            int64_t rewriteReadReplacementMs = 0;
            int64_t rewriteEraseReadClosureMs = 0;
            int64_t rewriteFillMs = 0;
            int64_t rewriteDomainGuardMs = 0;
            int64_t rewriteWritePortMs = 0;
            int64_t rewriteEraseWritesMs = 0;
            int64_t rewriteEraseRegsMs = 0;
            int64_t annotateMs = 0;
        };

        struct GroupProfileContext
        {
            std::size_t graphIndex = 0;
            std::size_t groupIndex = 0;
            std::size_t groupCount = 0;
            bool verbose = false;
        };

        bool shouldProfileGroupRow(const GroupProfileContext *groupProfile, std::size_t row, std::size_t rowCount)
        {
            if (groupProfile == nullptr || !groupProfile->verbose)
            {
                return false;
            }
            return row < 8 || row + 1 == rowCount || row % 64 == 0;
        }

        struct ValueUseInfo
        {
            std::size_t count = 0;
            OperationId onlyUser;
        };

        using ValueUseIndex = std::unordered_map<ValueId, ValueUseInfo, ValueIdHash>;
        using RegisterReadIndex = std::unordered_map<std::string, std::vector<OperationId>>;

        ValueUseIndex buildValueUseIndex(const Graph &graph)
        {
            ValueUseIndex uses;
            const auto ops = graph.operations();
            uses.reserve(ops.size() * 2);
            for (OperationId opId : ops)
            {
                const Operation op = graph.getOperation(opId);
                for (ValueId operand : op.operands())
                {
                    auto &info = uses[operand];
                    ++info.count;
                    if (info.count == 1)
                    {
                        info.onlyUser = opId;
                    }
                    else
                    {
                        info.onlyUser = OperationId::invalid();
                    }
                }
            }
            return uses;
        }

        RegisterReadIndex buildRegisterReadIndex(const Graph &graph)
        {
            RegisterReadIndex readsByReg;
            for (OperationId opId : graph.operations())
            {
                const Operation op = graph.getOperation(opId);
                if (op.kind() != OperationKind::kRegisterReadPort)
                {
                    continue;
                }
                const auto regSymbol = getStringAttr(op, "regSymbol");
                if (!regSymbol)
                {
                    continue;
                }
                readsByReg[*regSymbol].push_back(opId);
            }
            return readsByReg;
        }


        std::string rowKey(const std::vector<std::string> &regSymbols)
        {
            std::ostringstream out;
            for (const auto &symbol : regSymbols)
            {
                out << symbol.size() << ':' << symbol << ';';
            }
            return out.str();
        }

        std::string layoutKey(const AnchorCandidate &anchor)
        {
            std::ostringstream out;
            out << anchor.elementWidth << ':' << anchor.elementCount << ':' << rowKey(anchor.regSymbols);
            return out.str();
        }

        std::optional<std::size_t> contiguousSubsequenceOffset(const std::vector<std::string> &needle,
                                                               const std::vector<std::string> &haystack)
        {
            if (needle.empty() || needle.size() > haystack.size())
            {
                return std::nullopt;
            }
            const std::size_t lastStart = haystack.size() - needle.size();
            for (std::size_t start = 0; start <= lastStart; ++start)
            {
                bool matched = true;
                for (std::size_t i = 0; i < needle.size(); ++i)
                {
                    if (needle[i] != haystack[start + i])
                    {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                {
                    return start;
                }
            }
            return std::nullopt;
        }

        bool groupsCanShareStorage(const GroupCandidate &view,
                                   const GroupCandidate &storage,
                                   std::size_t *rowOffset = nullptr)
        {
            if (view.elementWidth != storage.elementWidth ||
                view.elementCount != view.regSymbols.size() ||
                storage.elementCount != storage.regSymbols.size())
            {
                return false;
            }
            const auto offset = contiguousSubsequenceOffset(view.regSymbols, storage.regSymbols);
            if (!offset)
            {
                return false;
            }
            if (rowOffset != nullptr)
            {
                *rowOffset = *offset;
            }
            return true;
        }

        bool groupSharesReadOpsWithFamily(
            const GroupCandidate &group,
            const std::unordered_map<OperationId, std::size_t, OperationIdHash> &readOwnerByOp)
        {
            for (const auto &anchor : group.anchors)
            {
                for (const OperationId readOp : anchor.readOps)
                {
                    if (readOp.valid() && readOwnerByOp.find(readOp) != readOwnerByOp.end())
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        void addGroupReadsToFamily(
            const GroupCandidate &group,
            std::size_t groupIndex,
            std::unordered_map<OperationId, std::size_t, OperationIdHash> &readOwnerByOp)
        {
            for (const auto &anchor : group.anchors)
            {
                for (const OperationId readOp : anchor.readOps)
                {
                    if (readOp.valid())
                    {
                        readOwnerByOp.emplace(readOp, groupIndex);
                    }
                }
            }
        }

        bool isRegisterDecl(const Graph &graph, const std::string &symbol)
        {
            const OperationId opId = graph.findOperation(symbol);
            if (!opId.valid())
            {
                return false;
            }
            return graph.getOperation(opId).kind() == OperationKind::kRegister;
        }

        bool hasOnlyUser(const ValueUseIndex &uses, ValueId value, OperationId user)
        {
            const auto it = uses.find(value);
            return it != uses.end() && it->second.count == 1 && it->second.onlyUser == user;
        }

        std::optional<slang::SVInt> parseConstLiteral(std::string_view literal)
        {
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                compact.push_back(ch);
            }
            if (compact.empty())
            {
                return std::nullopt;
            }

            bool negative = false;
            if (compact.front() == '-' || compact.front() == '+')
            {
                negative = compact.front() == '-';
                compact.erase(compact.begin());
            }
            if (compact.empty())
            {
                return std::nullopt;
            }

            try
            {
                slang::SVInt parsed = slang::SVInt::fromString(compact);
                if (negative)
                {
                    parsed = -parsed;
                }
                return parsed;
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<uint64_t> getConstantUInt64(const Graph &graph, ValueId value)
        {
            if (!value.valid() || graph.valueType(value) != ValueType::Logic)
            {
                return std::nullopt;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kConstant)
            {
                return std::nullopt;
            }
            const auto literal = getStringAttr(defOp, "constValue");
            if (!literal)
            {
                return std::nullopt;
            }
            auto parsed = parseConstLiteral(*literal);
            if (!parsed || parsed->hasUnknown())
            {
                return std::nullopt;
            }
            parsed = parsed->resize(static_cast<slang::bitwidth_t>(std::max<int32_t>(graph.valueWidth(value), 1)));
            const std::size_t wordCount =
                static_cast<std::size_t>((std::max<int32_t>(graph.valueWidth(value), 1) + 63) / 64);
            const std::uint64_t *raw = parsed->getRawPtr();
            for (std::size_t i = 1; i < wordCount; ++i)
            {
                if (raw[i] != UINT64_C(0))
                {
                    return std::nullopt;
                }
            }
            return raw[0];
        }

        std::string makeIntLiteral(int32_t width, uint64_t value)
        {
            const int32_t normalizedWidth = width > 0 ? width : 1;
            return std::to_string(normalizedWidth) + "'d" + std::to_string(value);
        }

        ValueId createConstantValue(Graph &graph,
                                    int32_t width,
                                    bool isSigned,
                                    std::string literal,
                                    std::string_view note)
        {
            const ValueId value = graph.createValue(graph.makeInternalValSym(),
                                                    width > 0 ? width : 1,
                                                    isSigned,
                                                    ValueType::Logic);
            const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                         graph.makeInternalOpSym());
            graph.addResult(op, value);
            graph.setAttr(op, "constValue", std::move(literal));
            const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(value, srcLoc);
            return value;
        }

        ValueId createBinaryOp(Graph &graph,
                               OperationKind kind,
                               ValueId lhs,
                               ValueId rhs,
                               int32_t outWidth,
                               bool outSigned,
                               std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  outWidth > 0 ? outWidth : 1,
                                                  outSigned,
                                                  ValueType::Logic);
            const OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createUnaryOp(Graph &graph,
                              OperationKind kind,
                              ValueId operand,
                              int32_t outWidth,
                              bool outSigned,
                              std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  outWidth > 0 ? outWidth : 1,
                                                  outSigned,
                                                  ValueType::Logic);
            const OperationId op = graph.createOperation(kind, graph.makeInternalOpSym());
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createConcat(Graph &graph,
                             std::span<const ValueId> operands,
                             int32_t outWidth,
                             bool outSigned,
                             std::string_view note)
        {
            const ValueId out = graph.createValue(graph.makeInternalValSym(),
                                                  outWidth > 0 ? outWidth : 1,
                                                  outSigned,
                                                  ValueType::Logic);
            const OperationId op = graph.createOperation(OperationKind::kConcat,
                                                         graph.makeInternalOpSym());
            for (ValueId operand : operands)
            {
                graph.addOperand(op, operand);
            }
            graph.addResult(op, out);
            const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), note);
            graph.setOpSrcLoc(op, srcLoc);
            graph.setValueSrcLoc(out, srcLoc);
            return out;
        }

        ValueId createAndChain(Graph &graph, std::span<const ValueId> terms, std::string_view note)
        {
            if (terms.empty())
            {
                return createConstantValue(graph, 1, false, "1'b1", note);
            }
            ValueId current = terms.front();
            for (std::size_t i = 1; i < terms.size(); ++i)
            {
                current = createBinaryOp(graph, OperationKind::kLogicAnd, current, terms[i], 1, false, note);
            }
            return current;
        }

        ValueId createOrChain(Graph &graph, std::span<const ValueId> terms, std::string_view note)
        {
            if (terms.empty())
            {
                return createConstantValue(graph, 1, false, "1'b0", note);
            }
            ValueId current = terms.front();
            for (std::size_t i = 1; i < terms.size(); ++i)
            {
                current = createBinaryOp(graph, OperationKind::kLogicOr, current, terms[i], 1, false, note);
            }
            return current;
        }

        std::optional<ValueId> unwrapAssign(const Graph &graph, ValueId value)
        {
            ValueId current = value;
            for (int depth = 0; depth < 8; ++depth)
            {
                const OperationId defOpId = graph.valueDef(current);
                if (!defOpId.valid())
                {
                    return current;
                }
                const Operation defOp = graph.getOperation(defOpId);
                if (defOp.kind() != OperationKind::kAssign || defOp.operands().size() != 1)
                {
                    return current;
                }
                current = defOp.operands().front();
            }
            return std::nullopt;
        }

        void flattenLogicAndTerms(const Graph &graph,
                                  ValueId value,
                                  std::vector<ValueId> &terms,
                                  std::unordered_set<ValueId, ValueIdHash> &seen)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped)
            {
                terms.push_back(value);
                return;
            }
            value = *unwrapped;
            if (!seen.insert(value).second)
            {
                return;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                terms.push_back(value);
                return;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if ((defOp.kind() == OperationKind::kLogicAnd ||
                 (defOp.kind() == OperationKind::kAnd && graph.valueWidth(value) == 1)) &&
                defOp.operands().size() == 2)
            {
                flattenLogicAndTerms(graph, defOp.operands()[0], terms, seen);
                flattenLogicAndTerms(graph, defOp.operands()[1], terms, seen);
                return;
            }
            terms.push_back(value);
        }

        std::vector<ValueId> flattenLogicAndTerms(const Graph &graph, ValueId value)
        {
            std::vector<ValueId> terms;
            std::unordered_set<ValueId, ValueIdHash> seen;
            flattenLogicAndTerms(graph, value, terms, seen);
            return terms;
        }

        void flattenLogicOrTerms(const Graph &graph,
                                 ValueId value,
                                 std::vector<ValueId> &terms,
                                 std::unordered_set<ValueId, ValueIdHash> &seen)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped)
            {
                terms.push_back(value);
                return;
            }
            value = *unwrapped;
            if (!seen.insert(value).second)
            {
                return;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                terms.push_back(value);
                return;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if ((defOp.kind() == OperationKind::kLogicOr ||
                 (defOp.kind() == OperationKind::kOr && graph.valueWidth(value) == 1)) &&
                defOp.operands().size() == 2)
            {
                flattenLogicOrTerms(graph, defOp.operands()[0], terms, seen);
                flattenLogicOrTerms(graph, defOp.operands()[1], terms, seen);
                return;
            }
            terms.push_back(value);
        }

        std::vector<ValueId> flattenLogicOrTerms(const Graph &graph, ValueId value)
        {
            std::vector<ValueId> terms;
            std::unordered_set<ValueId, ValueIdHash> seen;
            flattenLogicOrTerms(graph, value, terms, seen);
            return terms;
        }

        std::string valueSetKey(std::vector<ValueId> values)
        {
            std::sort(values.begin(), values.end(), [](ValueId lhs, ValueId rhs) {
                if (lhs.graph.index != rhs.graph.index)
                {
                    return lhs.graph.index < rhs.graph.index;
                }
                if (lhs.index != rhs.index)
                {
                    return lhs.index < rhs.index;
                }
                return lhs.generation < rhs.generation;
            });
            values.erase(std::unique(values.begin(), values.end()), values.end());
            std::ostringstream out;
            for (ValueId value : values)
            {
                out << value.graph.index << '.' << value.index << '.' << value.generation << ';';
            }
            return out.str();
        }

        bool valueVectorsEqual(std::span<const ValueId> lhs, std::span<const ValueId> rhs)
        {
            return lhs.size() == rhs.size() &&
                   std::equal(lhs.begin(), lhs.end(), rhs.begin());
        }

        bool sameValueAfterAssign(const Graph &graph, ValueId lhs, ValueId rhs)
        {
            const auto lhsUnwrapped = unwrapAssign(graph, lhs);
            const auto rhsUnwrapped = unwrapAssign(graph, rhs);
            return lhsUnwrapped && rhsUnwrapped && *lhsUnwrapped == *rhsUnwrapped;
        }

        std::optional<std::pair<ValueId, ValueId>> splitLogicOrTerms(const Graph &graph, ValueId value)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped)
            {
                return std::nullopt;
            }
            const OperationId opId = graph.valueDef(*unwrapped);
            if (!opId.valid())
            {
                return std::nullopt;
            }
            const Operation op = graph.getOperation(opId);
            if ((op.kind() != OperationKind::kLogicOr &&
                 !(op.kind() == OperationKind::kOr && graph.valueWidth(*unwrapped) == 1)) ||
                op.operands().size() != 2)
            {
                return std::nullopt;
            }
            return std::pair<ValueId, ValueId>{op.operands()[0], op.operands()[1]};
        }

        bool isNegationOf(const Graph &graph, ValueId value, ValueId expected)
        {
            const auto unwrapped = unwrapAssign(graph, value);
            if (!unwrapped)
            {
                return false;
            }
            const OperationId opId = graph.valueDef(*unwrapped);
            if (!opId.valid())
            {
                return false;
            }
            const Operation op = graph.getOperation(opId);
            if ((op.kind() != OperationKind::kLogicNot &&
                 !(op.kind() == OperationKind::kNot && graph.valueWidth(*unwrapped) == 1)) ||
                op.operands().size() != 1)
            {
                return false;
            }
            return sameValueAfterAssign(graph, op.operands().front(), expected);
        }

        bool commonTermsContainNegationOf(const Graph &graph,
                                          std::span<const ValueId> commonTerms,
                                          ValueId expected)
        {
            return std::any_of(commonTerms.begin(), commonTerms.end(), [&](ValueId term) {
                return isNegationOf(graph, term, expected);
            });
        }

        bool commonTermsExcludeResetTermImpl(const Graph &graph,
                                             std::span<const ValueId> commonTerms,
                                             ValueId resetTerm,
                                             std::unordered_set<ValueId, ValueIdHash> &seen)
        {
            // Prove exclusion through !term, !(A || B), or !A => !(A && B).
            const auto unwrappedReset = unwrapAssign(graph, resetTerm);
            if (!unwrappedReset || !seen.insert(*unwrappedReset).second)
            {
                return false;
            }
            resetTerm = *unwrappedReset;
            if (commonTermsContainNegationOf(graph, commonTerms, resetTerm))
            {
                return true;
            }
            for (ValueId commonTerm : commonTerms)
            {
                const auto unwrapped = unwrapAssign(graph, commonTerm);
                if (!unwrapped)
                {
                    continue;
                }
                const OperationId opId = graph.valueDef(*unwrapped);
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                if ((op.kind() != OperationKind::kLogicNot &&
                     !(op.kind() == OperationKind::kNot && graph.valueWidth(*unwrapped) == 1)) ||
                    op.operands().size() != 1)
                {
                    continue;
                }
                const auto excludedTerms = flattenLogicOrTerms(graph, op.operands().front());
                if (std::any_of(excludedTerms.begin(), excludedTerms.end(), [&](ValueId excluded) {
                        return sameValueAfterAssign(graph, excluded, resetTerm);
                    }))
                {
                    return true;
                }
            }

            const OperationId resetOpId = graph.valueDef(resetTerm);
            if (!resetOpId.valid())
            {
                return false;
            }
            const Operation resetOp = graph.getOperation(resetOpId);
            if ((resetOp.kind() != OperationKind::kLogicAnd &&
                 !(resetOp.kind() == OperationKind::kAnd && graph.valueWidth(resetTerm) == 1)) ||
                resetOp.operands().size() != 2)
            {
                return false;
            }
            return commonTermsExcludeResetTermImpl(graph, commonTerms, resetOp.operands()[0], seen) ||
                   commonTermsExcludeResetTermImpl(graph, commonTerms, resetOp.operands()[1], seen);
        }

        bool commonTermsExcludeResetTerm(const Graph &graph,
                                         std::span<const ValueId> commonTerms,
                                         ValueId resetTerm)
        {
            std::unordered_set<ValueId, ValueIdHash> seen;
            return commonTermsExcludeResetTermImpl(graph, commonTerms, resetTerm, seen);
        }

        bool commonTermsExcludeResetSet(const Graph &graph,
                                        std::span<const ValueId> commonTerms,
                                        std::span<const ValueId> resetTerms)
        {
            if (resetTerms.empty())
            {
                return true;
            }
            if (std::all_of(resetTerms.begin(), resetTerms.end(), [&](ValueId resetTerm) {
                    return commonTermsExcludeResetTerm(graph, commonTerms, resetTerm);
                }))
            {
                return true;
            }

            const std::string expectedKey = valueSetKey(std::vector<ValueId>(resetTerms.begin(), resetTerms.end()));
            for (ValueId commonTerm : commonTerms)
            {
                const auto unwrapped = unwrapAssign(graph, commonTerm);
                if (!unwrapped)
                {
                    continue;
                }
                const OperationId opId = graph.valueDef(*unwrapped);
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                if ((op.kind() != OperationKind::kLogicNot &&
                     !(op.kind() == OperationKind::kNot && graph.valueWidth(*unwrapped) == 1)) ||
                    op.operands().size() != 1)
                {
                    continue;
                }
                if (valueSetKey(flattenLogicOrTerms(graph, op.operands().front())) == expectedKey)
                {
                    return true;
                }
            }
            return false;
        }

        std::optional<EqualityTerm> matchEqualityTerm(const Graph &graph, ValueId term)
        {
            const auto unwrapped = unwrapAssign(graph, term);
            if (!unwrapped)
            {
                return std::nullopt;
            }
            term = *unwrapped;
            const OperationId opId = graph.valueDef(term);
            if (!opId.valid())
            {
                return std::nullopt;
            }
            const Operation op = graph.getOperation(opId);
            if (op.kind() == OperationKind::kReduceAnd && op.operands().size() == 1)
            {
                const ValueId addr = op.operands().front();
                const int32_t addrWidth = graph.valueWidth(addr);
                if (addrWidth <= 0 || addrWidth > 63)
                {
                    return std::nullopt;
                }
                const uint64_t row = (UINT64_C(1) << static_cast<uint32_t>(addrWidth)) - UINT64_C(1);
                return EqualityTerm{.op = opId,
                                    .addr = addr,
                                    .constant = ValueId::invalid(),
                                    .row = row};
            }
            if (op.kind() != OperationKind::kEq || op.operands().size() != 2)
            {
                return std::nullopt;
            }
            const auto lhsConst = getConstantUInt64(graph, op.operands()[0]);
            const auto rhsConst = getConstantUInt64(graph, op.operands()[1]);
            if (lhsConst && !rhsConst)
            {
                return EqualityTerm{.op = opId,
                                    .addr = op.operands()[1],
                                    .constant = op.operands()[0],
                                    .row = *lhsConst};
            }
            if (rhsConst && !lhsConst)
            {
                return EqualityTerm{.op = opId,
                                    .addr = op.operands()[0],
                                    .constant = op.operands()[1],
                                    .row = *rhsConst};
            }
            return std::nullopt;
        }

        std::optional<GuardMatch> matchGuard(const Graph &graph, ValueId guard)
        {
            const auto terms = flattenLogicAndTerms(graph, guard);
            std::optional<EqualityTerm> eq;
            std::vector<ValueId> commonTerms;
            commonTerms.reserve(terms.size());
            for (ValueId term : terms)
            {
                auto matchedEq = matchEqualityTerm(graph, term);
                if (matchedEq)
                {
                    if (eq)
                    {
                        return std::nullopt;
                    }
                    eq = std::move(*matchedEq);
                    continue;
                }
                commonTerms.push_back(term);
            }
            if (!eq)
            {
                return std::nullopt;
            }
            return GuardMatch{.addr = eq->addr, .commonTerms = std::move(commonTerms), .row = eq->row};
        }

        std::optional<EqualityTerm> matchNegatedEqualityTerm(const Graph &graph, ValueId term)
        {
            const auto unwrapped = unwrapAssign(graph, term);
            if (!unwrapped)
            {
                return std::nullopt;
            }
            const OperationId opId = graph.valueDef(*unwrapped);
            if (!opId.valid())
            {
                return std::nullopt;
            }
            const Operation op = graph.getOperation(opId);
            if ((op.kind() != OperationKind::kLogicNot &&
                 !(op.kind() == OperationKind::kNot && graph.valueWidth(*unwrapped) == 1)) ||
                op.operands().size() != 1)
            {
                return std::nullopt;
            }
            return matchEqualityTerm(graph, op.operands().front());
        }

        std::optional<PriorityGuardMatch> matchPriorityGuard(const Graph &graph, ValueId guard)
        {
            const std::vector<ValueId> terms = flattenLogicAndTerms(graph, guard);
            std::optional<EqualityTerm> positiveEq;
            std::vector<std::pair<ValueId, EqualityTerm>> negativeEqs;
            std::vector<ValueId> commonTerms;
            commonTerms.reserve(terms.size());
            for (ValueId term : terms)
            {
                if (auto eq = matchEqualityTerm(graph, term))
                {
                    if (positiveEq)
                    {
                        return std::nullopt;
                    }
                    positiveEq = std::move(*eq);
                    continue;
                }
                if (auto negativeEq = matchNegatedEqualityTerm(graph, term))
                {
                    negativeEqs.emplace_back(term, std::move(*negativeEq));
                    continue;
                }
                commonTerms.push_back(term);
            }
            if (!positiveEq)
            {
                return std::nullopt;
            }

            std::vector<ValueId> conflictAddrs;
            conflictAddrs.reserve(negativeEqs.size());
            for (const auto &[term, negativeEq] : negativeEqs)
            {
                if (negativeEq.row != positiveEq->row)
                {
                    commonTerms.push_back(term);
                    continue;
                }
                conflictAddrs.push_back(negativeEq.addr);
            }
            return PriorityGuardMatch{
                .addr = positiveEq->addr,
                .commonTerms = std::move(commonTerms),
                .conflictAddrs = std::move(conflictAddrs),
                .row = positiveEq->row,
            };
        }

        std::optional<CompoundWriteMatch> matchCompoundWrite(const Graph &graph, const WritePortInfo &write)
        {
            const auto orTerms = splitLogicOrTerms(graph, write.updateCond);
            if (!orTerms)
            {
                return std::nullopt;
            }
            const auto nextValue = unwrapAssign(graph, write.nextValue);
            if (!nextValue)
            {
                return std::nullopt;
            }
            const OperationId muxOpId = graph.valueDef(*nextValue);
            if (!muxOpId.valid())
            {
                return std::nullopt;
            }
            const Operation muxOp = graph.getOperation(muxOpId);
            if (muxOp.kind() != OperationKind::kMux ||
                muxOp.operands().size() != 3 ||
                muxOp.results().size() != 1)
            {
                return std::nullopt;
            }

            const std::array<ValueId, 2> arms{orTerms->first, orTerms->second};
            const ValueId muxCond = muxOp.operands()[0];
            const ValueId trueData = muxOp.operands()[1];
            const ValueId falseData = muxOp.operands()[2];
            for (std::size_t activeIndex = 0; activeIndex < arms.size(); ++activeIndex)
            {
                const std::size_t resetIndex = 1U - activeIndex;
                auto regularGuard = matchGuard(graph, arms[activeIndex]);
                if (!regularGuard)
                {
                    continue;
                }
                const auto resetGuard = unwrapAssign(graph, arms[resetIndex]);
                if (!resetGuard)
                {
                    continue;
                }
                if (!commonTermsContainNegationOf(graph, regularGuard->commonTerms, *resetGuard))
                {
                    continue;
                }
                if (sameValueAfterAssign(graph, muxCond, arms[activeIndex]))
                {
                    return CompoundWriteMatch{.regularGuard = std::move(*regularGuard),
                                              .regularData = trueData,
                                              .resetGuard = *resetGuard,
                                              .resetData = falseData};
                }
                if (sameValueAfterAssign(graph, muxCond, *resetGuard))
                {
                    return CompoundWriteMatch{.regularGuard = std::move(*regularGuard),
                                              .regularData = falseData,
                                              .resetGuard = *resetGuard,
                                              .resetData = trueData};
                }
            }
            return std::nullopt;
        }

        std::optional<ValueId> normalizedSliceDynamicIndex(const Graph &graph, ValueId startValue, int64_t elementWidth)
        {
            if (!startValue.valid() || elementWidth <= 0)
            {
                return std::nullopt;
            }
            if (elementWidth == 1)
            {
                return startValue;
            }
            const OperationId defOpId = graph.valueDef(startValue);
            if (!defOpId.valid())
            {
                return std::nullopt;
            }
            const Operation defOp = graph.getOperation(defOpId);
            if (defOp.kind() != OperationKind::kMul || defOp.operands().size() != 2 || defOp.results().size() != 1)
            {
                return std::nullopt;
            }
            const auto lhsConst = getConstantUInt64(graph, defOp.operands()[0]);
            if (lhsConst && *lhsConst == static_cast<uint64_t>(elementWidth))
            {
                return defOp.operands()[1];
            }
            const auto rhsConst = getConstantUInt64(graph, defOp.operands()[1]);
            if (rhsConst && *rhsConst == static_cast<uint64_t>(elementWidth))
            {
                return defOp.operands()[0];
            }
            return std::nullopt;
        }

        std::optional<AnchorCandidate> matchCommonConcatAnchor(const Graph &graph,
                                                               const ValueUseIndex &uses,
                                                               OperationId sliceOpId,
                                                               ValueId packedValue,
                                                               ValueId indexValue,
                                                               int64_t elementWidth,
                                                               std::string sliceKind)
        {
            if (!packedValue.valid() || !indexValue.valid() || elementWidth <= 0 || elementWidth > INT32_MAX)
            {
                return std::nullopt;
            }
            const OperationId concatOpId = graph.valueDef(packedValue);
            if (!concatOpId.valid())
            {
                return std::nullopt;
            }
            const Operation concatOp = graph.getOperation(concatOpId);
            if (concatOp.kind() != OperationKind::kConcat || concatOp.results().size() != 1)
            {
                return std::nullopt;
            }
            if (!hasOnlyUser(uses, concatOp.results().front(), sliceOpId))
            {
                return std::nullopt;
            }

            AnchorCandidate candidate;
            candidate.sliceOp = sliceOpId;
            candidate.concatOp = concatOpId;
            candidate.elementWidth = static_cast<int32_t>(elementWidth);
            candidate.sliceKind = std::move(sliceKind);
            candidate.indexValue = indexValue;

            const auto concatOperands = concatOp.operands();
            if (concatOperands.size() < 2)
            {
                return std::nullopt;
            }
            candidate.elementCount = concatOperands.size();
            candidate.readOps.reserve(concatOperands.size());
            candidate.regSymbols.reserve(concatOperands.size());
            candidate.operandRows.reserve(concatOperands.size());

            int32_t expectedWidth = 0;
            bool expectedSigned = false;
            bool initialized = false;
            for (std::size_t operandIndex = 0; operandIndex < concatOperands.size(); ++operandIndex)
            {
                const ValueId readValue = concatOperands[operandIndex];
                const OperationId readOpId = graph.valueDef(readValue);
                if (!readOpId.valid())
                {
                    return std::nullopt;
                }
                const Operation readOp = graph.getOperation(readOpId);
                if (readOp.kind() != OperationKind::kRegisterReadPort || readOp.results().size() != 1)
                {
                    return std::nullopt;
                }
                if (!hasOnlyUser(uses, readOp.results().front(), concatOpId))
                {
                    return std::nullopt;
                }
                const auto regSymbol = getStringAttr(readOp, "regSymbol");
                if (!regSymbol || !isRegisterDecl(graph, *regSymbol))
                {
                    return std::nullopt;
                }
                if (graph.valueType(readValue) != ValueType::Logic)
                {
                    return std::nullopt;
                }
                const int32_t width = graph.valueWidth(readValue);
                const bool isSigned = graph.valueSigned(readValue);
                if (width != candidate.elementWidth)
                {
                    return std::nullopt;
                }
                if (!initialized)
                {
                    expectedWidth = width;
                    expectedSigned = isSigned;
                    initialized = true;
                }
                else if (width != expectedWidth || isSigned != expectedSigned)
                {
                    return std::nullopt;
                }

                candidate.readOps.push_back(readOpId);
                candidate.regSymbols.push_back(*regSymbol);
                candidate.operandRows.push_back(static_cast<int64_t>(concatOperands.size() - 1 - operandIndex));
            }
            std::reverse(candidate.regSymbols.begin(), candidate.regSymbols.end());
            return candidate;
        }

        std::optional<AnchorCandidate> matchSliceArrayAnchor(const Graph &graph,
                                                             const ValueUseIndex &uses,
                                                             OperationId sliceOpId,
                                                             const Operation &sliceOp)
        {
            const auto operands = sliceOp.operands();
            const auto results = sliceOp.results();
            if (operands.size() != 2 || results.size() != 1)
            {
                return std::nullopt;
            }
            const auto sliceWidth = getAttr<int64_t>(sliceOp, "sliceWidth");
            if (!sliceWidth)
            {
                return std::nullopt;
            }
            return matchCommonConcatAnchor(
                graph, uses, sliceOpId, operands[0], operands[1], *sliceWidth, std::string("slice-array"));
        }

        std::optional<AnchorCandidate> matchSliceDynamicAnchor(const Graph &graph,
                                                               const ValueUseIndex &uses,
                                                               OperationId sliceOpId,
                                                               const Operation &sliceOp)
        {
            const auto operands = sliceOp.operands();
            const auto results = sliceOp.results();
            if (operands.size() != 2 || results.size() != 1)
            {
                return std::nullopt;
            }
            const auto sliceWidth = getAttr<int64_t>(sliceOp, "sliceWidth");
            if (!sliceWidth || *sliceWidth <= 0)
            {
                return std::nullopt;
            }
            const auto indexValue = normalizedSliceDynamicIndex(graph, operands[1], *sliceWidth);
            if (!indexValue)
            {
                return std::nullopt;
            }
            return matchCommonConcatAnchor(
                graph, uses, sliceOpId, operands[0], *indexValue, *sliceWidth, std::string("slice-dynamic"));
        }

        std::vector<AnchorCandidate> discoverIntentAnchors(const Graph &graph,
                                                           const ValueUseIndex &uses,
                                                           std::size_t minElementCount)
        {
            std::vector<AnchorCandidate> anchors;
            for (OperationId opId : graph.operations())
            {
                const Operation op = graph.getOperation(opId);
                std::optional<AnchorCandidate> anchor;
                if (op.kind() == OperationKind::kSliceArray)
                {
                    anchor = matchSliceArrayAnchor(graph, uses, opId, op);
                }
                else if (op.kind() == OperationKind::kSliceDynamic)
                {
                    anchor = matchSliceDynamicAnchor(graph, uses, opId, op);
                }
                else
                {
                    continue;
                }
                if (!anchor || anchor->elementCount < minElementCount)
                {
                    continue;
                }
                anchors.push_back(std::move(*anchor));
            }
            return anchors;
        }

        std::size_t repeatedSequencePeriod(std::span<const std::string> values)
        {
            if (values.empty())
            {
                return 0;
            }
            std::vector<std::size_t> prefix(values.size(), 0);
            for (std::size_t i = 1; i < values.size(); ++i)
            {
                std::size_t matched = prefix[i - 1];
                while (matched != 0 && values[i] != values[matched])
                {
                    matched = prefix[matched - 1];
                }
                if (values[i] == values[matched])
                {
                    ++matched;
                }
                prefix[i] = matched;
            }
            const std::size_t candidate = values.size() - prefix.back();
            return values.size() % candidate == 0 ? candidate : values.size();
        }

        std::vector<AnchorCandidate> discoverTrueOnlyStorageAnchors(const Graph &graph,
                                                                    const ValueUseIndex &uses,
                                                                    std::size_t minElementCount)
        {
            std::vector<AnchorCandidate> anchors;
            for (OperationId concatOpId : graph.operations())
            {
                const Operation concatOp = graph.getOperation(concatOpId);
                if (concatOp.kind() != OperationKind::kConcat || concatOp.results().size() != 1)
                {
                    continue;
                }
                const auto operands = concatOp.operands();
                if (operands.size() < minElementCount)
                {
                    continue;
                }
                const auto packedUseIt = uses.find(concatOp.results().front());
                const bool hasSharedPackedView =
                    packedUseIt != uses.end() && packedUseIt->second.count > 1;

                std::vector<std::string> operandRegSymbols;
                std::vector<OperationId> operandReadOps;
                operandRegSymbols.reserve(operands.size());
                operandReadOps.reserve(operands.size());
                int32_t elementWidth = 0;
                bool elementSigned = false;
                bool valid = true;
                bool hasSharedRead = false;
                for (ValueId operand : operands)
                {
                    const OperationId readOpId = graph.valueDef(operand);
                    if (!readOpId.valid())
                    {
                        valid = false;
                        break;
                    }
                    const Operation readOp = graph.getOperation(readOpId);
                    const auto regSymbol = getStringAttr(readOp, "regSymbol");
                    if (readOp.kind() != OperationKind::kRegisterReadPort ||
                        readOp.results().size() != 1 || !regSymbol || !isRegisterDecl(graph, *regSymbol) ||
                        graph.valueType(operand) != ValueType::Logic)
                    {
                        valid = false;
                        break;
                    }
                    if (elementWidth == 0)
                    {
                        elementWidth = graph.valueWidth(operand);
                        elementSigned = graph.valueSigned(operand);
                    }
                    else if (graph.valueWidth(operand) != elementWidth ||
                             graph.valueSigned(operand) != elementSigned)
                    {
                        valid = false;
                        break;
                    }
                    const auto useIt = uses.find(operand);
                    if (useIt == uses.end() || useIt->second.count != 1 ||
                        useIt->second.onlyUser != concatOpId)
                    {
                        hasSharedRead = true;
                    }
                    operandRegSymbols.push_back(*regSymbol);
                    operandReadOps.push_back(readOpId);
                }
                if (!valid || elementWidth <= 0)
                {
                    continue;
                }

                const std::size_t period = repeatedSequencePeriod(operandRegSymbols);
                const bool repeatedLayout = period < operandRegSymbols.size();
                if (period < minElementCount ||
                    (!repeatedLayout && !hasSharedRead && !hasSharedPackedView))
                {
                    continue;
                }
                std::unordered_set<std::string> uniqueRegs;
                bool uniquePeriod = true;
                for (std::size_t index = 0; index < period; ++index)
                {
                    if (!uniqueRegs.insert(operandRegSymbols[index]).second)
                    {
                        uniquePeriod = false;
                        break;
                    }
                }
                if (!uniquePeriod)
                {
                    continue;
                }

                AnchorCandidate anchor;
                anchor.concatOp = concatOpId;
                anchor.elementWidth = elementWidth;
                anchor.elementCount = period;
                anchor.sliceKind = "true-storage-only";
                anchor.regSymbols.assign(operandRegSymbols.begin(), operandRegSymbols.begin() + period);
                std::reverse(anchor.regSymbols.begin(), anchor.regSymbols.end());
                anchor.operandRows.reserve(period);
                for (std::size_t operandIndex = 0; operandIndex < period; ++operandIndex)
                {
                    anchor.operandRows.push_back(static_cast<int64_t>(period - 1 - operandIndex));
                }
                std::unordered_set<OperationId, OperationIdHash> seenReads;
                for (OperationId readOp : operandReadOps)
                {
                    if (seenReads.insert(readOp).second)
                    {
                        anchor.readOps.push_back(readOp);
                    }
                }
                anchors.push_back(std::move(anchor));
            }
            return anchors;
        }

        std::vector<GroupCandidate> groupTrueOnlyStorageAnchors(std::vector<AnchorCandidate> anchors)
        {
            std::vector<GroupCandidate> groups;
            std::unordered_map<std::string, std::size_t> indexByKey;
            for (AnchorCandidate &anchor : anchors)
            {
                const std::string key = layoutKey(anchor);
                auto [it, inserted] = indexByKey.emplace(key, groups.size());
                if (inserted)
                {
                    GroupCandidate group;
                    group.regSymbols = anchor.regSymbols;
                    group.elementWidth = anchor.elementWidth;
                    group.elementCount = anchor.elementCount;
                    group.trueOnlyStorage = true;
                    groups.push_back(std::move(group));
                }
                groups[it->second].anchors.push_back(std::move(anchor));
            }

            std::vector<std::size_t> order(groups.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
                if (groups[lhs].elementCount != groups[rhs].elementCount)
                {
                    return groups[lhs].elementCount > groups[rhs].elementCount;
                }
                return lhs < rhs;
            });
            std::unordered_set<std::string> claimedRegs;
            std::vector<GroupCandidate> filtered;
            for (std::size_t groupIndex : order)
            {
                const GroupCandidate &group = groups[groupIndex];
                if (std::any_of(group.regSymbols.begin(), group.regSymbols.end(), [&](const std::string &symbol) {
                        return claimedRegs.contains(symbol);
                    }))
                {
                    continue;
                }
                claimedRegs.insert(group.regSymbols.begin(), group.regSymbols.end());
                filtered.push_back(std::move(groups[groupIndex]));
            }
            return filtered;
        }

        GroupAnchorsResult groupAnchors(std::vector<AnchorCandidate> anchors)
        {
            GroupAnchorsResult result;
            std::unordered_map<std::string, std::size_t> indexByKey;
            for (auto &anchor : anchors)
            {
                const std::string key = layoutKey(anchor);
                auto it = indexByKey.find(key);
                if (it == indexByKey.end())
                {
                    const std::size_t groupIndex = result.groups.size();
                    indexByKey.emplace(key, groupIndex);
                    GroupCandidate group;
                    group.regSymbols = anchor.regSymbols;
                    group.elementWidth = anchor.elementWidth;
                    group.elementCount = anchor.elementCount;
                    result.groups.push_back(std::move(group));
                    it = indexByKey.find(key);
                }
                GroupCandidate &group = result.groups[it->second];
                group.anchors.push_back(std::move(anchor));
            }

            result.candidateGroups = result.groups.size();
            for (const GroupCandidate &group : result.groups)
            {
                result.candidateAnchors += group.anchors.size();
                result.candidateMembers += group.regSymbols.size();
            }

            for (std::size_t groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex)
            {
                GroupCandidate &group = result.groups[groupIndex];
                group.intentGroupName = "rtm_intent_" + std::to_string(groupIndex);
                group.storageGroupName = group.intentGroupName;
                group.storageRegSymbols = group.regSymbols;
                group.storageElementCount = group.elementCount;
                group.storageRowOffset = 0;
                group.sharesStorage = false;
            }

            std::unordered_map<std::string, std::vector<std::size_t>> ownersByReg;
            for (std::size_t groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex)
            {
                for (const std::string &regSymbol : result.groups[groupIndex].regSymbols)
                {
                    ownersByReg[regSymbol].push_back(groupIndex);
                }
            }

            std::vector<std::vector<std::size_t>> overlapping(result.groups.size());
            for (const auto &[regSymbol, owners] : ownersByReg)
            {
                (void)regSymbol;
                for (std::size_t i = 0; i < owners.size(); ++i)
                {
                    for (std::size_t j = i + 1; j < owners.size(); ++j)
                    {
                        overlapping[owners[i]].push_back(owners[j]);
                        overlapping[owners[j]].push_back(owners[i]);
                    }
                }
            }
            for (auto &neighbors : overlapping)
            {
                std::sort(neighbors.begin(), neighbors.end());
                neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
            }

            const std::size_t unassigned = std::numeric_limits<std::size_t>::max();
            std::vector<std::size_t> storageOf(result.groups.size(), unassigned);
            std::vector<std::size_t> rowOffsets(result.groups.size(), 0);
            std::vector<std::size_t> familySizes(result.groups.size(), 0);
            std::vector<uint8_t> conflicted(result.groups.size(), 0);
            std::vector<std::size_t> selectedStorageRoots;
            std::vector<std::unordered_map<OperationId, std::size_t, OperationIdHash>> familyReadOwnerByRoot(
                result.groups.size());

            std::vector<std::size_t> order;
            order.reserve(result.groups.size());
            for (std::size_t groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex)
            {
                order.push_back(groupIndex);
            }
            std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
                const GroupCandidate &left = result.groups[lhs];
                const GroupCandidate &right = result.groups[rhs];
                if (left.elementCount != right.elementCount)
                {
                    return left.elementCount > right.elementCount;
                }
                if (left.elementWidth != right.elementWidth)
                {
                    return left.elementWidth < right.elementWidth;
                }
                const std::string leftKey = rowKey(left.regSymbols);
                const std::string rightKey = rowKey(right.regSymbols);
                if (leftKey != rightKey)
                {
                    return leftKey < rightKey;
                }
                return lhs < rhs;
            });

            const auto groupsOverlap = [&](std::size_t lhs, std::size_t rhs) {
                const auto &neighbors = overlapping[lhs];
                return std::binary_search(neighbors.begin(), neighbors.end(), rhs);
            };

            for (const std::size_t groupIndex : order)
            {
                if (storageOf[groupIndex] != unassigned || conflicted[groupIndex] != 0)
                {
                    continue;
                }

                bool assignedToExistingStorage = false;
                bool conflictsWithExistingStorage = false;
                for (const std::size_t storageIndex : selectedStorageRoots)
                {
                    std::size_t rowOffset = 0;
                    if (groupsCanShareStorage(result.groups[groupIndex], result.groups[storageIndex], &rowOffset) &&
                        !groupSharesReadOpsWithFamily(result.groups[groupIndex],
                                                      familyReadOwnerByRoot[storageIndex]))
                    {
                        storageOf[groupIndex] = storageIndex;
                        rowOffsets[groupIndex] = rowOffset;
                        ++familySizes[storageIndex];
                        addGroupReadsToFamily(result.groups[groupIndex],
                                              groupIndex,
                                              familyReadOwnerByRoot[storageIndex]);
                        assignedToExistingStorage = true;
                        break;
                    }
                    if (groupsOverlap(groupIndex, storageIndex))
                    {
                        conflictsWithExistingStorage = true;
                    }
                }
                if (assignedToExistingStorage)
                {
                    continue;
                }
                if (conflictsWithExistingStorage)
                {
                    conflicted[groupIndex] = 1;
                    continue;
                }

                storageOf[groupIndex] = groupIndex;
                rowOffsets[groupIndex] = 0;
                familySizes[groupIndex] = 1;
                selectedStorageRoots.push_back(groupIndex);
                addGroupReadsToFamily(result.groups[groupIndex],
                                      groupIndex,
                                      familyReadOwnerByRoot[groupIndex]);
                for (std::size_t viewIndex = 0; viewIndex < result.groups.size(); ++viewIndex)
                {
                    if (viewIndex == groupIndex ||
                        storageOf[viewIndex] != unassigned ||
                        conflicted[viewIndex] != 0)
                    {
                        continue;
                    }
                    std::size_t rowOffset = 0;
                    if (groupsCanShareStorage(result.groups[viewIndex], result.groups[groupIndex], &rowOffset) &&
                        !groupSharesReadOpsWithFamily(result.groups[viewIndex],
                                                      familyReadOwnerByRoot[groupIndex]))
                    {
                        storageOf[viewIndex] = groupIndex;
                        rowOffsets[viewIndex] = rowOffset;
                        ++familySizes[groupIndex];
                        addGroupReadsToFamily(result.groups[viewIndex],
                                              viewIndex,
                                              familyReadOwnerByRoot[groupIndex]);
                    }
                }
            }

            for (std::size_t groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex)
            {
                if (conflicted[groupIndex] != 0)
                {
                    continue;
                }
                const std::size_t storageIndex = storageOf[groupIndex];
                if (storageIndex == unassigned)
                {
                    conflicted[groupIndex] = 1;
                    continue;
                }
                GroupCandidate &group = result.groups[groupIndex];
                const GroupCandidate &storage = result.groups[storageIndex];
                group.storageGroupName = storage.intentGroupName;
                group.storageRegSymbols = storage.regSymbols;
                group.storageElementCount = storage.elementCount;
                group.storageRowOffset = rowOffsets[groupIndex];
                group.sharesStorage = familySizes[storageIndex] > 1;
                group.overlapsOtherCandidate = !overlapping[groupIndex].empty();
            }

            std::vector<GroupCandidate> filteredGroups;
            filteredGroups.reserve(result.groups.size());
            for (std::size_t groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex)
            {
                if (conflicted[groupIndex] != 0)
                {
                    const GroupCandidate &group = result.groups[groupIndex];
                    ++result.conflictGroups;
                    result.conflictAnchors += group.anchors.size();
                    result.conflictMembers += group.regSymbols.size();
                    continue;
                }
                filteredGroups.push_back(std::move(result.groups[groupIndex]));
            }
            result.groups = std::move(filteredGroups);
            return result;
        }

        std::unordered_map<std::string, std::vector<WritePortInfo>> collectRegisterWrites(const Graph &graph)
        {
            std::unordered_map<std::string, std::vector<WritePortInfo>> writesByReg;
            for (OperationId opId : graph.operations())
            {
                const Operation op = graph.getOperation(opId);
                if (op.kind() != OperationKind::kRegisterWritePort || op.operands().size() < 4 || !op.results().empty())
                {
                    continue;
                }
                const auto regSymbol = getStringAttr(op, "regSymbol");
                if (!regSymbol)
                {
                    continue;
                }
                const auto eventEdges = getAttr<std::vector<std::string>>(op, "eventEdge");
                const std::size_t eventCount = op.operands().size() - 3;
                if (!eventEdges || eventEdges->size() != eventCount)
                {
                    continue;
                }
                WritePortInfo info;
                info.op = opId;
                info.updateCond = op.operands()[0];
                info.nextValue = op.operands()[1];
                info.mask = op.operands()[2];
                info.events.assign(op.operands().begin() + 3, op.operands().end());
                info.eventEdges = *eventEdges;
                writesByReg[*regSymbol].push_back(std::move(info));
            }
            return writesByReg;
        }

        bool isAllOnesMask(const Graph &graph, ValueId value, int32_t width)
        {
            if (width <= 0)
            {
                return false;
            }
            const OperationId defOpId = graph.valueDef(value);
            if (!defOpId.valid())
            {
                return false;
            }
            const Operation op = graph.getOperation(defOpId);
            if (op.kind() != OperationKind::kConstant)
            {
                return false;
            }
            const auto literal = getStringAttr(op, "constValue");
            if (!literal)
            {
                return false;
            }
            auto parsed = parseConstLiteral(*literal);
            if (!parsed || parsed->hasUnknown())
            {
                return false;
            }
            parsed = parsed->resize(static_cast<slang::bitwidth_t>(width));
            const std::size_t wordCount = static_cast<std::size_t>((width + 63) / 64);
            const std::uint64_t *raw = parsed->getRawPtr();
            for (std::size_t i = 0; i < wordCount; ++i)
            {
                const std::size_t bits = (i + 1u == wordCount) ? static_cast<std::size_t>(width) - i * 64u : 64u;
                const std::uint64_t expected = bits >= 64u ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - 1u);
                if (raw[i] != expected)
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<RegularWriteMatch> matchRegularWriteFamily(
            const Graph &graph,
            const GroupCandidate &group,
            const std::unordered_map<std::string, std::vector<WritePortInfo>> &writesByReg,
            std::vector<WritePortInfo> &leftoverWrites,
            const GroupProfileContext *groupProfile = nullptr)
        {
            if (group.regSymbols.empty() || group.elementCount != group.regSymbols.size())
            {
                return std::nullopt;
            }
            const auto matchStart = ProfileClock::now();
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=regular_write_match_start members=" + std::to_string(group.regSymbols.size()));
            }
            std::vector<WritePortInfo> selected(group.elementCount);
            std::unordered_set<OperationId, OperationIdHash> selectedOps;
            ValueId expectedAddr;
            ValueId expectedData;
            ValueId expectedMask;
            std::vector<ValueId> expectedEvents;
            std::vector<std::string> expectedEventEdges;
            std::optional<std::string> expectedCommonKey;
            std::vector<ValueId> expectedCommonTerms;
            std::vector<bool> seenRow(group.elementCount, false);
            std::vector<ValueId> splitResetData(group.elementCount);
            std::vector<WritePortInfo> splitResetWrites(group.elementCount);
            ValueId expectedSplitResetGuard;
            bool usingDirectWrites = false;
            bool usingCompoundWrites = false;

            for (std::size_t row = 0; row < group.regSymbols.size(); ++row)
            {
                auto it = writesByReg.find(group.regSymbols[row]);
                if (it == writesByReg.end())
                {
                    return std::nullopt;
                }
                std::optional<WritePortInfo> matchedWrite;
                std::optional<GuardMatch> matchedGuard;
                std::optional<ValueId> matchedData;
                std::optional<ValueId> matchedResetGuard;
                std::optional<ValueId> matchedResetData;
                bool matchedCompound = false;
                const auto rowStart = ProfileClock::now();
                if (shouldProfileGroupRow(groupProfile, row, group.regSymbols.size()))
                {
                    profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                               " group=" + std::to_string(groupProfile->groupIndex) +
                               "/" + std::to_string(groupProfile->groupCount) +
                               " stage=regular_write_row_start row=" + std::to_string(row) +
                               " writes=" + std::to_string(it->second.size()));
                }
                for (const WritePortInfo &write : it->second)
                {
                    const auto guard = matchGuard(graph, write.updateCond);
                    if (!guard || guard->row != row)
                    {
                        const auto compound = matchCompoundWrite(graph, write);
                        if (!compound || compound->regularGuard.row != row)
                        {
                            continue;
                        }
                        if (compound->regularGuard.row >= group.elementCount)
                        {
                            continue;
                        }
                        matchedWrite = write;
                        matchedGuard = compound->regularGuard;
                        matchedData = compound->regularData;
                        matchedResetGuard = compound->resetGuard;
                        matchedResetData = compound->resetData;
                        matchedCompound = true;
                        break;
                    }
                    if (guard->row >= group.elementCount)
                    {
                        continue;
                    }
                    matchedWrite = write;
                    matchedGuard = std::move(*guard);
                    matchedData = write.nextValue;
                    break;
                }
                if (shouldProfileGroupRow(groupProfile, row, group.regSymbols.size()))
                {
                    profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                               " group=" + std::to_string(groupProfile->groupIndex) +
                               "/" + std::to_string(groupProfile->groupCount) +
                               " stage=regular_write_row_done row=" + std::to_string(row) +
                               " ms=" + std::to_string(elapsedMs(rowStart)) +
                               " matched=" + std::to_string(matchedWrite.has_value() ? 1 : 0) +
                               " compound=" + std::to_string(matchedCompound ? 1 : 0));
                }
                if (!matchedWrite || !matchedGuard || !matchedData)
                {
                    return std::nullopt;
                }
                if (matchedCompound)
                {
                    usingCompoundWrites = true;
                    if (usingDirectWrites)
                    {
                        return std::nullopt;
                    }
                    if (!matchedResetGuard || !matchedResetData)
                    {
                        return std::nullopt;
                    }
                    if (!isAllOnesMask(graph, matchedWrite->mask, group.elementWidth))
                    {
                        return std::nullopt;
                    }
                    if (!expectedSplitResetGuard.valid())
                    {
                        expectedSplitResetGuard = *matchedResetGuard;
                    }
                    else if (!sameValueAfterAssign(graph, *matchedResetGuard, expectedSplitResetGuard))
                    {
                        return std::nullopt;
                    }
                    splitResetData[row] = *matchedResetData;
                    splitResetWrites[row] = *matchedWrite;
                }
                else
                {
                    usingDirectWrites = true;
                    if (usingCompoundWrites)
                    {
                        return std::nullopt;
                    }
                }
                if (seenRow[static_cast<std::size_t>(matchedGuard->row)])
                {
                    return std::nullopt;
                }
                seenRow[static_cast<std::size_t>(matchedGuard->row)] = true;

                if (!expectedAddr.valid())
                {
                    expectedAddr = matchedGuard->addr;
                    expectedData = *matchedData;
                    expectedMask = matchedWrite->mask;
                    expectedEvents = matchedWrite->events;
                    expectedEventEdges = matchedWrite->eventEdges;
                    expectedCommonKey = valueSetKey(matchedGuard->commonTerms);
                    expectedCommonTerms = matchedGuard->commonTerms;
                }
                else if (matchedGuard->addr != expectedAddr ||
                         *matchedData != expectedData ||
                         matchedWrite->mask != expectedMask ||
                         !valueVectorsEqual(matchedWrite->events, expectedEvents) ||
                         matchedWrite->eventEdges != expectedEventEdges ||
                         valueSetKey(matchedGuard->commonTerms) != expectedCommonKey)
                {
                    return std::nullopt;
                }
                selected[row] = *matchedWrite;
                selectedOps.insert(matchedWrite->op);
            }
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=regular_write_rows_done ms=" + std::to_string(elapsedMs(matchStart)));
            }
            if (!std::all_of(seenRow.begin(), seenRow.end(), [](bool v) { return v; }))
            {
                return std::nullopt;
            }

            leftoverWrites.clear();
            for (const std::string &regSymbol : group.regSymbols)
            {
                const auto it = writesByReg.find(regSymbol);
                if (it == writesByReg.end())
                {
                    return std::nullopt;
                }
                for (const WritePortInfo &write : it->second)
                {
                    if (!selectedOps.contains(write.op))
                    {
                        leftoverWrites.push_back(write);
                    }
                }
            }
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=regular_write_leftover_done ms=" + std::to_string(elapsedMs(matchStart)) +
                           " leftover=" + std::to_string(leftoverWrites.size()));
            }

            RegularWriteFamily family;
            family.writes = std::move(selected);
            family.addr = expectedAddr;
            family.data = expectedData;
            family.mask = expectedMask;
            family.events = std::move(expectedEvents);
            family.eventEdges = std::move(expectedEventEdges);
            family.commonTerms = std::move(expectedCommonTerms);

            RegularWriteMatch match;
            match.family = std::move(family);
            if (usingCompoundWrites)
            {
                ResetWriteFamily reset;
                reset.writes = std::move(splitResetWrites);
                reset.guard = expectedSplitResetGuard;
                reset.guardTerms.push_back(expectedSplitResetGuard);
                reset.mask = match.family.mask;
                reset.events = match.family.events;
                reset.eventEdges = match.family.eventEdges;
                reset.rowData = std::move(splitResetData);
                reset.data = reset.rowData.front();
                reset.packed = std::any_of(reset.rowData.begin(), reset.rowData.end(), [&](ValueId value) {
                    return value != reset.data;
                });
                match.splitReset = std::move(reset);
            }
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=regular_write_match_done ms=" + std::to_string(elapsedMs(matchStart)) +
                           " split_reset=" + std::to_string(match.splitReset.has_value() ? 1 : 0));
            }
            return match;
        }

        std::optional<ResetWriteFamily> matchResetWriteFamily(const Graph &graph,
                                                              const GroupCandidate &group,
                                                              std::span<const WritePortInfo> writes)
        {
            if (writes.empty())
            {
                return std::nullopt;
            }
            if (writes.size() != group.elementCount)
            {
                return std::nullopt;
            }
            ValueId expectedGuard;
            ValueId expectedMask;
            std::vector<ValueId> expectedEvents;
            std::vector<std::string> expectedEventEdges;
            std::vector<ValueId> rowData(group.elementCount);
            std::vector<WritePortInfo> ordered(group.elementCount);
            for (std::size_t i = 0; i < writes.size(); ++i)
            {
                const WritePortInfo &write = writes[i];
                if (!isAllOnesMask(graph, write.mask, group.elementWidth))
                {
                    return std::nullopt;
                }
                if (!expectedGuard.valid())
                {
                    expectedGuard = write.updateCond;
                    expectedMask = write.mask;
                    expectedEvents = write.events;
                    expectedEventEdges = write.eventEdges;
                }
                else if (write.updateCond != expectedGuard ||
                         write.mask != expectedMask ||
                         !valueVectorsEqual(write.events, expectedEvents) ||
                         write.eventEdges != expectedEventEdges)
                {
                    return std::nullopt;
                }
                rowData[i] = write.nextValue;
                ordered[i] = write;
            }
            ResetWriteFamily family;
            family.writes = std::move(ordered);
            family.guard = expectedGuard;
            family.guardTerms.push_back(expectedGuard);
            family.mask = expectedMask;
            family.events = std::move(expectedEvents);
            family.eventEdges = std::move(expectedEventEdges);
            family.rowData = std::move(rowData);
            family.data = family.rowData.front();
            family.packed = std::any_of(family.rowData.begin(), family.rowData.end(), [&](ValueId value) {
                return value != family.data;
            });
            return family;
        }

        std::optional<MuxWriteChain> matchMuxWriteChain(const Graph &graph, ValueId value)
        {
            MuxWriteChain chain;
            std::unordered_set<ValueId, ValueIdHash> seen;
            ValueId current = value;
            while (true)
            {
                const auto unwrapped = unwrapAssign(graph, current);
                if (!unwrapped || !seen.insert(*unwrapped).second)
                {
                    return std::nullopt;
                }
                current = *unwrapped;
                const OperationId opId = graph.valueDef(current);
                if (!opId.valid())
                {
                    chain.fallback = current;
                    break;
                }
                const Operation op = graph.getOperation(opId);
                if (op.kind() != OperationKind::kMux)
                {
                    chain.fallback = current;
                    break;
                }
                if (op.operands().size() != 3)
                {
                    return std::nullopt;
                }
                chain.branches.push_back(MuxWriteBranch{
                    .guard = op.operands()[0],
                    .data = op.operands()[1],
                });
                current = op.operands()[2];
            }
            while (!chain.branches.empty() &&
                   sameValueAfterAssign(graph, chain.branches.back().data, chain.fallback))
            {
                chain.branches.pop_back();
            }
            if (chain.branches.empty() || !chain.fallback.valid())
            {
                return std::nullopt;
            }
            return chain;
        }

        std::optional<ConsolidatedWriteMatch> matchConsolidatedWriteFamilies(
            const Graph &graph,
            const GroupCandidate &group,
            const std::unordered_map<std::string, std::vector<WritePortInfo>> &writesByReg,
            const GroupProfileContext *groupProfile = nullptr)
        {
            auto reject = [&](std::string_view reason,
                              std::size_t row,
                              std::size_t branch,
                              const std::string &extra = "") -> std::optional<ConsolidatedWriteMatch> {
                if (groupProfile != nullptr && groupProfile->verbose)
                {
                    std::string message =
                        "group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                        " group=" + std::to_string(groupProfile->groupIndex) +
                        "/" + std::to_string(groupProfile->groupCount) +
                        " stage=consolidated_write_reject reason=" + std::string(reason);
                    if (row != std::numeric_limits<std::size_t>::max())
                    {
                        message += " row=" + std::to_string(row);
                    }
                    if (branch != std::numeric_limits<std::size_t>::max())
                    {
                        message += " branch=" + std::to_string(branch);
                    }
                    if (!extra.empty())
                    {
                        message += " " + extra;
                    }
                    profileLog(message);
                }
                return std::nullopt;
            };
            constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
            if (group.regSymbols.empty() || group.elementCount != group.regSymbols.size())
            {
                return reject("invalid_group_shape", kNoIndex, kNoIndex);
            }

            std::vector<const WritePortInfo *> rowWrites;
            std::vector<std::vector<ValueId>> rowUpdateTerms;
            std::unordered_map<ValueId, std::size_t, ValueIdHash> updateTermRowCounts;
            rowWrites.reserve(group.regSymbols.size());
            rowUpdateTerms.reserve(group.regSymbols.size());
            for (std::size_t row = 0; row < group.regSymbols.size(); ++row)
            {
                const auto writeIt = writesByReg.find(group.regSymbols[row]);
                if (writeIt == writesByReg.end() || writeIt->second.size() != 1)
                {
                    const std::size_t writeCount = writeIt == writesByReg.end() ? 0 : writeIt->second.size();
                    return reject("write_count", row, kNoIndex, "writes=" + std::to_string(writeCount));
                }
                rowWrites.push_back(&writeIt->second.front());
                rowUpdateTerms.push_back(flattenLogicOrTerms(graph, writeIt->second.front().updateCond));

                std::unordered_set<ValueId, ValueIdHash> seenTerms;
                for (ValueId term : rowUpdateTerms.back())
                {
                    const auto unwrapped = unwrapAssign(graph, term);
                    const ValueId canonical = unwrapped ? *unwrapped : term;
                    if (seenTerms.insert(canonical).second)
                    {
                        ++updateTermRowCounts[canonical];
                    }
                }
            }

            ConsolidatedWriteMatch result;
            std::vector<std::string> commonKeys;
            std::vector<std::string> conflictKeys;
            std::optional<bool> hasReset;
            std::optional<std::string> resetTermsKey;
            for (std::size_t row = 0; row < group.regSymbols.size(); ++row)
            {
                const WritePortInfo &write = *rowWrites[row];
                const auto chain = matchMuxWriteChain(graph, write.nextValue);
                if (!chain)
                {
                    return reject("mux_chain", row, kNoIndex);
                }

                const std::vector<ValueId> &updateTerms = rowUpdateTerms[row];
                std::vector<uint8_t> consumed(updateTerms.size(), 0);
                for (const MuxWriteBranch &branch : chain->branches)
                {
                    bool found = false;
                    for (std::size_t termIndex = 0; termIndex < updateTerms.size(); ++termIndex)
                    {
                        if (consumed[termIndex] == 0 &&
                            sameValueAfterAssign(graph, updateTerms[termIndex], branch.guard))
                        {
                            consumed[termIndex] = 1;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        return reject("branch_not_in_update", row, kNoIndex);
                    }
                }

                std::vector<MuxWriteBranch> effectiveBranches = chain->branches;
                std::vector<ValueId> resetTerms;
                for (std::size_t termIndex = 0; termIndex < updateTerms.size(); ++termIndex)
                {
                    if (consumed[termIndex] == 0)
                    {
                        const auto unwrapped = unwrapAssign(graph, updateTerms[termIndex]);
                        const ValueId canonical = unwrapped ? *unwrapped : updateTerms[termIndex];
                        const auto termCountIt = updateTermRowCounts.find(canonical);
                        const bool sharedAcrossRows =
                            termCountIt != updateTermRowCounts.end() &&
                            termCountIt->second == group.regSymbols.size();
                        // A group-wide fill can contain an equality whose constant happens to match one row.
                        auto fallbackGuard = sharedAcrossRows
                                                 ? std::optional<PriorityGuardMatch>{}
                                                 : matchPriorityGuard(graph, updateTerms[termIndex]);
                        if (fallbackGuard && fallbackGuard->row == row)
                        {
                            effectiveBranches.push_back(MuxWriteBranch{
                                .guard = updateTerms[termIndex],
                                .data = chain->fallback,
                            });
                        }
                        else
                        {
                            resetTerms.push_back(updateTerms[termIndex]);
                        }
                    }
                }
                const bool rowHasReset = !resetTerms.empty();
                if (!hasReset)
                {
                    hasReset = rowHasReset;
                }
                else if (*hasReset != rowHasReset)
                {
                    return reject("reset_presence_mismatch", row, kNoIndex);
                }

                if (row == 0)
                {
                    result.regulars.resize(effectiveBranches.size());
                    commonKeys.resize(effectiveBranches.size());
                    conflictKeys.resize(effectiveBranches.size());
                }
                else if (result.regulars.size() != effectiveBranches.size())
                {
                    return reject("branch_count_mismatch",
                                  row,
                                  kNoIndex,
                                  "expected=" + std::to_string(result.regulars.size()) +
                                      " actual=" + std::to_string(effectiveBranches.size()));
                }

                for (std::size_t branchIndex = 0; branchIndex < effectiveBranches.size(); ++branchIndex)
                {
                    const MuxWriteBranch &branch = effectiveBranches[branchIndex];
                    auto guard = matchPriorityGuard(graph, branch.guard);
                    if (!guard || guard->row != row)
                    {
                        return reject("priority_guard",
                                      row,
                                      branchIndex,
                                      guard ? "guard_row=" + std::to_string(guard->row) : "unmatched=1");
                    }
                    if (rowHasReset)
                    {
                        if (!commonTermsExcludeResetSet(graph, guard->commonTerms, resetTerms))
                        {
                            std::string termNames;
                            for (ValueId resetTerm : resetTerms)
                            {
                                if (!termNames.empty())
                                {
                                    termNames += ",";
                                }
                                termNames += graph.symbolText(graph.valueSymbol(resetTerm));
                                termNames += commonTermsExcludeResetTerm(graph, guard->commonTerms, resetTerm)
                                                 ? ":excluded"
                                                 : ":live";
                            }
                            std::string commonNames;
                            for (ValueId commonTerm : guard->commonTerms)
                            {
                                if (!commonNames.empty())
                                {
                                    commonNames += ",";
                                }
                                commonNames += graph.symbolText(graph.valueSymbol(commonTerm));
                            }
                            return reject("reset_negation",
                                          row,
                                          branchIndex,
                                          "reset_terms=" + termNames + " common_terms=" + commonNames);
                        }
                    }

                    RegularWriteFamily &family = result.regulars[branchIndex];
                    const std::string commonKey = valueSetKey(guard->commonTerms);
                    const std::string conflictKey = valueSetKey(guard->conflictAddrs);
                    if (row == 0)
                    {
                        family.addr = guard->addr;
                        family.data = branch.data;
                        family.mask = write.mask;
                        family.events = write.events;
                        family.eventEdges = write.eventEdges;
                        family.commonTerms = std::move(guard->commonTerms);
                        family.conflictAddrs = std::move(guard->conflictAddrs);
                        commonKeys[branchIndex] = commonKey;
                        conflictKeys[branchIndex] = conflictKey;
                    }
                    else if (family.addr != guard->addr ||
                             family.data != branch.data ||
                             family.mask != write.mask ||
                             !valueVectorsEqual(family.events, write.events) ||
                             family.eventEdges != write.eventEdges ||
                             commonKeys[branchIndex] != commonKey ||
                             conflictKeys[branchIndex] != conflictKey)
                    {
                        return reject("family_mismatch",
                                      row,
                                      branchIndex,
                                      "addr=" + std::to_string(family.addr == guard->addr) +
                                          " data=" + std::to_string(family.data == branch.data) +
                                          " mask=" + std::to_string(family.mask == write.mask) +
                                          " events=" +
                                          std::to_string(valueVectorsEqual(family.events, write.events)) +
                                          " edges=" + std::to_string(family.eventEdges == write.eventEdges) +
                                          " common=" +
                                          std::to_string(commonKeys[branchIndex] == commonKey) +
                                          " conflicts=" +
                                          std::to_string(conflictKeys[branchIndex] == conflictKey));
                    }
                    family.writes.push_back(write);
                }

                if (rowHasReset)
                {
                    if (!result.reset)
                    {
                        ResetWriteFamily reset;
                        reset.guard = resetTerms.front();
                        reset.guardTerms = resetTerms;
                        reset.mask = write.mask;
                        reset.events = write.events;
                        reset.eventEdges = write.eventEdges;
                        result.reset = std::move(reset);
                        resetTermsKey = valueSetKey(resetTerms);
                    }
                    else if (!resetTermsKey || *resetTermsKey != valueSetKey(resetTerms) ||
                             result.reset->mask != write.mask ||
                             !valueVectorsEqual(result.reset->events, write.events) ||
                             result.reset->eventEdges != write.eventEdges)
                    {
                        return reject("reset_family_mismatch", row, kNoIndex);
                    }
                    result.reset->writes.push_back(write);
                    result.reset->rowData.push_back(chain->fallback);
                }
            }

            if (result.regulars.empty())
            {
                return reject("empty_regulars", kNoIndex, kNoIndex);
            }
            if (result.reset)
            {
                if (!isAllOnesMask(graph, result.reset->mask, group.elementWidth) && group.elementWidth != 1)
                {
                    return reject("reset_mask", kNoIndex, kNoIndex);
                }
                result.reset->data = result.reset->rowData.front();
                result.reset->packed = std::any_of(
                    result.reset->rowData.begin(), result.reset->rowData.end(), [&](ValueId value) {
                        return value != result.reset->data;
                    });
            }
            return result;
        }

        std::optional<std::vector<std::optional<std::string>>> collectRegisterInits(const Graph &graph,
                                                                                    const GroupCandidate &group)
        {
            std::vector<std::optional<std::string>> initValues;
            initValues.reserve(group.regSymbols.size());
            for (const std::string &regSymbol : group.regSymbols)
            {
                const OperationId regOpId = graph.findOperation(regSymbol);
                if (!regOpId.valid())
                {
                    return std::nullopt;
                }
                const Operation regOp = graph.getOperation(regOpId);
                if (regOp.attr("initKind").has_value() ||
                    regOp.attr("initFile").has_value() ||
                    regOp.attr("initStart").has_value() ||
                    regOp.attr("initLen").has_value())
                {
                    return std::nullopt;
                }
                initValues.push_back(getStringAttr(regOp, "initValue"));
            }
            return initValues;
        }

        bool trueReadClosureEligible(const Graph &graph,
                                     const ValueUseIndex &uses,
                                     const RegisterReadIndex &readsByReg,
                                     const GroupCandidate &group,
                                     const GroupProfileContext *groupProfile = nullptr)
        {
            if (group.trueOnlyStorage)
            {
                for (const std::string &regSymbol : group.regSymbols)
                {
                    const auto readIt = readsByReg.find(regSymbol);
                    if (readIt == readsByReg.end() || readIt->second.empty())
                    {
                        return false;
                    }
                    for (OperationId readOpId : readIt->second)
                    {
                        const Operation readOp = graph.getOperation(readOpId);
                        if (readOp.kind() != OperationKind::kRegisterReadPort ||
                            readOp.results().size() != 1 ||
                            getStringAttr(readOp, "regSymbol").value_or(std::string()) != regSymbol ||
                            graph.valueType(readOp.results().front()) != ValueType::Logic ||
                            graph.valueWidth(readOp.results().front()) != group.elementWidth)
                        {
                            return false;
                        }
                    }
                }
                return true;
            }
            std::unordered_set<OperationId, OperationIdHash> anchorReadOps;
            std::unordered_set<OperationId, OperationIdHash> anchorConcatOps;
            std::unordered_set<OperationId, OperationIdHash> anchorSliceOps;
            const auto closureStart = ProfileClock::now();
            for (const AnchorCandidate &anchor : group.anchors)
            {
                if (anchor.regSymbols != group.regSymbols ||
                    anchor.elementWidth != group.elementWidth ||
                    anchor.elementCount != group.elementCount)
                {
                    return false;
                }
                if (!anchorConcatOps.insert(anchor.concatOp).second ||
                    !anchorSliceOps.insert(anchor.sliceOp).second)
                {
                    return false;
                }
                for (OperationId readOp : anchor.readOps)
                {
                    if (!anchorReadOps.insert(readOp).second)
                    {
                        return false;
                    }
                }
                const Operation concatOp = graph.getOperation(anchor.concatOp);
                if (concatOp.results().size() != 1 ||
                    !hasOnlyUser(uses, concatOp.results().front(), anchor.sliceOp))
                {
                    return false;
                }
                for (OperationId readOp : anchor.readOps)
                {
                    const Operation read = graph.getOperation(readOp);
                    if (read.results().size() != 1 ||
                        !hasOnlyUser(uses, read.results().front(), anchor.concatOp))
                    {
                        return false;
                    }
                }
            }

            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=true_read_anchor_scan_done ms=" + std::to_string(elapsedMs(closureStart)) +
                           " anchor_reads=" + std::to_string(anchorReadOps.size()));
            }

            for (std::size_t regIndex = 0; regIndex < group.regSymbols.size(); ++regIndex)
            {
                const std::string &regSymbol = group.regSymbols[regIndex];
                const auto readIt = readsByReg.find(regSymbol);
                if (readIt == readsByReg.end())
                {
                    return false;
                }
                if (shouldProfileGroupRow(groupProfile, regIndex, group.regSymbols.size()))
                {
                    profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                               " group=" + std::to_string(groupProfile->groupIndex) +
                               "/" + std::to_string(groupProfile->groupCount) +
                               " stage=true_read_reg_check reg_reads=" + std::to_string(readIt->second.size()));
                }
                for (OperationId opId : readIt->second)
                {
                    if (!anchorReadOps.contains(opId))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        std::optional<TrueMergeCandidate> matchTrueMerge(
            const Graph &graph,
            const ValueUseIndex &uses,
            const RegisterReadIndex &readsByReg,
            const GroupCandidate &group,
            const std::unordered_map<std::string, std::vector<WritePortInfo>> &writesByReg,
            RegToMemProfile *profile = nullptr,
            const GroupProfileContext *groupProfile = nullptr)
        {
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=true_match_start anchors=" + std::to_string(group.anchors.size()) +
                           " members=" + std::to_string(group.regSymbols.size()));
            }
            auto stageStart = ProfileClock::now();
            const bool readClosureEligible = trueReadClosureEligible(graph, uses, readsByReg, group, groupProfile);
            const int64_t closureMs = elapsedMs(stageStart);
            if (profile != nullptr)
            {
                profile->trueClosureMs += closureMs;
            }
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=true_read_closure_done ms=" + std::to_string(closureMs) +
                           " eligible=" + std::to_string(readClosureEligible ? 1 : 0));
            }
            if (!readClosureEligible)
            {
                return std::nullopt;
            }
            stageStart = ProfileClock::now();
            auto initValues = collectRegisterInits(graph, group);
            const int64_t initMs = elapsedMs(stageStart);
            if (profile != nullptr)
            {
                profile->collectInitsMs += initMs;
            }
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=collect_inits_done ms=" + std::to_string(initMs) +
                           " ok=" + std::to_string(initValues.has_value() ? 1 : 0));
            }
            if (!initValues)
            {
                return std::nullopt;
            }
            std::vector<RegularWriteFamily> regulars;
            std::vector<WritePortInfo> leftoverWrites;
            std::optional<ResetWriteFamily> reset;
            stageStart = ProfileClock::now();
            auto consolidatedMatch = matchConsolidatedWriteFamilies(graph, group, writesByReg, groupProfile);
            const int64_t consolidatedMatchMs = elapsedMs(stageStart);
            if (profile != nullptr)
            {
                profile->regularWriteMatchMs += consolidatedMatchMs;
            }
            if (consolidatedMatch)
            {
                regulars = std::move(consolidatedMatch->regulars);
                reset = std::move(consolidatedMatch->reset);
            }
            else
            {
                stageStart = ProfileClock::now();
                auto regularMatch = matchRegularWriteFamily(graph, group, writesByReg, leftoverWrites, groupProfile);
                const int64_t regularMatchMs = elapsedMs(stageStart);
                if (profile != nullptr)
                {
                    profile->regularWriteMatchMs += regularMatchMs;
                }
                if (groupProfile != nullptr && groupProfile->verbose)
                {
                    profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                               " group=" + std::to_string(groupProfile->groupIndex) +
                               "/" + std::to_string(groupProfile->groupCount) +
                               " stage=regular_write_match_outer_done ms=" + std::to_string(regularMatchMs) +
                               " ok=" + std::to_string(regularMatch.has_value() ? 1 : 0) +
                               " leftover=" + std::to_string(leftoverWrites.size()));
                }
                if (!regularMatch)
                {
                    return std::nullopt;
                }
                if (regularMatch->splitReset)
                {
                    if (!leftoverWrites.empty())
                    {
                        return std::nullopt;
                    }
                    reset = std::move(regularMatch->splitReset);
                }
                regulars.push_back(std::move(regularMatch->family));
            }
            if (regulars.empty())
            {
                return std::nullopt;
            }
            if (!reset && !leftoverWrites.empty())
            {
                stageStart = ProfileClock::now();
                reset = matchResetWriteFamily(graph, group, leftoverWrites);
                const int64_t resetMs = elapsedMs(stageStart);
                if (profile != nullptr)
                {
                    profile->resetWriteMatchMs += resetMs;
                }
                if (groupProfile != nullptr && groupProfile->verbose)
                {
                    profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                               " group=" + std::to_string(groupProfile->groupIndex) +
                               "/" + std::to_string(groupProfile->groupCount) +
                               " stage=reset_write_match_done ms=" + std::to_string(resetMs) +
                               " ok=" + std::to_string(reset.has_value() ? 1 : 0));
                }
                if (!reset)
                {
                    return std::nullopt;
                }
                for (const RegularWriteFamily &regular : regulars)
                {
                    if (valueVectorsEqual(reset->events, regular.events) &&
                        reset->eventEdges == regular.eventEdges)
                    {
                        return std::nullopt;
                    }
                }
            }
            stageStart = ProfileClock::now();
            TrueMergeCandidate candidate;
            candidate.regulars = std::move(regulars);
            candidate.reset = std::move(reset);
            candidate.initValues = std::move(*initValues);
            candidate.regOps.reserve(group.regSymbols.size());
            candidate.readOpsByRow.reserve(group.regSymbols.size());
            for (const std::string &regSymbol : group.regSymbols)
            {
                const OperationId regOp = graph.findOperation(regSymbol);
                if (!regOp.valid())
                {
                    return std::nullopt;
                }
                candidate.regOps.push_back(regOp);
                if (group.trueOnlyStorage)
                {
                    const auto readIt = readsByReg.find(regSymbol);
                    if (readIt == readsByReg.end() || readIt->second.empty())
                    {
                        return std::nullopt;
                    }
                    candidate.readOpsByRow.push_back(readIt->second);
                }
            }

            std::unordered_set<OperationId, OperationIdHash> selectedWriteOps;
            for (const RegularWriteFamily &regular : candidate.regulars)
            {
                for (const WritePortInfo &write : regular.writes)
                {
                    selectedWriteOps.insert(write.op);
                }
            }
            if (candidate.reset)
            {
                for (const WritePortInfo &write : candidate.reset->writes)
                {
                    selectedWriteOps.insert(write.op);
                }
            }
            for (const std::string &regSymbol : group.regSymbols)
            {
                const auto writeIt = writesByReg.find(regSymbol);
                if (writeIt == writesByReg.end())
                {
                    return std::nullopt;
                }
                for (const WritePortInfo &write : writeIt->second)
                {
                    if (!selectedWriteOps.contains(write.op))
                    {
                        return std::nullopt;
                    }
                }
            }
            if (profile != nullptr)
            {
                profile->finalizeTrueMatchMs += elapsedMs(stageStart);
            }
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=true_match_done ms=" + std::to_string(elapsedMs(stageStart)));
            }
            return candidate;
        }

        std::string makeUniqueMemoryName(const Graph &graph, const std::vector<std::string> &regSymbols)
        {
            std::string base = "rtm_mem";
            if (!regSymbols.empty())
            {
                base += "$";
                base += Graph::normalizeComponent(regSymbols.front());
            }
            std::string candidate = base;
            int64_t suffix = 0;
            while (graph.findOperation(candidate).valid() || graph.findValue(candidate).valid())
            {
                ++suffix;
                candidate = base + "$" + std::to_string(suffix);
            }
            return candidate;
        }

        ValueId buildInDomainGuard(Graph &graph, ValueId addr, std::size_t rowCount)
        {
            if (rowCount == 0)
            {
                return createConstantValue(graph, 1, false, "1'b0", "empty_in_domain");
            }
            const int32_t addrWidth = graph.valueWidth(addr);
            if (addrWidth > 0)
            {
                if (addrWidth < 64 &&
                    rowCount >= (UINT64_C(1) << static_cast<uint32_t>(addrWidth)))
                {
                    return createConstantValue(graph, 1, false, "1'b1", "full_addr_domain");
                }
                const ValueId limit = createConstantValue(graph,
                                                           addrWidth,
                                                           false,
                                                           makeIntLiteral(addrWidth, rowCount),
                                                           "write_domain_limit");
                return createBinaryOp(graph,
                                      OperationKind::kLt,
                                      addr,
                                      limit,
                                      1,
                                      false,
                                      "write_in_domain_lt");
            }
            std::vector<ValueId> hits;
            hits.reserve(rowCount);
            for (std::size_t row = 0; row < rowCount; ++row)
            {
                const ValueId rowConst = createConstantValue(graph,
                                                             graph.valueWidth(addr),
                                                             graph.valueSigned(addr),
                                                             makeIntLiteral(graph.valueWidth(addr), row),
                                                             "write_row_const");
                hits.push_back(createBinaryOp(graph, OperationKind::kEq, addr, rowConst, 1, false, "write_in_domain_eq"));
            }
            return createOrChain(graph, hits, "write_in_domain_or");
        }

        bool rewriteTrueMerge(Graph &graph,
                              const GroupCandidate &group,
                              const TrueMergeCandidate &candidate,
                              RegToMemProfile *profile = nullptr,
                              const GroupProfileContext *groupProfile = nullptr)
        {
            const auto rewriteStart = ProfileClock::now();
            auto stageStart = rewriteStart;
            auto finishRewriteStage = [&](std::string_view stage,
                                          ProfileClock::time_point start,
                                          int64_t RegToMemProfile::*profileField = nullptr,
                                          const std::string &extra = "") {
                const int64_t ms = elapsedMs(start);
                if (profile != nullptr && profileField != nullptr)
                {
                    profile->*profileField += ms;
                }
                if (groupProfile == nullptr || !groupProfile->verbose)
                {
                    return;
                }
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=rewrite_" + std::string(stage) +
                           " ms=" + std::to_string(ms) +
                           (extra.empty() ? "" : " " + extra));
            };
            if (groupProfile != nullptr && groupProfile->verbose)
            {
                profileLog("group_detail graph_index=" + std::to_string(groupProfile->graphIndex) +
                           " group=" + std::to_string(groupProfile->groupIndex) +
                           "/" + std::to_string(groupProfile->groupCount) +
                           " stage=rewrite_start anchors=" + std::to_string(group.anchors.size()) +
                           " members=" + std::to_string(group.regSymbols.size()) +
                           " write_families=" + std::to_string(candidate.regulars.size()) +
                           " reset=" + std::to_string(candidate.reset.has_value() ? 1 : 0) +
                           " packed_reset=" + std::to_string(candidate.reset && candidate.reset->packed ? 1 : 0));
            }

            const std::string memSymbol = makeUniqueMemoryName(graph, group.regSymbols);
            const Operation firstReg = graph.getOperation(candidate.regOps.front());
            const bool isSigned = getAttr<bool>(firstReg, "isSigned").value_or(false);

            const OperationId memOp = graph.createOperation(OperationKind::kMemory, graph.internSymbol(memSymbol));
            graph.setAttr(memOp, "width", static_cast<int64_t>(group.elementWidth));
            graph.setAttr(memOp, "row", static_cast<int64_t>(group.elementCount));
            graph.setAttr(memOp, "isSigned", isSigned);
            std::vector<std::string> initKinds;
            std::vector<std::string> initFiles;
            std::vector<std::string> initValues;
            std::vector<int64_t> initStarts;
            std::vector<int64_t> initLens;
            for (std::size_t row = 0; row < candidate.initValues.size(); ++row)
            {
                if (!candidate.initValues[row])
                {
                    continue;
                }
                initKinds.push_back("literal");
                initFiles.emplace_back();
                initValues.push_back(*candidate.initValues[row]);
                initStarts.push_back(static_cast<int64_t>(row));
                initLens.push_back(1);
            }
            if (!initKinds.empty())
            {
                const std::size_t initRowCount = initKinds.size();
                graph.setAttr(memOp, "initKind", std::move(initKinds));
                graph.setAttr(memOp, "initFile", std::move(initFiles));
                graph.setAttr(memOp, "initValue", std::move(initValues));
                graph.setAttr(memOp, "initStart", std::move(initStarts));
                graph.setAttr(memOp, "initLen", std::move(initLens));
                graph.setOpSrcLoc(memOp, makeTransformSrcLoc(std::string(kPassId), "true_memory"));
                finishRewriteStage("memory_done", stageStart, &RegToMemProfile::rewriteMemoryMs,
                                   "init_rows=" + std::to_string(initRowCount));
            }
            else
            {
                graph.setOpSrcLoc(memOp, makeTransformSrcLoc(std::string(kPassId), "true_memory"));
                finishRewriteStage("memory_done", stageStart, &RegToMemProfile::rewriteMemoryMs,
                                   "init_rows=0");
            }

            stageStart = ProfileClock::now();
            std::size_t outputRebinds = 0;
            std::size_t storageReadReplacements = 0;
            if (group.trueOnlyStorage)
            {
                if (candidate.readOpsByRow.size() != group.elementCount)
                {
                    return false;
                }
                int32_t addrWidth = 1;
                std::size_t addressSpace = 2;
                while (addressSpace < group.elementCount)
                {
                    ++addrWidth;
                    addressSpace <<= 1;
                }
                for (std::size_t row = 0; row < candidate.readOpsByRow.size(); ++row)
                {
                    if (candidate.readOpsByRow[row].empty())
                    {
                        return false;
                    }
                    const Operation firstRead = graph.getOperation(candidate.readOpsByRow[row].front());
                    if (firstRead.results().size() != 1)
                    {
                        return false;
                    }
                    const ValueId rowConst = createConstantValue(graph,
                                                                 addrWidth,
                                                                 false,
                                                                 makeIntLiteral(addrWidth, row),
                                                                 "true_storage_read_row");
                    const ValueId readResult = graph.createValue(
                        graph.makeInternalValSym(),
                        group.elementWidth,
                        graph.valueSigned(firstRead.results().front()),
                        graph.valueType(firstRead.results().front()));
                    const OperationId readOp = graph.createOperation(OperationKind::kMemoryReadPort,
                                                                     graph.makeInternalOpSym());
                    graph.addOperand(readOp, rowConst);
                    graph.addResult(readOp, readResult);
                    graph.setAttr(readOp, "memSymbol", memSymbol);
                    const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), "true_storage_read");
                    graph.setOpSrcLoc(readOp, srcLoc);
                    graph.setValueSrcLoc(readResult, srcLoc);
                    for (OperationId oldReadOpId : candidate.readOpsByRow[row])
                    {
                        const Operation oldRead = graph.getOperation(oldReadOpId);
                        if (oldRead.results().size() != 1 ||
                            graph.valueSigned(oldRead.results().front()) != graph.valueSigned(readResult) ||
                            graph.valueType(oldRead.results().front()) != graph.valueType(readResult))
                        {
                            return false;
                        }
                        std::vector<std::string> outputPortsToRebind;
                        for (const auto &port : graph.outputPorts())
                        {
                            if (port.value == oldRead.results().front())
                            {
                                outputPortsToRebind.push_back(port.name);
                            }
                        }
                        for (const std::string &portName : outputPortsToRebind)
                        {
                            graph.bindOutputPort(portName, readResult);
                            ++outputRebinds;
                        }
                        if (!graph.eraseOp(oldReadOpId, std::array<ValueId, 1>{readResult}))
                        {
                            return false;
                        }
                        ++storageReadReplacements;
                    }
                }
            }
            else
            {
                for (const AnchorCandidate &anchor : group.anchors)
                {
                    const Operation sliceOp = graph.getOperation(anchor.sliceOp);
                    if (sliceOp.results().size() != 1)
                    {
                        return false;
                    }
                    const ValueId readResult = graph.createValue(graph.makeInternalValSym(),
                                                                 group.elementWidth,
                                                                 graph.valueSigned(sliceOp.results().front()),
                                                                 graph.valueType(sliceOp.results().front()));
                    const OperationId readOp = graph.createOperation(OperationKind::kMemoryReadPort,
                                                                     graph.makeInternalOpSym());
                    graph.addOperand(readOp, anchor.indexValue);
                    graph.addResult(readOp, readResult);
                    graph.setAttr(readOp, "memSymbol", memSymbol);
                    const auto srcLoc = makeTransformSrcLoc(std::string(kPassId), "true_read");
                    graph.setOpSrcLoc(readOp, srcLoc);
                    graph.setValueSrcLoc(readResult, srcLoc);
                    std::vector<std::string> outputPortsToRebind;
                    for (const auto &port : graph.outputPorts())
                    {
                        if (port.value == sliceOp.results().front())
                        {
                            outputPortsToRebind.push_back(port.name);
                        }
                    }
                    for (const std::string &portName : outputPortsToRebind)
                    {
                        graph.bindOutputPort(portName, readResult);
                        ++outputRebinds;
                    }
                    if (!graph.eraseOp(anchor.sliceOp, std::array<ValueId, 1>{readResult}))
                    {
                        return false;
                    }
                }
            }
            finishRewriteStage("read_replacement_done",
                               stageStart,
                               &RegToMemProfile::rewriteReadReplacementMs,
                               "anchors=" + std::to_string(group.anchors.size()) +
                                   " storage_reads=" + std::to_string(storageReadReplacements) +
                                   " output_rebinds=" + std::to_string(outputRebinds));

            stageStart = ProfileClock::now();
            std::unordered_set<OperationId, OperationIdHash> erasedOps;
            if (!group.trueOnlyStorage)
            {
                for (const AnchorCandidate &anchor : group.anchors)
                {
                    erasedOps.insert(anchor.sliceOp);
                }
            }
            auto eraseOpOnce = [&](OperationId op) -> bool {
                if (!op.valid())
                {
                    return true;
                }
                if (!erasedOps.insert(op).second)
                {
                    return true;
                }
                return graph.eraseOp(op);
            };
            std::size_t erasedConcatOps = 0;
            std::size_t erasedReadOps = 0;
            if (!group.trueOnlyStorage)
            {
                for (const AnchorCandidate &anchor : group.anchors)
                {
                    if (!eraseOpOnce(anchor.concatOp))
                    {
                        return false;
                    }
                    ++erasedConcatOps;
                    for (OperationId readOp : anchor.readOps)
                    {
                        if (!eraseOpOnce(readOp))
                        {
                            return false;
                        }
                        ++erasedReadOps;
                    }
                }
            }
            finishRewriteStage("erase_read_closure_done",
                               stageStart,
                               &RegToMemProfile::rewriteEraseReadClosureMs,
                               "concat_ops=" + std::to_string(erasedConcatOps) +
                                   " read_ops=" + std::to_string(erasedReadOps));

            stageStart = ProfileClock::now();
            if (candidate.reset)
            {
                ValueId fillData = candidate.reset->data;
                if (candidate.reset->packed)
                {
                    std::vector<ValueId> concatOperands;
                    concatOperands.reserve(candidate.reset->rowData.size());
                    for (auto it = candidate.reset->rowData.rbegin(); it != candidate.reset->rowData.rend(); ++it)
                    {
                        concatOperands.push_back(*it);
                    }
                    fillData = createConcat(graph,
                                            concatOperands,
                                            static_cast<int32_t>(group.elementWidth * group.elementCount),
                                            false,
                                            "true_reset_packed");
                }
                ValueId fillGuard = candidate.reset->guard;
                if (candidate.reset->guardTerms.size() > 1)
                {
                    fillGuard = createOrChain(graph, candidate.reset->guardTerms, "true_reset_guard_or");
                }
                if (!isAllOnesMask(graph, candidate.reset->mask, group.elementWidth))
                {
                    if (group.elementWidth != 1)
                    {
                        return false;
                    }
                    const std::array<ValueId, 2> fillGuardTerms = {fillGuard, candidate.reset->mask};
                    fillGuard = createAndChain(graph, fillGuardTerms, "true_reset_mask_guard");
                }
                const OperationId fillOp = graph.createOperation(OperationKind::kMemoryFillPort,
                                                                 graph.makeInternalOpSym());
                graph.addOperand(fillOp, fillGuard);
                graph.addOperand(fillOp, fillData);
                for (ValueId event : candidate.reset->events)
                {
                    graph.addOperand(fillOp, event);
                }
                graph.setAttr(fillOp, "memSymbol", memSymbol);
                graph.setAttr(fillOp, "eventEdge", candidate.reset->eventEdges);
                graph.setOpSrcLoc(fillOp, makeTransformSrcLoc(std::string(kPassId), "true_fill"));
            }
            finishRewriteStage("fill_done",
                               stageStart,
                               &RegToMemProfile::rewriteFillMs,
                               "reset=" + std::to_string(candidate.reset.has_value() ? 1 : 0));

            stageStart = ProfileClock::now();
            std::size_t emittedWritePorts = 0;
            for (const RegularWriteFamily &regular : candidate.regulars)
            {
                std::vector<ValueId> writeGuardTerms = regular.commonTerms;
                for (ValueId conflictAddr : regular.conflictAddrs)
                {
                    const ValueId conflict = createBinaryOp(graph,
                                                            OperationKind::kEq,
                                                            conflictAddr,
                                                            regular.addr,
                                                            1,
                                                            false,
                                                            "true_write_conflict_eq");
                    writeGuardTerms.push_back(createUnaryOp(graph,
                                                            OperationKind::kLogicNot,
                                                            conflict,
                                                            1,
                                                            false,
                                                            "true_write_conflict_not"));
                }
                writeGuardTerms.push_back(buildInDomainGuard(graph, regular.addr, group.elementCount));
                const ValueId writeGuard = createAndChain(graph, writeGuardTerms, "true_write_guard");
                const OperationId writeOp = graph.createOperation(OperationKind::kMemoryWritePort,
                                                                  graph.makeInternalOpSym());
                graph.addOperand(writeOp, writeGuard);
                graph.addOperand(writeOp, regular.addr);
                graph.addOperand(writeOp, regular.data);
                graph.addOperand(writeOp, regular.mask);
                for (ValueId event : regular.events)
                {
                    graph.addOperand(writeOp, event);
                }
                graph.setAttr(writeOp, "memSymbol", memSymbol);
                graph.setAttr(writeOp, "eventEdge", regular.eventEdges);
                graph.setOpSrcLoc(writeOp, makeTransformSrcLoc(std::string(kPassId), "true_write"));
                ++emittedWritePorts;
            }
            finishRewriteStage("write_ports_done",
                               stageStart,
                               &RegToMemProfile::rewriteWritePortMs,
                               "ports=" + std::to_string(emittedWritePorts));

            stageStart = ProfileClock::now();
            std::size_t erasedRegularWrites = 0;
            for (const RegularWriteFamily &regular : candidate.regulars)
            {
                for (const WritePortInfo &write : regular.writes)
                {
                    if (!eraseOpOnce(write.op))
                    {
                        return false;
                    }
                    ++erasedRegularWrites;
                }
            }
            std::size_t erasedResetWrites = 0;
            if (candidate.reset)
            {
                for (const WritePortInfo &write : candidate.reset->writes)
                {
                    if (!eraseOpOnce(write.op))
                    {
                        return false;
                    }
                    ++erasedResetWrites;
                }
            }
            finishRewriteStage("erase_writes_done",
                               stageStart,
                               &RegToMemProfile::rewriteEraseWritesMs,
                               "regular_writes=" + std::to_string(erasedRegularWrites) +
                                   " reset_writes=" + std::to_string(erasedResetWrites) +
                                   " unique_erased=" + std::to_string(erasedOps.size()));

            stageStart = ProfileClock::now();
            std::size_t erasedRegs = 0;
            for (OperationId regOp : candidate.regOps)
            {
                if (!eraseOpOnce(regOp))
                {
                    return false;
                }
                ++erasedRegs;
            }
            finishRewriteStage("erase_regs_done",
                               stageStart,
                               &RegToMemProfile::rewriteEraseRegsMs,
                               "regs=" + std::to_string(erasedRegs) +
                                   " unique_erased=" + std::to_string(erasedOps.size()));
            finishRewriteStage("done", rewriteStart);
            return true;
        }

        void annotateGroup(Graph &graph, const GroupCandidate &group, std::size_t fallbackGroupIndex)
        {
            const std::string groupName = group.intentGroupName.empty()
                                              ? "rtm_intent_" + std::to_string(fallbackGroupIndex)
                                              : group.intentGroupName;
            const std::string storageGroupName = group.storageGroupName.empty()
                                                     ? groupName
                                                     : group.storageGroupName;
            const std::vector<std::string> regSymbols = group.regSymbols;
            const std::vector<std::string> storageRegSymbols =
                group.storageRegSymbols.empty() ? group.regSymbols : group.storageRegSymbols;
            const std::size_t storageElementCount =
                group.storageElementCount == 0 ? group.elementCount : group.storageElementCount;
            std::vector<int64_t> rows;
            rows.reserve(group.elementCount);
            for (std::size_t i = 0; i < group.elementCount; ++i)
            {
                rows.push_back(static_cast<int64_t>(i));
            }

            for (const auto &anchor : group.anchors)
            {
                graph.setAttr(anchor.concatOp, "regToMem.intent.version", int64_t{1});
                graph.setAttr(anchor.concatOp, "regToMem.intent.group", groupName);
                graph.setAttr(anchor.concatOp, "regToMem.intent.role", std::string("concat"));
                graph.setAttr(anchor.concatOp, "regToMem.intent.mode", std::string("array-index"));
                graph.setAttr(anchor.concatOp, "regToMem.intent.elementWidth", static_cast<int64_t>(group.elementWidth));
                graph.setAttr(anchor.concatOp, "regToMem.intent.elementCount", static_cast<int64_t>(group.elementCount));
                graph.setAttr(anchor.concatOp, "regToMem.intent.regSymbols", regSymbols);
                graph.setAttr(anchor.concatOp, "regToMem.intent.operandRows", anchor.operandRows);
                graph.setAttr(anchor.concatOp, "regToMem.intent.storageGroup", storageGroupName);
                graph.setAttr(anchor.concatOp, "regToMem.intent.storageElementCount",
                              static_cast<int64_t>(storageElementCount));
                graph.setAttr(anchor.concatOp, "regToMem.intent.storageRegSymbols", storageRegSymbols);
                graph.setAttr(anchor.concatOp, "regToMem.intent.storageRowOffset",
                              static_cast<int64_t>(group.storageRowOffset));

                graph.setAttr(anchor.sliceOp, "regToMem.intent.version", int64_t{1});
                graph.setAttr(anchor.sliceOp, "regToMem.intent.group", groupName);
                graph.setAttr(anchor.sliceOp, "regToMem.intent.role", std::string("slice"));
                graph.setAttr(anchor.sliceOp, "regToMem.intent.mode", std::string("array-index"));
                graph.setAttr(anchor.sliceOp, "regToMem.intent.sliceKind", anchor.sliceKind);
                graph.setAttr(anchor.sliceOp, "regToMem.intent.elementWidth", static_cast<int64_t>(group.elementWidth));
                graph.setAttr(anchor.sliceOp, "regToMem.intent.elementCount", static_cast<int64_t>(group.elementCount));
                graph.setAttr(anchor.sliceOp, "regToMem.intent.storageGroup", storageGroupName);
                graph.setAttr(anchor.sliceOp, "regToMem.intent.storageElementCount",
                              static_cast<int64_t>(storageElementCount));
                graph.setAttr(anchor.sliceOp, "regToMem.intent.storageRowOffset",
                              static_cast<int64_t>(group.storageRowOffset));

                for (std::size_t i = 0; i < anchor.readOps.size(); ++i)
                {
                    const OperationId readOp = anchor.readOps[i];
                    const int64_t row = anchor.operandRows[i];
                    graph.setAttr(readOp, "regToMem.intent.version", int64_t{1});
                    graph.setAttr(readOp, "regToMem.intent.group", groupName);
                    graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
                    graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
                    graph.setAttr(readOp, "regToMem.intent.row", row);
                    graph.setAttr(readOp, "regToMem.intent.storageGroup", storageGroupName);
                    graph.setAttr(readOp, "regToMem.intent.storageRow", row + static_cast<int64_t>(group.storageRowOffset));
                }

                for (std::size_t row = 0; row < storageRegSymbols.size(); ++row)
                {
                    const OperationId regOp = graph.findOperation(storageRegSymbols[row]);
                    if (!regOp.valid())
                    {
                        continue;
                    }
                    graph.setAttr(regOp, "regToMem.intent.version", int64_t{1});
                    graph.setAttr(regOp, "regToMem.intent.group", storageGroupName);
                    graph.setAttr(regOp, "regToMem.intent.role", std::string("register"));
                    graph.setAttr(regOp, "regToMem.intent.mode", std::string("array-index"));
                    graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));
                    graph.setAttr(regOp, "regToMem.intent.elementWidth", static_cast<int64_t>(group.elementWidth));
                    graph.setAttr(regOp, "regToMem.intent.elementCount", static_cast<int64_t>(storageElementCount));
                    graph.setAttr(regOp, "regToMem.intent.storageGroup", storageGroupName);
                    graph.setAttr(regOp, "regToMem.intent.storageElementCount",
                                  static_cast<int64_t>(storageElementCount));
                }
            }
        }
    } // namespace

    RegToMemPass::RegToMemPass()
        : RegToMemPass(RegToMemOptions{})
    {
    }

    RegToMemPass::RegToMemPass(RegToMemOptions options)
        : Pass("reg-to-mem", "Reg-to-Mem", "Recover scalarized register arrays for GrhSIM"),
          options_(std::move(options))
    {
    }

    PassResult RegToMemPass::run()
    {
        PassResult result;

        RegToMemStats stats;
        RegToMemProfile profile;
        const auto passStart = ProfileClock::now();
        std::size_t graphIndex = 0;
        for (const auto &entry : design().graphs())
        {
            auto &graph = *entry.second;
            ++stats.graphs;
            ++graphIndex;
            const auto graphStart = ProfileClock::now();
            const std::size_t opCount = graph.operations().size();
            const std::size_t valueCount = graph.values().size();
            profileLog("graph_start index=" + std::to_string(graphIndex) +
                       " symbol=" + graph.symbol() +
                       " ops=" + std::to_string(opCount) +
                       " values=" + std::to_string(valueCount));

            auto stageStart = ProfileClock::now();
            const auto valueUses = buildValueUseIndex(graph);
            const int64_t buildUsesMs = elapsedMs(stageStart);
            profile.buildUsesMs += buildUsesMs;
            profileLog("graph_stage index=" + std::to_string(graphIndex) +
                       " stage=build_value_uses ms=" + std::to_string(buildUsesMs) +
                       " use_values=" + std::to_string(valueUses.size()));

            stageStart = ProfileClock::now();
            auto anchors = discoverIntentAnchors(graph, valueUses, options_.minElementCount);
            auto trueOnlyAnchors = options_.enableTrueMerge
                                       ? discoverTrueOnlyStorageAnchors(graph, valueUses, options_.minElementCount)
                                       : std::vector<AnchorCandidate>{};
            const int64_t discoverAnchorsMs = elapsedMs(stageStart);
            profile.discoverAnchorsMs += discoverAnchorsMs;
            profileLog("graph_stage index=" + std::to_string(graphIndex) +
                       " stage=discover_intent_anchors ms=" + std::to_string(discoverAnchorsMs) +
                       " intent_anchors=" + std::to_string(anchors.size()) +
                       " true_only_anchors=" + std::to_string(trueOnlyAnchors.size()));
            if (anchors.empty() && trueOnlyAnchors.empty())
            {
                profileLog("graph_done index=" + std::to_string(graphIndex) +
                           " total_ms=" + std::to_string(elapsedMs(graphStart)) +
                           " groups=0");
                continue;
            }

            stageStart = ProfileClock::now();
            auto groupedAnchors = groupAnchors(std::move(anchors));
            stats.intentCandidateGroups += groupedAnchors.candidateGroups;
            stats.intentCandidateAnchors += groupedAnchors.candidateAnchors;
            stats.intentCandidateMembers += groupedAnchors.candidateMembers;
            stats.intentConflictGroups += groupedAnchors.conflictGroups;
            stats.intentConflictAnchors += groupedAnchors.conflictAnchors;
            stats.intentConflictMembers += groupedAnchors.conflictMembers;
            auto groups = std::move(groupedAnchors.groups);
            auto trueOnlyGroups = groupTrueOnlyStorageAnchors(std::move(trueOnlyAnchors));
            std::unordered_set<std::string> intentGroupRegs;
            for (const GroupCandidate &group : groups)
            {
                intentGroupRegs.insert(group.regSymbols.begin(), group.regSymbols.end());
            }
            std::size_t acceptedTrueOnlyGroups = 0;
            for (GroupCandidate &trueOnlyGroup : trueOnlyGroups)
            {
                if (std::any_of(trueOnlyGroup.regSymbols.begin(),
                                trueOnlyGroup.regSymbols.end(),
                                [&](const std::string &symbol) { return intentGroupRegs.contains(symbol); }))
                {
                    continue;
                }
                groups.push_back(std::move(trueOnlyGroup));
                ++acceptedTrueOnlyGroups;
            }
            const int64_t groupAnchorsMs = elapsedMs(stageStart);
            profile.groupAnchorsMs += groupAnchorsMs;
            profileLog("graph_stage index=" + std::to_string(graphIndex) +
                       " stage=group_anchors ms=" + std::to_string(groupAnchorsMs) +
                       " candidate_groups=" + std::to_string(groupedAnchors.candidateGroups) +
                       " groups=" + std::to_string(groups.size()) +
                       " true_only_groups=" + std::to_string(acceptedTrueOnlyGroups) +
                       " conflict_groups=" + std::to_string(groupedAnchors.conflictGroups) +
                       " conflict_anchors=" + std::to_string(groupedAnchors.conflictAnchors));

            stageStart = ProfileClock::now();
            const auto readsByReg = options_.enableTrueMerge
                                        ? buildRegisterReadIndex(graph)
                                        : RegisterReadIndex{};
            const int64_t buildReadIndexMs = elapsedMs(stageStart);
            profile.buildReadIndexMs += buildReadIndexMs;
            profileLog("graph_stage index=" + std::to_string(graphIndex) +
                       " stage=build_read_index ms=" + std::to_string(buildReadIndexMs) +
                       " regs_with_reads=" + std::to_string(readsByReg.size()));

            stageStart = ProfileClock::now();
            const auto writesByReg = options_.enableTrueMerge
                                         ? collectRegisterWrites(graph)
                                         : std::unordered_map<std::string, std::vector<WritePortInfo>>{};
            const int64_t collectWritesMs = elapsedMs(stageStart);
            profile.collectWritesMs += collectWritesMs;
            profileLog("graph_stage index=" + std::to_string(graphIndex) +
                       " stage=collect_writes ms=" + std::to_string(collectWritesMs) +
                       " regs_with_writes=" + std::to_string(writesByReg.size()));

            std::size_t localGroupIndex = 0;
            std::size_t visitedGroups = 0;
            std::size_t graphTrueGroups = 0;
            std::size_t graphIntentGroups = 0;
            const std::size_t graphIntentCandidateGroups = groupedAnchors.candidateGroups;
            const std::size_t graphIntentConflictGroups = groupedAnchors.conflictGroups;
            std::size_t graphSkippedTrue = 0;
            for (auto &group : groups)
            {
                ++visitedGroups;
                if (group.anchors.empty())
                {
                    continue;
                }
                const bool verboseGroup = visitedGroups <= 20 || visitedGroups % 100 == 0 ||
                                          group.regSymbols.size() >= 500;
                GroupProfileContext groupProfile{
                    .graphIndex = graphIndex,
                    .groupIndex = visitedGroups,
                    .groupCount = groups.size(),
                    .verbose = verboseGroup,
                };
                if (verboseGroup)
                {
                    profileLog("group_progress graph_index=" + std::to_string(graphIndex) +
                               " group=" + std::to_string(visitedGroups) +
                               "/" + std::to_string(groups.size()) +
                               " anchors=" + std::to_string(group.anchors.size()) +
                               " members=" + std::to_string(group.regSymbols.size()) +
                               " element_width=" + std::to_string(group.elementWidth) +
                               " first_reg=" + group.regSymbols.front() +
                               " last_reg=" + group.regSymbols.back());
                }
                bool trueMerged = false;
                if (options_.enableTrueMerge && !group.sharesStorage && !group.overlapsOtherCandidate)
                {
                    auto trueCandidate = matchTrueMerge(graph,
                                                        valueUses,
                                                        readsByReg,
                                                        group,
                                                        writesByReg,
                                                        &profile,
                                                        &groupProfile);
                    if (trueCandidate)
                    {
                        stageStart = ProfileClock::now();
                        if (verboseGroup)
                        {
                            profileLog("group_progress graph_index=" + std::to_string(graphIndex) +
                                       " group=" + std::to_string(visitedGroups) +
                                       "/" + std::to_string(groups.size()) +
                                       " stage=rewrite_true_start anchors=" + std::to_string(group.anchors.size()) +
                                       " members=" + std::to_string(group.regSymbols.size()));
                        }
                        const bool rewriteOk = rewriteTrueMerge(graph, group, *trueCandidate, &profile, &groupProfile);
                        const int64_t rewriteMs = elapsedMs(stageStart);
                        profile.rewriteTrueMs += rewriteMs;
                        if (verboseGroup)
                        {
                            profileLog("group_progress graph_index=" + std::to_string(graphIndex) +
                                       " group=" + std::to_string(visitedGroups) +
                                       "/" + std::to_string(groups.size()) +
                                       " stage=rewrite_true_done ms=" + std::to_string(rewriteMs) +
                                       " ok=" + std::to_string(rewriteOk ? 1 : 0));
                        }
                        if (rewriteOk)
                        {
                            trueMerged = true;
                            ++stats.trueGroups;
                            ++graphTrueGroups;
                            stats.trueAnchors += group.anchors.size();
                            stats.trueMembers += group.regSymbols.size();
                            result.changed = true;
                        }
                        else
                        {
                            ++stats.skippedTrueCandidates;
                            ++graphSkippedTrue;
                        }
                    }
                    else
                    {
                        ++stats.skippedTrueCandidates;
                        ++graphSkippedTrue;
                    }
                }
                else if (options_.enableTrueMerge && (group.sharesStorage || group.overlapsOtherCandidate))
                {
                    ++stats.skippedTrueCandidates;
                    ++graphSkippedTrue;
                }
                if (trueMerged)
                {
                    continue;
                }
                if (group.trueOnlyStorage)
                {
                    continue;
                }
                if (options_.enableIntent)
                {
                    stageStart = ProfileClock::now();
                    annotateGroup(graph, group, localGroupIndex++);
                    profile.annotateMs += elapsedMs(stageStart);
                    stats.intentAnchors += group.anchors.size();
                    ++stats.intentGroups;
                    ++graphIntentGroups;
                    stats.intentMembers += group.regSymbols.size();
                    result.changed = true;
                }
            }
            profileLog("graph_done index=" + std::to_string(graphIndex) +
                       " total_ms=" + std::to_string(elapsedMs(graphStart)) +
                       " groups=" + std::to_string(groups.size()) +
                       " visited_groups=" + std::to_string(visitedGroups) +
                       " intent_candidate_groups=" + std::to_string(graphIntentCandidateGroups) +
                       " intent_conflict_groups=" + std::to_string(graphIntentConflictGroups) +
                       " true_groups=" + std::to_string(graphTrueGroups) +
                       " true_skipped=" + std::to_string(graphSkippedTrue) +
                       " intent_groups=" + std::to_string(graphIntentGroups));
        }

        profileLog("summary total_ms=" + std::to_string(elapsedMs(passStart)) +
                   " graphs=" + std::to_string(stats.graphs) +
                   " build_uses_ms=" + std::to_string(profile.buildUsesMs) +
                   " discover_anchors_ms=" + std::to_string(profile.discoverAnchorsMs) +
                   " group_anchors_ms=" + std::to_string(profile.groupAnchorsMs) +
                   " build_read_index_ms=" + std::to_string(profile.buildReadIndexMs) +
                   " collect_writes_ms=" + std::to_string(profile.collectWritesMs) +
                   " true_closure_ms=" + std::to_string(profile.trueClosureMs) +
                   " collect_inits_ms=" + std::to_string(profile.collectInitsMs) +
                   " regular_write_match_ms=" + std::to_string(profile.regularWriteMatchMs) +
                   " reset_write_match_ms=" + std::to_string(profile.resetWriteMatchMs) +
                   " finalize_true_match_ms=" + std::to_string(profile.finalizeTrueMatchMs) +
                   " rewrite_true_ms=" + std::to_string(profile.rewriteTrueMs) +
                   " rewrite_memory_ms=" + std::to_string(profile.rewriteMemoryMs) +
                   " rewrite_read_replacement_ms=" + std::to_string(profile.rewriteReadReplacementMs) +
                   " rewrite_erase_read_closure_ms=" + std::to_string(profile.rewriteEraseReadClosureMs) +
                   " rewrite_fill_ms=" + std::to_string(profile.rewriteFillMs) +
                   " rewrite_domain_guard_ms=" + std::to_string(profile.rewriteDomainGuardMs) +
                   " rewrite_write_port_ms=" + std::to_string(profile.rewriteWritePortMs) +
                   " rewrite_erase_writes_ms=" + std::to_string(profile.rewriteEraseWritesMs) +
                   " rewrite_erase_regs_ms=" + std::to_string(profile.rewriteEraseRegsMs) +
                   " annotate_ms=" + std::to_string(profile.annotateMs));

        diags().info(std::string(kPassId),
                     "reg-to-mem: graphs=" + std::to_string(stats.graphs) +
                         " intent_candidate_groups=" + std::to_string(stats.intentCandidateGroups) +
                         " intent_candidate_anchors=" + std::to_string(stats.intentCandidateAnchors) +
                         " intent_candidate_members=" + std::to_string(stats.intentCandidateMembers) +
                         " intent_conflict_groups=" + std::to_string(stats.intentConflictGroups) +
                         " intent_conflict_anchors=" + std::to_string(stats.intentConflictAnchors) +
                         " intent_conflict_members=" + std::to_string(stats.intentConflictMembers) +
                         " true_groups=" + std::to_string(stats.trueGroups) +
                         " true_anchors=" + std::to_string(stats.trueAnchors) +
                         " true_members=" + std::to_string(stats.trueMembers) +
                         " true_skipped=" + std::to_string(stats.skippedTrueCandidates) +
                         " intent_groups=" + std::to_string(stats.intentGroups) +
                         " intent_anchors=" + std::to_string(stats.intentAnchors) +
                         " intent_members=" + std::to_string(stats.intentMembers));
        return result;
    }

} // namespace wolvrix::lib::transform
