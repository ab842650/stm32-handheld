/* ─────────────────────────────────────────────────────────────
 * M1 測試 module —— 透過 syscall 表呼叫主韌體畫東西
 *
 * module 是分開編譯的，不知道主韌體任何函式的位址。它能畫圖，
 * 全靠主韌體在呼叫時傳進來的 `sys` 表（一堆填好真實位址的函式指標）。
 * 這就是 OS 的 syscall：app 不直接碰硬體，透過核心提供的介面呼叫服務。
 *
 * 注意：進入點簽章從 M0 的 (void) 改成收 (const syscall_t *sys)。
 * ───────────────────────────────────────────────────────────── */

#include "module_api.h"

__attribute__((section(".entry"), used))
int module_main(const syscall_t *sys)
{
    /* 內容區（y = 32~210）鋪一層深藍當背景 */
    sys->fill_rect(0, 32, MOD_SCREEN_W, 178, MOD_BLUE);

    /* 兩行字：全部透過 sys 表呼叫主韌體的 DrawString */
    sys->draw_str(92, 60,  "HELLO FROM MODULE",   MOD_WHITE,  MOD_BLUE);
    sys->draw_str(84, 90,  "loaded from SD card", MOD_YELLOW, MOD_BLUE);

    /* 三個色塊，證明能自由畫 */
    sys->fill_rect(60,  135, 40, 40, MOD_RED);
    sys->fill_rect(140, 135, 40, 40, MOD_GREEN);
    sys->fill_rect(220, 135, 40, 40, MOD_YELLOW);

    return 0x1234;
}
