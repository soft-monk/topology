// topology · src/policy.cc —— 规则包装载与校验（kind:"linkThresholds"）
//
// 依据：protocol.md §5（骨架 / 版本语义 / CTR-PL-01..08）
//      需求专篇 TPL-MODEL-01/02（节点类型与层级由规则声明）、TPL-Q-01（指标集可配）、
//      TPL-ST-01/02/03（阈值、迟滞、最小驻留由规则声明）、TPL-EVAL-01（算法可配）、
//      TPL-OPT-02（搜索空间）、TPL-OPT-06（曲线粒度）
//
// 口径：引擎内 MUST NOT 出现业务名词与业务数值；本文件只认识"形状"。
#include "internal.h"

#include <set>
#include <sstream>

namespace topology {
namespace detail {

namespace {

const std::set<std::string>& topLevelKeys() {
    static const std::set<std::string> kKeys = {
        "policiesNamespace", "schemaVersion", "kind", "items",  "structures",
        "states",            "hysteresis",    "metrics", "coverage", "meshProgress",
        "aggregation",       "evaluation",    "optimization", "curves",
    };
    return kKeys;
}

bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    const char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

/// 校验上下文：逐条问题 + 未知字段计数（CTR-PL-02/03）
struct Ctx {
    PolicyConfig cfg;
    PolicyInfo info;
    std::vector<LoadIssue> issues;
    int unknownFields = 0;
    int versionMajor = 0;
    bool versionBad = false;

