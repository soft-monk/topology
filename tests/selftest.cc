// tests/selftest.cc · 零依赖自测（手写断言，不引任何测试框架）
//
// 覆盖两套口径：
//   ① 需求专篇 docs/需求/topology需求专篇.md 的 34 条
//      （TPL-MODEL 6 / TPL-Q 6 / TPL-ST 6 / TPL-EVAL 4 / TPL-OPT 6 / TPL-NFR 6）
//   ② 需求专篇 §7 验收清单 16 行
//
// 全部用例**确定性**：时钟一律注入假时钟（TPL-NFR-03），不依赖真实时间、
// 不依赖当前工作目录（夹具/规则包路径由 CMake 注入的绝对路径给出）、不需要外部服务。
//
// 运行： selftest                → 跑全部（人读输出）
//        selftest --list         → 只列用例名
//        selftest --json         → 机检输出（acceptance.ps1 读它做需求↔用例对账）
//        selftest <名字片段>      → 只跑名字里含该片段的用例
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "topology/topology_engine.h"

using namespace topology;

namespace {

// ---------------------------------------------------------------- 断言框架
int g_asserts = 0;
int g_failed = 0;
int g_cases = 0;
int g_casesFailed = 0;
std::string g_case;
std::vector<std::string> g_failures;

struct CaseMeta {
    std::string name;
    std::vector<std::string> reqs;  // 覆盖的 TPL-* 需求编号
    int asserts = 0;
    int failed = 0;
    bool ok = false;
};
std::vector<CaseMeta> g_metas;
std::vector<std::string> g_reqs;
bool g_jsonMode = false;

/// 用例内声明"本用例覆盖哪些需求条目"（多条并列书写）
template <typename... Args>
void requires_(Args... ids) {
    for (const char* id : {ids...}) g_reqs.push_back(id);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string jsonArray(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) out += ",";
        out += "\"" + jsonEscape(v[i]) + "\"";
    }
    out += "]";
    return out;
}

/// 让 CHECK_EQ 能比较不同整型宽度（int / int64_t / size_t），并把 enum class 归一到整数
/// 打印出可读的期望/实际值（enum class 没有 operator<<）
template <typename T, typename = void>
struct Comparable {
    using type = T;
};
template <typename T>
struct Comparable<T, typename std::enable_if<std::is_integral<T>::value>::type> {
    using type = long long;
};
template <typename T>
struct Comparable<T, typename std::enable_if<std::is_enum<T>::value>::type> {
    using type = long long;
};

template <typename T>
typename Comparable<T>::type asComparable(const T& v) {
    return static_cast<typename Comparable<T>::type>(v);
}

void record(bool ok, const std::string& what, const char* file, int line) {
    ++g_asserts;
    if (ok) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "  (" << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

template <typename A, typename B>
void recordEq(const A& got, const B& want, const std::string& what, const char* file, int line) {
    ++g_asserts;
    const auto a = asComparable(got);
    const auto b = asComparable(want);
    if (a == b) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "  期望=" << b << " 实际=" << a << "  (" << file << ":"
        << line << ")";
    g_failures.push_back(oss.str());
}

void recordNear(double got, double want, double eps, const std::string& what, const char* file, int line) {
    ++g_asserts;
    if (std::fabs(got - want) <= eps) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "  期望=" << want << " 实际=" << got << " ±" << eps << "  ("
        << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

#define CHECK(cond) record((cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) record((cond), (msg), __FILE__, __LINE__)
#define CHECK_EQ(a, b) recordEq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) recordNear((a), (b), (eps), #a " ~= " #b, __FILE__, __LINE__)

// ---------------------------------------------------------------- 路径与文件

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

std::string fixturePath(const std::string& name) { return joinPath(TOPOLOGY_TEST_FIXTURES, name); }
std::string policyPath() { return TOPOLOGY_POLICY_FILE; }

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        record(false, "无法读取文件：" + path, __FILE__, __LINE__);
        return std::string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

json readJson(const std::string& path) {
    const std::string text = readText(path);
    if (text.empty()) return json::object();
    return json::parse(text);
}

std::string dump(const json& j) { return j.dump(); }

// ---------------------------------------------------------------- 宿主替身

/// 假时钟：时间完全可控，使迟滞与窗口测试可复现（TPL-NFR-03）
struct FakeClock : IClock {
    int64_t t = 1750000000000LL;
    int64_t nowMs() const override { return t; }
    void advance(int64_t ms) { t += ms; }
};

/// 记录型出口：验证 payload 形状、异常隔离与计数（TPL-NFR-02）
struct RecordingSink : ITopologySink {
    std::vector<TopologyChangedEvent> events;
    std::vector<StateChange> changes;
    int calls = 0;
    bool throwing = false;
    void onTopologyChanged(const TopologyChangedEvent& e) override {
        ++calls;
        if (throwing) throw std::runtime_error("sink exploded");
        events.push_back(e);
    }
    void onStateChanged(const StateChange& e) override {
        if (throwing) throw std::runtime_error("sink exploded");
        changes.push_back(e);
    }
};

/// 内存 store：验证"持久化走注入的反向接口"（TPL-NFR-01/02）
struct MemoryStore : ITopologyStore {
    std::map<std::string, json> data;
    int saves = 0;
    bool fail = false;
    bool save(const std::string& topologyId, const json& snapshot) override {
        ++saves;
        if (fail) return false;
        data[topologyId] = snapshot;
        return true;
    }
    bool load(const std::string& topologyId, json& out) override {
        const auto it = data.find(topologyId);
        if (it == data.end()) return false;
        out = it->second;
        return true;
    }
    bool remove(const std::string& topologyId) override { return data.erase(topologyId) > 0; }
};

/// 坐标来源替身（TPL-MODEL-03）
struct MapLayout : ILayoutProvider {
    std::map<std::string, LayoutPoint> points;
    int misses = 0;
    bool resolve(const LayoutRequest& req, LayoutPoint& out) override {
        const auto it = points.find(req.nodeId);
        if (it == points.end()) {
            ++misses;
            return false;
        }
        out = it->second;
        return true;
    }
};

/// 留痕出口替身
struct LogSink : ILogSink {
    std::vector<AuditEntry> audits;
    void commandAudit(const AuditEntry& e) override { audits.push_back(e); }
};

// ---------------------------------------------------------------- 夹具装配

struct Rig {
    std::shared_ptr<FakeClock> clock;
    std::shared_ptr<RecordingSink> sink;
    std::shared_ptr<MemoryStore> store;
    std::shared_ptr<MapLayout> layout;
    std::unique_ptr<TopologyEngine> engine;
};

Rig makeRig(const json& policy, bool withStore, bool withLayout, int64_t startMs = 1750000000000LL) {
    Rig rig;
    rig.clock = std::make_shared<FakeClock>();
    rig.clock->t = startMs;
    rig.sink = std::make_shared<RecordingSink>();
    rig.store = std::make_shared<MemoryStore>();
    rig.layout = std::make_shared<MapLayout>();
    TopologyEngineOptions opts;
    opts.clock = rig.clock;
    opts.sink = rig.sink;
    if (withStore) opts.store = rig.store;
    if (withLayout) opts.layout = rig.layout;
    rig.engine.reset(new TopologyEngine(opts));
    const LoadResult lr = rig.engine->loadPolicy(policy);
    CHECK_EQ(lr.code, 0);
    if (lr.code != 0) {
        for (const auto& i : lr.issues) std::fprintf(stderr, "  policy issue %s.%s: %s\n", i.path.c_str(), i.field.c_str(), i.reason.c_str());
    }
    return rig;
}

Rig makeRigFromFixture(const std::string& fixture, bool withStore = false, bool withLayout = false,
                       int64_t startMs = 1750000000000LL) {
    return makeRig(readJson(fixturePath(fixture)), withStore, withLayout, startMs);
}

/// 规则可改：把夹具规则打补丁后再装载（证明"换规则即换行为"）
json patched(const std::string& fixture, const std::function<void(json&)>& fn) {
    json p = readJson(fixturePath(fixture));
    fn(p);
    return p;
}

NodeSpec nodeSpec(const std::string& id, const std::string& typeKey, const std::string& name,
                  bool withPosition = true, double lng = 116.0, double lat = 39.0) {
    NodeSpec n;
    n.id = id;
    n.typeKey = typeKey;
    n.name = name;
    n.lng = lng;
    n.lat = lat;
    n.hasPosition = withPosition;
    return n;
}

EdgeSpec edgeSpec(const std::string& id, const std::string& from, const std::string& to) {
    EdgeSpec e;
    e.id = id;
    e.from = from;
    e.to = to;
    return e;
}

/// fsm-* 夹具：两个节点 + 一条边（指标只有 level）
void basicTopology(Rig& rig) {
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n1", "a", "节点一")).code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n2", "b", "节点二")).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
}

json levelEvent(const std::string& linkId, double level) {
    json e = json::object();
    e["linkId"] = linkId;
    e["level"] = level;
    return e;
}

/// 单指标策略下"灌一个采样值并推进窗口"：推进 > 窗口长度，使窗口内只剩本次采样，
/// 于是得分就是本次采样值（手算可核对）。
void feed(Rig& rig, const std::string& linkId, double level, int64_t stepMs = 1100) {
    const IngestResult r = rig.engine->ingest(levelEvent(linkId, level));
    CHECK_EQ(r.code, 0);
    rig.clock->advance(stepMs);
}

/// 原始档（无迟滞）序列的"越阈次数"
int bandFlips(const PolicyConfig& p, const std::vector<double>& seq) {
    int flips = 0;
    for (std::size_t i = 1; i < seq.size(); ++i) {
        const LinkState a = [&] {
            for (const auto& b : p.states) {
                if (seq[i - 1] >= b.min) return b.state;
            }
            return p.states.back().state;
        }();
        const LinkState b = [&] {
            for (const auto& s : p.states) {
                if (seq[i] >= s.min) return s.state;
            }
            return p.states.back().state;
        }();
        if (a != b) ++flips;
    }
    return flips;
}

// ================================================================ TPL-MODEL

void model01_node_type_comes_from_declared_key_not_name() {
    requires_("TPL-MODEL-01");
    Rig rig = makeRigFromFixture("two-structures.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    // 名字带业务词，但类型由 typeKey 决定
    const MutationResult r1 = rig.engine->addNode(nodeSpec("n1", "tierMid", "云端算力中心"));
    const MutationResult r2 = rig.engine->addNode(nodeSpec("n2", "tierTop", "边缘节点"));
    CHECK_EQ(r1.code, 0);
    CHECK_EQ(r2.code, 0);
    const auto a = rig.engine->node("n1");
    const auto b = rig.engine->node("n2");
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK_EQ(a->typeKey, std::string("tierMid"));
    CHECK_EQ(b->typeKey, std::string("tierTop"));
    CHECK_EQ(a->tier, 1);
    CHECK_EQ(b->tier, 0);
    // 类型名来自规则声明（display name），引擎不认识它
    CHECK_EQ(a->typeName, std::string("边缘节点"));

    // 改名不改类型：同名不同 typeKey → 类型随 key 变
    const MutationResult r3 = rig.engine->addNode(nodeSpec("n3", "tierTop", "云端算力中心"));
    CHECK_EQ(r3.code, 0);
    const auto c = rig.engine->node("n3");
    CHECK(c.has_value());
    CHECK_EQ(c->typeKey, std::string("tierTop"));
    CHECK_EQ(c->typeKey == a->typeKey, false);

    // 未声明的类型 → 拒绝 + 列出已声明类型（可读原因），且不靠名字兜底
    const MutationResult bad = rig.engine->addNode(nodeSpec("n4", "notDeclared", "边缘节点"));
    CHECK_EQ(bad.code, static_cast<int>(ErrorCode::BadRequest));
    CHECK(bad.data.contains("declaredTypes"));
    CHECK_EQ(bad.data.at("declaredTypes").size(), static_cast<std::size_t>(4));
}

void model02_same_data_under_two_structures() {
    requires_("TPL-MODEL-02");
    const json pkg = readJson(fixturePath("two-structures.json"));
    // 同一份数据：网状
    {
        Rig rig = makeRig(pkg, false, false);
        CHECK_EQ(rig.engine->configureTopology("t-mesh", "flat").code, 0);
        CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "tierTop", "上"), nodeSpec("n2", "tierMid", "中"),
                                       nodeSpec("n3", "tierEnd", "下")})
                     .code,
                 0);
        // mesh 结构 adjacentOnly=false：跨层连接允许
        CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n3")).code, 0);
        CHECK_EQ(rig.engine->topologyView().edges.size(), static_cast<std::size_t>(1));
        CHECK(rig.engine->validate().ok);
    }
    // 同一份数据：三层（层数由规则声明）
    {
        Rig rig = makeRig(pkg, false, false);
        CHECK_EQ(rig.engine->configureTopology("t-layered", "three").code, 0);
        CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "tierTop", "上"), nodeSpec("n2", "tierMid", "中"),
                                       nodeSpec("n3", "tierEnd", "下")})
                     .code,
                 0);
        CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
        CHECK_EQ(rig.engine->addEdge(edgeSpec("e2", "n2", "n3")).code, 0);
        // 越级连接被检出并给可读原因（n1 tier0 <-> n3 tier2）
        const MutationResult skip = rig.engine->addEdge(edgeSpec("e3", "n1", "n3"));
        CHECK_EQ(skip.code, static_cast<int>(ErrorCode::BadRequest));
        CHECK(skip.message.find("tier skip") != std::string::npos);
        CHECK_EQ(skip.data.at("fromTier").get<int>(), 0);
        CHECK_EQ(skip.data.at("toTier").get<int>(), 2);
        CHECK_EQ(rig.engine->topologyView().edges.size(), static_cast<std::size_t>(2));
        CHECK_EQ(rig.engine->capabilities().structures, 2);
    }
}

