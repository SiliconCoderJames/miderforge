// 记忆系统单测（M2 验收：trigram 中文检索、短词 LIKE 兜底、评分排序、L2 滚动）
#include "db/Database.h"
#include "memory/MemoryManager.h"
#include <QDir>
#include <QFile>
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
