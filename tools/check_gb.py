#!/usr/bin/env python3
"""
檢查 GB ROM，判斷這台（DMG 模擬器 + MBC 串流）能不能跑。

看三件事：
  - 卡匣類型 (0x147)：MBC1/2/3/5 都支援（有串流快取）
  - ROM 大小 (0x148)
  - GBC 旗標 (0x143)：0xC0=GBC 專屬（跑不了）、0x80=GBC 但相容 DMG（跑灰階）、其他=純 DMG

用法：
  python check_gb.py 某個.gb
  python check_gb.py *.gb          # 批次檢查一整包
"""
import sys, glob
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows cp950 相容
except Exception:
    pass

CART_TYPE = {
    0x00: "ROM ONLY", 0x01: "MBC1", 0x02: "MBC1+RAM", 0x03: "MBC1+RAM+BATT",
    0x05: "MBC2", 0x06: "MBC2+BATT",
    0x0F: "MBC3+RTC", 0x10: "MBC3+RTC+RAM", 0x11: "MBC3",
    0x12: "MBC3+RAM", 0x13: "MBC3+RAM+BATT",
    0x19: "MBC5", 0x1A: "MBC5+RAM", 0x1B: "MBC5+RAM+BATT",
    0x1C: "MBC5+RUMBLE", 0x1D: "MBC5+RUMBLE+RAM", 0x1E: "MBC5+RUMBLE+RAM+BATT",
}
ROM_SIZE = {0x00: "32KB", 0x01: "64KB", 0x02: "128KB", 0x03: "256KB",
            0x04: "512KB", 0x05: "1MB", 0x06: "2MB", 0x07: "4MB", 0x08: "8MB"}

def cgb_flag(b):
    if b == 0xC0: return "GBC-only"    # 需要 Game Boy Color
    if b & 0x80:  return "GBC/DMG"     # GBC 遊戲但相容原版 GB
    return "DMG"                        # 純原版 GB

def check(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x150:
        print("%-24s  ✗ 太小，不是有效 GB ROM" % path); return
    title = bytes(b if 0x20 <= b < 0x7F else 0x20 for b in data[0x134:0x143]).decode().strip()
    ctype = data[0x147]
    csize = data[0x148]
    gbc   = cgb_flag(data[0x143])

    if data[0x143] == 0xC0:
        verdict = "❌ GBC 專屬，跑不了"
    elif ctype not in CART_TYPE:
        verdict = "❌ MBC 不支援"
    elif gbc == "GBC/DMG":
        verdict = "✅ 可跑（灰階）"
    else:
        verdict = "✅ 可跑"

    print("%-22s %-14s %-9s %-16s %-6s %s" % (
        path, title[:14], gbc, CART_TYPE.get(ctype, "type=0x%02X" % ctype),
        ROM_SIZE.get(csize, "?"), verdict))

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return
    files = []
    for a in args:
        files += glob.glob(a)
    for f in sorted(files):
        check(f)

if __name__ == "__main__":
    main()
