// 三档路由器实现：关键词分类（规格 8）
#include "core/Router.h"
#include <QList>

namespace miderforge {

Router::Tier Router::classify(const QString& goal) {
    // flagship：架构规划/疑难排错/多步推理
    static const QStringList flagshipKeys = {
        QStringLiteral("架构规划"), QStringLiteral("架构设计"), QStringLiteral("疑难"), QStringLiteral("排错"),
        QStringLiteral("排查"), QStringLiteral("多步推理"), QStringLiteral("深度分析"), QStringLiteral("设计评审"),
    };
    // fast：格式化/重命名/简单查询/单文件小改
    static const QStringList fastKeys = {
        QStringLiteral("格式化"), QStringLiteral("重命名"), QStringLiteral("简单查询"), QStringLiteral("单文件"),
        QStringLiteral("改个名"), QStringLiteral("注释"), QStringLiteral("拼写"),
    };
    // fast 先于 flagship 判定：操作意图词（格式化/重命名…）优先于宾语修饰词。
    // "格式化一份架构设计文档"的操作是格式化（fast），"架构设计"只是宾语；先扫 flagship 会误升档
    for (const QString& k : fastKeys)
        if (goal.contains(k))
            return Tier::Fast;
    for (const QString& k : flagshipKeys)
        if (goal.contains(k))
            return Tier::Flagship;
    return Tier::Main; // 默认全部走 main 档（规格 8）
}

} // namespace miderforge
