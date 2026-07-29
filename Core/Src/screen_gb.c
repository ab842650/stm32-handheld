/* Peanut-GB 直譯器很吃 CPU，強制本檔用 -O3 編（不動專案其他設定）。 */
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

extern QueueHandle_t ui_event_queue;   /* 退出時清掉累積觸控 */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Game Boy 模擬器（內建 app，編進韌體）—— G0：載入 ROM + gb_init
 *
 * 用 Peanut-GB（單檔 header）。這個 .c 是唯一 include 它「實作」的檔案。
 * 工作記憶體（gb_s ≈ 16.7KB + 32KB ROM + cart RAM）放 CCM（.ccmram_bss，
 * 64KB 幾乎全空）；CCM 不能 DMA/執行，但當資料 RAM 完美。
 *
 * G0 目標：從 /GB 讀一個 .gb 進 CCM → gb_init 成功 → 顯示遊戲標題。
 * （還不畫遊戲畫面，那是 G1 的 lcd_draw_line）
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* 關掉吃 CPU 又用不到的功能（我們只顯示 4 灰階）→ 加速模擬 */
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#define PEANUT_GB_12_COLOUR         0
#include "peanut_gb.h"

#define GB_BANK_SIZE    (16 * 1024)     /* bank 0 大小（釘住）*/
#define GB_CARTRAM_MAX  (32 * 1024)     /* 存檔 RAM 最大 32KB（寶可夢就是 32KB）*/

/* 細粒度 ROM 快取（像 CPU cache）：512B 一條 line、32 條 direct-mapped = 16KB。
 * miss 只讀 512B（~1ms 而非整 bank 12ms），且能同時快取 32 個分散熱區。*/
#define GB_LINE_SZ    512
#define GB_LINE_BITS  9
#define GB_NLINES     32                /* 32 × 512B = 16KB */

/* ── 工作記憶體放 CCM（執行期才填，用 NOLOAD 段）── */
static struct gb_s gb          __attribute__((section(".ccmram_bss")));
static uint8_t gb_bank0[GB_BANK_SIZE]  __attribute__((section(".ccmram_bss"))); /* 固定 bank 0，釘住 */
static uint8_t gb_cache[GB_NLINES * GB_LINE_SZ] __attribute__((section(".ccmram_bss"))); /* 可切換區的 line 快取 */
static uint32_t gb_tag[GB_NLINES];      /* 每條 line 目前存哪個 512B 區塊(line 號)；0xFFFFFFFF=空 */
static uint8_t gb_cartram[GB_CARTRAM_MAX];   /* 存檔 RAM 放 SRAM（32KB，CCM 塞不下）*/
static FIL     gb_fil;          /* 遊玩期間保持開啟，供 gb_rom_read 換頁 */

static int gb_err_flag;

static void gb_cache_reset(void)
{
    for (int i = 0; i < GB_NLINES; i++) gb_tag[i] = 0xFFFFFFFFu;
}

/* ── Peanut-GB 要求的 callback ──
 * addr = 整顆 ROM 的線性位址。< 0x4000 是 bank 0（釘住）；其餘走 512B 的
 * direct-mapped 快取：命中直接回，miss 才從 SD 讀「一條 512B line」換進來。*/
static uint8_t gb_rom_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
    if (addr < 0x4000) return gb_bank0[addr];          /* bank 0 釘住 */

    uint32_t line = addr >> GB_LINE_BITS;              /* 整顆 ROM 的第幾條 512B line */
    uint32_t set  = line & (GB_NLINES - 1);            /* direct-mapped 落在哪一格 */
    if (gb_tag[set] != line) {                         /* miss → 只讀 512B */
        UINT br;
        f_lseek(&gb_fil, line << GB_LINE_BITS);
        f_read(&gb_fil, &gb_cache[set * GB_LINE_SZ], GB_LINE_SZ, &br);
        gb_tag[set] = line;
    }
    return gb_cache[set * GB_LINE_SZ + (addr & (GB_LINE_SZ - 1))];
}
static uint8_t gb_cart_ram_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
    return (addr < GB_CARTRAM_MAX) ? gb_cartram[addr] : 0xFF;
}
static int      gb_ram_dirty;   /* cart RAM 被寫過、還沒刷到 SD */
static uint32_t gb_ram_wtime;   /* 最後一次寫入的時間（用來 debounce）*/
static void gb_cart_ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t val)
{
    (void)g;
    if (addr < GB_CARTRAM_MAX) {
        gb_cartram[addr] = val;
        gb_ram_dirty = 1;
        gb_ram_wtime  = HAL_GetTick();   /* 記下時間，供自動存檔判斷 */
    }
}
static void gb_error_cb(struct gb_s *g, const enum gb_error_e err, const uint16_t addr)
{
    (void)g; (void)addr;
    gb_err_flag = (int)err + 1;
}

