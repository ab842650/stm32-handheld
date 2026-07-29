#include "screen.h"
#include "screen_notes.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include "fatfs.h"     /* f_opendir / f_readdir / f_open / f_read */
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Notes — 列出 /NOTES 資料夾裡的 .txt，點一個看內容（類記事本）
 *
 * 兩種模式：LIST（檔案清單）/ VIEW（看內文）。
 *   LIST：點檔名 → 進 VIEW；點 Back → 回主選單
 *   VIEW：點 Back → 回 LIST
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define NOTES_DIR    "/NOTES"
#define NOTES_MAX    6
#define ROW_H        28
#define ROW_Y0       (UI_CONTENT_Y + 8)
#define NOTES_BUFSZ  1024

static char notes_list[NOTES_MAX][13];   /* 8.3 檔名 */
static int  notes_count;
static int  notes_mode;                  /* 0=LIST，1=VIEW */
static char notebuf[NOTES_BUFSZ];        /* 內文（.bss，不佔 stack）*/

static int is_txt(const char *name)
{
    size_t n = strlen(name);
    return (n >= 4 && strcasecmp(name + n - 4, ".txt") == 0);
}

static int scan_txt(void)
{
    DIR     dir;
    FILINFO fno;
    int     n = 0;

    if (f_opendir(&dir, NOTES_DIR) != FR_OK) return 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) continue;
        if (!is_txt(fno.fname))   continue;
        strcpy(notes_list[n], fno.fname);
        if (++n >= NOTES_MAX) break;
    }
    f_closedir(&dir);
    return n;
}

/* 檔案清單 */
static void draw_list(void)
{
    notes_mode = 0;
    UI_DrawFrame("Notes", NULL, "Back");

    if (notes_count == 0) {
        UI_DrawCentered(UI_CONTENT_Y + UI_CONTENT_H / 2 - 8,
                        "no .txt in /NOTES", ILI9341_RED, ILI9341_BLACK);
        return;
    }
    for (int i = 0; i < notes_count; i++) {
        char disp[13];
        strcpy(disp, notes_list[i]);
        char *dot = strrchr(disp, '.');
        if (dot) *dot = 0;
        ILI9341_DrawString(20, ROW_Y0 + i * ROW_H + (ROW_H - 16) / 2,
                           disp, ILI9341_WHITE, ILI9341_BLACK);
    }
}

/* 看某個檔的內文 */
static void view_file(int idx)
{
    char path[32];
    strcpy(path, NOTES_DIR "/");
    strcat(path, notes_list[idx]);

    char title[13];
    strcpy(title, notes_list[idx]);
    char *dot = strrchr(title, '.');
    if (dot) *dot = 0;

    notes_mode = 1;
    UI_DrawFrame(title, NULL, "Back");

    FIL  fil;
    UINT br = 0;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        UI_DrawCentered(UI_CONTENT_Y + UI_CONTENT_H / 2 - 8,
                        "open failed", ILI9341_RED, ILI9341_BLACK);
        return;
    }
    f_read(&fil, notebuf, NOTES_BUFSZ - 1, &br);
    f_close(&fil);
    notebuf[br] = '\0';

    ILI9341_DrawString(4, UI_CONTENT_Y + 4, notebuf,
                       ILI9341_WHITE, ILI9341_BLACK);
}

static void notes_enter(void)
{
    notes_count = scan_txt();
    draw_list();
}

static void notes_touch(uint16_t x, uint16_t y)
{
    if (notes_mode == 1) {                      /* VIEW：Back 回清單 */
        if (UI_BackTouched(x, y)) draw_list();
        return;
    }
    /* LIST */
    if (UI_BackTouched(x, y)) { Screen_Pop(); return; }
    if (y >= ROW_Y0 && y < UI_SOFT_Y) {
        int idx = (y - ROW_Y0) / ROW_H;
        if (idx >= 0 && idx < notes_count) view_file(idx);
    }
}

static void notes_render(void) {}

void ScreenNotes_Register(void)
{
    Screen_Register(SCREEN_NOTES, (screen_t){
        .on_enter  = notes_enter,
        .on_touch  = notes_touch,
        .on_render = notes_render,
    });
}
