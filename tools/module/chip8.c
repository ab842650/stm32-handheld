/* ═══════════════════════════════════════════════════════════════════════
 * CHIP-8 interpreter — one module that runs many ROMs from /ROMS.
 *
 * Where the loader executes native code, this executes code for a different
 * architecture entirely. Uses sys->list_dir and sys->read_file to find and
 * load ROMs. 64x32 display at 4x, with a 4x4 touch keypad for the 16 keys.
 *
 * Build:  cd tools/module && bash build.sh chip8.c   -> CHIP8.BIN
 * ═══════════════════════════════════════════════════════════════════════ */

#include "module_api.h"

#define SCALE   4
#define DISP_X  32
#define DISP_Y  20

#define KX      6
#define KY      152
#define KW      76
#define KH      18
#define KCOL    78
#define KROW    22

/* keypad cell -> CHIP-8 key, in the traditional layout */
static const uint8_t KEYMAP[4][4] = {
    {0x1, 0x2, 0x3, 0xC},
    {0x4, 0x5, 0x6, 0xD},
    {0x7, 0x8, 0x9, 0xE},
    {0xA, 0x0, 0xB, 0xF},
};

/* built-in font: 16 hex digits, 5 bytes each */
static const uint8_t FONT[80] = {
    0xF0,0x90,0x90,0x90,0xF0, 0x20,0x60,0x20,0x20,0x70,
    0xF0,0x10,0xF0,0x80,0xF0, 0xF0,0x10,0xF0,0x10,0xF0,
    0x90,0x90,0xF0,0x10,0x10, 0xF0,0x80,0xF0,0x10,0xF0,
    0xF0,0x80,0xF0,0x90,0xF0, 0xF0,0x10,0x20,0x40,0x40,
    0xF0,0x90,0xF0,0x90,0xF0, 0xF0,0x90,0xF0,0x10,0xF0,
    0xF0,0x90,0xF0,0x90,0x90, 0xE0,0x90,0xE0,0x90,0xE0,
    0xF0,0x80,0x80,0x80,0xF0, 0xE0,0x90,0x90,0x90,0xE0,
    0xF0,0x80,0xF0,0x80,0xF0, 0xF0,0x80,0xF0,0x80,0x80,
};

typedef struct {
    uint8_t  mem[4096];
    uint8_t  V[16];
    uint16_t I, pc;
    uint16_t stack[16];
    uint8_t  sp;
    uint8_t  dt, st;          /* delay / sound timer */
    uint8_t  gfx[32][64];     /* 0/1 */
    uint8_t  keys[16];
    uint32_t rng;
    int      running;
} C8;

static uint32_t rng_next(C8 *c)
{
    uint32_t x = c->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    c->rng = x; return x;
}

static char hexch(uint8_t k) { return k < 10 ? (char)('0' + k) : (char)('A' + k - 10); }

static void build_rom_path(char *dst, const char *name)
{
    const char *p = "/ROMS/";
    int i = 0;
    while (*p) dst[i++] = *p++;
    while (*name) dst[i++] = *name++;
    dst[i] = 0;
}

static void wait_tap(const syscall_t *sys)
{
    uint16_t x, y;
    while (!sys->is_touched(&x, &y)) sys->delay_ms(30);
    while ( sys->is_touched(&x, &y)) sys->delay_ms(30);
}

/* ── Display ── */
static void draw_px(const syscall_t *sys, int px, int py, int on)
{
    sys->fill_rect(DISP_X + px * SCALE, DISP_Y + py * SCALE, SCALE, SCALE,
                   on ? MOD_WHITE : MOD_BLACK);
}
static void op_cls(const syscall_t *sys, C8 *c)
{
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++) c->gfx[y][x] = 0;
    sys->fill_rect(DISP_X, DISP_Y, 64 * SCALE, 32 * SCALE, MOD_BLACK);
}

static void draw_keypad(const syscall_t *sys)
{
    for (int r = 0; r < 4; r++)
        for (int cc = 0; cc < 4; cc++) {
            int x = KX + cc * KCOL, y = KY + r * KROW;
            sys->fill_rect(x, y, KW, KH, MOD_NAVY);
            char lbl[2] = { hexch(KEYMAP[r][cc]), 0 };
            sys->draw_str(x + KW / 2 - 4, y + 1, lbl, MOD_WHITE, MOD_NAVY);
        }
}
static void draw_ui(const syscall_t *sys, const char *rom)
{
    sys->fill_rect(0, 0, MOD_SCREEN_W, MOD_SCREEN_H, MOD_BLACK);
    sys->draw_str(4, 2, rom, MOD_WHITE, MOD_BLACK);
    sys->fill_rect(284, 0, 36, 18, MOD_RED);
    sys->draw_str(286, 2, "QUIT", MOD_WHITE, MOD_RED);
    draw_keypad(sys);
}

