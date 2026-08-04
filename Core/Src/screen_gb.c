/* The Peanut-GB interpreter is the hot path; -O3 just for this file. */
#pragma GCC optimize ("O3")

#include "screen.h"
#include "screen_gb.h"
#include "ili9341.h"
#include "xpt2046.h"
#include "ui.h"
#include "fatfs.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

extern QueueHandle_t ui_event_queue;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Game Boy emulator, built into the firmware rather than loaded as a module:
 * the emulator core is far too large for the 16 KB module region.
 *
 * Peanut-GB is a single-header implementation and this is the only file that
 * instantiates it. Working memory lives in CCM, which cannot be reached by DMA
 * and cannot be executed but is otherwise fast and was sitting empty.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Accuracy features we cannot display anyway; disabling them buys frames. */
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#define PEANUT_GB_12_COLOUR         0
#include "peanut_gb.h"

#define GB_BANK_SIZE    (16 * 1024)     /* pinned bank 0 */
#define GB_CARTRAM_MAX  (32 * 1024)     /* Pokemon uses the full 32 KB */

/* ROM cache, shaped like a CPU cache: 32 direct-mapped lines of 512 B.
 * A miss costs one 512 B read (~1 ms) instead of a whole 16 KB bank (~12 ms),
 * and 32 lines can hold the game's scattered hot spots at once. Replacing a
 * single-bank slot with this took Pokemon from single-digit fps to ~60. */
#define GB_LINE_SZ    512
#define GB_LINE_BITS  9
#define GB_NLINES     32                /* 32 × 512B = 16KB */

/* CCM, filled at run time, so the section is NOLOAD. */
static struct gb_s gb          __attribute__((section(".ccmram_bss")));
static uint8_t gb_bank0[GB_BANK_SIZE]  __attribute__((section(".ccmram_bss"))); /* always resident */
static uint8_t gb_cache[GB_NLINES * GB_LINE_SZ] __attribute__((section(".ccmram_bss"))); /* cache for the switchable region */
static uint32_t gb_tag[GB_NLINES];      /* which ROM line each slot holds; 0xFFFFFFFF = empty */
static uint8_t gb_cartram[GB_CARTRAM_MAX];   /* in SRAM; CCM has no room left */
static FIL     gb_fil;          /* stays open for gb_rom_read to page from */

static int gb_err_flag;
static int gb_sd_fail;   /* fr*1000+br from the last failed read */

static void gb_cache_reset(void)
{
    for (int i = 0; i < GB_NLINES; i++) gb_tag[i] = 0xFFFFFFFFu;
}

/* addr is a linear offset into the whole ROM. Below 0x4000 is pinned bank 0;
 * everything else goes through the direct-mapped line cache. */
static uint8_t gb_rom_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
    if (addr < 0x4000) return gb_bank0[addr];

    uint32_t line = addr >> GB_LINE_BITS;              /* line index within the ROM */
    uint32_t set  = line & (GB_NLINES - 1);            /* which cache slot it maps to */
    if (gb_tag[set] != line) {                         /* miss */
        UINT br = 0;
        FRESULT fr = f_lseek(&gb_fil, line << GB_LINE_BITS);
        if (fr == FR_OK)
            fr = f_read(&gb_fil, &gb_cache[set * GB_LINE_SZ], GB_LINE_SZ, &br);
        if (fr == FR_OK && br == GB_LINE_SZ) {
            gb_tag[set] = line;                        /* tag only on success, never cache a failed read */
        } else {
            gb_sd_fail = (int)fr * 1000 + (int)br;     /* leave the slot invalid so it retries */
        }
    }
    return gb_cache[set * GB_LINE_SZ + (addr & (GB_LINE_SZ - 1))];
}
static uint8_t gb_cart_ram_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
    return (addr < GB_CARTRAM_MAX) ? gb_cartram[addr] : 0xFF;
}
static int      gb_ram_dirty;   /* cart RAM written but not yet flushed */
static uint32_t gb_ram_wtime;   /* for the autosave debounce */
static void gb_cart_ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t val)
{
    (void)g;
    if (addr < GB_CARTRAM_MAX) {
        gb_cartram[addr] = val;
        gb_ram_dirty = 1;
        gb_ram_wtime  = HAL_GetTick();
    }
}
static void gb_error_cb(struct gb_s *g, const enum gb_error_e err, const uint16_t addr)
{
    (void)g; (void)addr;
    gb_err_flag = (int)err + 1;
}

