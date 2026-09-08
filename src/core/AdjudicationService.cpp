// 记忆矛盾裁决服务实现
#include "core/AdjudicationService.h"
#include "util/JsonExtract.h"
#include "util/Log.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QVariant>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace miderforge {

AdjudicationService::AdjudicationService(ProviderManager* providers, QObject* parent)
    : QObject(parent), m_providers(providers) {
    // 独立 ChatClient 实例：裁决请求与 AgentLoop 的主链路互不抢占（设计要求）
    m_chat = new ChatClient(this);
    connect(m_chat, &ChatClient::finished, this, [this](const StreamResult& result) {
        if (!m_busy)
            return;
        m_busy = false;
        emit finished(parseVerdicts(result.content, m_pairs));
    });
    connect(m_chat, &ChatClient::failed, this, [this](const QString& error, int, bool willRetry) {
        if (willRetry || !m_busy)
            return;
        m_busy = false;
        emit failed(error);
    });
}

AdjudicationService::~AdjudicationService() = default;

bool AdjudicationService::available() const {
    const ProviderConfig* p = m_providers ? m_providers->activeProvider() : nullptr;
    return p && p->configured;
}

QString AdjudicationService::buildPrompt(const QVector<MemoryManager::ContradictionPair>& pairs) {
    QString list;
    for (int i = 0; i < pairs.size(); ++i) {
        list += QStringLiteral("%1. [#%2] %3  ↔  [#%4] %5\n")
                    .arg(i + 1)
                    .arg(pairs[i].idA)
                    .arg(pairs[i].contentA.left(120))
                    .arg(pairs[i].idB)
                    .arg(pairs[i].contentB.left(120));
    }
    return QStringLiteral(
        "你是记忆一致性审查员。以下每对记忆高度相似但内容不同，请逐对判定关系。\n"
        "verdict 三选一：\n"
        "- contradiction：同一事实互相矛盾的说法（其中一条已过时）\n"
        "- duplicate：同一事实的重复记录（无增量信息）\n"
        "- complement：互补信息（各自有效，不冲突）\n"
        "\n待判定记忆对：\n%1\n"
        "请只输出一个 JSON 对象（不要 markdown 围栏），字段：\n"
        "{\"verdicts\":[{\"a\":<idA>,\"b\":<idB>,\"verdict\":\"...\",\"reason\":\"不超过30字\"}]}\n")
        .arg(list);
}

QVector<AdjudicationService::Verdict>
AdjudicationService::parseVerdicts(const QString& llmText,
                                   const QVector<MemoryManager::ContradictionPair>& pairs) {
    QVector<Verdict> out;
    const QJsonObject obj = jsonextract::extractObject(llmText).object();
    const auto arr = obj.value(QStringLiteral("verdicts")).toArray();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        bool okA = false, okB = false;
        const qint64 a = o.value(QStringLiteral("a")).toVariant().toLongLong(&okA);
        const qint64 b = o.value(QStringLiteral("b")).toVariant().toLongLong(&okB);
        if (!okA || !okB)
            continue;
        QString label = o.value(QStringLiteral("verdict")).toString().toLower();
        if (label != QLatin1String("contradiction") && label != QLatin1String("duplicate")
            && label != QLatin1String("complement"))
            label = QStringLiteral("unknown"); // 标签白名单外不猜
        // 对齐候选对（a/b 顺序宽容）：只有真实存在的对才采纳，防幻觉 id
        bool matched = false;
        for (const auto& p : pairs) {
            if ((p.idA == a && p.idB == b) || (p.idA == b && p.idB == a)) {
                if (!std::any_of(out.begin(), out.end(), [&](const Verdict& x) {
                        return (x.idA == a && x.idB == b) || (x.idA == b && x.idB == a);
                    })) {
                    out.push_back({a, b, label,
                                   o.value(QStringLiteral("reason")).toString().left(80)});
                }
                matched = true;
                break;
            }
        }
        (void)matched;
    }
    return out;
}

void AdjudicationService::judge(const QVector<MemoryManager::ContradictionPair>& pairs) {
    if (m_busy || pairs.isEmpty())
        return;
    const ProviderConfig* provider = m_providers ? m_providers->activeProvider() : nullptr;
    if (!provider || !provider->configured) {
        emit failed(QStringLiteral("未配置大模型供应商，无法进行 AI 裁决"));
        return;
    }
    m_pairs = pairs;
    m_busy = true;
    const QString model = m_providers->modelForTier(*provider, QStringLiteral("fast")); // 判定走 fast 档

    QJsonArray messages;
    QJsonObject sys;
    sys.insert("role", "system");
    sys.insert("content", buildPrompt(pairs));
    messages.append(sys);
    QJsonObject user;
    user.insert("role", "user");
    user.insert("content", QStringLiteral("请输出 JSON。"));
    messages.append(user);

    if (auto lg = logutil::logger())
        lg->info("矛盾裁决：{} 对候选交由 {} 判定", pairs.size(), provider->name.toStdString());
    m_chat->start(*provider, model, messages, QJsonArray());
}

} // namespace miderforge
