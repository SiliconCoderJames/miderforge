// 交互层纯逻辑单测：命令面板检索排序（CommandPalette::rank）
// 说明：面板本体与快捷键需人工/实机复核，这里只锁住可确定验证的排序语义
#include "app/CommandPalette.h"
#include <doctest/doctest.h>

using miderforge::CommandPalette;

TEST_CASE("命令面板：空查询返回全部且保持原序") {
    const QStringList titles = {QStringLiteral("会话"), QStringLiteral("任务队列"), QStringLiteral("记忆")};
    const auto r = CommandPalette::rank(titles, QString());
    REQUIRE(r.size() == 3);
    CHECK(r[0] == 0);
    CHECK(r[1] == 1);
    CHECK(r[2] == 2);
    // 全空白同样视为空查询
    CHECK(CommandPalette::rank(titles, QStringLiteral("   ")).size() == 3);
}

TEST_CASE("命令面板：前缀命中优先于中间命中，大小写不敏感") {
    const QStringList titles = {QStringLiteral("New Session"), QStringLiteral("记忆"), QStringLiteral("session list")};
    const auto r = CommandPalette::rank(titles, QStringLiteral("session"));
    REQUIRE(r.size() == 2);
    // "session list" 在 pos=0（真前缀命中）；"New Session" 命中在 pos=4（"Session" 是第二个词）
    CHECK(r[0] == 2);
    CHECK(r[1] == 0);
    // 大小写不敏感：两者都能命中，顺序不变
    const auto upper = CommandPalette::rank(titles, QStringLiteral("SESSION"));
    REQUIRE(upper.size() == 2);
    CHECK(upper[0] == 2);
    CHECK(upper[1] == 0);
    // 仅中间命中时只剩一条
    CHECK(CommandPalette::rank(titles, QStringLiteral("list")).size() == 1);
    CHECK(CommandPalette::rank(titles, QStringLiteral("记忆")).size() == 1);
}

TEST_CASE("命令面板：前缀命中优先于中间命中，同级短标题优先") {
    const QStringList titles = {QStringLiteral("打开设置"), QStringLiteral("设置"), QStringLiteral("设置主题皮肤")};
    const auto r = CommandPalette::rank(titles, QStringLiteral("设置"));
    REQUIRE(r.size() == 3);
    CHECK(r[0] == 1); // "设置"：pos=0 且最短
    CHECK(r[1] == 2); // "设置主题皮肤"：pos=0，较长
    CHECK(r[2] == 0); // "打开设置"：pos=2 靠后
}

TEST_CASE("命令面板：同级命中时更短的标题更精确") {
    // 三条都在 pos=0 命中「设置」，按长度升序：设置(2) < 设置项(3) < 设置主题皮肤(6)
    const QStringList titles = {QStringLiteral("设置主题皮肤"), QStringLiteral("设置"), QStringLiteral("设置项")};
    const auto r = CommandPalette::rank(titles, QStringLiteral("设置"));
    REQUIRE(r.size() == 3);
    CHECK(r[0] == 1); // 设置
    CHECK(r[1] == 2); // 设置项
    CHECK(r[2] == 0); // 设置主题皮肤
}

TEST_CASE("命令面板：无匹配返回空，不抛错") {
    const QStringList titles = {QStringLiteral("会话"), QStringLiteral("记忆")};
    CHECK(CommandPalette::rank(titles, QStringLiteral("zzz")).isEmpty());
    CHECK(CommandPalette::rank(QStringList(), QStringLiteral("x")).isEmpty());
}

TEST_CASE("命令面板：中文子串命中（无分词需求，直接子串）") {
    const QStringList titles = {QStringLiteral("任务队列"), QStringLiteral("技能库"), QStringLiteral("审计日志")};
    const auto r = CommandPalette::rank(titles, QStringLiteral("日志"));
    REQUIRE(r.size() == 1);
    CHECK(r[0] == 2);
    CHECK(CommandPalette::rank(titles, QStringLiteral("队列")).size() == 1);
}
