# GrhSIM 仿真模型总览

本文介绍当前 `grhsim-cpp` emitter 生成的 GrhSIM 模型。内容按代码实现整理，目标是
回答“模型是什么、如何生成、生成后怎样驱动、一次 `eval()` 做了什么，以及模型有
哪些边界”。调度算法的逐项推导请继续阅读
[GrhSIM 当前调度方法](grhsim-scheduling.md)；宽值 lowering 的细节见
[XiangShan GrhSIM 宽值说明](grhsim-xiangshan-width.md)。

## 先给结论

仓库中已经有调度、宽值和 emit combine 等专题文档，但此前没有一篇同时覆盖模型
定位、输入前置条件、生成物、运行时生命周期和端到端用法的中文总览。GrhSIM 可以
概括为：

> 把一个已经规范化、完成静态 activity schedule 的 GRH 顶层 graph，编译成一个
> 使用 C++ 标量/字数组和固定点调度循环的专用仿真类。

它不是在运行时解释 SystemVerilog AST 的通用解释器，也不是自动推进时间的事件队列。
生成模型只在调用者修改端口并调用 `eval()` 时求值；时钟、复位、测试激励和仿真时间
由 testbench 或外部 emulator 管理。

## 1. 模型在系统中的位置

### 1.1 名词

- **GRH**：Wolvrix 的 Graph RTL Hierarchy 中间表示。operation 描述组合运算、状态
  读写、memory 访问、DPI/system task 等；value 是 operation 的结果或 graph 端口。
- **activity-schedule**：把 GRH 的依赖图分割成可增量激活的 compute/commit
  supernode，并把结果写入同一个 `Session`。
- **grhsim-cpp**：读取 GRH 和上述 session 数据的 emitter，生成 C++ 源码和一个
  可编译的 Makefile。
- **GrhSIM 模型**：生成的 `GrhSIM_<top>` C++ 类及其 `eval()`、状态存储、批处理
  函数和 runtime helper。

当前没有单独的 `grhsim` 命令行解释器；常用入口是 Python
`Session.emit_grhsim_cpp(...)`，仓库里的 HDLBits/XS 脚本只是对这条 API 的集成封装。

### 1.2 总体数据流

```text
SystemVerilog 或 GRH JSON
          |
          v
  Session 中的 GRH graph
          |
  规范化/优化 passes
          |
  activity-schedule
    (写入 Session)
          |
  grhsim-cpp EmitModel
          |
  C++ 头文件、runtime、state、eval、schedule 分片
          |
  生成的 Makefile -> libgrhsim_<top>.a
          |
  testbench / XiangShan difftest emulator
```

对应的主要实现入口是：

| 层次 | 代码或文档 |
| --- | --- |
| C++ emitter 公共入口 | [`include/emit/grhsim_cpp.hpp`](../../include/emit/grhsim_cpp.hpp) |
| emitter 实现 | [`lib/emit/grhsim_cpp.cpp`](../../lib/emit/grhsim_cpp.cpp) |
| 通用输出选项和 session 指针 | [`include/core/emit.hpp`](../../include/core/emit.hpp) |
| activity schedule pass | [`lib/transform/activity_schedule.cpp`](../../lib/transform/activity_schedule.cpp) |
| schedule 运行时语义 | [`grhsim-scheduling.md`](grhsim-scheduling.md) |

## 2. 生成前必须准备什么

### 2.1 emitter 的硬性前置条件

`grhsim-cpp` 当前要求：

1. 只能传入一个 top graph。
2. 必须提供输出目录。
3. 必须在 `EmitOptions.session` 中找到同一个 graph 的 activity-schedule 数据。

缺少任一条件，emitter 会在生成阶段报错，而不是退回到“按 GRH 解释执行”。
因此，不能把 schedule 之后的 graph 单独复制到另一个空 `Session` 再直接 emit；应在
同一 session 中完成 pass 和 emit，或在新 session 中重新运行 schedule。

对于 `path=top`，当前 emitter 读取的 session key 是：

