// topology · topology_engine.h —— 唯一公开头文件
//
// 需求依据：docs/需求/topology需求专篇.md（TPL-MODEL 6 / TPL-Q 6 / TPL-ST 6 /
//           TPL-EVAL 4 / TPL-OPT 6 / TPL-NFR 6，共 34 条）
// 上游共享契约：phase-engine/docs/契约/protocol.md v1.0
//           （P1–P10、§1.4 PhaseContext、§3 错误码、§4 事件名、§5 policies、§6 反向接口命名）
// 冲突裁决：phase-engine/docs/契约/冲突裁决.md（C15 废弃 1001 / C16 收窄 1002）
// 消费侧图元契约（只读）：map-2d/doc/接口文档.md §3.3
//           （LinkItem{id,from,to,state?:green|yellow|red}、ClusterItem{id,lng,lat,name}）
//
// 三条不可协商的口径：
//   1. 引擎不认识任何业务名词。节点类型、层级、指标集、阈值、迟滞参数、评估算法、
//      搜索空间、曲线粒度**全部**来自注入的规则包（kind:"linkThresholds"）。
//      节点类型**只由规则声明的 key 判定**，MUST NOT 用名称子串推断（TPL-MODEL-01）。
//   2. 引擎不落库、不广播、不取系统时间。出口为反向接口：ITopologySink / ITopologyStore /
//      IClock（MUST 注入）与 ILogSink / ILayoutProvider（可选）；未注入时引擎仍 MUST 可工作。
//   3. 失败 MUST NOT 抛异常跨边界（P10）：一切裁决走 {code,message,data} 信封（protocol §3.1）。
//
// 宿主只允许 #include <topology/topology_engine.h>；其余头文件是内部实现细节。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace topology {

/// 引擎的 JSON 类型（键序可控，保证"同输入同输出"逐字节可比）。
using json = nlohmann::ordered_json;

// 引擎支持的 policies MAJOR（protocol §5.2：不匹配 → 拒绝装载 + 1006，MUST NOT 静默降级）
inline constexpr int kSupportedPoliciesMajor = 1;
// 引擎版本（capabilities 自述用）
inline constexpr const char* kEngineVersion = "0.1.0";

// ============================================================================
// §1 共享形状与枚举
// ============================================================================

/// protocol.md §3.2 码表的子集（逐值对齐）。`1001` 保留不用（ADR-C15-01）。
enum class ErrorCode {
    Ok = 0,
    BadRequest = 1000,
    Conflict = 1002,
    GateUnmet = 1003,
    NotFound = 1004,
    Internal = 1005,
    VersionMismatch = 1006,
};

/// 链路状态三值枚举（TPL-ST-05）。JSON：green/yellow/red —— **只出状态码，不出颜色值**（D4）。
enum class LinkState { Green, Yellow, Red };

/// 指标方向：越大越好 / 越小越好（TPL-Q-01）。
enum class MetricDirection { Higher, Lower };

/// 拓扑结构模式：平铺网状 / 多层（TPL-MODEL-02）。
enum class StructureMode { Mesh, Layered };

/// 节点质量派生口径：取最差关联边 / 取关联边均值（TPL-Q-05，可由规则改）。
enum class NodeAggregation { Worst, Mean };

/// “当前阶段”的唯一入参形状（protocol.md §1.4 逐字）。
///
/// 为什么本仓自己也声明一份：protocol.md P1 要求**引擎之间 MUST NOT 互相 import**，
/// 因此形状按共享契约各自声明，字段名/类型逐字一致，由宿主把 phase-engine 的
/// `phaseContext()` 结果转交过来。
struct PhaseContext {
    std::string phaseKey;
    int seq = 0;
    std::string scenarioKey;
    int64_t enteredAt = 0;  // epoch ms
    std::string missionId;
};

