// 权限门实现
#include "tools/PermissionGate.h"
#include <QDir>
#include <QProcess>
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
        QStringLiteral("(credential|secret|token|id_rsa|id_ed25519|id_ecdsa)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString p = QDir::fromNativeSeparators(path);
    const QString name = p.mid(p.lastIndexOf(QLatin1Char('/')) + 1);
    // .env 家族（.env / .env.local / prod.env 等）
    if (name.contains(QStringLiteral(".env"), Qt::CaseInsensitive))
        return true;
    // 证书/密钥容器家族
    static const QRegularExpression keyFile(QStringLiteral("\\.(pem|p12|pfx)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (keyFile.match(name).hasMatch())
        return true;
    // 敏感目录（路径组件级）：.ssh / .aws / .kube 下任何文件
    const QStringList comps = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& c : comps) {
        if (c.compare(QLatin1String(".ssh"), Qt::CaseInsensitive) == 0
            || c.compare(QLatin1String(".aws"), Qt::CaseInsensitive) == 0
            || c.compare(QLatin1String(".kube"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return sensitive.match(p).hasMatch();
}

namespace {

// git push 保护分支检测：token 化解析 refspec，替代单条黑名单正则
//（正则黑名单不完备（CWE-184）：--force / -u / HEAD:main / refs/heads/main / +main 均须命中）
bool gitPushHitsProtected(const QString& cmd) {
    const QStringList tokens =
        cmd.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    int pushIdx = -1;
    for (int i = 0; i < tokens.size(); ++i) {
        if (tokens[i].compare(QLatin1String("push"), Qt::CaseInsensitive) == 0) {
            pushIdx = i;
            break;
        }
    }
    if (pushIdx < 0)
        return false;
    static const QStringList kProtected = {
        QStringLiteral("main"), QStringLiteral("master"), QStringLiteral("protected")};
    for (int i = pushIdx + 1; i < tokens.size(); ++i) {
        const QString t = tokens[i];
        if (t.startsWith(QLatin1Char('-')))
            continue; // --force / -u / --force-with-lease 等开关跳过
        QString ref = t.section(QLatin1Char(':'), -1); // refspec 目标段：HEAD:main → main
        while (ref.startsWith(QLatin1Char('+')))
            ref.remove(0, 1); // +main 强推前缀
        if (ref.startsWith(QStringLiteral("refs/heads/"), Qt::CaseInsensitive))
            ref = ref.mid(11);
        for (const QString& b : kProtected)
            if (ref.compare(b, Qt::CaseInsensitive) == 0)
                return true;
    }
    return false;
}

} // namespace

QString PermissionGate::hardDenyReason(PermissionGate::Kind kind, const QString& target,
                                       const QString& workspaceRoot) const {
    if (kind == Kind::ReadFile || kind == Kind::WriteFile) {
        if (matchesForbiddenPath(target))
            return QStringLiteral("路径命中永不解禁清单（credential/secret/.env/token/id_rsa/密钥证书/.ssh）");
    }
    if (kind == Kind::RunCommand) {
        const QString cmd = target.trimmed();
        // git push 到保护分支（token 化解析，见 gitPushHitsProtected 注释）
        if (gitPushHitsProtected(cmd))
            return QStringLiteral("禁止 git push 到 main/master/protected 分支");
        // 磁盘级操作
        static const QRegularExpression diskLevel(
            QStringLiteral("\\b(format|diskpart|cipher\\s+/w)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (diskLevel.match(cmd).hasMatch())
            return QStringLiteral("禁止磁盘级操作");
        // 递归删除：rd /s、del /s、rmdir /s、rm -r/-rf/-fr、--recursive、Remove-Item -Recurse
        //（-rf 中 r 后跟字母不构成 \b，必须用"含 r 的短选项"整体匹配，否则 rm -rf 漏拦）
        static const QRegularExpression recursiveDel(
            QStringLiteral("\\b(rd|rmdir|del|rm|remove-item|ri)\\b[^|;&]*"
                           "(-[a-z]*r[a-z]*\\b|/s\\b)"),
            QRegularExpression::CaseInsensitiveOption);
        if (recursiveDel.match(cmd).hasMatch()) {
            // 粗策略：命令中任何绝对路径不在工作区内即拒绝；无绝对路径（相对路径钉死工作目录）则放行
            static const QRegularExpression absPath(
                QStringLiteral("[A-Za-z]:[\\\\/][^\\s\"&|;]+"));
            auto it = absPath.globalMatch(cmd);
            while (it.hasNext()) {
                const QString p = it.next().captured(0);
                if (!pathInWorkspace(p, workspaceRoot))
                    return QStringLiteral("工作区外递归删除被永久禁止：%1").arg(p);
            }
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
        // AutoEdit 与 FullAccess 一致：写入仅限工作区（最小权限；FullAccess 的“全”
        // 体现在命令/网络自动放行，而非放开写路径边界）
        return pathInWorkspace(target, workspaceRoot) ? Decision::Allowed : Decision::Denied;
    }

    if (kind == Kind::RunCommand) {
        // 命令白名单：首词必须在名单内（与执行侧同样用 QProcess::splitCommand 解析，
        // 保证"权限放行 = 可执行"，引号包裹的程序名两侧语义一致）
        const QStringList parts = QProcess::splitCommand(target.trimmed());
        const QString first = parts.value(0).toLower();
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
