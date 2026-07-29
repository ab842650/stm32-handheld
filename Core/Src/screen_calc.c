#include "screen.h"
#include "screen_calc.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 計算機
 *
 *   UI_CONTENT_Y ┌──────────────────┐
 *                │            1234  │  顯示列（右對齊）
 *   KEY_Y0       ├────┬────┬────┬───┤
 *                │ 7  │ 8  │ 9  │ / │
 *                │ 4  │ 5  │ 6  │ * │  keypad 4x4
 *                │ 1  │ 2  │ 3  │ - │
 *                │ C  │ 0  │ =  │ + │
 *   UI_SOFT_Y    └────┴────┴────┴───┘
 *
 * 繪圖與觸控命中共用同一組常數（KEY_W / KEY_H / KEY_Y0）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* ── 版面 ── */
#define DISP_Y     (UI_CONTENT_Y + 4)        /* 36 */
#define DISP_H     40                        /* 容納兩行：算式 + 結果 */
#define EXPR_Y     (DISP_Y + 2)              /* 上行：算式 */
#define VAL_Y      (DISP_Y + 22)             /* 下行：結果 */
#define KEY_Y0     (DISP_Y + DISP_H + 4)     /* 80 */
#define KEY_COLS   4
#define KEY_ROWS   4
#define KEY_W      (ILI9341_WIDTH / KEY_COLS)          /* 80 */
#define KEY_H      ((UI_SOFT_Y - KEY_Y0) / KEY_ROWS)   /* 33 */
#define KEY_GAP    2

static const char keys[KEY_ROWS][KEY_COLS] = {
    { '7', '8', '9', '/' },
    { '4', '5', '6', '*' },
    { '1', '2', '3', '-' },
    { 'C', '0', '=', '+' },
};

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 計算機狀態
 *
 *   cur          : 正在輸入的數字（按 1 2 → cur = 12）
 *   acc          : 已累積的結果
 *   pending_op   : 等著執行的運算子，0 = 沒有
 *   expr         : 已按過的算式字串（純顯示用，狀態機不讀它）
 *   just_equaled : 剛按完 '='，下一個數字要開始新算式
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static long acc;
static long cur;
static char pending_op;
static char expr[40];
static uint8_t expr_len;
static bool just_equaled;

static void expr_clear(void)
{
    expr_len = 0;
    expr[0] = '\0';
}

static void expr_add(char c)
{
    if (expr_len < sizeof(expr) - 1) {     /* 滿了就不再加，避免寫爆 buffer */
        expr[expr_len++] = c;
        expr[expr_len] = '\0';
    }
}

/* 重畫顯示列（兩行，皆右對齊）
 *
 *   ┌──────────────────┐
 *   │            12+5  │  算式（灰）
 *   │              17  │  結果（白）
 *   └──────────────────┘
 *
 * 顯示層只讀狀態，不改狀態。 */
static void calc_show(long v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", v);

    ILI9341_FillRect(0, DISP_Y, ILI9341_WIDTH, DISP_H, ILI9341_BLACK);

    /* 上行：算式。太長就只顯示尾端（跟真的計算機一樣往左捲）*/
    const uint16_t maxch = ILI9341_WIDTH / FONT_WIDTH - 2;      /* 38 字 */
    const char *e = expr;
    if (expr_len > maxch) e = expr + (expr_len - maxch);
    uint16_t ew = (uint16_t)strlen(e) * FONT_WIDTH;
    ILI9341_DrawString(ILI9341_WIDTH - 8 - ew, EXPR_Y, e,
                       ILI9341_LIGHTGRAY, ILI9341_BLACK);

    /* 下行：目前的值 */
    uint16_t w = (uint16_t)strlen(buf) * FONT_WIDTH;
    ILI9341_DrawString(ILI9341_WIDTH - 8 - w, VAL_Y, buf,
                       ILI9341_WHITE, ILI9341_BLACK);
}

