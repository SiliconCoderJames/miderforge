// 行级 diff 实现
#include "util/DiffUtil.h"
#include <QStringList>

namespace miderforge::DiffUtil {

namespace {
QStringList splitLines(const QString& text) {
    QString norm = text;
    norm.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList lines = norm.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast(); // 结尾换行产生的空行不参与比较
    return lines;
}
} // namespace

Diff unified(const QString& oldText, const QString& newText,
             int contextLines, int maxDiffLines) {
    Diff d;
    const QStringList a = splitLines(oldText);
    const QStringList b = splitLines(newText);

    // 公共前缀
    int pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre])
        ++pre;
    // 公共后缀（不与前缀重叠）
    int suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre
           && a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
        ++suf;

    const QStringList aMid = a.mid(pre, a.size() - pre - suf);
    const QStringList bMid = b.mid(pre, b.size() - pre - suf);
    if (aMid.isEmpty() && bMid.isEmpty())
        return d; // 文本相同

    d.added = bMid.size();
    d.removed = aMid.size();

    // unified 文本：@@ 头 + 前后文 + −/＋ 行（超限截断）
    QString out = QStringLiteral("@@ -%1,%2 +%3,%4 @@")
                      .arg(pre + 1).arg(aMid.size()).arg(pre + 1).arg(bMid.size());
    int bodyLines = 0;
    auto appendCtx = [&](const QStringList& src, int from, int to) {
        for (int i = from; i < to; ++i) {
            if (bodyLines >= maxDiffLines) {
                d.truncated = true;
                return;
            }
            out += QStringLiteral("\n %1").arg(src[i]);
            ++bodyLines;
        }
    };
    const int ctx = qMax(0, contextLines);
    appendCtx(a, qMax(0, pre - ctx), pre);
    for (const QString& l : aMid) {
        if (bodyLines >= maxDiffLines) { d.truncated = true; break; }
        out += QStringLiteral("\n-%1").arg(l);
        ++bodyLines;
    }
    for (const QString& l : bMid) {
        if (bodyLines >= maxDiffLines) { d.truncated = true; break; }
        out += QStringLiteral("\n+%1").arg(l);
        ++bodyLines;
    }
    if (!d.truncated)
        appendCtx(b, b.size() - suf, qMin(b.size(), b.size() - suf + ctx));
    if (d.truncated)
        out += QStringLiteral("\n…（diff 超出 %1 行上限，已截断）").arg(maxDiffLines);
    d.unified = out;
    return d;
}

} // namespace miderforge::DiffUtil
