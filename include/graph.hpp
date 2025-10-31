#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolf_sv {

enum class OperationKind {
  kConstant,

  kAdd,
  kSub,
  kMul,
  kDiv,
  kMod,
  kAnd,
  kOr,
  kXor,
  kNot,
  kShl,
  kLShr,
  kAShr,
  kMux,
  kSlice,
  kConcat,

  kRegister,
  kMemory,
  kMemoryReadPort,
  kMemoryWritePort,

  kInstance,

  kDisplay,
  kAssert,
  kDpic,
};

class Operation;

struct OperationUse {
  Operation *operation{};
  std::size_t operandIndex{};
};

class Value {
public:
  Value(std::string symbol, std::uint32_t width, bool isSigned);

  const std::string &symbol() const noexcept;
  void setSymbol(std::string symbol);

  std::uint32_t width() const noexcept;
  void setWidth(std::uint32_t width);

  bool isSigned() const noexcept;
  void setIsSigned(bool isSigned) noexcept;

  bool isInput() const noexcept;
  void setIsInput(bool isInput) noexcept;

  bool isOutput() const noexcept;
  void setIsOutput(bool isOutput) noexcept;

  Operation *defineOp() const noexcept;
  void setDefineOp(Operation *op) noexcept;

  const std::vector<OperationUse> &users() const noexcept;
  void addUser(Operation *operation, std::size_t operandIndex);

private:
  std::string symbol_;
  std::uint32_t width_{0};
  bool isSigned_{false};
  bool isInput_{false};
  bool isOutput_{false};
  Operation *defineOp_{nullptr};
  std::vector<OperationUse> userOps_;
};

class Operation {
public:
  using AttributeValue = std::any;
  using AttributeMap = std::unordered_map<std::string, AttributeValue>;

  Operation(OperationKind kind, std::string symbol = {});

  OperationKind kind() const noexcept;
  void setKind(OperationKind kind) noexcept;

  const std::string &symbol() const noexcept;
  void setSymbol(std::string symbol);

  const std::vector<Value *> &operands() const noexcept;
  void addOperand(Value *value);

  const std::vector<Value *> &results() const noexcept;
  void addResult(Value *value);

  const AttributeMap &attributes() const noexcept;

  void setAttribute(std::string key, AttributeValue value);
  void eraseAttribute(std::string_view key);
  AttributeValue *findAttribute(std::string_view key);
  const AttributeValue *findAttribute(std::string_view key) const;

  static bool isSupportedAttributeType(const AttributeValue &value);
  static void validateAttributeValue(const AttributeValue &value);

private:
  OperationKind kind_;
  std::string symbol_;
  std::vector<Value *> operands_;
  std::vector<Value *> results_;
  AttributeMap attributes_;
};

class Graph {
public:
  using PortMap = std::unordered_map<std::string, Value *>;

  explicit Graph(std::string moduleName);

  const std::string &moduleName() const noexcept;
  void setModuleName(std::string moduleName);

  bool isTopModule() const noexcept;
  void setIsTopModule(bool isTop) noexcept;

  bool isBlackBox() const noexcept;
  void setIsBlackBox(bool isBlackBox) noexcept;

  const PortMap &inputPorts() const noexcept;
  const PortMap &outputPorts() const noexcept;

  void addInputPort(std::string name, Value *value);
  void addOutputPort(std::string name, Value *value);

  Value *createValue(std::string symbol, std::uint32_t width, bool isSigned);
  Operation *createOperation(OperationKind kind, std::string symbol = {});

  const std::vector<std::unique_ptr<Value>> &values() const noexcept;
  const std::vector<std::unique_ptr<Operation>> &operations() const noexcept;

private:
  std::string moduleName_;
  PortMap inputPorts_;
  PortMap outputPorts_;
  bool isTopModule_{false};
  bool isBlackBox_{false};
  std::vector<std::unique_ptr<Value>> values_;
  std::vector<std::unique_ptr<Operation>> operations_;
};

class Netlist {
public:
  using GraphMap = std::unordered_map<std::string, std::unique_ptr<Graph>>;

  Graph &createGraph(std::string moduleName);
  Graph &emplaceGraph(std::unique_ptr<Graph> graph);

  Graph *getGraph(const std::string &moduleName) noexcept;
  const Graph *getGraph(const std::string &moduleName) const noexcept;

  const GraphMap &graphs() const noexcept;

private:
  GraphMap graphs_;
};

} // namespace wolf_sv
