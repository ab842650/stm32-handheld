#!/usr/bin/env python3
"""
產生一張 320x240 的 24-bit BMP 測試圖，用來驗證板子顯示 BMP 的
方向與顏色是否正確。

特徵：
  - 背景：左→右 紅漸層 + 上→下 藍漸層（看得出 X/Y 方向）
  - 四角不同色方塊：左上=紅 右上=綠 左下=藍 右下=黃
    （一眼看出有沒有上下顛倒 / 左右鏡像）
  - 正中央白色十字

輸出 test.bmp（24-bit），複製到 SD 卡根目錄。
"""
from PIL import Image, ImageDraw

W, H = 320, 240
img = Image.new("RGB", (W, H))
px = img.load()

# 背景漸層：R 隨 x、B 隨 y
for y in range(H):
    for x in range(W):
        px[x, y] = (x * 255 // (W - 1), 40, y * 255 // (H - 1))

d = ImageDraw.Draw(img)
M = 40  # 角落方塊邊長
d.rectangle([0, 0, M, M], fill=(255, 0, 0))                 # 左上 紅
d.rectangle([W - M, 0, W, M], fill=(0, 255, 0))            # 右上 綠
d.rectangle([0, H - M, M, H], fill=(0, 0, 255))            # 左下 藍
d.rectangle([W - M, H - M, W, H], fill=(255, 255, 0))     # 右下 黃

# 中央白色十字
d.line([W // 2, 0, W // 2, H], fill=(255, 255, 255), width=2)
d.line([0, H // 2, W, H // 2], fill=(255, 255, 255), width=2)

img.save("test.bmp")   # Pillow 對 RGB 影像預設存成 24-bit BMP
print("wrote test.bmp  (%dx%d, 24-bit)" % (W, H))