| key 后缀 | 是否必需 | 含义 |
| --- | --- | --- |
| `activity_schedule.supernode_to_ops` | 是 | 每个 supernode 包含的 operation |
| `activity_schedule.value_fanout` | 是 | 跨 supernode value 的扇出 |
| `activity_schedule.topo_order` | 是 | supernode 的拓扑顺序 |
| `activity_schedule.state_read_supernodes` | 是 | state symbol 到 reader compute supernode 的映射 |
| `activity_schedule.supernode_kind` | 否 | 存在时直接区分 compute/commit；缺失时 emitter 从 op kind 推断 |
| `activity_schedule.compute_nodes_by_supernode` | 否 | 当前会读取并保存在 `ScheduleRefs`，但后续 codegen 不使用 |

完整 key 列表和 pass 选项见
[activity-schedule 文档](../transform/activity-schedule.md)。

### 2.2 一个常见的前端流水线

不同设计可能需要不同的规范化 pass。HDLBits 端到端脚本
[`scripts/wolvrix_hdlbits_grhsim.py`](../../../scripts/wolvrix_hdlbits_grhsim.py) 给出了
一个可工作的参考顺序：

```text
read_sv
  -> xmr-resolve
  -> multidriven-guard
  -> latch-transparent-read
  -> hier-flatten
  -> comb-lane-pack
  -> comb-loop-elim
  -> slice-index-const
  -> simplify(semantics="2state")
  -> memory-init-check
  -> stats（可选但脚本会运行）
  -> activity-schedule
  -> emit_grhsim_cpp
```

这不是所有设计的强制清单，但有两个原则不变：

- 目标 graph 在 schedule 时应已满足该 pass 的层级和依赖约束；
- `activity-schedule` 必须先于 `emit_grhsim_cpp`，并且二者共享 session。

目标 graph 不能残留未 lower 的层级实例、blackbox 或 XMR operation。通常应先做
hierarchy flatten；blackbox 可 lower 成当前支持的 `kDpicImport` / `kDpicCall` 边界，
无需继续消除 DPIC operation。`activity-schedule` 不是层级结构的运行时解析器。

## 3. 静态模型：从 operation 到 supernode

### 3.1 调度分类不是硬件语义分类

`activity-schedule` 内部把 operation 分成四类：

| 调度类 | 常见 operation | 进入哪里 |
| --- | --- | --- |
| `Source` | constant、register/latch/memory read port | compute node 的输入或局部 source |
| `Compute` | assign、算术/逻辑、mux、slice、DPI/system task | compute node |
| `Sink` | register/latch/memory write、memory fill | commit node |
| `Declaration` | register、latch、memory、DPI import、层级声明 | 不进入运行时 schedule |

`kMemoryReadPort` 虽然属于 `Source`，但当前规范化 GRH 中它的 operand 是地址；同步、
enable 和事件条件由外围逻辑或状态写入表达。地址 operand 仍然参与依赖追踪。这里的
`Source` 只是调度器名称，不表示“没有依赖”。

### 3.2 中间 node 和最终 supernode

- **compute node**：从一个结果或 commit 输入反向追依赖，尽量吸收局部组合 producer。
  无法安全吸收的 value 记录为 boundary input。
- **commit node**：按 event/guard 等条件聚合 sink operation，保存写入所需的 data、
  address、mask、enable 等输入。
- **compute supernode**：将 compute node 在拓扑顺序上 coarsen/分段后的最终执行单元。
- **commit supernode**：由 commit node 形成的状态写入执行单元。

最终 schedule 保持 compute 与 commit 分离。supernode 之间的跨边界 value 形成
`value_fanout`；普通 tracked value 实际变化时触发后继 compute supernode，少数
emitter 专用 lowering 允许保守多激活。

### 3.3 activity bit 和 batch

每个 compute supernode 在运行时有一个 activity bit。emitter 将多个相邻或适合合并
的 supernode 编入一个 compute batch，将 sink supernode 编入 commit batch。batch 的
数量和每个 batch 的源文件分组是编译期决定的，不改变 RTL 的逻辑结果。

以下关系很重要：

```text
输入变化       -> 激活读取该输入的 compute supernode
compute value 变化 -> 按 value_fanout 激活后继 compute supernode
visible state 变化 -> 按 state_read_supernodes 激活 reader compute supernode
```

`commit` 目标不会通过普通 compute boundary fanout 决定是否扫描；generic fixed-point
的每个 round 都调用 commit batch，再在 body 内用 event expression/update guard 过滤
sink operation。可选 full-pass fast path 使用独立 dispatch。逐行解释见
[调度专题](grhsim-scheduling.md)。

## 4. 生成物和职责

假设 top graph 名为 `top_module`，输出目录通常包含：

