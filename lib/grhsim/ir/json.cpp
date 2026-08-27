#include "grhsim/ir/json.hpp"

#include "grhsim/ir/generic.hpp"

#include "slang/text/Json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        constexpr std::string_view kFormat = "wolvrix.grhsim.ir";
        constexpr int64_t kVersion = 1;

        struct JsonValue
        {
            using Array = std::vector<JsonValue>;
            using Object = std::map<std::string, JsonValue, std::less<>>;

            std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> value;

            bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(value); }
            bool isBool() const noexcept { return std::holds_alternative<bool>(value); }
            bool isInt() const noexcept { return std::holds_alternative<int64_t>(value); }
            bool isDouble() const noexcept { return std::holds_alternative<double>(value); }
            bool isString() const noexcept { return std::holds_alternative<std::string>(value); }
            bool isArray() const noexcept { return std::holds_alternative<Array>(value); }
            bool isObject() const noexcept { return std::holds_alternative<Object>(value); }

            bool asBool(std::string_view context) const
            {
                if (!isBool())
                {
                    throw std::runtime_error(std::string(context) + ": expected bool");
                }
                return std::get<bool>(value);
            }

            int64_t asInt(std::string_view context) const
            {
                if (!isInt())
                {
                    throw std::runtime_error(std::string(context) + ": expected integer");
                }
                return std::get<int64_t>(value);
            }

            template <typename UInt>
            UInt asUnsigned(std::string_view context) const
            {
                static_assert(std::is_unsigned_v<UInt>);
                const int64_t integer = asInt(context);
                if (integer < 0 ||
                    static_cast<uint64_t>(integer) > std::numeric_limits<UInt>::max())
                {
                    throw std::runtime_error(std::string(context) + ": unsigned integer is out of range");
                }
                return static_cast<UInt>(integer);
            }

            uint32_t asUInt32(std::string_view context) const
            {
                return asUnsigned<uint32_t>(context);
            }

            uint16_t asUInt16(std::string_view context) const
            {
                return asUnsigned<uint16_t>(context);
            }

            uint8_t asUInt8(std::string_view context) const
            {
                return asUnsigned<uint8_t>(context);
            }

            uint32_t asId(std::string_view context) const
            {
                const uint32_t integer = asUInt32(context);
                if (integer == OpId::kInvalid)
                {
                    throw std::runtime_error(std::string(context) + ": invalid 32-bit ID");
                }
                return integer;
            }

            std::optional<uint32_t> asNullableId(std::string_view context) const
            {
                const int64_t integer = asInt(context);
                if (integer == -1)
                {
                    return std::nullopt;
                }
                if (integer < 0 ||
                    static_cast<uint64_t>(integer) >= static_cast<uint64_t>(OpId::kInvalid))
                {
                    throw std::runtime_error(std::string(context) + ": invalid nullable 32-bit ID");
                }
                return static_cast<uint32_t>(integer);
            }

            double asDouble(std::string_view context) const
            {
                if (isDouble())
                {
                    return std::get<double>(value);
                }
                if (isInt())
                {
                    return static_cast<double>(std::get<int64_t>(value));
                }
                throw std::runtime_error(std::string(context) + ": expected number");
            }

            const std::string &asString(std::string_view context) const
            {
                if (!isString())
                {
                    throw std::runtime_error(std::string(context) + ": expected string");
                }
                return std::get<std::string>(value);
            }

            const Array &asArray(std::string_view context) const
            {
                if (!isArray())
                {
                    throw std::runtime_error(std::string(context) + ": expected array");
                }
                return std::get<Array>(value);
            }

            const Object &asObject(std::string_view context) const
            {
                if (!isObject())
                {
                    throw std::runtime_error(std::string(context) + ": expected object");
                }
                return std::get<Object>(value);
            }
        };

        class JsonParser
        {
        public:
            explicit JsonParser(std::string_view text)
                : current_(text.data()), end_(text.data() + text.size())
            {
            }

            JsonValue parse()
            {
                skipWhitespace();
                JsonValue result = parseValue();
                skipWhitespace();
                if (current_ != end_)
                {
                    throw std::runtime_error("unexpected trailing JSON data");
                }
                return result;
            }

        private:
            JsonValue parseValue()
            {
                if (current_ == end_)
                {
                    throw std::runtime_error("unexpected end of JSON input");
                }
                switch (*current_)
                {
                case '{': return parseObject();
                case '[': return parseArray();
                case '"': return JsonValue{parseString()};
                case 't': consumeLiteral("true"); return JsonValue{true};
                case 'f': consumeLiteral("false"); return JsonValue{false};
                case 'n': consumeLiteral("null"); return JsonValue{nullptr};
                default:
                    if (*current_ == '-' || (*current_ >= '0' && *current_ <= '9'))
                    {
                        return parseNumber();
                    }
                    throw std::runtime_error("invalid JSON value");
                }
            }

            JsonValue parseObject()
            {
                expect('{');
                skipWhitespace();
                JsonValue::Object object;
                if (consume('}'))
                {
                    return JsonValue{std::move(object)};
                }
                while (true)
                {
                    if (current_ == end_ || *current_ != '"')
                    {
                        throw std::runtime_error("JSON object key must be a string");
                    }
                    std::string key = parseString();
                    skipWhitespace();
                    expect(':');
                    skipWhitespace();
                    auto [it, inserted] = object.emplace(std::move(key), parseValue());
                    if (!inserted)
                    {
                        throw std::runtime_error("duplicate JSON object key: " + it->first);
                    }
                    skipWhitespace();
                    if (consume('}'))
                    {
                        break;
                    }
                    expect(',');
                    skipWhitespace();
                }
                return JsonValue{std::move(object)};
            }

            JsonValue parseArray()
            {
                expect('[');
                skipWhitespace();
                JsonValue::Array array;
                if (consume(']'))
                {
                    return JsonValue{std::move(array)};
                }
                while (true)
                {
                    array.push_back(parseValue());
                    skipWhitespace();
                    if (consume(']'))
                    {
                        break;
                    }
                    expect(',');
                    skipWhitespace();
                }
                return JsonValue{std::move(array)};
            }

            static uint32_t hexDigit(char ch)
            {
                if (ch >= '0' && ch <= '9') return static_cast<uint32_t>(ch - '0');
                if (ch >= 'a' && ch <= 'f') return static_cast<uint32_t>(ch - 'a' + 10);
                if (ch >= 'A' && ch <= 'F') return static_cast<uint32_t>(ch - 'A' + 10);
                throw std::runtime_error("invalid JSON unicode escape");
            }

            uint32_t parseHex4()
            {
                if (end_ - current_ < 4)
                {
                    throw std::runtime_error("truncated JSON unicode escape");
                }
                uint32_t value = 0;
                for (int index = 0; index < 4; ++index)
                {
                    value = (value << 4U) | hexDigit(*current_++);
                }
                return value;
            }

            static void appendUtf8(std::string &out, uint32_t codepoint)
            {
                if (codepoint <= 0x7fU)
                {
                    out.push_back(static_cast<char>(codepoint));
                }
                else if (codepoint <= 0x7ffU)
                {
                    out.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
                    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                }
                else if (codepoint <= 0xffffU)
                {
                    out.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
                    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
                    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                }
                else if (codepoint <= 0x10ffffU)
                {
                    out.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
                    out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
                    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
                    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                }
                else
                {
                    throw std::runtime_error("invalid JSON unicode codepoint");
                }
            }

            std::string parseString()
            {
                expect('"');
                std::string result;
                while (current_ != end_)
                {
                    const unsigned char ch = static_cast<unsigned char>(*current_++);
                    if (ch == '"')
                    {
                        return result;
                    }
                    if (ch < 0x20U)
                    {
                        throw std::runtime_error("control character in JSON string");
                    }
                    if (ch != '\\')
                    {
                        result.push_back(static_cast<char>(ch));
                        continue;
                    }
                    if (current_ == end_)
                    {
                        throw std::runtime_error("truncated JSON escape");
                    }
                    switch (*current_++)
                    {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u':
                    {
                        uint32_t codepoint = parseHex4();
                        if (codepoint >= 0xd800U && codepoint <= 0xdbffU)
                        {
                            if (end_ - current_ < 2 || current_[0] != '\\' || current_[1] != 'u')
                            {
                                throw std::runtime_error("missing low JSON unicode surrogate");
                            }
                            current_ += 2;
                            const uint32_t low = parseHex4();
                            if (low < 0xdc00U || low > 0xdfffU)
                            {
                                throw std::runtime_error("invalid low JSON unicode surrogate");
                            }
                            codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                                        (low - 0xdc00U);
                        }
                        else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU)
                        {
                            throw std::runtime_error("unexpected low JSON unicode surrogate");
                        }
                        appendUtf8(result, codepoint);
                        break;
                    }
                    default:
                        throw std::runtime_error("invalid JSON escape");
                    }
                }
                throw std::runtime_error("unterminated JSON string");
            }

            JsonValue parseNumber()
            {
                const char *start = current_;
                if (*current_ == '-') ++current_;
                if (current_ == end_) throw std::runtime_error("truncated JSON number");
                if (*current_ == '0')
                {
                    ++current_;
                }
                else
                {
                    if (*current_ < '1' || *current_ > '9')
                        throw std::runtime_error("invalid JSON number");
                    while (current_ != end_ && *current_ >= '0' && *current_ <= '9') ++current_;
                }
                bool floating = false;
                if (current_ != end_ && *current_ == '.')
                {
                    floating = true;
                    ++current_;
                    if (current_ == end_ || *current_ < '0' || *current_ > '9')
                        throw std::runtime_error("invalid JSON fraction");
                    while (current_ != end_ && *current_ >= '0' && *current_ <= '9') ++current_;
                }
                if (current_ != end_ && (*current_ == 'e' || *current_ == 'E'))
                {
                    floating = true;
                    ++current_;
                    if (current_ != end_ && (*current_ == '+' || *current_ == '-')) ++current_;
                    if (current_ == end_ || *current_ < '0' || *current_ > '9')
                        throw std::runtime_error("invalid JSON exponent");
                    while (current_ != end_ && *current_ >= '0' && *current_ <= '9') ++current_;
                }
                const std::string_view text(start, static_cast<std::size_t>(current_ - start));
                if (!floating)
                {
                    int64_t value = 0;
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
                    {
                        return JsonValue{value};
                    }
                }
                std::string owned(text);
                char *end = nullptr;
                const double value = std::strtod(owned.c_str(), &end);
                if (!end || end != owned.c_str() + owned.size() || !std::isfinite(value))
                {
                    throw std::runtime_error("invalid JSON number");
                }
                return JsonValue{value};
            }

            void consumeLiteral(std::string_view literal)
            {
                if (static_cast<std::size_t>(end_ - current_) < literal.size() ||
                    std::string_view(current_, literal.size()) != literal)
                {
                    throw std::runtime_error("invalid JSON literal");
                }
                current_ += literal.size();
            }

            void skipWhitespace()
            {
                while (current_ != end_ && (*current_ == ' ' || *current_ == '\n' ||
                                            *current_ == '\r' || *current_ == '\t'))
                {
                    ++current_;
                }
            }

            bool consume(char expected)
            {
                if (current_ != end_ && *current_ == expected)
                {
                    ++current_;
                    return true;
                }
                return false;
            }

            void expect(char expected)
            {
                if (!consume(expected))
                {
                    throw std::runtime_error(std::string("expected JSON token '") + expected + "'");
                }
            }

            const char *current_;
            const char *end_;
        };

        const JsonValue &field(const JsonValue::Object &object, std::string_view name,
                               std::string_view context)
        {
            const auto found = object.find(name);
            if (found == object.end())
            {
                throw std::runtime_error(std::string(context) + ": missing field '" +
                                         std::string(name) + "'");
            }
            return found->second;
        }

        std::string typeKindName(const TypeRec &type)
        {
            if (type.track == TypeTrack::Backend)
            {
                return "backend";
            }
            switch (static_cast<GenericTypeKind>(type.kind))
            {
            case GenericTypeKind::Logic: return "logic";
            case GenericTypeKind::Array: return "array";
            case GenericTypeKind::Real: return "real";
            case GenericTypeKind::String: return "string";
            }
            return {};
        }

        std::string_view stateKindName(StateKind kind)
        {
            switch (kind)
            {
            case StateKind::Input: return "in";
            case StateKind::Output: return "out";
            case StateKind::State: return "state";
            }
            return {};
        }

        StateKind parseStateKind(std::string_view text)
        {
            if (text == "in") return StateKind::Input;
            if (text == "out") return StateKind::Output;
            if (text == "state") return StateKind::State;
            throw std::runtime_error("unknown StateDecl kind: " + std::string(text));
        }

        std::string_view hostKindName(HostKind kind)
        {
            return kind == HostKind::Query ? "query" : "effect";
        }

        HostKind parseHostKind(std::string_view text)
        {
            if (text == "query") return HostKind::Query;
            if (text == "effect") return HostKind::Effect;
            throw std::runtime_error("unknown HostTable kind: " + std::string(text));
        }

        std::string_view directionName(HostParamDirection direction)
        {
            switch (direction)
            {
            case HostParamDirection::Input: return "input";
            case HostParamDirection::Output: return "output";
            case HostParamDirection::InOut: return "inout";
            case HostParamDirection::Return: return "return";
            }
            return {};
        }

        HostParamDirection parseDirection(std::string_view text)
        {
            if (text == "input") return HostParamDirection::Input;
            if (text == "output") return HostParamDirection::Output;
            if (text == "inout") return HostParamDirection::InOut;
            if (text == "return") return HostParamDirection::Return;
            throw std::runtime_error("unknown HostTable parameter direction: " + std::string(text));
        }

        std::string_view activationName(ActivationKind kind)
        {
            switch (kind)
            {
            case ActivationKind::Always: return "always";
            case ActivationKind::Posedge: return "posedge";
            case ActivationKind::Negedge: return "negedge";
            }
            return {};
        }

        ActivationKind parseActivation(std::string_view text)
        {
            if (text == "always") return ActivationKind::Always;
            if (text == "posedge") return ActivationKind::Posedge;
            if (text == "negedge") return ActivationKind::Negedge;
            throw std::runtime_error("unknown region activation: " + std::string(text));
        }

        void writeString(slang::JsonWriter &writer, std::string_view value)
        {
            writer.writeValue(value);
        }

        template <typename Id>
        void writeIds(slang::JsonWriter &writer, std::span<const Id> ids)
        {
            writer.startArray();
            for (Id id : ids)
            {
                writer.writeValue(static_cast<uint64_t>(id.raw));
            }
            writer.endArray();
        }

        void writeAttrValue(slang::JsonWriter &writer, const Module &module,
                            const AttrValue &value)
        {
            std::visit(
                [&](const auto &typed) {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t>)
                    {
                        writer.writeValue(typed);
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        if (!std::isfinite(typed))
                        {
                            throw std::runtime_error(
                                "cannot serialize a non-finite floating-point attribute");
                        }
                        writer.writeValue(typed);
                    }
                    else if constexpr (std::is_same_v<T, SymbolId>)
                    {
                        writeString(writer, module.symbol(typed));
                    }
                    else
                    {
                        writer.startArray();
                        for (const auto &item : typed)
                        {
                            if constexpr (std::is_same_v<T, std::vector<SymbolId>>)
                            {
                                writeString(writer, module.symbol(item));
                            }
                            else
                            {
                                if constexpr (std::is_same_v<T, std::vector<double>>)
                                {
                                    if (!std::isfinite(item))
                                    {
                                        throw std::runtime_error(
                                            "cannot serialize a non-finite floating-point attribute");
                                    }
                                }
                                writer.writeValue(item);
                            }
                        }
                        writer.endArray();
                    }
                },
                value);
        }

        std::string_view attrTypeName(const AttrValue &value)
        {
            switch (value.index())
            {
            case 0: return "bool";
            case 1: return "int";
            case 2: return "double";
            case 3: return "string";
            case 4: return "bool_array";
            case 5: return "int_array";
            case 6: return "double_array";
            case 7: return "string_array";
            default: return {};
            }
        }

        void writeAttrs(slang::JsonWriter &writer, const Module &module,
                        std::span<const AttrKV> attrs)
        {
            writer.startArray();
            for (const AttrKV &attr : attrs)
            {
                writer.startObject();
                writer.writeProperty("key");
                writeString(writer, module.symbol(attr.key));
                writer.writeProperty("type");
                writeString(writer, attrTypeName(attr.value));
                writer.writeProperty("value");
                writeAttrValue(writer, module, attr.value);
                writer.endObject();
            }
            writer.endArray();
        }

        AttrValue parseAttrValue(Module &module, std::string_view type, const JsonValue &value,
                                 std::string_view context)
        {
            if (type == "bool") return value.asBool(context);
            if (type == "int") return value.asInt(context);
            if (type == "double") return value.asDouble(context);
            if (type == "string") return module.intern(value.asString(context));
            const auto &array = value.asArray(context);
            if (type == "bool_array")
            {
                std::vector<bool> result;
                for (const JsonValue &item : array) result.push_back(item.asBool(context));
                return result;
            }
            if (type == "int_array")
            {
                std::vector<int64_t> result;
                for (const JsonValue &item : array) result.push_back(item.asInt(context));
                return result;
            }
            if (type == "double_array")
            {
                std::vector<double> result;
                for (const JsonValue &item : array) result.push_back(item.asDouble(context));
                return result;
            }
            if (type == "string_array")
            {
                std::vector<SymbolId> result;
                for (const JsonValue &item : array) result.push_back(module.intern(item.asString(context)));
                return result;
            }
            throw std::runtime_error(std::string(context) + ": unknown attribute type");
        }

        std::vector<AttrKV> parseAttrs(Module &module, const JsonValue &json,
                                       std::string_view context)
        {
            std::vector<AttrKV> result;
            for (const JsonValue &item : json.asArray(context))
            {
                const auto &object = item.asObject(context);
                const std::string &key = field(object, "key", context).asString(context);
                const std::string &type = field(object, "type", context).asString(context);
                result.push_back(AttrKV{
                    .key = module.intern(key),
                    .value = parseAttrValue(module, type, field(object, "value", context), context),
                });
            }
            return result;
        }

        template <typename Id>
        std::vector<Id> parseIds(const JsonValue &json, std::string_view context)
        {
            std::vector<Id> result;
            for (const JsonValue &item : json.asArray(context))
            {
                result.push_back(Id{item.asId(context)});
            }
            return result;
        }

        void requireId(uint32_t actual, std::size_t expected, std::string_view context)
        {
            if (actual != expected)
            {
                throw std::runtime_error(std::string(context) + ": IDs are not dense and ordered");
            }
        }
    } // namespace

    std::string storeJson(const Module &source, bool pretty)
    {
        Module module = source;
        module.freeze();
        wolvrix::lib::diag::Diagnostics diagnostics;
        if (!module.validate(diagnostics))
        {
            const std::string message = diagnostics.messages().empty()
                                            ? "GRHSIM Module validation failed"
                                            : diagnostics.messages().front().message;
            throw std::runtime_error(message);
        }

        slang::JsonWriter writer;
        writer.setPrettyPrint(pretty);
        writer.startObject();
        writer.writeProperty("format");
        writeString(writer, kFormat);
        writer.writeProperty("version");
        writer.writeValue(kVersion);
        writer.writeProperty("name");
        writeString(writer, module.name());

        writer.writeProperty("types");
        writer.startArray();
        for (const TypeRec &type : module.types())
        {
            writer.startObject();
            writer.writeProperty("track");
            writeString(writer, type.track == TypeTrack::Generic ? std::string_view("generic")
                                                                 : std::string_view("backend"));
            writer.writeProperty("kind");
            writeString(writer, typeKindName(type));
            writer.writeProperty("kind_id");
            writer.writeValue(static_cast<uint64_t>(type.kind));
            writer.writeProperty("dialect");
            writer.writeValue(static_cast<uint64_t>(type.dialect));
            writer.writeProperty("width");
            writer.writeValue(static_cast<uint64_t>(type.width));
            writer.writeProperty("rows");
            writer.writeValue(static_cast<uint64_t>(type.rows));
            writer.writeProperty("signed");
            writer.writeValue(type.isSigned);
            writer.writeProperty("element_type");
            writer.writeValue(type.elementType.valid() ? static_cast<int64_t>(type.elementType.raw) : -1);
            writer.writeProperty("refines");
            writer.writeValue(type.refines.valid() ? static_cast<int64_t>(type.refines.raw) : -1);
            writer.writeProperty("parameters");
            writer.startArray();
            const TypeId id{static_cast<uint32_t>(&type - module.types().data())};
            for (uint32_t parameter : module.typeParameters(id))
            {
                writer.writeValue(static_cast<uint64_t>(parameter));
            }
            writer.endArray();
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("states");
        writer.startArray();
        for (uint32_t index = 0; index < module.states().size(); ++index)
        {
            const StateEntry &state = module.states()[index];
            writer.startObject();
            writer.writeProperty("name");
            writeString(writer, module.symbol(state.name));
            writer.writeProperty("kind");
            writeString(writer, stateKindName(state.kind));
            writer.writeProperty("gen_type");
            writer.writeValue(static_cast<uint64_t>(state.genType.raw));
            writer.writeProperty("backend_type");
            writer.writeValue(state.backendType.valid() ? static_cast<int64_t>(state.backendType.raw) : -1);
            writer.writeProperty("init");
            writeAttrs(writer, module, module.stateInitAttrs(StateId{index}));
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("hosts");
        writer.startArray();
        for (uint32_t index = 0; index < module.hosts().size(); ++index)
        {
            const HostEntry &host = module.hosts()[index];
            writer.startObject();
            writer.writeProperty("entry");
            writeString(writer, module.symbol(host.entry));
            writer.writeProperty("kind");
            writeString(writer, hostKindName(host.kind));
            writer.writeProperty("binding");
            writeString(writer, module.symbol(host.binding));
            writer.writeProperty("signature");
            writer.startArray();
            for (const HostParam &parameter : module.hostSignature(HostId{index}))
            {
                writer.startObject();
                writer.writeProperty("has_name");
                writer.writeValue(parameter.name.valid());
                writer.writeProperty("name");
                writeString(writer, parameter.name.valid() ? module.symbol(parameter.name)
                                                           : std::string_view{});
                writer.writeProperty("type");
                writer.writeValue(static_cast<uint64_t>(parameter.type.raw));
                writer.writeProperty("direction");
                writeString(writer, directionName(parameter.direction));
                writer.endObject();
            }
            writer.endArray();
            writer.writeProperty("attrs");
            writeAttrs(writer, module, module.hostAttrs(HostId{index}));
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("ops");
        writer.startArray();
        for (OpId op : module.ops())
        {
            writer.startObject();
            writer.writeProperty("kind");
            writeString(writer, dialectRegistry().opName(module.kind(op)));
            writer.writeProperty("has_symbol");
            writer.writeValue(module.opSymbol(op).valid());
            writer.writeProperty("symbol");
            writeString(writer, module.opSymbol(op).valid() ? module.symbol(module.opSymbol(op))
                                                           : std::string_view{});
            writer.writeProperty("operands");
            writeIds(writer, module.operands(op));
            writer.writeProperty("results");
            writeIds(writer, module.results(op));
            writer.writeProperty("attrs");
            writeAttrs(writer, module, module.attrs(op));
            writer.writeProperty("region");
            const RegionId region = module.regionOf(op);
            writer.writeValue(region.valid() ? static_cast<int64_t>(region.raw) : -1);
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("edges");
        writer.startArray();
        for (EdgeId edge : module.edges())
        {
            writer.startObject();
            writer.writeProperty("type");
            writer.writeValue(static_cast<uint64_t>(module.edgeType(edge).raw));
            writer.writeProperty("def");
            writer.writeValue(static_cast<uint64_t>(module.def(edge).raw));
            writer.writeProperty("has_symbol");
            writer.writeValue(module.edgeSymbol(edge).valid());
            writer.writeProperty("symbol");
            writeString(writer, module.edgeSymbol(edge).valid() ? module.symbol(module.edgeSymbol(edge))
                                                               : std::string_view{});
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("regions");
        writer.startArray();
        for (RegionId region : module.regions())
        {
            const RegionRec *record = module.region(region);
            writer.startObject();
            writer.writeProperty("activation");
            writeString(writer, activationName(record->activation.kind));
            writer.writeProperty("activation_state");
            writer.writeValue(record->activation.state.valid()
                                  ? static_cast<int64_t>(record->activation.state.raw)
                                  : -1);
            writer.writeProperty("ops");
            writeIds(writer, module.regionOps(region));
            writer.writeProperty("deps");
            writeIds(writer, module.regionDeps(region));
            writer.endObject();
        }
        writer.endArray();

        writer.endObject();
        return std::string(writer.view());
    }

    Module loadJson(std::string_view json)
    {
        const JsonValue root = JsonParser(json).parse();
        const auto &object = root.asObject("root");
        if (field(object, "format", "root").asString("root.format") != kFormat)
        {
            throw std::runtime_error("not a GRHSIM IR JSON document");
        }
        if (field(object, "version", "root").asInt("root.version") != kVersion)
        {
            throw std::runtime_error("unsupported GRHSIM IR JSON version");
        }
        Module module(field(object, "name", "root").asString("root.name"));

        const auto &types = field(object, "types", "root").asArray("root.types");
        for (std::size_t index = 0; index < types.size(); ++index)
        {
            const auto &type = types[index].asObject("type");
            const std::string &track = field(type, "track", "type").asString("type.track");
            const std::string &kind = field(type, "kind", "type").asString("type.kind");
            const uint8_t kindId = field(type, "kind_id", "type").asUInt8("type.kind_id");
            const uint16_t dialect = field(type, "dialect", "type").asUInt16("type.dialect");
            const std::optional<uint32_t> element =
                field(type, "element_type", "type").asNullableId("type.element_type");
            const std::optional<uint32_t> refines =
                field(type, "refines", "type").asNullableId("type.refines");
            TypeId id = TypeId::invalid();
            if (track == "generic")
            {
                if (kind == "logic")
                {
                    id = module.internLogicType(field(type, "width", "type").asUInt32("type.width"),
                                                field(type, "signed", "type").asBool("type.signed"));
                }
                else if (kind == "array")
                {
                    const uint32_t rows = field(type, "rows", "type").asUInt32("type.rows");
                    if (!element) throw std::runtime_error("array Type is missing element_type");
                    id = module.internArrayType(rows, TypeId{*element});
                }
                else if (kind == "real")
                {
                    id = module.internRealType();
                }
                else if (kind == "string")
                {
                    id = module.internStringType();
                }
                else
                {
                    throw std::runtime_error("unknown generic Type kind");
                }
                const TypeRec *record = module.type(id);
                if (dialect != kGenericDialect || !record || record->kind != kindId)
                {
                    throw std::runtime_error("generic Type kind_id or dialect is inconsistent");
                }
            }
            else if (track == "backend")
            {
                if (kind != "backend") throw std::runtime_error("invalid backend Type kind");
                if (!refines) throw std::runtime_error("backend Type is missing refines");
                std::vector<uint32_t> parameters;
                for (const JsonValue &parameter :
                     field(type, "parameters", "type").asArray("type.parameters"))
                {
                    parameters.push_back(parameter.asUInt32("type.parameters[]"));
                }
                id = module.internBackendType(dialect, kindId, parameters,
                                              TypeId{*refines});
            }
            else
            {
                throw std::runtime_error("unknown Type track");
            }
            if (!id.valid()) throw std::runtime_error("invalid Type record");
            requireId(id.raw, index, "types");
        }

        const auto &states = field(object, "states", "root").asArray("root.states");
        for (std::size_t index = 0; index < states.size(); ++index)
        {
            const auto &state = states[index].asObject("state");
            const auto init = parseAttrs(module, field(state, "init", "state"), "state.init");
            const StateId id = module.addState(
                field(state, "name", "state").asString("state.name"),
                parseStateKind(field(state, "kind", "state").asString("state.kind")),
                TypeId{field(state, "gen_type", "state").asId("state.gen_type")}, init);
            if (!id.valid()) throw std::runtime_error("invalid StateDecl record");
            requireId(id.raw, index, "states");
            const std::optional<uint32_t> backend =
                field(state, "backend_type", "state").asNullableId("state.backend_type");
            if (backend && !module.setBackendType(id, TypeId{*backend}))
            {
                throw std::runtime_error("invalid StateDecl backend_type");
            }
        }

        const auto &hosts = field(object, "hosts", "root").asArray("root.hosts");
        for (std::size_t index = 0; index < hosts.size(); ++index)
        {
            const auto &host = hosts[index].asObject("host");
            std::vector<HostParam> signature;
            for (const JsonValue &parameterJson :
                 field(host, "signature", "host").asArray("host.signature"))
            {
                const auto &parameter = parameterJson.asObject("host.parameter");
                const bool hasName = field(parameter, "has_name", "host.parameter")
                                         .asBool("host.parameter.has_name");
                signature.push_back(HostParam{
                    .name = hasName
                                ? module.intern(field(parameter, "name", "host.parameter")
                                                    .asString("host.parameter.name"))
                                : SymbolId::invalid(),
                    .type = TypeId{field(parameter, "type", "host.parameter")
                                       .asId("host.parameter.type")},
                    .direction = parseDirection(
                        field(parameter, "direction", "host.parameter")
                            .asString("host.parameter.direction")),
                });
            }
            const auto attrs = parseAttrs(module, field(host, "attrs", "host"), "host.attrs");
            const HostId id = module.addHost(
                field(host, "entry", "host").asString("host.entry"),
                parseHostKind(field(host, "kind", "host").asString("host.kind")),
                signature,
                field(host, "binding", "host").asString("host.binding"), attrs);
            if (!id.valid()) throw std::runtime_error("invalid HostTable record");
            requireId(id.raw, index, "hosts");
        }

        const auto &ops = field(object, "ops", "root").asArray("root.ops");
        std::vector<std::vector<EdgeId>> expectedResults;
        std::vector<RegionId> expectedRegions;
        expectedResults.reserve(ops.size());
        expectedRegions.reserve(ops.size());
        for (std::size_t index = 0; index < ops.size(); ++index)
        {
            const auto &op = ops[index].asObject("op");
            const auto kind = dialectRegistry().find(field(op, "kind", "op").asString("op.kind"));
            if (!kind) throw std::runtime_error("unknown operation kind in JSON");
            const bool hasSymbol = field(op, "has_symbol", "op").asBool("op.has_symbol");
            const SymbolId symbol = hasSymbol
                                        ? module.intern(field(op, "symbol", "op").asString("op.symbol"))
                                        : SymbolId::invalid();
            const OpId id = module.createOp(*kind, symbol);
            if (!id.valid()) throw std::runtime_error("invalid operation record");
            requireId(id.raw, index, "ops");
            expectedResults.push_back(parseIds<EdgeId>(field(op, "results", "op"), "op.results"));
            const std::optional<uint32_t> region =
                field(op, "region", "op").asNullableId("op.region");
            expectedRegions.push_back(region ? RegionId{*region} : RegionId::invalid());
        }

        const auto &edges = field(object, "edges", "root").asArray("root.edges");
        for (std::size_t index = 0; index < edges.size(); ++index)
        {
            const auto &edge = edges[index].asObject("edge");
            const bool hasSymbol = field(edge, "has_symbol", "edge").asBool("edge.has_symbol");
            const SymbolId symbol = hasSymbol
                                        ? module.intern(field(edge, "symbol", "edge").asString("edge.symbol"))
                                        : SymbolId::invalid();
            const EdgeId id = module.addResult(
                OpId{field(edge, "def", "edge").asId("edge.def")},
                TypeId{field(edge, "type", "edge").asId("edge.type")}, symbol);
            if (!id.valid()) throw std::runtime_error("invalid edge record");
            requireId(id.raw, index, "edges");
        }

        for (std::size_t index = 0; index < ops.size(); ++index)
        {
            const auto &op = ops[index].asObject("op");
            const OpId id{static_cast<uint32_t>(index)};
            const auto operands = parseIds<EdgeId>(field(op, "operands", "op"), "op.operands");
            if (!module.setOperands(id, operands))
                throw std::runtime_error("invalid operation operands");
            const auto attrs = parseAttrs(module, field(op, "attrs", "op"), "op.attrs");
            for (AttrKV attr : attrs)
            {
                if (!module.setAttr(id, module.symbol(attr.key), std::move(attr.value)))
                    throw std::runtime_error("invalid operation attribute");
            }
            const auto actualResults = module.results(id);
            if (actualResults.size() != expectedResults[index].size() ||
                !std::equal(actualResults.begin(), actualResults.end(), expectedResults[index].begin()))
            {
                throw std::runtime_error("operation result order does not match edge definitions");
            }
        }

        const auto &regions = field(object, "regions", "root").asArray("root.regions");
        for (std::size_t index = 0; index < regions.size(); ++index)
        {
            const auto &region = regions[index].asObject("region");
            const ActivationKind activation = parseActivation(
                field(region, "activation", "region").asString("region.activation"));
            const std::optional<uint32_t> state =
                field(region, "activation_state", "region")
                    .asNullableId("region.activation_state");
            const RegionId id = module.createRegion(Activation{
                .kind = activation,
                .state = state ? StateId{*state} : StateId::invalid(),
            });
            if (!id.valid()) throw std::runtime_error("invalid region record");
            requireId(id.raw, index, "regions");
        }
        for (std::size_t index = 0; index < regions.size(); ++index)
        {
            const auto &region = regions[index].asObject("region");
            const RegionId id{static_cast<uint32_t>(index)};
            const auto regionOps = parseIds<OpId>(field(region, "ops", "region"), "region.ops");
            if (!module.setRegion(regionOps, id) || !module.setRegionOrder(id, regionOps))
                throw std::runtime_error("invalid region operation order");
            const auto deps = parseIds<RegionId>(field(region, "deps", "region"), "region.deps");
            for (RegionId dep : deps)
            {
                if (!module.addRegionDep(id, dep))
                    throw std::runtime_error("invalid region dependency");
            }
        }
        for (std::size_t index = 0; index < expectedRegions.size(); ++index)
        {
            const RegionId actual = module.regionOf(OpId{static_cast<uint32_t>(index)});
            const RegionId expected = expectedRegions[index];
            if (actual != expected)
            {
                throw std::runtime_error("operation region does not match Schedule");
            }
        }

        module.freeze();
        wolvrix::lib::diag::Diagnostics diagnostics;
        if (!module.validate(diagnostics))
        {
            const std::string message = diagnostics.messages().empty()
                                            ? "loaded GRHSIM Module is invalid"
                                            : diagnostics.messages().front().message;
            throw std::runtime_error(message);
        }
        return module;
    }

    bool structurallyEquivalent(const Module &lhs, const Module &rhs)
    {
        try
        {
            return storeJson(lhs, false) == storeJson(rhs, false);
        }
        catch (...)
        {
            return false;
        }
    }

} // namespace wolvrix::lib::grhsim
