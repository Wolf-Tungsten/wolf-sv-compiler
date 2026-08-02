#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    class ArrayLowerPass : public Pass
    {
    public:
        ArrayLowerPass();

        PassResult run() override;
    };

} // namespace wolvrix::lib::transform
