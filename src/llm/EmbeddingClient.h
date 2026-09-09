// 嵌入客户端（M6-A 语义检索）：libcurl 同步批量调用 /embeddings 端点。
// 阻塞式（含 DNS 解析），仅允许在 Agent 工作线程调用；失败不抛异常，返回 false 并给出错误文本。
// 复用已有聊天供应商的 base_url 与 API Key（如 zhipu），不引入第二套密钥。
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace miderforge {

class EmbeddingClient {
public:
    struct Config {
        QString baseUrl; // 形如 https://open.bigmodel.cn/api/paas/v4（不带 /embeddings 后缀）
        QString apiKey;  // 明文仅内存，落盘走 DPAPI（由 ProviderManager 统一管理）
        QString model = QStringLiteral("embedding-3");
        int timeoutSec = 20;
    };

    EmbeddingClient() = default;
    explicit EmbeddingClient(Config cfg);
    virtual ~EmbeddingClient() = default;

    // 配置是否足以发起调用（缺 baseUrl/模型名视为未配置，检索侧应走纯 FTS5 降级）
    bool valid() const;

    // 批量嵌入：out 与 texts 等长且按序对应。虚函数以便单测注入确定性桩
    virtual bool embed(const QStringList& texts, QVector<QVector<float>>* out, QString* err);

    // ---- 纯函数（暴露给单测） ----
    // 解析 /embeddings 响应：data[].embedding 浮点数组按 data[].index 对位；
    // 响应缺项或各向量维度不一致视为错误
    static bool parseEmbeddings(const QByteArray& body, QVector<QVector<float>>* out, QString* err);
    // 端点校验：仅 https、不允许 userinfo、host 解析结果必须全部为公网地址
    // （SSRF 红线与 fetch 工具同源：拒绝本机/内网/保留段；字面 IP 直接判定，域名才做 DNS）
    static bool checkEndpoint(const QString& baseUrl, QString* err);

private:
    Config m_cfg;
};

} // namespace miderforge
