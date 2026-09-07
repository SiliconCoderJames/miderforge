// 应用数据目录实现
#include "util/AppDirs.h"
#include <QDir>
#include <QStandardPaths>

namespace miderforge::appdirs {

QString root() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1String("/Miderforge");
}

QString file(const QString& rel) {
    return root() + QLatin1Char('/') + rel;
}

bool ensureLayout() {
    const QStringList dirs = {
        root(),
        file(QStringLiteral("memory")),
        file(QStringLiteral("skills")),
        file(QStringLiteral("config")),
        file(QStringLiteral("logs")),
        file(QStringLiteral("workspace")),
    };
    for (const QString& d : dirs) {
        if (!QDir().mkpath(d))
            return false;
    }
    return true;
}

QString workspaceRoot() {
    return file(QStringLiteral("workspace"));
}

} // namespace miderforge::appdirs
