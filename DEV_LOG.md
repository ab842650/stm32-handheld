# STM32F407 觸控手機介面 — 開發日誌

> 平台：STM32F407G-DISC1 ｜ 顯示：ILI9341 (SPI) ｜ 觸控：XPT2046 (SPI) ｜ RTOS：FreeRTOS
> 目標：在 STM32F407 上做出一個可用的類手機觸控介面（主選單 + 多個 App）

[TOC]

---

## 1. 專案簡介

**目標**：打造一個「能操作的手機」——開機進主選單，觸控切換到各個 App（計算機、時鐘、相片、遊戲…），可返回。

**目前定位**：以「做出一隻能用的手機」為主線，暫不做排程器研究。

---

## 2. 硬體與開發環境

| 項目 | 內容 |
|---|---|
| 主控 | STM32F407VGT6（Cortex-M4，168 MHz，FPU）|
| 顯示 | ILI9341，320×240，RGB565，SPI1 |
| 觸控 | XPT2046，SPI1（共用匯流排），T_IRQ = PC5（EXTI）|
| 除錯 | USART2 @ 115200（myprintf）|
| 工具 | STM32CubeIDE / CubeMX、Git |

### 接線速查

| 功能 | STM32 腳位 |
|---|---|
| 顯示 CS / DC / RST | PB0 / PB1 / PB2 |
| SPI1 SCK / MISO / MOSI | PA5 / PA6 / PA7 |
| 觸控 T_CS / T_IRQ | PC4 / PC5 |

---

## 3. 系統架構

```
Core/Src
  main.c          # 時脈/SPI/DMA/GPIO 初始化、Task 建立、UI/Input Task
  screen.c        # Screen Stack：push / pop / replace / 事件分派
  screen_*.c      # 各畫面（home / calc / game / photo …）
  myprintf.c      # UART debug 輸出

Drivers/BSP
  ili9341.c/.h    # 顯示驅動：SetWindow + DMA flush、FillScreen/FillRect/DrawPixel
  xpt2046.c/.h    # 觸控讀取 + 校正
```

**事件流**：`T_IRQ (EXTI)` → ISR notify → `InputTask` 讀座標去彈跳 → `ui_event_queue` → `UITask` → `Screen_OnTouch / OnRender`

**畫面模型**：每個 Screen 實作 `on_enter / on_exit / on_touch / on_render` 四個 callback，透過 vtable 註冊。

---

## 4. 開發進度總覽

**手機介面（基礎）**
- [x] 階段 0：驅動 — ILI9341 + XPT2046 點亮、觸控校正
- [x] 階段 1：UI 骨架 — Screen Stack、Input→Queue→UI 事件流、主選單可切換
- [x] 階段 2：**文字渲染** — DrawChar / DrawString（換行/折行）+ 點陣圖示 DrawBitmapMono
- [ ] 階段 3：Widget（Button / Label）抽象 ← 已有 `ui.c`；Button 待萃取
- [x] 階段 4：真實 App — 計算機、時鐘、Notes、Photo、Game 選單全完成
- [x] 階段 4.5：**SD 卡儲存** — 手寫 SPI2 + FatFs 掛載、讀檔
- [ ] 階段 5：畫面優化（framebuffer / 局部重畫，消閃爍）
- [x] 階段 6：相片 — SD 卡 BMP + JPG（TJpgDec）+ 資料夾相簿翻頁
- [x] 階段 6.5：SD 卡分資料夾（/GAMES /PHOTOS /NOTES /ROMS /GB）、Notes/Photo 檔案清單

**可載入 module 系統（自製 mini-OS）**
- [x] 階段 7：**Loader** — M0 固定位址載入原生碼、M1 syscall 表、M2a 位置無關(-fpic)、load-anywhere
- [ ] 階段 7.5：M2b 可寫全域變數 PIC（-mrwpi/r9）← 暫緩，遊戲狀態放 local 繞過

**遊戲 / 模擬器**
- [x] 階段 8：原生遊戲 module — 貪食蛇、Tetris（位置無關 .bin，存 SD）
- [x] 階段 9：**CHIP-8 模擬器** — 一個 module 跑多 ROM（/ROMS），syscall 加 read_file/list_dir
- [x] 階段 10：**Game Boy 模擬器**（Peanut-GB）— 內建 app、ROM 存 SD、CCM 工作記憶體、觸控 8 鍵、50fps（1x）
- [x] 階段 10.5：**MBC 大遊戲**（寶可夢等）— ROM 從 SD 串流 + ROM 選單。**細粒度快取（512B×32 direct-mapped CPU cache）→ 寶可夢從個位數 fps → ~60fps**
- [x] 階段 10.6：**GB 存檔** — cart RAM 32KB 移到 SRAM（縮 FreeRTOS heap 75→48KB 騰空間）、`.sav` 存 SD。**debounced 自動存檔**：遊戲內 SAVE → cart RAM dirty → 靜置 1.5s 自動刷到 SD（Peanut-GB 只在遊戲啟用 cart RAM 時才寫，故不會誤觸發）
- [ ] 階段 10.7：選單捲動（>6 ROM）← 之後

**WiFi（ESP32-S3 協處理器，UART 連線）**
- [x] 階段 11：**WiFi 連線** — ESP32-S3 當 WiFi 協處理器，STM32 走 USART3(PB10/PB11) 下高階指令
  - W0 單向 TX → W1 雙向 + **中斷接收 ring buffer**（手寫驅動）→ W2 ESP32 上網查 IP → W4 **NTP 對時**
- [x] 階段 11.5：**架構整理 + HTTP GET** — 搬進獨立 `vNetTask`（優先級 2）；W3 `WX?` 抓 wttr.in 天氣；首頁標題列顯示時間+天氣，移除獨立 Clock app
- [x] 階段 11.6：**SD 併發保護**（FatFs volume 鎖 + 壓力測試證明）。W5 無線下載（chunk+ack 協定）驗證過，但一直沒接 UI，**已移除**（見 2026-08-04）
- [x] 階段 12：**Discord 訊息 App** — ESP32 走 Discord REST API 收發訊息，STM32 端 Messages 畫面 + 首頁未讀徽章
- [x] 階段 12.5：**螢幕小鍵盤**（共用元件 `screen_kb.c`）— Messages 能打任意文字；**Notes 從唯讀變可寫**
- [ ] 階段 12.6：中文顯示（SD 點陣字庫 + UTF-8）← 已評估，暫緩

---

## 5. 開發日誌

> 由新到舊排列，最新的放最上面。

<!-- ↓↓↓ 複製這個模板開一則新日誌 ↓↓↓ -->
<!--
### YYYY-MM-DD ｜ <這次做了什麼>

**目標**：

**做了什麼**：
-

**遇到的問題**：
-

**怎麼解的**：
-

**學到 / 筆記**：
-

**下一步**：
-
-->
<!-- ↑↑↑ 複製上面 ↑↑↑ -->

### 2026-08-04 ｜ 螢幕小鍵盤（共用元件）+ Notes 變成可寫

**目標**：Messages 只能發 3 句預設，這是「真的能聊天」唯一的阻礙。做一個鍵盤，而且要能被其他畫面重複使用。

**做了什麼**：
- 新增 `screen_kb.c` / `screen_kb.h`，一個獨立畫面 `SCREEN_KB`。對外只有一個 API：
  ```c
  Keyboard_Open(title, initial, on_done_cb);   /* push 到呼叫者上面 */
  ```
- **版面**：文字框（y=32, h=36，游標 500ms 閃）+ 4 列鍵（32×34px，y=70/104/138/172）+ 軟鍵列 Back。
- **三個 layer**（小寫 / 大寫 / 數字符號）用 `ROWS[3][3]` 字串表定義，切 layer 只是換一組資料。
- Shift 打完一個字自動回小寫（跟手機一樣）；`123`/`ABC` 切數字層；`<=` 退格。
- **接進 Messages**：第 4 顆按鈕 `Type`。
- **接進 Notes**：VIEW 模式左軟鍵 `Add` → 打完 Send 就 `f_open(FA_WRITE|FA_OPEN_APPEND)` + `f_write` 接到檔尾，重讀顯示。**Notes 從唯讀變可寫。**

