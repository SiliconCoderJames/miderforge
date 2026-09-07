// 设置对话框实现
#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "core/AppContext.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace miderforge {

SettingsDialog::SettingsDialog(ProviderManager* pm, EmailNotifier* mail, QWidget* parent)
    : QDialog(parent), m_pm(pm), m_mail(mail) {
    setWindowTitle(QStringLiteral("Miderforge 设置"));
    resize(720, 520);
    auto* lay = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildProviderTab(), QStringLiteral("供应商"));
    tabs->addTab(buildMailTab(), QStringLiteral("邮件"));
    tabs->addTab(buildBudgetTab(), QStringLiteral("预算与熔断"));
    tabs->addTab(buildMemoryTab(), QStringLiteral("记忆"));
    // ⑤外观：深色主题为产品默认（规格 4.8），v1 不提供切换
    tabs->addTab(new QLabel(QStringLiteral("外观：当前为深色主题（Fusion + 规格色板），v1 固定。"), this),
                 QStringLiteral("外观"));
    lay->addWidget(tabs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

QWidget* SettingsDialog::buildProviderTab() {
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    m_providerTable = new QTableWidget(int(m_pm->all().size()), 2, w);
    m_providerTable->setHorizontalHeaderLabels({QStringLiteral("供应商"), QStringLiteral("接口地址 base_url")});
    m_providerTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    int row = 0;
    for (const auto& cfg : m_pm->all()) {
        auto* nameItem = new QTableWidgetItem(cfg.name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        m_providerTable->setItem(row, 0, nameItem);
        m_providerTable->setItem(row, 1, new QTableWidgetItem(cfg.baseUrl));
        auto* key = new QLineEdit(w);
        key->setEchoMode(QLineEdit::Password);
        key->setPlaceholderText(cfg.configured ? QStringLiteral("已配置（输入则覆盖）")
                                               : QStringLiteral("粘贴 API Key（DPAPI 加密存储）"));
        m_keyEdits.append(key);
        ++row;
    }
    lay->addWidget(new QLabel(QStringLiteral("API Key 修改后点〔保存〕；落盘为 DPAPI 密文。"), w), 0);
    lay->addWidget(m_providerTable, 1);
    return w;
}

QWidget* SettingsDialog::buildMailTab() {
    m_mail->loadConfig();
    const auto& cfg = m_mail->config();
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    m_smtpUrl = new QLineEdit(cfg.smtpUrl.isEmpty() ? QStringLiteral("smtps://smtp.qq.com:465")
                                                    : cfg.smtpUrl, w);
    m_from = new QLineEdit(cfg.from, w);
    m_mailPass = new QLineEdit(cfg.authCode, w);
    m_mailPass->setEchoMode(QLineEdit::Password);
    m_mailPass->setPlaceholderText(QStringLiteral("授权码（不是邮箱登录密码；QQ/163 需开启 SMTP 后生成）"));
    m_to = new QLineEdit(cfg.to, w);
    form->addRow(QStringLiteral("SMTP 服务器"), m_smtpUrl);
    form->addRow(QStringLiteral("发件人"), m_from);
    form->addRow(QStringLiteral("授权码"), m_mailPass);
    form->addRow(QStringLiteral("收件人"), m_to);
    auto* testRow = new QHBoxLayout();
    auto* testBtn = new QPushButton(QStringLiteral("发测试邮件"), w);
    connect(testBtn, &QPushButton::clicked, this, &SettingsDialog::onSendTestMail);
    testRow->addStretch(1);
    testRow->addWidget(testBtn);
    form->addRow(testRow);
    return w;
}

QWidget* SettingsDialog::buildBudgetTab() {
    const auto& limits = AppContext::instance().limits;
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    m_maxRounds = new QSpinBox(w);
    m_maxRounds->setRange(3, 200);
    m_maxRounds->setValue(limits.maxRounds);
    m_maxTokens = new QSpinBox(w);
    m_maxTokens->setRange(10000, 100000000);
    m_maxTokens->setSingleStep(50000);
    m_maxTokens->setValue(int(limits.maxTokens));
    m_maxSameFail = new QSpinBox(w);
    m_maxSameFail->setRange(2, 20);
    m_maxSameFail->setValue(limits.maxSameFailures);
    form->addRow(QStringLiteral("轮数上限"), m_maxRounds);
    form->addRow(QStringLiteral("单任务 token 限额"), m_maxTokens);
    form->addRow(QStringLiteral("连续相同失败次数"), m_maxSameFail);
    return w;
}

QWidget* SettingsDialog::buildMemoryTab() {
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    m_l1Limit = new QSpinBox(w);
    m_l1Limit->setRange(500, 100000);
    m_l1Limit->setSingleStep(500);
    m_l1Limit->setValue(AppContext::instance().l1TokenLimit);
    form->addRow(QStringLiteral("L1 核心记忆上限（tokens）"), m_l1Limit);
    return w;
}

void SettingsDialog::onSave() {
    // 供应商：base_url 与 Key
    int row = 0;
    for (const auto& cfg : m_pm->all()) {
        const QString newUrl = m_providerTable->item(row, 1)->text().trimmed();
        if (!newUrl.isEmpty() && newUrl != cfg.baseUrl)
            m_pm->setBaseUrl(cfg.name, newUrl);
        const QString key = m_keyEdits.at(row)->text().trimmed();
        if (!key.isEmpty())
            m_pm->saveKey(cfg.name, key);
        ++row;
    }
    // 邮件
    auto cfg = m_mail->config();
    cfg.smtpUrl = m_smtpUrl->text().trimmed();
    cfg.from = m_from->text().trimmed();
    cfg.to = m_to->text().trimmed();
    if (!m_mailPass->text().isEmpty())
        cfg.authCode = m_mailPass->text();
    cfg.enabled = !cfg.from.isEmpty() && !cfg.to.isEmpty() && !cfg.authCode.isEmpty();
    m_mail->saveConfig(cfg);
    // 预算与记忆
    auto& limits = AppContext::instance().limits;
    limits.maxRounds = m_maxRounds->value();
    limits.maxTokens = m_maxTokens->value();
    limits.maxSameFailures = m_maxSameFail->value();
    AppContext::instance().l1TokenLimit = m_l1Limit->value();
    accept();
}

void SettingsDialog::onSendTestMail() {
    // 先暂存当前表单再发（避免“填完没保存测试的是旧配置”）
    auto cfg = m_mail->config();
    cfg.smtpUrl = m_smtpUrl->text().trimmed();
    cfg.from = m_from->text().trimmed();
    cfg.to = m_to->text().trimmed();
    if (!m_mailPass->text().isEmpty())
        cfg.authCode = m_mailPass->text();
    cfg.enabled = true;
    m_mail->saveConfig(cfg);

    QString err;
    if (m_mail->send(QStringLiteral("Miderforge 测试邮件"),
                     QStringLiteral("<h3>Miderforge 邮件通知已就绪</h3><p>任务完成/失败/熔断时将发信至此。</p>"),
                     &err))
        QMessageBox::information(this, QStringLiteral("Miderforge"), QStringLiteral("测试邮件已发送，请查收。"));
    else
        QMessageBox::warning(this, QStringLiteral("Miderforge"),
                             QStringLiteral("发送失败：%1\n\n请检查：服务器地址/端口、授权码（非登录密码）、SMTP 服务是否已开启。").arg(err));
}

} // namespace miderforge
