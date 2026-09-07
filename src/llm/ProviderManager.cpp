// ProviderManager 实现
#include "llm/ProviderManager.h"
#include "util/AppDirs.h"
#include "util/Dpapi.h"
#include "util/Log.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <curl/curl.h>
#include <spdlog/spdlog.h>

namespace miderforge {

namespace {
constexpr const char* kConfigRel = "config/providers.json";
} // namespace

ProviderManager::ProviderManager(QObject* parent) : QObject(parent) {}

ProviderConfig ProviderManager::parseOne(const QJsonObject& obj) const {
    ProviderConfig cfg;
    cfg.name = obj.value("name").toString();
    cfg.baseUrl = obj.value("base_url").toString();
    cfg.role = obj.value("role").toString();
    const QJsonObject tiers = obj.value("tiers").toObject();
    for (const QString& tier : tiers.keys())
        cfg.tiers.insert(tier, tiers.value(tier).toString());
    cfg.extraBody = obj.value("extra_body").toObject();
    const QString enc = obj.value("api_key_dpapi").toString();
    cfg.configured = false;
    if (!enc.isEmpty() && !enc.startsWith(QLatin1Char('<'))) { // 模板占位符视为未配置
        if (auto plain = dpapi::decryptFromBase64(enc)) {
            cfg.key = *plain;
            cfg.configured = !cfg.key.isEmpty();
        }
    }
    return cfg;
}

bool ProviderManager::load() {
    m_providers.clear();
    m_active.clear();
    const QString path = appdirs::file(QString::fromLatin1(kConfigRel));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (auto lg = logutil::logger())
            lg->warn("providers.json 不存在：{}（将进入首次配置向导）", path.toStdString());
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonObject root = doc.object();
    m_active = root.value("active").toString();
    const QJsonArray arr = root.value("providers").toArray();
    for (const auto& v : arr) {
        ProviderConfig cfg = parseOne(v.toObject());
        if (cfg.name.isEmpty())
            continue; // 无名字的条目直接忽略
        if (cfg.tiers.isEmpty())
            continue;
        m_providers.push_back(std::move(cfg));
    }
    if (auto lg = logutil::logger())
        lg->info("providers.json 加载完成：{} 个供应商，active={}", m_providers.size(), m_active.toStdString());
    return !m_providers.empty();
}

bool ProviderManager::initializeFromTemplate() {
    m_providers.clear();
    ProviderConfig zhipu;
    zhipu.name = QStringLiteral("zhipu");
    zhipu.baseUrl = QStringLiteral("https://open.bigmodel.cn/api/paas/v4");
    zhipu.tiers = {{QStringLiteral("fast"), QStringLiteral("glm-5.3-flash")},
                   {QStringLiteral("main"), QStringLiteral("glm-5.3")},
                   {QStringLiteral("flagship"), QStringLiteral("glm-5.3")}};
    zhipu.extraBody = QJsonObject{{"thinking", QJsonObject{{"type", "enabled"}}},
                                  {"reasoning_effort", QStringLiteral("medium")}};
    ProviderConfig deepseek;
    deepseek.name = QStringLiteral("deepseek");
    deepseek.baseUrl = QStringLiteral("https://api.deepseek.com");
    deepseek.role = QStringLiteral("failover_backup");
    deepseek.tiers = {{QStringLiteral("fast"), QStringLiteral("deepseek-chat")},
                      {QStringLiteral("main"), QStringLiteral("deepseek-chat")},
                      {QStringLiteral("flagship"), QStringLiteral("deepseek-reasoner")}};
    m_providers.push_back(std::move(zhipu));
    m_providers.push_back(std::move(deepseek));
    m_active = QStringLiteral("zhipu");
    return writeJson();
}

bool ProviderManager::writeJson() const {
    QDir().mkpath(appdirs::file(QStringLiteral("config")));
    QJsonArray arr;
    for (const auto& cfg : m_providers) {
        QJsonObject obj;
        obj.insert("name", cfg.name);
        obj.insert("base_url", cfg.baseUrl);
        if (!cfg.role.isEmpty())
            obj.insert("role", cfg.role);
        // 密文回写：内存中的明文 key 不落盘
        const ProviderConfig* src = provider(cfg.name);
        QString cipher;
        if (src && src->configured) {
            // 已在内存的条目携带的是解密后的 key；重新加密写回
            if (auto c = dpapi::encryptToBase64(src->key))
                cipher = *c;
        }
        obj.insert("api_key_dpapi", cipher);
        QJsonObject tiers;
        for (auto it = cfg.tiers.constBegin(); it != cfg.tiers.constEnd(); ++it)
            tiers.insert(it.key(), it.value());
        obj.insert("tiers", tiers);
        if (!cfg.extraBody.isEmpty())
            obj.insert("extra_body", cfg.extraBody);
        arr.append(obj);
    }
    QJsonObject root;
    root.insert("active", m_active);
    root.insert("providers", arr);
    QFile f(appdirs::file(QString::fromLatin1(kConfigRel)));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool ProviderManager::saveKey(const QString& providerName, const QString& plainKey) {
    for (auto& cfg : m_providers) {
        if (cfg.name != providerName)
            continue;
        auto cipher = dpapi::encryptToBase64(plainKey);
        if (!cipher)
            return false; // DPAPI 失败绝不写明文兜底
        cfg.key = plainKey;
        cfg.configured = true;
        return writeJson();
    }
    return false;
}

bool ProviderManager::setBaseUrl(const QString& providerName, const QString& url) {
    for (auto& cfg : m_providers) {
        if (cfg.name != providerName)
            continue;
        cfg.baseUrl = url.trimmed();
        return writeJson();
    }
    return false;
}

bool ProviderManager::setActive(const QString& providerName) {
    if (!provider(providerName))
        return false;
    m_active = providerName;
    return writeJson();
}

const ProviderConfig* ProviderManager::provider(const QString& name) const {
    for (const auto& cfg : m_providers)
        if (cfg.name == name)
            return &cfg;
    return nullptr;
}

const ProviderConfig* ProviderManager::activeProvider() const {
    if (auto* p = provider(m_active))
        return p;
    // active 缺失时取第一个已配置的；再不行取第一个
    for (const auto& cfg : m_providers)
        if (cfg.configured)
            return &cfg;
    return m_providers.empty() ? nullptr : &m_providers.front();
}

const ProviderConfig* ProviderManager::failoverProvider() const {
    for (const auto& cfg : m_providers)
        if (cfg.role == QLatin1String("failover_backup"))
            return &cfg;
    return nullptr;
}

QString ProviderManager::modelForTier(const ProviderConfig& cfg, const QString& tier) const {
    if (auto it = cfg.tiers.constFind(tier); it != cfg.tiers.constEnd() && !it->isEmpty())
        return it.value();
    if (auto it = cfg.tiers.constFind(QStringLiteral("main")); it != cfg.tiers.constEnd())
        return it.value();
    return cfg.tiers.isEmpty() ? QString() : cfg.tiers.first();
}

bool ProviderManager::switchToFailover(const QString& reason) {
    const ProviderConfig* backup = failoverProvider();
    if (!backup || !backup->configured || backup->name == m_active)
        return false;
    if (auto lg = logutil::logger())
        lg->warn("故障转移：{} → {}（{}）", m_active.toStdString(), backup->name.toStdString(),
                 reason.toStdString());
    m_active = backup->name;
    m_failedOver = true;
    m_health[m_active] = Health::Unknown;
    const bool ok = writeJson();
    emit providerChanged();
    return ok;
}

void ProviderManager::setHealth(const QString& name, Health h) {
    m_health[name] = h;
    emit providerChanged();
}

bool ProviderManager::testConnection(const QString& name, QString* err, int* latencyMs) {
    const ProviderConfig* cfg = provider(name);
    if (!cfg) {
        if (err) *err = QStringLiteral("供应商不存在");
        return false;
    }
    // 决策: 同步 GET（10s 超时）；仅探测可达性（HTTP 状态 <500 即视为健康）
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (err) *err = QStringLiteral("curl 初始化失败");
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, cfg->baseUrl.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_off_t totalUs = 0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &totalUs);
    curl_easy_cleanup(curl);
    if (latencyMs)
        *latencyMs = static_cast<int>(totalUs / 1000);

    const bool ok = (rc == CURLE_OK && code < 500 && code != 0);
    setHealth(name, ok ? Health::Ok : Health::Fail);
    if (!ok && err)
        *err = rc != CURLE_OK ? QString::fromLatin1(curl_easy_strerror(rc))
                              : QStringLiteral("HTTP %1").arg(code);
    return ok;
}

} // namespace miderforge
