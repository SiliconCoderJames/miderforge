// 自由文本中的 JSON 对象提取（收尾提炼链路的容错解析）
#pragma once
#include <QJsonDocument>
#include <QString>

namespace miderforge::jsonextract {

// 从模型自由输出中提取第一个可解析的 JSON 对象：
// - 括号深度扫描 + 字符串/转义感知（首个 '{' 到末个 '}' 的切片会被中间示例对象破坏）
// - 存在多个候选对象时从最后一个向前尝试（模型的"正式回答"通常在末尾）
// 找不到任何可解析对象时返回 null 文档
QJsonDocument extractObject(const QString& text);

} // namespace miderforge::jsonextract