// ============================================================================
// §2 规则形状（policies/mapapp/linkThresholds.json，kind:"linkThresholds"）
// ============================================================================
//
// 段划分（同一文件的顶层兄弟段，遵循 protocol.md §5.5 的既有惯例）：
//   items[]        节点类型声明（§5.1 要求 items 必填）—— **类型只由 key 判定**
//   structures[]   结构声明（网状 / 多层；层数与层间关系由规则给定）
//   states[]       绿黄红分档阈值（对齐 §5.5 的 states 段）
//   hysteresis{}   迟滞参数 + 最小驻留（对齐 §5.5 的 hysteresis 段）
//   metrics{}      指标集（key/单位/方向/有效范围/valueMap/权重）+ 窗口参数
//   coverage{}     覆盖率口径（显式、可复算）
//   meshProgress{} 组网进度口径（显式、可复算）
//   aggregation{}  节点派生口径
//   evaluation{}   四项评估的算法项与权重 + 阈值提示
//   optimization{} 可调参数与步长、优化目标（搜索空间）
//   curves{}       曲线时间粒度与窗口长度
//
// 未列出的顶层段/字段：忽略 + 计入 `warnings[]` 与 `metrics().unknownFields`（CTR-PL-03）。

/// 节点类型（TPL-MODEL-01）：`{key, 名称, 层级, 图形建议?}`。引擎只按 `key` 判定。
struct NodeTypeDef {
    std::string key;      // 类型标识（唯一；引擎唯一依据）
    std::string name;     // 显示名（规则内容，引擎不解释、不用于判定）
    int tier = 0;         // 层级：0 最上层，数字越大越靠下
    std::string graphic;  // 图形建议（如 map-2d 的图元种类）；空 = 未声明
};

/// 层（仅 StructureMode::Layered 使用）。
struct LayerDef {
    std::string key;
    std::string name;
    int tier = 0;
};

/// 结构声明（TPL-MODEL-02）。
struct StructureDef {
    std::string key;
    StructureMode mode = StructureMode::Mesh;
    std::vector<LayerDef> layers;   // layered：层数由规则声明；mesh：空
    bool adjacentOnly = true;       // layered：是否只允许相邻层连接（越级 → 可检出）
};

/// 指标定义（TPL-Q-01）：key、单位、方向、有效范围；引擎 MUST NOT 内建指标名。
struct MetricDef {
    std::string key;       // 规范键（对外 camelCase，P4）
    std::string ledgerKey; // 台账列名（下划线）；缺省 = 引擎按下划线规则从 key 派生（TPL-Q-02）
    std::string unit;      // 单位（可空）
    MetricDirection direction = MetricDirection::Higher;
    double min = 0.0;      // 有效范围下界
    double max = 1.0;      // 有效范围上界
    double weight = 0.0;   // 参与状态评分（分档）的权重；0 = 只聚合/出曲线，不参与分档
    std::map<std::string, double> valueMap;  // 文本取值 → 数值（规则声明；未知文本保持原值）
    bool inScore() const { return weight > 0.0; }
};

/// 分档阈值（TPL-ST-01）：`key` 为规则取值（引擎只对照 green/yellow/red 三值枚举）。
struct StateBandDef {
    std::string key;
    LinkState state = LinkState::Red;
    double min = 0.0;  // 归一化得分 ≥ min 即落入该档
};

/// 迟滞与最小驻留（TPL-ST-02 / TPL-ST-03）。
struct HysteresisDef {
    double riseMargin = 0.0;   // 向上迁移（转好）需超出目标档阈值多少
    double fallMargin = 0.0;   // 向下迁移（转差）需低于当前档阈值多少
    int confirmCount = 1;      // 连续 N 次确认才迁移
    int64_t minDwellMs = 0;    // 状态变更后的最小驻留时长
};

/// 滑动窗口（TPL-Q-04；默认 1 s 对齐 TR-MSG-06）。
struct WindowDef {
    int64_t windowMs = 1000;
    int maxSamples = 600;
};

/// 覆盖率口径（TPL-Q-06）：显式、可复算。
struct CoverageDef {
    std::string mode = "sum";     // sum = Σ各链路覆盖；max = 取最大
    std::string source;           // 取哪个指标（metric key）
    double targetAreaKm2 = 1.0;   // 分母（目标区域面积）
};

/// 组网进度口径项（TPL-Q-06）：`source` 取引擎提供的派生输入名（闭集，见契约文档）。
struct MeshComponentDef {
    std::string source;
    double weight = 0.0;
};

/// 组网进度口径：Σ(weight × 派生输入) × scale。
struct MeshProgressDef {
    std::vector<MeshComponentDef> components;
    double scale = 100.0;
    std::string unit = "%";
};

