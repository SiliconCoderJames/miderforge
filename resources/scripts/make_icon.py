# Miderforge 应用图标生成器 v3:「一击」竖直锻击构图
# 设计语言(v3 重构,对 v2 做减法):
#   - 叙事:锻锤自上而下,锤击一枚点顶六棱蜜金锭——锤=Forge,六棱锭=Mider 血统(MiderHive),
#     一击之下的白热星芒=锻造瞬间。三个元素讲完品牌,再无第七层特效。
#   - 网格:2048 画布,12 列网格;锤头 928x450、柄 124 宽、锭 R=180,间距 60,全部落格。
#   - 光效只留三种:锤头一道机加工高光线、触点一团暖 radial 辉光、白热星芒本体。
#     玻璃高光/暗角/独立投影/散点火花全部删除(v2 的七层在 16px 处糊成一团)。
#   - 配色与 UI 深色主题同源:夜蓝底、蜜金锤、白热星。
# 产物:resources/assets/miderforge.ico(16..256)、miderforge-256.png(1024 源)、
#       icon-preview.png(变体 A/B + 尺寸阶梯,深浅底可读性检查)
# 用法:python resources/scripts/make_icon.py   (需 Pillow)
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
os.makedirs(ASSETS, exist_ok=True)

S = 2048  # 超采样画布

# 配色(与 UI 深色主题同源)
BG_TOP = (38, 46, 62)        # 夜蓝
BG_BOTTOM = (17, 22, 30)
HEAD_L = (255, 223, 166)     # 锤头亮部(上)
HEAD_R = (232, 146, 7)       # 锤头暗部(下)
HANDLE_T = (240, 185, 107)   # 柄顶
HANDLE_B = (156, 98, 20)     # 柄底
INGOT_T = (255, 217, 143)    # 锭亮部
INGOT_B = (217, 123, 6)      # 锭暗部
EMBER = (245, 158, 11)       # 触点辉光
STAR_W = (255, 249, 236)     # 星芒白热
STAR_A = (255, 196, 87)      # 星芒外圈


# ---------------------------------------------------------------- 渐变工具 ----
def v_gradient(size, top, bottom):
    w, h = size
    strip = Image.new("RGB", (1, 256))
    for y in range(256):
        t = y / 255.0
        strip.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return strip.resize((w, h), Image.BILINEAR)


def h_gradient(size, left, right):
    w, h = size
    strip = Image.new("RGB", (256, 1))
    for x in range(256):
        t = x / 255.0
        strip.putpixel((x, 0), tuple(int(l + (r - l) * t) for l, r in zip(left, right)))
    return strip.resize((w, h), Image.BILINEAR)


def radial_overlay(size, center, r_inner, r_outer, color, a_inner, a_outer, steps=72):
    ov = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    cx, cy = center
    for i in range(steps, 0, -1):
        t = i / steps
        r = r_inner + (r_outer - r_inner) * t
        a = int(a_inner + (a_outer - a_inner) * t)
        if a <= 0:
            continue
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color + (a,))
    return ov


def rounded_gradient(canvas, box, radius, grad):
    w, h = box[2] - box[0], box[3] - box[1]
    if grad.size != (w, h):
        grad = grad.resize((w, h), Image.BILINEAR)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w, h], radius=radius, fill=255)
    canvas.paste(grad, (box[0], box[1]), mask)


def hexagon(center, radius):
    """点顶正六边形顶点"""
    cx, cy = center
    return [(cx + radius * math.sin(math.radians(60 * i)),
             cy - radius * math.cos(math.radians(60 * i))) for i in range(6)]


