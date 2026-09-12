// 左栏（对齐 DSH/Codex 单列信息架构）：品牌行 → 搜索 → 新建会话 → 最近会话 → 设置 + L1 占用。
//
// 设计取舍：**不把功能页平铺在左栏**。原先「任务队列/技能库/记忆/供应商/审计日志」五项文字
// 常驻，占了近 200px、视觉重量压过会话列表本身，而这些都属于低频的配置/监控页。
// 现在统一收进设置对话框（左导航 + 堆叠页），左栏只保留高频的"会话"这件事——
// 与 Codex（New chat + Projects + Recents）和 DSH（工作区 + 会话）的信息架构一致。
#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace miderforge {

class Database;

class SidebarView : public QWidget {
    Q_OBJECT
public:
    explicit SidebarView(QWidget* parent = nullptr);

    // 会话列表（数据仍由 SessionView 负责，这里只做展示与点击转发）
    void loadSessions(Database* db);
    void setCurrentSession(qint64 id);
    // 空态提示：无历史会话时显示（Codex 的 "No chats" 同款）
    void setSessionEmptyHint(const QString& text);
    // 会话搜索框聚焦（Ctrl+K / 命令面板「搜索会话」跳转用）
    void focusSessionSearch();

    // 品牌行右侧的菜单按钮（MainWindow 挂主题/退出等动作）
    QToolButton* menuButton() const { return m_menuBtn; }

    // 顶部工作区行：显示当前工作目录名，点击触发 workspaceChangeRequested
    void setWorkspaceLabel(const QString& fullPath);

    // 侧栏底部 L1 核心记忆占用
    void setL1Usage(long long used, long long cap);

signals:
    void newSessionRequested();
    void sessionActivated(qint64 id);
    // 「搜索」行被点击 → 打开命令面板（Ctrl+K 的可见入口，否则纯快捷键没人发现得了）
    void searchRequested();
    // 底部「设置」→ 打开设置对话框（功能面板都收在里面）
    void settingsRequested();
    // 顶部工作区行被点击 → 主窗口弹目录选择器
    void workspaceChangeRequested();
    // 会话右键菜单动作（标题用原文，删除需上层确认后执行）
    void sessionRenameRequested(qint64 id, const QString& newTitle);
    void sessionDeleteRequested(qint64 id);
    // 会话归档 / 取消归档（消息与索引原样保留，仅从默认列表隐藏）
    void sessionArchiveRequested(qint64 id, bool archive);

private:
    void filterSessions(const QString& query); // 按标题子串切换行可见性（重载后保持过滤）
    void showSessionMenu(const QPoint& pos);   // 会话行右键菜单
    void beginRenameSession(QListWidgetItem* item);
    void beginRenameById(qint64 id); // 按 id 重命名：旧标题从库读，不依赖列表项指针

    QPushButton* m_newBtn = nullptr;
    QLineEdit* m_search = nullptr;      // 会话搜索/过滤
    QPushButton* m_wsBtn = nullptr;     // 工作区行（显示当前目录名，点击换目录）
    QListWidget* m_sessionList = nullptr;
    Database* m_db = nullptr;        // 会话列表数据源（重命名取权威旧标题也用它）
    bool m_showArchived = false;     // 「显示已归档」开关
    QToolButton* m_archivedToggle = nullptr;
    bool m_menuOpen = false; // 右键菜单开着时禁止重建列表（避免删掉菜单处理器持有的行）
    qint64 m_currentId = -1; // 应当高亮的会话 id（重建列表后据此恢复高亮）
    QLabel* m_recentHeader = nullptr;
    QLabel* m_sessionEmpty = nullptr;
    QLabel* m_l1Label = nullptr;
    QProgressBar* m_l1Bar = nullptr;
    QToolButton* m_menuBtn = nullptr;
};

} // namespace miderforge
