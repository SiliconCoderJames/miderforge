// ChatClient 实现：payload 组装、SSE 帧解析（双流字段+工具碎片）、指数退避重试
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "util/Log.h"
#include "util/Tokens.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrl>
#include <spdlog/spdlog.h>

namespace miderforge {

ChatClient::ChatClient(QObject* parent) : QObject(parent) {
    qRegisterMetaType<HttpClient::Request>();
    qRegisterMetaType<StreamResult>();

    m_http = new HttpClient(nullptr); // 无 parent 才能 moveToThread
    m_http->moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("miderforge-http"));
    m_thread.start();

    connect(this, &ChatClient::requestReady, m_http, &HttpClient::executeStream); // 自动 QueuedConnection
    connect(m_http, &HttpClient::streamEvent, this, &ChatClient::onStreamEvent);
    connect(m_http, &HttpClient::finished, this, &ChatClient::onFinished);

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_running)
            dispatch();
    });
}

ChatClient::~ChatClient() {
    m_retryTimer.stop();
    // 先取消在飞传输再退线程：quit() 只在事件循环空闲时生效，curl 仍阻塞在 executeStream
    // 槽里时线程退不出去，wait 超时走泄漏路径（进程退出时带着活线程，偶发 fastfail）。
    // cancelActive 是原子标志，progress 回调在下一块数据/连接阶段即中止传输，槽随即返回
    m_http->cancelActive();
    m_thread.quit();
    if (!m_thread.wait(10000)) {
        // 有界等待：卡死的 curl（如 DNS 悬挂）不再把整个进程拖成"无响应假死"。
        // 放弃等待并故意泄漏线程与 HttpClient（进程退出时由 OS 回收），
        // 绝不 delete 一个仍在别的线程执行 curl 的 QObject——那是必崩路径
        if (auto lg = logutil::logger())
            lg->critical("HTTP 线程 10s 未退出，放弃等待（退出时由系统回收）");
        return;
    }
    delete m_http;
    m_http = nullptr;
}

void ChatClient::start(const ProviderConfig& provider, const QString& model,
                       const QJsonArray& messages, const QJsonArray& tools) {
    if (m_running) {
        emit failed(QStringLiteral("已有请求在进行中"), 0, false);
        return;
    }
    if (!provider.configured) {
        emit failed(QStringLiteral("供应商 %1 未配置 API Key，请先完成首次配置").arg(provider.name), 0, false);
        return;
    }

    // ---- payload 组装（OpenAI 兼容 /chat/completions） ----
    QJsonObject payload;
    payload.insert("model", model);
    payload.insert("messages", messages);
    if (!tools.isEmpty())
        payload.insert("tools", tools);
    payload.insert("stream", true);
    // extra_body 逐键并入根级：GLM-5.3 必须带 thinking.type=enabled 与 reasoning_effort（踩坑清单 #4）
    for (auto it = provider.extraBody.constBegin(); it != provider.extraBody.constEnd(); ++it)
        payload.insert(it.key(), it.value());
    // 决策: DeepSeek 需要 stream_options.include_usage 才回 usage；其他厂商加该字段可能报错，按厂商名区分
    if (provider.name.contains(QLatin1String("deepseek"), Qt::CaseInsensitive)) {
        payload.insert("stream_options", QJsonObject{{"include_usage", true}});
    }

    m_pendingRequest.url = provider.baseUrl.endsWith(QLatin1Char('/'))
                               ? provider.baseUrl + QLatin1String("chat/completions")
                               : provider.baseUrl + QLatin1String("/chat/completions");
    m_pendingRequest.headers.clear();
    m_pendingRequest.headers.append({QStringLiteral("Authorization"), QStringLiteral("Bearer ") + provider.key});
    m_pendingRequest.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    if (auto lg = logutil::logger())
        lg->info("stream start: provider={} model={} url={}", provider.name.toStdString(),
                 model.toStdString(), m_pendingRequest.url.toStdString());

    resetAccumulators();
    m_retriesLeft = 2; // 指数退避重试 2 次
    m_retryIndex = 0;
    m_running = true;
    emit started();
    dispatch();
}

void ChatClient::dispatch() {
    // 经信号跨线程 queued 投递：executeStream 在工作线程执行（线程纪律，无字符串元类型名风险）
    emit requestReady(m_pendingRequest);
}

void ChatClient::cancel() {
    if (m_retryTimer.isActive()) {
        m_retryTimer.stop();
        m_running = false;
        StreamResult r;
        r.aborted = true;
        emit finished(r);
        return;
    }
    m_http->cancelActive(); // 原子标志，写回调与进度回调会在下一块数据时中止
}

void ChatClient::resetAccumulators() {
    m_content.clear();
    m_reasoning.clear();
    m_toolAcc.reset();
    m_finishReason.clear();
    m_promptTokens = 0;
    m_completionTokens = 0;
    m_sawDone = false;
}