/* ── Input: one key at a time ── */
static void poll_keys(const syscall_t *sys, C8 *c)
{
    for (int k = 0; k < 16; k++) c->keys[k] = 0;

    uint16_t x, y;
    if (!sys->is_touched(&x, &y)) return;
    if (x >= 284 && y < 18) { c->running = 0; return; }        /* QUIT */

    if (x >= KX && y >= KY) {
        int col = (x - KX) / KCOL, row = (y - KY) / KROW;
        if (col < 4 && row < 4) c->keys[KEYMAP[row][col]] = 1;
    }
}

/* Runs one opcode. Returns 1 if FX0A is blocking on a keypress. */
static int step(const syscall_t *sys, C8 *c)
{
    uint16_t op = (uint16_t)(c->mem[c->pc] << 8 | c->mem[c->pc + 1]);
    c->pc += 2;
    uint8_t  X = (op >> 8) & 0xF, Y = (op >> 4) & 0xF, N = op & 0xF;
    uint8_t  NN = op & 0xFF;
    uint16_t NNN = op & 0xFFF;

    switch (op >> 12) {
    case 0x0:
        if (op == 0x00E0) op_cls(sys, c);
        else if (op == 0x00EE) c->pc = c->stack[--c->sp];
        break;
    case 0x1: c->pc = NNN; break;
    case 0x2: c->stack[c->sp++] = c->pc; c->pc = NNN; break;
    case 0x3: if (c->V[X] == NN) c->pc += 2; break;
    case 0x4: if (c->V[X] != NN) c->pc += 2; break;
    case 0x5: if (c->V[X] == c->V[Y]) c->pc += 2; break;
    case 0x6: c->V[X] = NN; break;
    case 0x7: c->V[X] = (uint8_t)(c->V[X] + NN); break;
    case 0x8:
        switch (N) {
        case 0x0: c->V[X] = c->V[Y]; break;
        case 0x1: c->V[X] |= c->V[Y]; break;
        case 0x2: c->V[X] &= c->V[Y]; break;
        case 0x3: c->V[X] ^= c->V[Y]; break;
        case 0x4: { int s = c->V[X] + c->V[Y]; c->V[0xF] = s > 255; c->V[X] = (uint8_t)s; } break;
        case 0x5: { int b = c->V[X] >= c->V[Y]; c->V[X] = (uint8_t)(c->V[X] - c->V[Y]); c->V[0xF] = b; } break;
        case 0x6: { uint8_t l = c->V[X] & 1; c->V[X] >>= 1; c->V[0xF] = l; } break;
        case 0x7: { int b = c->V[Y] >= c->V[X]; c->V[X] = (uint8_t)(c->V[Y] - c->V[X]); c->V[0xF] = b; } break;
        case 0xE: { uint8_t h = (c->V[X] >> 7) & 1; c->V[X] <<= 1; c->V[0xF] = h; } break;
        }
        break;
    case 0x9: if (c->V[X] != c->V[Y]) c->pc += 2; break;
    case 0xA: c->I = NNN; break;
    case 0xB: c->pc = (uint16_t)(NNN + c->V[0]); break;
    case 0xC: c->V[X] = (uint8_t)(rng_next(c) & NN); break;
    case 0xD: {
        uint8_t vx = c->V[X] & 63, vy = c->V[Y] & 31;
        c->V[0xF] = 0;
        for (int row = 0; row < N; row++) {
            uint8_t sp = c->mem[c->I + row];
            for (int col = 0; col < 8; col++) {
                if (!(sp & (0x80 >> col))) continue;
                int px = vx + col, py = vy + row;
                if (px >= 64 || py >= 32) continue;        /* clip */
                if (c->gfx[py][px]) c->V[0xF] = 1;
                c->gfx[py][px] ^= 1;
                draw_px(sys, px, py, c->gfx[py][px]);
            }
        }
        break;
    }
    case 0xE:
        if (NN == 0x9E) { if (c->keys[c->V[X] & 0xF]) c->pc += 2; }
        else if (NN == 0xA1) { if (!c->keys[c->V[X] & 0xF]) c->pc += 2; }
        break;
    case 0xF:
        switch (NN) {
        case 0x07: c->V[X] = c->dt; break;
        case 0x0A: {
            int found = -1;
            for (int k = 0; k < 16; k++) if (c->keys[k]) { found = k; break; }
            if (found < 0) { c->pc -= 2; return 1; }        /* wait for a key */
            c->V[X] = (uint8_t)found;
        } break;
        case 0x15: c->dt = c->V[X]; break;
        case 0x18: c->st = c->V[X]; break;
        case 0x1E: c->I = (uint16_t)(c->I + c->V[X]); break;
        case 0x29: c->I = (uint16_t)(c->V[X] * 5); break;   /* font */
        case 0x33:
            c->mem[c->I]     = c->V[X] / 100;
            c->mem[c->I + 1] = (c->V[X] / 10) % 10;
            c->mem[c->I + 2] = c->V[X] % 10;
            break;
        case 0x55: for (int i = 0; i <= X; i++) c->mem[c->I + i] = c->V[i]; break;
        case 0x65: for (int i = 0; i <= X; i++) c->V[i] = c->mem[c->I + i]; break;
        }
        break;
    }
    return 0;
}

