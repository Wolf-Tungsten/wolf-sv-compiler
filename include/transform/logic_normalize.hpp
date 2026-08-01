#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    class LogicNormalizePass : public Pass
    {
    public:
        LogicNormalizePass();

        PassResult run() override;
    };

} // namespace wolvrix::lib::transform
