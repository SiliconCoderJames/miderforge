// 审计日志视图实现
#include "app/AuditLogView.h"
#include "app/Theme.h"
#include <QDateTime>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace miderforge {

namespace {
QString typeColor(const QString& type) {
    if (type.startsWith(QStringLiteral("permission_deny")) || type == QLatin1String("error")
        || type == QLatin1String("circuit_break"))
        return theme::colors::error().name();
    if (type.startsWith(QStringLiteral("permission_grant")))
        return theme::colors::warn().name();
    if (type.startsWith(QStringLiteral("task_status")) || type == QLatin1String("email_sent"))
        return theme::colors::success().name();
    return theme::colors::accent().name();
}
} // namespace

AuditLogView::AuditLogView(EventBus* events, QWidget* parent) : QWidget(parent), m_events(events) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(6);

    auto* top = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("📜 审计日志（append-only，全量操作留痕）"), this);
    title->setStyleSheet(QStringLiteral("font-size:11pt;font-weight:bold;color:%1;")
                                            .arg(theme::colors::text().name()));
    auto* refresh = new QPushButton(QStringLiteral("刷新"), this);
    refresh->setFlat(true);
    connect(refresh, &QPushButton::clicked, this, &AuditLogView::reload);
    top->addWidget(title);
    top->addStretch(1);
    top->addWidget(refresh);
    lay->addLayout(top);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("类型"),
                                        QStringLiteral("任务"), QStringLiteral("载荷 JSON")});
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    // 表格观感走 Theme::globalStyleSheet() 统一深色样式
    lay->addWidget(m_table, 1);

    connect(m_events, &EventBus::eventAppended, this, [this](const EventBus::Event& ev) {
        appendRow(ev); // 实时追加
    });
    reload();
}

void AuditLogView::reload() {
    m_table->setRowCount(0);
    for (const auto& ev : m_events->recentEvents())
        appendRow(ev);
    m_table->scrollToBottom();
}

void AuditLogView::appendRow(const EventBus::Event& ev) {
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    const QString time =
        QDateTime::fromSecsSinceEpoch(ev.ts).toString(QStringLiteral("MM-dd hh:mm:ss"));
    auto* typeItem = new QTableWidgetItem(ev.type);
    typeItem->setForeground(QColor(typeColor(ev.type)));
    m_table->setItem(row, 0, new QTableWidgetItem(time));
    m_table->setItem(row, 1, typeItem);
    m_table->setItem(row, 2, new QTableWidgetItem(ev.taskId >= 0 ? QString::number(ev.taskId)
                                                                 : QStringLiteral("-")));
    auto* payload = new QTableWidgetItem(
        QString::fromUtf8(QJsonDocument(ev.payload).toJson(QJsonDocument::Compact)));
    payload->setFont(theme::monoFont());
    payload->setForeground(QColor(theme::colors::textDim().name()));
    m_table->setItem(row, 3, payload);
    m_table->scrollToBottom();
}

} // namespace miderforge
