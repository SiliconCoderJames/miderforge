// 网络与技能工具实现
#include "tools/ExtraTools.h"
#include "core/AppContext.h"
#include "db/Database.h"
#include "memory/MemoryManager.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include "util/NetGuard.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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

// Hermes save/skip 策展门（纯逻辑、可单测）：待办清单/原始转储形态的内容不配进长期记忆。
// 特征：≥3 行都以任务清单记号开头（-、*、•、数字.、□/[ ]）→ 视为待办转储
bool contentIsTaskChecklist(const QString& content) {
    const QStringList lines = content.split(QLatin1Char('\n'));
    int markerLines = 0;
    static const QRegularExpression marker(
        QStringLiteral("^\\s*(-|\\*|•|\\u25a1|\\[[ xX]\\]|\\d+[.、)])\\s+\\S"));
    for (const QString& raw : lines) {
        if (raw.trimmed().isEmpty())
            continue;
        if (marker.match(raw).hasMatch())
            ++markerLines;
    }
    int nonEmpty = 0;
    for (const QString& raw : lines)
        if (!raw.trimmed().isEmpty())
            ++nonEmpty;
    return nonEmpty >= 3 && markerLines >= 3;
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
    registerAll(reg, nullptr, nullptr);
}

void ExtraTools::registerAll(ToolRegistry& reg, Database* db, MemoryManager* mem) {
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

    // ---------- memory_write（Hermes 式三动作：add/replace/remove） ----------
    if (mem) {
        ToolDef mw;
        mw.name = QStringLiteral("memory_write");
        mw.description = QStringLiteral(
            "写入/修订长期记忆（跨会话生效）。【该记】用户偏好与纠正、环境信息、项目约定、"
            "踩坑教训、完成的重要工作；【不该记】琐碎细节、可随时重新发现的信息、原始转储、"
            "会话临时内容。add 自动查重，完全相同的条目会被拒绝。");
        mw.parameters = QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                               {"action", QJsonObject{
                                              {"type", "string"},
                                              {"enum", QJsonArray{"add", "replace", "remove"}},
                                              {"description", "add=新增条目 replace=改写条目内容 remove=废弃条目"},
                                          }},
                               {"type", QJsonObject{
                                            {"type", "string"},
                                            {"enum", QJsonArray{"user_profile", "coding_pref", "task_lesson",
                                                                "project_facts", "session_summary"}},
                                            {"description", "记忆类型（仅 add 需要）"},
                                        }},
                               {"content", QJsonObject{
                                               {"type", "string"},
                                               {"description", "条目内容（add/replace 需要，≤500 字，一事一条）"},
                                           }},
                               {"importance", QJsonObject{
                                                  {"type", "number"},
                                                  {"description", "重要度 0~1（仅 add，默认 0.5）"},
                                              }},
                               {"id", QJsonObject{
                                          {"type", "integer"},
                                          {"description", "条目编号（replace/remove 需要）"},
                                      }},
                           }},
            {"required", QJsonArray{"action"}},
        };
    mw.handler = [mem](const QJsonObject& args, QString* err) -> QString {
        const QString action = args.value("action").toString().trimmed();
        if (action == QLatin1String("add")) {
            // Hermes save/skip 策展门：待办清单式内容、原始转储不配进记忆
            if (contentIsTaskChecklist(args.value("content").toString())) {
                if (err) *err = QStringLiteral("待办/清单类内容不应写入长期记忆（请走任务系统）");
                return {};
            }
            const QString type = args.value("type").toString().trimmed();
                const QString content = args.value("content").toString().trimmed();
                double importance = args.value("importance").toDouble(0.5);
                importance = qBound(0.0, importance, 1.0);
                if (type.isEmpty() || content.isEmpty()) {
                    if (err) *err = QStringLiteral("add 需要 type 与 content");
                    return {};
                }
                if (content.size() > 500) {
                    if (err) *err = QStringLiteral("条目过长（%1 字，上限 500）——请提炼为一事一条").arg(content.size());
                    return {};
                }
                QString reason;
                if (!isSafeMemoryContent(content, &reason)) {
                    if (err) *err = QStringLiteral("内容未通过安全扫描：%1").arg(reason);
                    return {};
                }
                if (mem->hasMemory(type, content)) {
                    return envelope(QJsonObject{{"ok", false},
                                                {"reason", QStringLiteral("已存在完全相同的条目，未重复添加")}});
                }
                const bool approval = AppContext::instance().memoryWriteApproval;
                const qint64 id = mem->addMemory(type, content, importance,
                                                 approval ? QStringLiteral("pending") : QStringLiteral("active"));
                if (id <= 0) {
                    if (err) *err = QStringLiteral("写入失败");
                    return {};
                }
                return envelope(QJsonObject{
                    {"ok", true},
                    {"id", id},
                    {"status", approval ? QStringLiteral("pending") : QStringLiteral("active")},
                    {"note", approval ? QStringLiteral("已暂存，等待人工批准后生效")
                                      : QStringLiteral("已写入长期记忆")},
                });
            }
            if (action == QLatin1String("replace")) {
                const qint64 id = args.value("id").toInteger();
                const QString content = args.value("content").toString().trimmed();
                if (id <= 0 || content.isEmpty()) {
                    if (err) *err = QStringLiteral("replace 需要 id 与 content");
                    return {};
                }
                if (content.size() > 500) {
                    if (err) *err = QStringLiteral("条目过长（%1 字，上限 500）").arg(content.size());
                    return {};
                }
                QString reason;
                if (!isSafeMemoryContent(content, &reason)) {
                    if (err) *err = QStringLiteral("内容未通过安全扫描：%1").arg(reason);
                    return {};
                }
                if (!mem->updateContent(id, content)) {
                    if (err) *err = QStringLiteral("条目不存在或改写失败");
                    return {};
                }
                return envelope(QJsonObject{{"ok", true}, {"id", id}, {"note", QStringLiteral("已改写")}});
            }
            if (action == QLatin1String("remove")) {
                const qint64 id = args.value("id").toInteger();
                if (id <= 0) {
                    if (err) *err = QStringLiteral("remove 需要 id");
                    return {};
                }
                if (!mem->archiveMemory(id)) {
                    if (err) *err = QStringLiteral("条目不存在或废弃失败");
                    return {};
                }
                return envelope(QJsonObject{{"ok", true}, {"id", id}, {"note", QStringLiteral("已废弃（标记，不物理删除）")}});
            }
            if (err) *err = QStringLiteral("未知 action：%1").arg(action);
            return {};
        };
        reg.add(std::move(mw));
    }

    // ---------- session_search（messages 表 FTS5 全文检索，Hermes session_search 同款思路） ----------
    if (db) {
        ToolDef ss;
        ss.name = QStringLiteral("session_search");
        ss.description = QStringLiteral(
            "全文检索历史会话消息（含自己的目标与助手摘要）。当需要回忆「之前做过什么/讨论过什么」时使用，"
            "记忆条目之外的细节都在这里。");
        ss.parameters = QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                               {"query", QJsonObject{
                                             {"type", "string"},
                                             {"description", "关键词（≥2 字符，支持中文）"},
                                         }},
                           }},
            {"required", QJsonArray{"query"}},
        };
        ss.handler = [db](const QJsonObject& args, QString* err) -> QString {
            const QString query = args.value("query").toString().trimmed();
            if (query.size() < 2) {
                if (err) *err = QStringLiteral("查询词至少 2 个字符");
                return {};
            }
            const auto rows = db->query(QStringLiteral(
                "SELECT m.session_id, m.role, m.ts, snippet(messages_fts, 2, '[', ']', '…', 24) AS snip "
                "FROM messages_fts f JOIN messages m ON m.id = f.rowid "
                "WHERE messages_fts MATCH ? ORDER BY rank LIMIT 8"), {query});
            QJsonArray hits;
            for (const auto& r : rows) {
                hits.append(QJsonObject{
                    {"session_id", r.value("session_id").toLongLong()},
                    {"role", r.value("role").toString()},
                    {"time", QDateTime::fromSecsSinceEpoch(r.value("ts").toLongLong())
                                 .toString(QStringLiteral("MM-dd hh:mm"))},
                    {"snippet", r.value("snip").toString()},
                });
            }
            return envelope(QJsonObject{{"ok", true}, {"query", query}, {"hits", hits},
                                        {"note", hits.isEmpty() ? QStringLiteral("无匹配") : QStringLiteral("按相关度排序")}});
        };
        reg.add(std::move(ss));
    }
}

