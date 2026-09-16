// topology · src/state.cc —— 状态机与迟滞（TPL-ST-01..06）
//
// 口径：
//   · 分档阈值来自规则，引擎 MUST NOT 硬编码（TPL-ST-01）；
//   · 迟滞：向上/向下迁移使用**不同阈值**（riseMargin/fallMargin）+ 连续 N 次确认（TPL-ST-02）；
//   · 最小驻留：状态变更后 MUST 驻留至少可配时长（TPL-ST-03）；
//   · 每次变更留痕：从哪到哪、越过的阈值与当时得分、起决定作用的指标与数值、逐指标快照（TPL-ST-04）；
//   · 输出只出状态码（green/yellow/red），不出颜色值（TPL-ST-05 / D4）；
//   · 手动覆盖：覆盖期间自动判定不生效，解除后立即回到自动（TPL-ST-06）。
#include "internal.h"

namespace topology {
namespace detail {

namespace {

int rankOf(LinkState s) {
    switch (s) {
        case LinkState::Green: return 0;
        case LinkState::Yellow: return 1;
        case LinkState::Red: return 2;
    }
    return 2;
}

StateBandDef bandDefOf(const PolicyConfig& p, LinkState s) {
    for (const auto& b : p.states) {
        if (b.state == s) return b;
    }
    StateBandDef d;
    d.state = s;
    d.min = 0.0;
    d.key = toString(s);
    return d;
}

}  // namespace

json judgementChecks(const EngineState& st, const LinkRecord& link, LinkState band, double threshold,
                     double margin, int confirmations) {
    json checks = json::object();
    checks["score"] = link.score;
    checks["band"] = toString(band);
    checks["threshold"] = threshold;
    checks["margin"] = margin;
    checks["confirmations"] = confirmations;
    checks["windowMs"] = st.policy.window.windowMs;
    json perMetric = json::object();
    for (const auto& m : st.policy.metrics) {
        const auto it = link.metrics.find(m.key);
        if (it == link.metrics.end() || !it->second.hasValue) continue;
        json c = json::object();
        c["value"] = it->second.value;
        c["normalized"] = normalizeMetric(m, it->second.value);
        c["inScore"] = m.inScore();
        perMetric[m.key] = c;
    }
    checks["metrics"] = perMetric;
    return checks;
}

void appendChange(EngineState& st, StateChange ch) {
    st.log.push_back(ch);
    ++st.metrics.stateChanges;
    const LinkRecord* link = findLink(st, ch.linkId);
    if (link != nullptr) emitChange(st, *link, ch);
}

void judgeLink(EngineState& st, LinkRecord& link, int64_t now) {
    if (!link.hasScore) return;  // 无数据不判定：MUST NOT 编造状态（TPL-Q-03 精神）
    const PolicyConfig& p = st.policy;
    FsmState& f = link.fsm;
    const LinkState raw = bandOf(p, link.score);

    if (!f.hasState) {  // 首次判定：直接落入当前档（不算"变更"）
        f.hasState = true;
        f.state = raw;
        f.since = now;
        f.hasPending = false;
        f.pendingCount = 0;
        return;
    }
    if (link.overridden) return;  // 人工覆盖期间自动判定不生效（TPL-ST-06）

    if (raw == f.state) {
        f.hasPending = false;
        f.pendingCount = 0;
        return;
    }

    const bool up = rankOf(raw) < rankOf(f.state);
    const double threshold = up ? bandMinOf(p, raw) : bandMinOf(p, f.state);
    const double margin = up ? p.hysteresis.riseMargin : p.hysteresis.fallMargin;
    // 迟滞：向上需超出目标档阈值 riseMargin；向下需低于当前档阈值 fallMargin
    const bool crossed = up ? (link.score >= threshold + margin) : (link.score < threshold - margin);
    if (!crossed) {
        f.hasPending = false;
        f.pendingCount = 0;
        ++st.metrics.suppressedChanges;
        return;
    }
    if (f.hasPending && f.pending == raw) {
        ++f.pendingCount;
    } else {
        f.hasPending = true;
        f.pending = raw;
        f.pendingCount = 1;
    }
    if (f.pendingCount < p.hysteresis.confirmCount) {  // 连续 N 次确认
        ++st.metrics.suppressedChanges;
        return;
    }
    if (now - f.since < p.hysteresis.minDwellMs) {  // 最小驻留
        ++st.metrics.suppressedChanges;
        return;
    }

    // 起决定作用的指标：下行取最弱（归一值最小）、上行取最强（归一值最大）
    std::string pickKey;
    double pickRaw = 0.0;
    double pickNorm = 0.0;
    bool found = false;
    for (const auto& m : p.metrics) {
        if (!m.inScore()) continue;
        double mean = 0.0;
        if (!linkMetricMean(link, m.key, mean)) continue;
        const double norm = normalizeMetric(m, mean);
        if (!found || (up ? (norm > pickNorm) : (norm < pickNorm))) {
            found = true;
            pickKey = m.key;
            pickRaw = mean;
            pickNorm = norm;
        }
    }

    StateChange ch;
    ch.ts = now;
    ch.linkId = link.id;
    ch.from = f.state;
    ch.to = raw;
    ch.score = link.score;
    ch.threshold = threshold;
    ch.margin = margin;
    ch.metric = pickKey;
    ch.value = pickRaw;
    ch.normalized = pickNorm;
    ch.confirmations = f.pendingCount;
    ch.manual = false;
    ch.reason = "hysteresis-confirmed";
    ch.checks = judgementChecks(st, link, raw, threshold, margin, f.pendingCount);

    f.state = raw;
    f.since = now;
    f.hasPending = false;
    f.pendingCount = 0;
    ++link.stateChanges;
    appendChange(st, ch);
}

MutationResult setOverrideImpl(EngineState& st, const std::string& linkId, LinkState state,
                               const std::string& operatorId, const std::string& reason, bool replace) {
    MutationResult r;
    if (!st.policyLoaded) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "policy not loaded";
        return r;
    }
    if (operatorId.empty() || reason.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "operatorId and reason are required for a manual override";
        return r;
    }
    LinkRecord* link = findLink(st, linkId);
    if (link == nullptr || link->removed) {
        r.code = static_cast<int>(ErrorCode::NotFound);
        r.message = "link not found";
        r.data = json{{"linkId", linkId}};
        return r;
    }
    if (link->overridden && link->overrideInfo.state == state) {
        r.code = 0;  // 幂等命中：code=0 + idempotent=true（CTR-EC-01 / ADR-C15）
        r.message = "override already in effect";
        r.data = json::object();
        r.data["linkId"] = linkId;
        r.data["state"] = toString(state);
        r.data["idempotent"] = true;
        return r;
    }
    if (link->overridden && !replace) {
        r.code = static_cast<int>(ErrorCode::Conflict);
        r.message = "link already has a different manual override; pass replace=true";
        r.data = json::object();
        r.data["linkId"] = linkId;
        r.data["existing"] = link->overrideInfo.toJson();
        return r;
    }
    LinkState before = state;
    const bool hasBefore = effectiveLinkState(*link, before);
    const int64_t now = nowOf(st);
    link->overridden = true;
    link->overrideInfo.linkId = linkId;
    link->overrideInfo.state = state;
    link->overrideInfo.operatorId = operatorId;
    link->overrideInfo.reason = reason;
    link->overrideInfo.at = now;
    link->fsm.since = now;

