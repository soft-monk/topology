// examples/full_flow/main.cc —— 全流程演示：
//   规则装载 → 网状拓扑 → 1 Hz 观测（含阈值附近震荡）→ 迟滞判定与留痕 → 四项评估
//   → 优化建议（不改状态）→ 真实窗口曲线 → 消费侧图元
//
// 全部时间来自注入的假时钟，因此**同一份输入每次跑结果完全一致**。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "topology/topology_engine.h"

using namespace topology;

namespace {

/// 输出一行（避开 printf 格式串里混排中文与多个占位符的解析歧义）
void say(const std::string& s) {
    std::fputs(s.c_str(), stdout);
    std::fputs("\n", stdout);
}

/// 数值转字符串
std::string num(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), (digits == 4) ? "%.4f" : "%.2f", v);
    return std::string(buf);
}

std::string i64(int64_t v) { return std::to_string(static_cast<long long>(v)); }

struct Clock : IClock {
    int64_t t = 1750000000000LL;
    int64_t nowMs() const override { return t; }
    void advance(int64_t ms) { t += ms; }
};

struct Sink : ITopologySink {
    int changes = 0;
    void onTopologyChanged(const TopologyChangedEvent& e) override {
        ++changes;
        std::string line = "    topology.changed edge=" + e.edgeId + " " + e.from + "->" + e.to +
                           " state=" + e.state + " score=" +
                           num(e.metrics.value("score", 0.0), 4) + "（越阈 threshold=" +
                           num(e.metrics.value("threshold", 0.0), 2) + " 指标=" +
                           e.metrics.value("metric", std::string("-")) + "=" +
                           num(e.metrics.value("value", 0.0), 3) + "）";
        say(line);
    }
};

/// 宿主侧持久化替身：真实系统里写库（引擎不接触 SQL，P3）
struct Store : ITopologyStore {
    std::string last;
    int saves = 0;
    bool save(const std::string& topologyId, const json& snapshot) override {
        (void)topologyId;
        last = snapshot.dump();
        ++saves;
        return true;
    }
    bool load(const std::string&, json& out) override {
        if (last.empty()) return false;
        out = json::parse(last);
        return true;
    }
    bool remove(const std::string&) override { return false; }
};

json observation(const std::string& linkId, double signal, double bandwidth, double latency,
                 double loss, double coverage) {
    json e = json::object();
    e["linkId"] = linkId;
    e["signal"] = signal;
    e["bandwidthMbps"] = bandwidth;
    e["latencyMs"] = latency;
    e["lossRate"] = loss;
    e["coverageKm2"] = coverage;
    return e;
}

}  // namespace

