R"WCPU(#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace wolvrix_grhsim_cpu_runtime {

struct Bits {
    std::uint64_t *data = nullptr;
    std::uint32_t width = 0;
};

struct ConstBits {
    const std::uint64_t *data = nullptr;
    std::uint32_t width = 0;
};

inline ConstBits as_const(Bits value) { return ConstBits{value.data, value.width}; }
inline std::size_t word_count(std::uint32_t width) { return (static_cast<std::size_t>(width) + 63U) / 64U; }

inline std::uint64_t low_mask(std::uint32_t bits) {
    return bits == 0 ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
}

inline void truncate(Bits value) {
    const std::uint32_t tail = value.width & 63U;
    if (tail != 0 && value.width != 0) {
        value.data[word_count(value.width) - 1U] &= low_mask(tail);
    }
}

inline void clear(Bits value) {
    std::fill_n(value.data, word_count(value.width), UINT64_C(0));
}

inline bool get_bit(ConstBits value, std::uint64_t index) {
    return index < value.width && ((value.data[index >> 6U] >> (index & 63U)) & UINT64_C(1)) != 0;
}

inline void set_bit(Bits value, std::uint64_t index, bool bit) {
    if (index >= value.width) return;
    const std::uint64_t mask = UINT64_C(1) << (index & 63U);
    std::uint64_t &word = value.data[index >> 6U];
    word = bit ? (word | mask) : (word & ~mask);
}

inline bool sign_bit(ConstBits value) {
    return value.width != 0 && get_bit(value, static_cast<std::uint64_t>(value.width) - 1U);
}

inline std::uint64_t extended_word(ConstBits value, std::size_t index, bool sign_extend) {
    const std::size_t count = word_count(value.width);
    const bool negative = sign_extend && sign_bit(value);
    if (index >= count) return negative ? ~UINT64_C(0) : UINT64_C(0);
    std::uint64_t word = value.data[index];
    const std::uint32_t tail = value.width & 63U;
    if (tail != 0 && index + 1U == count) {
        const std::uint64_t mask = low_mask(tail);
        word &= mask;
        if (negative) word |= ~mask;
    }
    return word;
}

inline void copy(Bits destination, ConstBits source, bool sign_extend = false) {
    const std::size_t count = word_count(destination.width);
    for (std::size_t index = 0; index < count; ++index) {
        destination.data[index] = extended_word(source, index, sign_extend);
    }
    truncate(destination);
}

inline bool equal(ConstBits lhs, ConstBits rhs) {
    const std::uint32_t width = std::max(lhs.width, rhs.width);
    const std::size_t count = word_count(width);
    for (std::size_t index = 0; index < count; ++index) {
        if (extended_word(lhs, index, false) != extended_word(rhs, index, false)) return false;
    }
    return true;
}

inline bool any(ConstBits value) {
    const std::size_t count = word_count(value.width);
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t word = value.data[index];
        if (index + 1U == count && (value.width & 63U) != 0) word &= low_mask(value.width & 63U);
        if (word != 0) return true;
    }
    return false;
}

inline bool all(ConstBits value) {
    if (value.width == 0) return false;
    const std::size_t count = word_count(value.width);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t remaining = value.width - static_cast<std::uint32_t>(index * 64U);
        const std::uint64_t expected = remaining >= 64U ? ~UINT64_C(0) : low_mask(remaining);
        if ((value.data[index] & expected) != expected) return false;
    }
    return true;
}

inline bool parity(ConstBits value) {
    bool result = false;
    const std::size_t count = word_count(value.width);
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t word = value.data[index];
        if (index + 1U == count && (value.width & 63U) != 0) word &= low_mask(value.width & 63U);
        result ^= (__builtin_parityll(word) != 0);
    }
    return result;
}

inline std::uint64_t to_u64(ConstBits value) {
    return value.width == 0 ? UINT64_C(0) : extended_word(value, 0, false);
}

inline bool to_index(ConstBits value, std::uint64_t &result) {
    result = to_u64(value);
    for (std::size_t index = 1; index < word_count(value.width); ++index) {
        if (value.data[index] != 0) return false;
    }
    return true;
}

