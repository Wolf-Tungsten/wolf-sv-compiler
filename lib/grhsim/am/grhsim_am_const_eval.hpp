#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_CONST_EVAL_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_CONST_EVAL_HPP

#include "grhsim/am/grhsim_am_program.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Bit-vector word helpers and the pure-op constant-folding evaluator. These
// mirror the evaluation semantics of lib/grhsim/am/grhsim_am_program_interpreter.cpp
// so folded constants match runtime evaluation bit for bit. Shared by the AM
// graph optimizer (grhsim_am_graph_optimize.cpp) and the GRH -> AM lowering
// (grh_ir_to_grhsim_am_graph.cpp); everything is inline so both translation
// units can include it. Internal to lib/grhsim/am.
namespace wolvrix::lib::grhsim::am::detail
{

// ------------------------------------------------------------------
// Bit-vector word helpers. These mirror the evaluation semantics of
// lib/grhsim/am/interpreter.cpp so folded constants match runtime
// evaluation bit for bit.
// ------------------------------------------------------------------

inline std::size_t wordCount(uint32_t width) {
    return (static_cast<std::size_t>(width) + 63U) / 64U;
}

inline uint64_t highWordMask(uint32_t width) {
    const uint32_t used = width % 64U;
    return used == 0 ? std::numeric_limits<uint64_t>::max()
                     : (UINT64_C(1) << used) - 1U;
}

inline void normalizeWords(std::vector<uint64_t> &words, uint32_t width) {
    words.resize(wordCount(width), 0);
    if (!words.empty()) {
        words.back() &= highWordMask(width);
    }
}

inline bool getBit(std::span<const uint64_t> words, uint64_t index) {
    const uint64_t word = index / 64U;
    return word < words.size() && ((words[word] >> (index % 64U)) & 1U) != 0;
}

inline void setBit(std::vector<uint64_t> &words, uint64_t index, bool value = true) {
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

inline bool isZero(std::span<const uint64_t> words) {
    return std::all_of(words.begin(), words.end(),
                       [](uint64_t word) { return word == 0; });
}

inline std::vector<uint64_t> resizedWords(std::span<const uint64_t> words,
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

inline int compareUnsigned(std::span<const uint64_t> lhs,
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

inline std::vector<uint64_t> addWords(std::span<const uint64_t> lhs,
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

inline std::vector<uint64_t> subtractWords(std::span<const uint64_t> lhs,
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

inline std::vector<uint64_t> negateWords(std::span<const uint64_t> source,
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

inline std::vector<uint64_t> multiplyWords(std::span<const uint64_t> lhs,
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

inline void shiftLeftOne(std::vector<uint64_t> &words, uint32_t width) {
    uint64_t carry = 0;
    for (uint64_t &word : words) {
        const uint64_t nextCarry = word >> 63U;
        word = (word << 1U) | carry;
        carry = nextCarry;
    }
    normalizeWords(words, width);
}

inline std::pair<std::vector<uint64_t>, std::vector<uint64_t>>
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

inline std::vector<uint64_t> shiftLeft(std::span<const uint64_t> source,
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

inline std::vector<uint64_t> shiftRight(std::span<const uint64_t> source,
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

inline uint64_t shiftAmountWords(std::span<const uint64_t> words, uint32_t limit) {
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

inline bool truth(const ConstOperand &operand) { return !isZero(operand.words); }

inline bool evaluatePure(Opcode opcode, const Type &resultType,
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
    case Opcode::Insert: {
        if (operands.size() != 2) {
            return false;
        }
        resultWords = resizedWords(operands[0].words, operands[0].type.bitWidth,
                                   resultType.bitWidth, resultType.signedness);
        const uint64_t dataWidth = operands[1].type.bitWidth;
        if (static_cast<uint64_t>(staticLsb) + dataWidth > resultType.bitWidth) {
            return false;
        }
        for (uint64_t bit = 0; bit < dataWidth; ++bit) {
            setBit(resultWords, staticLsb + bit, getBit(operands[1].words, bit));
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace wolvrix::lib::grhsim::am::detail

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_CONST_EVAL_HPP
