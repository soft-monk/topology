// examples/minimal/main.cc —— 最小用法（20 行级）：装载规则 → 建拓扑 → 灌一次观测 → 读状态
//
// 规则包路径由 CMake 以绝对路径注入（不依赖当前工作目录）。
#include <cstdio>
#include <memory>

#include "topology/topology_engine.h"

using namespace topology;

namespace {

/// 宿主侧假时钟（TPL-NFR-03：时间可注入）
struct Clock : IClock {
    int64_t t = 1750000000000LL;
    int64_t nowMs() const override { return t; }
};

/// 宿主侧出口：真实系统里在这里落库 + 广播 `topology.changed`
struct Sink : ITopologySink {
    void onTopologyChanged(const TopologyChangedEvent& e) override {
        std::printf("  [sink] topology.changed %s -> %s (%s)\n", e.from.c_str(), e.to.c_str(),
                    e.state.c_str());
    }
};

}  // namespace

int main() {
    TopologyEngine engine;
    engine.setClock(std::make_shared<Clock>());
    engine.setSink(std::make_shared<Sink>());

    const LoadResult loaded = engine.loadPolicyFile(TOPOLOGY_POLICY_FILE);
    if (loaded.code != 0) {
        std::printf("规则包装载失败：%s\n", loaded.message.c_str());
        for (const auto& i : loaded.issues) {
            std::printf("  - %s.%s: %s\n", i.path.c_str(), i.field.c_str(), i.reason.c_str());
        }
        return 1;
    }

    if (engine.configureTopology("demo", "mesh").code != 0) return 2;
    NodeSpec a;
    a.id = "n-a";
    a.typeKey = "cluster";  // 类型只由规则声明的 key 判定（TPL-MODEL-01）
    a.name = "集群一";
    a.lng = 116.20;
    a.lat = 39.90;
    a.hasPosition = true;
    NodeSpec b = a;
    b.id = "n-b";
    b.name = "集群二";
    b.lng = 116.60;
    b.lat = 39.60;
    if (engine.addNodes({a, b}).code != 0) return 3;
    EdgeSpec e;
    e.id = "e-ab";
    e.from = "n-a";
    e.to = "n-b";
    if (engine.addEdge(e).code != 0) return 4;

    // 一条链路质量观测（驼峰事件字段 → 引擎内归一为台账列名，TPL-Q-02）
    json event = json::object();
    event["linkId"] = "e-ab";
    event["from"] = "n-a";
    event["to"] = "n-b";
    event["signal"] = -52;
    event["bandwidthMbps"] = 140;
    event["latencyMs"] = 35;
    event["lossRate"] = 0.01;
    event["coverageKm2"] = 300;
    const IngestResult ingested = engine.ingest(event);
    if (ingested.code != 0) return 5;

    const auto quality = engine.linkQuality("e-ab");
    std::string line = "链路 " + quality->linkId + "：state=" + toString(quality->state) + " score=";
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", quality->score);
        line += buf;
    }
    line += "（窗口 " + std::to_string(static_cast<long long>(quality->windowMs)) + " ms）";
    line += "\n归一后的台账形状：";
    line += quality->toJson().at("metrics").dump();
    std::fputs(line.c_str(), stdout);
    std::fputs("\n", stdout);

    const auto stability = engine.linkStability();  // 供 `scoring` 只读调用的"链路稳定度"
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", stability.value_or(-1.0));
        std::string s = "链路稳定度：";
        s += buf;
        s += "\n";
        std::fputs(s.c_str(), stdout);
    }

    const json primitives = engine.primitives();  // 可直接喂给 map-2d 的图元
    std::string prim = "图元：links=";
    prim += std::to_string(static_cast<long long>(primitives.at("links").size()));
    prim += " clusters=";
    prim += std::to_string(static_cast<long long>(primitives.at("clusters").size()));
    prim += "\n";
    std::fputs(prim.c_str(), stdout);
    return 0;
}
