// topology · src/util.cc —— 内部基础工具（数值、字符串、摘要、派生输入名）
#include "internal.h"

#include <cstdio>
#include <sstream>

namespace topology {
namespace detail {

// ---------------------------------------------------------------- 时间

int64_t nowFrom(IClock* clock) {
    if (clock) return clock->nowMs();
    return 0;
}

IClock* clockOf(const TopologyEngineOptions& deps, std::shared_ptr<IClock>& fallback) {
    if (deps.clock) return deps.clock.get();
    if (!fallback) fallback = std::make_shared<SystemClock>();
    return fallback.get();
}

int64_t nowOf(EngineState& st) {
    IClock* c = clockOf(st.deps, st.systemClock);
    const int64_t now = c->nowMs();
    if (st.hasLastNow && now < st.lastNow) ++st.metrics.clockRegressions;  // 回拨如实使用
    st.lastNow = now;
    st.hasLastNow = true;
    return now;
}

// ---------------------------------------------------------------- 字符串

std::string toSnakeCase(const std::string& camel) {
    std::string out;
    out.reserve(camel.size() + 4);
    for (std::size_t i = 0; i < camel.size(); ++i) {
        const char c = camel[i];
        if (c >= 'A' && c <= 'Z') {
            if (i != 0 && camel[i - 1] != '_') out.push_back('_');
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string fnv1a64Hex(const std::string& bytes) {
    uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (unsigned char c : bytes) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;  // FNV prime
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

namespace {
void canonicalDump(const json& v, std::string& out) {
    switch (v.type()) {
        case json::value_t::object: {
            std::vector<std::string> keys;
            keys.reserve(v.size());
            for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
            std::sort(keys.begin(), keys.end());
            out.push_back('{');
            bool first = true;
            for (const auto& k : keys) {
                if (!first) out.push_back(',');
                first = false;
                out += json(k).dump();
                out.push_back(':');
                canonicalDump(v.at(k), out);
            }
            out.push_back('}');
            break;
        }
        case json::value_t::array: {
            out.push_back('[');
            bool first = true;
            for (const auto& e : v) {
                if (!first) out.push_back(',');
                first = false;
                canonicalDump(e, out);
            }
            out.push_back(']');
            break;
        }
        default:
            out += v.dump();
            break;
    }
}
}  // namespace

std::string canonicalJson(const json& v) {
    std::string out;
    canonicalDump(v, out);
    return out;
}

// ---------------------------------------------------------------- 数值

bool isInteger(const json& v) { return v.is_number_integer() || v.is_number_unsigned(); }

std::optional<int64_t> asInt(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_unsigned()) return static_cast<int64_t>(v.get<uint64_t>());
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return static_cast<int64_t>(d);
    }
    return std::nullopt;
}

bool numericValue(const json& v, const MetricDef& def, double& out) {
    if (v.is_number()) {
        out = v.get<double>();
        return true;
    }
    if (v.is_boolean()) {
        out = v.get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        const auto it = def.valueMap.find(s);
        if (it != def.valueMap.end()) {
            out = it->second;
            return true;
        }
        // 规则声明了 valueMap 但未命中，或未声明 valueMap：MUST NOT 猜、MUST NOT 用 0 覆盖
        return false;
    }
    return false;
}

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

double clampRange(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

double normalizeMetric(const MetricDef& def, double raw) {
    const double span = def.max - def.min;
    if (!(span > 0.0)) return 0.0;  // 装载校验保证 min < max；此处兜底不抛异常
    const double x = (def.direction == MetricDirection::Higher) ? (raw - def.min) : (def.max - raw);
    return clamp01(x / span);
}

// ---------------------------------------------------------------- 派生输入（闭集）

const std::vector<std::string>& fixedDerivations() {
    static const std::vector<std::string> kNames = {
        "stateScoreMean", "worstScore",    "scoreSpread",      "greenRatio",
        "yellowRatio",    "redRatio",      "linkUpRatio",      "stateStability",
        "nodeCoveredRatio", "coverageRatio", "meshProgress",   "linkCount",
        "nodeCount",      "stateChangeCount", "observationCount", "windowSamples",
    };
    return kNames;
}

bool isDerivationName(const std::string& source, const PolicyConfig& policy) {
    for (const auto& n : fixedDerivations()) {
        if (n == source) return true;
    }
    static const std::string kMetricPrefix = "metric.";
    static const std::string kScorePrefix = "metricScore.";
    if (source.compare(0, kScorePrefix.size(), kScorePrefix) == 0) {
        const std::string key = source.substr(kScorePrefix.size());
        const MetricDef* m = findMetric(policy, key);
        return m != nullptr && m->inScore();
    }
    if (source.compare(0, kMetricPrefix.size(), kMetricPrefix) == 0) {
        const std::string key = source.substr(kMetricPrefix.size());
        return findMetric(policy, key) != nullptr;
    }
    return false;
}

std::vector<std::string> derivationNames(const PolicyConfig& policy) {
    std::vector<std::string> names = fixedDerivations();
    for (const auto& m : policy.metrics) names.push_back("metric." + m.key);
    for (const auto& m : policy.metrics) {
        if (m.inScore()) names.push_back("metricScore." + m.key);
    }
    return names;
}

}  // namespace detail
}  // namespace topology
