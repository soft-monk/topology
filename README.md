# topology · 链路拓扑引擎

> **节点类型靠名称子串推断；链路"优化"是改数据库；曲线是写死的公式；状态没有迟滞会抖。**

本仓当前为**立项阶段**：只冻结了需求，尚未开始实现。

---

## 它解决什么问题

节点类型靠名称子串推断；链路"优化"是改数据库；曲线是写死的公式；状态没有迟滞会抖。

## 做 / 不做

| 做 | 不做 |
|---|---|
| 拓扑模型（网状 / 多层，节点坐标可注入） | 不用名称推断节点类型 |
| 质量聚合（事件归一到引擎、滑动窗口） | 不产出颜色语义（只出状态码） |
| 状态机与迟滞、最小驻留、变更日志 | 不做设备收包与健康统计 |
| 网络评估（结构化、可复算） | 不做曲线绘制 |
| 受约束优化建议（幂等、不累积） |  |

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/需求/topology需求专篇.md`](docs/需求/topology需求专篇.md) | 需求专篇（唯一权威）：条目编号、验收标准、边界、决策记录、风险、验收清单 |
| [`docs/契约/topology契约.md`](docs/契约/topology契约.md) | 接口契约：公开类型与方法、规则包 schema、关键口径、需求覆盖、开放问题 |
| [`docs/实现报告.md`](docs/实现报告.md) | 实现报告：构建与验收结果、交付物清单、缺陷处置对照、未决问题 |
| [业务引擎需求专篇 · 汇总索引](https://github.com/soft-monk/phase-engine/blob/main/docs/需求/业务引擎需求专篇_汇总索引.md) | 十个业务引擎的索引、共性口径、跨模块冲突与建仓顺序 |

## 状态


- 需求：已冻结（见上表）
- 契约（接口）：首版（`CTR-TPL-001` v1.0）
- 实现：**已完成首版**（34 条需求全部落地；自测 36 用例 / 868 断言全绿，验收脚本 48 项全绿）
- 规则包：`policies/mapapp/linkThresholds.json`

## 快速开始

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\bin\Release\selftest.exe                              # 零依赖自测（退出码 0/1）
.\build\bin\Release\example_full_flow.exe                     # 全流程示例
powershell -ExecutionPolicy Bypass -File scripts\acceptance.ps1 -SkipBuild   # 独立验收（退出码 0/1）
```

## 定位


这是一个**业务引擎**，与其它模块**互不 import**，只通过注入的反向接口与宿主装配。与业务相关的内容（阶段名、型号、权重、文案、阈值）一律通过规则包注入，不写进本仓代码。

## 许可


Apache License 2.0，见 [LICENSE](LICENSE)。
