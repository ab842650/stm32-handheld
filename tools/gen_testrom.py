#!/usr/bin/env python3
"""
手工組一個 CHIP-8 測試 ROM：一個 8x4 方塊在 64x32 畫面裡上下左右彈跳。
用來驗證模擬器（DXYN 繪圖 / timer 延遲 / 迴圈 / 邊界反彈）。

程式載入位址 0x200。組譯清單（位址: opcode  助記）：

  200: 6000  LD  V0,0        ; x = 0
  202: 6100  LD  V1,0        ; y = 0
  204: 6201  LD  V2,1        ; dx = +1
  206: 6301  LD  V3,1        ; dy = +1
  208: A230  LD  I,0x230     ; I -> 方塊圖形
  20A: D014  DRW V0,V1,4     ; 畫（8x4）
  20C: 6403  LD  V4,3        ; 延遲 3 frame
  20E: F415  LD  DT,V4
  210: F407  LD  V4,DT       ; 等 DT 歸零
  212: 3400  SE  V4,0
  214: 1210  JP  0x210
  216: A230  LD  I,0x230
  218: D014  DRW V0,V1,4     ; 擦（XOR 同位置）
  21A: 8024  ADD V0,V2       ; x += dx
  21C: 4038  SNE V0,56       ; 到右緣 → dx=-1
  21E: 62FF  LD  V2,255
  220: 4000  SNE V0,0        ; 到左緣 → dx=+1
  222: 6201  LD  V2,1
  224: 8134  ADD V1,V3       ; y += dy
  226: 411C  SNE V1,28       ; 到底 → dy=-1
  228: 63FF  LD  V3,255
  22A: 4100  SNE V1,0        ; 到頂 → dy=+1
  22C: 6301  LD  V3,1
  22E: 1208  JP  0x208       ; 迴圈
  230: FF FF FF FF           ; 8x4 實心方塊
"""
import os

words = [
    0x6000, 0x6100, 0x6201, 0x6301,
    0xA230, 0xD014,
    0x6403, 0xF415, 0xF407, 0x3400, 0x1210,
    0xA230, 0xD014,
    0x8024, 0x4038, 0x62FF, 0x4000, 0x6201,
    0x8134, 0x411C, 0x63FF, 0x4100, 0x6301,
    0x1208,
]

data = bytearray()
for w in words:
    data.append((w >> 8) & 0xFF)
    data.append(w & 0xFF)
# 位址 0x230 的方塊圖形（0x200 起算 offset 0x30 = 48）
assert len(data) == 0x30, "code length mismatch: %d" % len(data)
data += bytes([0xFF, 0xFF, 0xFF, 0xFF])

os.makedirs("roms", exist_ok=True)
with open("roms/TEST.CH8", "wb") as f:
    f.write(data)
print("wrote roms/TEST.CH8  (%d bytes)" % len(data))
