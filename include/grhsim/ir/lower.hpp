#ifndef WOLVRIX_GRHSIM_IR_LOWER_HPP
#define WOLVRIX_GRHSIM_IR_LOWER_HPP

#include "core/grh.hpp"
#include "grhsim/ir/module.hpp"

#include <optional>
#include <string_view>

namespace wolvrix::lib::grhsim
{

    struct LowerGrhsimOptions
    {
        std::optional<std::string> top;
    };

    std::optional<Module> lowerGrhToGrhsim(
        const wolvrix::lib::grh::Graph &graph,
        wolvrix::lib::diag::Diagnostics &diagnostics);

    std::optional<Module> lowerGrhToGrhsim(
        const wolvrix::lib::grh::Design &design,
        const LowerGrhsimOptions &options,
        wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_LOWER_HPP
