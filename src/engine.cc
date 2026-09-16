// topology · src/engine.cc —— 引擎门面（依赖注入 / 自述 / 观测 / 装配）
#include "internal.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace topology {

namespace detail {

void saveSnapshotIfPossible(EngineState& st) {
    if (!st.deps.store || !st.configured) return;
    try {
        if (!st.deps.store->save(st.topologyId, exportSnapshotImpl(st))) ++st.metrics.storeErrors;
    } catch (...) {
        ++st.metrics.storeErrors;  // 落库失败 MUST NOT 抛异常跨边界（P10）
    }
}

}  // namespace detail

int64_t SystemClock::nowMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- 构造与注入

struct TopologyEngine::Impl {
    detail::EngineState st;
    Impl() = default;
    explicit Impl(const TopologyEngineOptions& opts) { st.deps = opts; }
};

TopologyEngine::TopologyEngine() : impl_(new Impl()) {}
TopologyEngine::TopologyEngine(const TopologyEngineOptions& opts) : impl_(new Impl(opts)) {}
TopologyEngine::~TopologyEngine() = default;
TopologyEngine::TopologyEngine(TopologyEngine&&) noexcept = default;
TopologyEngine& TopologyEngine::operator=(TopologyEngine&&) noexcept = default;

void TopologyEngine::setStore(std::shared_ptr<ITopologyStore> store) { impl_->st.deps.store = std::move(store); }
void TopologyEngine::setClock(std::shared_ptr<IClock> clock) { impl_->st.deps.clock = std::move(clock); }
void TopologyEngine::setSink(std::shared_ptr<ITopologySink> sink) { impl_->st.deps.sink = std::move(sink); }
void TopologyEngine::setLog(std::shared_ptr<ILogSink> log) { impl_->st.deps.log = std::move(log); }
void TopologyEngine::setLayout(std::shared_ptr<ILayoutProvider> layout) { impl_->st.deps.layout = std::move(layout); }

const char* TopologyEngine::errorCodeName(int code) { return topology::errorCodeName(code); }

// ---------------------------------------------------------------- 规则

LoadResult TopologyEngine::loadPolicy(const json& pkg) { return detail::loadPolicyInto(impl_->st, pkg); }

LoadResult TopologyEngine::loadPolicyFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "policy file not readable: " + path;
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    json pkg;
    try {
        pkg = json::parse(text);
    } catch (const std::exception& ex) {
        LoadResult r;
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = std::string("policy file is not valid JSON: ") + ex.what();
        return r;
    }
    return loadPolicy(pkg);
}

LoadResult TopologyEngine::validatePolicy(const json& pkg) { return detail::validatePolicyInto(pkg, nullptr, nullptr); }

LoadResult validatePolicy(const json& pkg) { return detail::validatePolicyInto(pkg, nullptr, nullptr); }

PolicyInfo TopologyEngine::policyInfo() const { return impl_->st.info; }

std::optional<PolicyConfig> TopologyEngine::policy() const {
    if (!impl_->st.policyLoaded) return std::nullopt;
    return impl_->st.policy;
}

// ---------------------------------------------------------------- 拓扑模型

MutationResult TopologyEngine::configureTopology(const std::string& topologyId,
                                                const std::string& structureKey, bool reset) {
    MutationResult r = detail::configureTopologyImpl(impl_->st, topologyId, structureKey, reset);
    if (r.code == 0) detail::saveSnapshotIfPossible(impl_->st);
    return r;
}

MutationResult TopologyEngine::addNode(const NodeSpec& spec) {
    MutationResult r = detail::addNodeImpl(impl_->st, spec);
    if (r.code == 0) detail::saveSnapshotIfPossible(impl_->st);
    return r;
}

MutationResult TopologyEngine::addNodes(const std::vector<NodeSpec>& specs) {
    json added = json::array();
    json issues = json::array();
    for (const auto& s : specs) {
        MutationResult r = detail::addNodeImpl(impl_->st, s);
        if (r.code == 0) {
            added.push_back(r.data);
        } else {
            json j = json::object();
            j["nodeId"] = s.id;
            j["code"] = r.code;
            j["reason"] = r.message;
            issues.push_back(j);
        }
    }
    if (!added.empty()) detail::saveSnapshotIfPossible(impl_->st);
    MutationResult r;
    r.code = issues.empty() ? 0 : static_cast<int>(ErrorCode::BadRequest);
    r.message = issues.empty() ? "ok" : "some nodes were rejected";
    r.data = json::object();
    r.data["added"] = static_cast<int64_t>(added.size());
    r.data["issues"] = issues;
    return r;
}

