// 技能库视图实现
#include "app/SkillView.h"
#include "app/Theme.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace miderforge {

SkillView::SkillView(SkillManager* skills, QWidget* parent) : QWidget(parent), m_skills(skills) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* top = new QHBoxLayout();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索技能（FTS5 实时过滤）"));
    connect(m_search, &QLineEdit::textChanged, this, &SkillView::onSearchChanged);
    auto* newBtn = new QPushButton(QStringLiteral("＋ 手动新建技能"), this);
    connect(newBtn, &QPushButton::clicked, this, &SkillView::onNewSkill);
    top->addWidget(m_search, 1);
    top->addWidget(newBtn);
    lay->addLayout(top);

    auto* split = new QHBoxLayout();
    m_list = new QListWidget(this);
    m_list->setFixedWidth(380);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { onSelected(); });
    split->addWidget(m_list);

    auto* rightLay = new QVBoxLayout();
    m_meta = new QLabel(QStringLiteral("选择左侧技能查看详情"), this);
    m_meta->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;").arg(theme::colors::textDim.name()));
    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setFont(theme::monoFont());
    m_preview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background-color:%1;color:%2;border:1px solid #3d4045;border-radius:6px;padding:4px;}")
                               .arg(theme::colors::window.name(), theme::colors::text.name()));
    auto* btnRow = new QHBoxLayout();
    m_editBtn = new QPushButton(QStringLiteral("编辑"), this);
    m_deprecateBtn = new QPushButton(QStringLiteral("标记废弃"), this);
    connect(m_editBtn, &QPushButton::clicked, this, &SkillView::onEditSkill);
    connect(m_deprecateBtn, &QPushButton::clicked, this, &SkillView::onDeprecate);
    btnRow->addStretch(1);
    btnRow->addWidget(m_editBtn);
    btnRow->addWidget(m_deprecateBtn);
    rightLay->addWidget(m_meta);
    rightLay->addWidget(m_preview, 1);
    rightLay->addLayout(btnRow);
    split->addLayout(rightLay, 1);
    lay->addLayout(split, 1);
    reload();
}

void SkillView::reload() {
    onSearchChanged(m_search->text());
}

void SkillView::onSearchChanged(const QString& text) {
    m_records = m_skills->search(text, 50);
    m_list->clear();
    for (const auto& s : m_records) {
        const int rate = s.usageCount > 0 ? s.successCount * 100 / s.usageCount : 0;
        QString tag;
        if (s.status == QLatin1String("deprecated"))
            tag = QStringLiteral("【已废弃】");
        else if (s.status == QLatin1String("review"))
            tag = QStringLiteral("【待审查】");
        m_list->addItem(QStringLiteral("%1%2 v%3 · 成功率 %4% · 用 %5 次")
                            .arg(tag, s.name, QString::number(s.version),
                                 QString::number(rate), QString::number(s.usageCount)));
    }
}

void SkillView::onSelected() {
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_records.size())
        return;
    const auto& s = m_records[row];
    m_current = s.name;
    m_meta->setText(QStringLiteral("%1 · %2 · 使用 %3 次/成功 %4 次 · 平均 %5 轮")
                        .arg(s.name, s.description)
                        .arg(s.usageCount)
                        .arg(s.successCount)
                        .arg(s.avgRounds, 0, 'f', 1));
    m_preview->setPlainText(m_skills->loadSkillMd(s.name));
}

void SkillView::onNewSkill() {
    showEditor(QString(), QString(), QStringLiteral("## 适用场景\n\n## 执行步骤\n\n## 边界与坑\n\n## 验收标准\n"));
}

void SkillView::onEditSkill() {
    if (m_current.isEmpty())
        return;
    const SkillManager::SkillMeta s = m_skills->find(m_current);
    // frontmatter 去掉，正文进编辑器；名称/描述单独编辑
    QString name, desc, body;
    int version = 1;
    SkillManager::parseFrontmatter(m_skills->loadSkillMd(m_current), &name, &desc, &version, &body);
    showEditor(s.name, s.description, body);
}

void SkillView::showEditor(const QString& name, const QString& description, const QString& body) {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("技能编辑器"));
    dlg.resize(720, 620);
    auto* lay = new QFormLayout(&dlg);
    auto* nameEdit = new QLineEdit(name, &dlg);
    auto* descEdit = new QLineEdit(description, &dlg);
    auto* bodyEdit = new QPlainTextEdit(&dlg);
    bodyEdit->setFont(theme::monoFont());
    bodyEdit->setPlainText(body);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addRow(QStringLiteral("技能名"), nameEdit);
    lay->addRow(QStringLiteral("一句话描述"), descEdit);
    lay->addRow(QStringLiteral("正文（适用场景/执行步骤/边界与坑/验收标准）"), bodyEdit);
    lay->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!m_skills->writeSkill(nameEdit->text(), descEdit->text(), bodyEdit->toPlainText(), &err))
        QMessageBox::warning(this, QStringLiteral("Miderforge"), err);
    reload();
}

void SkillView::onDeprecate() {
    if (!m_current.isEmpty() && m_skills->markDeprecated(m_current))
        reload();
}

} // namespace miderforge
