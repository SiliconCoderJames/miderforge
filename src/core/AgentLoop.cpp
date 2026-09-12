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
#include "util/AppDirs.h"
#include "util/JsonExtract.h"
#include "util/Log.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <utility>
#include <spdlog/spdlog.h>

namespace miderforge {

namespace {

// 工具结果回填上下文的长度护栏：原始结果可达 1MB（≈25 万 token），不截断会一发吃掉任务预算大半
constexpr int kToolResultHead = 12000;
constexpr int kToolResultTail = 2000;

QString truncateToolResult(const QString& text) {
    if (text.size() <= kToolResultHead + kToolResultTail + 60)
        return text;
    return text.left(kToolResultHead)
           + QStringLiteral("\n…[结果过长已截断：完整 %1 字符；如需其余内容请用 list_dir/search_files 分段定位]…\n")
                 .arg(text.size())
           + text.right(kToolResultTail);
}

// 死循环检测签名：剔除数字（行号/时间戳/路径序号）后比较，
// "错误：第 42 行：未定义 foo" 与 "第 43 行：…" 应判定为同一类连续失败
QString failureSignature(const QString& text) {
    static const QRegularExpression digits(QStringLiteral("[0-9]+"));
    QString sig = text;
    sig.remove(digits);
    return sig.simplified().left(200);
}

// tool_call_id 生成：协议要求全局唯一，而供应商缺省 id 时 index 每轮从 0 起——
// 不带轮次前缀会跨轮撞 id（LLM 按错误 id 关联到第一轮的结果做决策）
QString toolCallId(const ToolCallParts& tc, int ordinal, int round) {
    if (!tc.id.isEmpty())
        return tc.id;
    return QStringLiteral("call_r%1_i%2").arg(round).arg(ordinal);
}

} // namespace

QString AgentLoop::stateName(State s) {
    switch (s) {
    case State::Idle: return QStringLiteral("空闲");
    case State::Planning: return QStringLiteral("规划中");
    case State::Executing: return QStringLiteral("执行中");
    case State::Observing: return QStringLiteral("观察中");
    case State::AwaitingPermission: return QStringLiteral("等待确认");
    case State::Reflecting: return QStringLiteral("反思中");
    case State::Paused: return QStringLiteral("已暂停");
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
    beginTask(goal, taskId, QJsonObject());
}

void AgentLoop::startWithCheckpoint(const QString& goal, qint64 taskId, const QJsonObject& checkpoint) {
    beginTask(goal, taskId, checkpoint);
}

void AgentLoop::beginTask(const QString& goal, qint64 taskId, const QJsonObject& checkpoint) {
    if (m_running) {
        emit loopFailed(QStringLiteral("已有任务在执行中"));
        return;
    }
    m_goal = goal;
    m_finalizing = false;
    m_finalizeRetries = 0;
    m_toolCallsThisTask = 0;
    m_tier = Router::classify(goal); // M4：三档路由按任务语义选档
    m_consecToolFailures = 0;
    m_tierEscalated = false;
    m_transportFailures = 0;
    m_failedOver = false;
    m_boundaryDenies = 0;           // P1-5b：每任务重置越界计数
    m_history = QJsonArray();
    m_round = 0;
    m_lastReflection.clear();
    m_injectedMemories.clear();
    m_lastPromptTokens = 0;
    m_toolCancelSeen = false;
    m_terminalStatus.clear();
    m_pauseRequested.store(false);
    m_paused = false;
    if (m_deps.tools)
        m_deps.tools->consumeToolCancel(); // 防御：清掉上一任务可能残留的取消标志
    m_running = true;
    m_breaker = Breaker(AppContext::instance().limits); // 每任务独立预算（设置页可改）
    m_gate.resetSessionGrants(); // 决策: "总是允许"随任务失效（会话粒度的保守实现）
    m_pendingCallId.clear();

    // M5-④ 断点恢复：在首轮派发前覆盖可恢复字段（顺序关键——start 末尾就是 runRound）
    const QJsonArray checkpointHistory = checkpoint.value(QStringLiteral("history")).toArray();
    const bool resumed = !checkpointHistory.isEmpty();
    if (resumed) {
        m_history = checkpointHistory;
        m_round = checkpoint.value(QStringLiteral("round")).toInt();
        m_tier = static_cast<Router::Tier>(
            checkpoint.value(QStringLiteral("tier")).toInt(static_cast<int>(Router::Tier::Main)));
        m_lastReflection = checkpoint.value(QStringLiteral("last_reflection")).toString();
        const qint64 tokens = qint64(checkpoint.value(QStringLiteral("tokens")).toDouble());
        m_breaker.addTokens(tokens > 0 ? tokens : 0);
        m_breaker.restoreRounds(m_round); // 熔断轮数与任务轮数同步恢复（自审发现：漏恢复会多出一截预算）
        m_lastPromptTokens = qint64(checkpoint.value(QStringLiteral("last_prompt_tokens")).toDouble());
    }

    // tasks 表持久化（M2）。外部 taskId（Scheduler 路径）行已存在且已标 running，
    // 直接沿用——再 INSERT 会产生永不结束的幽灵 running 行，且会被启动恢复重新入队重复执行
    if (m_deps.db) {
        if (taskId >= 0) {
            m_taskId = taskId;
        } else {
            qint64 id = 0;
            m_deps.db->executeInsert(
                QStringLiteral("INSERT INTO tasks(goal, status, created_at) VALUES(?, 'running', ?)"),
                {goal, QDateTime::currentSecsSinceEpoch()}, &id);
            m_taskId = id;
        }
        // 无检查点 = 全新开始：清掉该行可能遗留的旧断点
        if (!resumed)
            m_deps.db->execute(QStringLiteral("UPDATE tasks SET context_json=NULL WHERE id=?"),
                               {m_taskId});
    } else {
        m_taskId = taskId;
    }

    if (auto lg = logutil::logger()) {
        if (resumed)
            lg->info("任务 #{} 断点续跑：从第 {} 轮恢复（{} 条历史，已耗 {} tokens）", m_taskId,
                     m_round, m_history.size(), m_breaker.tokens());
    }

    emit taskStarted(goal);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", resumed ? "resumed" : "started"}, {"goal", goal}});
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
        m_terminalStatus = QStringLiteral("cancelled");
        setState(State::Failed);
        persistTaskEnd(false, QString(), QStringLiteral("已手动停止"));
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                                  QJsonObject{{"status", "cancelled"}});
        emit loopFinished(false, QStringLiteral("已手动停止"));
        return;
    }
    // M5 P2：挂起中的取消 = 直接判停（没有在跑的工具/流可等）
    if (m_paused) {
        m_pauseRequested.store(false);
        stopForUserCancel();
        return;
    }
    // M5 P3：工具执行中取消原来到不了工具层（此路径上 chat 并不在跑，cancel 是空操作）——
    // 先请求在跑工具中止，工具批收尾时检测到取消即整体判停
    if (m_deps.tools)
        m_deps.tools->requestToolCancel();
    m_deps.chat->cancel();
}

