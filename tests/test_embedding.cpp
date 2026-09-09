// 嵌入客户端单测（M6-A）：响应解析 / 端点校验（无网络的确定性路径）
// 真实网络调用不做单测（需要 Key），端到端项记录在 docs/ACCEPTANCE.md
#include "llm/EmbeddingClient.h"
#include "tools/ExtraTools.h"
#include "util/NetGuard.h"
#include <QHostAddress>
#include <doctest/doctest.h>

using miderforge::EmbeddingClient;
using miderforge::netguard::isPublicIp;

TEST_CASE("netguard 严格公网判定：RFC1918/CGNAT/保留段全部拒绝（fetch 与嵌入共用，Qt isGlobal 漏私有段回归）") {
    auto check = [](const char* s) { return isPublicIp(QHostAddress(QString::fromLatin1(s))); };
    CHECK_FALSE(check("192.168.1.5"));  // RFC1918（Qt isGlobal 误判为全局的段）
    CHECK_FALSE(check("10.0.0.3"));     // RFC1918
    CHECK_FALSE(check("172.16.0.9"));   // RFC1918
    CHECK_FALSE(check("172.31.255.1")); // RFC1918 上沿
    CHECK_FALSE(check("100.64.0.1"));   // CGNAT
    CHECK_FALSE(check("198.18.0.7"));   // 基准测试段
    CHECK_FALSE(check("203.0.113.9"));  // TEST-NET-3
    CHECK_FALSE(check("224.0.0.1"));    // 组播
    CHECK_FALSE(check("127.0.0.1"));    // 回环
    CHECK_FALSE(check("169.254.2.3"));  // 链路本地
    CHECK_FALSE(check("::1"));          // IPv6 回环
    CHECK_FALSE(check("fd12::1"));      // IPv6 ULA
    CHECK_FALSE(check("2001:db8::5"));  // IPv6 文档段
    CHECK(check("8.8.8.8"));
    CHECK(check("1.1.1.1"));
    CHECK(check("2606:4700::1111"));    // 公网 IPv6
    // fetch 工具走同一判定的回归：字面私有 IP 一律拒绝，公网字面 IP 放行
    QString err;
    CHECK_FALSE(miderforge::ExtraTools::checkFetchUrl(QStringLiteral("https://192.168.1.1/admin"), nullptr, &err));
    CHECK_FALSE(miderforge::ExtraTools::checkFetchUrl(QStringLiteral("https://10.1.2.3/"), nullptr, &err));
    CHECK(miderforge::ExtraTools::checkFetchUrl(QStringLiteral("https://8.8.8.8/"), nullptr, &err));
}

TEST_CASE("parseEmbeddings：zhipu 形响应按 index 对位且维度一致") {
    const QByteArray body = R"({
        "data": [
            {"index": 1, "embedding": [0.25, -0.5, 0.75], "object": "embedding"},
            {"index": 0, "embedding": [1.0, 0.0, -1.0], "object": "embedding"}
        ],
        "model": "embedding-3",
        "usage": {"prompt_tokens": 12, "total_tokens": 12}
    })";
    QVector<QVector<float>> out;
    QString err;
    CHECK(EmbeddingClient::parseEmbeddings(body, &out, &err));
    REQUIRE(out.size() == 2);
    CHECK(out[0].size() == 3);
    CHECK(out[0][0] == doctest::Approx(1.0f));
    CHECK(out[0][2] == doctest::Approx(-1.0f));
    CHECK(out[1][1] == doctest::Approx(-0.5f)); // index=1 对位到 out[1]
}

TEST_CASE("parseEmbeddings：非法 JSON / 维度不一致 / 缺项 / index 越界均报错") {
    QVector<QVector<float>> out;
    QString err;
    CHECK_FALSE(EmbeddingClient::parseEmbeddings(QByteArray("not json"), &out, &err));
    CHECK_FALSE(EmbeddingClient::parseEmbeddings(
        R"({"data":[{"index":0,"embedding":[1,2]},{"index":1,"embedding":[1,2,3]}]})", &out, &err));
    CHECK_FALSE(EmbeddingClient::parseEmbeddings(
        R"({"data":[{"index":0,"embedding":[1,2]},{"index":1}]})", &out, &err)); // index=1 缺 embedding
    CHECK_FALSE(EmbeddingClient::parseEmbeddings(
        R"({"data":[{"index":5,"embedding":[1,2]}]})", &out, &err));
    CHECK(out.isEmpty());
    // 空数据数组视为合法空结果（条数对账由调用方负责）
    CHECK(EmbeddingClient::parseEmbeddings(R"({"data":[]})", &out, &err));
    CHECK(out.isEmpty());
}

TEST_CASE("checkEndpoint：仅 https 公网；拒绝 http、userinfo、本机/内网/保留段") {
    QString err;
    CHECK_FALSE(EmbeddingClient::checkEndpoint(QStringLiteral("http://open.bigmodel.cn/api/v4"), &err));
    CHECK(EmbeddingClient::checkEndpoint(QStringLiteral("https://user:pass@open.bigmodel.cn/v4"), &err)
          == false); // userinfo
    CHECK_FALSE(EmbeddingClient::checkEndpoint(QStringLiteral("https://127.0.0.1/v4"), &err));
    CHECK_FALSE(EmbeddingClient::checkEndpoint(QStringLiteral("https://[::1]/v4"), &err));
    CHECK_FALSE(EmbeddingClient::checkEndpoint(QStringLiteral("https://192.168.1.5/v4"), &err));
    CHECK_FALSE(EmbeddingClient::checkEndpoint(QStringLiteral("https://10.0.0.3/v4"), &err));
    CHECK_FALSE(EmbeddingClient::checkEndpoint(QStringLiteral("https://169.254.1.2/v4"), &err)); // 链路本地
    CHECK(EmbeddingClient::checkEndpoint(QStringLiteral("https://8.8.8.8/v4"), &err)); // 字面公网 IP，无 DNS
}

TEST_CASE("EmbeddingClient：缺配置视为 invalid，embed 快速失败不发起网络") {
    EmbeddingClient missing;
    CHECK_FALSE(missing.valid());
    QVector<QVector<float>> out;
    QString err;
    CHECK_FALSE(missing.embed({QStringLiteral("你好")}, &out, &err));
    CHECK(out.isEmpty());
    CHECK_FALSE(err.isEmpty());

    EmbeddingClient configured{EmbeddingClient::Config{QStringLiteral("https://example.com/"), {},
                                                       QStringLiteral("embedding-3")}};
    CHECK(configured.valid()); // 尾斜杠被归一
}
