// 邮件通知（规格 11）：libcurl smtps 直连，零新依赖；中文标题 RFC 2047 B 编码；失败重试 1 次
#pragma once
#include <QObject>
#include <QString>

namespace miderforge {

class EventBus;

class EmailNotifier : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString smtpUrl;   // 如 smtps://smtp.qq.com:465
        QString from;      // 发件人邮箱
        QString authCode;  // 授权码（明文仅内存；落盘走 DPAPI）
        QString to;        // 收件人
        bool enabled = false;
    };

    explicit EmailNotifier(EventBus* events, QObject* parent = nullptr);

    // 从 %APPDATA%/Miderforge/config/notify.json 读取（授权码 DPAPI 密文）
    bool loadConfig();
    bool saveConfig(const Config& cfg); // 授权码 DPAPI 加密落盘
    const Config& config() const { return m_cfg; }

    // 同步发送（≤30s）；标题自动 RFC 2047；正文为 UTF-8 HTML；失败重试 1 次；写 events(email_sent)
    bool send(const QString& subject, const QString& htmlBody, QString* err = nullptr);

    // RFC 2047 B 编码（=?UTF-8?B?<base64>?=），超 60 字节自动切多段
    static QString rfc2047Encode(const QString& text);

    void setConfigForTest(const Config& cfg) { m_cfg = cfg; }

private:
    bool sendOnce(const QString& mimeMessage, QString* err);

    EventBus* m_events = nullptr;
    Config m_cfg;
};

} // namespace miderforge
