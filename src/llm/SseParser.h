// SSE（Server-Sent Events）流分帧解析器：curl 回调字节块不保证对齐事件边界，
// 必须累积缓冲、按空行（\n\n）切出完整帧后再交给上层 json::parse（踩坑清单 #1）
#pragma once
#include <QByteArray>
#include <QString>
#include <vector>

namespace miderforge {

class SseParser {
public:
    // 喂入任意长度字节块（不保证对齐事件边界）
    void feed(const char* data, size_t len);
    void feed(const QByteArray& bytes) { feed(bytes.constData(), size_t(bytes.size())); }

    // 取出所有已完整的 SSE 事件载荷：仅取 "data:" 行（多行 data 以 \n 合并），
    // event:/id:/retry:/注释行一律忽略。调用后内部缓冲清除已消费部分。
    std::vector<QString> takeCompleteEvents();

    // 流正常结束后调用：把缓冲中残留的、漏发结尾空行的最后一帧交出（容错非规范服务器）
    std::vector<QString> flushRemainder();

    void reset() { m_buffer.clear(); }
    bool hasPending() const { return !m_buffer.isEmpty(); }

private:
    QByteArray m_buffer;
};

} // namespace miderforge
