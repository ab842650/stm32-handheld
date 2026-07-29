#include "screen.h"
#include "screen_clock.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 時鐘
 *
 * 計時來源：FreeRTOS tick（xTaskGetTickCount）→ 開機至今的秒數。
 * 目前從 00:00:00 起算（之後可換成 STM32 RTC 週邊保留真實時間）。
 *
 * 更新方式：靠 vUITask 的定期 on_render（每 UI_TICK_MS 一次）。
 * 但每次都重畫會閃、也浪費 —— 所以只有「秒數真的變了」才重畫。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define CLOCK_SCALE   4                         /* 8x16 字放大 4 倍 → 32x64 */

static uint32_t last_shown = 0xFFFFFFFF;        /* 上次顯示的秒數，強制第一次重畫 */

/* 用放大字置中畫出 "HH:MM:SS" */
static void draw_time(uint32_t secs)
{
    uint32_t hh = (secs / 3600) % 24;
    uint32_t mm = (secs / 60) % 60;
    uint32_t ss = secs % 60;

    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);

    uint16_t cw = FONT_WIDTH  * CLOCK_SCALE;    /* 每字寬 32 */
    uint16_t ch = FONT_HEIGHT * CLOCK_SCALE;    /* 每字高 64 */
    uint16_t tw = (uint16_t)strlen(buf) * cw;   /* 整串寬 */
    uint16_t tx = (ILI9341_WIDTH - tw) / 2;
    uint16_t ty = UI_CONTENT_Y + (UI_CONTENT_H - ch) / 2;

    /* 逐字用 DrawBitmapMono 放大（DrawString 沒有放大，改用字形資料直接畫）*/
    for (int i = 0; buf[i] != '\0'; i++) {
        const uint8_t *g = font8x16[(uint8_t)buf[i] - FONT_FIRST];
        ILI9341_DrawBitmapMono(tx + i * cw, ty, g, FONT_WIDTH, FONT_HEIGHT,
                               CLOCK_SCALE, ILI9341_GREEN, ILI9341_BLACK);
    }
}

static void clock_enter(void)
{
    UI_DrawFrame("Clock", NULL, "Back");
    last_shown = 0xFFFFFFFF;                    /* 進來強制重畫一次 */
}

static void clock_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y))
        Screen_Pop();
}

/* 由 vUITask 定期呼叫；只有秒數變了才重畫 */
static void clock_render(void)
{
    uint32_t secs = xTaskGetTickCount() / configTICK_RATE_HZ;
    if (secs == last_shown) return;             /* 同一秒 → 不動 */
    last_shown = secs;
    draw_time(secs);
}

void ScreenClock_Register(void)
{
    Screen_Register(SCREEN_CLOCK, (screen_t){
        .on_enter  = clock_enter,
        .on_touch  = clock_touch,
        .on_render = clock_render,
    });
}