/* 依 op 把 a、b 算出結果。op == 0（沒有待處理運算子）時直接回傳 b。 */
static long calc_apply(long a, long b, char op)
{
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return (b != 0) ? a / b : 0;   /* 防除以零：會進 HardFault */
        default:  return b;                      /* 無 pending op → 直接取新值 */
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 按下一個鍵 → 更新狀態 + 更新顯示
 *
 * k：'0'~'9' / '+' '-' '*' '/' / '=' / 'C'
 *
 * 關鍵：按運算子時不是馬上算新的，而是「先把上一個 pending 的結算掉」，
 *       所以 2+3*4 連續按能一路算下去（逐步計算，無先乘除後加減）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void calc_on_key(char k)
{
    if (k >= '0' && k <= '9') {          /* 數字：往 cur 累積 */
        if (just_equaled) {              /* 剛算完 → 這個數字開始新算式 */
            expr_clear();
            just_equaled = false;
        }
        cur = cur * 10 + (k - '0');      /* '7'(ASCII 55) - '0'(48) = 7 */
        expr_add(k);
        calc_show(cur);
    }
    else if (k == 'C') {                 /* 清除：全部歸零 */
        acc = 0;
        cur = 0;
        pending_op = 0;
        just_equaled = false;
        expr_clear();
        calc_show(0);
    }
    else if (k == '=') {                 /* 結算並清掉 pending */
        acc = calc_apply(acc, cur, pending_op);
        pending_op = 0;
        cur = 0;
        just_equaled = true;
        expr_add('=');
        calc_show(acc);
    }
    else {                               /* 運算子：先結算上一個，再記住這次的 */
        acc = calc_apply(acc, cur, pending_op);
        pending_op = k;
        cur = 0;
        just_equaled = false;
        expr_add(k);
        calc_show(acc);
    }
}

/* 畫出整個 keypad */
static void calc_draw_keys(void)
{
    for (int r = 0; r < KEY_ROWS; r++) {
        for (int c = 0; c < KEY_COLS; c++) {
            char k = keys[r][c];

            uint16_t bg = ILI9341_DARKGRAY;             /* 數字：深灰 */
            if (k == 'C')                bg = ILI9341_RED;
            else if (k == '=')           bg = ILI9341_GREEN;
            else if (k < '0' || k > '9') bg = ILI9341_NAVY;   /* 運算子 */

            uint16_t x = c * KEY_W;
            uint16_t y = KEY_Y0 + r * KEY_H;

            /* 留 GAP 當格線 */
            ILI9341_FillRect(x + KEY_GAP, y + KEY_GAP,
                             KEY_W - KEY_GAP * 2, KEY_H - KEY_GAP * 2, bg);

            /* 鍵面文字置中 */
            char s[2] = { k, '\0' };
            ILI9341_DrawString(x + (KEY_W - FONT_WIDTH) / 2,
                               y + (KEY_H - FONT_HEIGHT) / 2,
                               s, ILI9341_WHITE, bg);
        }
    }
}

static void calc_enter(void)
{
    /* 進入時重置狀態 */
    acc = 0;
    cur = 0;
    pending_op = 0;
    just_equaled = false;
    expr_clear();

    UI_DrawFrame("Calculator", NULL, "Back");
    calc_draw_keys();
    calc_show(0);
}

static void calc_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y)) {
        Screen_Pop();
        return;
    }

    /* 把觸控座標換算成 keypad 的 (row, col) */
    if (y < KEY_Y0 || y >= KEY_Y0 + KEY_ROWS * KEY_H) return;
    int r = (y - KEY_Y0) / KEY_H;
    int c = x / KEY_W;
    if (r < 0 || r >= KEY_ROWS || c < 0 || c >= KEY_COLS) return;

    calc_on_key(keys[r][c]);
}

static void calc_render(void)
{
}

void ScreenCalc_Register(void)
{
    Screen_Register(SCREEN_CALC, (screen_t){
        .on_enter  = calc_enter,
        .on_touch  = calc_touch,
        .on_render = calc_render,
    });
}