inline void op_add(Bits destination, ConstBits lhs, ConstBits rhs, bool signed_mode) {
    unsigned __int128 carry = 0;
    for (std::size_t index = 0; index < word_count(destination.width); ++index) {
        const unsigned __int128 sum = static_cast<unsigned __int128>(extended_word(lhs, index, signed_mode)) +
                                      extended_word(rhs, index, signed_mode) + carry;
        destination.data[index] = static_cast<std::uint64_t>(sum);
        carry = sum >> 64U;
    }
    truncate(destination);
}

inline void op_sub(Bits destination, ConstBits lhs, ConstBits rhs, bool signed_mode) {
    std::uint64_t borrow = 0;
    for (std::size_t index = 0; index < word_count(destination.width); ++index) {
        const std::uint64_t left = extended_word(lhs, index, signed_mode);
        const std::uint64_t right = extended_word(rhs, index, signed_mode);
        const std::uint64_t first = left - right;
        const std::uint64_t first_borrow = left < right;
        const std::uint64_t result = first - borrow;
        const std::uint64_t second_borrow = first < borrow;
        destination.data[index] = result;
        borrow = first_borrow | second_borrow;
    }
    truncate(destination);
}

inline void op_mul(Bits destination, ConstBits lhs, ConstBits rhs, bool signed_mode) {
    clear(destination);
    const std::size_t count = word_count(destination.width);
    for (std::size_t left_index = 0; left_index < count; ++left_index) {
        unsigned __int128 carry = 0;
        const std::uint64_t left = extended_word(lhs, left_index, signed_mode);
        for (std::size_t right_index = 0; left_index + right_index < count; ++right_index) {
            const std::size_t output = left_index + right_index;
            const unsigned __int128 product =
                static_cast<unsigned __int128>(left) * extended_word(rhs, right_index, signed_mode) +
                destination.data[output] + carry;
            destination.data[output] = static_cast<std::uint64_t>(product);
            carry = product >> 64U;
        }
    }
    truncate(destination);
}

inline void negate(Bits value) {
    std::uint64_t carry = 1;
    for (std::size_t index = 0; index < word_count(value.width); ++index) {
        const std::uint64_t inverted = ~value.data[index];
        const std::uint64_t result = inverted + carry;
        carry = carry != 0 && result < inverted ? 1 : 0;
        value.data[index] = result;
    }
    truncate(value);
}

inline int compare_unsigned(ConstBits lhs, ConstBits rhs) {
    const std::uint32_t width = std::max(lhs.width, rhs.width);
    for (std::size_t index = word_count(width); index-- > 0;) {
        const std::uint64_t left = extended_word(lhs, index, false);
        const std::uint64_t right = extended_word(rhs, index, false);
        if (left != right) return left < right ? -1 : 1;
    }
    return 0;
}

inline void shift_left_one(Bits value) {
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < word_count(value.width); ++index) {
        const std::uint64_t next_carry = value.data[index] >> 63U;
        value.data[index] = (value.data[index] << 1U) | carry;
        carry = next_carry;
    }
    truncate(value);
}

inline void op_div(Bits destination, ConstBits lhs, ConstBits rhs, bool signed_mode, bool remainder) {
    std::vector<std::uint64_t> dividend_words(word_count(destination.width), UINT64_C(0));
    std::vector<std::uint64_t> divisor_words(word_count(destination.width), UINT64_C(0));
    std::vector<std::uint64_t> quotient_words(word_count(destination.width), UINT64_C(0));
    std::vector<std::uint64_t> remainder_words(word_count(destination.width), UINT64_C(0));
    Bits dividend{dividend_words.data(), destination.width};
    Bits divisor{divisor_words.data(), destination.width};
    Bits quotient{quotient_words.data(), destination.width};
    Bits partial{remainder_words.data(), destination.width};
    copy(dividend, lhs, signed_mode);
    copy(divisor, rhs, signed_mode);
    const bool lhs_negative = signed_mode && sign_bit(lhs);
    const bool rhs_negative = signed_mode && sign_bit(rhs);
    if (lhs_negative) negate(dividend);
    if (rhs_negative) negate(divisor);
    if (!any(as_const(divisor))) {
        clear(destination);
        return;
    }
    for (std::uint64_t bit = destination.width; bit-- > 0;) {
        shift_left_one(partial);
        set_bit(partial, 0, get_bit(as_const(dividend), bit));
        if (compare_unsigned(as_const(partial), as_const(divisor)) >= 0) {
            op_sub(partial, as_const(partial), as_const(divisor), false);
            set_bit(quotient, bit, true);
        }
    }
    if (remainder) {
        if (lhs_negative) negate(partial);
        copy(destination, as_const(partial));
    }
    else {
        if (lhs_negative != rhs_negative) negate(quotient);
        copy(destination, as_const(quotient));
    }
}

