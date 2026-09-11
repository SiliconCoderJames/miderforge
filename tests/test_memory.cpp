// 记忆系统单测（M2 验收：trigram 中文检索、短词 LIKE 兜底、评分排序、L2 滚动）
#include "db/Database.h"
#include "memory/MemoryManager.h"
#include "tools/ExtraTools.h"
#include "tools/ToolRegistry.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <cmath>
#include <doctest/doctest.h>
#include <memory>

using miderforge::Database;
using miderforge::MemoryManager;
using miderforge::ToolRegistry;
using miderforge::ftsquery::quoteTerm;

// 带工具注册表的夹具：session_search 需要 ToolRegistry + Database + MemoryManager
namespace {
struct ToolFixture {
    Database db;
    std::unique_ptr<MemoryManager> mem;
    std::unique_ptr<ToolRegistry> reg;
    QString dir;
    ToolFixture() {
        dir = QStringLiteral("./test-data/tool-%1").arg(QRandomGenerator::global()->generate());
        QDir().mkpath(dir);
        QString err;
        REQUIRE(db.open(dir + QStringLiteral("/t.db"), &err));
        mem = std::make_unique<MemoryManager>(&db, dir + QStringLiteral("/core.md"));
        reg = std::make_unique<ToolRegistry>();
        miderforge::ExtraTools::registerAll(*reg, &db, mem.get());
    }
    ~ToolFixture() {
        reg.reset();
        mem.reset();
        db.close();
        QDir(dir).removeRecursively();
    }
};
} // namespace

namespace {
// 每个用例独立临时库
struct MemFixture {
    Database db;
    std::unique_ptr<MemoryManager> mem;
    QString dir;
    MemFixture() {
        dir = QStringLiteral("./test-data/mem-%1").arg(QRandomGenerator::global()->generate());
        QDir().mkpath(dir);
        QString err;
        REQUIRE(db.open(dir + QStringLiteral("/t.db"), &err));
        // v1 顺带验证 sqlite-vec 静态加载成功（规格 2：仅加载，不使用）
        CHECK(db.loadVecExtension());
        mem = std::make_unique<MemoryManager>(&db, dir + QStringLiteral("/core.md"));
    }
    ~MemFixture() {
        mem.reset();
        db.close();
        QDir(dir).removeRecursively();
    }
};
} // namespace

TEST_CASE("灌入 100 条中文记忆：搜「编译器」命中「编译器报错修复记录」（trigram）") {
    MemFixture f;
    for (int i = 0; i < 99; ++i)
        f.mem->addMemory(QStringLiteral("project_facts"),
                         QStringLiteral("项目事实 %1：模块 %2 使用 CMake 组织，Qt 版本记录在案。").arg(i).arg(i),
                         0.4);
    f.mem->addMemory(QStringLiteral("task_lesson"),
                     QStringLiteral("编译器报错修复记录：MSVC C4819 与 UTF-8 无 BOM 源文件相关。"),
                     0.8);

    const auto hits = f.mem->retrieve(QStringLiteral("编译器 报错"), 5);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.first().content.contains(QStringLiteral("编译器")));
    // 命中条目 access_count 已回写
    const auto all = f.mem->listAll(QStringLiteral("task_lesson"));
    REQUIRE(all.size() == 1);
    CHECK(all.first().accessCount >= 1);
}

TEST_CASE("搜 2 字词「热键」走 LIKE 兜底仍能命中") {
    MemFixture f;
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("应用支持全局热键 Alt+Space 唤起主窗口。"), 0.7);
    const auto hits = f.mem->retrieve(QStringLiteral("热键"), 5);
    REQUIRE(hits.size() == 1);
    CHECK(hits.first().content.contains(QStringLiteral("热键")));
}