    void err(const std::string& path, const std::string& field, const std::string& reason) {
        LoadIssue i;
        i.path = path;
        i.field = field;
        i.reason = reason;
        issues.push_back(i);
    }
    void warn(const std::string& text) {
        info.warnings.push_back(text);
        ++unknownFields;
    }
    void scanUnknown(const json& obj, const std::set<std::string>& known,
                     const std::string& path) {
        if (!obj.is_object()) return;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (known.find(it.key()) == known.end()) {
                warn(path + "." + it.key() + " (unknown field, ignored)");
            }
        }
    }
    bool num(const json& v, const std::string& path, const std::string& field, double& out) {
        if (!v.is_number()) {
            err(path, field, "must be a number");
            return false;
        }
        out = v.get<double>();
        return true;
    }
    bool str(const json& v, const std::string& path, const std::string& field, std::string& out,
             bool allowEmpty = false) {
        if (!v.is_string()) {
            err(path, field, "must be a string");
            return false;
        }
        out = v.get<std::string>();
        if (!allowEmpty && out.empty()) {
            err(path, field, "must not be empty");
            return false;
        }
        return true;
    }
};

bool parseSchemaVersion(const std::string& s, int& major, int& minor, int& patch) {
    int a = 0, b = 0, c = 0;
    char extra = 0;
    if (std::sscanf(s.c_str(), "%d.%d.%d%c", &a, &b, &c, &extra) != 3) return false;
    if (a < 0 || b < 0 || c < 0) return false;
    major = a;
    minor = b;
    patch = c;
    return true;
}

// ---- 段解析 -------------------------------------------------------------

void parseNodeTypes(Ctx& c, const json& items) {
    if (!items.is_array() || items.empty()) {
        c.err("items", "items", "must be a non-empty array of node types");
        return;
    }
    std::set<std::string> seen;
    const std::set<std::string> known = {"key", "name", "tier", "graphic"};
    for (std::size_t i = 0; i < items.size(); ++i) {
        const json& e = items[i];
        const std::string path = "items[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, known, path);
        NodeTypeDef t;
        if (!e.contains("key") || !c.str(e.at("key"), path, "key", t.key)) continue;
        if (!isIdentifier(t.key)) {
            c.err(path, "key", "must match ^[A-Za-z][A-Za-z0-9_]*$");
            continue;
        }
        if (!seen.insert(t.key).second) {
            c.err(path, "key", "duplicate node type key");
            continue;
        }
        if (!e.contains("name") || !c.str(e.at("name"), path, "name", t.name)) continue;
        if (e.contains("tier")) {
            if (!e.at("tier").is_number_integer()) {
                c.err(path, "tier", "must be an integer");
                continue;
            }
            t.tier = e.at("tier").get<int>();
            if (t.tier < 0) {
                c.err(path, "tier", "must be >= 0");
                continue;
            }
        }
        if (e.contains("graphic")) c.str(e.at("graphic"), path, "graphic", t.graphic);
        c.cfg.nodeTypes.push_back(t);
    }
}

void parseStructures(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"key", "mode", "layers", "adjacentOnly"};
    const std::set<std::string> layerKnown = {"key", "name", "tier"};
    if (!seg.is_array() || seg.empty()) {
        c.err("structures", "structures", "must be a non-empty array");
        return;
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const json& e = seg[i];
        const std::string path = "structures[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, known, path);
        StructureDef s;
        if (!e.contains("key") || !c.str(e.at("key"), path, "key", s.key)) continue;
        if (!seen.insert(s.key).second) {
            c.err(path, "key", "duplicate structure key");
            continue;
        }
        std::string mode;
        if (!e.contains("mode") || !c.str(e.at("mode"), path, "mode", mode)) continue;
        const auto m = structureModeFromString(mode);
        if (!m.has_value()) {
            c.err(path, "mode", "must be \"mesh\" or \"layered\"");
            continue;
        }
        s.mode = *m;
        if (e.contains("adjacentOnly")) {
            if (!e.at("adjacentOnly").is_boolean()) {
                c.err(path, "adjacentOnly", "must be a boolean");
                continue;
            }
            s.adjacentOnly = e.at("adjacentOnly").get<bool>();
        }
        if (e.contains("layers")) {
            if (!e.at("layers").is_array()) {
                c.err(path, "layers", "must be an array");
                continue;
            }
            std::set<std::string> layerKeys;
            for (std::size_t k = 0; k < e.at("layers").size(); ++k) {
                const json& le = e.at("layers")[k];
                const std::string lpath = path + ".layers[" + std::to_string(k) + "]";
                if (!le.is_object()) {
                    c.err(lpath, "", "must be an object");
                    continue;
                }
                c.scanUnknown(le, layerKnown, lpath);
                LayerDef l;
                if (!le.contains("key") || !c.str(le.at("key"), lpath, "key", l.key)) continue;
                if (!layerKeys.insert(l.key).second) {
                    c.err(lpath, "key", "duplicate layer key");
                    continue;
                }
                if (le.contains("name")) c.str(le.at("name"), lpath, "name", l.name, true);
                if (le.contains("tier")) {
                    if (!le.at("tier").is_number_integer()) {
                        c.err(lpath, "tier", "must be an integer");
                        continue;
                    }
                    l.tier = le.at("tier").get<int>();
                }
                s.layers.push_back(l);
            }
        }
        if (s.mode == StructureMode::Layered && s.layers.empty()) {
            c.err(path, "layers", "layered structure must declare at least one layer");
            continue;
        }
        c.cfg.structures.push_back(s);
    }
}

void parseStates(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"key", "min"};
    if (!seg.is_array() || seg.size() != 3) {
        c.err("states", "states", "must declare exactly the three bands");
        return;
    }
    std::set<int> seenStates;
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const json& e = seg[i];
        const std::string path = "states[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, known, path);
        StateBandDef b;
        if (!e.contains("key") || !c.str(e.at("key"), path, "key", b.key)) continue;
        const auto st = linkStateFromString(b.key);
        if (!st.has_value()) {
            c.err(path, "key", "must be one of the three state keys");
            continue;
        }
        b.state = *st;
        if (!seenStates.insert(static_cast<int>(*st)).second) {
            c.err(path, "key", "duplicate state band");
            continue;
        }
        if (!e.contains("min") || !c.num(e.at("min"), path, "min", b.min)) continue;
        if (b.min < 0.0 || b.min > 1.0) {
            c.err(path, "min", "must be within [0,1] (score is normalized)");
            continue;
        }
        c.cfg.states.push_back(b);
    }
    if (c.cfg.states.size() == 3) {
        std::sort(c.cfg.states.begin(), c.cfg.states.end(),
                  [](const StateBandDef& a, const StateBandDef& b) { return a.min > b.min; });
    }
}

