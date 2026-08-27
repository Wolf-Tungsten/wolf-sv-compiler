#ifndef WOLVRIX_GRHSIM_PASS_SCHEDULE_TOPO_HPP
#define WOLVRIX_GRHSIM_PASS_SCHEDULE_TOPO_HPP

#include "grhsim/ir/pass.hpp"

#include <memory>

namespace wolvrix::lib::grhsim
{

    std::unique_ptr<SimPass> makeScheduleTopoPass();

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_PASS_SCHEDULE_TOPO_HPP
