// 主窗口（规格 4.1/4.2/4.9 骨架）：菜单栏+工具栏+左侧导航+中央堆叠区+状态栏；
// M0 实装会话视图，其余面板随对应里程碑（M1 审计/M2 任务与记忆/M3 技能/M4 供应商与托盘）逐步实装
#pragma once
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTreeWidget>

class QAction;
class QPushButton;

namespace miderforge {

class AdjudicationService;
class AgentLoop;
class AuditLogView;
class ChatClient;
class Database;
class EmailNotifier;
class EventBus;
class MemoryManager;
class ProviderManager;
class SessionView;
class SkillManager;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ProviderManager* pm, AgentLoop* loop, EventBus* events, Database* db,
               MemoryManager* mem, SkillManager* skills, EmailNotifier* mail,
               QWidget* parent = nullptr);

private slots:
    void switchNav(int index);
    void openSettings();
    void onProviderComboChanged(int index);

private:
    void buildMenus();
    void buildToolbar();
    void buildCentral();
    void buildStatusBar();
    void refreshProviderCombo();
    void refreshStatusLabels();
    void refreshL1Footer();
    QWidget* makePlaceholder(const QString& text) const;

    ProviderManager* m_pm = nullptr;
    AgentLoop* m_loop = nullptr;
    EventBus* m_events = nullptr;
    Database* m_db = nullptr;
    MemoryManager* m_mem = nullptr;
    SkillManager* m_skills = nullptr;
    EmailNotifier* m_mail = nullptr;
    QSystemTrayIcon* m_tray = nullptr;

    QTreeWidget* m_nav = nullptr;
    QStackedWidget* m_stack = nullptr;
    SessionView* m_sessionView = nullptr;
    QComboBox* m_providerCombo = nullptr;
    QProgressBar* m_l1Bar = nullptr;
    QLabel* m_l1BarLabel = nullptr;
    QLabel* m_statusModel = nullptr;
    QLabel* m_statusTokens = nullptr;
    QLabel* m_statusMode = nullptr;
    QLabel* m_statusQueue = nullptr;
    QAction* m_stopAction = nullptr;
    AdjudicationService* m_adjudicator = nullptr; // 矛盾扫描 v2：LLM 语义裁决（独立 ChatClient）
};

} // namespace miderforge
