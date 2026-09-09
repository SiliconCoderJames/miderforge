// 记忆视图实现
#include "app/MemoryView.h"
#include "app/Theme.h"
#include "util/Tokens.h"
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QShowEvent>
#include <QPushButton>
#include <QVBoxLayout>

namespace miderforge {

MemoryView::MemoryView(MemoryManager* mem, AdjudicationService* adjudicator, QWidget* parent)
    : QWidget(parent), m_mem(mem), m_adjudicator(adjudicator) {
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(12, 10, 12, 10);
    rootLay->setSpacing(8);

    // 顶栏：搜索 + 类型过滤
    auto* top = new QHBoxLayout();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索记忆（FTS5 全文，回车触发；2 字词自动 LIKE 兜底）"));
    connect(m_search, &QLineEdit::returnPressed, this, &MemoryView::onSearch);
    m_typeFilter = new QComboBox(this);
    m_typeFilter->addItems({QStringLiteral("全部"), QStringLiteral("用户画像"), QStringLiteral("编码偏好"),
                            QStringLiteral("任务教训"), QStringLiteral("项目事实"), QStringLiteral("会话摘要")});
    connect(m_typeFilter, &QComboBox::currentIndexChanged, this, &MemoryView::onFilterChanged);
    top->addWidget(m_search, 1);
    top->addWidget(m_typeFilter);
    rootLay->addLayout(top);

    // 左列表 + 右详情
    auto* split = new QHBoxLayout();
    m_list = new QListWidget(this);
    m_list->setFixedWidth(360);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { onSelected(); });
    split->addWidget(m_list);

    auto* rightLay = new QVBoxLayout();
    m_metaLabel = new QLabel(QStringLiteral("选择左侧记忆查看详情"), this);
    m_metaLabel->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;").arg(theme::colors::textDim.name()));
    m_detail = new QPlainTextEdit(this);
    m_detail->setFont(theme::monoFont());
    m_detail->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background-color:%1;color:%2;border:1px solid #3d4045;border-radius:6px;padding:4px;}")
                               .arg(theme::colors::window.name(), theme::colors::text.name()));
    auto* btnRow = new QHBoxLayout();
    m_saveBtn = new QPushButton(QStringLiteral("保存修改"), this);
    m_archiveBtn = new QPushButton(QStringLiteral("标记废弃"), this);
    auto* l1Btn = new QPushButton(QStringLiteral("编辑 L1 核心记忆"), this);
    auto* scanBtn = new QPushButton(QStringLiteral("🔍 矛盾扫描"), this);
    scanBtn->setToolTip(QStringLiteral("检测高度相似的可疑记忆对（同主题新旧两种说法），人工裁决归档哪条"));
    connect(scanBtn, &QPushButton::clicked, this, &MemoryView::onScanContradictions);
    connect(m_saveBtn, &QPushButton::clicked, this, &MemoryView::onSaveDetail);
    connect(m_archiveBtn, &QPushButton::clicked, this, &MemoryView::onArchive);
    connect(l1Btn, &QPushButton::clicked, this, &MemoryView::onEditL1);
    btnRow->addWidget(m_saveBtn);
    btnRow->addWidget(m_archiveBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(scanBtn);
    btnRow->addWidget(l1Btn);
    rightLay->addWidget(m_metaLabel);
    rightLay->addWidget(m_detail, 1);
    rightLay->addLayout(btnRow);
    split->addLayout(rightLay, 1);
    rootLay->addLayout(split, 1);

    // 底部：L1 占用
    m_l1Label = new QLabel(this);
    m_l1Label->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;").arg(theme::colors::textDim.name()));
    rootLay->addWidget(m_l1Label);
    reload();
}

void MemoryView::showEvent(QShowEvent* ev) {
    QWidget::showEvent(ev);
    reload(); // 切页刷新：任务沉淀/手动变更后所见即最新
}

void MemoryView::reload() {
    const int typeIdx = qBound(0, m_typeFilter->currentIndex(), 5);
    const QString typeKey[] = {QString(), QStringLiteral("user_profile"), QStringLiteral("coding_pref"),
                               QStringLiteral("task_lesson"), QStringLiteral("project_facts"),
                               QStringLiteral("session_summary")};
    m_records = m_mem->listAll(typeKey[typeIdx]);
    m_list->clear();
    for (const auto& r : m_records) {
        const QString time = QDateTime::fromSecsSinceEpoch(r.updatedAt).toString(QStringLiteral("MM-dd hh:mm"));
        QString stars;
        for (int i = 1; i <= 5; ++i)
            stars += (r.importance >= i * 0.2) ? QStringLiteral("★") : QStringLiteral("☆");
        m_list->addItem(QStringLiteral("[%1] %2  %3").arg(r.type, time, stars));
    }
    m_l1Label->setText(QStringLiteral("L1 核心记忆占用：%1 / 4000 tokens")
                           .arg(m_mem->l1Tokens()));
    m_currentId = -1;
    m_detail->clear();
    m_saveBtn->setEnabled(false); // 未选中记录时操作按钮不可点
    m_archiveBtn->setEnabled(false);
    m_metaLabel->setText(QStringLiteral("共 %1 条记忆").arg(m_records.size()));
}