void parseHysteresis(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"riseMargin", "fallMargin", "confirmCount", "minDwellMs"};
    if (!seg.is_object()) {
        c.err("hysteresis", "hysteresis", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "hysteresis");
    HysteresisDef h;
    if (seg.contains("riseMargin") && !c.num(seg.at("riseMargin"), "hysteresis", "riseMargin", h.riseMargin))
        return;
    if (seg.contains("fallMargin") && !c.num(seg.at("fallMargin"), "hysteresis", "fallMargin", h.fallMargin))
        return;
    if (seg.contains("confirmCount")) {
        if (!seg.at("confirmCount").is_number_integer()) {
            c.err("hysteresis", "confirmCount", "must be an integer >= 1");
            return;
        }
        h.confirmCount = seg.at("confirmCount").get<int>();
    }
    if (seg.contains("minDwellMs")) {
        const auto v = asInt(seg.at("minDwellMs"));
        if (!v.has_value()) {
            c.err("hysteresis", "minDwellMs", "must be an integer (epoch-ms duration)");
            return;
        }
        h.minDwellMs = *v;
    }
    if (h.riseMargin < 0.0 || h.fallMargin < 0.0) {
        c.err("hysteresis", "riseMargin/fallMargin", "must be >= 0");
        return;
    }
    if (h.confirmCount < 1) {
        c.err("hysteresis", "confirmCount", "must be >= 1");
        return;
    }
    if (h.minDwellMs < 0) {
        c.err("hysteresis", "minDwellMs", "must be >= 0");
        return;
    }
    // TPL-ST-02：装载的规则 MUST 真的产生迟滞（否则状态会在阈值附近抖动）
    if (!(h.riseMargin > 0.0) && !(h.fallMargin > 0.0) && h.confirmCount <= 1) {
        c.err("hysteresis", "riseMargin/fallMargin/confirmCount",
              "no hysteresis declared: at least one margin > 0 or confirmCount > 1 is required");
        return;
    }
    if (h.minDwellMs == 0) c.warn("hysteresis.minDwellMs = 0 (no minimum dwell declared)");
    c.cfg.hysteresis = h;
}

void parseMetrics(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"window", "items"};
    const std::set<std::string> itemKnown = {"key",  "ledgerKey", "unit", "direction",
                                             "min",  "max",       "weight", "valueMap"};
    if (!seg.is_object()) {
        c.err("metrics", "metrics", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "metrics");
    if (seg.contains("window")) {
        const json& w = seg.at("window");
        if (!w.is_object()) {
            c.err("metrics.window", "window", "must be an object");
        } else {
            c.scanUnknown(w, {"windowMs", "maxSamples"}, "metrics.window");
            if (w.contains("windowMs")) {
                const auto v = asInt(w.at("windowMs"));
                if (!v.has_value() || *v <= 0) {
                    c.err("metrics.window", "windowMs", "must be an integer > 0");
                } else {
                    c.cfg.window.windowMs = *v;
                }
            }
            if (w.contains("maxSamples")) {
                const auto v = asInt(w.at("maxSamples"));
                if (!v.has_value() || *v <= 0) {
                    c.err("metrics.window", "maxSamples", "must be an integer > 0");
                } else {
                    c.cfg.window.maxSamples = static_cast<int>(*v);
                }
            }
        }
    }
    if (!seg.contains("items") || !seg.at("items").is_array() || seg.at("items").empty()) {
        c.err("metrics.items", "items", "must be a non-empty array of metrics");
        return;
    }
    const json& items = seg.at("items");
    std::set<std::string> seenKeys;
    std::set<std::string> seenLedger;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const json& e = items[i];
        const std::string path = "metrics.items[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, itemKnown, path);
        MetricDef m;
        if (!e.contains("key") || !c.str(e.at("key"), path, "key", m.key)) continue;
        if (!isIdentifier(m.key)) {
            c.err(path, "key", "must match ^[A-Za-z][A-Za-z0-9_]*$");
            continue;
        }
        if (!seenKeys.insert(m.key).second) {
            c.err(path, "key", "duplicate metric key");
            continue;
        }
        m.ledgerKey = toSnakeCase(m.key);
        if (e.contains("ledgerKey") && !c.str(e.at("ledgerKey"), path, "ledgerKey", m.ledgerKey))
            continue;
        if (!seenLedger.insert(m.ledgerKey).second) {
            c.err(path, "ledgerKey", "duplicate ledger key");
            continue;
        }
        if (e.contains("unit")) c.str(e.at("unit"), path, "unit", m.unit, true);
        if (!e.contains("direction")) {
            c.err(path, "direction", "required (higher|lower)");
            continue;
        }
        std::string dir;
        if (!c.str(e.at("direction"), path, "direction", dir)) continue;
        const auto d = metricDirectionFromString(dir);
        if (!d.has_value()) {
            c.err(path, "direction", "must be \"higher\" or \"lower\"");
            continue;
        }
        m.direction = *d;
        if (!e.contains("min") || !c.num(e.at("min"), path, "min", m.min)) continue;
        if (!e.contains("max") || !c.num(e.at("max"), path, "max", m.max)) continue;
        if (!(m.max > m.min)) {
            c.err(path, "min/max", "must satisfy min < max");
            continue;
        }
        if (e.contains("weight")) {
            if (!c.num(e.at("weight"), path, "weight", m.weight)) continue;
            if (m.weight < 0.0) {
                c.err(path, "weight", "must be >= 0");
                continue;
            }
        }
        if (e.contains("valueMap")) {
            if (!e.at("valueMap").is_object()) {
                c.err(path, "valueMap", "must be an object of {text: number}");
                continue;
            }
            for (auto it = e.at("valueMap").begin(); it != e.at("valueMap").end(); ++it) {
                if (!it.value().is_number()) {
                    c.err(path + ".valueMap", it.key(), "must be a number");
                    continue;
                }
                m.valueMap[it.key()] = it.value().get<double>();
            }
        }
        c.cfg.metrics.push_back(m);
    }
    bool anyInScore = false;
    for (const auto& m : c.cfg.metrics) {
        if (m.inScore()) anyInScore = true;
    }
    if (!c.cfg.metrics.empty() && !anyInScore) {
        c.err("metrics.items", "weight",
              "at least one metric must declare weight > 0 (score cannot be formed otherwise)");
    }
}

