# XiangShan GrhSIM 宽值实现

本文说明当前 `grhsim-cpp` emitter 如何存储和生成宽 logic。这里的“宽值”指位宽
大于 64 bit 的 logic value。实现入口集中在
[`grhsim_cpp.cpp`](../../lib/emit/grhsim_cpp.cpp)；本文不把某次生成目录中的私有字段名
或 `sched_N.cpp` 编号当作稳定接口。

## 先给结论

- 宽 logic 的统一值表示是小端 word array：
  `std::array<std::uint64_t, ceil(width / 64)>`，`word[0]` 保存最低 64 bit。
- materialized value 按 word 数分桶，生成固定大小的嵌套 `std::array`；不使用
  `std::vector<std::array<...>>`。
- register/latch state 放在对齐的 byte arena 中，通过 typed reference 访问；memory
  则各自生成固定大小的 row array。
- supernode 内的非 materialized 宽值生成具名局部量，普通 helper 表达式通常使用
  `const auto`，direct concat 使用可写 word array。当前不 inline wide words 表达式。
- 宽运算直接使用 word-level helper；生成代码不使用 `_BitInt`，但 runtime helper
  使用 `unsigned __int128` 实现 carry、除法和不超过 128 bit 的快速路径。
- 当前普通 commit 路径直接更新 visible state，没有额外的 shadow/pending 可见阶段。

## 1. XiangShan 宽度分布只是快照

下面的数据来自工作区文件
`build/xs/grhsim/wolvrix_xs_post_stats.json`，文件时间为 2026-06-16 09:35 +0800。
它是一次 post-transform GRH 快照，不是 emitter 的固定性质；设计、pass 或生成参数变化
后应重新统计。

统计只计 `type == "logic"` 的 result：

| 指标 | 快照值 |
| --- | ---: |
| logic result 总数 | 5,119,863 |
| 宽 logic result（`> 64`） | 42,461（0.829%） |
| 不同 logic 位宽 | 344 |
| 最大 logic 位宽 | 79,263 |

宽 result 中最常见的 defining op 是 `kConcat`（10,691），其次是 `kMux`（5,951）、
`kAssign`（5,573）、`kShl`（3,388）和 `kOr`（2,966）。这说明宽值只占薄尾，但
concat、mux、shift 和 slice 的生成质量会显著影响 XiangShan 模型。

## 2. 存储模型

### 2.1 值类型

`logicCppType(width)` 的映射是：

| 位宽 | C++ 类型 |
| --- | --- |
| 1 | `bool` |
| 2..8 | `std::uint8_t` |
| 9..16 | `std::uint16_t` |
| 17..32 | `std::uint32_t` |
| 33..64 | `std::uint64_t` |
| `> 64` | `std::array<std::uint64_t, ceil(width / 64)>` |

所有产生宽 logic 的 helper 都按声明位宽截断最高 word 的无效 bit。不要把这些 padding
bit 当作设计状态，也不要只读写宽公开端口的 `word[0]`。

### 2.2 Materialized value

需要跨 supernode、进入 commit、公开输出、event/waveform 观察或满足其他持久化条件的
宽 value，会按 word 数放入固定槽位数组。概念上等价于：

```cpp
// W 是 word 数，S 是 emit 时已知的槽位数。
std::array<std::array<std::uint64_t, W>, S> value_words_W_slots_{};
```

槽位数量和私有字段名由当前 graph 与 materialization 决定，不是 testbench API。
不需要持久化的宽 value 留在对应 supernode body 中：

```cpp
const auto local_value = grhsim_add_words(lhs, rhs, width);
```

即使只有一个用户，wide local 当前也保留具名临时量；只有廉价的 scalar local 可能
直接替换到使用点。

### 2.3 State 和 memory

普通 register/latch 的 scalar 与 wide state 共享一个按类型对齐的 byte arena：

```cpp
alignas(std::uint64_t)
std::array<std::byte, kStateLogicStorageBytes> state_logic_storage_{};

auto &state = grhsim_value_storage_ref<
    std::array<std::uint64_t, W>>(state_logic_storage_, byte_offset);
```

每个 memory 则有自己的固定 row array：