| 生成物 | 职责 |
| --- | --- |
| `grhsim_top_module.hpp` | `GrhSIM_top_module` 类声明、公开端口、状态/值槽位、batch 常量和 API |
| `grhsim_top_module_runtime.hpp` | 标量、宽字、mask、slice、event edge 等通用 helper |
| `grhsim_top_module_state.cpp` | 构造/析构、`init()`、输出刷新、system task/DPI runtime 辅助代码 |
| `grhsim_top_module_state_init_N.cpp` | 分片后的输入、常量、state、event 和调度状态初始化 |
| `grhsim_top_module_eval.cpp` | `eval()` 的输入变化检测、fixed-point round、phase dispatch 和收尾 |
| `grhsim_top_module_sched_N.cpp` | 一个或多个 compute/commit batch 的具体 operation 代码 |
| `grhsim_top_module_sched_group_N.cpp` | `sched_batches_per_cpp > 1` 时的 batch 组合文件 |
| `grhsim_emit_stats.json` | 当前只包含 `packed_array_lane_emit` 统计 |
| `Makefile` | 使用 C++20/PCH 编译上述源文件并归档 `libgrhsim_top_module.a` |

集成脚本还可能额外写出设计 JSON、schedule 统计 JSON 或日志；这些不是 emitter 固有
产物。例如 HDLBits 会写 `dut_003.json`，XiangShan 会写
`activity_schedule_supernode_stats.json`。

典型目录形态：

```text
build/hdlbits-grhsim/grhtb_003/grhsim_top_module/
  grhsim_top_module.hpp
  grhsim_top_module_runtime.hpp
  grhsim_top_module_state.cpp
  grhsim_top_module_state_init_*.cpp
  grhsim_top_module_eval.cpp
  grhsim_top_module_sched_*.cpp
  grhsim_emit_stats.json
  Makefile
  libgrhsim_top_module.a       # make -C 后出现
```

源码仍保留旧 state-shadow/write-buffer commit 分片基础设施，但当前普通 emit 不填充
对应工作列表，因此不会生成 `state_commit_*.cpp`，也没有额外的 state-shadow phase。

每个生成文件，包括 header、runtime、state/eval/schedule、Makefile 和 stats，默认都受
`max_cpp_file_bytes` 限制；通用默认值是 4 GiB，显式传 0 表示不设上限。超大设计应
优先调整 batch 分割，而不是简单取消单文件保护。

## 5. 生成类的公开接口

生成类名是 `GrhSIM_<规范化后的 top 名>`。头文件中的端口声明是设计的实际接口，
端口名和 C++ 类型应以生成的 `.hpp` 为准。通常可以看到：

```cpp
GrhSIM_top_module dut;
dut.init();

// 端口名来自 top_module 的声明。
dut.a = false;
dut.b = true;
dut.eval();
bool y = dut.y;
```

普通 input/output 通常是对应的公开 C++ 字段；inout 会生成包含 `in`、`out`、`oe`
的端口对象。宽端口则是固定长度的 `std::array<std::uint64_t, N>`，不能只按低 64 bit
读写。

基础 API 包括：

| API | 说明 |
| --- | --- |
| 构造/析构 | 创建和释放模型内部存储；构造不等同于 RTL reset |
| `init()` | 把公开端口恢复默认值，初始化值/state，并把 event baseline 重置为“未建立” |
| `eval()` | 在当前端口值下执行 fixed point；若 activity 不收敛则不会返回 |
| `set_random_seed(seed)` | 为 `$random` 等模型随机源设置 seed；应在 `init()` 前调用 |
| `had_register_write_conflict()` | 兼容性占位 API；当前只有清零和读取路径，结果恒为 false，不能用于冲突检测 |
| `perf_counters()` / `reset_perf_counters()` | 仅在 `perf="eval"` 生成时存在 |
| `set_runtime_profile_enabled()` / `runtime_profile_enabled()` / `dump_runtime_profile()` | 始终声明；只有生成时启用 runtime profile 才真正记录数据，否则是 no-op/false |
| `configure_waveform(...)` 等 | 仅在启用 waveform emit 时存在 |
| `finish_requested()` / `stop_requested()` / `fatal_requested()` 等 | graph 需要 system-task runtime 时才生成；结束类 task 当前仍会终止宿主进程 |

