#ifndef BSP_ILI9341_H
#define BSP_ILI9341_H

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>

/* ── 硬體接線（依 project_spec）────────────────────────────────────────── */
#define ILI9341_SPI         SPI1

#define ILI9341_CS_PORT     GPIOB
#define ILI9341_CS_PIN      GPIO_PIN_0   /* PB0 */

#define ILI9341_DC_PORT     GPIOB
#define ILI9341_DC_PIN      GPIO_PIN_1   /* PB1 — Data/Command */

#define ILI9341_RST_PORT    GPIOB
#define ILI9341_RST_PIN     GPIO_PIN_2   /* PB2 */

/* ── 螢幕尺寸 ────────────────────────────────────────────────────────────── */
#define ILI9341_WIDTH       320
#define ILI9341_HEIGHT      240

/* ── RGB565 常用顏色 ─────────────────────────────────────────────────────── */
/* RGB565: R[15:11] G[10:5] B[4:0]，用 RGB888 換算：                        */
/*   R5 = R8 >> 3,  G6 = G8 >> 2,  B5 = B8 >> 3                            */
#define ILI9341_BLACK       0x0000
#define ILI9341_WHITE       0xFFFF
#define ILI9341_RED         0xF800   /* 11111 000000 00000 */
#define ILI9341_GREEN       0x07E0   /* 00000 111111 00000 */
#define ILI9341_BLUE        0x001F   /* 00000 000000 11111 */
#define ILI9341_CYAN        0x07FF
#define ILI9341_MAGENTA     0xF81F
#define ILI9341_YELLOW      0xFFE0
#define ILI9341_ORANGE      0xFD20
#define ILI9341_NAVY        0x0210   /* 深藍 — 標題列/軟鍵列 */
#define ILI9341_LIGHTGRAY   0xC618   /* 淺灰 — 圖示外框 */
#define ILI9341_DARKGRAY    0x4208   /* 深灰 — 按鍵底色 */

/* ── 常用 ILI9341 命令 ───────────────────────────────────────────────────── */
#define ILI9341_CMD_SWRESET     0x01   /* Software Reset */
#define ILI9341_CMD_SLPOUT      0x11   /* Sleep Out */
#define ILI9341_CMD_DISPON      0x29   /* Display On */
#define ILI9341_CMD_CASET       0x2A   /* Column Address Set */
#define ILI9341_CMD_PASET       0x2B   /* Page (Row) Address Set */
#define ILI9341_CMD_RAMWR       0x2C   /* Memory Write */
#define ILI9341_CMD_MADCTL      0x36   /* Memory Access Control (旋轉/掃描方向) */
#define ILI9341_CMD_COLMOD      0x3A   /* Pixel Format */

/* ── 公開 API ────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化 ILI9341。在 SPI1 已 init 完成後呼叫。
 * @param hspi  指向已初始化的 SPI handle（hspi1）
 */
void ILI9341_Init(SPI_HandleTypeDef *hspi);

/**
 * @brief 設定畫素寫入視窗（下次 RAMWR 的目標矩形）
 */
void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief 用單一顏色填滿整個螢幕（最快的驗測方式）
 */
void ILI9341_FillScreen(uint16_t color);

/**
 * @brief 填滿矩形區域
 */
void ILI9341_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief 畫單一像素
 */
void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief 在 (x, y)（字元左上角）畫一個 8x16 ASCII 字元
 * @param c   要畫的字元（0x20..0x7E，超出範圍以空白代替）
 * @param fg  前景色（字形亮點）
 * @param bg  背景色（字形暗點）
 */
void ILI9341_DrawChar(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);

void ILI9341_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);

/**
 * @brief 畫一張單色點陣圖（bit=1 用 fg，bit=0 用 bg），可整數倍放大
 * @param bmp    點陣資料，每列 ceil(w/8) bytes，MSB 為最左像素
 * @param w,h    來源點陣尺寸（像素）
 * @param scale  放大倍率（1 = 原尺寸）。放大後不可超過 48x48
 */
void ILI9341_DrawBitmapMono(uint16_t x, uint16_t y, const uint8_t *bmp,
                            uint16_t w, uint16_t h, uint8_t scale,
                            uint16_t fg, uint16_t bg);

/**
 * @brief 把一塊「已是 RGB565 大端序（每像素 hi,lo 兩 byte）」的像素緩衝
 *        直接灌進 (x,y,w,h) 矩形。用於顯示圖片（逐列或整塊）。
 * @param buf  w*h 個像素，row-major；須在一般 SRAM（DMA 可讀，勿放 CCM）
 */
void ILI9341_BlitBytes(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *buf);

#endif /* BSP_ILI9341_H */