void AgentLoop::pause() {
    if (!m_running || m_paused)
        return;
    m_pauseRequested.store(true);
    if (auto lg = logutil::logger())
        lg->info("任务 #{} 收到暂停请求，将在轮边界挂起", m_taskId);
}

void AgentLoop::resume() {
    if (!m_paused || !m_running)
        return;
    m_paused = false;
    if (m_deps.db)
        m_deps.db->execute(QStringLiteral("UPDATE tasks SET status='running' WHERE id=?"), {m_taskId});
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", "resumed"}});
    emit taskResumed();
    setState(State::Planning);
    runRound();
}

void AgentLoop::enterPaused() {
    m_paused = true;
    writeCheckpoint(); // 挂起行携带断点，重启后可续跑
    setState(State::Paused);
    if (m_deps.db)
        m_deps.db->execute(QStringLiteral("UPDATE tasks SET status='paused' WHERE id=?"), {m_taskId});
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", "paused"}});
    emit taskPaused();
}

void AgentLoop::writeCheckpoint() {
    if (!m_deps.db)
        return;
    QJsonObject ckpt;
    ckpt.insert(QStringLiteral("history"), m_history);
    ckpt.insert(QStringLiteral("round"), m_round);
    ckpt.insert(QStringLiteral("tier"), static_cast<int>(m_tier));
    ckpt.insert(QStringLiteral("tokens"), double(m_breaker.tokens()));
    ckpt.insert(QStringLiteral("last_prompt_tokens"), double(m_lastPromptTokens));
    ckpt.insert(QStringLiteral("last_reflection"), m_lastReflection);
    m_deps.db->execute(QStringLiteral("UPDATE tasks SET context_json=? WHERE id=?"),
                       {QString::fromUtf8(QJsonDocument(ckpt).toJson(QJsonDocument::Compact)),
                        m_taskId});
}

void AgentLoop::shutdownRequeue() {
    if (!m_running)
        return;
    m_running = false;
    m_pauseRequested.store(false);
    // 请求在跑工具快速中止（取消令牌），让嵌套工具循环尽快返回、退出不拖过 10s 有界等待
    if (m_deps.tools)
        m_deps.tools->requestToolCancel();
    if (m_deps.db)
        m_deps.db->execute(QStringLiteral("UPDATE tasks SET status='queued' WHERE id=?"), {m_taskId});
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", "interrupted"}});
    if (auto lg = logutil::logger())
        lg->warn("退出收束：任务 #{} 已放回队列，下次启动自动恢复", m_taskId);
}

