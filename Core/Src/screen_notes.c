#include "screen.h"
#include "screen_notes.h"
#include "screen_kb.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include "fatfs.h"
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* Notes — browse and append to .txt files in /NOTES.
 *
 * LIST mode shows the files; VIEW mode shows one file's contents and offers an
 * Add softkey that opens the keyboard and appends the typed line.
 *
 * Writing calls f_open/f_write straight from UITask. That is only safe because
 * _FS_REENTRANT is on: NetTask touches the card too, and before the volume
 * lock existed this pattern corrupted the filesystem. */

#define NOTES_DIR    "/NOTES"
#define NOTES_MAX    6
#define ROW_H        28
#define ROW_Y0       (UI_CONTENT_Y + 8)
#define NOTES_BUFSZ  1024

static char notes_list[NOTES_MAX][13];   /* 8.3 names */
static int  notes_count;
static int  notes_mode;                  /* 0 = LIST, 1 = VIEW */
static int  notes_cur = -1;              /* file shown in VIEW */
static char notebuf[NOTES_BUFSZ];        /* .bss, too big for a stack */

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

/* Keyboard callback: append the line, then reload so it shows immediately. */
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
    f_close(&fil);                              /* flushes the FAT to the card */

    view_file(notes_cur);
}

static void notes_enter(void)
{
    notes_count = scan_txt();
    /* on_enter also fires when the keyboard pops, so return to VIEW rather
     * than dropping back to the list. */
    if (notes_mode == 1 && notes_cur >= 0 && notes_cur < notes_count)
        view_file(notes_cur);
    else
        draw_list();
}

static void notes_touch(uint16_t x, uint16_t y)
{
    if (notes_mode == 1) {
        if (UI_BackTouched(x, y)) { notes_cur = -1; draw_list(); return; }
        /* Add softkey */
        if (y >= UI_SOFT_Y && x < ILI9341_WIDTH / 2) {
            char t[13];
            strcpy(t, notes_list[notes_cur]);
            char *dot = strrchr(t, '.');
            if (dot) *dot = 0;
            Keyboard_Open(t, "", note_append);
        }
        return;
    }
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
