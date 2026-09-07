// 权限门实现
#include "tools/PermissionGate.h"
#include <QDir>
#include <QRegularExpression>

namespace miderforge {

QStringList PermissionGate::commandWhitelist() {
    // 决策: v1 内置编译工具链白名单；后续由设置页注入用户自定义
    return {QStringLiteral("cmake"), QStringLiteral("ninja"), QStringLiteral("msbuild"),
            QStringLiteral("git"),   QStringLiteral("cl"),    QStringLiteral("clang-format"),
            QStringLiteral("where"), QStringLiteral("echo")};
}

QStringList PermissionGate::networkWhitelist() {
    // 决策: v1 默认空 = Full Access 下 http_fetch 全部走确认；后续由设置页配置放行域
    return {};
}

void PermissionGate::resetSessionGrants() {
    for (bool& b : m_alwaysAllowed)
        b = false;
}

bool PermissionGate::pathInWorkspace(const QString& absPath, const QString& workspaceRoot) {
    if (workspaceRoot.isEmpty())
        return false;
    const QString root = QDir::cleanPath(workspaceRoot) + QLatin1Char('/');
    const QString p = QDir::cleanPath(absPath);
    return p.startsWith(root, Qt::CaseInsensitive)
           || p.compare(QDir::cleanPath(workspaceRoot), Qt::CaseInsensitive) == 0;
}

bool PermissionGate::matchesForbiddenPath(const QString& path) const {
    // 永不解禁：路径含敏感名（踩坑防御：大小写不敏感 + 路径匹配）
    static const QRegularExpression sensitive(
        QStringLiteral("(credential|secret|token|id_rsa)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString p = QDir::fromNativeSeparators(path);
    const QString name = p.mid(p.lastIndexOf(QLatin1Char('/')) + 1);
    // .env 家族（.env / .env.local / prod.env 等）
    if (name.contains(QStringLiteral(".env"), Qt::CaseInsensitive))
        return true;
    return sensitive.match(p).hasMatch();
}

QString PermissionGate::hardDenyReason(PermissionGate::Kind kind, const QString& target,
                                       const QString& workspaceRoot) const {
    if (kind == Kind::ReadFile || kind == Kind::WriteFile) {
        if (matchesForbiddenPath(target))
            return QStringLiteral("路径命中永不解禁清单（credential/secret/.env/token/id_rsa）");
    }
    if (kind == Kind::RunCommand) {
        const QString cmd = target.trimmed();
        // git push 到保护分支
        static const QRegularExpression pushProtected(
            QStringLiteral("^git\\s+push\\s+\\S+\\s+(main|master|protected)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (pushProtected.match(cmd).hasMatch())
            return QStringLiteral("禁止 git push 到 main/master/protected 分支");
        // 磁盘级操作
        static const QRegularExpression diskLevel(
            QStringLiteral("\\b(format|diskpart|cipher\\s+/w)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (diskLevel.match(cmd).hasMatch())
            return QStringLiteral("禁止磁盘级操作");
        // 递归删除：rd /s、del /s、rmdir /s、rm -r —— 工作区外一律禁止
        static const QRegularExpression recursiveDel(
            QStringLiteral("\\b(rd|rmdir|del|rm|remove-item)\\b[^|;&]*((/s|-r\\b|--recursive\\b|/s\\b))"),
            QRegularExpression::CaseInsensitiveOption);
        if (recursiveDel.match(cmd).hasMatch()) {
            // 粗策略：命令中任何绝对路径不在工作区内即拒绝；无绝对路径（相对路径钉死工作目录）则放行
            static const QRegularExpression absPath(QStringLiteral("[A-Za-z]:\\\\[^\\s\"&|;]+"));
            auto it = absPath.globalMatch(cmd);
            bool sawOutside = false;
            while (it.hasNext()) {
                const QString p = it.next().captured(0);
                if (!pathInWorkspace(p, workspaceRoot)) {
                    sawOutside = true;
                    return QStringLiteral("工作区外递归删除被永久禁止：%1").arg(p);
                }
            }
            (void)sawOutside;
        }
    }
    // “以用户身份发送消息”不适用工具通道：邮件仅限任务通知（规格 10）
    return {};
}

PermissionGate::Decision PermissionGate::evaluate(PermissionMode mode, PermissionGate::Kind kind,
                                                  const QString& target,
                                                  const QString& workspaceRoot) const {
    // 第一道：永不解禁清单
    const QString hard = hardDenyReason(kind, target, workspaceRoot);
    if (!hard.isEmpty())
        return Decision::Denied;

    if (kind == Kind::ReadFile)
        return Decision::Allowed; // 三档均自动

    if (kind == Kind::WriteFile) {
        if (mode == PermissionMode::Suggest)
            return Decision::NeedsConfirm; // 仅生成提案需确认
        if (mode == PermissionMode::AutoEdit)
            return pathInWorkspace(target, workspaceRoot) ? Decision::Allowed : Decision::Denied;
        return Decision::Allowed; // Full Access（沙箱内）
    }

    if (kind == Kind::RunCommand) {
        // 命令白名单：首词必须在名单内
        const QString first = target.trimmed().split(QLatin1Char(' ')).value(0).toLower();
        if (!commandWhitelist().contains(first))
            return Decision::Denied;
        if (mode == PermissionMode::FullAccess)
            return Decision::Allowed; // 沙箱内自动
        if (alwaysAllowedForSession(Kind::RunCommand))
            return Decision::Allowed;
        return Decision::NeedsConfirm; // Suggest/Auto Edit 逐条确认
    }

    if (kind == Kind::Network) {
        if (mode == PermissionMode::FullAccess) {
            // 白名单域命中才全自动，否则确认
            static const QRegularExpression domainExtract(QStringLiteral("https?://([^/\\s]+)"));
            const auto m = domainExtract.match(target);
            if (m.hasMatch() && networkWhitelist().contains(m.captured(1)))
                return Decision::Allowed;
            if (alwaysAllowedForSession(Kind::Network))
                return Decision::Allowed;
            return Decision::NeedsConfirm;
        }
        if (alwaysAllowedForSession(Kind::Network))
            return Decision::Allowed;
        // 决策: 规格 9 表默认“需确认”与规格 10 矩阵“❌”取折中：Suggest/Auto Edit 走确认卡片
        return Decision::NeedsConfirm;
    }

    return Decision::Denied;
}

} // namespace miderforge
