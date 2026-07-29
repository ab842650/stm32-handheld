# STM32F407 觸控手機介面專案 — 規格與開發計畫

## 1. 專案目標

在 **STM32F407G-DISC1** 上，使用 **FreeRTOS** 打造一個類手機觸控介面系統，包含基本應用（時鐘、設定）與小遊戲，並進行 **排程器效能研究**（Priority-based vs EDF vs RT-EEVDF）。

---

## 2. 硬體規格

### 2.1 主控板
- **型號**：STM32F407G-DISC1
- **MCU**：STM32F407VGT6，Cortex-M4，168 MHz，FPU
- **記憶體**：192 KB SRAM（含 64 KB CCM）、1 MB Flash
- **內建周邊**：LIS3DSH 加速度計（SPI）、4× LED（PD12–15）、User Button（PA0）、USB OTG FS

### 2.2 顯示與觸控模組
- **型號**：ILI9341 + XPT2046，3.2"／2.8" 可選，320×240，16-bit RGB565
- **介面**：SPI，3.3V 邏輯準位（與 STM32 相容，免電平轉換）
- **內建**：SD 卡槽（SPI 共用匯流排）

### 2.3 接線表

| 模組腳位 | 功能 | STM32 接腳 | 備註 |
|---|---|---|---|
| VCC | 電源 | 3.3V | |
| GND | 地 | GND | |
| CS | 顯示片選 | PB0 | |
| RESET | 重置 | PB2 | |
| DC/RS | 命令/資料 | PB1 | |
| SDI (MOSI) | SPI 資料輸出 | PA7 | SPI1_MOSI |
| SCK | SPI 時脈 | PA5 | SPI1_SCK，共用 |
| SDO (MISO) | SPI 資料輸入 | PA6 | SPI1_MISO，共用 |
| LED | 背光 | 3.3V 或 PC6 | PC6 可做 PWM 調光 |
| T_CLK | 觸控時脈 | PA5 | 與顯示共用 |
| T_CS | 觸控片選 | PC4 | 獨立 CS |
| T_DIN | 觸控資料輸入 | PA7 | 共用 |
| T_DO | 觸控資料輸出 | PA6 | 共用 |
| T_IRQ | 觸控中斷 | PC5 | EXTI 中斷腳 |
| SD_CS | SD 卡片選（選用）| PC7（建議）| 與其他裝置共用 SPI1 |

> SPI1 同時承載顯示、觸控、SD 卡三個裝置，靠各自獨立 CS 腳切換，所有 CS 平時應保持高電位（未選中）。

---

## 3. 軟體架構

### 3.1 分層原則
- 依賴方向只能往下：`App → UI/Core → GFX → BSP`
- `Screens/` 只依賴 `UI/Core`，不可直接呼叫 `BSP/`
- 所有 `vTaskCreate` 集中在 `app_main.c`

### 3.2 檔案結構

```
Drivers/
  BSP/
    ili9341.c / .h         # SPI 顯示驅動，DMA 非阻塞傳輸
    xpt2046.c / .h         # 觸控座標讀取 + 校正
    lis3dsh.c / .h         # 加速度計驅動（板載）
    sdcard.c / .h          # SD 卡驅動
  GFX/
    gfx.c / .h             # 基本圖形函式（矩形、圓、文字）
    font.c / .h            # 點陣字體
    framebuf.c / .h        # Framebuffer 管理、dirty region、DMA flush

UI/
  Core/
    ui_engine.c / .h       # Screen Stack：push / pop / replace
    ui_event.h             # ui_event_t 定義
    ui_task.c / .h         # UI Task 主迴圈（16ms tick）
    widget.c / .h          # Button / Label / ProgressBar
  Screens/
    screen_home.c / .h     # 主選單
    screen_clock.c / .h    # 時鐘
    screen_settings.c / .h # 設定（亮度、音量）
    screen_sensor.c / .h   # 水平儀 / 計步器
    screen_game.c / .h     # 遊戲入口
  Games/
    game_snake.c / .h      # 貪吃蛇
    game_pong.c / .h       # Pong（加速度計控制）

App/
  app_main.c               # 所有 Task 建立點
  app_config.h             # 全域常數、Task 優先級、PERF_MEASURE 開關
  Services/
    svc_input.c / .h       # Input Task：觸控 + 按鈕 → ui_queue
    svc_sensor.c / .h      # Sensor Task：讀加速度計
    svc_storage.c / .h     # SD 卡讀寫（Mutex 保護）
    svc_audio.c / .h       # DAC 音效（選用）

Assets/
  icons.h                  # 圖示 C 陣列
  fonts/font_12.h
  fonts/font_20.h

Research/                  # 排程器研究專用
  rt_eevdf.c / .h          # RT-EEVDF 排程器實作（fork 自 FreeRTOS tasks.c）
  edf_sched.c / .h         # EDF 排程器實作
  perf_logger.c / .h       # DWT cycle counter、UART 量測輸出
```

