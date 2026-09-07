// 记忆视图（规格 4.6）：顶栏搜索（FTS5 回车触发）+类型过滤；左列表（类型/时间/重要度★）；右详情编辑器；
// 底部 L1 核心记忆专用编辑入口与 token 占用
#pragma once
#include "memory/MemoryManager.h"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QWidget>

class QPushButton;

namespace miderforge {

class MemoryView : public QWidget {
    Q_OBJECT
public:
    MemoryView(MemoryManager* mem, QWidget* parent = nullptr);

public slots:
    void reload(); // 重新拉取列表（含 L1 占用）

private slots:
    void onSearch();
    void onFilterChanged();
    void onSelected();
    void onSaveDetail();
    void onArchive();
    void onEditL1();

private:
    void showL1Editor(const QString& current, qint64 tokens);

    MemoryManager* m_mem = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_typeFilter = nullptr;
    QListWidget* m_list = nullptr;
    QPlainTextEdit* m_detail = nullptr;
    QLabel* m_metaLabel = nullptr;
    QLabel* m_l1Label = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_archiveBtn = nullptr;
    QVector<MemoryManager::MemoryRecord> m_records;
    qint64 m_currentId = -1;
    bool m_showingL1 = false;
};

} // namespace miderforge
