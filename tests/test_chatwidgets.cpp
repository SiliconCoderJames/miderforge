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
// ---------- Markdown → 富文本渲染器（mdToHtml v2）：排版层可测的纯函数 ----------
// 这些断言锁住"输出文本的观感契约"：标题分级、列表缩进、代码块语言标签、引用块、
// 行内强调与链接，以及**转义安全**（正文里的 HTML 必须被转义，不能注入）。
using miderforge::mdToHtml;

TEST_CASE("mdToHtml：标题分级（字号/字重/留白）") {
    const QString h1 = mdToHtml(QStringLiteral("# 一级标题"));
    CHECK(h1.contains(QStringLiteral("font-size:15pt")));
    CHECK(h1.contains(QStringLiteral("font-weight:600")));
    CHECK(h1.contains(QStringLiteral("一级标题")));
    CHECK(mdToHtml(QStringLiteral("### 三级")).contains(QStringLiteral("font-size:12.5pt")));
    CHECK(mdToHtml(QStringLiteral("#### 四级")).contains(QStringLiteral("font-size:11.5pt")));
    // 不是标题（#### 后没空格）→ 当普通段落
    CHECK_FALSE(mdToHtml(QStringLiteral("####紧贴")).contains(QStringLiteral("font-weight:600")));
}

TEST_CASE("mdToHtml：无序/有序列表与缩进层级") {
    const QString ul = mdToHtml(QStringLiteral("- 甲\n- 乙"));
    CHECK(ul.contains(QStringLiteral("<ul")));
    CHECK(ul.contains(QStringLiteral("-qt-list-indent")));
    CHECK(ul.contains(QStringLiteral("<li")));
    CHECK(ul.count(QStringLiteral("<li")) == 2);
    const QString ol = mdToHtml(QStringLiteral("1. 第一步\n2. 第二步"));
    CHECK(ol.contains(QStringLiteral("<ol")));
    CHECK(ol.count(QStringLiteral("<li")) == 2);
    CHECK(mdToHtml(QStringLiteral("- 甲\n  - 甲一")).contains(QStringLiteral("margin-left:14px")));
}

TEST_CASE("mdToHtml：围栏代码块带语言标签、等宽字体，块内不做 Markdown 解释") {
    const QString html = mdToHtml(QStringLiteral("```cpp\nint main() { **不是粗体** }\n```"));
    CHECK(html.contains(QStringLiteral("cpp")));
    CHECK(html.contains(QStringLiteral("font-family:'Consolas'")));
    CHECK(html.contains(QStringLiteral("white-space:pre-wrap")));
    CHECK(html.contains(QStringLiteral("**不是粗体**")));
    CHECK_FALSE(html.contains(QStringLiteral("<b>不是粗体")));
    CHECK(html.contains(QStringLiteral("background-color")));
}

TEST_CASE("mdToHtml：引用块 / 分隔线 / 段落行高") {
    const QString q = mdToHtml(QStringLiteral("> 引用一行"));
    CHECK(q.contains(QStringLiteral("border-left:3px solid")));
    CHECK(q.contains(QStringLiteral("引用一行")));
    CHECK(mdToHtml(QStringLiteral("---")).contains(QStringLiteral("<hr")));
    CHECK(mdToHtml(QStringLiteral("普通段落")).contains(QStringLiteral("line-height:165%")));
}

TEST_CASE("mdToHtml：行内强调、行内代码与链接") {
    const QString html = mdToHtml(
        QStringLiteral("**粗** *斜* ~~删~~ `code` [文档](https://example.com/a)"));
    CHECK(html.contains(QStringLiteral("<b>粗</b>")));
    CHECK(html.contains(QStringLiteral("<i>斜</i>")));
    CHECK(html.contains(QStringLiteral("<s>删</s>")));
    CHECK(html.contains(QStringLiteral("<code")));
    CHECK(html.contains(QStringLiteral("href=\"https://example.com/a\"")));
}

TEST_CASE("mdToHtml：HTML 转义安全——正文里的标签必须被转义（不可注入）") {
    const QString html = mdToHtml(QStringLiteral("<script>alert(1)</script>"));
    CHECK_FALSE(html.contains(QStringLiteral("<script>")));
    CHECK(html.contains(QStringLiteral("&lt;script&gt;")));
    const QString code = mdToHtml(QStringLiteral("```\n<img src=x onerror=alert(1)>\n```"));
    CHECK_FALSE(code.contains(QStringLiteral("<img")));
    CHECK(code.contains(QStringLiteral("&lt;img")));
}







TEST_CASE("mdToHtml：空输入返回空串") {
    CHECK(mdToHtml(QString()).isEmpty());
}

