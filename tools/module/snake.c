/* ═══════════════════════════════════════════════════════════════════════
 * Snake — a position-independent game module loaded from SD.
 *
 * All state lives in a local struct in module_main and is passed to helpers by
 * pointer. There are deliberately no file-scope globals: writable globals in
 * position-independent code would need -mrwpi/r9 support that this loader does
 * not have, and keeping state on the stack sidesteps that entirely.
 *
 * The module owns the screen and the UI task while it runs, polls touch
 * itself, and returns when the player quits. Hardware is only reachable
 * through the sys table.
 *
 * Build:  cd tools/module && bash build.sh snake.c   -> SNAKE.BIN
 * ═══════════════════════════════════════════════════════════════════════ */

#include "module_api.h"

/* 16 px cells, top 16 px is the status bar. */
#define CELL      16
#define FIELD_Y0  16
#define COLS      (MOD_SCREEN_W / CELL)          /* 20 */
#define ROWS      ((MOD_SCREEN_H - FIELD_Y0) / CELL)  /* 14 */
#define MAXLEN    (COLS * ROWS)                   /* longest possible snake fills the field */

#define SWIPE_TH  24     /* minimum swipe distance to turn, px */
#define POLL_MS   20     /* must be tight enough to track a swipe */

typedef struct {
    uint8_t  bx[MAXLEN], by[MAXLEN];  /* [0] is the head */
    int      len;
    int      dx, dy;                  /* direction */
    uint8_t  fx, fy;                  /* food cell */
    int      score;
    int      alive;
    int      running;
    uint32_t rng;                     /* xorshift seed */
    int      touching;                /* touched at the previous poll */
    uint16_t sx, sy;                  /* swipe origin */
} Snake;

static uint32_t rng_next(Snake *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;  x ^= x >> 17;  x ^= x << 5;
    g->rng = x;
    return x;
}

