#!/usr/bin/env python3
"""从 MAME snspell romset (Speak & Spell 1978, TMS5110/TMC0281) 提取完整词表。

生成 plugins/Narrator/src/dsp/tms/VocabSspell.h (orm::tms::sspell::kWords)。
ROM 索引结构 (逆向自 BrerDawg/ti_lpc 的 build_rom_word_addr_list, 无许可证;
数据本体为停产玩具的掩膜 ROM, 许可细节见 THIRD_PARTY_NOTICES.md):
  - vsm = rom0(0x0000-0x3FFF) + rom1(0x4000-0x7FFF) 拼接, 指针 >=0x4000 取第二盘
  - ROM 前 16 字节: 4 个词表 (第 k 项: vsm[k] = 词表字节数, 4+2k 起 2 字节 = 词表地址)
  - 词表项: 2 字节 = 词描述符地址 (word_ptr)
  - 描述符: 最多 8 字节文本, 每字节 (b&0x3f)+0x41 → 字母, 0x40 位 = 结束标记,
    其后 2 字节 = LPC 数据地址
  - 另有固定索引: 0x0c 起 26 字母, beep, 0-9 数字, "10", 若干短语, 4 个 tone
位序: 字节内 LSB-first (与 Tms5110Engine 的 BitReader 一致); 词数据 = 从 LPC 地址
起按帧扫描 (energy/repeat/pitch/k1..k10) 直到停止帧 (energy==0xF) 消耗的原始字节。
"""
import os
import re
import sys

ROM0 = "tmc0351n2l.vsm"
ROM1 = "tmc0352n2l.vsm"
MAX_TEXT = 8


def rev_bits(a):
    a = ((a & 0x55) << 1) | ((a & 0xAA) >> 1)
    a = ((a & 0x33) << 2) | ((a & 0xCC) >> 2)
    a = ((a << 4) | (a >> 4)) & 0xFF
    return a


class BitReader:
    def __init__(self, data):
        self.d = data
        self.pos = 0
        self.bit = 0

    def get(self, n):
        if self.pos >= len(self.d):
            return 0  # 越界按 0 填充, 令扫描自然结束
        w = rev_bits(self.d[self.pos]) << 8
        if self.pos + 1 < len(self.d) and self.bit + n > 8:
            w |= rev_bits(self.d[self.pos + 1])
        w = (w << self.bit) & 0xFFFF  # 与 Tms5110Engine 的 uint16_t 语义一致
        v = (w >> (16 - n)) & 0xFF
        self.bit += n
        if self.bit >= 8:
            self.bit -= 8
            self.pos += 1
        return v

    def used(self):
        return self.pos + (1 if self.bit > 0 else 0)


def u16(b, i):
    return b[i] | (b[i + 1] << 8)


def decode_text(rom, word_ptr):
    s = []
    for j in range(MAX_TEXT):
        b = rom[word_ptr + j]
        c = chr((b & 0x3F) + 0x41)
        if c == "[":  # 值 26 -> 撇号, 如 COULDN'T
            c = "'"
        s.append(c)
        if b & 0x40:  # 结束标记, 其后 2 字节是 LPC 地址
            lpc = u16(rom, word_ptr + j + 1)
            return "".join(s).strip(), lpc
    return None, None  # 超出 8 字节: 异常


def scan_word_bytes(rom, addr, max_frames=500, max_bytes=1500):
    """按帧扫描到停止帧, 返回消耗的原始字节 (talkie 约定位序)。"""
    if addr < 0 or addr >= len(rom):
        return b""
    br = BitReader(rom[addr:addr + max_bytes])
    for _ in range(max_frames):
        e = br.get(4)
        if e == 0xF:
            break
        if e == 0:
            continue
        rep = br.get(1)
        pitch = br.get(5)
        if rep:
            continue
        br.get(5)  # k1
        br.get(5)  # k2
        br.get(4)  # k3
        br.get(4)  # k4
        if pitch:
            br.get(4)
            br.get(4)
            br.get(4)  # k5..k7 (4 位)
            br.get(3)
            br.get(3)
            br.get(3)  # k8, k9, k10 (3 位)
    n = min(br.used(), max_bytes)
    return rom[addr:addr + n]


