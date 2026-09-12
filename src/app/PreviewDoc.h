// 内置查看器的渲染契约（纯函数，可单测）：把待查看文件转成 QTextBrowser 可渲染的 HTML。
// 放在 app 层而非 core：Markdown 渲染复用聊天区的 miderforge::mdToHtml（app 层），
// 而分层纪律禁止 core 反向依赖 app。
// 设计取舍：
//   - Markdown 与聊天区**同源**——同一份文档在消息里和查看器里观感必须一致；
//   - HTML 文件交给富文本引擎（Qt 富文本不执行脚本），但仍净化掉可加载/可外联面，
//     避免"看文档"变成"跑文档"；
//   - 其余文本一律转义后等宽预排版，绝不解释。
#pragma once
#include <QString>
#include <QStringList>

namespace miderforge::previewdoc {

// 可预览的扩展名（小写，不含点）
QStringList extensions();
// 是否值得在查看器里打开（按扩展名判定，大小写不敏感）
bool isPreviewable(const QString& path);
// 扩展名 → 渲染模式名（Markdown / HTML / 纯文本），供状态栏显示
QString modeName(const QString& path);
// 渲染为 QTextBrowser 可用的 HTML；path 仅用于判定模式，text 为文件内容
QString render(const QString& path, const QString& text);
// HTML 净化：剥离 script/iframe/object/embed/link/meta/base、on* 事件属性与 javascript: 链接
QString sanitizeHtml(const QString& html);

} // namespace miderforge::previewdoc
