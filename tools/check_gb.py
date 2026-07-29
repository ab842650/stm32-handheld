#!/usr/bin/env python3
"""
檢查 GB ROM 的卡匣類型/大小，判斷第一版模擬器（只吃 32KB 無 MBC）能不能跑。

用法：
  python check_gb.py 某個.gb
  python check_gb.py *.gb          # 批次檢查一整包
"""
import sys, glob

CART_TYPE = {
    0x00: "ROM ONLY (無 MBC)",
    0x01: "MBC1", 0x02: "MBC1+RAM", 0x03: "MBC1+RAM+BATT",
    0x05: "MBC2", 0x06: "MBC2+BATT",
    0x0F: "MBC3+RTC", 0x10: "MBC3+RTC+RAM", 0x11: "MBC3",
    0x12: "MBC3+RAM", 0x13: "MBC3+RAM+BATT",
    0x19: "MBC5", 0x1A: "MBC5+RAM", 0x1B: "MBC5+RAM+BATT",
    0x1C: "MBC5+RUMBLE", 0x1D: "MBC5+RUMBLE+RAM", 0x1E: "MBC5+RUMBLE+RAM+BATT",
}
ROM_SIZE = {0x00: "32KB", 0x01: "64KB", 0x02: "128KB", 0x03: "256KB",
            0x04: "512KB", 0x05: "1MB", 0x06: "2MB", 0x07: "4MB", 0x08: "8MB"}

def check(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x150:
        print("%-20s  ✗ 太小，不是有效 GB ROM" % path); return
    title = bytes(b if 0x20 <= b < 0x7F else 0x20 for b in data[0x134:0x143]).decode().strip()
    ctype = data[0x147]
    csize = data[0x148]
    ok = (ctype == 0x00 and csize == 0x00 and len(data) == 32768)
    print("%-24s %-16s type=0x%02X %-18s size=%-6s file=%d bytes  %s" % (
        path, title, ctype, CART_TYPE.get(ctype, "未知"),
        ROM_SIZE.get(csize, "?"), len(data),
        "✅ 第一版可跑" if ok else "❌ 需要 MBC 串流（之後）"))

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
