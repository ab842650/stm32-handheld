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
 * Clock. Time comes from the FreeRTOS tick plus the NTP offset; no RTC
 * peripheral is used. Redraws only when the second actually changes.
 *
 * No longer registered as an app — the home title bar shows the time instead.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define CLOCK_SCALE   4                         /* 8x16 glyphs at 4x = 32x64 */

static uint32_t last_shown = 0xFFFFFFFF;

extern volatile uint32_t g_clock_offset;        /* NTP offset */

static void draw_time(uint32_t secs)
{
    uint32_t hh = (secs / 3600) % 24;
    uint32_t mm = (secs / 60) % 60;
    uint32_t ss = secs % 60;

    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);

    uint16_t cw = FONT_WIDTH  * CLOCK_SCALE;
    uint16_t ch = FONT_HEIGHT * CLOCK_SCALE;
    uint16_t tw = (uint16_t)strlen(buf) * cw;
    uint16_t tx = (ILI9341_WIDTH - tw) / 2;
    uint16_t ty = UI_CONTENT_Y + (UI_CONTENT_H - ch) / 2;

    /* DrawString cannot scale, so feed the glyph data to DrawBitmapMono. */
    for (int i = 0; buf[i] != '\0'; i++) {
        const uint8_t *g = font8x16[(uint8_t)buf[i] - FONT_FIRST];
        ILI9341_DrawBitmapMono(tx + i * cw, ty, g, FONT_WIDTH, FONT_HEIGHT,
                               CLOCK_SCALE, ILI9341_GREEN, ILI9341_BLACK);
    }
}

static void clock_enter(void)
{
    UI_DrawFrame("Clock", NULL, "Back");
    last_shown = 0xFFFFFFFF;                    /* force the first redraw */
}

static void clock_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y))
        Screen_Pop();
}

static void clock_render(void)
{
    uint32_t secs = xTaskGetTickCount() / configTICK_RATE_HZ + g_clock_offset;
    if (secs == last_shown) return;
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