/// 评估项的一个加权分量（TPL-EVAL-01）：`source` = 派生输入名（闭集）。
struct EvaluationTermDef {
    std::string source;
    double weight = 1.0;
};

/// 阈值提示（TPL-EVAL-04）：结构化判定（布尔 + 依据），不含文案。
struct RequirementDef {
    std::string metric;         // 被判定项：评估项 key 或派生输入名
    std::string op = ">=";      // ">=" | "<="
    double limit = 0.0;
};

/// 评估项（TPL-EVAL-01/02）。
struct EvaluationItemDef {
    std::string key;
    double weight = 1.0;
    double scaleMin = 0.0;
    double scaleMax = 1.0;
    std::vector<EvaluationTermDef> terms;
    bool hasRequirement = false;
    RequirementDef requirement;
};

/// 评估段（TPL-EVAL）。`stabilityKey` 由规则声明"链路稳定度"对应哪个评估项 ——
/// 引擎 MUST NOT 内建评估项名，`linkStability()` 只按该声明取值（TPL-EVAL-03）。
struct EvaluationDef {
    std::vector<EvaluationItemDef> items;
    std::string stabilityKey;
};

/// 可调参数（TPL-OPT-02：哪些参数可调、步长、上下限）。
struct OptimizationParamDef {
    std::string metric;  // 必须是已声明指标
    double min = 0.0;
    double max = 0.0;
    double step = 0.0;   // 步长必须由规则给定（引擎内 MUST NOT 出现乘性常量）
};

/// 优化口径（TPL-OPT-02/05/06）。
struct OptimizationDef {
    std::string objectiveKind = "evaluationWeighted";  // 唯一支持取值
    std::vector<std::string> objectiveItems;           // 参与目标的评估项 key（空 = 全部）
    double minImprovement = 0.0;                       // 小于该改进量视为"无改进"
    int maxIterations = 64;                            // 迭代上限（R4）
    int64_t maxMs = 20;                                // 时限（以注入时钟计量；R4）
    std::vector<OptimizationParamDef> parameters;
};

/// 曲线口径（TPL-OPT-06）：时间粒度与窗口长度。
struct CurveDef {
    int64_t granularityMs = 1000;
    int maxPoints = 120;
    std::vector<std::string> metrics;  // 空 = 取全部参与评分的指标
};

/// 生效规则（装载后引擎持有的全部规则；CTR-PL-06 可导出）。
struct PolicyConfig {
    std::string policiesNamespace;
    std::string schemaVersion;
    std::vector<NodeTypeDef> nodeTypes;
    std::vector<StructureDef> structures;
    std::vector<StateBandDef> states;
    HysteresisDef hysteresis;
    WindowDef window;
    std::vector<MetricDef> metrics;
    CoverageDef coverage;
    MeshProgressDef meshProgress;
    NodeAggregation nodeAggregation = NodeAggregation::Worst;
    EvaluationDef evaluation;
    OptimizationDef optimization;
    CurveDef curves;
};

/// 规则自述。
struct PolicyInfo {
    bool loaded = false;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string policyVersion;  // "<namespace>:<schemaVersion>:<digest>"
    std::string digest;         // FNV-1a 64 → 16 位小写十六进制
    int nodeTypeCount = 0;
    int structureCount = 0;
    int metricCount = 0;
    int evaluationItemCount = 0;
    int parameterCount = 0;
    int64_t windowMs = 0;
    std::vector<std::string> warnings;  // 未知字段/未知顶层段（CTR-PL-03）
};

/// 逐条装载问题（`path` 形如 `metrics.items[3].direction`）。
struct LoadIssue {
    std::string path;
    std::string field;
    std::string reason;
};

/// 装载结果（**不抛异常**）。
struct LoadResult {
    int code = 0;
    std::string message;
    PolicyInfo data;
    std::vector<LoadIssue> issues;
    json toJson() const;
};

// ============================================================================
// §3 拓扑模型（TPL-MODEL）
// ============================================================================

