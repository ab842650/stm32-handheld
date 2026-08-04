#ifndef BSP_ILI9341_H
#define BSP_ILI9341_H

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>

/* ── Wiring ─────────────────────────────────────────────────────────────── */
#define ILI9341_SPI         SPI1

#define ILI9341_CS_PORT     GPIOB
#define ILI9341_CS_PIN      GPIO_PIN_0   /* PB0 */

#define ILI9341_DC_PORT     GPIOB
#define ILI9341_DC_PIN      GPIO_PIN_1   /* PB1 — Data/Command */

#define ILI9341_RST_PORT    GPIOB
#define ILI9341_RST_PIN     GPIO_PIN_2   /* PB2 */

#define ILI9341_WIDTH       320
#define ILI9341_HEIGHT      240

/* ── RGB565: R[15:11] G[10:5] B[4:0] ────────────────────────────────────── */
#define ILI9341_BLACK       0x0000
#define ILI9341_WHITE       0xFFFF
#define ILI9341_RED         0xF800
#define ILI9341_GREEN       0x07E0
#define ILI9341_BLUE        0x001F
#define ILI9341_CYAN        0x07FF
#define ILI9341_MAGENTA     0xF81F
#define ILI9341_YELLOW      0xFFE0
#define ILI9341_ORANGE      0xFD20
#define ILI9341_NAVY        0x0210   /* title / softkey bars */
#define ILI9341_LIGHTGRAY   0xC618   /* icon borders */
#define ILI9341_DARKGRAY    0x4208   /* key faces */

/* ── Commands ───────────────────────────────────────────────────────────── */
#define ILI9341_CMD_SWRESET     0x01   /* Software Reset */
#define ILI9341_CMD_SLPOUT      0x11   /* Sleep Out */
#define ILI9341_CMD_DISPON      0x29   /* Display On */
#define ILI9341_CMD_CASET       0x2A   /* Column Address Set */
#define ILI9341_CMD_PASET       0x2B   /* Page (Row) Address Set */
#define ILI9341_CMD_RAMWR       0x2C   /* Memory Write */
#define ILI9341_CMD_MADCTL      0x36   /* Memory Access Control (rotation/scan order) */
#define ILI9341_CMD_COLMOD      0x3A   /* Pixel Format */

/* ── API ────────────────────────────────────────────────────────────────── */

/** @param hspi  an already-initialised SPI handle (hspi1) */
void ILI9341_Init(SPI_HandleTypeDef *hspi);

/** Target rectangle for the next RAMWR. */
void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

void ILI9341_FillScreen(uint16_t color);
void ILI9341_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/** 8x16 ASCII glyph at (x,y) = top-left. Chars outside 0x20..0x7E draw as space. */
void ILI9341_DrawChar(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);

void ILI9341_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);

/**
 * 1-bpp bitmap, integer-scaled. bit=1 draws fg, bit=0 draws bg.
 * @param bmp    ceil(w/8) bytes per row, MSB = leftmost pixel
 * @param scale  1 = original size; scaled result must not exceed 48x48
 */
void ILI9341_DrawBitmapMono(uint16_t x, uint16_t y, const uint8_t *bmp,
                            uint16_t w, uint16_t h, uint8_t scale,
                            uint16_t fg, uint16_t bg);

/**
 * Blit a pre-formatted pixel buffer (RGB565 big-endian, hi byte first).
 * @param buf  w*h pixels, row-major. Must live in normal SRAM — DMA cannot read CCM.
 */
void ILI9341_BlitBytes(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *buf);

#endif /* BSP_ILI9341_H */
