# <pass-id>

> 类别：<rewrite / partition / schedule / analyze>（分类规则见
> [Pass 系统与流水线](../grhsim-ir-pipeline.md) 第 6 节）
> effects：`mutatesGraph = <bool>`，`preservesSchedule = <bool>`
> 注册名：`<pass-id>`；实现：`<源码路径>`

## 1. 功能

一句话：做什么，为什么做（性能动机或正确性动机）。

## 2. 变换规则

输入形态 → 输出形态：匹配的子图模式、重写结果、适用条件（何时不适用）。
附一段小的 GRHSIM IR 文本例子（before / after）。

## 3. 参数

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `<arg>` | `<int/bool/string>` | `<默认>` | <说明> |

无参数则删除本节。

## 4. 前置与后续

- 前置假设：进入本 pass 时 IR 应满足的状态（方言、是否已划分/已调度、
  依赖的注解）；
- 典型后续 pass。

## 5. 诊断

报错与警告的触发条件；是否可能使 pass 失败（`failed = true`）。

## 6. 示例

（可选）完整的 before / after 片段，或指向测试用例。