/// 节点入参：坐标可给可不给（TPL-MODEL-03：给定 / 由布局提供方派生 / 无坐标）。
/// 注意 `typeKey` —— 类型只由它判定；`name` 只用于展示，改名不影响类型（TPL-MODEL-01）。
struct NodeSpec {
    std::string id;
    std::string typeKey;
    std::string name;
    std::string clusterId;
    double lng = 0.0;
    double lat = 0.0;
    bool hasPosition = false;
};

/// 边入参（无向）。
struct EdgeSpec {
    std::string id;
    std::string from;
    std::string to;
};

/// 节点（引擎对外形状）。
struct TopologyNode {
    std::string id;
    std::string typeKey;
    std::string typeName;
    std::string name;
    std::string clusterId;
    std::string graphic;  // 规则声明的图形建议
    int tier = 0;
    double lng = 0.0;
    double lat = 0.0;
    bool hasPosition = false;
    json toJson() const;
};

/// 边（引擎对外形状）。`state` 只在有数据时出现（MUST NOT 编造状态）。
struct TopologyEdge {
    std::string id;
    std::string from;
    std::string to;
    LinkState state = LinkState::Green;
    bool hasState = false;
    double score = 0.0;
    bool hasScore = false;
    json toJson() const;
};

/// 拓扑视图（= 宿主 `/links/topology` 的 data 形状基线）。
struct TopologyView {
    std::string topologyId;
    std::string structureKey;
    std::vector<TopologyNode> nodes;
    std::vector<TopologyEdge> edges;
    json toJson() const;
};

/// 校验问题（TPL-MODEL-05：可检出并给可读原因）。
/// `kind` 为机制取值：isolated-node / dangling-edge / self-loop / duplicate-edge /
/// tier-skip / unknown-node-type / missing-position / structure-mismatch。
struct ValidationIssue {
    std::string kind;
    std::string entity;  // 节点或边 id（定位用）
    std::string path;    // 可读路径（如 "edges[3].to"）
    std::string reason;  // 可读原因
    bool fatal = true;   // false = 提示级
    json toJson() const;
};

struct ValidationReport {
    bool ok = true;
    std::vector<ValidationIssue> issues;
    json toJson() const;
};

/// 变更结果（统一信封 data 为自由 JSON）。
struct MutationResult {
    int code = 0;
    std::string message;
    json data = json::object();
    json toJson() const;
};

// ============================================================================
// §4 质量聚合（TPL-Q）
// ============================================================================

/// 单指标窗口聚合（TPL-Q-04：均值/极值；手算可核对）。
struct MetricAggregate {
    std::string key;
    std::string ledgerKey;
    std::string unit;
    double mean = 0.0;
    double min = 0.0;
    double max = 0.0;
    double last = 0.0;
    int samples = 0;
    json toJson() const;
};

/// 状态变更记录（TPL-ST-04：从哪到哪、哪个指标越阈、当时数值）。
struct StateChange {
    int64_t ts = 0;
    std::string linkId;
    LinkState from = LinkState::Green;
    LinkState to = LinkState::Green;
    double score = 0.0;      // 判定时的归一化得分
    double threshold = 0.0;  // 越过的档位阈值
    double margin = 0.0;     // 迟滞裕量（本次判定所用）
    std::string metric;      // 当时起决定作用的指标（下行取最弱、上行取最强）
    double value = 0.0;      // 该指标当时的原始数值
    double normalized = 0.0; // 该指标当时的归一化值
    int confirmations = 0;   // 连续确认次数
    bool manual = false;     // 是否人工覆盖产生
    std::string reason;      // 机制原因码：hysteresis-confirmed / manual-override / override-cleared
    json checks = json::object();  // 当时全部指标数值（判定过程可复原）
    json toJson() const;
};

/// 手动覆盖（TPL-ST-06）。
struct ManualOverride {
    std::string linkId;
    LinkState state = LinkState::Green;
    std::string operatorId;
    std::string reason;
    int64_t at = 0;
    json toJson() const;
};

/// 链路质量（TPL-Q-04/05）。
struct LinkQuality {
    std::string linkId;
    std::string from;
    std::string to;
    std::vector<MetricAggregate> metrics;
    double score = 0.0;
    bool hasScore = false;
    LinkState state = LinkState::Green;
    bool hasState = false;
    bool manual = false;          // 当前状态是否来自人工覆盖
    int stateChanges = 0;
    int64_t windowMs = 0;
    int64_t updatedAt = 0;
    json toJson() const;
};

