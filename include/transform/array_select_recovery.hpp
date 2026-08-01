#ifndef WOLVRIX_TRANSFORM_ARRAY_SELECT_RECOVERY_HPP
#define WOLVRIX_TRANSFORM_ARRAY_SELECT_RECOVERY_HPP

#include "core/transform.hpp"

#include <cstddef>

namespace wolvrix::lib::transform
{

    struct ArraySelectRecoveryOptions
    {
        // rewrite=false: census mode, only count matches and skip reasons
        // without modifying the graph.
        bool rewrite = true;
        // Minimum number of entries (rows) for a register family to be
        // recovered into a kMemory.
        std::size_t minEntries = 4;
    };

    class ArraySelectRecoveryPass : public Pass
    {
    public:
        ArraySelectRecoveryPass();
        explicit ArraySelectRecoveryPass(ArraySelectRecoveryOptions options);

        PassResult run() override;

    private:
        ArraySelectRecoveryOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_ARRAY_SELECT_RECOVERY_HPP
