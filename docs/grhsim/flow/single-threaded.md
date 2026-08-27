# grhsim 单线程方案（活动度调度路线）

> 定位：CPU 后端 / 单线程串行 eval / 当前默认方案
> 状态：阶段 1 最小实现与正确性验收已完成；HDLBits 162/162，XiangShan
> CoreMark + NEMU 连续对拍至 cycle 39,250 无 mismatch；入口：
> `wolvrix.pipelines.cpu_single_thread()`
> （Python 编排函数，见 [Pass 系统与流水线](../grhsim-ir-pipeline.md) 第 5 节）
>
> 本方案是现有活动度调度实现在新架构下的重组：compute/commit node、
> activity bit、supernode coarsen 等概念的出处见迁移前文档
> [activity-schedule](../../transform/activity-schedule.md) 与
> [GrhSIM 当前调度方法](../../emit/grhsim-scheduling.md)；差别是调度与激
> 活决策全部从 emitter 中取出，物化为显式 pass 与 Schedule。

## 1. 目标与思路

把仿真模型的 eval 实现为单线程串行循环：一轮按调度顺序执行各区域，迭代
至不动点。激活方式分两类：

- **恒真区域**（compute）：每轮都执行，承载组合计算与状态读；
- **边缘激活区域**（commit）：以（信号，边沿）为激活条件，承载状态写出
  与副作用调用，未激活时整片跳过（GRHSIM IR 4.2 节）。

全部调度决策在编译期完成并物化在 Schedule 中；运行时只执行，不做调度决
策。

## 2. pass 编排

| # | pass | 类别 | 当前状态 | 作用 |
| --- | --- | --- | --- | --- |
| 1 | `lower_grhsim` | 映射 | 阶段 1 已实现 | GRH → `generic.*` 逐操作直译（层次展开、XMR 解析已在 GRH 侧完成） |
| 2 | `fold-const` | rewrite | 阶段 2 | 常量折叠 |
| 3 | `dead-code-elim` | rewrite | 阶段 2 | 死代码消除 |
| 4 | `rewrite-array-views` | rewrite | 阶段 2 | 产生整数组 / packed 视图批量操作（迁自 GRH 侧的对应 pass，见 [generic 方言](../dialect/generic.md) 3.4 节） |
| 5 | `specialize-storage-cpu` | rewrite | 阶段 2 | 存储特化：两态编码、机器字打包（填写 StateDecl `backendType`） |
| 6 | `lower-cpu-*` | rewrite | 阶段 2 | 方言下降 `generic.*` → `cpu.*` |
| 7 | `partition-activity` | partition | 阶段 2 | 活动度划分：恒真 compute 区域 + 按（信号，边沿）的 commit 区域；SCC 不切开 |
| 8 | `schedule-topo` | schedule | 阶段 1 最简版已实现 | 当前建立单一恒真 Region，以 def-use 和内部事件依赖求 SCC 后生成确定性全序；阶段 2 再扩为多 Region |
| 9 | `emit_grhsim(backend="cpu")` | emit | 阶段 1 已实现 | 按 Schedule 逐 op 打印 C++ |

目标排序遵循契约：1～6 为图改写，7～8 为划分与调度，9 只读。阶段 1 的
实际 Python 流水线只有 1、8、9；2～7 尚未注册到默认流水线。

## 3. IR 形态演变

| 阶段完成后 | 方言 | Schedule | 存储 |
| --- | --- | --- | --- |
| 1（映射） | generic | 未调度 | 平凡布局 |
| 2～4（generic 优化） | generic | 未调度 | 平凡布局 |
| 5～6（特化与下降） | generic + cpu | 未调度 | 已特化 |
| 7（划分） | generic + cpu | 已划分 | 已特化 |
| 8（调度） | generic + cpu | 已调度（全序） | 已特化 |

## 4. 运行时形态

