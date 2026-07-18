# GrhSIM emit 内部合并与特化

本文只描述当前 `grhsim-cpp` emitter 已经实现的 target-specific lowering。实现集中在
[`grhsim_cpp.cpp`](../../lib/emit/grhsim_cpp.cpp)，不会修改 GRH graph、JSON schema 或
`OperationKind`。

## 当前结论

仓库中**没有**可注册 rule 的通用 `EmitCombineModel` / candidate / validate / commit
框架，也没有 `lib/emit/grhsim/` 子目录。当前实现采用保守的专用识别函数，并把结果
记录在 `EmitModel` 的若干 map/set 中：

| 机制 | 当前状态 | 作用 |
| --- | --- | --- |
| packed-array lane lowering | 默认启用 | 将带 `svPackedArray.*` 元数据的 `concat -> slice_array` 降成 lane array 直接索引 |
| reg-to-mem intent bypass | 输入 GRH 带 `regToMem.intent.*` 时启用 | 跳过可证明无独立用途的 read/concat，直接访问 intent storage row |
| 大宽值 concat 直接生成 | 默认启用 | 对满足阈值的宽 concat 直接向目标 word array 写入各 operand |
| materialization / local expression | 默认启用 | 只持久化跨边界或可观察 value，其余 value 留在 supernode 局部；廉价 scalar 可按条件 inline |
| 同 supernode state-read slot alias | 默认启用，可用环境变量关闭 | 同一 supernode 对同一 state 的重复读共享 materialized slot |
| single-writer state read forwarding | 默认关闭 | 通过环境变量启用后，部分 reader 直接读取 visible state，并调整 activation frontier |

这些机制共享 emitter 的 value storage、change detection 和 activity propagation helper，
但并不构成一个独立、可扩展的 combine overlay。

## Emit 流程中的位置

当前流程可概括为：

```text
读取 GRH + activity-schedule session 数据
  -> 建立 ports、state、write、event、supernode 模型
  -> 建立 input/state/boundary activation 关系
  -> discoverPackedArrayLaneViews()
  -> 规划 persistent/materialized/local values
  -> collectRegToMemIntentBypassOps()
  -> buildSameSupernodeStateReadSlotAliases()
  -> 可选 buildDirectSingleWriterStateReads()
  -> 生成 compute/commit batch C++
```

Emitter 只建立 codegen overlay，不改写 graph。未命中特化的 operation 始终回退到普通
scalar/word lowering。

## Packed-array lane lowering

### 识别的形态

当前只识别 `kConcat`，或仅由一个 `kAssign` 包装的 `kConcat`：

```text
packed = concat(elem[N-1], ..., elem[1], elem[0])
value  = packed                         // 可选的单一 assign wrapper
out    = slice_array(value, index)      // sliceWidth == elementWidth
```

`kConcat` 必须带 ingest 保留下来的 `svPackedArray.*` 元数据：

```text
svPackedArray.version                 == 1
svPackedArray.elementWidth            in [1, 64]
svPackedArray.elementCount            > 1
svPackedArray.indexLow/indexHigh      与 elementCount 一致
svPackedArray.laneOrder               == lsb_index_low | lsb_index_high
svPackedArray.concat.operand0Index
svPackedArray.concat.operandStride    == 1 | -1
```

此外还要求：

- concat operand 数量和每个 operand 的宽度与元数据一致；
- concat/result 宽度等于 `elementWidth * elementCount`，且因此是宽 logic value；
- index 是不超过 64 bit 的 logic value；
- `sliceWidth` 和 slice result 宽度都等于 `elementWidth`；
- packed value 的所有用户都是兼容的 `kSliceArray`；
- packed value不是公开 output/inout，也不是 waveform 强制观察值；
- assign wrapper 存在时，原 concat value 不能另有用户；跨 supernode source boundary
  也必须满足 emitter 的持久化约束。

当前**不**把普通 `kSliceDynamic` 当作 lane fast path。统计里的
`packed_array_slice_dynamic_legacy_users` 只用于观察旧形态；出现它会因 mixed user
回退普通 word lowering。

### 生成语义

命中后，packed bits 不再物化为 `std::array<std::uint64_t, W>`，而是物化为元素数组：

```cpp
std::array<ElemCppType, ElementCount> lanes{};
lanes[lane_of_operand_0] = truncate(elem_0, ElementWidth);
// ...

const std::size_t i = static_cast<std::size_t>(index);
result = i < ElementCount ? lanes[i] : ElemCppType{};
```

`lanes` 可以是 supernode 局部变量，也可以在跨边界时成为类成员。slice result 仍走
普通 assignment helper并精确检测变化；lane base/assign 本身在需要传播时则采用保守
激活，可能多执行后继，但不会漏激活。