void parseCoverage(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"mode", "source", "targetAreaKm2"};
    if (!seg.is_object()) {
        c.err("coverage", "coverage", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "coverage");
    if (seg.contains("mode") && !c.str(seg.at("mode"), "coverage", "mode", c.cfg.coverage.mode)) return;
    if (c.cfg.coverage.mode != "sum" && c.cfg.coverage.mode != "max") {
        c.err("coverage", "mode", "must be \"sum\" or \"max\"");
        return;
    }
    if (seg.contains("source") && !c.str(seg.at("source"), "coverage", "source", c.cfg.coverage.source))
        return;
    if (c.cfg.coverage.source.empty() || findMetric(c.cfg, c.cfg.coverage.source) == nullptr) {
        c.err("coverage", "source", "must reference a declared metric key");
        return;
    }
    if (seg.contains("targetAreaKm2")) {
        if (!c.num(seg.at("targetAreaKm2"), "coverage", "targetAreaKm2", c.cfg.coverage.targetAreaKm2))
            return;
    }
    if (!(c.cfg.coverage.targetAreaKm2 > 0.0)) {
        c.err("coverage", "targetAreaKm2", "must be > 0");
    }
}

void parseMeshProgress(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"components", "scale", "unit"};
    const std::set<std::string> compKnown = {"source", "weight"};
    if (!seg.is_object()) {
        c.err("meshProgress", "meshProgress", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "meshProgress");
    if (seg.contains("scale")) {
        if (!c.num(seg.at("scale"), "meshProgress", "scale", c.cfg.meshProgress.scale)) return;
        if (!(c.cfg.meshProgress.scale > 0.0)) {
            c.err("meshProgress", "scale", "must be > 0");
            return;
        }
    }
    if (seg.contains("unit")) c.str(seg.at("unit"), "meshProgress", "unit", c.cfg.meshProgress.unit, true);
    if (!seg.contains("components")) {
        c.err("meshProgress", "components", "required (explicit, recomputable)");
        return;
    }
    if (!seg.at("components").is_array() || seg.at("components").empty()) {
        c.err("meshProgress.components", "components", "must be a non-empty array");
        return;
    }
    double sumWeight = 0.0;
    for (std::size_t i = 0; i < seg.at("components").size(); ++i) {
        const json& e = seg.at("components")[i];
        const std::string path = "meshProgress.components[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, compKnown, path);
        MeshComponentDef comp;
        if (!e.contains("source") || !c.str(e.at("source"), path, "source", comp.source)) continue;
        if (!isDerivationName(comp.source, c.cfg)) {
            c.err(path, "source", "unknown derivation input");
            continue;
        }
        if (!e.contains("weight") || !c.num(e.at("weight"), path, "weight", comp.weight)) continue;
        if (comp.weight < 0.0) {
            c.err(path, "weight", "must be >= 0");
            continue;
        }
        sumWeight += comp.weight;
        c.cfg.meshProgress.components.push_back(comp);
    }
    if (!c.cfg.meshProgress.components.empty() && !(sumWeight > 0.0)) {
        c.err("meshProgress.components", "weight", "sum of weights must be > 0");
    }
}

void parseAggregation(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"node"};
    if (!seg.is_object()) {
        c.err("aggregation", "aggregation", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "aggregation");
    if (!seg.contains("node")) return;
    std::string mode;
    if (!c.str(seg.at("node"), "aggregation", "node", mode)) return;
    const auto a = nodeAggregationFromString(mode);
    if (!a.has_value()) {
        c.err("aggregation", "node", "must be \"worst\" or \"mean\"");
        return;
    }
    c.cfg.nodeAggregation = *a;
}

