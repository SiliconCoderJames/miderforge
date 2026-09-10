// 网络与技能工具：http_fetch（GET，TLS 强制校验）与 read_skill（加载 SKILL.md 全文）；
// memory_write（Hermes 式 add/replace/remove 三动作 + 查重 + 审批门 + 安全扫描）
// 与 session_search（messages 表 FTS5 全文检索）
#pragma once
#include <QString>
namespace miderforge {

class ToolRegistry;
class Database;
class MemoryManager;

namespace ExtraTools {

void registerAll(ToolRegistry& reg);
// Hermes 记忆三动作与会话检索：db/mem 注入后追加注册 memory_write / session_search（可空 = 跳过）
void registerAll(ToolRegistry& reg, Database* db, MemoryManager* mem);

// SSRF 防护判定（纯逻辑、可单测）：
// 1) 仅 http/https；2) 拒绝携带用户信息；3) localhost 与内网保留后缀字面量拦截；
// 4) DNS 解析后对【所有】结果地址做公网性判定——进制花样（2130706433/0177.0.0.1）、
//    IPv6 嵌套（::ffff:127.0.0.1）、DNS rebinding 的字面量形态全部归一为 IP 判定。
// 通过时 pinnedIp 回传首个公网地址（调用方用 CURLOPT_RESOLVE 钉住，防解析 TOCTOU）。
bool checkFetchUrl(const QString& url, QString* pinnedIp, QString* err);

// 记忆内容安全扫描（纯逻辑、可单测，Hermes 同款思路）：
// 拦截不可见 Unicode（零宽字符 U+200B-U+200F / U+2060-U+2064 / U+FEFF / 软连字符 U+00AD）
// 与典型提示词注入/外传指令串；通过返回 true，reason 给出拦截原因
bool isSafeMemoryContent(const QString& content, QString* reason);

} // namespace ExtraTools
} // namespace miderforge
