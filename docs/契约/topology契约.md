# topology · 链路拓扑引擎契约（v1.0）

| 项 | 内容 |
|---|---|
| 文档编号 | `CTR-TPL-001` |
| 版本 | **v1.0（首版，实现已落地）** |
| 适用范围 | `topology`：**后端 C++17 库**（无 Web 框架、无 SQL、无其它引擎内部依赖）。拓扑模型、质量聚合、状态机与迟滞、网络评估、优化建议、曲线 |
| 需求依据 | [`../需求/topology需求专篇.md`](../需求/topology需求专篇.md)（`TPL-MODEL/Q/ST/EVAL/OPT/NFR`，共 **34** 条） |
| 上游共享契约 | [`protocol.md`](../../../phase-engine/docs/契约/protocol.md) v1.0（P1–P10、§1.4 `PhaseContext`、§3 错误码、§4.4 `topology.changed`、§5 `policies`、§6 反向接口命名） |
| 冲突裁决 | [`冲突裁决.md`](../../../phase-engine/docs/契约/冲突裁决.md)（**C15** 废弃 `1001`、**C16** 收窄 `1002`；幂等成功一律 `code=0` + `data.idempotent=true`） |
| 消费侧图元契约（只读） | [`map-2d/doc/接口文档.md`](../../../map-2d/doc/接口文档.md) §3.3（`LinkItem{id,from,to,state?}`、`ClusterItem{id,lng,lat,name}`） |
| 唯一公开头 | `#include <topology/topology_engine.h>` |
| 命名空间 | `topology` |
| 构建 | CMake ≥ 3.20；目标 `topology`（静态库）+ `example_minimal` / `example_full_flow` + `selftest`（ctest） |
| 依赖 | **仅 `nlohmann/json`**（系统包优先，缺失时回落内置单头 `third_party/nlohmann/json.hpp`） |

---

## 0. 三条不可协商的口径

1. **引擎不认识任何业务名词。** 节点类型、层级、结构（网状 / 多层）、指标集、绿黄红阈值、
   迟滞参数、最小驻留、评估算法与权重、可调参数与步长、曲线粒度**全部**来自规则包
   （`kind: "linkThresholds"`）。节点类型**只由规则声明的 `typeKey` 判定**，MUST NOT 用名称子串推断
   （TPL-MODEL-01）。
2. **引擎不落库、不广播、不取系统时间。** 出口为反向接口：`ITopologySink` / `ITopologyStore` /
   `IClock`（宿主 MUST 注入）与 `ILogSink` / `ILayoutProvider`（可选）；未注入时引擎仍 MUST 可工作
   （纯内存 + 系统时钟 + 空 Sink）。
3. **失败 MUST NOT 抛异常跨边界**（P10）：一切裁决走 `{code,message,data}` 信封（protocol §3.1）。

---

## 1. 公开接口（唯一公开头）

### 1.1 类型

| 分组 | 类型 |
|---|---|
| 枚举 | `ErrorCode`（`0/1000/1002/1003/1004/1005/1006`，逐值对齐 protocol §3.2；**不产生 1001**）、`LinkState{Green,Yellow,Red}`、`MetricDirection{Higher,Lower}`、`StructureMode{Mesh,Layered}`、`NodeAggregation{Worst,Mean}` |
| 共享形状 | `PhaseContext`（`phaseKey / seq / scenarioKey / enteredAt / missionId`，protocol §1.4 逐字；引擎**不 import** `phase-engine`，按共享契约自行声明） |
| 规则形状 | `NodeTypeDef`、`LayerDef`、`StructureDef`、`MetricDef`、`StateBandDef`、`HysteresisDef`、`WindowDef`、`CoverageDef`、`MeshComponentDef`、`MeshProgressDef`、`EvaluationTermDef`、`RequirementDef`、`EvaluationItemDef`、`EvaluationDef`、`OptimizationParamDef`、`OptimizationDef`、`CurveDef`、`PolicyConfig`、`PolicyInfo`、`LoadIssue`、`LoadResult` |
| 拓扑模型 | `NodeSpec`、`EdgeSpec`、`TopologyNode`、`TopologyEdge`、`TopologyView`、`ValidationIssue`、`ValidationReport`、`MutationResult` |
| 质量聚合 | `MetricAggregate`、`StateChange`、`ManualOverride`、`LinkQuality`、`NodeQuality`、`DerivedMetrics`、`NormalizedObservation`、`IngestResult` |
| 评估 / 优化 / 曲线 | `EvaluationTerm`、`EvaluationItem`、`EvaluationResult`、`OptimizationSuggestion`、`OptimizationResult`、`CurvePoint`、`CurveSeries` |
| 反向接口与自述 | `ITopologySink`、`ITopologyStore`、`IClock`、`ILogSink`、`ILayoutProvider`、`SystemClock`、`AuditEntry`、`TopologyChangedEvent`、`LayoutRequest`、`LayoutPoint`、`Capabilities`、`Metrics`、`TopologyEngineOptions` |

