// 对抗测试用 mock SSE 服务器（按连接序号回放脚本）。tests/adversarial 自含版本：
// 与 tests/test_mockstream.cpp 的设施同构，但去 Q_OBJECT（lambda 连接，免 moc）。
// 已回应集合在 socket 销毁时即摘除：QTcpSocket 被 deleteLater 释放后新连接极可能
// 复用同一地址，陈旧指针会让 contains() 误判"已回应"→ 永不回响应 → 客户端挂死
#pragma once
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

struct MockScenario {
    QByteArray headers = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Connection: close\r\n";
    QVector<QByteArray> chunks;
};

class MockSseServer : public QObject {
public:
    bool start() {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket* sock = m_server.nextPendingConnection();
            if (!sock)
                return;
            const int idx = m_connCount++;
            const MockScenario sc = m_scenarios[qMin(idx, m_scenarios.size() - 1)];
            connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            // socket 销毁时同步摘除已回应标记（指针地址可能被后续连接复用）
            connect(sock, &QTcpSocket::destroyed, this, [this, sock] { m_replied.remove(sock); });
            connect(sock, &QTcpSocket::readyRead, this, [this, sock, sc] {
                if (m_replied.contains(sock))
                    return;
                m_replied.insert(sock);
                QByteArray body;
                for (const QByteArray& c : sc.chunks)
                    body += c;
                const QByteArray head = sc.headers + "Content-Length: "
                                        + QByteArray::number(body.size()) + "\r\n\r\n";
                sock->write(head);
                qint64 delay = 0;
                for (int i = 0; i < sc.chunks.size(); ++i) {
                    delay += 30; // 分块定时写出，制造真实网络分帧
                    const QByteArray chunk = sc.chunks[i];
                    const bool last = (i == sc.chunks.size() - 1);
                    QTimer::singleShot(int(delay), sock, [sock, chunk, last] {
                        sock->write(chunk);
                        if (last)
                            sock->flush();
                    });
                }
            });
        });
        return true;
    }
    quint16 port() const { return m_server.serverPort(); }
    void setScenarios(QVector<MockScenario> scs) { m_scenarios = std::move(scs); }
    int connectionCount() const { return m_connCount; }

private:
    QTcpServer m_server;
    QVector<MockScenario> m_scenarios;
    int m_connCount = 0;
    QSet<QTcpSocket*> m_replied;
};
