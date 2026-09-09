// 网络与技能工具实现
#include "tools/ExtraTools.h"
#include "core/AppContext.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include "util/NetGuard.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
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

// M5 P3：取消令牌进度回调——传输期间每块数据都会路过这里，置 1 即中止
int fetchProgressAbort(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* reg = static_cast<const ToolRegistry*>(userdata);
    return reg->toolCancelRequested() ? 1 : 0;
}

} // namespace

bool checkFetchUrl(const QString& url, QString* pinnedIp, QString* err) {
    const QUrl qurl(url);
    if (qurl.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0
        && qurl.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) {
        if (err) *err = QStringLiteral("仅支持 http/https 地址");
        return false;
    }
    if (!qurl.userName().isEmpty() || !qurl.password().isEmpty()) {
        if (err) *err = QStringLiteral("不允许携带用户信息的 URL");
        return false;
    }
    const QString host = qurl.host();
    if (host.isEmpty()) {
        if (err) *err = QStringLiteral("URL 缺少主机名");
        return false;
    }
    // 字面量第一道：本机名 + 内网保留后缀（解析前拦截，错误信息更明确）
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
        if (err) *err = QStringLiteral("禁止抓取本机地址");
        return false;
    }
    static const QStringList kBlockedSuffixes = {
        QStringLiteral(".local"),    QStringLiteral(".internal"), QStringLiteral(".localhost"),
        QStringLiteral(".test"),     QStringLiteral(".example"),  QStringLiteral(".arpa"),
        QStringLiteral(".lan"),      QStringLiteral(".home"),     QStringLiteral(".corp"),
    };
    for (const QString& s : kBlockedSuffixes) {
        if (host.endsWith(s, Qt::CaseInsensitive)) {
            if (err) *err = QStringLiteral("禁止抓取内网/保留域名：%1").arg(host);
            return false;
        }
    }
    // 关键一道：DNS 解析后按 IP 公网性判定。
    // 前缀匹配可被 0x7f000001 / 2130706433 / ::ffff:127.0.0.1 / DNS rebinding 绕过（OWASP SSRF），
    // 解析归一后这些全部落回真实 IP 判定
    const QHostInfo resolved = QHostInfo::fromName(host);
    if (resolved.error() != QHostInfo::NoError || resolved.addresses().isEmpty()) {
        if (err) *err = QStringLiteral("域名解析失败：%1").arg(host);
        return false;
    }
    QString firstGlobal;
    for (const QHostAddress& addr : resolved.addresses()) {
        if (netguard::isPublicIp(addr)) { // 严格公网判定（Qt isGlobal 漏 RFC1918，见 NetGuard.h）

            if (firstGlobal.isEmpty())
                firstGlobal = addr.toString();
        } else {
            if (err)
                *err = QStringLiteral("禁止抓取：%1 解析到非公网地址 %2").arg(host, addr.toString());
            return false;
        }
    }
    if (pinnedIp)
        *pinnedIp = firstGlobal;
    return true;
}

void ExtraTools::registerAll(ToolRegistry& reg) {
    // ---------- http_fetch（需确认 / Full Access 白名单自动；判定在权限门） ----------
    ToolDef http;
    http.name = QStringLiteral("http_fetch");
    http.description = QStringLiteral("GET 抓取 URL 文本内容（上限 256KB），证书校验强制开启，内网地址被拦截");
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
    http.handler = [&reg](const QJsonObject& args, QString* err) -> QString {
        // M5 P3：取消令牌入口快检
        if (reg.toolCancelRequested()) {
            if (err) *err = QStringLiteral("已被用户取消");
            return envelope(QJsonObject{{"ok", false}, {"cancelled", true}});
        }
        QString currentUrl = args.value("url").toString().trimmed();

        CURL* curl = curl_easy_init();
        if (!curl) {
            if (err) *err = QStringLiteral("curl 初始化失败");
            return {};
        }
        QByteArray body;
        long httpCode = 0;
        CURLcode rc = CURLE_OK;

        // 手动重定向循环：每一跳都重新过 SSRF 校验并钉住解析 IP。
        // （开 FOLLOWLOCATION 的话，公网页面 302 到 http://127.0.0.1 会绕过入口校验）
        constexpr int kMaxHops = 4;
        for (int hop = 0; hop < kMaxHops; ++hop) {
            QString pinnedIp;
            if (!checkFetchUrl(currentUrl, &pinnedIp, err)) {
                curl_easy_cleanup(curl);
                return {};
            }
            const QUrl u(currentUrl);
            const int port = u.port(u.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
                                        ? 443
                                        : 80);
            // 钉住校验时解析到的 IP：curl 不再二次解析，杜绝"校验用 A 记录、抓取被 rebinding 到内网"的窗口。
            // RESOLVE 条目格式 HOST:PORT:ADDRESS，三个占位符逐一替换（漏一次 %3 就会拼出 %3IP 的废条目，
            // curl 忽略废条目后回退自主 DNS，防护静默失效）；IPv6 地址必须加方括号与端口号冒号区分（curl ≥7.59）
            const QString pin = QStringLiteral("%1:%2:%3")
                                    .arg(u.host())
                                    .arg(port)
                                    .arg(u.host().contains(QLatin1Char(':'))
                                             ? QStringLiteral("[%1]").arg(pinnedIp)
                                             : pinnedIp);
            curl_slist* resolveList = curl_slist_append(nullptr, pin.toUtf8().constData());

            body.clear();
            curl_easy_reset(curl);
            curl_easy_setopt(curl, CURLOPT_URL, currentUrl.toUtf8().constData());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeToString);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
            curl_easy_setopt(curl, CURLOPT_RESOLVE, resolveList);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &fetchProgressAbort);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &reg);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); // 证书校验永不关闭
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Miderforge/0.1");
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            rc = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(resolveList);

            if (rc != CURLE_OK)
                break;
            if (httpCode >= 300 && httpCode < 400) {
                char* loc = nullptr;
                curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &loc);
                if (loc) {
                    currentUrl = QString::fromUtf8(loc);
                    curl_free(loc);
                    if (hop == kMaxHops - 1) {
                        if (err) *err = QStringLiteral("重定向次数过多（>%1）").arg(kMaxHops);
                        curl_easy_cleanup(curl);
                        return {};
                    }
                    continue; // 下一跳重新校验
                }
            }
            break;
        }
        curl_easy_cleanup(curl);

        // M5 P3：进度回调中止 → 判定为用户取消（区别于网络错误）
        if (rc == CURLE_ABORTED_BY_CALLBACK && reg.toolCancelRequested()) {
            if (err) *err = QStringLiteral("已被用户取消");
            return envelope(QJsonObject{{"ok", false}, {"cancelled", true}});
        }
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
            {"url", currentUrl},
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
