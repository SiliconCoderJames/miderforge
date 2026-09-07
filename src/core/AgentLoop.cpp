// AgentLoop 实现（M1 完整状态机）
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/EventBus.h"
#include "llm/ProviderManager.h"
#include "tools/FileTools.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include "util/Log.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <spdlog/spdlog.h>

namespace miderforge {

QString AgentLoop::stateName(State s) {
    switch (s) {
    case State::Idle: return QStringLiteral("空闲");
    case State::Planning: return QStringLiteral("规划中");
    case State::Executing: return QStringLiteral("执行中");
    case State::Observing: return QStringLiteral("观察中");
    case State::AwaitingPermission: return QStringLiteral("等待确认");
    case State::Done: return QStringLiteral("已完成");
    case State::Failed: return QStringLiteral("失败");
    case State::Halted: return QStringLiteral("已熔断");
    }
    return QStringLiteral("空闲");
}

AgentLoop::AgentLoop(Deps deps, QObject* parent)
    : QObject(parent), m_deps(deps) {
    connect(m_deps.chat, &ChatClient::streamReset, this, &AgentLoop::streamResetUi);
    connect(m_deps.chat, &ChatClient::thinkingDelta, this, &AgentLoop::thinkingDelta);
    connect(m_deps.chat, &ChatClient::contentDelta, this, &AgentLoop::contentDelta);
    connect(m_deps.chat, &ChatClient::toolCallDelta, this, &AgentLoop::toolCallDelta);
    connect(m_deps.chat, &ChatClient::finished, this, &AgentLoop::onStreamFinished);
    connect(m_deps.chat, &ChatClient::failed, this, &AgentLoop::onStreamFailed);
}

void AgentLoop::setState(State s) {
    m_state = s;
    emit stateChanged(stateName(s));
}

void AgentLoop::start(const QString& goal, qint64 taskId) {
    if (m_running) {
        emit loopFailed(QStringLiteral("已有任务在执行中"));
        return;
    }
    m_goal = goal;
    m_taskId = taskId;
    m_history = QJsonArray();
    m_round = 0;
    m_running = true;
    m_breaker = Breaker(); // 每任务独立预算
    m_gate.resetSessionGrants(); // 决策: "总是允许"随任务失效（会话粒度的保守实现）
    m_pendingCallId.clear();
    emit taskStarted(goal);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", "started"}, {"goal", goal}});
    setState(State::Planning);
    runRound();
}

void AgentLoop::cancel() {
    if (!m_running)
        return;
    if (m_state == State::AwaitingPermission) {
        // 授权挂起时取消：直接判停
        m_running = false;
        m_pendingCallId.clear();
        setState(State::Failed);
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                                  QJsonObject{{"status", "cancelled"}});
        emit loopFinished(false, QStringLiteral("已手动停止"));
        return;
    }
    m_deps.chat->cancel();
}

void AgentLoop::runRound() {
    m_breaker.beginRound();
    ++m_round;
    emit roundChanged(m_round, m_breaker.limits().maxRounds);
    setState(State::Planning);

    const ProviderConfig* provider = m_deps.providers ? m_deps.providers->activeProvider() : nullptr;
    if (!provider) {
        m_running = false;
        setState(State::Failed);
        emit loopFailed(QStringLiteral("未配置任何大模型供应商，请先完成首次配置"));
        return;
    }
    // 决策: M1 全部走 main 档；fast/flagship 三档路由在 M4 接入 Router
    const QString model = m_deps.providers->modelForTier(*provider, QStringLiteral("main"));

    if (m_deps.events)
        m_deps.events->append(QStringLiteral("llm_request"), m_taskId,
                              QJsonObject{{"provider", provider->name}, {"model", model},
                                          {"round", m_round}});

    QJsonArray messages;
    QJsonObject sys;
    sys.insert("role", "system");
    sys.insert("content", buildSystemPrompt());
    messages.append(sys);
    for (const auto& m : m_history)
        messages.append(m);

    AppContext::instance().activeModelLabel = QStringLiteral("%1·%2").arg(provider->name, model);
    m_deps.chat->start(*provider, model, messages,
                       m_deps.tools ? m_deps.tools->openAiSchemas() : QJsonArray());
}