void model03_coordinates_are_injectable() {
    requires_("TPL-MODEL-03");
    // ① 未注入布局：节点自带坐标优先，未给坐标 → hasPosition=false 并进入校验提示
    {
        Rig rig = makeRigFromFixture("fsm-basic.json");
        CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
        CHECK_EQ(rig.engine->addNode(nodeSpec("n1", "a", "一", true, 1.5, 2.5)).code, 0);
        CHECK_EQ(rig.engine->addNode(nodeSpec("n2", "b", "二", false)).code, 0);
        const auto n1 = rig.engine->node("n1");
        CHECK(n1->hasPosition);
        CHECK_NEAR(n1->lng, 1.5, 1e-12);
        const auto n2 = rig.engine->node("n2");
        CHECK_EQ(n2->hasPosition, false);
        const ValidationReport rep = rig.engine->validate();
        bool missing = false;
        for (const auto& i : rep.issues) {
            if (i.kind == "missing-position" && i.entity == "n2") missing = true;
        }
        CHECK(missing);
    }
    // ② 注入坐标来源：同一份数据换坐标源即换布局
    {
        Rig rig = makeRigFromFixture("fsm-basic.json", false, true);
        rig.layout->points["n1"] = LayoutPoint{10.0, 20.0};
        rig.layout->points["n2"] = LayoutPoint{30.0, 40.0};
        CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
        CHECK_EQ(rig.engine->addNode(nodeSpec("n1", "a", "一", false)).code, 0);
        CHECK_EQ(rig.engine->addNode(nodeSpec("n2", "b", "二", false)).code, 0);
        CHECK_NEAR(rig.engine->node("n1")->lng, 10.0, 1e-12);
        CHECK_NEAR(rig.engine->node("n2")->lat, 40.0, 1e-12);
        CHECK_EQ(rig.engine->topologyView().nodes.size(), static_cast<std::size_t>(2));
    }
    // ③ 坐标源解析不到 → hasPosition=false（保持未定，不编造坐标）
    {
        Rig rig = makeRigFromFixture("fsm-basic.json", false, true);
        CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
        CHECK_EQ(rig.engine->addNode(nodeSpec("n9", "a", "九", false)).code, 0);
        CHECK_EQ(rig.engine->node("n9")->hasPosition, false);
        CHECK_EQ(rig.engine->capabilities().layoutInjected, true);
    }
}

void model04_dedup_and_self_loop() {
    requires_("TPL-MODEL-04");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    // 无向重复边：反向再来一次只算一条（幂等成功 code=0 + idempotent）
    const MutationResult dup = rig.engine->addEdge(edgeSpec("e2", "n2", "n1"));
    CHECK_EQ(dup.code, 0);
    CHECK_EQ(dup.data.at("duplicate").get<bool>(), true);
    CHECK_EQ(dup.data.at("idempotent").get<bool>(), true);
    CHECK_EQ(dup.data.at("edgeId"), std::string("e1"));
    CHECK_EQ(rig.engine->topologyView().edges.size(), static_cast<std::size_t>(1));
    CHECK(rig.engine->metrics().duplicateEdges >= 1);
    // 自环 MUST 被拒绝
    const MutationResult self = rig.engine->addEdge(edgeSpec("e3", "n1", "n1"));
    CHECK_EQ(self.code, static_cast<int>(ErrorCode::BadRequest));
    CHECK(self.message.find("self loop") != std::string::npos);
    CHECK_EQ(rig.engine->topologyView().edges.size(), static_cast<std::size_t>(1));
    // 重复的边 id（端点不同）也被拒绝
    CHECK_EQ(rig.engine->addNode(nodeSpec("n3", "b", "三")).code, 0);
    const MutationResult dupId = rig.engine->addEdge(edgeSpec("e1", "n1", "n3"));
    CHECK_EQ(dupId.code, static_cast<int>(ErrorCode::BadRequest));
    // 移除路径：删边后节点才可删；仍带边的节点 → 冲突拒绝（1002）
    CHECK_EQ(rig.engine->removeEdge("e1").code, 0);
    CHECK_EQ(rig.engine->topologyView().edges.size(), static_cast<std::size_t>(0));
    CHECK_EQ(rig.engine->removeNode("n1").code, 0);
    CHECK_EQ(rig.engine->topologyView().nodes.size(), static_cast<std::size_t>(2));
    CHECK_EQ(rig.engine->removeNode("n1").code, static_cast<int>(ErrorCode::NotFound));
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e9", "n2", "n3")).code, 0);
    const MutationResult rm = rig.engine->removeNode("n2");
    CHECK_EQ(rm.code, static_cast<int>(ErrorCode::Conflict));
    CHECK_EQ(rm.data.at("edges").size(), static_cast<std::size_t>(1));
}

void model05_validation_locates_defects() {
    requires_("TPL-MODEL-05");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    // 悬挂边在声明期即被拒绝并定位（端点 + 路径）
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n1", "a", "一")).code, 0);
    const MutationResult dang = rig.engine->addEdge(edgeSpec("e-x", "n1", "nX"));
    CHECK_EQ(dang.code, static_cast<int>(ErrorCode::BadRequest));
    CHECK(dang.message.find("dangling edge") != std::string::npos);
    CHECK_EQ(dang.data.at("path"), std::string("edge.to"));
    CHECK_EQ(dang.data.at("missingNode"), std::string("nX"));
    // 孤立节点：无任何边
    CHECK_EQ(rig.engine->addNode(nodeSpec("n2", "b", "二")).code, 0);
    // 容错导入（历史/外部数据）→ 拓扑可含缺陷，由 validate() 检出并定位
    Rig rig2 = makeRigFromFixture("fsm-basic.json");
    const json snap = readJson(fixturePath("snapshot-dangling.json"));
    const MutationResult imp = rig2.engine->importSnapshot(snap, false);
    CHECK_EQ(imp.code, 0);
    CHECK(imp.data.at("issues").size() >= 2);
    CHECK_EQ(rig2.engine->topologyView().edges.size(), static_cast<std::size_t>(3));
    const ValidationReport rep = rig2.engine->validate();
    bool dangling = false, selfLoop = false, isolated = false, missingPos = false;
    for (const auto& i : rep.issues) {
        if (i.kind == "dangling-edge" && i.entity == "e-dangling") dangling = true;
        if (i.kind == "self-loop" && i.entity == "e-self") selfLoop = true;
        if (i.kind == "isolated-node") isolated = true;
        if (i.kind == "missing-position") missingPos = true;
    }
    CHECK_MSG(dangling, "悬挂边 MUST 被检出并定位");
    CHECK_MSG(selfLoop, "自环 MUST 被检出");
    CHECK_MSG(missingPos, "缺坐标 MUST 被提示");
    CHECK_EQ(rep.ok, false);  // 含致命问题
    const ValidationReport rep1 = rig.engine->validate();
    for (const auto& i : rep1.issues) {
        if (i.kind == "isolated-node" && i.entity == "n2") isolated = true;
    }
    CHECK_MSG(isolated, "孤立节点 MUST 被检出");
    (void)rig;
}

void model06_snapshot_roundtrip() {
    requires_("TPL-MODEL-06");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    feed(rig, "e1", 0.9);
    const json snap = rig.engine->exportSnapshot();
    CHECK_EQ(snap.at("topologyId"), std::string("t-1"));
    CHECK_EQ(snap.at("edges").at(0).at("state"), std::string("green"));
    // 导入到另一个引擎 → 拓扑与状态一致（回放复盘）
    Rig rig2 = makeRigFromFixture("fsm-basic.json");
    const MutationResult imp = rig2.engine->importSnapshot(snap);
    CHECK_EQ(imp.code, 0);
    CHECK_EQ(dump(rig2.engine->exportSnapshot()), dump(snap));
    CHECK(rig2.engine->validate().ok);
    // 非法快照（严格模式）整包拒绝，不留半装载状态
    const json bad = readJson(fixturePath("snapshot-dangling.json"));
    Rig rig3 = makeRigFromFixture("fsm-basic.json");
    CHECK_EQ(rig3.engine->importSnapshot(bad, true).code, static_cast<int>(ErrorCode::BadRequest));
    CHECK_EQ(rig3.engine->topologyView().nodes.size(), static_cast<std::size_t>(0));
}

// ================================================================ TPL-Q

