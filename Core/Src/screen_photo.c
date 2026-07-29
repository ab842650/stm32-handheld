#include "screen.h"
#include "screen_photo.h"
#include "ili9341.h"
#include "ui.h"
#include "fatfs.h"
#include "tjpgd.h"
#include <stdint.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Photo — 從 SD 卡讀圖全螢幕顯示，支援 BMP 與 JPG
 *
 *  BMP：自己解 header + 逐列 BGR→RGB565（三個坑：BGR / bottom-up / 4-byte padding）
 *  JPG：交給 TJpgDec 解碼，你只提供兩個 callback（input / output）把它接到 SD + 螢幕
 *
 *  圖片全螢幕顯示，點畫面任一處返回。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static uint8_t bgrbuf[ILI9341_WIDTH * 3];   /* BMP：一列 BGR 原始資料 */
static uint8_t rowbuf[ILI9341_WIDTH * 2];   /* BMP：一列轉好的 RGB565 */

/* 小端讀取（BMP header 是 little-endian）*/
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void photo_error(const char *msg)
{
    ILI9341_FillScreen(ILI9341_BLACK);
    UI_DrawCentered(ILI9341_HEIGHT / 2 - 8, msg, ILI9341_RED, ILI9341_BLACK);
}

/* 錯誤 + 一個數字代碼（給 JPEG 錯誤碼用）*/
static void photo_error_code(const char *msg, int code)
{
    char buf[40];
    int i = 0;
    while (msg[i] && i < 34) { buf[i] = msg[i]; i++; }
    buf[i++] = ' ';
    buf[i++] = '0' + (code % 10);
    buf[i]   = 0;
    photo_error(buf);
}

/* ═══════════════════════════ BMP ═══════════════════════════ */
static void photo_show_bmp(const char *path)
{
    FIL     fil;
    UINT    br;
    uint8_t hdr[54];

    if (f_open(&fil, path, FA_READ) != FR_OK) {
        photo_error("open fail");
        return;
    }

    f_read(&fil, hdr, 54, &br);
    if (br != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        f_close(&fil); photo_error("not a BMP"); return;
    }
    uint32_t offset = rd32(hdr + 10);
    int32_t  width  = (int32_t)rd32(hdr + 18);
    int32_t  height = (int32_t)rd32(hdr + 22);
    uint16_t bpp    = rd16(hdr + 28);
    uint32_t comp   = rd32(hdr + 30);

    if (bpp != 24 || comp != 0) {
        f_close(&fil); photo_error("need 24-bit BMP"); return;
    }

    uint16_t w = (width  > ILI9341_WIDTH ) ? ILI9341_WIDTH  : (uint16_t)width;
    uint16_t h = (height > ILI9341_HEIGHT) ? ILI9341_HEIGHT : (uint16_t)height;
    uint32_t rowbytes = ((uint32_t)width * 3 + 3) & ~3u;

    /* 置中位移 */
    uint16_t offx = (w < ILI9341_WIDTH ) ? (ILI9341_WIDTH  - w) / 2 : 0;
    uint16_t offy = (h < ILI9341_HEIGHT) ? (ILI9341_HEIGHT - h) / 2 : 0;

    ILI9341_FillScreen(ILI9341_BLACK);
    f_lseek(&fil, offset);

    for (int r = 0; r < h; r++) {
        f_read(&fil, bgrbuf, rowbytes, &br);

        for (int col = 0; col < w; col++) {
            uint8_t B = bgrbuf[col * 3 + 0];
            uint8_t G = bgrbuf[col * 3 + 1];
            uint8_t R = bgrbuf[col * 3 + 2];
            uint16_t color = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
            rowbuf[col * 2]     = color >> 8;      /* hi */
            rowbuf[col * 2 + 1] = color & 0xFF;    /* lo */
        }

        uint16_t screenY = h - 1 - r;               /* bottom-up → 翻回正立 */
        ILI9341_BlitBytes(offx, offy + screenY, w, 1, rowbuf);
    }

    f_close(&fil);
}