### 3.3 任務（Task）規劃

| Task | 優先級 | 職責 |
|---|---|---|
| `vUITask` | 高 (3) | 事件處理、畫面渲染、DMA flush |
| `vInputTask` | 高 (4) | 觸控 IRQ 去抖動、按鈕處理，送事件到 Queue |
| `vGameTask` | 中 (2) | 遊戲邏輯，平常 suspend，進入遊戲時 resume |
| `vSensorTask` | 低 (1) | 讀取加速度計，放入 Queue |
| `vStorageTask` | 低 (1) | SD 卡讀寫，Mutex 保護 SPI |

### 3.4 IPC 機制

| 機制 | 用途 |
|---|---|
| Queue (`ui_event_queue`) | 觸控/按鈕事件 → UI Task |
| Binary Semaphore (`dma_done_sem`) | DMA 傳輸完成通知 |
| Mutex (`spi_bus_mutex`) | 保護 SPI1 匯流排（顯示/觸控/SD 共用）|
| Event Group | 系統狀態旗標（如「遊戲中」「省電模式」）|

---

## 4. UI 設計原則

- **Screen vtable**：每個畫面實作 `on_enter / on_event / on_draw / on_exit` 四個函式
- **固定幀率**：目標 60 FPS，使用 `vTaskDelayUntil` 避免時間漂移
- **Dirty Region**：只 flush 有變化的矩形區域，減少 SPI 傳輸量
- **Framebuffer**：`frame_back` 放 CCM SRAM（CPU 繪圖快），DMA chunk buffer 放一般 SRAM（DMA 可存取）
- **觸控事件流**：`T_IRQ` 中斷 → ISR `xQueueSendFromISR` → Input Task 讀座標 → 送 `ui_event_t` 到 UI Task

---

## 5. 效能量測規劃

### 5.1 量測指標

| 指標 | 工具 | 說明 |
|---|---|---|
| FPS 穩定性 | DWT Cycle Counter | 平均 FPS、標準差、最差幀時間 |
| 觸控延遲 | GPIO + 示波器 | IRQ 觸發 → 畫面更新的端到端時間 |
| CPU 使用率 | `vTaskGetRunTimeStats()` | 各任務佔比、idle 比例 |
| Context Switch 次數 | SEGGER SystemView | 每秒切換次數、overhead |
| Deadline Miss Rate | 自訂 logger | 任務超過 deadline 的比例 |

### 5.2 量測原則
- 所有量測程式碼包在 `#ifdef PERF_MEASURE`，預設關閉
- **改一個變數、量一次**，不同時改兩個參數
- 每個優化版本一個 git branch，方便回頭比對
- 先建立 **Baseline**（原版 FreeRTOS + 高負載遊戲），所有後續比較都對照這組數字

### 5.3 負載設計（製造 worst case）
- Snake：蛇身長度可從 10 節增加到 100 節
- Pong：球速可倍增
- 目的：在 CPU 接近滿載時才能看出排程策略的差異

---

## 6. 排程器研究規劃

### 6.1 研究問題
> EEVDF 的 eligible + virtual deadline 雙條件選擇機制，移植到嵌入式即時系統後，在混合 UI + 遊戲負載下的效能表現是否優於傳統 Priority-based 和純 EDF 排程？

### 6.2 三路比較實驗
1. **Baseline**：FreeRTOS 原生 Priority-based + Round-Robin
2. **EDF**：修改 `taskSELECT_HIGHEST_PRIORITY_TASK()`，依任務 deadline 選擇
3. **RT-EEVDF**：weight 改為任務週期倒數，vruntime 換成真實 deadline，保留 eligible 篩選機制