/* ── 顯示：1x（160x144），畫面置於上方；下方放不透明控制（像掌機）── */
#define GB_SW  160
#define GB_SH  144
#define GB_X   ((ILI9341_WIDTH - GB_SW) / 2)   /* 80 */
#define GB_Y   8                                /* 畫面 y 8..152，下方留給按鍵 */
static const uint16_t GB_SHADE[4] = { 0x9DE1, 0x8D61, 0x3306, 0x09C1 }; /* 淺→深 */
static uint8_t gb_linebuf[GB_SW * 2];   /* 一列 RGB565（SRAM，可 DMA）*/

/* ── 觸控控制（螢幕座標，全在畫面下方，不透明畫一次）── */
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
#define GB_TOUCH_YADJ 10   /* 觸控 y 偏下修正：hit-test 時往上校正這麼多 px */

/* 螢幕點 (sx,sy) 落在哪個按鈕？回傳 joypad 位元（0=沒有）*/
static uint8_t gb_button_at(uint16_t sx, uint16_t sy)
{
    for (unsigned i = 0; i < GB_NBTN; i++) {
        const gbbtn_t *b = &GB_BTNS[i];
        if (sx >= b->x && sx < b->x + b->w && sy >= b->y && sy < b->y + b->h)
            return b->bit;
    }
    return 0;
}

/* 一次畫好所有控制鈕 + QUIT（不透明，之後不會被畫面覆蓋，畫在下方）*/
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

/* Peanut-GB 每畫好一列就呼叫：1x，直接轉 RGB565 貼上（無縮放、無合成）*/
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

/* ── ROM 選單 ── */
#define GB_LIST_MAX 6
#define GB_ROW_H    28
#define GB_ROW_Y0   (UI_CONTENT_Y + 8)
static char gb_roms[GB_LIST_MAX][13];
static int  gb_rom_count;

/* 掃 /GB 收集所有 .gb 檔名 */
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

/* 畫 ROM 選單（每列一個遊戲名，去掉 .gb）*/
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

/* 把 cart RAM 寫回 .sav（存到 SD）。只有「有存檔的遊戲」且「有未存變更」才寫。*/
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