**設計取捨**：
- **回呼順序：先 `Screen_Pop()` 再 `cb(text)`。** 反過來也能動，但 callback 裡若想再 `Screen_Push`（例如打完字跳確認頁），就會疊在還沒 pop 的鍵盤上面，堆疊順序亂掉。先 pop 讓 callback 拿到乾淨狀態。
- **繪圖與命中判定共用 `key_x(row, i)`** —— 跟 `screen_home` 的 `app_x()` 同一招。畫面和觸控區域不可能對不上，因為是同一個函式算的。
- **不用處理連發**：`vInputTask` 一次按壓只送一個事件（按下→送→等放開→清假通知），按住不放不會連續輸入。這是當初為了 XPT2046 PENIRQ 自我觸發做的設計，這裡白賺。
- 換 layer 時**整列先清乾淨再畫**：符號層第 2 列只有 6 鍵、小寫層 7 鍵，不清會留殘字。
- `KB_MAX 60`，剛好塞得進 `g_send_text[64]` 與 `cmd[80]`（`"SEND " + 60 + "\r\n"`）。

**為什麼現在才敢讓 Notes 寫 SD**：`f_write` 跑在 UITask，NetTask 也可能同時碰 SD。這在上次開 `_FS_REENTRANT` 之前會直接踩壞檔案系統（就是寶可夢卡開頭那次）。現在 FatFs volume 鎖會自動排隊，才敢做。

**順手評估了中文（結論：暫緩）**：
- **顯示不難**。16×16 點陣字，用 **Unicode CJK 區間 U+4E00–U+9FFF** 做成 SD 上的字型檔，`offset = (cp - 0x4E00) * 32` 純算術索引，656 KB，不需要 Big5 對照表。再抄 `gb_cache` 做個 128 格 glyph 快取（4 KB RAM），`DrawString` 改成變寬（ASCII 前進 8、中文前進 16）。
- 目前 flash 只用了 **136 KB / 1 MB**，塞 flash 也行，但會吃掉 loader 的空間，放 SD 較合理。
- **難的是輸入法**：注音→候選字字典又大又雜。繞路方案是「ESP32 當 IME 後端」（`IME <注音>` → `IMC <候選字>`），字典放它的 flash。
- 決定先不做。

**順手清掉 `net_download`**：W5 那套 chunk+ack 下載協定驗證過能用，但一直沒接到任何 UI 動作，留著只換來一個未使用警告。連同只有它在用的 `esp_read_line` / `esp_read_bytes` 一起移除。

- 檢查過**沒有任何 module 需要網路** —— syscall 表只有 `fill_rect / draw_str / delay_ms / is_touched / read_file / list_dir`，現有的 hello / snake / tetris / chip8 都用不到。所以「給 module 的網路 syscall」也從待辦拿掉，不是有東西在等它。
- ESP32 端的 `DL <url>` 保留（不佔 STM32 資源），要復原 STM32 端就 `git show c04cb1b -- Core/Src/main.c`。
- NetTask 堆疊維持 1024 words。當初是為了 `net_download` 裡的 `FIL`+512B buffer 從 512 加上去的，現在高水位量到只用 ~260 words，但先留餘裕不動它。

**下一步**：待定。剩下的想法：選單捲動（>6 ROM）、中文顯示、錄 demo 影片。

### 2026-08-03(續二）｜ Discord 訊息 App —— 這台機器會傳訊息了

**目標**：專案重點是「通訊」，讓掌機能跟 Discord 雙向傳訊息（不是只會抓資料）。

**做了什麼**：
- **ESP32 ⇄ Discord（REST API）**：bot token + `discord.com/api/v10`，`WiFiClientSecure`(setInsecure) + ArduinoJson 7.x。
  - 收：ESP32 **自己**每 3 秒 `GET /channels/{id}/messages?after={lastId}`，放進自己的 8 格佇列。第一次用 `?limit=1` 只取基準 ID（否則開機會把歷史訊息全倒出來）；略過 `author.bot`（否則自己送的 3 秒後又收回來，無限自問自答）。
  - 發：`POST /channels/{id}/messages`，body `{"content": ...}`。
- **UART 協定擴充**：`MSG?` → `MSG <user>: <text>` / `MSGNONE`；`SEND <text>` → `SENDOK` / `SENDERR`。
- **STM32 端**：`g_msgs[8][40]` ring buffer + `g_msg_n` / `g_msg_unread`；vNetTask 每秒 `MSG?` 取件（`MSGNONE` 不印，否則 debug 被洗版）；`Msg_Send()` 非阻塞排隊，由 NetTask 送出（按鈕不直接碰 UART，畫面不卡）。
- **Messages 畫面**（`screen_msg.c`）：最近 8 則、使用者名稱青色/內容白色、3 個預設發送鈕（Hi / OK / **Info** —— Info 會送出開機秒數+目前天氣，證明訊息真的來自這台機器）、送出狀態顯示。
- **首頁**：第 6 格加 `icon_msg` 信封圖示（手繪 16×16 點陣），右上角**紅色未讀數字徽章**。

**設計取捨**：
- 用 **REST 輪詢**而非 WebSocket Gateway —— Gateway 對 ESP32 偏重，輪詢 3 秒延遲對聊天完全夠用。
- **輪詢放 ESP32 自己做**，STM32 只用便宜的 `MSG?` 取件 → 網路節奏(rate limit)由 ESP32 管，兩邊解耦。

**限制（已知）**：
- 螢幕字型只有 ASCII，**中文訊息會被整個濾掉**。要中文得做點陣中文字庫 + UTF-8 解碼，暫不處理。
- 一則訊息一行 40 字元，超過截斷。
- 只能發預設短句 —— 任意文字要等螢幕鍵盤。

**踩到的坑**：
- Discord Developer Portal 的 **MESSAGE CONTENT INTENT 沒開的話，讀到的 `content` 是空字串**（不會報錯，超難查）。
- 錯誤碼：401=token 錯、403=bot 沒進伺服器/沒權限、404=頻道 ID 錯。

**下一步**：螢幕小鍵盤（能打任意文字，順便讓 Notes 從唯讀變可寫）。

### 2026-08-03(續）｜ 修 FatFs 併發：開啟 reentrant volume 鎖

**目標**：讓多個 task 存取 SD 不再踩爛（前一則的大坑），才能安全重開下載。

**做了什麼**：
- 開 FatFs 內建的 volume 鎖(`_FS_REENTRANT=1`)而非手動在 11 個 call site 加鎖 —— FatFs 會**自動在每個 `f_*` 進出上鎖/解鎖**，一個都不會漏、call site 零修改。
- `ffconf.h`：`_FS_REENTRANT 1`、`_SYNC_t = SemaphoreHandle_t`、include FreeRTOS.h/semphr.h。
- `option/syscall.c`：4 個 sync 函式(ff_cre/del_syncobj、ff_req/rel_grant)從 CMSIS-RTOS 改成原生 FreeRTOS(`xSemaphoreCreateMutex/Take/Give`)。
- 時序確認：volume mutex 在 `f_mount` 建立，而 f_mount 在 `SD_SelfTest`(vUITask，排程器啟動後)才跑 → 安全。

**學到 / 筆記**：
- FatFs 本來就有 thread-safe 機制，只是預設關；用它比手動包鎖更不易出錯。
- 每個 `f_*` 各自上鎖(非整段)，所以下載中 GB/UI 的 SD 存取仍能在安全點交錯，不互卡。

