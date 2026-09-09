# Miderforge 应用图标生成器：深色圆角底 + 琥珀锻造锤 + 蜂窝六边形描边（呼应 AgentHive）
# 产物：assets/miderforge.ico（16..256 多尺寸）、assets/miderforge-256.png（窗口图标）、预览图
# 用法：python scripts/make_icon.py   （需 Pillow）
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
os.makedirs(ASSETS, exist_ok=True)

S = 2048  # 超采样画布

# 配色（与 UI 深色主题同源，forge=琥珀、hive=蓝）
BG_TOP = (38, 45, 58)
BG_BOTTOM = (21, 26, 35)
HEX = (93, 153, 255, 70)          # 蜂窝描边：应用强调蓝，低透明度
AMBER_L = (255, 211, 122)         # 锤头亮部
AMBER_R = (245, 158, 11)          # 锤头暗部
HANDLE_L = (243, 181, 74)
HANDLE_R = (196, 124, 26)
GRIP = (138, 75, 8)
SHADOW = (0, 0, 0, 90)


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


def rounded_gradient(canvas, box, radius, grad):
    """按圆角矩形掩膜把渐变贴上画布（尺寸不符时自动拉伸到 box）"""
    w, h = box[2] - box[0], box[3] - box[1]
    if grad.size != (w, h):
        grad = grad.resize((w, h), Image.BILINEAR)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w, h], radius=radius, fill=255)
    canvas.paste(grad, (box[0], box[1]), mask)


def hexagon(draw, center, radius, width, color):
    """平顶蜂窝六边形描边（圆角接头）"""
    cx, cy = center
    pts = [(cx + radius * math.cos(math.radians(60 * i - 30)),
            cy + radius * math.sin(math.radians(60 * i - 30))) for i in range(6)]
    draw.line(pts + [pts[0]], fill=color, width=width, joint="curve")


def hammer_layer(rotation):
    """直立锻造锤（木槌式：头杆 + 两端加粗锤面，手柄在下）→ 旋转 rotation 度"""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # 投影层：同形状黑色剪影，偏移后模糊
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    for box in hammer_boxes():
        sd.rounded_rectangle([box[0] + 26, box[1] + 36, box[2] + 26, box[3] + 36], radius=70, fill=SHADOW)
    shadow = shadow.filter(ImageFilter.GaussianBlur(30))
    layer.alpha_composite(shadow)

    caps_g = h_gradient((210, 400), AMBER_R, AMBER_L)
    bar_g = h_gradient((928, 290), AMBER_L, AMBER_R)
    neck_g = h_gradient((112, 110), AMBER_R, AMBER_R)
    handle_g = v_gradient((148, 720), HANDLE_L, HANDLE_R)
    grip_g = h_gradient((204, 90), GRIP, GRIP)
    for box, r, g in zip(hammer_boxes(), [80, 80, 60, 40, 72, 44], [caps_g, caps_g, bar_g, neck_g, handle_g, grip_g]):
        rounded_gradient(layer, box, r, g)

    return layer.rotate(rotation, resample=Image.BICUBIC, expand=True)


def hammer_boxes():
    """直立锤各部件（x0,y0,x1,y1）：左右锤面、头杆、颈、手柄、握把带"""
    return [
        (430, 430, 640, 820),    # 左锤面（比头杆高，形成木槌轮廓）
        (1408, 430, 1618, 820),  # 右锤面
        (560, 490, 1488, 780),   # 头杆
        (968, 760, 1080, 880),   # 颈
        (950, 860, 1098, 1580),  # 手柄
        (922, 1360, 1126, 1450), # 握把防滑带
    ]


def compose(rotation):
    canvas = v_gradient((S, S), BG_TOP, BG_BOTTOM).convert("RGBA")

    # 圆角方底掩膜（整图裁形）
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([28, 28, S - 28, S - 28], radius=460, fill=255)

    # 蜂窝六边形描边（画在底上，被锤子部分遮挡形成层次）
    hexagon(ImageDraw.Draw(canvas), (S // 2, S // 2), 660, 26, HEX)

    layer = hammer_layer(rotation)
    canvas.alpha_composite(layer, (int((S - layer.width) / 2), int((S - layer.height) / 2)))

    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(canvas, (0, 0), mask)
    return out


def main():
    variants = {"A": compose(-45), "B": compose(45)}  # 两种对角朝向，预览挑选

    # 预览：两变体并排（深/浅两底检查边缘）
    pv = Image.new("RGB", (1024, 512), (240, 240, 240))
    for i, key in enumerate(("A", "B")):
        icon = variants[key].resize((512, 512), Image.LANCZOS)
        pv.paste(icon, (i * 512, 0), icon)
    pv.save(os.path.join(ASSETS, "icon-preview.png"))

    # 选定后由人工确认再落盘正式产物：默认写变体 B（头在左上，手柄扫向右下，阅读方向更顺）
    chosen = variants["B"]
    master = chosen.resize((1024, 1024), Image.LANCZOS)
    master.save(os.path.join(ASSETS, "miderforge-256.png"))  # 1024 源，qrc 引用名保持稳定
    master.save(
        os.path.join(ASSETS, "miderforge.ico"),
        format="ICO",
        sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)],
    )
    print("icon written:", os.path.join(ASSETS, "miderforge.ico"))


if __name__ == "__main__":
    main()
