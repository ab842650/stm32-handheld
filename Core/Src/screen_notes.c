#include "screen.h"
#include "screen_notes.h"
#include "screen_kb.h"
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
 *   VIEW：左軟鍵 Add → 螢幕鍵盤，打完 Send 就把該行 append 進檔案
 *         點 Back → 回 LIST
 *
 * 寫入是在 UITask 裡直接呼叫 f_open/f_write —— 現在安全，因為 FatFs 已經
 * 開了 volume 重入鎖（ffconf.h `_FS_REENTRANT`），跟 NetTask 撞到會自動排隊。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define NOTES_DIR    "/NOTES"
#define NOTES_MAX    6
#define ROW_H        28
#define ROW_Y0       (UI_CONTENT_Y + 8)
#define NOTES_BUFSZ  1024

static char notes_list[NOTES_MAX][13];   /* 8.3 檔名 */
static int  notes_count;
static int  notes_mode;                  /* 0=LIST，1=VIEW */
static int  notes_cur = -1;              /* VIEW 中的檔案索引（給 Add 用）*/
static char notebuf[NOTES_BUFSZ];        /* 內文（.bss，不佔 stack）*/

static void view_file(int idx);

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
    notes_cur  = idx;
    UI_DrawFrame(title, "Add", "Back");

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

/* 鍵盤按 Send 的回呼：把這一行接到目前檔案尾端，再重新載入顯示 */
static void note_append(const char *text)
{
    if (notes_cur < 0 || text == NULL || text[0] == '\0') return;

    char path[32];
    strcpy(path, NOTES_DIR "/");
    strcat(path, notes_list[notes_cur]);

    FIL  fil;
    UINT bw;
    if (f_open(&fil, path, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
        UI_DrawCentered(UI_SOFT_Y - 20, "write failed",
                        ILI9341_RED, ILI9341_BLACK);
        return;
    }
    f_write(&fil, text, (UINT)strlen(text), &bw);
    f_write(&fil, "\n", 1, &bw);
    f_close(&fil);                              /* close 才會把 FAT 寫回卡上 */

    view_file(notes_cur);                       /* 重讀，讓新的一行馬上看到 */
}

static void notes_enter(void)
{
    notes_count = scan_txt();
    /* 從鍵盤 pop 回來時 Screen 會再呼叫一次 on_enter；
     * 若剛才在 VIEW，就回到 VIEW 而不是彈回清單。 */
    if (notes_mode == 1 && notes_cur >= 0 && notes_cur < notes_count)
        view_file(notes_cur);
    else
        draw_list();
}

static void notes_touch(uint16_t x, uint16_t y)
{
    if (notes_mode == 1) {                      /* VIEW */
        if (UI_BackTouched(x, y)) { notes_cur = -1; draw_list(); return; }
        /* 左軟鍵 Add */
        if (y >= UI_SOFT_Y && x < ILI9341_WIDTH / 2) {
            char t[13];
            strcpy(t, notes_list[notes_cur]);
            char *dot = strrchr(t, '.');
            if (dot) *dot = 0;
            Keyboard_Open(t, "", note_append);
        }
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
