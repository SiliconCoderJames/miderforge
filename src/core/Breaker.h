// 熔断器（三重保险的纯逻辑计数器，可脱离 GUI 单测）：轮数上限 / 单任务 token 限额 / 连续相同失败检测
#pragma once
#include <QString>

namespace miderforge {

class Breaker {
public:
    struct Limits {
        int maxRounds = 25;            // 轮数上限（默认 25）
        long long maxTokens = 500000;  // 单任务 token 限额（默认 500K）
        int maxSameFailures = 3;       // 连续相同失败次数（返工死循环判定）
    };

    explicit Breaker(const Limits& limits = Limits()) : m_limits(limits) {}

    void beginRound() { ++m_round; }
    void addTokens(long long n) { m_tokens += n; }

    // 工具失败结果登记：连续相同失败文本计数；成功/不同失败均清零
    void recordToolFailure(const QString& errorText) {
        if (errorText == m_lastFailure)
            ++m_sameFailures;
        else {
            m_lastFailure = errorText;
            m_sameFailures = 1;
        }
    }
    void recordToolSuccess() {
        m_lastFailure.clear();
        m_sameFailures = 0;
    }

    enum class Reason { None, Rounds, Tokens, SameFailures };
    // 每轮末/每次工具后调用：任一触发即熔断
    Reason check() const {
        if (m_round >= m_limits.maxRounds)
            return Reason::Rounds;
        if (m_tokens >= m_limits.maxTokens)
            return Reason::Tokens;
        if (m_sameFailures >= m_limits.maxSameFailures)
            return Reason::SameFailures;
        return Reason::None;
    }

    static QString reasonText(Reason r) {
        switch (r) {
        case Reason::Rounds: return QStringLiteral("已达到轮数上限，任务熔断");
        case Reason::Tokens: return QStringLiteral("已达到单任务 token 限额，任务熔断");
        case Reason::SameFailures: return QStringLiteral("工具连续相同失败，判定返工死循环，任务熔断");
        case Reason::None: break;
        }
        return QString();
    }

    int rounds() const { return m_round; }
    long long tokens() const { return m_tokens; }
    const Limits& limits() const { return m_limits; }

private:
    Limits m_limits;
    int m_round = 0;
    long long m_tokens = 0;
    QString m_lastFailure;
    int m_sameFailures = 0;
};

} // namespace miderforge
