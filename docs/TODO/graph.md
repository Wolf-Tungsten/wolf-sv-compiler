# wolf-sv-compiler 图结构

借鉴 MLIR SSA 形式的描述，融合 RTL 描述设计的特点。

# 自底向上的层次结构

- 顶点和边
    - 顶点：用 Operation 建模组合逻辑操作、寄存器、模块实例化
    - 边：用 Value 建模操作、寄存器、模块实例化之间的连接
- 图
    - 作为 Operation 和 Value 的容器，包含一组关联的顶点和边
    - 每个图建模一个 Verilog Module
- 网表
    - 一个网表中包含多个图
    - 存在一个或多个图作为顶层模块

# 顶点 Operation

- 具有一个枚举类型的 type 字段，表示 Operation 的类型，Operation 的类型是有限的
- 具有一个字符串类型的 symbol 字段，为 Operation 添加可索引的标签
- 具有一个 operands 数组，包含对 Value 的指针，记录 Operation 的操作数
- 具有一个 results 数组，包含对 Value 的指针，记录 Operation 的结果
- 具有一个 attributes 字典，key 为字符串类型，value 为 std::any，记录 Operation 的元数据。attributes 的 value 仅允许存储 `bool`、`int64_t`、`uint64_t`、`double`、`std::string`、`std::vector<std::any>`、`std::map<std::string, std::any>` 等 JSON 兼容类型，序列化时按照 JSON 语义写出，遇到不受支持的类型时抛错

## Operation 的粗粒度分类

- 常量定义
- 组合逻辑操作：算数、布尔、移位、多路复用器、信号切片读取、信号拼接
- 时序逻辑部件：reg、mem、mem_read_port、mem_write_port
- 层次结构部件：instance
- 调试结构部件：display，assert，dpic 等

# 边 Value

- 满足静态单赋值特性，只能由一个 Operation 写入，可以被多个 Operation 读取
- 具有一个字符串类型的 symbol 字段，用于识别信号
- 具有一个位宽字段 width
- 具有一个标记有无符号的布尔字段 isSigned
- 具有一个标记是否为模块输入的布尔字段 isInput
- 具有一个标记是否为模块输出的布尔字段 isOutput
- 具有一个 defineOp 指针，指向写入 Op，若 Value 是模块的输入参数，则为空指针
- 具有一个 userOps 数组，元素为 `<Operation*, size_t operandIndex>` 二元组，记录该 Value 在各 Operation 中作为第几个操作数被引用；同一 Operation 多次使用同一个 Value 时会存储多个条目

# 图 Graph

- 具有一个 moduleName 字段标识模块名称
- 具有一个 inputPorts 字段，类型为 <std::string，Value*> 的字典，记录所有输入端口
- 具有一个 outputPorts 字段，类型为 <std::string，Value*> 的字典，记录所有输出端口
- 具有一个 isTopModule bool 字段，标识是否为顶层模块
- 具有一个 isBlackBox bool 字段，标识是否为黑盒模块
- 具有一个 values 数组，保存所有 value 的指针。Graph 对 values 拥有所有权，在 Graph 析构时销毁全部 values，并维持插入顺序用于遍历
- 具有一个 ops 数组，保存所有 operation 的指针。Graph 对 ops 拥有所有权，在 Graph 析构时销毁全部 operation，并尽量保持与 values 一致的拓扑顺序

# 网表

- 具有一个可以按照 moduleName 索引全部 Graph 的容器，要求 moduleName 在网表中唯一
- 通过 Graph 的 isTopModule 字段标记顶层模块，至少存在一个顶层 Graph，且顶层 Graph 不允许被其他 Graph 的 instance 引用
- instance Operation 的 attributes 中记录被例化模块的 moduleName，运行时通过该索引解析被例化 Graph；禁止跨网表引用
- 允许存在未被引用的 Graph（如库模块），但综合或导出流程默认仅从顶层 Graph 开始遍历可达子图
- 层次结构由 Graph 内的 instance Operation 建模，网表自身不额外存储层次边
