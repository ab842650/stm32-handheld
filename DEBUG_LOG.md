# STM32F407G-DISC1 專案除錯紀錄

> ⚠️ **已停止維護（2026-07-16）**：除錯記錄已併入 `DEV_LOG.md`，後續問題請記在 DEV_LOG。
> 本檔保留作為歷史存檔，內容不再更新。

**平台：** STM32F407G-DISC1  
**顯示器：** ILI9341（SPI）  
**觸控：** XPT2046（SPI）  
**RTOS：** FreeRTOS  
**日期：** 2026-06-21

---

## 問題一：ILI9341 灰色殘影

### 問題現象

螢幕最左側有一塊固定的灰色區域，無論畫面內容為何，該殘影始終存在。

### 根本原因

MADCTL 暫存器設定為 `0x48`（MX=1, BGR=1），缺少 MV bit（Row/Column Exchange）。在此設定下，顯示器的行列方向未正確對應 landscape 模式，導致部分像素偏移，造成灰色殘影。

### 解決方法

將 MADCTL 改為 `0x68`（MX=1, MV=1, BGR=1）。加入 MV=1（Row/Column Exchange）後，顯示器從 portrait 模式正確轉換為 landscape 模式，殘影消失。

---

## 問題二：HAL_Delay 卡死

### 問題現象

`ILI9341_Init()` 執行到第一個 `HAL_Delay(50)` 時整個系統卡死。UART 只印出 `"ili: rst high"` 就停住，後續初始化完全無法繼續。

### 根本原因

HAL timebase 使用 TIM2（`stm32f4xx_hal_timebase_tim.c`），TIM2 中斷 priority 設為 15。

程式在 FreeRTOS scheduler 啟動**之前**呼叫了 `xSemaphoreCreateMutex()`，而此函式內部的 critical section 會將 `BASEPRI` 設為 `0x50`（即 `configMAX_SYSCALL_INTERRUPT_PRIORITY`）。這個動作會 mask 所有 priority 數值 ≥ 5 的中斷，涵蓋 TIM2（priority=15）。

因此，`HAL_Delay()` 等待 TIM2 tick 的機制被 BASEPRI 封鎖，造成永久等待（infinite wait）。

### 解決方法

在 `ili9341.c` 中實作基於 **DWT Cycle Counter** 的延遲函式 `_delay_ms()`，完全不依賴 TIM2 或任何中斷機制。DWT cycle counter 直接讀取 CPU 硬體計數器，不受 BASEPRI 或中斷 mask 影響，解決卡死問題。

---

## 問題三：XPT2046 SPI 速度過高

### 問題現象

觸控控制器所有讀值皆為 `0x00`，無法取得任何有效的 ADC 資料。

### 根本原因

SPI1 的 prescaler 設定為 2，在 84MHz APB2 時脈下運作速度為 **42MHz**。然而 XPT2046 的 SPI 最高支援速度僅為 **2MHz**，遠超出規格導致通訊完全失敗，所有讀值為零。

### 解決方法

在 `XPT2046_ReadRaw()` 函式前後動態切換 SPI 速度：

1. 讀取觸控資料前，使用 `MODIFY_REG` 直接修改 SPI CR1 暫存器，將 prescaler 切換為 64（約 1.3MHz）
2. 讀取完成後，恢復 prescaler 為 2（42MHz）供 ILI9341 使用

切換流程搭配 `__HAL_SPI_DISABLE` / `__HAL_SPI_ENABLE` 確保暫存器修改在 SPI 停用狀態下進行，避免未定義行為。

---

## 問題四：XPT2046 座標軸映射錯誤

### 問題現象

觸控方向與螢幕座標完全不對應。例如往右滑動，螢幕上的點卻往下移動，所有方向皆錯誤。

### 根本原因

MADCTL 設定為 `0x68`（MX=1, MV=1, BGR=1）後，螢幕以 landscape 模式顯示，但 XPT2046 回傳的原始座標 `xr`、`yr` 仍對應觸控面板本身的實體方向。MV bit 造成顯示行列互換，使得觸控的 `xr/yr` 與螢幕的 X/Y 軸之間需要進行 **swap 加 flip** 才能正確對應。

### 解決方法

重新推導座標映射關係：

- `xr` 對應螢幕 **Y** 軸（正向映射）
- `yr` 對應螢幕 **X** 軸（翻轉映射）

最終映射公式：

```c
x = SCREEN_W - 1 - (yr - Y_MIN) * SCREEN_W / (Y_MAX - Y_MIN)
y = (xr - X_MIN) * SCREEN_H / (X_MAX - X_MIN)
```

---

## 問題五：XPT2046 校正值與硬體不符

### 問題現象