**驗證（壓力測試，已通過）**：
- 作法：兩個**同優先級**的 task 各自「寫可驗證 pattern → 讀回 → 逐 byte 比對」，靠時間片輪轉在 `f_*` 中途被切走，逼出真實併發。在 `ff_req_grant` 裝計數器：先用 0 timeout 試拿鎖，拿不到 → `ff_contend_n++`（代表**真的**有別的 task 正握著鎖）。
- 結果：靜置 ~10,100 次迭代 / **10,206 次真實併發競爭** / **MISMATCH=0** / fail=0；再加玩 GB（高優先級 SD 使用者）仍 **MISMATCH=0**；拔卡 `chkdsk` **零檔案錯誤**（對比出事那次會具名 `\GB\red.gb 大小不正確`）。
- chkdsk 的「128KB 遺失鏈結」是**燒錄/重置打斷寫入**造成的孤兒叢集（等同拔 USB 不退出），與併發無關。
- 測試碼保留在 main.c，`#define SD_STRESS_TEST` 設 0 關閉，改 1 可重跑。

**意外收穫：`_FS_LOCK` 2 → 6**
壓測時玩 GB 會噴 `FR_TOO_MANY_OPEN_FILES(18)`。原因是 `_FS_LOCK=2`（同時可開檔上限 2），而 GB 遊玩期間 `gb_fil` 常駐佔 1 個名額，再加兩個 stress task 就爆。這對之後「下載時同時用其他 SD 功能」是真實地雷，先調成 6（每個名額僅 8 bytes）。注意這是**獨立於 reentrancy 的另一個機制**，不是資料損壞（FatFs 在開檔前就乾淨拒絕）。

**下一步**：把 net_download 接到 UI(手動觸發下載)。

### 2026-08-03 ｜ W5 無線下載到 SD（跑通）+ 堆疊監測 + 踩到 FatFs 併發大坑

**目標**：讓 ESP32 從網路抓檔，透過 UART 傳給 STM32 寫進 SD 卡。

**做了什麼**：
- **下載協定（chunk + ack 流量控制）**：STM32 送 `DL url` → ESP32 回 `BEGIN` → 迴圈 `C <len>`+裸資料，STM32 收一塊寫一塊回 `A`，直到 `C 0`。每塊自帶長度（不需事先知道總大小，處理 HTTP/1.0 close-delimited）。**STM32 當節拍器**：寫完 SD 才 ack，ESP32 才送下一塊 → ring buffer 不會爆。
- STM32 端阻塞讀取 helper：`esp_read_line`（收標頭）、`esp_read_bytes`（收裸 byte），沒資料時 `vTaskDelay(1)` 讓出 CPU。
- 驗證：下載 example.com（559 byte）到 SD 成功。
- **堆疊監測工具**：`configCHECK_FOR_STACK_OVERFLOW=2` + `vApplicationStackOverflowHook`、`uxTaskGetStackHighWaterMark`（vNetTask 每 15s 印各 task 剩餘水位）。
- **gb_rom_read 防呆**：檢查 f_lseek/f_read 回傳值，失敗不標記快取（下次重讀）；GB 主迴圈檢查錯誤旗標，出錯顯示 `err/sd` 碼而非無聲空轉。

**遇到的問題（三個真實 bug）**：
1. ack `"A\n"` 的 `\n` 殘留 → ESP32 收到空指令回 `ERR unknown`。改送單一 `"A"`。
2. **NetTask 堆疊溢位**：`net_download` 把 `FIL`(~560B)+`buf[512]` 放堆疊 → 2KB 堆疊爆 → 踩爛記憶體 → **下載後觸控死掉**。改成 static + 堆疊加大。
3. **FatFs 併發踩爛 SD 卡（最大坑）**：下載跑在 NetTask、GB/Photo/Notes 讀 SD 在 UITask，`_FS_REENTRANT=0`（FatFs 非 thread-safe）→ 兩 task 同時碰 FatFs → 踩爛共用視窗緩衝，連**卡上 FAT/目錄項都寫壞**。症狀：Pokémon 卡在開頭、fps 飆幾萬（`gb_rom_read` f_lseek 回 FR_INT_ERR）。chkdsk 抓到 red.gb/gold.gb 大小錯誤 → 重 copy ROM 修復。

**學到 / 筆記**：
- **FatFs 非 thread-safe**：多 task 存取 SD 必須序列化（下一步加 `sd_mutex`）。共用硬體資源一定要鎖。
- 大結構（FIL、buffer）放 static 別放 task 堆疊。
- fps 異常飆高 = 模擬器空轉的訊號，往「資料/讀取錯誤」查。
- 除錯記憶體：`.su` 檔（-fstack-usage）看每函式堆疊、high-water 看 task 實際用量、溢位 hook 當場攔截。

**下一步**：加 `sd_mutex` 序列化所有 SD 存取 → 才安全重新開放下載（目前 net_download 已定義但停用）。

### 2026-08-01 ｜ 網路架構整理 + HTTP GET 抓天氣 + 首頁整合

**目標**：把臨時測試碼收乾淨（獨立 task），並讓手機真的抓網路資料（天氣）。

**做了什麼**：
- **`vNetTask`**：新增獨立 FreeRTOS task（優先級 2，低於 UI/Input），專職管 ESP32 UART。定期送 `TIME?`/`WX?`、撈 ring buffer 組行解析。`vTaskDelay(20ms)` 輪詢讓出 CPU。`g_time_valid` 旗標控制對時節奏（對到前 3s 重試、對到後 60s 校準）。vUITask 恢復乾淨。
- **W3 HTTP GET**：ESP32 新增 `WX?` → 用 `HTTPClient` 抓 `wttr.in/Taipei?format=%l:+%t` → 回一行天氣。ESP32 端濾掉非 ASCII（`°` 是 UTF-8 多位元組，STM32 字型只認 ASCII）。
- **首頁整合**：標題列（海軍藍）排入 Menu(左)｜天氣(黃、中)｜時間(右、每秒跳)。時間邏輯從獨立 Clock app 搬到首頁；APPS[] 移除 CLOCK（剩 5 圖示），main.c 拿掉 `ScreenClock_Register()`。

**遇到的問題**：
- 天氣顯示 `+33蚓`：度數符號 `°`(UTF-8 0xC2 0xB0) 亂碼 → ESP32 端只留 0x20~0x7E 的 ASCII。

**學到 / 筆記**：
- 分 task 的價值：把「可能拖很久」（網路傳輸）跟「要即時」（觸控/畫面）用優先級隔開，長傳輸再久也不卡畫面。
- HAL 是「硬體抽象層」，底下就是戳 SR/DR/CR1；HTTPClient 同理，把 TCP/握手/解析包掉。
- ISR 只搬 byte 進 ring buffer（快進快出），組行/解析放 task（慢工）。
- 局部重畫：只塗自己那塊（背景色要對上，標題列是 NAVY），不碰鄰居、不閃爍。

**下一步**：
- W5 下載檔案到 SD（正好練 UART+DMA）、給 module 用的網路 syscall。
- （天氣圖案暫不做；OTA/bootloader 使用者暫不需要。）

### 2026-07-30 ｜ WiFi 起步：ESP32-S3 協處理器 + UART 連線 + NTP 對時

**目標**：讓手機能上網。用 ESP32-S3 當 WiFi 協處理器（TCP/IP 跑在 ESP32），STM32 走 UART 下高階文字指令。

**做了什麼**：
- 硬體連線：STM32 **USART3, PB10(TX)/PB11(RX)**，AF7，APB1，115200 8N1；ESP32-S3 `Serial1` GPIO18/17；共地。查過腳位確定無衝突。
- ESP32 端（Arduino 自訂韌體）：WiFi STA 連線、NTP 對時（UTC+8）、文字協定指令 `PING`/`WIFI?`/`TIME?`。
- STM32 端接收（手寫驅動練習）：**中斷驅動 + 256B 環形緩衝區**。`HAL_UART_Receive_IT` 上膛 → `HAL_UART_RxCpltCallback` push byte + 重新上膛 → `USART3_IRQHandler`；NVIC 優先級 5。TX 用阻塞 `HAL_UART_Transmit`。
- 文字協定：組行（byte 累積到 `\n`）→ `esp_parse_line` sscanf 解析 → `TIME HH:MM:SS` 算出 `g_clock_offset` → 首頁時鐘加 offset 顯示真實時間。
- 里程碑：W0 單向 → W1 雙向 ring buffer → W2 上網查 IP → W4 NTP 時鐘變準。