/// 节点质量（TPL-Q-05：节点质量由其关联边派生，口径可配）。
struct NodeQuality {
    std::string nodeId;
    std::string typeKey;
    std::string derivation;  // "worst" | "mean"
    std::vector<std::string> edgeIds;
    std::vector<MetricAggregate> metrics;
    double score = 0.0;
    bool hasScore = false;
    LinkState state = LinkState::Green;
    bool hasState = false;
    json toJson() const;
};

/// 派生输入（评估/组网进度的显式输入；顺序固定 = 机制声明顺序，保证可复算与确定性）。
struct DerivedMetrics {
    std::string topologyId;
    std::string structureKey;
    std::vector<std::pair<std::string, double>> values;
    std::map<std::string, double> index;
    bool has(const std::string& key) const;
    double get(const std::string& key, double fallback = 0.0) const;
    json toJson() const;
};

/// 归一化后的观测（TPL-Q-02/03：引擎内完成驼峰→台账映射；缺字段保持原值）。
struct NormalizedObservation {
    std::string linkId;
    std::string from;
    std::string to;
    int64_t ts = 0;
    json metrics = json::object();  // 规范形状（camelCase）—— 前端不再归一
    json ledger = json::object();   // 台账形状（规则声明的 ledgerKey）
    std::vector<std::string> preserved;  // 事件未携带 → 保持原值
    std::vector<std::string> rejected;   // 取值无法解释 → 保持原值
    std::vector<std::string> ignored;    // 事件携带但不在指标集内的字段（引擎不解释）
    std::string reportedState;           // 事件自称的状态（宿主给的数据，引擎不采信）
    double score = 0.0;
    bool hasScore = false;
    LinkState state = LinkState::Green;
    bool hasState = false;
    bool stateChanged = false;
    bool hasChange = false;
    StateChange change;
    json toJson() const;
};

struct IngestResult {
    int code = 0;
    std::string message;
    NormalizedObservation data;
    json toJson() const;
};

// ============================================================================
// §5 网络评估与优化（TPL-EVAL / TPL-OPT）
// ============================================================================

/// 逐项依据（TPL-EVAL-02：数值 + 依据；**不含自然语言结论**）。
struct EvaluationTerm {
    std::string source;
    double weight = 0.0;
    double value = 0.0;
    double contribution = 0.0;
    json toJson() const;
};

struct EvaluationItem {
    std::string key;
    double value = 0.0;
    double weight = 0.0;
    double normalized = 0.0;  // 归一到 [scaleMin, scaleMax] 后的占比
    double scaleMin = 0.0;
    double scaleMax = 1.0;
    std::vector<EvaluationTerm> terms;
    bool hasRequirement = false;
    std::string requirementMetric;
    std::string requirementOp;
    double requirementLimit = 0.0;
    double requirementActual = 0.0;
    bool satisfied = false;
    json toJson() const;
};

struct EvaluationResult {
    int code = 0;
    std::string message;
    std::string topologyId;
    std::string structureKey;
    std::string phaseKey;      // 来自 PhaseContext（引擎只回显，不解释）
    std::string scenarioKey;
    std::string missionId;
    double overall = 0.0;
    std::vector<EvaluationItem> items;
    json inputs = json::object();  // 派生输入快照（逐项依据的输入侧，可手算）
    int64_t ts = 0;
    json toJson() const;
    /// 按 key 取评估项数值；不存在 → nullopt（MUST NOT 返回默认值）
    std::optional<double> itemValue(const std::string& key) const;
};

/// 优化建议项（TPL-OPT-02/04：产出建议值而非直接改状态）。
struct OptimizationSuggestion {
    std::string metric;
    std::string ledgerKey;
    std::string unit;
    double current = 0.0;
    double suggested = 0.0;
    double delta = 0.0;
    int steps = 0;                 // 走了几个声明步长
    json impact = json::object();  // 改了哪些参数 → 哪些评估项变化
    json toJson() const;
};

