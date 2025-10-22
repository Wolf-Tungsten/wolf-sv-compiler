#include <iostream>
#include <optional>
#include <string>

#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"

using namespace slang::driver;

namespace
{

    void dumpScope(const slang::ast::Scope &scope, int indent = 0)
    {
        const std::string indentStr(static_cast<size_t>(indent) * 2, ' ');
        for (const slang::ast::Symbol &symbol : scope.members())
        {
            std::cout << indentStr << "- [" << slang::ast::toString(symbol.kind) << "] ";
            if (symbol.name.empty())
            {
                std::cout << "<anonymous>";
            }
            else
            {
                std::cout << symbol.name;
            }
            std::cout << '\n';

            if (const auto *instance = symbol.as_if<slang::ast::InstanceSymbol>())
            {
                dumpScope(instance->body, indent + 1);
            }
            else if (symbol.isScope())
            {
                dumpScope(symbol.as<slang::ast::Scope>(), indent + 1);
            }
        }
    }

} // namespace

int main(int argc, char **argv)
{
    Driver driver;
    driver.addStandardArgs();

    std::optional<bool> dumpAst;
    driver.cmdLine.add("--dump-ast", dumpAst, "Dump a summary of the elaborated AST");

    if (!driver.parseCommandLine(argc, argv)) {
        return 1;
    }

    if (!driver.processOptions()) {
        return 2;
    }

    if (!driver.parseAllSources()) {
        return 3;
    }

    auto compilation = driver.createCompilation();
    driver.reportCompilation(*compilation, /* quiet */ false);

    auto &root = compilation->getRoot();

    driver.runAnalysis(*compilation);

    std::cout << "=== AST summary ===\n";
    std::cout << "Top-level instances: " << root.topInstances.size() << '\n';
    std::cout << "Compilation units : " << root.compilationUnits.size() << '\n';
    dumpScope(root);

    bool ok = driver.reportDiagnostics(/* quiet */ false);
    return ok ? 0 : 4;
}