void AgentLoop::runRound() {
    m_breaker.beginRound();
    ++m_round;
    emit roundChanged(m_round, m_breaker.limits().maxRounds);
    setState(State::Planning);

    // 故障转移冷却回切：距上次切换超过冷却期的任务先试主供应商（切换机制仍待命，再失败会再次转移）
    if (m_deps.providers)
        m_deps.providers->maybeRestorePrimary();

    const ProviderConfig* provider = m_deps.providers ? m_deps.providers->activeProvider() : nullptr;
    if (!provider) {
        m_running = false;
        setState(State::Failed);
        emit loopFailed(QStringLiteral("未配置任何大模型供应商，请先完成首次配置"));
        return;
    }
    // M4：三档路由；规格 8 默认走 main 档，fast/flagship 由 Router 分类或失败升档产生
    const QString tierName = Router::tierName(m_tier);
    const QString model = m_deps.providers->modelForTier(*provider, tierName);

    if (m_deps.events)
        m_deps.events->append(QStringLiteral("llm_request"), m_taskId,
                              QJsonObject{{"provider", provider->name}, {"model", model},
                                          {"round", m_round}, {"tier", tierName}});

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

QString AgentLoop::buildSystemPrompt() {
    // 组装顺序（规格 9）：角色设定 → L1 → 相关记忆 → 最近摘要 →（M3 追加可用技能）→ 工具 → 当前任务
    QString prompt = QStringLiteral(
        "你是运行在用户 Windows 电脑上的 Miderforge 编码 Agent，目标领域是 C++/CMake/Qt 项目开发。\n"
        "以中文思考和回答。需要操作文件/执行命令时调用提供的工具；工具结果以 role=tool 消息回填。\n"
        "任务完成后直接给出最终答复（不再调用工具）。\n"
        "注意：write_file 必须给出文件完整新内容；工作区外的写入会被拒绝。\n");

    // L1 核心记忆（常驻注入；Hermes 同款用量头部 + § 分条渲染）
    if (m_deps.mem) {
        const QString l1 = m_deps.mem->loadL1();
        if (!l1.isEmpty())
            prompt += QStringLiteral("\n===== %1 =====\n")
                          .arg(m_deps.mem->renderL1ForPrompt(l1, m_deps.mem->l1Tokens(),
                                                             AppContext::instance().l1TokenLimit));

        // 相关记忆：L3 检索 Top5。查询随轮次演化（RAG 的"动态相关"才有意义）：
        // 首轮用任务原文；之后混入模型最近的反思/计划文本，避免 25 轮注入同一批常量记忆
        QString retrievalQuery = m_goal;
        if (m_round > 1 && !m_lastReflection.isEmpty())
            retrievalQuery = m_goal + QLatin1Char(' ') + m_lastReflection.left(300);
        const auto related = m_deps.mem->retrieve(retrievalQuery, 5);
        if (!related.isEmpty()) {
            prompt += QStringLiteral("\n===== 相关记忆 =====\n");
            for (const auto& r : related) {
                prompt += QStringLiteral("- [%1] %2\n").arg(r.type, r.content.left(300));
                // 登记 id+摘要：收尾时作为"一致性失效"的候选清单（只允许归档这份清单内的编号）
                if (!m_injectedMemories.contains({r.id, r.content.left(100)}))
                    m_injectedMemories.append({r.id, r.content.left(100)});
            }
        }

        // 最近会话摘要：L2 最近 5 条
        const auto summaries = m_deps.mem->recentSummaries(5);
        if (!summaries.isEmpty()) {
            prompt += QStringLiteral("\n===== 最近会话摘要 =====\n");
            for (const auto& s : summaries)
                prompt += QStringLiteral("- %1\n").arg(s.content.left(200));
        }
    }

    // 可用技能：name+description 常驻列表（渐进披露：全文经 read_skill 工具加载）。
    // 跨层预取：与目标最相关的技能标 ⭐ 前排（注意力预取，不全文注入）
    if (m_deps.skills) {
        const auto skills = m_deps.skills->search(QString(), 20);
        if (!skills.isEmpty()) {
            QSet<QString> prefetchNames;
            const auto relevant = m_deps.skills->searchRelevant(m_goal, 3);
            for (const auto& s : relevant)
                prefetchNames.insert(s.name);
            prompt += QStringLiteral(
                "\n===== 可用技能（详情用 read_skill 加载；⭐ 为与当前目标疑似相关，可优先加载） =====\n");
            for (const auto& s : relevant)
                if (s.status == QLatin1String("active"))
                    prompt += QStringLiteral("- ⭐ %1：%2\n").arg(s.name, s.description);
            for (const auto& s : skills)
                if (s.status == QLatin1String("active") && !prefetchNames.contains(s.name))
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
    if (toolName == QLatin1String("read_skill")) {
        // P0-2：判定对象是 read_skill 实际读取的文件路径（skills/<name>/SKILL.md），
        // 让名字清单与读取保护区对全部读取通道一视同仁（skills/ 在保护区外，正常技能不受影响）
        *kind = PermissionGate::Kind::ReadFile;
        return appdirs::file(QStringLiteral("skills/%1/SKILL.md").arg(args.value("name").toString()));
    }
    // 其余（list_dir / search_files）按读取处理，判定对象是各自的路径参数
    *kind = PermissionGate::Kind::ReadFile;
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
        m_terminalStatus = QStringLiteral("cancelled");
        setState(State::Failed);
        persistTaskEnd(false, QString(), QStringLiteral("已手动停止"));
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                                  QJsonObject{{"status", "cancelled"}});
        emit loopFinished(false, QStringLiteral("已手动停止"));
        return;
    }

    // 增量记账：promptTokens 含整段重发的 history，按全量累加是 O(N²) 超线性口径，
    // 500K 预算撑不到标称轮数。只收"新增输入（本轮 prompt − 上轮 prompt）+ 本轮输出"
    const long long promptDelta = qMax<long long>(0, result.promptTokens - m_lastPromptTokens);
    m_lastPromptTokens = result.promptTokens;
    const long long charged = promptDelta + result.completionTokens;
    m_breaker.addTokens(charged);
    AppContext::instance().todayTokens += charged;
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
        const int callCount = int(result.toolCalls.size());
        for (int i = 0; i < callCount; ++i) {
            const auto& tc = result.toolCalls[i];
            calls.append(QJsonObject{
                {"id", toolCallId(tc, int(i), m_round)},
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

    // 记录本轮模型正文（后续轮次的记忆检索查询用它演化，见 buildSystemPrompt）
    if (!result.content.isEmpty())
        m_lastReflection = result.content;

    // ---- 纯文本答复：进入收尾流程（规格 9：成败都走） ----
    if (result.toolCalls.isEmpty()) {
        beginFinalize(true, result.content);
        return;
    }

    // ---- 工具批：逐个过权限门后执行 ----
    setState(State::Executing);
    const int toolCount = int(result.toolCalls.size());
    for (int i = 0; i < toolCount; ++i) {
        const auto& tc = result.toolCalls[i];
        const QString callId = toolCallId(tc, int(i), m_round);
        emit toolCallStarted(callId, tc.name, tc.arguments);

        PermissionGate::Kind kind = PermissionGate::Kind::ReadFile;
        const QString target = classifyTarget(tc.name, tc.arguments, &kind);

        const PermissionGate::Decision decision =
            m_gate.evaluate(AppContext::instance().permissionMode, kind, target,
                             AppContext::instance().workspaceRoot);
        if (decision == PermissionGate::Decision::Denied) {
            // 永不解禁/越界拒绝：按通道记审计（读=read_denied，其余=permission_deny），
            // 事件带六元组（actor/authorizer/target/operation/outcome/reason；ts/task_id 走 events 列）
            const QString reason = m_gate.hardDenyReason(kind, target,
                                                         AppContext::instance().workspaceRoot);
            if (m_deps.events) {
                const QString operation = kind == PermissionGate::Kind::ReadFile    ? QStringLiteral("read")
                                          : kind == PermissionGate::Kind::WriteFile ? QStringLiteral("write")
                                          : kind == PermissionGate::Kind::RunCommand
                                              ? QStringLiteral("run_command")
                                              : QStringLiteral("network");
                m_deps.events->append(kind == PermissionGate::Kind::ReadFile    ? QStringLiteral("read_denied")
                                      : kind == PermissionGate::Kind::RunCommand
                                          ? QStringLiteral("command_denied")
                                          : QStringLiteral("permission_deny"),
                                      m_taskId,
                                      QJsonObject{{"actor", "agent"},
                                                  {"authorizer", "permission_gate"},
                                                  {"operation", operation},
                                                  {"target", target},
                                                  {"outcome", "denied"},
                                                  {"reason", reason.isEmpty()
                                                                     ? QStringLiteral("workspace_boundary")
                                                                     : reason},
                                                  {"tool", tc.name}});
            }
            ++m_boundaryDenies; // P1-5b：越界/拒绝计数（技能固化门槛用）
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
        if (m_toolCancelSeen)
            break; // 用户已取消：不再执行本批剩余工具，收尾统一判停
    }
    finishToolBatch();
}

void AgentLoop::resumePermission(const QString& callId, int decision) {
    if (m_state != State::AwaitingPermission || callId != m_pendingCallId)
        return;
    if (decision == 2)
        ++m_boundaryDenies; // P1-5b：用户在确认卡上拒绝，同样计入越界
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
    if (decision == 1) {
        // "总是允许"授予【本次挂起工具】的真实通道（原先写死命令通道：
        // 网络工具点总是允许会授错通道，本会话后续网络请求仍反复弹卡）。
        // WriteFile 的授予存储后暂不被 evaluate 咨询（Suggest 档写文件保持逐次确认，安全优先）
        PermissionGate::Kind grantedKind = PermissionGate::Kind::RunCommand;
        classifyTarget(name, args, &grantedKind);
        m_gate.grantAlwaysForSession(grantedKind);
    }
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
    // M5 P3：消费取消令牌（工具响应与否都消费——用户在本任务内按了取消就要停）
    if (m_deps.tools && m_deps.tools->consumeToolCancel())
        m_toolCancelSeen = true;
    // 上下文护栏：回填 LLM 的结果必须截断（1MB 文件全文 ≈ 25 万 token，一发吃穿任务预算）
    resultText = truncateToolResult(resultText);
    emit toolCallFinished(callId, ok, resultText, ms);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("tool_result"), m_taskId,
                              QJsonObject{{"tool", name}, {"ok", ok},
                                          {"ms", double(ms)},
                                          {"result", resultText.left(2000)}});

    if (ok) {
        m_breaker.recordToolSuccess();
        m_consecToolFailures = 0;
    } else {
        m_breaker.recordToolFailure(failureSignature(resultText)); // 剔除数字后比对，见 failureSignature
        // M4：同一工具连续失败 2 次 → 升档（重试 2 次后自动升档，规格 8）
        ++m_consecToolFailures;
        if (m_consecToolFailures >= 2 && !m_tierEscalated
            && m_tier != Router::Tier::Flagship) {
            m_tier = Router::escalate(m_tier);
            m_tierEscalated = true;
            m_consecToolFailures = 0;
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("tier_escalate"), m_taskId,
                                      QJsonObject{{"tier", Router::tierName(m_tier)}});
        }
    }
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

    // M5 P3：用户取消优先于一切熔断——整体判停为 cancelled
    if (m_toolCancelSeen) {
        stopForUserCancel();
        return;
    }

    // M5 P2：暂停请求在轮边界安全点生效（不打断在跑的工具/流式）
    if (m_pauseRequested.exchange(false)) {
        enterPaused();
        return;
    }

    // ---- 三重熔断保险 ----
    writeCheckpoint(); // M5-④：每轮末持久化断点（终端态时由 persistTaskEnd 清除）

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
    long long l1Tokens = 0;
    if (m_deps.mem) {
        currentL1 = m_deps.mem->loadL1();
        l1Tokens = m_deps.mem->l1Tokens();
    }
    const long long l1Cap = AppContext::instance().l1TokenLimit;

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

    // 一致性失效候选清单（存储体系 write-through + invalidation）：
    // 本次注入过的 L3 记忆带编号列出；LLM 只能在这个清单里挑"已被本次新知覆盖"的旧条目，
    // 落库侧再用 filterSupersededIds 做编号白名单校验，双保险防幻觉编号误伤
    QString memorySection;
    if (!m_injectedMemories.isEmpty()) {
        memorySection = QStringLiteral("\n本次任务注入过的相关记忆（#编号 内容摘要）：\n");
        int shown = 0;
        for (const auto& [id, snippet] : m_injectedMemories) {
            if (shown >= 20) {
                memorySection += QStringLiteral("…等共 %1 条\n").arg(m_injectedMemories.size());
                break;
            }
            memorySection += QStringLiteral("  #%1 %2\n").arg(id).arg(snippet);
            ++shown;
        }
        memorySection += QStringLiteral(
            "若上面某条旧记忆已被本次任务的新知覆盖或推翻，请在 superseded_memory_ids "
            "中列出其编号（只能从上面列出的编号中选）；没有则给空数组。\n");
    }

    return QStringLiteral(
        "你是 Miderforge 的记忆管理员。一次任务刚刚结束，请把它沉淀进记忆系统。\n"
        "任务结局：%1\n任务摘要/失败原因：%2\n使用轮数：%3\n累计 tokens：%4\n"
        "\n当前 L1 核心记忆全文（约 %5 / 上限 %6 tokens）：\n%7\n%8%9\n"
        "请只输出一个 JSON 对象（不要 markdown 围栏），字段：\n"
        "{\n"
        "  \"result_summary\": \"任务结果摘要（≤200字）\",\n"
        "  \"session_summary\": \"本次会话摘要，格式：目标-做法-结果-教训（≤200 token）\",\n"
        "  \"l1_new\": \"合并提炼后的 L1 全文（保留旧有有效信息、删除过时项、融入本次新知，"
        "总量必须控制在 %6 tokens 以内）；若无需变动则为 null\",\n"
        "  \"lesson\": \"失败教训一句话 或 null\"%10%11\n"
        "}")
        .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败/熔断"),
             summaryOrReason.left(500), QString::number(m_round),
             QString::number(m_breaker.tokens()),
             QString::number(l1Tokens), QString::number(l1Cap),
             currentL1.isEmpty() ? QStringLiteral("（空）") : currentL1,
             memorySection, skillSection,
             skillSection.isEmpty() ? QString() : QStringLiteral(",\n  ...skill 字段见上"),
             memorySection.isEmpty() ? QString() : QStringLiteral(",\n  \"superseded_memory_ids\": [被覆盖记忆的编号数组]"));
}

void AgentLoop::onFinalizeFinished(const StreamResult& result) {
    m_finalizing = false;
    if (!result.aborted) {
        // 结构化提取：括号深度扫描 + 字符串感知，容忍围栏/前后闲话/中间示例对象
        const QJsonObject obj = jsonextract::extractObject(result.content).object();
        finalizeTaskWrites(obj); // 解析失败时 obj 为空 → 走兜底字段，绝不丢任务结局
    } else {
        finalizeTaskWrites(QJsonObject()); // 用户在收尾阶段取消：只落盘基础结果
    }
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
        // ③ L1 提炼式改写（用户手改保护 + P1-5a 投毒筛查）
        const QString l1New = parsed.value("l1_new").toString();
        bool l1Rewritten = false;
        bool l1Poisoned = false;
        QString l1PoisonReason;
        if (!l1New.isEmpty() && !m_deps.mem->l1UserEditedRecently()) {
            if (MemoryManager::l1RewriteSuspicious(l1New, &l1PoisonReason)) {
                // 命中外发 URL / 凭据特征：拒绝落盘（旧 L1 保持不动），
                // 事件留新旧 diff 摘要，六元组齐全
                l1Poisoned = true;
                if (m_deps.events)
                    m_deps.events->append(QStringLiteral("memory_rewrite_flagged"), m_taskId,
                                          QJsonObject{{"actor", "agent"},
                                                      {"authorizer", "memory_guard"},
                                                      {"target", "core.md"},
                                                      {"operation", "l1_rewrite"},
                                                      {"outcome", "blocked"},
                                                      {"reason", l1PoisonReason},
                                                      {"diff", MemoryManager::l1DiffSummary(
                                                                   m_deps.mem->loadL1(), l1New)}});
                if (auto lg = logutil::logger())
                    lg->warn("L1 改写被投毒筛查拦截：{}", l1PoisonReason.toStdString());
            } else {
                l1Rewritten = m_deps.mem->saveL1(l1New);
            }
        }
        // ⑤ 失败教训
        const QString lesson = parsed.value("lesson").toString();
        if (!ok && !lesson.isEmpty())
            m_deps.mem->addMemory(QStringLiteral("task_lesson"), lesson, 0.8);
        // ⑤b 一致性失效（存储体系 write-through + invalidation）：
        // 只有 L1 真的按新知改写成功后，才归档被覆盖/推翻的 L3 旧条目——否则会出现
        // "旧记忆被归档、新知识没写进去"的净知识丢失。未改写时留档并记录原因，便于排查。
        if (l1Rewritten && !m_injectedMemories.isEmpty()) {
            QVector<qint64> injectedIds;
            injectedIds.reserve(m_injectedMemories.size());
            for (const auto& [id, snippet] : m_injectedMemories) {
                (void)snippet;
                injectedIds.push_back(id);
            }
            const auto superseded = MemoryManager::filterSupersededIds(parsed, injectedIds);
            const int invalidated = m_deps.mem->archiveMemories(superseded);
            if (invalidated > 0) {
                if (m_deps.events)
                    m_deps.events->append(QStringLiteral("memory_invalidate"), m_taskId,
                                          QJsonObject{{"count", invalidated}});
                if (auto lg = logutil::logger())
                    lg->info("一致性失效：{} 条被本次新知覆盖的旧记忆已归档", invalidated);
            }
        } else if (!m_injectedMemories.isEmpty() && !l1Rewritten) {
            const QString why = l1New.isEmpty() ? QStringLiteral("模型未给出 l1_new")
                                : l1Poisoned    ? QStringLiteral("投毒筛查拦截")
                                                : QStringLiteral("检测到用户手改");
            if (auto lg = logutil::logger())
                lg->info("L1 未改写（{}），跳过一致性失效归档以免丢失旧记忆", why.toStdString());
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("memory_invalidate_skipped"), m_taskId,
                                      QJsonObject{{"reason", l1New.isEmpty() ? "no_l1_new"
                                                        : l1Poisoned ? "poisoned"
                                                                     : "user_edited"}});
        }
        // ⑤c L1 容量纪律（存储体系：每层有容量上限）。v1 软约束：提示词要求控制 + 超限告警
        const long long used = m_deps.mem->l1Tokens();
        const long long cap = AppContext::instance().l1TokenLimit;
        if (used > cap) {
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("l1_overflow"), m_taskId,
                                      QJsonObject{{"used", double(used)}, {"cap", double(cap)}});
            if (auto lg = logutil::logger())
                lg->warn("L1 核心记忆超限：{} / {} tokens，建议在记忆视图手动精简", used, cap);
        }
    }
    // ④ 技能自沉淀：交 UI 确认（或自动通过）后由 acceptSkillProposal 落盘
    const QString skillName = parsed.value("skill_name").toString();
    const QString skillDesc = parsed.value("skill_description").toString();
    const QString skillMd = parsed.value("skill_md").toString();
    if (ok && m_deps.skills && !skillName.isEmpty() && !skillMd.isEmpty()) {
        if (m_boundaryDenies > 0) {
            // P1-5b 固化门槛：任务虽"成功"，但期间发生过越界/拒绝——
            // 把失败路径沉淀成技能等于教坏下一代任务，拒绝并留审计
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("skill_solidify_denied"), m_taskId,
                                      QJsonObject{{"actor", "agent"},
                                                  {"authorizer", "skill_guard"},
                                                  {"target", skillName},
                                                  {"operation", "skill_solidify"},
                                                  {"outcome", "blocked"},
                                                  {"reason", QStringLiteral("任务期间发生 %1 次越界/拒绝事件")
                                                                 .arg(m_boundaryDenies)},
                                                  {"boundary_denies", double(m_boundaryDenies)}});
            if (auto lg = logutil::logger())
                lg->warn("技能固化被拒绝：{}（本任务 {} 次越界事件）", skillName.toStdString(),
                         m_boundaryDenies);
        } else {
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("skill_gen"), m_taskId,
                                      QJsonObject{{"skill", skillName}});
            emit skillProposed(skillName, skillDesc, skillMd);
        }
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

