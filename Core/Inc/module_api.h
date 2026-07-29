#ifndef MODULE_API_H
#define MODULE_API_H

#include <stdint.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Module ABI —— 主韌體與可載入 module 之間的「契約」
 *
 * 主韌體和每個 module 都 include 這個檔，struct 佈局必須完全一致。
 * 改這個 struct = 改 ABI → 主韌體和「所有」module 都要重新編譯，否則
 * module 會照舊佈局去讀函式指標 → 呼叫到錯的位址 → 當機。
 *
 * 關鍵：module 是分開編譯的，卻能呼叫主韌體的函式，靠的就是這張表——
 * 執行期主韌體把「填好真實位址的表」的指標傳給 module，module 從表裡
 * 取指標來呼叫，全程不需要知道主韌體函式的編譯期位址（也就免掉重定位）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* 螢幕尺寸 */
#define MOD_SCREEN_W   320
#define MOD_SCREEN_H   240

/* RGB565 顏色（讓 module 不必 include 整個 ili9341.h / HAL）*/
#define MOD_BLACK      0x0000
#define MOD_WHITE      0xFFFF
#define MOD_RED        0xF800
#define MOD_GREEN      0x07E0
#define MOD_BLUE       0x001F
#define MOD_YELLOW     0xFFE0
#define MOD_CYAN       0x07FF
#define MOD_MAGENTA    0xF81F
#define MOD_NAVY       0x0210    /* 深藍（按鈕底色）*/
#define MOD_GRAY       0x8410    /* 中灰 */

/* 主韌體提供給 module 的系統呼叫表。
 * 新功能一律「加在最後」——舊 module 讀前面的欄位偏移不變，不會壞。 */
typedef struct {
    void (*fill_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
    void (*draw_str) (uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
    void (*delay_ms) (uint32_t ms);
    int  (*is_touched)(uint16_t *x, uint16_t *y);   /* 有碰=1 並填座標；沒碰=0 */
    /* ── 檔案存取（讀 ROM / 關卡）── */
    int  (*read_file)(const char *path, void *buf, uint32_t maxlen); /* 讀整檔進 buf，回位元組數，失敗 -1 */
    int  (*list_dir) (const char *dir, char *out13, int max);        /* 列 dir 內檔名(每項 13 bytes)，回數量 */
} syscall_t;

/* module 進入點型別：收 syscall 表指標，回傳 int */
typedef int (*module_entry_t)(const syscall_t *sys);

#endif /* MODULE_API_H */