struct OptimizationResult {
    int code = 0;
    std::string message;
    bool improved = false;         // false = 无改进（TPL-OPT-05）
    std::string reason;            // improved / no-improvement / truncated
    std::string topologyId;
    std::string linkId;            // 空 = 拓扑级（按各参数均值）
    std::vector<OptimizationSuggestion> suggestions;
    EvaluationResult before;
    EvaluationResult after;
    int iterations = 0;
    bool truncated = false;        // R4：命中迭代/时限，返回已得最优
    json toJson() const;
};

// ============================================================================
// §6 曲线（TPL-OPT-06：由真实滑动窗口聚合产生，MUST NOT 用写死公式）
// ============================================================================

struct CurvePoint {
    int64_t ts = 0;
    int samples = 0;
    json values = json::object();  // { "<metricKey>": 窗口聚合值 }
    json toJson() const;
};

struct CurveSeries {
    std::string topologyId;
    std::string linkId;
    int64_t granularityMs = 0;
    int64_t windowMs = 0;
    std::vector<std::string> metrics;
    std::vector<CurvePoint> points;
    json toJson() const;
};

// ============================================================================
// §7 反向接口（宿主 MUST 实现）
// ============================================================================

/// 留痕条目（沿用 phase-engine 的命名，protocol §6）。
struct AuditEntry {
    int64_t at = 0;
    std::string actor;
    std::string action;
    std::string target;
    std::string detail;
    bool violation = false;
};

/// = `topology.changed` 事件的 data（protocol §4.4 已登记：{missionId, edgeId, from, to, state, metrics}）。
/// `ts` 为只增字段（CTR-EV-04）。信封由宿主 realtime-hub 侧包。
struct TopologyChangedEvent {
    std::string missionId;
    std::string edgeId;
    std::string from;
    std::string to;
    std::string state;  // green/yellow/red
    json metrics = json::object();  // 含迟滞判定结果（得分、阈值、决定指标、确认次数…）
    int64_t ts = 0;
    json toJson() const;
};

/// 出口：状态变更通知。MUST 立即返回、MUST NOT 阻塞（protocol §6）。
class ITopologySink {
public:
    virtual ~ITopologySink() = default;
    /// 边状态变更 → 宿主广播 `topology.changed`（已登记事件）。
    virtual void onTopologyChanged(const TopologyChangedEvent& e) = 0;
    /// 迟滞判定留痕（**不新增事件名**）：宿主可落库/日志；默认空实现。
    virtual void onStateChanged(const StateChange& e) { (void)e; }
};

/// 持久化出口（引擎不接触 SQL，P3）。快照为引擎导出的 JSON（可审计、可回放）。
class ITopologyStore {
public:
    virtual ~ITopologyStore() = default;
    virtual bool save(const std::string& topologyId, const json& snapshot) = 0;
    virtual bool load(const std::string& topologyId, json& out) = 0;
    virtual bool remove(const std::string& topologyId) = 0;
};

/// 时间注入（epoch 毫秒，P5/P9）。
class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t nowMs() const = 0;
};

/// 可选日志出口（缺省 = 静默）。
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void log(int level, const std::string& event, const json& data) {
        (void)level;
        (void)event;
        (void)data;
    }
    virtual void commandAudit(const AuditEntry& e) { (void)e; }
};

/// 坐标解析入参（**只读**）。
struct LayoutRequest {
    std::string nodeId;
    std::string clusterId;
    std::string typeKey;
    int tier = 0;
};

struct LayoutPoint {
    double lng = 0.0;
    double lat = 0.0;
};

/// 坐标来源（TPL-MODEL-03：场景给定 / 由集群位置派生 / 外部布局算法）。
/// 引擎 MUST NOT 硬编码坐标；未注入且节点未给坐标时，节点标注 hasPosition=false。
class ILayoutProvider {
public:
    virtual ~ILayoutProvider() = default;
    virtual bool resolve(const LayoutRequest& req, LayoutPoint& out) = 0;
};

/// 内置系统时钟：引擎唯一的非确定性来源（未注入 IClock 时回落）。
class SystemClock : public IClock {
public:
    int64_t nowMs() const override;
};

// ============================================================================
// §8 自述与注入
// ============================================================================