生成类没有 `step()` 或 `settle()` API，只有调用者显式调用 `init()` / `eval()`。
`init()` 会覆盖此前写入的 input/inout/output，因此必须先 `init()`，再 drive 端口。
它不等同于 RTL reset；设计中的同步/异步 reset 仍由公开 reset 端口和 `eval()` 驱动。

Runtime profile 生成当前通过底层 emit attribute `emit_runtime_profile` 或环境变量
`GRHSIM_EMIT_RUNTIME_PROFILE=1` 控制，Python `emit_grhsim_cpp(...)` 没有同名显式参数。
启用后 emit 阶段写 `grhsim_supernode_static.tsv`；运行时先调用
`set_runtime_profile_enabled(true)`，再调用 `dump_runtime_profile()` 写 fire count。默认
路径是 `grhsim_supernode_fire.tsv`，可用 `WOLVRIX_GRHSIM_SUPERNODE_TSV` 覆盖。

## 6. `eval()` 的运行时语义

### 6.1 一次调用不是一个自动时钟周期

模型没有内部时间推进器。调用者负责：

1. 写入输入和 inout 的驱动值；
2. 按需要翻转 clock/reset；
3. 每次输入变化后调用 `eval()`；
4. 在 `eval()` 返回后读取公开输出。

如果 clock 从 0 改为 1 后没有调用 `eval()`，模型不会观察到 posedge；如果连续调用
`eval()` 但端口值没有变化，也不会凭空产生新的边沿。还要注意，普通 event 在第一次
`eval()` 时会因为 `event_baseline_initialized_` 尚未建立而被抑制：通常应先在基线电平
调用一次 `eval()`，再翻转 clock 并调用下一次 `eval()`。带有特定异步 reset lowering
标记的 reset event 是例外，不能把这个例外推广到普通 clock。

例如，一个普通 posedge 驱动的 testbench 应显式建立低电平基线，并让每个边沿各占
一次 `eval()`：

```cpp
dut.init();
dut.clk = false;
dut.eval();       // 建立 prev 输入和 event baseline
dut.clk = true;
dut.eval();       // 观察 posedge
dut.clk = false;
dut.eval();       // 观察 negedge，并为下一次 posedge 建立低电平
```

### 6.2 仿真模型伪代码

关闭两个 full-pass specialization 时，生成模型可简化为下面的伪代码：

```text
init():
  reset public input/inout/output ports to defaults
  clear cached values; seed constants and random state
  initialize register/latch/memory state
  prev_inputs = defaulted inputs
  clear activity and event slots
  first_eval = true
  event_baseline_valid = false

eval():
  initial = first_eval
  pending = initial
  if initial:
    activate(all compute supernodes)
  else:
    for each changed input:
      activate(its compute readers)
      pending |= has_compute_readers || is_commit_operand

  classify direct input edges
  # 普通首次 edge 为 NONE；gsim.reset_kind=async 标记的 reset event 是例外

  while pending:
    pending = false

    for compute batch in schedule order:
      consume activity bits
      execute active supernodes in local topo order
      propagate changed results
      # 少数专用 lowering 可保守多激活

    reactivated_readers = false
    for commit batch in schedule order:
      for each sink whose event matches and updateCond is true:
        update visible register/latch/memory directly
        if visible state or row changed:
          activate(its compute readers)
          reactivated_readers |= has_readers

    pending = reactivated_readers || any_compute_activity
    clear round-local event edges

  flush deferred task output
  refresh public outputs
  optionally dump waveform
  prev_inputs = current_inputs
  event_baseline_valid = true
  first_eval = false
```

该 `while` 没有最大轮数或不收敛诊断。输入 graph 和前置 pass 必须保证 activity 最终
排空；否则 `eval()` 不会返回。参考流水线中的 `comb-loop-elim` 是为满足这一条件服务，
但 emitter 本身不会再设置 round limit。

两个默认关闭的 specialization 会绕过上述 generic loop：

- input full-pass：对满足 guard 的纯输入变化执行所有 compute full-pass batch，然后
  直接收尾，不扫描 commit；
- posedge full-pass：先执行 active compute，再 commit，清除已消费 event，然后按
  reader frontier 选择 sparse settle 或 dense compute full-pass，最后直接收尾。

当前这两个 early-return 收尾不会调用 generic path 的 deferred system-task text flush；
因此包含 `$strobe` 一类 deferred 输出的模型不应假定 specialization 与 generic path
具有相同的输出时机。

