// Agent 核心循环（M1 完整 ReAct 状态机）：PLANNING→EXECUTING→OBSERVING→[下轮|DONE|HALTED]，
// 三重熔断保险 + 权限门 + 事件审计；记忆/技能注入自 M2/M3 接入
#pragma once
#include "core/Breaker.h"
#include "core/Router.h"
#include "llm/ChatClient.h"
#include "tools/PermissionGate.h"
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <atomic>

namespace miderforge {

class ChatClient;
class Database;
class EventBus;
class MemoryManager;
class SkillManager;
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
        Reflecting,         // 反思中（收尾提炼：摘要/L1/教训）
        Paused,             // M5 P2：已暂停（轮边界挂起，可继续）
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
        Database* db = nullptr;         // M2：tasks 表持久化（可空）
        MemoryManager* mem = nullptr;   // M2：记忆注入与收尾提炼（可空）
        SkillManager* skills = nullptr; // M3：技能注入与自沉淀（可空）
    };
    AgentLoop(Deps deps, QObject* parent = nullptr);

    void start(const QString& goal, qint64 taskId = -1);
    // M5-④ 断点续跑入口：Scheduler 从 tasks.context_json 恢复时传入检查点
    //（history/轮次/路由档/token 预算），任务从断点的下一轮继续而非从头重跑
    void startWithCheckpoint(const QString& goal, qint64 taskId, const QJsonObject& checkpoint);
    void cancel();
    bool isRunning() const { return m_running; }
    long long totalTokens() const { return m_breaker.tokens(); }
    qint64 currentTaskId() const { return m_taskId; }

    // ---- M5 P2 暂停/恢复：轮边界安全点挂起（类人协作：打断→纠正→继续）----
    // pause() 只置请求标志，在下一个工具批收尾边界生效（不打断在跑的工具/流式）；
    // resume() 从挂起状态继续下一轮。挂起期间任务行 status='paused'，取消仍可直接判停
    void pause();
    void resume();
    bool isPaused() const { return m_paused; }
    // M5 P0：进程退出前的有界收束——在跑任务放回 queued（下次启动恢复重新入队），
    // 并请求在跑工具快速中止；不发 loopFinished（UI 即将销毁）
    void shutdownRequeue();

    // 权限卡片回调：decision 0=允许一次 1=本会话总是允许 2=拒绝
    void resumePermission(const QString& callId, int decision);
    // 技能确认回调：保存提案（同名 merge version+1）
    void acceptSkillProposal(const QString& name, const QString& description, const QString& md);

    // 手动重跑单个工具（工具卡「重跑」按钮）：**必须重新过权限门**——
    // 直接调 ToolRegistry::execute 会绕过永不解禁清单与三档权限，所以入口放在这里。
    // 返回执行结果 envelope；被拒绝时返回错误说明，*denied 置位
    QString rerunTool(const QString& toolName, const QString& argsJson, bool* denied = nullptr);

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
    void taskPaused();  // M5 P2：任务在轮边界挂起
    void taskResumed(); // M5 P2：任务从挂起继续
    // 自沉淀提案（工具调用≥5 且成功）：UI 弹确认（或按设置自动通过）后调 acceptSkillProposal
    void skillProposed(const QString& name, const QString& description, const QString& md);

private slots:
    void onStreamFinished(const miderforge::StreamResult& result);
    void onStreamFailed(const QString& error, int httpCode, bool willRetry);

private:
    void setState(State s);
    void beginTask(const QString& goal, qint64 taskId, const QJsonObject& checkpoint); // start/startWithCheckpoint 共用初始化
    void enterPaused(); // M5 P2：在轮边界进入挂起（持久化 paused + 通知 UI）
    void writeCheckpoint(); // M5-④：每轮末/挂起时把可恢复状态持久化到 tasks.context_json
    void runRound();
    void finishToolBatch(); // 工具批执行完：熔断检查 → 下一轮或继续
    void executePendingTool(int decision);
    void executeToolCall(const QString& callId, const QString& name, const QString& argumentsJson);
    void beginFinalize(bool ok, const QString& summaryOrReason); // 任务收尾（规格 9：成败都走）
    void onFinalizeFinished(const StreamResult& result);
    void finalizeTaskWrites(const QJsonObject& parsed); // 收尾产物落库（摘要/L1/教训）
    void persistTaskEnd(bool ok, const QString& resultSummary, const QString& failureReason);
    void stopForUserCancel(); // M5 P3：工具批中检测到取消 → 整体判停为 cancelled
    QString buildSystemPrompt();
    QString buildFinalizePrompt(bool ok, const QString& summaryOrReason) const;
    QString classifyTarget(const QString& toolName, const QString& argsJson, PermissionGate::Kind* kind) const;
    void handleTransportFailure(const QString& error, int httpCode); // M4：连续失败→故障转移

    Deps m_deps;
    PermissionGate m_gate; // 会话级权限门（含"总是允许"记忆）
    Breaker m_breaker;
    QJsonArray m_history;
    QString m_goal;
    qint64 m_taskId = -1;
    int m_round = 0;
    bool m_running = false;
    State m_state = State::Idle;
    bool m_finalizing = false;      // 收尾 LLM 调用进行中
    bool m_finalizeOk = false;      // 本次收尾对应的任务结局
    QString m_finalizeSummary;      // 结局摘要（熔断原因或最终答复）
    int m_finalizeRetries = 0;      // 收尾调用传输失败重试次数（上限 1，见 onStreamFailed）
    QString m_lastReflection;       // 模型最近一轮正文（后续轮次记忆检索的演化查询）
    // 本任务实际注入过的 L3 记忆（id + 内容摘要）：收尾时交给 LLM 做"一致性失效"判定，
    // 只有出现在这份清单里的编号才允许被归档（防幻觉编号误伤）
    QVector<QPair<qint64, QString>> m_injectedMemories;
    int m_toolCallsThisTask = 0;    // 自沉淀判定：工具调用 ≥5 次且成功
    int m_boundaryDenies = 0;       // P1-5b 固化门槛：本任务越界/拒绝事件计数（beginTask 归零）
    // M4 路由与故障转移
    Router::Tier m_tier = Router::Tier::Main;
    int m_consecToolFailures = 0;   // 同一工具连续失败（升档判定）
    bool m_tierEscalated = false;   // 本任务已升档一次
    int m_transportFailures = 0;    // 传输级失败计数（故障转移日志用；是否转移由 m_failedOver 控制）
    bool m_failedOver = false;      // 本任务已切换供应商
    qint64 m_lastPromptTokens = 0;  // 上一轮流prompt用量（增量记账：只收新增输入，避免重发 history 造成 O(N²) 口径）
    bool m_toolCancelSeen = false;  // M5 P3：工具响应取消令牌后置位，工具批收尾时判停
    QString m_terminalStatus;       // M5-①终态覆写（"cancelled"），空=按 ok/m_state 推导
    std::atomic<bool> m_pauseRequested{false}; // M5 P2：暂停请求（轮边界消费）
    bool m_paused = false;          // M5 P2：当前处于挂起状态

    // 待授权工具调用（AwaitingPermission 状态下挂起）
    QString m_pendingCallId;
    QString m_pendingToolName;
    QString m_pendingArgs;
};

} // namespace miderforge