/* 1x display up top, opaque controls below, handheld-style. */
#define GB_SW  160
#define GB_SH  144
#define GB_X   ((ILI9341_WIDTH - GB_SW) / 2)   /* 80 */
#define GB_Y   8                                /* leaves room for the buttons */
static const uint16_t GB_SHADE[4] = { 0x9DE1, 0x8D61, 0x3306, 0x09C1 }; /* light to dark */
static uint8_t gb_linebuf[GB_SW * 2];   /* one row, in SRAM so DMA can read it */

/* Touch controls, drawn once below the screen area. */
typedef struct { uint16_t x, y, w, h; uint8_t bit; uint16_t color; const char *label; } gbbtn_t;
static const gbbtn_t GB_BTNS[] = {
    {  42, 155, 28, 28, JOYPAD_UP,     ILI9341_NAVY,     "" },
    {  14, 183, 28, 28, JOYPAD_LEFT,   ILI9341_NAVY,     "" },
    {  70, 183, 28, 28, JOYPAD_RIGHT,  ILI9341_NAVY,     "" },
    {  42, 211, 28, 28, JOYPAD_DOWN,   ILI9341_NAVY,     "" },
    { 268, 168, 42, 42, JOYPAD_A,      ILI9341_RED,      "A" },
    { 212, 190, 42, 42, JOYPAD_B,      ILI9341_GREEN,    "B" },
    { 118, 212, 40, 20, JOYPAD_SELECT, ILI9341_DARKGRAY, "SEL" },
    { 164, 212, 40, 20, JOYPAD_START,  ILI9341_DARKGRAY, "STA" },
};
#define GB_NBTN       (sizeof(GB_BTNS) / sizeof(GB_BTNS[0]))
#define GB_TOUCH_YADJ 10   /* touches read low; shift hit-tests up by this */

/* Screen point -> joypad bit, 0 if it hit nothing. */
static uint8_t gb_button_at(uint16_t sx, uint16_t sy)
{
    for (unsigned i = 0; i < GB_NBTN; i++) {
        const gbbtn_t *b = &GB_BTNS[i];
        if (sx >= b->x && sx < b->x + b->w && sy >= b->y && sy < b->y + b->h)
            return b->bit;
    }
    return 0;
}

/* Drawn once; the game never paints over this area. */
static void draw_controls(void)
{
    for (unsigned i = 0; i < GB_NBTN; i++) {
        const gbbtn_t *b = &GB_BTNS[i];
        ILI9341_FillRect(b->x, b->y, b->w, b->h, b->color);
        if (b->label[0]) {
            int lw = (int)strlen(b->label) * 8;
            ILI9341_DrawString(b->x + (b->w - lw) / 2, b->y + (b->h - 16) / 2,
                               b->label, ILI9341_WHITE, b->color);
        }
    }
    ILI9341_FillRect(284, 2, 36, 18, ILI9341_RED);
    ILI9341_DrawString(286, 3, "QUIT", ILI9341_WHITE, ILI9341_RED);
}

/* Called once per scanline: convert to RGB565 and blit, no scaling. */
static void lcd_draw_line(struct gb_s *g, const uint8_t *pixels, const uint_fast8_t line)
{
    (void)g;
    for (int x = 0; x < GB_SW; x++) {
        uint16_t c = GB_SHADE[pixels[x] & 3];
        gb_linebuf[x * 2]     = c >> 8;
        gb_linebuf[x * 2 + 1] = c & 0xFF;
    }
    ILI9341_BlitBytes(GB_X, GB_Y + line, GB_SW, 1, gb_linebuf);
}

