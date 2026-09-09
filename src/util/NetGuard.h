// 公网性判定（SSRF 红线的共享底座）：http_fetch 与嵌入端点共用。
// 实测 Qt 6.8 的 QHostAddress::isGlobal() 把 RFC1918 私有段（10/8、172.16/12、192.168/16）
// 判为"全局"，单靠它挡不住内网——这里在 isGlobal 之上显式排除全部非公网段。
#pragma once
#include <QHostAddress>

namespace miderforge {
namespace netguard {

// 严格公网判定。除 Qt isGlobal() 的基础排除（回环/链路本地/组播/广播/未指定）外，显式排除：
// IPv4: 0.0.0.0/8、10/8、100.64/10(CGNAT)、169.254/16、172.16/12、192.0.0/24、192.0.2/24、
//       192.168/16、198.18/15、198.51.100/24、203.0.113/24、224/4、240/4
// IPv6: fc00::/7(ULA)、2001:db8::/32（文档段）
// 字面 IP 直接判定；域名请先自行 QHostInfo 解析，对每个解析结果调用本函数。
bool isPublicIp(const QHostAddress& addr);

} // namespace netguard
} // namespace miderforge
