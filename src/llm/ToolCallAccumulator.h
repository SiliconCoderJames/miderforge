// 工具调用参数累积器：流式 tool_calls 的 name/arguments 以碎片分片到达，
// 必须按 index 累积拼装，仅当 finish_reason=="tool_calls" 后才解析（踩坑清单 #2）
#pragma once
#include <QString>
#include <QVector>

namespace miderforge {

struct ToolCallParts {
    int index = -1;
    QString id;
    QString name;
    QString arguments; // 尚未解析的 JSON 字符串累积缓冲
};

class ToolCallAccumulator {
public:
    // 喂入一个增量分片；id/nameDelta 允许为空（多数分片只带 arguments 增量）
    void feedDelta(int index, const QString& id, const QString& nameDelta, const QString& argsDelta);

    // 终结并解析全部工具调用。每个调用的 arguments 尝试 json 解析：
    // 空串视为 {}；解析失败的调用 okAll 置 false 并跳过该条（容错：记错误，不崩溃）。
    // 返回解析后的深拷贝（arguments 字段替换为原始字符串保留，调用方自行 QJsonDocument 解析）。
    QVector<ToolCallParts> finalizeAll(bool* okAll) const;

    void reset();
    int count() const { return int(m_calls.size()); }
    const ToolCallParts* findByIndex(int index) const;

private:
    QVector<ToolCallParts> m_calls;
};

} // namespace miderforge
