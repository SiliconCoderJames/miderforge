// 嵌入客户端实现
#include "llm/EmbeddingClient.h"
#include "util/NetGuard.h"
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <curl/curl.h>

namespace miderforge {

namespace {

size_t writeAbsorb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<QByteArray*>(userdata);
    constexpr size_t kMaxBody = 32u * 1024u * 1024u; // 批量×2048 维的浮点文本可达数 MB，封顶 32MB
    if (buf->size() + QByteArray::size_type(size * nmemb) > QByteArray::size_type(kMaxBody))
        return 0; // 返回 0 使 curl 以写错误中止
    buf->append(ptr, QByteArray::size_type(size * nmemb));
    return size * nmemb;
}

} // namespace

EmbeddingClient::EmbeddingClient(Config cfg) : m_cfg(std::move(cfg)) {
    while (m_cfg.baseUrl.endsWith(QLatin1Char('/')))
        m_cfg.baseUrl.chop(1); // 统一去掉尾斜杠，端点 = baseUrl + "/embeddings"
}

bool EmbeddingClient::valid() const {
    return !m_cfg.baseUrl.isEmpty() && !m_cfg.model.isEmpty();
}

bool EmbeddingClient::checkEndpoint(const QString& baseUrl, QString* err) {
    const auto fail = [err](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    const QUrl u(baseUrl);
    if (u.scheme() != QLatin1String("https"))
        return fail(QStringLiteral("嵌入端点必须使用 https"));
    if (!u.userName().isEmpty() || !u.password().isEmpty())
        return fail(QStringLiteral("嵌入端点不允许携带 userinfo"));
    const QString host = u.host();
    if (host.isEmpty())
        return fail(QStringLiteral("嵌入端点缺少主机名"));
    QList<QHostAddress> addrs;
    const QHostAddress literal(host);
    if (!literal.isNull()) {
        addrs << literal;
    } else {
        addrs = QHostInfo::fromName(host).addresses(); // 阻塞 DNS：仅允许工作线程调用
    }
    if (addrs.isEmpty())
        return fail(QStringLiteral("嵌入端点主机解析失败：%1").arg(host));
    for (const auto& a : addrs)
        if (!netguard::isPublicIp(a))
            return fail(QStringLiteral("嵌入端点必须是公网地址（拒绝本机/内网/保留段）：%1").arg(a.toString()));
    return true;
}

bool EmbeddingClient::embed(const QStringList& texts, QVector<QVector<float>>* out, QString* err) {
    out->clear();
    if (texts.isEmpty())
        return true;
    if (!valid()) {
        if (err)
            *err = QStringLiteral("嵌入器未配置（缺 base_url 或模型名）");
        return false;
    }
    QString verr;
    if (!checkEndpoint(m_cfg.baseUrl, &verr)) {
        if (err)
            *err = verr;
        return false;
    }

    const QJsonObject body{{"model", m_cfg.model},
                           {"input", QJsonArray::fromStringList(texts)}};
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (err)
            *err = QStringLiteral("curl 初始化失败");
        return false;
    }
    const QByteArray url = (m_cfg.baseUrl + QStringLiteral("/embeddings")).toUtf8();
    const QByteArray auth = "Authorization: Bearer " + m_cfg.apiKey.toUtf8();
    QByteArray resp;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.constData());
    curl_easy_setopt(curl, CURLOPT_URL, url.constData());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.constData());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeAbsorb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_cfg.timeoutSec));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // 不跟随重定向：端点已按公网 https 校验，跟随跳转会绕过这道校验
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (err)
            *err = QStringLiteral("嵌入请求失败：%1").arg(QString::fromLatin1(curl_easy_strerror(rc)));
        return false;
    }
    if (code < 200 || code >= 300) {
        if (err)
            *err = QStringLiteral("嵌入请求 HTTP %1：%2").arg(code).arg(QString::fromUtf8(resp.left(512)));
        return false;
    }
    return parseEmbeddings(resp, out, err);
}

bool EmbeddingClient::parseEmbeddings(const QByteArray& body, QVector<QVector<float>>* out, QString* err) {
    const auto fail = [out, err](const QString& msg) {
        out->clear();
        if (err)
            *err = QStringLiteral("嵌入响应解析失败：%1").arg(msg);
        return false;
    };
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("不是合法 JSON 对象：%1").arg(pe.errorString()));
    const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
    if (data.isEmpty()) {
        out->clear(); // 空结果交由调用方做条数对账
        return true;
    }
    out->resize(data.size());
    int expectDim = -1;
    for (int d = 0; d < data.size(); ++d) {
        const QJsonObject item = data.at(d).toObject();
        const int idx = item.value(QStringLiteral("index")).toVariant().toInt();
        if (idx < 0 || idx >= data.size())
            return fail(QStringLiteral("index 越界：%1").arg(idx));
        const QJsonArray arr = item.value(QStringLiteral("embedding")).toArray();
        if (arr.isEmpty())
            return fail(QStringLiteral("第 %1 项 embedding 为空").arg(idx));
        if (expectDim < 0) {
            expectDim = arr.size();
        } else if (arr.size() != expectDim) {
            return fail(QStringLiteral("向量维度不一致（%1 vs %2）").arg(expectDim).arg(arr.size()));
        }
        QVector<float> vec(arr.size());
        for (int i = 0; i < arr.size(); ++i)
            vec[i] = float(arr.at(i).toDouble());
        (*out)[idx] = std::move(vec);
    }
    for (const auto& v : *out)
        if (v.isEmpty())
            return fail(QStringLiteral("响应缺项（index 不连续）"));
    return true;
}

} // namespace miderforge
