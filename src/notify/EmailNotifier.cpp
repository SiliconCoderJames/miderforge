// 邮件通知实现
#include "notify/EmailNotifier.h"
#include "core/EventBus.h"
#include "util/AppDirs.h"
#include "util/Dpapi.h"
#include "util/Log.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <curl/curl.h>
#include <cstring>
#include <spdlog/spdlog.h>

namespace miderforge {

namespace {
struct ReadCtx {
    QByteArray data;
    qint64 pos = 0;
};

// SMTP 上传读回调：把整封 MIME 消息喂给 curl
size_t readCallback(char* buffer, size_t size, size_t nitems, void* ud) {
    auto* ctx = static_cast<ReadCtx*>(ud);
    const size_t want = size * nitems;
    const size_t remain = size_t(ctx->data.size() - ctx->pos);
    const size_t n = qMin(want, remain);
    memcpy(buffer, ctx->data.constData() + ctx->pos, n);
    ctx->pos += qint64(n);
    return n;
}
} // namespace

EmailNotifier::EmailNotifier(EventBus* events, QObject* parent) : QObject(parent), m_events(events) {}

bool EmailNotifier::loadConfig() {
    QFile f(appdirs::file(QStringLiteral("config/notify.json")));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    m_cfg.smtpUrl = obj.value("smtp_url").toString(QStringLiteral("smtps://smtp.qq.com:465"));
    m_cfg.from = obj.value("from").toString();
    m_cfg.to = obj.value("to").toString();
    m_cfg.enabled = obj.value("enabled").toBool();
    const QString enc = obj.value("auth_code_dpapi").toString();
    if (!enc.isEmpty()) {
        if (auto plain = dpapi::decryptFromBase64(enc))
            m_cfg.authCode = *plain;
    }
    return !m_cfg.from.isEmpty() && !m_cfg.to.isEmpty();
}

bool EmailNotifier::saveConfig(const Config& cfg) {
    QDir().mkpath(appdirs::file(QStringLiteral("config")));
    QJsonObject obj;
    obj.insert("smtp_url", cfg.smtpUrl);
    obj.insert("from", cfg.from);
    obj.insert("to", cfg.to);
    obj.insert("enabled", cfg.enabled);
    if (auto enc = dpapi::encryptToBase64(cfg.authCode); enc && !cfg.authCode.isEmpty())
        obj.insert("auth_code_dpapi", *enc); // 落盘只有密文（踩坑清单 #9）
    QFile f(appdirs::file(QStringLiteral("config/notify.json")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(obj).toJson());
    m_cfg = cfg;
    return true;
}

QString EmailNotifier::rfc2047Encode(const QString& text) {
    // =?UTF-8?B?<base64>?=；每段 base64 载荷 ≤ 60 字符，段间以空格折叠
    const QByteArray utf8 = text.toUtf8();
    QString out;
    const int chunk = 40; // 源字节每 40 字节一段
    for (int i = 0; i < utf8.size(); i += chunk) {
        if (!out.isEmpty())
            out += QStringLiteral(" ");
        out += QStringLiteral("=?UTF-8?B?%1?=")
                   .arg(QString::fromLatin1(utf8.mid(i, chunk).toBase64()));
    }
    return out;
}

bool EmailNotifier::send(const QString& subject, const QString& htmlBody, QString* err) {
    if (!m_cfg.enabled || m_cfg.smtpUrl.isEmpty() || m_cfg.from.isEmpty() || m_cfg.to.isEmpty()) {
        if (err) *err = QStringLiteral("邮件未启用或配置不完整");
        return false;
    }
    // MIME 消息：UTF-8 HTML 正文（规格 11）
    const QString mime = QStringLiteral(
        "From: <%1>\r\n"
        "To: <%2>\r\n"
        "Subject: %3\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n"
        "%4")
                             .arg(m_cfg.from, m_cfg.to, rfc2047Encode(subject), htmlBody);

    // 失败重试 1 次（规格 14）
    QString lastErr;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        ok = sendOnce(mime, &lastErr);
        if (auto lg = logutil::logger(); !ok && lg)
            lg->warn("邮件发送第 {} 次失败：{}", attempt + 1, lastErr.toStdString());
    }
    if (m_events)
        m_events->append(QStringLiteral("email_sent"), -1,
                         QJsonObject{{"subject", subject}, {"ok", ok}, {"error", lastErr}});
    if (!ok && err)
        *err = lastErr;
    return ok;
}

bool EmailNotifier::sendOnce(const QString& mime, QString* err) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (err) *err = QStringLiteral("curl 初始化失败");
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, m_cfg.smtpUrl.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_cfg.from.toUtf8().constData()); // 授权码认证
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_cfg.authCode.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); // TLS 证书校验永不关闭（规格 14）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ("<" + m_cfg.from + ">").toUtf8().constData());

    curl_slist* rcpt = nullptr;
    const QByteArray to = ("<" + m_cfg.to + ">").toUtf8();
    rcpt = curl_slist_append(rcpt, to.constData());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);

    ReadCtx ctx{mime.toUtf8(), 0}; // 局部变量：static 会在并发 send 时互相覆盖正文
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, &readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        if (err) *err = QString::fromLatin1(curl_easy_strerror(rc));
        return false;
    }
    return true;
}

} // namespace miderforge