void parseEvaluation(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"items", "stabilityKey", "scale", "requirement"};
    const std::set<std::string> itemKnown = {"key", "weight", "scale", "terms", "requirement"};
    const std::set<std::string> termKnown = {"source", "weight"};
    const std::set<std::string> reqKnown = {"metric", "op", "limit"};
    if (!seg.is_object()) {
        c.err("evaluation", "evaluation", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "evaluation");
    if (!seg.contains("items") || !seg.at("items").is_array() || seg.at("items").empty()) {
        c.err("evaluation.items", "items", "must be a non-empty array of evaluation items");
        return;
    }
    const json& items = seg.at("items");
    if (!seg.contains("stabilityKey")) {
        c.err("evaluation", "stabilityKey",
              "required: declares which evaluation item is the read-only link stability");
    } else {
        c.str(seg.at("stabilityKey"), "evaluation", "stabilityKey", c.cfg.evaluation.stabilityKey);
    }
    std::set<std::string> seenKeys;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const json& e = items[i];
        const std::string path = "evaluation.items[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, itemKnown, path);
        EvaluationItemDef it;
        if (!e.contains("key") || !c.str(e.at("key"), path, "key", it.key)) continue;
        if (!isIdentifier(it.key)) {
            c.err(path, "key", "must match ^[A-Za-z][A-Za-z0-9_]*$");
            continue;
        }
        if (!seenKeys.insert(it.key).second) {
            c.err(path, "key", "duplicate evaluation item key");
            continue;
        }
        if (e.contains("weight")) {
            if (!c.num(e.at("weight"), path, "weight", it.weight)) continue;
        }
        if (it.weight < 0.0) {
            c.err(path, "weight", "must be >= 0");
            continue;
        }
        if (e.contains("scale")) {
            const json& s = e.at("scale");
            if (!s.is_object()) {
                c.err(path + ".scale", "scale", "must be an object");
                continue;
            }
            c.scanUnknown(s, {"min", "max"}, path + ".scale");
            if (s.contains("min") && !c.num(s.at("min"), path + ".scale", "min", it.scaleMin))
                continue;
            if (s.contains("max") && !c.num(s.at("max"), path + ".scale", "max", it.scaleMax))
                continue;
        }
        if (!(it.scaleMax > it.scaleMin)) {
            c.err(path, "scale", "must satisfy min < max");
            continue;
        }
        if (!e.contains("terms") || !e.at("terms").is_array() || e.at("terms").empty()) {
            c.err(path + ".terms", "terms", "must be a non-empty array");
            continue;
        }
        double termWeight = 0.0;
        for (std::size_t k = 0; k < e.at("terms").size(); ++k) {
            const json& te = e.at("terms")[k];
            const std::string tpath = path + ".terms[" + std::to_string(k) + "]";
            if (!te.is_object()) {
                c.err(tpath, "", "must be an object");
                continue;
            }
            c.scanUnknown(te, termKnown, tpath);
            EvaluationTermDef t;
            if (!te.contains("source") || !c.str(te.at("source"), tpath, "source", t.source)) continue;
            if (!isDerivationName(t.source, c.cfg)) {
                c.err(tpath, "source", "unknown derivation input");
                continue;
            }
            if (te.contains("weight")) {
                if (!c.num(te.at("weight"), tpath, "weight", t.weight)) continue;
            }
            if (t.weight < 0.0) {
                c.err(tpath, "weight", "must be >= 0");
                continue;
            }
            termWeight += t.weight;
            it.terms.push_back(t);
        }
        if (it.terms.empty() || !(termWeight > 0.0)) {
            c.err(path + ".terms", "weight", "sum of term weights must be > 0");
            continue;
        }
        if (e.contains("requirement")) {
            const json& r = e.at("requirement");
            if (!r.is_object()) {
                c.err(path + ".requirement", "requirement", "must be an object");
                continue;
            }
            c.scanUnknown(r, reqKnown, path + ".requirement");
            if (r.contains("metric")) {
                c.str(r.at("metric"), path + ".requirement", "metric", it.requirement.metric);
            }
            if (r.contains("op")) c.str(r.at("op"), path + ".requirement", "op", it.requirement.op);
            if (r.contains("limit"))
                c.num(r.at("limit"), path + ".requirement", "limit", it.requirement.limit);
            if (it.requirement.op != ">=" && it.requirement.op != "<=") {
                c.err(path + ".requirement", "op", "must be \">=\" or \"<=\"");
                continue;
            }
            it.hasRequirement = true;
        }
        c.cfg.evaluation.items.push_back(it);
    }
    if (!c.cfg.evaluation.items.empty()) {
        bool found = false;
        for (const auto& it : c.cfg.evaluation.items) {
            if (it.key == c.cfg.evaluation.stabilityKey) found = true;
        }
        if (!found) {
            c.err("evaluation", "stabilityKey", "must reference a declared evaluation item key");
        }
    }
    // requirement.metric：评估项 key 或派生输入名
    for (const auto& it : c.cfg.evaluation.items) {
        if (!it.hasRequirement) continue;
        if (it.requirement.metric.empty()) {
            c.err("evaluation.items", "requirement.metric", "must not be empty");
            continue;
        }
        bool ok = isDerivationName(it.requirement.metric, c.cfg);
        for (const auto& other : c.cfg.evaluation.items) {
            if (other.key == it.requirement.metric) ok = true;
        }
        if (!ok) {
            c.err("evaluation.items[" + it.key + "]", "requirement.metric",
                  "must reference an evaluation item key or a derivation input");
        }
    }
}

