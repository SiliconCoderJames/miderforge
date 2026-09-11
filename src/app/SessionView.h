// 会话视图（规格 4.3）：顶部变更审查条 + 中部消息流（用户/助手/工具卡片）+ 底部 Composer；
// 布局对齐 Codex/ZCode：顶栏极简、无独立工具栏，权限档/供应商/模型档全部内联在输入框底行；
// 流式渲染经 AssistantBlock 内部 200ms 批量刷新；任务在跑时发送自动入队不打断
#pragma once
#include "app/ChatWidgets.h"
#include "core/AgentLoop.h"
#include <QBoxLayout>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QPushButton;
class QVBoxLayout;

namespace miderforge {

class Database;
class ProviderManager;

class SessionView : public QWidget {
    Q_OBJECT
public:
    SessionView(Database* db, AgentLoop* loop, ProviderManager* pm, QWidget* parent = nullptr);

    void newSession();
    void focusInput();
    // 会话列表点击（左栏 SidebarView 转发）→ 切换/恢复会话；被拒绝（任务执行中）返回 false
    bool requestSwitchSession(qint64 id);
    qint64 currentSessionId() const { return m_currentSessionId; }

    // 空态引导卡点击 → 填入输入框（不自动发送，用户可再编辑）
    void fillInput(const QString& text);

protected:
    // 输入框键位：Enter 发送（ZCode/Codex 习惯）、Shift+Enter 换行、Ctrl+Enter 兼容旧习惯
    bool eventFilter(QObject* obj, QEvent* ev) override;

signals:
    void queueCountChanged(int n);
    // 权限模式变更（输入区下拉框 → 设置对话框同步）
    void permissionModeChanged(int mode);
    // 会话列表需刷新（建档/新消息/切换会话）→ 左栏重载
    void sessionsChanged();
    // 当前会话变化（-1 = 未建档的新会话）→ 左栏高亮同步
    void currentSessionChanged(qint64 id);

public slots:
    // 外部（设置对话框）改权限档 → 输入区同步
    void setPermissionMode(int mode);
    // 供应商切换或配置变化后刷新输入区下拉（ProviderManager 写盘后由主窗口调用）
    void refreshProviderCombo();

private slots:
    void onSend();
    void onStop();
    void onPauseToggled(); // M5 P2：暂停/继续切换
    void onNewSessionClicked();
    void onPickAttachment();     // Composer：附加上下文文件
    void onChangedFileClicked(const QString& path); // 变更条点击 → 查看 diff
    void onProviderChanged(int index);

private:
    QWidget* buildTopStrip();                       // 变更审查条 + 进度指标一行
    QWidget* buildInputArea();
    QWidget* buildComposerMetaRow(QWidget* parent); // 输入框底行：附件/权限/供应商/模型档
    void buildEmptyState();                         // 消息流空态引导卡
    void buildScrollToBottom();                     // 长对话「回到底部」悬浮按钮
    void updateScrollButton();                      // 仅在离开底部时显示
    void refreshChangesBar();                       // m_changes → 变更审查条
    ToolCallCard* makeCard(const QString& callId, const QString& toolName);
    void appendToFeed(QWidget* w);
    void scrollToEnd();
    void setStateLabel(const QString& text, const QColor& color);
    void startGoal(const QString& goal);
    void popQueueIfIdle();
    // 会话持久化（Claude/Codex 式会话列表）
    void ensureSession(const QString& firstGoal);
    void persistMessage(const char* role, const QString& content);
    void clearFeed();
    bool switchToSession(qint64 id);

    // AgentLoop 事件
    void onTaskStarted(const QString& goal);
    void onStateChanged(const QString& stateText);
    void onToolAwaitingConfirm(const QString& callId, const QString& toolName, const QString& riskNote,
                               const QString& target);
    void onRoundChanged(int round, int maxRounds);
    void onToolCallDelta(int index, const QString& id, const QString& name, const QString& args);
    void onToolCallStarted(const QString& callId, const QString& name, const QString& args);
    void onToolCallFinished(const QString& callId, bool ok, const QString& resultText, qint64 ms);
    void onLoopFinished(bool ok, const QString& summary);
    void onLoopFailed(const QString& error);
    void onStreamRetrying(const QString& reason);

    AgentLoop* m_loop = nullptr;
    Database* m_db = nullptr;
    ProviderManager* m_pm = nullptr;

    // 会话 id（左栏 SidebarView 负责列表展示/高亮，这里只持有当前会话状态）
    qint64 m_currentSessionId = -1;

    // 线程内变更审查（Codex 式）：顶部一条，取代原右侧 180px 固定栏
    struct ChangeInfo {
        int added = 0;
        int removed = 0;
        QString diff;
    };
    ChangeSummaryBar* m_changesBar = nullptr;
    QHash<QString, QString> m_callPaths; // callId → write_file 路径
    QHash<QString, ChangeInfo> m_changes; // path → 变更信息

    // Composer 供应商/权限/模型档
    QComboBox* m_providerCombo = nullptr;
    QLabel* m_modelLabel = nullptr; // 当前模型档（只读展示，三档路由自动选）

    // Composer 附件上下文
    QStringList m_attachedFiles;
    QWidget* m_chipsHost = nullptr;
    QHBoxLayout* m_chipsLay = nullptr;

    // 顶部进度条（从 Composer 底行上移：轮次/tokens/已用时/状态）
    QFrame* m_topStrip = nullptr;
    QLabel* m_goalLabel = nullptr;
    QLabel* m_roundLabel = nullptr;
    QLabel* m_tokensLabel = nullptr;
    QLabel* m_elapsedLabel = nullptr;
    QLabel* m_stateLabel = nullptr;
    QTimer m_elapsedTimer;
    QElapsedTimer m_taskClock;

    // 消息流
    QScrollArea* m_scroll = nullptr;
    QWidget* m_feedHost = nullptr;
    QVBoxLayout* m_feedLay = nullptr;
    QWidget* m_emptyState = nullptr; // 空态引导卡（有消息后隐藏）
    QToolButton* m_scrollBottomBtn = nullptr; // 「回到底部」悬浮按钮（长对话用）

    // 输入区
    QPlainTextEdit* m_input = nullptr;
    QComboBox* m_permCombo = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QPushButton* m_pauseBtn = nullptr; // M5 P2：暂停/继续

    // 流式过程中的临时状态
    AssistantBlock* m_curAssistant = nullptr;
    QMap<int, ToolCallCard*> m_roundCards; // 同一轮内 index → 卡片
    QMap<QString, ToolCallCard*> m_idCards; // call_id → 卡片
    QStringList m_pendingQueue;             // M0 简易任务队列（M2 迁入数据库任务表）
};

} // namespace miderforge
