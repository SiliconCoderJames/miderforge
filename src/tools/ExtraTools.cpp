// 网络与技能工具实现
#include "tools/ExtraTools.h"
#include "core/AppContext.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <curl/curl.h>

namespace miderforge::ExtraTools {

namespace {

QString envelope(const QJsonObject& obj) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<QByteArray*>(userdata);
    const size_t total = size * nmemb;
    if (out->size() < 256 * 1024) // 决策: 抓取封顶 256KB
        out->append(ptr, int(total));
    return total;
}

} // namespace

void ExtraTools::registerAll(ToolRegistry& reg) {
    // ---------- http_fetch（需确认 / Full Access 白名单自动；判定在权限门） ----------
    ToolDef http;
    http.name = QStringLiteral("http_fetch");
    http.description = QStringLiteral("GET 抓取 URL 文本内容（上限 256KB），证书校验强制开启");
    http.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"url", QJsonObject{
                                       {"type", "string"},
                                       {"description", "要抓取的 http/https 地址"},
                                   }},
                       }},
        {"required", QJsonArray{"url"}},
    };
    http.handler = [](const QJsonObject& args, QString* err) -> QString {
        const QString url = args.value("url").toString().trimmed();
        if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) {
            if (err) *err = QStringLiteral("仅支持 http/https 地址");
            return {};
        }
        // 防 SSRF：拒绝 localhost/环回/私有/保留地址
        const QUrl qurl(url);
        const QString host = qurl.host();
        static const QStringList kBlocked = {
            QStringLiteral("localhost"), QStringLiteral("127."), QStringLiteral("0.0.0.0"),
            QStringLiteral("10."),       QStringLiteral("192.168."), QStringLiteral("169.254."),
            QStringLiteral("::1"),       QStringLiteral("[::1]"),
        };
        for (const QString& b : kBlocked) {
            if (host.startsWith(b, Qt::CaseInsensitive)) {
                if (err) *err = QStringLiteral("禁止抓取内网/环回地址：%1").arg(host);
                return {};
            }
        }
        if (host.endsWith(QStringLiteral(".local"), Qt::CaseInsensitive)
            || host.endsWith(QStringLiteral(".internal"), Qt::CaseInsensitive)) {
            if (err) *err = QStringLiteral("禁止抓取内网域名：%1").arg(host);
            return {};
        }

        // 决策: v1 同步执行（UI 冻结 ≤15s 超时可接受）；M4+ 可迁移工作线程
        CURL* curl = curl_easy_init();
        if (!curl) {
            if (err) *err = QStringLiteral("curl 初始化失败");
            return {};
        }
        QByteArray body;
        curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); // 证书校验永不关闭
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Miderforge/0.1");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            if (err) *err = QStringLiteral("抓取失败：%1").arg(QString::fromLatin1(curl_easy_strerror(rc)));
            return {};
        }
        if (httpCode >= 400) {
            if (err) *err = QStringLiteral("HTTP %1").arg(httpCode);
            return {};
        }
        return envelope(QJsonObject{
            {"ok", true},
            {"url", url},
            {"http_code", int(httpCode)},
            {"bytes", int(body.size())},
            {"body", QString::fromUtf8(body)},
        });
    };
    reg.add(std::move(http));

    // ---------- read_skill（三档均自动；渐进披露的全文加载口） ----------
    ToolDef skill;
    skill.name = QStringLiteral("read_skill");
    skill.description = QStringLiteral("加载技能库中某技能的 SKILL.md 全文（技能名见系统提示词可用技能列表）");
    skill.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"name", QJsonObject{
                                        {"type", "string"},
                                        {"description", "技能名（目录名）"},
                                    }},
                       }},
        {"required", QJsonArray{"name"}},
    };
    skill.handler = [](const QJsonObject& args, QString* err) -> QString {
        const QString name = args.value("name").toString().trimmed();
        // 路径穿越防御：技能名只允许字母数字/下划线/连字符/中文
        bool ok = !name.isEmpty();
        for (const QChar ch : name) {
            if (!(ch.isLetterOrNumber() || ch == u'_' || ch == u'-'))
                ok = false;
        }
        if (!ok) {
            if (err) *err = QStringLiteral("技能名不合法");
            return {};
        }
        const QString path = appdirs::file(QStringLiteral("skills/%1/SKILL.md").arg(name));
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("技能不存在：%1").arg(name);
            return {};
        }
        return envelope(QJsonObject{
            {"ok", true},
            {"name", name},
            {"content", QString::fromUtf8(f.readAll())},
        });
    };
    reg.add(std::move(skill));
}

} // namespace miderforge::ExtraTools