/* ═══════════════════════════ JPG (TJpgDec) ═══════════════════════════
 *
 *  TJpgDec 跟 FatFs 同作者，核心 tjpgd.c 不管硬體，靠你給的兩個 callback
 *  把它接到「輸入(SD 檔)」與「輸出(螢幕)」：
 *
 *    jd_prepare(&jdec, jpg_in,  work, sz, &io)   讀 header、算尺寸
 *    jd_decomp (&jdec, jpg_out, scale)           解碼，逐塊呼叫 jpg_out
 *
 *  設定：JD_FORMAT=1 → 核心已幫你把每塊轉成 RGB565（見 tjpgdcnf.h）
 *  但那是「uint16_t 原生小端」，ILI9341 要大端(hi 先) → jpg_out 裡要 swap。
 */

/* TJpgDec 解碼用的工作記憶體（官方最小 3092 bytes；這裡給足）*/
static uint8_t jd_work[4096];

/* 傳給 callback 的「I/O 裝置」— 這裡就是開好的檔案 */
typedef struct {
    FIL *fp;
} jpg_io_t;

/* 置中位移：圖比螢幕小時，把每塊往右下推，讓整張置中 */
static uint16_t jpg_offx, jpg_offy;

/* ── input callback ──────────────────────────────────────────────────
 * 核心跟你要 nbyte 個 JPEG 位元組。
 *   buf != NULL → 從檔案讀 nbyte 到 buf，回傳「實際讀到」的位元組數
 *   buf == NULL → 跳過 nbyte（不需真的讀），回傳「跳過」的位元組數
 * 會用到：f_read / f_lseek / f_tell（f_tell(fp) = 目前檔案位置）
 */
static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    jpg_io_t *io = (jpg_io_t *)jd->device;

    if (buf) {
        UINT br;
        f_read(io->fp, buf, nbyte, &br);    /* 讀進 buf */
        return br;                          /* 回傳實際讀到的量 */
    } else {
        f_lseek(io->fp, f_tell(io->fp) + nbyte);   /* 跳過 nbyte */
        return nbyte;
    }
}

/* ── output callback ─────────────────────────────────────────────────
 * 核心解好一個矩形方塊就呼叫你一次：
 *   bitmap → 指向 (rw*rh) 個 uint16_t RGB565 像素，row-major（小端）
 *   rect   → 這塊在整張圖裡的螢幕座標（left/top/right/bottom，含端點）
 * 你要：把每像素兩 byte 對調成大端，然後 BlitBytes 貼到 (rect->left, rect->top)。
 * 回傳 1 = 繼續解碼；0 = 中止。
 *
 * 提示：可以「就地」交換 bitmap 裡的 byte（p = (p>>8)|(p<<8)），
 *       再把 bitmap 當成 uint8_t* 傳給 BlitBytes，不必另開緩衝。
 */
static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    uint16_t rw = rect->right  - rect->left + 1;
    uint16_t rh = rect->bottom - rect->top  + 1;

    /* 每像素 hi/lo 對調（小端 → 大端），就地改在 bitmap 裡 */
    uint16_t *src = (uint16_t *)bitmap;
    for (uint32_t i = 0; i < (uint32_t)rw * rh; i++) {
        src[i] = (src[i] >> 8) | (src[i] << 8);
    }

    ILI9341_BlitBytes(rect->left + jpg_offx, rect->top + jpg_offy,
                      rw, rh, (const uint8_t *)bitmap);
    return 1;   /* 繼續解碼下一塊 */
}

static void photo_show_jpg(const char *path)
{
    FIL      fil;
    jpg_io_t io = { &fil };
    JDEC     jdec;
    JRESULT  res;

    if (f_open(&fil, path, FA_READ) != FR_OK) {
        photo_error("open fail");
        return;
    }

    /* 讀 header、配置解碼器 */
    res = jd_prepare(&jdec, jpg_in, jd_work, sizeof jd_work, &io);
    if (res != JDR_OK) {
        f_close(&fil); photo_error_code("prepare err", res); return;
    }

    /* 算縮放：0..3 → 1/1, 1/2, 1/4, 1/8，讓圖塞進螢幕 */
    uint8_t scale = 0;
    while (scale < 3 && ((jdec.width  >> scale) > ILI9341_WIDTH ||
                         (jdec.height >> scale) > ILI9341_HEIGHT)) {
        scale++;
    }

    /* 縮放後尺寸 → 算置中位移（圖比螢幕小才置中，否則靠 0）*/
    uint16_t outW = jdec.width  >> scale;
    uint16_t outH = jdec.height >> scale;
    jpg_offx = (outW < ILI9341_WIDTH ) ? (ILI9341_WIDTH  - outW) / 2 : 0;
    jpg_offy = (outH < ILI9341_HEIGHT) ? (ILI9341_HEIGHT - outH) / 2 : 0;

    ILI9341_FillScreen(ILI9341_BLACK);
    res = jd_decomp(&jdec, jpg_out, scale);   /* 逐塊解碼 → 呼叫 jpg_out 貼圖 */
    if (res != JDR_OK) {
        photo_error_code("decode err", res);
    }

    f_close(&fil);
}