def star4(layer, center, r_long, r_short, core_r, color_core, color_halo):
    """四芒星芒:柔光晕 + 细长四芒 + 白热核。锻造触点的点睛一笔"""
    cx, cy = center
    halo = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([cx - r_long * 2.4, cy - r_long * 2.4,
                                  cx + r_long * 2.4, cy + r_long * 2.4],
                                 fill=color_halo + (110,))
    layer.alpha_composite(halo.filter(ImageFilter.GaussianBlur(40)))
    pts = []
    for i in range(8):
        ang = math.radians(90 * (i // 2) + 45 * (i % 2))
        r = r_long if i % 2 == 0 else r_short
        pts.append((cx + r * math.cos(ang), cy - r * math.sin(ang)))
    ImageDraw.Draw(layer).polygon(pts, fill=color_halo + (235,))
    core = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    ImageDraw.Draw(core).ellipse([cx - core_r, cy - core_r, cx + core_r, cy + core_r],
                                 fill=color_core + (255,))
    layer.alpha_composite(core.filter(ImageFilter.GaussianBlur(4)))


# ---------------------------------------------------------------- 锤体 ----
def hammer_boxes():
    """直立锻锤(击打方向朝下):头 slab 一体成型(v2 的五段拼接缝全部取消)"""
    return [
        (560, 430, 1488, 880),   # 锤头
        (962, 856, 1086, 1400),  # 手柄(上端没入锤头之下,视觉一体)
    ]


def hammer_layer(tilt_deg):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    head_g = v_gradient((928, 450), HEAD_L, HEAD_R)
    handle_g = v_gradient((124, 544), HANDLE_T, HANDLE_B)
    for box, r, g in zip(hammer_boxes(), [96, 54], [head_g, handle_g]):
        rounded_gradient(layer, box, r, g)
    # 机加工高光:头沿一道细白线,立即有金属感
    spec = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(spec).rounded_rectangle([614, 474, 1434, 512], radius=19,
                                           fill=(255, 255, 255, 82))
    layer.alpha_composite(spec)
    return layer.rotate(tilt_deg, resample=Image.BICUBIC, center=(1024, 900))


# ---------------------------------------------------------------- 合成 ----
def compose(tilt_deg):
    canvas = v_gradient((S, S), BG_TOP, BG_BOTTOM).convert("RGBA")
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([64, 64, S - 64, S - 64], radius=448, fill=255)

    # 触点辉光:锤与锭之间一团暖光,击打瞬间从缝里亮出来
    glow = radial_overlay((S, S), (1024, 1430), 70, 560, EMBER, 96, 0)
    canvas.alpha_composite(glow)

    # 六棱锭(点顶,血统暗号):锤下被锻之物
    ingot = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ig = v_gradient((380, 360), INGOT_T, INGOT_B)
    im = Image.new("L", (380, 360), 0)
    pts = [(190 + r * math.sin(math.radians(60 * i)),
            180 - r * math.cos(math.radians(60 * i))) for i, r in
           [(i, 178) for i in range(6)]]
    ImageDraw.Draw(im).polygon(pts, fill=255)
    ingot.paste(ig, (1024 - 190, 1640 - 180), im)
    canvas.alpha_composite(ingot)

    # 白热星芒:击打缝隙正中
    star4(canvas, (1024, 1424), 96, 30, 26, STAR_W, STAR_A)

    # 锤体(带 tilt 的动感)
    layer = hammer_layer(tilt_deg)
    canvas.alpha_composite(layer)

    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(canvas, (0, 0), mask)
    return out


def main():
    variants = {"A": compose(0), "B": compose(8)}  # A 端正 / B 微倾(动势)

    # 预览:上排 A/B 大图;下排 B 的尺寸阶梯(128/64/32/16)验证小尺寸可读性
    pv = Image.new("RGB", (1024, 1024), (238, 240, 243))
    for i, key in enumerate(("A", "B")):
        icon = variants[key].resize((512, 512), Image.LANCZOS)
        pv.paste(icon, (i * 512, 0), icon)
    ladder_x = [32, 192, 320, 416]
    ladder_s = [128, 64, 32, 16]
    for x, s_ in zip(ladder_x, ladder_s):
        icon = variants["B"].resize((s_, s_), Image.LANCZOS)
        pv.paste(icon, (x, 640), icon)
    pv.save(os.path.join(ASSETS, "icon-preview.png"))

    # 正式产物沿用既有文件名(引用点零改动):默认 B(微倾,有动势)
    master = variants["B"].resize((1024, 1024), Image.LANCZOS)
    master.save(os.path.join(ASSETS, "miderforge-256.png"))
    master.save(
        os.path.join(ASSETS, "miderforge.ico"),
        format="ICO",
        sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)],
    )
    print("icon written:", os.path.join(ASSETS, "miderforge.ico"))


if __name__ == "__main__":
    main()
