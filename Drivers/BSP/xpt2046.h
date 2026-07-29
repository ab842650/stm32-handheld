#ifndef BSP_XPT2046_H
#define BSP_XPT2046_H

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>
#include <stdbool.h>

/* ── 硬體接線 ────────────────────────────────────────────────────────────── */
#define XPT2046_CS_PORT    GPIOC
#define XPT2046_CS_PIN     GPIO_PIN_4   /* T_CS */
#define XPT2046_IRQ_PORT   GPIOC
#define XPT2046_IRQ_PIN    GPIO_PIN_5   /* T_IRQ，低電位表示有觸碰 */

/* ── 校正值（原始 ADC → 像素座標的映射範圍）────────────────────────────────
 * 先用這組預設值，若對不準再用 XPT2046_ReadRaw() 印出實際值後修正。      */
#define XPT2046_X_MIN    458
#define XPT2046_X_MAX   3590
#define XPT2046_Y_MIN    370
#define XPT2046_Y_MAX   3839

/* 螢幕解析度（與 ILI9341 一致）*/
#define XPT2046_SCREEN_W  320
#define XPT2046_SCREEN_H  240

/* ── 公開 API ────────────────────────────────────────────────────────────── */
void XPT2046_Init(SPI_HandleTypeDef *hspi);
bool XPT2046_ReadRaw(uint16_t *x_raw, uint16_t *y_raw);
bool XPT2046_ReadPixel(uint16_t *x_px, uint16_t *y_px);

/**
 * @brief 目前是否仍被觸碰（直接讀 T_IRQ 腳，不做 SPI 傳輸）
 * @note  T_IRQ 低電位 = 有觸碰。用於「等手指放開」的輪詢。
 */
bool XPT2046_IsTouched(void);

#endif /* BSP_XPT2046_H */