QString AgentLoop::rerunTool(const QString& toolName, const QString& argsJson, bool* denied) {
    if (denied)
        *denied = false;
    if (!m_deps.tools)
        return QStringLiteral("错误：工具注册表不可用");

    // 与自动执行同一条权限路径（classifyTarget + evaluate）：
    // 手动重跑不是"后门"，同样受永不解禁清单与三档权限约束
    PermissionGate::Kind kind = PermissionGate::Kind::ReadFile;
    const QString target = classifyTarget(toolName, argsJson, &kind);
    const auto decision = m_gate.evaluate(AppContext::instance().permissionMode, kind, target,
                                          AppContext::instance().workspaceRoot);
    if (decision != PermissionGate::Decision::Allowed) {
        if (denied)
            *denied = true;
        const QString why = decision == PermissionGate::Decision::Denied
                                ? QStringLiteral("该操作被权限系统永久拒绝")
                                : QStringLiteral("当前权限档（%1）下这类操作需逐次确认，"
                                                 "手动重跑无法弹确认卡片；请切到 Full Access，"
                                                 "或先在任务流中选「本会话总是允许」")
                                      .arg(permissionModeName(AppContext::instance().permissionMode));
        ++m_boundaryDenies; // P1-5b：手动重跑被拒同样计入越界
        if (m_deps.events) {
            // P0-2：读通道拒绝记 read_denied（与自动执行同口径，六元组齐全）
            const bool readChannel = kind == PermissionGate::Kind::ReadFile;
            const QString reason = m_gate.hardDenyReason(kind, target,
                                                         AppContext::instance().workspaceRoot);
            m_deps.events->append(readChannel ? QStringLiteral("read_denied")
                                  : kind == PermissionGate::Kind::RunCommand
                                      ? QStringLiteral("command_denied")
                                      : QStringLiteral("permission_deny"),
                                  m_taskId,
                                  QJsonObject{{"actor", "agent"},
                                              {"authorizer", "permission_gate"},
                                              {"operation", readChannel ? QStringLiteral("read")
                                                                        : QStringLiteral("manual_rerun")},
                                              {"target", target},
                                              {"outcome", "denied"},
                                              {"reason", reason.isEmpty()
                                                                 ? QStringLiteral("permission_gate")
                                                                 : reason},
                                              {"tool", toolName},
                                              {"via", "manual_rerun"}});
        }
        return QStringLiteral("错误：%1：%2").arg(why, target);
    }

    // 清掉可能残留的取消令牌：任务被"流式中取消"时 onStreamFinished 的 aborted 分支会直接
    // 判停且不消费令牌，于是令牌一直是 true。手动重跑发生在任务之间，不该继承上一个已结束
    // 任务的取消请求——否则工具入口快检会直接返回"已被用户取消"，看起来像重跑失败。
    if (m_deps.tools)
        m_deps.tools->consumeToolCancel();

    const auto res = m_deps.tools->execute(toolName, argsJson);
    if (!res.ok && denied)
        *denied = true; // 执行失败也要让调用方知道：否则 UI 会把失败画成绿色"已重跑"
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("tool_result"), m_taskId,
                              QJsonObject{{"tool", toolName}, {"ok", res.ok},
                                          {"ms", double(res.ms)}, {"via", "manual_rerun"},
                                          {"result", res.text.left(2000)}});
    return res.text;
}

