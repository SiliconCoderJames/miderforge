// 矛盾裁决服务单测（矛盾扫描 v2）：纯逻辑部分——提示词组装 + LLM 输出解析（标签白名单/id 校验）
#include "core/AdjudicationService.h"
#include <QJsonArray>
#include <QJsonObject>
#include <doctest/doctest.h>

using miderforge::AdjudicationService;
using miderforge::MemoryManager;

namespace {
QVector<MemoryManager::ContradictionPair> samplePairs() {
    QVector<MemoryManager::ContradictionPair> pairs;
    pairs.push_back({1, 2, QStringLiteral("项目用 Meson 组织"),
                     QStringLiteral("项目已迁移到 CMake"), QStringLiteral("project_facts"), 0.6});
    pairs.push_back({7, 9, QStringLiteral("缩进用空格"),
                     QStringLiteral("缩进用 Tab"), QStringLiteral("coding_pref"), 0.5});
    return pairs;
}
} // namespace

TEST_CASE("裁决提示词：包含编号对与三态说明") {
    const QString prompt = AdjudicationService::buildPrompt(samplePairs());
    CHECK(prompt.contains(QStringLiteral("#1")));
    CHECK(prompt.contains(QStringLiteral("#9")));
    CHECK(prompt.contains(QStringLiteral("contradiction")));
    CHECK(prompt.contains(QStringLiteral("duplicate")));
    CHECK(prompt.contains(QStringLiteral("complement")));
    CHECK(prompt.contains(QStringLiteral("verdicts")));
}

TEST_CASE("parseVerdicts：合法判定按 id 对齐候选对，非法标签归 unknown") {
    const QString llm = QStringLiteral(
        "{\"verdicts\":["
        "{\"a\":2,\"b\":1,\"verdict\":\"Contradiction\",\"reason\":\"迁移前后矛盾\"},"
        "{\"a\":7,\"b\":9,\"verdict\":\"nonsense\",\"reason\":\"标签非法归 unknown\"}]}");
    const auto out = AdjudicationService::parseVerdicts(llm, samplePairs());
    REQUIRE(out.size() == 2);
    // a/b 顺序宽容：第一对给了 (2,1) 也应命中候选 (1,2)
    bool hitFirst = (out[0].idA == 1 && out[0].idB == 2) || (out[0].idA == 2 && out[0].idB == 1);
    CHECK(hitFirst);
    CHECK(out[0].label == QStringLiteral("contradiction"));
    CHECK(out[1].label == QStringLiteral("unknown"));
}

TEST_CASE("parseVerdicts：幻觉 id 过滤、同对只采纳首条判定") {
    const QString llm = QStringLiteral(
        "{\"verdicts\":["
        "{\"a\":99,\"b\":100,\"verdict\":\"contradiction\",\"reason\":\"不存在\"},"
        "{\"a\":1,\"b\":2,\"verdict\":\"duplicate\",\"reason\":\"第二条\"},"
        "{\"a\":1,\"b\":2,\"verdict\":\"nonsense\",\"reason\":\"应被首条去重\"},"
        "{\"a\":1,\"b\":2,\"verdict\":\"duplicate\",\"reason\":\"重复判定\"}]}");
    const auto out = AdjudicationService::parseVerdicts(llm, samplePairs());
    REQUIRE(out.size() == 1);
    CHECK(out.front().label == QStringLiteral("duplicate"));
    CHECK(out.front().reason == QStringLiteral("第二条"));
}

TEST_CASE("parseVerdicts：围栏/前后闲话/无 JSON 均稳健") {
    const QString fenced = QStringLiteral("```json\n{\"verdicts\":[{\"a\":1,\"b\":2,"
                                          "\"verdict\":\"contradiction\",\"reason\":\"r\"}]}\n```");
    CHECK(AdjudicationService::parseVerdicts(fenced, samplePairs()).size() == 1);
    CHECK(AdjudicationService::parseVerdicts(QStringLiteral("没有 JSON"), samplePairs()).isEmpty());
    CHECK(AdjudicationService::parseVerdicts(QStringLiteral("{\"verdicts\":[]}"), samplePairs()).isEmpty());
}
