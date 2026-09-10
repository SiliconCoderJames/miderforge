// 会话视图（规格 4.3）：顶部任务状态条 + 中部消息流（用户/助手/工具卡片）+ 底部输入区；
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

class SessionView : public QWidget {
    Q_OBJECT
public:
    SessionView(Database* db, AgentLoop* loop, QWidget* parent = nullptr);

    void newSession();
    void focusInput();

protected:
    // 输入框键位：Enter 发送（ZCode/Codex 习惯）、Shift+Enter 换行、Ctrl+Enter 兼容旧习惯
    bool eventFilter(QObject* obj, QEvent* ev) override;

signals:
    void queueCountChanged(int n);
    // 权限模式变更（输入区下拉框 → 工具栏同步）
    void permissionModeChanged(int mode);

public slots:
    // 工具栏下拉框 → 输入区同步
    void setPermissionMode(int mode);

private slots:
    void onSend();
    void onStop();
    void onPauseToggled(); // M5 P2：暂停/继续切换
    void onSessionSelected(); // 会话列表点击 → 切换/恢复会话
    void onNewSessionClicked();
    void onPickAttachment();     // Composer：📎 附加上下文文件
    void onChangedFileClicked(QListWidgetItem* item); // 变更树点击 → 查看 diff

private:
    QWidget* buildStatusStrip();
    QWidget* buildInputArea();
    QWidget* buildSessionPanel();
    QWidget* buildChangesPanel();
    ToolCallCard* makeCard(const QString& callId, const QString& toolName);
    void appendToFeed(QWidget* w);
    void scrollToEnd();
    void setStateLabel(const QString& text, const QColor& color);
    void startGoal(const QString& goal);
    void popQueueIfIdle();
    // 会话持久化（Claude/Codex 式会话列表）
    void ensureSession(const QString& firstGoal);
    void persistMessage(const char* role, const QString& content);
    void loadSessions();
    void refreshSessionList();
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

    // 会话列表面板（Claude/Codex 式）
    QListWidget* m_sessionList = nullptr;
    qint64 m_currentSessionId = -1;

    // 变更文件右栏（Codex 式线程内变更审查）：write_file 调用 → 路径/diff 归档
    struct ChangeInfo {
        int added = 0;
        int removed = 0;
        QString diff;
    };
    QListWidget* m_changesList = nullptr;
    QLabel* m_changesTitle = nullptr;
    QHash<QString, QString> m_callPaths; // callId → write_file 路径
    QHash<QString, ChangeInfo> m_changes; // path → 变更信息

    // Composer 附件上下文
    QStringList m_attachedFiles;
    QWidget* m_attachRow = nullptr;
    QWidget* m_chipsHost = nullptr;
    QHBoxLayout* m_chipsLay = nullptr;

    // 顶部状态条
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