void q01_metric_set_is_rule_declared() {
    requires_("TPL-Q-01");
    Rig rig = makeRig(readJson(policyPath()), false, false);
    const Capabilities cap = rig.engine->capabilities();
    CHECK_EQ(cap.metrics, 9);
    CHECK_EQ(cap.policyLoaded, true);
    CHECK_EQ(cap.policiesMajor, 1);
    const auto pol = rig.engine->policy();
    CHECK(pol.has_value());
    // 单位 / 方向 / 有效范围全部来自规则
    CHECK_EQ(pol->metrics[0].key, std::string("signal"));
    CHECK_EQ(pol->metrics[0].unit, std::string("dBm"));
    CHECK(pol->metrics[0].direction == MetricDirection::Higher);
    CHECK_NEAR(pol->metrics[0].min, -120.0, 1e-12);
    bool foundLatency = false;
    for (const auto& m : pol->metrics) {
        if (m.key == "latencyMs") {
            foundLatency = true;
            CHECK(m.direction == MetricDirection::Lower);
            CHECK_NEAR(m.max, 500.0, 1e-12);
            CHECK_EQ(m.ledgerKey, std::string("latency_ms"));
        }
    }
    CHECK(foundLatency);
    // 换一份指标集（只改 key/范围）→ 聚合维度随之改变
    const json other = patched("fsm-basic.json", [](json& p) {
        p["metrics"]["items"][0]["key"] = "qualityIndex";
        p["metrics"]["items"][0]["max"] = 100;
        p["coverage"] = json::object();
        p["coverage"]["source"] = "qualityIndex";
        p["coverage"]["targetAreaKm2"] = 100;
    });
    Rig rig2 = makeRig(other, false, false);
    CHECK_EQ(rig2.engine->capabilities().metrics, 1);
    CHECK_EQ(rig2.engine->configureTopology("t", "flat").code, 0);
    CHECK_EQ(rig2.engine->addNode(nodeSpec("n1", "a", "一")).code, 0);
    CHECK_EQ(rig2.engine->addNode(nodeSpec("n2", "b", "二")).code, 0);
    CHECK_EQ(rig2.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["qualityIndex"] = 50;
    CHECK_EQ(rig2.engine->ingest(e).code, 0);
    const auto q = rig2.engine->linkQuality("e1");
    CHECK_EQ(q->metrics.at(0).key, std::string("qualityIndex"));
    // 批量入口：逐条结果 + 计数（部分失败也 MUST 逐条标注，不整体失败）
    const MutationResult batch = rig2.engine->ingestBatch({e, e});
    CHECK_EQ(batch.code, 0);
    CHECK_EQ(batch.data.at("accepted").get<int>(), 2);
    CHECK_EQ(batch.data.at("rejected").get<int>(), 0);
    json badBatch = json::object();
    badBatch["linkId"] = "nope";
    const MutationResult batch2 = rig2.engine->ingestBatch({e, badBatch});
    CHECK_EQ(batch2.code, 0);
    CHECK_EQ(batch2.data.at("accepted").get<int>(), 1);
    CHECK_EQ(batch2.data.at("rejected").get<int>(), 1);
}

void q02_normalization_happens_inside_engine() {
    requires_("TPL-Q-02");
    Rig rig = makeRigFromFixture("camel.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n1", "a", "一")).code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n2", "b", "二")).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    // 事件是驼峰（契约 §4 形状），台账是下划线 —— 映射由引擎负责
    json event = json::object();
    event["linkId"] = "e1";
    event["from"] = "n1";
    event["to"] = "n2";
    event["signal"] = "strong";
    event["bandwidthMbps"] = 120.0;
    event["latencyMs"] = 35.0;
    event["lossRate"] = 0.02;
    event["state"] = "green";
    const IngestResult r = rig.engine->ingest(event);
    CHECK_EQ(r.code, 0);
    CHECK_NEAR(r.data.metrics.at("bandwidthMbps").get<double>(), 120.0, 1e-12);
    CHECK_NEAR(r.data.ledger.at("bandwidth_mbps").get<double>(), 120.0, 1e-12);
    CHECK_NEAR(r.data.ledger.at("latency_ms").get<double>(), 35.0, 1e-12);
    CHECK_NEAR(r.data.ledger.at("loss_rate").get<double>(), 0.02, 1e-12);
    // 文本取值经规则声明的 valueMap 数值化
    CHECK_NEAR(r.data.ledger.at("signal").get<double>(), -50.0, 1e-12);
    // 单一形状：归一入口是纯函数（不改状态、不改指标）
    const json shape = rig.engine->normalizedShape(event);
    CHECK_NEAR(shape.at("ledger").at("bandwidth_mbps").get<double>(), 120.0, 1e-12);
    CHECK_EQ(shape.at("preserved").size(), static_cast<std::size_t>(1));  // coverageKm2 未携带
    const auto before = rig.engine->linkQuality("e1");
    rig.engine->normalizedShape(event);
    const auto after = rig.engine->linkQuality("e1");
    CHECK_EQ(dump(before->toJson()), dump(after->toJson()));
    // 事件自称的状态只回显、不采信（判定归引擎）
    CHECK_EQ(r.data.reportedState, std::string("green"));
    CHECK_EQ(shape.at("ignored").size(), static_cast<std::size_t>(0));
}

void q03_missing_fields_keep_previous_values() {
    requires_("TPL-Q-03");
    Rig rig = makeRigFromFixture("camel.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n1", "a", "一")).code, 0);
    CHECK_EQ(rig.engine->addNode(nodeSpec("n2", "b", "二")).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json full = json::object();
    full["linkId"] = "e1";
    full["bandwidthMbps"] = 120.0;
    full["latencyMs"] = 35.0;
    full["lossRate"] = 0.02;
    full["signal"] = -60.0;
    full["coverageKm2"] = 200.0;
    CHECK_EQ(rig.engine->ingest(full).code, 0);
    const double scoreBefore = rig.engine->linkQuality("e1")->score;
    // 只带 state 的事件：其它指标 MUST 保持原值（MUST NOT 用 0 或默认值覆盖）
    json only = json::object();
    only["linkId"] = "e1";
    only["state"] = "yellow";
    const IngestResult r = rig.engine->ingest(only);
    CHECK_EQ(r.code, 0);
    CHECK_NEAR(r.data.ledger.at("bandwidth_mbps").get<double>(), 120.0, 1e-12);
    CHECK_NEAR(r.data.ledger.at("latency_ms").get<double>(), 35.0, 1e-12);
    CHECK_NEAR(r.data.ledger.at("signal").get<double>(), -60.0, 1e-12);
    CHECK_EQ(r.data.preserved.size(), static_cast<std::size_t>(5));
    CHECK_EQ(r.data.rejected.size(), static_cast<std::size_t>(0));
    CHECK_NEAR(rig.engine->linkQuality("e1")->score, scoreBefore, 1e-12);
    // 无法解释的取值（未在 valueMap 内）同样保持原值，MUST NOT 变成 0
    json weird = json::object();
    weird["linkId"] = "e1";
    weird["signal"] = "unknown-level";
    const IngestResult r2 = rig.engine->ingest(weird);
    CHECK_EQ(r2.code, 0);
    CHECK_EQ(r2.data.rejected.size(), static_cast<std::size_t>(1));
    CHECK_NEAR(r2.data.ledger.at("signal").get<double>(), -60.0, 1e-12);
    CHECK(rig.engine->metrics().rejectedValues >= 1);
    CHECK(rig.engine->metrics().preservedFields >= 5);
}

void q04_sliding_window_aggregation_is_hand_checkable() {
    requires_("TPL-Q-04");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    rig.engine->ingest(levelEvent("e1", 1.0));
    rig.clock->advance(100);
    rig.engine->ingest(levelEvent("e1", 3.0));
    rig.clock->advance(100);
    rig.engine->ingest(levelEvent("e1", 5.0));
    rig.clock->advance(100);
    rig.engine->ingest(levelEvent("e1", 7.0));
    const auto q = rig.engine->linkQuality("e1");
    CHECK_EQ(q->metrics.size(), static_cast<std::size_t>(1));
    CHECK_EQ(q->metrics[0].samples, 4);
    CHECK_NEAR(q->metrics[0].mean, 4.0, 1e-12);  // (1+3+5+7)/4
    CHECK_NEAR(q->metrics[0].min, 1.0, 1e-12);
    CHECK_NEAR(q->metrics[0].max, 7.0, 1e-12);
    CHECK_NEAR(q->metrics[0].last, 7.0, 1e-12);
    CHECK_EQ(q->windowMs, 1000);
    // 推进超过窗口长度 → 旧样本滚出窗口
    rig.clock->advance(2000);
    rig.engine->ingest(levelEvent("e1", 9.0));
    const auto q2 = rig.engine->linkQuality("e1");
    CHECK_EQ(q2->metrics[0].samples, 1);
    CHECK_NEAR(q2->metrics[0].mean, 9.0, 1e-12);
    // 带 ts 的事件（回放）按事件时间入窗
    json back = levelEvent("e1", 5.0);
    back["ts"] = rig.clock->t;
    CHECK_EQ(rig.engine->ingest(back).code, 0);
    CHECK_EQ(rig.engine->linkQuality("e1")->metrics[0].samples, 2);
}

void q05_node_quality_derived_from_edges() {
    requires_("TPL-Q-05");
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二"),
                                   nodeSpec("n3", "a", "三")})
                 .code,
                 0);
    CHECK_EQ(rig.engine->addEdges({edgeSpec("e12", "n1", "n2"), edgeSpec("e23", "n2", "n3")}).code, 0);
    json a = json::object();
    a["linkId"] = "e12";
    a["up"] = 90.0;  // up: higher 0..100 → 0.9
    a["down"] = 10.0;  // down: lower 0..100 → 0.9  → score 0.9
    json b = json::object();
    b["linkId"] = "e23";
    b["up"] = 20.0;  // 0.2
    b["down"] = 80.0;  // 0.2 → score 0.2
    CHECK_EQ(rig.engine->ingest(a).code, 0);
    CHECK_EQ(rig.engine->ingest(b).code, 0);
    const auto n2 = rig.engine->nodeQuality("n2");  // worst 口径
    CHECK(n2.has_value());
    CHECK_EQ(n2->edgeIds.size(), static_cast<std::size_t>(2));
    CHECK_EQ(n2->derivation, std::string("worst"));
    CHECK_NEAR(n2->score, 0.2, 1e-9);
    CHECK(n2->state == LinkState::Red);
    CHECK_NEAR(n2->metrics.at(0).mean, 20.0, 1e-9);  // up 取最不利
    CHECK_NEAR(n2->metrics.at(1).mean, 80.0, 1e-9);  // down 取最不利
    // 口径可配：改成 mean 后同一份数据得到不同派生值
    const json meanPolicy = patched("optimizer.json", [](json& p) { p["aggregation"]["node"] = "mean"; });
    Rig rig2 = makeRig(meanPolicy, false, false);
    CHECK_EQ(rig2.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig2.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二"),
                                    nodeSpec("n3", "a", "三")})
                 .code,
                 0);
    CHECK_EQ(rig2.engine->addEdges({edgeSpec("e12", "n1", "n2"), edgeSpec("e23", "n2", "n3")}).code, 0);
    CHECK_EQ(rig2.engine->ingest(a).code, 0);
    CHECK_EQ(rig2.engine->ingest(b).code, 0);
    const auto n2b = rig2.engine->nodeQuality("n2");
    CHECK_EQ(n2b->derivation, std::string("mean"));
    CHECK_NEAR(n2b->score, 0.55, 1e-9);  // (0.9+0.2)/2
}

void q06_coverage_and_mesh_progress_are_explicit() {
    requires_("TPL-Q-06");
    Rig rig = makeRig(readJson(policyPath()), false, false);  // coverage: sum / 1200; mesh: 0.5*up + 0.5*score
    CHECK_EQ(rig.engine->configureTopology("t-1", "mesh").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "cluster", "集群一"), nodeSpec("n2", "cluster", "集群二"),
                                   nodeSpec("n3", "forward", "前沿")})
                 .code,
                 0);
    CHECK_EQ(rig.engine->addEdges({edgeSpec("e1", "n1", "n3"), edgeSpec("e2", "n2", "n3")}).code, 0);
    json a = json::object();
    a["linkId"] = "e1";
    a["signal"] = -50.0;      // (70/120) = 0.5833
    a["bandwidthMbps"] = 150;  // 0.75
    a["latencyMs"] = 40;       // 0.92
    a["lossRate"] = 0.01;      // 0.99
    a["coverageKm2"] = 300;    // 0.75
    json b = a;
    b["linkId"] = "e2";
    CHECK_EQ(rig.engine->ingest(a).code, 0);
    CHECK_EQ(rig.engine->ingest(b).code, 0);
    const DerivedMetrics d = rig.engine->derived();
    // 覆盖率 = Σ覆盖 / 目标面积 = (300+300)/1200 = 0.5
    CHECK_NEAR(d.get("coverageRatio"), 0.5, 1e-9);
    // 手算链路得分（见规则包权重 0.25/0.25/0.25/0.15/0.10）
    const double score = 0.25 * (70.0 / 120.0) + 0.25 * 0.75 + 0.25 * ((500.0 - 40.0) / 500.0) +
                         0.15 * (1.0 - 0.01) + 0.10 * 0.75;
    CHECK_NEAR(d.get("stateScoreMean"), score, 1e-9);
    CHECK_NEAR(d.get("linkUpRatio"), 1.0, 1e-9);
    // 组网进度 = (0.5*linkUpRatio + 0.5*stateScoreMean) * 100
    CHECK_NEAR(d.get("meshProgress"), (0.5 * 1.0 + 0.5 * score) * 100.0, 1e-6);
    CHECK_NEAR(rig.engine->linkQuality("e1")->metrics.at(1).mean, 150.0, 1e-9);
}

// ================================================================ TPL-ST

