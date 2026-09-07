// AgentLoop 实现（M1 状态机 + M2 记忆注入/收尾提炼）
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/EventBus.h"
#include "db/Database.h"
#include "llm/ProviderManager.h"
#include "memory/MemoryManager.h"
#include "skills/SkillManager.h"
#include "tools/FileTools.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include "util/Log.h"
#include <QDateTime>
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
    case State::Reflecting: return QStringLiteral("反思中");
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
    m_finalizing = false;
    m_toolCallsThisTask = 0;
    m_history = QJsonArray();
    m_round = 0;
    m_running = true;
    m_breaker = Breaker(); // 每任务独立预算
    m_gate.resetSessionGrants(); // 决策: "总是允许"随任务失效（会话粒度的保守实现）
    m_pendingCallId.clear();

    // tasks 表持久化（M2）
    if (m_deps.db) {
        qint64 id = 0;
        m_deps.db->executeInsert(
            QStringLiteral("INSERT INTO tasks(goal, status, created_at) VALUES(?, 'running', ?)"),
            {goal, QDateTime::currentSecsSinceEpoch()}, &id);
        m_taskId = taskId >= 0 ? taskId : id;
    } else {
        m_taskId = taskId;
    }

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
        persistTaskEnd(false, QString(), QStringLiteral("已手动停止"));
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
    // 组装顺序（规格 9）：角色设定 → L1 → 相关记忆 → 最近摘要 →（M3 追加可用技能）→ 工具 → 当前任务
    QString prompt = QStringLiteral(
        "你是运行在用户 Windows 电脑上的 Miderforge 编码 Agent，目标领域是 C++/CMake/Qt 项目开发。\n"
        "以中文思考和回答。需要操作文件/执行命令时调用提供的工具；工具结果以 role=tool 消息回填。\n"
        "任务完成后直接给出最终答复（不再调用工具）。\n"
        "注意：write_file 必须给出文件完整新内容；工作区外的写入会被拒绝。\n");

    // L1 核心记忆（常驻注入）
    if (m_deps.mem) {
        const QString l1 = m_deps.mem->loadL1();
        if (!l1.isEmpty())
            prompt += QStringLiteral("\n===== L1 核心记忆 =====\n%1\n").arg(l1);

        // 相关记忆：L3 检索 Top5（v1 直接用任务原文做查询；关键词提炼随收尾 LLM 顺带产出）
        const auto related = m_deps.mem->retrieve(m_goal, 5);
        if (!related.isEmpty()) {
            prompt += QStringLiteral("\n===== 相关记忆 =====\n");
            for (const auto& r : related)
                prompt += QStringLiteral("- [%1] %2\n").arg(r.type, r.content.left(300));
        }

        // 最近会话摘要：L2 最近 5 条
        const auto summaries = m_deps.mem->recentSummaries(5);
        if (!summaries.isEmpty()) {
            prompt += QStringLiteral("\n===== 最近会话摘要 =====\n");
            for (const auto& s : summaries)
                prompt += QStringLiteral("- %1\n").arg(s.content.left(200));
        }
    }

    // 可用技能：name+description 常驻列表（渐进披露：全文经 read_skill 工具加载）
    if (m_deps.skills) {
        const auto skills = m_deps.skills->search(QString(), 20);
        if (!skills.isEmpty()) {
            prompt += QStringLiteral("\n===== 可用技能（详情用 read_skill 加载） =====\n");
            for (const auto& s : skills)
                if (s.status == QLatin1String("active"))
                    prompt += QStringLiteral("- %1：%2\n").arg(s.name, s.description);
        }
    }

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
    if (m_finalizing) {
        onFinalizeFinished(result); // 收尾调用的回包走独立分支
        return;
    }

    if (result.aborted) {
        m_running = false;
        setState(State::Failed);
        persistTaskEnd(false, QString(), QStringLiteral("已手动停止"));
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

    // ---- 纯文本答复：进入收尾流程（规格 9：成败都走） ----
    if (result.toolCalls.isEmpty()) {
        beginFinalize(true, result.content);
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
    ++m_toolCallsThisTask;
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
    // read_skill 加载 = 技能使用统计（规格 7）
    if (ok && name == QLatin1String("read_skill") && m_deps.skills) {
        const QJsonObject args = QJsonDocument::fromJson(argumentsJson.toUtf8()).object();
        m_deps.skills->recordUsage(args.value("name").toString(), true, m_round);
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("skill_used"), m_taskId,
                                  QJsonObject{{"skill", args.value("name").toString()}});
    }
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
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("circuit_break"), m_taskId,
                                  QJsonObject{{"reason", Breaker::reasonText(reason)},
                                              {"rounds", m_round},
                                              {"tokens", double(m_breaker.tokens())}});
        if (auto lg = logutil::logger())
            lg->warn("熔断：{}", Breaker::reasonText(reason).toStdString());
        beginFinalize(false, Breaker::reasonText(reason));
        return;
    }

    setState(State::Executing);
    runRound();
}

