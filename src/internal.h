// topology · src/internal.h —— 内部实现细节（宿主 MUST NOT 包含本文件）
//
// 公开面只有 include/topology/topology_engine.h。
// 内部同样 MUST NOT 出现业务名词（节点类型显示名、指标名、区域名）—— 那些全在规则包（P6/P7）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "topology/topology_engine.h"

namespace topology {
namespace detail {

// ---------------------------------------------------------------- 基础工具

/// epoch 毫秒；未注入时钟时回落系统时钟（引擎唯一的非确定性来源）
int64_t nowFrom(IClock* clock);
IClock* clockOf(const TopologyEngineOptions& deps, std::shared_ptr<IClock>& fallback);

/// 通用 camelCase → snake_case（规则未声明 ledgerKey 时的缺省派生，TPL-Q-02）
std::string toSnakeCase(const std::string& camel);

/// FNV-1a 64 → 16 位小写十六进制（规则包字节的规范化摘要；零外部依赖）
std::string fnv1a64Hex(const std::string& bytes);
/// 规范化 JSON：对象键按 ASCII 升序、数组保序、数字最短表示、无空白
std::string canonicalJson(const json& v);

/// 数值化：数字直取；布尔取 0/1；字符串走规则声明的 valueMap（未命中 → false）
bool numericValue(const json& v, const MetricDef& def, double& out);

double clamp01(double v);
double clampRange(double v, double lo, double hi);
/// 指标原始值 → 归一化 [0,1]（方向与有效范围由规则声明）
double normalizeMetric(const MetricDef& def, double raw);

bool isInteger(const json& v);
std::optional<int64_t> asInt(const json& v);

// ---------------------------------------------------------------- 派生输入（闭集，机制声明）

/// 派生输入名：固定项（与指标无关）+ 逐指标项 `metric.<key>` / `metricScore.<key>`
const std::vector<std::string>& fixedDerivations();
bool isDerivationName(const std::string& name, const PolicyConfig& policy);
/// 固定顺序的派生输入名清单（含逐指标项，按规则声明顺序）
std::vector<std::string> derivationNames(const PolicyConfig& policy);

// ---------------------------------------------------------------- 内部状态

/// 窗口样本
struct Sample {
    int64_t ts = 0;
    double value = 0.0;
};

/// 单指标状态（`value` = 最近一次成功解释的取值；缺字段时保持该值，TPL-Q-03）
struct MetricState {
    std::deque<Sample> samples;
    bool hasValue = false;
    double value = 0.0;
    int64_t lastTs = 0;
};

/// 迟滞状态机（TPL-ST-02/03）
struct FsmState {
    bool hasState = false;
    LinkState state = LinkState::Green;
    int64_t since = 0;
    bool hasPending = false;
    LinkState pending = LinkState::Green;
    int pendingCount = 0;
};

/// 链路记录（= 模型里的一条无向边 + 其质量与状态）
struct LinkRecord {
    std::string id;
    std::string from;
    std::string to;
    bool removed = false;
    std::map<std::string, MetricState> metrics;  // metric key → 状态
    FsmState fsm;
    bool hasScore = false;
    double score = 0.0;
    bool overridden = false;
    ManualOverride overrideInfo;
    int stateChanges = 0;
    int64_t updatedAt = 0;
    bool hasNextPoint = false;
    int64_t nextPointTs = 0;
    std::deque<CurvePoint> curve;
};

struct NodeRecord {
    NodeSpec spec;
    std::string typeName;
    std::string graphic;
    int tier = 0;
    std::vector<std::string> edges;  // 关联边 id（装载顺序）
};

/// 引擎全局状态（TopologyEngine::Impl 持有）
struct EngineState {
    TopologyEngineOptions deps;        // 宿主注入的依赖
    std::shared_ptr<IClock> systemClock;  // 未注入时的内置时钟
    PolicyConfig policy;
    bool policyLoaded = false;
    PolicyInfo info;
    std::string topologyId;
    std::string structureKey;
    bool configured = false;
    std::map<std::string, NodeRecord> nodes;
    std::vector<LinkRecord> links;  // 装载顺序（输出顺序即此序）
    std::map<std::string, size_t> linkIndex;
    std::map<std::string, std::string> undirected;  // "min\x1fmax" → edgeId
    std::vector<StateChange> log;
    Metrics metrics;
    PhaseContext ctx;  // 宿主注入的当前上下文（事件负载 missionId 与评估回显用）
    int64_t lastNow = 0;
    bool hasLastNow = false;
};

/// 解析后的时钟（不推进状态）
int64_t nowOf(EngineState& st);

// ---------------------------------------------------------------- 规则（policy.cc）

/// 纯校验：失败时 MUST 给出逐条原因（CTR-PL-02），MUST NOT 部分装载
LoadResult validatePolicyInto(const json& pkg, PolicyConfig* out, PolicyInfo* info);

/// 原子装载：失败保留上一次成功装载的规则
LoadResult loadPolicyInto(EngineState& st, const json& pkg);

const MetricDef* findMetric(const PolicyConfig& p, const std::string& key);
const NodeTypeDef* findNodeType(const PolicyConfig& p, const std::string& key);
const StructureDef* findStructure(const PolicyConfig& p, const std::string& key);
const EvaluationItemDef* findEvaluationItem(const PolicyConfig& p, const std::string& key);
const OptimizationParamDef* findParameter(const PolicyConfig& p, const std::string& metric);

/// 得分 → 原始档（不含迟滞；TPL-ST-01）
LinkState bandOf(const PolicyConfig& p, double score);
/// 档位阈值（该档的 min）
double bandMinOf(const PolicyConfig& p, LinkState s);

// ---------------------------------------------------------------- 模型（model.cc）

MutationResult configureTopologyImpl(EngineState& st, const std::string& topologyId,
                                     const std::string& structureKey, bool reset);
MutationResult addNodeImpl(EngineState& st, const NodeSpec& spec);
MutationResult addEdgeImpl(EngineState& st, const EdgeSpec& spec);
MutationResult removeEdgeImpl(EngineState& st, const std::string& edgeId);
MutationResult removeNodeImpl(EngineState& st, const std::string& nodeId);
ValidationReport validateImpl(const EngineState& st);
json exportSnapshotImpl(const EngineState& st);
MutationResult importSnapshotImpl(EngineState& st, const json& snapshot, bool strict);
/// 不做端点/去重校验地插入一条边（仅容错导入用；缺陷交给 validate() 检出）
bool insertEdgeUnchecked(EngineState& st, const EdgeSpec& spec);
TopologyView viewImpl(const EngineState& st);
json primitivesImpl(const EngineState& st);
std::optional<TopologyNode> nodeOf(const EngineState& st, const std::string& nodeId);
std::optional<TopologyEdge> edgeOf(const EngineState& st, const std::string& edgeId);
LinkRecord* findLink(EngineState& st, const std::string& linkId);
const LinkRecord* findLink(const EngineState& st, const std::string& linkId);
/// 节点在结构声明中的层（mesh 结构 → 0）
int nodeTierOf(const PolicyConfig& p, const StructureDef& s, const NodeTypeDef& t);

// ---------------------------------------------------------------- 质量（quality.cc）

IngestResult ingestImpl(EngineState& st, const json& event);
std::optional<LinkQuality> linkQualityImpl(const EngineState& st, const std::string& linkId);
std::optional<NodeQuality> nodeQualityImpl(const EngineState& st, const std::string& nodeId);
std::vector<LinkQuality> linkQualitiesImpl(const EngineState& st);
NodeQuality nodeQualityOf(const EngineState& st, const NodeRecord& rec);
/// 派生输入（评估、组网进度、优化投影共用同一实现，保证口径一致）
DerivedMetrics derivedImpl(const EngineState& st);
/// 派生输入：由"一个投影值表"给出（优化用；指标性派生项取投影值）
DerivedMetrics derivedFrom(const EngineState& st, const std::map<std::string, double>& projected);
json normalizedShapeImpl(const EngineState& st, const json& event);

/// 窗口聚合（含均值/极值/样本数）
std::vector<MetricAggregate> aggregatesOf(const PolicyConfig& p, const LinkRecord& link);
/// 单指标窗口均值（无样本 → false）
bool linkMetricMean(const LinkRecord& link, const std::string& key, double& out);
/// 有效状态：人工覆盖优先（TPL-ST-06）
bool effectiveLinkState(const LinkRecord& link, LinkState& out);
/// 由窗口聚合算得分（只算参与评分的指标）
bool scoreOf(const PolicyConfig& p, const LinkRecord& link, double& out);
/// 由投影值表算得分
bool scoreOfValues(const PolicyConfig& p, const std::map<std::string, double>& values, double& out);

// ---------------------------------------------------------------- 状态机（state.cc）

/// 一次判定（含迟滞、最小驻留、留痕与出口通知）
void judgeLink(EngineState& st, LinkRecord& link, int64_t now);
MutationResult setOverrideImpl(EngineState& st, const std::string& linkId, LinkState state,
                               const std::string& operatorId, const std::string& reason, bool replace);
MutationResult clearOverrideImpl(EngineState& st, const std::string& linkId,
                                 const std::string& operatorId, const std::string& reason);
void appendChange(EngineState& st, StateChange ch);
/// 判定过程快照（可复原：得分、档位、阈值、逐指标数值与归一值）
json judgementChecks(const EngineState& st, const LinkRecord& link, LinkState band, double threshold,
                     double margin, int confirmations);

// ---------------------------------------------------------------- 评估与优化（eval.cc / optimize.cc）

EvaluationResult evaluateImpl(EngineState& st, const PhaseContext& ctx, const DerivedMetrics& d,
                              const std::string& linkId, bool countIt);
EvaluationResult evaluateWith(EngineState& st, const PhaseContext& ctx,
                              const std::map<std::string, double>& projected,
                              const std::string& linkId, bool countIt);
OptimizationResult optimizeImpl(EngineState& st, const PhaseContext& ctx, const std::string& linkId);
CurveSeries curvesImpl(const EngineState& st, const std::string& linkId);
/// TPL-EVAL-03：只读取"链路稳定度"（按规则声明的 stabilityKey，不内建评估项名）
std::optional<double> linkStabilityImpl(const EngineState& st);
/// 拓扑级/链路级的指标基线（窗口均值）
std::map<std::string, double> baselineValues(const EngineState& st, const std::string& linkId);

// ---------------------------------------------------------------- 通知（engine.cc）

void emitChange(EngineState& st, const LinkRecord& link, const StateChange& ch);
void saveSnapshotIfPossible(EngineState& st);

}  // namespace detail
}  // namespace topology
