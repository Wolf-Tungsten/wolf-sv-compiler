# <方案名>

> 定位：<后端 / 线程模型 / 目标场景>
> 状态：<规划 / 可用>；入口：<Python 函数或脚本路径>

## 1. 目标与思路

方案的核心思想：如何用三类图操作（子图替换 / 图划分 / 顺序调度，见
[GRHSIM IR](../grhsim-ir.md) 第 4 节）组织优化；生成代码的运行时形态是什
么。

## 2. pass 编排

| # | pass | 类别 | 作用 |
| --- | --- | --- | --- |
| 1 | `<pass-id>` | <rewrite/partition/schedule/analyze> | <一句话> |

排序约束：所有 rewrite / lower 类 pass 必须在 schedule 类 pass 之前——
改写图结构的 pass 不保持已有 Schedule（`preservesSchedule` 契约，见
[Pass 系统与流水线](../grhsim-ir-pipeline.md) 第 2 节）。

## 3. IR 形态演变

| 阶段完成后 | 方言 | Schedule | 存储 |
| --- | --- | --- | --- |
| 进入 | generic | 未调度 | 平凡布局 |
| … | | | |

## 4. 运行时形态

生成的 eval 结构伪代码。

## 5. 性能要点

本方案依赖哪些优化生效，各自的收益来源。

## 6. 已知限制

不适用场景、尚未支持的特性、后续扩展方向。
