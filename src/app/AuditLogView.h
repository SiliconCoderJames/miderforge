// 审计日志视图（规格 4.2 导航项）：events 追加流的只读展示——时间/类型/任务ID/载荷 JSON；
// 数据源为 EventBus（M1 JSONL，M2 切换 SQLite events 表），append-only 不可删改
#pragma once
#include "core/EventBus.h"
#include <QTableWidget>
#include <QWidget>

namespace miderforge {

class AuditLogView : public QWidget {
    Q_OBJECT
public:
    explicit AuditLogView(EventBus* events, QWidget* parent = nullptr);

public slots:
    void reload(); // 全量刷新（来自 JSONL/库）

private:
    void appendRow(const EventBus::Event& ev);

    EventBus* m_events = nullptr;
    QTableWidget* m_table = nullptr;
};

} // namespace miderforge