def parse_rom(rom):
    words = {}  # name -> (addr, bytes)

    def add(name, addr):
        if name is None or addr is None:
            return
        if addr < 0 or addr >= len(rom):
            return
        data = scan_word_bytes(rom, addr)
        if len(data) < 2:
            return
        words.setdefault(name, (addr, data))

    def ptr(pp):
        return u16(rom, pp)

    # 固定索引: 26 字母 (直接 LPC 地址)
    pp = 0x0C
    for i in range(26):
        add(chr(ord("A") + i), ptr(pp))
        pp += 2
    pp += 2  # beep: 跳过 (非语音)
    # 数字 0-9 + "10" (直接)
    for d in "0123456789":
        add(d, ptr(pp))
        pp += 2
    add("10", ptr(pp))
    pp += 2
    # 短语: 直接/间接地址按 ROM 中的交错顺序处理 (与 ti_lpc 一致)
    def phrase_group(direct, indirect):
        nonlocal pp
        for lab in direct:
            add(lab, ptr(pp))
            pp += 2
        for lab in indirect:
            p2 = ptr(pp)
            pp += 2
            add(lab, ptr(p2) if 0 <= p2 + 1 < len(rom) else None)

    phrase_group(["THAT_IS_CORRECT", "YOU_ARE_CORRECT", "THAT_IS_RIGHT", "YOU_ARE_RIGHT"], [])
    phrase_group([], ["WRONG", "THAT_IS_INCORRECT", "SPELL", "NOW_SPELL", "NEXT_SPELL",
                      "NOW_TRY", "TRY"])
    phrase_group(["SAY_IT", "I_WIN", "YOU_WIN"], [])
    phrase_group([], ["HERE_IS_YOUR_SCORE"])
    phrase_group(["PERFECT_SCORE"], [])
    # 4 个 tone: 跳过 (非语音)

    # 4 个词表 (vsm[k] = 词数, 每项 2 字节; 词表地址连续排列可验证)
    for k in range(4):
        word_cnt = rom[k]
        list_addr = ptr(4 + k * 2)
        for i in range(0, word_cnt * 2, 2):
            wa = list_addr + i
            if wa + 1 >= len(rom):
                break
            word_ptr = u16(rom, wa)
            if word_ptr + MAX_TEXT + 3 > len(rom):
                continue
            text, lpc = decode_text(rom, word_ptr)
            if text is None:
                continue
            add(text, lpc)
    return words


def header(words):
    items = sorted(words.items())  # 按名字排序
    lines = [
        "#pragma once",
        "",
        "// TMS5110 (TMC0281, 1978 Speak & Spell) 完整词表 — 自动生成 (tools/gen_sspell_vocab.py).",
        "// 提取自 MAME snspell romset (tmc0351n2l.vsm + tmc0352n2l.vsm, 停产玩具掩膜 ROM,",
        "// 许可细节见 THIRD_PARTY_NOTICES.md)。索引结构逆向自 BrerDawg/ti_lpc 工具,",
        "// 位序与参数表见 Tms5110Engine.cpp。",
        "",
        '#include "TmsWords.h"',
        "",
        "namespace orm::tms::sspell {",
        "",
    ]
    for name, (addr, data) in items:
        arr = ", ".join("0x%02X" % v for v in data)
        var = re.sub(r"[^A-Za-z0-9_]", "_", name)
        lines.append(f"inline const unsigned char kVocab_{var}[] = {{{arr}}};")
    lines.append("")
    lines.append("inline const Word kWords[] = {")
    for name, (addr, data) in items:
        var = re.sub(r"[^A-Za-z0-9_]", "_", name)
        lines.append(f'  {{"{name}", kVocab_{var}, (int)sizeof(kVocab_{var})}},')
    lines.append("};")
    lines.append(f"inline const int kNumWords = {len(items)};")
    lines.append("")
    lines.append("} // namespace orm::tms::sspell")
    lines.append("")
    return "\n".join(lines)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "assets/snsrom"
    out = sys.argv[2] if len(sys.argv) > 2 else "plugins/Narrator/src/dsp/tms/VocabSspell.h"
    rom = b"".join(open(os.path.join(root, f), "rb").read() for f in (ROM0, ROM1))
    words = parse_rom(rom)
    if not words:
        print("ERROR: no words extracted", file=sys.stderr)
        sys.exit(1)
    # 校验: 与 ti_lpc 样例词逐字节比对 (ISLE/COLOR)
    sample = {
        "ISLE": "45,AB,36,AE,D5,56,A7,3E,CA,D4,2A,EE,96,73,D5,55,57,5F,73,9C,6B,91,1E,27,FB,04,9F,34,A3,C6,CE,89,29,9A,A5,5F,EC,13,73,72,0D,CF,27,37,DE,7E,46,32,19,29,FA,FA,8C,20,B2,9A,7D,F3,9A,89,7B,8F,70,EF,36,13,F3,39,A5,DE,69,46,1A,3B,82,BB,F3,AC,73,CC,40,A2,43,44,4A,9F,76,3E,00,00,95",
        "COLOR": "01,B8,33,96,80,CF,5B,11,2C,E1,F3,56,AA,2B,39,42,A6,4A,B7,94,7D,84,CA,39,54,5D,E7,CA,A5,64,AF,A2,EC,34,C3,4A,57,2B,DC,71,47,54,36,C7,A0,6A,9F,AC,6A,99,E6,C4,3A,C5,F8,36,A9,6A,78,BA,B5,65,D2,95,F1,F6,31,DC,15,5D,C9,45,73,EC,39,67,5F,7E,E9,F5,88,12,0B,44,B5,19",
    }
    ok = True
    for name, hexstr in sample.items():
        got = words.get(name)
        want = [int(v, 16) for v in hexstr.split(",")]
        if not got:
            print(f"  WARN: sample word {name} not found in ROM")
            ok = False
        elif not want[: len(got[1])] == list(got[1]):
            # 提取止于停止帧; 样例可能多带几字节尾部填充
            print(f"  MISMATCH {name}: extracted {len(got[1])}B not prefix of sample {len(want)}B")
            ok = False
        else:
            print(f"  OK: {name} = sample prefix (extracted {len(got[1])}B, sample {len(want)}B)")
    with open(out, "w") as f:
        f.write(header(words))
    print(f"Wrote {len(words)} words -> {out}  (sample-check {'PASS' if ok else 'SEE ABOVE'})")


if __name__ == "__main__":
    main()