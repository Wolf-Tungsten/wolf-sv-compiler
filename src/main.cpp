#include <iostream>
#include "slang/driver/Driver.h"

using namespace slang::driver;

int main(int argc, char** argv) {
    // Create a driver instance
    Driver driver;
    driver.addStandardArgs();

    // Parse command line arguments
    if (!driver.parseCommandLine(argc, argv)) {
        return 1;
    }

    // Process options
    if (!driver.processOptions()) {
        return 2;
    }

    // Parse all source files
    if (!driver.parseAllSources()) {
        return 3;
    }

    // Run full compilation (create compilation, elaborate, analyze, and report)
    bool ok = driver.runFullCompilation();
    return ok ? 0 : 4;
}