TEST_CASE("LIKE 兜底通配符转义：% 与 _ 按字面匹配（审查 #13 回归）") {
    MemFixture f;
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("构建成功率 100%，今日达标。"), 0.5);
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("构建成功 100 次的运行记录。"), 0.5);
    // 2 字查询走 LIKE 路径；修复前 "0%" 的 % 是通配符，两条都会命中；转义后仅字面含 "0%" 的一条
    const auto hits = f.mem->retrieve(QStringLiteral("0%"), 5);
    REQUIRE(hits.size() == 1);
    CHECK(hits.first().content.contains(QStringLiteral("100%")));

    MemFixture g;
    g.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("字段 x_y 命名规范。"), 0.5);
    g.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral(" xy 组合其他说明。"), 0.5);
    // 修复前 "x_" 的 _ 匹配任意单字符，"xy" 也会被召回；转义后只命中字面 "x_"
    const auto hits2 = g.mem->retrieve(QStringLiteral("x_"), 5);
    REQUIRE(hits2.size() == 1);
    CHECK(hits2.first().content.contains(QStringLiteral("x_y")));
}

TEST_CASE("LIKE 兜底多词 AND：两个短词同时命中才召回") {
    MemFixture f;
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("全局热键绑定主窗口唤起。"), 0.5);
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("窗口关闭行为说明。"), 0.5);
    // 两词都 ≤2 字，走 LIKE 路径；只含其一的记录不应召回（修复前只按最短词匹配）
    const auto hits = f.mem->retrieve(QStringLiteral("热键 全局"), 5);
    REQUIRE(hits.size() == 1);
    CHECK(hits.first().content.contains(QStringLiteral("热键")));
}

TEST_CASE("L2 淘汰评分化：高价值旧摘要不被 FIFO 挤掉（评分保留 hot 席位）") {
    MemFixture f;
    qint64 valuableId = 0;
    for (int i = 0; i < 50; ++i) {
        const qint64 id = f.mem->addSessionSummary(QStringLiteral("普通摘要 %1").arg(i));
        if (i == 0)
            valuableId = id; // 最旧的一条
    }
    // 模拟一条被晋升为高价值（0.9）的旧摘要（实际场景：合并进 L1/被标重视）
    REQUIRE(f.db.execute(QStringLiteral("UPDATE memories SET importance=0.9 WHERE id=?"),
                         {valuableId}));
    // 第 51 条触发淘汰：51 抢 50 个 hot 席位
    f.mem->addSessionSummary(QStringLiteral("新来的第 51 条摘要"));

    const auto all = f.mem->listAll(QStringLiteral("session_summary"));
    REQUIRE(all.size() == 51);
    int hot = 0;
    int demoted = 0;
    bool valuableSurvived = false;
    for (const auto& r : all) {
        if (r.importance > 0.5) {
            ++hot;
            if (r.id == valuableId)
                valuableSurvived = true;
        } else {
            ++demoted;
        }
    }
    CHECK(hot == 50);
    CHECK(demoted == 1);
    // FIFO 会挤掉最旧的第 1 条；评分制下它 importance 最高，被挤掉的应是评分最低的旧条目
    CHECK(valuableSurvived);
}

TEST_CASE("一致性扫描：相似候选对检测与排除规则（矛盾扫描核心）") {
    using miderforge::MemoryManager;
    // 三元组 Dice：完全相同 = 1，无关 ≈ 0
    CHECK(MemoryManager::trigramDice(QStringLiteral("项目用 Meson 组织构建"),
                                     QStringLiteral("项目用 Meson 组织构建")) == doctest::Approx(1.0));
    CHECK(MemoryManager::trigramDice(QStringLiteral("缩进风格空格四宽"),
                                     QStringLiteral("SMTP 邮件服务器授权码")) < 0.35);

    MemFixture f;
    // 完全相同的冗余副本：不报矛盾（冗余 ≠ 矛盾）
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("项目用 Meson 组织构建脚本"), 0.5);
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("项目用 Meson 组织构建脚本"), 0.5);
    CHECK(f.mem->findContradictionCandidates().isEmpty());
}

