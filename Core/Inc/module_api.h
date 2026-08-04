#ifndef MODULE_API_H
#define MODULE_API_H

#include <stdint.h>

/* ABI between the firmware and loadable modules.
 *
 * Modules are compiled separately yet call into the firmware: at run time the
 * firmware hands over a pointer to this table with real addresses filled in,
 * so a module never needs to know any firmware address at compile time — which
 * is what lets it skip relocation entirely.
 *
 * Both sides include this header, so the layout must match exactly. Changing
 * the struct changes the ABI: rebuild the firmware AND every module, or a stale
 * module will read a function pointer at the old offset and jump into garbage. */

#define MOD_SCREEN_W   320
#define MOD_SCREEN_H   240

/* RGB565, duplicated here so modules need not pull in ili9341.h or the HAL. */
#define MOD_BLACK      0x0000
#define MOD_WHITE      0xFFFF
#define MOD_RED        0xF800
#define MOD_GREEN      0x07E0
#define MOD_BLUE       0x001F
#define MOD_YELLOW     0xFFE0
#define MOD_CYAN       0x07FF
#define MOD_MAGENTA    0xF81F
#define MOD_NAVY       0x0210
#define MOD_GRAY       0x8410

/* Syscall table. Always append new entries at the end: existing offsets stay
 * put, so modules built against an older version keep working. */
typedef struct {
    void (*fill_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
    void (*draw_str) (uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
    void (*delay_ms) (uint32_t ms);
    int  (*is_touched)(uint16_t *x, uint16_t *y);   /* 1 = touched, fills x/y */
    int  (*read_file)(const char *path, void *buf, uint32_t maxlen); /* bytes read, -1 on failure */
    int  (*list_dir) (const char *dir, char *out13, int max);        /* names, 13 bytes each; returns count */
} syscall_t;

/* Module entry point: receives the syscall table, returns a status. */
typedef int (*module_entry_t)(const syscall_t *sys);

#endif /* MODULE_API_H */