static void c8_init(C8 *c)
{
    for (int i = 0; i < 4096; i++) c->mem[i] = 0;
    for (int i = 0; i < 80; i++)   c->mem[i] = FONT[i];
    for (int i = 0; i < 16; i++)   c->V[i] = 0;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) c->gfx[y][x] = 0;
    for (int k = 0; k < 16; k++)   c->keys[k] = 0;
    c->I = 0; c->pc = 0x200; c->sp = 0; c->dt = 0; c->st = 0;
    c->rng = 0xC0FFEEu; c->running = 1;
}

/* Lists /ROMS and returns the chosen index, or -1 on quit. */
static int rom_menu(const syscall_t *sys, char roms[][13], int nrom)
{
    sys->fill_rect(0, 0, MOD_SCREEN_W, MOD_SCREEN_H, MOD_BLACK);
    sys->draw_str(90, 8, "SELECT ROM", MOD_YELLOW, MOD_BLACK);
    sys->fill_rect(284, 0, 36, 18, MOD_RED);
    sys->draw_str(286, 2, "QUIT", MOD_WHITE, MOD_RED);

    int y0 = 40, rh = 26;
    for (int i = 0; i < nrom; i++)
        sys->draw_str(20, y0 + i * rh, roms[i], MOD_WHITE, MOD_BLACK);

    for (;;) {
        uint16_t x, y;
        if (sys->is_touched(&x, &y)) {
            if (x >= 284 && y < 18) { while (sys->is_touched(&x, &y)) sys->delay_ms(20); return -1; }
            if (y >= y0) {
                int idx = (y - y0) / rh;
                if (idx >= 0 && idx < nrom) {
                    while (sys->is_touched(&x, &y)) sys->delay_ms(20);
                    return idx;
                }
            }
        }
        sys->delay_ms(20);
    }
}

__attribute__((section(".entry"), used))
int module_main(const syscall_t *sys)
{
    C8   c;
    char roms[12][13];

    int nrom = sys->list_dir("/ROMS", (char *)roms, 12);
    if (nrom == 0) {
        sys->fill_rect(0, 0, MOD_SCREEN_W, MOD_SCREEN_H, MOD_BLACK);
        sys->draw_str(52, 110, "no ROMs in /ROMS", MOD_RED, MOD_BLACK);
        wait_tap(sys);
        return 0;
    }

    int sel = rom_menu(sys, roms, nrom);
    if (sel < 0) return 0;

    c8_init(&c);
    char path[32];
    build_rom_path(path, roms[sel]);
    int n = sys->read_file(path, c.mem + 0x200, 4096 - 0x200);
    if (n <= 0) {
        sys->fill_rect(0, 0, MOD_SCREEN_W, MOD_SCREEN_H, MOD_BLACK);
        sys->draw_str(80, 110, "ROM load fail", MOD_RED, MOD_BLACK);
        wait_tap(sys);
        return 0;
    }

    draw_ui(sys, roms[sel]);

    /* Per frame: a batch of opcodes, the 60 Hz timers, then input. */
    while (c.running) {
        poll_keys(sys, &c);
        if (!c.running) break;

        for (int i = 0; i < 12; i++)
            if (step(sys, &c)) break;      /* blocked on a keypress */

        if (c.dt > 0) c.dt--;
        if (c.st > 0) c.st--;
        sys->delay_ms(16);
    }
    return 0;
}
