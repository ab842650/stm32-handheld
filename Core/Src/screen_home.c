#include "screen.h"
#include "screen_home.h"
#include "ili9341.h"
#include "myprintf.h"
#include "font.h"
#include "icons.h"
#include "ui.h"
#include <string.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 主選單（老手機 / Series 40 風）
 *
 * App 用資料表定義，位置由索引計算 —— 加一個 App 只要在 APPS[] 加一列。
 * 繪圖與觸控命中共用同一組常數，避免畫面與命中區域對不上。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define ICON_W        48    /* 60→48：容納 6 個 App（6*48=288 塞得進 320）*/
#define ICON_H        60
#define ICON_Y        76
#define ICON_BORDER   2
#define ICON_SCALE    3          /* 16x16 圖示放大 3 倍 → 48x48 */
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
    { ILI9341_ORANGE, icon_clock, "CLOCK", SCREEN_CLOCK },
    { ILI9341_CYAN,   icon_notes, "NOTES", SCREEN_NOTES },
    { ILI9341_MAGENTA, icon_game, "GB",    SCREEN_GB    },
};
#define APP_COUNT  ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* 平均分佈：APP_COUNT 個圖示 + (APP_COUNT+1) 個等寬間隙填滿螢幕寬 */
static uint16_t app_x(int i)
{
    uint16_t gap = (ILI9341_WIDTH - APP_COUNT * ICON_W) / (APP_COUNT + 1);
    return gap + i * (ICON_W + gap);
}

/* 畫一個圖示：淺灰外框 + 色塊本體 + 白色圖形 + 下方置中標籤 */
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

static void home_enter(void)
{
    UI_DrawFrame("Menu", "Select", NULL);
    for (int i = 0; i < APP_COUNT; i++)
        draw_icon(app_x(i), &APPS[i]);
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
    /* 主畫面不需要動態更新，留空 */
}

void ScreenHome_Register(void)
{
    Screen_Register(SCREEN_HOME, (screen_t){
        .on_enter  = home_enter,
        .on_touch  = home_touch,
        .on_render = home_render,
    });
}
