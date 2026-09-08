// JSON 提取单测：收尾提炼链路的结构化解析（括号深度扫描 + 字符串感知 + 末位候选优先）
#include "util/JsonExtract.h"
#include <QJsonObject>
#include <doctest/doctest.h>

using miderforge::jsonextract::extractObject;

TEST_CASE("纯 JSON 对象直接解析") {
    const auto doc = extractObject(QStringLiteral("{\"result_summary\":\"完成\"}"));
    REQUIRE_FALSE(doc.isNull());
    CHECK(doc.object().value("result_summary").toString() == QStringLiteral("完成"));
}

TEST_CASE("markdown 围栏包裹仍可解析") {
    const QString text = QStringLiteral("```json\n{\"a\": 1}\n```");
    const auto doc = extractObject(text);
    REQUIRE_FALSE(doc.isNull());
    CHECK(doc.object().value("a").toInt() == 1);
}

TEST_CASE("正文含示例对象时取末尾的正式对象（首个{到末个}切片会失败的形态）") {
    const QString text = QStringLiteral(
        "我先示例：{\"example\": true} 现在正式回答：{\"result_summary\":\"任务完成\",\"rounds\":7}");
    const auto doc = extractObject(text);
    REQUIRE_FALSE(doc.isNull());
    CHECK(doc.object().value("result_summary").toString() == QStringLiteral("任务完成"));
    CHECK_FALSE(doc.object().contains("example"));
}

TEST_CASE("字符串内的花括号与转义引号不破坏深度扫描") {
    const QString text = QStringLiteral(
        "{\"content\":\"模型输出含 {嵌套} 与 \\'引号\\' 以及 } 字符\",\"ok\":true}");
    const auto doc = extractObject(text);
    REQUIRE_FALSE(doc.isNull());
    CHECK(doc.object().value("ok").toBool() == true);
}

TEST_CASE("嵌套对象整体提取（内层闭合不提前截断）") {
    const QString text = QStringLiteral("前缀 {\"outer\": {\"inner\": 2}, \"n\": 1} 后缀");
    const auto doc = extractObject(text);
    REQUIRE_FALSE(doc.isNull());
    CHECK(doc.object().value("outer").toObject().value("inner").toInt() == 2);
}

TEST_CASE("无 JSON 与括号不平衡均返回 null 文档") {
    CHECK(extractObject(QStringLiteral("没有任何花括号的回答")).isNull());
    CHECK(extractObject(QStringLiteral("{\"a\":1")).isNull());   // 未闭合
    CHECK(extractObject(QStringLiteral("{\"a\":}")).isNull());   // 语法错误
}
