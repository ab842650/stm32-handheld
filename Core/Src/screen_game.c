#include "screen.h"
#include "screen_game.h"
#include "ili9341.h"
#include "ui.h"
#include "loader.h"
#include "fatfs.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Game — 遊戲選單（launcher）
 *
 * 掃 SD 卡根目錄所有 .BIN → 列成選單 → 點一個就 Loader_RunModule 啟動它。
 * 遊戲結束後回到選單。加新遊戲 = 丟一個新的 .BIN 進 SD，選單自動出現，
 * 韌體完全不用動 —— 這就是「換卡匣」。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define GAMES_DIR "/GAMES"              /* 遊戲 .bin 放這個資料夾 */
#define GAME_MAX  6                     /* 選單最多列幾個（塞得下內容區）*/
#define ROW_H     28
#define ROW_Y0    (UI_CONTENT_Y + 8)    /* 第一列 y */

static char bins[GAME_MAX][13];         /* 8.3 檔名 */
static int  bin_count;

extern QueueHandle_t ui_event_queue;    /* 遊戲期間累積的觸控要清掉 */

static int is_bin(const char *name)
{
    size_t n = strlen(name);
    return (n >= 4 && strcasecmp(name + n - 4, ".bin") == 0);
}

/* 掃 SD 根目錄收集 .BIN 檔名 */
static int scan_bins(void)
{
    DIR     dir;
    FILINFO fno;
    int     n = 0;

    if (f_opendir(&dir, GAMES_DIR) != FR_OK) return 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) continue;
        if (!is_bin(fno.fname))   continue;
        strcpy(bins[n], fno.fname);
        if (++n >= GAME_MAX) break;
    }
    f_closedir(&dir);
    return n;
}

/* 畫選單：每列一個遊戲名（去掉 .BIN）*/
static void draw_menu(void)
{
    UI_DrawFrame("Games", NULL, "Back");

    if (bin_count == 0) {
        UI_DrawCentered(UI_CONTENT_Y + UI_CONTENT_H / 2 - 8,
                        "no .BIN on SD card", ILI9341_RED, ILI9341_BLACK);
        return;
    }

    for (int i = 0; i < bin_count; i++) {
        char disp[13];
        strcpy(disp, bins[i]);
        char *dot = strrchr(disp, '.');
        if (dot) *dot = 0;                      /* 去掉 .BIN */
        ILI9341_DrawString(20, ROW_Y0 + i * ROW_H + (ROW_H - 16) / 2,
                           disp, ILI9341_WHITE, ILI9341_BLACK);
    }
}

static void launch(int idx)
{
    char path[32];
    strcpy(path, GAMES_DIR "/");            /* "/GAMES/" */
    strcat(path, bins[idx]);                /* "/GAMES/SNAKE.BIN" */

    int score = Loader_RunModule(path);
    (void)score;
    xQueueReset(ui_event_queue);            /* 清掉遊戲期間累積的觸控 */
    draw_menu();                            /* 回到選單 */
}

static void game_enter(void)
{
    bin_count = scan_bins();
    draw_menu();
}

static void game_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y)) { Screen_Pop(); return; }

    if (y >= ROW_Y0 && y < UI_SOFT_Y) {
        int idx = (y - ROW_Y0) / ROW_H;
        if (idx >= 0 && idx < bin_count) launch(idx);
    }
}

static void game_render(void) {}

void ScreenGame_Register(void)
{
    Screen_Register(SCREEN_GAME, (screen_t){
        .on_enter  = game_enter,
        .on_touch  = game_touch,
        .on_render = game_render,
    });
}
