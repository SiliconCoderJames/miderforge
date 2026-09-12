// 内置查看器渲染契约实现（纯函数，无 UI 依赖，可单测）
#include "app/PreviewDoc.h"

#include "app/ChatWidgets.h" // mdToHtml：与聊天区同源
#include "app/Theme.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QVector>

namespace miderforge::previewdoc {

QStringList extensions() {
    return {QStringLiteral("md"),    QStringLiteral("markdown"), QStringLiteral("html"),
            QStringLiteral("htm"),   QStringLiteral("txt"),      QStringLiteral("log"),
            QStringLiteral("json"),  QStringLiteral("cpp"),      QStringLiteral("h"),
            QStringLiteral("py"),    QStringLiteral("cmake"),    QStringLiteral("yml"),
            QStringLiteral("yaml")};
}

bool isPreviewable(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return !suffix.isEmpty() && extensions().contains(suffix);
}

QString modeName(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        return QStringLiteral("Markdown");
    if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm"))
        return QStringLiteral("HTML");
    return QStringLiteral("纯文本");
}

QString sanitizeHtml(const QString& html) {
    QString out = html;
    // 危险块整体剔除（含内容）
    static const QVector<QRegularExpression> killBlocks = {
        QRegularExpression(QStringLiteral("<script\\b[^>]*>.*?</script>"),
                           QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption),
        QRegularExpression(QStringLiteral("<iframe\\b[^>]*>.*?</iframe>"),
                           QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption),
        QRegularExpression(QStringLiteral("<object\\b[^>]*>.*?</object>"),
                           QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption),
        QRegularExpression(QStringLiteral("<embed\\b[^>]*>"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("<link\\b[^>]*>"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("<meta\\b[^>]*>"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("<base\\b[^>]*>"),
                           QRegularExpression::CaseInsensitiveOption),
    };
    for (const auto& re : killBlocks)
        out.remove(re);
    static const QRegularExpression onAttr(
        QStringLiteral("\\son[a-z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    out.remove(onAttr);
    static const QRegularExpression jsUrl(QStringLiteral("javascript\\s*:"),
                                          QRegularExpression::CaseInsensitiveOption);
    out.remove(jsUrl);
    return out;
}

QString render(const QString& path, const QString& text) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        return QStringLiteral("<div style=\"color:%1;\">%2</div>")
            .arg(theme::colors::text().name(), mdToHtml(text));
    if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm"))
        return sanitizeHtml(text);
    return QStringLiteral("<pre style=\"font-family:'Consolas';font-size:10pt;white-space:pre-wrap;"
                          "margin:0;color:%1;\">%2</pre>")
        .arg(theme::colors::textDim().name(), text.toHtmlEscaped());
}

} // namespace miderforge::previewdoc