void st01_thresholds_come_from_policy() {
    requires_("TPL-ST-01");
    const std::vector<double> seq = {0.80, 0.80, 0.80};
    // 阈值 A：green ≥ 0.75
    Rig rigA = makeRigFromFixture("fsm-basic.json");
    basicTopology(rigA);
    for (double v : seq) feed(rigA, "e1", v);
    CHECK(rigA.engine->linkState("e1").has_value());
    CHECK(*rigA.engine->linkState("e1") == LinkState::Green);
    // 阈值 B：把 green 抬到 0.95 → 同一份输入给出不同状态
    const json pk = patched("fsm-basic.json", [](json& p) {
        p["states"][0]["min"] = 0.95;
        p["states"][1]["min"] = 0.9;
        p["states"][2]["min"] = 0.0;
    });
    Rig rigB = makeRig(pk, false, false);
    basicTopology(rigB);
    for (double v : seq) feed(rigB, "e1", v);
    CHECK(*rigB.engine->linkState("e1") == LinkState::Red);
    CHECK_NEAR(rigA.engine->policy()->states[0].min, 0.75, 1e-12);
    CHECK_NEAR(rigB.engine->policy()->states[0].min, 0.95, 1e-12);
}

void st02_hysteresis_kills_oscillation() {
    requires_("TPL-ST-02");
    // 阈值附近震荡的输入序列：0.74 / 0.76 交替
    std::vector<double> seq;
    for (int i = 0; i < 20; ++i) seq.push_back((i % 2 == 0) ? 0.74 : 0.76);

    Rig rig = makeRigFromFixture("fsm-basic.json");  // rise/fall 0.05、confirm 3
    basicTopology(rig);
    for (double v : seq) feed(rig, "e1", v);
    const int withHysteresis = static_cast<int>(rig.engine->stateLog("e1").size());
    const int suppressed = static_cast<int>(rig.engine->metrics().suppressedChanges);

    // 对照组：把迟滞参数调到"名存实亡"（margin 极小、确认 1 次）→ 每次越阈都翻转
    const json control = patched("fsm-basic.json", [](json& p) {
        p["hysteresis"]["riseMargin"] = 1e-9;
        p["hysteresis"]["fallMargin"] = 1e-9;
        p["hysteresis"]["confirmCount"] = 1;
        p["hysteresis"]["minDwellMs"] = 0;
    });
    Rig rigC = makeRig(control, false, false);
    basicTopology(rigC);
    for (double v : seq) feed(rigC, "e1", v);
    const int withoutHysteresis = static_cast<int>(rigC.engine->stateLog("e1").size());

    const int crossings = bandFlips(*rig.engine->policy(), seq);
    CHECK_MSG(crossings >= 15, "输入序列确实在阈值附近反复越阈");
    CHECK_MSG(withHysteresis * 4 <= crossings, "迟滞后状态变更次数应显著低于越阈次数");
    CHECK_MSG(withHysteresis * 4 <= withoutHysteresis, "迟滞后变更次数应显著低于无迟滞对照");
    CHECK_MSG(withoutHysteresis >= 15, "对照组（无迟滞）应几乎每次越阈都翻转");
    CHECK(suppressed > 0);
    CHECK_EQ(withHysteresis, 0);
}

void st03_min_dwell_limits_change_rate() {
    requires_("TPL-ST-03");
    Rig rig = makeRigFromFixture("fsm-dwell.json");  // confirm 2、minDwell 5000
    basicTopology(rig);
    CHECK_NEAR(static_cast<double>(rig.engine->policy()->hysteresis.minDwellMs), 5000.0, 1e-9);
    // t=0 高 → 之后持续低 → 再持续高
    feed(rig, "e1", 0.9, 0);
    rig.clock->advance(1000);
    for (int i = 0; i < 10; ++i) feed(rig, "e1", 0.1, 1000);
    rig.clock->advance(1000);
    for (int i = 0; i < 10; ++i) feed(rig, "e1", 0.9, 1000);
    const auto log = rig.engine->stateLog("e1");
    CHECK_EQ(log.size(), static_cast<std::size_t>(2));
    if (log.size() == 2) {
        CHECK(log[0].to == LinkState::Red);
        CHECK(log[1].to == LinkState::Green);
        // 变更间隔 MUST ≥ 最小驻留时长
        CHECK(log[0].ts - rig.clock->t + 21000 >= 0);
        CHECK_MSG(log[1].ts - log[0].ts >= 5000, "两次变更间隔 MUST ≥ minDwellMs");
        CHECK_MSG(log[0].confirmations >= 2, "驻留未满时确认次数持续累积，到点才迁移");
    }
    // 理论上限：总时长 / minDwellMs + 1
    const int64_t span = 21000;
    const int upperBound = static_cast<int>(span / 5000) + 1;
    CHECK(static_cast<int>(log.size()) <= upperBound);
    CHECK(rig.engine->metrics().suppressedChanges > 0);
}

void st04_state_changes_are_explainable() {
    requires_("TPL-ST-04");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    feed(rig, "e1", 0.9);   // 初始 green
    feed(rig, "e1", 0.1);   // 第 1 次确认
    feed(rig, "e1", 0.1);   // 第 2 次
    feed(rig, "e1", 0.1);   // 第 3 次 → 迁移
    const auto log = rig.engine->stateLog("e1");
    CHECK_EQ(log.size(), static_cast<std::size_t>(1));
    if (log.empty()) return;
    const StateChange& c = log[0];
    CHECK(c.from == LinkState::Green);
    CHECK(c.to == LinkState::Red);
    CHECK_EQ(c.metric, std::string("level"));
    CHECK_NEAR(c.value, 0.1, 1e-9);
    CHECK_NEAR(c.score, 0.1, 1e-9);
    CHECK_NEAR(c.threshold, 0.75, 1e-9);  // 越过的是"当前 green 档"的下沿
    CHECK_NEAR(c.margin, 0.05, 1e-9);
    CHECK_EQ(c.confirmations, 3);
    CHECK_EQ(c.manual, false);
    CHECK_EQ(c.reason, std::string("hysteresis-confirmed"));
    // 判定过程可复原：逐指标数值 + 归一值都在留痕里
    CHECK(c.checks.contains("metrics"));
    CHECK_NEAR(c.checks.at("metrics").at("level").at("value").get<double>(), 0.1, 1e-9);
    CHECK_NEAR(c.checks.at("metrics").at("level").at("normalized").get<double>(), 0.1, 1e-9);
    CHECK_EQ(c.checks.at("band"), std::string("red"));
    // 用留痕里的指标数值独立复算得分，必须与记录一致
    const MetricDef& m = rig.engine->policy()->metrics[0];
    const double raw = c.checks.at("metrics").at("level").at("value").get<double>();
    const double norm = (raw - m.min) / (m.max - m.min);
    CHECK_NEAR(norm * m.weight / m.weight, c.checks.at("score").get<double>(), 1e-9);
    // JSON 形状（供宿主落库/广播）
    const json j = c.toJson();
    CHECK_EQ(j.at("from"), std::string("green"));
    CHECK_EQ(j.at("to"), std::string("red"));
    CHECK(j.contains("checks"));
}

void st05_state_output_is_three_valued_and_colorless() {
    requires_("TPL-ST-05");
    Rig rig = makeRig(readJson(policyPath()), false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "mesh").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "cluster", "一"), nodeSpec("n2", "cluster", "二")}).code,
             0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json good = json::object();
    good["linkId"] = "e1";
    good["signal"] = -50;
    good["bandwidthMbps"] = 150;
    good["latencyMs"] = 40;
    good["lossRate"] = 0.01;
    good["coverageKm2"] = 300;
    CHECK_EQ(rig.engine->ingest(good).code, 0);
    const auto st = rig.engine->linkState("e1");
    CHECK(st.has_value());
    const char* name = toString(*st);
    CHECK(std::string(name) == "green" || std::string(name) == "yellow" || std::string(name) == "red");
    CHECK_EQ(rig.engine->topologyView().edges[0].toJson().at("state"), std::string("green"));
    // 首次判定是"落档"不是"变更"，不广播；真实变更才出事件（协议语义：事件 = 变化）
    CHECK_EQ(rig.sink->events.size(), static_cast<std::size_t>(0));
    json bad = json::object();
    bad["linkId"] = "e1";
    bad["signal"] = -110.0;
    bad["bandwidthMbps"] = 5.0;
    bad["latencyMs"] = 480.0;
    bad["lossRate"] = 0.5;
    bad["coverageKm2"] = 10.0;
    for (int i = 0; i < 3; ++i) {  // 连续 N 次确认（规则包 confirmCount=3，且需越过 minDwellMs）
        rig.clock->advance(2000);
        CHECK_EQ(rig.engine->ingest(bad).code, 0);
    }
    CHECK_EQ(rig.sink->events.size(), static_cast<std::size_t>(1));
    if (!rig.sink->events.empty()) {
        CHECK_EQ(rig.sink->events[0].state, std::string("red"));
    }
    CHECK(*rig.engine->linkState("e1") == LinkState::Red);
    // 只出状态码，不出颜色值：整份输出里没有颜色字面量
    const json view = rig.engine->topologyView().toJson();
    const auto quality = rig.engine->linkQuality("e1");
    CHECK(quality.has_value());
    std::string all = dump(view) + dump(rig.engine->primitives());
    if (quality.has_value()) all += dump(quality->toJson());
    CHECK(all.find("rgb(") == std::string::npos);
    CHECK(all.find("#") == std::string::npos);
    CHECK(all.find("color") == std::string::npos);
}

void st06_manual_override_and_release() {
    requires_("TPL-ST-06");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    feed(rig, "e1", 0.1);  // 初始 red
    CHECK(*rig.engine->linkState("e1") == LinkState::Red);
    // 人工标记为 green：覆盖期间自动判定不生效
    const MutationResult ov = rig.engine->setOverride("e1", LinkState::Green, "op-1", "人工确认可用");
    CHECK_EQ(ov.code, 0);
    CHECK(*rig.engine->linkState("e1") == LinkState::Green);
    for (int i = 0; i < 5; ++i) feed(rig, "e1", 0.05);  // 仍然 red 的数据
    CHECK_MSG(*rig.engine->linkState("e1") == LinkState::Green, "覆盖期间自动判定 MUST NOT 生效");
    CHECK_EQ(rig.engine->overrides().size(), static_cast<std::size_t>(1));
    CHECK_EQ(rig.engine->overrides()[0].operatorId, std::string("op-1"));
    // 幂等：同一状态再设一次 → code=0 + idempotent=true，不留第二条日志
    const std::size_t logSize = rig.engine->stateLog("e1").size();
    const MutationResult again = rig.engine->setOverride("e1", LinkState::Green, "op-1", "重复");
    CHECK_EQ(again.code, 0);
    CHECK_EQ(again.data.at("idempotent").get<bool>(), true);
    CHECK_EQ(rig.engine->stateLog("e1").size(), logSize);
    // 不同状态未声明 replace → 冲突拒绝（1002）
    const MutationResult conflict = rig.engine->setOverride("e1", LinkState::Yellow, "op-2", "改判");
    CHECK_EQ(conflict.code, static_cast<int>(ErrorCode::Conflict));
    CHECK_EQ(rig.engine->setOverride("e1", LinkState::Yellow, "op-2", "改判", true).code, 0);
    // 人工动作缺 operator/reason → 参数错误
    CHECK_EQ(rig.engine->setOverride("e1", LinkState::Red, "", "x").code,
             static_cast<int>(ErrorCode::BadRequest));
    // 解除 → 立即恢复自动判定
    feed(rig, "e1", 0.05);
    const MutationResult cl = rig.engine->clearOverride("e1", "op-1", "恢复自动");
    CHECK_EQ(cl.code, 0);
    CHECK(*rig.engine->linkState("e1") == LinkState::Red);
    const auto log = rig.engine->stateLog("e1");
    bool sawClear = false;
    bool sawManual = false;
    for (const auto& c : log) {
        if (c.reason == "override-cleared") sawClear = true;
        if (c.manual && c.reason == "manual-override") sawManual = true;
    }
    CHECK(sawClear);
    CHECK(sawManual);
    const MutationResult cl2 = rig.engine->clearOverride("e1", "op-1", "再解除一次");
    CHECK_EQ(cl2.code, 0);
    CHECK_EQ(cl2.data.at("idempotent").get<bool>(), true);
}

// ================================================================ TPL-EVAL