TEST_CASE("一致性扫描：相似变体命中、无关对排除、归档退出候选") {
    MemFixture f;
    const qint64 a = f.mem->addMemory(QStringLiteral("project_facts"),
                                      QStringLiteral("项目用 Meson 组织构建脚本"), 0.5);
    const qint64 b = f.mem->addMemory(QStringLiteral("project_facts"),
                                      QStringLiteral("项目用 Meson 组织构建脚本，计划下周切换"), 0.5);
    f.mem->addMemory(QStringLiteral("task_lesson"),
                     QStringLiteral("SMTP 邮件通知需要授权码"), 0.8); // 无关对
    f.mem->addSessionSummary(QStringLiteral("项目用 Meson 组织构建脚本的会话摘要")); // 会话摘要不参与

    const auto pairs = f.mem->findContradictionCandidates();
    REQUIRE(pairs.size() == 1);
    CHECK(((pairs.front().idA == a && pairs.front().idB == b)
           || (pairs.front().idA == b && pairs.front().idB == a)));
    CHECK(pairs.front().similarity > 0.35);
    CHECK(pairs.front().similarity < 0.95);

    // 归档一侧后该对退出候选
    CHECK(f.mem->archiveMemory(b));
    CHECK(f.mem->findContradictionCandidates().isEmpty());
}

TEST_CASE("综合评分：高重要度+新近条目排前；归档条目不召回") {
    MemFixture f;
    f.mem->addMemory(QStringLiteral("coding_pref"), QStringLiteral("缩进风格：空格 4 宽。"), 0.9);
    f.mem->addMemory(QStringLiteral("coding_pref"), QStringLiteral("命名风格：成员变量 m_ 前缀。"), 0.3);
    f.mem->addMemory(QStringLiteral("coding_pref"), QStringLiteral("废弃旧条目：缩进用 Tab。"), 0.9);
    const auto all = f.mem->listAll(QStringLiteral("coding_pref"));
    f.mem->archiveMemory(all[2].id);

    const auto hits = f.mem->retrieve(QStringLiteral("缩进"), 5);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.first().content.contains(QStringLiteral("空格")));
    for (const auto& h : hits)
        CHECK_FALSE(h.content.contains(QStringLiteral("Tab"))); // 归档不召回
}

TEST_CASE("L1 核心记忆：保存-读取一致且 UTF-8 中文不乱码") {
    MemFixture f;
    const QString content = QStringLiteral("# 用户画像\n- 偏好深色主题与简洁中文回复");
    CHECK(f.mem->saveL1(content));
    CHECK(f.mem->loadL1() == content);
    CHECK(f.mem->l1Tokens() > 0);
}

TEST_CASE("L2 会话摘要：滚动上限 50 条，最旧降权") {
    MemFixture f;
    for (int i = 0; i < 55; ++i)
        f.mem->addSessionSummary(QStringLiteral("会话摘要 %1：目标-做法-结果-教训。").arg(i));
    const auto recent = f.mem->recentSummaries(5);
    REQUIRE(recent.size() == 5);
    CHECK(recent.first().content.contains(QStringLiteral("54"))); // 最新在前
    // 第 51 条起（最旧的）importance 被降到 0.2
    const auto all = f.mem->listAll(QStringLiteral("session_summary"));
    CHECK(all.size() == 55);
    double minImportance = 1.0;
    for (const auto& r : all)
        minImportance = qMin(minImportance, r.importance);
    CHECK(minImportance < 0.5); // 最旧的 5 条已降权
}

