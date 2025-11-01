#pragma once

#include <memory>
#include <mutex>

namespace slang::ast {
class RootSymbol;
class InstanceSymbol;
}  // namespace slang::ast

namespace wolf_sv {

class Netlist;

class Elaborate {
public:
  Elaborate();
  ~Elaborate();

  Elaborate(const Elaborate&) = delete;
  Elaborate& operator=(const Elaborate&) = delete;

  std::shared_ptr<Netlist> convert(const slang::ast::RootSymbol& root);

private:
  std::mutex mutex_;
  std::weak_ptr<Netlist> latestNetlist_;

  void processTopModule(const slang::ast::InstanceSymbol& instance);
};

}  // namespace wolf_sv
