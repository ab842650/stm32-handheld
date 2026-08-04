#include "loader.h"
#include "main.h"       /* CMSIS: __DSB() / __ISB() */
#include "fatfs.h"
#include "module_api.h"
#include "ili9341.h"
#include "xpt2046.h"
#include "myprintf.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
#include <string.h>

/* Module loader.
 *
 * Modules are built with -fpic, so they run correctly wherever they land. That
 * removes the need for a fixed reserved region: the .bin is read into an
 * ordinary static array whose address the linker picks. SRAM is executable on
 * this part, so a .bss array can be jumped into as code.
 *
 * The entry point is at offset 0 of the .bin and is called like a normal
 * function, receiving the syscall table as its argument. */

#define MODULE_SIZE  (16u * 1024u)

/* aligned(8) keeps word accesses to code and literal pools aligned. */
static uint8_t module_ram[MODULE_SIZE] __attribute__((aligned(8)));

static void sys_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));       /* yields, unlike a busy-wait */
}
static int sys_touched(uint16_t *x, uint16_t *y)
{
    return XPT2046_ReadPixel(x, y) ? 1 : 0;
}
/* Whole file into buf; returns bytes read, or -1 if it could not be opened. */
static int sys_read_file(const char *path, void *buf, uint32_t maxlen)
{
    FIL  f;
    UINT br = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    f_read(&f, buf, maxlen, &br);
    f_close(&f);
    return (int)br;
}
/* File names in dir, 13 bytes per entry, directories skipped. Returns count. */
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

/* Signatures already match syscall_t, so these can be assigned directly. */
static const syscall_t g_syscalls = {
    .fill_rect  = ILI9341_FillRect,
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

	f_read(&f, module_ram, MODULE_SIZE, &br);
	f_close(&f);

	/* Flush the write and the pipeline before executing what we just wrote. */
	__DSB();
	__ISB();

	myprintf("MODULE: loaded @ 0x%x (%u bytes)\r\n",
	         (unsigned)(uintptr_t)module_ram, br);

	/* Thumb bit set on the array address — position-independent code runs fine
	 * from wherever the linker happened to put the buffer. */
	module_entry_t entry = (module_entry_t)(((uintptr_t)module_ram) | 1u);
	return entry(&g_syscalls);
}
