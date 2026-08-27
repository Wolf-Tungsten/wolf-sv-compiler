#ifndef WOLVRIX_GRHSIM_IR_JSON_HPP
#define WOLVRIX_GRHSIM_IR_JSON_HPP

#include "grhsim/ir/module.hpp"

#include <string>
#include <string_view>

namespace wolvrix::lib::grhsim
{

    std::string storeJson(const Module &module, bool pretty = true);
    Module loadJson(std::string_view json);
    bool structurallyEquivalent(const Module &lhs, const Module &rhs);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_JSON_HPP
