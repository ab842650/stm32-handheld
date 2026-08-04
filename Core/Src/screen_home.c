#include "screen.h"
#include "screen_home.h"
#include "ili9341.h"
#include "myprintf.h"
#include "font.h"
#include "icons.h"
#include "ui.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

extern char              g_weather[];      /* updated by NetTask */
extern volatile uint32_t g_clock_offset;   /* NTP offset */
extern volatile uint8_t  g_msg_unread;

/* Title bar: "Menu" left, weather centred, clock right. */
#define BAR_TY   ((UI_TITLE_H - FONT_HEIGHT) / 2)
#define WX_X0    48              /* clear of "Menu" */
#define WX_W     196             /* clear of the clock */

/* Home menu. Apps live in a table and positions come from the index, so adding
 * one is a single row in APPS[]. Drawing and hit-testing share app_x(). */

#define ICON_W        48    /* 48 not 60, so six icons fit in 320 px */
#define ICON_H        60
#define ICON_Y        76
#define ICON_BORDER   2
#define ICON_SCALE    3          /* 16x16 icon -> 48x48 */
#define LABEL_Y       (ICON_Y + ICON_H + 6)

typedef struct {
    uint16_t        color;
    const uint8_t  *bmp;
    const char     *label;
    screen_id_t     screen;
} app_entry_t;

static const app_entry_t APPS[] = {
    { ILI9341_BLUE,   icon_calc,  "CALC",  SCREEN_CALC  },
    { ILI9341_GREEN,  icon_game,  "GAME",  SCREEN_GAME  },
    { ILI9341_RED,    icon_photo, "PHOTO", SCREEN_PHOTO },
    { ILI9341_CYAN,   icon_notes, "NOTES", SCREEN_NOTES },
    { ILI9341_MAGENTA, icon_game, "GB",    SCREEN_GB    },
    { ILI9341_ORANGE, icon_msg,   "MSG",   SCREEN_MSG   },
};
#define APP_COUNT  ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* Even spacing: APP_COUNT icons and APP_COUNT+1 equal gaps fill the width. */
static uint16_t app_x(int i)
{
    uint16_t gap = (ILI9341_WIDTH - APP_COUNT * ICON_W) / (APP_COUNT + 1);
    return gap + i * (ICON_W + gap);
}

static void draw_icon(uint16_t x, const app_entry_t *app)
{
    ILI9341_FillRect(x - ICON_BORDER, ICON_Y - ICON_BORDER,
                     ICON_W + ICON_BORDER * 2, ICON_H + ICON_BORDER * 2,
                     ILI9341_LIGHTGRAY);
    ILI9341_FillRect(x, ICON_Y, ICON_W, ICON_H, app->color);

    uint16_t isz = ICON_SIZE * ICON_SCALE;
    ILI9341_DrawBitmapMono(x + (ICON_W - isz) / 2, ICON_Y + (ICON_H - isz) / 2,
                           app->bmp, ICON_SIZE, ICON_SIZE, ICON_SCALE,
                           ILI9341_WHITE, app->color);

    uint16_t w  = (uint16_t)strlen(app->label) * FONT_WIDTH;
    uint16_t tx = x + (ICON_W - w) / 2;
    ILI9341_DrawString(tx, LABEL_Y, app->label, ILI9341_WHITE, ILI9341_BLACK);
}

/* Unread badge: repaint the icon clean, then overlay the count if non-zero. */
static void draw_msg_badge(void)
{
    int      i = APP_COUNT - 1;           /* MSG is always last */
    uint16_t x = app_x(i);

    draw_icon(x, &APPS[i]);
    if (g_msg_unread > 0) {
        char b[4];
        snprintf(b, sizeof(b), "%u", (unsigned)(g_msg_unread > 9 ? 9 : g_msg_unread));
        uint16_t bw = 14;
        uint16_t bx = x + ICON_W - bw;
        ILI9341_FillRect(bx, ICON_Y, bw, FONT_HEIGHT, ILI9341_RED);
        ILI9341_DrawString(bx + (bw - FONT_WIDTH) / 2, ICON_Y, b,
                           ILI9341_WHITE, ILI9341_RED);
    }
}

static char     wx_shown[48] = "";
static uint32_t tm_shown = 0xFFFFFFFF;
static uint8_t  unread_shown = 0xFF;

static void home_enter(void)
{
    /* Empty title: draw the bar and softkey, then place "Menu" on the left. */
    UI_DrawFrame("", "Select", NULL);
    ILI9341_DrawString(8, BAR_TY, "Menu", ILI9341_WHITE, ILI9341_NAVY);

    for (int i = 0; i < APP_COUNT; i++)
        draw_icon(app_x(i), &APPS[i]);

    wx_shown[0] = '\0';          /* force a full redraw on entry */
    tm_shown = 0xFFFFFFFF;
    unread_shown = 0xFF;
}

static void home_touch(uint16_t x, uint16_t y)
{
    if (y < ICON_Y || y >= ICON_Y + ICON_H) return;

    for (int i = 0; i < APP_COUNT; i++) {
        uint16_t ix = app_x(i);
        if (x >= ix && x < ix + ICON_W) {
            Screen_Push(APPS[i].screen);
            return;
        }
    }
}

static void home_render(void)
{
    /* clock, once per second */
    uint32_t secs = xTaskGetTickCount() / configTICK_RATE_HZ + g_clock_offset;
    if (secs != tm_shown) {
        tm_shown = secs;
        char tb[12];
        snprintf(tb, sizeof(tb), "%02lu:%02lu:%02lu",
                 (unsigned long)((secs / 3600) % 24),
                 (unsigned long)((secs / 60) % 60),
                 (unsigned long)(secs % 60));
        uint16_t w = (uint16_t)strlen(tb) * FONT_WIDTH;
        /* 2 px of slack when clearing, or digits leave fragments */
        ILI9341_FillRect(ILI9341_WIDTH - 8 - w - 2, 0, w + 10, UI_TITLE_H, ILI9341_NAVY);
        ILI9341_DrawString(ILI9341_WIDTH - 8 - w, BAR_TY, tb, ILI9341_WHITE, ILI9341_NAVY);
    }

    if (g_msg_unread != unread_shown) {
        unread_shown = g_msg_unread;
        draw_msg_badge();
    }

    if (strcmp(g_weather, wx_shown) != 0) {
        strcpy(wx_shown, g_weather);
        ILI9341_FillRect(WX_X0, 0, WX_W, UI_TITLE_H, ILI9341_NAVY);
        uint16_t w  = (uint16_t)strlen(g_weather) * FONT_WIDTH;
        uint16_t tx = WX_X0 + (WX_W - w) / 2;
        ILI9341_DrawString(tx, BAR_TY, g_weather, ILI9341_YELLOW, ILI9341_NAVY);
    }
}

void ScreenHome_Register(void)
{
    Screen_Register(SCREEN_HOME, (screen_t){
        .on_enter  = home_enter,
        .on_touch  = home_touch,
        .on_render = home_render,
    });
}
