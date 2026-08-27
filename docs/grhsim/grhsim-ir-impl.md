# GRHSIM IR 实现说明

> 状态：新架构文档。本文定义 GRHSIM IR 的内存数据结构与操作方法（C++ 实
> 现约定），面向层次展开后的大规模图（目标 10⁶~10⁷ op、10⁷~10⁸ 边）。概念
> 与语义以 [GRHSIM IR](grhsim-ir.md) 为准；本文只规定其工程表示。代码位
> 置约定：`include/grhsim/`、`lib/grhsim/`，命名空间 `wolvrix::grhsim`。

## 1. 设计原则

大规模扁平图决定了一切表示选择：

- **稠密 ID**：所有实体以 32-bit 下标标识，存取即数组索引，无指针、无逐
  实体堆分配。
- **SoA + 池化**：实体字段按列存储（Structure-of-Arrays）；变长信息（操
  作数、结果、使用列表、属性）全部放入模块级池化数组，实体记录只存
  (offset, count)。
- **双模态**：可变态支持 pass 的增删改写；冻结态把池压缩为连续 CSR 布局，
  供遍历密集的只读阶段（分析、emit）。沿用 GRH 的 `freeze()` 惯例。
- **批量变更**：删除用墓碑标记，pass 结束时一次性 `compact()` 回收，避免
  中途搬移数组。

字节预算（指导值，非硬约束）：op 主体 ≤ 32 B/op，边主体 ≤ 16 B/edge；
10⁶ op、10⁷ 边时主体存储控制在数百 MB 以内。

## 2. 标识符、符号与类型

```cpp
// 稠密 ID：uint32_t 下标，0xFFFFFFFF 为 invalid
struct OpId     { uint32_t raw; };   // 操作顶点
struct EdgeId   { uint32_t raw; };   // 数据流边（op 结果值）
struct StateId  { uint32_t raw; };   // StateDecl 条目
struct HostId   { uint32_t raw; };   // HostTable 条目
struct RegionId { uint32_t raw; };   // 调度区域
struct TypeId   { uint32_t raw; };   // 类型表条目
struct SymbolId { uint32_t raw; };   // 符号表条目
```

- **符号表**：字符串 interning，惯例同 GRH（`intern`/`lookup`/内部符号生
  成）；所有名称（op、边、状态、host 条目、属性键、字符串属性值）一律
  以 SymbolId 存储，不重复存字符串。
- **类型表**：generic 与后端类型统一 intern 为 TypeId（类型系统见
  grhsim-ir.md 第 3 节）：
  ```cpp
  struct TypeRec {       // 按 kind 解释 payload
      uint8_t  track;    // generic | backend
      uint8_t  kind;     // logic / array / real / string / 后端自定义
      uint16_t dialect;  // backend 类型所属方言，generic 为 0
      uint32_t payload;  // 类型参数池偏移（width、sign、row、布局描述等）
  };
  ```
- **OpKind**：`(dialect, opcode)` 打包为一个 uint32_t（高 16 位方言号、低
  16 位操作码）；方言号模块级注册表管理，generic 恒为 0。

## 3. 数据流图（V, E）

### 3.1 逻辑记录

```cpp
struct OpRec {              // 物理上按 SoA 分列存储
    OpKind   kind;
    SymbolId sym;           // 可 invalid（匿名 op）
    uint32_t operands;      // operandPool 偏移
    uint32_t results;       // resultPool 偏移
    uint16_t nOperands;
    uint16_t nResults;
    RegionId region;        // 未调度 = invalid
    uint32_t attrs;         // attrPool 偏移，无属性 = invalid
};

struct EdgeRec {            // 数据流边 = 某 op 的一个结果
    TypeId   type;          // 两轨之一（产生 op 的方言决定）
    OpId     def;           // 定义它的 op
    uint32_t uses;          // usePool 偏移
    uint32_t nUses;
    SymbolId sym;           // 可 invalid
};

struct Use {
    OpId     user;          // 使用方 op
    uint32_t operandIndex;  // 在其操作数列表中的位置
};
```

### 3.2 池化存储

模块级池：`operandPool: vector<EdgeId>`、`resultPool: vector<EdgeId>`、
`usePool: vector<Use>`、`attrPool: vector<AttrKV>`、各类参数池。op/边的变
长部分都是池中的连续段，遍历时不发生间接跳转以外的分配。

- op/边记录本体各在一个连续数组中；删除打墓碑（kind 置 invalid 档），
  池段随之成为空洞。
- `compact()`：压紧记录数组与池，重映射全部 ID；仅在 pass 边界调用。
- 冻结态：池段按调度顺序重排为 CSR，`operands(op)` 等访问为纯偏移读取。

### 3.3 use 列表

use 列表可变期随改写增量维护（pass 依赖 `replaceAllUses` 工作）；构建或
大修后可由 `rebuildUses()` 全量重建（O(|E|)）。冻结态下 use 段同样连续
化。

## 4. StateDecl、TrackSet、HostTable

