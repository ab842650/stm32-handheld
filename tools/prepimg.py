#!/usr/bin/env python3
"""
把任意圖片轉成板子能顯示的 JPEG：
  - baseline 編碼（TJpgDec 不支援 progressive）
  - 轉成 RGB（丟掉 alpha / CMYK）
  - 縮到 320x240 以內（等比例，省解碼記憶體）
  - 輸出 8.3 相容檔名（大寫、主檔名 ≤8 字）

用法：
  python prepimg.py 你的圖.jpg              -> 產生 OUT.JPG
  python prepimg.py 你的圖.png  PIC1        -> 產生 PIC1.JPG
  python prepimg.py *.jpg                    -> 批次，自動命名 IMG1.JPG, IMG2.JPG ...

產出的 .JPG 複製到 SD 卡根目錄即可。
"""
import sys
from PIL import Image

SCREEN = (320, 240)

def convert(src, out_name):
    img = Image.open(src).convert("RGB")     # 丟掉 alpha / CMYK
    img.thumbnail(SCREEN)                     # 等比例縮到塞得進螢幕
    out = out_name.upper()
    if not out.endswith(".JPG"):
        out += ".JPG"
    img.save(out, format="JPEG", quality=90, optimize=True, progressive=False)
    print("wrote %-12s (%dx%d, baseline)" % (out, img.width, img.height))

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    # 若第二個參數不是圖檔而是名字，當單檔改名用
    if len(args) == 2 and not args[1].lower().endswith(
            (".jpg", ".jpeg", ".png", ".bmp", ".gif")):
        convert(args[0], args[1])
        return
    # 否則批次：自動命名 IMG1, IMG2 ...
    for i, src in enumerate(args, 1):
        convert(src, "IMG%d" % i)

if __name__ == "__main__":
    main()
