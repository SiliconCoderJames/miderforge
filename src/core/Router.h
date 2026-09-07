// 三档路由器（规格 8）：按任务语义选档 fast/main/flagship；纯逻辑可单测。
// 执行中同一工具连续失败 2 次的升档判定也在此（tier_escalate）
#pragma once
#include <QString>

namespace miderforge {

class Router {
public:
    enum class Tier { Fast, Main, Flagship };

    // 关键词分类：架构规划/疑难排错/多步推理 → flagship；格式化/重命名/简单查询/单文件小改 → fast；默认 main
    static Tier classify(const QString& goal);

    static QString tierName(Tier t) {
        switch (t) {
        case Tier::Fast: return QStringLiteral("fast");
        case Tier::Main: return QStringLiteral("main");
        case Tier::Flagship: return QStringLiteral("flagship");
        }
        return QStringLiteral("main");
    }

    // 升档判定：当前档位与连续相同失败次数 → 返回升档后的档位（不可再升返回原档）
    static Tier escalate(Tier current) {
        switch (current) {
        case Tier::Fast: return Tier::Main;
        case Tier::Main: return Tier::Flagship;
        case Tier::Flagship: return Tier::Flagship;
        }
        return Tier::Main;
    }
};

} // namespace miderforge
