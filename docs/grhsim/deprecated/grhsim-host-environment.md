# GRHSIM-AM HostEnvironment 参考定义

> **DEPRECATED（已废弃）**：本文档自 2026-08-25 起废弃，内容仅保留作历史参考。
> GRHSIM 架构正在重新规划，新架构文档就绪前请勿将本文作为设计或实现依据。

> 本文给出 GRHSIM-AM 与宿主环境交互的参考边界。接口名称和物理 ABI 不是强制实现，
> 但后端必须保持本文列出的值、顺序、错误和生命周期语义。

GRHSIM-AM 的 Variable 和 Block 只描述仿真状态与行为。时间、终端输出、文件、随机源、
SystemFunction、SystemTask 和 DPI 实现来自 Machine 外部，统一记为 `HostEnvironment H`：

```text
Machine M = (Program P, State S, HostEnvironment H)
```

HostEnvironment 不属于 Program，也不能被 AM 指令通过 VarId 直接访问。一个 H 可以服务
一个或多个 Machine，但必须隔离每个 Machine 的文件句柄、deferred output、终止状态和
binding 私有状态，除非调用方明确要求共享。

## 1. HostValue

所有跨 HostEnvironment 边界的运行时参数和结果使用带 Type 的值：

```text
HostValue = BVValue(Type = BV<W, Sign>, Bits[W])
          | RealValue(Type = Real, Bits[64])
          | StringValue(Type = String, Bytes[])
```

- BV 按精确 W-bit 位模式传递；Sign 是 Type 的一部分，不改变 Bits；
- Real 按 IEEE-754 binary64 的完整 64-bit 位模式传递，包括正负零、无穷和 NaN payload；
- String 是带显式长度的有限 byte 序列，允许空串和内嵌 `0x00`，不隐含 UTF-8、locale
  或 NUL 终止；
- Array 不进入 system function/task 或 DPI 调用；需要传递数组时必须先 lower 为该
  binding 明确定义的标量序列，或扩展未来的 AM 类型系统；
- Host 返回值的 Type 必须与调用 schema 完全一致，不能依赖宿主语言的隐式整数提升、
  字符串截断或浮点类型转换。

DPI `AbiKind = real32` 是明确例外：HostValue 在 AM 一侧仍是 binary64 Real，DPI adapter
在调用边界按 DpiImport 规则执行 binary64/binary32 转换；转换前后的 AM Value Type 不变。

物理实现可以使用整数、word array、`double`、`std::string`、引用或指针。若某个物理
ABI 不能保留上述逻辑值，例如把含内嵌 NUL 的 String 直接传给 C `char *`，adapter 必须
完整转换或在调用前报告 unsupported，不能静默截断。

## 2. 参考接口

以下伪接口覆盖建议能力，不要求后端使用相同类名或函数签名：

```text
HostEnvironment:
    resolve_system_function(name, arg_types, result_type) -> FunctionBinding | Error
    invoke_system_function(binding, args) -> HostValue | Error

    resolve_system_task(name, arg_types) -> TaskBinding | Error
    invoke_system_task(binding, args) -> TaskOutcome | Error

    resolve_dpi_import(dpi_import) -> DpiBinding | Error
    invoke_dpi(binding, inputs, inouts) -> DpiOutcome | Error

    current_time() -> SimulationTime
    random_init_bits(request) -> Bits[request.width] | Error

    resolve_init_path(file_root, path) -> Path | Error
    read_init_file(path) -> ByteStream | Error

    emit_text(channel, bytes, newline, deferred) -> void | Error
    open_file(path, mode) -> FileHandle | Error
    flush_file(handle) -> void | Error
    close_file(handle) -> void | Error

    request_termination(kind, exit_code, message) -> TaskOutcome
    report_diagnostic(severity, message, context) -> void

    on_machine_created(machine_id)
    on_eval_begin(machine_id, eval_index)
    on_eval_end(machine_id, eval_index)
    on_finalize(machine_id)
```

参考结果类型为：

```text
TaskOutcome = continue
            | terminate(kind = finish | stop | fatal, exit_code, message)

DpiOutcome = (return?, outputs[], inouts[])
SimulationTime = (ticks: Nat, unit: TimeUnit)
TimeUnit = s | ms | us | ns | ps | fs
```

实现可以把这些能力合并到 binding 或拆成多个 service。Program 未使用某项能力时，H
不需要提供它。

## 3. Binding

Machine 创建期间必须完成所有实际引用 binding 的解析：

- `system.function` 以 `(name, argument Types, result Type)` 为签名；
- `system.task` 以 `(name, argument Types)` 为签名；
- `dpi.call` 以完整 DpiImport 的 Symbol、Parameter 顺序、Direction、Type、AbiKind 和
  Return 为签名。

