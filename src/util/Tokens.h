// 粗略 token 估算：CJK 等非 ASCII 字符按 1 字 1 token、ASCII 按 4 字符 1 token 计。
// 仅用于 UI 统计与预算参考（usage 缺失时的兜底），非精确分词。
#pragma once
#include <QString>

namespace miderforge::tokens {

long long estimate(const QString& text);

} // namespace miderforge::tokens
