// 熔断器单测（规格 15：熔断计数器必须可脱离 GUI 测试）
#include "core/Breaker.h"
#include <doctest/doctest.h>

using miderforge::Breaker;

TEST_CASE("轮数上限触发熔断") {
    Breaker b({5, 1000000, 3});
    for (int i = 0; i < 4; ++i)
        b.beginRound();
    CHECK(b.check() == Breaker::Reason::None);
    b.beginRound(); // 第 5 轮到达上限：本批工具后即熔断，不再发起下一轮
    CHECK(b.check() == Breaker::Reason::Rounds);
    CHECK(Breaker::reasonText(Breaker::Reason::Rounds).contains(QString("轮数")));
}

TEST_CASE("token 限额触发熔断") {
    Breaker b({1000, 500, 3});
    b.addTokens(499);
    CHECK(b.check() == Breaker::Reason::None);
    b.addTokens(1);
    CHECK(b.check() == Breaker::Reason::Tokens);
}

TEST_CASE("连续相同失败 3 次触发熔断，成功或不同失败清零") {
    Breaker b({1000, 1000000, 3});
    b.recordToolFailure("error: A");
    b.recordToolFailure("error: A");
    CHECK(b.check() == Breaker::Reason::None);
    b.recordToolFailure("error: A");
    CHECK(b.check() == Breaker::Reason::SameFailures);

    SUBCASE("成功清零") {
        b.recordToolSuccess();
        CHECK(b.check() == Breaker::Reason::None);
    }
    SUBCASE("不同失败文本重置计数并重新累计") {
        b.recordToolFailure("error: B");
        CHECK(b.check() == Breaker::Reason::None);
        b.recordToolFailure("error: B");
        CHECK(b.check() == Breaker::Reason::None);
        b.recordToolFailure("error: B");
        CHECK(b.check() == Breaker::Reason::SameFailures);
    }
}

TEST_CASE("默认参数与规格一致") {
    Breaker b;
    CHECK(b.limits().maxRounds == 25);
    CHECK(b.limits().maxTokens == 500000);
    CHECK(b.limits().maxSameFailures == 3);
}