    StateChange ch;
    ch.ts = now;
    ch.linkId = linkId;
    ch.from = hasBefore ? before : state;
    ch.to = state;
    ch.score = link->score;
    ch.threshold = bandMinOf(st.policy, state);
    ch.margin = 0.0;
    ch.confirmations = 0;
    ch.manual = true;
    ch.reason = "manual-override";
    ch.checks = judgementChecks(st, *link, state, ch.threshold, 0.0, 0);
    ++link->stateChanges;
    appendChange(st, ch);
    ++st.metrics.manualOverrides;

    AuditEntry a;
    a.at = now;
    a.actor = operatorId;
    a.action = "link-override";
    a.target = linkId;
    a.detail = reason;
    if (st.deps.log) st.deps.log->commandAudit(a);

    r.code = 0;
    r.message = "override applied";
    r.data = json::object();
    r.data["linkId"] = linkId;
    r.data["state"] = toString(state);
    r.data["idempotent"] = false;
    return r;
}

MutationResult clearOverrideImpl(EngineState& st, const std::string& linkId,
                                 const std::string& operatorId, const std::string& reason) {
    MutationResult r;
    if (!st.policyLoaded) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "policy not loaded";
        return r;
    }
    LinkRecord* link = findLink(st, linkId);
    if (link == nullptr || link->removed) {
        r.code = static_cast<int>(ErrorCode::NotFound);
        r.message = "link not found";
        r.data = json{{"linkId", linkId}};
        return r;
    }
    if (!link->overridden) {
        r.code = 0;  // 幂等命中
        r.message = "no override in effect";
        r.data = json::object();
        r.data["linkId"] = linkId;
        r.data["idempotent"] = true;
        return r;
    }
    const LinkState before = link->overrideInfo.state;
    const int64_t now = nowOf(st);
    link->overridden = false;
    link->overrideInfo = ManualOverride();
    ++st.metrics.overrideClears;
    link->fsm.since = now;
    // 解除后立即恢复自动判定（不等迟滞确认：权威来源刚从人工回到自动）
    if (link->hasScore) {
        const LinkState raw = bandOf(st.policy, link->score);
        link->fsm.hasState = true;
        link->fsm.hasPending = false;
        link->fsm.pendingCount = 0;
        if (raw != before) {
            StateChange ch;
            ch.ts = now;
            ch.linkId = linkId;
            ch.from = before;
            ch.to = raw;
            ch.score = link->score;
            ch.threshold = bandMinOf(st.policy, raw);
            ch.margin = 0.0;
            ch.confirmations = 0;
            ch.manual = false;
            ch.reason = "override-cleared";
            ch.checks = judgementChecks(st, *link, raw, ch.threshold, 0.0, 0);
            ++link->stateChanges;
            appendChange(st, ch);
            link->fsm.state = raw;
        } else {
            link->fsm.state = before;
        }
    }
    if (!operatorId.empty() || !reason.empty()) {
        AuditEntry a;
        a.at = now;
        a.actor = operatorId;
        a.action = "link-override-clear";
        a.target = linkId;
        a.detail = reason;
        if (st.deps.log) st.deps.log->commandAudit(a);
    }
    r.code = 0;
    r.message = "override cleared";
    r.data = json::object();
    r.data["linkId"] = linkId;
    r.data["idempotent"] = false;
    r.data["auto"] = link->fsm.hasState ? toString(link->fsm.state) : std::string();
    return r;
}

void emitChange(EngineState& st, const LinkRecord& link, const StateChange& ch) {
    if (!st.deps.sink) return;
    TopologyChangedEvent e;
    e.missionId = st.ctx.missionId;
    e.edgeId = link.id;
    e.from = link.from;
    e.to = link.to;
    e.state = toString(ch.to);
    e.ts = ch.ts;
    json m = json::object();
    m["score"] = ch.score;
    m["threshold"] = ch.threshold;
    m["margin"] = ch.margin;
    m["metric"] = ch.metric;
    m["value"] = ch.value;
    m["normalized"] = ch.normalized;
    m["confirmations"] = ch.confirmations;
    m["manual"] = ch.manual;
    m["reason"] = ch.reason;
    e.metrics = m;
    try {
        st.deps.sink->onTopologyChanged(e);  // protocol §4.4 topology.changed
        st.deps.sink->onStateChanged(ch);    // 迟滞判定留痕（不新增事件名）
    } catch (...) {
        ++st.metrics.sinkErrors;  // Sink 异常 MUST NOT 影响判定结果
    }
}

}  // namespace detail
}  // namespace topology
