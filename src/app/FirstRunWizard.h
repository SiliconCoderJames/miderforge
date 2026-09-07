// 首次运行向导：初始化数据目录、采集两家供应商 API Key（DPAPI 加密落盘，密文入 providers.json）
#pragma once
#include <QDialog>
#include <QLineEdit>

namespace miderforge {

class ProviderManager;

class FirstRunWizard : public QDialog {
    Q_OBJECT
public:
    explicit FirstRunWizard(ProviderManager* pm, QWidget* parent = nullptr);

private slots:
    void onSave();

private:
    ProviderManager* m_pm = nullptr;
    QLineEdit* m_zhipuUrl = nullptr;
    QLineEdit* m_zhipuKey = nullptr;
    QLineEdit* m_deepseekUrl = nullptr;
    QLineEdit* m_deepseekKey = nullptr;
};

} // namespace miderforge
