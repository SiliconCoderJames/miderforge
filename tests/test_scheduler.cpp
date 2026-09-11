// 回归锁：本轮 bug 修复对应的可测逻辑
//
// 覆盖两类历史上真实发生过的缺陷：
//   ① Scheduler 的"待启动任务"查询漏判 scheduled_at IS NULL —— SQL 里 `NULL = 0` 与
//      `NULL <= ?` 都求值为 NULL（非真），导致会话路径创建的任务永远不被调度
//   ② run_command 把成败只写在信封里、不设 *err，导致 ToolRegistry 认为"调用成功"，
//      失败的命令卡片显示 ✓ 且不触发同因失败熔断/升档
#include "core/Scheduler.h"
#include "db/Database.h"
#include "tools/CommandTools.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <doctest/doctest.h>
#include <memory>

using miderforge::Database;
using miderforge::ToolRegistry;

namespace {
struct DbFixture {
    Database db;
    QString dir;
    DbFixture() {
        dir = QStringLiteral("./test-data/sched-%1").arg(QRandomGenerator::global()->generate());
        QDir().mkpath(dir);
        QString err;
        REQUIRE(db.open(dir + QStringLiteral("/t.db"), &err));
    }
    ~DbFixture() {
        db.close();
        QDir(dir).removeRecursively();
    }
};
} // namespace

TEST_CASE("调度查询：scheduled_at 为 NULL 的排队任务必须能被选中（NULL=0 是 NULL 而非真）") {
    DbFixture f;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // 会话路径建的行：scheduled_at 为 NULL（AgentLoop 的 INSERT 不含该列）
    f.db.execute(QStringLiteral(
                     "INSERT INTO tasks(goal, status, created_at) VALUES(?, 'queued', ?)"),
                 {QStringLiteral("会话路径任务"), now});
    // 定时路径建的行：scheduled_at=0（立即）
    f.db.execute(QStringLiteral(
                     "INSERT INTO tasks(goal, status, scheduled_at, created_at) VALUES(?, 'queued', 0, ?)"),
                 {QStringLiteral("立即任务"), now - 100});

    // 与 Scheduler::tick 完全相同的筛选条件
    const auto rows = f.db.query(QStringLiteral(
        "SELECT id, goal FROM tasks WHERE status='queued' "
        "AND (scheduled_at IS NULL OR scheduled_at=0 OR scheduled_at<=?) "
        "ORDER BY created_at ASC, id ASC LIMIT 1"),
        {now});
    REQUIRE(rows.size() == 1);
    // created_at 更早的"立即任务"应先出队
    CHECK(rows.front().value("goal").toString() == QStringLiteral("立即任务"));

    // 若漏掉 `IS NULL` 分支，NULL 行永远选不出来——这正是修复前的缺陷
    const auto nullOnly = f.db.query(QStringLiteral(
        "SELECT COUNT(*) AS n FROM tasks WHERE status='queued' AND (scheduled_at=0 OR scheduled_at<=?)"),
        {now});
    REQUIRE(nullOnly.size() == 1);
    CHECK(nullOnly.front().value("n").toInt() == 1); // 旧条件只能看到 1 条（NULL 那条被漏掉）
}

TEST_CASE("启动恢复：重新入队时把 scheduled_at 的 NULL 归一为 0，避免卡在排队中") {
    DbFixture f;
    f.db.execute(QStringLiteral(
                     "INSERT INTO tasks(goal, status, created_at) VALUES(?, 'running', ?)"),
                 {QStringLiteral("中断的任务"), QDateTime::currentSecsSinceEpoch()});
    // 与 Scheduler 构造函数相同的恢复语句
    f.db.execute(QStringLiteral(
        "UPDATE tasks SET status='queued', scheduled_at=COALESCE(scheduled_at, 0) "
        "WHERE status IN ('running','paused')"), {});
    const auto rows = f.db.query(QStringLiteral(
        "SELECT status, scheduled_at FROM tasks WHERE goal=?"), {QStringLiteral("中断的任务")});
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().value("status").toString() == QStringLiteral("queued"));
    CHECK_FALSE(rows.front().value("scheduled_at").isNull()); // 不再是 NULL
    CHECK(rows.front().value("scheduled_at").toLongLong() == 0);
}

TEST_CASE("run_command：命令失败必须置 *err，让 ExecResult::ok 为 false（否则卡片谎报成功）") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    // cmake 在白名单内；用不存在的目录作为 -S → cmake 必然以非 0 退出
    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\":\"cmake -S Z:/__mf_no_such_dir__ -B Z:/__mf_nb__\"}"));
    CHECK_FALSE(res.ok); // 修复前这里是 true（失败只写在信封里）
    // 关键：失败时正文仍要带上信封（exit_code/output）供模型诊断，
    // ToolRegistry 只前置"错误：<原因>"，不得把信封整段丢弃
    CHECK(res.text.startsWith(QStringLiteral("错误：")));
    const int brace = res.text.indexOf(QLatin1Char('{'));
    REQUIRE(brace > 0);
    const QJsonObject env = QJsonDocument::fromJson(res.text.mid(brace).toUtf8()).object();
    CHECK(env.contains(QStringLiteral("exit_code")));
    CHECK(env.contains(QStringLiteral("output")));
    CHECK(env.value(QStringLiteral("ok")).toBool() == false);
}

TEST_CASE("run_command：成功命令仍为 ok，且信封 ok=true") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\":\"where cmake\"}"));
    // 白名单内的 where 必然成功（cmake 已随构建环境可用）
    CHECK(res.ok);
    const QJsonObject env = QJsonDocument::fromJson(res.text.toUtf8()).object();
    CHECK(env.value(QStringLiteral("ok")).toBool() == true);
    CHECK(env.value(QStringLiteral("exit_code")).toInt() == 0);
}

TEST_CASE("run_command：非白名单命令仍被拒绝（防线未被本次修改削弱）") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\":\"powershell -c whoami\"}"));
    CHECK_FALSE(res.ok);
    // 入口直接拒绝（handler 返回空正文），因此只应看到"错误：命令不在白名单内："说明
    CHECK(res.text.startsWith(QStringLiteral("错误：命令不在白名单内")));
    CHECK_FALSE(res.text.contains(QLatin1Char('{'))); // 没有被执行，故无信封
}
