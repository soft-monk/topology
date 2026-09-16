// topology · src/json.cc —— 对外 JSON 序列化（camelCase；键序 = 成员声明顺序；确定性）
#include "topology/topology_engine.h"

namespace topology {

namespace {

json valueOrNull(bool present, double v) { return present ? json(v) : json(); }

}  // namespace

// ---------------------------------------------------------------- 枚举 ⇄ 字符串

const char* toString(LinkState s) {
    switch (s) {
        case LinkState::Green: return "green";
        case LinkState::Yellow: return "yellow";
        case LinkState::Red: return "red";
    }
    return "unknown";
}

const char* toString(MetricDirection d) {
    return d == MetricDirection::Higher ? "higher" : "lower";
}

const char* toString(StructureMode m) { return m == StructureMode::Mesh ? "mesh" : "layered"; }

const char* toString(NodeAggregation a) { return a == NodeAggregation::Worst ? "worst" : "mean"; }

std::optional<LinkState> linkStateFromString(const std::string& s) {
    if (s == "green") return LinkState::Green;
    if (s == "yellow") return LinkState::Yellow;
    if (s == "red") return LinkState::Red;
    return std::nullopt;
}

std::optional<MetricDirection> metricDirectionFromString(const std::string& s) {
    if (s == "higher") return MetricDirection::Higher;
    if (s == "lower") return MetricDirection::Lower;
    return std::nullopt;
}

std::optional<StructureMode> structureModeFromString(const std::string& s) {
    if (s == "mesh") return StructureMode::Mesh;
    if (s == "layered") return StructureMode::Layered;
    return std::nullopt;
}

std::optional<NodeAggregation> nodeAggregationFromString(const std::string& s) {
    if (s == "worst") return NodeAggregation::Worst;
    if (s == "mean") return NodeAggregation::Mean;
    return std::nullopt;
}

const char* errorCodeName(int code) {
    switch (code) {
        case 0: return "ok";
        case 1000: return "bad-request";
        case 1002: return "conflict";
        case 1003: return "gate-unmet";
        case 1004: return "not-found";
        case 1005: return "internal";
        case 1006: return "version-mismatch";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------- 规则

json toJson(const PolicyInfo& v) {
    json j = json::object();
    j["loaded"] = v.loaded;
    j["policiesNamespace"] = v.policiesNamespace;
    j["schemaVersion"] = v.schemaVersion;
    j["policyVersion"] = v.policyVersion;
    j["digest"] = v.digest;
    j["nodeTypeCount"] = v.nodeTypeCount;
    j["structureCount"] = v.structureCount;
    j["metricCount"] = v.metricCount;
    j["evaluationItemCount"] = v.evaluationItemCount;
    j["parameterCount"] = v.parameterCount;
    j["windowMs"] = v.windowMs;
    j["warnings"] = v.warnings;
    return j;
}

json toJson(const LoadIssue& v) {
    json j = json::object();
    j["path"] = v.path;
    j["field"] = v.field;
    j["reason"] = v.reason;
    return j;
}

json LoadResult::toJson() const {
    json body = topology::toJson(this->data);
    body["issues"] = json::array();
    for (const auto& i : issues) body["issues"].push_back(topology::toJson(i));
    json j = json::object();
    j["code"] = code;
    j["message"] = message;
    j["data"] = body;
    return j;
}

// ---------------------------------------------------------------- 拓扑模型

json toJson(const PhaseContext& v) {
    json j = json::object();
    j["phaseKey"] = v.phaseKey;
    j["seq"] = v.seq;
    j["scenarioKey"] = v.scenarioKey;
    j["enteredAt"] = v.enteredAt;
    j["missionId"] = v.missionId;
    return j;
}

json TopologyNode::toJson() const {
    json j = json::object();
    j["id"] = id;
    j["typeKey"] = typeKey;
    j["typeName"] = typeName;
    j["name"] = name;
    if (!clusterId.empty()) j["clusterId"] = clusterId;
    if (!graphic.empty()) j["graphic"] = graphic;
    j["tier"] = tier;
    if (hasPosition) {
        j["lng"] = lng;
        j["lat"] = lat;
    }
    return j;
}

json TopologyEdge::toJson() const {
    json j = json::object();
    j["id"] = id;
    j["from"] = from;
    j["to"] = to;
    if (hasState) j["state"] = toString(state);  // 只出状态码（颜色语义属 map-2d）
    if (hasScore) j["score"] = score;
    return j;
}

json TopologyView::toJson() const {
    json j = json::object();
    j["topologyId"] = topologyId;
    j["structureKey"] = structureKey;
    j["nodes"] = json::array();
    for (const auto& n : nodes) j["nodes"].push_back(n.toJson());
    j["edges"] = json::array();
    for (const auto& e : edges) j["edges"].push_back(e.toJson());
    return j;
}

json ValidationIssue::toJson() const {
    json j = json::object();
    j["kind"] = kind;
    j["entity"] = entity;
    j["path"] = path;
    j["reason"] = reason;
    j["fatal"] = fatal;
    return j;
}

json ValidationReport::toJson() const {
    json j = json::object();
    j["ok"] = ok;
    j["issues"] = json::array();
    for (const auto& i : issues) j["issues"].push_back(i.toJson());
    return j;
}

json MutationResult::toJson() const {
    json j = json::object();
    j["code"] = code;
    j["message"] = message;
    j["data"] = data;
    return j;
}

// ---------------------------------------------------------------- 质量

json MetricAggregate::toJson() const {
    json j = json::object();
    j["key"] = key;
    j["ledgerKey"] = ledgerKey;
    j["unit"] = unit;
    j["mean"] = mean;
    j["min"] = min;
    j["max"] = max;
    j["last"] = last;
    j["samples"] = samples;
    return j;
}

json StateChange::toJson() const {
    json j = json::object();
    j["ts"] = ts;
    j["linkId"] = linkId;
    j["from"] = toString(from);
    j["to"] = toString(to);
    j["score"] = score;
    j["threshold"] = threshold;
    j["margin"] = margin;
    j["metric"] = metric;
    j["value"] = value;
    j["normalized"] = normalized;
    j["confirmations"] = confirmations;
    j["manual"] = manual;
    j["reason"] = reason;
    j["checks"] = checks;
    return j;
}

json ManualOverride::toJson() const {
    json j = json::object();
    j["linkId"] = linkId;
    j["state"] = toString(state);
    j["operatorId"] = operatorId;
    j["reason"] = reason;
    j["at"] = at;
    return j;
}

json LinkQuality::toJson() const {
    json j = json::object();
    j["linkId"] = linkId;
    j["from"] = from;
    j["to"] = to;
    j["metrics"] = json::array();
    for (const auto& m : metrics) j["metrics"].push_back(m.toJson());
    if (hasScore) j["score"] = score;
    if (hasState) j["state"] = toString(state);
    j["manual"] = manual;
    j["stateChanges"] = stateChanges;
    j["windowMs"] = windowMs;
    j["updatedAt"] = updatedAt;
    return j;
}

json NodeQuality::toJson() const {
    json j = json::object();
    j["nodeId"] = nodeId;
    j["typeKey"] = typeKey;
    j["derivation"] = derivation;
    j["edgeIds"] = edgeIds;
    j["metrics"] = json::array();
    for (const auto& m : metrics) j["metrics"].push_back(m.toJson());
    if (hasScore) j["score"] = score;
    if (hasState) j["state"] = toString(state);
    return j;
}

bool DerivedMetrics::has(const std::string& key) const { return index.find(key) != index.end(); }

double DerivedMetrics::get(const std::string& key, double fallback) const {
    const auto it = index.find(key);
    return it == index.end() ? fallback : it->second;
}

json DerivedMetrics::toJson() const {
    json j = json::object();
    j["topologyId"] = topologyId;
    j["structureKey"] = structureKey;
    json out = json::object();
    for (const auto& kv : this->values) out[kv.first] = kv.second;  // 固定顺序
    j["values"] = out;
    return j;
}

json NormalizedObservation::toJson() const {
    json j = json::object();
    j["linkId"] = linkId;
    j["from"] = from;
    j["to"] = to;
    j["ts"] = ts;
    j["metrics"] = metrics;
    j["ledger"] = ledger;
    j["preserved"] = preserved;
    j["rejected"] = rejected;
    j["ignored"] = ignored;
    if (!reportedState.empty()) j["reportedState"] = reportedState;
    if (hasScore) j["score"] = score;
    if (hasState) j["state"] = toString(state);
    j["stateChanged"] = stateChanged;
    if (hasChange) j["change"] = change.toJson();
    return j;
}

json IngestResult::toJson() const {
    json j = json::object();
    j["code"] = code;
    j["message"] = message;
    j["data"] = data.toJson();
    return j;
}

// ---------------------------------------------------------------- 评估与优化

json EvaluationTerm::toJson() const {
    json j = json::object();
    j["source"] = source;
    j["weight"] = weight;
    j["value"] = value;
    j["contribution"] = contribution;
    return j;
}

json EvaluationItem::toJson() const {
    json j = json::object();
    j["key"] = key;
    j["value"] = value;
    j["weight"] = weight;
    j["normalized"] = normalized;
    j["scaleMin"] = scaleMin;
    j["scaleMax"] = scaleMax;
    j["terms"] = json::array();
    for (const auto& t : terms) j["terms"].push_back(t.toJson());
    if (hasRequirement) {
        json r = json::object();
        r["metric"] = requirementMetric;
        r["op"] = requirementOp;
        r["limit"] = requirementLimit;
        r["actual"] = requirementActual;
        r["satisfied"] = satisfied;
        j["requirement"] = r;
    }
    return j;
}

json EvaluationResult::toJson() const {
    json j = json::object();
    j["code"] = code;
    j["message"] = message;
    j["topologyId"] = topologyId;
    j["structureKey"] = structureKey;
    j["phaseKey"] = phaseKey;
    j["scenarioKey"] = scenarioKey;
    j["missionId"] = missionId;
    j["overall"] = overall;
    j["items"] = json::array();
    for (const auto& i : items) j["items"].push_back(i.toJson());
    j["inputs"] = inputs;
    j["ts"] = ts;
    return j;
}

std::optional<double> EvaluationResult::itemValue(const std::string& key) const {
    for (const auto& i : items) {
        if (i.key == key) return i.value;
    }
    return std::nullopt;
}

json OptimizationSuggestion::toJson() const {
    json j = json::object();
    j["metric"] = metric;
    j["ledgerKey"] = ledgerKey;
    j["unit"] = unit;
    j["current"] = current;
    j["suggested"] = suggested;
    j["delta"] = delta;
    j["steps"] = steps;
    j["impact"] = impact;
    return j;
}

json OptimizationResult::toJson() const {
    json j = json::object();
    j["code"] = code;
    j["message"] = message;
    j["improved"] = improved;
    j["reason"] = reason;
    j["topologyId"] = topologyId;
    j["linkId"] = linkId;
    j["suggestions"] = json::array();
    for (const auto& s : suggestions) j["suggestions"].push_back(s.toJson());
    j["before"] = before.toJson();
    j["after"] = after.toJson();
    j["iterations"] = iterations;
    j["truncated"] = truncated;
    return j;
}

// ---------------------------------------------------------------- 曲线

json CurvePoint::toJson() const {
    json j = json::object();
    j["ts"] = ts;
    j["samples"] = samples;
    j["values"] = values;
    return j;
}

json CurveSeries::toJson() const {
    json j = json::object();
    j["topologyId"] = topologyId;
    j["linkId"] = linkId;
    j["granularityMs"] = granularityMs;
    j["windowMs"] = windowMs;
    j["metrics"] = metrics;
    j["points"] = json::array();
    for (const auto& p : points) j["points"].push_back(p.toJson());
    return j;
}

// ---------------------------------------------------------------- 出口与自述

json TopologyChangedEvent::toJson() const {
    json j = json::object();
    j["missionId"] = missionId;
    j["edgeId"] = edgeId;
    j["from"] = from;
    j["to"] = to;
    j["state"] = state;
    j["metrics"] = metrics;
    j["ts"] = ts;
    return j;
}

json toJson(const Capabilities& v) {
    json j = json::object();
    j["policyLoaded"] = v.policyLoaded;
    j["schemaVersion"] = v.schemaVersion;
    j["policyVersion"] = v.policyVersion;
    j["digest"] = v.digest;
    j["policiesMajor"] = v.policiesMajor;
    j["nodeTypes"] = v.nodeTypes;
    j["structures"] = v.structures;
    j["metrics"] = v.metrics;
    j["evaluationItems"] = v.evaluationItems;
    j["parameters"] = v.parameters;
    j["windowMs"] = v.windowMs;
    j["topologyId"] = v.topologyId;
    j["structureKey"] = v.structureKey;
    j["nodes"] = v.nodes;
    j["edges"] = v.edges;
    j["clockInjected"] = v.clockInjected;
    j["storeInjected"] = v.storeInjected;
    j["sinkInjected"] = v.sinkInjected;
    j["logInjected"] = v.logInjected;
    j["layoutInjected"] = v.layoutInjected;
    return j;
}

json toJson(const Metrics& v) {
    json j = json::object();
    j["observations"] = v.observations;
    j["rejectedObservations"] = v.rejectedObservations;
    j["metricUpdates"] = v.metricUpdates;
    j["preservedFields"] = v.preservedFields;
    j["rejectedValues"] = v.rejectedValues;
    j["stateChanges"] = v.stateChanges;
    j["suppressedChanges"] = v.suppressedChanges;
    j["manualOverrides"] = v.manualOverrides;
    j["overrideClears"] = v.overrideClears;
    j["evaluations"] = v.evaluations;
    j["optimizations"] = v.optimizations;
    j["noImprovement"] = v.noImprovement;
    j["curvePoints"] = v.curvePoints;
    j["duplicateEdges"] = v.duplicateEdges;
    j["rejectedEdges"] = v.rejectedEdges;
    j["rejectedNodes"] = v.rejectedNodes;
    j["layoutMisses"] = v.layoutMisses;
    j["sinkErrors"] = v.sinkErrors;
    j["storeErrors"] = v.storeErrors;
    j["clockRegressions"] = v.clockRegressions;
    j["unknownFields"] = v.unknownFields;
    return j;
}

}  // namespace topology