enum class BinaryKind : std::uint8_t { And, Or, Xor, Xnor };

inline void op_binary(Bits destination, ConstBits lhs, ConstBits rhs, bool signed_mode, BinaryKind kind) {
    for (std::size_t index = 0; index < word_count(destination.width); ++index) {
        const std::uint64_t left = extended_word(lhs, index, signed_mode);
        const std::uint64_t right = extended_word(rhs, index, signed_mode);
        switch (kind) {
        case BinaryKind::And: destination.data[index] = left & right; break;
        case BinaryKind::Or: destination.data[index] = left | right; break;
        case BinaryKind::Xor: destination.data[index] = left ^ right; break;
        case BinaryKind::Xnor: destination.data[index] = ~(left ^ right); break;
        }
    }
    truncate(destination);
}

inline void op_not(Bits destination, ConstBits source) {
    for (std::size_t index = 0; index < word_count(destination.width); ++index) {
        destination.data[index] = ~extended_word(source, index, false);
    }
    truncate(destination);
}

inline int compare(ConstBits lhs, ConstBits rhs, bool signed_mode) {
    const std::uint32_t width = std::max(lhs.width, rhs.width);
    if (signed_mode) {
        const bool lhs_negative = lhs.width != 0 && sign_bit(lhs);
        const bool rhs_negative = rhs.width != 0 && sign_bit(rhs);
        if (lhs_negative != rhs_negative) return lhs_negative ? -1 : 1;
    }
    for (std::size_t index = word_count(width); index-- > 0;) {
        const std::uint64_t left = extended_word(lhs, index, signed_mode);
        const std::uint64_t right = extended_word(rhs, index, signed_mode);
        if (left != right) return left < right ? -1 : 1;
    }
    return 0;
}

inline void set_bool(Bits destination, bool value) {
    clear(destination);
    if (destination.width != 0) destination.data[0] = value ? UINT64_C(1) : UINT64_C(0);
}

inline void op_clog2(Bits destination, ConstBits source) {
    std::uint64_t highest = 0;
    bool seen = false;
    bool multiple = false;
    for (std::uint64_t bit = 0; bit < source.width; ++bit) {
        if (!get_bit(source, bit)) continue;
        multiple |= seen;
        seen = true;
        highest = bit;
    }
    clear(destination);
    if (destination.width != 0 && seen) {
        destination.data[0] = highest + (multiple ? UINT64_C(1) : UINT64_C(0));
        truncate(destination);
    }
}

inline void op_shift(Bits destination, ConstBits source, ConstBits amount, bool left, bool arithmetic) {
    std::uint64_t shift = 0;
    if (!to_index(amount, shift) || shift >= destination.width) {
        clear(destination);
        if (!left && arithmetic && sign_bit(source)) {
            std::fill_n(destination.data, word_count(destination.width), ~UINT64_C(0));
            truncate(destination);
        }
        return;
    }
    clear(destination);
    for (std::uint64_t bit = 0; bit < destination.width; ++bit) {
        bool value = false;
        if (left) {
            value = bit >= shift && get_bit(source, bit - shift);
        } else if (bit + shift < source.width) {
            value = get_bit(source, bit + shift);
        } else if (arithmetic) {
            value = sign_bit(source);
        }
        set_bit(destination, bit, value);
    }
}