QString AgentLoop::buildSystemPrompt() const {
    // M1：角色设定 + 工具说明 + 当前任务。M2 起插入 L1/相关记忆/会话摘要段；M3 起插入可用技能段。
    QString prompt = QStringLiteral(
        "你是运行在用户 Windows 电脑上的 Miderforge 编码 Agent，目标领域是 C++/CMake/Qt 项目开发。\n"
        "以中文思考和回答。需要操作文件/执行命令时调用提供的工具；工具结果以 role=tool 消息回填。\n"
        "任务完成后直接给出最终答复（不再调用工具）。\n"
        "注意：write_file 必须给出文件完整新内容；工作区外的写入会被拒绝。\n");
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
                  .arg(m_breaker.limits().maxRounds);
    return prompt;
}

QString AgentLoop::classifyTarget(const QString& toolName, const QString& argsJson,
                                  PermissionGate::Kind* kind) const {
    const QJsonObject args = QJsonDocument::fromJson(argsJson.toUtf8()).object();
    const QString ws = AppContext::instance().workspaceRoot;
    if (toolName == QLatin1String("write_file")) {
        *kind = PermissionGate::Kind::WriteFile;
        return FileTools::resolveWorkspacePath(args.value("path").toString(), ws);
    }
    if (toolName == QLatin1String("run_command")) {
        *kind = PermissionGate::Kind::RunCommand;
        return args.value("command").toString();
    }
    if (toolName == QLatin1String("http_fetch")) {
        *kind = PermissionGate::Kind::Network;
        return args.value("url").toString();
    }
    *kind = PermissionGate::Kind::ReadFile; // read_file/list_dir/search_files/read_skill 自动
    return FileTools::resolveWorkspacePath(args.value("path").toString(), ws);
}

void AgentLoop::onStreamFinished(const StreamResult& result) {
    if (!m_running)
        return;

    if (result.aborted) {
        m_running = false;
        setState(State::Failed);
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                                  QJsonObject{{"status", "cancelled"}});
        emit loopFinished(false, QStringLiteral("已手动停止"));
        return;
    }

    m_breaker.addTokens(result.promptTokens + result.completionTokens);
    AppContext::instance().todayTokens += result.promptTokens + result.completionTokens;
    emit tokensChanged(m_breaker.tokens());

    // ---- assistant 消息入历史 ----
    QJsonObject assistantMsg;
    assistantMsg.insert("role", "assistant");
    if (result.content.isEmpty() && !result.toolCalls.isEmpty())
        assistantMsg.insert("content", QJsonValue::Null);
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

    // ---- 纯文本答复：任务完成收尾 ----
    if (result.toolCalls.isEmpty()) {
        m_running = false;
        setState(State::Done);
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                                  QJsonObject{{"status", "succeeded"}, {"rounds", m_round}});
        emit loopFinished(true, result.content);
        return;
    }

    // ---- 工具批：逐个过权限门后执行 ----
    setState(State::Executing);
    for (const auto& tc : result.toolCalls) {
        const QString callId = tc.id.isEmpty() ? QStringLiteral("call_%1").arg(tc.index) : tc.id;
        emit toolCallStarted(callId, tc.name, tc.arguments);

        PermissionGate::Kind kind = PermissionGate::Kind::ReadFile;
        const QString target = classifyTarget(tc.name, tc.arguments, &kind);

        const PermissionGate::Decision decision =
            m_gate.evaluate(AppContext::instance().permissionMode, kind, target,
                             AppContext::instance().workspaceRoot);
        if (decision == PermissionGate::Decision::Denied) {
            // 永不解禁/越界拒绝：记审计 + 结果回填，模型可自行改道
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("permission_deny"), m_taskId,
                                      QJsonObject{{"tool", tc.name}, {"target", target}});
            emit toolCallFinished(callId, false,
                                  QStringLiteral("错误：操作被权限系统永久拒绝：%1").arg(target), 0);
            m_history.append(QJsonObject{
                {"role", "tool"},
                {"tool_call_id", callId},
                {"content", QStringLiteral("错误：操作被权限系统拒绝（永不解禁清单/越界）。请改用工作区内的安全方案。")},
            });
            m_breaker.recordToolFailure(QStringLiteral("permission_denied"));
            continue;
        }
        if (decision == PermissionGate::Decision::NeedsConfirm) {
            // 挂起循环，等待 UI 权限卡片回调 resumePermission
            m_pendingCallId = callId;
            m_pendingToolName = tc.name;
            m_pendingArgs = tc.arguments;
            setState(State::AwaitingPermission);
            QString riskNote;
            if (kind == PermissionGate::Kind::WriteFile)
                riskNote = QStringLiteral("写入文件（将创建或覆盖目标文件）");
            else if (kind == PermissionGate::Kind::RunCommand)
                riskNote = QStringLiteral("执行命令（在工作区内、白名单环境、Job 沙箱中运行）");
            else
                riskNote = QStringLiteral("访问网络（GET 抓取，证书校验开启）");
            emit toolAwaitingConfirm(callId, tc.name, riskNote, target);
            return;
        }
        executeToolCall(callId, tc.name, tc.arguments);
    }
    finishToolBatch();
}

