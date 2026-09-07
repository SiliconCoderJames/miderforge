// 记忆视图实现
#include "app/MemoryView.h"
#include "app/Theme.h"
#include "util/Tokens.h"
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace miderforge {

MemoryView::MemoryView(MemoryManager* mem, QWidget* parent) : QWidget(parent), m_mem(mem) {
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
    connect(m_saveBtn, &QPushButton::clicked, this, &MemoryView::onSaveDetail);
    connect(m_archiveBtn, &QPushButton::clicked, this, &MemoryView::onArchive);
    connect(l1Btn, &QPushButton::clicked, this, &MemoryView::onEditL1);
    btnRow->addWidget(m_saveBtn);
    btnRow->addWidget(m_archiveBtn);
    btnRow->addStretch(1);
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