void eval01_four_items_are_recomputable() {
    requires_("TPL-EVAL-01");
    Rig rig = makeRigFromFixture("eval.json", false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二"),
                                   nodeSpec("n3", "a", "三")})
                 .code,
                 0);
    CHECK_EQ(rig.engine->addEdges({edgeSpec("e12", "n1", "n2"), edgeSpec("e23", "n2", "n3")}).code, 0);
    json e = json::object();
    e["linkId"] = "e12";
    e["x"] = 8.0;  // higher 0..10 → 0.8
    e["y"] = 2.0;  // lower  0..10 → 0.8   → 链路得分 0.8
    json e2 = e;
    e2["linkId"] = "e23";
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    CHECK_EQ(rig.engine->ingest(e2).code, 0);
    const EvaluationResult r = rig.engine->evaluate();
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.items.size(), static_cast<std::size_t>(4));
    // 手算四项（规则包 eval.json 的权重与 scale）
    CHECK_NEAR(r.itemValue("stateMean").value(), 0.8, 1e-9);
    CHECK_NEAR(r.itemValue("metricMix").value(), 0.8, 1e-9);
    CHECK_NEAR(r.itemValue("coverage").value(), 0.8, 1e-9);   // (8+8)/20
    CHECK_NEAR(r.itemValue("progress").value(), 100.0, 1e-9); // 组网进度 = 100 * linkUpRatio
    CHECK_NEAR(r.overall, (0.8 + 0.8 + 0.8 + 1.0) / 4.0, 1e-9);
    // 逐项依据：source + weight + value + contribution 可复算
    const EvaluationItem& mix = r.items[1];
    CHECK_EQ(mix.terms.size(), static_cast<std::size_t>(2));
    CHECK_NEAR(mix.terms[0].value, 0.8, 1e-9);
    CHECK_NEAR(mix.terms[0].contribution, 0.4, 1e-9);
    CHECK_NEAR((mix.terms[0].contribution + mix.terms[1].contribution) /
                   (mix.terms[0].weight + mix.terms[1].weight),
               mix.value, 1e-9);
    // 输入显式：派生输入快照在手（可手算复核）
    CHECK_NEAR(r.inputs.at("stateScoreMean").get<double>(), 0.8, 1e-9);
    CHECK_NEAR(r.inputs.at("coverageRatio").get<double>(), 0.8, 1e-9);
    CHECK_NEAR(r.inputs.at("meshProgress").get<double>(), 100.0, 1e-9);
    CHECK_EQ(r.items[0].key, std::string("stateMean"));
}

void eval02_output_is_structured_without_prose() {
    requires_("TPL-EVAL-02");
    Rig rig = makeRig(readJson(policyPath()), false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "mesh").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "cluster", "一"), nodeSpec("n2", "cluster", "二")}).code,
             0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["signal"] = -50;
    e["bandwidthMbps"] = 150;
    e["latencyMs"] = 40;
    e["lossRate"] = 0.01;
    e["coverageKm2"] = 300;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    const std::string text = dump(rig.engine->evaluate().toJson());
    // 无成句文案：整份输出是 ASCII（引擎零文案，P6），且不含句读
    bool ascii = true;
    for (unsigned char c : text) {
        if (c > 0x7F) ascii = false;
    }
    CHECK_MSG(ascii, "评估输出 MUST 只有结构化键值，不含自然语言文案");
    CHECK(text.find("。") == std::string::npos);
    CHECK(text.find("！") == std::string::npos);
    CHECK(text.find("支持") == std::string::npos);
    // 数值 + 逐项依据齐备
    const json j = rig.engine->evaluate().toJson();
    CHECK(j.at("items").is_array());
    CHECK(j.at("overall").is_number());
    CHECK(j.at("inputs").is_object());
    for (const auto& item : j.at("items")) {
        CHECK(item.at("value").is_number());
        CHECK(item.at("terms").is_array());
        for (const auto& t : item.at("terms")) {
            CHECK(t.at("value").is_number());
            CHECK(t.at("weight").is_number());
            CHECK(t.at("contribution").is_number());
        }
    }
}

