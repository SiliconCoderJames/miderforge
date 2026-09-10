// 记忆视图（规格 4.6）：顶栏搜索（FTS5 回车触发）+类型过滤；左列表（类型/时间/重要度★）；右详情编辑器；
// 底部 L1 核心记忆专用编辑入口与 token 占用
#pragma once
#include "core/AdjudicationService.h"
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
    MemoryView(MemoryManager* mem, AdjudicationService* adjudicator, QWidget* parent = nullptr);

public slots:
    void reload(); // 重新拉取列表（含 L1 占用）

protected:
    // 切到本页即重载（任务运行中会沉淀记忆，回到本页所见即最新）
    void showEvent(QShowEvent* ev) override;

private slots:
    void onSearch();
    void onFilterChanged();
    void onSelected();
    void onSaveDetail();
    void onArchive();
    void onEditL1();
    void onScanContradictions(); // 一致性扫描：相似候选对人工裁决
    void onApprovePending();     // Hermes write_approval：批准待审记忆
    void onRejectPending();      // 拒绝待审记忆（归档）

private:
    void showL1Editor(const QString& current, qint64 tokens);

    MemoryManager* m_mem = nullptr;
    AdjudicationService* m_adjudicator = nullptr; // 矛盾扫描 v2：可空（未接服务则隐藏 AI 按钮）
    QLineEdit* m_search = nullptr;
    QComboBox* m_typeFilter = nullptr;
    QListWidget* m_list = nullptr;
    QPlainTextEdit* m_detail = nullptr;
    QLabel* m_metaLabel = nullptr;
    QLabel* m_l1Label = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_archiveBtn = nullptr;
    QPushButton* m_approveBtn = nullptr; // 待审记忆：批准/拒绝（审批门开启时出现）
    QPushButton* m_rejectBtn = nullptr;
    QVector<MemoryManager::MemoryRecord> m_records;
    qint64 m_currentId = -1;
    bool m_showingL1 = false;
};

} // namespace miderforge
