// 任务队列视图（规格 4.4）：新建任务对话框（目标/计划时间/每日重复）+ 任务表 + 行点击事件时间线
#pragma once
#include "db/Database.h"
#include <QLabel>
#include <QTableView>
#include <QWidget>

namespace miderforge {

class AgentLoop;

// tasks 表的只读表模型（写操作走 SQL 后 reload）
class TasksModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit TasksModel(Database* db, QObject* parent = nullptr);
    void reload();
    int rowCount(const QModelIndex& = {}) const override { return int(m_rows.size()); }
    int columnCount(const QModelIndex& = {}) const override { return 8; }
    QVariant data(const QModelIndex& idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    qint64 taskIdAt(int row) const { return row >= 0 && row < int(m_rows.size()) ? m_rows[row].id : -1; }

private:
    struct Row {
        qint64 id;
        QString goal, status;
        int rounds;
        long long tokens;
        qint64 createdAt, scheduledAt;
        int repeatDaily;
    };
    Database* m_db = nullptr;
    QVector<Row> m_rows;
};

class TaskQueueView : public QWidget {
    Q_OBJECT
public:
    TaskQueueView(Database* db, AgentLoop* loop, QWidget* parent = nullptr);

public slots:
    void reload();

protected:
    // 切到本页即重载（任务在别处跑完/入队时保证所见即最新）
    void showEvent(QShowEvent* ev) override;

private slots:
    void onNewTask();
    void onRowSelected();
    void onCancelTask();

private:
    Database* m_db = nullptr;
    AgentLoop* m_loop = nullptr;
    TasksModel* m_model = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_detail = nullptr;
};

} // namespace miderforge
