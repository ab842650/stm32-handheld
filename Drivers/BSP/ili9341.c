#include "ili9341.h"
#include "myprintf.h"
#include "font.h"

static SPI_HandleTypeDef  *_hspi;
static SemaphoreHandle_t   _dma_done_sem = NULL;

extern SemaphoreHandle_t spi_bus_mutex;   /* created in main.c */

/* DWT busy-wait: usable before the scheduler starts, unlike HAL_Delay. */
static void _delay_ms(uint32_t ms)
{
    uint32_t cycles = (SystemCoreClock / 1000UL) * ms;
    uint32_t start  = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {}
}

static void _DMA_Sem_Init(void)
{
    if (_dma_done_sem == NULL) {
        _dma_done_sem = xSemaphoreCreateBinary();
    }
}

#define CS_LOW()   HAL_GPIO_WritePin(ILI9341_CS_PORT,  ILI9341_CS_PIN,  GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(ILI9341_CS_PORT,  ILI9341_CS_PIN,  GPIO_PIN_SET)
#define DC_CMD()   HAL_GPIO_WritePin(ILI9341_DC_PORT,  ILI9341_DC_PIN,  GPIO_PIN_RESET)
#define DC_DATA()  HAL_GPIO_WritePin(ILI9341_DC_PORT,  ILI9341_DC_PIN,  GPIO_PIN_SET)
#define RST_LOW()  HAL_GPIO_WritePin(ILI9341_RST_PORT, ILI9341_RST_PIN, GPIO_PIN_RESET)
#define RST_HIGH() HAL_GPIO_WritePin(ILI9341_RST_PORT, ILI9341_RST_PIN, GPIO_PIN_SET)

/* CS is toggled per transfer because NSS is in software mode. */
static void WriteCmd(uint8_t cmd)
{
    CS_LOW();
    DC_CMD();
    HAL_SPI_Transmit(_hspi, &cmd, 1, HAL_MAX_DELAY);
    CS_HIGH();
}

/* Command plus its data bytes, CS held low for the whole sequence. */
static void WriteCmd_Data(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    CS_LOW();

    DC_CMD();
    HAL_SPI_Transmit(_hspi, &cmd, 1, HAL_MAX_DELAY);

    if (len > 0) {
        DC_DATA();
        HAL_SPI_Transmit(_hspi, (uint8_t *)data, len, HAL_MAX_DELAY);
    }

    CS_HIGH();
}

/* Power/gamma register values below are the vendor defaults; changing them
 * throws the colour response off. */