// ---- 任务收尾流程（规格 9）：LLM 提炼 结果摘要/L2 摘要/L1 改写/失败教训，随后统一落库 ----
void AgentLoop::beginFinalize(bool ok, const QString& summaryOrReason) {
    setState(ok ? State::Reflecting : State::Halted);
    m_finalizeOk = ok;
    m_finalizeSummary = summaryOrReason;

    if (!m_deps.mem || !m_deps.providers) {
        // 无记忆系统（未配置）：直接落盘收尾
        finalizeTaskWrites(QJsonObject());
        return;
    }
    // 挂起工具（收尾调用不再带工具）
    m_finalizing = true;
    const ProviderConfig* provider = m_deps.providers->activeProvider();
    if (!provider) {
        finalizeTaskWrites(QJsonObject());
        return;
    }
    const QString model = m_deps.providers->modelForTier(*provider, QStringLiteral("main"));

    QJsonArray messages;
    QJsonObject sys;
    sys.insert("role", "system");
    sys.insert("content", buildFinalizePrompt(ok, summaryOrReason));
    messages.append(sys);
    QJsonObject user;
    user.insert("role", "user");
    user.insert("content", QStringLiteral("任务目标：%1\n\n请输出 JSON。").arg(m_goal));
    messages.append(user);
    // 决策: 收尾走 main 档（M4 起可下沉 fast 档）；不带工具，禁止模型再调用
    m_deps.chat->start(*provider, model, messages, QJsonArray());
}

QString AgentLoop::buildFinalizePrompt(bool ok, const QString& summaryOrReason) const {
    QString currentL1;
    if (m_deps.mem)
        currentL1 = m_deps.mem->loadL1();

    // 自沉淀判定（规格 7）：工具调用 ≥5 次且成功 → 请求生成 SKILL.md 草稿
    QString skillSection;
    if (ok && m_toolCallsThisTask >= 5 && m_deps.skills) {
        skillSection = QStringLiteral(
            "\n本次任务工具调用达 %1 次（≥5）且成功，符合技能沉淀条件。"
            "请在 JSON 中增加字段：\n"
            "  \"skill_name\": \"简短技能名（英文-kebab-case）\",\n"
            "  \"skill_description\": \"一句话描述（将常驻 system prompt）\",\n"
            "  \"skill_md\": \"完整 SKILL.md 正文（不含 frontmatter；含 适用场景/执行步骤/边界与坑/验收标准）\",\n"
            "若认为无可沉淀的通用方案，则三个字段均为 null。\n")
            .arg(m_toolCallsThisTask);
    }

    return QStringLiteral(
        "你是 Miderforge 的记忆管理员。一次任务刚刚结束，请把它沉淀进记忆系统。\n"
        "任务结局：%1\n任务摘要/失败原因：%2\n使用轮数：%3\n累计 tokens：%4\n"
        "\n当前 L1 核心记忆全文：\n%5\n%6\n"
        "请只输出一个 JSON 对象（不要 markdown 围栏），字段：\n"
        "{\n"
        "  \"result_summary\": \"任务结果摘要（≤200字）\",\n"
        "  \"session_summary\": \"本次会话摘要，格式：目标-做法-结果-教训（≤200 token）\",\n"
        "  \"l1_new\": \"合并提炼后的 L1 全文（保留旧有有效信息、删除过时项、融入本次新知）；若无需变动则为 null\",\n"
        "  \"lesson\": \"失败教训一句话 或 null\"%7\n"
        "}")
        .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败/熔断"),
             summaryOrReason.left(500), QString::number(m_round),
             QString::number(m_breaker.tokens()),
             currentL1.isEmpty() ? QStringLiteral("（空）") : currentL1,
             skillSection,
             skillSection.isEmpty() ? QString() : QStringLiteral(",\n  ...skill 字段见上"));
}

