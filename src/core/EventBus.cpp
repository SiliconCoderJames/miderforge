// 事件总线实现
#include "core/EventBus.h"
#include "db/Database.h"
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>

namespace miderforge {

namespace {
constexpr int kMaxInMemory = 2000;
static QMutex s_mutex; // append 可能来自任意线程（工具在嵌套循环/工作线程执行）
} // namespace

EventBus::EventBus(const QString& jsonlPath, QObject* parent) : QObject(parent), m_path(jsonlPath) {}

void EventBus::append(const QString& type, qint64 taskId, const QJsonObject& payload) {
    Event ev;
    ev.ts = QDateTime::currentSecsSinceEpoch();
    ev.type = type;
    ev.taskId = taskId;
    ev.payload = payload;

    QJsonObject record = payload;
    record.insert("ts", ev.ts);
    record.insert("type", type);
    if (taskId >= 0)
        record.insert("task_id", taskId);
    const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);

    QMutexLocker locker(&s_mutex);
    QFile f(m_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        f.write(line + QByteArrayLiteral("\n"));
    // M2：同步双写 events 表（append-only 审计；失败仅记日志不致命）
    if (m_db) {
        m_db->execute(QStringLiteral("INSERT INTO events(ts, type, task_id, payload_json) VALUES(?,?,?,?)"),
                      {ev.ts, ev.type, ev.taskId, QString::fromUtf8(line)});
    }
    m_recent.push_back(ev);
    if (m_recent.size() > kMaxInMemory)
        m_recent.remove(0, m_recent.size() - kMaxInMemory);
    locker.unlock();

    emit eventAppended(ev);
}

QVector<EventBus::Event> EventBus::loadAll(int maxKeep) const {
    QVector<Event> out;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        Event ev;
        ev.ts = obj.value("ts").toDouble();
        ev.type = obj.value("type").toString();
        ev.taskId = obj.value("task_id").toInt(-1);
        ev.payload = obj;
        out.push_back(ev);
    }
    if (out.size() > maxKeep)
        out.remove(0, out.size() - maxKeep);
    return out;
}

} // namespace miderforge