inline void op_concat(Bits destination, std::initializer_list<ConstBits> operands) {
    clear(destination);
    std::uint64_t offset = destination.width;
    for (ConstBits operand : operands) {
        offset -= operand.width;
        for (std::uint64_t bit = 0; bit < operand.width; ++bit) {
            set_bit(destination, offset + bit, get_bit(operand, bit));
        }
    }
}

inline void op_replicate(Bits destination, ConstBits source) {
    clear(destination);
    for (std::uint64_t bit = 0; bit < destination.width; ++bit) {
        set_bit(destination, bit, get_bit(source, bit % source.width));
    }
}

inline void op_slice(Bits destination, ConstBits source, std::uint64_t lsb, bool valid = true) {
    clear(destination);
    if (!valid || lsb >= source.width) return;
    for (std::uint64_t bit = 0; bit < destination.width && lsb + bit < source.width; ++bit) {
        set_bit(destination, bit, get_bit(source, lsb + bit));
    }
}

inline void op_array_mux(Bits destination, ConstBits select, ConstBits when_true, ConstBits when_false) {
    clear(destination);
    const std::uint32_t rows = select.width;
    const std::uint32_t element_width = rows == 0 ? 0 : destination.width / rows;
    for (std::uint32_t row = 0; row < rows; ++row) {
        const ConstBits source = get_bit(select, row) ? when_true : when_false;
        for (std::uint32_t bit = 0; bit < element_width; ++bit) {
            const std::uint64_t index = static_cast<std::uint64_t>(row) * element_width + bit;
            set_bit(destination, index, get_bit(source, index));
        }
    }
}

inline void op_onehot(Bits destination, ConstBits index) {
    clear(destination);
    std::uint64_t raw = 0;
    if (to_index(index, raw) && raw < destination.width) set_bit(destination, raw, true);
}

enum class ReduceKind : std::uint8_t { And, Or, Xor };

inline bool reduce_range(ConstBits source, std::uint64_t begin, std::uint32_t width, ReduceKind kind) {
    bool result = kind == ReduceKind::And;
    for (std::uint32_t bit = 0; bit < width; ++bit) {
        const bool value = get_bit(source, begin + bit);
        if (kind == ReduceKind::And) result &= value;
        else if (kind == ReduceKind::Or) result |= value;
        else result ^= value;
    }
    return result;
}

inline void op_array_reduce(Bits destination, ConstBits source, std::uint32_t element_width,
                            ReduceKind kind, bool lanes) {
    clear(destination);
    const std::uint32_t rows = element_width == 0 ? 0 : source.width / element_width;
    if (!lanes) {
        bool result = kind == ReduceKind::And;
        for (std::uint32_t row = 0; row < rows; ++row) {
            const bool lane = reduce_range(source, static_cast<std::uint64_t>(row) * element_width,
                                           element_width, kind);
            if (kind == ReduceKind::And) result &= lane;
            else if (kind == ReduceKind::Or) result |= lane;
            else result ^= lane;
        }
        set_bool(destination, result);
        return;
    }
    for (std::uint32_t row = 0; row < rows; ++row) {
        set_bit(destination, row,
                reduce_range(source, static_cast<std::uint64_t>(row) * element_width,
                             element_width, kind));
    }
}

inline void op_broadcast(Bits destination, ConstBits source) { op_replicate(destination, source); }

inline void copy_range(Bits destination, std::uint64_t destination_lsb,
                       ConstBits source, std::uint64_t source_lsb, std::uint64_t width) {
    for (std::uint64_t bit = 0; bit < width; ++bit) {
        set_bit(destination, destination_lsb + bit, get_bit(source, source_lsb + bit));
    }
}

inline void masked_merge(Bits destination, ConstBits data, ConstBits mask) {
    for (std::uint64_t bit = 0; bit < destination.width; ++bit) {
        if (get_bit(mask, bit)) set_bit(destination, bit, get_bit(data, bit));
    }
}