struct Capabilities {
    bool policyLoaded = false;
    std::string schemaVersion;
    std::string policyVersion;
    std::string digest;
    int policiesMajor = kSupportedPoliciesMajor;
    int nodeTypes = 0;
    int structures = 0;
    int metrics = 0;
    int evaluationItems = 0;
    int parameters = 0;
    int64_t windowMs = 0;
    std::string topologyId;
    std::string structureKey;
    int nodes = 0;
    int edges = 0;
    bool clockInjected = false;
    bool storeInjected = false;
    bool sinkInjected = false;
    bool logInjected = false;
    bool layoutInjected = false;
};

struct Metrics {
    int64_t observations = 0;
    int64_t rejectedObservations = 0;
    int64_t metricUpdates = 0;
    int64_t preservedFields = 0;   // 缺字段按"保持原值"处理的次数（TPL-Q-03）
    int64_t rejectedValues = 0;    // 取值无法解释（未知文本等），同样保持原值
    int64_t stateChanges = 0;
    int64_t suppressedChanges = 0; // 被迟滞/驻留拦下的迁移
    int64_t manualOverrides = 0;
    int64_t overrideClears = 0;
    int64_t evaluations = 0;
    int64_t optimizations = 0;
    int64_t noImprovement = 0;
    int64_t curvePoints = 0;
    int64_t duplicateEdges = 0;
    int64_t rejectedEdges = 0;
    int64_t rejectedNodes = 0;
    int64_t layoutMisses = 0;
    int64_t sinkErrors = 0;
    int64_t storeErrors = 0;
    int64_t clockRegressions = 0;
    int64_t unknownFields = 0;
};

/// 依赖注入结构：只有五个依赖，**没有业务配置**（业务全在规则包）。
struct TopologyEngineOptions {
    std::shared_ptr<ITopologyStore> store;   // 可空 → 不落库
    std::shared_ptr<IClock> clock;           // 可空 → SystemClock
    std::shared_ptr<ITopologySink> sink;     // 可空 → 空 Sink
    std::shared_ptr<ILogSink> log;           // 可空 → 静默
    std::shared_ptr<ILayoutProvider> layout; // 可空 → 需节点自带坐标
};

// ============================================================================
// §9 引擎门面
// ============================================================================

/// 链路拓扑引擎。构造后**不抛异常**；未装载规则时一切需要规则的入口返回 1005。
class TopologyEngine {
public:
    TopologyEngine();
    explicit TopologyEngine(const TopologyEngineOptions& opts);
    ~TopologyEngine();
    TopologyEngine(const TopologyEngine&) = delete;
    TopologyEngine& operator=(const TopologyEngine&) = delete;
    TopologyEngine(TopologyEngine&&) noexcept;
    TopologyEngine& operator=(TopologyEngine&&) noexcept;

    // ---- 规则装载与查询 ----
    LoadResult loadPolicy(const json& pkg);
    LoadResult loadPolicyFile(const std::string& path);
    static LoadResult validatePolicy(const json& pkg);
    PolicyInfo policyInfo() const;
    /// 生效规则的副本（CTR-PL-06）；未装载 → nullopt
    std::optional<PolicyConfig> policy() const;

    // ---- 拓扑模型（TPL-MODEL） ----
    /// 选择结构并初始化空拓扑；已有内容且 reset=false → 1002（冲突拒绝）
    MutationResult configureTopology(const std::string& topologyId, const std::string& structureKey,
                                     bool reset = false);
    MutationResult addNode(const NodeSpec& spec);
    MutationResult addNodes(const std::vector<NodeSpec>& specs);
    MutationResult addEdge(const EdgeSpec& spec);
    MutationResult addEdges(const std::vector<EdgeSpec>& specs);
    MutationResult removeEdge(const std::string& edgeId);
    MutationResult removeNode(const std::string& nodeId);
    TopologyView topologyView() const;
    std::optional<TopologyNode> node(const std::string& nodeId) const;
    std::optional<TopologyEdge> edge(const std::string& edgeId) const;
    ValidationReport validate() const;
    /// 快照导出（TPL-MODEL-06，供审计与回放复盘）
    json exportSnapshot() const;
    /// 快照导入（TPL-MODEL-06，供审计与回放复盘）
    ///
    /// `strict = true`（缺省）：快照内任一条目非法即整包拒绝（MUST NOT 半装载）。
    /// `strict = false`：**容错导入**（历史/外部数据），尽量装载并把逐条问题列在
    /// `data.issues` 里 —— 此时拓扑可能含悬挂边等缺陷，由 `validate()` 检出并定位。
    MutationResult importSnapshot(const json& snapshot, bool strict = true);
    /// 宿主加载：从 ITopologyStore 读快照（未注入 → 1005）；
    /// `topologyId` 为空 = 取当前已配置的拓扑 id
    MutationResult loadSnapshotFromStore(const std::string& topologyId = std::string());