### 1.2 方法

| 分组 | 方法 |
|---|---|
| 规则 | `loadPolicy(json)`、`loadPolicyFile(path)`、`●validatePolicy(json)`、`policyInfo()`、`policy()` |
| 模型 | `configureTopology(id, structureKey, reset)`、`addNode(s)`、`addNodes`、`addEdge`、`addEdges`、`removeEdge`、`removeNode`、`topologyView()`、`node(id)`、`edge(id)`、`validate()`、`exportSnapshot()`、`importSnapshot(snapshot, strict)`、`loadSnapshotFromStore(id)` |
| 质量 | `ingest(event)`、`ingestBatch(events)`、`linkQuality(id)`、`nodeQuality(id)`、`linkQualities()`、`derived()`、`normalizedShape(event)` |
| 状态机 | `linkState(id)`、`setOverride(...)`、`clearOverride(...)`、`overrides()`、`stateLog(linkId)`、`setPhaseContext(ctx)`、`phaseContext()` |
| 评估/优化/曲线 | `evaluate(ctx)`、`linkStability()`、`optimize(ctx, linkId)`、`curves(linkId)` |
| 消费侧适配 | `primitives()` |
| 注入与自述 | `setStore/setClock/setSink/setLog/setLayout`、`capabilities()`、`metrics()`、`●errorCodeName(code)`、4 个 `toString`、4 个 `*FromString`、`toJson(...)` |

### 1.3 错误码映射（本引擎产生场景）

| code | 场景 |
|---|---|
| `0` | 成功；**幂等命中**另带 `data.idempotent=true`（重复无向边、重复覆盖、重复解除、快照无问题） |
| `1000` | 参数非法：缺 `topologyId`/`linkId`/`edgeId`；**自环**；**悬挂边（端点不在节点集）**；**层级越级连接**；未声明的节点类型 / 结构；重复边 id；重复节点 id；规则包缺必填字段 |
| `1002` | 冲突拒绝：拓扑已配置且未 `reset`；节点仍有边；人工覆盖已存在且未 `replace` |
| `1003` | 前置条件未满足：规则包未声明搜索空间（`no-search-space`）、可调参数无窗口基线（`missing-baseline`） |
| `1004` | 链路 / 节点 / 边不存在；快照在 store 中不存在；事件引用的链路不在拓扑内 |
| `1005` | 规则包未装载；store 未注入 |
| `1006` | 规则包 `schemaVersion` 的 `MAJOR` 不受支持（MUST NOT 静默降级） |
| `1001` | **不产生**（ADR-C15-01） |

---

## 2. 规则包 `policies/mapapp/linkThresholds.json`

### 2.1 骨架（protocol §5.1）

```jsonc
{
  "policiesNamespace": "mapapp",
  "schemaVersion": "1.0.0",
  "kind": "linkThresholds",
  "items": [ /* 节点类型声明 */ ],
  /* … 兄弟段：structures / states / hysteresis / metrics / coverage / meshProgress /
       aggregation / evaluation / optimization / curves … */
}
```