void AgentLoop::stopForUserCancel() {
    m_running = false;
    m_terminalStatus = QStringLiteral("cancelled");
    setState(State::Failed);
    persistTaskEnd(false, QString(), QStringLiteral("已手动停止"));
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("task_status"), m_taskId,
                              QJsonObject{{"status", "cancelled"}});
    emit loopFinished(false, QStringLiteral("已手动停止"));
}

void AgentLoop::persistTaskEnd(bool ok, const QString& resultSummary, const QString& failureReason) {
    if (!m_deps.db)
        return;
    // M5-①终态语义：cancelled 覆写优先（用户取消 ≠ 失败），其余按结局推导
    const QString status = !m_terminalStatus.isEmpty()
                               ? m_terminalStatus
                               : (ok ? QStringLiteral("succeeded")
                                     : (m_state == State::Halted ? QStringLiteral("halted")
                                                                 : QStringLiteral("failed")));
    m_deps.db->execute(QStringLiteral(
        "UPDATE tasks SET status=?, rounds_used=?, tokens_in=?, result_summary=?, failure_reason=?, "
        "context_json=NULL, finished_at=? WHERE id=?"),
        {status, m_round, m_breaker.tokens(), resultSummary, failureReason,
         QDateTime::currentSecsSinceEpoch(), m_taskId});
}