    // ---- 质量聚合（TPL-Q） ----
    IngestResult ingest(const json& event);
    MutationResult ingestBatch(const std::vector<json>& events);
    std::optional<LinkQuality> linkQuality(const std::string& linkId) const;
    std::optional<NodeQuality> nodeQuality(const std::string& nodeId) const;
    std::vector<LinkQuality> linkQualities() const;
    DerivedMetrics derived() const;
    /// 归一（纯函数式，不改状态）：返回规范形状与台账形状（TPL-Q-02）
    json normalizedShape(const json& event) const;

    // ---- 状态机（TPL-ST） ----
    std::optional<LinkState> linkState(const std::string& linkId) const;
    MutationResult setOverride(const std::string& linkId, LinkState state,
                               const std::string& operatorId, const std::string& reason,
                               bool replace = false);
    MutationResult clearOverride(const std::string& linkId, const std::string& operatorId,
                                 const std::string& reason);
    std::vector<ManualOverride> overrides() const;
    /// 变更日志（TPL-ST-04）；linkId 为空 = 全部
    std::vector<StateChange> stateLog(const std::string& linkId = std::string()) const;
    /// 注入当前任务/阶段上下文（protocol.md §1.4）：状态变更事件负载的 `missionId`
    /// 与评估结果的回显字段取自它；引擎不解释 `phaseKey`/`scenarioKey`。
    void setPhaseContext(const PhaseContext& ctx);
    PhaseContext phaseContext() const;

    // ---- 评估 / 优化 / 曲线（TPL-EVAL / TPL-OPT） ----
    EvaluationResult evaluate(const PhaseContext& ctx = PhaseContext());
    /// TPL-EVAL-03：`scoring` 只读调用即可取得链路稳定度（= 同名评估项）
    std::optional<double> linkStability() const;
    OptimizationResult optimize(const PhaseContext& ctx, const std::string& linkId = std::string());
    CurveSeries curves(const std::string& linkId) const;

    /// 消费侧图元适配（map-2d §3.3）：links → LinkItem{id,from,to,state}、
    /// clusters → ClusterItem{id,lng,lat,name}；坐标缺失的链路列在 missingCoordinates。
    json primitives() const;

    // ---- 依赖注入（P8/P9） ----
    void setStore(std::shared_ptr<ITopologyStore> store);
    void setClock(std::shared_ptr<IClock> clock);
    void setSink(std::shared_ptr<ITopologySink> sink);
    void setLog(std::shared_ptr<ILogSink> log);
    void setLayout(std::shared_ptr<ILayoutProvider> layout);

    // ---- 自述与观测 ----
    Capabilities capabilities() const;
    Metrics metrics() const;
    static const char* errorCodeName(int code);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// §10 自由函数与枚举 ⇄ JSON
// ============================================================================

/// 规则包校验（纯函数；等价于 TopologyEngine::validatePolicy）
LoadResult validatePolicy(const json& pkg);
/// 错误码短名（未知码 → "unknown"）
const char* errorCodeName(int code);

const char* toString(LinkState s);
const char* toString(MetricDirection d);
const char* toString(StructureMode m);
const char* toString(NodeAggregation a);
/// 字符串 → 枚举；非法取值返回 nullopt（MUST NOT 猜）
std::optional<LinkState> linkStateFromString(const std::string& s);
std::optional<MetricDirection> metricDirectionFromString(const std::string& s);
std::optional<StructureMode> structureModeFromString(const std::string& s);
std::optional<NodeAggregation> nodeAggregationFromString(const std::string& s);

// JSON 序列化（camelCase；键序与成员声明顺序一致）
json toJson(const PolicyInfo& v);
json toJson(const LoadIssue& v);
json toJson(const PhaseContext& v);
json toJson(const Capabilities& v);
json toJson(const Metrics& v);

}  // namespace topology