这项优化并不要求 operand 来自 register/latch read；它依赖 packed-array 元数据和
consumer 形态，而不是“scalar state array”类型判断。

### 回退规则

以下任一情况都会完整回退到普通 concat/slice 生成：

- 元数据缺失、版本不对、宽度/索引顺序不一致；
- 有公开或 waveform 观察需求；
- 有 `kSliceDynamic`、普通算术或其他 mixed user；
- assign wrapper 不是唯一 consumer；
- source value 具有不兼容的 boundary fanout。

回退不改变 GRH 语义，只可能失去该优化的性能收益。

### Emit 统计

`grhsim_emit_stats.json` 当前只写一个 `packed_array_lane_emit` 对象：

```json
{
  "packed_array_lane_emit": {
    "packed_array_lane_emit_fallback_invalid_shape": 0,
    "packed_array_lane_emit_fallback_mixed_user": 0,
    "packed_array_lane_emit_selects": 1,
    "packed_array_lane_emit_values": 1,
    "packed_array_slice_array_users": 1,
    "packed_array_slice_dynamic_legacy_users": 0,
    "sv_packed_array_attr_concat_defops": 1,
    "sv_packed_array_attr_defops": 1
  }
}
```

它不是全部 materialization、schedule 或 runtime 性能统计的汇总文件。

## Reg-to-mem intent bypass

这是另一条独立的、基于显式属性的 codegen 路径。前置 transform 会用
`regToMem.intent.*` 描述一组 register 与逻辑 row storage 的对应关系；emitter 校验：

- concat、read、slice 属于同一 intent/storage group；
- row、element width/count 和原 register symbol 全部匹配；
- concat 的所有用户都是同组 `kSliceArray` 或 `kSliceDynamic`；
- 被跳过的 read/concat value 不需要 materialize 或 tracked change。

满足条件时，slice 直接读取 intent storage 的目标 row，冗余 read/concat operation
不发射普通 body。这里能支持 `kSliceDynamic`，是因为 index 语义由
`regToMem.intent.*` 明确定义；不要与 packed-array lane 路径混为一谈。

这条路径还会把 index dependency 和 storage reader activation 补回 schedule runtime
模型，避免“代码被旁路后依赖也丢失”。

## 其他局部特化

### 宽 concat

普通宽 concat 满足以下条件时走直接 statement lowering：

```text
result width >= 96
operand count >= 4
result 不是 event value
```

生成代码清零目标 word array，再按每个 operand 的最终 LSB offset 逐 source word 写入
一个或两个目标 word，并截断最高 word。其他 concat 走 `grhsim_concat_*_words` helper；
至少四个 scalar operand 的 concat 会优先使用 scalar-array helper。

### Materialization 和 inline

以下 value 会被持久化：公开 output/inout、waveform/event value、跨 supernode
boundary、commit operand、无本 graph def、非 logic、side-effect result、跨 phase value
以及 reg-to-mem intent index。其余 value 使用 supernode 局部变量；只有符合条件的
单用户廉价 scalar logic 表达式可直接替换到使用点。wide value 当前生成具名局部临时，
不走 single-user inline。

因此“未看到某个 GRH value 的类成员”不表示 operation 被删除，它可能只是局部化或
inline。私有 slot 名称不是稳定 API。

### State read alias 与 forwarding

`WOLVRIX_GRHSIM_STATE_READ_SLOT_ALIASES=0` 可以关闭默认启用的同 supernode 重复
state-read slot 共享。公开、waveform 和 event value不会被 alias。

`WOLVRIX_GRHSIM_DIRECT_SINGLE_WRITER_STATE_READS=1` 可启用 single-writer direct read；
默认关闭。该路径只对满足唯一 writer、相同 state/slot、无受保护观察等条件的 read
生效，并同步调整 state change 后的 reader activation。它是实验性 codegen 开关，
不是 Python API 的稳定参数。

## 不变量

无论走哪条特化，都必须保持：

1. public/waveform/event/commit boundary value 的可观察性；
2. 普通 tracked assignment 只有实际变化时才传播 boundary activity；direct wide
   concat 和 packed-array lane base/assign 允许保守多激活，但不能漏激活；
3. state 只有 visible value 或 memory row 实际变化时才激活 reader；
4. side-effect operation 和 ordered external call 不被局部表达式吞掉；
5. 匹配失败时回退普通 emitter，不产生部分替换。

## 验证

核心生成测试覆盖 lane storage、直接索引、越界返回零和统计字段：

```bash
ctest --test-dir wolvrix/build --output-on-failure -R '^emit-grhsim-cpp$'
```

大设计性能验证还应检查 `grhsim_emit_stats.json` 和生成的 `sched_*.cpp`，但不要依赖
某个固定 schedule 文件编号；batch 参数变化会重新编号和分组源文件。
