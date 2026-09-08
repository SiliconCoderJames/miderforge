// 端到端流式管线测试：本地 mock SSE 服务器 → HttpClient(curl 工作线程) → SseParser → ChatClient。
// 覆盖：正常双流（思考+正文）、工具调用碎片累积、流中途断线自动重试、401 错误透出
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <doctest/doctest.h>
#include <memory>
#include <optional>

using miderforge::ChatClient;
using miderforge::HttpClient;
using miderforge::ProviderConfig;
using miderforge::StreamResult;

namespace {

// 自旋等待谓词成立（驱动事件循环，不依赖 QtTest）
template <typename Pred>
bool waitFor(Pred&& pred, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return pred();
}

// 回放脚本：按连接序号重放
struct MockScenario {
    QByteArray headers = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Connection: close\r\n";
    QVector<QByteArray> chunks;
    int abortAfterChunk = -1; // >=0：发出第 N 块后强制断开（模拟断网）
};

} // namespace

// moc 需要可命名的类：mock SSE 服务器（按连接回放脚本）
class MockSseServer : public QObject {
    Q_OBJECT
public:
    bool start() {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, &MockSseServer::onNewConnection);
        return true;
    }
    quint16 port() const { return m_server.serverPort(); }
    void setScenarios(QVector<MockScenario> scs) { m_scenarios = std::move(scs); }
    int connectionCount() const { return m_connCount; }

private slots:
    void onNewConnection() {
        QTcpSocket* sock = m_server.nextPendingConnection();
        if (!sock)
            return;
        const int idx = m_connCount++;
        const MockScenario sc = m_scenarios[qMin(idx, m_scenarios.size() - 1)];
        connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock, sc] {
            if (m_replied.contains(sock))
                return;
            m_replied.insert(sock);
            QByteArray body;
            for (const QByteArray& c : sc.chunks)
                body += c;
            const QByteArray head = sc.headers + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
            if (sc.abortAfterChunk >= 0) {
                // 断线场景：只发前 N 块，随后硬断（curl 收到“传输提前终止”）
                qint64 sent = 0;
                for (int i = 0; i <= sc.abortAfterChunk && i < sc.chunks.size(); ++i)
                    sent += sc.chunks[i].size();
                sock->write(head + body.left(int(sent)));
                sock->flush(); // 先把部分数据推到内核，避免 abort 丢弃写缓冲（慢机器上的竞态）
                QTimer::singleShot(150, sock, [sock] { sock->abort(); });
                return;
            }
            sock->write(head);
            qint64 delay = 0;
            for (int i = 0; i < sc.chunks.size(); ++i) {
                delay += 30; // 分块定时写出，制造真实的网络分帧
                const QByteArray chunk = sc.chunks[i];
                const bool last = (i == sc.chunks.size() - 1);
                QTimer::singleShot(int(delay), sock, [sock, chunk, last] {
                    sock->write(chunk);
                    if (last)
                        sock->flush();
                });
            }
        });
    }

private:
    QTcpServer m_server;
    QVector<MockScenario> m_scenarios;
    int m_connCount = 0;
    QSet<QTcpSocket*> m_replied;
};

namespace {

ProviderConfig makeProvider(const QUrl& baseUrl) {
    ProviderConfig p;
    p.name = QStringLiteral("mock");
    p.baseUrl = baseUrl.toString();
    p.key = QStringLiteral("sk-test");
    p.configured = true;
    return p;
}

} // namespace

#include "test_mockstream.moc"

TEST_CASE("ChatClient 正常流式：思考/正文双流 + finish_reason=stop") {
    MockSseServer server;
    REQUIRE(server.start());
    const QByteArray f1 = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"思考A\"}}]}\n\n";
    const QByteArray f2 = "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n";
    const QByteArray f3 = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"思考B\"}}]}\n\n";
    const QByteArray f4 = "data: {\"choices\":[{\"delta\":{\"content\":\"，世界\"}}]}\n\n";
    const QByteArray f5 = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}\n\n";
    const QByteArray done = "data: [DONE]\n\n";
    // 故意把一帧切成两半，验证跨块边界（踩坑清单 #1）
    MockScenario sc;
    sc.chunks = {(f1 + f2.left(20)), (f2.mid(20) + f3), (f4 + f5 + done)};
    server.setScenarios({sc});

    ChatClient client;
    ProviderConfig provider = makeProvider(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port()));

    QString thinking, content;
    std::optional<StreamResult> result;
    QObject::connect(&client, &ChatClient::thinkingDelta, [&](const QString& d) { thinking += d; });
    QObject::connect(&client, &ChatClient::contentDelta, [&](const QString& d) { content += d; });
    QObject::connect(&client, &ChatClient::finished, [&](const StreamResult& r) { result = r; });

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"}, {"content", "打个招呼"}});
    client.start(provider, QStringLiteral("mock-model"), messages, QJsonArray());

    REQUIRE(waitFor([&] { return result.has_value(); }, 10000));
    CHECK_FALSE(result->aborted);
    CHECK(result->content == QString("你好，世界"));
    CHECK(result->reasoning == QString("思考A思考B"));
    CHECK(content == QString("你好，世界"));
    CHECK(result->finishReason == QString("stop"));
    CHECK(result->promptTokens == 10);
    CHECK(result->completionTokens == 5);
}

