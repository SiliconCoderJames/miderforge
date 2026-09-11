// 设置面板（原为独立 QDialog，现改为**嵌入主窗口内容区**的普通 QWidget）。
// 取舍：设置是"从工作区进去、改完回来"的一级页面，弹独立窗口会脱离主窗口布局、
// 多一个任务栏条目，也与 Codex/DSH 的"内容区切换"观感不一致。
// 各页本来就是 QWidget，因此只换基类、页面构建代码零改动。
#pragma once
#include "llm/ProviderManager.h"
#include "notify/EmailNotifier.h"
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;

namespace miderforge {

class SettingsDialog : public QWidget {
    Q_OBJECT
public:
    // 功能面板（任务队列/技能库/记忆/供应商/审计日志）由 MainWindow 持有并传入：
    // 它们按固定位置混排在设置页里，所以必须在构造时一次排定顺序——
    // 若改成构造后追加，导航顺序与 QStackedWidget 页序会错位。
    struct Panels {
        QWidget* taskQueue = nullptr;
        QWidget* skills = nullptr;
        QWidget* memory = nullptr;
        QWidget* providers = nullptr;
        QWidget* audit = nullptr;
    };

    // applyPermissionMode：与 Composer 权限档同源，保存时经它生效
    SettingsDialog(ProviderManager* pm, EmailNotifier* mail, const Panels& panels,
                   const std::function<void(int)>& applyPermissionMode, QWidget* parent = nullptr);

    // 直达某页（命令面板 / 视图菜单用）
    void showPage(int pageIndex);

signals:
    // 「返回工作区」「关闭」→ 主窗口切回会话页
    void closeRequested();
    // 「保存」已落盘 → 主窗口刷新供应商下拉/状态栏/L1 占用
    void saved();

private slots:
    void onSave();
    void onSendTestMail();
    void onTestHive();

private:
    QWidget* buildGeneralPage();
    QWidget* buildAppearancePage();   // 外观与主题（从原「通用」页拆出）
    QWidget* buildProviderPage();
    QWidget* buildMailPage();
    QWidget* buildBudgetPage();
    QWidget* buildMemoryPage();
    QWidget* buildHivePage();

    // 左导航：分组标题 + 条目（对齐参考图的信息架构）
    struct NavEntry {
        QString title;
        QString icon;       // 线性符号（不用 emoji，深色下渲染脏）
        QString section;    // 所属分组标题
        int page = -1;      // 对应堆叠页索引
        QPushButton* btn = nullptr;
    };
    void addNavSection(const QString& title);
    // 同时决定「导航条目」与「堆叠页序号」：先 addNavEntry 拿到 page，再由调用方 addWidget
    int addNavEntry(const QString& icon, const QString& title, const QString& section);
    void filterNav(const QString& query); // 设置搜索：按标题过滤并隐藏空分组
    void selectNav(int pageIndex);

    ProviderManager* m_pm = nullptr;
    EmailNotifier* m_mail = nullptr;
    std::function<void(int)> m_applyPermissionMode;

    QWidget* m_navHost = nullptr;
    QVBoxLayout* m_navLay = nullptr;
    QLineEdit* m_search = nullptr;
    QVector<NavEntry> m_navEntries;
    QVector<QPair<QString, QLabel*>> m_sectionLabels; // 分组标题（过滤时整体隐藏）
    QLabel* m_emptyHint = nullptr;                    // 搜索无结果提示
    int m_currentPage = 0;
    QStackedWidget* m_pages = nullptr;

    void loadHive();                    // 读 config/hive.json（主密钥为 DPAPI 密文）
    void saveHive();                    // 写回（密钥输入非空才覆盖密文）

    // 通用
    QComboBox* m_permCombo = nullptr;
    QComboBox* m_paletteCombo = nullptr; // 主题皮肤（保存后重启生效）
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
    QCheckBox* m_memApproval = nullptr; // Hermes write_approval 记忆写入审批门
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
