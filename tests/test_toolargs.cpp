// 工具调用参数累积器单测（踩坑清单 #2：arguments 分片流按 index 累积，终结时才解析）
#include "llm/ToolCallAccumulator.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <doctest/doctest.h>

using miderforge::ToolCallAccumulator;
using miderforge::ToolCallParts;

TEST_CASE("参数碎片跨多帧累积") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "call_1", "read_file", "{\"pa");
    acc.feedDelta(0, "", "", "th\": \"C:/x.txt\"}");
    const ToolCallParts* part = acc.findByIndex(0);
    REQUIRE(part != nullptr);
    CHECK(part->name == QString("read_file"));
    CHECK(part->id == QString("call_1"));
    CHECK(part->arguments == QString("{\"path\": \"C:/x.txt\"}"));

    bool ok = false;
    auto out = acc.finalizeAll(&ok);
    CHECK(ok);
    REQUIRE(out.size() == 1);
    const QJsonObject obj = QJsonDocument::fromJson(out[0].arguments.toUtf8()).object();
    CHECK(obj.value("path").toString() == QString("C:/x.txt"));
}

TEST_CASE("多工具调用按 index 分轨互不串扰") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "a", "t1", "{\"x\":");
    acc.feedDelta(1, "b", "t2", "{\"y\":");
    acc.feedDelta(0, "", "", "1}");
    acc.feedDelta(1, "", "", "2}");
    bool ok = false;
    auto out = acc.finalizeAll(&ok);
    CHECK(ok);
    REQUIRE(out.size() == 2);
    CHECK(out[0].index == 0);
    CHECK(out[0].name == QString("t1"));
    CHECK(out[0].arguments == QString("{\"x\":1}"));
    CHECK(out[1].index == 1);
    CHECK(out[1].name == QString("t2"));
    CHECK(out[1].arguments == QString("{\"y\":2}"));
}

TEST_CASE("name 分片（个别厂商行为）") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "c", "read_", "");
    acc.feedDelta(0, "", "file", "");
    const ToolCallParts* part = acc.findByIndex(0);
    REQUIRE(part != nullptr);
    CHECK(part->name == QString("read_file"));
}

TEST_CASE("空参数视为无参调用") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "c", "list_dir", "");
    bool ok = false;
    auto out = acc.finalizeAll(&ok);
    CHECK(ok);
    REQUIRE(out.size() == 1);
    CHECK(out[0].arguments.isEmpty());
}

TEST_CASE("非法 JSON 参数容错：不崩溃且 okAll=false") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "c", "t", "{\"broken\": "); // 残缺
    bool ok = true;
    auto out = acc.finalizeAll(&ok);
    CHECK_FALSE(ok);
    CHECK(out.empty()); // 坏调用被丢弃
}

TEST_CASE("混好坏调用：好的保留坏的丢弃") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "a", "good", "{\"k\":\"v\"}");
    acc.feedDelta(1, "b", "bad", "not-json{");
    bool ok = true;
    auto out = acc.finalizeAll(&ok);
    CHECK_FALSE(ok);
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == QString("good"));
}

TEST_CASE("reset 清空") {
    ToolCallAccumulator acc;
    acc.feedDelta(0, "c", "t", "{}");
    acc.reset();
    CHECK(acc.count() == 0);
    CHECK(acc.findByIndex(0) == nullptr);
}
