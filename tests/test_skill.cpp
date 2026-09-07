// 技能库单测（M3）：frontmatter 解析/序列化、同名 merge version+1 保留统计、检索、降权标记
#include "db/Database.h"
#include "skills/SkillManager.h"
#include <QDir>
#include <QRandomGenerator>
#include <doctest/doctest.h>
#include <memory>

using miderforge::Database;
using miderforge::SkillManager;

namespace {
struct SkillFixture {
    Database db;
    std::unique_ptr<SkillManager> skills;
    QString dir;
    SkillFixture() {
        dir = QStringLiteral("./test-data/skill-%1").arg(QRandomGenerator::global()->generate());
        QDir().mkpath(dir);
        QString err;
        REQUIRE(db.open(dir + QStringLiteral("/t.db"), &err));
        skills = std::make_unique<SkillManager>(&db, dir + QStringLiteral("/skills"));
    }
    ~SkillFixture() {
        skills.reset();
        db.close();
        QDir(dir).removeRecursively();
    }
};
} // namespace

TEST_CASE("SKILL.md 序列化-解析往返") {
    const QString md = SkillManager::serialize(QStringLiteral("csv-export"),
                                               QStringLiteral("导出带 BOM 的 UTF-8 CSV"), 3,
                                               QStringLiteral("## 步骤\n1. xxx\n"));
    QString name, desc, body;
    int version = 0;
    REQUIRE(SkillManager::parseFrontmatter(md, &name, &desc, &version, &body));
    CHECK(name == QString("csv-export"));
    CHECK(desc == QString("导出带 BOM 的 UTF-8 CSV"));
    CHECK(version == 3);
    CHECK(body.startsWith(QString("## 步骤")));
}

TEST_CASE("同名 merge：version+1 且保留累计统计") {
    SkillFixture f;
    REQUIRE(f.skills->writeSkill(QStringLiteral("csv-export"), QStringLiteral("描述一"),
                                 QStringLiteral("正文一")));
    f.skills->recordUsage(QStringLiteral("csv-export"), true, 6);
    REQUIRE(f.skills->writeSkill(QStringLiteral("csv-export"), QStringLiteral("描述二"),
                                 QStringLiteral("正文二")));
    const SkillManager::SkillMeta s = f.skills->find(QStringLiteral("csv-export"));
    CHECK(s.id > 0);
    CHECK(s.version == 2);       // version+1
    CHECK(s.usageCount == 1);    // 统计保留
    CHECK(s.successCount == 1);
    CHECK(s.description == QString("描述二")); // 描述更新
}

TEST_CASE("检索命中与手动新建技能") {
    SkillFixture f;
    REQUIRE(f.skills->writeSkill(QStringLiteral("utf8-csv-export"),
                                 QStringLiteral("表格内容导出为带 BOM 的 UTF-8 CSV"),
                                 QStringLiteral("## 适用场景\nCSV 导出\n")));
    const auto hits = f.skills->search(QStringLiteral("CSV"), 5);
    REQUIRE(hits.size() == 1);
    CHECK(hits.first().name == QString("utf8-csv-export"));
    // 全文可加载
    CHECK(f.skills->loadSkillMd(QStringLiteral("utf8-csv-export")).contains(QString("CSV")));
}

TEST_CASE("成功率<30% 且使用≥5 次 → 自动标记待审查") {
    SkillFixture f;
    REQUIRE(f.skills->writeSkill(QStringLiteral("flaky-skill"), QStringLiteral("不稳定技能"),
                                 QStringLiteral("正文")));
    for (int i = 0; i < 5; ++i)
        f.skills->recordUsage(QStringLiteral("flaky-skill"), i == 0, 4); // 1 成 4 败 = 20%
    CHECK(f.skills->find(QStringLiteral("flaky-skill")).status == QString("review"));
}

TEST_CASE("标记废弃=状态标记，不物理删除") {
    SkillFixture f;
    REQUIRE(f.skills->writeSkill(QStringLiteral("old-skill"), QStringLiteral("旧技能"),
                                 QStringLiteral("正文")));
    CHECK(f.skills->markDeprecated(QStringLiteral("old-skill")));
    const SkillManager::SkillMeta s = f.skills->find(QStringLiteral("old-skill"));
    CHECK(s.id > 0);
    CHECK(s.status == QString("deprecated"));
}
