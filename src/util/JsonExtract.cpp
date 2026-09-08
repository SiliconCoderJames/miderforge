#include "util/JsonExtract.h"
#include <QJsonParseError>
#include <utility>
#include <vector>

namespace miderforge::jsonextract {

QJsonDocument extractObject(const QString& text) {
    // 第一遍扫描：收集所有括号平衡的对象 span（字符串感知：引号内的括号/引号不计）
    std::vector<std::pair<int, int>> spans; // [start, end) UTF-16 位置
    int depth = 0;
    int objStart = -1;
    bool inString = false;
    bool escaped = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (inString) {
            if (escaped)
                escaped = false;
            else if (ch == u'\\')
                escaped = true;
            else if (ch == u'"')
                inString = false;
            continue;
        }
        if (ch == u'"') {
            inString = true;
        } else if (ch == u'{') {
            if (depth == 0)
                objStart = i;
            ++depth;
        } else if (ch == u'}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && objStart >= 0) {
                    spans.emplace_back(objStart, i + 1);
                    objStart = -1;
                }
            }
        }
    }

    // 第二遍：从最后一个候选向前尝试（正式回答通常在末尾，前面的多是示例）
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        QJsonParseError err{};
        const QJsonDocument doc =
            QJsonDocument::fromJson(text.mid(it->first, it->second - it->first).toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc;
    }
    return {};
}

} // namespace miderforge::jsonextract
