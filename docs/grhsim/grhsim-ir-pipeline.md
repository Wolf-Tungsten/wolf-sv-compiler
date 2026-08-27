# GRHSIM IR Pass 系统与流水线

> 状态：新架构文档。本文定义 GRHSIM IR 的 pass 系统：pass 接口、管理器、注
> 册表与 Python 编排。机制沿用 GRH IR 的 transform 体系
> （`core/transform.hpp`、`pybind Session`），差别只是作用对象从 `Design`
> 换成 `grhsim::Module`。pass 内的图操作规则见
> [GRHSIM IR 实现说明](grhsim-ir-impl.md)。

## 1. 与 GRH IR pass 体系的关系

同一套机制：`Pass` / `PassManager` / 名称注册表 / Python Session 编排。与
GRH IR 的差别：

- pass 作用于 `grhsim::Module`（单一扁平图），不是 `Design`；
- pass 内的增删改写只能走 `Module` 的三类图操作原语（实现说明第 6 节），
  不自管理存储；
- pass 运行期间 OpId/EdgeId 稳定，`compact()`/`freeze()` 由驱动器在 pass
  边界统一执行，pass 内禁止调用（实现说明第 7 节）。

## 2. Pass 接口

```cpp
class SimPass {
public:
    virtual ~SimPass() = default;
    virtual SimPassResult run(Module &module, SimPassContext &ctx) = 0;

    const std::string &id() const;          // 唯一 id，kebab-case
    const std::string &name() const;
    const std::string &description() const;
    SimPassEffects effects() const;         // 见下
};

struct SimPassResult {
    bool changed = false;
    bool failed = false;
    std::vector<std::string> artifacts;
};

struct SimPassEffects {
    bool mutatesGraph = true;        // false = 分析 pass，驱动器跳过 compact
    bool preservesSchedule = true;   // false = 改写后已有 Schedule 失效
};
```

`SimPassContext` 提供：诊断（沿用 `PassDiagnostics` 惯例）、日志、session
存取（跨 pass 共享值，同 GRH 的 `SessionStore` 机制）。

**Schedule 有效性契约**：pass 以 `preservesSchedule` 声明改写是否保持已有
调度信息有效。驱动器规则：若 pass 不保持而模块已调度，则清除 Schedule
（模块回到未调度状态）并给出诊断信息——绝不允许带着失效调度继续运行。

## 3. PassManager（驱动器）

```cpp
class SimPassManager {
public:
    void addPass(std::unique_ptr<SimPass> pass);
    SimPipelineResult run(Module &module);
    // options: stopOnError / emitTiming / verbosity / logSink / session
};
```

驱动器职责，pass 不负责：

- pass 间按需 `compact()`（有墓碑且下一 pass 要求稠密 ID 时）与
  freeze/unfreeze 切换；
- 按 `effects()` 维护 Schedule 合法性（第 2 节）；
- 逐 pass 计时、诊断汇总；`stopOnError` 语义同 GRH。

## 4. 注册表

与 GRH 的 `makePass` 同款：

```cpp
std::unique_ptr<SimPass> makeSimPass(std::string_view name,
                                     std::span<const std::string_view> args,
                                     std::string &error);
std::vector<std::string> availableSimPasses();
```

pass 参数为字符串数组；Python 侧负责把命名参数编译为 args（同 GRH 的
`_compile_*_kwargs` 惯例）。

## 5. Python 编排

GRHSIM IR 模块作为 Session 的一等对象，与 design 并列存放（独立命名空间，
键不冲突）：

```python
import wolvrix

s = wolvrix.Session()
s.read_sv("dut.sv", design="dut")
s.lower_grhsim(design="dut", module="sim")      # GRH → GRHSIM IR 映射

# 编排即脚本：顺序、重复、条件分支全由 Python 表达
s.run_sim_pass("fold-const", module="sim")
s.run_sim_pass("rewrite-array-views", module="sim")
s.run_sim_pass("partition-activity", module="sim")
s.run_sim_pass("schedule", module="sim")
s.emit_grhsim(module="sim", backend="cpu", out="sim.cpp")
```

- `run_sim_pass(name, module=..., args=[...], dryrun=False, log_level=...)`：
  参数与体验同 GRH 的 `run_pass`；`dryrun=True` 时克隆 module 试跑。
- `list_sim_passes()` 列出已注册 pass。
- **流水线不进 C++**：C++ 只提供原子 pass 与管理器；默认流水线（如 CPU
  后端的推荐顺序）只是库中的一个 Python 函数，用户可照抄改写。

## 6. Pass 命名与分类

pass id 为 kebab-case，按前缀分类（与 GRHSIM IR 第 4 节的三类图操作对应）：

| 前缀 | 类别 | 说明 |
| --- | --- | --- |
| `fold-*`、`rewrite-*`、`lower-*` | 子图替换 | 含方言下降（`lower-cpu-*` 等） |
| `partition-*` | 图划分 | 建立/调整区域 |
| `schedule-*` | 顺序调度 | 区域内序与区域间偏序 |
| `analyze-*` | 只读分析 | `mutatesGraph = false` |

分析产物优先物化为 IR 注解（op 属性、StateDecl、Schedule），不放 session——
信息应表达在 IR 上（GRHSIM IR 的设计原则）；session 只放不便物化的临时产
物。

## 7. 本文不定义的内容

- 具体 pass 清单（随实现滚动，以 `list_sim_passes()` 为准）。
- GRH → GRHSIM IR 映射（`lower_grhsim`）的细则（操作对应见 generic 方言文
  档第 3 节）。
- emit 后端与构建流程（后端方言文档）。
