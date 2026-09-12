# Miderforge 仓库社交卡片生成器(GitHub Settings → Social preview 用图)
# 产物:docs/assets/social-preview.png(1280x640,GitHub 规定比例 2:1)
# 设计:夜蓝纵向渐变底 + 左侧标题/中文标语/蜜金分隔条/技术栈行 +
#       右侧淡蓝蜂窝六边形(点顶、低透明度)+ 蜜金火花点缀
# 布局红线:所有文字限制在 x < 780,右侧 800..1280 留给六边形图形
# 用法:python resources/scripts/make_social_preview.py   (需 Pillow)
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "docs", "assets", "social-preview.png")
os.makedirs(os.path.dirname(OUT), exist_ok=True)

W, H = 1280, 640

BG_TOP = (40, 48, 63)
BG_BOTTOM = (19, 24, 33)
TITLE = (231, 236, 245)      # 主文本
TAGLINE = (184, 196, 216)    # 中文标语
SUB = (143, 160, 184)        # 次行
TECH = (100, 116, 139)       # 技术栈行
AMBER = (245, 158, 11)
AMBER_L = (255, 214, 130)
HEX_BLUE = (93, 153, 255)

TEXT_LIMIT_X = 780           # 文字右边界
FONT_DIR = r"C:\Windows\Fonts"


def load_font(names, size):
    for n in names:
        p = os.path.join(FONT_DIR, n)
        if os.path.exists(p):
            try:
                return ImageFont_truetype(p, size)
            except Exception:
                pass
    raise RuntimeError("no usable font found in " + FONT_DIR)


def ImageFont_truetype(path, size):
    from PIL import ImageFont
    return ImageFont.truetype(path, size)


def v_gradient(size, top, bottom):
    w, h = size
    strip = Image.new("RGB", (1, 256))
    for y in range(256):
        t = y / 255.0
        strip.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return strip.resize((w, h), Image.BILINEAR)


def hexagon_pts(center, radius):
    cx, cy = center
    return [(cx + radius * __import__("math").cos(__import__("math").radians(60 * i - 90)),
             cy + radius * __import__("math").sin(__import__("math").radians(60 * i - 90)))
            for i in range(6)]


def fit_font(names, text, max_width, start_size):
    """从 start_size 往下找第一个放得进 max_width 的字号"""
    size = start_size
    while size > 20:
        f = load_font(names, size)
        if f.getlength(text) <= max_width:
            return f
        size -= 4
    return load_font(names, 20)


def main():
    img = v_gradient((W, H), BG_TOP, BG_BOTTOM).convert("RGBA")
    d = ImageDraw.Draw(img)

    # ---- 右侧:一大一小两枚淡蓝六边形(点顶)+ 蜜金火花 ------------------
    overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    od.line(hexagon_pts((1064, 300), 252) + [hexagon_pts((1064, 300), 252)[0]],
            fill=HEX_BLUE + (46,), width=10, joint="curve")
    od.line(hexagon_pts((1120, 508), 96) + [hexagon_pts((1120, 508), 96)[0]],
            fill=HEX_BLUE + (26,), width=7, joint="curve")
    # 火花:大六边形顶点附近,渐远渐小
    for (sx, sy, r, a) in [(948, 92, 14, 235), (1010, 56, 9, 200), (1062, 30, 6, 160)]:
        od.ellipse([sx - r * 3, sy - r * 3, sx + r * 3, sy + r * 3],
                   fill=AMBER + (int(a * 0.14),))
    overlay = overlay.filter(ImageFilter.GaussianBlur(0.5))
    img.alpha_composite(overlay)
    d = ImageDraw.Draw(img)
    for (sx, sy, r, a) in [(948, 92, 14, 235), (1010, 56, 9, 200), (1062, 30, 6, 160)]:
        d.ellipse([sx - r, sy - r, sx + r, sy + r], fill=AMBER_L + (a,))

    # ---- 左侧文字(全部限制在 TEXT_LIMIT_X 之内)-----------------------
    x0 = 96
    title_font = fit_font(["segoeuib.ttf", "arialbd.ttf"], "Miderforge",
                          TEXT_LIMIT_X - x0, 132)
    d.text((x0, 148), "Miderforge", font=title_font, fill=TITLE)

    # 蜜金分隔条
    ty = 318
    d.rounded_rectangle([x0, ty, x0 + 240, ty + 10], radius=5, fill=AMBER)

    tag_font = fit_font(["msyh.ttc", "msyhbd.ttc", "simhei.ttf"], "会成长的桌面 AI Agent",
                        TEXT_LIMIT_X - x0, 54)
    d.text((x0, ty + 40), "会成长的桌面 AI Agent", font=tag_font, fill=TAGLINE)

    sub_font = fit_font(["msyh.ttc", "simhei.ttf"], "记忆 · 技能库 · 类人学习 · 自我成长",
                        TEXT_LIMIT_X - x0, 36)
    d.text((x0, ty + 128), "记忆 · 技能库 · 类人学习 · 自我成长", font=sub_font, fill=SUB)

    tech_font = fit_font(["consola.ttf", "segoeui.ttf"], "C++20 · Qt 6 · SQLite WAL+FTS5 · Windows",
                         TEXT_LIMIT_X - x0, 30)
    d.text((x0, H - 96), "C++20 · Qt 6 · SQLite WAL+FTS5 · Windows", font=tech_font, fill=TECH)

    img.convert("RGB").save(OUT, "PNG")
    print("social preview written:", OUT)


if __name__ == "__main__":
    main()
