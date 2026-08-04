#include "screen.h"
#include "screen_kb.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 螢幕鍵盤
 *
 * 版面（320x240）：
 *   y=32   文字框（顯示已輸入的內容 + 閃爍游標）
 *   y=70   q w e r t y u i o p        10 鍵
 *   y=104    a s d f g h j k l         9 鍵，置中
 *   y=138  ^  z x c v b n m       <=   Shift + 字母 + Backspace
 *   y=172  123 [   space   ]    Send
 *   y=210  軟鍵列：Back（取消）
 *
 * 三個 layer（小寫/大寫/數字符號）只是換一組字串資料表，繪圖與命中判定
 * 完全共用同一組座標計算 —— 跟 screen_home 的 APPS[] 是同一個套路。
 *
 * 觸控連發不用處理：vInputTask 一次按壓只送一個事件（按下→送→等放開），
 * 所以按住不放不會連續輸入。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* ── 版面常數 ────────────────────────────────────────────────────────── */
#define FLD_Y     UI_CONTENT_Y              /* 文字框 y = 32 */
#define FLD_H     36
#define FLD_PAD   4
#define FLD_COLS  ((ILI9341_WIDTH - FLD_PAD * 2) / FONT_WIDTH)   /* 39 */

#define KW        32                        /* 一般鍵寬 */
#define KH        34                        /* 鍵高 */
#define KY0       70                        /* 第 0 列的 y */
#define ROW_Y(r)  (KY0 + (r) * KH)          /* 70 / 104 / 138 / 172 */

#define WIDE_W    48                        /* Shift / Backspace 寬 */
#define R3_NUM_W  56                        /* "123" / "ABC" */
#define R3_SEND_W 80
#define R3_SPC_X  R3_NUM_W
#define R3_SPC_W  (ILI9341_WIDTH - R3_NUM_W - R3_SEND_W)   /* 184 */
#define R3_SEND_X (ILI9341_WIDTH - R3_SEND_W)              /* 240 */

/* 配色 */
#define C_KEY     ILI9341_DARKGRAY
#define C_FUNC    0x39C7                    /* 比 DARKGRAY 亮一階：功能鍵 */
#define C_SEND    ILI9341_GREEN
#define C_FIELD   0x2124                    /* 文字框底：接近黑的深灰 */

/* ── 按鍵動作編碼：0x20..0x7E 是字元本身，>=0x100 是功能鍵 ───────────── */
#define K_NONE    0
#define K_SHIFT   0x100
#define K_BKSP    0x101
#define K_NUM     0x102
#define K_SEND    0x103
#define K_SPACE   ' '

