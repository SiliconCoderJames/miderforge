// SSE 分帧解析实现：行尾归一化（\r\n 与孤立 \r 均视为行分隔）+ 按空行切块
#include "llm/SseParser.h"
#include <QList>

namespace miderforge {

void SseParser::feed(const char* data, size_t len) {
    // 部分代理/网关会在响应首块前置 UTF-8 BOM：剥掉，否则首个 "data:" 行匹配失败、首帧丢失
    if (m_buffer.isEmpty() && len >= 3
        && static_cast<unsigned char>(data[0]) == 0xEF
        && static_cast<unsigned char>(data[1]) == 0xBB
        && static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;
        len -= 3;
    }
    m_buffer.append(data, int(len));
}

static QString extractDataPayload(const QByteArray& block) {
    // 一个事件块内只取 data: 行；多行 data 以 \n 合并；event:/id:/retry:/注释行忽略
    QString payload;
    const QList<QByteArray> lines = block.split('\n');
    for (const QByteArray& line : lines) {
        if (!line.startsWith("data"))
            continue;
        QByteArray value;
        if (line == "data:" || line == "data") {
            value = QByteArray(); // 空 data 行
        } else if (line.size() > 5 && line.startsWith("data: ")) {
            value = line.mid(6);
        } else if (line.size() > 5 && line.startsWith("data:")) {
            value = line.mid(5);
        } else {
            continue; // 形如 dataXYZ 的字段名，不是 SSE data 行
        }
        if (!payload.isEmpty())
            payload += QLatin1Char('\n');
        payload += QString::fromUtf8(value);
    }
    return payload;
}

std::vector<QString> SseParser::takeCompleteEvents() {
    std::vector<QString> events;

    // 第一步：行尾归一化。\r\n 与孤立 \r 统一转成 \n。
    // 若缓冲末尾是孤立的 \r，它可能是跨块 \r\n 的前半截，暂不消费、留在缓冲待下块补全，
    // 避免误生成多余空行导致事件被提前切断。
    const int n = m_buffer.size();
    const int usable = (n > 0 && m_buffer.at(n - 1) == '\r') ? n - 1 : n;
    QByteArray norm;
    norm.reserve(n);
    for (int i = 0; i < usable; ++i) {
        const char c = m_buffer.at(i);
        if (c == '\r') {
            if (i + 1 < usable && m_buffer.at(i + 1) == '\n')
                ++i; // \r\n 整体折算为一个换行
            norm.append('\n');
        } else {
            norm.append(c);
        }
    }

    // 第二步：按 "\n\n" 空行切完整事件
    int start = 0;
    while (true) {
        const int pos = int(norm.indexOf("\n\n", start));
        if (pos < 0)
            break;
        const QString payload = extractDataPayload(norm.mid(start, pos - start));
        if (!payload.isEmpty())
            events.push_back(payload);
        start = pos + 2;
    }

    // 保留未消费尾部；若原缓冲以 \r 结尾则补回悬挂 \r
    m_buffer = norm.mid(start);
    if (usable < n)
        m_buffer.append('\r');
    return events;
}

std::vector<QString> SseParser::flushRemainder() {
    std::vector<QString> events;
    // 容错：流已结束但残留内容没有空行结尾——按一个事件交出（非规范服务器兜底）
    QByteArray norm;
    norm.reserve(m_buffer.size());
    for (int i = 0; i < m_buffer.size(); ++i) {
        const char c = m_buffer.at(i);
        if (c == '\r') {
            if (i + 1 < m_buffer.size() && m_buffer.at(i + 1) == '\n')
                ++i;
            norm.append('\n');
        } else {
            norm.append(c);
        }
    }
    const QString payload = extractDataPayload(norm);
    if (!payload.isEmpty())
        events.push_back(payload);
    m_buffer.clear();
    return events;
}

} // namespace miderforge
