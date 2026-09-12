# Miderforge 应用图标生成器 v2:夜蓝圆角底 + 蜜金锻造锤 + 锻造火花 + 淡蓝蜂窝六边形
# 设计语言与 docs/assets/logo.svg(README 头图)完全同源,由本脚本以 Pillow 复刻:
#   - 底:夜蓝纵向渐变 + 锤身后的「炉光」暖色 radial + 四角暗角 + 顶部玻璃高光
#   - 血统:一枚点顶蜂窝六边形细描边(低透明度,呼应姊妹项目 MiderHive)
#   - 主体:对角 45° 锻造锤(头在左上、手柄扫向右下),头上一条机加工感高光细线
#   - 点睛:锤头右上飞出三颗渐远渐小的锻造火花
# 产物:resources/assets/miderforge.ico(16..256 多尺寸)、resources/assets/miderforge-256.png
#       (1024 源,qrc 引用名保持稳定)、resources/assets/icon-preview.png(深浅底预览)
# 用法:python resources/scripts/make_icon.py   (需 Pillow)
import math
import os

from PIL import Image, ImageDraw, ImageFilter

# 本文件位于 resources/scripts/,故资源目录是同级 resources/assets(不能再多退一层)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
os.makedirs(ASSETS, exist_ok=True)

S = 2048  # 超采样画布

# 配色(与 UI 深色主题同源;forge=蜜金、hive=淡蓝)
BG_TOP = (40, 48, 63)
BG_BOTTOM = (19, 24, 33)
HEX = (93, 153, 255, 56)           # 蜂窝描边:应用强调蓝,低透明度,血统暗号
AMBER_L = (255, 214, 130)          # 锤头亮部
AMBER_R = (240, 146, 8)            # 锤头暗部(比 v1 更深,拉开花面层次)
HANDLE_L = (244, 186, 86)
HANDLE_R = (188, 118, 22)
GRIP = (134, 72, 8)
SPARK_L = (255, 231, 194)          # 火花近端(白热)
SPARK_R = (255, 196, 87)           # 火花远端(橙)
SHADOW = (0, 0, 0, 84)


# ---------------------------------------------------------------- 渐变工具 ----
def h_gradient(size, left, right):
    """水平渐变图"""
    w, h = size
    strip = Image.new("RGB", (256, 1))
    for x in range(256):
        t = x / 255.0
        strip.putpixel((x, 0), tuple(int(l + (r - l) * t) for l, r in zip(left, right)))
    return strip.resize((w, h), Image.BILINEAR)


def v_gradient(size, top, bottom):
    w, h = size
    strip = Image.new("RGB", (1, 256))
    for y in range(256):
        t = y / 255.0
        strip.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return strip.resize((w, h), Image.BILINEAR)


def radial_overlay(size, center, r_inner, r_outer, color, a_inner, a_outer, steps=72):
    """径向透明叠层:color 在 a_inner..a_outer 间随半径线性衰减(负值方向亦可)"""
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
    """按圆角矩形掩膜把渐变贴上画布(尺寸不符时自动拉伸到 box)"""
    w, h = box[2] - box[0], box[3] - box[1]
    if grad.size != (w, h):
        grad = grad.resize((w, h), Image.BILINEAR)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w, h], radius=radius, fill=255)
    canvas.paste(grad, (box[0], box[1]), mask)


def hexagon(draw, center, radius, width, color):
    """点顶蜂窝六边形描边(圆角接头);与 MiderHive 品牌六边形同构"""
    cx, cy = center
    pts = [(cx + radius * math.cos(math.radians(60 * i - 90)),
            cy + radius * math.sin(math.radians(60 * i - 90))) for i in range(6)]
    draw.line(pts + [pts[0]], fill=color, width=width, joint="curve")


# ---------------------------------------------------------------- 锤子主体 ----
def hammer_boxes():
    """直立锤各部件(x0,y0,x1,y1):两端锤面、头杆、手柄、握把带
    v2 比例:锤面略收窄、头杆更修长、手柄减细,整体重心上移,留出火花空间"""
    return [
        (456, 396, 676, 908),    # 左锤面
        (1372, 396, 1592, 908),  # 右锤面
        (580, 464, 1468, 846),   # 头杆
        (944, 800, 1104, 1592),  # 手柄(比 v1 细,去掉粗笨感)
        (924, 1316, 1124, 1392), # 握把防滑带(收窄、低调)
    ]


def sparks():
    """锻造火花:从头右上面向外飞溅,(x, y, 半径, 透明度),渐远渐小渐淡"""
    return [
        (1690, 470, 40, 255),
        (1790, 356, 28, 225),
        (1868, 244, 18, 190),
        (1622, 566, 12, 150),
    ]