static void num2str(char *b, int n)
{
    char t[8]; int i = 0;
    if (n == 0) { b[0] = '0'; b[1] = 0; return; }
    while (n > 0 && i < 7) { t[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0;
    while (i > 0) b[j++] = t[--i];
    b[j] = 0;
}

/* Inset by 1 px so the gaps read as grid lines. */
static void cell(const syscall_t *sys, int col, int row, uint16_t color)
{
    int px = col * CELL, py = FIELD_Y0 + row * CELL;
    sys->fill_rect(px, py, CELL, CELL, MOD_BLACK);
    if (color != MOD_BLACK)
        sys->fill_rect(px + 1, py + 1, CELL - 2, CELL - 2, color);
}

static void draw_score(const syscall_t *sys, int score)
{
    char buf[16] = "SCORE ";
    num2str(buf + 6, score);
    sys->fill_rect(0, 0, 160, 16, MOD_BLUE);
    sys->draw_str(4, 0, buf, MOD_WHITE, MOD_BLUE);
}

static void place_food(Snake *g, const syscall_t *sys)
{
    for (int tries = 0; tries < 300; tries++) {
        int fx = rng_next(g) % COLS;
        int fy = rng_next(g) % ROWS;
        int occ = 0;
        for (int i = 0; i < g->len; i++)
            if (g->bx[i] == fx && g->by[i] == fy) { occ = 1; break; }
        if (!occ) { g->fx = (uint8_t)fx; g->fy = (uint8_t)fy;
                    cell(sys, fx, fy, MOD_YELLOW); return; }
    }
}

static void snake_init(Snake *g, const syscall_t *sys)
{
    sys->fill_rect(0, 0, MOD_SCREEN_W, MOD_SCREEN_H, MOD_BLACK);
    sys->fill_rect(0, 0, MOD_SCREEN_W, FIELD_Y0, MOD_BLUE);   /* status bar */

    g->len = 3;
    g->bx[0] = 8; g->by[0] = 7;   /* head */
    g->bx[1] = 7; g->by[1] = 7;
    g->bx[2] = 6; g->by[2] = 7;
    g->dx = 1; g->dy = 0;
    g->score = 0;
    g->alive = 1;
    g->running = 1;
    g->rng = 0x1234567u;
    g->touching = 0;

    for (int i = 0; i < g->len; i++) cell(sys, g->bx[i], g->by[i], MOD_GREEN);

    draw_score(sys, 0);
    sys->fill_rect(288, 0, 32, 16, MOD_RED);
    sys->draw_str(288, 0, "QUIT", MOD_WHITE, MOD_RED);

    place_food(g, sys);
}

/* Dominant axis wins; a 180 degree reversal is rejected. */
static void set_dir(Snake *g, int dx, int dy)
{
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int ndx = 0, ndy = 0;
    if (adx > ady) ndx = (dx > 0) ? 1 : -1;
    else           ndy = (dy > 0) ? 1 : -1;
    if (ndx == -g->dx && ndy == -g->dy) return;
    g->dx = ndx; g->dy = ndy;
}

/* Remembers where the touch started and turns once it has moved far enough,
 * then resets the origin so one continuous drag can turn repeatedly. */
static void poll_input(Snake *g, const syscall_t *sys)
{
    uint16_t x, y;
    if (!sys->is_touched(&x, &y)) { g->touching = 0; return; }

    if (x >= 288 && y < 16) { g->running = 0; return; }

    if (!g->touching) {                     /* touch began */
        g->touching = 1;
        g->sx = x; g->sy = y;
        return;
    }

    int dx = (int)x - (int)g->sx;
    int dy = (int)y - (int)g->sy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx >= SWIPE_TH || ady >= SWIPE_TH) {
        set_dir(g, dx, dy);
        g->sx = x; g->sy = y;                   /* allows chained turns */
        g->rng ^= ((uint32_t)x << 16) ^ y ^ (g->rng << 1);
    }
}

static void snake_step(Snake *g, const syscall_t *sys)
{
    int nx = g->bx[0] + g->dx;
    int ny = g->by[0] + g->dy;

    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) { g->alive = 0; return; }
    for (int i = 0; i < g->len; i++)
        if (g->bx[i] == nx && g->by[i] == ny) { g->alive = 0; return; }

    int eat = (nx == g->fx && ny == g->fy);
    int newlen = g->len;

    if (eat) { newlen = g->len + 1; g->score++; draw_score(sys, g->score); }
    else     { cell(sys, g->bx[g->len - 1], g->by[g->len - 1], MOD_BLACK); } /* erase the tail */

    for (int i = newlen - 1; i > 0; i--) { g->bx[i] = g->bx[i-1]; g->by[i] = g->by[i-1]; }
    g->len = newlen;
    g->bx[0] = (uint8_t)nx; g->by[0] = (uint8_t)ny;
    cell(sys, nx, ny, MOD_GREEN);

    if (eat) place_food(g, sys);
}

/* Entry point, placed in .entry so it lands at offset 0 of the .bin. */
__attribute__((section(".entry"), used))
int module_main(const syscall_t *sys)
{
    Snake g;
    snake_init(&g, sys);

    int acc = 0;                           /* ms accumulated since the last step */
    while (g.running && g.alive) {
        poll_input(&g, sys);
        if (!g.running) break;

        sys->delay_ms(POLL_MS);
        acc += POLL_MS;

        int tick = 180 - g.score * 4;      /* speeds up as the score rises */
        if (tick < 80) tick = 80;
        if (acc >= tick) {
            snake_step(&g, sys);
            acc = 0;
        }
    }

    if (!g.alive) {                        /* game over: wait for a tap */
        sys->fill_rect(60, 96, 200, 48, MOD_BLUE);
        sys->draw_str(124, 104, "GAME OVER",   MOD_WHITE,  MOD_BLUE);
        sys->draw_str(116, 124, "tap to exit", MOD_YELLOW, MOD_BLUE);
        uint16_t x, y;
        while (!sys->is_touched(&x, &y)) sys->delay_ms(30);
        while ( sys->is_touched(&x, &y)) sys->delay_ms(30);
    }

    return g.score;
}