inline std::uint64_t splitmix64(std::uint64_t &state) {
    state += UINT64_C(0x9e3779b97f4a7c15);
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

inline void op_array_lane_const(Bits destination, std::uint32_t element_width,
                                std::span<const std::int64_t> values) {
    clear(destination);
    for (std::size_t row = 0; row < values.size(); ++row) {
        const std::uint64_t raw = static_cast<std::uint64_t>(values[row]);
        for (std::uint32_t bit = 0; bit < element_width; ++bit) {
            const bool value = bit < 64U ? ((raw >> bit) & UINT64_C(1)) != 0
                                         : values[row] < 0;
            set_bit(destination, static_cast<std::uint64_t>(row) * element_width + bit, value);
        }
    }
}

enum class PendingWriteKind : std::uint8_t { Single, Lanes, Fill };

struct PendingWriteDesc {
    PendingWriteKind kind = PendingWriteKind::Single;
    std::size_t state_offset = 0;
    std::uint32_t state_width = 0;
    std::uint32_t rows = 0;
    std::uint32_t element_width = 0;
    std::size_t data_offset = 0;
    std::size_t mask_offset = 0;
};

inline void apply_pending_write(const PendingWriteDesc &desc,
                                std::uint64_t address,
                                std::vector<std::uint64_t> &state_words,
                                const std::vector<std::uint64_t> &pending_words) {
    Bits state{state_words.data() + desc.state_offset, desc.state_width};
    if (desc.kind == PendingWriteKind::Single) {
        if (address >= desc.rows) return;
        ConstBits data{pending_words.data() + desc.data_offset, desc.element_width};
        ConstBits mask{pending_words.data() + desc.mask_offset, desc.element_width};
        const std::uint64_t base = address * desc.element_width;
        for (std::uint32_t bit = 0; bit < desc.element_width; ++bit) {
            if (get_bit(mask, bit)) set_bit(state, base + bit, get_bit(data, bit));
        }
        return;
    }
    if (desc.kind == PendingWriteKind::Lanes) {
        ConstBits lanes{pending_words.data() + desc.mask_offset, desc.rows};
        ConstBits data{pending_words.data() + desc.data_offset, desc.state_width};
        for (std::uint32_t row = 0; row < desc.rows; ++row) {
            if (get_bit(lanes, row)) {
                copy_range(state, static_cast<std::uint64_t>(row) * desc.element_width,
                           data, static_cast<std::uint64_t>(row) * desc.element_width,
                           desc.element_width);
            }
        }
        return;
    }
    copy(state, ConstBits{pending_words.data() + desc.data_offset, desc.state_width});
}

enum class TaskArgKind : std::uint8_t { Logic, Real, String };

struct TaskArg {
    TaskArgKind kind = TaskArgKind::Logic;
    ConstBits logic{};
    bool is_signed = false;
    double real = 0.0;
    std::string_view string{};

    static TaskArg from_logic(ConstBits value, bool signed_value = false) {
        TaskArg result;
        result.logic = value;
        result.is_signed = signed_value;
        return result;
    }
    static TaskArg from_real(double value) {
        TaskArg result;
        result.kind = TaskArgKind::Real;
        result.real = value;
        return result;
    }
    static TaskArg from_string(std::string_view value) {
        TaskArg result;
        result.kind = TaskArgKind::String;
        result.string = value;
        return result;
    }
};

inline std::string logic_text(const TaskArg &arg, char format) {
    if (format == 'b') {
        std::string result;
        result.reserve(arg.logic.width);
        for (std::uint32_t bit = arg.logic.width; bit-- > 0;) {
            result.push_back(get_bit(arg.logic, bit) ? '1' : '0');
        }
        return result.empty() ? std::string("0") : result;
    }
    std::vector<std::uint64_t> words(arg.logic.data,
                                     arg.logic.data + word_count(arg.logic.width));
    Bits value{words.data(), arg.logic.width};
    truncate(value);
    const bool negative = format == 'd' && arg.is_signed && sign_bit(as_const(value));
    if (negative) negate(value);
    const std::uint32_t base = format == 'h' || format == 'x' ? 16U
                               : format == 'o'                ? 8U
                                                              : 10U;
    if (!any(as_const(value))) return "0";
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    while (any(as_const(value))) {
        unsigned __int128 carry = 0;
        for (std::size_t index = words.size(); index-- > 0;) {
            const unsigned __int128 current = (carry << 64U) | words[index];
            words[index] = static_cast<std::uint64_t>(current / base);
            carry = current % base;
        }
        result.push_back(digits[static_cast<std::size_t>(carry)]);
    }
    if (negative) result.push_back('-');
    std::reverse(result.begin(), result.end());
    return result;
}

inline std::string task_arg_text(const TaskArg &arg, char format) {
    if (format == 's') {
        return arg.kind == TaskArgKind::String ? std::string(arg.string) : std::string{};
    }
    if (format == 'c' && arg.kind == TaskArgKind::Logic) {
        return std::string(1, static_cast<char>(to_u64(arg.logic) & UINT64_C(0xff)));
    }
    if (arg.kind == TaskArgKind::String) return std::string(arg.string);
    if (arg.kind == TaskArgKind::Real) {
        std::ostringstream stream;
        stream << arg.real;
        return stream.str();
    }
    return logic_text(arg, format == 'x' ? 'h' : format);
}

inline std::string format_task_message(std::span<const TaskArg> args) {
    if (args.empty()) return {};
    if (args.front().kind != TaskArgKind::String) {
        std::string result;
        for (std::size_t index = 0; index < args.size(); ++index) {
            if (index != 0) result.push_back(' ');
            result += task_arg_text(args[index], 'd');
        }
        return result;
    }

    const std::string_view format = args.front().string;
    std::size_t argument = 1;
    std::string result;
    for (std::size_t index = 0; index < format.size(); ++index) {
        if (format[index] != '%' || index + 1U >= format.size()) {
            result.push_back(format[index]);
            continue;
        }
        if (format[index + 1U] == '%') {
            result.push_back('%');
            ++index;
            continue;
        }
        ++index;
        bool left = false;
        char fill = ' ';
        if (format[index] == '-') {
            left = true;
            ++index;
        }
        if (index < format.size() && format[index] == '0') {
            fill = '0';
            ++index;
        }
        std::size_t field_width = 0;
        while (index < format.size() && format[index] >= '0' && format[index] <= '9') {
            field_width = field_width * 10U + static_cast<std::size_t>(format[index] - '0');
            ++index;
        }
        while (index < format.size() &&
               (format[index] == 'l' || format[index] == 'L' || format[index] == 'z')) ++index;
        if (index >= format.size()) break;
        const char specifier = format[index];
        if (specifier == 'm') {
            result += "top";
            continue;
        }
        if (argument >= args.size()) {
            result.push_back('%');
            result.push_back(specifier);
            continue;
        }
        std::string item = task_arg_text(args[argument++], specifier);
        if (item.size() < field_width) {
            const std::string padding(field_width - item.size(), fill);
            item = left ? item + padding : padding + item;
        }
        result += item;
    }
    return result;
}

inline std::string format_task_message(std::initializer_list<TaskArg> args) {
    return format_task_message(std::span<const TaskArg>(args.begin(), args.size()));
}

template <typename T>
inline void import_port(Bits destination, const T &source) {
    static_assert(std::is_integral_v<T> || std::is_same_v<T, bool>);
    clear(destination);
    destination.data[0] = static_cast<std::uint64_t>(source);
    truncate(destination);
}

template <typename T, std::size_t N>
inline void import_port(Bits destination, const std::array<T, N> &source) {
    clear(destination);
    const std::size_t count = std::min(N, word_count(destination.width));
    for (std::size_t index = 0; index < count; ++index) destination.data[index] = source[index];
    truncate(destination);
}

template <typename T>
inline void export_port(T &destination, ConstBits source) {
    static_assert(std::is_integral_v<T> || std::is_same_v<T, bool>);
    destination = static_cast<T>(to_u64(source));
}

template <typename T, std::size_t N>
inline void export_port(std::array<T, N> &destination, ConstBits source) {
    for (std::size_t index = 0; index < N; ++index) {
        destination[index] = static_cast<T>(extended_word(source, index, false));
    }
}

} // namespace wolvrix_grhsim_cpu_runtime
)WCPU"
