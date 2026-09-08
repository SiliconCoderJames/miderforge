// SseParser 单测（M0 验收要求 ≥6 个用例，含跨块边界用例）
#include "llm/SseParser.h"
#include <doctest/doctest.h>

using miderforge::SseParser;

static std::vector<QString> feedAll(SseParser& p, std::initializer_list<const char*> chunks) {
    for (const char* c : chunks)
        p.feed(c, strlen(c));
    return p.takeCompleteEvents();
}

TEST_CASE("单个事件·单块到达") {
    SseParser p;
    auto ev = feedAll(p, {"data: {\"a\":1}\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("{\"a\":1}"));
}

TEST_CASE("多个事件·同块到达") {
    SseParser p;
    auto ev = feedAll(p, {"data: one\n\ndata: two\n\ndata: three\n\n"});
    REQUIRE(ev.size() == 3);
    CHECK(ev[0] == QString("one"));
    CHECK(ev[1] == QString("two"));
    CHECK(ev[2] == QString("three"));
}

TEST_CASE("跨块边界·事件中段被切开") {
    SseParser p;
    p.feed(QByteArray("data: {\"choices\":[{\"del"));
    auto ev1 = p.takeCompleteEvents();
    CHECK(ev1.empty()); // 半帧不得交出
    p.feed(QByteArray("ta\":{\"content\":\"你好\"}]}\n\n"));
    auto ev2 = p.takeCompleteEvents();
    REQUIRE(ev2.size() == 1);
    CHECK(ev2[0] == QString("{\"choices\":[{\"delta\":{\"content\":\"你好\"}]}"));
}

TEST_CASE("CRLF 行尾与 CRLF 空行分隔") {
    SseParser p;
    auto ev = feedAll(p, {"data: alpha\r\n\r\ndata: beta\r\n\r\n"});
    REQUIRE(ev.size() == 2);
    CHECK(ev[0] == QString("alpha"));
    CHECK(ev[1] == QString("beta"));
}

TEST_CASE("CRLF 被切在 \\r 与 \\n 之间（悬挂回车）") {
    SseParser p;
    p.feed("data: x\r", 8);
    CHECK(p.takeCompleteEvents().empty()); // \r 悬挂，不得误判为空行
    p.feed("\ndata: y\r\n\r\n", 13);
    auto ev = p.takeCompleteEvents();
    // "data: x\r\n" 只是一个行尾：x 与 y 之间没有空行，属于同一事件的两行 data
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("x\ny"));
}

TEST_CASE("[DONE] 哨兵帧原样透传") {
    SseParser p;
    auto ev = feedAll(p, {"data: [DONE]\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("[DONE]"));
}

TEST_CASE("注释行与 event/id 字段被忽略") {
    SseParser p;
    auto ev = feedAll(p, {": keep-alive\n\n"
                          "event: delta\nid: 42\ndata: payload\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("payload"));
}

TEST_CASE("多行 data 合并") {
    SseParser p;
    auto ev = feedAll(p, {"data: line1\ndata: line2\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("line1\nline2"));
}

TEST_CASE("data 后无空格") {
    SseParser p;
    auto ev = feedAll(p, {"data:no-space\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("no-space"));
}

TEST_CASE("不完整尾部保留·不得提前解析") {
    SseParser p;
    p.feed(QByteArray("data: first\n\ndata: secon"));
    auto ev = p.takeCompleteEvents();
    REQUIRE(ev.size() == 1); // 只有 first 完整
    CHECK(ev[0] == QString("first"));
    CHECK(p.hasPending());
    p.feed(QByteArray("d\n\n"));
    ev = p.takeCompleteEvents();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("second"));
}

TEST_CASE("flushRemainder 交出漏发空行的最后一帧") {
    SseParser p;
    p.feed(QByteArray("data: done-frame")); // 用 QByteArray 重载，避免手写长度偏差
    CHECK(p.takeCompleteEvents().empty());
    auto ev = p.flushRemainder();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("done-frame"));
    CHECK_FALSE(p.hasPending());
}

TEST_CASE("reset 清空全部状态") {
    SseParser p;
    p.feed("data: half", 11);
    p.reset();
    CHECK_FALSE(p.hasPending());
    CHECK(p.takeCompleteEvents().empty());
    CHECK(p.flushRemainder().empty());
}

TEST_CASE("UTF-8 BOM 前置的首帧不丢（部分代理/网关行为）") {
    SseParser p;
    // 字符串拼接断开十六进制转义：MSVC 会把 "\xEF\xBB\xBFda" 当作一个超范围转义
    const char withBom[] = "\xEF\xBB\xBF" "data: first\n\n";
    p.feed(withBom, strlen(withBom));
    auto ev = p.takeCompleteEvents();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == QString("first"));
}

TEST_CASE("BOM 只在缓冲区起点剥离，流中段出现按普通数据对待") {
    SseParser p;
    p.feed("data: x\n\n", 10);
    const char withBom[] = "\xEF\xBB\xBF" "data: y\n\n";
    p.feed(withBom, strlen(withBom));
    auto ev = p.takeCompleteEvents();
    REQUIRE(ev.size() == 1); // 带 BOM 的行不是合法 data 行，整块被忽略
    CHECK(ev[0] == QString("x"));
}