TEST_CASE("ChatClient 工具调用：参数碎片按 index 累积，finish_reason=tool_calls") {
    MockSseServer server;
    REQUIRE(server.start());
    const QByteArray t1 = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"pa\"}}]}}]}\n\n";
    const QByteArray t2 = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"th\\\": \\\"C:/a.txt\\\"}\"}}]}}]}\n\n";
    const QByteArray f3 = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n";
    const QByteArray done = "data: [DONE]\n\n";
    MockScenario sc;
    sc.chunks = {t1.left(60), t1.mid(60) + t2, f3 + done};
    server.setScenarios({sc});

    ChatClient client;
    ProviderConfig provider = makeProvider(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port()));

    std::optional<StreamResult> result;
    QObject::connect(&client, &ChatClient::finished, [&](const StreamResult& r) { result = r; });

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"}, {"content", "读文件"}});
    client.start(provider, QStringLiteral("mock-model"), messages, QJsonArray());

    REQUIRE(waitFor([&] { return result.has_value(); }, 10000));
    REQUIRE(result->toolCalls.size() == 1);
    CHECK(result->toolCalls[0].name == QString("read_file"));
    CHECK(result->toolCalls[0].id == QString("call_1"));
    const QJsonObject args = QJsonDocument::fromJson(result->toolCalls[0].arguments.toUtf8()).object();
    CHECK(args.value("path").toString() == QString("C:/a.txt"));
    CHECK(result->finishReason == QString("tool_calls"));
}

TEST_CASE("流中途断线：指数退避自动重试后成功") {
    MockSseServer server;
    REQUIRE(server.start());

    const QByteArray f1 = "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n";
    const QByteArray f2 = "data: {\"choices\":[{\"delta\":{\"content\":\"，世界\"}}]}\n\n";
    const QByteArray f3 = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    const QByteArray done = "data: [DONE]\n\n";

    MockScenario broken;
    broken.chunks = {f1, f2, f3, done};
    broken.abortAfterChunk = 0; // 发出第 0 块后硬断
    MockScenario healthy;
    healthy.chunks = {f1 + f2, f3 + done};
    server.setScenarios({broken, healthy});

    ChatClient client;
    ProviderConfig provider = makeProvider(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port()));

    std::optional<StreamResult> result;
    bool sawRetry = false;
    bool sawReset = false;
    QObject::connect(&client, &ChatClient::failed, [&](const QString&, int, bool willRetry) {
        if (willRetry)
            sawRetry = true;
    });
    QObject::connect(&client, &ChatClient::streamReset, [&] { sawReset = true; });
    QObject::connect(&client, &ChatClient::finished, [&](const StreamResult& r) { result = r; });

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"}, {"content", "断线测试"}});
    client.start(provider, QStringLiteral("mock-model"), messages, QJsonArray());

    REQUIRE(waitFor([&] { return result.has_value(); }, 45000)); // 含 1s 退避；CI 慢机器上留足余量
    CHECK(sawRetry);
    CHECK(sawReset);
    CHECK(server.connectionCount() >= 2); // 断线后确实重连
    CHECK(result->content == QString("你好，世界")); // 重试后完整内容
}

TEST_CASE("curl 基线：不可达地址应返回错误而非崩溃") {
    // 经 ChatClient（curl 在工作线程执行）对死端口发请求：应收到 failed 而非崩溃
    ChatClient client;
    ProviderConfig provider;
    provider.name = QStringLiteral("dead");
    provider.baseUrl = QStringLiteral("http://127.0.0.1:9");
    provider.key = QStringLiteral("k");
    provider.configured = true;

    std::optional<QString> err;
    QObject::connect(&client, &ChatClient::failed, [&](const QString& e, int, bool willRetry) {
        if (!willRetry)
            err = e;
    });
    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"}, {"content", "hi"}});
    client.start(provider, QStringLiteral("m"), messages, QJsonArray());
    waitFor([&] { return err.has_value(); }, 20000); // 含 2 次退避重试（1s+2s）
    REQUIRE(err.has_value());
}

TEST_CASE("HTTP 401：不重试，透出 API 错误信息") {
    MockSseServer server;
    REQUIRE(server.start());
    MockScenario sc;
    sc.headers = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: close\r\n";
    sc.chunks = {"{\"error\":{\"message\":\"无效的 API Key\"}}"};
    server.setScenarios({sc});

    ChatClient client;
    ProviderConfig provider = makeProvider(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port()));

    std::optional<QString> err;
    int errCode = 0;
    QObject::connect(&client, &ChatClient::failed, [&](const QString& e, int code, bool willRetry) {
        if (!willRetry) {
            err = e;
            errCode = code;
        }
    });

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"}, {"content", "hi"}});
    client.start(provider, QStringLiteral("mock-model"), messages, QJsonArray());

    REQUIRE(waitFor([&] { return err.has_value(); }, 10000));
    CHECK(errCode == 401);
    CHECK(err->contains(QString("无效的 API Key")));
}