MutationResult TopologyEngine::addEdge(const EdgeSpec& spec) {
    MutationResult r = detail::addEdgeImpl(impl_->st, spec);
    if (r.code == 0) detail::saveSnapshotIfPossible(impl_->st);
    return r;
}

MutationResult TopologyEngine::addEdges(const std::vector<EdgeSpec>& specs) {
    int added = 0;
    int duplicates = 0;
    json issues = json::array();
    for (const auto& s : specs) {
        MutationResult r = detail::addEdgeImpl(impl_->st, s);
        if (r.code != 0) {
            json j = json::object();
            j["edgeId"] = s.id;
            j["code"] = r.code;
            j["reason"] = r.message;
            issues.push_back(j);
            continue;
        }
        if (r.data.value("duplicate", false)) {
            ++duplicates;
        } else {
            ++added;
        }
    }
    if (added > 0) detail::saveSnapshotIfPossible(impl_->st);
    MutationResult r;
    r.code = issues.empty() ? 0 : static_cast<int>(ErrorCode::BadRequest);
    r.message = issues.empty() ? "ok" : "some edges were rejected";
    r.data = json::object();
    r.data["added"] = added;
    r.data["duplicates"] = duplicates;
    r.data["issues"] = issues;
    return r;
}

MutationResult TopologyEngine::removeEdge(const std::string& edgeId) {
    MutationResult r = detail::removeEdgeImpl(impl_->st, edgeId);
    if (r.code == 0) detail::saveSnapshotIfPossible(impl_->st);
    return r;
}

MutationResult TopologyEngine::removeNode(const std::string& nodeId) {
    MutationResult r = detail::removeNodeImpl(impl_->st, nodeId);
    if (r.code == 0) detail::saveSnapshotIfPossible(impl_->st);
    return r;
}

TopologyView TopologyEngine::topologyView() const { return detail::viewImpl(impl_->st); }

std::optional<TopologyNode> TopologyEngine::node(const std::string& nodeId) const {
    return detail::nodeOf(impl_->st, nodeId);
}

std::optional<TopologyEdge> TopologyEngine::edge(const std::string& edgeId) const {
    return detail::edgeOf(impl_->st, edgeId);
}

ValidationReport TopologyEngine::validate() const { return detail::validateImpl(impl_->st); }

json TopologyEngine::exportSnapshot() const { return detail::exportSnapshotImpl(impl_->st); }

MutationResult TopologyEngine::importSnapshot(const json& snapshot, bool strict) {
    MutationResult r = detail::importSnapshotImpl(impl_->st, snapshot, strict);
    if (r.code == 0) detail::saveSnapshotIfPossible(impl_->st);
    return r;
}

MutationResult TopologyEngine::loadSnapshotFromStore(const std::string& topologyId) {
    MutationResult r;
    if (!impl_->st.deps.store) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "store not injected";
        return r;
    }
    const std::string id = topologyId.empty() ? impl_->st.topologyId : topologyId;
    if (id.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "topologyId required";
        return r;
    }
    json snap;
    bool found = false;
    try {
        found = impl_->st.deps.store->load(id, snap);
    } catch (...) {
        ++impl_->st.metrics.storeErrors;
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "store load failed";
        return r;
    }
    if (!found) {
        r.code = static_cast<int>(ErrorCode::NotFound);
        r.message = "snapshot not found in store";
        return r;
    }
    return importSnapshot(snap);
}

// ---------------------------------------------------------------- 质量聚合

IngestResult TopologyEngine::ingest(const json& event) { return detail::ingestImpl(impl_->st, event); }

MutationResult TopologyEngine::ingestBatch(const std::vector<json>& events) {
    json items = json::array();
    int accepted = 0;
    int rejected = 0;
    int stateChanges = 0;
    for (const auto& e : events) {
        IngestResult r = detail::ingestImpl(impl_->st, e);
        if (r.code == 0) {
            ++accepted;
            if (r.data.stateChanged) ++stateChanges;
        } else {
            ++rejected;
        }
        items.push_back(r.toJson());
    }
    MutationResult r;
    r.code = 0;
    r.message = "ok";
    r.data = json::object();
    r.data["accepted"] = accepted;
    r.data["rejected"] = rejected;
    r.data["stateChanges"] = stateChanges;
    r.data["results"] = items;
    return r;
}

std::optional<LinkQuality> TopologyEngine::linkQuality(const std::string& linkId) const {
    return detail::linkQualityImpl(impl_->st, linkId);
}

std::optional<NodeQuality> TopologyEngine::nodeQuality(const std::string& nodeId) const {
    return detail::nodeQualityImpl(impl_->st, nodeId);
}