void parseOptimization(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"objective",   "objectiveItems", "minImprovement",
                                         "maxIterations", "maxMs",         "parameters"};
    const std::set<std::string> pKnown = {"metric", "min", "max", "step"};
    if (!seg.is_object()) {
        c.err("optimization", "optimization", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "optimization");
    if (seg.contains("objective")) {
        const json& o = seg.at("objective");
        if (o.is_string()) {
            c.str(o, "optimization", "objective", c.cfg.optimization.objectiveKind);
        } else if (o.is_object()) {
            c.scanUnknown(o, {"kind", "items"}, "optimization.objective");
            if (o.contains("kind"))
                c.str(o.at("kind"), "optimization.objective", "kind",
                      c.cfg.optimization.objectiveKind);
            if (o.contains("items") && o.at("items").is_array()) {
                for (const auto& k : o.at("items")) {
                    if (k.is_string()) c.cfg.optimization.objectiveItems.push_back(k.get<std::string>());
                }
            }
        } else {
            c.err("optimization", "objective", "must be a string or an object");
        }
    }
    if (c.cfg.optimization.objectiveKind != "evaluationWeighted") {
        c.err("optimization", "objective", "unsupported objective kind");
    }
    if (seg.contains("objectiveItems")) {
        if (!seg.at("objectiveItems").is_array()) {
            c.err("optimization", "objectiveItems", "must be an array");
        } else {
            if (!c.cfg.optimization.objectiveItems.empty()) {
                c.err("optimization", "objectiveItems", "declared twice (objective.items and objectiveItems)");
            } else {
                for (const auto& k : seg.at("objectiveItems")) {
                    if (k.is_string()) c.cfg.optimization.objectiveItems.push_back(k.get<std::string>());
                }
            }
        }
    }
    if (seg.contains("minImprovement") &&
        !c.num(seg.at("minImprovement"), "optimization", "minImprovement",
               c.cfg.optimization.minImprovement))
        return;
    if (c.cfg.optimization.minImprovement < 0.0) {
        c.err("optimization", "minImprovement", "must be >= 0");
        return;
    }
    if (seg.contains("maxIterations")) {
        const auto v = asInt(seg.at("maxIterations"));
        if (!v.has_value() || *v < 1) {
            c.err("optimization", "maxIterations", "must be an integer >= 1");
            return;
        }
        c.cfg.optimization.maxIterations = static_cast<int>(*v);
    }
    if (seg.contains("maxMs")) {
        const auto v = asInt(seg.at("maxMs"));
        if (!v.has_value() || *v < 1) {
            c.err("optimization", "maxMs", "must be an integer >= 1");
            return;
        }
        c.cfg.optimization.maxMs = *v;
    }
    if (!seg.contains("parameters") || !seg.at("parameters").is_array()) {
        c.err("optimization.parameters", "parameters", "must be an array (may be empty)");
        return;
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < seg.at("parameters").size(); ++i) {
        const json& e = seg.at("parameters")[i];
        const std::string path = "optimization.parameters[" + std::to_string(i) + "]";
        if (!e.is_object()) {
            c.err(path, "", "must be an object");
            continue;
        }
        c.scanUnknown(e, pKnown, path);
        OptimizationParamDef p;
        if (!e.contains("metric") || !c.str(e.at("metric"), path, "metric", p.metric)) continue;
        if (findMetric(c.cfg, p.metric) == nullptr) {
            c.err(path, "metric", "must reference a declared metric key");
            continue;
        }
        if (!seen.insert(p.metric).second) {
            c.err(path, "metric", "duplicate parameter");
            continue;
        }
        if (!e.contains("min") || !c.num(e.at("min"), path, "min", p.min)) continue;
        if (!e.contains("max") || !c.num(e.at("max"), path, "max", p.max)) continue;
        if (!e.contains("step") || !c.num(e.at("step"), path, "step", p.step)) continue;
        if (!(p.max > p.min)) {
            c.err(path, "min/max", "must satisfy min < max");
            continue;
        }
        if (!(p.step > 0.0)) {
            c.err(path, "step", "must be > 0 (the engine has no built-in step)");
            continue;
        }
        c.cfg.optimization.parameters.push_back(p);
    }
    for (const auto& k : c.cfg.optimization.objectiveItems) {
        if (findEvaluationItem(c.cfg, k) == nullptr) {
            c.err("optimization", "objectiveItems", "must reference declared evaluation item keys");
            break;
        }
    }
}

