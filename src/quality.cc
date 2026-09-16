// topology · src/quality.cc —— 质量聚合（TPL-Q-01..06）
//
// 口径：
//   · 指标集、单位、方向、有效范围、文本取值映射全部来自规则（TPL-Q-01）；
//   · 事件字段归一（驼峰 → 台账列名）在**引擎内**完成（TPL-Q-02 / D3）；
//   · 缺失字段与无法解释的取值一律**保持原值**，MUST NOT 用 0 覆盖（TPL-Q-03）；
//   · 滑动窗口聚合（均值/极值），窗口长度可配、默认 1 s（TPL-Q-04）；
//   · 边与节点双维度聚合，口径可配（TPL-Q-05）；
//   · 覆盖率与组网进度口径显式、可复算（TPL-Q-06）。
#include "internal.h"

#include <set>

namespace topology {
namespace detail {

namespace {

/// 事件保留字段（不属指标集，引擎按协议消费）
const std::set<std::string>& reservedFields() {
    static const std::set<std::string> kFields = {"linkId", "from", "to", "ts", "state"};
    return kFields;
}

}  // namespace

/// 窗口均值（无样本 → false）
bool linkMetricMean(const LinkRecord& link, const std::string& key, double& out) {
    const auto it = link.metrics.find(key);
    if (it == link.metrics.end() || it->second.samples.empty()) return false;
    double sum = 0.0;
    for (const auto& s : it->second.samples) sum += s.value;
    out = sum / static_cast<double>(it->second.samples.size());
    return true;
}

/// 有效状态：人工覆盖优先（TPL-ST-06）
bool effectiveLinkState(const LinkRecord& link, LinkState& out) {
    if (link.overridden) {
        out = link.overrideInfo.state;
        return true;
    }
    if (link.fsm.hasState) {
        out = link.fsm.state;
        return true;
    }
    return false;
}

namespace {

/// 窗口均值 + 样本数（内部便利）
bool windowMean(const LinkRecord& link, const std::string& key, double& out, int* samples = nullptr) {
    if (!linkMetricMean(link, key, out)) return false;
    if (samples) *samples = static_cast<int>(link.metrics.at(key).samples.size());
    return true;
}

double indexGet(const std::map<std::string, double>& idx, const std::string& key) {
    const auto it = idx.find(key);
    return it == idx.end() ? 0.0 : it->second;
}

/// 覆盖率与组网进度（两个依赖其它派生输入的复合项，统一在此复算）
void recomputeComposite(std::map<std::string, double>& idx, const PolicyConfig& p,
                        const EngineState& st) {
    // 覆盖率：显式口径（sum = Σ各链路覆盖 / 目标面积；max = 取最大 / 目标面积）
    double cov = 0.0;
    if (!p.coverage.source.empty() && p.coverage.targetAreaKm2 > 0.0) {
        double acc = 0.0;
        bool any = false;
        for (const auto& l : st.links) {
            if (l.removed) continue;
            double v = 0.0;
            if (!windowMean(l, p.coverage.source, v)) continue;
            if (!any) {
                acc = v;
                any = true;
            } else {
                acc = (p.coverage.mode == "max") ? (acc > v ? acc : v) : (acc + v);
            }
        }
        cov = acc / p.coverage.targetAreaKm2;
    }
    idx["coverageRatio"] = clamp01(cov);

    double wsum = 0.0;
    double acc = 0.0;
    for (const auto& c : p.meshProgress.components) {
        acc += c.weight * indexGet(idx, c.source);
        wsum += c.weight;
    }
    idx["meshProgress"] = (wsum > 0.0) ? (acc / wsum * p.meshProgress.scale) : 0.0;
}

/// 派生输入的统一实现；`projected` 非空时，指标性派生项取投影值（优化用，见契约文档）
DerivedMetrics computeDerivations(const EngineState& st, const std::map<std::string, double>* projected) {
    const PolicyConfig& p = st.policy;
    DerivedMetrics d;
    d.topologyId = st.topologyId;
    d.structureKey = st.structureKey;

    std::map<std::string, double> idx;
    int liveLinks = 0;
    for (const auto& l : st.links) {
        if (!l.removed) ++liveLinks;
    }
    idx["linkCount"] = static_cast<double>(liveLinks);
    idx["nodeCount"] = static_cast<double>(st.nodes.size());
    idx["stateChangeCount"] = static_cast<double>(st.metrics.stateChanges);
    idx["observationCount"] = static_cast<double>(st.metrics.observations);
    int totalSamples = 0;
    for (const auto& l : st.links) {
        if (l.removed) continue;
        for (const auto& kv : l.metrics) totalSamples += static_cast<int>(kv.second.samples.size());
    }
    idx["windowSamples"] = static_cast<double>(totalSamples);

    // 逐指标：跨链路窗口均值（可手算）；投影模式下用投影值
    for (const auto& m : p.metrics) {
        double sum = 0.0;
        int n = 0;
        for (const auto& l : st.links) {
            if (l.removed) continue;
            double v = 0.0;
            if (windowMean(l, m.key, v)) {
                sum += v;
                ++n;
            }
        }
        double value = n > 0 ? (sum / static_cast<double>(n)) : 0.0;
        if (projected != nullptr) {
            const auto it = projected->find(m.key);
            if (it != projected->end()) value = it->second;
        }
        idx["metric." + m.key] = value;
        if (m.inScore()) idx["metricScore." + m.key] = normalizeMetric(m, value);
    }

    // 得分与有效状态
    double scoreSum = 0.0;
    int scoreN = 0;
    double bestScore = 0.0;
    double worstScore = 0.0;
    int greens = 0, yellows = 0, reds = 0, up = 0;
    std::map<std::string, bool> covered;
    for (const auto& kv : st.nodes) covered[kv.first] = false;
    for (const auto& l : st.links) {
        if (l.removed) continue;
        double s = 0.0;
        if (scoreOf(p, l, s)) {
            if (scoreN == 0 || s > bestScore) bestScore = s;
            if (scoreN == 0 || s < worstScore) worstScore = s;
            scoreSum += s;
            ++scoreN;
        }
        LinkState ls;
        if (effectiveLinkState(l, ls)) {
            if (ls == LinkState::Green) ++greens;
            else if (ls == LinkState::Yellow) ++yellows;
            else ++reds;
            if (ls != LinkState::Red) {
                ++up;
                if (covered.count(l.from)) covered[l.from] = true;
                if (covered.count(l.to)) covered[l.to] = true;
            }
        }
    }
    if (projected != nullptr) {
        double s = 0.0;
        if (scoreOfValues(p, *projected, s)) {
            scoreSum = s;
            bestScore = s;
            worstScore = s;
            scoreN = 1;
            const LinkState band = bandOf(p, s);
            greens = (band == LinkState::Green) ? 1 : 0;
            yellows = (band == LinkState::Yellow) ? 1 : 0;
            reds = (band == LinkState::Red) ? 1 : 0;
            up = (band == LinkState::Red) ? 0 : 1;
            idx["__projected__"] = 1.0;
        }
    }
    idx["stateScoreMean"] = scoreN > 0 ? (scoreSum / static_cast<double>(scoreN)) : 0.0;
    idx["worstScore"] = scoreN > 0 ? worstScore : 0.0;
    idx["scoreSpread"] = scoreN > 0 ? (bestScore - worstScore) : 0.0;
    const double denom = static_cast<double>(liveLinks > 0 ? liveLinks : 0);
    idx["greenRatio"] = denom > 0.0 ? (static_cast<double>(greens) / denom) : 0.0;
    idx["yellowRatio"] = denom > 0.0 ? (static_cast<double>(yellows) / denom) : 0.0;
    idx["redRatio"] = denom > 0.0 ? (static_cast<double>(reds) / denom) : 0.0;
    idx["linkUpRatio"] = denom > 0.0 ? (static_cast<double>(up) / denom) : 0.0;
    const double changes = static_cast<double>(st.metrics.stateChanges);
    const double linkBase = static_cast<double>(liveLinks > 0 ? liveLinks : 1);
    idx["stateStability"] = 1.0 - std::min(1.0, changes / linkBase);
    int coveredNodes = 0;
    for (const auto& kv : covered) {
        if (kv.second) ++coveredNodes;
    }
    idx["nodeCoveredRatio"] =
        st.nodes.empty() ? 0.0 : (static_cast<double>(coveredNodes) / static_cast<double>(st.nodes.size()));

    recomputeComposite(idx, p, st);
    if (projected != nullptr && idx.count("__projected__") != 0) {
        // 覆盖率在投影模式下按"代表链路 × 链路数"给出（sum）/ 代表值（max）
        if (!p.coverage.source.empty() && p.coverage.targetAreaKm2 > 0.0) {
            const double v = indexGet(idx, "metric." + p.coverage.source);
            const double acc = (p.coverage.mode == "max") ? v : v * static_cast<double>(liveLinks);
            idx["coverageRatio"] = clamp01(acc / p.coverage.targetAreaKm2);
        }
        double wsum = 0.0;
        double acc = 0.0;
        for (const auto& c : p.meshProgress.components) {
            acc += c.weight * indexGet(idx, c.source);
            wsum += c.weight;
        }
        idx["meshProgress"] = (wsum > 0.0) ? (acc / wsum * p.meshProgress.scale) : 0.0;
    }

    for (const auto& name : derivationNames(p)) {
        d.values.push_back(std::make_pair(name, indexGet(idx, name)));
    }
    d.index = idx;
    return d;
}

/// 曲线点：由窗口聚合产生（TPL-OPT-06；MUST NOT 用写死公式）
void appendCurvePoint(EngineState& st, LinkRecord& link, int64_t ts) {
    const CurveDef& c = st.policy.curves;
    if (c.granularityMs <= 0 || c.metrics.empty()) return;
    if (!link.hasNextPoint) {
        link.hasNextPoint = true;
        link.nextPointTs = ts;
    }
    if (ts < link.nextPointTs) return;
    CurvePoint p;
    p.ts = ts;
    json values = json::object();
    for (const auto& key : c.metrics) {
        const auto it = link.metrics.find(key);
        if (it == link.metrics.end() || it->second.samples.empty()) continue;
        double sum = 0.0;
        for (const auto& s : it->second.samples) sum += s.value;
        const double mean = sum / static_cast<double>(it->second.samples.size());
        values[key] = mean;  // 真实窗口聚合值，不是公式
        p.samples = static_cast<int>(it->second.samples.size());
    }
    p.values = values;
    link.curve.push_back(p);
    while (static_cast<int>(link.curve.size()) > c.maxPoints) link.curve.pop_front();
    while (link.nextPointTs <= ts) link.nextPointTs += c.granularityMs;
    ++st.metrics.curvePoints;
}

void buildShape(const EngineState& st, const LinkRecord& link, NormalizedObservation& out) {
    json metrics = json::object();
    json ledger = json::object();
    for (const auto& m : st.policy.metrics) {
        const auto it = link.metrics.find(m.key);
        if (it == link.metrics.end() || !it->second.hasValue) continue;
        metrics[m.key] = it->second.value;   // 规范形状（camelCase）
        ledger[m.ledgerKey] = it->second.value;  // 台账形状（规则声明的 ledgerKey）
    }
    out.metrics = metrics;
    out.ledger = ledger;
    LinkState ls;
    if (effectiveLinkState(link, ls)) {
        out.state = ls;
        out.hasState = true;
    }
    if (link.hasScore) {
        out.score = link.score;
        out.hasScore = true;
    }
}

}  // namespace

// ---------------------------------------------------------------- 聚合与得分

std::vector<MetricAggregate> aggregatesOf(const PolicyConfig& p, const LinkRecord& link) {
    std::vector<MetricAggregate> out;
    for (const auto& m : p.metrics) {
        const auto it = link.metrics.find(m.key);
        if (it == link.metrics.end() || it->second.samples.empty()) continue;
        MetricAggregate a;
        a.key = m.key;
        a.ledgerKey = m.ledgerKey;
        a.unit = m.unit;
        a.samples = static_cast<int>(it->second.samples.size());
        a.min = it->second.samples.front().value;
        a.max = a.min;
        double sum = 0.0;
        for (const auto& s : it->second.samples) {
            sum += s.value;
            if (s.value < a.min) a.min = s.value;
            if (s.value > a.max) a.max = s.value;
        }
        a.mean = sum / static_cast<double>(a.samples);
        a.last = it->second.hasValue ? it->second.value : it->second.samples.back().value;
        out.push_back(a);
    }
    return out;
}

bool scoreOf(const PolicyConfig& p, const LinkRecord& link, double& out) {
    double wsum = 0.0;
    double acc = 0.0;
    for (const auto& m : p.metrics) {
        if (!m.inScore()) continue;
        double mean = 0.0;
        if (!windowMean(link, m.key, mean)) continue;
        acc += m.weight * normalizeMetric(m, mean);
        wsum += m.weight;
    }
    if (!(wsum > 0.0)) return false;
    out = acc / wsum;
    return true;
}

bool scoreOfValues(const PolicyConfig& p, const std::map<std::string, double>& values, double& out) {
    double wsum = 0.0;
    double acc = 0.0;
    for (const auto& m : p.metrics) {
        if (!m.inScore()) continue;
        const auto it = values.find(m.key);
        if (it == values.end()) continue;
        acc += m.weight * normalizeMetric(m, it->second);
        wsum += m.weight;
    }
    if (!(wsum > 0.0)) return false;
    out = acc / wsum;
    return true;
}

DerivedMetrics derivedImpl(const EngineState& st) { return computeDerivations(st, nullptr); }

DerivedMetrics derivedFrom(const EngineState& st, const std::map<std::string, double>& projected) {
    return computeDerivations(st, &projected);
}

// ---------------------------------------------------------------- 对外查询

std::optional<LinkQuality> linkQualityImpl(const EngineState& st, const std::string& linkId) {
    const LinkRecord* l = findLink(st, linkId);
    if (l == nullptr || l->removed) return std::nullopt;
    LinkQuality q;
    q.linkId = l->id;
    q.from = l->from;
    q.to = l->to;
    q.metrics = aggregatesOf(st.policy, *l);
    q.score = l->score;
    q.hasScore = l->hasScore;
    LinkState ls;
    if (effectiveLinkState(*l, ls)) {
        q.state = ls;
        q.hasState = true;
    }
    q.manual = l->overridden;
    q.stateChanges = l->stateChanges;
    q.windowMs = st.policy.window.windowMs;
    q.updatedAt = l->updatedAt;
    return q;
}

std::vector<LinkQuality> linkQualitiesImpl(const EngineState& st) {
    std::vector<LinkQuality> out;
    for (const auto& l : st.links) {
        if (l.removed) continue;
        const auto q = linkQualityImpl(st, l.id);
        if (q.has_value()) out.push_back(*q);
    }
    return out;
}

NodeQuality nodeQualityOf(const EngineState& st, const NodeRecord& rec) {
    NodeQuality q;
    q.nodeId = rec.spec.id;
    q.typeKey = rec.spec.typeKey;
    q.derivation = toString(st.policy.nodeAggregation);
    const bool worst = (st.policy.nodeAggregation == NodeAggregation::Worst);
    // 关联边（去重后的无向边，一个节点最多出现一次）
    std::vector<const LinkRecord*> links;
    for (const auto& eid : rec.edges) {
        const LinkRecord* l = findLink(st, eid);
        if (l == nullptr || l->removed) continue;
        links.push_back(l);
        q.edgeIds.push_back(eid);
    }
    for (const auto& m : st.policy.metrics) {
        double sum = 0.0;
        double worstValue = 0.0;
        int n = 0;
        for (const LinkRecord* l : links) {
            double v = 0.0;
            if (!windowMean(*l, m.key, v)) continue;
            if (n == 0) {
                worstValue = v;
            } else if (worst) {
                // "最不利"随方向而定：越大越好的指标取最小，越小越好的指标取最大
                const bool smallerIsWorse = (m.direction == MetricDirection::Higher);
                if (smallerIsWorse ? (v < worstValue) : (v > worstValue)) worstValue = v;
            }
            sum += v;
            ++n;
        }
        if (n == 0) continue;
        MetricAggregate a;
        a.key = m.key;
        a.ledgerKey = m.ledgerKey;
        a.unit = m.unit;
        a.samples = n;
        // 节点口径：worst = 取最不利的关联边；mean = 取关联边均值（TPL-Q-05，口径可配）
        a.mean = worst ? worstValue : (sum / static_cast<double>(n));
        a.min = a.mean;
        a.max = a.mean;
        a.last = a.mean;
        q.metrics.push_back(a);
    }
    double acc = 0.0;
    double pick = 0.0;
    int n = 0;
    for (const LinkRecord* l : links) {
        if (!l->hasScore) continue;
        if (n == 0 || (worst ? l->score < pick : l->score > pick)) pick = l->score;
        acc += l->score;
        ++n;
    }
    if (n > 0) {
        q.score = worst ? pick : (acc / static_cast<double>(n));
        q.hasScore = true;
        q.state = bandOf(st.policy, q.score);  // 节点状态由派生得分分档（无迟滞：主体是链路）
        q.hasState = true;
    }
    return q;
}

std::optional<NodeQuality> nodeQualityImpl(const EngineState& st, const std::string& nodeId) {
    const auto it = st.nodes.find(nodeId);
    if (it == st.nodes.end()) return std::nullopt;
    return nodeQualityOf(st, it->second);
}

// ---------------------------------------------------------------- 归一（纯函数式）

json normalizedShapeImpl(const EngineState& st, const json& event) {
    json out = json::object();
    json metrics = json::object();
    json ledger = json::object();
    json preserved = json::array();
    json rejected = json::array();
    json ignored = json::array();
    std::set<std::string> declared;
    for (const auto& m : st.policy.metrics) declared.insert(m.key);
    if (event.is_object()) {
        for (const auto& m : st.policy.metrics) {
            if (!event.contains(m.key)) {
                preserved.push_back(m.key);
                continue;
            }
            double v = 0.0;
            if (!numericValue(event.at(m.key), m, v)) {
                rejected.push_back(m.key);
                continue;
            }
            metrics[m.key] = v;
            ledger[m.ledgerKey] = v;
        }
        for (auto it = event.begin(); it != event.end(); ++it) {
            if (declared.count(it.key()) != 0) continue;
            if (reservedFields().count(it.key()) != 0) continue;
            ignored.push_back(it.key());
        }
    }
    out["linkId"] = event.is_object() ? event.value("linkId", std::string()) : std::string();
    if (event.is_object()) {
        if (event.contains("from")) out["from"] = event.at("from");
        if (event.contains("to")) out["to"] = event.at("to");
        if (event.contains("state")) out["reportedState"] = event.at("state");
    }
    out["metrics"] = metrics;
    out["ledger"] = ledger;
    out["preserved"] = preserved;
    out["rejected"] = rejected;
    out["ignored"] = ignored;
    return out;
}

// ---------------------------------------------------------------- 观测入口

IngestResult ingestImpl(EngineState& st, const json& event) {
    IngestResult res;
    if (!st.policyLoaded) {
        res.code = static_cast<int>(ErrorCode::Internal);
        res.message = "policy not loaded";
        return res;
    }
    if (!event.is_object()) {
        res.code = static_cast<int>(ErrorCode::BadRequest);
        res.message = "event must be a JSON object";
        ++st.metrics.rejectedObservations;
        return res;
    }
    const std::string linkId = event.value("linkId", std::string());
    if (linkId.empty()) {
        res.code = static_cast<int>(ErrorCode::BadRequest);
        res.message = "linkId required";
        ++st.metrics.rejectedObservations;
        return res;
    }
    LinkRecord* link = findLink(st, linkId);
    if (link == nullptr || link->removed) {
        res.code = static_cast<int>(ErrorCode::NotFound);
        res.message = "link not in topology";
        ++st.metrics.rejectedObservations;
        return res;
    }

    // 时间：事件自带 ts 优先（回放场景），否则取注入时钟；迟滞与窗口都以它为准（P9）
    const int64_t clockNow = nowOf(st);
    int64_t ts = clockNow;
    if (event.contains("ts")) {
        const auto v = asInt(event.at("ts"));
        if (v.has_value()) ts = *v;
    }
    const int64_t ref = std::max(clockNow, ts);

    NormalizedObservation& obs = res.data;
    obs.linkId = linkId;
    obs.ts = ts;
    obs.from = event.value("from", link->from);
    obs.to = event.value("to", link->to);

    // 逐指标归一：缺字段 → 保持原值（TPL-Q-03）
    std::set<std::string> declared;
    for (const auto& m : st.policy.metrics) {
        declared.insert(m.key);
        const auto it = event.find(m.key);
        if (it == event.end()) {
            obs.preserved.push_back(m.key);
            ++st.metrics.preservedFields;
            continue;
        }
        double v = 0.0;
        if (!numericValue(*it, m, v)) {
            obs.rejected.push_back(m.key);  // MUST NOT 用 0 覆盖
            ++st.metrics.rejectedValues;
            continue;
        }
        MetricState& ms = link->metrics[m.key];
        ms.samples.push_back(Sample{ts, v});
        ms.hasValue = true;
        ms.value = v;
        ms.lastTs = ts;
        ++st.metrics.metricUpdates;
    }
    for (auto it = event.begin(); it != event.end(); ++it) {
        if (declared.count(it.key()) != 0) continue;
        if (reservedFields().count(it.key()) != 0) continue;
        obs.ignored.push_back(it.key());
    }
    if (event.contains("state") && event.at("state").is_string()) {
        obs.reportedState = event.at("state").get<std::string>();
    }

    // 滑动窗口裁剪（按窗口长度与样本上限）
    const int64_t cutoff = ref - st.policy.window.windowMs;
    for (auto& kv : link->metrics) {
        std::deque<Sample>& q = kv.second.samples;
        while (!q.empty() && q.front().ts < cutoff) q.pop_front();
        for (std::size_t i = 0; i < q.size();) {
            if (q[i].ts < cutoff) {
                q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        while (static_cast<int>(q.size()) > st.policy.window.maxSamples) q.pop_front();
    }

    // 得分（窗口均值）→ 状态判定（迟滞）
    double score = 0.0;
    if (scoreOf(st.policy, *link, score)) {
        link->score = score;
        link->hasScore = true;
    }
    link->updatedAt = ts;
    const int changesBefore = link->stateChanges;
    judgeLink(st, *link, ts);
    obs.stateChanged = (link->stateChanges != changesBefore);

    appendCurvePoint(st, *link, ts);
    buildShape(st, *link, obs);
    if (obs.stateChanged && !st.log.empty()) {
        obs.hasChange = true;
        obs.change = st.log.back();
    }
    ++st.metrics.observations;
    res.code = 0;
    res.message = "ok";
    return res;
}

}  // namespace detail
}  // namespace topology