void ILI9341_Init(SPI_HandleTypeDef *hspi)
{
    _hspi = hspi;

    /* Hardware reset */
    myprintf("ili: rst high\r\n");
    RST_HIGH();
    _delay_ms(50);
    myprintf("ili: rst low\r\n");
    RST_LOW();
    _delay_ms(50);
    myprintf("ili: rst high2\r\n");
    RST_HIGH();
    _delay_ms(200);
    myprintf("ili: hw reset done\r\n");

    WriteCmd(ILI9341_CMD_SWRESET);
    myprintf("ili: swreset sent\r\n");
    _delay_ms(150);
    myprintf("ili: swreset done\r\n");

    myprintf("ili: power regs\r\n");
    WriteCmd_Data(0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3);
    WriteCmd_Data(0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
    WriteCmd_Data(0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3);
    WriteCmd_Data(0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
    WriteCmd_Data(0xF7, (uint8_t[]){0x20}, 1);
    WriteCmd_Data(0xEA, (uint8_t[]){0x00, 0x00}, 2);

    /* Power Control 1 & 2 */
    WriteCmd_Data(0xC0, (uint8_t[]){0x23}, 1);
    WriteCmd_Data(0xC1, (uint8_t[]){0x10}, 1);

    /* VCOM Control */
    WriteCmd_Data(0xC5, (uint8_t[]){0x3E, 0x28}, 2);
    WriteCmd_Data(0xC7, (uint8_t[]){0x86}, 1);

    /* 16-bit RGB565 */
    WriteCmd_Data(ILI9341_CMD_COLMOD, (uint8_t[]){0x55}, 1);

    /* MADCTL 0xE8 = MY | MX | MV | BGR.
     *   MV  swaps rows/columns: portrait 240x320 -> landscape 320x240.
     *       Without it the controller still thinks the short edge is 240 and
     *       writing 320 pixels per row leaves grey smears.
     *   MY  corrects a vertical flip (0x68 lacked it: text drew upside down
     *       near the bottom, which solid rectangles did not reveal).
     *   BGR this module is wired BGR, not RGB.
     * Touch mapping in xpt2046.c must match whatever is set here. */
    WriteCmd_Data(ILI9341_CMD_MADCTL, (uint8_t[]){0xE8}, 1);

    /* Frame Rate Control: 79 Hz */
    WriteCmd_Data(0xB1, (uint8_t[]){0x00, 0x18}, 2);

    /* Display Function Control */
    WriteCmd_Data(0xB6, (uint8_t[]){0x08, 0x82, 0x27}, 3);

    /* 3Gamma disable */
    WriteCmd_Data(0xF2, (uint8_t[]){0x00}, 1);

    /* Gamma set curve 1 */
    WriteCmd_Data(0x26, (uint8_t[]){0x01}, 1);

    WriteCmd_Data(0xE0,
        (uint8_t[]){0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                    0x37,0x07,0x10,0x03,0x0E,0x09,0x00}, 15);
    WriteCmd_Data(0xE1,
        (uint8_t[]){0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                    0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F}, 15);

    /* Datasheet requires 120 ms after Sleep Out before Display On. */
    myprintf("ili: slpout\r\n");
    WriteCmd(ILI9341_CMD_SLPOUT);
    _delay_ms(120);
    myprintf("ili: slpout done\r\n");

    WriteCmd(ILI9341_CMD_DISPON);
    _delay_ms(10);
    myprintf("ili: dispon done\r\n");

    _DMA_Sem_Init();
}

/* CASET/PASET define a rectangle, then RAMWR streams pixels into it and the
 * controller wraps at the right edge by itself. Setting a window once and
 * pushing a block is what makes drawing fast. */
void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    /* Column (X) */
    buf[0] = x0 >> 8;  buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8;  buf[3] = x1 & 0xFF;
    WriteCmd_Data(ILI9341_CMD_CASET, buf, 4);

    /* Row / Page (Y) */
    buf[0] = y0 >> 8;  buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8;  buf[3] = y1 & 0xFF;
    WriteCmd_Data(ILI9341_CMD_PASET, buf, 4);

    WriteCmd(ILI9341_CMD_RAMWR);
}

/* Signalling DMA completion with a semaphore rather than a polled flag lets
 * the drawing task block, so the CPU is free during the transfer. */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1 && _dma_done_sem != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(_dma_done_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* Rows per DMA burst. Bigger = fewer transfers; 8 rows costs 5 KB of SRAM. */
#define CHUNK_ROWS  8
#define CHUNK_BYTES (ILI9341_WIDTH * 2 * CHUNK_ROWS)   /* 5120 bytes */

/* Repeats buf until total_pixels have been sent. Caller holds the bus mutex
 * and has already asserted CS/DC. All buffers passed here must be in normal
 * SRAM — DMA cannot reach CCM. */
static void _DMA_Flush(uint8_t *buf, uint16_t buf_pixels, uint32_t total_pixels)
{
    uint32_t sent = 0;
    while (sent < total_pixels) {
        uint16_t n = (total_pixels - sent >= buf_pixels) ? buf_pixels
                                                         : (uint16_t)(total_pixels - sent);
        HAL_SPI_Transmit_DMA(_hspi, buf, n * 2);
        xSemaphoreTake(_dma_done_sem, portMAX_DELAY);
        sent += n;
    }
}

void ILI9341_FillScreen(uint16_t color)
{
    static uint8_t chunk_buf[CHUNK_BYTES];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < ILI9341_WIDTH * CHUNK_ROWS; i++) {
        chunk_buf[i * 2]     = hi;
        chunk_buf[i * 2 + 1] = lo;
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(chunk_buf, ILI9341_WIDTH * CHUNK_ROWS,
               (uint32_t)ILI9341_WIDTH * ILI9341_HEIGHT);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

void ILI9341_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;
    if (x + w > ILI9341_WIDTH)  w = ILI9341_WIDTH  - x;
    if (y + h > ILI9341_HEIGHT) h = ILI9341_HEIGHT - y;

    /* One row is enough — _DMA_Flush repeats it h times. */
    static uint8_t row_buf[ILI9341_WIDTH * 2];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < w; i++) {
        row_buf[i * 2]     = hi;
        row_buf[i * 2 + 1] = lo;
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + w - 1, y + h - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(row_buf, w, (uint32_t)w * h);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

/* Costs a full CASET+PASET+RAMWR per pixel — fine occasionally, not in bulk. */
void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;

    ILI9341_SetWindow(x, y, x, y);

    uint8_t buf[2] = {color >> 8, color & 0xFF};
    CS_LOW();
    DC_DATA();
    HAL_SPI_Transmit(_hspi, buf, 2, HAL_MAX_DELAY);
    CS_HIGH();
}

/* Expand the glyph into a pixel buffer first, then send the whole 8x16 cell in
 * one transfer. font8x16 stores 16 bytes per char, one per row, bit7 leftmost. */
void ILI9341_DrawChar(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint8_t)c < FONT_FIRST || (uint8_t)c > FONT_LAST) {
        c = ' ';
    }
    if (x + FONT_WIDTH > ILI9341_WIDTH ||
        y + FONT_HEIGHT > ILI9341_HEIGHT) {
        return;
    }

    const uint8_t *g = font8x16[(uint8_t)c - FONT_FIRST];

    static uint8_t buf[FONT_WIDTH * FONT_HEIGHT * 2];

    for(int row = 0;row<FONT_HEIGHT;row++){
    	for(int col=0 ; col<FONT_WIDTH;col++){
    	    int bit = (g[row] >> (7 - col)) & 1;
    	    int idx = (row * FONT_WIDTH + col) * 2;
    		uint16_t color = (bit == 1) ? fg : bg;
    		buf[idx]   = color >> 8;
    		buf[idx+1] = color & 0xFF;
    	}
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + FONT_WIDTH - 1, y + FONT_HEIGHT - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(buf, FONT_WIDTH * FONT_HEIGHT, FONT_WIDTH * FONT_HEIGHT);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

void ILI9341_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
	uint16_t cx = x;
	uint16_t cy = y;

	for(int i = 0;str[i]!='\0';i++){
		if(str[i]=='\n'){
			cx=x;
			cy+=FONT_HEIGHT;
			continue;
		}
		if(cx + FONT_WIDTH > ILI9341_WIDTH){   /* wrap, but still draw the char */
			cx=x;
			cy+=FONT_HEIGHT;
		}
		ILI9341_DrawChar(cx,cy,str[i],fg,bg);
		cx+=FONT_WIDTH;
	}


}

/* Same shape as DrawChar, plus a variable row stride and integer scaling. */
#define BMP_MAX_PX  (48 * 48)
static uint8_t bmp_buf[BMP_MAX_PX * 2];

void ILI9341_DrawBitmapMono(uint16_t x, uint16_t y, const uint8_t *bmp,
                            uint16_t w, uint16_t h, uint8_t scale,
                            uint16_t fg, uint16_t bg)
{
    if (scale == 0) return;

    uint16_t ow = w * scale;
    uint16_t oh = h * scale;
    if ((uint32_t)ow * oh > BMP_MAX_PX) return;
    if (x + ow > ILI9341_WIDTH || y + oh > ILI9341_HEIGHT) return;

    uint16_t stride = (w + 7) / 8;            /* source bytes per row */

    for (uint16_t sy = 0; sy < h; sy++) {
        for (uint16_t sx = 0; sx < w; sx++) {
            int bit = (bmp[sy * stride + (sx >> 3)] >> (7 - (sx & 7))) & 1;
            uint16_t color = bit ? fg : bg;

            /* one source pixel -> scale x scale block */
            for (uint8_t dy = 0; dy < scale; dy++) {
                uint32_t row = (uint32_t)(sy * scale + dy) * ow;
                for (uint8_t dx = 0; dx < scale; dx++) {
                    uint32_t idx = (row + sx * scale + dx) * 2;
                    bmp_buf[idx]     = color >> 8;
                    bmp_buf[idx + 1] = color & 0xFF;
                }
            }
        }
    }

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + ow - 1, y + oh - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush(bmp_buf, ow * oh, (uint32_t)ow * oh);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}

/* Caller supplies pixels already in RGB565 big-endian; used row-by-row by the
 * image decoders. */
void ILI9341_BlitBytes(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *buf)
{
    if (x >= ILI9341_WIDTH || y >= ILI9341_HEIGHT) return;

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);
    ILI9341_SetWindow(x, y, x + w - 1, y + h - 1);
    CS_LOW();
    DC_DATA();
    _DMA_Flush((uint8_t *)buf, (uint32_t)w * h, (uint32_t)w * h);
    CS_HIGH();
    xSemaphoreGive(spi_bus_mutex);
}


