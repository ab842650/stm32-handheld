/* ═══════════════════════════════════════════════════════════════════════
 * 貪食蛇 —— 一個「位置無關、存在 SD 卡上」的遊戲 module (M3)
 *
 * M3 架構重點（跟一般 app 不同的地方）：
 *  ① 狀態全放 module_main 裡的 local struct（在堆疊上 → 天生位置無關），
 *     完全不用 file-scope 全域變數 → 繞過 M2b（-fpic 就夠）
 *  ② module 自己跑遊戲迴圈、自己輪詢觸控、自己決定何時退出（return）——
 *     它執行時佔用整個畫面與 UI task，像全螢幕 app
 *  ③ 只能透過 sys 表碰硬體（畫圖/觸控/延遲），不知道也不需要知道主韌體位址
 *  ④ helper 函式一律用「傳 &state 指標」共享狀態，不靠全域
 *
 * 編譯：  cd tools/module && bash build.sh snake.c   → SNAKE.BIN
 * ═══════════════════════════════════════════════════════════════════════ */

#include "module_api.h"

/* ── 版面：16px 格子，頂端 16px 當狀態列 ── */
#define CELL      16
#define FIELD_Y0  16
#define COLS      (MOD_SCREEN_W / CELL)          /* 20 */
#define ROWS      ((MOD_SCREEN_H - FIELD_Y0) / CELL)  /* 14 */
#define MAXLEN    (COLS * ROWS)                   /* 280：蛇最長 = 塞滿場地 */

#define SWIPE_TH  24     /* 觸發轉向的最小滑動距離(px) */
#define POLL_MS   20     /* 觸控輪詢間隔（要夠密才抓得到滑動軌跡）*/

/* 所有遊戲狀態（放 local，不用全域）*/
typedef struct {
    uint8_t  bx[MAXLEN], by[MAXLEN];  /* 蛇身格子座標，[0]=頭 */
    int      len;
    int      dx, dy;                  /* 前進方向 */
    uint8_t  fx, fy;                  /* 食物格子 */
    int      score;
    int      alive;
    int      running;
    uint32_t rng;                     /* 亂數種子（xorshift）*/
    int      touching;                /* 上次輪詢時是否正被觸碰 */
    uint16_t sx, sy;                  /* 這次滑動的起點 */
} Snake;

/* ── 小工具 ── */
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

/* 畫一格：先清成黑，再畫內縮 1px 的色塊（留出格線感）*/
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
    sys->fill_rect(0, 0, MOD_SCREEN_W, FIELD_Y0, MOD_BLUE);   /* 狀態列 */

    g->len = 3;
    g->bx[0] = 8; g->by[0] = 7;   /* 頭 */
    g->bx[1] = 7; g->by[1] = 7;
    g->bx[2] = 6; g->by[2] = 7;
    g->dx = 1; g->dy = 0;         /* 向右 */
    g->score = 0;
    g->alive = 1;
    g->running = 1;
    g->rng = 0x1234567u;
    g->touching = 0;

    for (int i = 0; i < g->len; i++) cell(sys, g->bx[i], g->by[i], MOD_GREEN);

    draw_score(sys, 0);
    sys->fill_rect(288, 0, 32, 16, MOD_RED);       /* QUIT 鈕 */
    sys->draw_str(288, 0, "QUIT", MOD_WHITE, MOD_RED);

    place_food(g, sys);
}

/* 依滑動向量 (dx,dy) 設方向（取主軸；禁止 180 度回頭）*/
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

/* 輪詢觸控：偵測「滑動軌跡」設方向 / 按 QUIT 退出。
 * 記住觸碰起點，等手指移動超過門檻就依方向轉向，並重設起點（可連續滑）。*/
static void poll_input(Snake *g, const syscall_t *sys)
{
    uint16_t x, y;
    if (!sys->is_touched(&x, &y)) { g->touching = 0; return; }

    if (x >= 288 && y < 16) { g->running = 0; return; }   /* QUIT 角落 */

    if (!g->touching) {                     /* 手指剛按下 → 記起點 */
        g->touching = 1;
        g->sx = x; g->sy = y;
        return;
    }

    int dx = (int)x - (int)g->sx;
    int dy = (int)y - (int)g->sy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx >= SWIPE_TH || ady >= SWIPE_TH) {   /* 滑夠遠 → 轉向 */
        set_dir(g, dx, dy);
        g->sx = x; g->sy = y;                   /* 重設起點，支援連續轉 */
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
    else     { cell(sys, g->bx[g->len - 1], g->by[g->len - 1], MOD_BLACK); } /* 抹尾 */

    for (int i = newlen - 1; i > 0; i--) { g->bx[i] = g->bx[i-1]; g->by[i] = g->by[i-1]; }
    g->len = newlen;
    g->bx[0] = (uint8_t)nx; g->by[0] = (uint8_t)ny;
    cell(sys, nx, ny, MOD_GREEN);

    if (eat) place_food(g, sys);
}

/* ── module 進入點（放 .entry → .bin offset 0）── */
__attribute__((section(".entry"), used))
int module_main(const syscall_t *sys)
{
    Snake g;
    snake_init(&g, sys);

    int acc = 0;                           /* 距離上次前進累積的時間 */
    while (g.running && g.alive) {
        poll_input(&g, sys);               /* 每 POLL_MS 抓一次觸控（追滑動）*/
        if (!g.running) break;

        sys->delay_ms(POLL_MS);
        acc += POLL_MS;

        int tick = 180 - g.score * 4;      /* 吃越多越快 */
        if (tick < 80) tick = 80;
        if (acc >= tick) {                 /* 到了才前進一格 */
            snake_step(&g, sys);
            acc = 0;
        }
    }

    if (!g.alive) {                        /* 撞死 → GAME OVER，點一下退出 */
        sys->fill_rect(60, 96, 200, 48, MOD_BLUE);
        sys->draw_str(124, 104, "GAME OVER",   MOD_WHITE,  MOD_BLUE);
        sys->draw_str(116, 124, "tap to exit", MOD_YELLOW, MOD_BLUE);
        uint16_t x, y;
        while (!sys->is_touched(&x, &y)) sys->delay_ms(30);   /* 等按下 */
        while ( sys->is_touched(&x, &y)) sys->delay_ms(30);   /* 等放開 */
    }

    return g.score;
}