解析必须得到唯一结果。缺失、重复或不兼容 binding 会使 Machine 创建失败，不能推迟到
首次调用，也不能让未知名称静默 no-op。binding handle 是后端内部对象，不进入 Program
Attribute；实现可以在 Machine 创建后缓存它。

HostEnvironment 不提供 Blackbox 解析或绑定入口。`kBlackbox` 在 GRHSIM-AM lowering 中
无条件非法，不能把 system/DPI binding 机制扩展解释为隐式 Blackbox 支持；需要的外部
行为必须在上游显式改写为 DpiImport/`dpi.call`。

SystemFunction 的 Result、DPI return/output/inout 只有在 host 调用正常返回且全部 Type
校验通过后才提交到 VariableArea。失败时不提交部分 AM Result。Host 已经执行的 I/O、
外部函数副作用或远端请求不要求回滚，因此 binding 应在产生不可逆效果前完成可执行的
参数检查。

## 4. 时间

`eval()` 不自动推进时间。调用方负责在两次 `eval()` 之间设置或推进 HostEnvironment 的
SimulationTime，并同时更新需要变化的 clock/input Variable。`time/stime/realtime` 等
SystemFunction binding 读取 `current_time()`，再按其自身签名转换为 BV 或 Real。

同一次 `eval()` 的全部指令观察同一个逻辑 time point，除非 binding 明确属于外部并发
接口；普通 HostEnvironment 不应在一次 `eval()` 中自行推进时间。波形时间戳也应读取
同一 SimulationTime。

## 5. 输出、文件和终止

参考 HostEnvironment 建议提供：

- stdout、stderr、日志和用户自定义 channel；
- 带 Machine 隔离的文件句柄表；
- `FileRoot` 和初始化文件路径解析；
- immediate text 与 deferred text 队列，供 `display/write/strobe` 类 binding 使用；
- finish、stop、fatal 三种终止请求及 exit code；
- info、warning、error、fatal 诊断 sink。

同一 Machine 内的可观察调用按 AM 实际指令执行顺序到达 H。deferred text 最迟在当前
`eval()` 正常结束或 `finalize()` 时按入队顺序 flush。终止请求必须先执行尚未执行的
final 调用并 flush 输出，再返回调用方或终止宿主进程；库式后端建议返回结构化
TaskOutcome，而不是强制调用 `exit()`。

## 6. Random Init

HostEnvironment 可以为无显式 seed 的 `random` InitExpr 提供 Machine 级 seed，或直接
为每次求值提供恰好 W bit 的结果。请求至少应包含：

```text
RandomInitRequest = (explicit_seed?, VarId, action_index, element_index?, width)
```

`explicit_seed` 存在时必须原样传递，不能被 HostEnvironment 忽略；同一个 fill 中的每个
Array 元素有独立请求。AM 不要求不同 HostEnvironment 使用相同 PRNG 或产生相同结果。
推荐的 SplitMix64、stream 划分和低 word 优先位填充方式见
[Variable Init 的 Random 章节](grhsim-am.md#94-random)。`undef` 不调用随机接口；它仍是
AM 层未定义行为，后端可以选择任意类型合法值。

## 7. 生命周期和并发

参考调用顺序为：

```text
resolve bindings
initialize Variables using file/random services
on_machine_created

repeat:
    on_eval_begin
    eval
    flush eval-scoped deferred output
    on_eval_end

finalize final calls
on_finalize
close Machine-owned resources
```

同一个 Machine 不能重入 `eval()`、`finalize()` 或 Host callback。默认 binding 在调用它的
Machine 线程上执行；并行后端若要并发调用，必须由 binding 显式声明 thread-safe，并且
仍要保持 AM 定义的有副作用调用顺序。

HostEnvironment 的异常不得穿过不支持异常的 ABI 边界。后端应转换为结构化 Error，附带
Machine、Block、instruction、binding 名称和参数 Type 等诊断上下文。

## 8. 最小实现

只运行纯 Logic 且不使用 random/file/system/DPI 的 Program，可以使用空
HostEnvironment。实际 RTL 仿真通常至少需要：

1. SimulationTime 存储；
2. SystemFunction/SystemTask registry；
3. stdout/stderr 和诊断输出；
4. finish/stop/fatal 处理；
5. FileRoot、初始化文件读取和文件句柄；
6. random init source；
7. DPI registry 和 ABI adapter；
8. finalize 与资源清理。

波形、性能计数、远程调用和异步 I/O 可以作为扩展能力，但不能改变没有使用这些扩展时
Program 的 VariableArea 结果或调用顺序。
