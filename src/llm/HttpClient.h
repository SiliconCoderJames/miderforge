// 工作线程 HTTP 客户端（libcurl）：请求只在专属 QThread 内执行，
// 一切结果（SSE 帧/完成/失败）只经 Qt 信号自动 QueuedConnection 回 UI 线程（踩坑清单 #5 线程纪律）
#pragma once
#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QString>
#include <QList>
#include <atomic>
#include <curl/curl.h>
#include <memory>

namespace miderforge {

class SseParser;

class HttpClient : public QObject {
    Q_OBJECT
public:
    struct Request {
        QString url;
        QList<QPair<QString, QString>> headers;
        QByteArray body;
        int connectTimeoutSec = 15;
    };


    explicit HttpClient(QObject* parent = nullptr);
    ~HttpClient() override;

    void startThread();
    void stopThread();

    // 取消当前传输（线程安全）：写回调读到标志即中止
    void cancelActive() { m_cancel.store(true); }

public slots:
    // 在工作线程执行一次流式 POST；每个完整 SSE 帧经 streamEvent 信号交出
    void executeStream(const Request& req);

signals:
    // 一个完整 SSE 事件载荷（已剥离 "data:" 前缀与行尾符）
    void streamEvent(const QString& ssePayload);
    // 传输结束：httpCode 为 HTTP 状态码（0=未到达），curlError 为 curl 层错误文本，
    // errorBody 为 4xx/5xx 时响应体的前 4KB（供上层展示 API 错误信息）
    void finished(int httpCode, const QString& curlError, const QString& errorBody, bool cancelledByUser);

private:
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t);

    std::atomic<bool> m_cancel{false};
    std::unique_ptr<SseParser> m_sse;
    QByteArray m_rawBody;      // 全量原始响应（错误体提取用，封顶 1MB）
    QString m_lastErrorBody;
};

} // namespace miderforge

Q_DECLARE_METATYPE(miderforge::HttpClient::Request)
