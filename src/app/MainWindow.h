// 主窗口（规格 4.1/4.2/4.9 骨架）：菜单栏 + 单列左栏（SidebarView）+ 中央堆叠区 + 状态栏；
// 布局对齐 Codex/ZCode：无独立工具栏，权限/供应商/模型档在 Composer 底行，导航在左栏
#pragma once
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QStackedWidget>
#include <QStringList>
#include <QSystemTrayIcon>

class QAction;

namespace miderforge {

class AdjudicationService;
class AgentLoop;
class AuditLogView;
class ChatClient;
class CommandPalette;
class Database;
class EmailNotifier;
class EventBus;
class MemoryManager;
class MemoryView;
class PreviewPane;
class ProviderManager;
class ProviderPanel;
class SessionView;
class SettingsDialog;
class SidebarView;
class SkillManager;
class SkillView;
class TaskQueueView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ProviderManager* pm, AgentLoop* loop, EventBus* events, Database* db,
               MemoryManager* mem, SkillManager* skills, EmailNotifier* mail,
               QWidget* parent = nullptr);

private slots:
    void switchNav(int index);
    void openSettings();

private:
    void buildMenus();
    void buildCentral();
    void buildStatusBar();
    void buildShortcuts();          // 全局快捷键：Ctrl+K/L/F/,
    void openCommandPalette();      // Ctrl+K 命令面板（会话 + 动作）
    void restoreLastSession();      // 启动恢复最近会话（无历史则保持空态引导卡）
    void openSettingsAt(int pageIndex); // 打开设置并直达某面板页（pageIndex<0 = 默认页）
    void openPreview();                 // 打开内置查看器（HTML / Markdown / 文本）
    void refreshStatusLabels();
    void refreshL1Footer();
    void applyPermissionMode(int idx); // 设置对话框 → Composer 权限下拉
    void refreshProviderCombo();       // 设置对话框关闭后转发给 Composer 刷新供应商下拉

    ProviderManager* m_pm = nullptr;
    AgentLoop* m_loop = nullptr;
    EventBus* m_events = nullptr;
    Database* m_db = nullptr;
    MemoryManager* m_mem = nullptr;
    SkillManager* m_skills = nullptr;
    EmailNotifier* m_mail = nullptr;
    QSystemTrayIcon* m_tray = nullptr;

    QWidget* m_activityBar = nullptr;   // 保留占位：活动栏已并入 SidebarView（见 buildCentral）
    QStringList m_pageNames;            // 页面名（视图菜单用，含「设置」）
    QStackedWidget* m_stack = nullptr;
    SessionView* m_sessionView = nullptr;
    SidebarView* m_sidebar = nullptr;   // 单列左栏：品牌/搜索/新建/最近会话/设置/L1
    // 功能面板：本窗口持有，设置对话框借去当堆叠页（见 buildCentral 的说明）
    TaskQueueView* m_taskQueuePage = nullptr;
    SkillView* m_skillPage = nullptr;
    MemoryView* m_memoryPage = nullptr;
    ProviderPanel* m_providerPage = nullptr;
    AuditLogView* m_auditPage = nullptr;
    SettingsDialog* m_settings = nullptr; // 持有：嵌入内容区，避免每次进入都重挂面板
    int m_settingsPageIndex = -1;         // 设置页在 m_stack 中的索引
    PreviewPane* m_previewPage = nullptr; // 内置查看器（HTML/Markdown/文本）
    int m_previewIndex = -1;              // 查看器页在 m_stack 中的索引
    // 说明：权限档/供应商/停止/新建会话的控件均已迁至 Composer 与左栏，此处不再持有
    QLabel* m_statusTokens = nullptr;
    QLabel* m_statusQueue = nullptr;
    int m_queueCount = 0;               // 队列真实计数（由 SessionView 信号驱动，不再被硬写 0）
    AdjudicationService* m_adjudicator = nullptr; // 矛盾扫描 v2：LLM 语义裁决（独立 ChatClient）
};

} // namespace miderforge
