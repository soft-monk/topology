// topology · src/model.cc —— 拓扑模型（TPL-MODEL-01..06）
//
// 口径：
//   · 节点类型**只由规则声明的 typeKey 判定**，MUST NOT 用名称子串推断（TPL-MODEL-01）；
//   · 两种结构（平铺网状 / 多层）由规则声明（TPL-MODEL-02）；
//   · 坐标来源可注入（TPL-MODEL-03），引擎不硬编码坐标；
//   · 无向边去重、自环拒绝（TPL-MODEL-04）；
//   · 孤立节点、悬挂边、层级越级连接可检出并给可读原因（TPL-MODEL-05）；
//   · 快照导出/导入（TPL-MODEL-06）。
#include "internal.h"

#include <set>

namespace topology {
namespace detail {

namespace {

std::string undirectedKey(const std::string& a, const std::string& b) {
    return (a < b) ? (a + "\x1f" + b) : (b + "\x1f" + a);
}

MutationResult fail(int code, const std::string& message, const json& data = json::object()) {
    MutationResult r;
    r.code = code;
    r.message = message;
    r.data = data;
    return r;
}

MutationResult ok(const json& data = json::object(), const std::string& message = "ok") {
    MutationResult r;
    r.code = 0;
    r.message = message;
    r.data = data;
    return r;
}

/// 该结构的层里是否声明了某个 tier
bool layerDeclaresTier(const StructureDef& s, int tier) {
    for (const auto& l : s.layers) {
        if (l.tier == tier) return true;
    }
    return false;
}

}  // namespace

int nodeTierOf(const PolicyConfig& p, const StructureDef& s, const NodeTypeDef& t) {
    (void)p;
    (void)s;  // 层级始终来自节点类型声明（TPL-MODEL-01）；结构只决定是否强制层间约束
    return t.tier;
}

LinkRecord* findLink(EngineState& st, const std::string& linkId) {
    const auto it = st.linkIndex.find(linkId);
    if (it == st.linkIndex.end()) return nullptr;
    if (it->second >= st.links.size()) return nullptr;
    return &st.links[it->second];
}

const LinkRecord* findLink(const EngineState& st, const std::string& linkId) {
    const auto it = st.linkIndex.find(linkId);
    if (it == st.linkIndex.end()) return nullptr;
    if (it->second >= st.links.size()) return nullptr;
    return &st.links[it->second];
}

MutationResult configureTopologyImpl(EngineState& st, const std::string& topologyId,
                                     const std::string& structureKey, bool reset) {
    if (!st.policyLoaded) return fail(static_cast<int>(ErrorCode::Internal), "policy not loaded");
    if (topologyId.empty()) return fail(static_cast<int>(ErrorCode::BadRequest), "topologyId required");
    const StructureDef* s = findStructure(st.policy, structureKey);
    if (s == nullptr) {
        json known = json::array();
        for (const auto& x : st.policy.structures) known.push_back(x.key);
        json data = json::object();
        data["structureKey"] = structureKey;
        data["declaredStructures"] = known;
        return fail(static_cast<int>(ErrorCode::BadRequest),
                    "structure not declared by the policy", data);
    }
    if (st.configured && !reset && (!st.nodes.empty() || !st.links.empty())) {
        json data = json::object();
        data["topologyId"] = st.topologyId;
        data["structureKey"] = st.structureKey;
        data["nodes"] = static_cast<int64_t>(st.nodes.size());
        data["edges"] = static_cast<int64_t>(st.links.size());
        return fail(static_cast<int>(ErrorCode::Conflict),
                    "topology already configured; pass reset=true to replace it", data);
    }
    st.topologyId = topologyId;
    st.structureKey = structureKey;
    st.configured = true;
    st.nodes.clear();
    st.links.clear();
    st.linkIndex.clear();
    st.undirected.clear();
    st.log.clear();
    json data = json::object();
    data["topologyId"] = topologyId;
    data["structureKey"] = structureKey;
    data["mode"] = toString(s->mode);
    data["layers"] = static_cast<int64_t>(s->layers.size());
    return ok(data);
}

MutationResult addNodeImpl(EngineState& st, const NodeSpec& spec) {
    if (!st.policyLoaded) return fail(static_cast<int>(ErrorCode::Internal), "policy not loaded");
    if (!st.configured) return fail(static_cast<int>(ErrorCode::Internal), "topology not configured");
    if (spec.id.empty()) return fail(static_cast<int>(ErrorCode::BadRequest), "node id required");
    const NodeTypeDef* t = findNodeType(st.policy, spec.typeKey);
    if (t == nullptr) {
        // 类型判定**只看规则声明的 key**，与 name 无关（TPL-MODEL-01）
        json data = json::object();
        data["nodeId"] = spec.id;
        data["typeKey"] = spec.typeKey;
        json known = json::array();
        for (const auto& x : st.policy.nodeTypes) known.push_back(x.key);
        data["declaredTypes"] = known;
        ++st.metrics.rejectedNodes;
        return fail(static_cast<int>(ErrorCode::BadRequest),
                    "node type not declared by the policy", data);
    }
    if (st.nodes.find(spec.id) != st.nodes.end()) {
        ++st.metrics.rejectedNodes;
        return fail(static_cast<int>(ErrorCode::BadRequest), "duplicate node id",
                    json{{"nodeId", spec.id}});
    }
    const StructureDef* s = findStructure(st.policy, st.structureKey);
    NodeRecord rec;
    rec.spec = spec;
    rec.typeName = t->name;
    rec.graphic = t->graphic;
    rec.tier = s ? nodeTierOf(st.policy, *s, *t) : t->tier;
    if (s && s->mode == StructureMode::Layered && !s->layers.empty() &&
        !layerDeclaresTier(*s, rec.tier)) {
        ++st.metrics.rejectedNodes;
        json data = json::object();
        data["nodeId"] = spec.id;
        data["typeTier"] = rec.tier;
        json tiers = json::array();
        for (const auto& l : s->layers) tiers.push_back(l.tier);
        data["declaredTiers"] = tiers;
        return fail(static_cast<int>(ErrorCode::BadRequest),
                    "node type tier is not declared by the selected structure", data);
    }
    if (!rec.spec.hasPosition && st.deps.layout) {
        LayoutRequest req;
        req.nodeId = spec.id;
        req.clusterId = spec.clusterId;
        req.typeKey = spec.typeKey;
        req.tier = rec.tier;
        LayoutPoint pt;
        if (st.deps.layout->resolve(req, pt)) {
            rec.spec.lng = pt.lng;
            rec.spec.lat = pt.lat;
            rec.spec.hasPosition = true;
        } else {
            ++st.metrics.layoutMisses;
        }
    } else if (!rec.spec.hasPosition && !st.deps.layout) {
        ++st.metrics.layoutMisses;
    }
    st.nodes.emplace(spec.id, rec);
    json data = json::object();
    data["nodeId"] = spec.id;
    data["typeKey"] = spec.typeKey;
    data["tier"] = rec.tier;
    data["hasPosition"] = rec.spec.hasPosition;
    return ok(data);
}

MutationResult addEdgeImpl(EngineState& st, const EdgeSpec& spec) {
    if (!st.policyLoaded) return fail(static_cast<int>(ErrorCode::Internal), "policy not loaded");
    if (!st.configured) return fail(static_cast<int>(ErrorCode::Internal), "topology not configured");
    if (spec.from.empty() || spec.to.empty())
        return fail(static_cast<int>(ErrorCode::BadRequest), "edge endpoints required");
    if (spec.from == spec.to) {
        // TPL-MODEL-04：自环 MUST 被拒绝
        ++st.metrics.rejectedEdges;
        return fail(static_cast<int>(ErrorCode::BadRequest), "self loop rejected",
                    json{{"edgeId", spec.id}, {"nodeId", spec.from}});
    }
    if (st.nodes.find(spec.from) == st.nodes.end()) {
        // TPL-MODEL-05：悬挂边（端点不在节点集）可检出并定位
        ++st.metrics.rejectedEdges;
        json data = json::object();
        data["edgeId"] = spec.id;
        data["path"] = "edge.from";
        data["missingNode"] = spec.from;
        return fail(static_cast<int>(ErrorCode::BadRequest), "dangling edge: endpoint not in node set",
                    data);
    }
    if (st.nodes.find(spec.to) == st.nodes.end()) {
        ++st.metrics.rejectedEdges;
        json data = json::object();
        data["edgeId"] = spec.id;
        data["path"] = "edge.to";
        data["missingNode"] = spec.to;
        return fail(static_cast<int>(ErrorCode::BadRequest), "dangling edge: endpoint not in node set",
                    data);
    }
    if (!spec.id.empty()) {
        const auto byId = st.linkIndex.find(spec.id);
        if (byId != st.linkIndex.end() && !st.links[byId->second].removed) {
            ++st.metrics.duplicateEdges;
            json data = json::object();
            data["edgeId"] = spec.id;
            data["duplicate"] = true;
            data["existingFrom"] = st.links[byId->second].from;
            data["existingTo"] = st.links[byId->second].to;
            return fail(static_cast<int>(ErrorCode::BadRequest), "duplicate edge id", data);
        }
    }
    // 层级越级连接（layered + adjacentOnly）：MUST 可检出并给可读原因
    const StructureDef* s = findStructure(st.policy, st.structureKey);
    if (s && s->mode == StructureMode::Layered && s->adjacentOnly) {
        const int ta = st.nodes.at(spec.from).tier;
        const int tb = st.nodes.at(spec.to).tier;
        const int diff = (ta > tb) ? (ta - tb) : (tb - ta);
        if (diff > 1) {
            ++st.metrics.rejectedEdges;
            json data = json::object();
            data["edgeId"] = spec.id;
            data["fromTier"] = ta;
            data["toTier"] = tb;
            return fail(static_cast<int>(ErrorCode::BadRequest),
                        "tier skip rejected: non-adjacent layers", data);
        }
    }
    // 无向去重（保留现状 seen 集合语义）：重复边只出现一次，按幂等成功返回（CTR-EC-01）
    const std::string key = undirectedKey(spec.from, spec.to);
    const auto dup = st.undirected.find(key);
    if (dup != st.undirected.end()) {
        const LinkRecord* existing = findLink(st, dup->second);
        if (existing != nullptr && !existing->removed) {
            ++st.metrics.duplicateEdges;
            json data = json::object();
            data["edgeId"] = existing->id;
            data["duplicate"] = true;
            data["idempotent"] = true;
            data["from"] = existing->from;
            data["to"] = existing->to;
            return ok(data, "duplicate undirected edge ignored");
        }
    }
    LinkRecord rec;
    rec.id = spec.id.empty() ? ("e:" + spec.from + "-" + spec.to) : spec.id;
    if (st.linkIndex.find(rec.id) != st.linkIndex.end()) {
        ++st.metrics.rejectedEdges;
        return fail(static_cast<int>(ErrorCode::BadRequest), "duplicate edge id after naming",
                    json{{"edgeId", rec.id}});
    }
    rec.from = spec.from;
    rec.to = spec.to;
    for (const auto& m : st.policy.metrics) rec.metrics[m.key] = MetricState();
    st.links.push_back(rec);
    const std::size_t idx = st.links.size() - 1;
    st.linkIndex[rec.id] = idx;
    st.undirected[key] = rec.id;
    st.nodes.at(rec.from).edges.push_back(rec.id);
    st.nodes.at(rec.to).edges.push_back(rec.id);
    json data = json::object();
    data["edgeId"] = rec.id;
    data["from"] = rec.from;
    data["to"] = rec.to;
    data["duplicate"] = false;
    return ok(data);
}

MutationResult removeEdgeImpl(EngineState& st, const std::string& edgeId) {
    LinkRecord* link = findLink(st, edgeId);
    if (link == nullptr || link->removed) {
        return fail(static_cast<int>(ErrorCode::NotFound), "edge not found", json{{"edgeId", edgeId}});
    }
    link->removed = true;
    st.undirected.erase(undirectedKey(link->from, link->to));
    for (auto& kv : st.nodes) {
        auto& v = kv.second.edges;
        v.erase(std::remove(v.begin(), v.end(), edgeId), v.end());
    }
    return ok(json{{"edgeId", edgeId}, {"removed", true}});
}

MutationResult removeNodeImpl(EngineState& st, const std::string& nodeId) {
    const auto it = st.nodes.find(nodeId);
    if (it == st.nodes.end()) {
        return fail(static_cast<int>(ErrorCode::NotFound), "node not found", json{{"nodeId", nodeId}});
    }
    std::vector<std::string> live;
    for (const auto& eid : it->second.edges) {
        const LinkRecord* l = findLink(st, eid);
        if (l != nullptr && !l->removed) live.push_back(eid);
    }
    if (!live.empty()) {
        json data = json::object();
        data["nodeId"] = nodeId;
        data["edges"] = live;
        return fail(static_cast<int>(ErrorCode::Conflict),
                    "node still has edges; remove them first", data);
    }
    st.nodes.erase(it);
    return ok(json{{"nodeId", nodeId}, {"removed", true}});
}

TopologyView viewImpl(const EngineState& st) {
    TopologyView v;
    v.topologyId = st.topologyId;
    v.structureKey = st.structureKey;
    for (const auto& kv : st.nodes) {  // 节点按 id 升序（确定性）
        TopologyNode n;
        n.id = kv.second.spec.id;
        n.typeKey = kv.second.spec.typeKey;
        n.typeName = kv.second.typeName;
        n.name = kv.second.spec.name;
        n.clusterId = kv.second.spec.clusterId;
        n.graphic = kv.second.graphic;
        n.tier = kv.second.tier;
        n.lng = kv.second.spec.lng;
        n.lat = kv.second.spec.lat;
        n.hasPosition = kv.second.spec.hasPosition;
        v.nodes.push_back(n);
    }
    for (const auto& l : st.links) {  // 边按装载顺序（确定性）
        if (l.removed) continue;
        TopologyEdge e;
        e.id = l.id;
        e.from = l.from;
        e.to = l.to;
        if (l.overridden) {
            e.state = l.overrideInfo.state;
            e.hasState = true;
        } else if (l.fsm.hasState) {
            e.state = l.fsm.state;
            e.hasState = true;
        }
        e.score = l.score;
        e.hasScore = l.hasScore;
        v.edges.push_back(e);
    }
    return v;
}

std::optional<TopologyNode> nodeOf(const EngineState& st, const std::string& nodeId) {
    const auto it = st.nodes.find(nodeId);
    if (it == st.nodes.end()) return std::nullopt;
    TopologyNode n;
    n.id = it->second.spec.id;
    n.typeKey = it->second.spec.typeKey;
    n.typeName = it->second.typeName;
    n.name = it->second.spec.name;
    n.clusterId = it->second.spec.clusterId;
    n.graphic = it->second.graphic;
    n.tier = it->second.tier;
    n.lng = it->second.spec.lng;
    n.lat = it->second.spec.lat;
    n.hasPosition = it->second.spec.hasPosition;
    return n;
}

std::optional<TopologyEdge> edgeOf(const EngineState& st, const std::string& edgeId) {
    const LinkRecord* l = findLink(st, edgeId);
    if (l == nullptr || l->removed) return std::nullopt;
    TopologyEdge e;
    e.id = l->id;
    e.from = l->from;
    e.to = l->to;
    if (l->overridden) {
        e.state = l->overrideInfo.state;
        e.hasState = true;
    } else if (l->fsm.hasState) {
        e.state = l->fsm.state;
        e.hasState = true;
    }
    e.score = l->score;
    e.hasScore = l->hasScore;
    return e;
}

ValidationReport validateImpl(const EngineState& st) {
    ValidationReport rep;
    if (!st.policyLoaded) {
        ValidationIssue i;
        i.kind = "policy-not-loaded";
        i.reason = "policy not loaded";
        rep.issues.push_back(i);
        rep.ok = false;
        return rep;
    }
    // 节点类型 + 孤立节点 + 坐标
    for (const auto& kv : st.nodes) {
        const NodeRecord& n = kv.second;
        if (findNodeType(st.policy, n.spec.typeKey) == nullptr) {
            ValidationIssue i;
            i.kind = "unknown-node-type";
            i.entity = n.spec.id;
            i.path = "nodes[" + n.spec.id + "].typeKey";
            i.reason = "node type is not declared by the policy";
            rep.issues.push_back(i);
        }
        bool hasLiveEdge = false;
        for (const auto& eid : n.edges) {
            const LinkRecord* l = findLink(st, eid);
            if (l != nullptr && !l->removed) hasLiveEdge = true;
        }
        if (!hasLiveEdge) {
            ValidationIssue i;
            i.kind = "isolated-node";
            i.entity = n.spec.id;
            i.path = "nodes[" + n.spec.id + "]";
            i.reason = "node has no edge";
            i.fatal = false;
            rep.issues.push_back(i);
        }
        if (!n.spec.hasPosition) {
            ValidationIssue i;
            i.kind = "missing-position";
            i.entity = n.spec.id;
            i.path = "nodes[" + n.spec.id + "]";
            i.reason = "no coordinate from the scenario and none resolved by the layout provider";
            i.fatal = false;
            rep.issues.push_back(i);
        }
    }
    // 边：悬挂、自环、重复、层级越级
    const StructureDef* s = findStructure(st.policy, st.structureKey);
    std::set<std::string> seenPairs;
    int idx = -1;
    for (const auto& l : st.links) {
        ++idx;
        if (l.removed) continue;
        const std::string path = "edges[" + std::to_string(idx) + "]";
        if (l.from == l.to) {
            ValidationIssue i;
            i.kind = "self-loop";
            i.entity = l.id;
            i.path = path + ".from/to";
            i.reason = "edge endpoints are the same node";
            rep.issues.push_back(i);
            continue;
        }
        if (st.nodes.find(l.from) == st.nodes.end()) {
            ValidationIssue i;
            i.kind = "dangling-edge";
            i.entity = l.id;
            i.path = path + ".from";
            i.reason = "endpoint not in node set: " + l.from;
            rep.issues.push_back(i);
            continue;
        }
        if (st.nodes.find(l.to) == st.nodes.end()) {
            ValidationIssue i;
            i.kind = "dangling-edge";
            i.entity = l.id;
            i.path = path + ".to";
            i.reason = "endpoint not in node set: " + l.to;
            rep.issues.push_back(i);
            continue;
        }
        if (!seenPairs.insert(undirectedKey(l.from, l.to)).second) {
            ValidationIssue i;
            i.kind = "duplicate-edge";
            i.entity = l.id;
            i.path = path;
            i.reason = "undirected duplicate of an earlier edge";
            rep.issues.push_back(i);
        }
        if (s && s->mode == StructureMode::Layered && s->adjacentOnly) {
            const int ta = st.nodes.at(l.from).tier;
            const int tb = st.nodes.at(l.to).tier;
            const int diff = (ta > tb) ? (ta - tb) : (tb - ta);
            if (diff > 1) {
                ValidationIssue i;
                i.kind = "tier-skip";
                i.entity = l.id;
                i.path = path;
                i.reason = "connects non-adjacent layers (tier " + std::to_string(ta) + " <-> " +
                           std::to_string(tb) + ")";
                rep.issues.push_back(i);
            }
        }
    }
    rep.ok = true;
    for (const auto& i : rep.issues) {
        if (i.fatal) rep.ok = false;
    }
    return rep;
}

json exportSnapshotImpl(const EngineState& st) {
    json snap = json::object();
    snap["topologyId"] = st.topologyId;
    snap["structureKey"] = st.structureKey;
    snap["policyVersion"] = st.info.policyVersion;
    snap["nodes"] = json::array();
    for (const auto& kv : st.nodes) {
        const NodeRecord& n = kv.second;
        json j = json::object();
        j["id"] = n.spec.id;
        j["typeKey"] = n.spec.typeKey;
        j["name"] = n.spec.name;
        if (!n.spec.clusterId.empty()) j["clusterId"] = n.spec.clusterId;
        j["hasPosition"] = n.spec.hasPosition;
        if (n.spec.hasPosition) {
            j["lng"] = n.spec.lng;
            j["lat"] = n.spec.lat;
        }
        snap["nodes"].push_back(j);
    }
    snap["edges"] = json::array();
    for (const auto& l : st.links) {
        if (l.removed) continue;
        json j = json::object();
        j["id"] = l.id;
        j["from"] = l.from;
        j["to"] = l.to;
        if (l.hasScore) j["score"] = l.score;
        if (l.overridden) {
            j["state"] = toString(l.overrideInfo.state);
            j["manual"] = true;
            json o = json::object();
            o["operatorId"] = l.overrideInfo.operatorId;
            o["reason"] = l.overrideInfo.reason;
            o["at"] = l.overrideInfo.at;
            j["override"] = o;
        } else if (l.fsm.hasState) {
            j["state"] = toString(l.fsm.state);
            j["manual"] = false;
        }
        snap["edges"].push_back(j);
    }
    return snap;
}

bool insertEdgeUnchecked(EngineState& st, const EdgeSpec& spec) {
    if (spec.from.empty() || spec.to.empty()) return false;
    LinkRecord rec;
    rec.id = spec.id.empty() ? ("e:" + spec.from + "-" + spec.to) : spec.id;
    if (st.linkIndex.find(rec.id) != st.linkIndex.end()) return false;
    rec.from = spec.from;
    rec.to = spec.to;
    for (const auto& m : st.policy.metrics) rec.metrics[m.key] = MetricState();
    st.links.push_back(rec);
    const std::size_t idx = st.links.size() - 1;
    st.linkIndex[rec.id] = idx;
    st.undirected[undirectedKey(rec.from, rec.to)] = rec.id;
    const auto a = st.nodes.find(rec.from);
    if (a != st.nodes.end()) a->second.edges.push_back(rec.id);
    const auto b = st.nodes.find(rec.to);
    if (b != st.nodes.end()) b->second.edges.push_back(rec.id);
    return true;
}

MutationResult importSnapshotImpl(EngineState& st, const json& snapshot, bool strict) {
    if (!st.policyLoaded) return fail(static_cast<int>(ErrorCode::Internal), "policy not loaded");
    if (!snapshot.is_object()) return fail(static_cast<int>(ErrorCode::BadRequest), "snapshot must be an object");
    const std::string topologyId = snapshot.value("topologyId", std::string());
    const std::string structureKey = snapshot.value("structureKey", std::string());
    if (topologyId.empty()) return fail(static_cast<int>(ErrorCode::BadRequest), "snapshot.topologyId required");
    if (structureKey.empty())
        return fail(static_cast<int>(ErrorCode::BadRequest), "snapshot.structureKey required");
    if (!snapshot.contains("nodes") || !snapshot.at("nodes").is_array() || !snapshot.contains("edges") ||
        !snapshot.at("edges").is_array()) {
        return fail(static_cast<int>(ErrorCode::BadRequest), "snapshot.nodes/edges must be arrays");
    }
    // 先整体校验，再原子替换（失败 MUST NOT 半装载）
    EngineState tmp = st;
    json issues = json::array();
    MutationResult cfg = configureTopologyImpl(tmp, topologyId, structureKey, true);
    if (cfg.code != 0) return cfg;
    for (const auto& n : snapshot.at("nodes")) {
        if (!n.is_object()) {
            issues.push_back("nodes[] element must be an object");
            continue;
        }
        NodeSpec spec;
        spec.id = n.value("id", std::string());
        spec.typeKey = n.value("typeKey", std::string());
        spec.name = n.value("name", std::string());
        spec.clusterId = n.value("clusterId", std::string());
        spec.hasPosition = n.value("hasPosition", false);
        if (spec.hasPosition) {
            spec.lng = n.value("lng", 0.0);
            spec.lat = n.value("lat", 0.0);
        }
        MutationResult r = addNodeImpl(tmp, spec);
        if (r.code != 0) {
            json d = json::object();
            d["nodeId"] = spec.id;
            d["reason"] = r.message;
            issues.push_back(d);
        }
    }
    for (const auto& e : snapshot.at("edges")) {
        if (!e.is_object()) {
            issues.push_back("edges[] element must be an object");
            continue;
        }
        EdgeSpec spec;
        spec.id = e.value("id", std::string());
        spec.from = e.value("from", std::string());
        spec.to = e.value("to", std::string());
        MutationResult r = addEdgeImpl(tmp, spec);
        if (r.code != 0) {
            json d = json::object();
            d["edgeId"] = spec.id;
            d["reason"] = r.message;
            issues.push_back(d);
            if (!strict) {
                // 容错导入：仍装载该边（缺陷由 validate() 检出并定位）
                if (spec.id.empty()) spec.id = "e:" + spec.from + "-" + spec.to;
                insertEdgeUnchecked(tmp, spec);
            }
        }
    }
    if (!issues.empty() && strict) {
        json data = json::object();
        data["issues"] = issues;
        return fail(static_cast<int>(ErrorCode::BadRequest), "snapshot rejected", data);
    }
    // 恢复状态（状态是快照的一部分：回放复盘要一致）
    const int64_t now = nowOf(tmp);
    for (const auto& e : snapshot.at("edges")) {
        if (!e.is_object()) continue;
        const std::string id = e.value("id", std::string());
        LinkRecord* l = findLink(tmp, id);
        if (l == nullptr) continue;
        if (e.contains("score") && e.at("score").is_number()) {
            l->score = e.at("score").get<double>();
            l->hasScore = true;
        }
        if (e.contains("state") && e.at("state").is_string()) {
            const auto s = linkStateFromString(e.at("state").get<std::string>());
            if (s.has_value()) {
                if (e.value("manual", false)) {
                    l->overridden = true;
                    l->overrideInfo.linkId = id;
                    l->overrideInfo.state = *s;
                    l->overrideInfo.at = now;
                    if (e.contains("override") && e.at("override").is_object()) {
                        l->overrideInfo.operatorId = e.at("override").value("operatorId", std::string());
                        l->overrideInfo.reason = e.at("override").value("reason", std::string());
                    }
                } else {
                    l->fsm.hasState = true;
                    l->fsm.state = *s;
                    l->fsm.since = now;
                }
            }
        }
    }
    st = tmp;
    json data = json::object();
    data["topologyId"] = topologyId;
    data["structureKey"] = structureKey;
    data["nodes"] = static_cast<int64_t>(st.nodes.size());
    data["edges"] = static_cast<int64_t>(st.links.size());
    data["issues"] = issues;
    return ok(data, strict ? "snapshot imported" : "snapshot imported with issues");
}

json primitivesImpl(const EngineState& st) {
    // 消费侧图元适配（map-2d/doc/接口文档.md §3.3）：
    //   LinkItem{id, from:[lng,lat], to:[lng,lat], state?}
    //   ClusterItem{id, lng, lat, name}
    // 哪个节点类型是"集群图元"由规则声明的 graphic 决定（引擎不认识业务名）。
    json out = json::object();
    out["topologyId"] = st.topologyId;
    out["structureKey"] = st.structureKey;
    json links = json::array();
    json clusters = json::array();
    json graphics = json::array();
    json missing = json::array();
    json missingClusters = json::array();
    for (const auto& l : st.links) {
        if (l.removed) continue;
        const auto ita = st.nodes.find(l.from);
        const auto itb = st.nodes.find(l.to);
        if (ita == st.nodes.end() || itb == st.nodes.end()) {
            missing.push_back(l.id);
            continue;
        }
        if (!ita->second.spec.hasPosition || !itb->second.spec.hasPosition) {
            missing.push_back(l.id);
            continue;
        }
        json j = json::object();
        j["id"] = l.id;
        j["from"] = json::array({ita->second.spec.lng, ita->second.spec.lat});
        j["to"] = json::array({itb->second.spec.lng, itb->second.spec.lat});
        if (l.overridden) {
            j["state"] = toString(l.overrideInfo.state);
        } else if (l.fsm.hasState) {
            j["state"] = toString(l.fsm.state);  // 只出状态码（颜色语义属 map-2d，D4）
        }
        links.push_back(j);
    }
    for (const auto& kv : st.nodes) {
        const NodeRecord& n = kv.second;
        if (n.graphic == "cluster") {  // 图元种类名对齐消费侧契约（map-2d ClusterItem）
            if (!n.spec.hasPosition) {
                missingClusters.push_back(n.spec.id);
                continue;
            }
            json c = json::object();
            c["id"] = n.spec.id;
            c["lng"] = n.spec.lng;
            c["lat"] = n.spec.lat;
            if (!n.spec.name.empty()) c["name"] = n.spec.name;
            clusters.push_back(c);
        } else {
            json g = json::object();
            g["id"] = n.spec.id;
            g["graphic"] = n.graphic.empty() ? std::string("node") : n.graphic;
            if (n.spec.hasPosition) {
                g["lng"] = n.spec.lng;
                g["lat"] = n.spec.lat;
            }
            graphics.push_back(g);
        }
    }
    out["links"] = links;
    out["clusters"] = clusters;
    out["graphics"] = graphics;
    out["missingCoordinates"] = missing;
    out["missingClusterCoordinates"] = missingClusters;
    return out;
}

}  // namespace detail
}  // namespace topology
