// 会话管理查询契约（纯字符串，可单测）：侧栏 UI 与验收测试共用同一份 SQL。
// 意义：界面用的 SQL 与测试断言的 SQL 是同一条——否则"测试绿了但界面走的是另一条语句"
// 就是假验证（本仓库在 session_search 列号越界事故上吃过这个亏）。
#pragma once
#include <QDateTime>
#include <QString>

namespace miderforge::sessionq {

// 会话列表排序：**置顶在前** → 活动 → 已归档，同组内按更新时间倒序。
// 默认隐藏归档项；显示归档时一并带出 archived_at / pinned_at 供 UI 打标与行内按钮定态。
inline QString listSql(bool showArchived) {
    const QString base =
        QStringLiteral("SELECT id, title, updated_at, archived_at, pinned_at FROM sessions ");
    const QString order = QStringLiteral("ORDER BY (pinned_at IS NOT NULL) DESC, "
                                         "(archived_at IS NOT NULL), updated_at DESC ");
    return showArchived ? base + order + QStringLiteral("LIMIT 200")
                        : base + QStringLiteral("WHERE archived_at IS NULL ") + order
                              + QStringLiteral("LIMIT 100");
}

// 归档 = 打时间戳（不删数据，消息与 FTS 索引原样保留；取消归档即可复原）
inline QString archiveSql() {
    return QStringLiteral("UPDATE sessions SET archived_at=strftime('%s','now') WHERE id=?");
}
inline QString unarchiveSql() {
    return QStringLiteral("UPDATE sessions SET archived_at=NULL WHERE id=?");
}

// 置顶 = 打时间戳（不改 updated_at：置顶不等于"最近使用过"，排序由 pinned_at 单独承载）
inline QString pinSql() {
    return QStringLiteral("UPDATE sessions SET pinned_at=strftime('%s','now') WHERE id=?");
}
inline QString unpinSql() {
    return QStringLiteral("UPDATE sessions SET pinned_at=NULL WHERE id=?");
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
