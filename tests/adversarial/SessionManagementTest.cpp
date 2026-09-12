// 会话管理验收（重命名 / 归档 / 取消归档）：断言落在数据库物理结果上，
// 且使用 core/SessionQueries.h 里 UI 实际调用的同一条 SQL（契约共用，杜绝假验证）。
#include "core/SessionQueries.h"
#include "db/Database.h"
#include <QFile>
#include <QTemporaryDir>
#include <QVariantMap>
#include <doctest/doctest.h>
#include <vector>

using miderforge::Database;

namespace {

qint64 insertSession(Database& db, const QString& title, qint64 updatedAt) {
    db.execute(QStringLiteral("INSERT INTO sessions(title, created_at, updated_at) VALUES(?,?,?)"),
               {title, updatedAt, updatedAt});
    const auto rows = db.query(QStringLiteral("SELECT id FROM sessions WHERE title=?"), {title});
    return rows.empty() ? -1 : rows.front().value(QStringLiteral("id")).toLongLong();
}

QStringList titlesOf(const std::vector<QVariantMap>& rows) {
    QStringList out;
    for (const auto& r : rows)
        out << r.value(QStringLiteral("title")).toString();
    return out;
}

} // namespace

TEST_SUITE("acceptance.session_management")
{

TEST_CASE("schema：sessions 表带 archived_at（新建库即有；老库由 migrate 补列）") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    Database db;
    REQUIRE(db.open(tmp.path() + QStringLiteral("/s.db")));
    const auto cols = db.query(QStringLiteral("PRAGMA table_info(sessions)"), {});
    bool hasArchived = false;
    for (const auto& c : cols)
        if (c.value(QStringLiteral("name")).toString() == QStringLiteral("archived_at"))
            hasArchived = true;
    CHECK(hasArchived);
}

TEST_CASE("迁移：老库（缺 archived_at）重新 open 时自动补列且不丢数据") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/old.db");
    {
        Database db;
        REQUIRE(db.open(path));
        REQUIRE(insertSession(db, QStringLiteral("老库会话"), 500) > 0);
        // 模拟迁移前的老库形态：把后补列删掉（SQLite 3.35+ 支持 DROP COLUMN）
        db.execute(QStringLiteral("ALTER TABLE sessions DROP COLUMN archived_at"), {});
        const auto cols = db.query(QStringLiteral("PRAGMA table_info(sessions)"), {});
        bool gone = true;
        for (const auto& c : cols)
            if (c.value(QStringLiteral("name")).toString() == QStringLiteral("archived_at"))
                gone = false;
        REQUIRE(gone); // 前提成立：此刻确实没有该列
    }
    {
        Database db;
        REQUIRE(db.open(path)); // open → migrate 应把列补回来
        const auto cols = db.query(QStringLiteral("PRAGMA table_info(sessions)"), {});
        bool hasArchived = false;
        for (const auto& c : cols)
            if (c.value(QStringLiteral("name")).toString() == QStringLiteral("archived_at"))
                hasArchived = true;
        CHECK(hasArchived);
        // 老数据没丢，且默认列表仍能列出
        const auto rows = db.query(miderforge::sessionq::listSql(false), {});
        CHECK(titlesOf(rows) == QStringList({QStringLiteral("老库会话")}));
        // 补列后归档功能可用
        const qint64 id = rows.front().value(QStringLiteral("id")).toLongLong();
        db.execute(miderforge::sessionq::archiveSql(), {id});
        CHECK(db.query(miderforge::sessionq::listSql(false), {}).empty());
    }
}

TEST_CASE("重命名：标题落库且 updated_at 不变（改名不该把会话顶到列表最前）") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    Database db;
    REQUIRE(db.open(tmp.path() + QStringLiteral("/s.db")));
    const qint64 id = insertSession(db, QStringLiteral("旧标题"), 1000);
    REQUIRE(id > 0);

    db.execute(miderforge::sessionq::renameSql(), {QStringLiteral("新标题"), id});
    const auto rows = db.query(QStringLiteral("SELECT title, updated_at FROM sessions WHERE id=?"),
                               {id});
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().value(QStringLiteral("title")).toString() == QStringLiteral("新标题"));
    CHECK(rows.front().value(QStringLiteral("updated_at")).toLongLong() == 1000);

    // 重命名对话框的旧标题来源：库读（按 id），不依赖列表项
    const auto t = db.query(miderforge::sessionq::titleSql(), {id});
    REQUIRE(t.size() == 1);
    CHECK(t.front().value(QStringLiteral("title")).toString() == QStringLiteral("新标题"));
}

TEST_CASE("归档：默认列表隐藏归档项，显示归档时排在活动项之后且带 archived_at") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    Database db;
    REQUIRE(db.open(tmp.path() + QStringLiteral("/s.db")));
    const qint64 keep = insertSession(db, QStringLiteral("活动会话"), 2000);
    const qint64 hide = insertSession(db, QStringLiteral("待归档会话"), 3000); // 更新时间更晚
    REQUIRE(keep > 0);
    REQUIRE(hide > 0);

    // 归档前：两条都在，且 updated_at 新的排前面
    auto rows = db.query(miderforge::sessionq::listSql(false), {});
    CHECK(titlesOf(rows) == QStringList({QStringLiteral("待归档会话"), QStringLiteral("活动会话")}));

    db.execute(miderforge::sessionq::archiveSql(), {hide});

    // ① 默认列表：归档项消失
    rows = db.query(miderforge::sessionq::listSql(false), {});
    CHECK(titlesOf(rows) == QStringList({QStringLiteral("活动会话")}));

    // ② 显示归档：两条都在，归档项排后且带时间戳
    rows = db.query(miderforge::sessionq::listSql(true), {});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].value(QStringLiteral("title")).toString() == QStringLiteral("活动会话"));
    CHECK(rows[1].value(QStringLiteral("title")).toString() == QStringLiteral("待归档会话"));
    CHECK(rows[0].value(QStringLiteral("archived_at")).toLongLong() == 0);
    CHECK(rows[1].value(QStringLiteral("archived_at")).toLongLong() > 0);

    // ③ 数据没被删：消息与索引仍在（归档 ≠ 删除）
    const auto all = db.query(QStringLiteral("SELECT COUNT(*) n FROM sessions"), {});
    CHECK(all.front().value(QStringLiteral("n")).toLongLong() == 2);

    // ④ 取消归档：回到默认列表
    db.execute(miderforge::sessionq::unarchiveSql(), {hide});
    rows = db.query(miderforge::sessionq::listSql(false), {});
    CHECK(titlesOf(rows) == QStringList({QStringLiteral("待归档会话"), QStringLiteral("活动会话")}));
}

} // TEST_SUITE
