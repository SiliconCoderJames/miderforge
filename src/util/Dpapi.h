// Windows DPAPI 封装：CryptProtectData/CryptUnprotectData，per-user 作用域；
// providers.json 中只允许出现密文，绝不落明文（踩坑清单 #9）
#pragma once
#include <optional>
#include <QString>

namespace miderforge::dpapi {

// 明文 → Base64 密文（当前用户可解）。失败返回 nullopt
std::optional<QString> encryptToBase64(const QString& plain);
// Base64 密文 → 明文（仅本用户本机可解）。失败返回 nullopt
std::optional<QString> decryptFromBase64(const QString& b64);

} // namespace miderforge::dpapi
