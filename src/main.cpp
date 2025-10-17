#include <iostream>
#include <vector>
#include <string>
#include "slang/driver/Driver.h"

using namespace slang;
using namespace slang::driver;

int main(int argc, char** argv) {
    // Create a driver instance
    Driver driver;
    driver.addStandardArgs();

    // Parse command line arguments
    if (!driver.parseCommandLine(argc, argv)) {
        return 1;
    }

    // Run the compilation
    if (!driver.processOptions()) {
        return 2;
    }

    bool success = driver.parseAllSources();

    if (!success) {
        std::cerr << "Compilation failed" << std::endl;
        return 3;
    }

    auto compilation = driver.createCompilation();
    if (!compilation) {
        std::cerr << "Failed to create compilation" << std::endl;
        return 4;
    }

    // Report any diagnostics
    auto& diags = compilation->getAllDiagnostics();
    for (auto& diag : diags) {
        std::cout << driver.reportDiagnostic(diag);
    }

    // Check for errors
    if (!diags.empty()) {
        bool hasErrors = false;
        for (auto& diag : diags) {
            if (diag.isError()) {
                hasErrors = true;
                break;
            }
        }
        if (hasErrors) {
            std::cerr << "Compilation completed with errors" << std::endl;
            return 5;
        }
    }

    std::cout << "Compilation successful" << std::endl;
    return 0;
}