| 段 | 必填 | 内容 |
|---|---|---|
| `items[]` | MUST | 节点类型：`{key, name, tier, graphic?}`。**类型判定只看 `key`**，`name` 只用于展示（改 `key` 才改类型；改 `name` 不影响） |
| `structures[]` | SHOULD（缺省回落单一平铺结构） | `{key, mode: mesh\|layered, layers[]?, adjacentOnly?}`；`layered` 的**层数与层间关系由规则声明** |
| `states[]` | MUST | 三个分档：`{key: green\|yellow\|red, min}`（归一化得分 ≥ `min` 落入该档）。**引擎无内置阈值** |
| `hysteresis` | MUST | `{riseMargin, fallMargin, confirmCount, minDwellMs}`；装载时校验"确实产生迟滞"（至少一个 margin > 0 或 confirmCount > 1），否则拒绝装载（TPL-ST-02 在装载期守住） |
| `metrics.window` | SHOULD | `{windowMs, maxSamples}`，缺省 1000 ms（对齐 `TR-MSG-06` 1 s 刷新） |
| `metrics.items[]` | MUST | `{key, ledgerKey?, unit?, direction: higher\|lower, min, max, weight, valueMap?}`；`ledgerKey` 缺省由引擎按驼峰→下划线派生；`weight = 0` 表示只聚合/出曲线、不参与分档评分 |
| `coverage` | SHOULD | `{mode: sum\|max, source: <metricKey>, targetAreaKm2}`：覆盖率 = 聚合覆盖 / 目标面积（显式、可复算） |
| `meshProgress` | SHOULD | `{scale, unit, components: [{source, weight}]}`：组网进度 = Σ(weight × 派生输入) / Σweight × scale |
| `aggregation.node` | SHOULD | `worst`（取最不利关联边）\| `mean`（关联边均值），缺省 `worst` |
| `evaluation` | MUST | `{stabilityKey, items: [{key, weight, scale{min,max}, terms: [{source, weight}], requirement?}]}`；`stabilityKey` 声明"链路稳定度"是哪个评估项（引擎不内建评估项名） |
| `optimization` | SHOULD | `{objective{kind, items}, minImprovement, maxIterations, maxMs, parameters: [{metric, min, max, step}]}`；**步长必须由规则给定**（引擎内无乘性常量） |
| `curves` | SHOULD | `{granularityMs, maxPoints, metrics[]}`，缺省取全部参与评分的指标 |

### 2.2 派生输入（闭集，引擎提供、规则引用）

固定项：`stateScoreMean`、`worstScore`、`scoreSpread`、`greenRatio`、`yellowRatio`、`redRatio`、
`linkUpRatio`、`stateStability`、`nodeCoveredRatio`、`coverageRatio`、`meshProgress`、
`linkCount`、`nodeCount`、`stateChangeCount`、`observationCount`、`windowSamples`。

逐指标项：`metric.<key>`（跨链路窗口均值）、`metricScore.<key>`（其上归一值，仅 `weight > 0` 的指标）。

> 未在闭集内的 `source` → **拒绝装载整包**并逐条给原因（`evaluation.terms[i].source` / `meshProgress.components[i].source`）。

### 2.3 装载纪律（CTR-PL-01..08）

- 失败 MUST 拒绝**整包**并逐条给"路径 + 字段 + 原因"（`issues[]`）。
- 失败 MUST NOT 破坏上一次成功装载的规则（**原子替换**）。
- 未知字段 / 未知顶层段 → **装载成功** + 计入 `PolicyInfo.warnings` 与 `metrics().unknownFields`。
- `MAJOR` 不符 → `1006`。
- 规则包 MUST NOT 含可执行代码（纯数据）；引用一律用 `key`（MUST NOT 用显示名匹配）。
- `PolicyInfo.digest` = FNV-1a 64（规范化 JSON：对象键 ASCII 升序、数组保序）；`policyVersion = "<ns>:<schemaVersion>:<digest>"`。

---

## 3. 关键口径

### 3.1 事件归一（TPL-Q-02/03）

输入就是契约 §4 的**驼峰事件**（`{linkId, from, to, ts?, signal, bandwidthMbps, latencyMs, lossRate,
coverageKm2, meshProgress, state?}`）。引擎内完成：

- `metrics` → 规范形状（camelCase，指标 key 由规则声明）；
- `ledger` → 台账形状（snake_case，`ledgerKey` 由规则声明，缺省引擎派生）；
- **缺字段 → 保持原值**（列入 `preserved`，计数 `metrics().preservedFields`）；
- **无法解释的取值**（如未在 `valueMap` 内的文本）→ **保持原值**（列入 `rejected`），MUST NOT 用 0 覆盖；
- 事件自称的 `state` 只回显为 `reportedState`，**判定归引擎**（阈值来自规则，不采信上报值）；
- 非指标字段列入 `ignored`（引擎不解释）。
- `normalizedShape(event)` 是**纯函数式**入口（不改状态），供宿主/前端确认"单一形状"。

