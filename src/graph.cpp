#include "graph.hpp"

#include <stdexcept>

namespace wolf_sv {

Value::Value(std::string symbol, std::uint32_t width, bool isSigned)
    : symbol_(std::move(symbol)), width_(width), isSigned_(isSigned) {}

const std::string &Value::symbol() const noexcept { return symbol_; }

void Value::setSymbol(std::string symbol) { symbol_ = std::move(symbol); }

std::uint32_t Value::width() const noexcept { return width_; }

void Value::setWidth(std::uint32_t width) { width_ = width; }

bool Value::isSigned() const noexcept { return isSigned_; }

void Value::setIsSigned(bool isSigned) noexcept { isSigned_ = isSigned; }

bool Value::isInput() const noexcept { return isInput_; }

void Value::setIsInput(bool isInput) noexcept { isInput_ = isInput; }

bool Value::isOutput() const noexcept { return isOutput_; }

void Value::setIsOutput(bool isOutput) noexcept { isOutput_ = isOutput; }

Operation *Value::defineOp() const noexcept { return defineOp_; }

void Value::setDefineOp(Operation *op) noexcept { defineOp_ = op; }

const std::vector<OperationUse> &Value::users() const noexcept {
  return userOps_;
}

void Value::addUser(Operation *operation, std::size_t operandIndex) {
  if (operation == nullptr) {
    throw std::invalid_argument("user operation pointer must not be null");
  }
  userOps_.push_back(OperationUse{operation, operandIndex});
}

Operation::Operation(OperationKind kind, std::string symbol)
    : kind_(kind), symbol_(std::move(symbol)) {}

OperationKind Operation::kind() const noexcept { return kind_; }

void Operation::setKind(OperationKind kind) noexcept { kind_ = kind; }

const std::string &Operation::symbol() const noexcept { return symbol_; }

void Operation::setSymbol(std::string symbol) { symbol_ = std::move(symbol); }

const std::vector<Value *> &Operation::operands() const noexcept {
  return operands_;
}

void Operation::addOperand(Value *value) {
  if (value == nullptr) {
    throw std::invalid_argument("operand value pointer must not be null");
  }
  const std::size_t index = operands_.size();
  operands_.push_back(value);
  value->addUser(this, index);
}

const std::vector<Value *> &Operation::results() const noexcept {
  return results_;
}

void Operation::addResult(Value *value) {
  if (value == nullptr) {
    throw std::invalid_argument("result value pointer must not be null");
  }
  results_.push_back(value);
  value->setDefineOp(this);
}

const Operation::AttributeMap &Operation::attributes() const noexcept {
  return attributes_;
}

void Operation::setAttribute(std::string key, AttributeValue value) {
  if (key.empty()) {
    throw std::invalid_argument("attribute key must not be empty");
  }
  validateAttributeValue(value);
  attributes_.insert_or_assign(std::move(key), std::move(value));
}

void Operation::eraseAttribute(std::string_view key) {
  attributes_.erase(std::string(key));
}

Operation::AttributeValue *Operation::findAttribute(std::string_view key) {
  auto it = attributes_.find(std::string(key));
  return it == attributes_.end() ? nullptr : &it->second;
}

const Operation::AttributeValue *
Operation::findAttribute(std::string_view key) const {
  auto it = attributes_.find(std::string(key));
  return it == attributes_.end() ? nullptr : &it->second;
}

bool Operation::isSupportedAttributeType(const AttributeValue &value) {
  if (!value.has_value()) {
    return false;
  }
  if (value.type() == typeid(bool) || value.type() == typeid(std::int64_t) ||
      value.type() == typeid(std::uint64_t) || value.type() == typeid(double) ||
      value.type() == typeid(std::string)) {
    return true;
  }
  if (const auto *vectorValue =
          std::any_cast<const std::vector<std::any>>(&value)) {
    for (const auto &element : *vectorValue) {
      if (!isSupportedAttributeType(element)) {
        return false;
      }
    }
    return true;
  }
  if (const auto *mapValue =
          std::any_cast<const std::map<std::string, std::any>>(&value)) {
    for (const auto &pair : *mapValue) {
      if (!isSupportedAttributeType(pair.second)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

void Operation::validateAttributeValue(const AttributeValue &value) {
  if (!isSupportedAttributeType(value)) {
    throw std::invalid_argument(
        "attribute value must be JSON-compatible (bool, int64_t, uint64_t, "
        "double, string, vector<any>, map<string, any>)");
  }
}

Graph::Graph(std::string moduleName) : moduleName_(std::move(moduleName)) {}

const std::string &Graph::moduleName() const noexcept { return moduleName_; }

void Graph::setModuleName(std::string moduleName) {
  moduleName_ = std::move(moduleName);
}

bool Graph::isTopModule() const noexcept { return isTopModule_; }

void Graph::setIsTopModule(bool isTop) noexcept { isTopModule_ = isTop; }

bool Graph::isBlackBox() const noexcept { return isBlackBox_; }

void Graph::setIsBlackBox(bool isBlackBox) noexcept {
  isBlackBox_ = isBlackBox;
}

const Graph::PortMap &Graph::inputPorts() const noexcept { return inputPorts_; }

const Graph::PortMap &Graph::outputPorts() const noexcept {
  return outputPorts_;
}

void Graph::addInputPort(std::string name, Value *value) {
  if (value == nullptr) {
    throw std::invalid_argument("input port value pointer must not be null");
  }
  value->setIsInput(true);
  inputPorts_.insert_or_assign(std::move(name), value);
}

void Graph::addOutputPort(std::string name, Value *value) {
  if (value == nullptr) {
    throw std::invalid_argument("output port value pointer must not be null");
  }
  value->setIsOutput(true);
  outputPorts_.insert_or_assign(std::move(name), value);
}

Value *Graph::createValue(std::string symbol, std::uint32_t width,
                          bool isSigned) {
  auto value = std::make_unique<Value>(std::move(symbol), width, isSigned);
  Value *raw = value.get();
  values_.push_back(std::move(value));
  return raw;
}

Operation *Graph::createOperation(OperationKind kind, std::string symbol) {
  auto operation = std::make_unique<Operation>(kind, std::move(symbol));
  Operation *raw = operation.get();
  operations_.push_back(std::move(operation));
  return raw;
}

const std::vector<std::unique_ptr<Value>> &Graph::values() const noexcept {
  return values_;
}

const std::vector<std::unique_ptr<Operation>> &
Graph::operations() const noexcept {
  return operations_;
}

Graph &Netlist::createGraph(std::string moduleName) {
  auto graph = std::make_unique<Graph>(std::move(moduleName));
  Graph &ref = *graph;
  emplaceGraph(std::move(graph));
  return ref;
}

Graph &Netlist::emplaceGraph(std::unique_ptr<Graph> graph) {
  if (graph == nullptr) {
    throw std::invalid_argument("graph pointer must not be null");
  }
  const std::string moduleName = graph->moduleName();
  auto [it, inserted] = graphs_.emplace(moduleName, std::move(graph));
  if (!inserted) {
    throw std::invalid_argument(
        "graph with the same module name already exists in netlist");
  }
  return *it->second;
}

Graph *Netlist::getGraph(const std::string &moduleName) noexcept {
  auto it = graphs_.find(moduleName);
  return it == graphs_.end() ? nullptr : it->second.get();
}

const Graph *Netlist::getGraph(const std::string &moduleName) const noexcept {
  auto it = graphs_.find(moduleName);
  return it == graphs_.end() ? nullptr : it->second.get();
}

const Netlist::GraphMap &Netlist::graphs() const noexcept { return graphs_; }

} // namespace wolf_sv
