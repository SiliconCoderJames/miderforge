// 三档路由器单测（M4）：语义分档 + 升档链
#include "core/Router.h"
#include <doctest/doctest.h>

using miderforge::Router;

TEST_CASE("架构规划/疑难排错/多步推理 → flagship") {
    CHECK(Router::classify(QStringLiteral("做一次整体的架构规划")) == Router::Tier::Flagship);
    CHECK(Router::classify(QStringLiteral("排查这个疑难崩溃")) == Router::Tier::Flagship);
    CHECK(Router::classify(QStringLiteral("多步推理：先分析依赖再设计")) == Router::Tier::Flagship);
}

TEST_CASE("格式化/重命名/简单查询 → fast") {
    CHECK(Router::classify(QStringLiteral("把所有文件按 clang-format 格式化")) == Router::Tier::Fast);
    CHECK(Router::classify(QStringLiteral("重命名这个变量")) == Router::Tier::Fast);
    CHECK(Router::classify(QStringLiteral("简单查询：这个宏在哪定义")) == Router::Tier::Fast);
}

TEST_CASE("默认 main 档") {
    CHECK(Router::classify(QStringLiteral("给 Qt 程序新增 CSV 导出功能")) == Router::Tier::Main);
    CHECK(Router::classify(QStringLiteral("")) == Router::Tier::Main);
}

TEST_CASE("失败升级链：fast→main→flagship（封顶）") {
    CHECK(Router::escalate(Router::Tier::Fast) == Router::Tier::Main);
    CHECK(Router::escalate(Router::Tier::Main) == Router::Tier::Flagship);
    CHECK(Router::escalate(Router::Tier::Flagship) == Router::Tier::Flagship);
}
