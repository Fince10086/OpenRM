#!/usr/bin/env python3
"""生成 ORM Narrator 的 TMS 词表头文件。

从 ArminJo/Talkie 的某套词表源 .cpp 中提取每个词的 LPC 位流字节数组，
生成一个自包含头文件，词条以 `orm::tms::<bank>` 命名空间内的
`kWords[]` / `kNumWords` 暴露，供 Tms5220Engine 按音色查询。

用法:
    python3 tools/gen_tms_vocab.py <talkie_vocab.cpp> <bank> <out.h>

词条名 = 源变量名去掉 ROM 前缀（如 sp2_/spt_/spa_/spc_ 之后的第一个下划线）。
Talkie 源数据所有词表都用同一套 TMS5220 帧格式, 因此同一解码器可播。
"""
import re
import sys

ARRAY_RE = re.compile(
    r"extern\s+const\s+uint8_t\s+(\w+)\[\]\s+PROGMEM\s*=\s*\{(?P<body>[^}]*)\};",
    re.DOTALL,
)
BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{1,2})\b|([0-9A-Fa-f]{1,2})\b")


def parse_cpp(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()
    words = []  # (var_name, [bytes])
    for m in ARRAY_RE.finditer(src):
        var = m.group(1)
        body = m.group("body")
        vals = []
        for bm in BYTE_RE.finditer(body):
            tok = bm.group(1) or bm.group(2)
            vals.append(int(tok, 16))
        if vals:
            words.append((var, vals))
    return words


def word_name(var):
    # 去掉 ROM 前缀（第一个下划线及之前的部分）
    i = var.find("_")
    return var[i + 1 :] if i >= 0 else var


def build_header(entries, bank):
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// TMS5220 LPC 位流词汇表 (自动生成, 勿手改).")
    lines.append("// 数据来源: ArminJo/Talkie (GPL) — 本插件以 GPL 分发, 许可兼容.")
    lines.append("// 帧格式与 Tms5220Engine 的 BitReader 解码一致.")
    lines.append('')
    lines.append('#include "TmsWords.h"')
    lines.append("")
    lines.append(f"namespace orm::tms::{bank} {{")
    lines.append("")
    for i, (var, vals) in enumerate(entries):
        arr = ", ".join(f"0x{v:02X}" for v in vals)
        lines.append(
            f"inline const unsigned char kVocab_{var}[] = {{{arr}}};"
        )
    lines.append("")
    lines.append("inline const Word kWords[] = {")
    for var, vals in entries:
        name = word_name(var)
        lines.append(
            f'  {{"{name}", kVocab_{var}, (int)sizeof(kVocab_{var})}},'
        )
    lines.append("};")
    lines.append(f"inline const int kNumWords = {len(entries)};")
    lines.append("")
    lines.append(f"}} // namespace orm::tms::{bank}")
    lines.append("")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    src_path, bank, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    entries = parse_cpp(src_path)
    if not entries:
        print("ERROR: no words parsed from %s" % src_path, file=sys.stderr)
        sys.exit(1)
    header = build_header(entries, bank)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header)
    print(f"OK: {len(entries)} words -> {out_path} (namespace orm::tms::{bank})")


if __name__ == "__main__":
    main()
