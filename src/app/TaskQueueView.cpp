// 任务队列视图实现
#include "app/TaskQueueView.h"
#include "app/Theme.h"
#include "core/AgentLoop.h"
#include <QDateTime>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace miderforge {

// ---------- TasksModel ----------
TasksModel::TasksModel(Database* db, QObject* parent) : QAbstractTableModel(parent), m_db(db) {
    reload();
}

void TasksModel::reload() {
    beginResetModel();
    m_rows.clear();
    for (const auto& r : m_db->query(QStringLiteral(
             "SELECT id, goal, status, rounds_used, tokens_in, created_at, scheduled_at, repeat_daily "
             "FROM tasks ORDER BY id DESC LIMIT 500"), {})) {
        m_rows.push_back({r.value("id").toLongLong(), r.value("goal").toString(),
                          r.value("status").toString(), r.value("rounds_used").toInt(),
                          r.value("tokens_in").toLongLong(), r.value("created_at").toLongLong(),
                          r.value("scheduled_at").toLongLong(), r.value("repeat_daily").toInt()});
    }
    endResetModel();
}

QVariant TasksModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || role != Qt::DisplayRole)
        return {};
    const Row& r = m_rows[idx.row()];
    switch (idx.column()) {
    case 0: return r.id;
    case 1: return r.goal.length() > 40 ? r.goal.left(40) + QStringLiteral("…") : r.goal;
    case 2: return r.status;
    case 3: return r.rounds;
    case 4: return r.tokens;
    case 5: return QDateTime::fromSecsSinceEpoch(r.createdAt).toString(QStringLiteral("MM-dd hh:mm"));
    case 6: return r.scheduledAt > 0
                  ? QDateTime::fromSecsSinceEpoch(r.scheduledAt).toString(QStringLiteral("MM-dd hh:mm"))
                  : QStringLiteral("立即");
    case 7: return r.repeatDaily ? QStringLiteral("每天") : QStringLiteral("-");
    }
    return {};
}

QVariant TasksModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    static const QStringList headers = {QStringLiteral("ID"), QStringLiteral("目标"), QStringLiteral("状态"),
                                        QStringLiteral("轮数"), QStringLiteral("tokens"), QStringLiteral("创建时间"),
                                        QStringLiteral("计划开始"), QStringLiteral("重复")};
    return headers.value(section);
}

// ---------- TaskQueueView ----------
TaskQueueView::TaskQueueView(Database* db, AgentLoop* loop, QWidget* parent)
    : QWidget(parent), m_db(db), m_loop(loop) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* top = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("📋 任务队列"), this);
    title->setStyleSheet(QStringLiteral("font-size:11pt;font-weight:bold;color:%1;").arg(theme::colors::text.name()));
    auto* newBtn = new QPushButton(QStringLiteral("＋ 新建任务"), this);
    auto* cancelBtn = new QPushButton(QStringLiteral("取消选中任务"), this);
    connect(newBtn, &QPushButton::clicked, this, &TaskQueueView::onNewTask);
    connect(cancelBtn, &QPushButton::clicked, this, &TaskQueueView::onCancelTask);
    top->addWidget(title);
    top->addStretch(1);
    top->addWidget(newBtn);
    top->addWidget(cancelBtn);
    lay->addLayout(top);

    m_model = new TasksModel(db, this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setAlternatingRowColors(true);
    m_table->setStyleSheet(QStringLiteral(
        "QTableView{background-color:%1;color:%2;gridline-color:#35383d;}"
        "QHeaderView::section{background-color:%3;color:%4;border:none;padding:4px;}")
                               .arg(theme::colors::panel.name(), theme::colors::text.name(),
                                    theme::colors::window.name(), theme::colors::textDim.name()));
    connect(m_table, &QTableView::clicked, this, &TaskQueueView::onRowSelected);
    lay->addWidget(m_table, 2);

    m_detail = new QLabel(QStringLiteral("点击任务行查看步骤时间线"), this);
    m_detail->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setStyleSheet(QStringLiteral(
        "QLabel{background-color:%1;color:%2;border:1px solid #3d4045;border-radius:6px;padding:6px;}")
                                .arg(theme::colors::window.name(), theme::colors::textDim.name()));
    lay->addWidget(m_detail, 1);
    reload();
}