TEST_CASE("updateContent 与 archiveMemory（废弃=标记不物理删）") {
    MemFixture f;
    const qint64 id = f.mem->addMemory(QStringLiteral("project_facts"), QStringLiteral("旧事实"), 0.5);
    CHECK(f.mem->updateContent(id, QStringLiteral("新事实")));
    CHECK(f.mem->archiveMemory(id));
    const auto all = f.mem->listAll(QStringLiteral("project_facts"));
    REQUIRE(all.size() == 1); // 未物理删除
    CHECK(all.first().status == QStringLiteral("archived"));
}

TEST_CASE("一致性失效：filterSupersededIds 编号白名单校验（M4.5 回归）") {
    using miderforge::MemoryManager;
    QJsonObject parsed;
    parsed.insert(QStringLiteral("superseded_memory_ids"),
                  QJsonArray{2, QStringLiteral("3"), 9, QStringLiteral("abc"), 0, 2});

    const auto out = MemoryManager::filterSupersededIds(parsed, {1, 2, 3});
    // 只保留注入清单内的合法编号：字符串数字接受、重复去重、越界(9)/非法(abc)/非正(0)剔除
    REQUIRE(out.size() == 2);
    CHECK(out.contains(2));
    CHECK(out.contains(3));
    // 空字段/空清单
    CHECK(MemoryManager::filterSupersededIds(QJsonObject{}, {1, 2, 3}).isEmpty());
    CHECK(MemoryManager::filterSupersededIds(parsed, {}).isEmpty());
}

TEST_CASE("M5-④ tasks.context_json 迁移列存在且可读写（断点续跑存储）") {
    MemFixture f;
    // 老库/新库迁移后都应带 context_json 列（ALTER TABLE 增量补列）
    REQUIRE(f.db.execute(QStringLiteral(
        "INSERT INTO tasks(goal, status, context_json, created_at) VALUES(?, 'queued', ?, ?)"),
        {QStringLiteral("断点任务"),
         QStringLiteral("{\"history\":[],\"round\":3,\"tier\":2,\"tokens\":1234}"),
         QDateTime::currentSecsSinceEpoch()}));
    const auto rows = f.db.query(QStringLiteral(
        "SELECT context_json FROM tasks WHERE goal=?"), {QStringLiteral("断点任务")});
    REQUIRE(rows.size() == 1);
    const auto ckpt = QJsonDocument::fromJson(
        rows.front().value("context_json").toString().toUtf8()).object();
    CHECK(ckpt.value("round").toInt() == 3);
    CHECK(ckpt.value("tokens").toDouble() == 1234.0);
    // 终态清除断点
    CHECK(f.db.execute(QStringLiteral("UPDATE tasks SET context_json=NULL WHERE goal=?"),
                       {QStringLiteral("断点任务")}));
    const auto rows2 = f.db.query(QStringLiteral(
        "SELECT context_json FROM tasks WHERE goal=?"), {QStringLiteral("断点任务")});
    CHECK(rows2.front().value("context_json").isNull());
}

TEST_CASE("一致性失效：archiveMemories 批量归档且不物理删（M4.5 回归）") {
    MemFixture f;
    const qint64 a = f.mem->addMemory(QStringLiteral("project_facts"), QStringLiteral("项目用 Meson 组织。"), 0.5);
    const qint64 b = f.mem->addMemory(QStringLiteral("project_facts"), QStringLiteral("构建入口在 scripts/。"), 0.5);
    const qint64 c = f.mem->addMemory(QStringLiteral("project_facts"), QStringLiteral("项目已迁移到 CMake。"), 0.6);
    CAPTURE(a);
    CAPTURE(b);
    CAPTURE(c);
    REQUIRE(a != b);
    REQUIRE(b != c);
    REQUIRE(a != c);

    CHECK(f.mem->archiveMemories({a, b}) == 2);
    const auto all = f.mem->listAll(QStringLiteral("project_facts"));
    REQUIRE(all.size() == 3); // 全在（废弃不物理删）
    int archived = 0;
    for (const auto& r : all) {
        if (r.id == a || r.id == b)
            CHECK(r.status == QStringLiteral("archived"));
        if (r.status == QStringLiteral("archived"))
            ++archived;
    }
    CHECK(archived == 2);
    // c 仍是 active：下次检索只有迁移后的事实会命中
    const auto hits = f.mem->retrieve(QStringLiteral("CMake 迁移"), 5);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.first().id == c);
    CHECK(f.mem->archiveMemories({}) == 0); // 空清单安全
}

