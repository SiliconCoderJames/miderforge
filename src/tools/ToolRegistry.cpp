// 工具注册表实现
#include "tools/ToolRegistry.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <algorithm>

namespace miderforge {

void ToolRegistry::add(ToolDef def) {
    // 同名注册视为编程错误：忽略后到者（最简可行防御）
    if (find(def.name))
        return;
    m_tools.push_back(std::move(def));
}

const ToolDef* ToolRegistry::find(const QString& name) const {
    for (const auto& t : m_tools)
        if (t.name == name)
            return &t;
    return nullptr;
}

ToolRegistry::ExecResult ToolRegistry::execute(const QString& name, const QString& argumentsJson) const {
    ExecResult result;
    QElapsedTimer timer;
    timer.start();

    const ToolDef* def = find(name);
    if (!def) {
        result.text = QStringLiteral("错误：未知工具 %1").arg(name);
        result.ms = timer.elapsed();
        return result;
    }

    QJsonObject args;
    const QString trimmed = argumentsJson.trimmed();
    if (!trimmed.isEmpty()) {
        QJsonParseError parseErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            result.text = QStringLiteral("错误：工具 %1 的参数不是合法 JSON 对象：").arg(name) + parseErr.errorString();
            result.ms = timer.elapsed();
            return result;
        }
        args = doc.object();
    }

    QString err;
    result.text = def->handler(args, &err);
    result.ok = err.isEmpty();
    if (!result.ok)
        result.text = QStringLiteral("错误：") + err;
    result.ms = timer.elapsed();
    return result;
}

QJsonArray ToolRegistry::openAiSchemas() const {
    QJsonArray arr;
    for (const auto& t : m_tools) {
        arr.append(QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{
                             {"name", t.name},
                             {"description", t.description},
                             {"parameters", t.parameters},
                         }},
        });
    }
    return arr;
}

QStringList ToolRegistry::names() const {
    QStringList out;
    for (const auto& t : m_tools)
        out << t.name;
    return out;
}

} // namespace miderforge