### 6.3 compute phase

compute batch 会读取并清除自己负责的 activity bit，然后按 supernode 内的局部拓扑顺序
执行 operation。普通 tracked assignment 只有在实际值变化时才继续传播：

- event value 会更新本 round 的 edge slot；
- 有跨 supernode 用户的 boundary value 会置位后继 compute bit；
- 同一 activity word 中的后继有时可在当前 batch 的局部变量中直接继续执行，其他
  后继留给后续 batch 或下一 round。

direct wide concat 和 packed-array lane base/assign 当前可保守激活 fanout，即允许多算
但不能漏算。因此模型不是每次 `eval()` 无条件重算整个 graph；第一次调用会全激活，
之后通常只重算受输入、状态或事件影响的区域。

### 6.4 commit phase 和状态更新

generic fixed-point 的每个 round 都会调度 commit batch，但具体 sink 是否执行取决于：

- `updateCond`（可包含 enable/reset lowering）和 exact event 表达式；
- 写 mask、memory address/range 等条件；
- memory row/address 是否有效。

当前普通 commit 路径的生成代码直接更新 visible state。源码中仍保留旧 state-shadow
压缩基础设施的类型和分片接口，但该路径目前被禁用，当前不会生成对应文件；
这些内部字段也不是 testbench 可以依赖的额外时序阶段。对外统一遵循：

1. 只有 visible state 的值真正变化，才算一次状态变化；
2. 状态变化会激活 `state_read_supernodes` 对应的 compute reader；
3. reader 在 commit phase 之后的下一 fixed-point round 执行。

这保证了“本 round 先完成组合计算，再提交状态，再让状态读者看到新值”的顺序。
memory read 直接按当前地址读取生成的 memory array；memory write 在 commit phase
按地址、mask 和 guard 更新相应 row，并可触发行级 reader 激活。通用动态地址越界读
返回零、越界写不更新；行数为 2 的幂时，生成代码可用地址 mask 实现 wrap，静态可证
地址范围时则会省略检查。

多写碰撞不能简单依赖 operation 在 graph 中出现的顺序。只有带有
`memoryWrite.priorityGroup` 和连续、唯一 `memoryWrite.priority` 的同组写，
schedule 才会校验并按 priority 从大到小排列（priority 0 表示最高优先级、最后执行）。未带这些属性
的同 memory 多写不提供可依赖的通用碰撞优先级；需要确定结果时，应在前端 lowering
阶段显式编码 priority 或互斥条件。

### 6.4.1 memory port 的规范化形态

为了避免把外围控制误认为 memory port 的隐式 operand，当前 GRH 形态可以按下表读取：

| operation | operand 顺序 | result/attrs |
| --- | --- | --- |
| `kMemoryReadPort` | `oper[0] = addr` | `res[0] = data`；`memSymbol` 指向目标 memory |
| `kMemoryWritePort` | `oper[0] = updateCond`、`oper[1] = addr`、`oper[2] = data`、`oper[3] = mask`、`oper[4..] = events` | 无 result；`memSymbol` 指向目标 memory，`eventEdge` 给出每个 event 的边沿 |
| `kMemoryFillPort` | `oper[0] = updateCond`、`oper[1] = data`、`oper[2..] = events` | 无 result；data 是单 row 或整个 memory 的 packed logic |

例如下面的 RTL：

```systemverilog
always @(posedge clk) begin
    if (wen)
        mem[addr] <= data;
end
```

对应的关键 lowering 约束是：

```text
updateCond = wen
addr       = addr
data       = data
mask       = all_ones(memory_width)
events     = [clk]
eventEdge  = ["posedge"]
```

同步读则不是给 `kMemoryReadPort` 增加 clock operand，而是把它的 `data` result 再由
上层 register 捕获。完整的 GRH operand/attr 规范见
[GRH IR 的 memory 章节](../grh/grh-ir.md)。

`kMemoryFillPort` 的 row-width data 会广播到所有 row；`rowCount * rowWidth` 的 packed
data 按 LSB-row-first 切分，即 row `i` 读取 `[i * rowWidth +: rowWidth]`。commit 逐 row
比较和更新，只要任一 row 变化才激活 memory reader。

