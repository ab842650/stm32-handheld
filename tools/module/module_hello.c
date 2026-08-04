/* ─────────────────────────────────────────────────────────────
 * Smallest possible module: draws through the syscall table.
 *
 * The module is compiled separately and knows no firmware addresses. It can
 * draw only because the firmware passes in `sys`, a table of function pointers
 * filled with real addresses at call time — the same bargain as an OS syscall
 * interface.
 * ───────────────────────────────────────────────────────────── */

#include "module_api.h"

__attribute__((section(".entry"), used))
int module_main(const syscall_t *sys)
{
    sys->fill_rect(0, 32, MOD_SCREEN_W, 178, MOD_BLUE);

    sys->draw_str(92, 60,  "HELLO FROM MODULE",   MOD_WHITE,  MOD_BLUE);
    sys->draw_str(84, 90,  "loaded from SD card", MOD_YELLOW, MOD_BLUE);

    sys->fill_rect(60,  135, 40, 40, MOD_RED);
    sys->fill_rect(140, 135, 40, 40, MOD_GREEN);
    sys->fill_rect(220, 135, 40, 40, MOD_YELLOW);

    return 0x1234;
}