void ChatClient::onStreamEvent(const QString& ssePayload) {
    if (ssePayload.trimmed() == QLatin1String("[DONE]")) {
        m_sawDone = true; // 哨兵帧：流式正常收尾标志
        return;
    }
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(ssePayload.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        // 容错：残帧/心跳注释直接忽略并记录，不崩溃
        if (auto lg = logutil::logger())
            lg->warn("SSE 帧解析失败: {} | 片段: {}", parseErr.errorString().toStdString(),
                     ssePayload.left(120).toStdString());
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value("choices").toArray();
    if (!choices.isEmpty()) {
        const QJsonObject choice = choices.at(0).toObject();
        const QJsonObject delta = choice.value("delta").toObject();

        const QString reasoning = delta.value("reasoning_content").toString(); // 缺字段当空处理
        if (!reasoning.isEmpty()) {
            m_reasoning += reasoning;
            emit thinkingDelta(reasoning);
        }
        const QString content = delta.value("content").toString();
        if (!content.isEmpty()) {
            m_content += content;
            emit contentDelta(content);
        }
        const QJsonArray tcs = delta.value("tool_calls").toArray();
        for (const auto& tcVal : tcs) {
            const QJsonObject tc = tcVal.toObject();
            const int index = tc.contains("index") ? tc.value("index").toInt() : 0;
            const QString id = tc.value("id").toString();
            const QJsonObject fn = tc.value("function").toObject();
            const QString nameDelta = fn.value("name").toString();
            const QString argsDelta = fn.value("arguments").toString();
            m_toolAcc.feedDelta(index, id, nameDelta, argsDelta);
            const ToolCallParts* acc = m_toolAcc.findByIndex(index);
            emit toolCallDelta(index, id, nameDelta, acc ? acc->arguments : argsDelta);
        }
        const QString fr = choice.value("finish_reason").toString();
        if (!fr.isEmpty())
            m_finishReason = fr;
    }
    // usage 一般只在末帧出现；缺字段保持 0
    const QJsonObject usage = root.value("usage").toObject();
    if (!usage.isEmpty()) {
        m_promptTokens = usage.value("prompt_tokens").toInt();
        m_completionTokens = usage.value("completion_tokens").toInt();
    }
}

void ChatClient::onFinished(int httpCode, const QString& curlError, const QString& errorBody, bool cancelledByUser) {
    if (!m_running)
        return; // 迟到的旧传输回调，丢弃

    if (cancelledByUser) {
        m_running = false;
        StreamResult r;
        r.aborted = true;
        emit finished(r);
        return;
    }

    const bool transportDead = !curlError.isEmpty();
    const bool retriableStatus = (httpCode == 429 || httpCode >= 500);
    const bool clientError = httpCode >= 400 && !retriableStatus;

    if ((transportDead || retriableStatus) && m_retriesLeft > 0) {
        // 断线/限流/服务端错误：指数退避 + 随机抖动后整体重发。
        // 无上限的 2^n 会失控、无抖动的整秒重试会撞供应商限流窗口（backoff + jitter）
        --m_retriesLeft;
        ++m_retryIndex;
        const int baseMs = qMin(1000 * (1 << (m_retryIndex - 1)), 8000); // 退避上限 8s
        const int backoffMs = baseMs + QRandomGenerator::global()->bounded(qMax(1, baseMs / 4));
        emit streamReset(); // UI 清空本次部分渲染
        resetAccumulators();
        if (auto lg = logutil::logger())
            lg->warn("流式请求失败（http={} err={}），{}ms 后第 {} 次重试", httpCode,
                     curlError.toStdString(), backoffMs, m_retryIndex);
        emit failed(curlError.isEmpty() ? QStringLiteral("HTTP %1").arg(httpCode) : curlError,
                    httpCode, true);
        m_retryTimer.start(backoffMs);
        return;
    }

    if (transportDead || retriableStatus || clientError || httpCode == 0) {
        m_running = false;
        QString msg = curlError.isEmpty() ? QStringLiteral("HTTP %1").arg(httpCode) : curlError;
        if (!errorBody.isEmpty()) {
            // 透出 API 侧错误说明（智谱/DeepSeek 的 error.message）
            const QJsonDocument errDoc = QJsonDocument::fromJson(errorBody.toUtf8());
            const QJsonObject errObj = errDoc.object();
            const QString apiMsg = errObj.value("error").toObject().value("message").toString();
            if (!apiMsg.isEmpty())
                msg += QStringLiteral("：") + apiMsg;
            else
                msg += QStringLiteral("：") + errorBody.left(200);
        }
        emit failed(msg, httpCode, false);
        return;
    }

    // ---- 正常收尾：终结工具调用参数（仅此刻才 parse，踩坑清单 #2） ----
    m_running = false;
    StreamResult r;
    r.content = m_content;
    r.reasoning = m_reasoning;
    r.finishReason = m_finishReason;
    bool argsOk = true;
    r.toolCalls = m_toolAcc.finalizeAll(&argsOk);
    if (!argsOk) {
        // 参数拼装失败：不致命，丢弃坏调用并记录（上层据此可看到空工具轮）
        if (auto lg = logutil::logger())
            lg->error("工具调用参数 JSON 解析失败，已丢弃");
        emit failed(QStringLiteral("工具调用参数解析失败（模型输出不合法）"), httpCode, false);
        return;
    }
    r.promptTokens = m_promptTokens;
    r.completionTokens = m_completionTokens > 0 ? m_completionTokens
                                                : tokens::estimate(m_content + m_reasoning); // usage 缺失时兜底估算
    r.aborted = false;
    emit finished(r);
}

} // namespace miderforge