`kMemory` 的 `initKind/initFile/initValue/initStart/initLen` 在 emit 阶段解析；literal、
`readmemh` 和 `readmemb` 结果会写进生成的 init C++，`$random` row 在 `init()` 时求值。
运行时模型不再打开原始 init file，因此重新生成前该文件必须可读，生成后的模型则不
依赖它继续存在。

### 6.5 event 和输入 baseline

生成类会保存 `prev_*` 输入字段，并在 `eval()` 结束时更新。直接输入 event 的边沿
保存在 event edge slot 中，slot 的生命周期是一个 fixed-point round，而不是完整的
`eval()` 或“一个时钟周期”。round 结束会清空它；下一 round 若仍需该事件，必须重新
由输入变化或 compute result 产生。

### 6.6 side effect、system task 和 DPI

system task/DPI 调用被当作有副作用的显式 schedule 边界，按 schedule 顺序、条件和
事件执行。生成 runtime 对部分 `$display`、文件操作、结束请求等提供辅助接口；DPI
import 的 input/output 参数也可被生成代码绑定。这里使用的是生成器写出的直接
`extern "C"` C++ 声明和项目约定的 C++ 参数类型，不是完整的 IEEE `svdpi` ABI；调用者
必须按生成声明提供并链接实现。

`$finish`、`$stop`、`$fatal` 会先设置请求字段，再通过 `std::exit()` 终止宿主进程；
通常无法等 `eval()` 返回后再轮询这些标志。

这不是完整的 SystemVerilog task/function runtime。当前特殊支持的 system function
包括 `clog2`、`fopen` 和 `ferror`；不支持的 operation、部分 function 结果类型和 DPI
`inout` 参数会在 emit 阶段产生诊断。未知 system-task 名若通过形态校验，则进入 runtime
helper 后静默 no-op，不会自动报告 unsupported。

含 system-task runtime 的模型在析构时调用一次 `finalize()`：执行 final proc task，
flush deferred text，并 flush/close 文件句柄。`$finish/$stop/$fatal` 也先 finalize，再
调用 `std::exit()`。

## 7. 值和状态的存储方式

### 7.1 标量、宽值和非 logic 值

当前生成器按声明宽度选择 C++ 表示：

| GRH 值 | 典型 C++ 表示 |
| --- | --- |
| logic，宽度 1 | `bool` |
| logic，宽度 2 到 8 | `std::uint8_t` |
| logic，宽度 9 到 16 | `std::uint16_t` |
| logic，宽度 17 到 32 | `std::uint32_t` |
| logic，宽度 33 到 64 | `std::uint64_t` |
| logic，宽度大于 64 | `std::array<std::uint64_t, ceil(width / 64)>` |
| real | `double` |
| string | `std::string` |

宽值的每个 word 是 64 bit，helper 在运算结束时按声明宽度截断；加法、移位、
concat、replicate、slice、比较和 reduction 都有对应的 word-level helper。详细例子
见 [宽值说明](grhsim-xiangshan-width.md)。

### 7.2 value materialization

不是每个 GRH value 都变成类成员。生成器通常会：

- 为公开输出、跨 supernode boundary、事件采样、DPI 返回值等保留持久 slot；
- 对只在一个 supernode 内使用的中间 value 生成局部 `const auto`；
- 对合适的单用户廉价 scalar 表达式直接 inline；wide local 保留具名临时量。

这既是语义实现也是代码规模控制手段。不要在 testbench 中依赖 `val_*` 等私有内部
字段；它们不是稳定接口。

### 7.3 逻辑语义边界

当前 GrhSIM 热路径使用 C++ 二值标量/字数组表示 logic。仓库的 HDLBits 参考流水线
明确运行 `simplify(semantics="2state")`，所以 GrhSIM 不应被理解为完整的四态 X/Z
解释器。若设计依赖未知态传播，必须先确认前端 lowering 和目标验证流程是否提供了
相应语义。

## 8. 如何生成和运行

### 8.1 推荐的 HDLBits smoke

先准备环境并安装 Python 包：

```bash
# 首次使用时可先执行：cp env.sh.template env.sh
source env.sh
make py_install
make run_hdlbits_grhsim DUT=003
```

顶层 target 会调用 `testcase/hdlbits/Makefile`，实际顺序是：

```text
Python 读取 DUT -> 运行 pass -> emit C++
  -> make -C 生成目录构建静态库
  -> 编译 grhtb_003.cpp
  -> 运行 testbench
```

默认产物在：

