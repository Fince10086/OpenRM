#!/usr/bin/env python3
# 将 OPPO Sans(可变字体) 按插件用到的中文字符子集化并实例化字重,
# 再与对应字重的 Outfit 合成 Mixed 字体: ASCII 用 Outfit 字形, 中文用 OPPO 字形
# 用法: subset_font.py <OPPO源字体> <Outfit目录> <Strings.h> <输出目录>
import io
import os
import sys
import tempfile

os.environ["SOURCE_DATE_EPOCH"] = "0"

from fontTools import subset
from fontTools.merge import Merger
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

WEIGHTS = [
    ("Regular", 400, "Outfit-Regular.ttf"),
    ("SemiBold", 600, "Outfit-SemiBold.ttf"),
    ("Bold", 700, "Outfit-Bold.ttf"),
]


def collect_charset(paths):
    chars = set()
    for p in paths:
        with open(p, encoding="utf-8") as f:
            for ch in f.read():
                if ord(ch) > 0x7F:
                    chars.add(ch)
    return chars


def build_cjk_part(source, wght, charset):
    font = TTFont(source)
    instantiateVariableFont(font, {"wght": wght}, inplace=True)

    cmap = font.getBestCmap()
    missing = [ord(c) for c in sorted(charset) if ord(c) not in cmap]
    if missing:
        detail = " ".join(f"U+{cp:04X}" for cp in missing[:20])
        print(f"WARNING: wght={wght} 缺少 {len(missing)} 个字符: {detail}", file=sys.stderr)

    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.drop_tables += ["STAT", "vhea", "vmtx"]
    sub = subset.Subsetter(options=opts)
    sub.populate(text="".join(sorted(charset)))
    sub.subset(font)
    return font


def write_if_changed(path, data):
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    with open(path, "wb") as f:
        f.write(data)
    return True


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    source, outfit_dir, strings_file, out_dir = sys.argv[1:5]
    os.makedirs(out_dir, exist_ok=True)

    charset = collect_charset([strings_file])
    print(f"subset_font.py: 中文字符 {len(charset)} 个 (来自 {os.path.basename(strings_file)})")

    for suffix, wght, outfit_name in WEIGHTS:
        cjk = build_cjk_part(source, wght, charset)
        # 临时文件放系统临时目录: 若放进 out_dir, 构建时删除会触发沙箱的
        # 批量删除保护导致 make 失败; 系统临时目录不受影响。
        fd, tmp_path = tempfile.mkstemp(suffix=".ttf")
        os.close(fd)
        cjk.save(tmp_path)
        merged = Merger().merge([os.path.join(outfit_dir, outfit_name), tmp_path])
        os.remove(tmp_path)
        buf = io.BytesIO()
        merged.save(buf)
        changed = write_if_changed(os.path.join(out_dir, f"Mixed-{suffix}.ttf"), buf.getvalue())
        print(f"  Mixed-{suffix}.ttf ({outfit_name} + OPPO wght={wght}): "
              f"{len(buf.getvalue()) / 1024:.1f} KB [{'生成' if changed else '未变'}]")


if __name__ == "__main__":
    main()