void AgentLoop::handleTransportFailure(const QString& error, int httpCode) {
    // M4 故障转移（规格 8）：传输类失败（429/超时/5xx/无响应）切 failover_backup。
    // 触发次数由调用方控制（每任务至多一次，见 onStreamFailed 的 m_failedOver 兜底）——
    // 这里不再自设 ">= 2 次" 门槛：那会让首次失败直接判死任务，使转移永远无法发生。
    const bool transportLike = (httpCode == 0 || httpCode == 429 || httpCode >= 500);
    if (!transportLike)
        return;
    ++m_transportFailures;
    if (!m_deps.providers)
        return;
    if (m_deps.providers->switchToFailover(error)) {
        m_failedOver = true;
        if (auto lg = logutil::logger())
            lg->warn("供应商故障转移：{}（本任务累计传输级失败 {} 次）", error.toStdString(),
                     m_transportFailures);
        m_transportFailures = 0;
        if (m_deps.events)
            m_deps.events->append(QStringLiteral("provider_switch"), m_taskId,
                                  QJsonObject{{"reason", error}});
        // P1-5c failover 权限联动：供应商已切换（新端点未被用户审阅过）→
        // Full Access 自动降为 Auto Edit，事件留六元组。
        // 注：AppContext 非 QObject，设置页组合框在下次打开时读到新值（UI 即时刷新留 Roadmap）。
        if (AppContext::instance().permissionMode == PermissionMode::FullAccess) {
            AppContext::instance().permissionMode = PermissionMode::AutoEdit;
            if (m_deps.events)
                m_deps.events->append(QStringLiteral("permission_downgrade_on_failover"),
                                      m_taskId,
                                      QJsonObject{{"actor", "system"},
                                                  {"authorizer", "permission_gate"},
                                                  {"target", "app_context.permission_mode"},
                                                  {"operation", "downgrade"},
                                                  {"outcome", "full_access->auto_edit"},
                                                  {"reason", error}});
            if (auto lg = logutil::logger())
                lg->warn("故障转移联动：权限档 Full Access → Auto Edit");
        }
        // 自动重试本轮：任务仍可继续
        m_running = true;
        runRound();
    }
}

