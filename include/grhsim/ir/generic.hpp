#ifndef WOLVRIX_GRHSIM_IR_GENERIC_HPP
#define WOLVRIX_GRHSIM_IR_GENERIC_HPP

#include "grhsim/ir/module.hpp"

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grhsim
{

    inline constexpr uint16_t kGenericDialect = 0;

    enum class GenericOpcode : uint16_t
    {
        Const,
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        And,
        Or,
        Xor,
        Xnor,
        Not,
        Lt,
        Le,
        Gt,
        Ge,
        Eq,
        Ne,
        CaseEq,
        CaseNe,
        WildEq,
        WildNe,
        LogicAnd,
        LogicOr,
        LogicNot,
        ReduceAnd,
        ReduceNand,
        ReduceOr,
        ReduceNor,
        ReduceXor,
        ReduceXnor,
        Shl,
        LShr,
        AShr,
        Mux,
        Assign,
        Concat,
        Replicate,
        SliceStatic,
        SliceDynamic,
        SliceArray,
        ArrayLaneConst,
        ArrayMux,
        ArrayOnehot,
        ArrayReduceOr,
        ArrayReduceAnd,
        ArrayReduceXor,
        ArrayReduceLanesOr,
        ArrayReduceLanesAnd,
        ArrayReduceLanesXor,
        ArrayBroadcast,
        InRead,
        OutWrite,
        RegRead,
        RegWrite,
        LatchRead,
        LatchWrite,
        MemRead,
        MemReadAll,
        MemWrite,
        MemWriteLanes,
        MemFill,
        HostCall,
    };

    constexpr OpKind genericOp(GenericOpcode opcode) noexcept
    {
        return OpKind::get(kGenericDialect, static_cast<uint16_t>(opcode));
    }

    enum class AttrType : uint8_t
    {
        Bool,
        Int,
        Double,
        String,
        BoolArray,
        IntArray,
        DoubleArray,
        StringArray,
    };

    struct AttrSchema
    {
        std::string name;
        AttrType type = AttrType::String;
        bool required = false;
    };

    struct OpSchema
    {
        OpKind kind;
        std::string name;
        uint32_t minOperands = 0;
        uint32_t maxOperands = 0;
        uint32_t minResults = 0;
        uint32_t maxResults = 0;
        std::vector<AttrSchema> attrs;
    };

    class DialectRegistry
    {
    public:
        bool registerDialect(uint16_t id, std::string name);
        bool registerOp(OpSchema schema);
        const OpSchema *find(OpKind kind) const noexcept;
        std::optional<OpKind> find(std::string_view qualifiedName) const noexcept;
        std::string_view dialectName(uint16_t id) const noexcept;
        std::string_view opName(OpKind kind) const noexcept;
        std::vector<std::string> availableOps() const;

        bool validateOp(const Module &module, OpId op,
                        wolvrix::lib::diag::Diagnostics &diagnostics) const;

    private:
        std::vector<std::string> dialects_;
        std::vector<OpSchema> ops_;
    };

    const DialectRegistry &dialectRegistry();
    std::string_view genericOpcodeName(GenericOpcode opcode) noexcept;
    std::optional<GenericOpcode> parseGenericOpcode(std::string_view name) noexcept;

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_GENERIC_HPP