void MemoryView::onSearch() {
    const QString q = m_search->text().trimmed();
    if (q.isEmpty()) {
        reload();
        return;
    }
    // 搜索：复用检索打分（综合排序），展示 Top 20
    const auto hits = m_mem->retrieve(q, 20);
    m_records = hits;
    m_list->clear();
    for (const auto& r : m_records) {
        const QString time = QDateTime::fromSecsSinceEpoch(r.updatedAt).toString(QStringLiteral("MM-dd hh:mm"));
        m_list->addItem(QStringLiteral("[命中] %1  %2").arg(r.type, time));
    }
    m_metaLabel->setText(QStringLiteral("搜索「%1」命中 %2 条").arg(q).arg(hits.size()));
}

void MemoryView::onFilterChanged() {
    m_search->clear();
    reload();
}

void MemoryView::onSelected() {
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_records.size())
        return;
    const auto& r = m_records[row];
    m_currentId = r.id;
    m_showingL1 = false;
    m_saveBtn->setEnabled(true);
    m_archiveBtn->setEnabled(true);
    m_metaLabel->setText(QStringLiteral("id=%1 · 类型=%2 · 重要度=%3 · 访问 %4 次")
                             .arg(r.id).arg(r.type).arg(r.importance).arg(r.accessCount));
    m_detail->setPlainText(r.content);
}

void MemoryView::onSaveDetail() {
    if (m_showingL1) {
        m_mem->saveL1(m_detail->toPlainText()); // 用户手改 L1（优先级最高）
        QMessageBox::information(this, QStringLiteral("Miderforge"),
                                 QStringLiteral("L1 核心记忆已保存。24 小时内 Agent 不会自动覆盖该文件。"));
        reload();
        return;
    }
    if (m_currentId >= 0 && m_mem->updateContent(m_currentId, m_detail->toPlainText())) {
        if (m_mem->recordAccess(m_currentId))
            (void)m_currentId;
        reload();
    }
}

void MemoryView::onArchive() {
    if (m_currentId >= 0 && m_mem->archiveMemory(m_currentId))
        reload();
}

void MemoryView::onEditL1() {
    const QString current = m_mem->loadL1();
    showL1Editor(current, m_mem->l1Tokens());
}