void parseCurves(Ctx& c, const json& seg) {
    const std::set<std::string> known = {"granularityMs", "maxPoints", "metrics"};
    if (!seg.is_object()) {
        c.err("curves", "curves", "must be an object");
        return;
    }
    c.scanUnknown(seg, known, "curves");
    if (seg.contains("granularityMs")) {
        const auto v = asInt(seg.at("granularityMs"));
        if (!v.has_value() || *v <= 0) {
            c.err("curves", "granularityMs", "must be an integer > 0");
            return;
        }
        c.cfg.curves.granularityMs = *v;
    }
    if (seg.contains("maxPoints")) {
        const auto v = asInt(seg.at("maxPoints"));
        if (!v.has_value() || *v < 2) {
            c.err("curves", "maxPoints", "must be an integer >= 2");
            return;
        }
        c.cfg.curves.maxPoints = static_cast<int>(*v);
    }
    if (seg.contains("metrics")) {
        if (!seg.at("metrics").is_array()) {
            c.err("curves", "metrics", "must be an array");
            return;
        }
        for (const auto& k : seg.at("metrics")) {
            if (!k.is_string()) {
                c.err("curves.metrics", "metrics", "elements must be strings");
                continue;
            }
            const std::string key = k.get<std::string>();
            if (findMetric(c.cfg, key) == nullptr) {
                c.err("curves.metrics", key, "must reference a declared metric key");
                continue;
            }
            c.cfg.curves.metrics.push_back(key);
        }
    }
}

}  // namespace

