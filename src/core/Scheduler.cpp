// 任务调度器实现
#include "core/Scheduler.h"
#include "core/AgentLoop.h"
#include "db/Database.h"
#include "util/Log.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <spdlog/spdlog.h>

namespace miderforge {

Scheduler::Scheduler(Database* db, AgentLoop* loop, QObject* parent)
    : QObject(parent), m_db(db), m_loop(loop) {
    connect(&m_timer, &QTimer::timeout, this, &Scheduler::onTick);
    m_timer.start(20 * 1000); // 20 秒轮询（定时任务到点即启）
    connect(m_loop, &AgentLoop::loopFinished, this, &Scheduler::onLoopFinished);

    // 崩溃恢复：上一会话中断遗留的 running / paused 任务重新入队。
    // paused 行的内存状态（history）已随进程消失，挂起不可续 → 重新排队；
    // 否则 tick 只挑 queued，幽灵 running 会永久卡死（任务表与内存状态分裂的后果）
    const auto stale = m_db->query(
        QStringLiteral("SELECT id FROM tasks WHERE status IN ('running','paused')"), {});
    if (!stale.empty()) {
        m_db->execute(QStringLiteral(
            "UPDATE tasks SET status='queued' WHERE status IN ('running','paused')"), {});
        if (auto lg = logutil::logger())
            lg->warn("启动恢复：{} 个中断/挂起任务已重新入队", stale.size());
    }
}

void Scheduler::tick() {
    if (m_loop->isRunning())
        return;
    // 到点/待启动的队列任务：scheduled_at=0(立即) 或 <=now；context_json 为 M5-④ 断点（可能为空）
    const auto rows = m_db->query(QStringLiteral(
        "SELECT id, goal, context_json FROM tasks WHERE status='queued' AND (scheduled_at=0 OR scheduled_at<=?) "
        "ORDER BY created_at ASC, id ASC LIMIT 1"),
        {QDateTime::currentSecsSinceEpoch()});
    if (rows.empty())
        return;
    const qint64 id = rows.front().value("id").toLongLong();
    const QString goal = rows.front().value("goal").toString();
    // 标记 running 后启动（抢占式：仅当仍为 queued 时生效）
    m_db->execute(QStringLiteral("UPDATE tasks SET status='running' WHERE id=? AND status='queued'"), {id});
    const auto check = m_db->query(QStringLiteral("SELECT goal FROM tasks WHERE id=? AND status='running'"), {id});
    if (check.empty())
        return; // 未抢占成功（并发/已取消）
    m_runningTaskId = id;
    if (auto lg = logutil::logger())
        lg->info("调度器启动任务 #{}", id);
    // M5-④：携带断点启动（无断点时为空对象 = 全新开始）
    const QString ctx = rows.front().value("context_json").toString();
    const QJsonObject checkpoint = QJsonDocument::fromJson(ctx.toUtf8()).object();
    m_loop->startWithCheckpoint(goal, id, checkpoint);
}

void Scheduler::onLoopFinished(bool ok, const QString& summary) {
    (void)summary;
    const qint64 id = m_runningTaskId;
    m_runningTaskId = -1;
    if (id < 0)
        return;
    // 每日重复：完成/失败后重新排到明天同一时刻
    const auto rows = m_db->query(QStringLiteral("SELECT repeat_daily FROM tasks WHERE id=?"), {id});
    if (!rows.empty() && rows.front().value("repeat_daily").toInt() == 1) {
        m_db->execute(QStringLiteral(
            "INSERT INTO tasks(goal, status, scheduled_at, repeat_daily, created_at) "
            "SELECT goal, 'queued', ?, 1, ? FROM tasks WHERE id=?"),
            {QDateTime::currentDateTime().addDays(1).toSecsSinceEpoch(),
             QDateTime::currentSecsSinceEpoch(), id});
    }
    (void)ok;
    // 立即衔接下一个排队任务
    QTimer::singleShot(0, this, [this] { tick(); });
}

} // namespace miderforge
