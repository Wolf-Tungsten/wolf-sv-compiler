#ifndef WOLVRIX_GRHSIM_EMIT_CPU_SINGLE_THREAD_HPP
#define WOLVRIX_GRHSIM_EMIT_CPU_SINGLE_THREAD_HPP

#include "core/diagnostics.hpp"
#include "grhsim/ir/module.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{

    struct CpuSingleThreadEmitOptions
    {
        std::filesystem::path outputDirectory;
        std::size_t opsPerSourceFile = 50000;
        uint32_t fixedPointIterationLimit = 100;
    };

    struct CpuSingleThreadEmitResult
    {
        bool success = false;
        std::vector<std::string> artifacts;
    };

    CpuSingleThreadEmitResult emitCpuSingleThread(
        const Module &module,
        const CpuSingleThreadEmitOptions &options,
        wolvrix::lib::diag::Diagnostics &diagnostics);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_EMIT_CPU_SINGLE_THREAD_HPP
