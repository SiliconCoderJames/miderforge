// 设置对话框（ZCode/Codex 风格左导航）：①通用（权限模式等）②供应商（providers.json 表格编辑+Key DPAPI）
// ③邮件（SMTP+测试邮件）④任务与预算 ⑤记忆与检索（L1 上限）⑥蜂巢（AgentHive 本地连接，仅回环，主密钥 DPAPI）
#pragma once
#include "llm/ProviderManager.h"
#include "notify/EmailNotifier.h"
#include <QDialog>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QSpinBox;
class QTableWidget;

namespace miderforge {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // applyPermissionMode：与主窗口顶部权限下拉框同源，保存时经它生效（AppContext + 会话输入区双向同步）
    SettingsDialog(ProviderManager* pm, EmailNotifier* mail,
                   const std::function<void(int)>& applyPermissionMode, QWidget* parent = nullptr);

private slots:
    void onSave();
    void onSendTestMail();
    void onTestHive();

private:
    QWidget* buildGeneralPage();
    QWidget* buildProviderPage();
    QWidget* buildMailPage();
    QWidget* buildBudgetPage();
    QWidget* buildMemoryPage();
    QWidget* buildHivePage();

    void loadHive();                    // 读 config/hive.json（主密钥为 DPAPI 密文）
    void saveHive();                    // 写回（密钥输入非空才覆盖密文）

    ProviderManager* m_pm = nullptr;
    EmailNotifier* m_mail = nullptr;
    std::function<void(int)> m_applyPermissionMode;

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_pages = nullptr;

    // 通用
    QComboBox* m_permCombo = nullptr;
    // 供应商
    QTableWidget* m_providerTable = nullptr;
    QList<QLineEdit*> m_keyEdits; // 与表格行对应
    QCheckBox* m_embEnabled = nullptr;   // 语义检索（M6-A）
    QComboBox* m_embProvider = nullptr;
    QLineEdit* m_embModel = nullptr;
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
    // 蜂巢（AgentHive）
    QCheckBox* m_hiveEnabled = nullptr;
    QLineEdit* m_hiveHost = nullptr;
    QLineEdit* m_hivePort = nullptr;
    QLineEdit* m_hiveName = nullptr;
    QLineEdit* m_hiveKey = nullptr;
    QLabel* m_hiveStatus = nullptr;
    QByteArray m_hiveResp;      // 健康检查响应累积（分片到达）
    QByteArray m_hiveKeyCipher; // 已存密文（与输入框互补：输入框为空时保留原密文）
};

} // namespace miderforge
