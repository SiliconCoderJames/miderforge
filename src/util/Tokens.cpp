// token 估算实现
#include "util/Tokens.h"
#include <QString>

namespace miderforge::tokens {

long long estimate(const QString& text) {
    long long cjk = 0, ascii = 0;
    for (const QChar ch : text) {
        if (ch.isNullOrWhitespace())
            continue;
        if (ch.unicode() < 0x80)
            ++ascii;
        else
            ++cjk;
    }
    return cjk + (ascii + 3) / 4;
}

} // namespace miderforge::tokens
