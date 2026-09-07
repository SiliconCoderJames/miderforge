// 任务调度器：轮询 tasks 表，按计划时间/入队顺序启动任务；每日重复任务完成后重新排队（M4 定时任务基础）
#pragma once
#include <QObject>
#include <QTimer>

namespace miderforge {

class AgentLoop;
class Database;

class Scheduler : public QObject {
    Q_OBJECT
public:
    Scheduler(Database* db, AgentLoop* loop, QObject* parent = nullptr);
    // 立即扫一轮（UI 可手动触发）
    void tick();

private slots:
    void onTick() { tick(); }
    void onLoopFinished(bool ok, const QString& summary);

private:
    void startNextQueued();

    Database* m_db = nullptr;
    AgentLoop* m_loop = nullptr;
    QTimer m_timer;
    qint64 m_runningTaskId = -1;
};

} // namespace miderforge
