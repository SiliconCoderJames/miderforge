// 首次运行向导实现
#include "app/FirstRunWizard.h"
#include "app/Theme.h"
#include "llm/ProviderManager.h"
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace miderforge {

FirstRunWizard::FirstRunWizard(ProviderManager* pm, QWidget* parent)
    : QDialog(parent), m_pm(pm) {
    setWindowTitle(QStringLiteral("Miderforge 首次配置"));
    setMinimumWidth(560);

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("<b>欢迎使用 Miderforge</b>"), this);
    auto* hint = new QLabel(
        QStringLiteral("首次运行需要配置至少一家大模型供应商的 API Key。<br/>"
                       "Key 使用 Windows DPAPI 加密后存储在本机配置文件中，不会以明文落盘，也不会上传。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;").arg(theme::colors::textDim().name()));
    lay->addWidget(title);
    lay->addWidget(hint);

    auto makeGroup = [this, &lay](const QString& name, const QString& url, const QString& applyHint,
                                  QLineEdit** urlEdit, QLineEdit** keyEdit) {
        auto* groupLay = new QFormLayout();
        auto* header = new QLabel(QStringLiteral("<b>%1</b>　<a href=\"%2\">%3</a>").arg(name, url, applyHint), this);
        header->setOpenExternalLinks(true);
        groupLay->addRow(header);
        *urlEdit = new QLineEdit(url, this);
        groupLay->addRow(QStringLiteral("接口地址"), *urlEdit);
        *keyEdit = new QLineEdit(this);
        (*keyEdit)->setEchoMode(QLineEdit::Password);
        (*keyEdit)->setPlaceholderText(QStringLiteral("粘贴 API Key（留空则跳过该供应商）"));
        groupLay->addRow(QStringLiteral("API Key"), *keyEdit);
        auto* container = new QWidget(this);
        container->setLayout(groupLay);
        container->setStyleSheet(QStringLiteral(
            "QWidget{background-color:%1;border:1px solid #35383d;border-radius:6px;}")
                                    .arg(theme::colors::panel().name()));
        lay->addWidget(container);
    };

    makeGroup(QStringLiteral("智谱（默认主力）"), QStringLiteral("https://open.bigmodel.cn/api/paas/v4"),
              QStringLiteral("申请入口 open.bigmodel.cn"), &m_zhipuUrl, &m_zhipuKey);
    makeGroup(QStringLiteral("DeepSeek（故障转移备胎）"), QStringLiteral("https://api.deepseek.com"),
              QStringLiteral("申请入口 platform.deepseek.com"), &m_deepseekUrl, &m_deepseekKey);

    auto* btnRow = new QHBoxLayout();
    auto* skip = new QPushButton(QStringLiteral("跳过"), this);
    auto* save = new QPushButton(QStringLiteral("保存并完成"), this);
    save->setDefault(true);
    connect(skip, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &FirstRunWizard::onSave);
    btnRow->addStretch(1);
    btnRow->addWidget(skip);
    btnRow->addWidget(save);
    lay->addLayout(btnRow);
}

void FirstRunWizard::onSave() {
    // 接口地址如有改动一并写回
    if (m_zhipuUrl && !m_zhipuUrl->text().trimmed().isEmpty())
        m_pm->setBaseUrl(QStringLiteral("zhipu"), m_zhipuUrl->text());
    if (m_deepseekUrl && !m_deepseekUrl->text().trimmed().isEmpty())
        m_pm->setBaseUrl(QStringLiteral("deepseek"), m_deepseekUrl->text());

    if (!m_zhipuKey->text().trimmed().isEmpty() && !m_pm->saveKey(QStringLiteral("zhipu"), m_zhipuKey->text().trimmed())) {
        QMessageBox::warning(this, QStringLiteral("Miderforge"), QStringLiteral("智谱 API Key 保存失败（DPAPI 加密异常）"));
        return;
    }
    if (!m_deepseekKey->text().trimmed().isEmpty() && !m_pm->saveKey(QStringLiteral("deepseek"), m_deepseekKey->text().trimmed())) {
        QMessageBox::warning(this, QStringLiteral("Miderforge"), QStringLiteral("DeepSeek API Key 保存失败（DPAPI 加密异常）"));
        return;
    }
    accept();
}

} // namespace miderforge
