// 设置对话框（规格 4.10）：①供应商（providers.json 表格编辑+Key DPAPI）②邮件（SMTP+测试邮件）
// ③预算与熔断 ④记忆（L1 上限）⑤外观（深色主题固定）
#pragma once
#include "llm/ProviderManager.h"
#include "notify/EmailNotifier.h"
#include <QDialog>

class QLineEdit;
class QSpinBox;
class QTableWidget;

namespace miderforge {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(ProviderManager* pm, EmailNotifier* mail, QWidget* parent = nullptr);

private slots:
    void onSave();
    void onSendTestMail();

private:
    QWidget* buildProviderTab();
    QWidget* buildMailTab();
    QWidget* buildBudgetTab();
    QWidget* buildMemoryTab();

    ProviderManager* m_pm = nullptr;
    EmailNotifier* m_mail = nullptr;
    QTableWidget* m_providerTable = nullptr;
    QList<QLineEdit*> m_keyEdits; // 与表格行对应
    // 邮件
    QLineEdit* m_smtpUrl = nullptr;
    QLineEdit* m_from = nullptr;
    QLineEdit* m_mailPass = nullptr;
    QLineEdit* m_to = nullptr;
    // 预算
    QSpinBox* m_maxRounds = nullptr;
    QSpinBox* m_maxTokens = nullptr;
    QSpinBox* m_maxSameFail = nullptr;
    QSpinBox* m_l1Limit = nullptr;
    QSpinBox* m_mailPort = nullptr;
};

} // namespace miderforge
