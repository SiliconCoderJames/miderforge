// 公网性判定实现
#include "util/NetGuard.h"
#include <QAbstractSocket>

namespace miderforge {
namespace netguard {

namespace {
// CIDR 前缀包含：ip/net 按 bits 位掩码比较
bool inRange(quint32 ip, quint32 net, int bits) {
    const quint32 mask = bits <= 0 ? 0u : quint32(0xFFFFFFFFu) << (32 - bits);
    return (ip & mask) == (net & mask);
}
} // namespace

bool isPublicIp(const QHostAddress& addr) {
    if (!addr.isGlobal())
        return false; // Qt 基础判定：回环/链路本地/组播/广播/未指定（IPv4-mapped 归一后同样命中回环等）

    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = addr.toIPv4Address();
        bool blocked = inRange(ip, 0x00000000u, 8)     // 0.0.0.0/8 本网络
                       || inRange(ip, 0x0A000000u, 8)  // 10/8 RFC1918
                       || inRange(ip, 0x64400000u, 10) // 100.64/10 运营商级 NAT
                       || inRange(ip, 0xA9FE0000u, 16) // 169.254/16 链路本地
                       || inRange(ip, 0xAC100000u, 12) // 172.16/12 RFC1918
                       || inRange(ip, 0xC0000000u, 24) // 192.0.0/24 IETF 协议保留
                       || inRange(ip, 0xC0000200u, 24) // 192.0.2/24 TEST-NET-1
                       || inRange(ip, 0xC0A80000u, 16) // 192.168/16 RFC1918
                       || inRange(ip, 0xC6120000u, 15) // 198.18/15 基准测试
                       || inRange(ip, 0xC6336400u, 24) // 198.51.100/24 TEST-NET-2
                       || inRange(ip, 0xCB007100u, 24) // 203.0.113/24 TEST-NET-3
                       || inRange(ip, 0xE0000000u, 4)  // 224/4 组播
                       || inRange(ip, 0xF0000000u, 4); // 240/4 保留
        return !blocked;
    }
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = addr.toIPv6Address();
        if (v6[0] == 0xfc || v6[0] == 0xfd)
            return false; // fc00::/7 唯一本地地址（ULA）
        if (v6[0] == 0x20 && v6[1] == 0x01 && v6[2] == 0x0d && v6[3] == 0xb8)
            return false; // 2001:db8::/32 文档专用
        // fe80::/10、ff00::/8、::、::1 已由 isGlobal 排除
        return true;
    }
    return false; // 未知协议保守拒绝
}

} // namespace netguard
} // namespace miderforge
