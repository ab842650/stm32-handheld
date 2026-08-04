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
 * Photo — full-screen BMP and JPEG viewer reading from SD.
 *
 * BMP is decoded here: the three things that catch people out are BGR channel
 * order, bottom-up row order, and rows padded to 4 bytes.
 * JPEG goes to TJpgDec, which only needs an input and an output callback.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static uint8_t bgrbuf[ILI9341_WIDTH * 3];   /* one row of raw BGR */
static uint8_t rowbuf[ILI9341_WIDTH * 2];   /* one row converted to RGB565 */

/* BMP headers are little-endian */
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

/* message plus a numeric code */
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

        uint16_t screenY = h - 1 - r;               /* BMP rows run bottom-up */
        ILI9341_BlitBytes(offx, offy + screenY, w, 1, rowbuf);
    }

    f_close(&fil);
}

/* ═══════════════════════════ JPG (TJpgDec) ═══════════════════════════
 *
 * TJpgDec knows nothing about the hardware; jpg_in feeds it from the file and
 * jpg_out puts decoded blocks on screen.
 *
 * JD_FORMAT=1 means the core hands back RGB565 already, but as native
 * little-endian uint16_t, while the ILI9341 wants high byte first — hence the
 * byte swap in jpg_out.
 */

/* TJpgDec workspace; 3092 bytes is the documented minimum */
static uint8_t jd_work[4096];

/* The "device" handed to the callbacks is just the open file. */
typedef struct {
    FIL *fp;
} jpg_io_t;

/* Offset that centres an image smaller than the screen. */
static uint16_t jpg_offx, jpg_offy;

/* ── input callback ──────────────────────────────────────────────────
 * Supply nbyte of JPEG data. A NULL buf means skip that many bytes instead of
 * reading them. Returns how many were actually read or skipped.
 */
static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    jpg_io_t *io = (jpg_io_t *)jd->device;

    if (buf) {
        UINT br;
        f_read(io->fp, buf, nbyte, &br);
        return br;
    } else {
        f_lseek(io->fp, f_tell(io->fp) + nbyte);
        return nbyte;
    }
}

/* ── output callback ─────────────────────────────────────────────────
 * Called once per decoded block. bitmap holds rw*rh little-endian RGB565
 * pixels; rect gives its inclusive position in the image. Swapping the bytes
 * in place avoids needing a second buffer. Return 1 to continue, 0 to abort.
 */
static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    uint16_t rw = rect->right  - rect->left + 1;
    uint16_t rh = rect->bottom - rect->top  + 1;

    /* little-endian -> big-endian, in place */
    uint16_t *src = (uint16_t *)bitmap;
    for (uint32_t i = 0; i < (uint32_t)rw * rh; i++) {
        src[i] = (src[i] >> 8) | (src[i] << 8);
    }

    ILI9341_BlitBytes(rect->left + jpg_offx, rect->top + jpg_offy,
                      rw, rh, (const uint8_t *)bitmap);
    return 1;
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

    res = jd_prepare(&jdec, jpg_in, jd_work, sizeof jd_work, &io);
    if (res != JDR_OK) {
        f_close(&fil); photo_error_code("prepare err", res); return;
    }

    /* scale 0..3 = 1/1, 1/2, 1/4, 1/8 — shrink until it fits */
    uint8_t scale = 0;
    while (scale < 3 && ((jdec.width  >> scale) > ILI9341_WIDTH ||
                         (jdec.height >> scale) > ILI9341_HEIGHT)) {
        scale++;
    }

    uint16_t outW = jdec.width  >> scale;
    uint16_t outH = jdec.height >> scale;
    jpg_offx = (outW < ILI9341_WIDTH ) ? (ILI9341_WIDTH  - outW) / 2 : 0;
    jpg_offy = (outH < ILI9341_HEIGHT) ? (ILI9341_HEIGHT - outH) / 2 : 0;

    ILI9341_FillScreen(ILI9341_BLACK);
    res = jd_decomp(&jdec, jpg_out, scale);
    if (res != JDR_OK) {
        photo_error_code("decode err", res);
    }

    f_close(&fil);
}

/* ── Album ── */

#define PHOTO_DIR  "/PHOTOS"
#define PHOTO_MAX  32
static char photo_list[PHOTO_MAX][13];  /* 8.3 names; _USE_LFN is 0 */
static int  photo_count;
static int  photo_idx;

static int is_image(const char *name)
{
    size_t n = strlen(name);
    return (n >= 4 && strcasecmp(name + n - 4, ".bmp")  == 0)
        || (n >= 4 && strcasecmp(name + n - 4, ".jpg")  == 0)
        || (n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0);
}

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

static int photo_scan(void)
{
    DIR     dir;
    FILINFO fno;
    int     n = 0;

    if (f_opendir(&dir, PHOTO_DIR) != FR_OK) return 0;

    /* fname[0]==0 marks the end of the directory, not an error */
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) continue;
        if (!is_image(fno.fname))  continue;
        strcpy(photo_list[n], fno.fname);
        if (++n >= PHOTO_MAX) break;
    }

    f_closedir(&dir);
    return n;
}

/* Back hint drawn on top of the image. */
#define BACK_W  54
#define BACK_H  18
static void photo_backhint(void)
{
    ILI9341_FillRect(0, 0, BACK_W, BACK_H, ILI9341_NAVY);
    ILI9341_DrawString(4, 1, "<Back", ILI9341_WHITE, ILI9341_NAVY);
}

static void photo_show_current(void)
{
    if (photo_count == 0) { photo_error("no images"); return; }

    char path[32];
    strcpy(path, PHOTO_DIR "/");         /* "/PHOTOS/" */
    strcat(path, photo_list[photo_idx]); /* "/PHOTOS/TEST.JPG" */
    photo_show(path);

    photo_backhint();               /* after the image, so it stays visible */
}

static void photo_enter(void)
{
    photo_count = photo_scan();
    photo_idx   = 0;
    photo_show_current();
}

/* Back in the corner; otherwise left half = previous, right half = next. */
static void photo_touch(uint16_t x, uint16_t y)
{
    if (photo_count == 0) { Screen_Pop(); return; }

    if (x < BACK_W && y < BACK_H) {
        Screen_Pop();
    } else if (x < ILI9341_WIDTH / 2) {
        photo_idx = (photo_idx - 1 + photo_count) % photo_count;
        photo_show_current();
    } else {
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