```cpp
std::array<std::array<std::uint64_t, W>, RowCount> state_mem_name{};
```

带 `regToMem.intent.*` 的 register group 也可使用按 element 数固定的 row storage。
当前普通 commit 代码直接比较并更新这些 visible state/row；只有值实际变化时才激活
state reader。源码中保留的 shadow/write-buffer 基础设施当前普通生成路径不使用，
不能据此推导额外的 NBA phase。

## 3. 宽运算 lowering

### 3.1 算术、位运算和比较

宽算术与位运算在 `uint64_t` word 上执行。加法按 word 传播 carry：

```text
carry = 0
for i in 0 .. word_count-1:
  sum = u128(lhs[i]) + u128(rhs[i]) + carry
  out[i] = low64(sum)
  carry = sum >> 64
truncate_tail(out, width)
```

部分不超过 128 bit 的输入会先尝试 `unsigned __int128` 快速路径；更宽的加、乘、除、
模等操作回退到显式多 word 算法。bitwise、比较、reduction 和 cast 同样保持 word array
表示，不经 `_BitInt` 中转。

### 3.2 Shift 和 slice

shift 把位移量拆成 `wordShift` 和 `bitShift`，再跨相邻 word 合并。动态 slice 同样用
`start / 64` 和 `start % 64` 找到起始 word：

```text
for each destination word i:
  out[i] = src[src_word + i] >> bit_shift
  if bit_shift != 0:
    out[i] |= src[src_word + i + 1] << (64 - bit_shift)
truncate_tail(out, result_width)
```

越过输入范围的部分补零；signed cast/shift 另按声明语义做符号扩展。

### 3.3 Concat 和 replicate

普通 concat 按 GRH operand 顺序从 MSB 向 LSB 移动 cursor，再把每个 operand 插入目标
word array。当前有三类路径：

| 条件 | 生成方式 |
| --- | --- |
| result `>= 96` bit、operand 至少 4 个、result 不是 event | 直接清零目标 array，并生成逐 operand、逐 word 的插入 statement |
| 至少 4 个 scalar operand | 使用 uniform/general scalar-array concat helper |
| 其他情况 | 使用通用 words concat 表达式/helper |

直接 concat 路径概念上是：

```text
out = zero_words
cursor = result_width
for operand in operands:
  cursor -= operand.width
  insert_bits(out, cursor, operand)
truncate_tail(out, result_width)
```

不要用固定的 `grhsim_SimTop_sched_N.cpp` 路径举证。schedule batch 和文件数量会随
graph、选项和 emitter 版本重新编号、重新分组。

### 3.4 Packed-array lane 特化

带 `svPackedArray.*` 元数据且只被兼容 `kSliceArray` 使用的 concat，可以不建立 packed
word array，而改成 element lane array 后直接按 index 读取。它与通用宽值 helper 是
并列的 emitter 特化，完整约束见
[GrhSIM emit 内部合并与特化](grhsim-emit-combines.md)。

## 4. Change propagation

普通 materialized 宽 assignment 会先计算 `next_words`，逐 word 比较并写回，只有实际
变化才传播 boundary activity。两条专用路径更保守：

- direct wide concat 清零并重写目标后，会直接激活其 fanout；
- packed-array lane base/assign 也可直接激活 fanout。

这些路径可能多执行后继，但不能漏激活。visible register/latch/memory state 仍按实际
变化激活 reader。

## 5. 构建与检查

生成 Makefile 使用 Clang 风格的 PCH 参数 `-include-pch`。GNU make 自带的 `CXX`
默认值可能覆盖文件中的 `CXX ?= clang++`，所以应显式指定：

```bash
make -C build/xs/grhsim/grhsim_emit CXX=clang++ -j"$(nproc)"
```

验证宽值改动时至少检查：

1. 生成测试覆盖标量/宽值边界、tail truncation、concat、shift、slice 和 memory row；
2. 运行结果或 difftest 与参考模型一致；
3. 生成代码中没有意外的大 packed concat 物化或失控的源文件膨胀；
4. 性能分析按 op 形态和 materialization 解释，不把上述一次宽度快照当作永久分布。
