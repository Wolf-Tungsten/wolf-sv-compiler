#include <iostream>
#include <optional>
#include <string>

#include "slang/ast/ASTSerializer.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"
#include "slang/text/Json.h"

#include "elaborate.hpp"

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
    driver.options.compilationFlags[slang::ast::CompilationFlags::AllowTopLevelIfacePorts] = true;
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

    driver.runAnalysis(*compilation);

    auto &root = compilation->getRoot();

    if (dumpAst == true) {
        std::cout << "=== AST JSON ===\n";

        slang::JsonWriter writer;
        writer.setPrettyPrint(true);

        slang::ast::ASTSerializer serializer(*compilation, writer);
        serializer.serialize(root);
        writer.writeNewLine();

        std::cout << writer.view();
    }

    wolf_sv::Elaborate elaborator;
    auto netlist = elaborator.convert(root);
    (void)netlist;

    bool ok = driver.reportDiagnostics(/* quiet */ false);
    return ok ? 0 : 4;
}