void TaskQueueView::reload() {
    m_model->reload();
}

void TaskQueueView::onNewTask() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("新建任务"));
    dlg.resize(520, 320);
    auto* lay = new QVBoxLayout(&dlg);
    auto* goalEdit = new QTextEdit(&dlg);
    goalEdit->setPlaceholderText(QStringLiteral("任务目标（支持多行）"));
    auto* timeRow = new QHBoxLayout();
    auto* schedCheck = new QCheckBox(QStringLiteral("计划开始时间："), &dlg);
    auto* schedAt = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600), &dlg);
    schedAt->setEnabled(false);
    schedAt->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    connect(schedCheck, &QCheckBox::toggled, schedAt, &QDateTimeEdit::setEnabled);
    auto* repeat = new QCheckBox(QStringLiteral("重复：每天"), &dlg);
    timeRow->addWidget(schedCheck);
    timeRow->addWidget(schedAt);
    timeRow->addStretch(1);
    timeRow->addWidget(repeat);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(goalEdit, 1);
    lay->addLayout(timeRow);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString goal = goalEdit->toPlainText().trimmed();
    if (goal.isEmpty())
        return;
    const qint64 scheduled = schedCheck->isChecked()
                                 ? schedAt->dateTime().toSecsSinceEpoch()
                                 : 0; // 0 = 立即（队列调度器按序启动）
    m_db->execute(QStringLiteral("INSERT INTO tasks(goal, status, scheduled_at, repeat_daily, created_at) "
                                 "VALUES(?, 'queued', ?, ?, ?)"),
                  {goal, scheduled, repeat->isChecked() ? 1 : 0, QDateTime::currentSecsSinceEpoch()});
    m_db->execute(QStringLiteral("INSERT INTO events(ts, type, task_id, payload_json) VALUES(?,?,0,?)"),
                   {QDateTime::currentSecsSinceEpoch(), QStringLiteral("task_status"),
                    QStringLiteral("{\"status\":\"queued\"}")});
    reload();
}

void TaskQueueView::onCancelTask() {
    const QModelIndex idx = m_table->currentIndex();
    if (!idx.isValid())
        return;
    const qint64 id = m_model->taskIdAt(idx.row());
    if (m_loop->isRunning() && id == m_loop->currentTaskId()) {
        m_loop->cancel(); // 运行中任务走取消流程
        return;
    }
    m_db->execute(QStringLiteral("UPDATE tasks SET status='cancelled' WHERE id=? AND status IN ('queued')"), {id});
    reload();
}

void TaskQueueView::onRowSelected() {
    const QModelIndex idx = m_table->currentIndex();
    if (!idx.isValid())
        return;
    const qint64 id = m_model->taskIdAt(idx.row());
    // 右侧详情：该任务的 events 时间线
    const auto rows = m_db->query(QStringLiteral(
        "SELECT ts, type, payload_json FROM events WHERE task_id=? ORDER BY ts ASC, id ASC LIMIT 200"), {id});
    QString html = QStringLiteral("<b>任务 #%1 时间线</b><br/>").arg(id);
    for (const auto& r : rows) {
        const QString time = QDateTime::fromSecsSinceEpoch(r.value("ts").toLongLong())
                                 .toString(QStringLiteral("MM-dd hh:mm:ss"));
        html += QStringLiteral("%1 · %2 · <span style='font-family:Consolas'>%3</span><br/>")
                    .arg(time, r.value("type").toString(), r.value("payload_json").toString().left(160));
    }
    m_detail->setText(html);
}

} // namespace miderforge
