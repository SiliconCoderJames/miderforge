// Agent 核心循环（M0 简版）：多轮「对话→工具调用→结果回填」直到模型给出最终答复；
// M1 扩展为完整 ReAct 状态机（权限门/沙箱/熔断器/审计），M2 接入记忆注入
#pragma once
#include "llm/ChatClient.h"
#include <QJsonArray>
#include <QObject>
#include <QString>

namespace miderforge {

class ChatClient;
class ToolRegistry;
class ProviderManager;

class AgentLoop : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxRounds = 25; // 轮数上限（默认 25，达到即熔断）

    struct Deps {
        ChatClient* chat = nullptr;
        ToolRegistry* tools = nullptr;
        ProviderManager* providers = nullptr;
    };
    AgentLoop(Deps deps, QObject* parent = nullptr);

    void start(const QString& goal);
    void cancel();
    bool isRunning() const { return m_running; }
    long long totalTokens() const { return m_totalTokens; }

signals:
    void taskStarted(const QString& goal);
    void roundChanged(int round, int maxRounds);
    void tokensChanged(long long total);
    // ChatClient 流事件的透明转发（UI 直接连这些）
    void streamResetUi();
    void streamRetrying(const QString& reason);
    void thinkingDelta(const QString& delta);
    void contentDelta(const QString& delta);
    void toolCallDelta(int index, const QString& id, const QString& name, const QString& argsSoFar);
    void toolCallStarted(const QString& callId, const QString& name, const QString& args);
    void toolCallFinished(const QString& callId, bool ok, const QString& resultText, qint64 ms);
    void loopFinished(bool ok, const QString& summary);
    void loopFailed(const QString& error);

private slots:
    void onStreamFinished(const miderforge::StreamResult& result);
    void onStreamFailed(const QString& error, int httpCode, bool willRetry);

private:
    void runRound();
    QString buildSystemPrompt() const;

    Deps m_deps;
    QJsonArray m_history;
    QString m_goal;
    int m_round = 0;
    bool m_running = false;
    long long m_totalTokens = 0;
};

} // namespace miderforge