/* ═══════════════════════════ 相簿 ═══════════════════════════ */

#define PHOTO_DIR  "/PHOTOS"            /* 圖片放這個資料夾 */
#define PHOTO_MAX  32                   /* 最多記幾張 */
static char photo_list[PHOTO_MAX][13];  /* 8.3 短檔名 + '\0'（_USE_LFN=0）*/
static int  photo_count;                /* 掃到幾張 */
static int  photo_idx;                  /* 目前顯示第幾張 */

/* 檔名是不是我們支援的圖檔（副檔名比對，不分大小寫）*/
static int is_image(const char *name)
{
    size_t n = strlen(name);
    return (n >= 4 && strcasecmp(name + n - 4, ".bmp")  == 0)
        || (n >= 4 && strcasecmp(name + n - 4, ".jpg")  == 0)
        || (n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0);
}

/* 依副檔名分派到 BMP / JPG 解碼器 */
static void photo_show(const char *path)
{
    if (is_image(path)) {
        size_t n = strlen(path);
        if (strcasecmp(path + n - 4, ".bmp") == 0) photo_show_bmp(path);
        else                                       photo_show_jpg(path);
    } else {
        photo_error("unknown type");
    }
}

/* 掃 PHOTO_DIR 資料夾，把圖檔名收進 photo_list[]，回傳張數。 */
static int photo_scan(void)
{
    DIR     dir;
    FILINFO fno;
    int     n = 0;

    if (f_opendir(&dir, PHOTO_DIR) != FR_OK) return 0;

    /* f_readdir 一次吐一筆；讀完後 fname[0]==0 表示結束（非錯誤）*/
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) continue;     /* 跳過資料夾 */
        if (!is_image(fno.fname))  continue;    /* 只收圖檔 */
        strcpy(photo_list[n], fno.fname);
        if (++n >= PHOTO_MAX) break;            /* 陣列滿就停 */
    }

    f_closedir(&dir);
    return n;
}

/* 左上角畫一個看得見的返回鍵（蓋在圖上）*/
#define BACK_W  54
#define BACK_H  18
static void photo_backhint(void)
{
    ILI9341_FillRect(0, 0, BACK_W, BACK_H, ILI9341_NAVY);
    ILI9341_DrawString(4, 1, "<Back", ILI9341_WHITE, ILI9341_NAVY);
}

/* 顯示目前這張 */
static void photo_show_current(void)
{
    if (photo_count == 0) { photo_error("no images"); return; }

    char path[32];
    strcpy(path, PHOTO_DIR "/");         /* "/PHOTOS/" */
    strcat(path, photo_list[photo_idx]); /* "/PHOTOS/TEST.JPG" */
    photo_show(path);

    photo_backhint();               /* 圖畫完後再蓋上返回鍵 */
}

/* ═══════════════════════════ 進入點 ═══════════════════════════ */
static void photo_enter(void)
{
    photo_count = photo_scan();
    photo_idx   = 0;
    photo_show_current();
}

/* 觸控：左上角返回鍵；其餘左半 = 上一張、右半 = 下一張 */
static void photo_touch(uint16_t x, uint16_t y)
{
    if (photo_count == 0) { Screen_Pop(); return; }

    if (x < BACK_W && y < BACK_H) {         /* 左上角返回鍵 → 回主畫面 */
        Screen_Pop();
    } else if (x < ILI9341_WIDTH / 2) {     /* 左半 → 上一張 */
        photo_idx = (photo_idx - 1 + photo_count) % photo_count;
        photo_show_current();
    } else {                                /* 右半 → 下一張 */
        photo_idx = (photo_idx + 1) % photo_count;
        photo_show_current();
    }
}

static void photo_render(void) {}

void ScreenPhoto_Register(void)
{
    Screen_Register(SCREEN_PHOTO, (screen_t){
        .on_enter  = photo_enter,
        .on_touch  = photo_touch,
        .on_render = photo_render,
    });
}
