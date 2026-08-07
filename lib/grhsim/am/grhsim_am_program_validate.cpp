#include "grhsim/am/grhsim_am_program_validate.hpp"

#include "grhsim/am/grhsim_am_opcode_traits.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    namespace
    {
        class Validator
        {
        public:
            explicit Validator(const ValidationOptions &options)
                : options_(options)
            {
                if (options_.maxErrors == 0)
                {
                    options_.maxErrors = 1;
                }
            }

            bool full() const noexcept { return result_.errors.size() >= options_.maxErrors; }
            bool semantic() const noexcept { return options_.level == ValidationLevel::Semantic; }

            void error(std::string message)
            {
                if (!full())
                {
                    result_.errors.push_back(std::move(message));
                }
            }

            ValidationResult finish() { return std::move(result_); }

        private:
            ValidationOptions options_;
            ValidationResult result_;
        };

        bool isPureCombinational(Opcode opcode) noexcept
        {
            return (opcode >= Opcode::Assign && opcode <= Opcode::SliceArray) ||
                   (opcode >= Opcode::ArrayMux && opcode <= Opcode::ArrayReduceLanesXor);
        }

        bool isChanged(Opcode opcode) noexcept
        {
            return opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                   opcode == Opcode::ChangedNeg;
        }

        bool isActivation(Opcode opcode) noexcept
        {
            return opcode == Opcode::ActForward || opcode == Opcode::ActBackward;
        }

        bool validOpcode(Opcode opcode) noexcept
        {
            return opcode <= Opcode::MemoryWriteCondMask;
        }

        bool validString(ProgramView view, StringId id)
        {
            return id.valid() && id.value < view.stringCount() && !view.string(id).empty();
        }

        const Type *variableType(ProgramView view, VariableId variable)
        {
            if (!variable.valid() || variable.value >= view.variableCount())
            {
                return nullptr;
            }
            const TypeId type = view.variable(variable).type;
            return type.valid() && type.value < view.typeCount() ? &view.type(type) : nullptr;
        }

        const InitDescriptor *variableInit(ProgramView view, VariableId variable)
        {
            if (!variable.valid() || variable.value >= view.variableCount())
            {
                return nullptr;
            }
            const InitId init = view.variable(variable).init;
            return init.valid() && init.value < view.initCount() ? &view.init(init) : nullptr;
        }

        bool isBitVector(const Type *type) noexcept
        {
            return type && type->kind == TypeKind::BitVector && type->bitWidth != 0 &&
                   type->elementCount == 0 && type->signedness <= Signedness::Signed;
        }

        bool isBitVector1(const Type *type) noexcept
        {
            return isBitVector(type) && type->bitWidth == 1;
        }

        bool isUnsignedBitVector1(const Type *type) noexcept
        {
            return isBitVector1(type) && type->signedness == Signedness::Unsigned;
        }

        bool isHostValueType(const Type *type) noexcept
        {
            return isBitVector(type) ||
                   (type && (type->kind == TypeKind::Real || type->kind == TypeKind::String) &&
                    type->bitWidth == 0 && type->elementCount == 0 &&
                    type->signedness == Signedness::Unsigned);
        }

        bool sameType(const Type *lhs, const Type *rhs) noexcept
        {
            return lhs && rhs && *lhs == *rhs;
        }

        bool matchesBitVector(const Type *type, uint64_t width, Signedness signedness) noexcept
        {
            return width <= std::numeric_limits<uint32_t>::max() && isBitVector(type) &&
                   type->bitWidth == width && type->signedness == signedness;
        }

        bool isMutable(ProgramView view, VariableId variable)
        {
            const InitDescriptor *init = variableInit(view, variable);
            return init && init->kind != InitKind::Constant;
        }

        bool dpiAbiCompatible(const Type *type, DpiAbiKind abi) noexcept
        {
            if (!type)
            {
                return false;
            }
            switch (abi)
            {
            case DpiAbiKind::Integral:
                return type->kind == TypeKind::BitVector;
            case DpiAbiKind::Real64:
            case DpiAbiKind::Real32:
                return type->kind == TypeKind::Real;
            case DpiAbiKind::String:
                return type->kind == TypeKind::String;
            }
            return false;
        }

        struct DpiImportEntry
        {
            std::string_view symbol;
            DpiImportId import;
        };

        void validateTypeTable(ProgramView view, Validator &validator)
        {
            for (uint32_t index = 0; index < view.typeCount() && !validator.full(); ++index)
            {
                const Type &type = view.type(TypeId{index});
                bool valid = false;
                if (type.signedness > Signedness::Signed)
                {
                    validator.error("invalid AM type signedness: type=" + std::to_string(index));
                    continue;
                }
                switch (type.kind)
                {
                case TypeKind::BitVector:
                    valid = type.bitWidth != 0 && type.elementCount == 0;
                    break;
                case TypeKind::Array:
                    valid = type.bitWidth != 0 && type.elementCount != 0;
                    break;
                case TypeKind::Real:
                case TypeKind::String:
                    valid = type.bitWidth == 0 && type.elementCount == 0 &&
                            type.signedness == Signedness::Unsigned;
                    break;
                default:
                    break;
                }
                if (!valid)
                {
                    validator.error("invalid AM type record: type=" + std::to_string(index));
                }
            }
        }

        void validateVariables(ProgramView view, Validator &validator)
        {
            for (uint32_t index = 0; index < view.variableCount() && !validator.full(); ++index)
            {
                const VariableRecord &variable = view.variable(VariableId{index});
                if (!variable.type.valid() || variable.type.value >= view.typeCount())
                {
                    validator.error("AM variable has invalid TypeId: variable=" + std::to_string(index));
                    continue;
                }
                if (!variable.init.valid() || variable.init.value >= view.initCount())
                {
                    validator.error("AM variable has invalid InitId: variable=" + std::to_string(index));
                    continue;
                }
                const InitDescriptor &init = view.init(variable.init);
                const Type &variableType = view.type(variable.type);
                if (init.kind == InitKind::Undef || init.kind == InitKind::Zero)
                {
                    if (init.payload != 0 || init.count != 0)
                    {
                        validator.error("AM built-in init has an unexpected payload: variable=" +
                                        std::to_string(index));
                    }
                }
                else if (init.kind == InitKind::Constant)
                {
                    const LiteralId literal{init.payload};
                    if (!literal.valid() || literal.value >= view.literalCount())
                    {
                        validator.error("AM constant init has invalid LiteralId: variable=" +
                                        std::to_string(index));
                    }
                    else
                    {
                        const TypeId literalType = view.literal(literal).type;
                        if (variableType.kind == TypeKind::Array || !literalType.valid() ||
                            literalType.value >= view.typeCount() ||
                            view.type(literalType) != variableType)
                        {
                            validator.error("AM constant init type mismatch: variable=" +
                                            std::to_string(index));
                        }
                    }
                }
                else if (init.kind == InitKind::Actions)
                {
                    const auto actions = view.initActions(variable.init);
                    bool sawSet = false;
                    const Type expectedScalar =
                        variableType.kind == TypeKind::Array
                            ? Type{
                                  .kind = TypeKind::BitVector,
                                  .signedness = variableType.signedness,
                                  .bitWidth = variableType.bitWidth,
                                  .elementCount = 0,
                              }
                            : variableType;
                    for (const InitAction &action : actions)
                    {
                        if (action.kind > InitActionKind::Load || action.format > LoadFormat::Binary ||
                            action.rangeKind > InitRangeKind::Span ||
                            action.expression.kind > InitExprKind::RandomSeeded)
                        {
                            validator.error("AM init action contains an invalid enum value: variable=" +
                                            std::to_string(index));
                            continue;
                        }
                        if (action.kind == InitActionKind::Set)
                        {
                            if (variableType.kind == TypeKind::Array || sawSet)
                            {
                                validator.error("AM set init is invalid for its target: variable=" +
                                                std::to_string(index));
                            }
                            sawSet = true;
                        }
                        else if (action.kind == InitActionKind::Fill)
                        {
                            const uint64_t end = action.start + action.count;
                            if (variableType.kind != TypeKind::Array || action.count == 0 ||
                                end < action.start || end > variableType.elementCount)
                            {
                                validator.error("AM fill init range is invalid: variable=" +
                                                std::to_string(index));
                            }
                        }
                        else if (action.kind == InitActionKind::Load)
                        {
                            const bool invalidSpan =
                                action.rangeKind == InitRangeKind::Span &&
                                (action.count == 0 ||
                                 action.start > std::numeric_limits<uint64_t>::max() - action.count);
                            if (variableType.kind != TypeKind::Array || !action.path.valid() ||
                                action.path.value >= view.stringCount() ||
                                view.string(action.path).empty() ||
                                action.format > LoadFormat::Binary ||
                                action.rangeKind > InitRangeKind::Span ||
                                invalidSpan)
                            {
                                validator.error("AM load init is invalid: variable=" +
                                                std::to_string(index));
                            }
                            continue;
                        }
                        else
                        {
                            validator.error("AM init action has an invalid kind: variable=" +
                                            std::to_string(index));
                            continue;
                        }

                        if (action.expression.kind == InitExprKind::Literal)
                        {
                            bool validLiteral = action.expression.literal.valid() &&
                                                action.expression.literal.value < view.literalCount();
                            if (validLiteral)
                            {
                                const TypeId literalType = view.literal(action.expression.literal).type;
                                validLiteral = literalType.valid() && literalType.value < view.typeCount() &&
                                               view.type(literalType) == expectedScalar;
                            }
                            if (!validLiteral)
                            {
                                validator.error("AM init literal type is invalid: variable=" +
                                                std::to_string(index));
                            }
                        }
                        else if (action.expression.kind == InitExprKind::Random ||
                                 action.expression.kind == InitExprKind::RandomSeeded)
                        {
                            if (expectedScalar.kind != TypeKind::BitVector)
                            {
                                validator.error("AM random init requires a bit-vector target: variable=" +
                                                std::to_string(index));
                            }
                        }
                        else
                        {
                            validator.error("AM init expression has an invalid kind: variable=" +
                                            std::to_string(index));
                        }
                    }
                }
                else
                {
                    validator.error("AM variable has an invalid InitKind: variable=" +
                                    std::to_string(index));
                }
            }

            VariableId previous;
            for (const VariableLabel &label : view.variableLabels())
            {
                if (!label.variable.valid() || label.variable.value >= view.variableCount() ||
                    !label.label.valid() || label.label.value >= view.stringCount())
                {
                    validator.error("AM variable label has an invalid ID");
                    break;
                }
                if (previous.valid() && previous.value >= label.variable.value)
                {
                    validator.error("AM variable labels are not sparse-sorted and unique");
                    break;
                }
                previous = label.variable;
            }
        }

        void validateDpiImports(ProgramView view,
                                Validator &validator,
                                std::vector<DpiImportEntry> &entries)
        {
            entries.reserve(view.dpiImportCount());
            for (uint32_t index = 0; index < view.dpiImportCount() && !validator.full(); ++index)
            {
                const DpiImportId id{index};
                const DpiImportView import = view.dpiImport(id);
                if (!validString(view, import.symbol))
                {
                    validator.error("AM DPI import has an invalid or empty symbol: import=" +
                                    std::to_string(index));
                }
                else
                {
                    entries.push_back(DpiImportEntry{
                        .symbol = view.string(import.symbol),
                        .import = id,
                    });
                }

                std::vector<std::string_view> parameterNames;
                parameterNames.reserve(import.parameters.size());
                for (std::size_t parameterIndex = 0;
                     parameterIndex < import.parameters.size() && !validator.full();
                     ++parameterIndex)
                {
                    const DpiParameter &parameter = import.parameters[parameterIndex];
                    bool valid = true;
                    if (!validString(view, parameter.name))
                    {
                        valid = false;
                    }
                    else
                    {
                        parameterNames.push_back(view.string(parameter.name));
                    }
                    if (!parameter.type.valid() || parameter.type.value >= view.typeCount() ||
                        parameter.direction > DpiDirection::Inout ||
                        parameter.abi > DpiAbiKind::String)
                    {
                        valid = false;
                    }
                    else if (!dpiAbiCompatible(&view.type(parameter.type), parameter.abi))
                    {
                        valid = false;
                    }
                    if (!valid)
                    {
                        validator.error("AM DPI parameter declaration is invalid: import=" +
                                        std::to_string(index) + " parameter=" +
                                        std::to_string(parameterIndex));
                    }
                }
                std::sort(parameterNames.begin(), parameterNames.end());
                if (std::adjacent_find(parameterNames.begin(), parameterNames.end()) !=
                    parameterNames.end())
                {
                    validator.error("AM DPI parameter names must be unique: import=" +
                                    std::to_string(index));
                }

                const DpiReturn &returnValue = import.returnValue;
                if (returnValue.present &&
                    (!returnValue.type.valid() || returnValue.type.value >= view.typeCount() ||
                     returnValue.abi > DpiAbiKind::String ||
                     (returnValue.type.valid() && returnValue.type.value < view.typeCount() &&
                      !dpiAbiCompatible(&view.type(returnValue.type), returnValue.abi))))
                {
                    validator.error("AM DPI return declaration is invalid: import=" +
                                    std::to_string(index));
                }
            }

            std::sort(entries.begin(), entries.end(), [](const DpiImportEntry &lhs,
                                                         const DpiImportEntry &rhs) {
                return lhs.symbol < rhs.symbol;
            });
            for (std::size_t index = 1; index < entries.size() && !validator.full(); ++index)
            {
                if (entries[index - 1].symbol == entries[index].symbol)
                {
                    validator.error("AM DPI import symbols must be unique");
                    break;
                }
            }
        }

        void validateInstructionShape(ProgramView view,
                                      InstructionId instruction,
                                      Validator &validator)
        {
            const Opcode opcode = view.opcode(instruction);
            const std::size_t resultCount = view.results(instruction).size();
            const std::size_t operandCount = view.operands(instruction).size();
            bool shapeValid = true;

            if (!validOpcode(opcode))
            {
                validator.error("AM instruction has an invalid opcode: instruction=" +
                                std::to_string(instruction.value));
            }
            else if ((opcode >= Opcode::Add && opcode <= Opcode::Xnor) ||
                (opcode >= Opcode::Eq && opcode <= Opcode::LogicOr) ||
                (opcode >= Opcode::Shl && opcode <= Opcode::ArithmeticShr) ||
                opcode == Opcode::SliceDynamic || opcode == Opcode::SliceArray)
            {
                shapeValid = resultCount == 1 && operandCount == 2;
            }
            else if (opcode == Opcode::Assign || opcode == Opcode::Not ||
                     opcode == Opcode::LogicNot ||
                     (opcode >= Opcode::ReduceAnd && opcode <= Opcode::ReduceXnor) ||
                     opcode == Opcode::Replicate || opcode == Opcode::SliceStatic)
            {
                shapeValid = resultCount == 1 && operandCount == 1;
            }
            else
            {
                switch (opcode)
                {
                case Opcode::Mux:
                    shapeValid = resultCount == 1 && operandCount == 3;
                    break;
                case Opcode::ArrayMux:
                    shapeValid = resultCount == 1 && operandCount == 3;
                    break;
                case Opcode::ArrayReduceOr:
                case Opcode::ArrayReduceAnd:
                case Opcode::ArrayReduceXor:
                case Opcode::ArrayBroadcast:
                case Opcode::ArrayOnehot:
                case Opcode::ArrayReduceLanesOr:
                case Opcode::ArrayReduceLanesAnd:
                case Opcode::ArrayReduceLanesXor:
                    shapeValid = resultCount == 1 && operandCount == 1;
                    break;
                case Opcode::MemoryReadAll:
                    shapeValid = resultCount == 1 && operandCount == 1;
                    break;
                case Opcode::MemoryWriteLanes:
                    shapeValid = resultCount == 0 && operandCount >= 3;
                    break;
                case Opcode::Concat:
                    shapeValid = resultCount == 1 && operandCount >= 1;
                    break;
                case Opcode::ChangedAny:
                case Opcode::ChangedPos:
                case Opcode::ChangedNeg:
                    shapeValid = resultCount == 1 && operandCount == 2;
                    break;
                case Opcode::RegisterWrite:
                case Opcode::RegisterWriteCond:
                case Opcode::RegisterWriteMask:
                case Opcode::RegisterWriteCondMask:
                case Opcode::MemoryWrite:
                case Opcode::MemoryWriteCond:
                case Opcode::MemoryWriteMask:
                case Opcode::MemoryWriteCondMask:
                    shapeValid = resultCount == 0 &&
                                 operandCount >= stateWriteLayout(opcode).fixedCount;
                    break;
                case Opcode::LatchWrite:
                case Opcode::LatchWriteCond:
                case Opcode::LatchWriteMask:
                case Opcode::LatchWriteCondMask:
                    // Latch writes are eventless: exactly the fixed operands.
                    shapeValid = resultCount == 0 &&
                                 operandCount == stateWriteLayout(opcode).fixedCount;
                    break;
                case Opcode::MemoryRead:
                    shapeValid = resultCount == 1 && operandCount == 2;
                    break;
                case Opcode::MemoryFill:
                    shapeValid = resultCount == 0 && operandCount >= 2;
                    break;
                case Opcode::SystemFunction:
                    shapeValid = resultCount == 1;
                    break;
                case Opcode::SystemTask:
                    shapeValid = resultCount == 0 && operandCount >= 1;
                    break;
                case Opcode::DpiCall:
                    shapeValid = operandCount >= 1;
                    break;
                case Opcode::ActForward:
                case Opcode::ActBackward:
                    shapeValid = resultCount == 0 && operandCount == 1;
                    break;
                default:
                    break;
                }
            }

            if (!shapeValid)
            {
                validator.error("AM instruction has invalid result/operand arity: instruction=" +
                                std::to_string(instruction.value) + " opcode=" +
                                std::string(toString(opcode)));
            }

            for (VariableId variable : view.results(instruction))
            {
                if (!variable.valid() || variable.value >= view.variableCount())
                {
                    validator.error("AM instruction has invalid result VariableId: instruction=" +
                                    std::to_string(instruction.value));
                    break;
                }
                const InitDescriptor *init = variableInit(view, variable);
                if (init && init->kind == InitKind::Constant)
                {
                    validator.error("AM instruction writes a constant variable: instruction=" +
                                    std::to_string(instruction.value));
                    break;
                }
            }
            for (VariableId variable : view.operands(instruction))
            {
                if (!variable.valid() || variable.value >= view.variableCount())
                {
                    validator.error("AM instruction has invalid operand VariableId: instruction=" +
                                    std::to_string(instruction.value));
                    break;
                }
            }

            bool attributesPresent = true;
            switch (opcode)
            {
            case Opcode::SliceStatic:
                attributesPresent = view.sliceStaticAttributes(instruction).has_value();
                break;
            case Opcode::SystemFunction:
                if (auto attributes = view.systemFunctionAttributes(instruction))
                {
                    attributesPresent = validString(view, attributes->name) &&
                                        attributes->schedule <= CallSchedule::Final;
                }
                else
                {
                    attributesPresent = false;
                }
                break;
            case Opcode::SystemTask:
                if (auto attributes = view.systemTaskAttributes(instruction))
                {
                    attributesPresent = validString(view, attributes->name) &&
                                        attributes->schedule <= CallSchedule::Final &&
                                        attributes->eventMode <= HostEventMode::Pending &&
                                        attributes->eventCount < operandCount;
                }
                else
                {
                    attributesPresent = false;
                }
                break;
            case Opcode::DpiCall:
                if (auto attributes = view.dpiCallAttributes(instruction))
                {
                    attributesPresent = validString(view, attributes->importSymbol) &&
                                        attributes->eventMode <= HostEventMode::Pending &&
                                        attributes->eventCount < operandCount;
                }
                else
                {
                    attributesPresent = false;
                }
                break;
            case Opcode::ActForward:
            case Opcode::ActBackward:
                if (auto attributes = view.activationAttributes(instruction))
                {
                    attributesPresent = !attributes->targets.empty();
                    BlockId previous;
                    for (BlockId target : attributes->targets)
                    {
                        if (!target.valid() ||
                            (previous.valid() && previous.value >= target.value))
                        {
                            attributesPresent = false;
                            break;
                        }
                        previous = target;
                    }
                }
                else
                {
                    attributesPresent = false;
                }
                break;
            default:
                break;
            }
            if (!attributesPresent)
            {
                validator.error("AM instruction is missing or has invalid typed attributes: instruction=" +
                                std::to_string(instruction.value));
            }
        }

        void semanticInstructionError(Validator &validator,
                                      InstructionId instruction,
                                      std::string_view message)
        {
            validator.error(std::string(message) + ": instruction=" +
                            std::to_string(instruction.value));
        }

        Signedness commonSignedness(const Type &lhs, const Type &rhs) noexcept
        {
            return lhs.signedness == Signedness::Signed && rhs.signedness == Signedness::Signed
                       ? Signedness::Signed
                       : Signedness::Unsigned;
        }

        void validateCombinationalSemantics(ProgramView view,
                                            InstructionId instruction,
                                            Validator &validator)
        {
            const Opcode opcode = view.opcode(instruction);
            const auto results = view.results(instruction);
            const auto operands = view.operands(instruction);
            for (VariableId variable : results)
            {
                if (!isBitVector(variableType(view, variable)))
                {
                    semanticInstructionError(
                        validator, instruction,
                        "AM combinational instruction requires bit-vector variables");
                    return;
                }
            }
            for (VariableId variable : operands)
            {
                if (!isBitVector(variableType(view, variable)))
                {
                    semanticInstructionError(
                        validator, instruction,
                        "AM combinational instruction requires bit-vector variables");
                    return;
                }
            }

            if (results.size() != 1)
            {
                return;
            }
            const Type *result = variableType(view, results.front());
            bool valid = true;
            if (opcode == Opcode::Assign)
            {
                return;
            }
            if (opcode >= Opcode::Add && opcode <= Opcode::Xnor)
            {
                if (operands.size() != 2)
                {
                    return;
                }
                const Type *lhs = variableType(view, operands[0]);
                const Type *rhs = variableType(view, operands[1]);
                const Signedness sign = commonSignedness(*lhs, *rhs);
                uint64_t width = 0;
                if (opcode == Opcode::Mul)
                {
                    width = static_cast<uint64_t>(lhs->bitWidth) + rhs->bitWidth;
                }
                else if (opcode == Opcode::Div || opcode == Opcode::Mod)
                {
                    width = lhs->bitWidth;
                }
                else
                {
                    width = std::max(lhs->bitWidth, rhs->bitWidth);
                }
                valid = matchesBitVector(result, width, sign);
            }
            else if (opcode == Opcode::Not)
            {
                valid = operands.size() == 1 &&
                        sameType(result, variableType(view, operands.front()));
            }
            else if (opcode >= Opcode::Eq && opcode <= Opcode::Ge)
            {
                valid = operands.size() == 2 && isUnsignedBitVector1(result);
            }
            else if (opcode == Opcode::LogicAnd || opcode == Opcode::LogicOr)
            {
                valid = operands.size() == 2 && isUnsignedBitVector1(result);
            }
            else if (opcode == Opcode::LogicNot ||
                     (opcode >= Opcode::ReduceAnd && opcode <= Opcode::ReduceXnor))
            {
                valid = operands.size() == 1 && isUnsignedBitVector1(result);
            }
            else if (opcode >= Opcode::Shl && opcode <= Opcode::ArithmeticShr)
            {
                valid = operands.size() == 2 &&
                        sameType(result, variableType(view, operands.front()));
            }
            else if (opcode == Opcode::Mux)
            {
                if (operands.size() != 3)
                {
                    return;
                }
                const Type *trueValue = variableType(view, operands[1]);
                const Type *falseValue = variableType(view, operands[2]);
                valid = isBitVector1(variableType(view, operands[0])) &&
                        result->signedness == commonSignedness(*trueValue, *falseValue);
            }
            else if (opcode == Opcode::Concat)
            {
                uint64_t width = 0;
                for (VariableId operand : operands)
                {
                    width += variableType(view, operand)->bitWidth;
                }
                valid = !operands.empty() &&
                        matchesBitVector(result, width, Signedness::Unsigned);
            }
            else if (opcode == Opcode::Replicate)
            {
                if (operands.size() != 1)
                {
                    return;
                }
                const Type *source = variableType(view, operands.front());
                valid = result->signedness == Signedness::Unsigned &&
                        result->bitWidth % source->bitWidth == 0;
            }
            else if (opcode == Opcode::SliceStatic)
            {
                if (operands.size() != 1)
                {
                    return;
                }
                const auto attributes = view.sliceStaticAttributes(instruction);
                if (!attributes)
                {
                    return;
                }
                const Type *base = variableType(view, operands.front());
                const uint64_t end = static_cast<uint64_t>(attributes->lsb) + result->bitWidth;
                valid = result->signedness == Signedness::Unsigned && end <= base->bitWidth;
            }
            else if (opcode == Opcode::SliceDynamic)
            {
                valid = operands.size() == 2 && result->signedness == Signedness::Unsigned;
            }
            else if (opcode == Opcode::SliceArray)
            {
                if (operands.size() != 2)
                {
                    return;
                }
                const Type *base = variableType(view, operands.front());
                valid = result->signedness == Signedness::Unsigned && result->bitWidth != 0 &&
                        base->bitWidth % result->bitWidth == 0;
            }
            else if (opcode == Opcode::ArrayMux)
            {
                if (operands.size() != 3)
                {
                    return;
                }
                const Type *sel = variableType(view, operands[0]);
                const Type *trueValue = variableType(view, operands[1]);
                const Type *falseValue = variableType(view, operands[2]);
                // Lane selection is bitwise (like mux), so only the lane
                // widths are structural: t/f must cover the full packed
                // width; signedness follows the common mux convention.
                valid = sel->bitWidth != 0 &&
                        result->bitWidth % sel->bitWidth == 0 &&
                        trueValue->bitWidth == result->bitWidth &&
                        falseValue->bitWidth == result->bitWidth &&
                        result->signedness == commonSignedness(*trueValue, *falseValue);
            }
            else if (opcode == Opcode::ArrayReduceOr ||
                     opcode == Opcode::ArrayReduceAnd ||
                     opcode == Opcode::ArrayReduceXor)
            {
                valid = operands.size() == 1 && isUnsignedBitVector1(result);
            }
            else if (opcode == Opcode::ArrayReduceLanesOr ||
                     opcode == Opcode::ArrayReduceLanesAnd ||
                     opcode == Opcode::ArrayReduceLanesXor)
            {
                if (operands.size() != 1)
                {
                    return;
                }
                const Type *data = variableType(view, operands.front());
                valid = result->signedness == Signedness::Unsigned && result->bitWidth != 0 &&
                        data->bitWidth % result->bitWidth == 0;
            }
            else if (opcode == Opcode::ArrayBroadcast)
            {
                if (operands.size() != 1)
                {
                    return;
                }
                const Type *source = variableType(view, operands.front());
                valid = source->bitWidth != 0 &&
                        result->bitWidth % source->bitWidth == 0;
            }
            else if (opcode == Opcode::ArrayOnehot)
            {
                valid = operands.size() == 1;
            }

            if (!valid)
            {
                const auto describeType = [](const Type *type) {
                    if (!type)
                    {
                        return std::string("<null>");
                    }
                    std::string text =
                        type->kind == TypeKind::Array ? "array<" : "bv<";
                    text += std::to_string(type->bitWidth);
                    if (type->kind == TypeKind::Array)
                    {
                        text += " x " + std::to_string(type->elementCount);
                    }
                    text += type->signedness == Signedness::Signed ? ",s>" : ",u>";
                    return text;
                };
                std::string detail = " opcode=" + std::string(toString(opcode)) +
                                     " result=" + describeType(result) + " operands=[";
                for (std::size_t index = 0; index < operands.size(); ++index)
                {
                    if (index != 0)
                    {
                        detail += ", ";
                    }
                    detail += describeType(variableType(view, operands[index]));
                }
                detail += "]";
                semanticInstructionError(
                    validator, instruction,
                    "AM combinational instruction has an invalid Type signature" +
                        detail);
            }
        }

        void validateChangedSemantics(ProgramView view,
                                      InstructionId instruction,
                                      Validator &validator)
        {
            const auto results = view.results(instruction);
            const auto operands = view.operands(instruction);
            if (results.size() != 1 || operands.size() != 2)
            {
                return;
            }
            const VariableId result = results.front();
            const VariableId newValue = operands[0];
            const VariableId oldValue = operands[1];
            const Type *resultType = variableType(view, result);
            const Type *newType = variableType(view, newValue);
            const Type *oldType = variableType(view, oldValue);
            bool valid = isUnsignedBitVector1(resultType) && sameType(newType, oldType) &&
                         result != newValue && result != oldValue && newValue != oldValue &&
                         isMutable(view, result) && isMutable(view, oldValue);
            const Opcode opcode = view.opcode(instruction);
            if (opcode == Opcode::ChangedPos || opcode == Opcode::ChangedNeg)
            {
                valid = valid && isBitVector1(newType);
            }
            const InitDescriptor *oldInit = variableInit(view, oldValue);
            valid = valid && oldInit && oldInit->kind == InitKind::Undef;
            if (!valid)
            {
                semanticInstructionError(
                    validator, instruction,
                    "AM changed instruction has invalid Types, aliases, or old state");
            }
        }

        bool validateEventRange(ProgramView view,
                                std::span<const VariableId> operands,
                                std::size_t begin,
                                VariableId target = VariableId::invalid())
        {
            for (std::size_t index = begin; index < operands.size(); ++index)
            {
                if (!isUnsignedBitVector1(variableType(view, operands[index])) ||
                    (target.valid() && operands[index] == target))
                {
                    return false;
                }
            }
            return true;
        }

        void validateStateSemantics(ProgramView view,
                                    InstructionId instruction,
                                    Validator &validator)
        {
            const Opcode opcode = view.opcode(instruction);
            const auto results = view.results(instruction);
            const auto operands = view.operands(instruction);
            bool valid = true;
            switch (opcode)
            {
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
            {
                const StateWriteLayout layout = stateWriteLayout(opcode);
                if (!results.empty() || operands.size() < layout.fixedCount)
                {
                    return;
                }
                const VariableId target = operands[layout.targetIndex];
                const Type *targetType = variableType(view, target);
                const Type *dataType = variableType(view, operands[layout.dataIndex]);
                valid = isMutable(view, target);
                if (layout.hasCond)
                {
                    valid = valid && isBitVector1(variableType(view, operands[0]));
                }
                if (layout.memory)
                {
                    const std::size_t addrIndex = layout.hasCond ? 1 : 0;
                    const bool targetIsArray =
                        targetType && targetType->kind == TypeKind::Array;
                    valid = valid && targetIsArray &&
                            isBitVector(variableType(view, operands[addrIndex]));
                    if (targetIsArray)
                    {
                        valid = valid &&
                                matchesBitVector(dataType, targetType->bitWidth,
                                                 targetType->signedness);
                        if (layout.hasMask)
                        {
                            const Type *maskType =
                                variableType(view, operands[addrIndex + 1]);
                            valid = valid && isBitVector(maskType) &&
                                    maskType->bitWidth == targetType->bitWidth;
                        }
                    }
                    valid = valid &&
                            validateEventRange(view, operands, layout.fixedCount, target);
                }
                else
                {
                    valid = valid && isBitVector(targetType) &&
                            sameType(dataType, targetType);
                    if (layout.hasMask)
                    {
                        const Type *maskType =
                            variableType(view, operands[layout.hasCond ? 1 : 0]);
                        valid = valid && sameType(maskType, targetType);
                    }
                    // Latch writes carry no events; register writes trail
                    // theirs after the fixed operands.
                    if (!isLatchWriteOpcode(opcode))
                    {
                        valid = valid &&
                                validateEventRange(view, operands, layout.fixedCount, target);
                    }
                }
                break;
            }
            case Opcode::MemoryRead:
                if (results.size() != 1 || operands.size() != 2)
                {
                    return;
                }
                {
                    const Type *targetType = variableType(view, operands[0]);
                    const Type *resultType = variableType(view, results.front());
                    valid = targetType && targetType->kind == TypeKind::Array &&
                            isBitVector(variableType(view, operands[1])) &&
                            matchesBitVector(resultType, targetType->bitWidth,
                                             targetType->signedness);
                }
                break;
            case Opcode::MemoryFill:
                if (!results.empty() || operands.size() < 2)
                {
                    return;
                }
                {
                    const VariableId target = operands[1];
                    const Type *targetType = variableType(view, target);
                    const Type *dataType = variableType(view, operands[0]);
                    uint64_t packedWidth = 0;
                    if (targetType)
                    {
                        packedWidth = static_cast<uint64_t>(targetType->elementCount) *
                                      targetType->bitWidth;
                    }
                    valid = targetType && targetType->kind == TypeKind::Array &&
                            isBitVector(dataType) &&
                            dataType->bitWidth == packedWidth &&
                            isMutable(view, target) &&
                            validateEventRange(view, operands, 2, target);
                }
                break;
            case Opcode::MemoryReadAll:
                if (results.size() != 1 || operands.size() != 1)
                {
                    return;
                }
                {
                    const Type *targetType = variableType(view, operands[0]);
                    const Type *resultType = variableType(view, results.front());
                    valid = targetType && targetType->kind == TypeKind::Array &&
                            isBitVector(resultType) &&
                            resultType->bitWidth == static_cast<uint64_t>(targetType->elementCount) *
                                                        targetType->bitWidth;
                }
                break;
            case Opcode::MemoryWriteLanes:
                if (!results.empty() || operands.size() < 3)
                {
                    return;
                }
                {
                    const VariableId target = operands[2];
                    const Type *targetType = variableType(view, target);
                    const Type *laneMaskType = variableType(view, operands[0]);
                    const Type *dataType = variableType(view, operands[1]);
                    valid = targetType && targetType->kind == TypeKind::Array &&
                            isBitVector(laneMaskType) &&
                            laneMaskType->bitWidth == targetType->elementCount &&
                            isBitVector(dataType) &&
                            dataType->bitWidth == static_cast<uint64_t>(targetType->elementCount) *
                                                      targetType->bitWidth &&
                            isMutable(view, target) &&
                            validateEventRange(view, operands, 3, target);
                }
                break;
            default:
                return;
            }
            if (!valid)
            {
                semanticInstructionError(
                    validator, instruction,
                    "AM state or memory instruction has an invalid Type or target");
            }
        }

        void validateSystemSemantics(ProgramView view,
                                     InstructionId instruction,
                                     Validator &validator)
        {
            const Opcode opcode = view.opcode(instruction);
            const auto results = view.results(instruction);
            const auto operands = view.operands(instruction);
            bool valid = true;
            if (opcode == Opcode::SystemFunction)
            {
                if (results.size() != 1)
                {
                    return;
                }
                valid = isHostValueType(variableType(view, results.front()));
                for (VariableId operand : operands)
                {
                    valid = valid && isHostValueType(variableType(view, operand));
                }
            }
            else if (opcode == Opcode::SystemTask)
            {
                const auto attributes = view.systemTaskAttributes(instruction);
                if (!attributes || !results.empty() || attributes->eventCount >= operands.size())
                {
                    return;
                }
                const std::size_t eventBegin = operands.size() - attributes->eventCount;
                valid = isBitVector1(variableType(view, operands.front())) &&
                        validateEventRange(view, operands, eventBegin) &&
                        !(attributes->schedule == CallSchedule::Final &&
                          attributes->eventCount != 0);
                for (std::size_t index = 1; index < eventBegin; ++index)
                {
                    valid = valid && isHostValueType(variableType(view, operands[index]));
                }
            }
            else
            {
                return;
            }
            if (!valid)
            {
                semanticInstructionError(
                    validator, instruction,
                    "AM system call has an invalid Type, event list, or final schedule");
            }
        }

        const DpiImportEntry *findDpiImport(const std::vector<DpiImportEntry> &entries,
                                            std::string_view symbol)
        {
            const auto it = std::lower_bound(
                entries.begin(), entries.end(), symbol,
                [](const DpiImportEntry &entry, std::string_view value) {
                    return entry.symbol < value;
                });
            return it != entries.end() && it->symbol == symbol ? &*it : nullptr;
        }

        bool variableMatchesType(ProgramView view, VariableId variable, TypeId expected)
        {
            return expected.valid() && expected.value < view.typeCount() &&
                   sameType(variableType(view, variable), &view.type(expected));
        }

        void validateDpiCallSemantics(ProgramView view,
                                      InstructionId instruction,
                                      const std::vector<DpiImportEntry> &entries,
                                      Validator &validator)
        {
            const auto results = view.results(instruction);
            const auto operands = view.operands(instruction);
            const auto attributes = view.dpiCallAttributes(instruction);
            if (!attributes || operands.empty() || attributes->eventCount >= operands.size() ||
                !validString(view, attributes->importSymbol))
            {
                return;
            }
            const DpiImportEntry *entry =
                findDpiImport(entries, view.string(attributes->importSymbol));
            if (!entry)
            {
                semanticInstructionError(validator, instruction,
                                         "AM DPI call refers to an unknown import");
                return;
            }
            const DpiImportView import = view.dpiImport(entry->import);
            std::size_t inputCount = 0;
            std::size_t outputCount = 0;
            std::size_t inoutCount = 0;
            for (const DpiParameter &parameter : import.parameters)
            {
                switch (parameter.direction)
                {
                case DpiDirection::Input:
                    ++inputCount;
                    break;
                case DpiDirection::Output:
                    ++outputCount;
                    break;
                case DpiDirection::Inout:
                    ++inoutCount;
                    break;
                default:
                    return;
                }
            }

            const std::size_t expectedOperands =
                1 + inputCount + inoutCount + attributes->eventCount;
            const std::size_t expectedResults =
                (import.returnValue.present ? 1U : 0U) + outputCount + inoutCount;
            bool valid = operands.size() == expectedOperands && results.size() == expectedResults &&
                         isBitVector1(variableType(view, operands.front()));
            if (!valid)
            {
                semanticInstructionError(
                    validator, instruction,
                    "AM DPI call has an invalid operand or result count");
                return;
            }

            std::size_t operandPosition = 1;
            for (DpiDirection direction : {DpiDirection::Input, DpiDirection::Inout})
            {
                for (const DpiParameter &parameter : import.parameters)
                {
                    if (parameter.direction == direction)
                    {
                        valid = valid && variableMatchesType(
                                             view, operands[operandPosition++], parameter.type);
                    }
                }
            }
            const std::size_t eventBegin = operands.size() - attributes->eventCount;
            valid = valid && operandPosition == eventBegin &&
                    validateEventRange(view, operands, eventBegin);

            std::size_t resultPosition = 0;
            if (import.returnValue.present)
            {
                valid = variableMatchesType(view, results[resultPosition++],
                                            import.returnValue.type) &&
                        valid;
            }
            for (DpiDirection direction : {DpiDirection::Output, DpiDirection::Inout})
            {
                for (const DpiParameter &parameter : import.parameters)
                {
                    if (parameter.direction == direction)
                    {
                        valid = valid && variableMatchesType(
                                             view, results[resultPosition++], parameter.type);
                    }
                }
            }
            for (std::size_t lhs = 0; lhs < results.size(); ++lhs)
            {
                for (std::size_t rhs = lhs + 1; rhs < results.size(); ++rhs)
                {
                    if (results[lhs] == results[rhs])
                    {
                        valid = false;
                    }
                }
            }
            if (!valid)
            {
                semanticInstructionError(
                    validator, instruction,
                    "AM DPI call does not match its import signature");
            }
        }

        void validateInstructionSemantics(ProgramView view,
                                          InstructionId instruction,
                                          const std::vector<DpiImportEntry> &dpiImports,
                                          Validator &validator)
        {
            const Opcode opcode = view.opcode(instruction);
            if (!validOpcode(opcode))
            {
                return;
            }
            if (isPureCombinational(opcode))
            {
                validateCombinationalSemantics(view, instruction, validator);
            }
            else if (isChanged(opcode))
            {
                validateChangedSemantics(view, instruction, validator);
            }
            else if (isStateWriteOpcode(opcode) || opcode == Opcode::MemoryRead ||
                     opcode == Opcode::MemoryReadAll)
            {
                validateStateSemantics(view, instruction, validator);
            }
            else if (opcode == Opcode::SystemFunction || opcode == Opcode::SystemTask)
            {
                validateSystemSemantics(view, instruction, validator);
            }
            else if (opcode == Opcode::DpiCall)
            {
                validateDpiCallSemantics(view, instruction, dpiImports, validator);
            }
            else if (isActivation(opcode))
            {
                const auto operands = view.operands(instruction);
                if (operands.size() == 1 &&
                    !isUnsignedBitVector1(variableType(view, operands.front())))
                {
                    semanticInstructionError(
                        validator, instruction,
                        "AM activation event must be an unsigned one-bit bit vector");
                }
            }
        }

        void validateChangedOldOwnership(ProgramView view, Validator &validator)
        {
            struct ChangedOldOwner
            {
                uint32_t variable = 0;
                uint32_t instruction = 0;
            };
            std::vector<ChangedOldOwner> owners;
            for (uint32_t index = 0; index < view.instructionCount() && !validator.full(); ++index)
            {
                const InstructionId instruction{index};
                if (!isChanged(view.opcode(instruction)))
                {
                    continue;
                }
                const auto operands = view.operands(instruction);
                if (operands.size() != 2 || !operands[1].valid() ||
                    operands[1].value >= view.variableCount())
                {
                    continue;
                }
                owners.push_back(ChangedOldOwner{
                    .variable = operands[1].value,
                    .instruction = index,
                });
            }
            std::sort(owners.begin(), owners.end(), [](const ChangedOldOwner &lhs,
                                                       const ChangedOldOwner &rhs) {
                return lhs.variable < rhs.variable ||
                       (lhs.variable == rhs.variable && lhs.instruction < rhs.instruction);
            });
            for (std::size_t index = 1; index < owners.size() && !validator.full(); ++index)
            {
                if (owners[index - 1].variable == owners[index].variable)
                {
                    semanticInstructionError(
                        validator, InstructionId{owners[index].instruction},
                        "AM changed old variable is shared by multiple detectors");
                }
            }

            const auto findOwner = [&](VariableId variable) -> const ChangedOldOwner * {
                if (!variable.valid())
                {
                    return nullptr;
                }
                const auto it = std::lower_bound(
                    owners.begin(), owners.end(), variable.value,
                    [](const ChangedOldOwner &owner, uint32_t value) {
                        return owner.variable < value;
                    });
                return it != owners.end() && it->variable == variable.value ? &*it : nullptr;
            };

            for (uint32_t index = 0; index < view.instructionCount() && !validator.full(); ++index)
            {
                const InstructionId instruction{index};
                for (VariableId result : view.results(instruction))
                {
                    if (findOwner(result))
                    {
                        semanticInstructionError(
                            validator, instruction,
                            "AM changed old variable is written outside its detector");
                        break;
                    }
                }
                const auto operands = view.operands(instruction);
                for (std::size_t position = 0; position < operands.size(); ++position)
                {
                    const VariableId operand = operands[position];
                    const ChangedOldOwner *owner = findOwner(operand);
                    if (owner && !(owner->instruction == index && position == 1))
                    {
                        semanticInstructionError(
                            validator, instruction,
                            "AM changed old variable is used outside its detector");
                        break;
                    }
                }
            }
        }

        void validateInstructionTable(ProgramView view,
                                      Validator &validator,
                                      bool requireSingleResultWriter,
                                      const std::vector<DpiImportEntry> &dpiImports)
        {
            std::vector<uint32_t> producer;
            if (validator.semantic() && requireSingleResultWriter)
            {
                producer.assign(view.variableCount(), std::numeric_limits<uint32_t>::max());
            }

            for (uint32_t index = 0; index < view.instructionCount() && !validator.full(); ++index)
            {
                const InstructionId instruction{index};
                validateInstructionShape(view, instruction, validator);
                if (validator.semantic() && !validator.full())
                {
                    validateInstructionSemantics(view, instruction, dpiImports, validator);
                }
                if (!validator.semantic() || !requireSingleResultWriter)
                {
                    continue;
                }
                const auto operands = view.operands(instruction);
                for (VariableId result : view.results(instruction))
                {
                    if (!result.valid() || result.value >= producer.size())
                    {
                        continue;
                    }
                    if (std::find(operands.begin(), operands.end(), result) != operands.end())
                    {
                        validator.error("Linear AM normal form has a Result/Operand alias: variable=" +
                                        std::to_string(result.value));
                        break;
                    }
                    if (producer[result.value] != std::numeric_limits<uint32_t>::max())
                    {
                        validator.error("Linear AM normal form has multiple result writers: variable=" +
                                        std::to_string(result.value));
                        break;
                    }
                    producer[result.value] = index;
                }
            }
            if (validator.semantic() && !validator.full())
            {
                validateChangedOldOwnership(view, validator);
            }
        }

        void validateCommon(ProgramView view,
                            Validator &validator,
                            bool requireSingleResultWriter)
        {
            if (!view.valid())
            {
                validator.error("AM program is empty or moved-from");
                return;
            }
            std::vector<DpiImportEntry> dpiImports;
            validateTypeTable(view, validator);
            if (!validator.full())
            {
                validateVariables(view, validator);
            }
            if (!validator.full())
            {
                validateDpiImports(view, validator, dpiImports);
            }
            if (!validator.full())
            {
                validateInstructionTable(view, validator, requireSingleResultWriter, dpiImports);
            }
        }

        void validateScheduledBlocks(const ScheduledProgram &program, Validator &validator)
        {
            const ProgramView view = program.view();
            if (program.blockCount() == 0)
            {
                validator.error("ScheduledProgram has no EntryBlock");
                return;
            }

            std::vector<uint32_t> definitionBlock;
            if (validator.semantic())
            {
                definitionBlock.assign(view.variableCount(), std::numeric_limits<uint32_t>::max());
            }

            for (uint32_t blockIndex = 0; blockIndex < program.blockCount() && !validator.full(); ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0; position < program.blockSize(block) && !validator.full(); ++position)
                {
                    const InstructionId instruction = program.blockInstruction(block, position);
                    if (!instruction.valid() || instruction.value >= view.instructionCount())
                    {
                        validator.error("ScheduledProgram block contains an invalid InstructionId");
                        continue;
                    }
                    const Opcode opcode = view.opcode(instruction);
                    if (blockIndex == 0 &&
                        !(isPureCombinational(opcode) || isChanged(opcode) || opcode == Opcode::ActForward))
                    {
                        validator.error("EntryBlock contains a forbidden opcode: instruction=" +
                                        std::to_string(instruction.value) + " opcode=" +
                                        std::string(toString(opcode)));
                    }

                    if (isActivation(opcode))
                    {
                        const auto attributes = view.activationAttributes(instruction);
                        if (!attributes)
                        {
                            continue;
                        }
                        for (BlockId target : attributes->targets)
                        {
                            if (!target.valid() || target.value >= program.blockCount())
                            {
                                validator.error("activation target is outside the Program: instruction=" +
                                                std::to_string(instruction.value));
                                break;
                            }
                            if (opcode == Opcode::ActForward && target.value <= blockIndex)
                            {
                                validator.error("act.f target is not strictly forward: instruction=" +
                                                std::to_string(instruction.value));
                                break;
                            }
                            if (opcode == Opcode::ActBackward && target.value == 0)
                            {
                                validator.error("act.b targets EntryBlock: instruction=" +
                                                std::to_string(instruction.value));
                                break;
                            }
                        }
                        if (validator.semantic())
                        {
                            const auto operands = view.operands(instruction);
                            if (operands.size() == 1 && operands.front().valid() &&
                                operands.front().value < definitionBlock.size() &&
                                definitionBlock[operands.front().value] != blockIndex)
                            {
                                validator.error("activation event is not defined earlier in the same Block: instruction=" +
                                                std::to_string(instruction.value));
                            }
                        }
                    }

                    if (validator.semantic())
                    {
                        for (VariableId result : view.results(instruction))
                        {
                            if (result.valid() && result.value < definitionBlock.size())
                            {
                                definitionBlock[result.value] = blockIndex;
                            }
                        }
                    }
                }

                if (validator.semantic())
                {
                    for (std::size_t position = 0; position < program.blockSize(block); ++position)
                    {
                        const InstructionId instruction = program.blockInstruction(block, position);
                        for (VariableId result : view.results(instruction))
                        {
                            if (result.valid() && result.value < definitionBlock.size() &&
                                definitionBlock[result.value] == blockIndex)
                            {
                                definitionBlock[result.value] = std::numeric_limits<uint32_t>::max();
                            }
                        }
                    }
                }
            }
        }
    } // namespace

    ValidationResult validate(const LinearProgram &program, const ValidationOptions &options)
    {
        return validate(program.view(), options);
    }

    ValidationResult validate(ProgramView view, const ValidationOptions &options)
    {
        Validator validator(options);
        validateCommon(view, validator, true);
        if (view.valid())
        {
            for (uint32_t index = 0; index < view.instructionCount() && !validator.full(); ++index)
            {
                if (isActivation(view.opcode(InstructionId{index})))
                {
                    validator.error("LinearProgram contains an activation instruction: instruction=" +
                                    std::to_string(index));
                }
            }
        }
        return validator.finish();
    }

    ValidationResult validate(const ScheduledProgram &program, const ValidationOptions &options)
    {
        Validator validator(options);
        validateCommon(program.view(), validator, false);
        if (!validator.full() && program.valid())
        {
            validateScheduledBlocks(program, validator);
        }
        return validator.finish();
    }

} // namespace wolvrix::lib::grhsim::am
