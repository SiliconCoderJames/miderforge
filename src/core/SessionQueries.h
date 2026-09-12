// 会话管理查询契约（纯字符串，可单测）：侧栏 UI 与验收测试共用同一份 SQL。
// 意义：界面用的 SQL 与测试断言的 SQL 是同一条——否则"测试绿了但界面走的是另一条语句"
// 就是假验证（本仓库在 session_search 列号越界事故上吃过这个亏）。
#pragma once
#include <QDateTime>
#include <QString>

namespace miderforge::sessionq {

// 会话列表：默认隐藏已归档；显示归档时归档项排在活动项之后，并带出 archived_at 供 UI 打标
inline QString listSql(bool showArchived) {
    return showArchived
               ? QStringLiteral("SELECT id, title, updated_at, archived_at FROM sessions "
                                "ORDER BY (archived_at IS NOT NULL), updated_at DESC LIMIT 200")
               : QStringLiteral("SELECT id, title, updated_at FROM sessions "
                                "WHERE archived_at IS NULL ORDER BY updated_at DESC LIMIT 100");
}

// 归档 = 打时间戳（不删数据，消息与 FTS 索引原样保留；取消归档即可复原）
inline QString archiveSql() {
    return QStringLiteral("UPDATE sessions SET archived_at=strftime('%s','now') WHERE id=?");
}
inline QString unarchiveSql() {
    return QStringLiteral("UPDATE sessions SET archived_at=NULL WHERE id=?");
}

// 重命名：只改标题，不动 updated_at（否则改个名字就把会话顶到列表最前，排序被搅乱）
inline QString renameSql() {
    return QStringLiteral("UPDATE sessions SET title=? WHERE id=?");
}

// 重命名对话框的权威旧标题：从库读，不依赖列表项指针——
// 单击切换会话会触发 loadSessions()→clear()，双击第二击时列表项可能已被重建销毁
inline QString titleSql() {
    return QStringLiteral("SELECT title FROM sessions WHERE id=?");
}

// 会话行的时间显示：相对时间比裸日期更好读（"今天 14:32" 一眼知道新旧）。
// 纯函数，可单测；now 由调用方传入便于测试。
inline QString relativeTime(qint64 epochSec, qint64 nowSec) {
    if (epochSec <= 0)
        return QString();
    const QDateTime then = QDateTime::fromSecsSinceEpoch(epochSec);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(nowSec);
    const qint64 days = then.date().daysTo(now.date());
    if (days == 0)
        return QStringLiteral("今天 %1").arg(then.toString(QStringLiteral("HH:mm")));
    if (days == 1)
        return QStringLiteral("昨天 %1").arg(then.toString(QStringLiteral("HH:mm")));
    if (days < 7)
        return QStringLiteral("%1 天前").arg(days);
    if (then.date().year() == now.date().year())
        return then.toString(QStringLiteral("MM-dd"));
    return then.toString(QStringLiteral("yyyy-MM-dd"));
}

} // namespace miderforge::sessionq