// ---- M6-A 语义检索 ----

#include "llm/EmbeddingClient.h"
#include <QHash>

namespace {
// 确定性桩嵌入器：按文本查表返回固定向量，未登记的文本给零向量
class StubEmbedder : public miderforge::EmbeddingClient {
public:
    StubEmbedder()
        : EmbeddingClient(Config{QStringLiteral("https://stub.test"), {},
                                 QStringLiteral("stub-embed")}) {}
    bool embed(const QStringList& texts, QVector<QVector<float>>* out, QString*) override {
        for (const auto& t : texts)
            out->push_back(m_map.value(t, QVector<float>{0.f, 0.f, 0.f}));
        return true;
    }
    QHash<QString, QVector<float>> m_map;
};
} // namespace

TEST_CASE("向量序列化与余弦：float32 BLOB 往返、零向量/维度不一致守卫") {
    const QVector<float> v{0.25f, -1.5f, 3.75f, 1e-9f};
    const auto blob = MemoryManager::packVector(v);
    const auto back = MemoryManager::unpackVector(blob);
    REQUIRE(back.size() == 4); // 含 0x00 字节不截断（Database BLOB 分支回归）
    for (int i = 0; i < 4; ++i)
        CHECK(back[i] == doctest::Approx(v[i]));

    CHECK(MemoryManager::cosineSim({1, 0}, {1, 0}) == doctest::Approx(1.0));
    CHECK(MemoryManager::cosineSim({1, 0}, {0, 1}) == doctest::Approx(0.0));
    CHECK(MemoryManager::cosineSim({1, 0}, {-1, 0}) == doctest::Approx(-1.0));
    CHECK(MemoryManager::cosineSim({1, 0}, {0, 0, 0}) != // 零向量→0；维度不一致→NaN
          MemoryManager::cosineSim({1, 0}, {0, 0, 0})); // NaN 自不等
    CHECK(std::isnan(MemoryManager::cosineSim({1, 0}, {1, 0, 0})));
}

TEST_CASE("RRF 融合：双通道名次融合，双通道同时命中的 id 居首") {
    const auto fused = MemoryManager::fuseRRF({1, 2, 3}, {3, 4});
    REQUIRE(fused.size() == 4);
    CHECK(fused[0].first == 3); // 1/(60+0)+1/(60+0) 双通道叠加
    CHECK(fused[1].first == 1); // 1/60
    CHECK(fused[2].first == 2); // 1/61 与 id4 同分，id 升序决胜（输出确定可测）
    CHECK(fused[3].first == 4);
}

TEST_CASE("语义召回：词面不同语义近的记忆经向量通道召回置顶，缺向量渐进补齐") {
    MemFixture f;
    static StubEmbedder stub; // 静态：跨用例存活，规避 doctest 泄漏检查
    for (int i = 0; i < 99; ++i)
        f.mem->addMemory(QStringLiteral("project_facts"),
                         QStringLiteral("项目事实 %1：模块 %2 使用 CMake 组织。").arg(i).arg(i), 0.4);
    const qint64 lesson =
        f.mem->addMemory(QStringLiteral("task_lesson"),
                         QStringLiteral("SQLite 死锁的排查教训：写事务锁等待超时导致任务失败。"), 0.8);
    // 词面完全不含查询词（trigram MATCH 召回不了它）；语义上与查询同向
    stub.m_map[QStringLiteral("SQLite 死锁的排查教训：写事务锁等待超时导致任务失败。")] = {1.f, 0.f};
    stub.m_map[QStringLiteral("上次数据库锁问题的排查")] = {1.f, 0.f};
    f.mem->setEmbedder(&stub);
    // 同秒写入的 updated_at 并列，渐进补齐不保证最新条入选首批：先显式全量补嵌
    CHECK(f.mem->backfillEmbeddings(100) == 100);

    const auto hits = f.mem->retrieve(QStringLiteral("上次数据库锁问题的排查"), 5);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.first().id == lesson);
    // 补嵌入确实落库（float32 BLOB），且 fillers 的零向量/维度不匹配不会污染召回
    const auto all = f.mem->listAll(QStringLiteral("task_lesson"));
    REQUIRE(all.size() == 1);
    CHECK(all.first().accessCount >= 1);
}

