// 工具调用参数累积器实现
#include "llm/ToolCallAccumulator.h"
#include <QJsonDocument>
#include <algorithm>

namespace miderforge {

void ToolCallAccumulator::feedDelta(int index, const QString& id, const QString& nameDelta, const QString& argsDelta) {
    ToolCallParts* call = nullptr;
    for (auto& c : m_calls) {
        if (c.index == index) {
            call = &c;
            break;
        }
    }
    if (!call) {
        ToolCallParts fresh;
        fresh.index = index;
        m_calls.push_back(fresh);
        call = &m_calls.last();
    }
    if (!id.isEmpty())
        call->id = id;
    call->name += nameDelta; // 个别厂商 name 也分片
    call->arguments += argsDelta;
}

QVector<ToolCallParts> ToolCallAccumulator::finalizeAll(bool* okAll) const {
    bool allOk = true;
    QVector<ToolCallParts> out;
    // 按 index 升序输出，保证多工具调用顺序稳定
    QVector<ToolCallParts> sorted = m_calls;
    std::sort(sorted.begin(), sorted.end(), [](const ToolCallParts& a, const ToolCallParts& b) { return a.index < b.index; });
    for (const auto& c : sorted) {
        const QString trimmed = c.arguments.trimmed();
        if (trimmed.isEmpty()) {
            out.push_back(c); // 空参数 = 无参调用，合法
            continue;
        }
        QJsonParseError err{};
        QJsonDocument::fromJson(trimmed.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            allOk = false; // 参数碎片拼装结果不合法：容错不崩溃，由调用方记 events(error)
            continue;
        }
        out.push_back(c);
    }
    if (okAll)
        *okAll = allOk;
    return out;
}

void ToolCallAccumulator::reset() { m_calls.clear(); }

const ToolCallParts* ToolCallAccumulator::findByIndex(int index) const {
    for (const auto& c : m_calls)
        if (c.index == index)
            return &c;
    return nullptr;
}

} // namespace miderforge
