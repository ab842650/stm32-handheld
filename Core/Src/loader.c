#include "loader.h"
#include "main.h"       /* 帶進 CMSIS：__DSB() / __ISB() */
#include "fatfs.h"
#include "module_api.h" /* syscall_t / module_entry_t 契約 */
#include "ili9341.h"    /* 給 module 用的畫圖函式 */
#include "xpt2046.h"    /* 觸控 */
#include "myprintf.h"   /* demo：印出載入位址 */
#include "FreeRTOS.h"
#include "task.h"       /* vTaskDelay */
#include <stdint.h>
#include <string.h>     /* strcpy（list_dir）*/

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * MCU 版 loader（M2a：位置無關 module）
 *
 * module 用 -fpic 編譯 → 位置無關，載到任意位址都能跑。所以不再需要固定
 * 預留區，直接讀進主韌體的一個 static 陣列 module_ram[]（位址由 linker
 * 隨便給）。SRAM 可執行 → 這塊 .bss 陣列可以當「碼」跑。
 *
 * module 被「當一般函式呼叫」，進入點在 .bin 的 offset 0，並把 syscall
 * 表指標當參數傳給它。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define MODULE_SIZE  (16u * 1024u)      /* module 緩衝大小 */

/* module 載入緩衝：普通 static 陣列，位址由 linker 決定（絕不是 0x2001C000）。
 * aligned(8)：確保碼/文字池的字組存取對齊。 */
static uint8_t module_ram[MODULE_SIZE] __attribute__((aligned(8)));

/* ── syscall wrapper：把主韌體功能包成 module 能呼叫的統一介面 ── */
static void sys_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));       /* RTOS 友善延遲（讓出 CPU）*/
}
static int sys_touched(uint16_t *x, uint16_t *y)
{
    return XPT2046_ReadPixel(x, y) ? 1 : 0;
}
/* 讀整個檔進 buf；回實際讀到的位元組數，開檔失敗回 -1 */
static int sys_read_file(const char *path, void *buf, uint32_t maxlen)
{
    FIL  f;
    UINT br = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    f_read(&f, buf, maxlen, &br);
    f_close(&f);
    return (int)br;
}
/* 列出 dir 內的檔名（跳過資料夾），每項 13 bytes 寫進 out；回檔案數 */
static int sys_list_dir(const char *dir, char *out13, int max)
{
    DIR     d;
    FILINFO fno;
    int     n = 0;
    if (f_opendir(&d, dir) != FR_OK) return 0;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0] != 0 && n < max) {
        if (fno.fattrib & AM_DIR) continue;
        strcpy(out13 + n * 13, fno.fname);
        n++;
    }
    f_closedir(&d);
    return n;
}

/* 主韌體提供給 module 的系統呼叫表（填好真實函式位址）*/
static const syscall_t g_syscalls = {
    .fill_rect  = ILI9341_FillRect,      /* 簽章與 syscall_t 一致，可直接指派 */
    .draw_str   = ILI9341_DrawString,
    .delay_ms   = sys_delay,
    .is_touched = sys_touched,
    .read_file  = sys_read_file,
    .list_dir   = sys_list_dir,
};

int Loader_RunModule(const char *path)
{
	FIL f;
	UINT br;

	if(f_open(&f,path,FA_READ)!= FR_OK)return -1;

	f_read(&f, module_ram, MODULE_SIZE, &br);       // 讀進「隨便的」static 陣列
	f_close(&f);

	__DSB();
	__ISB();

	/* demo：印出 module 實際載到哪 —— 你會看到它不是 0x2001C000 */
	myprintf("MODULE: loaded @ 0x%x (%u bytes)\r\n",
	         (unsigned)(uintptr_t)module_ram, br);

	/* 位址取自陣列指標、設 Thumb bit → 位置無關的碼載到這裡照樣跑 */
	module_entry_t entry = (module_entry_t)(((uintptr_t)module_ram) | 1u);
	return entry(&g_syscalls);                      // 跳進去，並把 syscall 表傳給它
}