TEST_CASE("内容更新即向量失效：embedding 置 NULL 等待重嵌（write-through invalidation）") {
    MemFixture f;
    static StubEmbedder stub;
    stub.m_map[QStringLiteral("旧内容")] = {1.f, 0.f};
    f.mem->setEmbedder(&stub);
    const qint64 id = f.mem->addMemory(QStringLiteral("coding_pref"), QStringLiteral("旧内容"), 0.5);
    CHECK(f.mem->backfillEmbeddings(8) == 1);
    // 落库校验：BLOB 长度 = 维度 × sizeof(float)
    const auto rows1 = f.db.query(
        QStringLiteral("SELECT embedding FROM memories WHERE id=?"), {id});
    CHECK(rows1[0].value("embedding").toByteArray().size() == 2 * int(sizeof(float)));
    // 改内容 → 向量失效
    f.mem->updateContent(id, QStringLiteral("新内容：偏好四空格缩进"));
    const auto rows2 = f.db.query(
        QStringLiteral("SELECT embedding FROM memories WHERE id=?"), {id});
    CHECK(rows2[0].value("embedding").isNull());
}

TEST_CASE("嵌入熔断：连续失败 3 次后本进程停用语义通道，FTS5 主路不受影响") {
    MemFixture f;
    class FailingEmbedder : public miderforge::EmbeddingClient {
    public:
        FailingEmbedder()
            : EmbeddingClient(Config{QStringLiteral("https://stub.test"), {},
                                     QStringLiteral("stub-embed")}) {}
        bool embed(const QStringList&, QVector<QVector<float>>*, QString* err) override {
            ++calls;
            if (err)
                *err = QStringLiteral("网络不可达（模拟）");
            return false;
        }
        int calls = 0;
    };
    auto stub = std::make_unique<FailingEmbedder>();
    auto* stubPtr = stub.get();
    f.mem->setEmbedder(stubPtr);
    // 一条词面可召回的记录 + 一条只有语义才能召回的记录
    const qint64 ftsHit = f.mem->addMemory(QStringLiteral("task_lesson"),
        QStringLiteral("CMake 生成器表达式 learning：生成器表达式在 add_custom_command 里求值时机。"), 0.7);
    f.mem->addMemory(QStringLiteral("task_lesson"),
        QStringLiteral("SQLite 死锁的排查教训：写事务锁等待超时导致任务失败。"), 0.8);

    // 第 1 次 retrieve：backfill 失败(+1) + 查询嵌入失败(+1) = 2 次
    const auto h1 = f.mem->retrieve(QStringLiteral("生成器表达式"), 5);
    CHECK(stubPtr->calls == 2);
    REQUIRE_FALSE(h1.empty());
    CHECK(h1.first().id == ftsHit); // FTS 主路照常
    // 第 2 次：+2 → 4 次 ≥3 熔断；之后不再调用嵌入
    (void)f.mem->retrieve(QStringLiteral("生成器表达式"), 5);
    CHECK(stubPtr->calls == 4);
    (void)f.mem->retrieve(QStringLiteral("生成器表达式"), 5);
    (void)f.mem->retrieve(QStringLiteral("生成器表达式"), 5);
    CHECK(stubPtr->calls == 4); // 熔断后嵌入零调用
    const auto h3 = f.mem->retrieve(QStringLiteral("生成器表达式"), 5);
    CHECK(stubPtr->calls == 4);
    REQUIRE_FALSE(h3.empty());
    CHECK(h3.first().id == ftsHit);
    (void)stub.release(); // 进程级测试，交还 OS
}

