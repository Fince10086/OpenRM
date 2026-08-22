#!/usr/bin/env python3
# 将 OPPO Sans 可变字体按插件实际用到的字符子集化, 并实例化出三个字重
# 用法: subset_font.py <source.ttf> <Strings.h> [<更多文本文件>...] <输出目录>
import io
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

os.environ["SOURCE_DATE_EPOCH"] = "0"

WEIGHTS = [("Regular", 400), ("SemiBold", 600), ("Bold", 700)]


def collect_charset(paths):
    chars = {chr(c) for c in range(0x20, 0x7F)}
    for p in paths:
        with open(p, encoding="utf-8") as f:
            for ch in f.read():
                if ord(ch) > 0x7F:
                    chars.add(ch)
    return chars


def build_weight(source, wght, charset):
    font = TTFont(source)
    if "fvar" in font:
        instantiateVariableFont(font, {"wght": wght}, inplace=True)

    cmap = font.getBestCmap()
    missing = [ord(c) for c in sorted(charset) if ord(c) not in cmap]
    if missing:
        detail = " ".join(f"U+{cp:04X}" for cp in missing[:20])
        print(f"WARNING: wght={wght} 缺少 {len(missing)} 个字符: {detail}", file=sys.stderr)

    opts = subset.Options()
    opts.layout_features = ["*"]
    sub = subset.Subsetter(options=opts)
    sub.populate(text="".join(sorted(charset)))
    sub.subset(font)

    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


def write_if_changed(path, data):
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    with open(path, "wb") as f:
        f.write(data)
    return True


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    source = sys.argv[1]
    text_files = sys.argv[2:-1]
    out_dir = sys.argv[-1]
    os.makedirs(out_dir, exist_ok=True)

    charset = collect_charset(text_files)
    print(f"subset_font.py: {len(charset)} 个字符 (来自 {', '.join(os.path.basename(p) for p in text_files)})")

    for suffix, wght in WEIGHTS:
        data = build_weight(source, wght, charset)
        out = os.path.join(out_dir, f"OPPOSans-ZH-{suffix}.ttf")
        changed = write_if_changed(out, data)
        state = "生成" if changed else "未变"
        print(f"  OPPOSans-ZH-{suffix}.ttf (wght={wght}): {len(data) / 1024:.1f} KB [{state}]")


if __name__ == "__main__":
    main()