void eval03_stability_is_read_only_for_scoring() {
    requires_("TPL-EVAL-03");
    Rig rig = makeRigFromFixture("eval.json", false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["x"] = 8.0;
    e["y"] = 2.0;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    const auto stab = rig.engine->linkStability();
    CHECK(stab.has_value());
    CHECK_NEAR(*stab, 0.8, 1e-9);
    // 只读：调用不改变任何状态、不产生事件、不计入评估次数
    const std::string before = dump(rig.engine->exportSnapshot());
    const int64_t evals = rig.engine->metrics().evaluations;
    const std::size_t events = rig.sink->events.size();
    const auto again = rig.engine->linkStability();
    CHECK_NEAR(*again, 0.8, 1e-9);
    CHECK_EQ(dump(rig.engine->exportSnapshot()), before);
    CHECK_EQ(rig.engine->metrics().evaluations, evals);
    CHECK_EQ(rig.sink->events.size(), events);
    // 阈值提示：结构化的布尔 + 依据（评测项自带 requirement）
    const EvaluationResult r = rig.engine->evaluate();
    const EvaluationItem& item = r.items[0];
    CHECK(item.hasRequirement);
    CHECK_EQ(item.requirementOp, std::string(">="));
    CHECK_NEAR(item.requirementLimit, 0.5, 1e-9);
    CHECK_NEAR(item.requirementActual, 0.8, 1e-9);
    CHECK_EQ(item.satisfied, true);
}

void eval04_requirement_hint_is_reproducible() {
    requires_("TPL-EVAL-04");
    Rig rig = makeRigFromFixture("eval.json", false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json bad = json::object();
    bad["linkId"] = "e1";
    bad["x"] = 1.0;   // 0.1
    bad["y"] = 9.0;   // 0.1 → 得分 0.1 < 0.5
    CHECK_EQ(rig.engine->ingest(bad).code, 0);
    const EvaluationResult r1 = rig.engine->evaluate();
    const EvaluationItem& item = r1.items[0];
    CHECK(item.hasRequirement);
    CHECK_NEAR(item.requirementActual, 0.1, 1e-9);
    CHECK_EQ(item.satisfied, false);
    const json j = item.toJson();
    CHECK_EQ(j.at("requirement").at("satisfied").get<bool>(), false);
    CHECK_EQ(j.at("requirement").at("op"), std::string(">="));
    // 判定可复现：同样输入再跑一次，结论逐字节一致
    const EvaluationResult r2 = rig.engine->evaluate();
    CHECK_EQ(dump(r1.toJson()), dump(r2.toJson()));
}

// ================================================================ TPL-OPT

void opt01_suggestions_follow_the_declared_step() {
    requires_("TPL-OPT-01");
    // 步长 10 → 建议值 = 当前 + 8×10（到搜索空间上界），步数 8
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["up"] = 20.0;
    e["down"] = 80.0;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    const OptimizationResult r1 = rig.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(r1.code, 0);
    CHECK_EQ(r1.improved, true);
    CHECK_EQ(r1.suggestions.size(), static_cast<std::size_t>(2));
    const double step = rig.engine->policy()->optimization.parameters[0].step;
    CHECK_NEAR(step, 10.0, 1e-12);
    for (const auto& s : r1.suggestions) {
        CHECK_NEAR(s.delta, s.suggested - s.current, 1e-9);
        CHECK_NEAR(std::fabs(s.delta) - static_cast<double>(s.steps) * step, 0.0, 1e-9);
        CHECK(s.suggested <= 100.0 + 1e-9);
        CHECK(s.suggested >= 0.0 - 1e-9);
    }
    // 换步长即换搜索粒度：步长 20 → 步数减半（引擎里没有乘性常量）
    const json step20 = patched("optimizer.json", [](json& p) {
        p["optimization"]["parameters"][0]["step"] = 20;
        p["optimization"]["parameters"][1]["step"] = 20;
    });
    Rig rig2 = makeRig(step20, false, false);
    CHECK_EQ(rig2.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig2.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig2.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    CHECK_EQ(rig2.engine->ingest(e).code, 0);
    const OptimizationResult r2 = rig2.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(r2.suggestions.size(), static_cast<std::size_t>(2));
    if (r2.suggestions.size() == 2) {
        CHECK_EQ(r2.suggestions[0].steps, 4);
        CHECK_NEAR(r2.suggestions[0].suggested, 100.0, 1e-9);
    }
    CHECK_NEAR(r1.before.overall, 0.2, 1e-9);
    CHECK_NEAR(r1.after.overall, 1.0, 1e-9);
}

void opt02_suggestions_never_touch_state() {
    requires_("TPL-OPT-02");
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["up"] = 30.0;
    e["down"] = 70.0;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    const std::string snapBefore = dump(rig.engine->exportSnapshot());
    const std::string qualityBefore = dump(rig.engine->linkQuality("e1")->toJson());
    const LinkState stateBefore = *rig.engine->linkState("e1");
    const std::size_t eventsBefore = rig.sink->events.size();
    const OptimizationResult r = rig.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(r.code, 0);
    CHECK(!r.suggestions.empty());
    // 建议只是建议：状态、快照、质量、事件全都不变
    CHECK_EQ(dump(rig.engine->exportSnapshot()), snapBefore);
    CHECK_EQ(dump(rig.engine->linkQuality("e1")->toJson()), qualityBefore);
    CHECK(*rig.engine->linkState("e1") == stateBefore);
    CHECK_EQ(rig.sink->events.size(), eventsBefore);
    // 建议值 ∈ 声明的搜索空间，且沿指标方向（up 越大越好 → 建议更大）
    for (const auto& s : r.suggestions) {
        if (s.metric == "up") CHECK(s.suggested > s.current);
        if (s.metric == "down") CHECK(s.suggested < s.current);
    }
    // 未声明搜索空间 → 前置条件未满足（1003）而不是编一个算法
    Rig rig2 = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig2);
    const OptimizationResult r2 = rig2.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(r2.code, static_cast<int>(ErrorCode::GateUnmet));
    CHECK_EQ(r2.reason, std::string("no-search-space"));
    CHECK_EQ(r2.suggestions.size(), static_cast<std::size_t>(0));
}

void opt03_ten_runs_converge_without_drift() {
    requires_("TPL-OPT-03");
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["up"] = 20.0;
    e["down"] = 80.0;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    // 连续 10 次优化：结果逐字节一致（不累积、不漂移）
    const std::string first = dump(rig.engine->optimize(PhaseContext(), "e1").toJson());
    for (int i = 0; i < 9; ++i) {
        CHECK_EQ(dump(rig.engine->optimize(PhaseContext(), "e1").toJson()), first);
    }
    // 闭环：宿主把建议值灌回去，再优化 → 继续建议；到上界后返回"无改进"
    double up = 20.0;
    double down = 80.0;
    int rounds = 0;
    for (; rounds < 12; ++rounds) {
        const OptimizationResult r = rig.engine->optimize(PhaseContext(), "e1");
        if (!r.improved) break;
        for (const auto& s : r.suggestions) {
            if (s.metric == "up") up = s.suggested;
            if (s.metric == "down") down = s.suggested;
        }
        json next = json::object();
        next["linkId"] = "e1";
        next["up"] = up;
        next["down"] = down;
        rig.clock->advance(2000);
        CHECK_EQ(rig.engine->ingest(next).code, 0);
    }
    CHECK_MSG(rounds < 12, "闭环优化 MUST 收敛（不无限继续建议）");
    const OptimizationResult last = rig.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(last.improved, false);
    CHECK_EQ(last.reason, std::string("no-improvement"));
    CHECK_EQ(last.suggestions.size(), static_cast<std::size_t>(0));
    CHECK_NEAR(last.after.overall, last.before.overall, 1e-12);
    CHECK(rig.engine->metrics().noImprovement >= 1);
}

void opt04_result_is_explainable() {
    requires_("TPL-OPT-04");
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["up"] = 40.0;
    e["down"] = 60.0;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    const OptimizationResult r = rig.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(r.improved, true);
    CHECK(r.after.overall > r.before.overall);
    const json j = r.toJson();
    CHECK(j.contains("before"));
    CHECK(j.contains("after"));
    CHECK(j.at("suggestions").is_array());
    for (const auto& s : r.suggestions) {
        const json sj = s.toJson();
        CHECK(sj.contains("metric"));
        CHECK(sj.contains("current"));
        CHECK(sj.contains("suggested"));
        CHECK(sj.contains("delta"));
        CHECK(sj.contains("steps"));
        // 改了哪些参数 → 哪些评估项变化（逐项前后对比）
        CHECK(sj.at("impact").is_object());
        CHECK(sj.at("impact").contains("stability"));
        CHECK(sj.at("impact").at("stability").at("after").get<double>() >
              sj.at("impact").at("stability").at("before").get<double>());
    }
    // 目标项之外的评估项也在 after 里逐项可见
    CHECK(r.after.items.size() >= 2);
    CHECK(r.after.itemValue("headroom").has_value());
}

void opt05_monotonic_and_no_improvement() {
    requires_("TPL-OPT-05");
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    // 已经处于最优（up 顶格、down 到底）→ 无改进
    json best = json::object();
    best["linkId"] = "e1";
    best["up"] = 100.0;
    best["down"] = 0.0;
    CHECK_EQ(rig.engine->ingest(best).code, 0);
    const OptimizationResult r = rig.engine->optimize(PhaseContext(), "e1");
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.improved, false);
    CHECK_EQ(r.reason, std::string("no-improvement"));
    CHECK(r.suggestions.empty());
    CHECK_MSG(r.after.overall >= r.before.overall - 1e-12, "优化后整体评估 MUST NOT 变差");
    // 中间态：优化后一定不差，且建议值在界内
    json mid = json::object();
    mid["linkId"] = "e1";
    mid["up"] = 10.0;
    mid["down"] = 90.0;
    rig.clock->advance(2000);
    CHECK_EQ(rig.engine->ingest(mid).code, 0);
    const OptimizationResult r2 = rig.engine->optimize(PhaseContext(), "e1");
    CHECK(r2.after.overall >= r2.before.overall - 1e-12);
    CHECK(r2.improved);
    const auto polOpt = rig.engine->policy();  // 先取副本，避免绑定临时对象的成员
    CHECK(polOpt.has_value());
    for (const auto& s : r2.suggestions) {
        const OptimizationParamDef* def = nullptr;
        if (polOpt.has_value()) {
            for (const auto& param : polOpt->optimization.parameters) {
                if (param.metric == s.metric) def = &param;
            }
        }
        CHECK(def != nullptr);
        if (def != nullptr) {
            CHECK(s.suggested >= def->min - 1e-9);
            CHECK(s.suggested <= def->max + 1e-9);
        }
    }
}

void opt06_curves_come_from_real_windows() {
    requires_("TPL-OPT-06");
    Rig rig = makeRigFromFixture("optimizer.json");
    CHECK_EQ(rig.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(rig.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    // 时间粒度 1000 ms；每次推进 1001 ms 使窗口内只剩本次采样 → 曲线点 = 手算窗口均值
    const double values[3] = {10.0, 20.0, 30.0};
    for (int i = 0; i < 3; ++i) {
        json e = json::object();
        e["linkId"] = "e1";
        e["up"] = values[i];
        e["down"] = 100.0 - values[i];
        CHECK_EQ(rig.engine->ingest(e).code, 0);
        rig.clock->advance(1001);
    }
    const CurveSeries c = rig.engine->curves("e1");
    CHECK_EQ(c.granularityMs, 1000);
    CHECK_EQ(c.windowMs, 1000);
    CHECK_EQ(c.points.size(), static_cast<std::size_t>(3));
    for (std::size_t i = 0; i < c.points.size() && i < 3; ++i) {
        CHECK_NEAR(c.points[i].values.at("up").get<double>(), values[i], 1e-9);
        CHECK_EQ(c.points[i].samples, 1);
    }
    CHECK_EQ(c.metrics.size(), static_cast<std::size_t>(2));
    // 恒定输入 → 曲线必须平（写死公式会给出随时间上升的形状）
    Rig flat = makeRigFromFixture("optimizer.json");
    CHECK_EQ(flat.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(flat.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(flat.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    for (int i = 0; i < 3; ++i) {
        json e = json::object();
        e["linkId"] = "e1";
        e["up"] = 42.0;
        e["down"] = 50.0;
        CHECK_EQ(flat.engine->ingest(e).code, 0);
        flat.clock->advance(1001);
    }
    const CurveSeries fc = flat.engine->curves("e1");
    CHECK_EQ(fc.points.size(), static_cast<std::size_t>(3));
    for (const auto& p : fc.points) {
        CHECK_NEAR(p.values.at("up").get<double>(), 42.0, 1e-9);
    }
    // 环形缓冲：超过 maxPoints 后只保留最近的点
    Rig ring = makeRigFromFixture("optimizer.json");
    CHECK_EQ(ring.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(ring.engine->addNodes({nodeSpec("n1", "a", "一"), nodeSpec("n2", "a", "二")}).code, 0);
    CHECK_EQ(ring.engine->addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    for (int i = 0; i < 12; ++i) {
        json e = json::object();
        e["linkId"] = "e1";
        e["up"] = static_cast<double>(i);
        e["down"] = 50.0;
        CHECK_EQ(ring.engine->ingest(e).code, 0);
        ring.clock->advance(1001);
    }
    const CurveSeries rc = ring.engine->curves("e1");
    CHECK_EQ(rc.points.size(), static_cast<std::size_t>(8));  // maxPoints
    CHECK_NEAR(rc.points.front().values.at("up").get<double>(), 4.0, 1e-9);  // 第 5 个点
    CHECK_NEAR(rc.points.back().values.at("up").get<double>(), 11.0, 1e-9);
    CHECK(ring.engine->metrics().curvePoints >= 12);
}

// ================================================================ TPL-NFR

void nfr01_zero_dependencies_memory_only() {
    requires_("TPL-NFR-01");
    // 空环境：不注入任何依赖也能工作（纯内存）
    Rig rig = makeRigFromFixture("fsm-basic.json");
    const Capabilities cap = rig.engine->capabilities();
    CHECK_EQ(cap.clockInjected, true);
    CHECK_EQ(cap.storeInjected, false);
    CHECK_EQ(cap.sinkInjected, true);
    CHECK_EQ(cap.logInjected, false);
    CHECK_EQ(cap.layoutInjected, false);
    basicTopology(rig);
    feed(rig, "e1", 0.9);
    CHECK(rig.engine->linkQuality("e1").has_value());
    CHECK(rig.engine->evaluate().code == 0);
    // 完全不注入（连时钟/出口都没有）也能构造与工作
    TopologyEngine bare;
    const Capabilities c0 = bare.capabilities();
    CHECK_EQ(c0.clockInjected, false);
    CHECK_EQ(c0.sinkInjected, false);
    CHECK_EQ(bare.loadPolicy(readJson(fixturePath("fsm-basic.json"))).code, 0);
    CHECK_EQ(bare.configureTopology("t", "flat").code, 0);
    CHECK_EQ(bare.addNode(nodeSpec("n1", "a", "一")).code, 0);
    CHECK_EQ(bare.addNode(nodeSpec("n2", "b", "二")).code, 0);
    CHECK_EQ(bare.addEdge(edgeSpec("e1", "n1", "n2")).code, 0);
    CHECK_EQ(bare.ingest(levelEvent("e1", 0.9)).code, 0);
    CHECK(bare.linkState("e1").has_value());
    // 未装载规则时：需要规则的能力全部返回结构化错误，MUST NOT 崩
    TopologyEngine empty;
    CHECK_EQ(empty.capabilities().policyLoaded, false);
    CHECK_EQ(empty.ingest(levelEvent("e1", 1.0)).code, static_cast<int>(ErrorCode::Internal));
    CHECK_EQ(empty.evaluate().code, static_cast<int>(ErrorCode::Internal));
    CHECK_EQ(empty.configureTopology("t", "flat").code, static_cast<int>(ErrorCode::Internal));
    CHECK_EQ(empty.optimize(PhaseContext(), "e1").code, static_cast<int>(ErrorCode::Internal));
}

void nfr02_sink_and_store_are_injected_interfaces() {
    requires_("TPL-NFR-02");
    Rig rig = makeRigFromFixture("fsm-basic.json", true, false);
    basicTopology(rig);
    // 模型变更走注入的 store（引擎不接触 SQL）
    CHECK(rig.store->saves > 0);
    CHECK(rig.store->data.count("t-1") == 1);
    // missionId 来自注入的 PhaseContext
    PhaseContext ctx;
    ctx.phaseKey = "T3";
    ctx.seq = 3;
    ctx.scenarioKey = "scenario-1";
    ctx.missionId = "m-1";
    rig.engine->setPhaseContext(ctx);
    feed(rig, "e1", 0.9);  // 落档 green（不广播）
    CHECK_EQ(rig.sink->events.size(), static_cast<std::size_t>(0));
    feed(rig, "e1", 0.1);  // 真实变更：green -> red
    feed(rig, "e1", 0.1);
    feed(rig, "e1", 0.1);
    CHECK_EQ(rig.sink->events.size(), static_cast<std::size_t>(1));
    if (!rig.sink->events.empty()) {
        const json e = rig.sink->events[0].toJson();
        // protocol §4.4 已登记负载字段逐字在内
        CHECK(e.contains("missionId"));
        CHECK(e.contains("edgeId"));
        CHECK(e.contains("from"));
        CHECK(e.contains("to"));
        CHECK(e.contains("state"));
        CHECK(e.contains("metrics"));
        CHECK_EQ(e.at("missionId"), std::string("m-1"));
        CHECK_EQ(e.at("edgeId"), std::string("e1"));
        CHECK_EQ(e.at("from"), std::string("n1"));
        CHECK_EQ(e.at("to"), std::string("n2"));
        CHECK_EQ(e.at("state"), std::string("red"));
        // 迟滞判定结果随事件一起给出
        CHECK(e.at("metrics").contains("score"));
        CHECK(e.at("metrics").contains("threshold"));
        CHECK(e.at("metrics").contains("metric"));
        CHECK(e.at("metrics").contains("manual"));
    }
    CHECK_EQ(rig.sink->changes.size(), static_cast<std::size_t>(1));
    // 出口抛异常 MUST NOT 影响判定（状态已提交）
    Rig rig2 = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig2);
    rig2.sink->throwing = true;
    feed(rig2, "e1", 0.9);
    CHECK(*rig2.engine->linkState("e1") == LinkState::Green);
    CHECK_EQ(rig2.engine->metrics().sinkErrors, 0);  // 首次落档不通知（无变更）
    feed(rig2, "e1", 0.1);
    feed(rig2, "e1", 0.1);
    feed(rig2, "e1", 0.1);
    CHECK(*rig2.engine->linkState("e1") == LinkState::Red);
    CHECK(rig2.engine->metrics().sinkErrors >= 1);
    // 从 store 载回快照
    Rig rig3 = makeRigFromFixture("fsm-basic.json", true, false);
    rig3.engine->setPhaseContext(ctx);
    CHECK(rig3.store->save("t-1", rig.engine->exportSnapshot()));
    const MutationResult lr = rig3.engine->loadSnapshotFromStore("t-1");
    CHECK_EQ(lr.code, 0);
    CHECK_EQ(dump(rig3.engine->exportSnapshot()), dump(rig.engine->exportSnapshot()));
    // 未注入 store → 结构化失败（1005），MUST NOT 崩
    Rig rig4 = makeRigFromFixture("fsm-basic.json", false, false);
    CHECK_EQ(rig4.engine->loadSnapshotFromStore("t-1").code, static_cast<int>(ErrorCode::Internal));
}

void nfr03_fake_clock_makes_time_reproducible() {
    requires_("TPL-NFR-03");
    // 结果分解：`raw` 含绝对时间戳；`decisions` 只含判定结果（与起点绝对时间无关）
    struct Run {
        std::string raw;
        std::string decisions;
    };
    auto run = [](int64_t startMs) {
        Rig rig = makeRigFromFixture("fsm-dwell.json", false, false, startMs);
        basicTopology(rig);
        for (int i = 0; i < 8; ++i) feed(rig, "e1", (i % 3 == 0) ? 0.9 : 0.1, 700);
        Run out;
        json full = json::object();
        json decisions = json::object();
        full["log"] = json::array();
        decisions["log"] = json::array();
        for (const auto& c : rig.engine->stateLog("e1")) {
            full["log"].push_back(c.toJson());
            json d = json::object();
            d["from"] = toString(c.from);
            d["to"] = toString(c.to);
            d["metric"] = c.metric;
            d["value"] = c.value;
            d["score"] = c.score;
            d["threshold"] = c.threshold;
            d["margin"] = c.margin;
            d["confirmations"] = c.confirmations;
            d["reason"] = c.reason;
            decisions["log"].push_back(d);
        }
        full["quality"] = rig.engine->linkQuality("e1")->toJson();
        const auto q = rig.engine->linkQuality("e1");
        decisions["state"] = q->hasState ? toString(q->state) : std::string();
        decisions["score"] = q->score;
        decisions["samples"] = q->metrics.at(0).samples;
        decisions["mean"] = q->metrics.at(0).mean;
        full["snapshot"] = rig.engine->exportSnapshot();
        decisions["edges"] = json::array();
        for (const auto& e : rig.engine->topologyView().edges) {
            decisions["edges"].push_back(e.toJson().at("state"));
        }
        out.raw = dump(full);
        out.decisions = dump(decisions);
        return out;
    };
    const Run a = run(1750000000000LL);
    const Run b = run(1750000000000LL);
    CHECK_MSG(a.raw == b.raw, "同一假时钟 + 同一输入 → 逐字节一致");
    const Run c = run(1900000000000LL);
    CHECK_MSG(a.decisions == c.decisions, "判定结果只取决于相对推进，与起点绝对时间无关");
    // 时间推进决定窗口内容：不推进则样本一直留在窗口里
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    rig.engine->ingest(levelEvent("e1", 1.0));
    rig.engine->ingest(levelEvent("e1", 3.0));
    CHECK_EQ(rig.engine->linkQuality("e1")->metrics[0].samples, 2);
    rig.clock->advance(5000);
    rig.engine->ingest(levelEvent("e1", 5.0));
    CHECK_EQ(rig.engine->linkQuality("e1")->metrics[0].samples, 1);
    CHECK_NEAR(rig.engine->linkQuality("e1")->metrics[0].mean, 5.0, 1e-9);
}

void nfr04_determinism_double_run() {
    requires_("TPL-NFR-04");
    auto run = [] {
        Rig rig = makeRig(readJson(policyPath()), true, true);
        rig.layout->points["n3"] = LayoutPoint{117.0, 40.0};
        rig.engine->configureTopology("t-1", "mesh");
        rig.engine->addNodes({nodeSpec("n1", "cluster", "集群一", true, 116.0, 39.0),
                              nodeSpec("n2", "cluster", "集群二", true, 116.5, 39.5),
                              nodeSpec("n3", "forward", "指挥节点", false)});
        rig.engine->addEdges({edgeSpec("e1", "n1", "n3"), edgeSpec("e2", "n2", "n3"),
                              edgeSpec("e3", "n2", "n1")});
        for (int i = 0; i < 6; ++i) {
            json e = json::object();
            e["linkId"] = (i % 2 == 0) ? "e1" : "e2";
            e["signal"] = -50.0 - 5.0 * i;
            e["bandwidthMbps"] = 120.0 - 10.0 * i;
            e["latencyMs"] = 30.0 + 20.0 * i;
            e["lossRate"] = 0.01 * i;
            e["coverageKm2"] = 250.0;
            rig.engine->ingest(e);
            rig.clock->advance(1000);
        }
        PhaseContext ctx;
        ctx.missionId = "m-1";
        ctx.phaseKey = "T2";
        ctx.scenarioKey = "scenario-1";
        json out = json::object();
        out["snapshot"] = rig.engine->exportSnapshot();
        out["view"] = rig.engine->topologyView().toJson();
        out["primitives"] = rig.engine->primitives();
        out["quality"] = json::array();
        for (const auto& q : rig.engine->linkQualities()) out["quality"].push_back(q.toJson());
        out["derived"] = rig.engine->derived().toJson();
        out["eval"] = rig.engine->evaluate(ctx).toJson();
        out["opt"] = rig.engine->optimize(ctx, "e1").toJson();
        out["curves"] = rig.engine->curves("e1").toJson();
        out["log"] = json::array();
        for (const auto& c : rig.engine->stateLog()) out["log"].push_back(c.toJson());
        out["metrics"] = toJson(rig.engine->metrics());
        return dump(out);
    };
    const std::string a = run();
    const std::string b = run();
    CHECK_MSG(a == b, "同输入同假时钟 MUST 逐字节一致（含迟滞状态机）");
    CHECK(a.size() > 1000);
}

void nfr05_structured_failures_and_self_description() {
    requires_("TPL-NFR-05");
    CHECK(std::string(kEngineVersion).size() > 0);
    CHECK_EQ(std::string(errorCodeName(0)), std::string("ok"));
    CHECK_EQ(std::string(errorCodeName(1006)), std::string("version-mismatch"));
    CHECK_EQ(std::string(errorCodeName(9999)), std::string("unknown"));
    CHECK_EQ(std::string(TopologyEngine::errorCodeName(1004)), std::string("not-found"));
    Rig rig = makeRigFromFixture("fsm-basic.json");
    basicTopology(rig);
    // 一切非法入口都必须返回结构化结果，MUST NOT 抛异常跨边界（P10）
    json notObject = json::array();
    CHECK_EQ(rig.engine->ingest(notObject).code, static_cast<int>(ErrorCode::BadRequest));
    json noId = json::object();
    CHECK_EQ(rig.engine->ingest(noId).code, static_cast<int>(ErrorCode::BadRequest));
    json unknownLink = json::object();
    unknownLink["linkId"] = "nope";
    CHECK_EQ(rig.engine->ingest(unknownLink).code, static_cast<int>(ErrorCode::NotFound));
    CHECK_EQ(rig.engine->linkQuality("nope").has_value(), false);
    CHECK_EQ(rig.engine->nodeQuality("nope").has_value(), false);
    CHECK_EQ(rig.engine->node("nope").has_value(), false);
    CHECK_EQ(rig.engine->edge("nope").has_value(), false);
    CHECK_EQ(rig.engine->linkState("nope").has_value(), false);
    CHECK_EQ(rig.engine->removeEdge("nope").code, static_cast<int>(ErrorCode::NotFound));
    CHECK_EQ(rig.engine->removeNode("nope").code, static_cast<int>(ErrorCode::NotFound));
    CHECK_EQ(rig.engine->clearOverride("nope", "op", "r").code, static_cast<int>(ErrorCode::NotFound));
    CHECK_EQ(rig.engine->optimize(PhaseContext(), "nope").code, static_cast<int>(ErrorCode::NotFound));
    CHECK_EQ(rig.engine->curves("nope").points.size(), static_cast<std::size_t>(0));
    CHECK_EQ(rig.engine->setOverride("e1", LinkState::Green, "", "").code,
             static_cast<int>(ErrorCode::BadRequest));
    // 重复配置拓扑 → 冲突拒绝（1002），除非显式 reset
    CHECK_EQ(rig.engine->configureTopology("t-2", "flat").code, static_cast<int>(ErrorCode::Conflict));
    CHECK_EQ(rig.engine->configureTopology("t-2", "flat", true).code, 0);
    CHECK_EQ(rig.engine->topologyView().nodes.size(), static_cast<std::size_t>(0));
    // 未声明的结构 → 参数错误 + 列出已声明结构
    const MutationResult badStruct = rig.engine->configureTopology("t-3", "nope-structure", true);
    CHECK_EQ(badStruct.code, static_cast<int>(ErrorCode::BadRequest));
    CHECK(badStruct.data.contains("declaredStructures"));
    // 规则包校验（纯函数）与 MAJOR 拒绝
    CHECK_EQ(validatePolicy(readJson(policyPath())).code, 0);
    CHECK_EQ(TopologyEngine::validatePolicy(readJson(fixturePath("bad-major.json"))).code,
             static_cast<int>(ErrorCode::VersionMismatch));
    // PhaseContext 按共享契约形状被消费并回显
    PhaseContext ctx;
    ctx.phaseKey = "T4";
    ctx.seq = 4;
    ctx.scenarioKey = "scenario-2";
    ctx.enteredAt = 123456;
    ctx.missionId = "m-9";
    rig.engine->setPhaseContext(ctx);
    const EvaluationResult ev = rig.engine->evaluate();
    CHECK_EQ(ev.phaseKey, std::string("T4"));
    CHECK_EQ(ev.scenarioKey, std::string("scenario-2"));
    CHECK_EQ(ev.missionId, std::string("m-9"));
    CHECK_EQ(rig.engine->phaseContext().seq, 4);
    const json pc = toJson(rig.engine->phaseContext());
    CHECK_EQ(pc.at("phaseKey"), std::string("T4"));
    CHECK_EQ(pc.at("enteredAt").get<int64_t>(), 123456);
}

void nfr06_performance_1000_edges() {
    requires_("TPL-NFR-06");
    Rig rig = makeRig(readJson(policyPath()), false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "mesh").code, 0);
    const int kEdges = 1000;
    std::vector<NodeSpec> nodes;
    nodes.reserve(kEdges + 1);
    for (int i = 0; i <= kEdges; ++i) {
        nodes.push_back(nodeSpec("n" + std::to_string(i), (i % 2 == 0) ? "cluster" : "forward",
                                 "节点" + std::to_string(i), true, 116.0 + 0.001 * i, 39.0));
    }
    CHECK_EQ(rig.engine->addNodes(nodes).code, 0);
    std::vector<EdgeSpec> edges;
    edges.reserve(kEdges);
    for (int i = 0; i < kEdges; ++i) {
        edges.push_back(edgeSpec("e" + std::to_string(i), "n" + std::to_string(i),
                                 "n" + std::to_string(i + 1)));
    }
    CHECK_EQ(rig.engine->addEdges(edges).code, 0);
    CHECK_EQ(rig.engine->capabilities().edges, kEdges);

    auto makeEvent = [](int i, int tick) {
        json e = json::object();
        e["linkId"] = "e" + std::to_string(i);
        e["signal"] = -50.0 - (i % 20);
        e["bandwidthMbps"] = 100.0 + (i % 50);
        e["latencyMs"] = 40.0 + (i % 30);
        e["lossRate"] = 0.01 * (i % 5);
        e["coverageKm2"] = 200.0 + (tick % 10);
        return e;
    };
    // 预热
    for (int i = 0; i < 200; ++i) rig.engine->ingest(makeEvent(i, 0));
    std::vector<double> samples;
    samples.reserve(kEdges * 2);
    for (int tick = 0; tick < 2; ++tick) {
        for (int i = 0; i < kEdges; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            rig.engine->ingest(makeEvent(i, tick));
            const auto t1 = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        rig.clock->advance(1000);  // 1 Hz 输入
    }
    std::sort(samples.begin(), samples.end());
    const double p95 = samples[static_cast<std::size_t>(samples.size() * 95 / 100)];
    const auto t0 = std::chrono::steady_clock::now();
    const DerivedMetrics d = rig.engine->derived();
    const auto t1 = std::chrono::steady_clock::now();
    const double sweepMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!g_jsonMode) {
        std::string line = "       1000 边 · 单次聚合+判定 P95 = " + std::to_string(p95) +
                           " ms（" + std::to_string(samples.size()) + " 次取样）｜全量派生扫描 " +
                           std::to_string(sweepMs) + " ms\n";
        std::fputs(line.c_str(), stdout);
    }
    CHECK(d.get("linkCount") == 1000.0);
#ifdef NDEBUG
    CHECK_MSG(p95 <= 1.0, "TPL-NFR-06：1000 条边、1 Hz 下单次聚合 + 判定 P95 MUST ≤ 1 ms");
#else
    CHECK_MSG(p95 <= 5.0, "Debug 构建放宽断言（Release 由 acceptance.ps1 断言 ≤ 1 ms）");
#endif
}

void nfr07_primitives_match_consumer_contract() {
    requires_("TPL-MODEL-03", "TPL-ST-05");
    Rig rig = makeRigFromFixture("two-structures.json", false, false);
    CHECK_EQ(rig.engine->configureTopology("t-1", "three").code, 0);
    CHECK_EQ(rig.engine->addNodes({nodeSpec("n1", "tierTop", "上", true, 116.0, 39.0),
                                   nodeSpec("n2", "tierMid", "中", true, 116.5, 39.5),
                                   nodeSpec("n3", "tierEnd", "下", false)})
                 .code,
                 0);
    CHECK_EQ(rig.engine->addEdges({edgeSpec("e1", "n1", "n2"), edgeSpec("e2", "n2", "n3")}).code, 0);
    json e = json::object();
    e["linkId"] = "e1";
    e["level"] = 0.9;
    CHECK_EQ(rig.engine->ingest(e).code, 0);
    const json p = rig.engine->primitives();
    // LinkItem{id, from:[lng,lat], to:[lng,lat], state?}
    CHECK(p.at("links").is_array());
    CHECK_EQ(p.at("links").at(0).at("id"), std::string("e1"));
    CHECK(p.at("links").at(0).at("from").is_array());
    CHECK_EQ(p.at("links").at(0).at("from").size(), static_cast<std::size_t>(2));
    CHECK_NEAR(p.at("links").at(0).at("from").at(0).get<double>(), 116.0, 1e-9);
    CHECK_EQ(p.at("links").at(0).at("state"), std::string("green"));
    // 端点缺坐标的链路被列出而不是编造坐标（n3 无坐标）
    CHECK_EQ(p.at("missingCoordinates").size(), static_cast<std::size_t>(1));
    CHECK_EQ(p.at("missingCoordinates").at(0), std::string("e2"));
    // ClusterItem{id, lng, lat, name}：由规则声明的 graphic 决定
    CHECK_EQ(p.at("clusters").size(), static_cast<std::size_t>(0));
    CHECK_EQ(p.at("missingClusterCoordinates").size(), static_cast<std::size_t>(1));
    CHECK_EQ(p.at("graphics").size(), static_cast<std::size_t>(2));
    Rig rig2 = makeRigFromFixture("two-structures.json", false, false);
    CHECK_EQ(rig2.engine->configureTopology("t-1", "flat").code, 0);
    CHECK_EQ(rig2.engine->addNode(nodeSpec("c1", "tierEnd", "集群一", true, 1.0, 2.0)).code, 0);
    const json p2 = rig2.engine->primitives();
    CHECK_EQ(p2.at("clusters").size(), static_cast<std::size_t>(1));
    CHECK_EQ(p2.at("clusters").at(0).at("id"), std::string("c1"));
    CHECK_EQ(p2.at("clusters").at(0).at("name"), std::string("集群一"));
    CHECK_NEAR(p2.at("clusters").at(0).at("lat").get<double>(), 2.0, 1e-9);
}

void nfr08_policy_loading_discipline() {
    requires_("TPL-MODEL-01", "TPL-Q-01", "TPL-ST-01", "TPL-EVAL-01", "TPL-OPT-02");
    Rig rig = makeRigFromFixture("fsm-basic.json");
    // kind 不符
    CHECK_EQ(rig.engine->loadPolicy(readJson(fixturePath("bad-kind.json"))).code,
             static_cast<int>(ErrorCode::BadRequest));
    // 缺必填字段 → 整包拒绝 + 逐条原因（条目 + 字段）
    const LoadResult miss = rig.engine->loadPolicy(readJson(fixturePath("bad-missing-required.json")));
    CHECK_EQ(miss.code, static_cast<int>(ErrorCode::BadRequest));
    CHECK(!miss.issues.empty());
    CHECK(miss.issues[0].path.find("metrics.items") != std::string::npos);
    CHECK_EQ(miss.issues[0].field, std::string("direction"));
    // 重复节点类型 key
    CHECK_EQ(rig.engine->loadPolicy(readJson(fixturePath("bad-dup-node-type.json"))).code,
             static_cast<int>(ErrorCode::BadRequest));
    // 未知派生输入名（评估算法引用了引擎不认识的输入）
    CHECK_EQ(rig.engine->loadPolicy(readJson(fixturePath("bad-unknown-source.json"))).code,
             static_cast<int>(ErrorCode::BadRequest));
    // 无迟滞的规则包被拒绝（TPL-ST-02 在装载期即守住）
    const LoadResult noHyst = rig.engine->loadPolicy(readJson(fixturePath("bad-no-hysteresis.json")));
    CHECK_EQ(noHyst.code, static_cast<int>(ErrorCode::BadRequest));
    // 失败 MUST NOT 破坏上一次成功装载的规则（原子替换）
    CHECK_EQ(rig.engine->policyInfo().loaded, true);
    CHECK_EQ(rig.engine->policyInfo().metricCount, 1);
    CHECK_EQ(rig.engine->policyInfo().schemaVersion, std::string("1.0.0"));
    // MAJOR 不受支持 → 1006，MUST NOT 静默降级
    CHECK_EQ(rig.engine->loadPolicy(readJson(fixturePath("bad-major.json"))).code,
             static_cast<int>(ErrorCode::VersionMismatch));
    CHECK_EQ(rig.engine->policyInfo().schemaVersion, std::string("1.0.0"));
    // 未知字段 → 装载成功 + 计入告警统计（前向兼容）
    Rig rig2 = makeRigFromFixture("unknown-fields.json");
    CHECK(rig2.engine->policyInfo().warnings.size() >= 2);
    CHECK(rig2.engine->metrics().unknownFields >= 2);
    // 规则包可导出（审计与问题复现）
    const auto pol = rig2.engine->policy();
    CHECK(pol.has_value());
    CHECK_NEAR(static_cast<double>(pol->window.windowMs), 2000.0, 1e-9);
    CHECK_EQ(rig2.engine->policyInfo().digest.size(), static_cast<std::size_t>(16));
    CHECK(rig2.engine->policyInfo().policyVersion.rfind("selftest:1.1.0:", 0) == 0);
}

// ---------------------------------------------------------------- 用例注册表

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    {"model01_node_type_comes_from_declared_key_not_name",
     model01_node_type_comes_from_declared_key_not_name},
    {"model02_same_data_under_two_structures", model02_same_data_under_two_structures},
    {"model03_coordinates_are_injectable", model03_coordinates_are_injectable},
    {"model04_dedup_and_self_loop", model04_dedup_and_self_loop},
    {"model05_validation_locates_defects", model05_validation_locates_defects},
    {"model06_snapshot_roundtrip", model06_snapshot_roundtrip},
    {"q01_metric_set_is_rule_declared", q01_metric_set_is_rule_declared},
    {"q02_normalization_happens_inside_engine", q02_normalization_happens_inside_engine},
    {"q03_missing_fields_keep_previous_values", q03_missing_fields_keep_previous_values},
    {"q04_sliding_window_aggregation_is_hand_checkable",
     q04_sliding_window_aggregation_is_hand_checkable},
    {"q05_node_quality_derived_from_edges", q05_node_quality_derived_from_edges},
    {"q06_coverage_and_mesh_progress_are_explicit", q06_coverage_and_mesh_progress_are_explicit},
    {"st01_thresholds_come_from_policy", st01_thresholds_come_from_policy},
    {"st02_hysteresis_kills_oscillation", st02_hysteresis_kills_oscillation},
    {"st03_min_dwell_limits_change_rate", st03_min_dwell_limits_change_rate},
    {"st04_state_changes_are_explainable", st04_state_changes_are_explainable},
    {"st05_state_output_is_three_valued_and_colorless",
     st05_state_output_is_three_valued_and_colorless},
    {"st06_manual_override_and_release", st06_manual_override_and_release},
    {"eval01_four_items_are_recomputable", eval01_four_items_are_recomputable},
    {"eval02_output_is_structured_without_prose", eval02_output_is_structured_without_prose},
    {"eval03_stability_is_read_only_for_scoring", eval03_stability_is_read_only_for_scoring},
    {"eval04_requirement_hint_is_reproducible", eval04_requirement_hint_is_reproducible},
    {"opt01_suggestions_follow_the_declared_step", opt01_suggestions_follow_the_declared_step},
    {"opt02_suggestions_never_touch_state", opt02_suggestions_never_touch_state},
    {"opt03_ten_runs_converge_without_drift", opt03_ten_runs_converge_without_drift},
    {"opt04_result_is_explainable", opt04_result_is_explainable},
    {"opt05_monotonic_and_no_improvement", opt05_monotonic_and_no_improvement},
    {"opt06_curves_come_from_real_windows", opt06_curves_come_from_real_windows},
    {"nfr01_zero_dependencies_memory_only", nfr01_zero_dependencies_memory_only},
    {"nfr02_sink_and_store_are_injected_interfaces",
     nfr02_sink_and_store_are_injected_interfaces},
    {"nfr03_fake_clock_makes_time_reproducible", nfr03_fake_clock_makes_time_reproducible},
    {"nfr04_determinism_double_run", nfr04_determinism_double_run},
    {"nfr05_structured_failures_and_self_description",
     nfr05_structured_failures_and_self_description},
    {"nfr06_performance_1000_edges", nfr06_performance_1000_edges},
    {"nfr07_primitives_match_consumer_contract", nfr07_primitives_match_consumer_contract},
    {"nfr08_policy_loading_discipline", nfr08_policy_loading_discipline},
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc < 2) std::system("chcp 65001 > nul");  // 控制台切 UTF-8，否则中文用例名会乱码
#endif
    std::string filter;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--json") {
            g_jsonMode = true;
        } else {
            filter = arg;
        }
    }
    if (listOnly) {
        for (const auto& c : kCases) std::printf("%s\n", c.name);
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& c : kCases) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        g_case = c.name;
        g_reqs.clear();
        const int failedBefore = g_failed;
        const int assertsBefore = g_asserts;
        ++g_cases;
        try {
            c.fn();
        } catch (const std::exception& ex) {
            record(false, std::string("用例抛出异常：") + ex.what(), __FILE__, __LINE__);
        } catch (...) {
            record(false, "用例抛出未知异常", __FILE__, __LINE__);
        }
        const bool ok = (g_failed == failedBefore);
        if (!ok) ++g_casesFailed;
        CaseMeta meta;
        meta.name = c.name;
        meta.reqs = g_reqs;
        meta.asserts = g_asserts - assertsBefore;
        meta.failed = g_failed - failedBefore;
        meta.ok = ok;
        g_metas.push_back(std::move(meta));
        if (!g_jsonMode) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
            std::fflush(stdout);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (g_jsonMode) {
        std::string out = "{\n";
        out += "  \"engineVersion\": \"" + jsonEscape(kEngineVersion) + "\",\n";
        out += "  \"cases\": " + std::to_string(g_cases) + ",\n";
        out += "  \"casesFailed\": " + std::to_string(g_casesFailed) + ",\n";
        out += "  \"asserts\": " + std::to_string(g_asserts) + ",\n";
        out += "  \"assertsFailed\": " + std::to_string(g_failed) + ",\n";
        out += "  \"elapsedMs\": " + std::to_string(ms) + ",\n";
        out += "  \"result\": \"" + std::string(g_failed == 0 ? "ALL GREEN" : "FAILED") + "\",\n";
        out += "  \"details\": [";
        for (std::size_t i = 0; i < g_metas.size(); ++i) {
            const CaseMeta& m = g_metas[i];
            out += (i == 0 ? "\n" : ",\n");
            out += "    {\"name\": \"" + jsonEscape(m.name) + "\", \"ok\": " +
                   (m.ok ? "true" : "false") + ", \"asserts\": " + std::to_string(m.asserts) +
                   ", \"failed\": " + std::to_string(m.failed) + ", \"reqs\": " +
                   jsonArray(m.reqs) + "}";
        }
        out += "\n  ],\n";
        out += "  \"failures\": " + jsonArray(g_failures) + "\n";
        out += "}\n";
        std::fputs(out.c_str(), stdout);
        return g_failed == 0 ? 0 : 1;
    }

    std::printf("\n================ topology selftest ================\n");
    std::printf("用例 %d 个（失败 %d ）｜断言 %d 条（失败 %d ）｜耗时 %.1f ms\n", g_cases,
                g_casesFailed, g_asserts, g_failed, ms);
    if (!g_failures.empty()) {
        std::printf("\n---- 失败明细 ----\n");
        for (const auto& f : g_failures) std::printf("  %s\n", f.c_str());
    }
    std::printf("结果： %s\n", g_failed == 0 ? "ALL GREEN" : "FAILED");
    std::printf("==================================================\n");
    return g_failed == 0 ? 0 : 1;
}
