#include "ili9341.h"
#include "myprintf.h"
#include "font.h"

/* ── 內部變數 ────────────────────────────────────────────────────────────── */
static SPI_HandleTypeDef  *_hspi;
static SemaphoreHandle_t   _dma_done_sem = NULL;

/* SPI bus mutex，由 main.c 建立 */
extern SemaphoreHandle_t spi_bus_mutex;

/* DWT busy-wait（不依賴 TIM2 中斷，scheduler 啟動前也能用）*/
static void _delay_ms(uint32_t ms)
{
    uint32_t cycles = (SystemCoreClock / 1000UL) * ms;
    uint32_t start  = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {}
}

static void _DMA_Sem_Init(void)
{
    if (_dma_done_sem == NULL) {
        _dma_done_sem = xSemaphoreCreateBinary();
    }
}

/* ── 內部 GPIO 巨集（比函式呼叫快，inline 展開）─────────────────────────── */
#define CS_LOW()   HAL_GPIO_WritePin(ILI9341_CS_PORT,  ILI9341_CS_PIN,  GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(ILI9341_CS_PORT,  ILI9341_CS_PIN,  GPIO_PIN_SET)
#define DC_CMD()   HAL_GPIO_WritePin(ILI9341_DC_PORT,  ILI9341_DC_PIN,  GPIO_PIN_RESET)
#define DC_DATA()  HAL_GPIO_WritePin(ILI9341_DC_PORT,  ILI9341_DC_PIN,  GPIO_PIN_SET)
#define RST_LOW()  HAL_GPIO_WritePin(ILI9341_RST_PORT, ILI9341_RST_PIN, GPIO_PIN_RESET)
#define RST_HIGH() HAL_GPIO_WritePin(ILI9341_RST_PORT, ILI9341_RST_PIN, GPIO_PIN_SET)

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 底層傳輸：WriteCmd / WriteData
 *
 * SPI 傳輸的流程永遠是：
 *   1. CS 拉低（選中裝置）
 *   2. DC 設成 CMD 或 DATA
 *   3. HAL_SPI_Transmit 送位元組
 *   4. CS 拉高（釋放匯流排）
 *
 * 注意：CS 每次傳輸都拉高/拉低，是因為我們用 soft CS（NSS = Software）。
 * 若連續送命令＋資料，可以 CS 保持低電位直到整個序列結束——
 * 見 ILI9341_WriteCmd_Data()。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static void WriteCmd(uint8_t cmd)
{
    CS_LOW();
    DC_CMD();
    HAL_SPI_Transmit(_hspi, &cmd, 1, HAL_MAX_DELAY);
    CS_HIGH();
}