void AgentLoop::resumePermission(const QString& callId, int decision) {
    if (m_state != State::AwaitingPermission || callId != m_pendingCallId)
        return;
    if (m_deps.events)
        m_deps.events->append(decision == 2 ? QStringLiteral("permission_deny")
                                            : QStringLiteral("permission_grant"),
                              m_taskId,
                              QJsonObject{{"tool", m_pendingToolName},
                                          {"always", decision == 1}});
    executePendingTool(decision);
}

void AgentLoop::executePendingTool(int decision) {
    const QString callId = m_pendingCallId;
    const QString name = m_pendingToolName;
    const QString args = m_pendingArgs;
    m_pendingCallId.clear();
    m_pendingToolName.clear();
    m_pendingArgs.clear();

    if (decision == 2) {
        // 拒绝后不执行
        setState(State::Observing);
        emit toolCallFinished(callId, false, QStringLiteral("用户拒绝该操作"), 0);
        m_history.append(QJsonObject{
            {"role", "tool"},
            {"tool_call_id", callId},
            {"content", QStringLiteral("错误：用户拒绝了该操作。请调整方案或改用无需确认的操作。")},
        });
        m_breaker.recordToolFailure(QStringLiteral("user_denied"));
        finishToolBatch();
        return;
    }
    if (decision == 1)
        m_gate.grantAlwaysForSession(PermissionGate::Kind::RunCommand); // 总是允许仅命令通道
    setState(State::Executing);
    executeToolCall(callId, name, args);
    finishToolBatch();
}

void AgentLoop::executeToolCall(const QString& callId, const QString& name, const QString& argumentsJson) {
    setState(State::Observing);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("tool_call"), m_taskId,
                              QJsonObject{{"tool", name}, {"args", argumentsJson}});

    QString resultText;
    bool ok = false;
    qint64 ms = 0;
    if (m_deps.tools) {
        const auto res = m_deps.tools->execute(name, argumentsJson);
        resultText = res.text;
        ok = res.ok;
        ms = res.ms;
    } else {
        resultText = QStringLiteral("错误：工具注册表不可用");
    }
    emit toolCallFinished(callId, ok, resultText, ms);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("tool_result"), m_taskId,
                              QJsonObject{{"tool", name}, {"ok", ok},
                                          {"ms", double(ms)},
                                          {"result", resultText.left(2000)}});

    if (ok)
        m_breaker.recordToolSuccess();
    else
        m_breaker.recordToolFailure(resultText.left(200)); // 相同失败文本检测
    m_history.append(QJsonObject{
        {"role", "tool"},
        {"tool_call_id", callId},
        {"content", resultText},
    });
}

void AgentLoop::finishToolBatch() {
    if (!m_running)
        return;

    // ---- 三重熔断保险 ----
    const Breaker::Reason reason = m_breaker.check();
    if (reason != Breaker::Reason::None) {
        m_running = false;
        setState(State::Halted);
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("circuit_break"), m_taskId,
                                  QJsonObject{{"reason", Breaker::reasonText(reason)},
                                              {"rounds", m_round},
                                              {"tokens", double(m_breaker.tokens())}});
        if (auto lg = logutil::logger())
            lg->warn("熔断：{}", Breaker::reasonText(reason).toStdString());
        emit loopFinished(false, Breaker::reasonText(reason));
        return;
    }

    setState(State::Executing);
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
    setState(State::Failed);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("error"), m_taskId,
                              QJsonObject{{"error", error}, {"http", httpCode}});
    emit loopFailed(error + (httpCode > 0 ? QStringLiteral("（HTTP %1）").arg(httpCode) : QString()));
}

} // namespace miderforge