**遇到的問題**：
- 編譯 `esp_rx_pop` static 宣告在使用之後 → 隱式宣告衝突 → 加前向宣告。
- 測試中一度觸控失靈 → 移除測試碼再加回又正常 → 判定是燒錄/接線暫時性，非測試碼衝突（測試碼每 2 秒 <2ms，且觸控走獨立 EXTI+InputTask，不受 UI 迴圈影響）。

**學到 / 筆記**：
- 「收」比「送」難：對方發送時機不可控 → 中斷 + ring buffer 解耦（ISR 快進快出、task 慢慢撈）。
- ring buffer head(ISR寫)/tail(task讀) 各碰各的，單生產者單消費者天然免鎖；`volatile` 必要。
- HAL 中斷接收要在回呼裡「重新上膛」才會連續收。
- NVIC 優先級數字 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`(5) 才能在 ISR 用 FromISR API。
- 協定設計成文字行（`\n` 結尾），好除錯、好擴充。

**下一步**：
- 把臨時測試碼搬進獨立 `vNetTask`（長傳輸不拖畫面）。
- W3 HTTP GET（抓天氣/API）、W5 下載檔案到 SD、給 module 用的網路 syscall。

### 2026-07-27~28 ｜ Game Boy 模擬器（Peanut-GB，內建 app）+ 效能調校

**目標**：在 F407 上跑 GB 遊戲。ROM 存 SD（模擬器固定=主機、ROM=卡匣）。

**做了什麼（G0~G2）**：
- **走「編進韌體」路線**（非可載入 module）：GB 模擬器碼 ~19KB 太大塞不進 16KB module 區；編進 Flash 還能用整個 libc。用 **Peanut-GB**（單檔 header）
- **工作記憶體放 CCM**：`gb_s`(~16.7KB WRAM+VRAM+regs) + 32KB ROM + 8KB cart RAM 放 `.ccmram_bss`（linker 加的 NOLOAD 段，startup 不複製、不佔 Flash）。CCM 不可 DMA/執行但當資料 RAM 完美
- **G0**：`screen_gb.c` 從 /GB 讀第一個 .gb 進 CCM → gb_init → 顯示遊戲標題。`tools/check_gb.py` 檢查 ROM 是否 32KB 無 MBC（header 0x147 卡匣類型、0x148 大小）
- **G1**：`lcd_draw_line` callback（160px 低 2bit 灰階 → DMG 綠 RGB565 → BlitBytes）。主迴圈跑 gb_run_frame
- **G2**：觸控 8 鍵。`gb.direct.joypad`（active-low）；十字鍵 + A/B + START/SELECT + QUIT。**卡關過**：一度以為輸入壞掉，除錯發現 joypad 值正確（jFE）→ 其實是「沒 START 鍵無法開始遊戲」；補上 START 就能玩

**效能調校（13fps → 50fps）**：
- **-O0 是主因**：Debug build 預設 -O0，Peanut-GB 直譯器慢 3~5 倍。`screen_gb.c` 加 `#pragma GCC optimize ("O3")` → 13→26fps；專案全域 -O2 →（繪圖非瓶頸，僅 26→27）
- **關掉吃 CPU 的模擬選項**：`PEANUT_GB_HIGH_LCD_ACCURACY 0`（逐像素精確 PPU）+ `PEANUT_GB_12_COLOUR 0`（用不到）→ 27→34fps
- **降回 1x（160×144）**：像素少一半（DMA 減半）+ 控制鈕移到下方不透明（省掉半透明逐像素合成）→ 34→**50fps**。掌機排版：上螢幕、下按鍵
- 確認 ART（prefetch/I-cache/D-cache）本來就開、CPU 確在 168MHz

**學到 / 筆記**：
- ⭐ **瓶頸判斷**：優化繪圖碼（專案 -O2）幾乎沒動 fps → 繪圖不是瓶頸，是**模擬運算**（CPU 直譯）。放大畫面主要加 DMA，而 1x 時 DMA 只佔約 1/4 → 模擬才是天花板
- ⭐ **-O0 vs -O2/O3**：計算密集碼在 Debug -O0 下差 3~5 倍。可用 `#pragma GCC optimize` 只優化單檔、不動專案除錯設定
- **放大 = 更多螢幕像素 = 更多 DMA = 掉 fps**，跟來源解析度/糊不糊無關（DMA 看的是目的地像素數）。1x/50fps 是這塊板子的甜蜜點；真·全速大畫面 GB 要換 H7（有 cache + 高時脈 + 外部 SDRAM）
- **JIT 不可行**：需可執行 RAM 放編譯碼，但 SRAM 滿了、CCM 不可執行（＝當初 loader 的老問題）→ 沒地方放
- **單點觸控**：一次一鍵，同時兩鍵（如按住方向+跳）做不到（電阻式限制）
- **輸入 debug**：joypad 值對但遊戲不動 → 先確認 ROM 是否互動式、有沒有 START；別急著怪輸入路徑

**下一步（可選）**：MBC bank 從 SD 串流（跑 >32KB 遊戲）；存檔（cart RAM 寫回 SD）；ROM 選單（多個 .gb）

---

### 2026-07-24 ｜ CHIP-8 模擬器 + syscall 表擴充（檔案存取）

**目標**：一個 module 跑多個遊戲——寫 CHIP-8 模擬器，ROM 存 SD。做這個自然逼出「module 要能讀檔」→ 擴充 syscall 表。

**做了什麼**：
- **syscall 表長大（ABI 擴充）**：`module_api.h` 的 `syscall_t` **在表尾**加 `read_file(path,buf,max)`、`list_dir(dir,out13,max)`（加尾巴 → 舊 module 前面欄位偏移不變、不會壞）。`loader.c` 補對應 wrapper（包 f_open/f_read、f_opendir/f_readdir）
- **防 memset 連結錯**：build.sh 加 `-fno-tree-loop-distribute-patterns`——GCC 會把清零/複製迴圈自動轉成 `memset`/`memcpy` 呼叫，但 module 是 `-nostdlib` 沒 libc → 連結失敗。這旗標關掉該轉換
- **堆疊加大**：vUITask 1024→2560 words(10KB)，因 CHIP-8 狀態（4KB 記憶體 + 2KB 顯示緩衝 ≈ 6.5KB）放在 module_main 的 local struct（在堆疊上）
- **CHIP-8 模擬器** `tools/module/chip8.c` → CHIP8.BIN（位置無關，2392 bytes）：完整 35 opcode、64×32 顯示放大 4 倍、下方 4×4 觸控鍵盤對應 16 鍵、進來用 `list_dir` 列 `/ROMS` 做 ROM 選單、`read_file` 載 ROM 到 mem+0x200
- **測試 ROM** `tools/gen_testrom.py`：手工組 CHIP-8 機器碼產 `TEST.CH8`（8x4 方塊上下左右彈跳）驗證
- 實測：彈跳方塊正常動、鍵盤顯示、選單/載入都對 ✅

**學到 / 筆記**：
- ⭐ **syscall 表隨需求成長**：真 OS 就是這樣（Linux 300+ syscall）。加在表尾保持 ABI 相容。反組譯 module 看 `ldr [r0,#20]` 就是讀新加的 list_dir
- ⭐ **模擬器 = 執行別的架構的碼**：跟 loader 執行原生碼是同一種思想的延伸（VM）。一個模擬器 module + 一堆 ROM 檔 = 無限遊戲
- **module 無 libc**：`-nostdlib` 下不能用 memset/memcpy/strcpy，要嘛自己寫、要嘛用 `-fno-tree-loop-distribute-patterns` 擋掉編譯器自動生成的呼叫
- **CHIP-8 天生會閃**：XOR 畫 sprite → 移動要先擦再畫，擦的瞬間是空的。真遊戲也閃，非 bug
- **大 module 狀態放堆疊**：目前靠加大 UI task 堆疊撐（CHIP-8 6.5KB OK）；Game Boy（32KB+）就撐不住，之後要給 module 專屬工作記憶體區

