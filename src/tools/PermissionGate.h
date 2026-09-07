// 权限门（Codex 三档 + 永不解禁清单）：软件主防线；读写路径判定与命令白名单全部为纯逻辑、可脱离 GUI 单测
#pragma once
#include "core/AppContext.h"
#include <QString>
#include <QStringList>

namespace miderforge {

class PermissionGate {
public:
    enum class Kind { ReadFile, WriteFile, RunCommand, Network };
    enum class Decision { Allowed, Denied, NeedsConfirm };

    // ---- 评估入口 ----
    // 绝对路径归一后判定；workspaceRoot 用于 Auto Edit 的区内/区外判定
    Decision evaluate(PermissionMode mode, Kind kind, const QString& target,
                      const QString& workspaceRoot) const;

    // 永不解禁清单：任何档位直接拒绝（credential/secret/.env/token/id_rsa、
    // 工作区外递归删除/磁盘级操作、git push 到保护分支）。命中返回拒绝理由，未命中返回空
    QString hardDenyReason(Kind kind, const QString& target, const QString& workspaceRoot) const;

    // 命令白名单（首词判定；可配置，v1 内置编译工具链）
    static QStringList commandWhitelist();
    // 网络白名单域名（v1 内置为空=全需确认；Full Access 下命中才全自动）
    static QStringList networkWhitelist();

    // ---- 会话内"总是允许"记忆（仅本会话有效） ----
    void grantAlwaysForSession(Kind kind) { m_alwaysAllowed[int(kind)] = true; }
    bool alwaysAllowedForSession(Kind kind) const { return m_alwaysAllowed[int(kind)]; }
    void resetSessionGrants();

private:
    static bool pathInWorkspace(const QString& absPath, const QString& workspaceRoot);
    bool matchesForbiddenPath(const QString& path) const;

    bool m_alwaysAllowed[4] = {false, false, false, false};
};

} // namespace miderforge
