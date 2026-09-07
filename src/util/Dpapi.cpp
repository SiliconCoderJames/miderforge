// DPAPI 实现文件
#include "util/Dpapi.h"
#include <QByteArray>
#include <windows.h>
#include <wincrypt.h>
#include <vector>

namespace miderforge::dpapi {

std::optional<QString> encryptToBase64(const QString& plain) {
    const QByteArray utf8 = plain.toUtf8();
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.constData()));
    in.cbData = DWORD(utf8.size());
    DATA_BLOB out{};
    // CRYPTPROTECT_UI_FORBIDDEN：服务/无界面环境也可调用；默认 per-user 作用域
    if (!CryptProtectData(&in, L"Miderforge.apikey", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    const QByteArray raw(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(raw.toBase64());
}

std::optional<QString> decryptFromBase64(const QString& b64) {
    const QByteArray raw = QByteArray::fromBase64(b64.toLatin1());
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(raw.constData()));
    in.cbData = DWORD(raw.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    const QByteArray plain(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return QString::fromUtf8(plain);
}

} // namespace miderforge::dpapi