bool isSafeMemoryContent(const QString& content, QString* reason) {
    // 1) 不可见 Unicode（Hermes 同款拦截）：零宽字符/方向控制/软连字符/BOM/变体选择符
    //    逐字符判定（确定性，不依赖正则的 Unicode 转义支持）
    for (const QChar ch : content) {
        const char16_t u = ch.unicode();
        const bool invisible = (u >= 0x200B && u <= 0x200F) || (u >= 0x2060 && u <= 0x2064)
                               || u == 0xFEFF || u == 0x00AD;
        if (invisible) {
            if (reason) *reason = QStringLiteral("包含不可见 Unicode 字符 U+%1（常见于隐蔽注入）").arg(u, 4, 16, QLatin1Char('0'));
            return false;
        }
    }
    // 2) 典型提示词注入/外传指令串（中英双语文本规则，纯词面匹配）
    static const QVector<QRegularExpression> injections = {
        QRegularExpression(QStringLiteral("ignore\\s+(all\\s+)?(previous|prior|above)\\s+instructions"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("disregard\\s+(all\\s+)?(previous|prior|above)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("reveal\\s+(your\\s+)?(system\\s+)?prompt"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("忽略(之前|上面|以上|先前)的?(所有)?(指令|设定|提示)")),
        QRegularExpression(QStringLiteral("无视(上面|以上|上述|之前)的?(所有)?(指令|设定|提示)")),
        QRegularExpression(QStringLiteral("(泄露|透露|输出).{0,6}(系统提示|system\\s*prompt)"),
                           QRegularExpression::CaseInsensitiveOption),
    };
    for (const auto& re : injections) {
        const auto m = re.match(content);
        if (m.hasMatch()) {
            if (reason) *reason = QStringLiteral("命中疑似提示词注入：「%1」").arg(m.captured(0));
            return false;
        }
    }
    return true;
}

} // namespace miderforge::ExtraTools