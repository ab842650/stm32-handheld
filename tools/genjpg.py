#!/usr/bin/env python3
"""
產生一張 320x240 的 JPEG 測試圖，用來驗證板子上 TJpgDec 解碼是否正確。

跟 genbmp.py 同一個測試圖案，方便沿用「四角顏色」驗證法：
  - 四角：左上=紅 右上=綠 左下=藍 右下=黃
  - 背景：R 隨 x、B 隨 y 的漸層
  - 中央白色十字

輸出 test.jpg，複製到 SD 卡根目錄。
（JPEG 有失真壓縮，四角顏色會有一點點暈開/偏差，屬正常。）
"""
from PIL import Image, ImageDraw

W, H = 320, 240
img = Image.new("RGB", (W, H))
px = img.load()

for y in range(H):
    for x in range(W):
        px[x, y] = (x * 255 // (W - 1), 40, y * 255 // (H - 1))

d = ImageDraw.Draw(img)
M = 40
d.rectangle([0, 0, M, M], fill=(255, 0, 0))              # 左上 紅
d.rectangle([W - M, 0, W, M], fill=(0, 255, 0))          # 右上 綠
d.rectangle([0, H - M, M, H], fill=(0, 0, 255))          # 左下 藍
d.rectangle([W - M, H - M, W, H], fill=(255, 255, 0))    # 右下 黃

d.line([W // 2, 0, W // 2, H], fill=(255, 255, 255), width=2)
d.line([0, H // 2, W, H // 2], fill=(255, 255, 255), width=2)

# quality=90：畫質好、檔案又小；baseline（TJpgDec 不支援 progressive）
img.save("test.jpg", quality=90, optimize=True, progressive=False)
print("wrote test.jpg  (%dx%d, baseline JPEG)" % (W, H))