LoadResult validatePolicyInto(const json& pkg, PolicyConfig* out, PolicyInfo* info) {
    Ctx c;
    LoadResult res;
    if (!pkg.is_object()) {
        res.code = static_cast<int>(ErrorCode::BadRequest);
        res.message = "policy must be a JSON object";
        if (info) *info = c.info;
        return res;
    }
    c.scanUnknown(pkg, topLevelKeys(), "policy");

    std::string kind;
    if (!pkg.contains("kind") || !pkg.at("kind").is_string()) {
        c.err("policy", "kind", "required (must be \"linkThresholds\")");
    } else {
        kind = pkg.at("kind").get<std::string>();
        if (kind != "linkThresholds") {
            c.err("policy", "kind", "must be \"linkThresholds\"");
        }
    }
    if (!pkg.contains("policiesNamespace") || !pkg.at("policiesNamespace").is_string() ||
        pkg.at("policiesNamespace").get<std::string>().empty()) {
        c.err("policy", "policiesNamespace", "required non-empty string");
    } else {
        c.cfg.policiesNamespace = pkg.at("policiesNamespace").get<std::string>();
    }
    if (!pkg.contains("schemaVersion") || !pkg.at("schemaVersion").is_string()) {
        c.err("policy", "schemaVersion", "required (semantic version MAJOR.MINOR.PATCH)");
    } else {
        const std::string sv = pkg.at("schemaVersion").get<std::string>();
        c.cfg.schemaVersion = sv;
        int major = 0, minor = 0, patch = 0;
        if (!parseSchemaVersion(sv, major, minor, patch)) {
            c.err("policy", "schemaVersion", "must match MAJOR.MINOR.PATCH");
        } else {
            c.versionMajor = major;
            if (major != kSupportedPoliciesMajor) {
                c.versionBad = true;
            }
        }
    }

    if (!pkg.contains("items")) {
        c.err("items", "items", "required (node type declarations)");
    } else {
        parseNodeTypes(c, pkg.at("items"));
    }
    if (pkg.contains("structures")) parseStructures(c, pkg.at("structures"));
    if (!pkg.contains("states")) {
        c.err("states", "states", "required (thresholds MUST come from the policy)");
    } else {
        parseStates(c, pkg.at("states"));
    }
    if (!pkg.contains("hysteresis")) {
        c.err("hysteresis", "hysteresis", "required (no built-in hysteresis in the engine)");
    } else {
        parseHysteresis(c, pkg.at("hysteresis"));
    }
    if (!pkg.contains("metrics")) {
        c.err("metrics", "metrics", "required (metric set MUST come from the policy)");
    } else {
        parseMetrics(c, pkg.at("metrics"));
    }
    if (pkg.contains("coverage")) parseCoverage(c, pkg.at("coverage"));
    if (pkg.contains("meshProgress")) parseMeshProgress(c, pkg.at("meshProgress"));
    if (pkg.contains("aggregation")) parseAggregation(c, pkg.at("aggregation"));
    if (pkg.contains("evaluation")) {
        parseEvaluation(c, pkg.at("evaluation"));
    } else {
        c.err("evaluation", "evaluation", "required (algorithms and weights MUST come from the policy)");
    }
    if (pkg.contains("optimization")) parseOptimization(c, pkg.at("optimization"));
    if (pkg.contains("curves")) parseCurves(c, pkg.at("curves"));

    if (c.cfg.structures.empty() && c.issues.empty()) {
        StructureDef s;
        s.key = "mesh";  // 缺省结构（CTR-PL-04；结构与取值仍可由规则覆盖）
        s.mode = StructureMode::Mesh;
        c.cfg.structures.push_back(s);
        c.warn("structures not declared, fell back to a single flat structure");
    }
    if (c.cfg.curves.metrics.empty()) {
        for (const auto& m : c.cfg.metrics) {
            if (m.inScore()) c.cfg.curves.metrics.push_back(m.key);
        }
    }

    PolicyInfo pi;
    pi.loaded = c.issues.empty() && !c.versionBad;
    pi.policiesNamespace = c.cfg.policiesNamespace;
    pi.schemaVersion = c.cfg.schemaVersion;
    pi.nodeTypeCount = static_cast<int>(c.cfg.nodeTypes.size());
    pi.structureCount = static_cast<int>(c.cfg.structures.size());
    pi.metricCount = static_cast<int>(c.cfg.metrics.size());
    pi.evaluationItemCount = static_cast<int>(c.cfg.evaluation.items.size());
    pi.parameterCount = static_cast<int>(c.cfg.optimization.parameters.size());
    pi.windowMs = c.cfg.window.windowMs;
    pi.warnings = c.info.warnings;
    pi.digest = fnv1a64Hex(canonicalJson(pkg));
    pi.policyVersion = pi.policiesNamespace + ":" + pi.schemaVersion + ":" + pi.digest;

    res.issues = c.issues;
    res.data = pi;
    if (c.versionBad) {
        res.code = static_cast<int>(ErrorCode::VersionMismatch);
        res.message = "unsupported policies MAJOR (engine supports " +
                      std::to_string(kSupportedPoliciesMajor) + ")";
    } else if (!c.issues.empty()) {
        res.code = static_cast<int>(ErrorCode::BadRequest);
        res.message = "policy rejected: " + std::to_string(c.issues.size()) + " issue(s)";
    } else {
        res.code = 0;
        res.message = "ok";
    }
    if (out) *out = c.cfg;
    if (info) *info = pi;
    return res;
}

LoadResult loadPolicyInto(EngineState& st, const json& pkg) {
    PolicyConfig cfg;
    PolicyInfo info;
    LoadResult res = validatePolicyInto(pkg, &cfg, &info);
    if (res.code == 0) {
        st.policy = cfg;  // 原子替换：失败时保留上一次成功装载的规则
        st.policyLoaded = true;
        st.info = info;
        st.metrics.unknownFields += static_cast<int64_t>(info.warnings.size());
    }
    return res;
}

// ---------------------------------------------------------------- 查表

const MetricDef* findMetric(const PolicyConfig& p, const std::string& key) {
    for (const auto& m : p.metrics) {
        if (m.key == key) return &m;
    }
    return nullptr;
}

const NodeTypeDef* findNodeType(const PolicyConfig& p, const std::string& key) {
    for (const auto& t : p.nodeTypes) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

const StructureDef* findStructure(const PolicyConfig& p, const std::string& key) {
    for (const auto& s : p.structures) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

const EvaluationItemDef* findEvaluationItem(const PolicyConfig& p, const std::string& key) {
    for (const auto& i : p.evaluation.items) {
        if (i.key == key) return &i;
    }
    return nullptr;
}

const OptimizationParamDef* findParameter(const PolicyConfig& p, const std::string& metric) {
    for (const auto& i : p.optimization.parameters) {
        if (i.metric == metric) return &i;
    }
    return nullptr;
}

LinkState bandOf(const PolicyConfig& p, double score) {
    for (const auto& b : p.states) {  // 已按 min 降序
        if (score >= b.min) return b.state;
    }
    return p.states.empty() ? LinkState::Red : p.states.back().state;
}

double bandMinOf(const PolicyConfig& p, LinkState s) {
    for (const auto& b : p.states) {
        if (b.state == s) return b.min;
    }
    return 0.0;
}

}  // namespace detail
}  // namespace topology
