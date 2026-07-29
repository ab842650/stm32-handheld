#!/usr/bin/env python3
"""
8x16 bitmap font generator for ILI9341.

讀一個 TrueType 等寬字型，把 ASCII 0x20..0x7E 逐字渲染成 8x16 點陣，
打包成 const uint8_t font8x16[95][16]，輸出 font.c / font.h。

打包規則：
  - 每個字 = 16 個 byte，byte[row] 代表第 row 列（0=最上）
  - 每個 byte 的 bit7 = 最左像素，bit0 = 最右像素（MSB first）
  - 位元 = 1 代表亮點（前景），= 0 代表暗點（背景）

用法：
  python genfont.py              # 只印預覽，不寫檔
  python genfont.py --write      # 產生 ../Core/Src/font.c 與 ../Core/Inc/font.h
"""
import argparse
import os
from PIL import Image, ImageFont, ImageDraw

# ── 可調參數 ──────────────────────────────────────────────
FONT_PATH = r"C:\Windows\Fonts\consola.ttf"
CELL_W    = 8     # 每個字寬（像素／bit 數）
CELL_H    = 16    # 每個字高（byte 數）
PT_SIZE   = 15    # 字型 point size（決定字形佔多滿；14~16 之間微調）
X_OFF     = 0     # 水平微調（正=往右）
Y_OFF     = -1    # 垂直微調（正=往下）
THRESHOLD = 128   # 二值化門檻（灰階 > 門檻算亮點）
FIRST_CH  = 0x20
LAST_CH   = 0x7E
# ──────────────────────────────────────────────────────────

N = LAST_CH - FIRST_CH + 1


def render_char(font, ch):
    """把一個字元渲染成 CELL_H 個 byte。"""
    img = Image.new("L", (CELL_W, CELL_H), 0)          # 黑底
    draw = ImageDraw.Draw(img)
    draw.text((X_OFF, Y_OFF), ch, fill=255, font=font) # 白字
    rows = []
    px = img.load()
    for y in range(CELL_H):
        b = 0
        for x in range(CELL_W):
            if px[x, y] > THRESHOLD:
                b |= (1 << (7 - x))                    # bit7 = 最左
        rows.append(b)
    return rows


def preview(glyph, ch):
    print(f"'{ch}' (0x{ord(ch):02X})")
    for b in glyph:
        line = "".join("#" if (b >> (7 - x)) & 1 else "." for x in range(CELL_W))
        print("  " + line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="輸出 font.c / font.h")
    ap.add_argument("--all", action="store_true", help="預覽全部 95 個字")
    args = ap.parse_args()

    font = ImageFont.truetype(FONT_PATH, PT_SIZE)
    glyphs = [render_char(font, chr(c)) for c in range(FIRST_CH, LAST_CH + 1)]

    # 預覽：預設挑幾個代表字，--all 則全部
    sample = "AQ0g8@#Wgjyi" if not args.all else "".join(chr(c) for c in range(FIRST_CH, LAST_CH + 1))
    for ch in sample:
        preview(glyphs[ord(ch) - FIRST_CH], ch)

    if not args.write:
        print("\n(預覽模式，未寫檔。字形 OK 後加 --write 正式產生。)")
        return

    here = os.path.dirname(os.path.abspath(__file__))
    inc  = os.path.normpath(os.path.join(here, "..", "Core", "Inc", "font.h"))
    src  = os.path.normpath(os.path.join(here, "..", "Core", "Src", "font.c"))

    with open(inc, "w", encoding="utf-8") as f:
        f.write(
            "#ifndef FONT_H\n#define FONT_H\n\n#include <stdint.h>\n\n"
            f"/* 8x16 ASCII bitmap font, chars 0x{FIRST_CH:02X}..0x{LAST_CH:02X} */\n"
            f"#define FONT_WIDTH   {CELL_W}\n"
            f"#define FONT_HEIGHT  {CELL_H}\n"
            f"#define FONT_FIRST   0x{FIRST_CH:02X}\n"
            f"#define FONT_LAST    0x{LAST_CH:02X}\n\n"
            f"extern const uint8_t font8x16[{N}][{CELL_H}];\n\n"
            "#endif /* FONT_H */\n"
        )

    with open(src, "w", encoding="utf-8") as f:
        f.write('#include "font.h"\n\n')
        f.write(f"const uint8_t font8x16[{N}][{CELL_H}] = {{\n")
        for c in range(FIRST_CH, LAST_CH + 1):
            g = glyphs[c - FIRST_CH]
            bytes_ = ", ".join(f"0x{b:02X}" for b in g)
            f.write(f"    {{ {bytes_} }}, /* 0x{c:02X} '{chr(c)}' */\n")
        f.write("};\n")

    print(f"\n寫入完成：\n  {inc}\n  {src}")


if __name__ == "__main__":
    main()