```text
build/hdlbits-grhsim/grhtb_003/grhsim_top_module/
```

全量 smoke 可运行：

```bash
make run_all_hdlbits_grhsim_tests
```

### 8.2 自定义设计的 Python API

下面是一个最小参考模板。实际设计可按需要增删规范化 pass，但必须保留
`activity-schedule` 到 `emit_grhsim_cpp` 的同 session 关系：

为使后面的 testbench 示例具体，假定 `top.sv` 至少包含如下接口：

```systemverilog
module top_module(input logic a, output logic y);
    assign y = a;
endmodule
```

```python
from pathlib import Path
import wolvrix

top = "top_module"
out = Path("build/grhsim-demo")
out.mkdir(parents=True, exist_ok=True)

with wolvrix.Session() as sess:
    sess.log_level = "info"
    sess.read_sv("top.sv", out_design="design.main",
                 slang_args=["--top", top])

    for name, kwargs in [
        ("xmr-resolve", {}),
        ("multidriven-guard", {}),
        ("latch-transparent-read", {}),
        ("hier-flatten", {"sym_protect": "hierarchy"}),
        ("comb-lane-pack", {}),
        ("comb-loop-elim", {}),
        ("slice-index-const", {}),
        ("simplify", {"semantics": "2state"}),
        ("memory-init-check", {}),
    ]:
        sess.run_pass(name, design="design.main", **kwargs)

    sess.run_pass("activity-schedule", design="design.main", path=top)
    sess.emit_grhsim_cpp(
        design="design.main",
        output=str(out),
        top=[top],
        waveform="off",
        perf="off",
    )
```

emitter 会生成 Makefile，但不会替用户生成最终可执行文件。该 Makefile 虽声明
`CXX ?= clang++`，GNU make 的内建 `CXX=g++` 通常已经使 `?=` 失效，而编译规则使用
Clang 风格的 `-include-pch`。当前应在命令行显式传 `CXX=clang++`。

```bash
source env.sh
make py_install
python3 emit_grhsim.py
make -C build/grhsim-demo CXX=clang++ -j"$(nproc)"
clang++ -std=c++20 -O2 -Ibuild/grhsim-demo \
    tb.cpp build/grhsim-demo/libgrhsim_top_module.a \
    -o build/grhsim-demo/tb
build/grhsim-demo/tb
```

`tb.cpp` 中应构造生成类、先调用 `init()`，再写端口并调用 `eval()`：

```cpp
#include "grhsim_top_module.hpp"

int main()
{
    GrhSIM_top_module dut;
    dut.init();
    dut.a = true;
    dut.eval();
    // 端口名和类型以生成的 grhsim_top_module.hpp 为准。
    return dut.y ? 0 : 1;
}
```

启用 `waveform="declared-symbols"` 时，生成类才会出现 waveform 配置 API；构建
Wolvrix 时还需要 vendored `libfst` 和 zlib。生成后 waveform runtime 默认仍关闭，
自定义 testbench 必须调用 `configure_waveform(true, "trace.fst")` 或
`set_waveform_enabled(true)`。波形时间戳是 `eval()` 调用序号，不是独立仿真时间；
`declared-symbols` 也不会把整个 memory 内容作为普通信号逐项记录。最终链接命令通常
还要补 `-lz`。

### 8.3 生成选项和大模型分片

Python API 支持以下与生成规模或诊断有关的选项：

| 参数 | 默认值 | 作用 |
| --- | ---: | --- |
| `max_cpp_file_bytes` | 4 GiB | 单个生成文件上限；0 表示不设上限 |
| `sched_batch_max_ops` | 512 | batch 的 operation 数上限 |
| `sched_batch_max_estimated_lines` | 4096 | batch 的估算源码行数上限 |
| `sched_batch_target_count` | 0 | compute/commit 分别应用的软目标；0 表示关闭，不保证精确 batch 数 |
| `sched_batches_per_cpp` | 1 | 一个 `sched` 源文件包含多少 batch；输入 0 也钳为 1 |
| `emit_parallelism` | hardware concurrency | 并行生成 schedule 源文件的并发度；输入 0 表示 auto，不表示运行时并行 |
| `waveform` | `off` | `off` 或 `declared-symbols` |
| `perf` | `off` | `off` 或 `eval`，后者生成 eval 计数器 |
| `input_fullpass_specialization` | false | 是否生成输入变化场景的 compute full-pass 变体 |
| `posedge_fullpass_specialization` | false | 是否生成单一 direct-input event 场景的 event full-pass 变体；名称沿用现有 API |