### 3.2 状态机与迟滞（TPL-ST-01..06）

- 得分 `score = Σ w_i · norm_i(value_i) / Σ w_i`，只算 `weight > 0` 且有窗口样本的指标；
  `norm` 按规则声明的方向与有效范围归一化（越大越好 / 越小越好）。
- 原始档 = `states[]` 中 `score ≥ min` 的最高档。
- **迟滞**：向上迁移需 `score ≥ 目标档 min + riseMargin`；向下迁移需 `score < 当前档 min − fallMargin`；
  且 MUST 连续 `confirmCount` 次确认。
- **最小驻留**：状态变更后 `minDwellMs` 内 MUST NOT 再次迁移（确认次数继续累积，驻留期满即迁移）。
- 时间基准 = 事件的 `ts`（缺省取注入时钟），一次判定恰好读一次窗口状态。
- **变更留痕**（`StateChange`）：`ts / linkId / from / to / score / threshold / margin / metric / value /
  normalized / confirmations / manual / reason / checks`，其中 `checks.metrics` 含**当时全部指标的数值与归一值**
  —— 判定过程可逐项复原（`checks.score` 可由 `checks.metrics` 独立复算）。
  `metric` 的语义：**下行取最弱指标、上行取最强指标**（越阈的"决定指标"）。
- `reason` 为机制原因码：`hysteresis-confirmed` / `manual-override` / `override-cleared`（MUST NOT 是文案）。
- **首次落档不算"变更"**：链路在拿到第一个可判定样本时直接落入当前档，不写日志、不发事件
  （事件语义 ="变化"）；初始状态由 `topologyView()` / `linkQuality()` 提供。
- **无数据不判定**：没有窗口样本时 MUST NOT 编造状态（`hasState = false`）。
- **手动覆盖**（TPL-ST-06）：覆盖期间自动判定不生效；解除后立即恢复自动（不等迟滞确认）；
  重复设同一状态 / 重复解除 = 幂等成功（`code=0` + `idempotent=true`）；改判需 `replace=true`（否则 `1002`）。

### 3.3 网络评估（TPL-EVAL-01..04）

- 评估项 = 声明项与权重的加权平均，再按 `scale` 归一到 `[0,1]`；`overall` = 各评估项归一值的加权平均。
- 输出**结构化**：`items[].terms[]` 带 `source/weight/value/contribution`；`inputs` 是派生输入快照 → 任一数值可手算复核。
- 输出**不含自然语言结论**（无文案；阈值提示是 `{metric, op, limit, actual, satisfied}` 结构化判定）。
- `linkStability()` 只读取值（不取时钟、不计数、不改状态），供 `scoring` 作为"链路稳定度"输入项；
  **具体是哪个评估项由规则包 `evaluation.stabilityKey` 声明**（引擎 MUST NOT 内建评估项名）。
- `EvaluationResult` 回显 `PhaseContext` 的 `missionId / phaseKey / scenarioKey`（引擎不解释阶段语义）。

### 3.4 优化（TPL-OPT-01..06）

- **只产出建议值**，MUST NOT 改状态、MUST NOT 落库、MUST NOT 广播（`optimize()` 前后快照逐字节一致）。
- 搜索空间完全来自 `optimization.parameters[]`（可调指标、上下限、**步长**）；
  候选值 = 当前值 ± 整数倍步长并夹在 `[min,max]`；方向由指标的 `direction` 决定。
- 目标 = `objective.items` 指定的评估项归一值加权平均（空 = 全部评估项）。
- **单调**：仅当 `after.overall > before.overall` 且改进量 ≥ `minImprovement` 才接受；否则
  `improved=false` + `reason="no-improvement"` + **空建议**，且 `after == before`（MUST NOT 变差）。
- **幂等**：建议是（当前窗口聚合 + 规则）的纯函数 → 连续 10 次调用结果逐字节一致（无累积乘数）。
- 解释：`suggestions[]` 带 `metric/current/suggested/delta/steps` 与逐评估项的 `impact{before,after,delta}`。
- `before/after` 都是**投影估计**（代表链路值表），不是真实状态（真实状态不变）。
- 上限与时限（R4）：`maxIterations` / `maxMs`（以注入时钟计量）；命中上限 → `truncated=true` 但仍返回已得最优。