std::vector<LinkQuality> TopologyEngine::linkQualities() const {
    return detail::linkQualitiesImpl(impl_->st);
}

DerivedMetrics TopologyEngine::derived() const { return detail::derivedImpl(impl_->st); }

json TopologyEngine::normalizedShape(const json& event) const {
    return detail::normalizedShapeImpl(impl_->st, event);
}

// ---------------------------------------------------------------- 状态机

std::optional<LinkState> TopologyEngine::linkState(const std::string& linkId) const {
    const detail::LinkRecord* l = detail::findLink(impl_->st, linkId);
    if (l == nullptr || l->removed) return std::nullopt;
    LinkState s;
    if (detail::effectiveLinkState(*l, s)) return s;
    return std::nullopt;
}

MutationResult TopologyEngine::setOverride(const std::string& linkId, LinkState state,
                                          const std::string& operatorId, const std::string& reason,
                                          bool replace) {
    return detail::setOverrideImpl(impl_->st, linkId, state, operatorId, reason, replace);
}

MutationResult TopologyEngine::clearOverride(const std::string& linkId, const std::string& operatorId,
                                            const std::string& reason) {
    return detail::clearOverrideImpl(impl_->st, linkId, operatorId, reason);
}

std::vector<ManualOverride> TopologyEngine::overrides() const {
    std::vector<ManualOverride> out;
    for (const auto& l : impl_->st.links) {
        if (l.removed || !l.overridden) continue;
        out.push_back(l.overrideInfo);
    }
    return out;
}

std::vector<StateChange> TopologyEngine::stateLog(const std::string& linkId) const {
    if (linkId.empty()) return impl_->st.log;
    std::vector<StateChange> out;
    for (const auto& c : impl_->st.log) {
        if (c.linkId == linkId) out.push_back(c);
    }
    return out;
}

void TopologyEngine::setPhaseContext(const PhaseContext& ctx) { impl_->st.ctx = ctx; }

PhaseContext TopologyEngine::phaseContext() const { return impl_->st.ctx; }

// ---------------------------------------------------------------- 评估 / 优化 / 曲线

EvaluationResult TopologyEngine::evaluate(const PhaseContext& ctx) {
    if (!impl_->st.policyLoaded) {
        EvaluationResult res;
        res.code = static_cast<int>(ErrorCode::Internal);
        res.message = "policy not loaded";
        return res;
    }
    const DerivedMetrics d = detail::derivedImpl(impl_->st);
    return detail::evaluateImpl(impl_->st, ctx, d, std::string(), true);
}

std::optional<double> TopologyEngine::linkStability() const {
    return detail::linkStabilityImpl(impl_->st);
}

OptimizationResult TopologyEngine::optimize(const PhaseContext& ctx, const std::string& linkId) {
    return detail::optimizeImpl(impl_->st, ctx, linkId);
}

CurveSeries TopologyEngine::curves(const std::string& linkId) const {
    return detail::curvesImpl(impl_->st, linkId);
}

json TopologyEngine::primitives() const { return detail::primitivesImpl(impl_->st); }

// ---------------------------------------------------------------- 自述

Capabilities TopologyEngine::capabilities() const {
    Capabilities c;
    const detail::EngineState& st = impl_->st;
    c.policyLoaded = st.policyLoaded;
    c.schemaVersion = st.info.schemaVersion;
    c.policyVersion = st.info.policyVersion;
    c.digest = st.info.digest;
    c.policiesMajor = kSupportedPoliciesMajor;
    c.nodeTypes = st.info.nodeTypeCount;
    c.structures = st.info.structureCount;
    c.metrics = st.info.metricCount;
    c.evaluationItems = st.info.evaluationItemCount;
    c.parameters = st.info.parameterCount;
    c.windowMs = st.info.windowMs;
    c.topologyId = st.topologyId;
    c.structureKey = st.structureKey;
    int nodes = 0;
    for (const auto& kv : st.nodes) {
        (void)kv;
        ++nodes;
    }
    int edges = 0;
    for (const auto& l : st.links) {
        if (!l.removed) ++edges;
    }
    c.nodes = nodes;
    c.edges = edges;
    c.clockInjected = static_cast<bool>(st.deps.clock);
    c.storeInjected = static_cast<bool>(st.deps.store);
    c.sinkInjected = static_cast<bool>(st.deps.sink);
    c.logInjected = static_cast<bool>(st.deps.log);
    c.layoutInjected = static_cast<bool>(st.deps.layout);
    return c;
}

Metrics TopologyEngine::metrics() const { return impl_->st.metrics; }

}  // namespace topology