// ============ 回归：L1 自动改写闸门（曾因基准只在内存里而永久失效）============
// 旧实现把"Agent 上次写入"记在成员变量 m_lastAgentWrite，进程重启即丢失；
// 而 core.md 只在首次为空时被写过一次 → 之后每次启动都判成"用户手建"，
// l1UserEditedRecently() 恒为 true，L1 提炼式改写从未真正运行过。

TEST_CASE("L1 闸门：Agent 自己写入登记了跨进程基准，重启后仍允许继续改写") {
    MemFixture f;
    const QString l1 = QStringLiteral("# 用户画像\n- 偏好深色主题");
    CHECK(f.mem->saveL1(l1));
    CHECK_FALSE(f.mem->l1UserEditedRecently()); // 自己写的 → 允许改写

    // 模拟进程重启：新实例装载同一 core.md 与同一状态文件
    {
        MemoryManager reopened(&f.db, f.dir + QStringLiteral("/core.md"));
        CHECK_FALSE(reopened.l1UserEditedRecently()); // 旧实现这里恒为 true（F1 根因）
        CHECK(reopened.loadL1() == l1);
        // 重启后仍能完成一次改写
        CHECK(reopened.saveL1(QStringLiteral("# 用户画像\n- 偏好深色主题\n- 用中文回复")));
    }
    CHECK_FALSE(f.mem->l1UserEditedRecently());
}

TEST_CASE("L1 闸门：检测到外部手改则拒绝改写；内容改回基准后恢复允许") {
    MemFixture f;
    const QString l1 = QStringLiteral("# 用户画像\n- 偏好深色主题");
    REQUIRE(f.mem->saveL1(l1));

    // 模拟用户直接编辑 core.md（不经 saveL1，故不更新基准）
    {
        QFile w(f.dir + QStringLiteral("/core.md"));
        REQUIRE(w.open(QIODevice::WriteOnly | QIODevice::Truncate));
        w.write(QStringLiteral("# 用户画像\n- 用户手写的内容，不许覆盖").toUtf8());
    }
    CHECK(f.mem->l1UserEditedRecently()); // 内容变了 → 保护

    // 内容恢复成基准 → 视为无人改动，重新允许改写
    REQUIRE(f.mem->saveL1(l1));
    CHECK_FALSE(f.mem->l1UserEditedRecently());
}

TEST_CASE("L1 闸门：无基准状态文件（旧版本遗留 core.md）时保守保护，保存一次即建立基准") {
    MemFixture f;
    // 只有 core.md，没有状态文件 —— 等价于从旧版本升级上来的用户
    {
        QFile w(f.dir + QStringLiteral("/core.md"));
        REQUIRE(w.open(QIODevice::WriteOnly | QIODevice::Truncate));
        w.write(QStringLiteral("# 用户画像\n- 升级前遗留内容").toUtf8());
    }
    CHECK(f.mem->l1UserEditedRecently()); // 无法确证 → 保护（宁可漏改写，不可覆盖用户内容）
    REQUIRE(f.mem->saveL1(QStringLiteral("# 用户画像\n- 用户确认过的内容")));
    CHECK_FALSE(f.mem->l1UserEditedRecently()); // 保存建立基准后恢复自动改写
}

TEST_CASE("L1 闸门：文件不存在时不保护（允许首次建立）") {
    MemFixture f;
    CHECK_FALSE(f.mem->l1UserEditedRecently());
}