**下一步**：
- 抓真 CHIP-8 ROM（PONG/TETRIS/BRIX/INVADERS）丟 /ROMS 玩
- 更多原生遊戲 / Game Boy（需解決大 module 工作記憶體）

---

### 2026-07-24 ｜ 遊戲選單 + Tetris + SD 卡分資料夾

**目標**：把「SD 卡遊戲機」做完整——能列出多個遊戲挑著玩、多一個遊戲、SD 卡整理成資料夾。

**做了什麼**：
- **遊戲選單（launcher）**：`screen_game.c` 從「寫死載入 SNAKE.BIN」改成用 `f_readdir` 掃 `/GAMES` 列出所有 `.BIN` → 點一個 `Loader_RunModule("/GAMES/xxx.BIN")` 啟動 → 結束回選單。加遊戲 = 丟一個 `.BIN` 進 `/GAMES`，韌體不動
- **Tetris module** `tools/module/tetris.c` → `TETRIS.BIN`（位置無關，1832 bytes）：10×20 棋盤 + 7 種方塊（16-bit 遮罩表 `PIECES[7][4]`）+ 消行 + 加速；右側觸控按鈕 `< > ROTATE DROP` + QUIT；狀態全放 local struct
- **SD 卡分資料夾**：`/GAMES`（遊戲 .bin）、`/PHOTOS`（圖片）、`/NOTES`（.txt）。三個 app 的掃描/開檔路徑都改成對應資料夾，開檔時 `strcat` 補完整路徑
- **Notes 升級**：從「寫死讀 test.txt」改成列出 `/NOTES` 所有 `.txt` 的檔案清單（LIST/VIEW 兩模式：點檔名看內文、Back 回清單）
- **build.sh** 輸出 `<NAME>.BIN`；module_api.h 加 `MOD_NAVY`/`MOD_GRAY`

**學到 / 筆記**：
- **launcher 模式**跟相簿的 `f_readdir` 掃描 + 索引選取是同一套（screen_game / screen_photo / screen_notes 三個 app 現在共用這個 pattern）
- **絕對路徑**：`_FS_RPATH=0` 下用 `/GAMES/SNAKE.BIN` 這種從根目錄的絕對路徑；資料夾名也要 8.3（GAMES/PHOTOS/NOTES 大寫）
- **Tetris 4x4 遮罩表**：每方塊每旋轉一個 uint16，bit `0x8000>>(r*4+c)` 表示格子占用 → 迭代畫圖/碰撞都很省
- **增量繪圖**：方塊移動只擦舊 4 格、畫新 4 格；只有消行才 `redraw_board`（200 格），避免每步全畫

**下一步**：
- CHIP-8 模擬器（一個 module 跑多個 ROM）
- 更多原生遊戲（2048、Breakout）
- Game Boy 模擬器（大魔王）

---

### 2026-07-24 ｜ 可載入 module — M3：貪食蛇（第一個 SD 卡上的原生遊戲）

**目標**：把前面 M0~M2a 的 loader 兌現成一個真的能玩、存在 SD 卡上的遊戲。

**做了什麼**：
- **貪食蛇 module** `tools/module/snake.c` → `SNAKE.BIN`（位置無關，1163 bytes，零重定位）
- **繞過 M2b**：所有遊戲狀態放在 `module_main` 的一個 local `Snake` struct（在堆疊上 → 天生位置無關），helper 一律傳 `&g` 指標，完全不用 file-scope 全域變數
- **module 自成一格**：自己跑遊戲迴圈、自己輪詢觸控、自己畫、自己決定退出（return 分數）。只透過 `sys` 表碰硬體（fill_rect/draw_str/delay_ms/is_touched）
- **滑動控制**：每 20ms 輪詢觸控，記住觸碰起點，滑動超過 24px 就依主軸轉向（禁 180° 回頭）；蛇依分數 80~180ms 前進一格。右上角 QUIT 鈕、撞死 GAME OVER
- **啟動器** `screen_game.c`：載入 `SNAKE.BIN`、結束後 `xQueueReset(ui_event_queue)` 清掉遊戲期間 vInputTask 累積的觸控事件、重畫外框顯示分數
- **build.sh** 改成輸出 `<NAME>.BIN`（snake.c → SNAKE.BIN），支援多個 module

**學到 / 筆記**：
- ⭐ **遊戲 = module = SD 上的 .bin**。換遊戲只換 SD 檔、韌體完全不動 —— 真正的「換卡匣」。實測：改 snake.c 重編、只覆蓋 SD 上 SNAKE.BIN、板子退出再進 GAME 就跑新版，不用重燒
- **local struct 繞過 M2b**：遊戲狀態放堆疊(SP 相對→天生位置無關)，就不需要可寫全域變數的 PIC。612+ bytes 的 struct 在 vUITask 4KB 堆疊內沒問題
- **全螢幕 app 模型**：module 執行時佔用 UI task，要自己處理輸入迴圈與退出；退出後主韌體要清觸控佇列（`xQueueReset`）避免 vInputTask 遊戲期間累積的事件在退出後誤觸
- **滑動偵測**：要比「遊戲步進」更密地輪詢觸控（20ms）才追得到軌跡，不能只每步一次

**下一步**：
- 更多遊戲 module（Sokoban 資料驅動、2048…）
- 啟動器升級：`f_readdir` 列出 SD 上所有 `.bin` 讓玩家選（不再寫死 SNAKE.BIN）
- （可選）M2b：可寫全域變數的 PIC

---

### 2026-07-24 ｜ 可載入 module — M2a：位置無關（PIC）+ load anywhere

**目標**：拿掉「module 必須連結+載到固定位址 0x2001C000」的限制，讓同一份 module 載到任意位址都能跑。

**做了什麼**：
- **踩雷修正**：一開始用 `-mropi`/`-mrwpi` → GCC 報 `unrecognized command-line option`。**那是 armclang(ARM Compiler)的選項，GCC 沒有。** GCC 要用 `-fpic`
- **`-fpic` 實測**：對比反組譯——無 PIC 時字串用 `ldr [pc]` 載入**絕對位址** `0x2001c07c`；`-fpic` 後變成 `ldr [pc]（載入相對偏移）→ add rX, pc`（PC 相對），文字池存的是 `R_ARM_REL32` 相對偏移，**連結時算死、執行期零重定位**（`readelf -r` 確認 no relocations）
- **build.sh**：加 `-fpic -fno-jump-tables`；**module.ld** ORIGIN 改 `0`（象徵位置無關），故意不放 `.data/.bss`（有可寫全域變數會連結報錯提醒 → 那要進 M2b）
- **load anywhere（loader.c）**：拿掉固定預留區，改讀進普通 `static uint8_t module_ram[16K] aligned(8)`（位址由 linker 給）、跳 `(uintptr_t)module_ram|1`。**FLASH.ld 還原 RAM 112K→128K**、移除 MODULE region
- **實測**：module 載到 `0x200000a0`（RAM 最開頭，離舊的 0x2001C000 ~113KB），GAME 畫面照樣正確顯示 ✅

**學到 / 筆記**：
- ⭐ **`-mropi/-mrwpi` 是 armclang；GCC 用 `-fpic`**。這是很多人踩的坑
- ⭐ **PIC 的本質**：把「引用自己的資料」從『literal pool 存絕對位址』改成『載入相對偏移 + add PC』。因為 rodata 跟碼在同一份 image、相對距離固定，偏移連結時就算死 → **執行期不需要任何重定位**，loader 載進去直接跳
- **SRAM 可執行**：module 現在住在普通 `.bss` 陣列（0x2000xxxx），Cortex-M4 的 SRAM 區可 fetch 指令，故陣列能當碼跑；aligned(8) 確保文字池字組存取對齊
- **限制**：`-fpic` 只解決唯讀部分。可寫全域變數（`.data/.bss`）要 M2b（`-msingle-pic-base` + loader 設 r9/配 RW 區）

**下一步**：
- M2b：可寫全域變數的位置無關（真遊戲需要狀態）
- M3：把小遊戲（Sokoban）編成 module 存 SD

---

### 2026-07-23 ｜ 可載入 module — M1：syscall 表（module 呼叫主韌體）

