// 大模型对话客户端（厂商兼容层）：统一 stream_chat 语义，智谱/DeepSeek 的协议差异全部消化在本层内部。
// 内部持有专属工作线程跑 curl；对外只暴露 Qt 信号；断线/5xx/429 自动指数退避重试 2 次
#pragma once
#include "llm/HttpClient.h"
#include "llm/ToolCallAccumulator.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <QTimer>

namespace miderforge {

struct ProviderConfig;

// 一次流式回合的最终产物
struct StreamResult {
    QString content;
    QString reasoning;       // reasoning_content（思考过程，智谱/DeepSeek-R1 双流字段，踩坑清单 #3）
    QString finishReason;
    QVector<ToolCallParts> toolCalls;
    long long promptTokens = 0;
    long long completionTokens = 0;
    bool aborted = false;    // 用户主动停止
};

Q_DECLARE_METATYPE(miderforge::StreamResult)

class ChatClient : public QObject {
    Q_OBJECT
public:
    explicit ChatClient(QObject* parent = nullptr);
    ~ChatClient() override;

    // 发起一轮流式对话。baseUrl/模型/Key/extra_body 由 ProviderConfig 提供（三档路由在 M4 接入 Router）
    void start(const class ProviderConfig& provider, const QString& model,
               const QJsonArray& messages, const QJsonArray& tools);
    void cancel();

    bool isRunning() const { return m_running; }

signals:
    void started();
    // 派发请求到工作线程（跨线程 queued 投递；executeStream 在 curl 线程执行）
    void requestReady(HttpClient::Request req);
    // 重试前通知 UI：清空本次已渲染的部分内容（流中途断网 → 重连后整体重发）
    void streamReset();
    void thinkingDelta(const QString& delta);
    void contentDelta(const QString& delta);
    // 实时参数碎片（UI 工具卡片可边流边显示）
    void toolCallDelta(int index, const QString& id, const QString& name, const QString& argsSoFar);
    void finished(const miderforge::StreamResult& result);
    void failed(const QString& error, int httpCode, bool willRetry);

private slots:
    void onStreamEvent(const QString& ssePayload);
    void onFinished(int httpCode, const QString& curlError, const QString& errorBody, bool cancelledByUser);

private:
    void dispatch();
    void resetAccumulators();

    HttpClient* m_http = nullptr; // 生命周期归本对象；线程对象 m_thread 退出时一并清理
    QThread m_thread;
    QTimer m_retryTimer;
    HttpClient::Request m_pendingRequest;
    bool m_running = false;
    int m_retriesLeft = 0;
    int m_retryIndex = 0;

    // 流式累积状态（UI 线程内，onStreamEvent 全部在其上执行）
    QString m_content;
    QString m_reasoning;
    ToolCallAccumulator m_toolAcc;
    QString m_finishReason;
    long long m_promptTokens = 0;
    long long m_completionTokens = 0;
    bool m_sawDone = false;
};

} // namespace miderforge
