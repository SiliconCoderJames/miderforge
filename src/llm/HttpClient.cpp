// HttpClient 实现：libcurl easy 句柄 RAII、TLS 校验强制开启、低速率断流检测、SSE 分帧
#include "llm/HttpClient.h"
#include "llm/SseParser.h"
#include "util/Log.h"
#include <QThread>
#include <curl/curl.h>
#include <spdlog/spdlog.h>

namespace miderforge {

namespace {
// curl 句柄 RAII 守卫（资源 RAII 纪律）
struct CurlGuard {
    CURL* h = nullptr;
    CurlGuard() : h(curl_easy_init()) {}
    ~CurlGuard() { if (h) curl_easy_cleanup(h); }
    CurlGuard(const CurlGuard&) = delete;
    CurlGuard& operator=(const CurlGuard&) = delete;
    operator CURL*() const { return h; }
};
struct SlistGuard {
    curl_slist* list = nullptr;
    ~SlistGuard() { curl_slist_free_all(list); }
    void append(const QByteArray& line) { list = curl_slist_append(list, line.constData()); }
};
} // namespace

HttpClient::HttpClient(QObject* parent) : QObject(parent) {
    m_sse = std::make_unique<SseParser>();
}

HttpClient::~HttpClient() {
    stopThread();
}

void HttpClient::startThread() {
    // 空实现占位：对象由外部 moveToThread 到专属线程（见 ChatClient 构造）
}

void HttpClient::stopThread() {
    // 线程由持有方（ChatClient）负责 quit/wait；此处仅保证取消标志置位
    m_cancel.store(true);
}

size_t HttpClient::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* self = static_cast<HttpClient*>(userdata);
    const size_t total = size * nmemb;
    if (self->m_cancel.load())
        return 0; // 返回 0 → curl 以 CURLE_WRITE_ERROR 中止传输
    if (self->m_rawBody.size() < 1024 * 1024) {
        self->m_rawBody.append(ptr, int(total));
    } else if (!self->m_rawBodyCapped) {
        // 触顶只告警一次：静默截断会让上层拿到的"错误体"缺尾且无人知晓
        self->m_rawBodyCapped = true;
        if (auto lg = logutil::logger())
            lg->warn("原始响应体超过 1MB 封顶，超出部分丢弃（错误体提取不完整）");
    }

    // 累积缓冲、切完整帧后才发信号（禁止把半个 JSON 帧交给上层）
    self->m_sse->feed(ptr, total);
    for (const QString& ev : self->m_sse->takeCompleteEvents())
        emit self->streamEvent(ev);
    return total;
}

int HttpClient::progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* self = static_cast<HttpClient*>(userdata);
    return self->m_cancel.load() ? 1 : 0; // 非 0 → 中止
}

void HttpClient::executeStream(const Request& req) {
    // 每次请求独立句柄，状态零残留
    m_cancel.store(false);
    m_sse->reset();
    m_rawBody.clear();
    m_rawBodyCapped = false;
    m_lastErrorBody.clear();

    CurlGuard curl;
    if (!curl) {
        emit finished(0, QStringLiteral("curl 初始化失败"), QString(), false);
        return;
    }

    SlistGuard headers;
    headers.append(QByteArray("Content-Type: application/json"));
    headers.append(QByteArray("Accept: text/event-stream"));
    for (const auto& [name, value] : req.headers)
        headers.append((name + ": " + value).toUtf8());

    curl_easy_setopt(curl, CURLOPT_URL, req.url.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.constData());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Miderforge/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpClient::writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &HttpClient::progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this); // 不设置默认为 NULL：progress 回调解引用即崩
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(req.connectTimeoutSec));
    // 低速率断流检测：连续 90 秒吞吐 < 1 B/s 视为死流（防“连接还在但数据断了”）
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);
    // TLS：证书校验强制开启，任何情况下不得关闭（踩坑清单 #6）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // 决策: schannel 后端用系统证书库，无需显式 CAINFO；如换 OpenSSL 后端再由配置注入 CA 路径
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // 启用 gzip 等内置压缩
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    // 流结束后交出残留半帧（容错漏发空行的服务器）
    for (const QString& ev : m_sse->flushRemainder())
        emit streamEvent(ev);

    QString curlError;
    if (rc != CURLE_OK && !m_cancel.load())
        curlError = QString::fromLatin1(curl_easy_strerror(rc));

    if (httpCode >= 400)
        m_lastErrorBody = QString::fromUtf8(m_rawBody.left(4096));

    if (m_cancel.load() && rc != CURLE_OK)
        emit finished(int(httpCode), QStringLiteral("已取消"), m_lastErrorBody, true);
    else
        emit finished(int(httpCode), curlError, m_lastErrorBody, m_cancel.load());

    if (auto lg = logutil::logger())
        lg->info("HTTP {} {} rc={} body={}", httpCode, req.url.toStdString(), int(rc),
                 m_lastErrorBody.left(200).toStdString());
}

} // namespace miderforge
