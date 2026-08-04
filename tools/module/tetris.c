/* ═══════════════════════════════════════════════════════════════════════
 * Tetris — position-independent game module loaded from SD.
 *
 * 10x20 board on the left, touch buttons on the right. Like snake.c, all state
 * lives in a local struct rather than file-scope globals.
 *
 * Build:  cd tools/module && bash build.sh tetris.c   -> TETRIS.BIN
 * ═══════════════════════════════════════════════════════════════════════ */

#include "module_api.h"

#define COLS      10
#define ROWS      20
#define CELL      11
#define BOARD_X   4
#define BOARD_Y   8          /* board occupies x 4..114, y 8..228 */

#define POLL_MS   30

/* 7 pieces x 4 rotations as 4x4 bit masks; 0x8000 is top-left, one nibble per row. */
static const uint16_t PIECES[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, /* I */
    {0x0660, 0x0660, 0x0660, 0x0660}, /* O */
    {0x0E40, 0x4C40, 0x4E00, 0x4640}, /* T */
    {0x06C0, 0x8C40, 0x6C00, 0x4620}, /* S */
    {0x0C60, 0x4C80, 0xC600, 0x2640}, /* Z */
    {0x44C0, 0x8E00, 0xC880, 0xE200}, /* J */
    {0x4460, 0x0E80, 0xC440, 0x2E00}, /* L */
};
/* indexed by board value: 0 empty, 1..7 piece type + 1 */
static const uint16_t COLORS[8] = {
    MOD_BLACK, MOD_CYAN, MOD_YELLOW, MOD_MAGENTA,
    MOD_GREEN, MOD_RED, MOD_BLUE, MOD_WHITE
};

typedef struct {
    uint8_t  board[ROWS][COLS];   /* 0 = empty */
    int      type, rot, px, py;   /* active piece */
    int      nexttype;
    int      score, lines;
    int      alive, running;
    uint32_t rng;
    int      touching;            /* edge detect: one action per tap */
} Tetris;

static uint32_t rng_next(Tetris *g)
{
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x; return x;
}
static void num2str(char *b, int n)
{
    char t[8]; int i = 0;
    if (n <= 0) { b[0] = '0'; b[1] = 0; return; }
    while (n > 0 && i < 7) { t[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0; while (i > 0) b[j++] = t[--i]; b[j] = 0;
}
static int occ(int type, int rot, int r, int c)
{
    return (PIECES[type][rot] & (0x8000 >> (r * 4 + c))) != 0;
}

static void draw_cell(const syscall_t *sys, int col, int row, uint16_t color)
{
    int px = BOARD_X + col * CELL, py = BOARD_Y + row * CELL;
    sys->fill_rect(px, py, CELL, CELL, MOD_BLACK);
    if (color != MOD_BLACK)
        sys->fill_rect(px + 1, py + 1, CELL - 2, CELL - 2, color);
}

/* MOD_BLACK erases. */
static void draw_piece(const syscall_t *sys, int type, int rot, int px, int py, uint16_t color)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (occ(type, rot, r, c) && (py + r) >= 0)
                draw_cell(sys, px + c, py + r, color);
}

/* Legal placement: in bounds and not overlapping. */
static int valid(Tetris *g, int type, int rot, int px, int py)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!occ(type, rot, r, c)) continue;
            int bc = px + c, br = py + r;
            if (bc < 0 || bc >= COLS || br >= ROWS) return 0;
            if (br >= 0 && g->board[br][bc])        return 0;
        }
    return 1;
}

static void draw_score(const syscall_t *sys, Tetris *g)
{
    char buf[16] = "LINES ";
    num2str(buf + 6, g->lines);
    sys->fill_rect(120, 6, 200, 16, MOD_BLACK);
    sys->draw_str(120, 6, buf, MOD_WHITE, MOD_BLACK);
}

static void redraw_board(const syscall_t *sys, Tetris *g)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            draw_cell(sys, c, r, COLORS[g->board[r][c]]);
}

static void draw_buttons(const syscall_t *sys)
{
    sys->fill_rect(284, 0, 36, 20, MOD_RED);
    sys->draw_str(286, 2, "QUIT", MOD_WHITE, MOD_RED);
    /* ◄  ► */
    sys->fill_rect(120, 56, 92, 44, MOD_NAVY);
    sys->draw_str(158, 70, "<", MOD_WHITE, MOD_NAVY);
    sys->fill_rect(216, 56, 92, 44, MOD_NAVY);
    sys->draw_str(254, 70, ">", MOD_WHITE, MOD_NAVY);
    /* ROTATE */
    sys->fill_rect(120, 110, 188, 44, MOD_NAVY);
    sys->draw_str(180, 124, "ROTATE", MOD_WHITE, MOD_NAVY);
    /* DROP */
    sys->fill_rect(120, 164, 188, 44, MOD_NAVY);
    sys->draw_str(196, 178, "DROP", MOD_WHITE, MOD_NAVY);
}

static void spawn(Tetris *g)
{
    g->type = g->nexttype;
    g->nexttype = rng_next(g) % 7;
    g->rot = 0; g->px = 3; g->py = 0;
    if (!valid(g, g->type, g->rot, g->px, g->py)) g->alive = 0;
}