**目標**：讓載入的 module 能「做事」——透過一張系統呼叫表呼叫主韌體的畫圖/觸控/延遲。

**做了什麼**：
- **ABI 契約** `Core/Inc/module_api.h`：定義 `syscall_t`（函式指標表：`fill_rect` / `draw_str` / `delay_ms` / `is_touched`）+ RGB565 顏色 + `module_entry_t`。主韌體與 module 共用同一份（module build 加 `-I ../../Core/Inc`）
- **loader** 建 `static const syscall_t g_syscalls`，填入真實函式位址（`ILI9341_FillRect`/`ILI9341_DrawString` 簽章一致可直接指派；`delay_ms`/`is_touched` 包 wrapper 接 `vTaskDelay`/`XPT2046_ReadPixel`）；`entry(&g_syscalls)` 把表指標傳給 module
- **module** `module_main` 簽章改收 `const syscall_t *sys`，用 `sys->fill_rect(...)` / `sys->draw_str(...)` 畫背景+文字+三色塊
- **啟動器** `screen_game.c`：進 GAME 只畫外框 + 啟動 module，module 自己畫內容
- 結果：GAME 畫面出現 module 畫的深藍底 + 兩行字 + 紅綠黃色塊 ✅

**學到 / 筆記**：
- ⭐ **syscall 的本質**：反組譯 module 看到 `ldr r6, [r0, #0]; blx r6` —— module 裡「沒有」主韌體函式的位址，它只是從傳進來的表（r0=sys）讀指標再呼叫。位址是主韌體執行期填的 → 分開編譯的 module 能呼叫主韌體、又免重定位。**這就是 OS syscall 的機制**
- **遊戲 = module**：未來的遊戲就是一個收 `sys` 的 `module_main`，編成 `.bin` 放 SD。`screen_game.c` 只是「點圖示啟動」的殼，不是遊戲本身。遊戲需要更多功能（讀關卡檔/音效/亂數）就往 `syscall_t` 表加 —— 真實 OS 就是這樣長大的（Linux 300+ syscall）
- **ABI 契約**：`syscall_t` struct 佈局兩邊必須一致；改它 = 改 ABI，主韌體和所有 module 都要重編，否則 module 照舊佈局讀指標 → 呼叫錯位址

**下一步**：
- M2：位置無關（PIC `-mropi/-mrwpi` 或重定位表）→ module 載到任意位址、可多個並存，不再綁死 0x2001C000
- 之後 M3：把小遊戲（Sokoban，資料驅動）編成 module 存 SD

---

### 2026-07-23 ｜ 可載入 module — M0：從 SD 卡載入原生碼並執行

**目標**：探討「為何 MCU 不能像電腦/手機把程式存在儲存裝置」後，決定自己做一個 MCU 版 loader —— 把原生機器碼從 SD 卡載入 RAM 執行（＝手刻 OS 的 program loader）。M0 是概念驗證：固定位址、無重定位。

**做了什麼**：
- **切 RAM 給 module**：`FLASH.ld` 把 RAM 128K→112K，預留頂端 16K（MODULE region `0x2001C000`）給可載入的 module。主韌體碰不到那塊。（CCM 不可執行，只接 D-bus，故 module 一定放主 SRAM）
- **module 建置鏈** `tools/module/`：`module.ld`（連結到 0x2001C000，`.entry` section 排最前 → 進入點在 .bin offset 0）、`module_hello.c`（`module_main` return 0x1234）、`build.sh`（用 IDE 內建 arm-none-eabi 工具鏈編 + objcopy 成 `HELLO.BIN`）。反組譯確認 HELLO.BIN = 6 bytes：`movw r0,#0x1234; bx lr`
- **loader** `Core/Src/loader.c`：`Loader_RunModule(path)` —— f_open/f_read 讀進 `(void*)MODULE_BASE` / f_close / `__DSB();__ISB();` / 函式指標 `(entry)(MODULE_BASE|1)` 跳進去。main.c 開機呼叫 + myprintf 印回傳值
- **結果**：序列埠印出 `MODULE: ret=0x1234` ✅

**遇到的問題**：
- 加了 loader 呼叫後，**觸控時好時壞**（有時正常、有時死）。序列埠仍印出 `ret=0x1234`（module 乾淨返回），一度懷疑是接觸不良或燒錄問題

**怎麼解的**：
- 二分法：把 loader 呼叫註解掉 → 觸控恢復穩定 → 確定是這段造成
- 「**時好時壞**」是關鍵線索：燒錄問題會「一直壞」，run-to-run 飄動 = **堆疊在邊緣溢出**（踩壞與否看中斷插入時機）
- 查出 `vUITask` 堆疊只有 **512 words(2KB)**，而 `FIL`（`_FS_TINY=0` → 含 512B 磁區緩衝）約 550B，再加 `f_read` 深層呼叫 + `entry()` 跳轉 → 爆堆疊踩壞 FreeRTOS heap 裡相鄰的 task 堆疊/queue
- 堆疊加大 512→1024 words → 全部功能穩定 ✅

**學到 / 筆記**：
- ⭐ **沒 MMU 也能載入原生碼**：MMU 只是「位址無關」問題的硬體解法之一；沒它可用固定位址（M0）、PIC（-mropi/-mrwpi）或手動重定位。MMU 真正不可取代的是**記憶體保護 + 虛擬記憶體**，不是「載入程式」本身
- **M0「固定位址」的本質**：編譯位址（module.ld ORIGIN）＝載入位址（loader MODULE_BASE），碼裡的絕對位址才會剛好對，躲掉重定位。代價：一次一個、只能載到那個唯一位址
- **Cortex-M 三坑**：跳轉位址要 `|1`（Thumb bit）；寫碼後跳前要 `__DSB();__ISB();`（碼是用「資料寫入」放進 RAM，要讓指令抓取看到並重抓 pipeline）；module 當一般函式呼叫 → 不需 vector table/startup。float ABI 要跟主韌體一致
- **馮紐曼**：`f_read` 把檔案「當資料」寫進一塊 RAM，下一刻那塊 RAM 被「當程式碼」執行 —— 資料與碼同源
- ⭐ **debug 心法：「時好時壞」≠ 燒錄問題**。run-to-run 飄動指向「邊緣性溢出/競態」（踩壞與否看時序）。`FIL`（_FS_TINY=0 含 512B 磁區緩衝）約 550B，別在小堆疊的 task（vUITask 原本才 2KB）上宣告；FatFs 很吃堆疊，task 堆疊要留夠。之後 module 若跑重活，堆疊要再評估

**下一步（loader 路線圖 M1→M3）**：
- M1：syscall 表（函式指標 struct）讓 module 呼叫主韌體功能（畫圖/延遲/讀觸控）
- M2：位置無關（PIC 或重定位表）→ module 載到哪都能跑、可多個並存
- M3：把小遊戲（Sokoban，資料驅動）編成 module 存 SD → 「遊戲存卡上」成真

---

### 2026-07-22 ｜ Photo App — 從 SD 卡讀 24-bit BMP 全螢幕顯示

**目標**：讓板子顯示真實圖片，先從最單純的 BMP 開始。

**做了什麼**：
- 驅動新增 **`ILI9341_BlitBytes(x,y,w,h,buf)`**：把「已是 RGB565 大端序」的像素緩衝直接 DMA 灌進矩形（給圖片用，逐列或整塊）
- **Photo App `screen_photo.c`**：`f_open("test.bmp")` → 讀 54-byte header 解析寬高/資料位移/bpp → 逐列讀 BGR → 轉 RGB565 → `BlitBytes` 上螢幕；點畫面任一處返回
- **測試圖產生器 `tools/genbmp.py`**：Pillow 產 320×240 24-bit BMP，四角不同色（左上紅/右上綠/左下藍/右下黃）+ 漸層 + 中央十字，一眼看出方向/顏色/鏡像對不對
- 驗證：四角顏色與方向全對 ✅
- **JPG 顯示（TJpgDec R0.03）**：官方庫放 `Middlewares/Third_Party/TjpgDec/`（跟 FatFs 同層），`.cproject` 加 include path；config `JD_FORMAT=1`（核心直接輸出 RGB565）、`JD_FASTDECODE=1`（M4 的 32-bit 優化）；`screen_photo.c` 加 JPG 路徑，自己提供兩個 callback（`jpg_in`：從 FIL 讀/跳 bytes；`jpg_out`：每像素 hi/lo swap 後 `BlitBytes`），`jd_prepare`/`jd_decomp` 收尾；`photo_show()` 依副檔名分派 BMP/JPG；`tools/genjpg.py` 產 baseline JPEG 測試圖。驗證：320×240 JPEG 正常顯示，檔案 5KB（BMP 要 230KB）✅

