#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    class OnehotToMuxPass : public Pass
    {
    public:
        OnehotToMuxPass();

        PassResult run() override;
    };

} // namespace wolvrix::lib::transform
