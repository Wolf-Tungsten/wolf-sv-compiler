# Elaborate

wolf-sv-compiler 的 elaborate 过程将 slang 的 AST 转换为 wolf_sv::Netlist

class Elaborate 完成上述建图转换过程，管理过程中所需的数据结构，支持线程安全

class Elaborate 的 convert 方法接收 slang 的 auto &root = compilation->getRoot() 作为入参，返回一个 wolf_sv::Netlist*

## 从顶层模块开始

在 elaborate 的 convert 中添加逻辑：

- 在 slang AST 中查找所有顶层模块
- 对于每个顶层模块
    - 打印名称
    - 解析端口数据类型，打印端口数据类型

TODO：创建一个 processTopModule 函数，将 convert 中循环体的内容封装起来，并修改 convert 调用 processTopModule