// ============ 回归：FTS5 MATCH 查询词必须引号包裹 ============
// 裸绑用户文本会被 FTS5 当查询语法解析，C++/CMake 项目最常见的词元全部报错，
// 而 Database::query 只循环 while(step==SQLITE_ROW)，错误被静默吞掉 → 零召回无任何提示。

TEST_CASE("FTS5 词元转义：引号包裹并转义内部双引号") {
    CHECK(quoteTerm(QStringLiteral("构建")) == QStringLiteral("\"构建\""));
    CHECK(quoteTerm(QStringLiteral("  CMakeLists.txt  ")) == QStringLiteral("\"CMakeLists.txt\""));
    CHECK(quoteTerm(QStringLiteral("a\"b")) == QStringLiteral("\"a\"\"b\"")); // 内部 " 翻倍
    CHECK(quoteTerm(QStringLiteral("   ")).isEmpty());
    CHECK(quoteTerm(QString()).isEmpty());
}

TEST_CASE("FTS5 检索：代码标识符（点/加号/双冒号/连字符）能被正确召回") {
    MemFixture f;
    f.mem->addMemory(QStringLiteral("project_facts"),
                     QStringLiteral("本项目用 CMakeLists.txt 配置 Qt6::Widgets，语言标准 C++20，源码 utf-8。"),
                     0.8);
    // 这些词元在旧实现下会抛 fts5 语法错误（syntax error near "."/"+"、no such column）
    for (const char* term : {"CMakeLists.txt", "C++20", "Qt6::Widgets", "utf-8"}) {
        const auto hits = f.mem->retrieve(QString::fromLatin1(term), 5);
        CHECK_MESSAGE(!hits.empty(), "term=" << term);
    }
    // 中文路径照常
    CHECK_FALSE(f.mem->retrieve(QStringLiteral("语言标准"), 5).empty());
}

TEST_CASE("session_search：能真正召回历史消息（snippet 列号曾越界导致永远空结果）") {
    ToolFixture f;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    f.db.execute(QStringLiteral("INSERT INTO sessions(title, created_at, updated_at) VALUES(?,?,?)"),
                 {QStringLiteral("检索重构"), now, now});
    f.db.execute(QStringLiteral(
                     "INSERT INTO messages(session_id, role, content, ts) VALUES(1,'user',?,?)"),
                 {QStringLiteral("请帮我重构记忆模块的检索逻辑"), now});
    f.db.execute(QStringLiteral(
                     "INSERT INTO messages(session_id, role, content, ts) VALUES(1,'assistant',?,?)"),
                 {QStringLiteral("好的，我来重构记忆模块的检索"), now + 1});

    const auto res = f.reg->execute(QStringLiteral("session_search"),
                                    QStringLiteral("{\"query\":\"记忆模块\"}"));
    REQUIRE(res.ok);
    const QJsonObject obj = QJsonDocument::fromJson(res.text.toUtf8()).object();
    const QJsonArray hits = obj.value(QStringLiteral("hits")).toArray();
    CHECK_MESSAGE(hits.size() == 2, "hits=" << hits.size() << " text=" << res.text.toStdString());
    if (!hits.isEmpty()) {
        // snippet 命中高亮：命中词被 [ ] 包裹
        bool highlighted = false;
        for (const auto& h : hits)
            if (h.toObject().value(QStringLiteral("snippet")).toString().contains(QLatin1Char('[')))
                highlighted = true;
        CHECK(highlighted);
    }
    // 带语法字符的查询词不应报错（旧实现直接抛 fts5 syntax error）
    const auto res2 = f.reg->execute(QStringLiteral("session_search"),
                                     QStringLiteral("{\"query\":\"CMakeLists.txt\"}"));
    CHECK(res2.ok);
    CHECK(QJsonDocument::fromJson(res2.text.toUtf8()).object().value(QStringLiteral("hits")).toArray().isEmpty());
}
