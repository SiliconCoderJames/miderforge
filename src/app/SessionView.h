// 会话视图（规格 4.3）：顶部任务状态条 + 中部消息流（用户/助手/工具卡片）+ 底部输入区；
// 流式渲染经 AssistantBlock 内部 200ms 批量刷新；任务在跑时发送自动入队不打断
#pragma once
#include "app/ChatWidgets.h"
#include "core/AgentLoop.h"
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QTimer>
#include <QWidget>

class QPushButton;
class QVBoxLayout;

namespace miderforge {

class SessionView : public QWidget {
    Q_OBJECT
public:
    SessionView(AgentLoop* loop, QWidget* parent = nullptr);

    void newSession();
    void focusInput();

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

private:
    QWidget* buildStatusStrip();
    QWidget* buildInputArea();
    ToolCallCard* makeCard(const QString& callId, const QString& toolName);
    void appendToFeed(QWidget* w);
    void scrollToEnd();
    void setStateLabel(const QString& text, const QColor& color);
    void startGoal(const QString& goal);
    void popQueueIfIdle();

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

    // 流式过程中的临时状态
    AssistantBlock* m_curAssistant = nullptr;
    QMap<int, ToolCallCard*> m_roundCards; // 同一轮内 index → 卡片
    QMap<QString, ToolCallCard*> m_idCards; // call_id → 卡片
    QStringList m_pendingQueue;             // M0 简易任务队列（M2 迁入数据库任务表）
};

} // namespace miderforge
