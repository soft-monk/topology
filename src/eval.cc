// topology · src/eval.cc —— 网络评估（TPL-EVAL-01..04）
//
// 口径：
//   · 四项评估的算法项与权重**全部来自规则**，输入显式（派生输入快照 + 逐项依据），可复算；
//   · 结构化输出：数值 + 逐项依据 + 阈值判定，**不含自然语言结论**（文案归 llm-provider）；
//   · `linkStability()` 只读取值（供 `scoring` 作为"链路稳定度"输入项）。
#include "internal.h"

namespace topology {
namespace detail {

namespace {

/// 评估的唯一实现（`countIt=false` 时不取时钟、不计数 → 优化内循环可安全调用）
EvaluationResult buildEvaluation(EngineState& st, const PhaseContext& ctx, const DerivedMetrics& d,
                                 bool countIt) {
    const PolicyConfig& p = st.policy;
    EvaluationResult res;
    res.topologyId = st.topologyId;
    res.structureKey = st.structureKey;
    const PhaseContext& c = (ctx.missionId.empty() && ctx.phaseKey.empty()) ? st.ctx : ctx;
    res.missionId = c.missionId;
    res.phaseKey = c.phaseKey;
    res.scenarioKey = c.scenarioKey;

    for (const auto& def : p.evaluation.items) {
        EvaluationItem item;
        item.key = def.key;
        item.weight = def.weight;
        item.scaleMin = def.scaleMin;
        item.scaleMax = def.scaleMax;
        double wsum = 0.0;
        double acc = 0.0;
        for (const auto& t : def.terms) {
            EvaluationTerm term;
            term.source = t.source;
            term.weight = t.weight;
            term.value = d.get(t.source, 0.0);
            term.contribution = t.weight * term.value;
            acc += term.contribution;
            wsum += t.weight;
            item.terms.push_back(term);
        }
        item.value = wsum > 0.0 ? (acc / wsum) : 0.0;
        const double span = item.scaleMax - item.scaleMin;
        item.normalized = span > 0.0 ? clamp01((item.value - item.scaleMin) / span) : 0.0;
        res.items.push_back(item);
    }
    // 阈值判定（TPL-EVAL-04：布尔 + 依据，可复现；可引用其它评估项或派生输入）
    for (std::size_t i = 0; i < res.items.size(); ++i) {
        const EvaluationItemDef* def = findEvaluationItem(p, res.items[i].key);
        if (def == nullptr || !def->hasRequirement) continue;
        const std::string target = def->requirement.metric;
        double actual = 0.0;
        bool found = false;
        for (const auto& other : res.items) {
            if (other.key == target) {
                actual = other.value;
                found = true;
                break;
            }
        }
        if (!found) actual = d.get(target, 0.0);
        EvaluationItem& item = res.items[i];
        item.hasRequirement = true;
        item.requirementMetric = target;
        item.requirementOp = def->requirement.op;
        item.requirementLimit = def->requirement.limit;
        item.requirementActual = actual;
        item.satisfied = (def->requirement.op == ">=") ? (actual >= def->requirement.limit)
                                                       : (actual <= def->requirement.limit);
    }
    double wsum = 0.0;
    double acc = 0.0;
    for (const auto& item : res.items) {
        if (!(item.weight > 0.0)) continue;
        acc += item.weight * item.normalized;
        wsum += item.weight;
    }
    res.overall = wsum > 0.0 ? (acc / wsum) : 0.0;
    res.inputs = d.toJson().at("values");
    res.ts = countIt ? nowOf(st) : (st.hasLastNow ? st.lastNow : 0);
    if (countIt) ++st.metrics.evaluations;
    res.code = 0;
    res.message = "ok";
    return res;
}

}  // namespace

EvaluationResult evaluateImpl(EngineState& st, const PhaseContext& ctx, const DerivedMetrics& d,
                              const std::string& linkId, bool countIt) {
    (void)linkId;
    if (!st.policyLoaded) {
        EvaluationResult res;
        res.code = static_cast<int>(ErrorCode::Internal);
        res.message = "policy not loaded";
        return res;
    }
    return buildEvaluation(st, ctx, d, countIt);
}

EvaluationResult evaluateWith(EngineState& st, const PhaseContext& ctx,
                              const std::map<std::string, double>& projected,
                              const std::string& linkId, bool countIt) {
    if (!st.policyLoaded) {
        EvaluationResult res;
        res.code = static_cast<int>(ErrorCode::Internal);
        res.message = "policy not loaded";
        return res;
    }
    const DerivedMetrics d = derivedFrom(st, projected);
    return evaluateImpl(st, ctx, d, linkId, countIt);
}

std::optional<double> linkStabilityImpl(const EngineState& st) {
    if (!st.policyLoaded) return std::nullopt;
    const std::string& key = st.policy.evaluation.stabilityKey;
    if (key.empty()) return std::nullopt;
    const EvaluationItemDef* def = findEvaluationItem(st.policy, key);
    if (def == nullptr) return std::nullopt;
    const DerivedMetrics d = derivedImpl(st);  // 只读：不取时钟、不计数、不改状态
    double wsum = 0.0;
    double acc = 0.0;
    for (const auto& t : def->terms) {
        acc += t.weight * d.get(t.source, 0.0);
        wsum += t.weight;
    }
    if (!(wsum > 0.0)) return std::nullopt;
    return acc / wsum;
}

}  // namespace detail
}  // namespace topology
