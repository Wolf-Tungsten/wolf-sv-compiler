#include "grhsim/am/grhsim_am_graph_optimize.hpp"

#include "grhsim/am/grhsim_am_program.hpp"
#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim::am {

    namespace {

        constexpr std::string_view kDiagnosticContext = "grhsim.am.optimize";
        constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

        // ------------------------------------------------------------------
        // Bit-vector word helpers. These mirror the evaluation semantics of
        // lib/grhsim/am/interpreter.cpp so folded constants match runtime
        // evaluation bit for bit.
        // ------------------------------------------------------------------

        std::size_t wordCount(uint32_t width) {
            return (static_cast<std::size_t>(width) + 63U) / 64U;
        }

        uint64_t highWordMask(uint32_t width) {
            const uint32_t used = width % 64U;
            return used == 0 ? std::numeric_limits<uint64_t>::max()
                             : (UINT64_C(1) << used) - 1U;
        }

        void normalizeWords(std::vector<uint64_t> &words, uint32_t width) {
            words.resize(wordCount(width), 0);
            if (!words.empty()) {
                words.back() &= highWordMask(width);
            }
        }

        bool getBit(std::span<const uint64_t> words, uint64_t index) {
            const uint64_t word = index / 64U;
            return word < words.size() && ((words[word] >> (index % 64U)) & 1U) != 0;
        }

        void setBit(std::vector<uint64_t> &words, uint64_t index, bool value = true) {
            const uint64_t word = index / 64U;
            if (word >= words.size()) {
                return;
            }
            const uint64_t mask = UINT64_C(1) << (index % 64U);
            if (value) {
                words[word] |= mask;
            } else {
                words[word] &= ~mask;
            }
        }

        bool isZero(std::span<const uint64_t> words) {
            return std::all_of(words.begin(), words.end(),
                               [](uint64_t word) { return word == 0; });
        }

        std::vector<uint64_t> resizedWords(std::span<const uint64_t> words,
                                           uint32_t sourceWidth, uint32_t width,
                                           Signedness extension) {
            std::vector<uint64_t> result(wordCount(width), 0);
            const std::size_t copied = std::min(result.size(), words.size());
            std::copy_n(words.begin(), copied, result.begin());
            if (width > sourceWidth && extension == Signedness::Signed &&
                getBit(words, sourceWidth - 1U)) {
                for (uint64_t bit = sourceWidth; bit < width; ++bit) {
                    setBit(result, bit);
                }
            }
            normalizeWords(result, width);
            return result;
        }

        int compareUnsigned(std::span<const uint64_t> lhs,
                            std::span<const uint64_t> rhs) {
            const std::size_t size = std::max(lhs.size(), rhs.size());
            for (std::size_t offset = 0; offset < size; ++offset) {
                const std::size_t index = size - offset - 1U;
                const uint64_t left = index < lhs.size() ? lhs[index] : 0;
                const uint64_t right = index < rhs.size() ? rhs[index] : 0;
                if (left != right) {
                    return left < right ? -1 : 1;
                }
            }
            return 0;
        }

        std::vector<uint64_t> addWords(std::span<const uint64_t> lhs,
                                       std::span<const uint64_t> rhs, uint32_t width) {
            std::vector<uint64_t> result(wordCount(width), 0);
            unsigned __int128 carry = 0;
            for (std::size_t index = 0; index < result.size(); ++index) {
                const unsigned __int128 sum =
                    static_cast<unsigned __int128>(lhs[index]) +
                    static_cast<unsigned __int128>(rhs[index]) + carry;
                result[index] = static_cast<uint64_t>(sum);
                carry = sum >> 64U;
            }
            normalizeWords(result, width);
            return result;
        }

        std::vector<uint64_t> subtractWords(std::span<const uint64_t> lhs,
                                            std::span<const uint64_t> rhs,
                                            uint32_t width) {
            std::vector<uint64_t> result(wordCount(width), 0);
            uint64_t borrow = 0;
            for (std::size_t index = 0; index < result.size(); ++index) {
                const uint64_t right = rhs[index];
                const uint64_t withBorrow = right + borrow;
                const bool additionOverflow = withBorrow < right;
                const bool nextBorrow = additionOverflow || lhs[index] < withBorrow;
                result[index] = lhs[index] - withBorrow;
                borrow = nextBorrow ? 1U : 0U;
            }
            normalizeWords(result, width);
            return result;
        }

        std::vector<uint64_t> negateWords(std::span<const uint64_t> source,
                                          uint32_t width) {
            std::vector<uint64_t> result(source.begin(), source.end());
            for (uint64_t &word : result) {
                word = ~word;
            }
            uint64_t carry = 1;
            for (uint64_t &word : result) {
                const uint64_t old = word;
                word += carry;
                carry = carry != 0 && word < old ? 1U : 0U;
                if (carry == 0) {
                    break;
                }
            }
            normalizeWords(result, width);
            return result;
        }

        std::vector<uint64_t> multiplyWords(std::span<const uint64_t> lhs,
                                            std::span<const uint64_t> rhs,
                                            uint32_t width) {
            std::vector<uint64_t> result(wordCount(width), 0);
            for (std::size_t leftIndex = 0; leftIndex < lhs.size(); ++leftIndex) {
                unsigned __int128 carry = 0;
                for (std::size_t rightIndex = 0;
                     rightIndex < rhs.size() && leftIndex + rightIndex < result.size();
                     ++rightIndex) {
                    const std::size_t resultIndex = leftIndex + rightIndex;
                    const unsigned __int128 product =
                        static_cast<unsigned __int128>(lhs[leftIndex]) * rhs[rightIndex] +
                        result[resultIndex] + carry;
                    result[resultIndex] = static_cast<uint64_t>(product);
                    carry = product >> 64U;
                }
            }
            normalizeWords(result, width);
            return result;
        }

        void shiftLeftOne(std::vector<uint64_t> &words, uint32_t width) {
            uint64_t carry = 0;
            for (uint64_t &word : words) {
                const uint64_t nextCarry = word >> 63U;
                word = (word << 1U) | carry;
                carry = nextCarry;
            }
            normalizeWords(words, width);
        }

        std::pair<std::vector<uint64_t>, std::vector<uint64_t>>
        divideUnsigned(std::span<const uint64_t> dividend,
                       std::span<const uint64_t> divisor, uint32_t width) {
            std::vector<uint64_t> quotient(wordCount(width), 0);
            std::vector<uint64_t> remainder(wordCount(width), 0);
            if (isZero(divisor)) {
                return {std::move(quotient), std::move(remainder)};
            }
            for (uint64_t offset = 0; offset < width; ++offset) {
                const uint64_t bit = static_cast<uint64_t>(width) - offset - 1U;
                shiftLeftOne(remainder, width);
                setBit(remainder, 0, getBit(dividend, bit));
                if (compareUnsigned(remainder, divisor) >= 0) {
                    remainder = subtractWords(remainder, divisor, width);
                    setBit(quotient, bit);
                }
            }
            return {std::move(quotient), std::move(remainder)};
        }

        std::vector<uint64_t> shiftLeft(std::span<const uint64_t> source,
                                        uint32_t width, uint64_t amount) {
            std::vector<uint64_t> result(wordCount(width), 0);
            if (amount >= width) {
                return result;
            }
            const std::size_t wordShift = static_cast<std::size_t>(amount / 64U);
            const uint32_t bitShift = amount % 64U;
            for (std::size_t sourceIndex = 0; sourceIndex < source.size(); ++sourceIndex) {
                const std::size_t targetIndex = sourceIndex + wordShift;
                if (targetIndex < result.size()) {
                    result[targetIndex] |= source[sourceIndex] << bitShift;
                }
                if (bitShift != 0 && targetIndex + 1U < result.size()) {
                    result[targetIndex + 1U] |= source[sourceIndex] >> (64U - bitShift);
                }
            }
            normalizeWords(result, width);
            return result;
        }

        std::vector<uint64_t> shiftRight(std::span<const uint64_t> source,
                                         uint32_t width, uint64_t amount, bool signFill) {
            std::vector<uint64_t> result(wordCount(width), signFill ? UINT64_MAX : 0);
            if (amount < width) {
                std::fill(result.begin(), result.end(), 0);
                const std::size_t wordShift = static_cast<std::size_t>(amount / 64U);
                const uint32_t bitShift = amount % 64U;
                for (std::size_t targetIndex = 0; targetIndex < result.size();
                     ++targetIndex) {
                    const std::size_t sourceIndex = targetIndex + wordShift;
                    if (sourceIndex < source.size()) {
                        result[targetIndex] |= source[sourceIndex] >> bitShift;
                    }
                    if (bitShift != 0 && sourceIndex + 1U < source.size()) {
                        result[targetIndex + 1U] |= source[sourceIndex + 1U]
                                                    << (64U - bitShift);
                    }
                }
                if (signFill) {
                    for (uint64_t bit = static_cast<uint64_t>(width) - amount; bit < width;
                         ++bit) {
                        setBit(result, bit);
                    }
                }
            }
            normalizeWords(result, width);
            return result;
        }

        uint64_t shiftAmountWords(std::span<const uint64_t> words, uint32_t limit) {
            for (std::size_t index = 1; index < words.size(); ++index) {
                if (words[index] != 0) {
                    return limit;
                }
            }
            return words.empty() ? 0 : std::min<uint64_t>(words.front(), limit);
        }

        // ------------------------------------------------------------------
        // Constant folding evaluator (pure opcodes only).
        // ------------------------------------------------------------------

        struct ConstOperand {
            Type type;
            std::span<const uint64_t> words;
        };

        bool truth(const ConstOperand &operand) { return !isZero(operand.words); }

        bool evaluatePure(Opcode opcode, const Type &resultType,
                          std::span<const ConstOperand> operands, uint32_t staticLsb,
                          std::vector<uint64_t> &resultWords) {
            const auto unaryWords = [&]() {
                return resizedWords(operands[0].words, operands[0].type.bitWidth,
                                    resultType.bitWidth, operands[0].type.signedness);
            };
            const auto binaryWords = [&]() {
                const Signedness common =
                    operands[0].type.signedness == Signedness::Signed &&
                            operands[1].type.signedness == Signedness::Signed
                        ? Signedness::Signed
                        : Signedness::Unsigned;
                return std::pair{
                    resizedWords(operands[0].words, operands[0].type.bitWidth,
                                 resultType.bitWidth, common),
                    resizedWords(operands[1].words, operands[1].type.bitWidth,
                                 resultType.bitWidth, common),
                };
            };

            switch (opcode) {
            case Opcode::Assign: {
                if (operands.size() != 1) {
                    return false;
                }
                resultWords = unaryWords();
                return true;
            }
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::Xnor: {
                if (operands.size() != 2) {
                    return false;
                }
                auto [lhs, rhs] = binaryWords();
                if (opcode == Opcode::Add) {
                    resultWords = addWords(lhs, rhs, resultType.bitWidth);
                } else if (opcode == Opcode::Sub) {
                    resultWords = subtractWords(lhs, rhs, resultType.bitWidth);
                } else if (opcode == Opcode::Mul) {
                    resultWords = multiplyWords(lhs, rhs, resultType.bitWidth);
                } else if (opcode == Opcode::Div || opcode == Opcode::Mod) {
                    if (isZero(rhs)) {
                        resultWords.assign(wordCount(resultType.bitWidth), 0);
                    } else {
                        const bool signedOperation =
                            resultType.signedness == Signedness::Signed;
                        const bool lhsNegative =
                            signedOperation && getBit(lhs, resultType.bitWidth - 1U);
                        const bool rhsNegative =
                            signedOperation && getBit(rhs, resultType.bitWidth - 1U);
                        const std::vector<uint64_t> magnitudeLhs =
                            lhsNegative ? negateWords(lhs, resultType.bitWidth) : lhs;
                        const std::vector<uint64_t> magnitudeRhs =
                            rhsNegative ? negateWords(rhs, resultType.bitWidth) : rhs;
                        auto [quotient, remainder] = divideUnsigned(
                            magnitudeLhs, magnitudeRhs, resultType.bitWidth);
                        if (opcode == Opcode::Div) {
                            resultWords = lhsNegative != rhsNegative
                                              ? negateWords(quotient, resultType.bitWidth)
                                              : std::move(quotient);
                        } else {
                            resultWords = lhsNegative
                                              ? negateWords(remainder, resultType.bitWidth)
                                              : std::move(remainder);
                        }
                    }
                } else {
                    resultWords.resize(lhs.size());
                    for (std::size_t index = 0; index < lhs.size(); ++index) {
                        if (opcode == Opcode::And) {
                            resultWords[index] = lhs[index] & rhs[index];
                        } else if (opcode == Opcode::Or) {
                            resultWords[index] = lhs[index] | rhs[index];
                        } else if (opcode == Opcode::Xor) {
                            resultWords[index] = lhs[index] ^ rhs[index];
                        } else {
                            resultWords[index] = ~(lhs[index] ^ rhs[index]);
                        }
                    }
                    normalizeWords(resultWords, resultType.bitWidth);
                }
                return true;
            }
            case Opcode::Not: {
                if (operands.size() != 1) {
                    return false;
                }
                resultWords = unaryWords();
                for (uint64_t &word : resultWords) {
                    word = ~word;
                }
                normalizeWords(resultWords, resultType.bitWidth);
                return true;
            }
            case Opcode::Eq:
            case Opcode::Ne:
            case Opcode::Lt:
            case Opcode::Le:
            case Opcode::Gt:
            case Opcode::Ge: {
                if (operands.size() != 2) {
                    return false;
                }
                const uint32_t width =
                    std::max(operands[0].type.bitWidth, operands[1].type.bitWidth);
                const Signedness common =
                    operands[0].type.signedness == Signedness::Signed &&
                            operands[1].type.signedness == Signedness::Signed
                        ? Signedness::Signed
                        : Signedness::Unsigned;
                const auto lhs =
                    resizedWords(operands[0].words, operands[0].type.bitWidth, width, common);
                const auto rhs =
                    resizedWords(operands[1].words, operands[1].type.bitWidth, width, common);
                int ordering = 0;
                if (common == Signedness::Signed) {
                    const bool lhsNegative = getBit(lhs, width - 1U);
                    const bool rhsNegative = getBit(rhs, width - 1U);
                    ordering = lhsNegative != rhsNegative
                                   ? (lhsNegative ? -1 : 1)
                                   : compareUnsigned(lhs, rhs);
                } else {
                    ordering = compareUnsigned(lhs, rhs);
                }
                bool predicate = false;
                if (opcode == Opcode::Eq) {
                    predicate = ordering == 0;
                } else if (opcode == Opcode::Ne) {
                    predicate = ordering != 0;
                } else if (opcode == Opcode::Lt) {
                    predicate = ordering < 0;
                } else if (opcode == Opcode::Le) {
                    predicate = ordering <= 0;
                } else if (opcode == Opcode::Gt) {
                    predicate = ordering > 0;
                } else {
                    predicate = ordering >= 0;
                }
                resultWords = {predicate ? 1U : 0U};
                return true;
            }
            case Opcode::LogicAnd:
            case Opcode::LogicOr: {
                if (operands.size() != 2) {
                    return false;
                }
                const bool value = opcode == Opcode::LogicAnd
                                       ? truth(operands[0]) && truth(operands[1])
                                       : truth(operands[0]) || truth(operands[1]);
                resultWords = {value ? 1U : 0U};
                return true;
            }
            case Opcode::LogicNot: {
                if (operands.size() != 1) {
                    return false;
                }
                resultWords = {!truth(operands[0]) ? 1U : 0U};
                return true;
            }
            case Opcode::ReduceAnd:
            case Opcode::ReduceNand:
            case Opcode::ReduceOr:
            case Opcode::ReduceNor:
            case Opcode::ReduceXor:
            case Opcode::ReduceXnor: {
                if (operands.size() != 1) {
                    return false;
                }
                bool reduced = false;
                if (opcode == Opcode::ReduceAnd || opcode == Opcode::ReduceNand) {
                    reduced = true;
                    for (uint64_t bit = 0; bit < operands[0].type.bitWidth; ++bit) {
                        reduced = reduced && getBit(operands[0].words, bit);
                    }
                    if (opcode == Opcode::ReduceNand) {
                        reduced = !reduced;
                    }
                } else if (opcode == Opcode::ReduceOr || opcode == Opcode::ReduceNor) {
                    reduced = truth(operands[0]);
                    if (opcode == Opcode::ReduceNor) {
                        reduced = !reduced;
                    }
                } else {
                    unsigned parity = 0;
                    for (uint64_t word : operands[0].words) {
                        parity ^= std::popcount(word) & 1U;
                    }
                    reduced = parity != 0;
                    if (opcode == Opcode::ReduceXnor) {
                        reduced = !reduced;
                    }
                }
                resultWords = {reduced ? 1U : 0U};
                return true;
            }
            case Opcode::Shl:
            case Opcode::LogicalShr:
            case Opcode::ArithmeticShr: {
                if (operands.size() != 2) {
                    return false;
                }
                const uint64_t amount =
                    shiftAmountWords(operands[1].words, resultType.bitWidth);
                if (opcode == Opcode::Shl) {
                    resultWords =
                        shiftLeft(operands[0].words, resultType.bitWidth, amount);
                } else {
                    const bool signFill =
                        opcode == Opcode::ArithmeticShr &&
                        operands[0].type.signedness == Signedness::Signed &&
                        getBit(operands[0].words, resultType.bitWidth - 1U);
                    resultWords = shiftRight(operands[0].words, resultType.bitWidth, amount,
                                             signFill);
                }
                return true;
            }
            case Opcode::Mux: {
                if (operands.size() != 3) {
                    return false;
                }
                const ConstOperand &selected =
                    truth(operands[0]) ? operands[1] : operands[2];
                const Signedness common =
                    operands[1].type.signedness == Signedness::Signed &&
                            operands[2].type.signedness == Signedness::Signed
                        ? Signedness::Signed
                        : Signedness::Unsigned;
                resultWords = resizedWords(selected.words, selected.type.bitWidth,
                                           resultType.bitWidth, common);
                return true;
            }
            case Opcode::Concat: {
                if (operands.empty()) {
                    return false;
                }
                resultWords.assign(wordCount(resultType.bitWidth), 0);
                uint64_t destination = 0;
                for (std::size_t offset = 0; offset < operands.size(); ++offset) {
                    const ConstOperand &source = operands[operands.size() - offset - 1U];
                    for (uint64_t bit = 0; bit < source.type.bitWidth; ++bit) {
                        setBit(resultWords, destination + bit, getBit(source.words, bit));
                    }
                    destination += source.type.bitWidth;
                }
                return true;
            }
            case Opcode::Replicate: {
                if (operands.size() != 1 || operands[0].type.bitWidth == 0) {
                    return false;
                }
                resultWords.assign(wordCount(resultType.bitWidth), 0);
                for (uint64_t destination = 0; destination < resultType.bitWidth;
                     destination += operands[0].type.bitWidth) {
                    for (uint64_t bit = 0; bit < operands[0].type.bitWidth; ++bit) {
                        setBit(resultWords, destination + bit,
                               getBit(operands[0].words, bit));
                    }
                }
                return true;
            }
            case Opcode::SliceStatic:
            case Opcode::SliceDynamic:
            case Opcode::SliceArray: {
                const bool isStatic = opcode == Opcode::SliceStatic;
                if (operands.size() != (isStatic ? 1U : 2U)) {
                    return false;
                }
                uint64_t start = 0;
                if (isStatic) {
                    start = staticLsb;
                } else {
                    const uint64_t index =
                        shiftAmountWords(operands[1].words, operands[0].type.bitWidth);
                    start = opcode == Opcode::SliceArray
                                ? index * static_cast<uint64_t>(resultType.bitWidth)
                                : index;
                }
                resultWords.assign(wordCount(resultType.bitWidth), 0);
                for (uint64_t bit = 0; bit < resultType.bitWidth; ++bit) {
                    if (start <= std::numeric_limits<uint64_t>::max() - bit) {
                        setBit(resultWords, bit, getBit(operands[0].words, start + bit));
                    }
                }
                return true;
            }
            default:
                return false;
            }
        }

        bool isCommutative(Opcode opcode) noexcept {
            switch (opcode) {
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::Xnor:
            case Opcode::Eq:
            case Opcode::Ne:
            case Opcode::LogicAnd:
            case Opcode::LogicOr:
                return true;
            default:
                return false;
            }
        }

        // ------------------------------------------------------------------
        // Hash-cons keys.
        // ------------------------------------------------------------------

        void hashCombine(std::size_t &hash, uint64_t value) noexcept {
            hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6U) + (hash >> 2U);
        }

        void hashType(std::size_t &hash, const Type &type) noexcept {
            hashCombine(hash, static_cast<uint8_t>(type.kind));
            hashCombine(hash, static_cast<uint8_t>(type.signedness));
            hashCombine(hash, type.bitWidth);
            hashCombine(hash, type.elementCount);
        }

        struct ConstantKey {
            Type type;
            std::vector<uint64_t> words;

            bool operator==(const ConstantKey &) const = default;
        };

        struct ConstantKeyHash {
            std::size_t operator()(const ConstantKey &key) const noexcept {
                std::size_t hash = 0;
                hashType(hash, key.type);
                for (uint64_t word : key.words) {
                    hashCombine(hash, word);
                }
                return hash;
            }
        };

        struct CseKey {
            Opcode opcode = Opcode::Assign;
            Type resultType;
            uint32_t staticLsb = 0;
            std::vector<VariableId> operands;

            bool operator==(const CseKey &) const = default;
        };

        struct CseKeyHash {
            std::size_t operator()(const CseKey &key) const noexcept {
                std::size_t hash = 0;
                hashCombine(hash, static_cast<uint8_t>(key.opcode));
                hashType(hash, key.resultType);
                hashCombine(hash, key.staticLsb);
                for (VariableId operand : key.operands) {
                    hashCombine(hash, operand.value);
                }
                return hash;
            }
        };

        // ------------------------------------------------------------------
        // Optimizer.
        // ------------------------------------------------------------------

        class AmGraphOptimizer {
        public:
            AmGraphOptimizer(AmGraph &graph,
                             const AmOptimizeOptions &options,
                             diag::Diagnostics &diagnostics)
                : graph_(graph), options_(options), diagnostics_(diagnostics),
                  view_(graph.program()) {}

            bool run() {
                if (!view_.valid()) {
                    diagnostics_.error("AM optimize requires a valid linear program",
                                       std::string(kDiagnosticContext));
                    return false;
                }
                analyze();
                if (options_.constFold || options_.cse || options_.assignAlias ||
                    options_.constMemFold || options_.logicUnify ||
                    options_.muxNotAbsorb || options_.notUnify ||
                    options_.sliceFuse) {
                    bool changed = true;
                    while (changed) {
                        changed = false;
                        if (options_.constFold) {
                            changed |= foldPass();
                        }
                        if (options_.logicUnify) {
                            changed |= logicUnifyPass();
                        }
                        if (options_.muxNotAbsorb) {
                            changed |= muxNotAbsorbPass();
                        }
                        if (options_.notUnify) {
                            changed |= notUnifyPass();
                        }
                        if (options_.sliceFuse) {
                            changed |= sliceFusePass();
                        }
                        if (options_.assignAlias) {
                            changed |= assignAliasPass();
                        }
                        if (options_.constMemFold) {
                            changed |= constMemFoldPass();
                        }
                        if (options_.cse) {
                            changed |= csePass();
                        }
                    }
                }
                markLiveInstructions();
                std::size_t kept = 0;
                for (uint8_t live : live_) {
                    kept += live != 0;
                }
                if (kept == live_.size() && newConstants_.empty() &&
                    rewrites_.empty()) {
                    // Nothing was removed or rewritten; the graph is untouched.
                    return validateSelf(graph_);
                }
                return compact(kept);
            }

        private:
            struct ConstantSlot {
                TypeId typeId;
                Type type;
                std::vector<uint64_t> words;
            };

            VariableRole roleOf(VariableId variable) const {
                return graph_.valueFacts(variable).roles;
            }

            // Same mechanical mapping as lowering.cpp's effect table fill and
            // pipeline.cpp's expectedInstructionEffect.
            InstructionEffect expectedEffect(InstructionId instruction) const {
                const Opcode opcode = view_.opcode(instruction);
                if (opcode == Opcode::SystemFunction) {
                    const auto attributes = view_.systemFunctionAttributes(instruction);
                    return attributes && attributes->hasSideEffects
                               ? InstructionEffect::HostEffect
                               : InstructionEffect::HostRead;
                }
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

            void analyze() {
                const uint32_t variableCount =
                    static_cast<uint32_t>(view_.variableCount());
                const uint32_t instructionCount =
                    static_cast<uint32_t>(view_.instructionCount());
                oldVariableCount_ = variableCount;
                oldInstructionCount_ = instructionCount;
                alias_.assign(variableCount, VariableId::invalid());
                transformDead_.assign(instructionCount, 0);

                // Canonicalize duplicate constant variables so CSE keys and
                // folded operands share one representative per (type, value).
                for (uint32_t index = 0; index < variableCount; ++index) {
                    const auto constant = constantWordsOf(index);
                    if (!constant) {
                        continue;
                    }
                    ConstantKey key{
                        .type = constant->type,
                        .words = std::vector<uint64_t>(constant->words.begin(),
                                                       constant->words.end()),
                    };
                    const auto [iter, inserted] = constantVars_.emplace(std::move(key), index);
                    canonicalConst_[index] = inserted ? index : iter->second;
                }

                inOrderedEffect_.assign(instructionCount, 0);
                for (const OrderedEffect &effect :
                     graph_.orderedEffects()) {
                    if (effect.instruction.valid() &&
                        effect.instruction.value < instructionCount) {
                        inOrderedEffect_[effect.instruction.value] = 1;
                    }
                }

                // Producer lookup (first def wins; the program is SSA-style
                // single-def in practice) for local pattern passes.
                producerOf_.assign(variableCount, kInvalidIndex);
                for (uint32_t index = 0; index < instructionCount; ++index) {
                    for (VariableId result : view_.results(InstructionId{index})) {
                        if (result.valid() && result.value < variableCount &&
                            producerOf_[result.value] == kInvalidIndex) {
                            producerOf_[result.value] = index;
                        }
                    }
                }

                // Commit-referenced variables: operands of any instruction
                // that is neither pure nor a plain state read (state writes,
                // change detectors, host effects, activation), closed
                // transitively over single-operand Assign forwarding
                // (result -> operand). A state-read Assign whose result is in
                // this set is a genuine commit read-old snapshot and must
                // stay; anything else is a compute-side wire that may be
                // bypassed (stateReadAlias). The closure is conservative:
                // some chained Assigns may ultimately not alias, which only
                // keeps a snapshot that could have been bypassed.
                commitOperand_.assign(variableCount, 0);
                std::vector<uint32_t> forward(variableCount, kInvalidIndex);
                for (uint32_t index = 0; index < instructionCount; ++index) {
                    const InstructionId instruction{index};
                    if (view_.opcode(instruction) == Opcode::Assign) {
                        const auto results = view_.results(instruction);
                        const auto operands = view_.operands(instruction);
                        if (results.size() == 1 && operands.size() == 1 &&
                            results.front().valid() && operands.front().valid() &&
                            results.front().value < variableCount &&
                            forward[results.front().value] == kInvalidIndex) {
                            forward[results.front().value] = operands.front().value;
                        }
                    }
                    const InstructionEffect effect = expectedEffect(instruction);
                    if (effect == InstructionEffect::Pure ||
                        effect == InstructionEffect::StateRead) {
                        continue;
                    }
                    for (VariableId operand : view_.operands(instruction)) {
                        if (operand.valid() && operand.value < variableCount) {
                            commitOperand_[operand.value] = 1;
                        }
                    }
                }
                // Close over Assign forwarding chains: a commit operand V is
                // ultimately read as resolve(V) = V, forward[V], ... so every
                // variable on the forward chain of a commit-referenced
                // variable is itself commit-referenced. Chains share
                // suffixes; stop a walk at the first already-marked link.
                for (uint32_t variable = 0; variable < variableCount; ++variable) {
                    if (commitOperand_[variable] == 0) {
                        continue;
                    }
                    uint32_t link = forward[variable];
                    while (link != kInvalidIndex && link < variableCount &&
                           commitOperand_[link] == 0) {
                        commitOperand_[link] = 1;
                        link = forward[link];
                    }
                }
            }

            // A variable may be aliased to a variable with an identical value
            // when it is neither externally driven nor a state target.
            // Interface-referenced variables (output ports, declared
            // observables) may be aliased: compact() re-points the interface
            // entries to the alias representative.
            bool aliasable(VariableId variable) const {
                if (!variable.valid() || variable.value >= oldVariableCount_) {
                    return false;
                }
                const VariableRole role = roleOf(variable);
                if (options_.interfaceAlias) {
                    if (hasRole(role, VariableRole::State) ||
                        hasRole(role, VariableRole::ExternalInput)) {
                        return false;
                    }
                } else if (role != VariableRole::None) {
                    return false;
                }
                const VariableRecord &record = view_.variable(variable);
                if (!record.init.valid() || record.init.value >= view_.initCount()) {
                    return false;
                }
                return view_.init(record.init).kind != InitKind::Constant;
            }

            uint32_t resolveVariable(uint32_t value) const {
                uint32_t resolved = value;
                while (resolved < alias_.size() && alias_[resolved].valid()) {
                    resolved = alias_[resolved].value;
                }
                const auto canonical = canonicalConst_.find(resolved);
                return canonical != canonicalConst_.end() ? canonical->second : resolved;
            }

            // Local rewrites recorded by the pattern passes (logic
            // unification, mux select-not absorption). Rewrites keep the
            // result variable and the pure effect; only the opcode and/or
            // the operand list change. Every consumer-facing read below
            // (fold, CSE, alias, compact) must go through these accessors so
            // rewrites recorded in an earlier fixpoint round are visible.
            struct InstructionRewrite {
                Opcode opcode;
                std::vector<VariableId> operands;
                std::optional<uint32_t> staticLsb;
            };

            Opcode effectiveOpcode(uint32_t index) const {
                const auto found = rewrites_.find(index);
                return found != rewrites_.end()
                           ? found->second.opcode
                           : view_.opcode(InstructionId{index});
            }

            // SliceStatic lsb honoring rewrites (slice fusion rewrites the
            // lsb); nullopt when the instruction carries no slice attribute.
            std::optional<uint32_t> effectiveStaticLsb(uint32_t index) const {
                const auto found = rewrites_.find(index);
                if (found != rewrites_.end()) {
                    return found->second.staticLsb;
                }
                const auto attributes =
                    view_.sliceStaticAttributes(InstructionId{index});
                if (!attributes) {
                    return std::nullopt;
                }
                return attributes->lsb;
            }

            // Resolved (alias-chased) operands honoring rewrites.
            void effectiveOperands(uint32_t index,
                                   std::vector<VariableId> &out) const {
                out.clear();
                const auto found = rewrites_.find(index);
                if (found != rewrites_.end()) {
                    for (VariableId operand : found->second.operands) {
                        out.push_back(operand.valid() ? VariableId{resolveVariable(
                                                            operand.value)}
                                                      : operand);
                    }
                    return;
                }
                for (VariableId operand : view_.operands(InstructionId{index})) {
                    out.push_back(operand.valid()
                                      ? VariableId{resolveVariable(operand.value)}
                                      : operand);
                }
            }

            uint32_t bitWidthOf(VariableId variable) const {
                if (!variable.valid()) {
                    return 0;
                }
                if (variable.value < oldVariableCount_) {
                    const VariableRecord &record = view_.variable(variable);
                    if (!record.type.valid() ||
                        record.type.value >= view_.typeCount()) {
                        return 0;
                    }
                    const Type &type = view_.type(record.type);
                    return type.kind == TypeKind::BitVector ? type.bitWidth : 0;
                }
                const std::size_t slot = variable.value - oldVariableCount_;
                if (slot >= newConstants_.size()) {
                    return 0;
                }
                const Type &type = newConstants_[slot].type;
                return type.kind == TypeKind::BitVector ? type.bitWidth : 0;
            }

            bool isOneBit(VariableId variable) const {
                if (!variable.valid()) {
                    return false;
                }
                if (variable.value < oldVariableCount_) {
                    const VariableRecord &record = view_.variable(variable);
                    if (!record.type.valid() ||
                        record.type.value >= view_.typeCount()) {
                        return false;
                    }
                    const Type &type = view_.type(record.type);
                    return type.kind == TypeKind::BitVector && type.bitWidth == 1;
                }
                const std::size_t slot = variable.value - oldVariableCount_;
                if (slot >= newConstants_.size()) {
                    return false;
                }
                const ConstantSlot &constant = newConstants_[slot];
                return constant.type.kind == TypeKind::BitVector &&
                       constant.type.bitWidth == 1;
            }

            std::optional<ConstOperand> constantWordsOf(uint32_t variable) const {
                if (variable >= oldVariableCount_) {
                    const std::size_t slot = variable - oldVariableCount_;
                    if (slot >= newConstants_.size()) {
                        return std::nullopt;
                    }
                    const ConstantSlot &constant = newConstants_[slot];
                    return ConstOperand{.type = constant.type,
                                        .words = std::span<const uint64_t>(constant.words)};
                }
                const VariableRecord &record = view_.variable(VariableId{variable});
                if (!record.init.valid() || record.init.value >= view_.initCount()) {
                    return std::nullopt;
                }
                const InitDescriptor &init = view_.init(record.init);
                if (init.kind != InitKind::Constant) {
                    return std::nullopt;
                }
                const LiteralId literalId{init.payload};
                if (!literalId.valid() || literalId.value >= view_.literalCount()) {
                    return std::nullopt;
                }
                const LiteralView literal = view_.literal(literalId);
                if (!literal.type.valid() || literal.type.value >= view_.typeCount()) {
                    return std::nullopt;
                }
                const Type &type = view_.type(literal.type);
                if (type.kind != TypeKind::BitVector) {
                    return std::nullopt;
                }
                return ConstOperand{.type = type, .words = literal.words};
            }

            uint32_t internConstant(TypeId typeId, const Type &type,
                                    std::vector<uint64_t> words) {
                normalizeWords(words, type.bitWidth);
                ConstantKey key{.type = type, .words = std::move(words)};
                const auto found = constantVars_.find(key);
                if (found != constantVars_.end()) {
                    return found->second;
                }
                const uint32_t id =
                    oldVariableCount_ + static_cast<uint32_t>(newConstants_.size());
                newConstants_.push_back(ConstantSlot{
                    .typeId = typeId,
                    .type = type,
                    .words = key.words,
                });
                constantVars_.emplace(std::move(key), id);
                return id;
            }

            bool foldPass() {
                bool changed = false;
                std::vector<VariableId> operands;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    const InstructionId instruction{index};
                    const Opcode opcode = effectiveOpcode(index);
                    if (opcodeTraits(opcode).effect != OpcodeEffect::Pure) {
                        continue;
                    }
                    const auto results = view_.results(instruction);
                    if (results.size() != 1 || !aliasable(results.front())) {
                        continue;
                    }
                    const VariableId result = results.front();
                    effectiveOperands(index, operands);
                    std::vector<ConstOperand> values;
                    values.reserve(operands.size());
                    bool allConstant = !operands.empty();
                    for (VariableId operand : operands) {
                        if (!operand.valid()) {
                            allConstant = false;
                            break;
                        }
                        const std::optional<ConstOperand> value =
                            constantWordsOf(operand.value);
                        if (!value) {
                            allConstant = false;
                            break;
                        }
                        values.push_back(*value);
                    }
                    if (!allConstant) {
                        continue;
                    }
                    const VariableRecord &record = view_.variable(result);
                    if (!record.type.valid() || record.type.value >= view_.typeCount()) {
                        continue;
                    }
                    const Type &resultType = view_.type(record.type);
                    if (resultType.kind != TypeKind::BitVector) {
                        continue;
                    }
                    uint32_t staticLsb = 0;
                    if (opcode == Opcode::SliceStatic) {
                        const std::optional<uint32_t> lsb =
                            effectiveStaticLsb(index);
                        if (!lsb) {
                            continue;
                        }
                        staticLsb = *lsb;
                    }
                    std::vector<uint64_t> folded;
                    if (!evaluatePure(opcode, resultType, values, staticLsb, folded)) {
                        continue;
                    }
                    alias_[result.value] =
                        VariableId{internConstant(record.type, resultType, std::move(folded))};
                    transformDead_[index] = 1;
                    ++foldedCount_;
                    changed = true;
                }
                return changed;
            }

            bool csePass() {
                std::unordered_map<CseKey, uint32_t, CseKeyHash> representatives;
                std::vector<VariableId> operands;
                bool changed = false;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    const InstructionId instruction{index};
                    const Opcode opcode = effectiveOpcode(index);
                    if (opcodeTraits(opcode).effect != OpcodeEffect::Pure) {
                        continue;
                    }
                    const auto results = view_.results(instruction);
                    if (results.size() != 1) {
                        continue;
                    }
                    const VariableRecord &record = view_.variable(results.front());
                    if (!record.type.valid() || record.type.value >= view_.typeCount()) {
                        continue;
                    }
                    CseKey key;
                    key.opcode = opcode;
                    key.resultType = view_.type(record.type);
                    if (opcode == Opcode::SliceStatic) {
                        const std::optional<uint32_t> lsb =
                            effectiveStaticLsb(index);
                        if (!lsb) {
                            continue;
                        }
                        key.staticLsb = *lsb;
                    }
                    bool validOperands = true;
                    effectiveOperands(index, operands);
                    for (VariableId operand : operands) {
                        if (!operand.valid()) {
                            validOperands = false;
                            break;
                        }
                        key.operands.push_back(operand);
                    }
                    if (!validOperands) {
                        continue;
                    }
                    if (isCommutative(opcode)) {
                        std::sort(key.operands.begin(), key.operands.end());
                    }
                    const auto [iter, inserted] =
                        representatives.emplace(std::move(key), index);
                    if (inserted) {
                        continue;
                    }
                    const VariableId duplicate = results.front();
                    const VariableId representative =
                        view_.results(InstructionId{iter->second}).front();
                    if (representative == duplicate || !aliasable(duplicate)) {
                        continue;
                    }
                    alias_[duplicate.value] = representative;
                    transformDead_[index] = 1;
                    ++cseCount_;
                    changed = true;
                }
                return changed;
            }

            // Bypass single-operand Assign instructions: the result aliases
            // the (resolved) operand directly. An Assign reading a state
            // variable whose result can reach a commit-side operand (the
            // commitOperand_ closure) is a commit read-old snapshot (see
            // lowering preCommitValue) and must stay, or commit blocks would
            // observe this round's committed value instead of the pre-commit
            // value. State reads that only feed compute logic are bypassed
            // when stateReadAlias is on: consumers then read the state
            // variable directly, which is value-identical (Assign is a pure
            // identity) and only shortens the activation chain.
            bool assignAliasPass() {
                bool changed = false;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    const InstructionId instruction{index};
                    if (view_.opcode(instruction) != Opcode::Assign) {
                        continue;
                    }
                    const auto results = view_.results(instruction);
                    const auto operands = view_.operands(instruction);
                    if (results.size() != 1 || operands.size() != 1) {
                        continue;
                    }
                    const VariableId result = results.front();
                    const VariableId operand = operands.front();
                    if (!operand.valid() || !aliasable(result)) {
                        continue;
                    }
                    if (hasRole(roleOf(operand), VariableRole::State) &&
                        (!options_.stateReadAlias ||
                         (result.value < commitOperand_.size() &&
                          commitOperand_[result.value] != 0))) {
                        continue;
                    }
                    const VariableRecord &resultRecord = view_.variable(result);
                    const VariableRecord &operandRecord = view_.variable(operand);
                    if (!resultRecord.type.valid() || !operandRecord.type.valid() ||
                        resultRecord.type.value >= view_.typeCount() ||
                        operandRecord.type.value >= view_.typeCount()) {
                        continue;
                    }
                    if (view_.type(resultRecord.type) != view_.type(operandRecord.type)) {
                        continue;
                    }
                    const uint32_t resolved = resolveVariable(operand.value);
                    if (resolved == result.value) {
                        continue;
                    }
                    alias_[result.value] = VariableId{resolved};
                    transformDead_[index] = 1;
                    ++aliasCount_;
                    changed = true;
                }
                return changed;
            }

            // Rewrite 1-bit LogicAnd/LogicOr/LogicNot to the bitwise
            // And/Or/Not. On 1-bit operands truth(x) is x itself, so the
            // forms are semantically identical; unifying them lets CSE merge
            // the parallel copies that Chisel's `&`/`&&` distinction
            // otherwise keeps alive.
            bool logicUnifyPass() {
                bool changed = false;
                std::vector<VariableId> operands;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    Opcode target;
                    switch (effectiveOpcode(index)) {
                    case Opcode::LogicAnd:
                        target = Opcode::And;
                        break;
                    case Opcode::LogicOr:
                        target = Opcode::Or;
                        break;
                    case Opcode::LogicNot:
                        target = Opcode::Not;
                        break;
                    default:
                        continue;
                    }
                    const auto results =
                        view_.results(InstructionId{index});
                    if (results.size() != 1 || !isOneBit(results.front())) {
                        continue;
                    }
                    effectiveOperands(index, operands);
                    bool allOneBit = !operands.empty();
                    for (VariableId operand : operands) {
                        if (!isOneBit(operand)) {
                            allOneBit = false;
                            break;
                        }
                    }
                    if (!allOneBit) {
                        continue;
                    }
                    rewrites_[index] = InstructionRewrite{
                        .opcode = target,
                        .operands = operands,
                    };
                    ++logicUnifyCount_;
                    changed = true;
                }
                return changed;
            }

            // Absorb Not/LogicNot driving Mux selects:
            // mux(!c, a, b) == mux(c, b, a). Both forms are only absorbed
            // when the inverted operand is 1 bit wide: the IR requires a
            // 1-bit Mux select (a wider c would need a ne(c, 0) per Mux,
            // which is worse than keeping one shared Not). The absorption
            // applies when the inverted value feeds ONLY Mux selects
            // (single-use or shared across any number of Muxes): every
            // consumer Mux gets its data arms swapped and the Not
            // instruction is removed. A select that is (directly or through
            // aliases) referenced by the interface keeps its producer alive.
            bool muxNotAbsorbPass() {
                const std::size_t variableSlots =
                    oldVariableCount_ + newConstants_.size();
                std::vector<uint8_t> pinned(variableSlots, 0);
                for (uint32_t variable = 0; variable < oldVariableCount_;
                     ++variable) {
                    const VariableRole role = roleOf(VariableId{variable});
                    if (hasRole(role, VariableRole::ExternalOutput) ||
                        hasRole(role, VariableRole::Observable)) {
                        const uint32_t resolved = resolveVariable(variable);
                        if (resolved < pinned.size()) {
                            pinned[resolved] = 1;
                        }
                    }
                }
                // Consumer lists over the effective (rewritten, resolved)
                // operand view.
                std::vector<std::vector<uint32_t>> consumers(variableSlots);
                std::vector<VariableId> operands;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    effectiveOperands(index, operands);
                    for (VariableId operand : operands) {
                        if (operand.valid() && operand.value < consumers.size()) {
                            consumers[operand.value].push_back(index);
                        }
                    }
                }
                std::vector<VariableId> notOperands;
                std::vector<VariableId> muxOperands;
                bool changed = false;
                for (uint32_t producer = 0; producer < oldInstructionCount_;
                     ++producer) {
                    if (transformDead_[producer] != 0) {
                        continue;
                    }
                    const Opcode notOpcode = effectiveOpcode(producer);
                    if (notOpcode != Opcode::Not &&
                        notOpcode != Opcode::LogicNot) {
                        continue;
                    }
                    const auto producerResults =
                        view_.results(InstructionId{producer});
                    if (producerResults.size() != 1) {
                        continue;
                    }
                    const VariableId select = producerResults.front();
                    if (!select.valid() || select.value >= consumers.size() ||
                        pinned[select.value] != 0 ||
                        consumers[select.value].empty()) {
                        continue;
                    }
                    effectiveOperands(producer, notOperands);
                    if (notOperands.size() != 1 || !notOperands.front().valid()) {
                        continue;
                    }
                    if (!isOneBit(select) || !isOneBit(notOperands.front())) {
                        continue;
                    }
                    bool allMuxSelects = true;
                    for (uint32_t consumer : consumers[select.value]) {
                        if (effectiveOpcode(consumer) != Opcode::Mux) {
                            allMuxSelects = false;
                            break;
                        }
                        effectiveOperands(consumer, muxOperands);
                        if (muxOperands.size() != 3 || muxOperands[0] != select) {
                            allMuxSelects = false;
                            break;
                        }
                    }
                    if (!allMuxSelects) {
                        continue;
                    }
                    for (uint32_t consumer : consumers[select.value]) {
                        effectiveOperands(consumer, muxOperands);
                        rewrites_[consumer] = InstructionRewrite{
                            .opcode = Opcode::Mux,
                            .operands = {notOperands.front(), muxOperands[2],
                                         muxOperands[1]},
                        };
                    }
                    transformDead_[producer] = 1;
                    ++muxAbsorbCount_;
                    changed = true;
                }
                return changed;
            }
            // Canonicalize 1-bit Not to Eq(x, 0): the reference flow
            // represents boolean negation as an equality against zero, so
            // this form both matches it and lets CSE merge the negation with
            // an explicit x == 0 computed elsewhere. Runs after logicUnify
            // (1-bit LogicNot has already become Not).
            bool notUnifyPass() {
                bool changed = false;
                std::vector<VariableId> operands;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0 ||
                        effectiveOpcode(index) != Opcode::Not) {
                        continue;
                    }
                    const auto results = view_.results(InstructionId{index});
                    if (results.size() != 1 || !isOneBit(results.front())) {
                        continue;
                    }
                    effectiveOperands(index, operands);
                    if (operands.size() != 1 || !isOneBit(operands.front())) {
                        continue;
                    }
                    const VariableRecord &record = view_.variable(results.front());
                    if (!record.type.valid() ||
                        record.type.value >= view_.typeCount()) {
                        continue;
                    }
                    const Type &type = view_.type(record.type);
                    const uint32_t zero = internConstant(
                        record.type, type, std::vector<uint64_t>{UINT64_C(0)});
                    rewrites_[index] = InstructionRewrite{
                        .opcode = Opcode::Eq,
                        .operands = {operands.front(), VariableId{zero}},
                    };
                    ++notUnifyCount_;
                    changed = true;
                }
                return changed;
            }


            // Fuse SliceStatic chains and drop identity slices:
            //   slice(slice(x, l1), l2) -> slice(x, l1 + l2)
            //   slice(x, 0) with identical result/operand types -> forward
            // Both rewrites keep the outer result variable; a fused-away
            // intermediate slice dies via DCE once its last consumer is
            // rewritten. The width guards keep every original slice fully
            // in-bounds so the zero-fill semantics of out-of-range reads are
            // preserved bit for bit.
            bool sliceFusePass() {
                bool changed = false;
                std::vector<VariableId> operands;
                std::vector<VariableId> parentOperands;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0 ||
                        effectiveOpcode(index) != Opcode::SliceStatic) {
                        continue;
                    }
                    const auto results = view_.results(InstructionId{index});
                    if (results.size() != 1) {
                        continue;
                    }
                    const std::optional<uint32_t> lsb = effectiveStaticLsb(index);
                    if (!lsb) {
                        continue;
                    }
                    effectiveOperands(index, operands);
                    if (operands.size() != 1 || !operands[0].valid()) {
                        continue;
                    }
                    const VariableId source = operands[0];
                    const uint32_t outerWidth = bitWidthOf(results.front());
                    const uint32_t midWidth = bitWidthOf(source);
                    if (outerWidth == 0 || midWidth == 0) {
                        continue;
                    }
                    if (*lsb == 0 && midWidth == outerWidth &&
                        aliasable(results.front()) &&
                        source.value < oldVariableCount_ &&
                        view_.variable(results.front()).type ==
                            view_.variable(source).type) {
                        // Same discipline as the state-read Assign bypass:
                        // forwarding a state read to a commit-side consumer
                        // would expose the post-commit value.
                        if (hasRole(roleOf(source), VariableRole::State) &&
                            results.front().value < commitOperand_.size() &&
                            commitOperand_[results.front().value] != 0) {
                            continue;
                        }
                        alias_[results.front().value] = VariableId{source.value};
                        transformDead_[index] = 1;
                        ++sliceFuseCount_;
                        changed = true;
                        continue;
                    }
                    if (source.value >= producerOf_.size()) {
                        continue;
                    }
                    const uint32_t parent = producerOf_[source.value];
                    if (parent == kInvalidIndex || transformDead_[parent] != 0 ||
                        effectiveOpcode(parent) != Opcode::SliceStatic) {
                        continue;
                    }
                    const auto parentResults =
                        view_.results(InstructionId{parent});
                    if (parentResults.size() != 1 ||
                        parentResults.front() != source) {
                        continue;
                    }
                    effectiveOperands(parent, parentOperands);
                    if (parentOperands.size() != 1 ||
                        !parentOperands.front().valid()) {
                        continue;
                    }
                    const std::optional<uint32_t> parentLsb =
                        effectiveStaticLsb(parent);
                    if (!parentLsb) {
                        continue;
                    }
                    const uint32_t baseWidth = bitWidthOf(parentOperands.front());
                    if (baseWidth == 0 || *lsb + outerWidth > midWidth ||
                        *parentLsb + midWidth > baseWidth) {
                        continue;
                    }
                    rewrites_[index] = InstructionRewrite{
                        .opcode = Opcode::SliceStatic,
                        .operands = {parentOperands.front()},
                        .staticLsb = *lsb + *parentLsb,
                    };
                    ++sliceFuseCount_;
                    changed = true;
                }
                return changed;
            }


            // Fold MemoryRead instructions with a constant address on memories
            // that are never written. Storage is zero-initialized (emitted
            // init() memsets the member region; the interpreter zero-fills),
            // so Undef/Zero init reads as zero, and Constant init carries the
            // packed lane literal (lowering arrayLaneConstInit layout: lane k
            // occupies bits [k*elemWidth, (k+1)*elemWidth)). Out-of-range
            // addresses read as zero (interpreter MemoryRead semantics).
            bool constMemFoldPass() {
                if (!writtenMemories_) {
                    std::vector<uint8_t> written(oldVariableCount_, 0);
                    for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                        const InstructionId instruction{index};
                        const OpcodeTraits traits = opcodeTraits(view_.opcode(instruction));
                        if (!traits.memoryAccess || !traits.hasOrderedEffect ||
                            traits.stateTargetOperand == OpcodeTraits::kNoTargetOperand) {
                            continue;
                        }
                        const auto operands = view_.operands(instruction);
                        if (traits.stateTargetOperand >= operands.size()) {
                            continue;
                        }
                        const VariableId target = operands[traits.stateTargetOperand];
                        if (target.valid() && target.value < oldVariableCount_) {
                            written[target.value] = 1;
                        }
                    }
                    writtenMemories_ = std::move(written);
                }
                const std::vector<uint8_t> &written = *writtenMemories_;
                bool changed = false;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    const InstructionId instruction{index};
                    if (view_.opcode(instruction) != Opcode::MemoryRead) {
                        continue;
                    }
                    const auto results = view_.results(instruction);
                    const auto operands = view_.operands(instruction);
                    if (results.size() != 1 || operands.size() != 2) {
                        continue;
                    }
                    const VariableId result = results.front();
                    const VariableId memory = operands[0];
                    const VariableId address = operands[1];
                    if (!memory.valid() || !address.valid() || !aliasable(result) ||
                        memory.value >= oldVariableCount_) {
                        continue;
                    }
                    if (written[memory.value] != 0) {
                        continue;
                    }
                    const VariableRecord &memoryRecord = view_.variable(memory);
                    if (!memoryRecord.type.valid() ||
                        memoryRecord.type.value >= view_.typeCount()) {
                        continue;
                    }
                    const Type &memoryType = view_.type(memoryRecord.type);
                    if (memoryType.elementCount == 0 || memoryType.bitWidth == 0) {
                        continue;
                    }
                    const std::optional<ConstOperand> addressValue =
                        constantWordsOf(resolveVariable(address.value));
                    if (!addressValue || addressValue->words.empty()) {
                        continue;
                    }
                    const VariableRecord &resultRecord = view_.variable(result);
                    if (!resultRecord.type.valid() ||
                        resultRecord.type.value >= view_.typeCount()) {
                        continue;
                    }
                    const Type &resultType = view_.type(resultRecord.type);
                    if (resultType.kind != TypeKind::BitVector ||
                        resultType.bitWidth != memoryType.bitWidth) {
                        continue;
                    }
                    uint64_t addressIndex = addressValue->words[0];
                    for (std::size_t word = 1; word < addressValue->words.size(); ++word) {
                        if (addressValue->words[word] != 0) {
                            addressIndex = std::numeric_limits<uint64_t>::max();
                            break;
                        }
                    }
                    std::vector<uint64_t> folded(wordCount(resultType.bitWidth), 0);
                    if (addressIndex < memoryType.elementCount) {
                        if (!memoryRecord.init.valid() ||
                            memoryRecord.init.value >= view_.initCount()) {
                            continue;
                        }
                        const InitDescriptor &init = view_.init(memoryRecord.init);
                        if (init.kind == InitKind::Constant) {
                            const LiteralId literalId{init.payload};
                            if (!literalId.valid() ||
                                literalId.value >= view_.literalCount()) {
                                continue;
                            }
                            const LiteralView literal = view_.literal(literalId);
                            const uint64_t base =
                                addressIndex * static_cast<uint64_t>(memoryType.bitWidth);
                            for (uint32_t bit = 0; bit < resultType.bitWidth; ++bit) {
                                if (getBit(literal.words, base + bit)) {
                                    setBit(folded, bit);
                                }
                            }
                        } else if (init.kind != InitKind::Zero &&
                                   init.kind != InitKind::Undef) {
                            continue;  // Actions init loads contents at runtime
                        }
                    }
                    alias_[result.value] = VariableId{internConstant(
                        resultRecord.type, resultType, std::move(folded))};
                    transformDead_[index] = 1;
                    ++memFoldCount_;
                    changed = true;
                }
                return changed;
            }

            void markLiveInstructions() {
                live_.assign(oldInstructionCount_, 0);
                if (!options_.dce) {
                    for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                        live_[index] = transformDead_[index] == 0 ? 1 : 0;
                    }
                    return;
                }

                std::vector<uint32_t> producer(oldVariableCount_, kInvalidIndex);
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    for (VariableId result : view_.results(InstructionId{index})) {
                        if (result.valid() && result.value < producer.size() &&
                            producer[result.value] == kInvalidIndex) {
                            producer[result.value] = index;
                        }
                    }
                }

                std::vector<uint32_t> worklist;
                const auto mark = [&](uint32_t index) {
                    if (index < live_.size() && live_[index] == 0 &&
                        transformDead_[index] == 0) {
                        live_[index] = 1;
                        worklist.push_back(index);
                    }
                };

                // Roots: side-effecting instructions and instructions pinned
                // by the ordered-effect chains (read/write order semantics).
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (transformDead_[index] != 0) {
                        continue;
                    }
                    const InstructionEffect effect = expectedEffect(InstructionId{index});
                    if (effect != InstructionEffect::Pure &&
                        effect != InstructionEffect::StateRead) {
                        mark(index);
                    }
                    if (inOrderedEffect_[index] != 0) {
                        mark(index);
                    }
                }
                // Roots: producers of externally visible variables. Aliased
                // variables root their alias representative instead.
                for (uint32_t variable = 0; variable < oldVariableCount_; ++variable) {
                    const VariableRole role = roleOf(VariableId{variable});
                    if (!hasRole(role, VariableRole::ExternalOutput) &&
                        !hasRole(role, VariableRole::Observable)) {
                        continue;
                    }
                    const uint32_t resolved = resolveVariable(variable);
                    if (resolved < producer.size() && producer[resolved] != kInvalidIndex) {
                        mark(producer[resolved]);
                    }
                }

                std::vector<VariableId> operands;
                while (!worklist.empty()) {
                    const uint32_t index = worklist.back();
                    worklist.pop_back();
                    effectiveOperands(index, operands);
                    for (VariableId operand : operands) {
                        if (!operand.valid()) {
                            continue;
                        }
                        if (operand.value < producer.size() &&
                            producer[operand.value] != kInvalidIndex) {
                            mark(producer[operand.value]);
                        }
                    }
                }
            }

            bool validateSelf(const AmGraph &candidate) {
                const ValidationResult validation =
                    validate(candidate, ValidationOptions{
                                              .level = ValidationLevel::Semantic,
                                          });
                if (validation.success()) {
                    return true;
                }
                constexpr std::size_t kMaxReportedErrors = 16;
                const std::size_t reported =
                    std::min(validation.errors.size(), kMaxReportedErrors);
                for (std::size_t index = 0; index < reported; ++index) {
                    diagnostics_.error("AM optimize produced an invalid graph: " +
                                           validation.errors[index],
                                       std::string(kDiagnosticContext));
                }
                return false;
            }

            bool compact(std::size_t keptInstructions) {
                std::vector<uint32_t> instructionRemap(oldInstructionCount_, kInvalidIndex);
                uint32_t nextInstruction = 0;
                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (live_[index] != 0) {
                        instructionRemap[index] = nextInstruction++;
                    }
                }

                AmGraph next;
                const ProgramStorageStats stats = view_.storageStats();
                ProgramReserve reserve{};
                reserve.types = view_.typeCount();
                reserve.strings = view_.stringCount();
                reserve.stringBytes = stats.arena(ProgramArena::StringBytes).elements;
                reserve.initDescriptors = stats.arena(ProgramArena::InitDescriptors).elements;
                reserve.initActions = stats.arena(ProgramArena::InitActions).elements;
                reserve.literals = view_.literalCount() + newConstants_.size();
                reserve.literalWords = stats.arena(ProgramArena::LiteralWords).elements;
                reserve.literalBytes = stats.arena(ProgramArena::LiteralBytes).elements;
                reserve.variables = oldVariableCount_ + newConstants_.size();
                reserve.variableLabels = stats.arena(ProgramArena::VariableLabels).elements;
                reserve.instructions = keptInstructions;
                reserve.operands = stats.arena(ProgramArena::Operands).elements;
                reserve.results = stats.arena(ProgramArena::Results).elements;
                reserve.sliceStaticAttributes =
                    stats.arena(ProgramArena::SliceStaticAttributes).elements;
                reserve.systemFunctionAttributes =
                    stats.arena(ProgramArena::SystemFunctionAttributes).elements;
                reserve.systemTaskAttributes =
                    stats.arena(ProgramArena::SystemTaskAttributes).elements;
                reserve.dpiCallAttributes =
                    stats.arena(ProgramArena::DpiCallAttributes).elements;
                reserve.dpiImports = view_.dpiImportCount();
                reserve.dpiParameters = stats.arena(ProgramArena::DpiParameters).elements;
                next.reserve(reserve);

                // Types, strings, and literals are copied in order so their
                // IDs stay identical; only InstructionIds are compacted.
                for (uint32_t index = 0; index < view_.typeCount(); ++index) {
                    next.addType(view_.type(TypeId{index}));
                }
                for (uint32_t index = 0; index < view_.stringCount(); ++index) {
                    next.addString(view_.string(StringId{index}));
                }
                for (uint32_t index = 0; index < view_.literalCount(); ++index) {
                    const LiteralView literal = view_.literal(LiteralId{index});
                    const Type &type = view_.type(literal.type);
                    if (type.kind == TypeKind::String) {
                        next.addStringLiteral(literal.type, literal.bytes);
                    } else {
                        next.addBitLiteral(literal.type, literal.words);
                    }
                }

                // Roles with the interface-visibility carry, computed before
                // the variables are created so each variable lands with its
                // final facts: interface visibility roles move from aliased
                // variables to their alias representatives so the roles stay
                // consistent with the re-pointed ProgramInterface.
                std::vector<VariableRole> roles;
                roles.reserve(oldVariableCount_ + newConstants_.size());
                for (uint32_t index = 0; index < oldVariableCount_; ++index) {
                    roles.push_back(roleOf(VariableId{index}));
                }
                roles.resize(oldVariableCount_ + newConstants_.size(), VariableRole::None);
                constexpr uint8_t kInterfaceRoleBits =
                    static_cast<uint8_t>(VariableRole::ExternalOutput) |
                    static_cast<uint8_t>(VariableRole::Observable);
                for (uint32_t index = 0; index < oldVariableCount_; ++index) {
                    if (!alias_[index].valid()) {
                        continue;
                    }
                    const uint8_t role = static_cast<uint8_t>(roles[index]);
                    const uint8_t carried = role & kInterfaceRoleBits;
                    if (carried == 0) {
                        continue;
                    }
                    roles[index] = static_cast<VariableRole>(role & ~kInterfaceRoleBits);
                    const uint32_t resolved = resolveVariable(index);
                    if (resolved < roles.size()) {
                        roles[resolved] = static_cast<VariableRole>(
                            static_cast<uint8_t>(roles[resolved]) | carried);
                    }
                }

                for (uint32_t index = 0; index < oldVariableCount_; ++index) {
                    const VariableRecord &record = view_.variable(VariableId{index});
                    const InitDescriptor &descriptor = view_.init(record.init);
                    InitId init;
                    switch (descriptor.kind) {
                    case InitKind::Undef:
                        init = next.undefInit();
                        break;
                    case InitKind::Zero:
                        init = next.zeroInit();
                        break;
                    case InitKind::Constant:
                        init = next.addConstantInit(LiteralId{descriptor.payload});
                        break;
                    case InitKind::Actions:
                        init = next.addActionsInit(view_.initActions(record.init));
                        break;
                    }
                    AmValueFacts facts = graph_.valueFacts(VariableId{index});
                    facts.roles = roles[index];
                    next.addVariable(record.type, init,
                                     view_.variableLabel(VariableId{index}), facts);
                }
                for (std::size_t index = 0; index < newConstants_.size(); ++index) {
                    const ConstantSlot &constant = newConstants_[index];
                    const LiteralId literal =
                        next.addBitLiteral(constant.typeId, constant.words);
                    AmValueFacts facts;
                    facts.kind = AmValueKind::Constant;
                    facts.roles = roles[oldVariableCount_ + index];
                    next.addVariable(constant.typeId, next.addConstantInit(literal),
                                     std::nullopt, facts);
                }

                for (uint32_t index = 0; index < view_.dpiImportCount(); ++index) {
                    const DpiImportView import = view_.dpiImport(DpiImportId{index});
                    next.addDpiImport(import.symbol, import.parameters,
                                      import.returnValue);
                }

                for (uint32_t index = 0; index < oldInstructionCount_; ++index) {
                    if (live_[index] == 0) {
                        continue;
                    }
                    const InstructionId instruction{index};
                    const Opcode opcode = effectiveOpcode(index);
                    std::vector<VariableId> operands;
                    effectiveOperands(index, operands);
                    const InstructionId rebuilt =
                        next.addInstruction(opcode, view_.results(instruction),
                                            operands);
                    if (rebuilt.value != instructionRemap[index]) {
                        diagnostics_.error("AM optimize lost dense instruction alignment",
                                           std::string(kDiagnosticContext));
                        return false;
                    }
                    if (opcode == Opcode::SliceStatic) {
                        if (const std::optional<uint32_t> lsb =
                                effectiveStaticLsb(index)) {
                            next.setSliceStaticAttributes(rebuilt, *lsb);
                        }
                    }
                    if (const auto attributes = view_.systemFunctionAttributes(instruction)) {
                        next.setSystemFunctionAttributes(rebuilt, *attributes);
                    }
                    if (const auto attributes = view_.systemTaskAttributes(instruction)) {
                        next.setSystemTaskAttributes(rebuilt, *attributes);
                    }
                    if (const auto attributes = view_.dpiCallAttributes(instruction)) {
                        next.setDpiCallAttributes(rebuilt, *attributes);
                    }
                    next.setInstructionEffect(rebuilt, expectedEffect(instruction));
                }

                // Interface entries keep referring to the eliminated
                // duplicates unless re-pointed to their alias representatives
                // (values are identical by construction).
                ProgramInterface rebuiltInterface = graph_.interface();
                for (PortBinding &port : rebuiltInterface.ports) {
                    if (port.input.valid()) {
                        port.input = VariableId{resolveVariable(port.input.value)};
                    }
                    if (port.output.valid()) {
                        port.output = VariableId{resolveVariable(port.output.value)};
                    }
                    if (port.outputEnable.valid()) {
                        port.outputEnable =
                            VariableId{resolveVariable(port.outputEnable.value)};
                    }
                }
                for (VariableLabel &declared : rebuiltInterface.declaredVariables) {
                    if (declared.variable.valid()) {
                        declared.variable =
                            VariableId{resolveVariable(declared.variable.value)};
                    }
                }
                next.mutableInterface() = std::move(rebuiltInterface);

                for (const OrderedEffect &effect : graph_.orderedEffects()) {
                    if (effect.instruction.valid() &&
                        effect.instruction.value < oldInstructionCount_ &&
                        instructionRemap[effect.instruction.value] != kInvalidIndex) {
                        next.orderedEffects().push_back(OrderedEffect{
                            .instruction =
                                InstructionId{instructionRemap[effect.instruction.value]},
                            .group = effect.group,
                            .ordinal = effect.ordinal,
                        });
                    }
                }

                if (!validateSelf(next)) {
                    return false;
                }
                const std::size_t removed = oldInstructionCount_ - keptInstructions;
                const std::size_t accounted =
                    foldedCount_ + cseCount_ + aliasCount_ + memFoldCount_ +
                    muxAbsorbCount_ + sliceFuseCount_;
                diagnostics_.info(
                    "am.optimize: instructions " + std::to_string(oldInstructionCount_) +
                        " -> " + std::to_string(keptInstructions) +
                        " (fold=" + std::to_string(foldedCount_) +
                        " cse=" + std::to_string(cseCount_) +
                        " alias=" + std::to_string(aliasCount_) +
                        " memfold=" + std::to_string(memFoldCount_) +
                        " unify=" + std::to_string(logicUnifyCount_) +
                        " muxabsorb=" + std::to_string(muxAbsorbCount_) +
                        " notunify=" + std::to_string(notUnifyCount_) +
                        " slicefuse=" + std::to_string(sliceFuseCount_) +
                        " dce=" + std::to_string(removed > accounted
                                                    ? removed - accounted
                                                    : std::size_t{0}) +
                        ") constants_added=" + std::to_string(newConstants_.size()),
                    std::string(kDiagnosticContext));
                graph_ = std::move(next);
                return true;
            }

            AmGraph &graph_;
            const AmOptimizeOptions &options_;
            diag::Diagnostics &diagnostics_;
            ProgramView view_;

            uint32_t oldVariableCount_ = 0;
            uint32_t oldInstructionCount_ = 0;
            std::vector<VariableId> alias_;
            std::vector<uint8_t> transformDead_;
            std::vector<uint8_t> live_;
            std::vector<uint8_t> inOrderedEffect_;
            std::vector<uint32_t> producerOf_;
            std::vector<uint8_t> commitOperand_;
            std::unordered_map<uint32_t, InstructionRewrite> rewrites_;
            std::optional<std::vector<uint8_t>> writtenMemories_;
            std::vector<ConstantSlot> newConstants_;
            std::unordered_map<ConstantKey, uint32_t, ConstantKeyHash> constantVars_;
            std::unordered_map<uint32_t, uint32_t> canonicalConst_;
            std::size_t foldedCount_ = 0;
            std::size_t cseCount_ = 0;
            std::size_t aliasCount_ = 0;
            std::size_t memFoldCount_ = 0;
            std::size_t logicUnifyCount_ = 0;
            std::size_t muxAbsorbCount_ = 0;
            std::size_t notUnifyCount_ = 0;
            std::size_t sliceFuseCount_ = 0;
        };

    } // namespace

    bool optimizeAmGraph(AmGraph &graph,
                         const AmOptimizeOptions &options,
                         wolvrix::lib::diag::Diagnostics &diagnostics) {
        if (!options.dce && !options.constFold && !options.cse && !options.assignAlias &&
            !options.constMemFold && !options.logicUnify && !options.muxNotAbsorb &&
            !options.notUnify && !options.sliceFuse) {
            return true;
        }
        try {
            AmGraphOptimizer optimizer(graph, options, diagnostics);
            return optimizer.run();
        } catch (const std::exception &error) {
            diagnostics.error(std::string("AM optimize failed: ") + error.what(),
                              std::string(kDiagnosticContext));
            return false;
        }
    }

} // namespace wolvrix::lib::grhsim::am
