// 内置查看器渲染契约单测（纯函数）：Markdown 与聊天区同源、HTML 净化、其余转义预排版。
#include "app/PreviewDoc.h"
#include <doctest/doctest.h>

using namespace miderforge::previewdoc;



TEST_CASE("查看器：可预览扩展名判定（大小写不敏感 + 未知扩展名不放行）") {
    CHECK(isPreviewable(QStringLiteral("E:/ws/说明.md")));
    CHECK(isPreviewable(QStringLiteral("E:/ws/page.HTML")));
    CHECK(isPreviewable(QStringLiteral("E:/ws/notes.txt")));
    CHECK(isPreviewable(QStringLiteral("E:/ws/data.json")));
    CHECK_FALSE(isPreviewable(QStringLiteral("E:/ws/app.exe")));
    CHECK_FALSE(isPreviewable(QStringLiteral("E:/ws/noext")));
    CHECK(modeName(QStringLiteral("a.md")) == QStringLiteral("Markdown"));
    CHECK(modeName(QStringLiteral("a.htm")) == QStringLiteral("HTML"));
    CHECK(modeName(QStringLiteral("a.cpp")) == QStringLiteral("纯文本"));
}

TEST_CASE("查看器：Markdown 渲染与聊天区同源（标题分级 / 列表 / 代码块）") {
    const QString html = render(QStringLiteral("doc.md"),
                                QStringLiteral("# 标题\n\n- 甲\n- 乙\n\n```cpp\nint a = 1;\n```"));
    CHECK(html.contains(QStringLiteral("font-size:15pt")));          // 标题分级来自 mdToHtml
    CHECK(html.contains(QStringLiteral("<ul")));                      // 列表
    CHECK(html.contains(QStringLiteral("font-family:'Consolas'")));   // 代码块等宽
    CHECK(html.contains(QStringLiteral("cpp")));                      // 语言标签
}

TEST_CASE("查看器：HTML 文件按原样渲染，但净化脚本/事件属性/外联面") {
    const QString html = render(QStringLiteral("page.html"),
                                QStringLiteral("<h1>标题</h1><script>alert(1)</script>"
                                               "<p onclick=\"x()\">正文</p>"
                                               "<iframe src=\"http://evil\"></iframe>"
                                               "<a href=\"javascript:alert(2)\">链接</a>"));
    CHECK(html.contains(QStringLiteral("<h1>标题</h1>")));   // 正文结构保留
    CHECK(html.contains(QStringLiteral("正文")));
    CHECK_FALSE(html.contains(QStringLiteral("<script")));   // 脚本块剔除
    CHECK_FALSE(html.contains(QStringLiteral("onclick")));   // 事件属性剔除
    CHECK_FALSE(html.contains(QStringLiteral("<iframe")));   // 内联框架剔除
    CHECK_FALSE(html.contains(QStringLiteral("javascript:")));// 伪协议剔除
}

TEST_CASE("查看器：纯文本转义 + 等宽预排版（不解释任何标记）") {
    const QString html = render(QStringLiteral("notes.txt"),
                                QStringLiteral("<b>不是粗体</b>\n第二行"));
    CHECK(html.contains(QStringLiteral("&lt;b&gt;")));       // 转义
    CHECK_FALSE(html.contains(QStringLiteral("<b>不是粗体")));
    CHECK(html.contains(QStringLiteral("white-space:pre-wrap")));
    CHECK(html.contains(QStringLiteral("第二行")));
}

TEST_CASE("查看器：未知扩展名按纯文本处理（不放行富文本）") {
    const QString html = render(QStringLiteral("weird.bin"), QStringLiteral("<script>x</script>"));
    CHECK(html.contains(QStringLiteral("&lt;script&gt;")));
}

TEST_CASE("查看器：sanitizeHtml 幂等且保留正常标签属性") {
    const QString src = QStringLiteral("<p class=\"a\" data-x=\"1\">hi</p>");
    CHECK(sanitizeHtml(src).contains(QStringLiteral("data-x=\"1\"")));
    CHECK(sanitizeHtml(sanitizeHtml(src)) == sanitizeHtml(src));
}
