#include "grhsim/emit/cpu_single_thread.hpp"

#include "grhsim/ir/generic.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::string_view kRuntimeHeader =
#include "cpu_single_thread_runtime.hpp.inc"
            ;

        enum class ValueClass : uint8_t
        {
            Logic,
            Real,
            String,
            Invalid,
        };

        struct ValueLayout
        {
            ValueClass valueClass = ValueClass::Invalid;
            uint32_t width = 0;
            bool isSigned = false;
            std::size_t offset = 0;
        };

        struct StateLayout : ValueLayout
        {
            StateId id;
            StateKind kind = StateKind::State;
            uint32_t rows = 0;
            uint32_t elementWidth = 0;
            std::string name;
        };

        struct PendingWriteLayout
        {
            OpId op;
            GenericOpcode opcode = GenericOpcode::MemWrite;
            StateId state;
            std::size_t scheduleIndex = 0;
            std::size_t dataOffset = 0;
            std::size_t maskOffset = 0;
            int64_t priority = 0;
        };

        struct PortLayout
        {
            std::string name;
            std::string identifier;
            StateId input = StateId::invalid();
            StateId output = StateId::invalid();
            StateId outputEnable = StateId::invalid();

            bool inout() const noexcept
            {
                return input.valid() && output.valid() && outputEnable.valid();
            }
        };

        struct DpiParameter
        {
            HostParam parameter;
            std::string typeName;
            std::string baseType;
            std::string declarationType;
        };

        struct DpiLayout
        {
            HostId host;
            std::string binding;
            std::string field;
            std::string returnType = "void";
            std::vector<DpiParameter> parameters;
        };

        struct RandomInit
        {
            std::size_t stateOffset = 0;
            uint32_t stateWidth = 0;
            uint64_t bitOffset = 0;
            uint32_t width = 0;
        };

        struct EmitModel
        {
            std::string stem;
            std::string className;
            std::vector<OpId> schedule;
            std::vector<ValueLayout> edges;
            std::vector<StateLayout> states;
            std::vector<int64_t> trackIndexByState;
            std::vector<StateId> trackSet;
            std::vector<int64_t> eventSlotByState;
            std::vector<StateId> eventStates;
            std::vector<PendingWriteLayout> pendingWrites;
            std::vector<std::size_t> pendingWriteByOp;
            std::vector<std::size_t> memoryWriteOrder;
            std::vector<PortLayout> ports;
            std::vector<DpiLayout> dpi;
            std::vector<int64_t> dpiByHost;
            std::vector<uint64_t> initialStateWords;
            std::vector<RandomInit> randomInitializers;
            std::size_t edgeWords = 0;
            std::size_t edgeReals = 0;
            std::size_t edgeStrings = 0;
            std::size_t stateWords = 0;
            std::size_t stateReals = 0;
            std::size_t stateStrings = 0;
            std::size_t pendingWords = 0;
        };

        std::size_t wordCount(uint32_t width)
        {
            return (static_cast<std::size_t>(width) + 63U) / 64U;
        }

        std::string sanitizeIdentifier(std::string_view text)
        {
            std::string result;
            result.reserve(text.size() + 1U);
            for (unsigned char ch : text)
            {
                result.push_back(std::isalnum(ch) || ch == '_' ? static_cast<char>(ch) : '_');
            }
            if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front())))
            {
                result.insert(result.begin(), '_');
            }
            static const std::unordered_set<std::string> keywords{
                "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
                "bool", "break", "case", "catch", "char", "class", "compl", "concept",
                "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
                "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do",
                "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
                "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
                "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
                "operator", "or", "or_eq", "private", "protected", "public", "register",
                "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
                "static", "static_assert", "static_cast", "struct", "switch", "template",
                "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
                "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
                "wchar_t", "while", "xor", "xor_eq",
            };
            if (keywords.contains(result))
            {
                result.push_back('_');
            }
            return result;
        }

        std::string escapeCppString(std::string_view text)
        {
            std::ostringstream out;
            for (unsigned char ch : text)
            {
                switch (ch)
                {
                case '\\': out << "\\\\"; break;
                case '"': out << "\\\""; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (ch < 0x20U || ch >= 0x7fU)
                    {
                        out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<unsigned>(ch) << std::dec;
                    }
                    else
                    {
                        out << static_cast<char>(ch);
                    }
                    break;
                }
            }
            return out.str();
        }

        const AttrValue *findAttr(const Module &module, std::span<const AttrKV> attrs,
                                  std::string_view name)
        {
            for (const AttrKV &attr : attrs)
            {
                if (module.symbol(attr.key) == name)
                {
                    return &attr.value;
                }
            }
            return nullptr;
        }

        template <typename T>
        const T *attrAs(const Module &module, std::span<const AttrKV> attrs,
                        std::string_view name)
        {
            const AttrValue *value = findAttr(module, attrs, name);
            return value ? std::get_if<T>(value) : nullptr;
        }

        template <typename T>
        const T *opAttr(const Module &module, OpId op, std::string_view name)
        {
            const AttrValue *value = module.attr(op, name);
            return value ? std::get_if<T>(value) : nullptr;
        }

        std::string_view symbolAttr(const Module &module, OpId op, std::string_view name)
        {
            const SymbolId *symbol = opAttr<SymbolId>(module, op, name);
            return symbol ? module.symbol(*symbol) : std::string_view{};
        }

        std::vector<std::string> symbolArrayAttr(const Module &module,
                                                 std::span<const AttrKV> attrs,
                                                 std::string_view name)
        {
            std::vector<std::string> result;
            const auto *values = attrAs<std::vector<SymbolId>>(module, attrs, name);
            if (!values)
            {
                return result;
            }
            result.reserve(values->size());
            for (SymbolId value : *values)
            {
                result.emplace_back(module.symbol(value));
            }
            return result;
        }

        std::optional<std::vector<uint64_t>> parseLogicLiteral(std::string_view literal,
                                                               uint32_t width)
        {
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch != '_' && !std::isspace(static_cast<unsigned char>(ch)))
                {
                    compact.push_back(ch);
                }
            }
            if (compact.empty() || compact.front() == '"' || compact.front() == '$')
            {
                return std::nullopt;
            }
            bool negative = false;
            if (compact.front() == '-' || compact.front() == '+')
            {
                negative = compact.front() == '-';
                compact.erase(compact.begin());
            }
            try
            {
                slang::SVInt value = slang::SVInt::fromString(compact);
                if (negative)
                {
                    value = -value;
                }
                value = value.resize(static_cast<slang::bitwidth_t>(width));
                if (value.hasUnknown())
                {
                    value.flattenUnknowns();
                }
                std::vector<uint64_t> words(wordCount(width), 0);
                const uint64_t *raw = value.getRawPtr();
                std::copy_n(raw, words.size(), words.begin());
                if ((width & 63U) != 0)
                {
                    words.back() &= (UINT64_C(1) << (width & 63U)) - UINT64_C(1);
                }
                return words;
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        void writePackedBit(std::vector<uint64_t> &words, std::size_t base,
                            uint64_t bit, bool value)
        {
            const std::size_t index = base + static_cast<std::size_t>(bit >> 6U);
            const uint64_t mask = UINT64_C(1) << (bit & 63U);
            words[index] = value ? words[index] | mask : words[index] & ~mask;
        }

        void writePackedValue(std::vector<uint64_t> &destination, std::size_t base,
                              uint64_t lsb, uint32_t width,
                              const std::vector<uint64_t> &value)
        {
            for (uint32_t bit = 0; bit < width; ++bit)
            {
                const bool set = ((value[bit >> 6U] >> (bit & 63U)) & UINT64_C(1)) != 0;
                writePackedBit(destination, base, lsb + bit, set);
            }
        }

        std::optional<uint64_t> parseAddress(std::string_view text)
        {
            uint64_t value = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size()
                       ? std::optional<uint64_t>(value)
                       : std::nullopt;
        }

        std::vector<std::string> tokenizeReadmem(std::string_view text)
        {
            std::vector<std::string> tokens;
            std::string token;
            bool lineComment = false;
            bool blockComment = false;
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const char ch = text[index];
                const char next = index + 1U < text.size() ? text[index + 1U] : '\0';
                if (lineComment)
                {
                    if (ch == '\n') lineComment = false;
                    continue;
                }
                if (blockComment)
                {
                    if (ch == '*' && next == '/')
                    {
                        blockComment = false;
                        ++index;
                    }
                    continue;
                }
                if (ch == '/' && next == '/')
                {
                    if (!token.empty())
                    {
                        tokens.push_back(std::move(token));
                        token.clear();
                    }
                    lineComment = true;
                    ++index;
                    continue;
                }
                if (ch == '/' && next == '*')
                {
                    if (!token.empty())
                    {
                        tokens.push_back(std::move(token));
                        token.clear();
                    }
                    blockComment = true;
                    ++index;
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(ch)))
                {
                    if (!token.empty())
                    {
                        tokens.push_back(std::move(token));
                        token.clear();
                    }
                }
                else
                {
                    token.push_back(ch);
                }
            }
            if (!token.empty()) tokens.push_back(std::move(token));
            return tokens;
        }

        bool isSuffix(std::string_view value, std::string_view suffix)
        {
            return value.size() >= suffix.size() &&
                   value.substr(value.size() - suffix.size()) == suffix;
        }

        std::string stripSuffix(std::string_view value, std::string_view suffix)
        {
            return isSuffix(value, suffix)
                       ? std::string(value.substr(0, value.size() - suffix.size()))
                       : std::string(value);
        }

        ValueLayout layoutForType(const Module &module, TypeId typeId,
                                  std::size_t &logicWords,
                                  std::size_t &reals,
                                  std::size_t &strings,
                                  wolvrix::lib::diag::Diagnostics &diagnostics,
                                  std::string_view context,
                                  bool allowArray)
        {
            const TypeRec *type = module.type(typeId);
            if (!type || type->track != TypeTrack::Generic)
            {
                diagnostics.error("emit-cpu-single-thread requires generic semantic Types",
                                  std::string(context));
                return {};
            }
            ValueLayout result;
            if (type->kind == static_cast<uint8_t>(GenericTypeKind::Logic))
            {
                result.valueClass = ValueClass::Logic;
                result.width = type->width;
                result.isSigned = type->isSigned;
                result.offset = logicWords;
                logicWords += wordCount(result.width);
                return result;
            }
            if (type->kind == static_cast<uint8_t>(GenericTypeKind::Array) && allowArray)
            {
                const TypeRec *element = module.type(type->elementType);
                const uint64_t width = element ? static_cast<uint64_t>(type->rows) * element->width : 0;
                if (!element || width == 0 || width > std::numeric_limits<uint32_t>::max())
                {
                    diagnostics.error("array StateDecl exceeds the CPU backend width limit",
                                      std::string(context));
                    return {};
                }
                result.valueClass = ValueClass::Logic;
                result.width = static_cast<uint32_t>(width);
                result.isSigned = element->isSigned;
                result.offset = logicWords;
                logicWords += wordCount(result.width);
                return result;
            }
            if (type->kind == static_cast<uint8_t>(GenericTypeKind::Real))
            {
                result.valueClass = ValueClass::Real;
                result.offset = reals++;
                return result;
            }
            if (type->kind == static_cast<uint8_t>(GenericTypeKind::String))
            {
                result.valueClass = ValueClass::String;
                result.offset = strings++;
                return result;
            }
            diagnostics.error("unsupported generic Type in CPU emitter", std::string(context));
            return {};
        }

        std::string publicLogicType(uint32_t width)
        {
            if (width == 1) return "bool";
            if (width <= 8) return "std::uint8_t";
            if (width <= 16) return "std::uint16_t";
            if (width <= 32) return "std::uint32_t";
            if (width <= 64) return "std::uint64_t";
            return "std::array<std::uint64_t, " + std::to_string(wordCount(width)) + ">";
        }

        std::string lowerText(std::string_view text)
        {
            std::string result;
            result.reserve(text.size());
            for (unsigned char ch : text)
            {
                result.push_back(static_cast<char>(std::tolower(ch)));
            }
            return result;
        }

        std::string dpiBaseType(const Module &module, TypeId typeId, std::string_view typeName)
        {
            const TypeRec *type = module.type(typeId);
            const std::string lowered = lowerText(typeName);
            if (lowered == "string") return "std::string";
            if (lowered == "shortreal") return "float";
            if (lowered == "real") return "double";
            const uint32_t width = type ? type->width : 1U;
            const bool isSigned = type && type->isSigned;
            if (width == 1) return "bool";
            if (width <= 8) return isSigned ? "std::int8_t" : "std::uint8_t";
            if (width <= 16) return isSigned ? "std::int16_t" : "std::uint16_t";
            if (width <= 32) return isSigned ? "std::int32_t" : "std::uint32_t";
            if (width <= 64) return isSigned ? "std::int64_t" : "std::uint64_t";
            return publicLogicType(width);
        }

        bool hostIsDpi(const Module &module, HostId host)
        {
            return findAttr(module, module.hostAttrs(host), "argsDirection") != nullptr;
        }

        bool initializeState(const Module &module, EmitModel &model,
                             wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            model.initialStateWords.assign(model.stateWords, 0);
            for (const StateLayout &layout : model.states)
            {
                if (layout.kind != StateKind::State || layout.valueClass != ValueClass::Logic)
                {
                    continue;
                }
                const auto attrs = module.stateInitAttrs(layout.id);
                if (layout.rows == 0)
                {
                    const SymbolId *literalId = attrAs<SymbolId>(module, attrs, "initValue");
                    if (!literalId) continue;
                    const std::string_view literal = module.symbol(*literalId);
                    if (literal == "$random")
                    {
                        model.randomInitializers.push_back(
                            RandomInit{layout.offset, layout.width, 0, layout.width});
                        continue;
                    }
                    const auto words = parseLogicLiteral(literal, layout.width);
                    if (!words)
                    {
                        diagnostics.error("state initValue is not a valid logic literal", layout.name);
                        return false;
                    }
                    std::copy(words->begin(), words->end(),
                              model.initialStateWords.begin() + layout.offset);
                    continue;
                }

                const auto kinds = symbolArrayAttr(module, attrs, "initKind");
                const auto files = symbolArrayAttr(module, attrs, "initFile");
                const auto values = symbolArrayAttr(module, attrs, "initValue");
                const auto *starts = attrAs<std::vector<int64_t>>(module, attrs, "initStart");
                const auto *lengths = attrAs<std::vector<int64_t>>(module, attrs, "initLen");
                if (kinds.empty() && files.empty() && values.empty() && !starts && !lengths)
                {
                    continue;
                }
                if (!starts || !lengths || kinds.size() != files.size() ||
                    kinds.size() != starts->size() || kinds.size() != lengths->size())
                {
                    diagnostics.error("memory initialization attributes are incomplete", layout.name);
                    return false;
                }
                for (std::size_t init = 0; init < kinds.size(); ++init)
                {
                    const int64_t rawStart = (*starts)[init];
                    const int64_t rawLength = (*lengths)[init];
                    const uint64_t begin = rawStart < 0 ? 0U : static_cast<uint64_t>(rawStart);
                    const uint64_t end = rawStart < 0
                                             ? layout.rows
                                             : std::min<uint64_t>(layout.rows,
                                                  rawLength <= 0 ? layout.rows
                                                                 : begin + static_cast<uint64_t>(rawLength));
                    if (begin >= end) continue;
                    if (kinds[init] == "literal")
                    {
                        const std::string literal = init < values.size() && !values[init].empty()
                                                        ? values[init]
                                                        : "0";
                        if (literal == "$random")
                        {
                            for (uint64_t row = begin; row < end; ++row)
                            {
                                model.randomInitializers.push_back(RandomInit{
                                    layout.offset,
                                    layout.width,
                                    row * layout.elementWidth,
                                    layout.elementWidth,
                                });
                            }
                            continue;
                        }
                        const auto words = parseLogicLiteral(literal, layout.elementWidth);
                        if (!words)
                        {
                            diagnostics.error("memory literal initialization is invalid", layout.name);
                            return false;
                        }
                        for (uint64_t row = begin; row < end; ++row)
                        {
                            writePackedValue(model.initialStateWords, layout.offset,
                                             row * layout.elementWidth,
                                             layout.elementWidth, *words);
                        }
                        continue;
                    }
                    if (kinds[init] != "readmemh" && kinds[init] != "readmemb")
                    {
                        diagnostics.error("unsupported memory initKind: " + kinds[init], layout.name);
                        return false;
                    }
                    if (files[init].empty())
                    {
                        diagnostics.error("memory initialization file is empty", layout.name);
                        return false;
                    }
                    std::ifstream input(files[init], std::ios::binary);
                    if (!input.is_open())
                    {
                        diagnostics.error("failed to open memory initialization file", files[init]);
                        return false;
                    }
                    const std::string text{std::istreambuf_iterator<char>(input),
                                           std::istreambuf_iterator<char>()};
                    uint64_t row = begin;
                    for (const std::string &token : tokenizeReadmem(text))
                    {
                        if (!token.empty() && token.front() == '@')
                        {
                            const auto address = parseAddress(std::string_view(token).substr(1));
                            if (!address)
                            {
                                diagnostics.error("invalid readmem address token", files[init]);
                                return false;
                            }
                            row = *address;
                            continue;
                        }
                        if (row >= end) break;
                        const std::string literal = std::to_string(layout.elementWidth) +
                            (kinds[init] == "readmemh" ? "'h" : "'b") + token;
                        const auto words = parseLogicLiteral(literal, layout.elementWidth);
                        if (!words)
                        {
                            diagnostics.error("invalid readmem data token", files[init]);
                            return false;
                        }
                        writePackedValue(model.initialStateWords, layout.offset,
                                         row * layout.elementWidth,
                                         layout.elementWidth, *words);
                        ++row;
                    }
                }
            }
            return true;
        }

        std::optional<EmitModel> buildModel(const Module &module,
                                            wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            EmitModel model;
            model.stem = sanitizeIdentifier(module.name());
            model.className = "GrhSIM_" + model.stem;
            model.schedule = module.linearize();

            if (module.name().empty())
            {
                diagnostics.error("emit-cpu-single-thread requires a non-empty module name");
                return std::nullopt;
            }
            if (!module.hasSchedule() || model.schedule.size() != module.opCount())
            {
                diagnostics.error("emit-cpu-single-thread requires a complete Schedule",
                                  module.name());
                return std::nullopt;
            }
            for (RegionId region : module.regions())
            {
                const RegionRec *record = module.region(region);
                if (!record || record->activation.kind != ActivationKind::Always)
                {
                    diagnostics.error("the minimal CPU emitter only accepts always-active regions",
                                      "region=" + std::to_string(region.raw));
                    return std::nullopt;
                }
            }

            const std::size_t edgeSlots = module.edges().empty()
                                              ? 0
                                              : static_cast<std::size_t>(module.edges().back().raw) + 1U;
            model.edges.resize(edgeSlots);
            for (EdgeId edge : module.edges())
            {
                model.edges[edge.raw] = layoutForType(
                    module, module.edgeType(edge), model.edgeWords, model.edgeReals,
                    model.edgeStrings, diagnostics, "edge=" + std::to_string(edge.raw), false);
            }

            model.states.reserve(module.states().size());
            for (uint32_t raw = 0; raw < module.states().size(); ++raw)
            {
                const StateId state{raw};
                const StateEntry *entry = module.state(state);
                StateLayout layout;
                static_cast<ValueLayout &>(layout) = layoutForType(
                    module, entry->genType, model.stateWords, model.stateReals,
                    model.stateStrings, diagnostics, "state=" + std::to_string(raw), true);
                layout.id = state;
                layout.kind = entry->kind;
                layout.name = std::string(module.symbol(entry->name));
                const TypeRec *type = module.type(entry->genType);
                if (type && type->kind == static_cast<uint8_t>(GenericTypeKind::Array))
                {
                    const TypeRec *element = module.type(type->elementType);
                    layout.rows = type->rows;
                    layout.elementWidth = element ? element->width : 0;
                }
                model.states.push_back(std::move(layout));
            }
            if (diagnostics.hasError()) return std::nullopt;

            // Synthetic event values are lowered as output sinks marked with
            // eventState=true. Record their pulse slots before ports are
            // assembled so they can never escape as public outputs.
            model.eventSlotByState.assign(model.states.size(), -1);
            for (OpId op : model.schedule)
            {
                if (module.kind(op) != genericOp(GenericOpcode::OutWrite))
                {
                    continue;
                }
                const AttrValue *marker = module.attr(op, "eventState");
                const bool marked = marker && std::get_if<bool>(marker) &&
                                    *std::get_if<bool>(marker);
                if (!marked)
                {
                    continue;
                }
                const StateId target = module.findState(symbolAttr(module, op, "port"));
                const auto operands = module.operands(op);
                const StateLayout *layout = target.valid() && target.raw < model.states.size()
                                                 ? &model.states[target.raw]
                                                 : nullptr;
                if (!layout || layout->kind != StateKind::Output || operands.size() != 1 ||
                    layout->valueClass != ValueClass::Logic || layout->width != 1 ||
                    module.edgeType(operands[0]) != module.state(target)->genType)
                {
                    diagnostics.error("eventState out_write is malformed",
                                      "op=" + std::to_string(op.raw));
                    continue;
                }
                if (model.eventSlotByState[target.raw] >= 0)
                {
                    diagnostics.error("eventState output has multiple writers",
                                      layout->name);
                    continue;
                }
                model.eventSlotByState[target.raw] =
                    static_cast<int64_t>(model.eventStates.size());
                model.eventStates.push_back(target);
            }
            if (diagnostics.hasError()) return std::nullopt;

            model.trackSet = module.deriveTrackSet();
            model.trackIndexByState.assign(model.states.size(), -1);
            for (std::size_t index = 0; index < model.trackSet.size(); ++index)
            {
                model.trackIndexByState[model.trackSet[index].raw] = static_cast<int64_t>(index);
            }

            const std::size_t opSlots = module.ops().empty()
                                            ? 0
                                            : static_cast<std::size_t>(module.ops().back().raw) + 1U;
            model.pendingWriteByOp.assign(opSlots, std::numeric_limits<std::size_t>::max());
            for (std::size_t scheduleIndex = 0; scheduleIndex < model.schedule.size(); ++scheduleIndex)
            {
                const OpId op = model.schedule[scheduleIndex];
                const OpKind kind = module.kind(op);
                if (kind.dialect() != kGenericDialect)
                {
                    diagnostics.error("emit-cpu-single-thread only supports the generic dialect",
                                      "op=" + std::to_string(op.raw));
                    continue;
                }
                const auto opcode = static_cast<GenericOpcode>(kind.opcode());
                if (opcode != GenericOpcode::MemWrite &&
                    opcode != GenericOpcode::MemWriteLanes && opcode != GenericOpcode::MemFill)
                {
                    continue;
                }
                const StateId state = module.findState(symbolAttr(module, op, "state"));
                const StateLayout &stateLayout = model.states[state.raw];
                PendingWriteLayout write;
                write.op = op;
                write.opcode = opcode;
                write.state = state;
                write.scheduleIndex = scheduleIndex;
                write.priority = opAttr<int64_t>(module, op, "memoryWrite.priority")
                                     ? *opAttr<int64_t>(module, op, "memoryWrite.priority")
                                     : 0;
                write.dataOffset = model.pendingWords;
                model.pendingWords += wordCount(opcode == GenericOpcode::MemWrite
                                                    ? stateLayout.elementWidth
                                                    : stateLayout.width);
                write.maskOffset = model.pendingWords;
                if (opcode == GenericOpcode::MemWrite)
                    model.pendingWords += wordCount(stateLayout.elementWidth);
                else if (opcode == GenericOpcode::MemWriteLanes)
                    model.pendingWords += wordCount(stateLayout.rows);
                const std::size_t index = model.pendingWrites.size();
                model.pendingWriteByOp[op.raw] = index;
                model.pendingWrites.push_back(write);
                model.memoryWriteOrder.push_back(index);
            }
            std::stable_sort(model.memoryWriteOrder.begin(), model.memoryWriteOrder.end(),
                             [&](std::size_t lhs, std::size_t rhs) {
                                 const PendingWriteLayout &left = model.pendingWrites[lhs];
                                 const PendingWriteLayout &right = model.pendingWrites[rhs];
                                 if (left.state != right.state) return left.state < right.state;
                                 if (left.priority != right.priority) return left.priority > right.priority;
                                 return left.scheduleIndex < right.scheduleIndex;
                             });

            std::unordered_map<std::string, StateId> stateByName;
            for (const StateLayout &state : model.states) stateByName.emplace(state.name, state.id);
            std::unordered_set<uint32_t> consumed;
            std::set<std::string> usedIdentifiers;
            auto uniqueIdentifier = [&](std::string_view name) {
                const std::string base = sanitizeIdentifier(name);
                std::string result = base;
                for (uint32_t ordinal = 1; usedIdentifiers.contains(result); ++ordinal)
                    result = base + "_" + std::to_string(ordinal);
                usedIdentifiers.insert(result);
                return result;
            };
            for (const StateLayout &state : model.states)
            {
                if (state.kind != StateKind::Input || consumed.contains(state.id.raw)) continue;
                const std::string base = stripSuffix(state.name, "$in");
                const auto out = stateByName.find(base + "$out");
                const auto oe = stateByName.find(base + "$oe");
                PortLayout port;
                port.name = base;
                port.identifier = uniqueIdentifier(base);
                port.input = state.id;
                if (out != stateByName.end() && oe != stateByName.end())
                {
                    port.output = out->second;
                    port.outputEnable = oe->second;
                    consumed.insert(out->second.raw);
                    consumed.insert(oe->second.raw);
                }
                consumed.insert(state.id.raw);
                model.ports.push_back(std::move(port));
            }
            for (const StateLayout &state : model.states)
            {
                if (state.kind != StateKind::Output || consumed.contains(state.id.raw) ||
                    model.eventSlotByState[state.id.raw] >= 0)
                    continue;
                const std::string base = stripSuffix(state.name, "$out");
                model.ports.push_back(PortLayout{
                    .name = base,
                    .identifier = uniqueIdentifier(base),
                    .output = state.id,
                });
                consumed.insert(state.id.raw);
            }

            std::unordered_set<uint32_t> referencedHosts;
            for (OpId op : model.schedule)
            {
                if (module.kind(op) == genericOp(GenericOpcode::HostCall))
                {
                    const HostId host = module.findHost(symbolAttr(module, op, "entry"));
                    if (host.valid()) referencedHosts.insert(host.raw);
                }
            }
            model.dpiByHost.assign(module.hosts().size(), -1);
            for (uint32_t raw = 0; raw < module.hosts().size(); ++raw)
            {
                const HostId hostId{raw};
                if (!referencedHosts.contains(raw) || !hostIsDpi(module, hostId)) continue;
                const HostEntry *host = module.host(hostId);
                const auto attrs = module.hostAttrs(hostId);
                const auto typeNames = symbolArrayAttr(module, attrs, "argsType");
                DpiLayout dpi;
                dpi.host = hostId;
                dpi.binding = std::string(module.symbol(host->binding));
                dpi.field = "host_" + std::to_string(raw);
                std::size_t argument = 0;
                for (const HostParam &parameter : module.hostSignature(hostId))
                {
                    const std::string typeName = parameter.direction == HostParamDirection::Return
                                                     ? (attrAs<SymbolId>(module, attrs, "returnType")
                                                            ? std::string(module.symbol(*attrAs<SymbolId>(
                                                                  module, attrs, "returnType")))
                                                            : "logic")
                                                     : (argument < typeNames.size() ? typeNames[argument]
                                                                                  : "logic");
                    const std::string base = dpiBaseType(module, parameter.type, typeName);
                    if (parameter.direction == HostParamDirection::Return)
                    {
                        dpi.returnType = base;
                        continue;
                    }
                    std::string declaration = base;
                    if (parameter.direction == HostParamDirection::Output ||
                        parameter.direction == HostParamDirection::InOut)
                        declaration += " *";
                    else if (lowerText(typeName) == "string")
                        declaration = "const char *";
                    else
                    {
                        const TypeRec *type = module.type(parameter.type);
                        if (type && type->kind == static_cast<uint8_t>(GenericTypeKind::Logic) &&
                            type->width > 64)
                            declaration = "const " + base + " &";
                    }
                    dpi.parameters.push_back(DpiParameter{
                        .parameter = parameter,
                        .typeName = typeName,
                        .baseType = base,
                        .declarationType = declaration,
                    });
                    ++argument;
                }
                model.dpiByHost[raw] = static_cast<int64_t>(model.dpi.size());
                model.dpi.push_back(std::move(dpi));
            }

            if (!initializeState(module, model, diagnostics) || diagnostics.hasError())
                return std::nullopt;
            return model;
        }

        bool writeFile(const fs::path &path, std::string_view contents,
                       wolvrix::lib::diag::Diagnostics &diagnostics)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                diagnostics.error("failed to open generated artifact", path.string());
                return false;
            }
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!output)
            {
                diagnostics.error("failed to write generated artifact", path.string());
                return false;
            }
            return true;
        }

        class CpuEmitter
        {
        public:
            CpuEmitter(const Module &module, const EmitModel &model,
                       const CpuSingleThreadEmitOptions &options,
                       wolvrix::lib::diag::Diagnostics &diagnostics)
                : module_(module), model_(model), options_(options), diagnostics_(diagnostics)
            {
            }

            CpuSingleThreadEmitResult run()
            {
                CpuSingleThreadEmitResult result;
                std::error_code error;
                fs::create_directories(options_.outputDirectory, error);
                if (error)
                {
                    diagnostics_.error("failed to create CPU emitter output directory: " +
                                           error.message(),
                                       options_.outputDirectory.string());
                    return result;
                }
                const std::string prefix = "grhsim_" + model_.stem;
                const fs::path runtimePath = options_.outputDirectory / (prefix + "_runtime.hpp");
                const fs::path supportPath = options_.outputDirectory / (prefix + "_support.hpp");
                const fs::path headerPath = options_.outputDirectory / (prefix + ".hpp");
                const fs::path corePath = options_.outputDirectory / (prefix + ".cpp");
                const fs::path makefilePath = options_.outputDirectory / "Makefile";

                if (!writeFile(runtimePath, kRuntimeHeader, diagnostics_) ||
                    !writeFile(supportPath, emitSupport(), diagnostics_) ||
                    !writeFile(headerPath, emitHeader(), diagnostics_) ||
                    !writeFile(corePath, emitCore(), diagnostics_))
                    return result;

                result.artifacts = {runtimePath.string(), supportPath.string(), headerPath.string(),
                                    corePath.string()};
                const std::size_t chunkSize = std::max<std::size_t>(1, options_.opsPerSourceFile);
                const std::size_t chunks = (model_.schedule.size() + chunkSize - 1U) / chunkSize;
                for (std::size_t chunk = 0; chunk < chunks; ++chunk)
                {
                    std::ostringstream name;
                    name << prefix << "_ops_" << std::setw(4) << std::setfill('0') << chunk
                         << ".cpp";
                    const fs::path path = options_.outputDirectory / name.str();
                    if (!writeFile(path, emitChunk(chunk, chunkSize), diagnostics_)) return result;
                    result.artifacts.push_back(path.string());
                }
                if (!writeFile(makefilePath, emitMakefile(chunks), diagnostics_)) return result;
                result.artifacts.push_back(makefilePath.string());
                result.success = !diagnostics_.hasError();
                return result;
            }

        private:
            std::string bits(const ValueLayout &layout, std::string_view arena) const
            {
                return "rt::Bits{" + std::string(arena) + ".data() + " +
                       std::to_string(layout.offset) + ", " + std::to_string(layout.width) + "U}";
            }

            std::string constBits(const ValueLayout &layout, std::string_view arena) const
            {
                return "rt::ConstBits{" + std::string(arena) + ".data() + " +
                       std::to_string(layout.offset) + ", " + std::to_string(layout.width) + "U}";
            }

            const ValueLayout &edge(EdgeId id) const { return model_.edges[id.raw]; }
            const StateLayout &state(StateId id) const { return model_.states[id.raw]; }

            StateId targetState(OpId op, std::string_view attr) const
            {
                return module_.findState(symbolAttr(module_, op, attr));
            }

            bool edgeSigned(EdgeId id) const { return edge(id).isSigned; }

            std::string eventExpression(OpId op) const
            {
                const auto *events = opAttr<std::vector<SymbolId>>(module_, op, "events");
                const auto *edges = opAttr<std::vector<SymbolId>>(module_, op, "eventEdge");
                if (!events || events->empty()) return "true";
                std::ostringstream expression;
                for (std::size_t index = 0; index < events->size(); ++index)
                {
                    const StateId stateId = module_.findState(module_.symbol((*events)[index]));
                    const StateLayout &layout = state(stateId);
                    const bool posedge = module_.symbol((*edges)[index]) == "posedge";
                    if (index != 0) expression << " || ";
                    const int64_t eventSlot = stateId.valid() &&
                                                      stateId.raw < model_.eventSlotByState.size()
                                                  ? model_.eventSlotByState[stateId.raw]
                                                  : -1;
                    if (eventSlot >= 0)
                    {
                        expression << "(event_pulse_"
                                   << (posedge ? "posedge" : "negedge") << "_[" << eventSlot
                                   << "] != 0)";
                    }
                    else
                    {
                        const int64_t track = stateId.valid() &&
                                                      stateId.raw < model_.trackIndexByState.size()
                                                  ? model_.trackIndexByState[stateId.raw]
                                                  : -1;
                        expression << "((edge_track_[" << track << "] == "
                                   << (posedge ? "0" : "1")
                                   << ") && ((state_words_[" << layout.offset
                                   << "] & UINT64_C(1)) == "
                                   << (posedge ? "UINT64_C(1)" : "UINT64_C(0)" ) << "))";
                    }
                }
                return "(" + expression.str() + ")";
            }

            std::string taskArg(EdgeId id) const
            {
                const ValueLayout &layout = edge(id);
                if (layout.valueClass == ValueClass::Logic)
                    return "rt::TaskArg::from_logic(" + constBits(layout, "edge_words_") + ", " +
                           (layout.isSigned ? "true" : "false") + ")";
                if (layout.valueClass == ValueClass::Real)
                    return "rt::TaskArg::from_real(edge_reals_[" + std::to_string(layout.offset) + "])";
                return "rt::TaskArg::from_string(edge_strings_[" + std::to_string(layout.offset) + "])";
            }

            void assignCppToEdge(std::ostream &out, const ValueLayout &layout,
                                 std::string_view value, std::string_view indent) const
            {
                if (layout.valueClass == ValueClass::Logic)
                {
                    if (layout.width <= 64)
                    {
                        out << indent << "rt::clear(" << bits(layout, "edge_words_") << ");\n";
                        out << indent << "edge_words_[" << layout.offset
                            << "] = static_cast<std::uint64_t>(" << value << ");\n";
                        out << indent << "rt::truncate(" << bits(layout, "edge_words_") << ");\n";
                    }
                    else
                    {
                        out << indent << "rt::import_port(" << bits(layout, "edge_words_") << ", "
                            << value << ");\n";
                    }
                }
                else if (layout.valueClass == ValueClass::Real)
                    out << indent << "edge_reals_[" << layout.offset << "] = " << value << ";\n";
                else
                    out << indent << "edge_strings_[" << layout.offset << "] = " << value << ";\n";
            }

            std::string emitSupport() const
            {
                std::ostringstream out;
                out << "#pragma once\n\n#include <array>\n#include <cstdint>\n#include <string>\n\n";
                std::set<std::string> declarations;
                for (const DpiLayout &dpi : model_.dpi)
                {
                    std::ostringstream declaration;
                    declaration << "extern \"C\" " << dpi.returnType << ' ' << dpi.binding << '(';
                    for (std::size_t index = 0; index < dpi.parameters.size(); ++index)
                    {
                        if (index != 0) declaration << ", ";
                        declaration << dpi.parameters[index].declarationType;
                    }
                    declaration << ");";
                    declarations.insert(declaration.str());
                }
                for (const std::string &declaration : declarations) out << declaration << '\n';
                return out.str();
            }

            std::string emitHeader() const
            {
                std::ostringstream out;
                const std::string prefix = "grhsim_" + model_.stem;
                const std::size_t chunkSize = std::max<std::size_t>(1, options_.opsPerSourceFile);
                const std::size_t chunks = (model_.schedule.size() + chunkSize - 1U) / chunkSize;
                out << "#pragma once\n\n#include <array>\n#include <cstddef>\n#include <cstdint>\n"
                       "#include <initializer_list>\n#include <string>\n#include <string_view>\n#include <vector>\n\n";
                out << "#include \"" << prefix << "_runtime.hpp\"\n\n";
                out << "#ifndef WOLVRIX_GRHSIM_PERF\n#define WOLVRIX_GRHSIM_PERF 0\n#endif\n\n";
                out << "class " << model_.className << " {\npublic:\n";
                out << "    struct HostTable {\n";
                for (const DpiLayout &dpi : model_.dpi)
                {
                    out << "        using " << dpi.field << "_type = " << dpi.returnType
                        << " (*)(";
                    for (std::size_t index = 0; index < dpi.parameters.size(); ++index)
                    {
                        if (index != 0) out << ", ";
                        out << dpi.parameters[index].declarationType;
                    }
                    out << ");\n";
                    out << "        " << dpi.field << "_type " << dpi.field
                        << " = nullptr;\n";
                }
                out << "    };\n\n";
                out << "    " << model_.className << "();\n";
                out << "    ~" << model_.className << "() = default;\n";
                out << "    void init();\n    void eval();\n    void finalize();\n";
                out << "    void set_random_seed(std::uint64_t seed);\n";
                out << "    void set_time(std::uint64_t value);\n";
                out << "    [[nodiscard]] std::uint64_t time() const;\n";
                out << "    HostTable &host_table();\n    const HostTable &host_table() const;\n";
                out << "    [[nodiscard]] bool finish_requested() const;\n";
                out << "    [[nodiscard]] bool stop_requested() const;\n";
                out << "    [[nodiscard]] bool fatal_requested() const;\n";
                out << "    [[nodiscard]] int system_exit_code() const;\n";
                out << "    void set_runtime_profile_enabled(bool enabled);\n";
                out << "    [[nodiscard]] bool runtime_profile_enabled() const;\n";
                out << "    void dump_runtime_profile() const;\n";
                out << "    static constexpr bool kRuntimeProfileCompiled = false;\n\n";
                for (const PortLayout &port : model_.ports)
                {
                    const StateLayout &layout = state(port.input.valid() ? port.input : port.output);
                    const std::string type = publicLogicType(layout.width);
                    if (port.inout())
                    {
                        out << "    struct Inout_" << port.identifier << " {\n"
                            << "        " << type << " in = {};\n"
                            << "        " << type << " out = {};\n"
                            << "        bool oe = false;\n"
                            << "    } " << port.identifier << ";\n";
                    }
                    else
                        out << "    " << type << ' ' << port.identifier << " = {};\n";
                }
                out << "\nprivate:\n";
                for (std::size_t chunk = 0; chunk < chunks; ++chunk)
                    out << "    void eval_ops_" << chunk << "();\n";
                out << "    void apply_pending_memory_writes();\n";
                out << "    void execute_system_task(std::string_view name, "
                       "std::initializer_list<wolvrix_grhsim_cpu_runtime::TaskArg> args);\n\n";
                out << "    HostTable host_table_{};\n";
                out << "    std::vector<std::uint64_t> edge_words_;\n"
                       "    std::vector<double> edge_reals_;\n"
                       "    std::vector<std::string> edge_strings_;\n"
                       "    std::vector<std::uint64_t> state_words_;\n"
                       "    std::vector<std::uint64_t> next_state_words_;\n"
                       "    std::vector<double> state_reals_;\n"
                       "    std::vector<double> next_state_reals_;\n"
                       "    std::vector<std::string> state_strings_;\n"
                       "    std::vector<std::string> next_state_strings_;\n"
                       "    std::vector<std::uint8_t> edge_track_;\n"
                       "    std::vector<std::uint8_t> event_pulse_posedge_;\n"
                       "    std::vector<std::uint8_t> event_pulse_negedge_;\n"
                       "    std::vector<std::uint8_t> pending_write_enabled_;\n"
                       "    std::vector<std::uint64_t> pending_write_address_;\n"
                       "    std::vector<std::uint64_t> pending_write_words_;\n";
                out << "    std::uint64_t random_seed_ = 0;\n"
                       "    std::uint64_t random_state_ = 0;\n"
                       "    std::uint64_t simulation_time_ = 0;\n"
                       "    bool finish_requested_ = false;\n"
                       "    bool stop_requested_ = false;\n"
                       "    bool fatal_requested_ = false;\n"
                       "    int system_exit_code_ = 0;\n"
                       "    bool finalized_ = false;\n"
                       "};\n";
                return out.str();
            }

            void emitPortImport(std::ostream &out, const PortLayout &port) const
            {
                if (!port.input.valid()) return;
                const StateLayout &layout = state(port.input);
                out << "    rt::import_port(" << bits(layout, "state_words_") << ", "
                    << port.identifier << (port.inout() ? ".in" : "") << ");\n";
            }

            void emitPortExport(std::ostream &out, const PortLayout &port) const
            {
                if (!port.output.valid()) return;
                const StateLayout &layout = state(port.output);
                if (port.inout())
                {
                    out << "    rt::export_port(" << port.identifier << ".out, "
                        << constBits(layout, "state_words_") << ");\n";
                    const StateLayout &oe = state(port.outputEnable);
                    out << "    rt::export_port(" << port.identifier << ".oe, "
                        << constBits(oe, "state_words_") << ");\n";
                }
                else
                    out << "    rt::export_port(" << port.identifier << ", "
                        << constBits(layout, "state_words_") << ");\n";
            }

            std::string emitCore() const
            {
                using namespace std::string_view_literals;
                std::ostringstream out;
                const std::string prefix = "grhsim_" + model_.stem;
                const std::size_t chunkSize = std::max<std::size_t>(1, options_.opsPerSourceFile);
                const std::size_t chunks = (model_.schedule.size() + chunkSize - 1U) / chunkSize;
                out << "#include \"" << prefix << ".hpp\"\n"
                    << "#include \"" << prefix << "_support.hpp\"\n\n"
                       "#include <algorithm>\n#include <iostream>\n#include <stdexcept>\n\n"
                       "namespace rt = wolvrix_grhsim_cpu_runtime;\n\n";
                out << model_.className << "::" << model_.className << "()\n"
                    << "    : edge_words_(" << model_.edgeWords << ", 0),\n"
                    << "      edge_reals_(" << model_.edgeReals << ", 0.0),\n"
                    << "      edge_strings_(" << model_.edgeStrings << "),\n"
                    << "      state_words_(" << model_.stateWords << ", 0),\n"
                    << "      next_state_words_(" << model_.stateWords << ", 0),\n"
                    << "      state_reals_(" << model_.stateReals << ", 0.0),\n"
                    << "      next_state_reals_(" << model_.stateReals << ", 0.0),\n"
                    << "      state_strings_(" << model_.stateStrings << "),\n"
                    << "      next_state_strings_(" << model_.stateStrings << "),\n"
                    << "      edge_track_(" << model_.trackSet.size() << ", 0),\n"
                    << "      event_pulse_posedge_(" << model_.eventStates.size() << ", 0),\n"
                    << "      event_pulse_negedge_(" << model_.eventStates.size() << ", 0),\n"
                    << "      pending_write_enabled_(" << model_.pendingWrites.size() << ", 0),\n"
                    << "      pending_write_address_(" << model_.pendingWrites.size() << ", 0),\n"
                    << "      pending_write_words_(" << model_.pendingWords << ", 0)\n{\n";
                for (const DpiLayout &dpi : model_.dpi)
                {
                    out << "    host_table_." << dpi.field << " = &::" << dpi.binding << ";\n";
                }
                out << "}\n\n";
                out << "void " << model_.className << "::init()\n{\n"
                       "    std::fill(edge_words_.begin(), edge_words_.end(), UINT64_C(0));\n"
                       "    std::fill(edge_reals_.begin(), edge_reals_.end(), 0.0);\n"
                       "    std::fill(edge_strings_.begin(), edge_strings_.end(), std::string{});\n"
                       "    std::fill(state_words_.begin(), state_words_.end(), UINT64_C(0));\n"
                       "    std::fill(state_reals_.begin(), state_reals_.end(), 0.0);\n"
                       "    std::fill(state_strings_.begin(), state_strings_.end(), std::string{});\n"
                       "    std::fill(event_pulse_posedge_.begin(), event_pulse_posedge_.end(), 0);\n"
                       "    std::fill(event_pulse_negedge_.begin(), event_pulse_negedge_.end(), 0);\n"
                       "    random_state_ = random_seed_;\n"
                       "    finish_requested_ = false;\n    stop_requested_ = false;\n"
                       "    fatal_requested_ = false;\n    system_exit_code_ = 0;\n"
                       "    finalized_ = false;\n";
                for (std::size_t index = 0; index < model_.initialStateWords.size(); ++index)
                    if (model_.initialStateWords[index] != 0)
                        out << "    state_words_[" << index << "] = UINT64_C("
                            << model_.initialStateWords[index] << ");\n";
                for (const RandomInit &random : model_.randomInitializers)
                {
                    out << "    {\n        rt::Bits target{state_words_.data() + " << random.stateOffset
                        << ", " << random.stateWidth << "U};\n"
                           "        std::vector<std::uint64_t> random_words(rt::word_count("
                        << random.width << "U), UINT64_C(0));\n"
                           "        rt::Bits random_value{random_words.data(), "
                        << random.width << "U};\n"
                           "        for (std::size_t word = 0; word < random_words.size(); ++word)\n"
                           "            random_words[word] = rt::splitmix64(random_state_);\n"
                           "        rt::truncate(random_value);\n"
                           "        rt::copy_range(target, "
                        << random.bitOffset
                        << "U, rt::as_const(random_value), 0, random_value.width);\n    }\n";
                }
                for (const PortLayout &port : model_.ports)
                {
                    if (port.inout()) out << "    " << port.identifier << " = {};\n";
                    else out << "    " << port.identifier << " = {};\n";
                }
                for (std::size_t index = 0; index < model_.trackSet.size(); ++index)
                {
                    const StateLayout &layout = state(model_.trackSet[index]);
                    out << "    edge_track_[" << index << "] = static_cast<std::uint8_t>(state_words_["
                        << layout.offset << "] & UINT64_C(1));\n";
                }
                out << "    next_state_words_ = state_words_;\n"
                       "    next_state_reals_ = state_reals_;\n"
                       "    next_state_strings_ = state_strings_;\n"
                       "}\n\n";
                out << "void " << model_.className << "::set_random_seed(std::uint64_t seed) "
                       "{ random_seed_ = seed; }\n";
                out << "void " << model_.className << "::set_time(std::uint64_t value) "
                       "{ simulation_time_ = value; }\n";
                out << "std::uint64_t " << model_.className << "::time() const "
                       "{ return simulation_time_; }\n";
                out << model_.className << "::HostTable &" << model_.className
                    << "::host_table() { return host_table_; }\n";
                out << "const " << model_.className << "::HostTable &" << model_.className
                    << "::host_table() const { return host_table_; }\n";
                out << "bool " << model_.className << "::finish_requested() const { return finish_requested_; }\n"
                       "bool " << model_.className << "::stop_requested() const { return stop_requested_; }\n"
                       "bool " << model_.className << "::fatal_requested() const { return fatal_requested_; }\n"
                       "int " << model_.className << "::system_exit_code() const { return system_exit_code_; }\n"
                       "void " << model_.className << "::set_runtime_profile_enabled(bool) {}\n"
                       "bool " << model_.className << "::runtime_profile_enabled() const { return false; }\n"
                       "void " << model_.className << "::dump_runtime_profile() const {}\n"
                       "void " << model_.className << "::finalize() { finalized_ = true; }\n\n";

                out << "void " << model_.className
                    << "::execute_system_task(std::string_view name, std::initializer_list<rt::TaskArg> values)\n{\n"
                       "    const std::span<const rt::TaskArg> args(values.begin(), values.size());\n"
                       "    if (name == \"finish\" || name == \"stop\") {\n"
                       "        if (name == \"finish\") finish_requested_ = true; else stop_requested_ = true;\n"
                       "        if (!args.empty() && args.front().kind == rt::TaskArgKind::Logic)\n"
                       "            system_exit_code_ = static_cast<int>(rt::to_u64(args.front().logic));\n"
                       "        return;\n    }\n"
                       "    if (name == \"fatal\") {\n"
                       "        fatal_requested_ = true; finish_requested_ = true; system_exit_code_ = 1;\n"
                       "        std::size_t first = 0;\n"
                       "        if (!args.empty() && args.front().kind == rt::TaskArgKind::Logic) {\n"
                       "            system_exit_code_ = static_cast<int>(rt::to_u64(args.front().logic)); first = 1;\n"
                       "        }\n"
                       "        std::cerr << \"[fatal] \" << rt::format_task_message(args.subspan(first)) << '\\n';\n"
                       "        return;\n    }\n"
                       "    if (name == \"fwrite\") {\n"
                       "        if (args.empty()) return;\n"
                       "        const std::uint64_t handle = args.front().kind == rt::TaskArgKind::Logic\n"
                       "            ? rt::to_u64(args.front().logic) : UINT64_C(0);\n"
                       "        std::ostream &stream = (handle == UINT64_C(2) || handle == UINT64_C(0x80000002))\n"
                       "            ? std::cerr : std::cout;\n"
                       "        stream << rt::format_task_message(args.subspan(1));\n        return;\n    }\n"
                       "    const bool newline = name != \"write\";\n"
                       "    std::ostream &stream = (name == \"error\" || name == \"warning\")\n"
                       "        ? std::cerr : std::cout;\n"
                       "    stream << rt::format_task_message(args);\n"
                       "    if (newline) stream << '\\n';\n"
                       "}\n\n";

                out << "void " << model_.className << "::apply_pending_memory_writes()\n{\n";
                if (!model_.pendingWrites.empty())
                {
                    out << "    static constexpr std::array<rt::PendingWriteDesc, "
                        << model_.pendingWrites.size() << "> descriptions{{\n";
                    for (const PendingWriteLayout &write : model_.pendingWrites)
                    {
                        const StateLayout &layout = state(write.state);
                        const char *kind = write.opcode == GenericOpcode::MemWrite
                                               ? "rt::PendingWriteKind::Single"
                                               : write.opcode == GenericOpcode::MemWriteLanes
                                                     ? "rt::PendingWriteKind::Lanes"
                                                     : "rt::PendingWriteKind::Fill";
                        out << "        rt::PendingWriteDesc{" << kind << ", " << layout.offset << ", "
                            << layout.width << "U, " << layout.rows << "U, "
                            << layout.elementWidth << "U, " << write.dataOffset << ", "
                            << write.maskOffset << "},\n";
                    }
                    out << "    }};\n"
                           "    static constexpr std::array<std::size_t, "
                        << model_.memoryWriteOrder.size() << "> order{{";
                    for (std::size_t index = 0; index < model_.memoryWriteOrder.size(); ++index)
                    {
                        if (index != 0) out << ", ";
                        out << model_.memoryWriteOrder[index];
                    }
                    out << "}};\n"
                           "    for (std::size_t index : order) {\n"
                           "        if (pending_write_enabled_[index])\n"
                           "            rt::apply_pending_write(descriptions[index], pending_write_address_[index],\n"
                           "                                next_state_words_, pending_write_words_);\n"
                           "    }\n";
                }
                out << "}\n\n";

                out << "void " << model_.className << "::eval()\n{\n";
                for (const PortLayout &port : model_.ports) emitPortImport(out, port);
                out << "    for (std::uint32_t round = 0;; ++round) {\n"
                       "        if (round >= " << options_.fixedPointIterationLimit << "U)\n"
                       "            throw std::runtime_error(\"GRHSIM fixed-point iteration limit exceeded\");\n"
                       "        next_state_words_ = state_words_;\n"
                       "        next_state_reals_ = state_reals_;\n"
                       "        next_state_strings_ = state_strings_;\n"
                       "        std::fill(event_pulse_posedge_.begin(), event_pulse_posedge_.end(), 0);\n"
                       "        std::fill(event_pulse_negedge_.begin(), event_pulse_negedge_.end(), 0);\n"
                       "        std::fill(pending_write_enabled_.begin(), pending_write_enabled_.end(), 0);\n";
                for (std::size_t chunk = 0; chunk < chunks; ++chunk)
                    out << "        eval_ops_" << chunk << "();\n";
                out << "        apply_pending_memory_writes();\n";
                for (std::size_t index = 0; index < model_.trackSet.size(); ++index)
                {
                    const StateLayout &layout = state(model_.trackSet[index]);
                    out << "        edge_track_[" << index
                        << "] = static_cast<std::uint8_t>(state_words_[" << layout.offset
                        << "] & UINT64_C(1));\n";
                }
                out << "        const bool stable = next_state_words_ == state_words_ &&\n"
                       "                            next_state_reals_ == state_reals_ &&\n"
                       "                            next_state_strings_ == state_strings_;\n"
                       "        state_words_.swap(next_state_words_);\n"
                       "        state_reals_.swap(next_state_reals_);\n"
                       "        state_strings_.swap(next_state_strings_);\n"
                       "        if (stable) {\n"
                       "            std::fill(event_pulse_posedge_.begin(), event_pulse_posedge_.end(), 0);\n"
                       "            std::fill(event_pulse_negedge_.begin(), event_pulse_negedge_.end(), 0);\n"
                       "            break;\n"
                       "        }\n"
                       "    }\n";
                for (const PortLayout &port : model_.ports) emitPortExport(out, port);
                out << "}\n";
                return out.str();
            }

            void emitLogicConstant(std::ostream &out, const ValueLayout &result,
                                   std::string_view literal, OpId op) const
            {
                const auto words = parseLogicLiteral(literal, result.width);
                if (!words)
                {
                    diagnostics_.error("invalid logic constant in CPU emitter",
                                       "op=" + std::to_string(op.raw));
                    return;
                }
                for (std::size_t index = 0; index < words->size(); ++index)
                    out << "    edge_words_[" << result.offset + index << "] = UINT64_C("
                        << (*words)[index] << ");\n";
            }

            void emitSystemHostCall(std::ostream &out, OpId op, HostId hostId,
                                    const HostEntry &host) const
            {
                const auto operands = module_.operands(op);
                const auto results = module_.results(op);
                const std::string name(module_.symbol(host.binding));
                const std::string guard = host.kind == HostKind::Effect
                                              ? "rt::any(" + constBits(edge(operands[0]), "edge_words_") +
                                                    ") && " + eventExpression(op)
                                              : "true";
                if (host.kind == HostKind::Effect)
                {
                    out << "    if (" << guard << ") {\n        execute_system_task(\""
                        << escapeCppString(name) << "\", {";
                    for (std::size_t index = 1; index < operands.size(); ++index)
                    {
                        if (index != 1) out << ", ";
                        out << taskArg(operands[index]);
                    }
                    out << "});\n    }\n";
                    return;
                }
                if (results.empty()) return;
                const ValueLayout &result = edge(results[0]);
                if (name == "time")
                    assignCppToEdge(out, result, "simulation_time_", "    ");
                else if (name == "random" || name == "urandom")
                {
                    out << "    for (std::size_t word = 0; word < rt::word_count(" << bits(result, "edge_words_")
                        << ".width); ++word) edge_words_[" << result.offset
                        << " + word] = rt::splitmix64(random_state_);\n"
                           "    rt::truncate(" << bits(result, "edge_words_") << ");\n";
                }
                else if (name == "clog2" && !operands.empty())
                {
                    out << "    rt::op_clog2(" << bits(result, "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ");\n";
                }
                else
                    diagnostics_.error("unsupported system function in CPU emitter: " + name,
                                       "op=" + std::to_string(op.raw));
                (void)hostId;
            }

            void emitDpiHostCall(std::ostream &out, OpId op, const DpiLayout &dpi,
                                 const HostEntry &host) const
            {
                const auto operands = module_.operands(op);
                const auto results = module_.results(op);
                const std::size_t condition = host.kind == HostKind::Effect ? 1U : 0U;
                const std::string guard = host.kind == HostKind::Effect
                                              ? "rt::any(" + constBits(edge(operands[0]), "edge_words_") +
                                                    ") && " + eventExpression(op)
                                              : "true";
                out << "    if (" << guard << " && host_table_." << dpi.field << ") {\n";
                std::size_t operandIndex = condition;
                std::size_t parameterIndex = 0;
                std::size_t resultIndex = 0;
                std::vector<std::string> arguments;
                std::string returnLocal;
                for (const HostParam &parameter : module_.hostSignature(dpi.host))
                {
                    if (parameter.direction == HostParamDirection::Return)
                    {
                        returnLocal = "dpi_return_" + std::to_string(op.raw);
                        continue;
                    }
                    const DpiParameter &dpiParam = dpi.parameters[parameterIndex++];
                    const std::string local = "dpi_arg_" + std::to_string(op.raw) + "_" +
                                              std::to_string(parameterIndex - 1U);
                    out << "        " << dpiParam.baseType << ' ' << local << "{};\n";
                    if (parameter.direction == HostParamDirection::Input ||
                        parameter.direction == HostParamDirection::InOut)
                    {
                        const ValueLayout &actual = edge(operands[operandIndex++]);
                        if (actual.valueClass == ValueClass::Logic)
                        {
                            if (actual.width <= 64)
                                out << "        " << local << " = static_cast<" << dpiParam.baseType
                                    << ">(rt::to_u64(" << constBits(actual, "edge_words_") << "));\n";
                            else
                                out << "        rt::export_port(" << local << ", "
                                    << constBits(actual, "edge_words_") << ");\n";
                        }
                        else if (actual.valueClass == ValueClass::Real)
                            out << "        " << local << " = static_cast<" << dpiParam.baseType
                                << ">(edge_reals_[" << actual.offset << "]);\n";
                        else
                            out << "        " << local << " = edge_strings_[" << actual.offset << "];\n";
                    }
                    if (parameter.direction == HostParamDirection::Output ||
                        parameter.direction == HostParamDirection::InOut)
                        arguments.push_back("&" + local);
                    else if (lowerText(dpiParam.typeName) == "string")
                        arguments.push_back(local + ".c_str()");
                    else
                        arguments.push_back(local);
                }
                out << "        ";
                if (!returnLocal.empty()) out << dpi.returnType << ' ' << returnLocal << " = ";
                out << "host_table_." << dpi.field << '(';
                for (std::size_t index = 0; index < arguments.size(); ++index)
                {
                    if (index != 0) out << ", ";
                    out << arguments[index];
                }
                out << ");\n";
                parameterIndex = 0;
                for (const HostParam &parameter : module_.hostSignature(dpi.host))
                {
                    std::string value;
                    if (parameter.direction == HostParamDirection::Return)
                        value = returnLocal;
                    else
                    {
                        value = "dpi_arg_" + std::to_string(op.raw) + "_" +
                                std::to_string(parameterIndex++);
                        if (parameter.direction == HostParamDirection::Input) continue;
                    }
                    assignCppToEdge(out, edge(results[resultIndex++]), value, "        ");
                }
                out << "    }\n";
            }

            void emitOp(std::ostream &out, OpId op) const
            {
                const GenericOpcode opcode = static_cast<GenericOpcode>(module_.kind(op).opcode());
                const auto operands = module_.operands(op);
                const auto results = module_.results(op);
                out << "    // op " << op.raw << " generic." << genericOpcodeName(opcode);
                if (const SymbolId symbol = module_.opSymbol(op); symbol.valid())
                    out << " source=" << module_.symbol(symbol);
                out << "\n";
                const auto unaryReduction = [&](std::string_view expression) {
                    out << "    rt::set_bool(" << bits(edge(results[0]), "edge_words_") << ", "
                        << expression << ");\n";
                };
                switch (opcode)
                {
                case GenericOpcode::Const:
                {
                    const ValueLayout &result = edge(results[0]);
                    const std::string_view literal = symbolAttr(module_, op, "value");
                    if (result.valueClass == ValueClass::Logic)
                        emitLogicConstant(out, result, literal, op);
                    else if (result.valueClass == ValueClass::Real)
                    {
                        try
                        {
                            const double value = std::stod(std::string(literal));
                            out << "    edge_reals_[" << result.offset << "] = "
                                << std::setprecision(std::numeric_limits<double>::max_digits10)
                                << value << ";\n";
                        }
                        catch (const std::exception &)
                        {
                            diagnostics_.error("invalid real constant", "op=" + std::to_string(op.raw));
                        }
                    }
                    else
                    {
                        const std::string expression = literal.size() >= 2 && literal.front() == '"' &&
                                                               literal.back() == '"'
                                                           ? std::string(literal)
                                                           : "\"" + escapeCppString(literal) + "\"";
                        out << "    edge_strings_[" << result.offset << "] = " << expression << ";\n";
                    }
                    break;
                }
                case GenericOpcode::Add:
                case GenericOpcode::Sub:
                case GenericOpcode::Mul:
                {
                    const char *helper = opcode == GenericOpcode::Add ? "rt::op_add"
                                         : opcode == GenericOpcode::Sub ? "rt::op_sub"
                                                                         : "rt::op_mul";
                    out << "    " << helper << '(' << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << constBits(edge(operands[1]), "edge_words_") << ", "
                        << (edge(results[0]).isSigned ? "true" : "false") << ");\n";
                    break;
                }
                case GenericOpcode::Div:
                case GenericOpcode::Mod:
                    out << "    rt::op_div(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << constBits(edge(operands[1]), "edge_words_") << ", "
                        << (edge(results[0]).isSigned ? "true" : "false") << ", "
                        << (opcode == GenericOpcode::Mod ? "true" : "false") << ");\n";
                    break;
                case GenericOpcode::And:
                case GenericOpcode::Or:
                case GenericOpcode::Xor:
                case GenericOpcode::Xnor:
                {
                    const char *kind = opcode == GenericOpcode::And ? "rt::BinaryKind::And"
                                       : opcode == GenericOpcode::Or ? "rt::BinaryKind::Or"
                                       : opcode == GenericOpcode::Xor ? "rt::BinaryKind::Xor"
                                                                       : "rt::BinaryKind::Xnor";
                    out << "    rt::op_binary(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << constBits(edge(operands[1]), "edge_words_") << ", "
                        << (edge(results[0]).isSigned ? "true" : "false") << ", " << kind
                        << ");\n";
                    break;
                }
                case GenericOpcode::Not:
                    out << "    rt::op_not(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ");\n";
                    break;
                case GenericOpcode::Lt:
                case GenericOpcode::Le:
                case GenericOpcode::Gt:
                case GenericOpcode::Ge:
                {
                    const char *test = opcode == GenericOpcode::Lt ? "< 0"
                                       : opcode == GenericOpcode::Le ? "<= 0"
                                       : opcode == GenericOpcode::Gt ? "> 0"
                                                                     : ">= 0";
                    const bool signedMode = edgeSigned(operands[0]) && edgeSigned(operands[1]);
                    unaryReduction("rt::compare(" + constBits(edge(operands[0]), "edge_words_") +
                                   ", " + constBits(edge(operands[1]), "edge_words_") + ", " +
                                   (signedMode ? "true" : "false") + ") " + test);
                    break;
                }
                case GenericOpcode::Eq:
                case GenericOpcode::CaseEq:
                case GenericOpcode::WildEq:
                case GenericOpcode::Ne:
                case GenericOpcode::CaseNe:
                case GenericOpcode::WildNe:
                {
                    const bool inverse = opcode == GenericOpcode::Ne || opcode == GenericOpcode::CaseNe ||
                                         opcode == GenericOpcode::WildNe;
                    unaryReduction(std::string(inverse ? "!" : "") + "rt::equal(" +
                                   constBits(edge(operands[0]), "edge_words_") + ", " +
                                   constBits(edge(operands[1]), "edge_words_") + ")");
                    break;
                }
                case GenericOpcode::LogicAnd:
                    unaryReduction("rt::any(" + constBits(edge(operands[0]), "edge_words_") + ") && rt::any(" +
                                   constBits(edge(operands[1]), "edge_words_") + ")");
                    break;
                case GenericOpcode::LogicOr:
                    unaryReduction("rt::any(" + constBits(edge(operands[0]), "edge_words_") + ") || rt::any(" +
                                   constBits(edge(operands[1]), "edge_words_") + ")");
                    break;
                case GenericOpcode::LogicNot:
                    unaryReduction("!rt::any(" + constBits(edge(operands[0]), "edge_words_") + ")");
                    break;
                case GenericOpcode::ReduceAnd:
                case GenericOpcode::ReduceNand:
                    unaryReduction(std::string(opcode == GenericOpcode::ReduceNand ? "!" : "") +
                                   "rt::all(" + constBits(edge(operands[0]), "edge_words_") + ")");
                    break;
                case GenericOpcode::ReduceOr:
                case GenericOpcode::ReduceNor:
                    unaryReduction(std::string(opcode == GenericOpcode::ReduceNor ? "!" : "") +
                                   "rt::any(" + constBits(edge(operands[0]), "edge_words_") + ")");
                    break;
                case GenericOpcode::ReduceXor:
                case GenericOpcode::ReduceXnor:
                    unaryReduction(std::string(opcode == GenericOpcode::ReduceXnor ? "!" : "") +
                                   "rt::parity(" + constBits(edge(operands[0]), "edge_words_") + ")");
                    break;
                case GenericOpcode::Shl:
                case GenericOpcode::LShr:
                case GenericOpcode::AShr:
                    out << "    rt::op_shift(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << constBits(edge(operands[1]), "edge_words_") << ", "
                        << (opcode == GenericOpcode::Shl ? "true" : "false") << ", "
                        << (opcode == GenericOpcode::AShr ? "true" : "false") << ");\n";
                    break;
                case GenericOpcode::Mux:
                    out << "    rt::copy(" << bits(edge(results[0]), "edge_words_") << ", rt::any("
                        << constBits(edge(operands[0]), "edge_words_") << ") ? "
                        << constBits(edge(operands[1]), "edge_words_") << " : "
                        << constBits(edge(operands[2]), "edge_words_") << ");\n";
                    break;
                case GenericOpcode::Assign:
                    out << "    rt::copy(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << (edgeSigned(operands[0]) ? "true" : "false") << ");\n";
                    break;
                case GenericOpcode::Concat:
                    out << "    rt::op_concat(" << bits(edge(results[0]), "edge_words_") << ", {";
                    for (std::size_t index = 0; index < operands.size(); ++index)
                    {
                        if (index != 0) out << ", ";
                        out << constBits(edge(operands[index]), "edge_words_");
                    }
                    out << "});\n";
                    break;
                case GenericOpcode::Replicate:
                    out << "    rt::op_replicate(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ");\n";
                    break;
                case GenericOpcode::SliceStatic:
                    out << "    rt::op_slice(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << *opAttr<int64_t>(module_, op, "lsb") << "U);\n";
                    break;
                case GenericOpcode::SliceDynamic:
                case GenericOpcode::SliceArray:
                {
                    const std::string index = "slice_index_" + std::to_string(op.raw);
                    out << "    std::uint64_t " << index << " = 0;\n"
                        << "    const bool slice_valid_" << op.raw << " = rt::to_index("
                        << constBits(edge(operands[1]), "edge_words_") << ", " << index << ");\n";
                    if (opcode == GenericOpcode::SliceArray)
                        out << "    " << index << " *= " << edge(results[0]).width << "U;\n";
                    out << "    rt::op_slice(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", " << index
                        << ", slice_valid_" << op.raw << ");\n";
                    break;
                }
                case GenericOpcode::ArrayLaneConst:
                {
                    const auto *values = opAttr<std::vector<int64_t>>(module_, op, "values");
                    out << "    static constexpr std::array<std::int64_t, " << values->size()
                        << "> lane_values_" << op.raw << "{{";
                    for (std::size_t index = 0; index < values->size(); ++index)
                    {
                        if (index != 0) out << ", ";
                        out << "std::int64_t{" << (*values)[index] << "}";
                    }
                    out << "}};\n    rt::op_array_lane_const(" << bits(edge(results[0]), "edge_words_")
                        << ", " << *opAttr<int64_t>(module_, op, "elem_width") << "U, lane_values_"
                        << op.raw << ");\n";
                    break;
                }
                case GenericOpcode::ArrayMux:
                    out << "    rt::op_array_mux(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << constBits(edge(operands[1]), "edge_words_") << ", "
                        << constBits(edge(operands[2]), "edge_words_") << ");\n";
                    break;
                case GenericOpcode::ArrayOnehot:
                    out << "    rt::op_onehot(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ");\n";
                    break;
                case GenericOpcode::ArrayReduceOr:
                case GenericOpcode::ArrayReduceAnd:
                case GenericOpcode::ArrayReduceXor:
                case GenericOpcode::ArrayReduceLanesOr:
                case GenericOpcode::ArrayReduceLanesAnd:
                case GenericOpcode::ArrayReduceLanesXor:
                {
                    const bool lanes = opcode == GenericOpcode::ArrayReduceLanesOr ||
                                       opcode == GenericOpcode::ArrayReduceLanesAnd ||
                                       opcode == GenericOpcode::ArrayReduceLanesXor;
                    const char *kind = opcode == GenericOpcode::ArrayReduceOr ||
                                               opcode == GenericOpcode::ArrayReduceLanesOr
                                           ? "rt::ReduceKind::Or"
                                           : opcode == GenericOpcode::ArrayReduceAnd ||
                                                     opcode == GenericOpcode::ArrayReduceLanesAnd
                                                 ? "rt::ReduceKind::And"
                                                 : "rt::ReduceKind::Xor";
                    out << "    rt::op_array_reduce(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ", "
                        << *opAttr<int64_t>(module_, op, "elem_width") << "U, " << kind << ", "
                        << (lanes ? "true" : "false") << ");\n";
                    break;
                }
                case GenericOpcode::ArrayBroadcast:
                    out << "    rt::op_broadcast(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(edge(operands[0]), "edge_words_") << ");\n";
                    break;
                case GenericOpcode::InRead:
                {
                    const StateLayout &layout = state(targetState(op, "port"));
                    out << "    rt::copy(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(layout, "state_words_") << ");\n";
                    break;
                }
                case GenericOpcode::OutWrite:
                {
                    const StateLayout &layout = state(targetState(op, "port"));
                    const int64_t eventSlot = layout.id.valid() &&
                                                      layout.id.raw < model_.eventSlotByState.size()
                                                  ? model_.eventSlotByState[layout.id.raw]
                                                  : -1;
                    if (eventSlot >= 0)
                    {
                        out << "    const bool event_old_" << op.raw << " = (state_words_["
                            << layout.offset << "] & UINT64_C(1)) != 0;\n"
                            << "    const bool event_new_" << op.raw << " = rt::any("
                            << constBits(edge(operands[0]), "edge_words_") << ");\n"
                            << "    event_pulse_posedge_[" << eventSlot << "] = "
                            << "static_cast<std::uint8_t>(!event_old_" << op.raw
                            << " && event_new_" << op.raw << ");\n"
                            << "    event_pulse_negedge_[" << eventSlot << "] = "
                            << "static_cast<std::uint8_t>(event_old_" << op.raw
                            << " && !event_new_" << op.raw << ");\n"
                            << "    rt::copy(" << bits(layout, "next_state_words_") << ", "
                            << constBits(edge(operands[0]), "edge_words_") << ");\n";
                    }
                    else
                    {
                        out << "    rt::copy(" << bits(layout, "next_state_words_") << ", "
                            << constBits(edge(operands[0]), "edge_words_") << ");\n";
                    }
                    break;
                }
                case GenericOpcode::RegRead:
                case GenericOpcode::LatchRead:
                {
                    const StateLayout &layout = state(targetState(op, "state"));
                    out << "    rt::copy(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(layout, "state_words_") << ");\n";
                    break;
                }
                case GenericOpcode::RegWrite:
                case GenericOpcode::LatchWrite:
                {
                    const StateLayout &layout = state(targetState(op, "state"));
                    const std::string event = opcode == GenericOpcode::RegWrite ? eventExpression(op) : "true";
                    out << "    if (" << event << " && rt::any("
                        << constBits(edge(operands[0]), "edge_words_") << "))\n"
                        << "        rt::masked_merge(" << bits(layout, "next_state_words_") << ", "
                        << constBits(edge(operands[1]), "edge_words_") << ", "
                        << constBits(edge(operands[2]), "edge_words_") << ");\n";
                    break;
                }
                case GenericOpcode::MemRead:
                {
                    const StateLayout &layout = state(targetState(op, "state"));
                    const std::string address = "mem_address_" + std::to_string(op.raw);
                    out << "    std::uint64_t " << address << " = 0;\n"
                        << "    if (rt::to_index(" << constBits(edge(operands[0]), "edge_words_") << ", "
                        << address << ") && " << address << " < " << layout.rows << "U) {\n"
                        << "        rt::clear(" << bits(edge(results[0]), "edge_words_") << ");\n"
                        << "        rt::copy_range(" << bits(edge(results[0]), "edge_words_") << ", 0, "
                        << constBits(layout, "state_words_") << ", " << address << " * "
                        << layout.elementWidth << "U, " << layout.elementWidth << "U);\n"
                        << "    } else rt::clear(" << bits(edge(results[0]), "edge_words_") << ");\n";
                    break;
                }
                case GenericOpcode::MemReadAll:
                {
                    const StateLayout &layout = state(targetState(op, "state"));
                    out << "    rt::copy(" << bits(edge(results[0]), "edge_words_") << ", "
                        << constBits(layout, "state_words_") << ");\n";
                    break;
                }
                case GenericOpcode::MemWrite:
                case GenericOpcode::MemWriteLanes:
                case GenericOpcode::MemFill:
                {
                    const std::size_t writeIndex = model_.pendingWriteByOp[op.raw];
                    const PendingWriteLayout &write = model_.pendingWrites[writeIndex];
                    const StateLayout &layout = state(write.state);
                    const std::string guard = eventExpression(op);
                    if (opcode == GenericOpcode::MemWrite)
                    {
                        out << "    pending_write_enabled_[" << writeIndex << "] = static_cast<std::uint8_t>("
                            << guard << " && rt::any(" << constBits(edge(operands[0]), "edge_words_")
                            << ") && rt::to_index(" << constBits(edge(operands[1]), "edge_words_")
                            << ", pending_write_address_[" << writeIndex << "]) && "
                            << "pending_write_address_[" << writeIndex << "] < " << layout.rows << "U);\n"
                            << "    if (pending_write_enabled_[" << writeIndex << "]) {\n"
                            << "        rt::copy(rt::Bits{pending_write_words_.data() + " << write.dataOffset << ", "
                            << layout.elementWidth << "U}, "
                            << constBits(edge(operands[2]), "edge_words_") << ");\n"
                            << "        rt::copy(rt::Bits{pending_write_words_.data() + " << write.maskOffset << ", "
                            << layout.elementWidth << "U}, "
                            << constBits(edge(operands[3]), "edge_words_") << ");\n    }\n";
                    }
                    else
                    {
                        const std::size_t conditionIndex = opcode == GenericOpcode::MemFill ? 0U :
                                                                         std::numeric_limits<std::size_t>::max();
                        out << "    pending_write_enabled_[" << writeIndex << "] = static_cast<std::uint8_t>("
                            << guard;
                        if (conditionIndex != std::numeric_limits<std::size_t>::max())
                            out << " && rt::any(" << constBits(edge(operands[conditionIndex]), "edge_words_") << ")";
                        out << ");\n    if (pending_write_enabled_[" << writeIndex << "]) {\n";
                        const std::size_t dataIndex = opcode == GenericOpcode::MemFill ? 1U : 1U;
                        out << "        rt::copy(rt::Bits{pending_write_words_.data() + " << write.dataOffset << ", "
                            << layout.width << "U}, " << constBits(edge(operands[dataIndex]), "edge_words_")
                            << ");\n";
                        if (opcode == GenericOpcode::MemWriteLanes)
                            out << "        rt::copy(rt::Bits{pending_write_words_.data() + " << write.maskOffset << ", "
                                << layout.rows << "U}, " << constBits(edge(operands[0]), "edge_words_")
                                << ");\n";
                        out << "    }\n";
                    }
                    break;
                }
                case GenericOpcode::HostCall:
                {
                    const HostId hostId = module_.findHost(symbolAttr(module_, op, "entry"));
                    const HostEntry *host = module_.host(hostId);
                    const int64_t dpiIndex = hostId.valid() && hostId.raw < model_.dpiByHost.size()
                                                 ? model_.dpiByHost[hostId.raw]
                                                 : -1;
                    if (dpiIndex >= 0)
                        emitDpiHostCall(out, op, model_.dpi[static_cast<std::size_t>(dpiIndex)], *host);
                    else
                        emitSystemHostCall(out, op, hostId, *host);
                    break;
                }
                }
            }

            std::string emitChunk(std::size_t chunk, std::size_t chunkSize) const
            {
                std::ostringstream out;
                out << "#include \"grhsim_" << model_.stem << ".hpp\"\n\n"
                       "namespace rt = wolvrix_grhsim_cpu_runtime;\n\n"
                    << "void " << model_.className << "::eval_ops_" << chunk << "()\n{\n";
                const std::size_t begin = chunk * chunkSize;
                const std::size_t end = std::min(model_.schedule.size(), begin + chunkSize);
                for (std::size_t index = begin; index < end; ++index) emitOp(out, model_.schedule[index]);
                out << "}\n";
                return out.str();
            }

            std::string emitMakefile(std::size_t chunks) const
            {
                std::ostringstream out;
                const std::string prefix = "grhsim_" + model_.stem;
                out << "CXX ?= c++\nAR ?= ar\nARFLAGS ?= rcs\n"
                       "CXXFLAGS ?= -std=c++20 -O3\n"
                    << "LIB := lib" << prefix << ".a\n"
                    << "SRCS := " << prefix << ".cpp";
                for (std::size_t chunk = 0; chunk < chunks; ++chunk)
                    out << ' ' << prefix << "_ops_" << std::setw(4) << std::setfill('0') << chunk
                        << ".cpp";
                out << "\nOBJS := $(SRCS:.cpp=.o)\n\n"
                       "all: $(LIB)\n\n"
                       "$(LIB): $(OBJS)\n\t$(AR) $(ARFLAGS) $@ $^\n\n"
                       "%.o: %.cpp " << prefix << ".hpp " << prefix << "_runtime.hpp "
                    << prefix << "_support.hpp\n"
                       "\t$(CXX) $(CXXFLAGS) -I. -c $< -o $@\n\n"
                       "clean:\n\t$(RM) $(OBJS) $(LIB)\n";
                return out.str();
            }

            const Module &module_;
            const EmitModel &model_;
            const CpuSingleThreadEmitOptions &options_;
            wolvrix::lib::diag::Diagnostics &diagnostics_;
        };

    } // namespace

    CpuSingleThreadEmitResult emitCpuSingleThread(
        const Module &module,
        const CpuSingleThreadEmitOptions &options,
        wolvrix::lib::diag::Diagnostics &diagnostics)
    {
        CpuSingleThreadEmitResult result;
        if (options.outputDirectory.empty())
        {
            diagnostics.error("emit-cpu-single-thread requires an output directory");
            return result;
        }
        if (options.opsPerSourceFile == 0)
        {
            diagnostics.error("opsPerSourceFile must be greater than zero");
            return result;
        }
        if (options.fixedPointIterationLimit == 0)
        {
            diagnostics.error("fixedPointIterationLimit must be greater than zero");
            return result;
        }
        if (!module.validate(diagnostics) || diagnostics.hasError())
        {
            return result;
        }
        const auto model = buildModel(module, diagnostics);
        if (!model || diagnostics.hasError())
        {
            return result;
        }
        return CpuEmitter(module, *model, options, diagnostics).run();
    }

} // namespace wolvrix::lib::grhsim