**學到 / 筆記**：
- ⭐ **BMP 三個坑**：① 像素是 **BGR** 不是 RGB ② **bottom-up**（最下列先存，所以 `screenY = h-1-r` 翻回正立）③ 每列 padding 到 4 的倍數（320 寬 ×3 = 960 剛好整除，無 padding）
- header 是 **little-endian**，寬高在 offset 18/22、bpp 在 28、資料起點在 10
- RGB888→565：`((R>>3)<<11) | ((G>>2)<<5) | (B>>3)`；`<<` 優先級高於 `|`，不加括號也對
- 一張 320×240 24-bit BMP = 230KB，真實照片不切實際 → 改 JPG
- ⭐ **TJpgDec = FatFs 的翻版思路**：核心不管硬體，你給 input/output 兩個 callback 把它接到你的來源/去處（等同 diskio glue）。input 的 `buf==NULL` 代表「跳過」要 `f_lseek`；`jd->device` 是 `jd_prepare` 傳的 dev 指標（這裡放 FIL）
- **RGB565 byte order 坑**：`JD_FORMAT=1` 給的是 uint16_t 原生小端，ILI9341 要大端 → `jpg_out` 裡就地 swap（`(p>>8)|(p<<8)`）再 blit
- TJpgDec 只吃 **baseline** JPEG，不支援 progressive（`genjpg.py` 已指定 `progressive=False`）

**下一步**：
- 相簿：`f_readdir` 列出 SD 卡圖檔（.bmp/.jpg），左右滑/點兩側翻頁

---

### 2026-07-21 ｜ 時鐘、SD 卡（手寫 SPI2 + FatFs）、Notes App

**目標**：加更多「像手機」的功能——會走的時鐘、用 SD 卡當硬碟，並讀檔顯示文字。

**做了什麼**：
- **時鐘 App**：新增「定期重繪」基礎建設——`vUITask` 由「無限等觸控」改成「等觸控但最多 200ms 就醒來 `on_render`」，讓動態畫面（時鐘/之後的遊戲）不靠觸控也會更新；時鐘用 FreeRTOS tick 計時，放大字（`DrawBitmapMono` scale 4），只在秒數變了才重畫（dirty check）
- **主選單改資料驅動**：App 改成 `APPS[]` 表 + 索引算位置，加一個 App 只要加一列（現有 5 個）
- **SD 卡（重點）**：**手寫 `MX_SPI2_Init` + `HAL_SPI_MspInit` 的 SPI2 分支**（PB13/14/15 = AF5、PC7 = SD_CS），接上 lab5 留下的 FatFs + `user_diskio_spi.c`（SD-over-SPI）；獨立 SPI2，與顯示/觸控的 SPI1 分開，不必整合 mutex
- **掛載自測**：開機 `f_mount` → 讀 `test.txt` → 印序列埠，成功讀到卡
- **Notes App**：讀 SD 上的文字檔用 `DrawString` 顯示（＝ FatFs 讀檔 + 文字渲染兩個現成零件的組合），第一個真正用到 SD 的 App

**遇到的問題**：
- 專案改名殘留：CubeMX 打不開，因為只有 `lab5.ioc`，內部名稱也還是 lab5 → CubeMX 找不到 `final_project.ioc`
- **這專案不能安全用 CubeMX 重新產生**：`MX_GPIO_Init` 等手寫 init 沒有 USER CODE 標記，一按 Generate 會被清光（顯示/觸控 GPIO 全毀）
- 觸控突然失效：把 `SD_SPI_HANDLE` 改成尚未建立的 `hspi2` → 連結失敗 → 燒不進新韌體、跑舊的
- copy-paste 半改：`MX_SPI2_Init` 整段還是 `hspi1`；MSP 分支的埠還是 `GPIOA`/`AF5_SPI1`

**怎麼解的**：
- `.ioc` 改名成 `final_project.ioc` + 改內部 `ProjectName`（原檔備份 `lab5.ioc.bak`）
- **決定不用 CubeMX regen，改手寫 SPI2**（跟專案風格一致，也正好練週邊設定）；動 CubeMX 前先把 main.c 手寫的 globals/prototype/EXTI callback 搬進 USER CODE 保護區
- `SD_SPI_HANDLE` 先指回 `hspi1`（避免連結失敗），等 `hspi2` 建好再指回去
- 逐一 review 抓出沒改到的 `hspi1` / `GPIOA` / `AF5_SPI1`

**學到 / 筆記**：
- ⭐ **移植 FatFs 的本質**＝提供 diskio 那 5 個磁區讀寫函式，把它接到你的儲存；核心 `ff.c` 不用碰。分三層：SPI 慣例（mode/線）／STM32 硬體（prescaler 是 2 的次方、APB 時脈）／SD 卡規格（≤400kHz 初始化、慢快兩段）——搞清楚哪條規則是誰訂的，換裝置/換晶片才知道什麼會變
- **SPI 沒有規定速度**：SCK = 匯流排時脈 ÷ 2的次方分頻；速度上限由「從裝置規格 + 接線品質」決定，不是 SPI 訂的
- **會大量傳輸的裝置（SD）給獨立匯流排**，省掉跟顯示搶 SPI 的 mutex 麻煩
- **copy 週邊設定的鐵律**：先問「裡面每個舊週邊名（handle / GPIO 埠 / pin / AF）是不是全換了？」——半改不會編譯錯，只會跑起來壞
- **手寫 init 的專案不要 CubeMX regen**（除非先把所有手寫碼包進 USER CODE）；`PA13 = SWDIO`，誤設成 AF 會斷除錯線

**下一步**：
- Notes 加捲動 / 列出多個 `.txt` 選檔
- Photo App（先 Python 轉 raw RGB565，再進階到板上解析 BMP）
- 把 SD `f_mount` 從自測抽成乾淨的開機掛載

---

### 2026-07-17 ｜ DrawString、圖示系統、老手機 UI、計算機

**目標**：把文字渲染收尾，讓介面像一支真的（老）手機，並做出第一個能用的 App。

**做了什麼**：
- **`ILI9341_DrawString()`**：游標逐字推進，支援 `'\n'` 換行與超出右界自動折行（`'\0'` 停迴圈）
- **圖示系統**：`tools/genicons.py` 用 ASCII art 設計 16×16 圖示 → 產出 `icons.c/.h`（`icon_calc` / `icon_game` / `icon_photo`）；驅動新增 **`ILI9341_DrawBitmapMono()`**（寬度可變 `stride=ceil(w/8)`、支援整數倍放大，16×16 ×3 → 48×48）
- **共用 UI 模組 `ui.c` / `ui.h`**：`UI_DrawFrame(title, left, right)`、`UI_DrawCentered()`、`UI_BackTouched()`，加版面常數 `UI_TITLE_H` / `UI_SOFT_Y` / `UI_CONTENT_Y` / `UI_CONTENT_H`
- **主選單改老手機（Series 40）風**：深藍標題列 + 三個圖示（淺灰外框 + 色塊 + 白色圖形 + 下方標籤）+ 底部軟鍵列；calc/game/photo 三個子畫面統一套用同一外框，各自縮到約 20 行
- **計算機**：4×4 keypad（排版與觸控命中共用同一組常數）、兩行顯示（算式 + 結果）、狀態機 `acc` / `cur` / `pending_op`、`calc_apply()` 含除零防護
- 新增顏色 `ILI9341_NAVY` / `LIGHTGRAY` / `DARKGRAY`