觸碰位置與畫面顯示的紅點有明顯偏移，即使方向正確，點的位置仍不準確。

### 根本原因

程式碼使用的預設校正值（`X_MIN=300`, `X_MAX=3800` 等）為通用參考值，與實際硬體的 ADC 輸出範圍不符。不同批次或廠商的 XPT2046 模組，其 ADC 輸出的最小值與最大值存在差異。

### 解決方法

加入除錯輸出，分別觸碰螢幕四個角落，透過 UART 印出各點的 raw ADC 值（`xr`, `yr`）。根據實測資料重新設定校正值：

| 參數 | 原始值 | 實測值 |
|------|--------|--------|
| X_MIN | 300 | 458 |
| X_MAX | 3800 | 3590 |
| Y_MIN | 300 | 370 |
| Y_MAX | 3800 | 3839 |

---

## 問題六：專案名稱殘留（lab5 → final_project）

### 問題現象

燒錄後執行的是舊韌體行為，UART 出現 SD mount retry 錯誤，與新專案的預期行為不符。

### 根本原因

`final_project` 是從 `lab5` 複製而來。STM32CubeIDE 的以下三個設定檔內部仍殘留 `lab5` 的名稱與路徑參考：

- `.project`
- `.cproject`
- `.launch`

IDE 在建置或燒錄時參照到舊專案的設定，導致實際執行的韌體不是 `final_project` 的程式碼。

### 解決方法

手動編輯上述三個設定檔，將所有 `lab5` 字串替換為 `final_project`，包含專案名稱、路徑參考及 launch configuration 目標設定，確保 IDE 完整指向新專案。

---

## 問題七：顯示方向上下顛倒（加入文字渲染後才發現）

**日期：** 2026-07-16

### 背景

新增文字渲染功能：以 Python + Pillow 讀取 Consolas 字型，將 ASCII 0x20–0x7E 渲染成 8×16 點陣，產生 `font8x16[95][16]`（`font.c` / `font.h`）。`ILI9341_DrawChar()` 讀取字形、在緩衝區逐像素填入前景/背景色（RGB565），再以單次 DMA 送出整個字元矩形。

### 問題現象

畫出的文字（`ABC`）出現在螢幕**左下角**且**上下顛倒**，但先前的按鈕（`FillRect`）看起來一切正常。

### 根本原因

MADCTL 設定為 `0x68`（MX=1, MV=1, BGR=1），其中 **MY=0**，導致垂直方向相對於預期的「左上為原點」是翻轉的。

先前只用 `FillRect` 畫純色方塊——**純色矩形上下顛倒後外觀完全相同**，因此方向錯誤一直被隱藏。文字是第一個「具有上下之分」的內容，一畫出來就暴露了面板真正的方向。此問題並非 `DrawChar` 的繪圖邏輯錯誤（座標由 `SetWindow` 決定，`DrawChar` 未更動位置）。

### 解決方法

將 MADCTL 由 `0x68` 改為 `0xE8`（加入 **MY=1**，Row Address Order）：

```
0x68 + 0x80(MY) = 0xE8  →  MY=1, MX=1, MV=1, BGR=1
```

文字恢復為左上角、正立顯示。

---

## 問題八：更改顯示方向後觸控 X 軸失準

**日期：** 2026-07-16

### 問題現象

問題七將 MADCTL 改為 `0xE8` 後，觸控與畫面不再對齊。返回按鈕畫在**視覺左上角** `(0,0)~(60,40)`，但點左上角無反應，改點**右上角**才會觸發返回——即觸控的 **X 軸方向相反**。

### 根本原因

`XPT2046_ReadPixel()` 的座標映射是針對舊的 MADCTL=`0x68` 推導的（見問題四）。顯示方向改為 `0xE8` 後垂直軸翻轉，但觸控映射未同步更新，使得觸控 X 軸相對於新的顯示方向變成反向。

### 解決方法

移除 X 映射中多餘的反轉項（`SCREEN_W - 1 - …`），使其與 `0xE8` 的顯示方向一致。更新後的映射公式（取代問題四的 X 公式）：

```c
x = (yr - Y_MIN) * SCREEN_W / (Y_MAX - Y_MIN)   // 不再反轉
y = (xr - X_MIN) * SCREEN_H / (X_MAX - X_MIN)
```

在 `ReadPixel` 暫時加入 debug 輸出，觸碰四個角落確認 raw ADC 值與換算後像素座標；四角映射（左上≈0,0；右上≈319,0；左下≈0,239；右下≈319,239）皆正確後移除 debug，觸控與顯示完全對齊。

---

*本除錯紀錄涵蓋 final_project 開發過程中所有已知問題及解決方案。*
