// 主题系统单测：forge 品牌色板 + QSS token 注入（纯字符串构建，无需 QApplication 实例）
//
// ⚠️ 测试隔离：paletteIndex() 读的是 QSettings 持久化值（用户切过皮肤就不是 forge 了），
// 因此断言具体颜色前必须先把色板切到 forge，测完再还原——否则本用例的结果会取决于
// 开发机上最后一次选的主题（实测：把皮肤切到 ZCode 后本文件 3 个用例全红）。
#include "app/Theme.h"
#include <QColor>
#include <QSettings>
#include <doctest/doctest.h>

namespace miderforge {

namespace {
// 固定到 forge 品牌主题，析构时还原用户原设置
struct ForgeThemeGuard {
    int saved = theme::paletteIndex();
    ForgeThemeGuard() { theme::setPaletteIndex(0); }
    ~ForgeThemeGuard() { theme::setPaletteIndex(saved); }
};
} // namespace

TEST_CASE("theme：forge 品牌色板默认生效，colors:: 实时函数一致") {
    ForgeThemeGuard guard;
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
    ForgeThemeGuard guard;
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

TEST_CASE("theme：品牌签名渐变（左栏签名线/空态徽标/聚焦态共用）") {
    ForgeThemeGuard guard;
    const QString g = theme::brandGradient();
    // 必须同时含品牌色与强调色两个 stop，且是合法 qlineargradient
    CHECK(g.startsWith(QStringLiteral("qlineargradient(")));
    CHECK(g.contains(QStringLiteral("#fb923c"))); // brand
    CHECK(g.contains(QStringLiteral("#f97316"))); // accent
    CHECK(g.contains(QStringLiteral("stop:0")));
    CHECK(g.contains(QStringLiteral("stop:1")));
    // 方向可定制（空态徽标用对角线）
    const QString diag = theme::brandGradient(0.0, 0.0, 1.0, 1.0);
    CHECK(diag.contains(QStringLiteral("x2:1")));
    CHECK(diag.contains(QStringLiteral("y2:1")));
    // 竖向（下拉/分隔线场景）
    CHECK(theme::brandGradient(0.0, 0.0, 0.0, 1.0).contains(QStringLiteral("y2:1")));
}

} // namespace miderforge
