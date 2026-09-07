// 事件总线（append-only 审计流）：代码中任何路径禁止 UPDATE/DELETE；
// M1 落地为 JSONL 追加文件 + 内存环形通知，M2 起同步写入 SQLite events 表
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

namespace miderforge {

class Database;

class EventBus : public QObject {
    Q_OBJECT
public:
    struct Event {
        qint64 ts = 0; // unix 秒（规格：SQLite 时间统一存 unix 秒）
        QString type;  // llm_request|tool_call|tool_result|permission_grant|permission_deny|
                       // circuit_break|tier_escalate|provider_switch|skill_gen|skill_used|
                       // memory_update|task_status|email_sent|error
        qint64 taskId = -1;
        QJsonObject payload;
    };

    explicit EventBus(const QString& jsonlPath, QObject* parent = nullptr);

    // M2：双写 SQLite events 表（append-only）；未设置则只写 JSONL
    void setDatabase(Database* db) { m_db = db; }

    // 追加一条事件：写 JSONL（+数据库）+ 发 eventAppended；失败不致命（日志降级）
    void append(const QString& type, qint64 taskId, const QJsonObject& payload);

    // 启动时从 JSONL 回读（审计日志视图数据源；封顶最近 maxKeep 条）
    QVector<Event> loadAll(int maxKeep = 2000) const;

    const QVector<Event>& recentEvents() const { return m_recent; }

signals:
    void eventAppended(const miderforge::EventBus::Event& event);

private:
    QString m_path;
    QVector<Event> m_recent; // 内存环形（审计视图直读）
    Database* m_db = nullptr;
};

} // namespace miderforge

Q_DECLARE_METATYPE(miderforge::EventBus::Event)