void AgentLoop::onStreamFailed(const QString& error, int httpCode, bool willRetry) {
    if (!m_running)
        return;
    if (willRetry) {
        emit streamRetrying(error);
        return;
    }
    // 收尾阶段传输失败：任务本身已跑完，结局不得被改写成失败。
    // 重试一次收尾调用；仍失败则空 JSON 兜底落盘（丢提炼产物，保任务状态与基础记忆）
    if (m_finalizing) {
        if (m_finalizeRetries < 1) {
            ++m_finalizeRetries;
            m_finalizing = false;
            if (auto lg = logutil::logger())
                lg->warn("收尾调用传输失败，重试一次：{}", error.toStdString());
            beginFinalize(m_finalizeOk, m_finalizeSummary);
            return;
        }
        m_finalizing = false;
        finalizeTaskWrites(QJsonObject());
        return;
    }
    // 传输类失败先给故障转移一次机会。
    // 旧实现在这里要求 m_transportFailures >= 2 才调用 handleTransportFailure，但
    // m_transportFailures 在 beginTask 归零、而第一次传输类失败必定走到本函数末尾把任务
    // 判失败（persistTaskEnd + loopFailed）——单个任务内永远攒不到 2 次，switchToFailover
    // 因此成了不可达死代码，"故障转移 + 冷却自动回切"在运行时从未生效。
    // 现在：每个任务最多自动转移一次（m_failedOver 兜底防来回切），备胎也失败才判失败。
    // 注：ChatClient 自身还有 2 次内部重试，故实际 HTTP 尝试次数为 3。
    if (!m_failedOver) {
        m_running = false; // handleTransportFailure 内部可能重启任务
        setState(State::Failed);
        handleTransportFailure(error, httpCode);
        if (m_running)
            return; // 已切换供应商并重跑本轮
    }
    // ⚠️ 必须无条件清 m_running：上面那个 if 只在"尚未转移过"时进入，
    // 于是"转移成功 → 备胎也失败"这条路径会跳过块内的 m_running=false，
    // 留下"已 emit loopFailed、任务行已判失败，但 isRunning() 仍为 true"的僵死态：
    // 新目标只会进 SessionView 的待办队列、beginTask 拒绝启动、Scheduler::tick 永远早退
    // —— 整个任务引擎卡死且无法自愈，只能重启进程。
    m_running = false;
    setState(State::Failed);
    persistTaskEnd(false, QString(), error);
    if (m_deps.events)
        m_deps.events->append(QStringLiteral("error"), m_taskId,
                              QJsonObject{{"error", error}, {"http", httpCode}});
    emit loopFailed(error + (httpCode > 0 ? QStringLiteral("（HTTP %1）").arg(httpCode) : QString()));
}

} // namespace miderforge