int main() {
    auto clock = std::make_shared<Clock>();
    auto sink = std::make_shared<Sink>();
    auto store = std::make_shared<Store>();

    TopologyEngineOptions opts;
    opts.clock = clock;
    opts.sink = sink;
    opts.store = store;
    TopologyEngine engine(opts);

    const LoadResult loaded = engine.loadPolicyFile(TOPOLOGY_POLICY_FILE);
    if (loaded.code != 0) {
        say("规则包装载失败：" + loaded.message);
        return 1;
    }
    say("规则包：" + loaded.data.policyVersion + "（节点类型 " +
        std::to_string(loaded.data.nodeTypeCount) + " 个 / 指标 " +
        std::to_string(loaded.data.metricCount) + " 个 / 窗口 " + i64(loaded.data.windowMs) + " ms）");

    // ---- 网状拓扑：6 集群 + 1 指挥节点，层与结构均由规则声明（TPL-MODEL-02）
    if (engine.configureTopology("demo-mesh", "mesh").code != 0) return 2;
    std::vector<NodeSpec> nodes;
    NodeSpec forward;
    forward.id = "cmd";
    forward.typeKey = "forward";
    forward.name = "指挥节点";
    forward.lng = 116.40;
    forward.lat = 39.75;
    forward.hasPosition = true;
    nodes.push_back(forward);
    for (int i = 0; i < 6; ++i) {
        NodeSpec n;
        n.id = "c" + std::to_string(i);
        n.typeKey = "cluster";  // 类型判定只看规则 key，与 name 无关
        n.name = "集群 " + std::to_string(i + 1);
        n.lng = 116.10 + 0.12 * i;
        n.lat = 39.55 + 0.06 * i;
        n.hasPosition = true;
        nodes.push_back(n);
    }
    if (engine.addNodes(nodes).code != 0) return 3;
    std::vector<EdgeSpec> edges;
    for (int i = 0; i < 6; ++i) {
        EdgeSpec e;
        e.id = "e-c" + std::to_string(i);
        e.from = "c" + std::to_string(i);
        e.to = "cmd";
        edges.push_back(e);
    }
    EdgeSpec dup;  // 无向重复边只出现一次（TPL-MODEL-04）
    dup.id = "e-dup";
    dup.from = "cmd";
    dup.to = "c0";
    edges.push_back(dup);
    const MutationResult added = engine.addEdges(edges);
    say("装载：nodes=" + i64(static_cast<int64_t>(engine.topologyView().nodes.size())) + " edges=" +
        i64(static_cast<int64_t>(engine.topologyView().edges.size())) + "（重复边按幂等忽略 " +
        i64(added.data.value("duplicates", static_cast<int64_t>(0))) + " 条）");

    // ---- 1 Hz 观测：前 3 秒链路质量差、之后恢复，用于演示状态迁移与迟滞
    say("\n[1] 灌入 15 秒观测（e-c3 全程在阈值附近震荡；其余链路前 4 秒质量差）");
    for (int tick = 0; tick < 15; ++tick) {
        for (int i = 0; i < 6; ++i) {
            IngestResult r;
            const bool degraded = (tick < 4);
            if (i == 3) {
                const double wobble = (tick % 2 == 0) ? 0.74 : 0.76;
                r = engine.ingest(observation("e-c3", -120.0 + 120.0 * wobble, 200.0 * wobble,
                                              500.0 * (1.0 - wobble), 1.0 - wobble, 300.0));
            } else if (degraded) {
                r = engine.ingest(observation("e-c" + std::to_string(i), -110.0, 6.0, 470.0, 0.45, 20.0));
            } else {
                r = engine.ingest(observation("e-c" + std::to_string(i), -50.0 - 3.0 * i,
                                              140.0 - 8.0 * i, 30.0 + 4.0 * i, 0.005 * i, 300.0));
            }
            if (r.code != 0) {
                say("观测被拒绝：" + r.message);
                return 4;
            }
        }
        clock->advance(1000);
    }
    say("    出口收到 " + std::to_string(sink->changes) + " 次状态变更；被迟滞/驻留拦下 " +
        i64(engine.metrics().suppressedChanges) + " 次（抑制比可查 metrics）");

    // ---- 状态与留痕（TPL-ST-04：从哪到哪、越阈指标与数值）
    say("\n[2] 边状态与变更留痕");
    for (const auto& e : engine.topologyView().edges) {
        const auto q = engine.linkQuality(e.id);
        say("    " + e.id + " state=" + (e.hasState ? std::string(toString(e.state)) : std::string("-")) +
            " score=" + num(q->score, 4));
    }
    for (const auto& c : engine.stateLog()) {
        say("    留痕 t=" + i64(c.ts) + " " + c.linkId + ": " + toString(c.from) + " -> " +
            toString(c.to) + "（越阈指标 " + c.metric + "=" + num(c.value, 3) + "、得分 " +
            num(c.score, 4) + " vs 阈值 " + num(c.threshold, 2) + "、确认 " +
            std::to_string(c.confirmations) + " 次）");
    }

    // ---- 四项评估（结构化、可复算、无文案）
    say("\n[3] 网络评估（结构化输出，输入显式可复算）");
    PhaseContext ctx;
    ctx.phaseKey = "T2";
    ctx.seq = 2;
    ctx.scenarioKey = "scenario-1";
    ctx.missionId = "m-demo";
    engine.setPhaseContext(ctx);
    const EvaluationResult ev = engine.evaluate(ctx);
    for (const auto& item : ev.items) {
        std::string line = "    " + item.key + " = " + num(item.value, 4) + "（权重 " +
                           num(item.weight, 2) + "）";
        if (item.hasRequirement) {
            line += "  阈值 " + item.requirementMetric + " " + item.requirementOp + " " +
                    num(item.requirementLimit, 2) + " → " + (item.satisfied ? "满足" : "不满足");
        }
        say(line);
    }
    say("    overall=" + num(ev.overall, 4) + "；链路稳定度（供 scoring 只读）=" +
        num(engine.linkStability().value_or(-1.0), 4));

    // ---- 优化建议：只出建议值，不改状态（TPL-OPT-02）
    say("\n[4] 优化建议（不改状态、可解释、单调、幂等）");
    const OptimizationResult opt = engine.optimize(ctx, "e-c5");
    if (opt.code != 0) {
        say("    未产出建议：" + opt.message + "（" + opt.reason + "）");
    } else if (!opt.improved) {
        say("    无改进（reason=" + opt.reason + "）");
    } else {
        const auto pol = engine.policy();
        for (const auto& s : opt.suggestions) {
            std::string step = "0";
            if (pol.has_value()) {
                for (const auto& param : pol->optimization.parameters) {
                    if (param.metric == s.metric) step = num(param.step, 2);
                }
            }
            say("    " + s.metric + " " + num(s.current, 2) + " → " + num(s.suggested, 2) + "（Δ" +
                num(s.delta, 2) + "，" + std::to_string(s.steps) + " 步 × 每步 " + step + "）");
        }
        say("    评估 " + num(opt.before.overall, 4) + " → " + num(opt.after.overall, 4) + "（单调不减）");
    }
    const std::string first = opt.toJson().dump();
    say("    幂等：再跑一次结果一致 = " +
        std::string(engine.optimize(ctx, "e-c5").toJson().dump() == first ? "是" : "否"));

    // ---- 曲线：来自真实滑动窗口聚合
    say("\n[5] 曲线（真实窗口聚合，非公式）");
    const CurveSeries series = engine.curves("e-c0");
    say("    粒度 " + i64(series.granularityMs) + " ms / 窗口 " + i64(series.windowMs) + " ms，共 " +
        i64(static_cast<int64_t>(series.points.size())) + " 个点");
    for (std::size_t i = 0; i < series.points.size() && i < 3; ++i) {
        say("      t=" + i64(series.points[i].ts) + " samples=" +
            std::to_string(series.points[i].samples) + " values=" + series.points[i].values.dump());
    }

    // ---- 消费侧图元（map-2d §3.3）与审计快照（TPL-MODEL-06）
    const json primitives = engine.primitives();
    say("\n[6] 图元适配：links=" +
        i64(static_cast<int64_t>(primitives.at("links").size())) + " clusters=" +
        i64(static_cast<int64_t>(primitives.at("clusters").size())) + " missingCoordinates=" +
        i64(static_cast<int64_t>(primitives.at("missingCoordinates").size())));
    say("    快照落库次数=" + std::to_string(store->saves));
    const json snapshot = engine.exportSnapshot();
    TopologyEngine replay(opts);
    if (replay.loadPolicyFile(TOPOLOGY_POLICY_FILE).code != 0) return 5;
    if (replay.importSnapshot(snapshot).code != 0) return 6;
    if (replay.exportSnapshot().dump() != snapshot.dump()) {
        say("快照往返不一致");
        return 7;
    }
    say("    拓扑快照往返一致 ✔");
    return 0;
}
