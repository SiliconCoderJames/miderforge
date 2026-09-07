// 工具注册表：JSON Schema 声明 + 统一执行入口；全部工具调用经此注册（v1 不用 MCP，自建注册表）
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <vector>

namespace miderforge {

struct ToolDef {
    QString name;
    QString description;
    QJsonObject parameters; // JSON Schema（type/properties/required）
    // 处理函数：入参为解析后的参数对象；返回给 LLM 的结果文本；*err 非空表示执行失败
    std::function<QString(const QJsonObject& args, QString* err)> handler;
};

class ToolRegistry {
public:
    void add(ToolDef def);
    const ToolDef* find(const QString& name) const;

    struct ExecResult {
        bool ok = false;
        QString text;   // 给 LLM 的回填文本（成功为结果 envelope，失败为错误说明）
        qint64 ms = 0;  // 执行耗时
    };
    // 统一执行：查找→解析参数→计时调用 handler；未知工具/参数不合法均返回失败
    ExecResult execute(const QString& name, const QString& argumentsJson) const;

    // 转为 OpenAI tools 数组（type=function）
    QJsonArray openAiSchemas() const;
    QStringList names() const;
    int count() const { return int(m_tools.size()); }

private:
    std::vector<ToolDef> m_tools;
};

} // namespace miderforge
