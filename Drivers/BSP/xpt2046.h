#ifndef BSP_XPT2046_H
#define BSP_XPT2046_H

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Wiring ─────────────────────────────────────────────────────────────── */
#define XPT2046_CS_PORT    GPIOC
#define XPT2046_CS_PIN     GPIO_PIN_4   /* T_CS */
#define XPT2046_IRQ_PORT   GPIOC
#define XPT2046_IRQ_PIN    GPIO_PIN_5   /* T_IRQ, low = touched */

/* Calibration: raw ADC range that maps onto the screen. Measured on this
 * panel by printing XPT2046_ReadRaw() while touching each corner. */
#define XPT2046_X_MIN    458
#define XPT2046_X_MAX   3590
#define XPT2046_Y_MIN    370
#define XPT2046_Y_MAX   3839

#define XPT2046_SCREEN_W  320
#define XPT2046_SCREEN_H  240

void XPT2046_Init(SPI_HandleTypeDef *hspi);
bool XPT2046_ReadRaw(uint16_t *x_raw, uint16_t *y_raw);
bool XPT2046_ReadPixel(uint16_t *x_px, uint16_t *y_px);

/** Reads T_IRQ only, no SPI traffic — safe to poll while waiting for release. */
bool XPT2046_IsTouched(void);

#endif /* BSP_XPT2046_H */
