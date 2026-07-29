#ifndef UI_H
#define UI_H

#include "ili9341.h"
#include <stdint.h>
#include <stdbool.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 共用版面（老手機 / Series 40 風）
 *
 *   y=0            ┌──────────────┐
 *                  │    title     │  標題列（深藍）
 *   UI_CONTENT_Y   ├──────────────┤
 *                  │              │  內容區（各畫面自己畫）
 *   UI_SOFT_Y      ├──────────────┤
 *                  │ left   right │  軟鍵列（深藍）
 *   HEIGHT         └──────────────┘
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define UI_TITLE_H    32
#define UI_SOFT_H     30
#define UI_SOFT_Y     (ILI9341_HEIGHT - UI_SOFT_H)   /* 210 */
#define UI_CONTENT_Y  UI_TITLE_H                     /* 32 */
#define UI_CONTENT_H  (UI_SOFT_Y - UI_TITLE_H)       /* 178 */

/**
 * @brief 在 y 畫一行「水平置中於整個螢幕」的文字
 */
void UI_DrawCentered(uint16_t y, const char *s, uint16_t fg, uint16_t bg);

/**
 * @brief 清畫面並畫出標準外框：標題列 + 底部軟鍵列
 * @param title  標題列文字（置中）
 * @param left   左軟鍵文字，NULL = 不畫
 * @param right  右軟鍵文字，NULL = 不畫
 */
void UI_DrawFrame(const char *title, const char *left, const char *right);

/**
 * @brief 觸控點是否落在右下角的 Back 軟鍵區（軟鍵列的右半邊）
 */
bool UI_BackTouched(uint16_t x, uint16_t y);

#endif /* UI_H */