**遇到的問題**：
- **一次觸碰被當成很多次**（按 `1` 變成 `111`，按住不放會狂跳）
- 計算機按了運算子後畫面看不出來（只顯示數字）

**怎麼解的**：
- 觸控重複：`vInputTask` 改成三段式收尾——**等手指放開 → 等彈跳 50ms → `ulTaskNotifyTake(pdTRUE, 0)` 清掉累積的假通知**；驅動新增 `XPT2046_IsTouched()`（只讀 T_IRQ 腳，不走 SPI，不搶 mutex）
- 運算子顯示：加 `expr[]` 算式字串（**純顯示用，狀態機不讀**）+ `just_equaled`，改成兩行顯示（上：算式／灰，下：結果／白），算式過長只顯示尾端

**學到 / 筆記**：
- ⭐ **XPT2046 的 PENIRQ 在 ADC 轉換期間會被停用（拉高）**，轉換完才回低 → **「讀座標」這個動作本身就製造一個新的下降緣**，EXTI 設 FALLING 就會自己觸發自己。所以處理完必須把期間累積的通知清掉（`timeout=0` 的 `ulTaskNotifyTake` 不是在等、是在**丟**），否則前面等再久都沒用
- 整數除以零在 Cortex-M4 會進 **HardFault**（不是回傳 inf），除法一定要先擋 `b == 0`
- `'7' - '0'` = 7：ASCII 數字是連續排列的
- **抽象要等重複出現才做**：標題列/軟鍵列重複三次後才抽成 `ui.c`；Button widget 先不做，等 home 圖示 + calc keypad 兩個真實案例累積起來再萃取
- **狀態要靠真實需求掙得存在**：`entering` 是「想像中會用到」→ 實作後發現多餘 → 刪掉；`just_equaled` 是真的解決了一個具體 bug → 留下

**下一步**：
- Button widget 重構（現在有 home 圖示 + calc keypad 兩個真實案例可萃取）
- 按鍵按下的反白回饋
- Game / Photo 仍是 `coming soon` placeholder

---

### 2026-07-16 ｜ 文字渲染起步

**目標**：讓螢幕能畫出文字（`DrawChar` / `DrawString`），這是所有 App 的前提。

**做了什麼**：
- 用 Python + Pillow 從 Consolas 字型產生 8×16 點陣字體 `font8x16[95][16]`（`font.c` / `font.h`），涵蓋 ASCII `0x20`–`0x7E`（含大小寫與符號）
- 寫產生器腳本 `tools/genfont.py`（可先 ASCII 預覽字形，確認後 `--write` 輸出）
- 實作 `ILI9341_DrawChar()`：範圍檢查 → 取字形 → 雙層迴圈逐像素填 RGB565 緩衝區（亮點 `fg`／暗點 `bg`）→ SetWindow 框住字框 → 單次 DMA 送出
- 在 `screen_home` 用逐字迴圈畫出 `hello world`（＝ `DrawString` 的原型）

**遇到的問題**：
- 文字出現在**左下角且上下顛倒**，但按鈕（純色 `FillRect`）看起來一切正常
- 修正方向後，**觸控 X 軸失準**：左上返回鈕要點右上角才會觸發

**怎麼解的**：
- MADCTL `0x68` → `0xE8`（加入 MY bit）：文字恢復左上、正立
- 移除 `XPT2046_ReadPixel()` 中 X 映射多餘的反轉項，使觸控與新顯示方向一致；`ReadPixel` 暫時印 raw+px，觸碰四角驗證對齊後移除 debug
- 關鍵改動：MADCTL `0x68`→`0xE8`、`ReadPixel` X 公式去掉 `SCREEN_W-1-` 反轉

**學到 / 筆記**：
- 點陣字體格式：`font8x16[95][16]`，每字 16 列、每 byte 一列 8 點、MSB 在左
- 高效做法：SetWindow 框住整個字框 → 一次把整塊像素灌出去（同 FillRect），不要逐點 DrawPixel
- **純色方塊會掩蓋顯示方向錯誤**——上下顛倒的純色矩形外觀相同，文字是第一個能暴露方向的內容
- **顯示方向與觸控映射必須共用同一套座標系**：改了 MADCTL 一定要同步檢查／更新觸控映射

**下一步**：
- 把 `hello world` 那段逐字迴圈正式包成 `ILI9341_DrawString()`（含 `\n` 換行、超出寬度處理）
- 主選單加上 App 名稱文字標籤（CALC / GAME / PHOTO）

---

## 6. 已知問題與解法

> 詳細除錯記錄見 `DEBUG_LOG.md`。這裡放簡表方便回顧。

| 問題 | 根因 | 解法 |
|---|---|---|
| 灰色殘影 | MADCTL 缺 MV bit | 改 `0x68`（MX+MV+BGR）|
| HAL_Delay 卡死 | BASEPRI mask 掉 TIM2 | 改用 DWT cycle counter 延遲 |
| 觸控全 0 | SPI 42 MHz 超過 XPT2046 上限 | 讀觸控時動態降頻到 ~1.3 MHz |
| 觸控軸錯亂 | landscape 下 xr/yr 需 swap+flip | 重推映射公式 |
| 觸控偏移 | 預設校正值不符實機 | 四角實測重設 min/max |
| 文字倒立/落在下方 | MADCTL 缺 MY，純色方塊掩蓋方向錯誤 | 改 `0xE8`（加 MY）|
| 改方向後觸控 X 反 | 觸控映射仍為舊 `0x68` | 移除 X 反轉項，重新對齊 |
| 一次觸碰算成很多次 | PENIRQ 在 ADC 轉換時被停用 → 讀取自己製造下降緣 | 等放開 + 彈跳 + 清累積通知 |
| CubeMX 打不開 | 只有 `lab5.ioc`，內部名稱也是 lab5 | 改名 `final_project.ioc` + 改 ProjectName |
| 觸控失效（連結失敗） | `SD_SPI_HANDLE` 指向尚未建立的 `hspi2` | 先指回 `hspi1`，建好 hspi2 再改 |
| 加 loader 後觸控時好時壞 | vUITask 2KB 堆疊被 FIL(550B)+f_read+跳轉爆掉，踩壞相鄰記憶體 | vUITask 堆疊 512→1024 words |

---

## 7. 待辦 / 想法

### 🎮 Game Boy 模擬器（規劃完成，下次開工）
- **走「編進韌體」路線**（不是可載入 module）：GB 模擬器碼 ~20~30KB 太大，塞不進 16KB module 區、SRAM 也滿了 → 放 Flash（1MB）最務實，還拿回整個 libc
- GB = 內建 app（新 `screen_gb.c`）；ROM 放 SD /ROMS → 「模擬器固定 + ROM 可換」＝主機+卡匣
- **記憶體**：碼→Flash；gb_s(WRAM+VRAM+regs ~18KB)+32KB ROM→**CCM**（64KB 幾乎全空）；顯示行緩衝→SRAM
- 用 **Peanut-GB**（單檔 header，callback 式），不自己寫
- 第一版鎖 **32KB 無 MBC ROM（Tetris）**：整顆進 CCM，`gb_rom_read` 一行查表
- **MBC**（大 ROM 分頁晶片）：Peanut-GB 幫你模擬切換，你只做 `gb_rom_read(linear)`；大 ROM 要「bank 從 SD 串流+CCM 快取」＝迷你分頁虛擬記憶體（G3 之後）
- 路線圖：G0 骨架+讀 ROM+gb_init → G1 lcd_draw_line 顯示 → G2 觸控按鈕玩 Tetris → G3 效能/存檔/多 ROM → MBC 串流



- [ ] 中文字體？（先做 ASCII，中文之後再說）
- [ ] 主選單加 App 名稱文字標籤（等 DrawString 好了）
- [ ] 計算機：keypad + 顯示列 + 四則運算
- [ ] 時鐘：RTC or tick 累加
- [ ] 設定頁：背光 PWM 調亮度（PC6）