### 6.3 修改範圍（fork FreeRTOS，不動官方 source）
- TCB 結構新增欄位：`xTaskDeadline`、`xTaskWeight`、`xVirtualRuntime`
- 修改任務選擇巨集，從 O(1) bitmap 改為依排程策略掃描 ready list
- 新增 API：`vTaskSetDeadline()`、`vTaskSetWeight()`

### 6.4 預期產出
- 三種排程策略在同一遊戲負載下的 FPS jitter、觸控延遲、deadline miss rate 對照表與折線圖
- 結論不論優劣都有研究價值（驗證或反駁 EEVDF 思想在即時系統的適用性）

---

## 7. 開發步驟（依階段執行）

### 階段 1：硬體驅動（預估 1–2 週）
- [ ] STM32CubeIDE 專案建立，Clock 168 MHz，啟用 FreeRTOS Middleware
- [ ] SPI1 設定（DMA 模式），GPIO 設定（CS/DC/RST/IRQ）
- [ ] `ili9341.c` 移植，`ILI9341_FillScreen()` 驗證螢幕點亮
- [ ] `xpt2046.c` 實作，T_IRQ 中斷讀座標，UART 印出確認
- [ ] 觸控 4 點校正，座標對齊像素
- [ ] **交付標準**：點螢幕任意位置，終端機印出正確像素座標

### 階段 2：UI 框架（預估 2 週）
- [ ] `ui_event_t` 定義，建立 `ui_event_queue`
- [ ] Input Task 完成：觸控/按鈕事件彙整 → Queue
- [ ] `ui_engine.c` Screen Stack 實作（push/pop/replace）
- [ ] `framebuf.c` 完成：dirty region、DMA flush、CCM 記憶體配置
- [ ] 主選單畫面（圖示格狀排列）
- [ ] 時鐘、設定畫面
- [ ] **交付標準**：主選單可觸控切換至各畫面，返回正常

### 階段 3：遊戲實作（預估 2–3 週）
- [ ] Snake 邏輯 + 渲染，dirty region 繪製
- [ ] Pong 邏輯，串接加速度計控制
- [ ] Game Task suspend/resume 管理
- [ ] 負載壓力設計（蛇身長度、球速可調）
- [ ] **交付標準**：兩款遊戲可玩，高負載時可觀察到卡頓現象

### 階段 4：效能量測（預估 1–2 週）
- [ ] DWT Cycle Counter 初始化，`PERF_MEASURE` 量測框架
- [ ] FPS / draw time / flush time UART 即時輸出
- [ ] SEGGER SystemView 整合，視覺化 context switch
- [ ] 建立 Baseline 數據（原版 FreeRTOS + 高負載遊戲）
- [ ] 應用層優化實驗：Tick Rate、Queue 深度、優先級配置
- [ ] **交付標準**：完整 Baseline 報告，包含 FPS、延遲、CPU 使用率數據

### 階段 5：排程器研究（預估 3–4 週，可延伸）
- [ ] Fork FreeRTOS source 到獨立 repo/branch
- [ ] EDF 排程器實作與驗證
- [ ] RT-EEVDF 排程器實作與驗證
- [ ] 三路比較實驗執行，數據收集
- [ ] 結果分析與報告撰寫
- [ ] **交付標準**：三種排程策略的量化比較報告（表格 + 圖表）

---

## 8. 開發工具

| 工具 | 用途 |
|---|---|
| STM32CubeIDE | 主要開發環境 |
| STM32CubeMX | Pin/Clock/Middleware 設定 |
| SEGGER SystemView | 任務切換時間軸視覺化（免費）|
| 邏輯分析儀 / 示波器 | GPIO 時間戳記量測（觸控延遲）|
| Git | 版本控制，每個排程器版本獨立 branch |

---

## 9. 風險與注意事項

- **SPI 匯流排共用**：顯示、觸控、SD 卡共用 SPI1，必須用 Mutex 保護，避免並發存取衝突
- **CCM 記憶體限制**：CCM 只有 CPU 可存取，DMA 無法直接從 CCM 讀取，需要中介 buffer
- **EDF/EEVDF 過載行為**：過載時 deadline miss 的任務不確定，與 Priority-based 的確定性失敗形成對比，這正是研究的觀察重點
- **觸控杜邦線長度**：超過 20cm 建議 SPI 速度不超過 21 MHz，避免訊號完整性問題