### 3.5 曲线（TPL-OPT-06）

- 曲线点**由真实滑动窗口聚合产生**：每 `granularityMs` 追加一个点，`values` = 各指标**当时的窗口聚合值**，
  `samples` = 参与聚合的样本数；环形缓冲保留最近 `maxPoints` 个点。
- 引擎内 MUST NOT 出现任何曲线公式（验收脚本对 `exp(` / `-3.2` / `-2.8` 做零命中检查）。

### 3.6 确定性

同一（规则包字节 + 输入序列 + 假时钟）→ 输出**逐字节一致**：节点按 id 升序、边按装载顺序、
派生输入按固定顺序、JSON 键序 = 成员声明顺序；不使用 `unordered_map` 遍历顺序、指针、随机数、
进程/线程号、本地时区。

---

## 4. 反向接口与事件

| 接口 | 方法 | 说明 |
|---|---|---|
| `ITopologySink` | `onTopologyChanged(const TopologyChangedEvent&)`（MUST 实现） | = `topology.changed` 事件的 `data`：`{missionId, edgeId, from, to, state, metrics}` + 只增字段 `ts`；`metrics` 含 `score/threshold/margin/metric/value/normalized/confirmations/manual/reason`（迟滞判定结果） |
| | `onStateChanged(const StateChange&)`（可选） | 迟滞判定留痕，**不新增事件名**；宿主可落库/日志 |
| `ITopologyStore` | `save/load/remove(topologyId, json)` | 快照持久化；引擎不接触 SQL |
| `IClock` | `nowMs()` | epoch 毫秒；未注入回落 `SystemClock` |
| `ILogSink` | `log/commandAudit` | 人工覆盖与解除的留痕（可选） |
| `ILayoutProvider` | `resolve(LayoutRequest, LayoutPoint&)` | 坐标来源注入（场景给定坐标优先） |

**事件与一致性**：

- 引擎**不产生 WS 信封**（宿主 `realtime-hub` 侧包 `{type,data,ts}`）；`ts` MUST 用引擎给的值。
- `missionId` 来自 `setPhaseContext(ctx)`（宿主经 `phase-engine::phaseContext()` 取到后转交）。
- Sink 抛出的异常被吞掉并计入 `metrics().sinkErrors`，MUST NOT 影响判定结果。
- 出口 MUST 立即返回、MUST NOT 阻塞（不做网络 IO / 等锁 / 落库）。

---

## 5. 消费侧适配

| 产物 | 映射 |
|---|---|
| `topologyView().edges[]` | `map-2d` `LinkItem` 的 `id` / `state`（`from`/`to` 由 `primitives()` 给坐标） |
| `primitives().links[]` | `LinkItem{id, from:[lng,lat], to:[lng,lat], state?}`（无坐标的链路列入 `missingCoordinates`，MUST NOT 编造坐标） |
| `primitives().clusters[]` | `ClusterItem{id, lng, lat, name}`；**哪些节点类型是集群图元由规则 `graphic` 声明**（`"cluster"`） |
| `primitives().graphics[]` | 其余图元的 `{id, graphic, lng?, lat?}`，供宿主 / `view-composer` 适配 |
| 状态取值 | 只有 `green`/`yellow`/`red` **状态码**；颜色到像素的映射属 `map-2d` 主题（D4） |

---

## 6. 需求覆盖对照（34 条）

