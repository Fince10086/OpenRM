#!/usr/bin/env python3
"""生成 OpenRM 系列插件统一 App 图标（macOS 标准圆角矩形 + 白底黑字品牌名）。

用途:
  本脚本为 Analyzer 插件生成 ORMAnalyzer.icns (macOS) 与 ORMAnalyzer.ico (Windows)。
  做下一个插件时, 复制本脚本并修改下方 PLUGIN 段 (名字/输出文件名) 即可复用。

特性:
  - 主图: 1024x1024, macOS Big Sur 规范圆角矩形 (radius 185), 白底 #FFFFFF, 黑字 #000000
  - 文字: 插件粗体字体 (Mixed-Bold.ttf, Outfit Bold 子集), 两行品牌名 (ORM / Analyzer),
          两行同字号粗体、左对齐、整体垂直居中; 水平以最宽一行 (Analyzer) 为基准左右对称
  - 输出: {ICON_NAME}.icns (iconutil) 与 {ICON_NAME}.ico (Win 多尺寸)

用法: python make_icon_analyzer.py <resources_dir> <bold_ttf> <out_png_dir>
示例: python make_icon_analyzer.py plugins/Analyzer/resources \
        plugins/Analyzer/resources/fonts/Mixed-Bold.ttf /tmp/icon_out
"""

import sys
from PIL import Image, ImageDraw, ImageFont

# ---- PLUGIN 段: 做新插件时改这里 ----
ICON_NAME = "ORMAnalyzer"    # 输出文件名 (icns/ico 均用此名, 与 iPlug2 自动发现规则一致)
TEXT_TOP = "ORM"             # 第一行
TEXT_BOTTOM = "Analyzer"     # 第二行
# -----------------------------------

S = 1024
CONTENT = 832          # macOS 规范: 设计区 832x832 居中, 外边距 96px
OFFSET = (S - CONTENT) // 2   # 96
RADIUS = int(0.22 * CONTENT)  # 圆角半径 = 832 的 22% ≈ 183 (Apple Design Resources)
BG = (255, 255, 255)   # 白底
FG = (0, 0, 0)         # 黑字
LEFT = 93              # 左对齐起点（最终以最宽一行水平居中, 该值仅决定行内布局; =115×832/1024）
GAP = 28               # 两行间距 (=34×832/1024)
TEXT_SIZE = 146        # 两行统一字号, 粗体 (=180×832/1024 等比例缩小)

# Apple 风格图标阴影 (可调): 黑色圆角矩形模糊后整体下移, 本体覆盖其上
# 若需要阴影, 取消 render_1024 中阴影段落的注释并调整以下参数
SHADOW_ALPHA = 90      # 阴影不透明度 (0-255, 90 ≈ 35%)
SHADOW_BLUR = 100      # 高斯模糊半径 (px)
SHADOW_OFFSET_Y = 36   # 阴影向下偏移 (px)
SHADOW_ENABLED = False # 当前版本不启用阴影


def render_1024(font_path):
    font_top = ImageFont.truetype(font_path, TEXT_SIZE)
    font_bot = ImageFont.truetype(font_path, TEXT_SIZE)

    # 校验最宽行不超出设计区右缘
    bb = font_bot.getbbox(TEXT_BOTTOM)
    if LEFT + bb[2] > S - 80:
        raise SystemExit(f"{TEXT_BOTTOM} too wide: right={LEFT + bb[2]}")

    # 1. 本体: 白底圆角矩形 (832 居中) 画在透明画布上
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([OFFSET, OFFSET, OFFSET + CONTENT, OFFSET + CONTENT],
                        radius=RADIUS, fill=BG)

    if SHADOW_ENABLED:
        from PIL import ImageFilter
        # 阴影层: 黑色圆角矩形 (832 居中) 向下偏移后高斯模糊, 本体覆盖其上
        shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        sd.rounded_rectangle([OFFSET, OFFSET + SHADOW_OFFSET_Y,
                              OFFSET + CONTENT, OFFSET + CONTENT + SHADOW_OFFSET_Y],
                             radius=RADIUS, fill=(0, 0, 0, SHADOW_ALPHA))
        shadow = shadow.filter(ImageFilter.GaussianBlur(SHADOW_BLUR))
        img.alpha_composite(shadow)

    # 3. 在临时透明画布上画两行文字, 用 getbbox 拿真实像素包围盒
    #    (字体 getbbox 含 line gap, 不可直接用于居中, 用像素 bbox 最稳)
    text_layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    td = ImageDraw.Draw(text_layer)
    td.text((LEFT, 0),               TEXT_TOP,    font=font_top, fill=FG)
    td.text((LEFT, TEXT_SIZE + GAP), TEXT_BOTTOM, font=font_bot, fill=FG)
    tb = text_layer.getbbox()
    text_h = tb[3] - tb[1]
    text_w = tb[2] - tb[0]
    # 垂直居中
    paste_y = int(round((S - text_h) / 2 - tb[1]))
    # 水平居中: 以整段文字 (即最宽行) 的左右边界为基准对称留白
    paste_x = int(round((S - text_w) / 2 - tb[0]))

    # 4. 把文字层 alpha_composite 到主图, 整段文字精确居中
    img.alpha_composite(text_layer, (paste_x, paste_y))
    return img


def make_iconset(img, out_dir):
    import os
    names = {
        "icon_16x16.png": 16, "icon_16x16@2x.png": 32,
        "icon_32x32.png": 32, "icon_32x32@2x.png": 64,
        "icon_128x128.png": 128, "icon_128x128@2x.png": 256,
        "icon_256x256.png": 256, "icon_256x256@2x.png": 512,
        "icon_512x512.png": 512, "icon_512x512@2x.png": 1024,
    }
    os.makedirs(out_dir, exist_ok=True)
    for name, px in names.items():
        img.resize((px, px), Image.LANCZOS).save(os.path.join(out_dir, name))
    return out_dir


def make_ico(img, path):
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(path, format="ICO", sizes=sizes)


def main():
    res_dir, font_path, png_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    img = render_1024(font_path)
    img.save(f"{png_dir}/icon_1024.png")
    make_iconset(img, f"{png_dir}/icon.iconset")
    make_ico(img, f"{res_dir}/{ICON_NAME}.ico")
    print(f"icon_1024.png + iconset + {ICON_NAME}.ico done")


if __name__ == "__main__":
    main()
