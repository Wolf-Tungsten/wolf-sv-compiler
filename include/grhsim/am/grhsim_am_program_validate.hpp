#ifndef WOLVRIX_GRHSIM_AM_GRHSIM_AM_PROGRAM_VALIDATE_HPP
#define WOLVRIX_GRHSIM_AM_GRHSIM_AM_PROGRAM_VALIDATE_HPP

#include "grhsim/am/grhsim_am_program.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim::am
{

    enum class ValidationLevel
    {
        Structural,
        Semantic,
    };

    struct ValidationOptions
    {
        ValidationLevel level = ValidationLevel::Structural;
        std::size_t maxErrors = 32;
    };

    struct ValidationResult
    {
        std::vector<std::string> errors;

        bool success() const noexcept { return errors.empty(); }
    };

    ValidationResult validate(const LinearProgram &program,
                              const ValidationOptions &options = {});
    // Linear-program checks over a bare view (used by the AmGraph path, where
    // the graph storage stands in for the linear program).
    ValidationResult validate(ProgramView program,
                              const ValidationOptions &options = {});
    ValidationResult validate(const ScheduledProgram &program,
                              const ValidationOptions &options = {});

} // namespace wolvrix::lib::grhsim::am

#endif // WOLVRIX_GRHSIM_AM_GRHSIM_AM_PROGRAM_VALIDATE_HPP