/* ── ROM menu ── */
#define GB_LIST_MAX 6
#define GB_ROW_H    28
#define GB_ROW_Y0   (UI_CONTENT_Y + 8)
static char gb_roms[GB_LIST_MAX][13];
static int  gb_rom_count;

static int scan_roms(void)
{
    DIR dir; FILINFO fno; int n = 0;
    if (f_opendir(&dir, "/GB") != FR_OK) return 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) continue;
        size_t k = strlen(fno.fname);
        if (k >= 3 && strcasecmp(fno.fname + k - 3, ".gb") == 0) {
            strcpy(gb_roms[n], fno.fname);
            if (++n >= GB_LIST_MAX) break;
        }
    }
    f_closedir(&dir);
    return n;
}

static void gb_menu(void)
{
    UI_DrawFrame("GameBoy", NULL, "Back");
    if (gb_rom_count == 0) {
        UI_DrawCentered(UI_CONTENT_Y + UI_CONTENT_H / 2 - 8, "no .gb in /GB",
                        ILI9341_RED, ILI9341_BLACK);
        return;
    }
    for (int i = 0; i < gb_rom_count; i++) {
        char disp[13];
        strcpy(disp, gb_roms[i]);
        char *dot = strrchr(disp, '.');
        if (dot) *dot = 0;
        ILI9341_DrawString(20, GB_ROW_Y0 + i * GB_ROW_H + (GB_ROW_H - 16) / 2,
                           disp, ILI9341_WHITE, ILI9341_BLACK);
    }
}

