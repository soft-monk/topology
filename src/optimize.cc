// topology · src/optimize.cc —— 优化建议与曲线（TPL-OPT-01..06）
//
// 口径：
//   · 搜索空间（哪些参数可调、步长、上下限）与目标全部来自规则，引擎内 MUST NOT 出现
//     乘性常量或硬编码目标（TPL-OPT-01/02，D2）；
//   · 只产出**建议值**，MUST NOT 直接改状态、MUST NOT 落库；
//   · 幂等：建议是对（当前窗口聚合 + 规则）的纯函数，连续调用 10 次逐字节一致（TPL-OPT-03）；
//   · 单调：整体评估不改善（< minImprovement）→ 返回"无改进"且不带建议（TPL-OPT-05）；
//   · 解释：改了哪些参数 → 哪些指标变化 → 评估变化（TPL-OPT-04）；
//   · 曲线点由真实滑动窗口聚合产生（TPL-OPT-06），MUST NOT 用写死公式。
#include "internal.h"

namespace topology {
namespace detail {

namespace {

double objectiveValue(const EvaluationResult& ev, const std::vector<std::string>& items) {
    if (items.empty()) return ev.overall;
    double wsum = 0.0;
    double acc = 0.0;
    for (const auto& it : ev.items) {
        bool wanted = false;
        for (const auto& k : items) {
            if (k == it.key) wanted = true;
        }
        if (!wanted) continue;
        acc += it.weight * it.normalized;
        wsum += it.weight;
    }
    return wsum > 0.0 ? (acc / wsum) : ev.overall;
}

double overallOf(const EvaluationResult& ev, const std::string& key) {
    for (const auto& it : ev.items) {
        if (it.key == key) return it.value;
    }
    return 0.0;
}

}  // namespace

std::map<std::string, double> baselineValues(const EngineState& st, const std::string& linkId) {
    std::map<std::string, double> out;
    const PolicyConfig& p = st.policy;
    if (!linkId.empty()) {
        const LinkRecord* l = findLink(st, linkId);
        if (l == nullptr || l->removed) return out;
        for (const auto& m : p.metrics) {
            double v = 0.0;
            if (linkMetricMean(*l, m.key, v)) out[m.key] = v;
        }
        return out;
    }
    for (const auto& m : p.metrics) {
        double sum = 0.0;
        int n = 0;
        for (const auto& l : st.links) {
            if (l.removed) continue;
            double v = 0.0;
            if (linkMetricMean(l, m.key, v)) {
                sum += v;
                ++n;
            }
        }
        if (n > 0) out[m.key] = sum / static_cast<double>(n);
    }
    return out;
}

OptimizationResult optimizeImpl(EngineState& st, const PhaseContext& ctx, const std::string& linkId) {
    OptimizationResult res;
    if (!st.policyLoaded) {
        res.code = static_cast<int>(ErrorCode::Internal);
        res.message = "policy not loaded";
        return res;
    }
    const PolicyConfig& p = st.policy;
    res.topologyId = st.topologyId;
    res.linkId = linkId;

    if (!linkId.empty()) {
        const LinkRecord* l = findLink(st, linkId);
        if (l == nullptr || l->removed) {
            res.code = static_cast<int>(ErrorCode::NotFound);
            res.message = "link not found";
            res.reason = "link-not-found";
            return res;
        }
    }
    if (p.optimization.parameters.empty()) {
        // 搜索空间未声明 = 前置条件未满足（1003）
        res.code = static_cast<int>(ErrorCode::GateUnmet);
        res.message = "no search space declared by the policy";
        res.reason = "no-search-space";
        return res;
    }

    const std::map<std::string, double> base = baselineValues(st, linkId);
    std::vector<const OptimizationParamDef*> params;
    for (const auto& param : p.optimization.parameters) {
        if (base.find(param.metric) != base.end()) params.push_back(&param);
    }
    if (params.empty()) {
        res.code = static_cast<int>(ErrorCode::GateUnmet);
        res.message = "no window data for the declared parameters";
        res.reason = "missing-baseline";
        return res;
    }

    const EvaluationResult before = evaluateWith(st, ctx, base, linkId, false);
    res.before = before;
    res.after = before;
    const double beforeObjective = objectiveValue(before, p.optimization.objectiveItems);

    std::map<std::string, double> best = base;
    std::map<std::string, double> current = base;
    double bestObjective = beforeObjective;
    std::map<std::string, int> stepsUsed;

    const int64_t t0 = nowOf(st);
    int iterations = 0;
    bool truncated = false;
    bool progress = true;
    bool stop = false;
    while (progress && !stop) {
        progress = false;
        for (const auto* param : params) {
            if (iterations >= p.optimization.maxIterations) {
                truncated = true;  // R4：迭代上限，返回已得最优并标注截断
                stop = true;
                break;
            }
            const MetricDef* md = findMetric(p, param->metric);
            const double dir = (md != nullptr && md->direction == MetricDirection::Lower) ? -1.0 : 1.0;
            const double from = best[param->metric];
            const double candidate = clampRange(from + dir * param->step, param->min, param->max);
            if (candidate == from) continue;  // 已到搜索空间边界
            std::map<std::string, double> trial = best;
            trial[param->metric] = candidate;
            const EvaluationResult ev = evaluateWith(st, ctx, trial, linkId, false);
            ++iterations;
            // 单调性：只有"严格改善且达到声明的最小改进量"才接受（TPL-OPT-05）
            if (ev.overall > bestObjective &&
                (ev.overall - bestObjective) >= p.optimization.minImprovement) {
                best = trial;
                bestObjective = ev.overall;
                res.after = ev;
                stepsUsed[param->metric] = stepsUsed[param->metric] + 1;
                progress = true;
            }
            const int64_t nowMs = nowOf(st);
            if (nowMs - t0 >= p.optimization.maxMs) {  // R4：时限（以注入时钟计量）
                truncated = true;
                stop = true;
                break;
            }
        }
    }
    res.iterations = iterations;
    res.truncated = truncated;

    // 只产出建议值（不改状态）；未改善即"无改进"（TPL-OPT-05）
    const bool improved = (bestObjective > beforeObjective) &&
                          (bestObjective - beforeObjective) >= p.optimization.minImprovement;
    if (!improved) {
        res.improved = false;
        res.reason = "no-improvement";
        res.suggestions.clear();
        res.after = before;
        ++st.metrics.noImprovement;
    } else {
        res.improved = true;
        res.reason = truncated ? "truncated" : "improved";
        for (const auto* param : params) {
            const double from = current[param->metric];
            const double to = best[param->metric];
            if (to == from) continue;
            const MetricDef* md = findMetric(p, param->metric);
            OptimizationSuggestion s;
            s.metric = param->metric;
            s.ledgerKey = md != nullptr ? md->ledgerKey : toSnakeCase(param->metric);
            s.unit = md != nullptr ? md->unit : std::string();
            s.current = from;
            s.suggested = to;
            s.delta = to - from;
            s.steps = stepsUsed.count(param->metric) ? stepsUsed[param->metric] : 0;
            json impact = json::object();  // 评估变化（逐项前后对比，可核对）
            for (const auto& item : res.after.items) {
                json j = json::object();
                j["before"] = overallOf(res.before, item.key);
                j["after"] = overallOf(res.after, item.key);
                j["delta"] = overallOf(res.after, item.key) - overallOf(res.before, item.key);
                impact[item.key] = j;
            }
            s.impact = impact;
            res.suggestions.push_back(s);
        }
    }
    res.code = 0;
    res.message = improved ? "ok" : "no improvement";
    ++st.metrics.optimizations;
    return res;
}

CurveSeries curvesImpl(const EngineState& st, const std::string& linkId) {
    CurveSeries series;
    series.topologyId = st.topologyId;
    series.linkId = linkId;
    series.granularityMs = st.policy.curves.granularityMs;
    series.windowMs = st.policy.window.windowMs;
    series.metrics = st.policy.curves.metrics;
    const LinkRecord* l = findLink(st, linkId);
    if (l == nullptr || l->removed) return series;
    for (const auto& p : l->curve) series.points.push_back(p);
    return series;
}

}  // namespace detail
}  // namespace topology
