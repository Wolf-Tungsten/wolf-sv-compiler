# wolf-sv-compiler 图结构

借鉴 MLIR SSA 形式的描述，融合 RTL 描述设计的特点。

# 自底向上的层次结构

- 顶点和边
    - 顶点：用 Operation 建模组合逻辑操作、寄存器、模块实例化
    - 边：用 Value 建模操作、寄存器、模块实例化之间的连接
- 图
    - 作为 Operation 和 Value 的容器，包含一组关联的顶点和边
    - 每个图建模一个 Verilog Module
- 电路
    - 一个电路中包含多个图
    - 存在一个或多个图作为顶层模块
    - 图之间通过特定类型的 Operation 建立联系

# 顶点 Operation

- 具有一个枚举类型的 type 字段，表示 Operation 的类型，Operation 的类型是有限的
- 具有一个字符串类型的 symbol 字段，为 Operation 添加可索引的标签
- 具有一个 operands 数组，包含对 Value 的指针，记录 Operation 的操作数
- 具有一个 results 数组，包含对 Value 的指针，记录 Operation 的结果
- 具有一个 attributes 字典，key 为字符串类型，value 为任意类型，记录 Operation 的元数据

## Operation 的粗粒度分类

- 常量定义
- 组合逻辑操作：算数、布尔、移位、多路复用器、信号切片读取、信号切片赋值、数组解引用赋值、数组解引用读取
- 时序逻辑部件：reg、mem、mem_read_port、mem_write_port
- 层次结构部件：instance
- 调试结构部件：display，assert，dpic 等

# 边 Value

- 