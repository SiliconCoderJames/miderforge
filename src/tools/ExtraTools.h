// 网络与技能工具：http_fetch（GET，TLS 强制校验）与 read_skill（加载 SKILL.md 全文）
#pragma once
#include <QString>
namespace miderforge {

class ToolRegistry;

namespace ExtraTools {

void registerAll(ToolRegistry& reg);

// SSRF 防护判定（纯逻辑、可单测）：
// 1) 仅 http/https；2) 拒绝携带用户信息；3) localhost 与内网保留后缀字面量拦截；
// 4) DNS 解析后对【所有】结果地址做公网性判定——进制花样（2130706433/0177.0.0.1）、
//    IPv6 嵌套（::ffff:127.0.0.1）、DNS rebinding 的字面量形态全部归一为 IP 判定。
// 通过时 pinnedIp 回传首个公网地址（调用方用 CURLOPT_RESOLVE 钉住，防解析 TOCTOU）。
bool checkFetchUrl(const QString& url, QString* pinnedIp, QString* err);

} // namespace ExtraTools
} // namespace miderforge
