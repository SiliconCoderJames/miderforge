// 网络与技能工具：http_fetch（GET，TLS 强制校验）与 read_skill（加载 SKILL.md 全文）
#pragma once
namespace miderforge {

class ToolRegistry;

namespace ExtraTools {

void registerAll(ToolRegistry& reg);

} // namespace ExtraTools
} // namespace miderforge