这些参数只改变分片、辅助路径或观测数据，不改变 GRH 的逻辑语义。大设计首先应
用 batch 参数控制单文件大小和编译压力。`grhsim_emit_stats.json` 当前只含
packed-array lane 统计，不能代替完整的 schedule/runtime profile。

### 8.4 XiangShan 集成路径

在仓库配置完整、已有 XiangShan generated-src 和 ready-to-run 镜像时，可使用：

```bash
source env.sh
make xs_wolf_grhsim_emu
make run_xs_wolf_grhsim_emu XS_SIM_MAX_CYCLE=2000
```

前一个 target 负责 GRH/schedule、C++ emit、模型编译和 difftest emulator 构建；后
一个 target 驱动 CoreMark，并通过 NEMU difftest 校验。主要产物通常位于
`build/xs/grhsim/`，日志位于 `build/logs/xs/`。

波形必须在生成和运行两侧同时打开：

```bash
make xs_wolf_grhsim_emu WOLVRIX_GRHSIM_WAVEFORM=1
make run_xs_wolf_grhsim_emu \
    WOLVRIX_GRHSIM_WAVEFORM=1 XS_WAVEFORM=1 XS_SIM_MAX_CYCLE=2000
```

XiangShan 的 pass 链和调度参数比 HDLBits 更复杂，遇到规模或性能问题时应先阅读
对应脚本和 [调度专题](grhsim-scheduling.md)，不要把 XS 专用环境变量当成通用 GRH
接口。

## 9. 常见问题和边界

| 现象 | 先检查什么 |
| --- | --- |
| `missing activity-schedule session data` | 是否在同一 `Session`、同一 `top/path` 先运行了 `activity-schedule` |
| `expects exactly one top graph` | emit 调用是否只传了一个 top；多 top 需要分别生成 |
| 改了输入但输出不变 | 是否先 `init()`，每次端口变化后是否调用 `eval()`，读的是否是公开输出字段 |
| 时序逻辑没有更新 | clock 是否确实跨 `eval()` 调用形成边沿；`init()` 不等于 RTL reset |
| waveform emit 失败 | Wolvrix 是否带 `libfst` 支持；生成时是否启用 waveform，运行时是否显式调用配置 API 打开 |
| emit 报 unsupported op/DPI | 查看 operation symbol；当前 DPI `inout` 和部分 system function 不是通用支持范围 |
| 生成源码太大或编译很慢 | 调低 batch 上限，保持或减小 `sched_batches_per_cpp` 以缩小单个翻译单元；只有文件数量过多时才增大它，并检查 `max_cpp_file_bytes` |
| 依赖未知态行为 | 确认前端是否走 2-state simplify；GrhSIM logic 热路径不是完整四态模型 |
| 试图从多个线程共享一个模型实例 | 当前 batch 按生成顺序单线程执行，模型实例没有线程安全承诺 |

## 10. 深入阅读和源码定位

- [GrhSIM 当前调度方法](grhsim-scheduling.md)：activity class、supernode、activation
  edge、fixed-point round 的精确定义。
- [activity-schedule pass](../transform/activity-schedule.md)：pass 选项、DAG 构造和
  session 输出 key。
- [GrhSIM emit 内部合并与特化](grhsim-emit-combines.md)：emit 内部 pattern lowering
  和 materialization 优化，不是公开 GRH dialect。
- [XiangShan GrhSIM 宽值说明](grhsim-xiangshan-width.md)：`uint64_t` word array 和
  宽操作 helper 的具体 lowering。
- [`scripts/wolvrix_hdlbits_grhsim.py`](../../../scripts/wolvrix_hdlbits_grhsim.py)：
  小设计的端到端参考脚本。
- [`scripts/wolvrix_xs_grhsim.py`](../../../scripts/wolvrix_xs_grhsim.py)：XiangShan
  大设计的集成驱动。

当生成结果与预期不符时，建议按这个顺序定位：先确认 GRH/transform 结果，再检查
activity-schedule 的 session 统计，最后查看生成的 `eval.cpp` 和对应 `sched_*.cpp`。
不要直接修改生成源码来修复设计语义；应修正输入、pass 或 emitter，再重新生成。