/* 送命令 + 緊接著的多個資料位元組，CS 全程保持低電位（一次片選）*/
static void WriteCmd_Data(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    CS_LOW();

    DC_CMD();
    HAL_SPI_Transmit(_hspi, &cmd, 1, HAL_MAX_DELAY);

    if (len > 0) {
        DC_DATA();
        HAL_SPI_Transmit(_hspi, (uint8_t *)data, len, HAL_MAX_DELAY);
    }

    CS_HIGH();
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ILI9341_Init
 *
 * 初始化流程：
 *   1. 硬體 Reset（RST 腳拉低再拉高）——讓 IC 回到出廠狀態
 *   2. 軟體 Reset（0x01 命令）——雙保險
 *   3. 電源控制暫存器設定（廠商建議值，一般不需要改）
 *   4. 設定色彩格式 0x55 = 16-bit RGB565
 *   5. 設定 MADCTL 0x48：MX=1（水平翻轉）+ BGR=1（硬體顏色順序）
 *      → 讓 (0,0) 在左上角，RGB 順序與螢幕模組一致
 *   6. 設定 Gamma 曲線（廠商預設值，改了畫面色彩會跑掉）
 *   7. Sleep Out → 等 120ms（ILI9341 datasheet 規定）
 *   8. Display On
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void ILI9341_Init(SPI_HandleTypeDef *hspi)
{
    _hspi = hspi;

    /* 1. 硬體 Reset */
    myprintf("ili: rst high\r\n");
    RST_HIGH();
    _delay_ms(50);
    myprintf("ili: rst low\r\n");
    RST_LOW();
    _delay_ms(50);
    myprintf("ili: rst high2\r\n");
    RST_HIGH();
    _delay_ms(200);
    myprintf("ili: hw reset done\r\n");

    /* 2. 軟體 Reset */
    WriteCmd(ILI9341_CMD_SWRESET);
    myprintf("ili: swreset sent\r\n");
    _delay_ms(150);
    myprintf("ili: swreset done\r\n");

    /* 3. 電源控制（廠商建議值） */
    myprintf("ili: power regs\r\n");
    WriteCmd_Data(0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3);
    WriteCmd_Data(0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
    WriteCmd_Data(0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3);
    WriteCmd_Data(0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
    WriteCmd_Data(0xF7, (uint8_t[]){0x20}, 1);
    WriteCmd_Data(0xEA, (uint8_t[]){0x00, 0x00}, 2);

    /* Power Control 1 & 2 */
    WriteCmd_Data(0xC0, (uint8_t[]){0x23}, 1);
    WriteCmd_Data(0xC1, (uint8_t[]){0x10}, 1);

    /* VCOM Control */
    WriteCmd_Data(0xC5, (uint8_t[]){0x3E, 0x28}, 2);
    WriteCmd_Data(0xC7, (uint8_t[]){0x86}, 1);

    /* 4. 色彩格式：16-bit RGB565 */
    WriteCmd_Data(ILI9341_CMD_COLMOD, (uint8_t[]){0x55}, 1);

    /* 5. MADCTL：掃描方向 + BGR
     *
     *  Bit7 MY  = Row Address Order（上下翻轉）
     *  Bit6 MX  = Column Address Order（左右翻轉）
     *  Bit5 MV  = Row/Col Exchange（橫直互換）← 這是關鍵
     *  Bit3 BGR = 1 → 模組實際接線是 BGR 順序而非 RGB
     *
     *  0xE8 = 0b11101000 → MY=1, MX=1, MV=1, BGR=1
     *  MV=1：把 portrait（240×320）轉成 landscape（320×240）
     *  MY=1：修正上下翻轉（原本 0x68 少了 MY，文字會倒立且落在下方）
     *  少了 MV 的話 IC 以為寬邊是 240，寫入 320 個像素就會有灰色殘影
     */
    WriteCmd_Data(ILI9341_CMD_MADCTL, (uint8_t[]){0xE8}, 1);

    /* Frame Rate Control：79 Hz */
    WriteCmd_Data(0xB1, (uint8_t[]){0x00, 0x18}, 2);

    /* Display Function Control */
    WriteCmd_Data(0xB6, (uint8_t[]){0x08, 0x82, 0x27}, 3);

    /* 3Gamma disable */
    WriteCmd_Data(0xF2, (uint8_t[]){0x00}, 1);

    /* Gamma set curve 1 */
    WriteCmd_Data(0x26, (uint8_t[]){0x01}, 1);

    /* 6. Gamma 曲線（廠商預設，控制每個亮度級距的非線性補正） */
    WriteCmd_Data(0xE0,
        (uint8_t[]){0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                    0x37,0x07,0x10,0x03,0x0E,0x09,0x00}, 15);
    WriteCmd_Data(0xE1,
        (uint8_t[]){0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                    0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F}, 15);

    /* 7. Sleep Out — datasheet 要求等 120ms 後才能 Display On */
    myprintf("ili: slpout\r\n");
    WriteCmd(ILI9341_CMD_SLPOUT);
    _delay_ms(120);
    myprintf("ili: slpout done\r\n");

    /* 8. Display On */
    WriteCmd(ILI9341_CMD_DISPON);
    _delay_ms(10);
    myprintf("ili: dispon done\r\n");

    /* 建立 DMA 完成 semaphore（必須在 scheduler 啟動後才有效）*/
    _DMA_Sem_Init();
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ILI9341_SetWindow
 *
 * ILI9341 的繪圖模型：先用 CASET + PASET 定義矩形，
 * 再用 RAMWR 開始寫像素——IC 自動在矩形內換行，
 * 不需要每個像素都重設座標（這是效能關鍵）。
 *
 * CASET（Column Address Set, 0x2A）格式：
 *   [SC_hi, SC_lo, EC_hi, EC_lo]  → 起始欄、結束欄（各 16-bit big-endian）
 * PASET（Page/Row Address Set, 0x2B）格式同上，但換成列。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    /* Column (X) */
    buf[0] = x0 >> 8;  buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8;  buf[3] = x1 & 0xFF;
    WriteCmd_Data(ILI9341_CMD_CASET, buf, 4);

    /* Row / Page (Y) */
    buf[0] = y0 >> 8;  buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8;  buf[3] = y1 & 0xFF;
    WriteCmd_Data(ILI9341_CMD_PASET, buf, 4);

    /* 開始寫入（後續 WriteData 的資料都會填到這個視窗）*/
    WriteCmd(ILI9341_CMD_RAMWR);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * DMA-based FillScreen
 *
 * 流程：
 *   1. 準備一列像素資料（640 bytes）放進 line_buf（普通 SRAM，DMA 可存取）
 *   2. SetWindow 告訴 IC 畫全螢幕
 *   3. 每列用 HAL_SPI_Transmit_DMA 非阻塞發出
 *   4. 用 volatile flag 等 DMA 完成，再發下一列
 *
 * 注意：line_buf 必須在普通 SRAM（不能放 CCM），DMA 才能讀到。
 *       靜態 local 變數預設放 .bss，會在普通 SRAM，沒問題。
 *
 * 現在用 volatile flag busy-wait，之後加 FreeRTOS 後改成：
 *   xSemaphoreTakeFromISR → CPU 在等待期間可以做其他事
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * DMA 完成通知：Binary Semaphore
 *
 * 為什麼用 Semaphore 而不是 volatile flag？
 *
 * volatile flag：  CPU busy-wait → 100% CPU 使用率，其他 task 搶不到時間
 * Binary Semaphore：Task 呼叫 xSemaphoreTake → 進入 Blocked 狀態 → CPU
 *                   去跑其他 task → DMA 完成 IRQ → xSemaphoreGiveFromISR
 *                   → FreeRTOS 把 UI Task 移回 Ready queue
 *
 * 結果：DMA 傳輸期間 CPU 使用率接近 0%，其他 task 正常運作。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
/* HAL DMA 完成 callback（在 IRQ context 執行）
 * xSemaphoreGiveFromISR：ISR 安全版的 Give，
 * pxHigherPriorityTaskWoken 告訴 FreeRTOS 要不要立刻切換 task */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1 && _dma_done_sem != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(_dma_done_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* CHUNK_ROWS：每次 DMA 傳幾列。越大 → DMA 次數越少 → overhead 越少。
 * 8 列 = 5120 bytes，30 次 DMA，記憶體佔用合理。                         */
#define CHUNK_ROWS  8
#define CHUNK_BYTES (ILI9341_WIDTH * 2 * CHUNK_ROWS)   /* 5120 bytes */

/* 內部 DMA flush：在已取得 mutex、CS 拉低後呼叫，送 total_pixels 個像素 */
static void _DMA_Flush(uint8_t *buf, uint16_t buf_pixels, uint32_t total_pixels)
{
    uint32_t sent = 0;
    while (sent < total_pixels) {
        uint16_t n = (total_pixels - sent >= buf_pixels) ? buf_pixels
                                                         : (uint16_t)(total_pixels - sent);
        HAL_SPI_Transmit_DMA(_hspi, buf, n * 2);
        xSemaphoreTake(_dma_done_sem, portMAX_DELAY);
        sent += n;
    }
}

void ILI9341_FillScreen(uint16_t color)
{
    static uint8_t chunk_buf[CHUNK_BYTES];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < ILI9341_WIDTH * CHUNK_ROWS; i++) {
        chunk_buf[i * 2]     = hi;
        chunk_buf[i * 2 + 1] = lo;
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(chunk_buf, ILI9341_WIDTH * CHUNK_ROWS,
               (uint32_t)ILI9341_WIDTH * ILI9341_HEIGHT);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

void ILI9341_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;
    if (x + w > ILI9341_WIDTH)  w = ILI9341_WIDTH  - x;
    if (y + h > ILI9341_HEIGHT) h = ILI9341_HEIGHT - y;

    /* 用一列 buffer（不需要整個矩形）*/
    static uint8_t row_buf[ILI9341_WIDTH * 2];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < w; i++) {
        row_buf[i * 2]     = hi;
        row_buf[i * 2 + 1] = lo;
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + w - 1, y + h - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(row_buf, w, (uint32_t)w * h);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ILI9341_DrawPixel
 *
 * 單點繪圖：SetWindow 到 1×1 的矩形，送 2 bytes 顏色。
 * 效能較差（每次都要送 CASET + PASET + RAMWR + 2 bytes），
 * 適合偶爾用，不適合大量繪圖。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;

    ILI9341_SetWindow(x, y, x, y);

    uint8_t buf[2] = {color >> 8, color & 0xFF};
    CS_LOW();
    DC_DATA();
    HAL_SPI_Transmit(_hspi, buf, 2, HAL_MAX_DELAY);
    CS_HIGH();
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ILI9341_DrawChar
 *
 * 把一個 ASCII 字元畫在 (x, y)（字元左上角），前景 fg、背景 bg。
 * 字形來自 font8x16[]：每字 16 bytes，g[row] 是第 row 列，bit7 = 最左像素。
 *
 * 作法：先在 buf[] 把整個 8×16 = 128 個像素的 RGB565 顏色鋪好
 *       （亮點填 fg、暗點填 bg），再 SetWindow 到字元矩形、一次 DMA 送出。
 *       一個字只送一次 DMA，比逐點 DrawPixel 快得多。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void ILI9341_DrawChar(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    /* ── 1. 範圍檢查 ─────────────────────────────────────────────────── */
    if ((uint8_t)c < FONT_FIRST || (uint8_t)c > FONT_LAST) {
        c = ' ';                                   /* 不支援的字元用空白代替 */
    }
    if (x + FONT_WIDTH > ILI9341_WIDTH ||          /* 整個字超出螢幕就不畫 */
        y + FONT_HEIGHT > ILI9341_HEIGHT) {
        return;
    }

    /* ── 2. 拿字形（指向這個字的 16 bytes）───────────────────────────── */
    const uint8_t *g = font8x16[(uint8_t)c - FONT_FIRST];

    /* ── 3. 鋪 pixel buffer（← 你要填的核心迴圈）─────────────────────────
     * buf 存整個字的像素，每像素 2 bytes（RGB565 大端：先 hi 再 lo）。
     * 像素 (row, col) 的線性索引 = row * FONT_WIDTH + col，
     *                  buf 的 byte 偏移 = 索引 * 2。
     *
     * 要做的事：
     *   for row = 0 .. FONT_HEIGHT-1
     *     for col = 0 .. FONT_WIDTH-1
     *       取 g[row] 的第 col 個 bit（bit7 = 最左，即 col=0）
     *       bit == 1 → 這格用 fg，否則用 bg
     *       把選定顏色的 hi、lo 兩個 byte 寫進 buf 對應位置
     */
    static uint8_t buf[FONT_WIDTH * FONT_HEIGHT * 2];


    /* TODO: 你的雙層迴圈寫在這裡 */
    for(int row = 0;row<FONT_HEIGHT;row++){
    	for(int col=0 ; col<FONT_WIDTH;col++){
    	    int bit = (g[row] >> (7 - col)) & 1;
    	    int idx = (row * FONT_WIDTH + col) * 2;
    		uint16_t color = (bit == 1) ? fg : bg;
    		buf[idx]   = color >> 8;   // 送出去
    		buf[idx+1] = color & 0xFF;
    	}
    }


    /* ── 4. SetWindow + DMA 送出（管線已接好，沿用 FillRect 的作法）──── */
    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + FONT_WIDTH - 1, y + FONT_HEIGHT - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(buf, FONT_WIDTH * FONT_HEIGHT, FONT_WIDTH * FONT_HEIGHT);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

void ILI9341_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
	uint16_t cx = x;
	uint16_t cy = y;

	for(int i = 0;str[i]!='\0';i++){
		if(str[i]=='\n'){          /* 換行字元：回行首、下移一行，不畫 */
			cx=x;
			cy+=FONT_HEIGHT;
			continue;
		}
		if(cx + FONT_WIDTH > ILI9341_WIDTH){  /* 這行右邊沒空間 → 折到下一行（字仍要畫）*/
			cx=x;
			cy+=FONT_HEIGHT;
		}
		ILI9341_DrawChar(cx,cy,str[i],fg,bg);
		cx+=FONT_WIDTH;
	}


}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ILI9341_DrawBitmapMono
 *
 * 跟 DrawChar 同一個套路：取 bit → 選 fg/bg → 填 buffer → SetWindow + DMA。
 * 差別只有兩點：
 *   1. 寬度可變 → 每列的 byte 數 stride = ceil(w/8)，不再固定 1
 *   2. 支援放大 → 1 個來源像素展開成 scale×scale 的方塊
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define BMP_MAX_PX  (48 * 48)
static uint8_t bmp_buf[BMP_MAX_PX * 2];

void ILI9341_DrawBitmapMono(uint16_t x, uint16_t y, const uint8_t *bmp,
                            uint16_t w, uint16_t h, uint8_t scale,
                            uint16_t fg, uint16_t bg)
{
    if (scale == 0) return;

    uint16_t ow = w * scale;                  /* 輸出寬 */
    uint16_t oh = h * scale;                  /* 輸出高 */
    if ((uint32_t)ow * oh > BMP_MAX_PX) return;               /* buffer 放不下 */
    if (x + ow > ILI9341_WIDTH || y + oh > ILI9341_HEIGHT) return;

    uint16_t stride = (w + 7) / 8;            /* 來源每列幾個 byte */

    for (uint16_t sy = 0; sy < h; sy++) {
        for (uint16_t sx = 0; sx < w; sx++) {
            /* 取出來源像素的 bit（MSB 在左，同 font） */
            int bit = (bmp[sy * stride + (sx >> 3)] >> (7 - (sx & 7))) & 1;
            uint16_t color = bit ? fg : bg;

            /* 把這 1 個來源像素放大成 scale×scale 區塊 */
            for (uint8_t dy = 0; dy < scale; dy++) {
                uint32_t row = (uint32_t)(sy * scale + dy) * ow;
                for (uint8_t dx = 0; dx < scale; dx++) {
                    uint32_t idx = (row + sx * scale + dx) * 2;
                    bmp_buf[idx]     = color >> 8;
                    bmp_buf[idx + 1] = color & 0xFF;
                }
            }
        }
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + ow - 1, y + oh - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(bmp_buf, ow * oh, (uint32_t)ow * oh);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ILI9341_BlitBytes
 *
 * 跟 FillRect 同一套（SetWindow → DMA），差別只在資料由呼叫端提供、
 * 且已經是 RGB565 大端序（hi,lo）。用於圖片：呼叫端每轉好一列就送一列。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void ILI9341_BlitBytes(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *buf)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + w - 1, y + h - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush((uint8_t *)buf, (uint32_t)w * h, (uint32_t)w * h);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}


