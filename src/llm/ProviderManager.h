// 供应商配置管理：读写 config/providers.json，API Key 仅以 DPAPI 密文落盘；
// M0 提供加载/解密/Key 写入；三档路由与故障转移的完整逻辑在 M4 扩展
#pragma once
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <vector>

namespace miderforge {

struct ProviderConfig {
    QString name;
    QString baseUrl;
    QString role;                // 空=主供应商；"failover_backup"=故障转移备胎
    QString key;                 // 解密后的明文，仅存在于内存
    QMap<QString, QString> tiers; // fast | main | flagship → 模型名
    QJsonObject extraBody;       // 厂商特殊参数（GLM-5.3 必须带 thinking/reasoning_effort，踩坑清单 #4）
    bool configured = false;     // 是否持有可解密的 API Key
};

class ProviderManager : public QObject {
    Q_OBJECT
public:
    explicit ProviderManager(QObject* parent = nullptr);

    // 从 %APPDATA%/Miderforge/config/providers.json 加载；文件不存在时返回 false（可用 initializeFromTemplate）
    bool load();
    // 首次运行：按内置模板创建 zhipu+deepseek 两个条目并写盘（Key 留空待向导补）
    bool initializeFromTemplate();
    // 保存单个供应商的明文 Key：内部 DPAPI 加密后写回 json（落盘的只有密文）
    bool saveKey(const QString& providerName, const QString& plainKey);
    // 更新供应商接口地址并写回（首次向导用）
    bool setBaseUrl(const QString& providerName, const QString& url);
    // 设置当前激活供应商并写回
    bool setActive(const QString& providerName);

    const ProviderConfig* provider(const QString& name) const;
    const ProviderConfig* activeProvider() const;
    // 备胎供应商（role == failover_backup），M4 故障转移用；没有则返回 nullptr
    const ProviderConfig* failoverProvider() const;
    // 三档映射；缺档回退 main 档（最简可行）
    QString modelForTier(const ProviderConfig& cfg, const QString& tier) const;

    const std::vector<ProviderConfig>& all() const { return m_providers; }

    // ---- M4 故障转移与状态 ----
    // 当前供应商连续 2 次 429/超时/5xx 后调用：切到 failover_backup 并写盘；成功返回 true
    bool switchToFailover(const QString& reason);
    bool failedOver() const { return m_failedOver; }
    // 供应商连接状态（供应商视图状态灯：绿=健康/红=故障/灰=未配置）
    enum class Health { Unknown, Ok, Fail };
    Health health(const QString& name) const { return m_health.value(name, Health::Unknown); }
    void setHealth(const QString& name, Health h);
    // 测试连接：GET base_url（10s 超时），更新健康状态并返回结果
    bool testConnection(const QString& name, QString* err = nullptr);

signals:
    void providerChanged(); // 供应商切换/健康状态变化（UI 状态灯）

private:
    bool writeJson() const;
    ProviderConfig parseOne(const QJsonObject& obj) const;

    std::vector<ProviderConfig> m_providers;
    QString m_active;
    bool m_failedOver = false;
    QMap<QString, Health> m_health;
};

} // namespace miderforge
