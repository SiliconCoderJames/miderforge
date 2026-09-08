// 应用级共享上下文：权限模式、工作区路径、当日 token 统计等跨面板共享状态（M0 最简全局态；M2 起部分迁移数据库）
#pragma once
#include "core/Breaker.h"
#include <QString>
#include <atomic>

namespace miderforge {

enum class PermissionMode {
    Suggest = 0,   // 默认：读自动/写提案/命令与网络禁止
    AutoEdit = 1,  // 工作区内写自动，区外拒绝；命令逐条确认
    FullAccess = 2 // 全自动（沙箱内），网络白名单放行
};

QString permissionModeName(PermissionMode mode);

class AppContext {
public:
    static AppContext& instance();

    PermissionMode permissionMode = PermissionMode::Suggest;
    QString workspaceRoot;
    // 今日累计（M0 会话内统计；M2 起由 events 表汇总）。
    // 原子化：当前所有读写都在 GUI 线程，但该对象天然跨模块共享，防止后续引入工作线程时静默变成数据竞争
    std::atomic<long long> todayTokens{0};
    QString activeModelLabel;       // 状态栏展示用："zhipu·glm-5.3"
    Breaker::Limits limits;         // 预算与熔断（设置页可改：轮数 25/token 500K/相同失败 3）
    int l1TokenLimit = 4000;        // L1 记忆上限（设置页可改）
    bool autoAcceptSkills = false;  // 技能自沉淀自动通过（设置页可改）

private:
    AppContext() = default;
};

} // namespace miderforge
