// 聊天控件纯逻辑单测：变更审查条摘要文案（ChatWidgets::ChangeSummaryBar::formatSummary）
// 说明：mider_ui 层可测的只有不依赖 QApplication 的纯函数——控件构造与布局需人工复核
#include "app/ChatWidgets.h"
#include <doctest/doctest.h>

using miderforge::ChangeSummaryBar;

TEST_CASE("变更审查条摘要：文件数 + 增删行；无增删行时不显示 +0/−0") {
    // 正常的增删
    CHECK(ChangeSummaryBar::formatSummary(3, 42, 11) == QStringLiteral("变更 3 个文件  +42/−11"));
    // 纯新增（removed 为 0 仍显示，因为确实有变更行）
    CHECK(ChangeSummaryBar::formatSummary(1, 5, 0) == QStringLiteral("变更 1 个文件  +5/−0"));
    // 纯删除
    CHECK(ChangeSummaryBar::formatSummary(2, 0, 7) == QStringLiteral("变更 2 个文件  +0/−7"));
    // 只有覆盖但行数统计为 0（内容相同/仅换行）：不显示 +0/−0 噪声
    CHECK(ChangeSummaryBar::formatSummary(4, 0, 0) == QStringLiteral("变更 4 个文件"));
    // 空态
    CHECK(ChangeSummaryBar::formatSummary(0, 0, 0) == QStringLiteral("无文件变更"));
    CHECK(ChangeSummaryBar::formatSummary(-1, 0, 0) == QStringLiteral("无文件变更"));
}

TEST_CASE("变更审查条摘要：中文与负号可读性（− 为 U+2212，非 ASCII 连字符）") {
    const QString s = ChangeSummaryBar::formatSummary(1, 1, 1);
    CHECK(s.contains(QChar(0x2212)));          // 真正的减号
    CHECK_FALSE(s.contains(QLatin1Char('-'))); // 不是 ASCII '-'（避免与文件路径混淆）
}
