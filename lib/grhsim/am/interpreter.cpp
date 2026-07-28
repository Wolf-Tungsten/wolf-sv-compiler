#include "grhsim/am/interpreter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace wolvrix::lib::grhsim::am {

    namespace {
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
            return word < words.size() &&
                   ((words[word] >> (index % 64U)) & 1U) != 0;
        }

        void setBit(std::vector<uint64_t> &words, uint64_t index,
                    bool value = true) {
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

        std::vector<uint64_t> resizedWords(const InterpreterValue &value,
                                           uint32_t width,
                                           Signedness extension) {
            std::vector<uint64_t> result(wordCount(width), 0);
            const Type &sourceType = value.type();
            const std::size_t copied =
                std::min(result.size(), value.words().size());
            std::copy_n(value.words().begin(), copied, result.begin());
            if (width > sourceType.bitWidth &&
                extension == Signedness::Signed &&
                getBit(value.words(), sourceType.bitWidth - 1U)) {
                for (uint64_t bit = sourceType.bitWidth; bit < width; ++bit) {
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
                                       std::span<const uint64_t> rhs,
                                       uint32_t width) {
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
                const bool nextBorrow =
                    additionOverflow || lhs[index] < withBorrow;
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
            for (std::size_t leftIndex = 0; leftIndex < lhs.size();
                 ++leftIndex) {
                unsigned __int128 carry = 0;
                for (std::size_t rightIndex = 0;
                     rightIndex < rhs.size() &&
                     leftIndex + rightIndex < result.size();
                     ++rightIndex) {
                    const std::size_t resultIndex = leftIndex + rightIndex;
                    const unsigned __int128 product =
                        static_cast<unsigned __int128>(lhs[leftIndex]) *
                            rhs[rightIndex] +
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
            const std::size_t wordShift =
                static_cast<std::size_t>(amount / 64U);
            const uint32_t bitShift = amount % 64U;
            for (std::size_t sourceIndex = 0; sourceIndex < source.size();
                 ++sourceIndex) {
                const std::size_t targetIndex = sourceIndex + wordShift;
                if (targetIndex < result.size()) {
                    result[targetIndex] |= source[sourceIndex] << bitShift;
                }
                if (bitShift != 0 && targetIndex + 1U < result.size()) {
                    result[targetIndex + 1U] |=
                        source[sourceIndex] >> (64U - bitShift);
                }
            }
            normalizeWords(result, width);
            return result;
        }

        std::vector<uint64_t> shiftRight(std::span<const uint64_t> source,
                                         uint32_t width, uint64_t amount,
                                         bool signFill) {
            std::vector<uint64_t> result(wordCount(width),
                                         signFill ? UINT64_MAX : 0);
            if (amount < width) {
                std::fill(result.begin(), result.end(), 0);
                const std::size_t wordShift =
                    static_cast<std::size_t>(amount / 64U);
                const uint32_t bitShift = amount % 64U;
                for (std::size_t targetIndex = 0; targetIndex < result.size();
                     ++targetIndex) {
                    const std::size_t sourceIndex = targetIndex + wordShift;
                    if (sourceIndex < source.size()) {
                        result[targetIndex] |= source[sourceIndex] >> bitShift;
                    }
                    if (bitShift != 0 && sourceIndex + 1U < source.size()) {
                        result[targetIndex] |= source[sourceIndex + 1U]
                                               << (64U - bitShift);
                    }
                }
                if (signFill) {
                    for (uint64_t bit = static_cast<uint64_t>(width) - amount;
                         bit < width; ++bit) {
                        setBit(result, bit);
                    }
                }
            }
            normalizeWords(result, width);
            return result;
        }

        uint64_t shiftAmount(const InterpreterValue &value, uint32_t limit) {
            const auto words = value.words();
            for (std::size_t index = 1; index < words.size(); ++index) {
                if (words[index] != 0) {
                    return limit;
                }
            }
            return words.empty() ? 0 : std::min<uint64_t>(words.front(), limit);
        }

        InterpreterValue makeBitValue(const Type &type,
                                      std::vector<uint64_t> words) {
            return InterpreterValue::bitVector(type.bitWidth, type.signedness,
                                               words);
        }

        uint64_t splitMix64(uint64_t &state) {
            state += UINT64_C(0x9e3779b97f4a7c15);
            uint64_t value = state;
            value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
            value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
            return value ^ (value >> 31U);
        }

        std::string defaultHostError(std::string_view operation) {
            return "HostEnvironment does not provide " + std::string(operation);
        }

    } // namespace

    InterpreterValue::InterpreterValue() = default;

    InterpreterValue::InterpreterValue(const Type &type,
                                       std::vector<uint64_t> words,
                                       std::string bytes)
        : type_(type), words_(std::move(words)), bytes_(std::move(bytes)) {}

    InterpreterValue InterpreterValue::zero(const Type &type) {
        switch (type.kind) {
        case TypeKind::BitVector:
            return InterpreterValue(
                type, std::vector<uint64_t>(wordCount(type.bitWidth), 0), {});
        case TypeKind::Real:
            return InterpreterValue(type, std::vector<uint64_t>{0}, {});
        case TypeKind::String:
            return InterpreterValue(type, {}, {});
        case TypeKind::Array:
            return InterpreterValue(
                type,
                std::vector<uint64_t>(
                    wordCount(type.bitWidth) * type.elementCount, 0),
                {});
        }
        throw std::invalid_argument("invalid AM TypeKind");
    }

    InterpreterValue
    InterpreterValue::bitVector(uint32_t width, Signedness signedness,
                                std::span<const uint64_t> words) {
        const Type type = Type::bitVector(width, signedness);
        if (words.size() != wordCount(width)) {
            throw std::invalid_argument(
                "AM interpreter bit-vector payload has the wrong word count");
        }
        std::vector<uint64_t> payload(words.begin(), words.end());
        normalizeWords(payload, width);
        return InterpreterValue(type, std::move(payload), {});
    }

    InterpreterValue InterpreterValue::real(uint64_t bits) {
        return InterpreterValue(Type::real(), std::vector<uint64_t>{bits}, {});
    }

    InterpreterValue InterpreterValue::string(std::string_view bytes) {
        return InterpreterValue(Type::string(), {}, std::string(bytes));
    }

    InterpreterValue
    InterpreterValue::array(uint32_t elementCount, uint32_t elementWidth,
                            Signedness signedness,
                            std::span<const uint64_t> elementWords) {
        const Type type = Type::array(elementCount, elementWidth, signedness);
        const std::size_t stride = wordCount(elementWidth);
        if (elementWords.size() != stride * elementCount) {
            throw std::invalid_argument(
                "AM interpreter array payload has the wrong word count");
        }
        std::vector<uint64_t> payload(elementWords.begin(), elementWords.end());
        for (std::size_t element = 0; element < elementCount; ++element) {
            payload[(element + 1U) * stride - 1U] &= highWordMask(elementWidth);
        }
        return InterpreterValue(type, std::move(payload), {});
    }

    std::span<const uint64_t>
    InterpreterValue::arrayElementWords(std::size_t index) const {
        if (type_.kind != TypeKind::Array || index >= type_.elementCount) {
            throw std::out_of_range(
                "invalid AM interpreter array element access");
        }
        const std::size_t stride = wordCount(type_.bitWidth);
        return std::span<const uint64_t>(words_.data() + index * stride,
                                         stride);
    }

    uint64_t InterpreterValue::realBits() const {
        if (type_.kind != TypeKind::Real) {
            throw std::logic_error("AM interpreter value is not Real");
        }
        return words_.front();
    }

    bool HostEnvironment::resolveSystemFunction(ProgramView, InstructionId,
                                                std::string &error) {
        error = defaultHostError("a system-function binding");
        return false;
    }

    bool HostEnvironment::resolveSystemTask(ProgramView, InstructionId,
                                            std::string &error) {
        error = defaultHostError("a system-task binding");
        return false;
    }

    bool HostEnvironment::resolveDpiCall(ProgramView, InstructionId,
                                         std::string &error) {
        error = defaultHostError("a DPI binding");
        return false;
    }

    bool HostEnvironment::invokeSystemFunction(
        ProgramView, InstructionId, std::span<const InterpreterValue>,
        InterpreterValue &, std::string &error) {
        error = defaultHostError("system-function invocation");
        return false;
    }

    bool HostEnvironment::invokeSystemTask(ProgramView, InstructionId,
                                           std::span<const InterpreterValue>,
                                           std::string &error) {
        error = defaultHostError("system-task invocation");
        return false;
    }

    bool HostEnvironment::invokeDpiCall(ProgramView, InstructionId,
                                        std::span<const InterpreterValue>,
                                        std::vector<InterpreterValue> &,
                                        std::string &error) {
        error = defaultHostError("DPI invocation");
        return false;
    }

    bool HostEnvironment::readInitFile(std::string_view, std::string &,
                                       std::string &error) {
        error = defaultHostError("init-file loading");
        return false;
    }

    struct Interpreter::Impl {
        const ExecutableModel &model;
        ProgramView program;
        HostEnvironment *host = nullptr;
        InterpreterOptions options;
        std::vector<InterpreterValue> values;
        std::vector<bool> constants;
        std::vector<bool> protectedVariables;
        std::vector<VariableId> changedResults;
        std::vector<uint64_t> dirtyChangedBits;
        std::vector<VariableId> dirtyChangedResults;
        std::vector<bool> active;
        std::vector<bool> callCompleted;
        std::vector<bool> pendingHostEvents;
        std::vector<BlockId> instructionBlocks;
        std::unordered_set<uint32_t> crossBlockChangedResults;
        std::optional<InterpreterDiagnostic> initializationDiagnostic;
        bool firstEval = true;
        bool finalized = false;
        bool failed = false;
        bool successfulEval = false;
        bool dirtySinceEval = false;
        bool backwardFired = false;
        uint64_t roundCounter = 0;
        uint64_t randomState = 0;

        Impl(const ExecutableModel &model, HostEnvironment *host,
             const InterpreterOptions &options)
            : model(model), program(model.program.view()), host(host),
              options(options), randomState(options.randomSeed) {
            initialize();
        }

        InterpreterDiagnostic
        diagnostic(InterpreterErrorCode code, std::string message,
                   BlockId block = BlockId::invalid(),
                   InstructionId instruction = InstructionId::invalid(),
                   VariableId variable = VariableId::invalid()) const {
            return InterpreterDiagnostic{
                .code = code,
                .message = std::move(message),
                .block = block,
                .instruction = instruction,
                .variable = variable,
            };
        }

        InterpreterResult
        fail(InterpreterErrorCode code, std::string message,
             BlockId block = BlockId::invalid(),
             InstructionId instruction = InstructionId::invalid(),
             VariableId variable = VariableId::invalid()) {
            failed = true;
            return InterpreterResult{
                .diagnostic = diagnostic(code, std::move(message), block,
                                         instruction, variable),
                .roundsExecuted = roundCounter + 1U,
            };
        }

        const Type &variableType(VariableId variable) const {
            return program.type(program.variable(variable).type);
        }

        InterpreterValue literalValue(LiteralId literal,
                                      const Type &expected) const {
            const LiteralView source = program.literal(literal);
            const Type &type = program.type(source.type);
            if (type != expected) {
                throw std::logic_error(
                    "AM init literal Type does not match its target");
            }
            switch (type.kind) {
            case TypeKind::BitVector:
                return InterpreterValue::bitVector(
                    type.bitWidth, type.signedness, source.words);
            case TypeKind::Real:
                if (source.words.size() != 1) {
                    throw std::logic_error(
                        "AM Real literal has the wrong payload size");
                }
                return InterpreterValue::real(source.words.front());
            case TypeKind::String:
                return InterpreterValue::string(source.bytes);
            case TypeKind::Array:
                break;
            }
            throw std::logic_error("AM Array cannot be a scalar literal");
        }

        InterpreterValue randomBitValue(const Type &type,
                                        uint64_t &state) const {
            std::vector<uint64_t> words(wordCount(type.bitWidth));
            for (uint64_t &word : words) {
                word = splitMix64(state);
            }
            normalizeWords(words, type.bitWidth);
            return InterpreterValue::bitVector(type.bitWidth, type.signedness,
                                               words);
        }

        InterpreterValue evaluateInitExpr(const InitExpr &expression,
                                          const Type &type,
                                          uint64_t &seedState) const {
            if (expression.kind == InitExprKind::Literal) {
                return literalValue(expression.literal, type);
            }
            if (type.kind != TypeKind::BitVector) {
                throw std::logic_error(
                    "AM random init requires a bit-vector target");
            }
            return randomBitValue(type, seedState);
        }

        static void assignArrayElement(InterpreterValue &array,
                                       std::size_t element,
                                       const InterpreterValue &value) {
            const std::size_t stride = wordCount(array.type_.bitWidth);
            std::copy(value.words_.begin(), value.words_.end(),
                      array.words_.begin() + element * stride);
        }

        bool initializeVariable(VariableId variable, std::string &error) {
            const VariableRecord &record = program.variable(variable);
            const Type &type = program.type(record.type);
            const InitDescriptor &init = program.init(record.init);
            InterpreterValue value = InterpreterValue::zero(type);
            constants[variable.value] = init.kind == InitKind::Constant;

            try {
                if (init.kind == InitKind::Constant) {
                    value = literalValue(LiteralId{init.payload}, type);
                } else if (init.kind == InitKind::Actions) {
                    const auto actions = program.initActions(record.init);
                    for (std::size_t actionIndex = 0;
                         actionIndex < actions.size(); ++actionIndex) {
                        const InitAction &action = actions[actionIndex];
                        if (action.kind == InitActionKind::Load) {
                            error = "AM interpreter init-file loading is not "
                                    "implemented";
                            return false;
                        }
                        uint64_t expressionState =
                            action.expression.kind == InitExprKind::RandomSeeded
                                ? action.expression.seed
                                : randomState;
                        if (action.kind == InitActionKind::Set) {
                            value = evaluateInitExpr(action.expression, type,
                                                     expressionState);
                        } else {
                            const Type elementType =
                                Type::bitVector(type.bitWidth, type.signedness);
                            const uint64_t end = action.start + action.count;
                            for (uint64_t element = action.start; element < end;
                                 ++element) {
                                const InterpreterValue elementValue =
                                    evaluateInitExpr(action.expression,
                                                     elementType,
                                                     expressionState);
                                assignArrayElement(
                                    value, static_cast<std::size_t>(element),
                                    elementValue);
                            }
                        }
                        if (action.expression.kind == InitExprKind::Random) {
                            randomState = expressionState;
                        }
                    }
                }
            } catch (const std::exception &exception) {
                error = exception.what();
                return false;
            }
            values.push_back(std::move(value));
            return true;
        }

        void initialize() {
            if (options.maxRounds == 0) {
                initializationDiagnostic =
                    diagnostic(InterpreterErrorCode::InvalidModel,
                               "AM interpreter maxRounds must be positive");
                return;
            }
            const ValidationResult validation = validate(
                model, ValidationOptions{.level = options.validationLevel,
                                         .maxErrors = 1});
            if (!validation.success()) {
                initializationDiagnostic = diagnostic(
                    InterpreterErrorCode::InvalidModel,
                    "invalid AM ExecutableModel: " + validation.errors.front());
                return;
            }

            constants.assign(program.variableCount(), false);
            protectedVariables.assign(program.variableCount(), false);
            values.reserve(program.variableCount());
            for (uint32_t index = 0; index < program.variableCount(); ++index) {
                std::string error;
                if (!initializeVariable(VariableId{index}, error)) {
                    initializationDiagnostic =
                        diagnostic(InterpreterErrorCode::InitializationFailed,
                                   "failed to initialize AM variable %" +
                                       std::to_string(index) + ": " + error,
                                   BlockId::invalid(), InstructionId::invalid(),
                                   VariableId{index});
                    return;
                }
            }

            instructionBlocks.assign(program.instructionCount(),
                                     BlockId::invalid());
            for (uint32_t blockIndex = 0;
                 blockIndex < model.program.blockCount(); ++blockIndex) {
                const BlockId block{blockIndex};
                for (std::size_t position = 0;
                     position < model.program.blockSize(block); ++position) {
                    instructionBlocks[model.program
                                          .blockInstruction(block, position)
                                          .value] = block;
                }
            }

            std::vector<BlockId> changedProducerBlocks(program.variableCount(),
                                                       BlockId::invalid());
            for (uint32_t index = 0; index < program.instructionCount();
                 ++index) {
                const InstructionId instruction{index};
                const Opcode opcode = program.opcode(instruction);
                if (opcode == Opcode::ChangedAny ||
                    opcode == Opcode::ChangedPos ||
                    opcode == Opcode::ChangedNeg) {
                    const auto operands = program.operands(instruction);
                    const auto results = program.results(instruction);
                    protectedVariables[operands[1].value] = true;
                    protectedVariables[results[0].value] = true;
                    changedResults.push_back(results[0]);
                    changedProducerBlocks[results[0].value] =
                        instructionBlocks[index];
                }

                std::string error;
                bool resolved = true;
                if (opcode == Opcode::SystemFunction) {
                    resolved = host && host->resolveSystemFunction(
                                           program, instruction, error);
                } else if (opcode == Opcode::SystemTask) {
                    resolved = host && host->resolveSystemTask(
                                           program, instruction, error);
                } else if (opcode == Opcode::DpiCall) {
                    resolved = host && host->resolveDpiCall(program,
                                                            instruction, error);
                }
                if (!resolved) {
                    if (error.empty()) {
                        error = "no HostEnvironment was supplied";
                    }
                    initializationDiagnostic = diagnostic(
                        InterpreterErrorCode::MissingHostBinding,
                        "failed to resolve " + std::string(toString(opcode)) +
                            " instruction " + std::to_string(index) + ": " +
                            error,
                        instructionBlocks[index], instruction);
                    return;
                }
            }

            // A changed result consumed by at least one instruction in a
            // different Block is round-local state and must be cleared at the
            // end of every round; same-Block results are rewritten before
            // every read and need no clearing.
            for (uint32_t index = 0; index < program.instructionCount();
                 ++index) {
                const InstructionId instruction{index};
                const BlockId consumerBlock = instructionBlocks[index];
                for (VariableId operand : program.operands(instruction)) {
                    const BlockId producerBlock =
                        changedProducerBlocks[operand.value];
                    if (producerBlock.valid() &&
                        consumerBlock != producerBlock) {
                        crossBlockChangedResults.insert(operand.value);
                    }
                }
            }

            active.assign(model.program.blockCount(), false);
            dirtyChangedBits.assign(
                (static_cast<std::size_t>(program.variableCount()) + 63U) / 64U,
                0);
            dirtyChangedResults.reserve(changedResults.size());
            callCompleted.assign(program.instructionCount(), false);
            pendingHostEvents.assign(program.instructionCount(), false);
            for (VariableId result : changedResults) {
                setChangedResult(result, false);
            }
            clearChangedResults();
        }

        void setChangedResult(VariableId result, bool event) {
            values[result.value] = InterpreterValue::bitVector(
                1, Signedness::Unsigned,
                std::array<uint64_t, 1>{event ? 1U : 0U});

            if (!event || !crossBlockChangedResults.contains(result.value)) {
                return;
            }

            const std::size_t word = result.value / 64U;
            const uint64_t bit = UINT64_C(1) << (result.value % 64U);
            if ((dirtyChangedBits[word] & bit) != 0) {
                return;
            }
            dirtyChangedBits[word] |= bit;
            dirtyChangedResults.push_back(result);
        }

        void clearChangedResults() {
            for (VariableId result : dirtyChangedResults) {
                values[result.value] =
                    InterpreterValue::zero(variableType(result));
                dirtyChangedBits[result.value / 64U] &=
                    ~(UINT64_C(1) << (result.value % 64U));
            }
            dirtyChangedResults.clear();
        }

        static bool truth(const InterpreterValue &value) {
            return !isZero(value.words());
        }

        std::vector<InterpreterValue>
        snapshot(std::span<const VariableId> operands) const {
            std::vector<InterpreterValue> result;
            result.reserve(operands.size());
            for (VariableId operand : operands) {
                result.push_back(values[operand.value]);
            }
            return result;
        }

        InterpreterResult
        executePure(BlockId block, InstructionId instruction, Opcode opcode,
                    std::span<const VariableId> results,
                    const std::vector<InterpreterValue> &operands) {
            const Type &resultType = variableType(results[0]);
            std::vector<uint64_t> resultWords;
            const auto unaryWords = [&]() {
                return resizedWords(operands[0], resultType.bitWidth,
                                    operands[0].type().signedness);
            };
            const auto binaryWords = [&]() {
                const Signedness common =
                    operands[0].type().signedness == Signedness::Signed &&
                            operands[1].type().signedness == Signedness::Signed
                        ? Signedness::Signed
                        : Signedness::Unsigned;
                return std::pair{
                    resizedWords(operands[0], resultType.bitWidth, common),
                    resizedWords(operands[1], resultType.bitWidth, common),
                };
            };

            switch (opcode) {
            case Opcode::Assign:
                resultWords = unaryWords();
                break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::Xnor: {
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
                            signedOperation &&
                            getBit(lhs, resultType.bitWidth - 1U);
                        const bool rhsNegative =
                            signedOperation &&
                            getBit(rhs, resultType.bitWidth - 1U);
                        const std::vector<uint64_t> magnitudeLhs =
                            lhsNegative ? negateWords(lhs, resultType.bitWidth)
                                        : lhs;
                        const std::vector<uint64_t> magnitudeRhs =
                            rhsNegative ? negateWords(rhs, resultType.bitWidth)
                                        : rhs;
                        auto [quotient, remainder] = divideUnsigned(
                            magnitudeLhs, magnitudeRhs, resultType.bitWidth);
                        if (opcode == Opcode::Div) {
                            resultWords =
                                lhsNegative != rhsNegative
                                    ? negateWords(quotient, resultType.bitWidth)
                                    : std::move(quotient);
                        } else {
                            resultWords = lhsNegative
                                              ? negateWords(remainder,
                                                            resultType.bitWidth)
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
                break;
            }
            case Opcode::Not:
                resultWords = unaryWords();
                for (uint64_t &word : resultWords) {
                    word = ~word;
                }
                normalizeWords(resultWords, resultType.bitWidth);
                break;
            case Opcode::Eq:
            case Opcode::Ne:
            case Opcode::Lt:
            case Opcode::Le:
            case Opcode::Gt:
            case Opcode::Ge: {
                const uint32_t width = std::max(operands[0].type().bitWidth,
                                                operands[1].type().bitWidth);
                const Signedness common =
                    operands[0].type().signedness == Signedness::Signed &&
                            operands[1].type().signedness == Signedness::Signed
                        ? Signedness::Signed
                        : Signedness::Unsigned;
                const auto lhs = resizedWords(operands[0], width, common);
                const auto rhs = resizedWords(operands[1], width, common);
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
                if (opcode == Opcode::Eq)
                    predicate = ordering == 0;
                else if (opcode == Opcode::Ne)
                    predicate = ordering != 0;
                else if (opcode == Opcode::Lt)
                    predicate = ordering < 0;
                else if (opcode == Opcode::Le)
                    predicate = ordering <= 0;
                else if (opcode == Opcode::Gt)
                    predicate = ordering > 0;
                else
                    predicate = ordering >= 0;
                resultWords = {predicate ? 1U : 0U};
                break;
            }
            case Opcode::LogicAnd:
                resultWords = {truth(operands[0]) && truth(operands[1]) ? 1U
                                                                        : 0U};
                break;
            case Opcode::LogicOr:
                resultWords = {truth(operands[0]) || truth(operands[1]) ? 1U
                                                                        : 0U};
                break;
            case Opcode::LogicNot:
                resultWords = {!truth(operands[0]) ? 1U : 0U};
                break;
            case Opcode::ReduceAnd:
            case Opcode::ReduceNand:
            case Opcode::ReduceOr:
            case Opcode::ReduceNor:
            case Opcode::ReduceXor:
            case Opcode::ReduceXnor: {
                bool reduced = false;
                if (opcode == Opcode::ReduceAnd ||
                    opcode == Opcode::ReduceNand) {
                    reduced = true;
                    for (uint64_t bit = 0; bit < operands[0].type().bitWidth;
                         ++bit) {
                        reduced = reduced && getBit(operands[0].words(), bit);
                    }
                    if (opcode == Opcode::ReduceNand)
                        reduced = !reduced;
                } else if (opcode == Opcode::ReduceOr ||
                           opcode == Opcode::ReduceNor) {
                    reduced = truth(operands[0]);
                    if (opcode == Opcode::ReduceNor)
                        reduced = !reduced;
                } else {
                    unsigned parity = 0;
                    for (uint64_t word : operands[0].words()) {
                        parity ^= std::popcount(word) & 1U;
                    }
                    reduced = parity != 0;
                    if (opcode == Opcode::ReduceXnor)
                        reduced = !reduced;
                }
                resultWords = {reduced ? 1U : 0U};
                break;
            }
            case Opcode::Shl:
            case Opcode::LogicalShr:
            case Opcode::ArithmeticShr: {
                const uint64_t amount =
                    shiftAmount(operands[1], resultType.bitWidth);
                if (opcode == Opcode::Shl) {
                    resultWords = shiftLeft(operands[0].words(),
                                            resultType.bitWidth, amount);
                } else {
                    const bool signFill =
                        opcode == Opcode::ArithmeticShr &&
                        operands[0].type().signedness == Signedness::Signed &&
                        getBit(operands[0].words(), resultType.bitWidth - 1U);
                    resultWords =
                        shiftRight(operands[0].words(), resultType.bitWidth,
                                   amount, signFill);
                }
                break;
            }
            case Opcode::Mux: {
                const InterpreterValue &selected =
                    truth(operands[0]) ? operands[1] : operands[2];
                const Signedness common =
                    operands[1].type().signedness == Signedness::Signed &&
                            operands[2].type().signedness == Signedness::Signed
                        ? Signedness::Signed
                        : Signedness::Unsigned;
                resultWords =
                    resizedWords(selected, resultType.bitWidth, common);
                break;
            }
            case Opcode::Concat: {
                resultWords.assign(wordCount(resultType.bitWidth), 0);
                uint64_t destination = 0;
                for (std::size_t offset = 0; offset < operands.size();
                     ++offset) {
                    const InterpreterValue &source =
                        operands[operands.size() - offset - 1U];
                    for (uint64_t bit = 0; bit < source.type().bitWidth;
                         ++bit) {
                        setBit(resultWords, destination + bit,
                               getBit(source.words(), bit));
                    }
                    destination += source.type().bitWidth;
                }
                break;
            }
            case Opcode::Replicate: {
                resultWords.assign(wordCount(resultType.bitWidth), 0);
                for (uint64_t destination = 0;
                     destination < resultType.bitWidth;
                     destination += operands[0].type().bitWidth) {
                    for (uint64_t bit = 0; bit < operands[0].type().bitWidth;
                         ++bit) {
                        setBit(resultWords, destination + bit,
                               getBit(operands[0].words(), bit));
                    }
                }
                break;
            }
            case Opcode::SliceStatic:
            case Opcode::SliceDynamic:
            case Opcode::SliceArray: {
                uint64_t start = 0;
                if (opcode == Opcode::SliceStatic) {
                    start = program.sliceStaticAttributes(instruction)->lsb;
                } else {
                    const uint64_t index =
                        shiftAmount(operands[1], operands[0].type().bitWidth);
                    start =
                        opcode == Opcode::SliceArray
                            ? index * static_cast<uint64_t>(resultType.bitWidth)
                            : index;
                }
                resultWords.assign(wordCount(resultType.bitWidth), 0);
                for (uint64_t bit = 0; bit < resultType.bitWidth; ++bit) {
                    if (start <= std::numeric_limits<uint64_t>::max() - bit) {
                        setBit(resultWords, bit,
                               getBit(operands[0].words(), start + bit));
                    }
                }
                break;
            }
            default:
                return fail(InterpreterErrorCode::UnsupportedOpcode,
                            "AM interpreter does not implement opcode " +
                                std::string(toString(opcode)),
                            block, instruction);
            }

            values[results[0].value] =
                makeBitValue(resultType, std::move(resultWords));
            return {};
        }

        InterpreterResult
        executeHost(BlockId block, InstructionId instruction, Opcode opcode,
                    std::span<const VariableId> results,
                    std::span<const VariableId> operandIds,
                    const std::vector<InterpreterValue> &operands,
                    bool finalPhase) {
            if (opcode == Opcode::SystemFunction) {
                const auto attributes =
                    *program.systemFunctionAttributes(instruction);
                if ((attributes.schedule == CallSchedule::Final) !=
                    finalPhase) {
                    return {};
                }
                if (attributes.schedule == CallSchedule::Once &&
                    callCompleted[instruction.value]) {
                    return {};
                }
                InterpreterValue result;
                std::string error;
                if (!host->invokeSystemFunction(program, instruction, operands,
                                                result, error)) {
                    return fail(InterpreterErrorCode::HostError,
                                "system.function failed: " + error, block,
                                instruction);
                }
                if (result.type() != variableType(results[0])) {
                    return fail(InterpreterErrorCode::HostError,
                                "system.function returned the wrong AM Type",
                                block, instruction);
                }
                values[results[0].value] = std::move(result);
                if (attributes.schedule == CallSchedule::Once) {
                    callCompleted[instruction.value] = true;
                }
                return {};
            }

            if (opcode == Opcode::SystemTask) {
                const auto attributes =
                    *program.systemTaskAttributes(instruction);
                if ((attributes.schedule == CallSchedule::Final) !=
                    finalPhase) {
                    return {};
                }
                if (attributes.schedule == CallSchedule::Once &&
                    callCompleted[instruction.value]) {
                    return {};
                }
                const std::size_t eventBegin =
                    operands.size() - attributes.eventCount;
                bool eventHit = attributes.eventCount == 0;
                for (std::size_t index = eventBegin; index < operands.size();
                     ++index) {
                    eventHit = eventHit || truth(operands[index]);
                }
                const bool retainEvent =
                    attributes.eventMode == HostEventMode::Pending;
                if (retainEvent && attributes.eventCount != 0 && eventHit) {
                    pendingHostEvents[instruction.value] = true;
                }
                const bool fire = truth(operands[0]) &&
                                  (finalPhase || attributes.eventCount == 0 ||
                                   (!retainEvent && eventHit) ||
                                   pendingHostEvents[instruction.value]);
                if (!fire) {
                    return {};
                }
                const std::span<const InterpreterValue> arguments(
                    operands.data() + 1U, eventBegin - 1U);
                std::string error;
                if (!host->invokeSystemTask(program, instruction, arguments,
                                            error)) {
                    return fail(InterpreterErrorCode::HostError,
                                "system.task failed: " + error, block,
                                instruction);
                }
                if (attributes.schedule == CallSchedule::Once) {
                    callCompleted[instruction.value] = true;
                }
                if (retainEvent && attributes.eventCount != 0) {
                    pendingHostEvents[instruction.value] = false;
                }
                return {};
            }

            const auto attributes = *program.dpiCallAttributes(instruction);
            const std::size_t eventBegin =
                operands.size() - attributes.eventCount;
            bool eventHit = attributes.eventCount == 0;
            for (std::size_t index = eventBegin; index < operands.size();
                 ++index) {
                eventHit = eventHit || truth(operands[index]);
            }
            const bool retainEvent =
                attributes.eventMode == HostEventMode::Pending;
            if (retainEvent && attributes.eventCount != 0 && eventHit) {
                pendingHostEvents[instruction.value] = true;
            }
            if (!truth(operands[0]) ||
                (attributes.eventCount != 0 &&
                 !(retainEvent ? pendingHostEvents[instruction.value]
                                : eventHit))) {
                return {};
            }
            const std::span<const InterpreterValue> arguments(
                operands.data() + 1U, eventBegin - 1U);
            std::vector<InterpreterValue> hostResults;
            std::string error;
            if (!host->invokeDpiCall(program, instruction, arguments,
                                     hostResults, error)) {
                return fail(InterpreterErrorCode::HostError,
                            "dpi.call failed: " + error, block, instruction);
            }
            if (hostResults.size() != results.size()) {
                return fail(InterpreterErrorCode::HostError,
                            "dpi.call returned the wrong result count", block,
                            instruction);
            }
            for (std::size_t index = 0; index < results.size(); ++index) {
                if (hostResults[index].type() != variableType(results[index])) {
                    return fail(
                        InterpreterErrorCode::HostError,
                        "dpi.call returned a result with the wrong AM Type",
                        block, instruction);
                }
            }
            for (std::size_t index = 0; index < results.size(); ++index) {
                values[results[index].value] = std::move(hostResults[index]);
            }
            if (retainEvent && attributes.eventCount != 0) {
                pendingHostEvents[instruction.value] = false;
            }
            (void)operandIds;
            return {};
        }

        InterpreterResult executeInstruction(BlockId block,
                                             InstructionId instruction,
                                             bool finalPhase = false) {
            const Opcode opcode = program.opcode(instruction);
            const auto operandIds = program.operands(instruction);
            const auto results = program.results(instruction);
            const std::vector<InterpreterValue> operands = snapshot(operandIds);

            if (opcode <= Opcode::SliceArray) {
                return executePure(block, instruction, opcode, results,
                                   operands);
            }
            if (opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                opcode == Opcode::ChangedNeg) {
                bool changed = operands[0] != operands[1];
                if (opcode == Opcode::ChangedPos) {
                    changed = !truth(operands[1]) && truth(operands[0]);
                } else if (opcode == Opcode::ChangedNeg) {
                    changed = truth(operands[1]) && !truth(operands[0]);
                }
                setChangedResult(results[0], changed);
                values[operandIds[1].value] = operands[0];
                return {};
            }
            if (opcode == Opcode::RegisterWrite ||
                opcode == Opcode::LatchWrite) {
                const std::size_t targetIndex = 3;
                bool fire = truth(operands[0]);
                if (opcode == Opcode::RegisterWrite) {
                    bool eventHit = false;
                    for (std::size_t index = 4; index < operands.size();
                         ++index) {
                        eventHit = eventHit || truth(operands[index]);
                    }
                    fire = fire && eventHit;
                }
                if (fire) {
                    InterpreterValue next = operands[targetIndex];
                    for (uint64_t bit = 0; bit < next.type().bitWidth; ++bit) {
                        if (getBit(operands[1].words(), bit)) {
                            setBit(next.words_, bit,
                                   getBit(operands[2].words(), bit));
                        }
                    }
                    values[operandIds[targetIndex].value] = std::move(next);
                }
                return {};
            }
            if (opcode == Opcode::MemoryRead) {
                const Type &memoryType = operands[0].type();
                const uint64_t address =
                    shiftAmount(operands[1], memoryType.elementCount);
                if (address < memoryType.elementCount) {
                    values[results[0].value] = InterpreterValue::bitVector(
                        memoryType.bitWidth, memoryType.signedness,
                        operands[0].arrayElementWords(
                            static_cast<std::size_t>(address)));
                } else {
                    values[results[0].value] =
                        InterpreterValue::zero(variableType(results[0]));
                }
                return {};
            }
            if (opcode == Opcode::MemoryWrite) {
                const std::size_t targetIndex = 4;
                const Type &memoryType = operands[targetIndex].type();
                const uint64_t address =
                    shiftAmount(operands[1], memoryType.elementCount);
                bool eventHit = false;
                for (std::size_t index = 5; index < operands.size(); ++index) {
                    eventHit = eventHit || truth(operands[index]);
                }
                const bool fire = truth(operands[0]) && eventHit &&
                                  address < memoryType.elementCount;
                if (fire) {
                    InterpreterValue next = operands[targetIndex];
                    const std::size_t stride = wordCount(memoryType.bitWidth);
                    const std::size_t offset =
                        static_cast<std::size_t>(address) * stride;
                    for (uint64_t bit = 0; bit < memoryType.bitWidth; ++bit) {
                        if (getBit(operands[2].words(), bit)) {
                            setBit(next.words_,
                                   static_cast<uint64_t>(offset) * 64U + bit,
                                   getBit(operands[3].words(), bit));
                        }
                    }
                    values[operandIds[targetIndex].value] = std::move(next);
                }
                return {};
            }
            if (opcode == Opcode::MemoryFill) {
                const std::size_t targetIndex = 2;
                const Type &memoryType = operands[targetIndex].type();
                bool eventHit = false;
                for (std::size_t index = 3; index < operands.size(); ++index) {
                    eventHit = eventHit || truth(operands[index]);
                }
                const bool fire = truth(operands[0]) && eventHit;
                if (fire) {
                    InterpreterValue next = operands[targetIndex];
                    const std::size_t stride = wordCount(memoryType.bitWidth);
                    for (uint64_t element = 0;
                         element < memoryType.elementCount; ++element) {
                        for (uint64_t bit = 0; bit < memoryType.bitWidth;
                             ++bit) {
                            const uint64_t sourceBit =
                                operands[1].type().bitWidth ==
                                        memoryType.bitWidth
                                    ? bit
                                    : element * memoryType.bitWidth + bit;
                            setBit(next.words_,
                                   element * static_cast<uint64_t>(stride) *
                                           64U +
                                       bit,
                                   getBit(operands[1].words(), sourceBit));
                        }
                    }
                    values[operandIds[targetIndex].value] = std::move(next);
                }
                return {};
            }
            if (opcode == Opcode::SystemFunction ||
                opcode == Opcode::SystemTask || opcode == Opcode::DpiCall) {
                return executeHost(block, instruction, opcode, results,
                                   operandIds, operands, finalPhase);
            }
            if (opcode == Opcode::ActForward || opcode == Opcode::ActBackward) {
                if (truth(operands[0])) {
                    const auto attributes =
                        *program.activationAttributes(instruction);
                    for (BlockId target : attributes.targets) {
                        active[target.value] = true;
                    }
                    if (opcode == Opcode::ActBackward) {
                        backwardFired = true;
                    }
                }
                return {};
            }
            return fail(InterpreterErrorCode::UnsupportedOpcode,
                        "AM interpreter does not implement opcode value " +
                            std::to_string(static_cast<unsigned>(opcode)),
                        block, instruction);
        }

        InterpreterResult executeBlock(BlockId block) {
            for (std::size_t position = 0;
                 position < model.program.blockSize(block); ++position) {
                const InstructionId instruction =
                    model.program.blockInstruction(block, position);
                InterpreterResult result =
                    executeInstruction(block, instruction);
                if (!result.success()) {
                    return result;
                }
            }
            return {};
        }

        InterpreterResult eval() {
            if (initializationDiagnostic) {
                return InterpreterResult{.diagnostic =
                                             initializationDiagnostic};
            }
            if (finalized || failed) {
                return InterpreterResult{
                    .diagnostic = diagnostic(
                        InterpreterErrorCode::InvalidLifecycle,
                        finalized ? "cannot eval a finalized AM interpreter"
                                  : "cannot eval an AM interpreter after an "
                                    "execution error"),
                };
            }

            const bool initial = firstEval;
            roundCounter = 0;
            std::fill(active.begin(), active.end(), false);
            std::fill(pendingHostEvents.begin(), pendingHostEvents.end(),
                      false);
            clearChangedResults();

            InterpreterResult result = executeBlock(BlockId{0});
            if (!result.success()) {
                return result;
            }

            const uint32_t blockCount = model.program.blockCount();
            const uint32_t computeEnd = model.commitBlockBegin != 0
                                            ? model.commitBlockBegin
                                            : blockCount;
            if (initial) {
                for (uint32_t block = 1; block < computeEnd; ++block) {
                    active[block] = true;
                }
            }

            while (true) {
                backwardFired = false;
                // Compute phase: ascending, active-filtered, cleared as scanned.
                for (uint32_t block = 1; block < computeEnd; ++block) {
                    if (!active[block]) {
                        continue;
                    }
                    active[block] = false;
                    result = executeBlock(BlockId{block});
                    if (!result.success()) {
                        result.roundsExecuted = roundCounter + 1U;
                        return result;
                    }
                }
                // Commit phase: ascending, always executed.
                for (uint32_t block = model.commitBlockBegin;
                     block < model.commitBlockEnd; ++block) {
                    result = executeBlock(BlockId{block});
                    if (!result.success()) {
                        result.roundsExecuted = roundCounter + 1U;
                        return result;
                    }
                }
                clearChangedResults();
                ++roundCounter;
                if (roundCounter > options.maxRounds) {
                    result = fail(
                        InterpreterErrorCode::NonConvergent,
                        "AM eval exceeded the configured round limit");
                    result.roundsExecuted = roundCounter;
                    return result;
                }
                if (!backwardFired) {
                    break;
                }
            }

            if (initial) {
                firstEval = false;
            }
            successfulEval = true;
            dirtySinceEval = false;
            return InterpreterResult{
                .diagnostic = std::nullopt,
                .roundsExecuted = roundCounter,
            };
        }

        InterpreterResult finalize() {
            if (initializationDiagnostic) {
                return InterpreterResult{.diagnostic =
                                             initializationDiagnostic};
            }
            if (finalized) {
                return {};
            }
            if (failed || !successfulEval || dirtySinceEval) {
                return InterpreterResult{
                    .diagnostic =
                        diagnostic(InterpreterErrorCode::InvalidLifecycle,
                                   "AM finalize requires a successful eval "
                                   "with no later external write"),
                };
            }
            for (uint32_t blockIndex = 0;
                 blockIndex < model.program.blockCount(); ++blockIndex) {
                const BlockId block{blockIndex};
                for (std::size_t position = 0;
                     position < model.program.blockSize(block); ++position) {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = program.opcode(instruction);
                    bool isFinal = false;
                    if (opcode == Opcode::SystemFunction) {
                        isFinal = program.systemFunctionAttributes(instruction)
                                      ->schedule == CallSchedule::Final;
                    } else if (opcode == Opcode::SystemTask) {
                        isFinal = program.systemTaskAttributes(instruction)
                                      ->schedule == CallSchedule::Final;
                    }
                    if (isFinal) {
                        InterpreterResult result =
                            executeInstruction(block, instruction, true);
                        if (!result.success()) {
                            return result;
                        }
                    }
                }
            }
            finalized = true;
            return {};
        }

        InterpreterResult write(VariableId variable,
                                const InterpreterValue &value) {
            if (initializationDiagnostic) {
                return InterpreterResult{.diagnostic =
                                             initializationDiagnostic};
            }
            if (finalized || failed || !variable.valid() ||
                variable.value >= values.size()) {
                return InterpreterResult{
                    .diagnostic = diagnostic(
                        InterpreterErrorCode::InvalidAccess,
                        "invalid external AM variable write",
                        BlockId::invalid(), InstructionId::invalid(), variable),
                };
            }
            if (constants[variable.value] ||
                protectedVariables[variable.value]) {
                return InterpreterResult{
                    .diagnostic = diagnostic(
                        InterpreterErrorCode::InvalidAccess,
                        "external write targets a constant or "
                        "changed-owned AM variable",
                        BlockId::invalid(), InstructionId::invalid(), variable),
                };
            }
            if (value.type() != variableType(variable)) {
                return InterpreterResult{
                    .diagnostic = diagnostic(
                        InterpreterErrorCode::InvalidAccess,
                        "external AM variable write has the wrong Type",
                        BlockId::invalid(), InstructionId::invalid(), variable),
                };
            }
            values[variable.value] = value;
            dirtySinceEval = true;
            return {};
        }
    };

    Interpreter::Interpreter(const ExecutableModel &model,
                             HostEnvironment *host,
                             const InterpreterOptions &options)
        : impl_(std::make_unique<Impl>(model, host, options)) {}

    Interpreter::~Interpreter() = default;
    Interpreter::Interpreter(Interpreter &&) noexcept = default;
    Interpreter &Interpreter::operator=(Interpreter &&) noexcept = default;

    bool Interpreter::ready() const noexcept {
        return impl_ && !impl_->initializationDiagnostic.has_value();
    }

    const std::optional<InterpreterDiagnostic> &
    Interpreter::initializationDiagnostic() const noexcept {
        return impl_->initializationDiagnostic;
    }

    InterpreterResult Interpreter::eval() { return impl_->eval(); }

    InterpreterResult Interpreter::finalize() { return impl_->finalize(); }

    InterpreterResult Interpreter::write(VariableId variable,
                                         const InterpreterValue &value) {
        return impl_->write(variable, value);
    }

    const InterpreterValue &Interpreter::value(VariableId variable) const {
        if (!impl_ || !variable.valid() ||
            variable.value >= impl_->values.size()) {
            throw std::out_of_range("invalid AM interpreter VariableId");
        }
        return impl_->values[variable.value];
    }

    std::span<const InterpreterValue> Interpreter::values() const noexcept {
        return impl_ ? std::span<const InterpreterValue>(impl_->values)
                     : std::span<const InterpreterValue>();
    }

    bool Interpreter::firstEval() const noexcept {
        return impl_ && impl_->firstEval;
    }

    bool Interpreter::finalized() const noexcept {
        return impl_ && impl_->finalized;
    }

    uint64_t Interpreter::roundCounter() const noexcept {
        return impl_ ? impl_->roundCounter : 0;
    }

} // namespace wolvrix::lib::grhsim::am