/* Move or rotate if the result is legal. */
static int try_move(Tetris *g, const syscall_t *sys, int dx, int dy, int drot)
{
    int nrot = (g->rot + drot) & 3;
    int nx = g->px + dx, ny = g->py + dy;
    if (!valid(g, g->type, nrot, nx, ny)) return 0;
    draw_piece(sys, g->type, g->rot, g->px, g->py, MOD_BLACK);
    g->rot = nrot; g->px = nx; g->py = ny;
    draw_piece(sys, g->type, g->rot, g->px, g->py, COLORS[g->type + 1]);
    return 1;
}

static void lock_next(Tetris *g, const syscall_t *sys)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (occ(g->type, g->rot, r, c) && (g->py + r) >= 0)
                g->board[g->py + r][g->px + c] = (uint8_t)(g->type + 1);

    int cleared = 0;
    for (int r = ROWS - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < COLS; c++) if (!g->board[r][c]) { full = 0; break; }
        if (full) {
            for (int rr = r; rr > 0; rr--)
                for (int c = 0; c < COLS; c++) g->board[rr][c] = g->board[rr - 1][c];
            for (int c = 0; c < COLS; c++) g->board[0][c] = 0;
            cleared++; r++;                     /* re-check the same row */
        }
    }
    if (cleared) {
        g->lines += cleared;
        redraw_board(sys, g);
        draw_score(sys, g);
    }
    spawn(g);
    if (g->alive)
        draw_piece(sys, g->type, g->rot, g->px, g->py, COLORS[g->type + 1]);
}

static void hard_drop(Tetris *g, const syscall_t *sys)
{
    draw_piece(sys, g->type, g->rot, g->px, g->py, MOD_BLACK);
    while (valid(g, g->type, g->rot, g->px, g->py + 1)) g->py++;
    draw_piece(sys, g->type, g->rot, g->px, g->py, COLORS[g->type + 1]);
    lock_next(g, sys);
}

static void poll_input(Tetris *g, const syscall_t *sys)
{
    uint16_t x, y;
    if (!sys->is_touched(&x, &y)) { g->touching = 0; return; }
    if (g->touching) return;                    /* still held */
    g->touching = 1;
    g->rng ^= ((uint32_t)x << 16) ^ y;

    if (x >= 284 && y < 20)                      g->running = 0;             /* QUIT */
    else if (y >= 56 && y < 100 && x < 212)      try_move(g, sys, -1, 0, 0); /* ◄ */
    else if (y >= 56 && y < 100 && x >= 216)     try_move(g, sys, +1, 0, 0); /* ► */
    else if (y >= 110 && y < 154)                try_move(g, sys, 0, 0, 1);  /* ROTATE */
    else if (y >= 164 && y < 208)                hard_drop(g, sys);          /* DROP */
}

__attribute__((section(".entry"), used))
int module_main(const syscall_t *sys)
{
    Tetris g;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) g.board[r][c] = 0;
    g.score = 0; g.lines = 0;
    g.alive = 1; g.running = 1;
    g.rng = 0x2468ace1u; g.touching = 0;

    sys->fill_rect(0, 0, MOD_SCREEN_W, MOD_SCREEN_H, MOD_BLACK);
    sys->fill_rect(BOARD_X - 2, BOARD_Y - 2, COLS * CELL + 4, 2, MOD_WHITE);
    sys->fill_rect(BOARD_X - 2, BOARD_Y + ROWS * CELL, COLS * CELL + 4, 2, MOD_WHITE);
    sys->fill_rect(BOARD_X - 2, BOARD_Y - 2, 2, ROWS * CELL + 4, MOD_WHITE);
    sys->fill_rect(BOARD_X + COLS * CELL, BOARD_Y - 2, 2, ROWS * CELL + 4, MOD_WHITE);
    draw_buttons(sys);
    draw_score(sys, &g);

    g.nexttype = (int)(rng_next(&g) % 7);
    spawn(&g);
    draw_piece(sys, g.type, g.rot, g.px, g.py, COLORS[g.type + 1]);

    int acc = 0;
    while (g.running && g.alive) {
        poll_input(&g, sys);
        if (!g.running) break;

        sys->delay_ms(POLL_MS);
        acc += POLL_MS;

        int tick = 600 - g.lines * 20;          /* speeds up with lines cleared */
        if (tick < 120) tick = 120;
        if (acc >= tick) {
            if (!try_move(&g, sys, 0, 1, 0))    /* cannot fall: lock it */
                lock_next(&g, sys);
            acc = 0;
        }
    }

    if (!g.alive) {
        sys->fill_rect(40, 100, 240, 48, MOD_BLUE);
        sys->draw_str(104, 108, "GAME OVER",   MOD_WHITE,  MOD_BLUE);
        sys->draw_str(96,  128, "tap to exit", MOD_YELLOW, MOD_BLUE);
        uint16_t x, y;
        while (!sys->is_touched(&x, &y)) sys->delay_ms(30);
        while ( sys->is_touched(&x, &y)) sys->delay_ms(30);
    }
    return g.lines;
}