/* 載入並執行一個 ROM（選單點選後呼叫，阻塞到玩家 QUIT）*/
static void gb_run(const char *romname)
{
    /* 開檔並「保持開啟」（gb_rom_read 之後要靠它換頁）；只先讀 bank 0 進 CCM */
    char path[32];
    strcpy(path, "/GB/");
    strcat(path, romname);
    UINT br = 0;
    if (f_open(&gb_fil, path, FA_READ) != FR_OK) {
        UI_DrawCentered(UI_CONTENT_Y + 40, "ROM open fail", ILI9341_RED, ILI9341_BLACK);
        return;
    }
    f_read(&gb_fil, gb_bank0, GB_BANK_SIZE, &br);   /* bank 0（釘住）*/
    gb_cache_reset();                                /* 清空 line 快取 */
    /* 注意：gb_fil 不關，留給 gb_rom_read 換頁；退出時才 f_close */

    /* 初始化模擬器 */
    gb_err_flag = 0;
    enum gb_init_error_e e = gb_init(&gb, gb_rom_read, gb_cart_ram_read,
                                     gb_cart_ram_write, gb_error_cb, NULL);

    char line[40];
    if (e != GB_INIT_NO_ERROR) {
        /* 0x147=卡匣類型、0x148=ROM 大小碼（header 在 bank 0）*/
        snprintf(line, sizeof(line), "init err %d  type=0x%02X size=0x%02X",
                 (int)e, gb_bank0[0x147], gb_bank0[0x148]);
        UI_DrawCentered(UI_CONTENT_Y + 40, line, ILI9341_RED, ILI9341_BLACK);
        UI_DrawCentered(UI_CONTENT_Y + 66, "cartridge unsupported",
                        ILI9341_LIGHTGRAY, ILI9341_BLACK);
        f_close(&gb_fil);
        uint16_t wx, wy;                                  /* 等點一下再回選單 */
        while (XPT2046_ReadPixel(&wx, &wy))  vTaskDelay(pdMS_TO_TICKS(30));
        while (!XPT2046_ReadPixel(&wx, &wy)) vTaskDelay(pdMS_TO_TICKS(30));
        return;
    }

    (void)line;

    /* ── 讀存檔：/GB/NAME.sav → cart RAM ── */
    size_t save_size = 0;
    gb_get_save_size_s(&gb, &save_size);          /* 這遊戲的存檔大小（無=0）*/
    char savpath[32];
    strcpy(savpath, "/GB/");
    strcat(savpath, romname);
    { char *d = strrchr(savpath, '.');            /* 把副檔名換成 .sav */
      strcpy(d ? d : savpath + strlen(savpath), ".sav"); }
    memset(gb_cartram, 0, GB_CARTRAM_MAX);        /* 沒存檔就從全 0 開始 */
    if (save_size > 0) {
        FIL  sf;
        UINT sbr;
        if (f_open(&sf, savpath, FA_READ) == FR_OK) {
            f_read(&sf, gb_cartram, save_size, &sbr);
            f_close(&sf);
        }
    }
    gb_ram_dirty = 0;                             /* 剛載入，乾淨 */

    /* G1：註冊畫線 callback → 開始跑遊戲 */
    gb_init_lcd(&gb, lcd_draw_line);
    gb.direct.frame_skip = 1;   /* 每 2 幀只畫 1 幀 → 邏輯跑更接近 60fps */

    ILI9341_FillScreen(ILI9341_BLACK);
    draw_controls();

    /* 主迴圈：跑 frame（內部畫圖 + 疊按鈕）→ 讀觸控設 joypad → 檢查 QUIT */
    uint32_t fps_t0 = HAL_GetTick();
    uint32_t fps_n  = 0;
    for (;;) {
        gb_run_frame(&gb);

        uint16_t tx = 0, ty = 0;
        uint8_t  jp = 0xFF;                  /* active-low：0xFF = 全放開 */
        if (XPT2046_ReadPixel(&tx, &ty)) {
            if (tx >= 284 && ty < 22) break;               /* QUIT */
            uint16_t hy = (ty > GB_TOUCH_YADJ) ? ty - GB_TOUCH_YADJ : 0;  /* y 校正 */
            uint8_t bit = gb_button_at(tx, hy);
            if (bit) jp &= ~bit;                           /* 按下 = 清該位元 */
        }
        gb.direct.joypad = jp;

        uint32_t now = HAL_GetTick();

        /* 自動存檔：cart RAM 寫完靜下來 1.5s 就刷到 SD
         * → 遊戲內按 SAVE 後，約 1.5 秒自動持久化到 SD（不必退出）*/
        if (gb_ram_dirty && (now - gb_ram_wtime) > 1500) {
            gb_flush_sav(savpath, save_size);
        }

        /* --- FPS：每秒算一次「模擬了幾幀」(60=真實速度) --- */
        fps_n++;
        if (now - fps_t0 >= 1000) {
            char s[12];
            snprintf(s, sizeof(s), "%lufps", (unsigned long)(fps_n * 1000 / (now - fps_t0)));
            ILI9341_DrawString(2, 0, s, ILI9341_WHITE, ILI9341_BLACK);
            fps_n = 0; fps_t0 = now;
        }
    }

    gb_flush_sav(savpath, save_size);   /* 退出時再刷一次（若有未存變更）*/
    f_close(&gb_fil);                   /* 關掉 ROM 檔（回選單）*/
}

static void gb_enter(void)
{
    gb_rom_count = scan_roms();
    gb_menu();
}

static void gb_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y)) { Screen_Pop(); return; }    /* Back → 主選單 */

    if (y >= GB_ROW_Y0 && y < UI_SOFT_Y) {
        int idx = (y - GB_ROW_Y0) / GB_ROW_H;
        if (idx >= 0 && idx < gb_rom_count) {
            gb_run(gb_roms[idx]);          /* 阻塞玩到 QUIT */
            xQueueReset(ui_event_queue);   /* 清掉遊戲期間累積的觸控 */
            gb_menu();                     /* 回 ROM 選單 */
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
