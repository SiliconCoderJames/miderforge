// AppContext 实现
#include "core/AppContext.h"

namespace miderforge {

QString permissionModeName(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::Suggest: return QStringLiteral("Suggest");
    case PermissionMode::AutoEdit: return QStringLiteral("Auto Edit");
    case PermissionMode::FullAccess: return QStringLiteral("Full Access");
    }
    return QStringLiteral("Suggest");
}

AppContext& AppContext::instance() {
    static AppContext ctx;
    return ctx;
}

} // namespace miderforge
