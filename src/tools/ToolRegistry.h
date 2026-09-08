// 工具注册表：JSON Schema 声明 + 统一执行入口；全部工具调用经此注册（v1 不用 MCP，自建注册表）
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
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

    // ---- M5 P3 取消令牌：用户取消传导至执行中的工具（原先到不了工具层）----
    // AgentLoop::cancel() 置位 → 在跑工具（命令轮询/网络进度回调）读到即中止 →
    // AgentLoop 在工具批收尾时消费标志并整体判停。工具只读，消费由 AgentLoop 负责
    void requestToolCancel() { m_toolCancel.store(true); }
    bool toolCancelRequested() const { return m_toolCancel.load(); }
    bool consumeToolCancel() { return m_toolCancel.exchange(false); }

private:
    std::vector<ToolDef> m_tools;
    std::atomic<bool> m_toolCancel{false};
};

} // namespace miderforge