void MemoryView::onScanContradictions() {
    // 一致性扫描 v1：确定性候选对（字符三元组 Dice ∈ [0.35,0.95]）+ 人工裁决归档。
    // LLM 语义裁决为后续增强（对候选对批量提问，成本可控）
    const auto pairs = m_mem->findContradictionCandidates();
    if (pairs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("矛盾扫描"),
                                 QStringLiteral("未发现高度相似的可疑记忆对。"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("矛盾扫描：%1 个可疑记忆对").arg(pairs.size()));
    dlg.resize(760, 480);
    auto* lay = new QVBoxLayout(&dlg);
    auto* hint = new QLabel(
        QStringLiteral("以下记忆对高度相似但内容不同（可能是同一事实的新旧两种说法）。\n"
                       "选择一条保留在列表中，将另一条标记废弃："),
        &dlg);
    auto* list = new QListWidget(&dlg);
    for (const auto& p : pairs)
        list->addItem(QStringLiteral("[#%1] %2  ↔  [#%3] %4  （相似度 %5%）")
                          .arg(p.idA)
                          .arg(p.contentA.left(40))
                          .arg(p.idB)
                          .arg(p.contentB.left(40))
                          .arg(int(p.similarity * 100)));
    auto* btnRow = new QHBoxLayout();
    auto* archiveA = new QPushButton(QStringLiteral("废弃左侧（#A）"), &dlg);
    auto* archiveB = new QPushButton(QStringLiteral("废弃右侧（#B）"), &dlg);
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), &dlg);
    archiveA->setEnabled(false);
    archiveB->setEnabled(false);
    connect(list, &QListWidget::currentRowChanged, this, [&](int row) {
        archiveA->setEnabled(row >= 0);
        archiveB->setEnabled(row >= 0);
    });
    auto onArchiveSide = [&](bool sideA) {
        const int row = list->currentRow();
        if (row < 0 || row >= pairs.size())
            return;
        const qint64 id = sideA ? pairs[row].idA : pairs[row].idB;
        m_mem->archiveMemory(id);
        delete list->takeItem(row); // 已裁决：从候选中移除
    };
    connect(archiveA, &QPushButton::clicked, &dlg, [&] { onArchiveSide(true); });
    connect(archiveB, &QPushButton::clicked, &dlg, [&] { onArchiveSide(false); });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    // 矛盾扫描 v2：AI 语义裁决（独立 ChatClient 实例，不与 AgentLoop 主链路抢占）
    auto* aiStatus = new QLabel(&dlg);
    aiStatus->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;").arg(theme::colors::textDim.name()));
    if (m_adjudicator && m_adjudicator->available()) {
        auto* aiBtn = new QPushButton(QStringLiteral("🤖 AI 语义裁决"), &dlg);
        aiBtn->setToolTip(QStringLiteral("交由大模型逐对判定：矛盾/重复/互补（走 fast 档）"));
        connect(aiBtn, &QPushButton::clicked, &dlg, [&, aiBtn] {
            aiBtn->setEnabled(false);
            aiStatus->setText(QStringLiteral("AI 裁决中…"));
            m_adjudicator->judge(pairs);
        });
        connect(m_adjudicator, &AdjudicationService::finished, &dlg,
                [this, aiBtn, aiStatus, &list, &pairs](const QVector<AdjudicationService::Verdict>& verdicts) {
                    aiBtn->setEnabled(true);
                    if (verdicts.isEmpty()) {
                        aiStatus->setText(QStringLiteral("AI 未给出有效判定。"));
                        return;
                    }
                    for (const auto& v : verdicts) {
                        for (int r = 0; r < list->count(); ++r) {
                            const auto& p = pairs[r];
                            if (!((p.idA == v.idA && p.idB == v.idB)
                                  || (p.idA == v.idB && p.idB == v.idA)))
                                continue;
                            QString tag = QStringLiteral("[?] ");
                            if (v.label == QLatin1String("contradiction")) {
                                tag = QStringLiteral("[矛盾] ");
                                list->item(r)->setForeground(QColor(0xe0, 0x6c, 0x60));
                            } else if (v.label == QLatin1String("duplicate")) {
                                tag = QStringLiteral("[重复] ");
                                list->item(r)->setForeground(QColor(0x9d, 0xa2, 0xa6));
                            } else if (v.label == QLatin1String("complement")) {
                                tag = QStringLiteral("[互补] ");
                            }
                            list->item(r)->setText(tag + list->item(r)->text()
                                                   + QStringLiteral("  — %1").arg(v.reason));
                            break;
                        }
                    }
                    aiStatus->setText(QStringLiteral("AI 裁决完成：%1 对已标注。").arg(verdicts.size()));
                });
        connect(m_adjudicator, &AdjudicationService::failed, &dlg,
                [aiBtn, aiStatus](const QString& error) {
                    aiBtn->setEnabled(true);
                    aiStatus->setText(QStringLiteral("AI 裁决失败：%1").arg(error));
                });
        btnRow->insertWidget(0, aiBtn);
    }

    btnRow->addWidget(archiveA);
    btnRow->addWidget(archiveB);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    lay->addWidget(hint);
    lay->addWidget(list, 1);
    lay->addWidget(aiStatus);
    lay->addLayout(btnRow);
    dlg.exec();
    reload(); // 裁决后刷新主列表
}

void MemoryView::showL1Editor(const QString& current, qint64 tokens) {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("L1 核心记忆（memory/core.md，上限 4000 tokens）"));
    dlg.resize(680, 560);
    auto* lay = new QVBoxLayout(&dlg);
    auto* editor = new QPlainTextEdit(&dlg);
    editor->setFont(theme::monoFont());
    editor->setPlainText(current);
    auto* info = new QLabel(QStringLiteral("当前占用 %1 tokens（>80%% 变黄提示；四节：用户画像/编码偏好/禁区/活跃项目状态）").arg(tokens), &dlg);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(info);
    lay->addWidget(editor, 1);
    lay->addWidget(buttons);
    m_showingL1 = true;
    if (dlg.exec() == QDialog::Accepted) {
        m_mem->saveL1(editor->toPlainText());
        QMessageBox::information(this, QStringLiteral("Miderforge"),
                                 QStringLiteral("L1 已保存，下一任务生效。"));
        reload();
    }
    m_showingL1 = false;
}

} // namespace miderforge
