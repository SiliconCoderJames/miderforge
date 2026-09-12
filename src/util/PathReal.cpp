#include "util/PathReal.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace miderforge::pathreal {

namespace {

// 归一化 GetFinalPathNameByHandleW 的输出：'/' 分隔、去 \\?\ 前缀（UNC → //host/share）、去尾斜杠
QString normalizeFinalPath(QString p) {
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (p.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive))
        p = QStringLiteral("//") + p.mid(8);
    else if (p.startsWith(QStringLiteral("//?/"), Qt::CaseInsensitive))
        p = p.mid(4);
    while (p.size() > 2 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

#ifdef Q_OS_WIN
enum class Probe { Ok, NotFound, Error };

// 打开 path（目录或文件，跟随 reparse point）并取最终路径
Probe queryFinalPath(const QString& path, QString* finalPath) {
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    HANDLE h = ::CreateFileW(w.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            return Probe::Error; // 拒绝访问/网络不可达等：一律 fail-closed
        // 目标缺失：区分"真不存在"与"悬空 reparse point"（组件本身存在但链接目标不可达）。
        // 悬空必须 fail-closed——否则经由悬空符号链接的写入会在链接目标处凭空创建文件
        HANDLE h2 = ::CreateFileW(w.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h2 != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h2);
            return Probe::Error;
        }
        return Probe::NotFound;
    }
    wchar_t stackBuf[1024];
    const DWORD n = ::GetFinalPathNameByHandleW(h, stackBuf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) {
        ::CloseHandle(h);
        return Probe::Error;
    }
    if (n < 1024) {
        *finalPath = normalizeFinalPath(QString::fromWCharArray(stackBuf, int(n)));
    } else {
        std::wstring big(size_t(n), L'\0');
        if (::GetFinalPathNameByHandleW(h, big.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) != n) {
            ::CloseHandle(h);
            return Probe::Error;
        }
        *finalPath = normalizeFinalPath(QString::fromWCharArray(big.data(), int(n)));
    }
    ::CloseHandle(h);
    return Probe::Ok;
}
#endif // Q_OS_WIN

// 真实路径的父目录；已在根（"C:" / "//host/share"）返回空（父回退到此钳制）
QString parentOf(const QString& realPath) {
    if (realPath.startsWith(QLatin1String("//"))) {
        const int second = realPath.indexOf(QLatin1Char('/'), 2);
        if (second < 0)
            return {};
        const int third = realPath.indexOf(QLatin1Char('/'), second + 1);
        if (third < 0)
            return {};
        return realPath.left(third);
    }
    const int cut = realPath.lastIndexOf(QLatin1Char('/'));
    if (cut <= 0)
        return {};
    return realPath.left(cut);
}

} // namespace

QString resolveReal(const QString& absPath) {
    if (absPath.trimmed().isEmpty())
        return {};
#ifdef Q_OS_WIN
    QString p = QDir::fromNativeSeparators(absPath);
    // 显式设备命名空间：剥壳后按常规处理（\\?\C:\... 与 C:\... 同卷；\\?\UNC\... → //host/share/...）
    if (p.startsWith(QStringLiteral("//?/"), Qt::CaseInsensitive)) {
        p = p.mid(4);
        if (p.startsWith(QStringLiteral("UNC/"), Qt::CaseInsensitive))
            p = QStringLiteral("//") + p.mid(4);
    }
    const bool unc = p.startsWith(QLatin1String("//"));
    const QStringList comps = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (comps.isEmpty())
        return {};
    QString current;
    if (unc) {
        if (comps.size() < 2)
            return {};
        if (queryFinalPath(QStringLiteral("//") + comps[0] + QLatin1Char('/') + comps[1], &current)
            != Probe::Ok)
            return {};
    } else {
        if (comps[0].size() != 2 || comps[0][1] != QLatin1Char(':') || !comps[0][0].isLetter())
            return {}; // 非绝对盘符路径不在此判定（调用方必须先解析为绝对路径）→ fail-closed
        if (queryFinalPath(comps[0] + QLatin1Char('/'), &current) != Probe::Ok)
            return {};
    }
    for (int i = unc ? 2 : 1; i < comps.size(); ++i) {
        const QString& c = comps[i];
        if (c == QLatin1String(".")) {
            continue;
        }
        if (c == QLatin1String("..")) {
            const QString up = parentOf(current); // 已是真实路径：词法父=真实父
            if (!up.isEmpty())
                current = up;
            continue;
        }
        QString probed;
        const Probe pr = queryFinalPath(current + QLatin1Char('/') + c, &probed);
        if (pr == Probe::Ok) {
            current = probed; // 已存在组件：展开 reparse point 后继续
            continue;
        }
        if (pr == Probe::Error)
            return {}; // 解析不了：按越界处理
        // 首个不存在的组件：其余只做词法拼接（不可能再藏 reparse point）
        for (int j = i; j < comps.size(); ++j) {
            const QString& t = comps[j];
            if (t == QLatin1String("."))
                continue;
            if (t == QLatin1String("..")) {
                const QString up = parentOf(current);
                if (!up.isEmpty())
                    current = up;
                continue;
            }
            current += QLatin1Char('/') + t;
        }
        return current;
    }
    return current;
#else
    // 非 Windows 平台仅保证可编译（本仓库面向 Windows 发布）：canonicalFilePath 同语义近似
    const QString unified = QDir::fromNativeSeparators(absPath);
    if (!unified.startsWith(QLatin1Char('/')))
        return {};
    const QStringList comps = unified.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int take = comps.size(); take >= 1; --take) {
        const QString candidate = QLatin1Char('/') + comps.mid(0, take).join(QLatin1Char('/'));
        const QString canon = QFileInfo(candidate).canonicalFilePath();
        if (!canon.isEmpty()) {
            QString full = canon;
            for (int j = take; j < comps.size(); ++j) {
                const QString& t = comps[j];
                if (t == QLatin1String("..")) {
                    const int cut = full.lastIndexOf(QLatin1Char('/'));
                    if (cut > 0)
                        full = full.left(cut);
                    continue;
                }
                if (t == QLatin1String("."))
                    continue;
                full += QLatin1Char('/') + t;
            }
            return full;
        }
    }
    return {};
#endif
}

bool isInsideOrEqual(const QString& realPath, const QString& realRoot) {
    if (realPath.isEmpty() || realRoot.isEmpty())
        return false; // 任一侧解析失败：按越界处理（fail-closed）
    QString root = realRoot;
    while (root.size() > 2 && root.endsWith(QLatin1Char('/')))
        root.chop(1);
    if (root.isEmpty())
        return false;
    if (realPath.compare(root, Qt::CaseInsensitive) == 0)
        return true;
    return realPath.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

} // namespace miderforge::pathreal