void AgentLoop::onFinalizeFinished(const StreamResult& result) {
    m_finalizing = false;
    QString extracted;
    if (!result.aborted) {
        // 从回复中提取首个 JSON 对象（容错 markdown 围栏）
        const QString text = result.content;
        const int l = int(text.indexOf(QLatin1Char('{')));
        const int r = int(text.lastIndexOf(QLatin1Char('}')));
        if (l >= 0 && r > l) {
            QJsonParseError err{};
            const QJsonObject obj = QJsonDocument::fromJson(text.mid(l, r - l + 1).toUtf8(), &err).object();
            if (err.error == QJsonParseError::NoError)
                extracted = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            finalizeTaskWrites(obj);
        } else {
            finalizeTaskWrites(QJsonObject());
        }
    } else {
        finalizeTaskWrites(QJsonObject()); // 用户在收尾阶段取消：只落盘基础结果
    }
    (void)extracted;
}

void AgentLoop::finalizeTaskWrites(const QJsonObject& parsed) {
    const bool ok = m_finalizeOk;
    const QString resultSummary = parsed.value("result_summary").toString(m_finalizeSummary);
    persistTaskEnd(ok, resultSummary, ok ? QString() : m_finalizeSummary);

    if (m_deps.mem) {
        // ② L2 会话摘要
        const QString sessionSummary = parsed.value("session_summary").toString();
        if (!sessionSummary.isEmpty())
            m_deps.mem->addSessionSummary(sessionSummary.left(400));
        // ③ L1 提炼式改写（用户 24h 内手改保护）
        const QString l1New = parsed.value("l1_new").toString();
        if (!l1New.isEmpty() && !m_deps.mem->l1UserEditedRecently())
            m_deps.mem->saveL1(l1New);
        // ⑤ 失败教训
        const QString lesson = parsed.value("lesson").toString();
        if (!ok && !lesson.isEmpty())
            m_deps.mem->addMemory(QStringLiteral("task_lesson"), lesson, 0.8);
    }
    // ④ 技能自沉淀：交 UI 确认（或自动通过）后由 acceptSkillProposal 落盘
    const QString skillName = parsed.value("skill_name").toString();
    const QString skillDesc = parsed.value("skill_description").toString();
    const QString skillMd = parsed.value("skill_md").toString();
    if (ok && m_deps.skills && !skillName.isEmpty() && !skillMd.isEmpty()) {
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("skill_gen"), m_taskId,
                                  QJsonObject{{"skill", skillName}});
        emit skillProposed(skillName, skillDesc, skillMd);
    }
    // ⑥ 邮件+托盘通知在 M4 接入；此处先写审计
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", ok ? "succeeded" : "failed"},
                                          {"rounds", m_round},
                                          {"tokens", double(m_breaker.tokens())}});

    m_running = false;
    setState(ok ? State::Done : State::Halted);
    emit loopFinished(ok, resultSummary);
}

void AgentLoop::acceptSkillProposal(const QString& name, const QString& description, const QString& md) {
    if (!m_deps.skills)
        return;
    QString err;
    if (m_deps.skills->writeSkill(name, description, md, &err)) {
        if (auto lg = logutil::logger())
            lg->info("技能已沉淀：{}", name.toStdString());
    } else if (auto lg = logutil::logger()) {
        lg->error("技能落盘失败：{}", err.toStdString());
    }
}

void AgentLoop::persistTaskEnd(bool ok, const QString& resultSummary, const QString& failureReason) {
    if (!m_deps.db)
        return;
    m_deps.db->execute(QStringLiteral(
        "UPDATE tasks SET status=?, rounds_used=?, tokens_in=?, result_summary=?, failure_reason=?, finished_at=? "
        "WHERE id=?"),
        {ok ? QStringLiteral("succeeded") : (m_state == State::Halted ? QStringLiteral("halted")
                                                                     : QStringLiteral("failed")),
         m_round, m_breaker.tokens(), resultSummary, failureReason,
         QDateTime::currentSecsSinceEpoch(), m_taskId});
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
    persistTaskEnd(false, QString(), error);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("error"), m_taskId,
                              QJsonObject{{"error", error}, {"http", httpCode}});
    emit loopFailed(error + (httpCode > 0 ? QStringLiteral("（HTTP %1）").arg(httpCode) : QString()));
}

} // namespace miderforge