/* layer 0=小寫 1=大寫 2=數字符號 */
static const char *ROWS[3][3] = {
    { "qwertyuiop", "asdfghjkl", "zxcvbnm"  },
    { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"  },
    { "1234567890", "-/:;()$&@", ".,?!'\"" },
};

/* ── 狀態 ────────────────────────────────────────────────────────────── */
static char       kb_buf[KB_MAX + 1];
static int        kb_len;
static char       kb_title[20];
static kb_done_fn kb_cb;
static int        kb_layer;
static uint8_t    cur_shown;      /* 上次畫出的游標明滅狀態 */

/* ── 座標計算 ────────────────────────────────────────────────────────── */

/* 第 row 列的第 i 個字母鍵的 x。字母在該列內水平置中。 */
static uint16_t key_x(int row, int i)
{
    int n = (int)strlen(ROWS[kb_layer][row]);

    if (row == 2) {
        /* 第 2 列兩側被 Shift / Backspace 佔掉，字母擠在中間 288..272 之間 */
        int span = ILI9341_WIDTH - WIDE_W * 2;                 /* 224 */
        return (uint16_t)(WIDE_W + (span - n * KW) / 2 + i * KW);
    }
    return (uint16_t)((ILI9341_WIDTH - n * KW) / 2 + i * KW);
}

/* ── 繪圖 ────────────────────────────────────────────────────────────── */

/* 一顆鍵：色塊 + 1px 黑框（黑框當縫隙，不用額外算 gap）+ 置中標籤 */
static void draw_key(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     uint16_t color, const char *label)
{
    ILI9341_FillRect(x, y, w, h, ILI9341_BLACK);
    ILI9341_FillRect(x + 1, y + 1, w - 2, h - 2, color);

    uint16_t tw = (uint16_t)strlen(label) * FONT_WIDTH;
    ILI9341_DrawString(x + (w - tw) / 2, y + (h - FONT_HEIGHT) / 2,
                       label, ILI9341_WHITE, color);
}

static void draw_keys(void)
{
    char lab[2] = { 0, 0 };

    for (int row = 0; row < 3; row++) {
        const char *s = ROWS[kb_layer][row];
        uint16_t    y = ROW_Y(row);

        /* 這一列整條先清乾淨：換 layer 時字數可能變少，避免留下舊字 */
        ILI9341_FillRect(0, y, ILI9341_WIDTH, KH, ILI9341_BLACK);

        for (int i = 0; s[i] != '\0'; i++) {
            lab[0] = s[i];
            draw_key(key_x(row, i), y, KW, KH, C_KEY, lab);
        }
    }

    /* 第 2 列兩側的功能鍵 */
    draw_key(0, ROW_Y(2), WIDE_W, KH, C_FUNC,
             (kb_layer == 1) ? "^^" : "^");
    draw_key(ILI9341_WIDTH - WIDE_W, ROW_Y(2), WIDE_W, KH, C_FUNC, "<=");

    /* 第 3 列 */
    uint16_t y3 = ROW_Y(3);
    draw_key(0,         y3, R3_NUM_W,  KH, C_FUNC, (kb_layer == 2) ? "ABC" : "123");
    draw_key(R3_SPC_X,  y3, R3_SPC_W,  KH, C_KEY,  "space");
    draw_key(R3_SEND_X, y3, R3_SEND_W, KH, C_SEND, "Send");
}

/* 文字框：太長就只顯示尾端（游標永遠看得到）*/
static void draw_field(void)
{
    ILI9341_FillRect(0, FLD_Y, ILI9341_WIDTH, FLD_H, C_FIELD);

    int         vis   = (kb_len > FLD_COLS - 1) ? FLD_COLS - 1 : kb_len;
    const char *tail  = kb_buf + (kb_len - vis);
    uint16_t    ty    = FLD_Y + (FLD_H - FONT_HEIGHT) / 2;

    if (vis > 0) {
        char tmp[FLD_COLS + 1];
        memcpy(tmp, tail, vis);
        tmp[vis] = '\0';
        ILI9341_DrawString(FLD_PAD, ty, tmp, ILI9341_WHITE, C_FIELD);
    }
    cur_shown = 0xFF;    /* 強制下一輪重畫游標 */
}

/* 游標：畫在文字後面的一個實心塊，on 亮 off 用底色蓋掉 */
static void draw_cursor(uint8_t on)
{
    int      vis = (kb_len > FLD_COLS - 1) ? FLD_COLS - 1 : kb_len;
    uint16_t cx  = FLD_PAD + (uint16_t)vis * FONT_WIDTH;
    uint16_t cy  = FLD_Y + (FLD_H - FONT_HEIGHT) / 2;

    ILI9341_FillRect(cx, cy, FONT_WIDTH - 1, FONT_HEIGHT,
                     on ? ILI9341_CYAN : C_FIELD);
}

/* ── 命中判定：把觸控座標翻成一個動作碼 ─────────────────────────────── */
static int hit_test(uint16_t x, uint16_t y)
{
    for (int row = 0; row < 3; row++) {
        if (y < ROW_Y(row) || y >= ROW_Y(row) + KH) continue;

        if (row == 2) {
            if (x < WIDE_W)                     return K_SHIFT;
            if (x >= ILI9341_WIDTH - WIDE_W)    return K_BKSP;
        }
        const char *s = ROWS[kb_layer][row];
        for (int i = 0; s[i] != '\0'; i++) {
            uint16_t kx = key_x(row, i);
            if (x >= kx && x < kx + KW) return (unsigned char)s[i];
        }
        return K_NONE;
    }

    if (y >= ROW_Y(3) && y < ROW_Y(3) + KH) {
        if (x < R3_NUM_W)      return K_NUM;
        if (x >= R3_SEND_X)    return K_SEND;
        return K_SPACE;
    }
    return K_NONE;
}

/* ── 畫面 callbacks ─────────────────────────────────────────────────── */

static void kb_enter(void)
{
    UI_DrawFrame(kb_title, NULL, "Back");
    draw_field();
    draw_keys();
}

static void kb_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y)) { Screen_Pop(); return; }   /* 取消，不回呼 */

    int k = hit_test(x, y);
    switch (k) {
        case K_NONE:
            return;

        case K_SHIFT:
            kb_layer = (kb_layer == 1) ? 0 : 1;
            draw_keys();
            return;

        case K_NUM:
            kb_layer = (kb_layer == 2) ? 0 : 2;
            draw_keys();
            return;

        case K_BKSP:
            if (kb_len > 0) { kb_buf[--kb_len] = '\0'; draw_field(); }
            return;

        case K_SEND: {
            kb_done_fn cb = kb_cb;
            Screen_Pop();                       /* 先還原呼叫者的畫面 */
            if (cb != NULL) cb(kb_buf);         /* 再回呼，裡面可以安全 push */
            return;
        }

        default:                                 /* 一般字元 */
            if (kb_len < KB_MAX) {
                kb_buf[kb_len++] = (char)k;
                kb_buf[kb_len]   = '\0';
                draw_field();
                /* 大寫只作用一個字（跟手機一樣），打完自動回小寫 */
                if (kb_layer == 1) { kb_layer = 0; draw_keys(); }
            }
            return;
    }
}

static void kb_render(void)
{
    uint8_t on = (uint8_t)((xTaskGetTickCount() / pdMS_TO_TICKS(500)) & 1);
    if (on != cur_shown) {
        cur_shown = on;
        draw_cursor(on);
    }
}

/* ── 對外 API ────────────────────────────────────────────────────────── */

void Keyboard_Open(const char *title, const char *initial, kb_done_fn on_done)
{
    strncpy(kb_title, (title != NULL) ? title : "Input", sizeof(kb_title) - 1);
    kb_title[sizeof(kb_title) - 1] = '\0';

    kb_buf[0] = '\0';
    if (initial != NULL) {
        strncpy(kb_buf, initial, KB_MAX);
        kb_buf[KB_MAX] = '\0';
    }
    kb_len   = (int)strlen(kb_buf);
    kb_cb    = on_done;
    kb_layer = 0;

    Screen_Push(SCREEN_KB);
}

void ScreenKb_Register(void)
{
    Screen_Register(SCREEN_KB, (screen_t){
        .on_enter  = kb_enter,
        .on_touch  = kb_touch,
        .on_render = kb_render,
    });
}