```cpp
void eval(Sim& s) {
    do {
        s.next = s.current;
        clear_round_local_event_pulses(s);
        execute_linearized_schedule(s);  // 当前只有一个恒真 Region
        apply_pending_memory_writes(s);
        s.sync_external_edge_history();
        swap(s.current, s.next);
    } while (!stable(s));
}
```

`stable()` 对应一轮后 `S' == S` 的判定；振荡由生成时可配置的迭代上限诊
断（默认 100，仿真模型 5.5 节）。阶段 1 每轮执行完整全序。旧方案的运行
时 activity bit 监测要到阶段 2 才变为显式 Region 与编译期激活条件。

### 4.1 内部合成事件

外部端口或普通状态事件使用 `edge_track_` 保存跨 round 的一位历史。内部组
合值不能直接作为 StateDecl 事件源，因此 `lower_grhsim` 将它提升为名字带
`$event` 后缀的内部 Output StateDecl，并在对应 `generic.out_write` 上标记
`eventState = true`。

这类 writer 不作为公开 C++ 输出端口发射。它执行时同时完成三件事：

1. 比较 `state_words_` 中的旧值与当前数据流新值；
2. 写入本轮专用的 posedge/negedge pulse slot；
3. 把新值写入 `next_state_words_`，作为后续 round 的事件历史。

`schedule-topo` 把 writer → 所有引用该 StateDecl 的事件消费者作为虚拟依赖
纳入 SCC 与最终全序，因此 pulse 在产生的同一 round 即可被消费。pulse 在
每轮开始和 eval 收敛退出时清零，静态电平不会在后续 round 或下一次 eval
中重复触发。

## 5. 性能要点

- **激活跳过**：无事件时 commit 区域整片不执行——时钟沿之间的求值只跑
  compute 部分；
- **编译期调度**：无运行时调度开销，op 顺序即生成代码顺序；
- **两态编码与机器字打包**：存储特化的直接收益；
- **整数组批量操作**：`array_*` / 整数组读写把逐位操作合并为字级操作。

前四项是目标形态；阶段 1 只具备编译期全序和 generic bit-vector 运行时。
2026-08-27 的 XiangShan CoreMark 模型包含 3,970,919 个 op 和 417 个内部事
件 pulse slot。以 5,000 op/源文件、Clang 22 `-O1` 构建的 229 MiB emulator
执行首个 host cycle 约 34.25 秒，随后约 0.275 秒/cycle。CoreMark + NEMU
连续运行至 cycle 39,250，模型计时 3:00:35.914，全程无 mismatch，并在
cycle 5,000 / 8,750 / 10,000 / 20,000 / 30,000 精确匹配已有 GSim golden
检查点。工具会话在约 3 小时处被回收，因而没有实测 `-C 50000` 的最终
`instrCnt = 73584` / `cycleCnt = 49998`；经确认停止剩余长跑并接受阶段 1
正确性。性能优化仍按阶段 2 推进，此处不把未观测的最终计数记作实测结果。

## 6. 已知限制

- **当前只有一个恒真 Region**：尚无 commit Region 激活跳过；旧方案的
  activity bit 细化（compute 子树按输
  入变化跳过、supernode coarsen）尚未表达——它需要"边界值变化"激活条件，
  属于 cpu 方言对区域激活条件形式的扩展（激活条件形式由方言文档定义，
  见 generic 方言第 5 节），在 cpu 方言文档中补齐；
- `fold-const`、`dead-code-elim`、`specialize-storage-cpu` 和 `lower-cpu-*`
  尚未迁入 GRHSIM IR 默认流水线；XiangShan 正确性长跑可执行，但约 3.8
  小时/50,000 host cycle，仍不适合作为高频回归；
- 当前 CPU emitter 不发射 waveform 或 perf instrumentation；
- 单线程，不利用区域间偏序的并行性（多线程方案另立 flow 文档）；
- 零延迟振荡不做静态排除，运行时按迭代上限诊断。