/* Flush cart RAM to .sav, only if the game has save RAM and it is dirty. */
static void gb_flush_sav(const char *savpath, size_t save_size)
{
    if (save_size == 0 || !gb_ram_dirty) return;
    ILI9341_DrawString(2, 0, "saving...", ILI9341_WHITE, ILI9341_BLACK);
    FIL  sf;
    UINT sbw;
    if (f_open(&sf, savpath, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        f_write(&sf, gb_cartram, save_size, &sbw);
        f_close(&sf);
    }
    gb_ram_dirty = 0;
}

/* Blocks until the player hits QUIT. */
static void gb_run(const char *romname)
{
    /* Keep the file open: gb_rom_read pages from it for the whole session. */
    char path[32];
    strcpy(path, "/GB/");
    strcat(path, romname);
    UINT br = 0;
    if (f_open(&gb_fil, path, FA_READ) != FR_OK) {
        UI_DrawCentered(UI_CONTENT_Y + 40, "ROM open fail", ILI9341_RED, ILI9341_BLACK);
        return;
    }
    f_read(&gb_fil, gb_bank0, GB_BANK_SIZE, &br);
    gb_cache_reset();

    gb_err_flag = 0;
    gb_sd_fail  = 0;
    enum gb_init_error_e e = gb_init(&gb, gb_rom_read, gb_cart_ram_read,
                                     gb_cart_ram_write, gb_error_cb, NULL);

    char line[40];
    if (e != GB_INIT_NO_ERROR) {
        /* 0x147 = cart type, 0x148 = ROM size code */
        snprintf(line, sizeof(line), "init err %d  type=0x%02X size=0x%02X",
                 (int)e, gb_bank0[0x147], gb_bank0[0x148]);
        UI_DrawCentered(UI_CONTENT_Y + 40, line, ILI9341_RED, ILI9341_BLACK);
        UI_DrawCentered(UI_CONTENT_Y + 66, "cartridge unsupported",
                        ILI9341_LIGHTGRAY, ILI9341_BLACK);
        f_close(&gb_fil);
        uint16_t wx, wy;                                  /* wait for a tap */
        while (XPT2046_ReadPixel(&wx, &wy))  vTaskDelay(pdMS_TO_TICKS(30));
        while (!XPT2046_ReadPixel(&wx, &wy)) vTaskDelay(pdMS_TO_TICKS(30));
        return;
    }

    (void)line;

    size_t save_size = 0;
    gb_get_save_size_s(&gb, &save_size);          /* 0 if this game has no save RAM */
    char savpath[32];
    strcpy(savpath, "/GB/");
    strcat(savpath, romname);
    { char *d = strrchr(savpath, '.');            /* swap the extension for .sav */
      strcpy(d ? d : savpath + strlen(savpath), ".sav"); }
    memset(gb_cartram, 0, GB_CARTRAM_MAX);        /* no save file: start zeroed */
    if (save_size > 0) {
        FIL  sf;
        UINT sbr;
        if (f_open(&sf, savpath, FA_READ) == FR_OK) {
            f_read(&sf, gb_cartram, save_size, &sbr);
            f_close(&sf);
        }
    }
    gb_ram_dirty = 0;

    gb_init_lcd(&gb, lcd_draw_line);
    gb.direct.frame_skip = 1;   /* draw every other frame; logic keeps up better */

    ILI9341_FillScreen(ILI9341_BLACK);
    draw_controls();

    uint32_t fps_t0 = HAL_GetTick();
    uint32_t fps_n  = 0;
    for (;;) {
        gb_run_frame(&gb);

        /* Show the code and stop, rather than spinning silently as it used to. */
        if (gb_err_flag || gb_sd_fail) {
            char e[40];
            snprintf(e, sizeof(e), "err=%d sd=%d", gb_err_flag, gb_sd_fail);
            ILI9341_DrawString(2, 0, e, ILI9341_RED, ILI9341_BLACK);
            uint16_t wx, wy;                              /* wait for a tap */
            while (XPT2046_ReadPixel(&wx, &wy))  vTaskDelay(pdMS_TO_TICKS(30));
            while (!XPT2046_ReadPixel(&wx, &wy)) vTaskDelay(pdMS_TO_TICKS(30));
            break;
        }

        uint16_t tx = 0, ty = 0;
        uint8_t  jp = 0xFF;                  /* active low: 0xFF = nothing pressed */
        if (XPT2046_ReadPixel(&tx, &ty)) {
            if (tx >= 284 && ty < 22) break;               /* QUIT */
            uint16_t hy = (ty > GB_TOUCH_YADJ) ? ty - GB_TOUCH_YADJ : 0;
            uint8_t bit = gb_button_at(tx, hy);
            if (bit) jp &= ~bit;                           /* pressed = bit cleared */
        }
        gb.direct.joypad = jp;

        uint32_t now = HAL_GetTick();

        /* Autosave once cart RAM has been quiet for 1.5 s, so an in-game
         * save reaches the card without the player quitting. */
        if (gb_ram_dirty && (now - gb_ram_wtime) > 1500) {
            gb_flush_sav(savpath, save_size);
        }

        /* frames emulated per second; 60 is real hardware speed */
        fps_n++;
        if (now - fps_t0 >= 1000) {
            char s[12];
            snprintf(s, sizeof(s), "%lufps", (unsigned long)(fps_n * 1000 / (now - fps_t0)));
            ILI9341_DrawString(2, 0, s, ILI9341_WHITE, ILI9341_BLACK);
            fps_n = 0; fps_t0 = now;
        }
    }

    gb_flush_sav(savpath, save_size);
    f_close(&gb_fil);
}

static void gb_enter(void)
{
    gb_rom_count = scan_roms();
    gb_menu();
}

static void gb_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y)) { Screen_Pop(); return; }

    if (y >= GB_ROW_Y0 && y < UI_SOFT_Y) {
        int idx = (y - GB_ROW_Y0) / GB_ROW_H;
        if (idx >= 0 && idx < gb_rom_count) {
            gb_run(gb_roms[idx]);
            xQueueReset(ui_event_queue);   /* drop touches queued during play */
            gb_menu();
        }
    }
}

static void gb_render(void) {}

void ScreenGB_Register(void)
{
    Screen_Register(SCREEN_GB, (screen_t){
        .on_enter  = gb_enter,
        .on_touch  = gb_touch,
        .on_render = gb_render,
    });
}