```cpp
struct StateEntry {
    SymbolId name;
    uint8_t  kind;          // in / out / state（三个名字空间）
    TypeId   genType;       // generic 类型，恒定
    TypeId   backendType;   // invalid = 未特化（平凡布局）
    uint32_t init;          // init 记录池偏移，无 = invalid
};

struct HostEntry {
    SymbolId entry;
    uint8_t  kind;          // query / effect
    uint32_t signature;     // 签名池偏移（参数类型、方向序列）
    SymbolId binding;       // 宿主符号
};
```

- StateDecl 是 `vector<StateEntry>` + 名称到 StateId 的符号索引。
- TrackSet 不持久化：由 `deriveTrackSet()` 从图上 op 的 events 属性与区域
  激活条件汇总为 StateId 位集（bitset），供验证与后端使用。

## 5. Schedule

```cpp
struct RegionRec {
    uint32_t ops;        // regionOpPool 偏移：区域内 op 全序（OpId 段）
    uint32_t nOps;
    uint32_t activation; // 激活条件记录：恒真 | (StateId, posedge/negedge)
    uint32_t deps;       // regionDepPool 偏移：区域依赖边（出边）
    uint32_t nDeps;
};
```

- 区域归属存于 op 记录的 `region` 字段（O(1) 查询）。
- 区域内顺序 = `regionOpPool` 段的顺序；区域间偏序 = 区域依赖图
  （RegionId 为节点的 DAG，邻接出边池化存储）。
- 未调度模块：`region` 全为 invalid、无 RegionRec；调度 pass 建立区域与
  顺序后，emitter 只消费本结构（GRHSIM IR 第 6 节）。
- `linearize()`：把偏序展平为一个全序（OpId 排列），供串行 emit；并行
  emit 直接按区域依赖图分层取区域。

## 6. 模块与操作方法

```cpp
class Module {
    SymbolTable symbols;
    TypeTable   types;
    // SoA：OpRec 各列 + EdgeRec 各列
    // 池：operandPool / resultPool / usePool / attrPool / …
    vector<StateEntry> states;
    vector<HostEntry>  hosts;
    vector<RegionRec>  regions;
    bool               frozen;
};
```

API 分组（与 GRHSIM IR 第 4 节的三类图操作对应）：

**查询与遍历**（只读；冻结态下为 CSR 快路径）

```cpp
OpId/EdgeId 范围:        m.ops() / m.edges() / m.states() / m.regions()
op 访问:                 m.kind(op) / m.operands(op) / m.results(op) / m.attrs(op)
use-def:                 m.def(edge) / m.users(edge)
按名查询:                m.findState("clk") / m.lookupSymbol(...)
调度访问:                m.regionOf(op) / m.regionOps(region) / m.regionDeps(region)
```

**子图替换原语**（pass 使用；自动维护 use 列表）

```cpp
OpId   newOp = m.createOp(kind, sym);          // 结果为后续 addResult
void   m.setOperands(op, operands);            // 整段替换
EdgeId m.addResult(op, type, sym);
void   m.replaceAllUses(oldEdge, newEdge);
void   m.eraseOp(op);                          // 墓碑，defer 到 compact()
```

**图划分原语**

```cpp
RegionId r = m.createRegion(activation);
void     m.setRegion(op, r);                   // 批量入口 setRegion(ops, r)
void     m.mergeRegions(dst, src);
```

**调度原语**

```cpp
void m.setRegionOrder(region, opOrder);        // 区域内全序
void m.addRegionDep(u, v);                     // 区域间偏序边
void m.linearize();                            // 展平（可选）
```

**验证与维护**

```cpp
void m.validate();   // 类型/驱动唯一/区域合法性（SCC 不切开）/调度与依赖一致
void m.freeze();     // 池 CSR 化，只读；任何写操作自动解冻
void m.compact();    // 回收墓碑与池空洞，重映射 ID
```

错误处理沿用仓库惯例：API 热路径不抛异常，非法输入进入
`PassDiagnostics`；`validate()` 汇总诊断。

## 7. 大规模图工程要点

- **遍历顺序**：分析与 emit 一律按 `regionOpPool`/`linearize()` 的顺序数组
  遍历，不按 ID 顺序——前者在 CSR 化后是连续的，cache 行为可预期。
- **批量改写模式**：pass 内只做墓碑与追加；pass 边界由驱动器统一
  `compact()`。禁止 pass 内逐条搬移数组。
- **use 列表开销**：use 条目 8 B；对扇出巨大的边（时钟、复位）这是主要
  内存项，预算时按 `8 × |E|` 估。
- **符号/类型去重**：全部 intern，比较即 ID 比较；禁止在热路径比较字符
  串。
- **并行**：emit 与分析可按区域依赖图分层并行（区域是串行单位，层间无
  依赖）；共享结构只读，产物按区域汇集。
- **ID 重映射只发生在 compact/freeze**：pass 运行期间 OpId/EdgeId 稳定，
  分析结果可用 ID 作键缓存。

## 8. 本文不定义的内容

- 序列化格式（大规模图不宜 JSON；二进制 dump/load 由后续文档定义）。
- 具体 pass 列表与驱动器组织（[Pass 系统与流水线](grhsim-ir-pipeline.md)）。
- C++ 详细签名与异常/诊断细则（以头文件为准）。