| 域 | 条目 | 落点 |
|---|---|---|
| `TPL-MODEL` 6 | 01/02/03/04/05/06 | 01：`items[].key` 判定 + 验收强制改名不变（C09/C09b/C10）；02：`structures[]`（mesh/layered，层数由规则声明）；03：`ILayoutProvider` + 节点自带坐标；04：无向去重（幂等成功）+ 自环拒绝；05：`validate()` + `addEdge` 定位性拒绝（悬挂/越级）；06：`exportSnapshot`/`importSnapshot`（严格 / 容错） |
| `TPL-Q` 6 | 01/02/03/04/05/06 | 01：`metrics.items[]`（key/单位/方向/范围/权重/valueMap）；02：引擎内驼峰→台账归一（`normalizedShape`）；03：保持原值（`preserved`/`rejected`）；04：滑动窗口均值/极值；05：`nodeQuality`（worst/mean 可配）；06：`coverage` + `meshProgress` 显式口径 |
| `TPL-ST` 6 | 01/02/03/04/05/06 | 01：`states[]` 阈值；02：`hysteresis`（装载期强制生效）；03：`minDwellMs`；04：`StateChange` + `checks`；05：`LinkState` 三值 + 无色值检查；06：`setOverride`/`clearOverride` |
| `TPL-EVAL` 4 | 01/02/03/04 | 01：`evaluation.items[]`（算法+权重+输入快照）；02：结构化无文案（ASCII 断言）；03：`linkStability()` 只读；04：`requirement` 结构化判定 |
| `TPL-OPT` 6 | 01/02/03/04/05/06 | 01：无硬编码常量（C13）+ 步长驱动；02：`optimization.parameters[]` + 只出建议；03：纯函数幂等 + 闭环收敛；04：`suggestions[]` + `impact`；05：单调 + `no-improvement`；06：真实窗口曲线（C14） |
| `TPL-NFR` 6 | 01/02/03/04/05/06 | 01：零依赖 + 内置单头回落构建（C04/C12）；02：反向接口注入（C20）；03：`IClock` 假时钟；04：确定性双跑；05：独立构建/示例/测试/验收脚本；06：1000 边 P95 ≤ 1 ms |

---

## 7. 开放问题（需人类裁决，不阻塞首版）

| # | 问题 | 影响 | 本实现的取法 |
|---|---|---|---|
| R-TPL-01 | **protocol.md §5.1 要求 `items` 必填，而 §5.5 的 `linkThresholds` 骨架示例没有 `items`（只有 `nodeTypes`/`states`/`hysteresis`）** | 规则包 schema | 本引擎取 `items[]` = 节点类型清单（满足 §5.1），并保留 `states`/`hysteresis` 兄弟段（对齐 §5.5）。建议 protocol 下次修订时统一措辞 |
| R-TPL-02 | 需求 §7 写"**全仓**检索…零命中"，但业务取值与协议事件字段名必然出现在规则包与测试数据里 | 机检脚本口径 | 按 phase-engine 的既有澄清（§12.4 #1）收窄为"**引擎产物**（include/src/examples/scripts/CMakeLists）零命中 + 规则包与测试数据作为合法住所的正向对照（C09b）"，并明确 `docs/` 豁免 |
| R-TPL-03 | 需求 §0 写"共 **27** 条需求"，§8 与 §3 实际是 **34** 条 | 追踪对账 | 按 §8 的 34 条实现与对账（本次验收按 34 条） |
| R-TPL-04 | 绿/黄/红阈值的**权威取值**与依据哪几个指标（§9 待确认 1） | `states` / 指标权重 | 规则包给出可算通的一组（green 0.75 / yellow 0.45，权重 0.25/0.25/0.25/0.15/0.10），**取值属规则包内容**，可随裁决替换 |
| R-TPL-05 | 四项评估的算法与权重（§9 待确认 3） | `evaluation` | 用派生输入的线性加权（可手算），权重 0.35/0.25/0.20/0.20；换权重即换结论 |
| R-TPL-06 | 链路的"初始状态"与"首个样本落档"是否应广播（事件语义 = 变化） | 宿主观感 | 本实现：首次落档不发事件（初始状态经 `topologyView()`/`linkQuality()` 取）；如需广播，宿主可在装载后主动查询一次 |
| R-TPL-07 | 最小驻留期内"确认次数继续累积"是否符合预期 | 迟滞行为 | 本实现：累积（驻留期满即刻迁移）；备选是"驻留期内不计数" |
| R-TPL-08 | `telemetry.link.quality` 事件自带 `state` 字段，引擎是否应采信 | TPL-ST-01 权威性 | 本实现：**不采信**（只回显 `reportedState`），状态一律由规则阈值判定 |
| R-TPL-09 | `topology.changed` 负载需要 `missionId`，而引擎只能在 `setPhaseContext()` 后给出 | 宿主装配 | 本实现：`missionId` 取自 `setPhaseContext()` 注入的 `PhaseContext`；未注入时为空串 |
| R-TPL-10 | 前端 `useStore.ts` 的 link 归一逻辑（D3 要求删除）属**其它仓库** | TPL-Q-02 全链路 | 引擎已提供统一形状（`normalizedShape`/`ledger`），前端改造须在 mapApp 仓进行（本波 MUST NOT 改） |
