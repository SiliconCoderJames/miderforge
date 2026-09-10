// 主题系统单测：forge 品牌色板 + QSS token 注入（纯字符串构建，无需 QApplication 实例）
#include "app/Theme.h"
#include <QColor>
#include <doctest/doctest.h>

namespace miderforge {

TEST_CASE("theme：forge 品牌色板默认生效，colors:: 实时函数一致") {
    const auto& pal = theme::palette();
    CHECK(QString::fromLatin1(pal.id) == QStringLiteral("forge"));
    CHECK(QString::fromUtf8(pal.zh).contains(QStringLiteral("熔炉")));
    CHECK(pal.accent == QColor(0xf9, 0x73, 0x16)); // 炉火橙
    CHECK(pal.brand == QColor(0xfb, 0x92, 0x3c));  // 品牌亮橙
    CHECK(theme::colors::accent() == pal.accent);
    CHECK(theme::colors::brand() == pal.brand);
    CHECK(theme::colors::window() == pal.window);
    // 四套色板（forge + 三致敬皮肤）id 唯一
    const auto& ps = theme::palettes();
    CHECK(ps.size() == 4);
    for (int i = 0; i < ps.size(); ++i)
        for (int k = i + 1; k < ps.size(); ++k)
            CHECK(QString::fromLatin1(ps[i].id) != QString::fromLatin1(ps[k].id));
}

TEST_CASE("theme：全局 QSS 注入炉火橙与派生色，token 全部替换") {
    const QString qss = theme::globalStyleSheet();
    CHECK(qss.contains(QStringLiteral("#f97316")));         // accent
    // primary hover 用 accent 派生亮色（lighter(112)），必然比 accent 更亮且非原值
    const QString accentHi = QColor(0xf9, 0x73, 0x16).lighter(112).name();
    CHECK(qss.contains(accentHi));
    CHECK_FALSE(qss.contains(QStringLiteral("%accent")));   // 无残留 token
    CHECK_FALSE(qss.contains(QStringLiteral("%panel")));
    CHECK_FALSE(qss.contains(QStringLiteral("%line")));
    CHECK(theme::brandTagline() == QStringLiteral("锻造云脑 · 常驻本机"));
}

} // namespace miderforge
