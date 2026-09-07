// Agent 核心循环（M1 完整 ReAct 状态机）：PLANNING→EXECUTING→OBSERVING→[下轮|DONE|HALTED]，
// 三重熔断保险 + 权限门 + 事件审计；记忆/技能注入自 M2/M3 接入
#pragma once
#include "core/Breaker.h"
#include "llm/ChatClient.h"
#include "tools/PermissionGate.h"
#include <QJsonArray>
#include <QObject>
#include <QString>

namespace miderforge {

class ChatClient;
class EventBus;
class ToolRegistry;
class ProviderManager;

class AgentLoop : public QObject {
    Q_OBJECT
public:
    // ReAct 状态机
    enum class State {
        Idle,               // 空闲
        Planning,           // 规划中（LLM 思考）
        Executing,          // 执行中（工具调用）
        Observing,          // 观察中（结果回填）
        AwaitingPermission, // 等待用户授权
        Done,               // 已完成
        Failed,             // 失败
        Halted              // 已熔断
    };
    Q_ENUM(State)

    struct Deps {
        ChatClient* chat = nullptr;
        ToolRegistry* tools = nullptr;
        ProviderManager* providers = nullptr;
        EventBus* events = nullptr;
    };
    AgentLoop(Deps deps, QObject* parent = nullptr);

    void start(const QString& goal, qint64 taskId = -1);
    void cancel();
    bool isRunning() const { return m_running; }
    long long totalTokens() const { return m_breaker.tokens(); }

    // 权限卡片回调：decision 0=允许一次 1=本会话总是允许 2=拒绝
    void resumePermission(const QString& callId, int decision);

    static QString stateName(State s);

signals:
    void taskStarted(const QString& goal);
    void stateChanged(const QString& stateText); // 规划中/执行中/等待确认/已完成/已熔断…
    void roundChanged(int round, int maxRounds);
    void tokensChanged(long long total);
    // 流事件转发
    void streamResetUi();
    void streamRetrying(const QString& reason);
    void thinkingDelta(const QString& delta);
    void contentDelta(const QString& delta);
    void toolCallDelta(int index, const QString& id, const QString& name, const QString& argsSoFar);
    void toolCallStarted(const QString& callId, const QString& name, const QString& args);
    void toolCallFinished(const QString& callId, bool ok, const QString& resultText, qint64 ms);
    // 权限确认请求（UI 弹卡片）：riskNote = 操作类型+风险说明，target = 路径/命令全文
    void toolAwaitingConfirm(const QString& callId, const QString& toolName, const QString& riskNote,
                             const QString& target);
    void loopFinished(bool ok, const QString& summary);
    void loopFailed(const QString& error);

private slots:
    void onStreamFinished(const miderforge::StreamResult& result);
    void onStreamFailed(const QString& error, int httpCode, bool willRetry);

private:
    void setState(State s);
    void runRound();
    void finishToolBatch(); // 工具批执行完：熔断检查 → 下一轮或继续
    void executePendingTool(int decision);
    void executeToolCall(const QString& callId, const QString& name, const QString& argumentsJson);
    QString buildSystemPrompt() const;
    QString classifyTarget(const QString& toolName, const QString& argsJson, PermissionGate::Kind* kind) const;

    Deps m_deps;
    PermissionGate m_gate; // 会话级权限门（含"总是允许"记忆）
    Breaker m_breaker;
    QJsonArray m_history;
    QString m_goal;
    qint64 m_taskId = -1;
    int m_round = 0;
    bool m_running = false;
    State m_state = State::Idle;

    // 待授权工具调用（AwaitingPermission 状态下挂起）
    QString m_pendingCallId;
    QString m_pendingToolName;
    QString m_pendingArgs;
};

} // namespace miderforge
