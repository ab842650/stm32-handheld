#include "screen.h"
#include "screen_calc.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Calculator
 *
 *   UI_CONTENT_Y ┌──────────────────┐
 *                │            1234  │  display, right aligned
 *   KEY_Y0       ├────┬────┬────┬───┤
 *                │ 7  │ 8  │ 9  │ / │
 *                │ 4  │ 5  │ 6  │ * │  keypad 4x4
 *                │ 1  │ 2  │ 3  │ - │
 *                │ C  │ 0  │ =  │ + │
 *   UI_SOFT_Y    └────┴────┴────┴───┘
 *
 * Drawing and hit-testing share KEY_W / KEY_H / KEY_Y0.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define DISP_Y     (UI_CONTENT_Y + 4)        /* 36 */
#define DISP_H     40                        /* two lines: expression and result */
#define EXPR_Y     (DISP_Y + 2)              /* expression */
#define VAL_Y      (DISP_Y + 22)             /* result */
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
 *   cur          : number being typed
 *   acc          : running result
 *   pending_op   : operator waiting to be applied, 0 = none
 *   expr         : the keys pressed so far, for display only
 *   just_equaled : '=' was the last key, so a digit starts a new expression
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
    if (expr_len < sizeof(expr) - 1) {     /* stop appending rather than overrun */
        expr[expr_len++] = c;
        expr[expr_len] = '\0';
    }
}

/* Both display lines are right aligned.
 *
 *   ┌──────────────────┐
 *   │            12+5  │  expression, grey
 *   │              17  │  result, white
 *   └──────────────────┘
 *
 * This only reads state, never changes it. */
static void calc_show(long v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", v);

    ILI9341_FillRect(0, DISP_Y, ILI9341_WIDTH, DISP_H, ILI9341_BLACK);

    /* Scrolls left when too long, like a real calculator. */
    const uint16_t maxch = ILI9341_WIDTH / FONT_WIDTH - 2;
    const char *e = expr;
    if (expr_len > maxch) e = expr + (expr_len - maxch);
    uint16_t ew = (uint16_t)strlen(e) * FONT_WIDTH;
    ILI9341_DrawString(ILI9341_WIDTH - 8 - ew, EXPR_Y, e,
                       ILI9341_LIGHTGRAY, ILI9341_BLACK);

    uint16_t w = (uint16_t)strlen(buf) * FONT_WIDTH;
    ILI9341_DrawString(ILI9341_WIDTH - 8 - w, VAL_Y, buf,
                       ILI9341_WHITE, ILI9341_BLACK);
}

/* Applies op to a and b; op == 0 means nothing is pending, so return b. */
static long calc_apply(long a, long b, char op)
{
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return (b != 0) ? a / b : 0;   /* divide by zero would HardFault */
        default:  return b;
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Handle one keypress.
 *
 * k：'0'~'9' / '+' '-' '*' '/' / '=' / 'C'
 *
 * An operator key settles the previous pending operation before recording the
 * new one, so a chain like 2+3*4 evaluates left to right. No precedence.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void calc_on_key(char k)
{
    if (k >= '0' && k <= '9') {          /* digit */
        if (just_equaled) {              /* start a fresh expression */
            expr_clear();
            just_equaled = false;
        }
        cur = cur * 10 + (k - '0');      /* '7'(ASCII 55) - '0'(48) = 7 */
        expr_add(k);
        calc_show(cur);
    }
    else if (k == 'C') {                 /* clear */
        acc = 0;
        cur = 0;
        pending_op = 0;
        just_equaled = false;
        expr_clear();
        calc_show(0);
    }
    else if (k == '=') {                 /* settle and clear pending */
        acc = calc_apply(acc, cur, pending_op);
        pending_op = 0;
        cur = 0;
        just_equaled = true;
        expr_add('=');
        calc_show(acc);
    }
    else {                               /* settle the previous op, then record this one */
        acc = calc_apply(acc, cur, pending_op);
        pending_op = k;
        cur = 0;
        just_equaled = false;
        expr_add(k);
        calc_show(acc);
    }
}

static void calc_draw_keys(void)
{
    for (int r = 0; r < KEY_ROWS; r++) {
        for (int c = 0; c < KEY_COLS; c++) {
            char k = keys[r][c];

            uint16_t bg = ILI9341_DARKGRAY;             /* digits */
            if (k == 'C')                bg = ILI9341_RED;
            else if (k == '=')           bg = ILI9341_GREEN;
            else if (k < '0' || k > '9') bg = ILI9341_NAVY;   /* operators */

            uint16_t x = c * KEY_W;
            uint16_t y = KEY_Y0 + r * KEY_H;

            /* GAP acts as the grid line */
            ILI9341_FillRect(x + KEY_GAP, y + KEY_GAP,
                             KEY_W - KEY_GAP * 2, KEY_H - KEY_GAP * 2, bg);

            char s[2] = { k, '\0' };
            ILI9341_DrawString(x + (KEY_W - FONT_WIDTH) / 2,
                               y + (KEY_H - FONT_HEIGHT) / 2,
                               s, ILI9341_WHITE, bg);
        }
    }
}

static void calc_enter(void)
{
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
