// 记忆系统单测（M2 验收：trigram 中文检索、短词 LIKE 兜底、评分排序、L2 滚动）
#include "db/Database.h"
#include "memory/MemoryManager.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <doctest/doctest.h>
#include <memory>

using miderforge::Database;
using miderforge::MemoryManager;

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
