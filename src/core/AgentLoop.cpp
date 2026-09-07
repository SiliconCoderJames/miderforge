// AgentLoop 实现（M0 简版）
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "llm/ProviderManager.h"
#include "tools/ToolRegistry.h"
#include "util/Log.h"
#include <QJsonObject>
#include <spdlog/spdlog.h>

namespace miderforge {

AgentLoop::AgentLoop(Deps deps, QObject* parent) : QObject(parent), m_deps(deps) {
    connect(m_deps.chat, &ChatClient::streamReset, this, &AgentLoop::streamResetUi);
    connect(m_deps.chat, &ChatClient::thinkingDelta, this, &AgentLoop::thinkingDelta);
    connect(m_deps.chat, &ChatClient::contentDelta, this, &AgentLoop::contentDelta);
    connect(m_deps.chat, &ChatClient::toolCallDelta, this, &AgentLoop::toolCallDelta);
    connect(m_deps.chat, &ChatClient::finished, this, &AgentLoop::onStreamFinished);
    connect(m_deps.chat, &ChatClient::failed, this, &AgentLoop::onStreamFailed);
}

void AgentLoop::start(const QString& goal) {
    if (m_running) {
        emit loopFailed(QStringLiteral("已有任务在执行中"));
        return;
    }
    m_goal = goal;
    m_history = QJsonArray();
    m_round = 0;
    m_totalTokens = 0;
    m_running = true;
    emit taskStarted(goal);
    runRound();
}

void AgentLoop::cancel() {
    if (m_running)
        m_deps.chat->cancel();
}

QString AgentLoop::buildSystemPrompt() const {
    // M0：角色设定 + 工具说明 + 当前任务。M2 起按规格插入 L1/相关记忆/会话摘要/可用技能段。
    QString prompt = QStringLiteral(
        "你是运行在用户 Windows 电脑上的 Miderforge 编码 Agent，目标领域是 C++/CMake/Qt 项目开发。\n"
        "以中文思考和回答。需要读取文件等操作时调用提供的工具；工具执行结果会以 role=tool 消息回填。\n"
        "任务完成后直接给出最终答复（不再调用工具）。\n");
    prompt += QStringLiteral("\n可用工具：\n");
    if (m_deps.tools) {
        const QJsonArray schemas = m_deps.tools->openAiSchemas();
        for (const auto& v : schemas) {
            const QJsonObject fn = v.toObject().value("function").toObject();
            prompt += QStringLiteral("- %1：%2\n")
                          .arg(fn.value("name").toString(), fn.value("description").toString());
        }
    }
    prompt += QStringLiteral("\n当前任务：%1\n工作区：%2\n当前轮次：%3/%4\n")
                  .arg(m_goal, AppContext::instance().workspaceRoot)
                  .arg(m_round)
                  .arg(kMaxRounds);
    return prompt;
}

void AgentLoop::runRound() {
    ++m_round;
    emit roundChanged(m_round, kMaxRounds);

    const ProviderConfig* provider = m_deps.providers ? m_deps.providers->activeProvider() : nullptr;
    if (!provider) {
        m_running = false;
        emit loopFailed(QStringLiteral("未配置任何大模型供应商，请先完成首次配置"));
        return;
    }
    // 决策: M0 全部走 main 档；fast/flagship 的三档路由在 M4 接入 Router
    const QString model = m_deps.providers->modelForTier(*provider, QStringLiteral("main"));

    // 重组消息：系统提示词（含当前轮次）+ 历史
    QJsonArray messages;
    QJsonObject sys;
    sys.insert("role", "system");
    sys.insert("content", buildSystemPrompt());
    messages.append(sys);
    for (const auto& m : m_history)
        messages.append(m);

    AppContext::instance().activeModelLabel =
        QStringLiteral("%1·%2").arg(provider->name, model);
    m_deps.chat->start(*provider, model, messages, m_deps.tools ? m_deps.tools->openAiSchemas() : QJsonArray());
}

void AgentLoop::onStreamFinished(const StreamResult& result) {
    if (!m_running)
        return;

    if (result.aborted) {
        m_running = false;
        emit loopFinished(false, QStringLiteral("已手动停止"));
        return;
    }

    m_totalTokens += result.promptTokens + result.completionTokens;
    AppContext::instance().todayTokens += result.promptTokens + result.completionTokens;
    emit tokensChanged(m_totalTokens);

    // ---- 组装 assistant 消息写入历史 ----
    QJsonObject assistantMsg;
    assistantMsg.insert("role", "assistant");
    if (result.content.isEmpty() && !result.toolCalls.isEmpty())
        assistantMsg.insert("content", QJsonValue::Null); // 工具轮允许空正文
    else
        assistantMsg.insert("content", result.content);
    if (!result.toolCalls.isEmpty()) {
        QJsonArray calls;
        for (const auto& tc : result.toolCalls) {
            calls.append(QJsonObject{
                {"id", tc.id.isEmpty() ? QStringLiteral("call_%1").arg(tc.index) : tc.id},
                {"type", "function"},
                {"function", QJsonObject{
                                 {"name", tc.name},
                                 {"arguments", tc.arguments.isEmpty() ? QStringLiteral("{}") : tc.arguments},
                             }},
            });
        }
        assistantMsg.insert("tool_calls", calls);
    }
    m_history.append(assistantMsg);

    // ---- 纯文本答复：任务完成 ----
    if (result.toolCalls.isEmpty()) {
        m_running = false;
        emit loopFinished(true, result.content);
        return;
    }

    // ---- 工具轮：逐个执行并回填 role=tool ----
    for (const auto& tc : result.toolCalls) {
        const QString callId = tc.id.isEmpty() ? QStringLiteral("call_%1").arg(tc.index) : tc.id;
        emit toolCallStarted(callId, tc.name, tc.arguments);
        QString err;
        QString resultText;
        qint64 ms = 0;
        if (m_deps.tools) {
            const auto execRes = m_deps.tools->execute(tc.name, tc.arguments);
            resultText = execRes.text;
            ms = execRes.ms;
            emit toolCallFinished(callId, execRes.ok, resultText, ms);
        } else {
            err = QStringLiteral("工具注册表不可用");
            resultText = QStringLiteral("错误：") + err;
            emit toolCallFinished(callId, false, resultText, 0);
        }
        m_history.append(QJsonObject{
            {"role", "tool"},
            {"tool_call_id", callId},
            {"content", resultText},
        });
    }

    // 简化熔断（M1 扩展为完整三重保险）：达到轮数上限即停
    if (m_round >= kMaxRounds) {
        m_running = false;
        emit loopFinished(false, QStringLiteral("已达到 %1 轮上限，任务熔断").arg(kMaxRounds));
        return;
    }
    runRound();
}

void AgentLoop::onStreamFailed(const QString& error, int httpCode, bool willRetry) {
    if (!m_running)
        return;
    if (willRetry) {
        emit streamRetrying(error);
        return;
    }
    m_running = false;
    emit loopFailed(error + (httpCode > 0 ? QStringLiteral("（HTTP %1）").arg(httpCode) : QString()));
}

} // namespace miderforge
