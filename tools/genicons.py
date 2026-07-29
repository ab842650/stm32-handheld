#!/usr/bin/env python3
"""
16x16 單色圖示產生器。

在下面用 ASCII art 設計圖示（'#' = 亮點 / 1，'.' = 暗點 / 0），
轉成 C 陣列輸出 icons.c / icons.h。

打包規則（與字體 font8x16 相同）：
  - 每列 2 個 byte（16 bits），bit7 of byte0 = 最左像素
  - 共 16 列 → 每個圖示 32 bytes

用法：
  python genicons.py            # 只印預覽
  python genicons.py --write    # 產生 ../Core/Inc/icons.h 與 ../Core/Src/icons.c
"""
import argparse
import os

SIZE = 16

ICONS = {
    # 計算機：外框 + 顯示幕 + 按鍵
    "calc": [
        "................",
        "..############..",
        "..#..........#..",
        "..#.########.#..",
        "..#.#......#.#..",
        "..#.########.#..",
        "..#..........#..",
        "..#.##.##.##.#..",
        "..#..........#..",
        "..#.##.##.##.#..",
        "..#..........#..",
        "..#.##.##.##.#..",
        "..#..........#..",
        "..############..",
        "................",
        "................",
    ],
    # 遊戲：經典小外星人
    "game": [
        "................",
        "................",
        "................",
        "....#.....#.....",
        ".....#...#......",
        "....#######.....",
        "...##.###.##....",
        "..###########...",
        "..#.#######.#...",
        "..#.#.....#.#...",
        ".....##.##......",
        "................",
        "................",
        "................",
        "................",
        "................",
    ],
    # 記事本：文件 + 文字橫線
    "notes": [
        "................",
        "..##########....",
        "..#........#....",
        "..#.######.#....",
        "..#........#....",
        "..#.######.#....",
        "..#........#....",
        "..#.######.#....",
        "..#........#....",
        "..#.####...#....",
        "..#........#....",
        "..##########....",
        "................",
        "................",
        "................",
        "................",
    ],
    # 時鐘：圓框 + 指針（12 點 + 3 點方向）
    "clock": [
        "................",
        ".....######.....",
        "...##......##...",
        "..#....##....#..",
        ".#.....##.....#.",
        ".#.....##.....#.",
        "#......##......#",
        "#......#####...#",
        "#..............#",
        ".#............#.",
        ".#............#.",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "................",
        "................",
    ],
    # 相片：相框 + 太陽 + 山
    "photo": [
        "................",
        "..############..",
        "..#..........#..",
        "..#..##......#..",
        "..#..##......#..",
        "..#..........#..",
        "..#.....##...#..",
        "..#....####..#..",
        "..#...######.#..",
        "..#..#########..",
        "..############..",
        "................",
        "................",
        "................",
        "................",
        "................",
    ],
}


def validate(name, art):
    if len(art) != SIZE:
        raise ValueError(f"{name}: 需要 {SIZE} 列，實際 {len(art)}")
    for i, row in enumerate(art):
        if len(row) != SIZE:
            raise ValueError(f"{name} 第 {i} 列：需要 {SIZE} 字元，實際 {len(row)}")


def pack(art):
    """每列 16 bits → 2 bytes（MSB 在左）。"""
    out = []
    for row in art:
        hi = lo = 0
        for x in range(8):
            if row[x] == "#":
                hi |= 1 << (7 - x)
        for x in range(8, 16):
            if row[x] == "#":
                lo |= 1 << (7 - (x - 8))
        out += [hi, lo]
    return out


def preview(name, art):
    print(f"--- {name} ---")
    for row in art:
        print("  " + row.replace("#", "██").replace(".", "  "))
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    for name, art in ICONS.items():
        validate(name, art)
        preview(name, art)

    if not args.write:
        print("(預覽模式，未寫檔。加 --write 產生 icons.c / icons.h)")
        return

    here = os.path.dirname(os.path.abspath(__file__))
    inc = os.path.normpath(os.path.join(here, "..", "Core", "Inc", "icons.h"))
    src = os.path.normpath(os.path.join(here, "..", "Core", "Src", "icons.c"))

    with open(inc, "w", encoding="utf-8") as f:
        f.write("#ifndef ICONS_H\n#define ICONS_H\n\n#include <stdint.h>\n\n")
        f.write(f"/* 16x16 單色圖示，每列 2 bytes（MSB 在左），共 {SIZE} 列 = 32 bytes */\n")
        f.write(f"#define ICON_SIZE  {SIZE}\n\n")
        for name in ICONS:
            f.write(f"extern const uint8_t icon_{name}[{SIZE * 2}];\n")
        f.write("\n#endif /* ICONS_H */\n")

    with open(src, "w", encoding="utf-8") as f:
        f.write('#include "icons.h"\n\n')
        for name, art in ICONS.items():
            data = pack(art)
            f.write(f"const uint8_t icon_{name}[{SIZE * 2}] = {{\n")
            for r in range(SIZE):
                b0, b1 = data[r * 2], data[r * 2 + 1]
                f.write(f"    0x{b0:02X}, 0x{b1:02X},   /* {art[r]} */\n")
            f.write("};\n\n")

    print(f"寫入完成：\n  {inc}\n  {src}")


if __name__ == "__main__":
    main()
