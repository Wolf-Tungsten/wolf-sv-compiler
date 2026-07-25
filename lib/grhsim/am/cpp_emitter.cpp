#include "grhsim/am/cpp_emitter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
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

        std::string valueExpr(VariableId variable)
        {
            return "values_[" + std::to_string(variable.value) + "]";
        }

        std::string boolExpr(VariableId variable)
        {
            return "(" + valueExpr(variable) + " != 0)";
        }

        std::string maskExpr(uint32_t width)
        {
            if (width >= 64)
            {
                return "UINT64_MAX";
            }
            return "((UINT64_C(1) << " + std::to_string(width) + ") - UINT64_C(1))";
        }

        struct EmitState
        {
            struct Storage
            {
                uint64_t offset = 0;
                uint32_t wordCount = 0;
            };

            ProgramView program;
            std::vector<Type> variableTypes;
            std::vector<Storage> variableStorage;
            uint64_t wideWords = 0;
            uint64_t realValues = 0;
            uint64_t stringValues = 0;
            std::unordered_map<uint32_t, uint32_t> onceSlotByInstruction;
            uint32_t onceSlotCount = 0;
            std::unordered_map<uint32_t, uint32_t> pendingEventSlotByInstruction;
            uint32_t pendingEventSlotCount = 0;
            std::unordered_map<uint32_t, uint32_t> completedCommitWriteSlotByInstruction;
            uint32_t completedCommitWriteSlotCount = 0;
            std::vector<uint32_t> commitEventSlotByVariable;
            std::unordered_map<uint32_t, DpiImportId> dpiImportBySymbol;
            std::vector<bool> referencedDpiImports;
            std::vector<InstructionId> finalSystemTasks;
        };

        const Type &variableType(const EmitState &state, VariableId variable)
        {
            return state.variableTypes.at(variable.value);
        }

        const EmitState::Storage &variableStorage(const EmitState &state, VariableId variable)
        {
            return state.variableStorage.at(variable.value);
        }

        std::string changedResultCallPrefix(const EmitState &state, VariableId variable)
        {
            const uint32_t slot = state.commitEventSlotByVariable.at(variable.value);
            if (slot == std::numeric_limits<uint32_t>::max())
            {
                return "set_changed_result(" + std::to_string(variable.value) + ", ";
            }
            return "set_commit_changed_result(" + std::to_string(slot) + ", ";
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
            return "resize_value(" + valueExpr(variable) + ", " +
                   std::to_string(source.bitWidth) + ", " +
                   (signedness == Signedness::Signed ? "true" : "false") + ", " +
                   std::to_string(width) + ")";
        }

        std::string wordDataExpr(const EmitState &state, VariableId variable)
        {
            const Type &type = variableType(state, variable);
            return (isWideBitVector(state, variable) || type.kind == TypeKind::Array)
                       ? wideDataExpr(state, variable)
                       : "&" + valueExpr(variable);
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
                    return valueExpr(target) + " = resize_value(" + valueExpr(source) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " + sign + ", " +
                           std::to_string(targetType.bitWidth) + ");\n";
                }
                return valueExpr(target) + " = (" + wordDataExpr(state, source) + ")[0] & " +
                       maskExpr(targetType.bitWidth) + ";\n";
            }
            if (sourceType.bitWidth <= 64)
            {
                return "assign_words_from_scalar(" + wideDataExpr(state, target) + ", " +
                       std::to_string(targetType.bitWidth) + ", " + valueExpr(source) + ", " +
                       std::to_string(sourceType.bitWidth) + ", " + sign + ");\n";
            }
            return "assign_words(" + wideDataExpr(state, target) + ", " +
                   std::to_string(targetType.bitWidth) + ", " + wideDataExpr(state, source) +
                   ", " + std::to_string(sourceType.bitWidth) + ", " + sign + ");\n";
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
                return "TaskArgument::logic_scalar(" + valueExpr(variable) + ", " +
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
                           valueExpr(variable) + " & " + maskExpr(type.bitWidth) + ")";
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
                    return valueExpr(target) + " = static_cast<std::uint64_t>(" +
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
            std::string expression = boolExpr(operands.front());
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
                expression += boolExpr(operands[index]);
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
                expression += boolExpr(operands[index]);
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
            if (name != "fwrite" && name != "finish")
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
                fire = boolExpr(operands.front()) + " && " + pending;
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
                code += "const std::uint64_t " + handleName + " = " + valueExpr(handle) +
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
                code += "finishRequested_ = true;\n";
                if (argumentCount != 0)
                {
                    const Type &exitType = variableType(state, operands[1]);
                    if (exitType.kind != TypeKind::BitVector || exitType.bitWidth > 64)
                    {
                        error = "finish system.task exit code must be scalar logic";
                        return std::nullopt;
                    }
                    code += "systemExitCode_ = static_cast<int>(" + valueExpr(operands[1]) +
                            " & " + maskExpr(exitType.bitWidth) + ");\n";
                }
                else
                {
                    code += "systemExitCode_ = 0;\n";
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
                fire = boolExpr(operands.front()) + " && " + pending;
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
                return valueExpr(results.front()) + " = (" + expression + ") ? 1 : 0;\n";
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
                    return "if (" + boolExpr(operands[0]) + ") { " +
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
                    const std::string helper =
                        opcode == Opcode::SliceDynamic ? "slice_dynamic_words" : "slice_array_words";
                    return helper + "(" + wordDataExpr(state, results.front()) + ", " +
                           std::to_string(resultType.bitWidth) + ", " +
                           wordDataExpr(state, operands[0]) + ", " +
                           std::to_string(sourceType.bitWidth) + ", " +
                           wordDataExpr(state, operands[1]) + ", " +
                           std::to_string(indexType.bitWidth) + ");\n";
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

        std::optional<std::string> emitInstruction(const EmitState &state,
                                                   InstructionId instruction,
                                                   std::string &error)
        {
            const Opcode opcode = state.program.opcode(instruction);
            const auto operands = state.program.operands(instruction);
            const auto results = state.program.results(instruction);
            const auto resultAssign = [&](std::string expression) {
                const Type &resultType = variableType(state, results.front());
                return valueExpr(results.front()) + " = (" + expression + ") & " +
                       maskExpr(resultType.bitWidth) + ";\n";
            };
            const auto binaryOperands = [&](const Type &resultType) {
                const std::string lhs = resizedExpr(
                    state, operands[0], resultType.bitWidth, resultType.signedness);
                const std::string rhs = resizedExpr(
                    state, operands[1], resultType.bitWidth, resultType.signedness);
                return std::array<std::string, 2>{lhs, rhs};
            };
            const auto emitEventfulStateWrite =
                [&](std::size_t eventBegin, std::string condition, std::string body) {
                    const std::string eventHit = eventHitExpr(
                        state, operands,
                        static_cast<uint32_t>(operands.size() - eventBegin));
                    const auto slot =
                        state.completedCommitWriteSlotByInstruction.find(instruction.value);
                    if (slot == state.completedCommitWriteSlotByInstruction.end())
                    {
                        return "if (" + condition + " && " + eventHit + ") { " + body +
                               " }\n";
                    }

                    const std::string completed =
                        "completedCommitWrites_[" + std::to_string(slot->second) + "]";
                    const std::string event =
                        "commit_event_hit_" + std::to_string(instruction.value);
                    return "{ const bool " + event + " = " + eventHit + ";\n" +
                           "if (" + event + " && !" + completed + ") {\n" +
                           completed + " = true;\n" +
                           "if (" + condition + ") { " + body + " }\n" +
                           "}\n}\n";
                };

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
                    return resultAssign(wideDataExpr(state, operands.front()) + "[0]");
                }
                const std::string sign =
                    sourceType.signedness == Signedness::Signed ? "true" : "false";
                if (sourceType.bitWidth <= 64)
                {
                    return "assign_words_from_scalar(" + wideDataExpr(state, results.front()) +
                           ", " + std::to_string(resultType.bitWidth) + ", " +
                           valueExpr(operands.front()) + ", " +
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
                    changedResultCallPrefix(state, results.front());
                const Type &type = variableType(state, operands.front());
                if (type.kind == TypeKind::Array)
                {
                    if (opcode != Opcode::ChangedAny)
                    {
                        error = "edge changed opcode requires a bit-vector operand";
                        return std::nullopt;
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

            if ((opcode == Opcode::RegisterWrite || opcode == Opcode::LatchWrite) &&
                isWideBitVector(state, operands[3]))
            {
                const uint32_t width = variableType(state, operands[3]).bitWidth;
                const std::string body =
                    "masked_write_words(" + wideDataExpr(state, operands[3]) + ", " +
                    wideDataExpr(state, operands[2]) + ", " +
                    wideDataExpr(state, operands[1]) + ", " +
                    std::to_string(width) + ");";
                if (opcode == Opcode::RegisterWrite)
                {
                    return emitEventfulStateWrite(4, boolExpr(operands[0]), body);
                }
                return "if (" + boolExpr(operands[0]) + ") { " + body + " }\n";
            }

            const bool deferredUnsupported =
                opcode == Opcode::MemoryRead || opcode == Opcode::MemoryWrite ||
                opcode == Opcode::MemoryFill || opcode == Opcode::SystemFunction ||
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
                    return resultAssign("~" + valueExpr(operands.front()));
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
                    return resultAssign(boolExpr(operands[0]) + " && " + boolExpr(operands[1]));
                case Opcode::LogicOr:
                    return resultAssign(boolExpr(operands[0]) + " || " + boolExpr(operands[1]));
                case Opcode::LogicNot:
                    return resultAssign("!" + boolExpr(operands[0]));
                case Opcode::ReduceAnd:
                case Opcode::ReduceNand:
                {
                    const Type &sourceType = variableType(state, operands.front());
                    const std::string reduced = "((" + valueExpr(operands.front()) + " & " +
                                                maskExpr(sourceType.bitWidth) + ") == " +
                                                maskExpr(sourceType.bitWidth) + ")";
                    return resultAssign(opcode == Opcode::ReduceAnd ? reduced : "!(" + reduced + ")");
                }
                case Opcode::ReduceOr:
                case Opcode::ReduceNor:
                {
                    const std::string reduced = boolExpr(operands.front());
                    return resultAssign(opcode == Opcode::ReduceOr ? reduced : "!(" + reduced + ")");
                }
                case Opcode::ReduceXor:
                case Opcode::ReduceXnor:
                {
                    const std::string reduced = "(std::popcount(" + valueExpr(operands.front()) + ") & 1U)";
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
                    return resultAssign(std::string(helper) + "(" + valueExpr(operands[0]) + ", " +
                                        valueExpr(operands[1]) + ", " +
                                        std::to_string(resultType.bitWidth) + ", " +
                                        (resultType.signedness == Signedness::Signed ? "true" : "false") + ")");
                }
                case Opcode::Mux:
                {
                    const Type &resultType = variableType(state, results.front());
                    return resultAssign(boolExpr(operands[0]) + " ? " +
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
                                std::to_string(accumulated) + ", " + valueExpr(operand) + ", " +
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
                                valueExpr(operands.front()) + ", " +
                                std::to_string(sourceType.bitWidth) + ");\n";
                    }
                    code += resultAssign("replicate_" + suffix);
                    code += "}\n";
                    return code;
                }
                case Opcode::SliceStatic:
                {
                    const auto attributes = state.program.sliceStaticAttributes(instruction);
                    return resultAssign("slice_value(" + valueExpr(operands.front()) + ", " +
                                        std::to_string(attributes->lsb) + ", " +
                                        std::to_string(variableType(state, results.front()).bitWidth) + ")");
                }
                case Opcode::SliceDynamic:
                    return resultAssign("slice_value(" + valueExpr(operands[0]) + ", " +
                                        valueExpr(operands[1]) + ", " +
                                        std::to_string(variableType(state, results.front()).bitWidth) + ")");
                case Opcode::SliceArray:
                    return resultAssign("slice_array_value(" + valueExpr(operands[0]) + ", " +
                                        valueExpr(operands[1]) + ", " +
                                        std::to_string(variableType(state, results.front()).bitWidth) + ", " +
                                        std::to_string(variableType(state, operands[0]).bitWidth) + ")");
                case Opcode::ChangedAny:
                case Opcode::ChangedPos:
                case Opcode::ChangedNeg:
                {
                    std::string event;
                    if (opcode == Opcode::ChangedAny)
                    {
                        event = valueExpr(operands[0]) + " != " + valueExpr(operands[1]);
                    }
                    else if (opcode == Opcode::ChangedPos)
                    {
                        event = "(" + valueExpr(operands[1]) + " == 0 && " +
                                valueExpr(operands[0]) + " != 0)";
                    }
                    else
                    {
                        event = "(" + valueExpr(operands[1]) + " != 0 && " +
                                valueExpr(operands[0]) + " == 0)";
                    }
                    return changedResultCallPrefix(state, results.front()) + event +
                           ");\n" +
                           valueExpr(operands[1]) + " = " + valueExpr(operands[0]) + ";\n";
                }
                case Opcode::RegisterWrite:
                {
                    const VariableId target = operands[3];
                    const std::string body =
                        valueExpr(target) + " = ((" + valueExpr(target) + " & ~" +
                        valueExpr(operands[1]) + ") | (" + valueExpr(operands[2]) + " & " +
                        valueExpr(operands[1]) + ")) & " +
                        maskExpr(variableType(state, target).bitWidth) + ";";
                    return emitEventfulStateWrite(4, boolExpr(operands[0]), body);
                }
                case Opcode::LatchWrite:
                {
                    const VariableId target = operands[3];
                    return "if (" + boolExpr(operands[0]) + ") { " + valueExpr(target) + " = ((" +
                           valueExpr(target) + " & ~" + valueExpr(operands[1]) + ") | (" +
                           valueExpr(operands[2]) + " & " + valueExpr(operands[1]) + ")) & " +
                           maskExpr(variableType(state, target).bitWidth) + "; }\n";
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
                        code += valueExpr(results.front()) + " = 0; } else { " +
                                valueExpr(results.front()) + " = (" + row + ")[0] & " +
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
                {
                    const Type &memoryType = variableType(state, operands[4]);
                    const Type &addressType = variableType(state, operands[1]);
                    const uint32_t stride = variableStorage(state, operands[4]).wordCount;
                    const std::string suffix = std::to_string(instruction.value);
                    const std::string index = "memory_index_" + suffix;
                    std::string code = "{ const std::size_t " + index + " = index_words(" +
                                       wordDataExpr(state, operands[1]) + ", " +
                                       std::to_string(addressType.bitWidth) + ", " +
                                       std::to_string(memoryType.elementCount) + ");\n";
                    const std::string condition =
                        boolExpr(operands[0]) + " && " + index + " != " +
                        std::to_string(memoryType.elementCount);
                    const std::string body =
                        "masked_write_words(" + wordDataExpr(state, operands[4]) + " + " +
                        index + " * " + std::to_string(stride) + ", " +
                        wordDataExpr(state, operands[3]) + ", " +
                        wordDataExpr(state, operands[2]) + ", " +
                        std::to_string(memoryType.bitWidth) + ");";
                    code += emitEventfulStateWrite(5, condition, body) + "}\n";
                    return code;
                }
                case Opcode::MemoryFill:
                {
                    const Type &memoryType = variableType(state, operands[2]);
                    const Type &dataType = variableType(state, operands[1]);
                    const uint32_t stride = variableStorage(state, operands[2]).wordCount;
                    const std::string suffix = std::to_string(instruction.value);
                    const std::string element = "memory_element_" + suffix;
                    std::string body = "for (std::size_t " + element + " = 0; " + element +
                                       " < " + std::to_string(memoryType.elementCount) + "; ++" +
                                       element + ") { ";
                    const std::string target = wordDataExpr(state, operands[2]) + " + " + element +
                                               " * " + std::to_string(stride);
                    if (dataType.bitWidth == memoryType.bitWidth)
                    {
                        body += "assign_words(" + target + ", " +
                                std::to_string(memoryType.bitWidth) + ", " +
                                wordDataExpr(state, operands[1]) + ", " +
                                std::to_string(dataType.bitWidth) + ", false);";
                    }
                    else
                    {
                        body += "slice_words(" + target + ", " +
                                std::to_string(memoryType.bitWidth) + ", " +
                                wordDataExpr(state, operands[1]) + ", " +
                                std::to_string(dataType.bitWidth) + ", " + element + " * " +
                                std::to_string(memoryType.bitWidth) + ");";
                    }
                    body += " }";
                    return emitEventfulStateWrite(3, boolExpr(operands[0]), body);
                }
                case Opcode::ActForward:
                case Opcode::ActBackward:
                {
                    const auto attributes = state.program.activationAttributes(instruction);
                    std::string code = "if (" + boolExpr(operands.front()) + ") {\n";
                    for (BlockId target : attributes->targets)
                    {
                        code += opcode == Opcode::ActForward ? "activate_forward(" :
                                                              "activate_backward(";
                        code += std::to_string(target.value) + ");\n";
                    }
                    code += "}\n";
                    return code;
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

        std::string blockSourceFunctionName(std::size_t sourceIndex,
                                            std::size_t partIndex)
        {
            std::string name = "execute_blocks_" + std::to_string(sourceIndex);
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

        std::string blockSourcePrologue(std::string_view prefix,
                                        std::string_view className,
                                        std::size_t sourceIndex,
                                        std::size_t partIndex)
        {
            return "#include \"" + std::string(prefix) + ".hpp\"\n" +
                   "#include \"" + std::string(prefix) + "_support.hpp\"\n\n" +
                   "void " + std::string(className) + "::" +
                   blockSourceFunctionName(sourceIndex, partIndex) +
                   "(std::size_t block) {\n    switch (block) {\n";
        }

        constexpr std::string_view kBlockSourceEpilogue =
            "    default: throw std::runtime_error(\"invalid AM BlockId\");\n"
            "    }\n}\n";

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
        measureBlockCase(const ExecutableModel &model,
                         const EmitState &state,
                         std::size_t blockIndex,
                         wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            uint64_t byteCount = static_cast<uint64_t>(
                ("    case " + std::to_string(blockIndex) + ": {\n").size());
            const BlockId block{static_cast<uint32_t>(blockIndex)};
            for (std::size_t index = 0; index < model.program.blockSize(block); ++index)
            {
                const InstructionId instruction =
                    model.program.blockInstruction(block, index);
                std::string error;
                const std::optional<std::string> code =
                    emitInstruction(state, instruction, error);
                if (!code)
                {
                    diagnostics.error(error + ": instruction=" +
                                          std::to_string(instruction.value),
                                      std::string(kContext));
                    return std::nullopt;
                }
                const std::optional<uint64_t> codeBytes =
                    indentedLineByteCount(*code, "        ");
                if (!codeBytes || !addByteCount(byteCount, *codeBytes))
                {
                    diagnostics.error("AM C++ emitter source size overflow: block=" +
                                          std::to_string(blockIndex),
                                      std::string(kContext));
                    return std::nullopt;
                }
            }
            constexpr std::string_view kBlockCaseEpilogue =
                "        break;\n    }\n";
            if (!addByteCount(byteCount, kBlockCaseEpilogue.size()))
            {
                diagnostics.error("AM C++ emitter source size overflow: block=" +
                                      std::to_string(blockIndex),
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
                    partBytes = static_cast<uint64_t>(
                                    blockSourcePrologue(prefix,
                                                        className,
                                                        sourceIndex,
                                                        partIndex)
                                        .size()) +
                                static_cast<uint64_t>(kBlockSourceEpilogue.size());
                };
                resetPartBytes();

                for (std::size_t blockIndex = firstBlock;
                     blockIndex < endBlock;
                     ++blockIndex)
                {
                    const std::optional<uint64_t> blockBytes =
                        measureBlockCase(model, state, blockIndex, diagnostics);
                    if (!blockBytes)
                    {
                        return std::nullopt;
                    }
                    const bool exceedsBudget =
                        partBytes > maxSourceBytes ||
                        *blockBytes > maxSourceBytes - partBytes;
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
                    }
                    if (!addByteCount(partBytes, *blockBytes))
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
        std::vector<uint32_t> commitEventVariables;
        std::vector<uint32_t> commitWriteInstructions;
        if (model.commitBlockBegin != 0)
        {
            std::vector<bool> changedVariable(program.variableCount(), false);
            for (uint32_t instructionIndex = 0;
                 instructionIndex < program.instructionCount(); ++instructionIndex)
            {
                const InstructionId instruction{instructionIndex};
                const Opcode opcode = program.opcode(instruction);
                if (opcode == Opcode::ChangedAny || opcode == Opcode::ChangedPos ||
                    opcode == Opcode::ChangedNeg)
                {
                    changedVariable[program.results(instruction).front().value] = true;
                }
            }
            for (uint32_t blockIndex = model.commitBlockBegin;
                 blockIndex < model.commitBlockEnd; ++blockIndex)
            {
                const BlockId block{blockIndex};
                for (std::size_t position = 0;
                     position < model.program.blockSize(block); ++position)
                {
                    const InstructionId instruction =
                        model.program.blockInstruction(block, position);
                    const Opcode opcode = program.opcode(instruction);
                    const auto operands = program.operands(instruction);
                    std::size_t eventBegin = operands.size();
                    if (opcode == Opcode::RegisterWrite)
                    {
                        commitWriteInstructions.push_back(instruction.value);
                        eventBegin = 4;
                    }
                    else if (opcode == Opcode::MemoryWrite)
                    {
                        commitWriteInstructions.push_back(instruction.value);
                        eventBegin = 5;
                    }
                    else if (opcode == Opcode::MemoryFill)
                    {
                        commitWriteInstructions.push_back(instruction.value);
                        eventBegin = 3;
                    }
                    for (std::size_t index = eventBegin; index < operands.size(); ++index)
                    {
                        if (changedVariable[operands[index].value])
                        {
                            commitEventVariables.push_back(operands[index].value);
                        }
                    }
                }
            }
            std::sort(commitEventVariables.begin(), commitEventVariables.end());
            commitEventVariables.erase(
                std::unique(commitEventVariables.begin(), commitEventVariables.end()),
                commitEventVariables.end());
            std::sort(commitWriteInstructions.begin(), commitWriteInstructions.end());
            commitWriteInstructions.erase(
                std::unique(commitWriteInstructions.begin(), commitWriteInstructions.end()),
                commitWriteInstructions.end());
        }
        EmitState state{.program = program};
        state.commitEventSlotByVariable.assign(
            program.variableCount(), std::numeric_limits<uint32_t>::max());
        for (uint32_t slot = 0; slot < commitEventVariables.size(); ++slot)
        {
            state.commitEventSlotByVariable[commitEventVariables[slot]] = slot;
        }
        for (uint32_t instruction : commitWriteInstructions)
        {
            state.completedCommitWriteSlotByInstruction.emplace(
                instruction, state.completedCommitWriteSlotCount++);
        }
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
                static_cast<std::size_t>(Opcode::ActBackward) + 1U;
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
            const std::string name(program.string(port.name));
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
        const std::size_t blockCount = model.program.blockCount();
        const std::size_t blockSourceCount =
            blockCount / *blocksPerSource + (blockCount % *blocksPerSource == 0 ? 0 : 1);
        const std::size_t activityWordCount = (blockCount + 63U) / 64U;
        const std::size_t activitySummaryWordCount = (activityWordCount + 63U) / 64U;
        const std::size_t dirtyChangedWordCount =
            (static_cast<std::size_t>(program.variableCount()) + 63U) / 64U;
        const std::size_t commitEventWordCount =
            (commitEventVariables.size() + 63U) / 64U;

        const std::string prefix = "grhsim_" + options.modelName;
        const std::string className = "GrhSIM_" + options.modelName;
        const std::optional<BlockSourcePlan> blockSourcePlan =
            planBlockSources(model,
                             state,
                             *blocksPerSource,
                             blockSourceCount,
                             *maxSourceBytes,
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
               << "#include <array>\n#include <chrono>\n#include <cstddef>\n#include <cstdint>\n#include <string>\n#include <vector>\n\n"
               << "#define WOLVRIX_GRHSIM_PERF 0\n\n"
               << "class " << className << " {\npublic:\n"
               << "    " << className << "();\n"
               << "    ~" << className << "();\n"
               << "    void init();\n"
               << "    void eval();\n"
               << "    void finalize();\n"
               << "    void set_random_seed(std::uint64_t seed);\n"
               << "    [[nodiscard]] bool had_register_write_conflict() const;\n"
               << "    void set_runtime_profile_enabled(bool enabled);\n"
               << "    [[nodiscard]] bool runtime_profile_enabled() const;\n"
               << "    void dump_runtime_profile() const;\n"
               << "    static constexpr bool kRuntimeProfileCompiled = false;\n"
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
            header << "    " << cppPortType(type) << " " << program.string(port.name)
                   << "{};\n";
        }
        header << "\nprivate:\n"
               << "    void execute_block(std::size_t block);\n";
        for (const std::vector<BlockSourcePart> &sourceParts : *blockSourcePlan)
        {
            for (const BlockSourcePart &part : sourceParts)
            {
                header << "    void "
                       << blockSourceFunctionName(part.sourceIndex, part.partIndex)
                       << "(std::size_t block);\n";
            }
        }
        header << "    void activate_forward(std::size_t block);\n"
               << "    void activate_backward(std::size_t block);\n"
               << "    void activate_all_blocks();\n"
               << "    void execute_active_blocks();\n"
               << "    bool execute_next_commit_group();\n"
               << "    void capture_pending_commit_operands();\n"
               << "    [[nodiscard]] bool has_active_blocks() const;\n"
               << "    [[nodiscard]] bool has_next_active_blocks() const;\n"
               << "    [[nodiscard]] bool has_pending_commit_blocks() const;\n"
               << "    [[nodiscard]] static bool is_commit_block(std::size_t block);\n"
               << "    void capture_commit_events();\n"
               << "    void restore_commit_events();\n"
               << "    void clear_pending_commit_events();\n"
               << "    void set_changed_result(std::size_t variable, bool event) {\n"
               << "        values_[variable] = event ? 1 : 0;\n"
               << "        if (event) mark_changed_result(variable);\n"
               << "    }\n"
               << "    void set_commit_changed_result(std::uint32_t commitEventSlot, bool event) {\n"
               << "        const std::size_t variable = kCommitEventVariables_[commitEventSlot];\n"
               << "        values_[variable] = event ? 1 : 0;\n"
               << "        if (event) mark_commit_changed_result(variable, commitEventSlot);\n"
               << "    }\n"
               << "    void mark_changed_result(std::size_t variable);\n"
               << "    void mark_commit_changed_result(std::size_t variable, std::uint32_t commitEventSlot);\n"
               << "    void clear_changed_results();\n"
               << "    static constexpr std::uint64_t bit_mask(std::uint32_t width) {\n"
               << "        return width >= 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1);\n"
               << "    }\n"
               << "    static std::size_t word_count(std::uint32_t width);\n"
               << "    static bool any_words(const std::uint64_t *value, std::uint32_t width);\n"
               << "    static bool equal_words(const std::uint64_t *lhs, const std::uint64_t *rhs, std::uint32_t width);\n"
               << "    static void assign_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend);\n"
               << "    static void assign_words_from_scalar(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t source, std::uint32_t sourceWidth, bool signExtend);\n"
               << "    static void masked_write_words(std::uint64_t *target, const std::uint64_t *data, const std::uint64_t *mask, std::uint32_t width);\n"
               << "    static std::uint64_t resized_word(const std::uint64_t *source, std::uint32_t sourceWidth, bool signExtend, std::size_t index);\n"
               << "    static void zero_words(std::uint64_t *target, std::uint32_t width);\n"
               << "    static void bitwise_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *lhs, std::uint32_t lhsWidth, bool lhsSigned, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool rhsSigned, std::uint32_t operation);\n"
               << "    static void arithmetic_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *lhs, std::uint32_t lhsWidth, bool lhsSigned, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool rhsSigned, std::uint32_t operation);\n"
               << "    static void not_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, bool sourceSigned);\n"
               << "    static int compare_words(const std::uint64_t *lhs, std::uint32_t lhsWidth, const std::uint64_t *rhs, std::uint32_t rhsWidth, bool isSigned);\n"
               << "    static bool reduce_words(const std::uint64_t *source, std::uint32_t width, std::uint32_t operation);\n"
               << "    static std::size_t index_words(const std::uint64_t *source, std::uint32_t width, std::size_t limit);\n"
               << "    static std::uint64_t extract_word(const std::uint64_t *source, std::uint32_t width, std::uint64_t start);\n"
               << "    static void insert_words(std::uint64_t *target, std::uint32_t targetWidth, std::uint64_t targetLsb, const std::uint64_t *source, std::uint32_t sourceWidth);\n"
               << "    static void slice_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, std::uint64_t start);\n"
               << "    static void slice_dynamic_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, const std::uint64_t *index, std::uint32_t indexWidth);\n"
               << "    static void slice_array_words(std::uint64_t *target, std::uint32_t targetWidth, const std::uint64_t *source, std::uint32_t sourceWidth, const std::uint64_t *index, std::uint32_t indexWidth);\n"
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
               << "    static constexpr std::size_t kActivityWordCount = " << activityWordCount
               << ";\n"
               << "    static constexpr std::size_t kActivitySummaryWordCount = "
               << activitySummaryWordCount << ";\n"
               << "    static constexpr std::size_t kDirtyChangedWordCount = "
               << dirtyChangedWordCount << ";\n"
               << "    static constexpr std::size_t kCommitEventCount = "
               << commitEventVariables.size() << ";\n"
               << "    static constexpr std::size_t kCommitEventWordCount = "
               << commitEventWordCount << ";\n"
               << "    static const std::array<std::uint32_t, kCommitEventCount> "
                  "kCommitEventVariables_;\n"
               << "    static constexpr std::size_t kCommitOperandCaptureCount = "
               << model.commitOperandCaptures.size() << ";\n"
               << "    static const std::array<std::uint32_t, kCommitBlockCount + 1> "
                  "kCommitOperandCaptureOffsets_;\n"
               << "    static const std::array<std::uint32_t, kCommitOperandCaptureCount> "
                  "kCommitOperandCaptureSources_;\n"
               << "    static const std::array<std::uint32_t, kCommitOperandCaptureCount> "
                  "kCommitOperandCaptureTargets_;\n"
               << "    static const std::array<std::uint32_t, kCommitOperandCaptureCount> "
                  "kCommitOperandCaptureWords_;\n"
               << "    std::array<std::uint64_t, " << program.variableCount() << "> values_{};\n"
               << "    std::array<std::uint64_t, " << state.wideWords << "> wideValues_{};\n"
               << "    std::array<std::uint64_t, " << state.realValues << "> realValues_{};\n"
               << "    std::array<std::string, " << state.stringValues << "> stringValues_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> activeWords_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> nextActiveWords_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> pendingCommitWords_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> forcedCommitWords_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> nextCommitWords_{};\n"
               << "    std::array<std::uint64_t, kActivityWordCount> capturedCommitWords_{};\n"
               << "    std::array<std::uint64_t, kActivitySummaryWordCount> activeSummary_{};\n"
               << "    std::array<std::uint64_t, kActivitySummaryWordCount> nextActiveSummary_{};\n"
               << "    std::array<std::uint64_t, kActivitySummaryWordCount> pendingCommitSummary_{};\n"
               << "    std::array<std::uint64_t, kActivitySummaryWordCount> nextCommitSummary_{};\n"
               << "    std::array<std::uint64_t, kDirtyChangedWordCount> dirtyChangedBits_{};\n"
               << "    std::vector<std::uint32_t> dirtyChangedResults_;\n"
               << "    std::vector<std::uint32_t> dirtyCommitEventSlots_;\n"
               << "    std::vector<std::uint32_t> pendingCommitEventSlots_;\n"
               << "    std::array<std::uint64_t, kCommitEventWordCount> pendingCommitEventBits_{};\n"
               << "    std::array<bool, " << state.completedCommitWriteSlotCount
               << "> completedCommitWrites_{};\n"
               << "    std::array<bool, " << state.onceSlotCount << "> onceCompleted_{};\n"
               << "    std::array<bool, " << state.pendingEventSlotCount
               << "> pendingHostEvents_{};\n"
               << "    bool firstEval_ = true;\n"
               << "    std::uint64_t epochCounter_ = 0;\n"
               << "    std::uint64_t randomSeed_ = 0;\n"
               << "    bool runtimeProfileEnabled_ = false;\n"
               << "    std::uint64_t profileEvalCalls_ = 0;\n"
               << "    std::uint64_t profileEpochs_ = 0;\n"
               << "    std::uint64_t profileBlockExecs_ = 0;\n"
               << "    std::uint64_t profileCommitBlockExecs_ = 0;\n"
               << "    std::uint64_t profileCommitGroups_ = 0;\n"
               << "    std::uint64_t profileActivateForward_ = 0;\n"
               << "    std::uint64_t profileActivateBackward_ = 0;\n"
               << "    std::uint64_t profileChangedMarks_ = 0;\n"
               << "    std::uint64_t profileCommitChangedMarks_ = 0;\n"
               << "    std::uint64_t profileChangedClears_ = 0;\n"
               << "    std::uint64_t profileCaptureBlocks_ = 0;\n"
               << "    std::uint64_t profileCaptureWords_ = 0;\n"
               << "    std::uint64_t profileComputeNs_ = 0;\n"
               << "    std::uint64_t profileCommitNs_ = 0;\n"
               << "    std::uint64_t profileEvalNs_ = 0;\n"
               << "    std::array<std::uint64_t, kBlockCount> profilePerBlockExecs_{};\n"
               << "    bool finalized_ = false;\n"
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
        runtime << "const std::array<std::uint32_t, " << commitEventVariables.size()
                << "> " << className << "::kCommitEventVariables_ = {\n";
        constexpr std::size_t commitEventsPerLine = 16;
        for (std::size_t index = 0; index < commitEventVariables.size(); ++index)
        {
            if (index % commitEventsPerLine == 0)
            {
                runtime << "    ";
            }
            runtime << commitEventVariables[index];
            if (index + 1 != commitEventVariables.size())
            {
                runtime << ", ";
            }
            if (index % commitEventsPerLine + 1 == commitEventsPerLine ||
                index + 1 == commitEventVariables.size())
            {
                runtime << '\n';
            }
        }
        runtime << "};\n\n";
        const auto emitUint32Array = [&](std::string_view name,
                                         std::span<const uint32_t> values,
                                         std::size_t declaredSize) {
            runtime << "const std::array<std::uint32_t, " << declaredSize << "> "
                    << className << "::" << name << " = {\n";
            constexpr std::size_t valuesPerLine = 16;
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                if (index % valuesPerLine == 0)
                {
                    runtime << "    ";
                }
                runtime << values[index];
                if (index + 1 != values.size())
                {
                    runtime << ", ";
                }
                if (index % valuesPerLine + 1 == valuesPerLine ||
                    index + 1 == values.size())
                {
                    runtime << '\n';
                }
            }
            runtime << "};\n\n";
        };
        std::vector<uint32_t> captureOffsets = model.commitOperandCaptureOffsets;
        if (captureOffsets.empty())
        {
            captureOffsets.resize(
                static_cast<std::size_t>(model.commitBlockEnd - model.commitBlockBegin) + 1,
                0);
        }
        std::vector<uint32_t> captureSources;
        std::vector<uint32_t> captureTargets;
        std::vector<uint32_t> captureWords;
        captureSources.reserve(model.commitOperandCaptures.size());
        captureTargets.reserve(model.commitOperandCaptures.size());
        captureWords.reserve(model.commitOperandCaptures.size());
        for (const CommitOperandCapture &capture : model.commitOperandCaptures)
        {
            const Type &type = variableType(state, capture.source);
            if (type.bitWidth <= 64)
            {
                captureSources.push_back(capture.source.value);
                captureTargets.push_back(capture.target.value);
                captureWords.push_back(0);
            }
            else
            {
                const EmitState::Storage &sourceStorage =
                    variableStorage(state, capture.source);
                const EmitState::Storage &targetStorage =
                    variableStorage(state, capture.target);
                captureSources.push_back(sourceStorage.offset);
                captureTargets.push_back(targetStorage.offset);
                captureWords.push_back(sourceStorage.wordCount);
            }
        }
        emitUint32Array("kCommitOperandCaptureOffsets_", captureOffsets,
                        captureOffsets.size());
        emitUint32Array("kCommitOperandCaptureSources_", captureSources,
                        captureSources.size());
        emitUint32Array("kCommitOperandCaptureTargets_", captureTargets,
                        captureTargets.size());
        emitUint32Array("kCommitOperandCaptureWords_", captureWords,
                        captureWords.size());
        runtime << className << "::" << className << "() = default;\n"
               << className << "::~" << className << "() { finalize(); }\n\n"
               << "std::size_t " << className
               << "::word_count(std::uint32_t width) { return (static_cast<std::size_t>(width) + 63U) / 64U; }\n"
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
               << "std::uint64_t " << className
               << "::extract_word(const std::uint64_t *source, std::uint32_t width, std::uint64_t start) {\n"
               << "    if (start >= width) return 0;\n"
               << "    const std::size_t sourceWord = static_cast<std::size_t>(start / 64U);\n"
               << "    const std::uint32_t shift = static_cast<std::uint32_t>(start % 64U);\n"
               << "    std::uint64_t value = source[sourceWord] >> shift;\n"
               << "    if (shift != 0 && sourceWord + 1U < word_count(width)) value |= source[sourceWord + 1U] << (64U - shift);\n"
               << "    const std::uint64_t remaining = static_cast<std::uint64_t>(width) - start;\n"
               << "    return value & bit_mask(static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, 64U)));\n"
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
               << "::init() {\n    values_.fill(0); wideValues_.fill(0); realValues_.fill(0);\n"
               << "    for (std::string &value : stringValues_) value.clear();\n"
               << "    activeWords_.fill(0); nextActiveWords_.fill(0);\n"
               << "    activeSummary_.fill(0); nextActiveSummary_.fill(0);\n"
               << "    pendingCommitWords_.fill(0); forcedCommitWords_.fill(0); nextCommitWords_.fill(0);\n"
               << "    pendingCommitSummary_.fill(0); nextCommitSummary_.fill(0);\n"
               << "    pendingCommitEventBits_.fill(0);\n"
               << "    dirtyCommitEventSlots_.clear(); pendingCommitEventSlots_.clear();\n"
               << "    completedCommitWrites_.fill(false);\n"
               << "    dirtyChangedBits_.fill(0); dirtyChangedResults_.clear(); onceCompleted_.fill(false);\n"
               << "    firstEval_ = true; epochCounter_ = 0; finalized_ = false;\n"
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
                runtime << "    values_[" << variable.value << "] = (" << value << ") & "
                        << maskExpr(type.bitWidth) << ";\n";
            }
            else if (type.kind == TypeKind::BitVector)
            {
                std::optional<LiteralView> literal;
                if (expression.kind == InitExprKind::Literal)
                {
                    literal = program.literal(expression.literal);
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
                << "::activate_forward(std::size_t block) {\n"
                << "    if (runtimeProfileEnabled_) ++profileActivateForward_;\n"
                << "    if (block >= kBlockCount) throw std::runtime_error(\"invalid AM BlockId\");\n"
                << "    const std::size_t word = block / 64U;\n"
                << "    if (is_commit_block(block)) {\n"
                << "        const std::uint64_t bit = UINT64_C(1) << (block % 64U);\n"
                << "        if ((pendingCommitWords_[word] & bit) == 0) capturedCommitWords_[word] &= ~bit;\n"
                << "        pendingCommitWords_[word] |= UINT64_C(1) << (block % 64U);\n"
                << "        pendingCommitSummary_[word / 64U] |= UINT64_C(1) << (word % 64U);\n"
                << "        return;\n"
                << "    }\n"
                << "    activeWords_[word] |= UINT64_C(1) << (block % 64U);\n"
                << "    activeSummary_[word / 64U] |= UINT64_C(1) << (word % 64U);\n"
                << "}\n\nvoid " << className
                << "::activate_backward(std::size_t block) {\n"
                << "    if (runtimeProfileEnabled_) ++profileActivateBackward_;\n"
                << "    if (block >= kBlockCount) throw std::runtime_error(\"invalid AM BlockId\");\n"
                << "    const std::size_t word = block / 64U;\n"
                << "    if (is_commit_block(block)) {\n"
                << "        const std::uint64_t bit = UINT64_C(1) << (block % 64U);\n"
                << "        if ((pendingCommitWords_[word] & bit) == 0) capturedCommitWords_[word] &= ~bit;\n"
                << "        nextCommitWords_[word] |= UINT64_C(1) << (block % 64U);\n"
                << "        nextCommitSummary_[word / 64U] |= UINT64_C(1) << (word % 64U);\n"
                << "        return;\n"
                << "    }\n"
                << "    nextActiveWords_[word] |= UINT64_C(1) << (block % 64U);\n"
                << "    nextActiveSummary_[word / 64U] |= UINT64_C(1) << (word % 64U);\n"
                << "}\n\nvoid " << className
                << "::activate_all_blocks() {\n"
                << "    activeWords_.fill(UINT64_MAX);\n"
                << "    const std::size_t tailBits = kBlockCount - (kActivityWordCount - 1U) * 64U;\n"
                << "    activeWords_.back() &= bit_mask(static_cast<std::uint32_t>(tailBits));\n"
                << "    activeWords_[0] &= ~UINT64_C(1);\n"
                << "    for (std::size_t block = kCommitBlockBegin; block < kCommitBlockEnd; ++block) {\n"
                << "        const std::size_t word = block / 64U;\n"
                << "        const std::uint64_t bit = UINT64_C(1) << (block % 64U);\n"
                << "        activeWords_[word] &= ~bit;\n"
                << "        forcedCommitWords_[word] |= bit;\n"
                << "        pendingCommitSummary_[word / 64U] |= UINT64_C(1) << (word % 64U);\n"
                << "    }\n"
                << "    activeSummary_.fill(0);\n"
                << "    for (std::size_t word = 0; word < kActivityWordCount; ++word) {\n"
                << "        if (activeWords_[word] != 0) activeSummary_[word / 64U] |= UINT64_C(1) << (word % 64U);\n"
                << "    }\n"
                << "}\n\nvoid " << className
                << "::execute_active_blocks() {\n"
                << "    for (std::size_t summaryWord = 0; summaryWord < kActivitySummaryWordCount; ++summaryWord) {\n"
                << "        while (activeSummary_[summaryWord] != 0) {\n"
                << "            const std::size_t summaryBit = static_cast<std::size_t>(std::countr_zero(activeSummary_[summaryWord]));\n"
                << "            const std::size_t activityWord = summaryWord * 64U + summaryBit;\n"
                << "            if (activityWord >= kActivityWordCount) {\n"
                << "                activeSummary_[summaryWord] &= ~(UINT64_C(1) << summaryBit);\n"
                << "                continue;\n"
                << "            }\n"
                << "            while (activeWords_[activityWord] != 0) {\n"
                << "                const std::size_t blockBit = static_cast<std::size_t>(std::countr_zero(activeWords_[activityWord]));\n"
                << "                activeWords_[activityWord] &= ~(UINT64_C(1) << blockBit);\n"
                << "                if (activeWords_[activityWord] == 0) {\n"
                << "                    activeSummary_[summaryWord] &= ~(UINT64_C(1) << summaryBit);\n"
                << "                }\n"
                << "                const std::size_t block = activityWord * 64U + blockBit;\n"
                << "                if (block != 0 && block < kBlockCount) execute_block(block);\n"
                << "            }\n"
                << "        }\n"
                << "    }\n"
                << "}\n\nbool " << className
                << "::has_active_blocks() const {\n"
                << "    for (const std::uint64_t summary : activeSummary_) {\n"
                << "        if (summary != 0) return true;\n"
                << "    }\n"
                << "    return false;\n"
                << "}\n\nbool " << className
                << "::has_next_active_blocks() const {\n"
                << "    for (std::size_t word = 0; word < kActivitySummaryWordCount; ++word) {\n"
                << "        if (nextActiveSummary_[word] != 0 || nextCommitSummary_[word] != 0) return true;\n"
                << "    }\n"
                << "    return false;\n"
                << "}\n\nbool " << className
                << "::has_pending_commit_blocks() const {\n"
                << "    for (const std::uint64_t summary : pendingCommitSummary_) {\n"
                << "        if (summary != 0) return true;\n"
                << "    }\n"
                << "    return false;\n"
                << "}\n\nvoid " << className
                << "::capture_pending_commit_operands() {\n"
                << "    for (std::size_t word = 0; word < kActivityWordCount; ++word) {\n"
                << "        std::uint64_t uncaptured = (pendingCommitWords_[word] | forcedCommitWords_[word]) & ~capturedCommitWords_[word];\n"
                << "        while (uncaptured != 0) {\n"
                << "            const std::size_t bit = static_cast<std::size_t>(std::countr_zero(uncaptured));\n"
                << "            uncaptured &= ~(UINT64_C(1) << bit);\n"
                << "            const std::size_t block = word * 64U + bit;\n"
                << "            if (block < kCommitBlockBegin || block >= kCommitBlockEnd) continue;\n"
                << "            if (runtimeProfileEnabled_) ++profileCaptureBlocks_;\n"
                << "            const std::size_t local = block - kCommitBlockBegin;\n"
                << "            for (std::uint32_t index = kCommitOperandCaptureOffsets_[local]; index < kCommitOperandCaptureOffsets_[local + 1]; ++index) {\n"
                << "                const std::uint32_t words = kCommitOperandCaptureWords_[index];\n"
                << "                if (runtimeProfileEnabled_) profileCaptureWords_ += words == 0 ? 1 : words;\n"
                << "                if (words == 0) values_[kCommitOperandCaptureTargets_[index]] = values_[kCommitOperandCaptureSources_[index]];\n"
                << "                else std::copy_n(wideValues_.data() + kCommitOperandCaptureSources_[index], words, wideValues_.data() + kCommitOperandCaptureTargets_[index]);\n"
                << "            }\n"
                << "            capturedCommitWords_[word] |= UINT64_C(1) << bit;\n"
                << "        }\n"
                << "    }\n"
                << "}\n\nbool " << className
                << "::execute_next_commit_group() {\n";
        for (std::size_t group = 0; group + 1 < model.commitGroupOffsets.size(); ++group)
        {
            const uint32_t begin = model.commitGroupOffsets[group];
            const uint32_t end = model.commitGroupOffsets[group + 1];
            runtime << "    if (";
            for (uint32_t index = begin; index < end; ++index)
            {
                if (index != begin)
                {
                    runtime << " || ";
                }
                const uint32_t block = model.commitBlockOrder[index].value;
                runtime << "((pendingCommitWords_[" << block / 64U
                        << "] | forcedCommitWords_[" << block / 64U
                        << "]) & capturedCommitWords_[" << block / 64U
                        << "] & (UINT64_C(1) << " << block % 64U << ")) != 0";
            }
            runtime << ") {\n";
            for (uint32_t index = begin; index < end; ++index)
            {
                const uint32_t block = model.commitBlockOrder[index].value;
                runtime << "        const bool selectedCommitBlock" << block
                        << " = ((pendingCommitWords_[" << block / 64U
                        << "] | forcedCommitWords_[" << block / 64U
                        << "]) & capturedCommitWords_[" << block / 64U
                        << "] & (UINT64_C(1) << " << block % 64U << ")) != 0;\n";
            }
            for (uint32_t index = begin; index < end; ++index)
            {
                const uint32_t block = model.commitBlockOrder[index].value;
                runtime << "        if (selectedCommitBlock" << block << ") {\n"
                        << "            pendingCommitWords_[" << block / 64U
                        << "] &= ~(UINT64_C(1) << " << block % 64U << ");\n"
                        << "            forcedCommitWords_[" << block / 64U
                        << "] &= ~(UINT64_C(1) << " << block % 64U << ");\n"
                        << "            if ((pendingCommitWords_[" << block / 64U
                        << "] | forcedCommitWords_[" << block / 64U
                        << "]) == 0) pendingCommitSummary_[" << (block / 64U) / 64U
                        << "] &= ~(UINT64_C(1) << " << (block / 64U) % 64U << ");\n"
                        << "            capturedCommitWords_[" << block / 64U
                        << "] &= ~(UINT64_C(1) << " << block % 64U << ");\n"
                        << "        }\n";
            }
            for (uint32_t index = begin; index < end; ++index)
            {
                const uint32_t block = model.commitBlockOrder[index].value;
                runtime << "        if (selectedCommitBlock" << block
                        << ") execute_block(" << block << ");\n";
            }
            runtime << "        if (runtimeProfileEnabled_) ++profileCommitGroups_;\n"
                    << "        return true;\n"
                    << "    }\n";
        }
        runtime << "    return false;\n"
                << "}\n\nvoid " << className
                << "::capture_commit_events() {\n"
                << "    for (const std::uint32_t slot : dirtyCommitEventSlots_) {\n"
                << "        const std::uint32_t variable = kCommitEventVariables_[slot];\n"
                << "        if (values_[variable] == 0) continue;\n"
                << "        const std::size_t word = slot / 64U;\n"
                << "        const std::uint64_t bit = UINT64_C(1) << (slot % 64U);\n"
                << "        if ((pendingCommitEventBits_[word] & bit) == 0) {\n"
                << "            pendingCommitEventBits_[word] |= bit;\n"
                << "            pendingCommitEventSlots_.push_back(slot);\n"
                << "        }\n"
                << "    }\n"
                << "}\n\nvoid " << className
                << "::restore_commit_events() {\n"
                << "    for (const std::uint32_t slot : pendingCommitEventSlots_) {\n"
                << "        set_commit_changed_result(slot, true);\n"
                << "    }\n"
                << "}\n\nvoid " << className
                << "::clear_pending_commit_events() {\n"
                << "    for (const std::uint32_t slot : pendingCommitEventSlots_) {\n"
                << "        pendingCommitEventBits_[slot / 64U] &= ~(UINT64_C(1) << (slot % 64U));\n"
                << "    }\n"
                << "    pendingCommitEventSlots_.clear();\n"
                << "}\n\nvoid " << className
                << "::mark_commit_changed_result(std::size_t variable, std::uint32_t commitEventSlot) {\n"
                << "    if (runtimeProfileEnabled_) ++profileCommitChangedMarks_;\n"
                << "    const std::size_t word = variable / 64U;\n"
                << "    const std::uint64_t bit = UINT64_C(1) << (variable % 64U);\n"
                << "    if ((dirtyChangedBits_[word] & bit) == 0) {\n"
                << "        dirtyChangedBits_[word] |= bit;\n"
                << "        dirtyChangedResults_.push_back(static_cast<std::uint32_t>(variable));\n"
                << "        dirtyCommitEventSlots_.push_back(commitEventSlot);\n"
                << "    }\n"
                << "}\n\nvoid " << className
                << "::mark_changed_result(std::size_t variable) {\n"
                << "    if (runtimeProfileEnabled_) ++profileChangedMarks_;\n"
                << "    const std::size_t word = variable / 64U;\n"
                << "    const std::uint64_t bit = UINT64_C(1) << (variable % 64U);\n"
                << "    if ((dirtyChangedBits_[word] & bit) == 0) {\n"
                << "        dirtyChangedBits_[word] |= bit;\n"
                << "        dirtyChangedResults_.push_back(static_cast<std::uint32_t>(variable));\n"
                << "    }\n"
                << "}\n\nvoid " << className
                << "::clear_changed_results() {\n"
                << "    if (runtimeProfileEnabled_) profileChangedClears_ += dirtyChangedResults_.size();\n"
                << "    for (const std::uint32_t variable : dirtyChangedResults_) {\n"
                << "        values_[variable] = 0;\n"
                << "        dirtyChangedBits_[variable / 64U] &= ~(UINT64_C(1) << (variable % 64U));\n"
                << "    }\n"
                << "    dirtyChangedResults_.clear();\n"
                << "    dirtyCommitEventSlots_.clear();\n"
                << "}\n\nvoid " << className << "::execute_block(std::size_t block) {\n"
                << "    if (block >= kBlockCount) throw std::runtime_error(\"invalid AM BlockId\");\n"
                << "    if (runtimeProfileEnabled_) {\n"
                << "        profilePerBlockExecs_[block] += 1;\n"
                << "        if (is_commit_block(block)) ++profileCommitBlockExecs_; else ++profileBlockExecs_;\n"
                << "    }\n"
                << "    switch (block / " << *blocksPerSource << "U) {\n";
        for (std::size_t sourceIndex = 0;
             sourceIndex < blockSourcePlan->size();
             ++sourceIndex)
        {
            const std::vector<BlockSourcePart> &sourceParts =
                (*blockSourcePlan)[sourceIndex];
            if (sourceParts.size() == 1)
            {
                runtime << "    case " << sourceIndex << ": "
                        << blockSourceFunctionName(sourceIndex, 0)
                        << "(block); return;\n";
                continue;
            }
            runtime << "    case " << sourceIndex << ":\n";
            for (std::size_t partIndex = 0;
                 partIndex + 1U < sourceParts.size();
                 ++partIndex)
            {
                const BlockSourcePart &part = sourceParts[partIndex];
                runtime << "        if (block < " << part.endBlock << "U) { "
                        << blockSourceFunctionName(sourceIndex, part.partIndex)
                        << "(block); return; }\n";
            }
            const BlockSourcePart &lastPart = sourceParts.back();
            runtime << "        "
                    << blockSourceFunctionName(sourceIndex, lastPart.partIndex)
                    << "(block); return;\n";
        }
        runtime << "    default: throw std::runtime_error(\"invalid AM BlockId\");\n"
                << "    }\n"
                << "}\n\n"
                << "void " << className << "::finalize() {\n"
                << "    if (finalized_) return;\n"
                << "    finalized_ = true;\n";
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
                << "    if (runtimeProfileEnabled_) ++profileEvalCalls_;\n"
                << "    const auto profileEvalStart = std::chrono::steady_clock::now();\n";
        for (const PortBinding &port : model.interface.ports)
        {
            if (port.direction != PortDirection::Input)
            {
                continue;
            }
            const Type &type = variableType(state, port.input);
            if (type.bitWidth <= 64)
            {
                runtime << "    values_[" << port.input.value
                        << "] = static_cast<std::uint64_t>(" << program.string(port.name)
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
                            << program.string(port.name) << "[" << word << "] & "
                            << maskExpr(bits) << ";\n";
                }
            }
        }
        for (const PreCommitSnapshot &snapshot : model.preCommitSnapshots)
        {
            const Type &type = variableType(state, snapshot.source);
            runtime << "    "
                    << assignVariableStatement(state, snapshot.target,
                                               snapshot.source,
                                               type.signedness);
        }
        runtime << "    const bool initial = firstEval_;\n"
                << "    epochCounter_ = 0;\n"
                << "    activeWords_.fill(0); activeSummary_.fill(0);\n"
                << "    nextActiveWords_.fill(0); nextActiveSummary_.fill(0);\n"
                << "    pendingCommitWords_.fill(0); forcedCommitWords_.fill(0); pendingCommitSummary_.fill(0);\n"
                << "    nextCommitWords_.fill(0); nextCommitSummary_.fill(0);\n"
                << "    capturedCommitWords_.fill(0);\n"
                << "    clear_pending_commit_events();\n"
                << "    completedCommitWrites_.fill(false);\n"
                << "    pendingHostEvents_.fill(false);\n"
                << "    clear_changed_results();\n"
                << "    execute_block(0);\n"
                << "    if (initial) activate_all_blocks();\n"
                << "    while (true) {\n"
                << "        const auto profileComputeStart = runtimeProfileEnabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};\n"
                << "        execute_active_blocks();\n"
                << "        if (runtimeProfileEnabled_) profileComputeNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - profileComputeStart).count());\n"
                << "        if (has_next_active_blocks()) {\n"
                << "            activeWords_ = nextActiveWords_;\n"
                << "            activeSummary_ = nextActiveSummary_;\n"
                << "            for (std::size_t word = 0; word < kActivityWordCount; ++word) pendingCommitWords_[word] |= nextCommitWords_[word];\n"
                << "            for (std::size_t word = 0; word < kActivitySummaryWordCount; ++word) pendingCommitSummary_[word] |= nextCommitSummary_[word];\n"
                << "            nextActiveWords_.fill(0); nextActiveSummary_.fill(0);\n"
                << "            nextCommitWords_.fill(0); nextCommitSummary_.fill(0);\n"
                << "            if (has_pending_commit_blocks()) capture_commit_events();\n"
                << "            ++epochCounter_;\n"
                << "            clear_changed_results();\n"
                << "            if (epochCounter_ > UINT64_C(1000000)) throw std::runtime_error(\"AM eval did not converge\");\n"
                << "            continue;\n"
                << "        }\n"
                << "        if (has_pending_commit_blocks()) {\n"
                << "            const auto profileCommitStart = runtimeProfileEnabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};\n"
                << "            capture_pending_commit_operands();\n"
                << "            capture_commit_events();\n"
                << "            restore_commit_events();\n"
                << "            if (!execute_next_commit_group()) throw std::runtime_error(\"pending commit Block is absent from its execution plan\");\n"
                << "            if (runtimeProfileEnabled_) profileCommitNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - profileCommitStart).count());\n"
                << "            if (has_next_active_blocks()) {\n"
                << "                for (std::size_t word = 0; word < kActivityWordCount; ++word) {\n"
                << "                    activeWords_[word] |= nextActiveWords_[word];\n"
                << "                    pendingCommitWords_[word] |= nextCommitWords_[word];\n"
                << "                }\n"
                << "                for (std::size_t word = 0; word < kActivitySummaryWordCount; ++word) {\n"
                << "                    activeSummary_[word] |= nextActiveSummary_[word];\n"
                << "                    pendingCommitSummary_[word] |= nextCommitSummary_[word];\n"
                << "                }\n"
                << "                nextActiveWords_.fill(0); nextActiveSummary_.fill(0);\n"
                << "                nextCommitWords_.fill(0); nextCommitSummary_.fill(0);\n"
                << "                if (has_pending_commit_blocks()) capture_commit_events();\n"
                << "                ++epochCounter_;\n"
                << "                clear_changed_results();\n"
                << "                if (epochCounter_ > UINT64_C(1000000)) throw std::runtime_error(\"AM eval did not converge\");\n"
                << "                continue;\n"
                << "            }\n"
                << "            if (has_active_blocks()) { clear_changed_results(); continue; }\n"
                << "            if (!has_pending_commit_blocks()) { clear_pending_commit_events(); clear_changed_results(); }\n"
                << "            continue;\n"
                << "        }\n"
                << "        break;\n"
                << "    }\n"
                << "    if (initial) firstEval_ = false;\n"
                << "    if (runtimeProfileEnabled_) {\n"
                << "        profileEpochs_ += epochCounter_;\n"
                << "        profileEvalNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - profileEvalStart).count());\n"
                << "    }\n";
        for (const PortBinding &port : model.interface.ports)
        {
            if (port.direction != PortDirection::Output)
            {
                continue;
            }
            const Type &type = variableType(state, port.output);
            if (type.bitWidth <= 64)
            {
                runtime << "    " << program.string(port.name) << " = static_cast<"
                        << cppScalarType(type.bitWidth) << ">(values_["
                        << port.output.value << "]);\n";
            }
            else
            {
                const EmitState::Storage &storage = variableStorage(state, port.output);
                for (uint32_t word = 0; word < storage.wordCount; ++word)
                {
                    runtime << "    " << program.string(port.name) << "[" << word
                            << "] = wideValues_[" << storage.offset + word << "];\n";
                }
            }
        }
        runtime << "}\n\n"
                << "void " << className
                << "::set_random_seed(std::uint64_t seed) { randomSeed_ = seed; }\n"
                << "bool " << className
                << "::had_register_write_conflict() const { return false; }\n"
                << "void " << className
                << "::set_runtime_profile_enabled(bool enabled) { runtimeProfileEnabled_ = enabled; }\n"
                << "bool " << className
                << "::runtime_profile_enabled() const { return runtimeProfileEnabled_; }\n"
                << "void " << className << "::dump_runtime_profile() const {\n"
                << "    const std::uint64_t totalBlockExecs = profileBlockExecs_ + profileCommitBlockExecs_;\n"
                << "    const double evalMs = static_cast<double>(profileEvalNs_) / 1.0e6;\n"
                << "    const double computeMs = static_cast<double>(profileComputeNs_) / 1.0e6;\n"
                << "    const double commitMs = static_cast<double>(profileCommitNs_) / 1.0e6;\n"
                << "    std::cerr << \"[am-profile] eval calls: \" << profileEvalCalls_ << \", epochs: \" << profileEpochs_\n"
                << "              << \" (\" << (profileEvalCalls_ != 0 ? static_cast<double>(profileEpochs_) / static_cast<double>(profileEvalCalls_) : 0.0) << \" per eval)\\n\";\n"
                << "    std::cerr << \"[am-profile] block execs: \" << totalBlockExecs << \" (compute \" << profileBlockExecs_\n"
                << "              << \", commit \" << profileCommitBlockExecs_ << \", commit groups \" << profileCommitGroups_ << \")\\n\";\n"
                << "    std::cerr << \"[am-profile] activations: forward \" << profileActivateForward_ << \", backward \" << profileActivateBackward_ << \"\\n\";\n"
                << "    std::cerr << \"[am-profile] changed marks: \" << profileChangedMarks_ << \" (commit events \" << profileCommitChangedMarks_\n"
                << "              << \"), clears \" << profileChangedClears_ << \"\\n\";\n"
                << "    std::cerr << \"[am-profile] commit captures: blocks \" << profileCaptureBlocks_ << \", words \" << profileCaptureWords_ << \"\\n\";\n"
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
                << "}\n"
                << "bool " << className
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
        makefile << "CXX ?= clang++\n"
                 << "AR ?= ar\n"
                 << "ARFLAGS ?= rcs\n"
                 << "CXXFLAGS ?= -std=c++20 -O3\n"
                 << "LIB := lib" << prefix << ".a\n"
                 << "SRCS :=";
        for (const std::string &sourceName : sourceNames)
        {
            makefile << " " << sourceName;
        }
        makefile << "\n"
                 << "OBJS := $(SRCS:.cpp=.o)\n\n"
                 << "all: $(LIB)\n\n"
                 << "$(LIB): $(OBJS)\n\t$(AR) $(ARFLAGS) $@ $^\n\n"
                 << "%.o: %.cpp " << prefix << ".hpp " << prefix << "_support.hpp\n"
                 << "\t$(CXX) $(CXXFLAGS) -I. -c $< -o $@\n\n"
                 << "clean:\n\trm -f $(OBJS) $(LIB)\n";

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

                blockSource << blockSourcePrologue(prefix,
                                                   className,
                                                   part.sourceIndex,
                                                   part.partIndex);
                for (std::size_t blockIndex = part.firstBlock;
                     blockIndex < part.endBlock;
                     ++blockIndex)
                {
                    blockSource << "    case " << blockIndex << ": {\n";
                    const BlockId block{static_cast<uint32_t>(blockIndex)};
                    for (std::size_t index = 0;
                         index < model.program.blockSize(block);
                         ++index)
                    {
                        const InstructionId instruction =
                            model.program.blockInstruction(block, index);
                        std::string error;
                        const std::optional<std::string> code =
                            emitInstruction(state, instruction, error);
                        if (!code)
                        {
                            diagnostics.error(error + ": instruction=" +
                                                  std::to_string(instruction.value),
                                              std::string(kContext));
                            blocksGenerated = false;
                            break;
                        }
                        writeIndentedLines(blockSource, *code, "        ");
                    }
                    if (!blocksGenerated)
                    {
                        break;
                    }
                    blockSource << "        break;\n    }\n";
                }
                if (!blocksGenerated)
                {
                    break;
                }
                blockSource << kBlockSourceEpilogue;
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
        result.artifacts.reserve(stagedArtifacts.size());
        for (const StagedArtifact &artifact : stagedArtifacts)
        {
            result.artifacts.push_back(artifact.destination.string());
        }
        return result;
    }

} // namespace wolvrix::lib::grhsim::am