def hammer_layer(rotation):
    """直立锻造锤 + 投影 + 高光线 + 火花 → 旋转 rotation 度"""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # 投影层:同形状黑色剪影,偏移后重模糊(比 v1 更散,落地更稳)
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    for box in hammer_boxes():
        sd.rounded_rectangle([box[0] + 28, box[1] + 40, box[2] + 28, box[3] + 40],
                             radius=70, fill=SHADOW)
    shadow = shadow.filter(ImageFilter.GaussianBlur(56))
    layer.alpha_composite(shadow)

    caps_g = h_gradient((220, 512), AMBER_R, AMBER_L)
    bar_g = h_gradient((888, 382), AMBER_L, AMBER_R)
    handle_g = v_gradient((160, 792), HANDLE_L, HANDLE_R)
    grip_g = h_gradient((200, 76), GRIP, GRIP)
    for box, r, g in zip(hammer_boxes(), [84, 84, 88, 66, 38],
                         [caps_g, caps_g, bar_g, handle_g, grip_g]):
        rounded_gradient(layer, box, r, g)

    # 机加工感:头杆上缘一条细高光(白,低透明度),立刻「贵」起来
    spec = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(spec).rounded_rectangle([608, 498, 1444, 540], radius=21,
                                           fill=(255, 255, 255, 64))
    layer.alpha_composite(spec)

    # 火花:每颗先铺一圈柔光,再点实体(近端白热、远端偏橙)
    spark_soft = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    spd = ImageDraw.Draw(spark_soft)
    for x, y, r, a in sparks():
        spd.ellipse([x - r * 3, y - r * 3, x + r * 3, y + r * 3],
                    fill=SPARK_R + (int(a * 0.16),))
    layer.alpha_composite(spark_soft.filter(ImageFilter.GaussianBlur(26)))
    sd2 = ImageDraw.Draw(layer)
    for i, (x, y, r, a) in enumerate(sparks()):
        col = SPARK_L if i < 2 else SPARK_R
        sd2.ellipse([x - r, y - r, x + r, y + r], fill=col + (a,))

    return layer.rotate(rotation, resample=Image.BICUBIC, expand=True)


# ---------------------------------------------------------------- 合成 ----
def compose(rotation):
    canvas = v_gradient((S, S), BG_TOP, BG_BOTTOM).convert("RGBA")

    # 圆角方底掩膜(整图裁形)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([64, 64, S - 64, S - 64], radius=448, fill=255)

    # 炉光:锤身后一团暖色 radial,主体从底色里「浮」出来
    glow = radial_overlay((S, S), (S // 2, int(S * 0.46)), 80, 700, (245, 158, 11), 0, 62)
    canvas.alpha_composite(glow)

    # 蜂窝六边形描边(点顶、细线、低透明度;画在锤后形成层次)
    hexagon(ImageDraw.Draw(canvas), (S // 2, S // 2), 640, 20, HEX)

    layer = hammer_layer(rotation)
    canvas.alpha_composite(layer, (int((S - layer.width) / 2), int((S - layer.height) / 2)))

    # 顶部玻璃高光:一条竖向白→透明渐变压在上沿(克制,不能抢主体)
    glass = v_gradient((S, int(S * 0.48)), (255, 255, 255), (0, 0, 0)).convert("RGBA")
    glass.putalpha(glass.split()[0].point(lambda v: int(v * 0.10)))
    canvas.alpha_composite(glass)

    # 四角暗角:径向透明→深黑,视线收向中心
    vig = radial_overlay((S, S), (S // 2, S // 2), int(S * 0.30), int(S * 0.74),
                         (0, 0, 0), 0, 78, steps=96)
    canvas.alpha_composite(vig)

    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(canvas, (0, 0), mask)
    return out


def main():
    variants = {"A": compose(-45), "B": compose(45)}  # 两种对角朝向,预览挑选

    # 预览:两变体并排(深/浅两底检查边缘)
    pv = Image.new("RGB", (1024, 512), (240, 240, 240))
    for i, key in enumerate(("A", "B")):
        icon = variants[key].resize((512, 512), Image.LANCZOS)
        pv.paste(icon, (i * 512, 0), icon)
    pv.save(os.path.join(ASSETS, "icon-preview.png"))

    # 选定后由人工确认再落盘正式产物:默认写变体 B(头在左上,手柄扫向右下,阅读方向更顺)
    chosen = variants["B"]
    master = chosen.resize((1024, 1024), Image.LANCZOS)
    master.save(os.path.join(ASSETS, "miderforge-256.png"))  # 1024 源,qrc 引用名保持稳定
    master.save(
        os.path.join(ASSETS, "miderforge.ico"),
        format="ICO",
        sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)],
    )
    print("icon written:", os.path.join(ASSETS, "miderforge.ico"))


if __name__ == "__main__":
    main()
