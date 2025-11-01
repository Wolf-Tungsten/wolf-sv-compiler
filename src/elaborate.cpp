#include "elaborate.hpp"

#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "graph.hpp"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/types/Type.h"

namespace wolf_sv {

Elaborate::Elaborate() = default;
Elaborate::~Elaborate() = default;

std::shared_ptr<Netlist> Elaborate::convert(const slang::ast::RootSymbol& root) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto netlist = std::make_shared<Netlist>();
  latestNetlist_ = netlist;

  for (const auto* instance : root.topInstances) {
    if (!instance) {
      continue;
    }
    processTopModule(*instance);
  }

  return netlist;
}

void Elaborate::processTopModule(const slang::ast::InstanceSymbol& instance) {
  if (!instance.isModule()) {
    return;
  }

  std::cout << "Top module: " << instance.name << '\n';

  const auto& portList = instance.body.getPortList();
  for (const auto* portSymbol : portList) {
    if (!portSymbol) {
      continue;
    }

    switch (portSymbol->kind) {
      case slang::ast::SymbolKind::Port: {
        const auto* port = static_cast<const slang::ast::PortSymbol*>(portSymbol);
        std::cout << "  port " << port->name << ": " << port->getType().toString() << '\n';
        break;
      }
      case slang::ast::SymbolKind::MultiPort: {
        const auto* multiPort = static_cast<const slang::ast::MultiPortSymbol*>(portSymbol);
        throw std::runtime_error("multi-port ports are unsupported: " +
                                 std::string(multiPort->name));
      }
      case slang::ast::SymbolKind::InterfacePort: {
        const auto* ifacePort = static_cast<const slang::ast::InterfacePortSymbol*>(portSymbol);
        std::string typeDesc;
        if (ifacePort->interfaceDef) {
          typeDesc.append(ifacePort->interfaceDef->name);
          if (!ifacePort->modport.empty()) {
            typeDesc.push_back('.');
            typeDesc.append(ifacePort->modport);
          }
        }
        else if (ifacePort->isGeneric) {
          typeDesc = "generic";
        }
        else {
          typeDesc = "<invalid>";
        }

        std::cout << "  port " << ifacePort->name << ": interface " << typeDesc << '\n';
        break;
      }
      default:
        std::cout << "  port " << portSymbol->name << ": <unsupported port kind>\n";
        break;
    }
  }
}

}  // namespace wolf_sv
