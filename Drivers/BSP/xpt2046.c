#include "xpt2046.h"
#include "myprintf.h"

static SPI_HandleTypeDef *_hspi;

/* SPI bus mutex，由 main.c 建立，這裡只 extern 使用 */
extern SemaphoreHandle_t spi_bus_mutex;

#define T_CS_LOW()   HAL_GPIO_WritePin(XPT2046_CS_PORT, XPT2046_CS_PIN, GPIO_PIN_RESET)
#define T_CS_HIGH()  HAL_GPIO_WritePin(XPT2046_CS_PORT, XPT2046_CS_PIN, GPIO_PIN_SET)

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * XPT2046 命令格式（1 byte）
 *
 *  Bit 7   : Start（固定 1）
 *  Bit 6-4 : 通道選擇 A2:A0
 *              101 = X 位置
 *              001 = Y 位置
 *  Bit 3   : Mode（0 = 12-bit）
 *  Bit 2   : SER/DFR（0 = differential，噪聲較低）
 *  Bit 1-0 : PD（00 = 量完省電，11 = 常開）
 *
 *  0xD0 = 1_101_0_0_00 → X, 12-bit, differential
 *  0x90 = 1_001_0_0_00 → Y, 12-bit, differential
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define CMD_X  0xD0
#define CMD_Y  0x90

/* 讀單一通道，多次取樣取中位數，降低雜訊 */
#define NUM_SAMPLES 5

static int cmp_u16(const void *a, const void *b) {
    return (int)(*(uint16_t*)a) - (int)(*(uint16_t*)b);
}

static uint16_t ReadChannel(uint8_t cmd)
{
    uint16_t samples[NUM_SAMPLES];
    uint8_t tx[3], rx[3];

    for (int i = 0; i < NUM_SAMPLES; i++) {
        tx[0] = cmd; tx[1] = 0; tx[2] = 0;
        HAL_SPI_TransmitReceive(_hspi, tx, rx, 3, HAL_MAX_DELAY);
        /* 回傳值：rx[1] 高 7 bit + rx[2] 高 5 bit = 12 bit */
        samples[i] = ((rx[1] << 8) | rx[2]) >> 3;
    }

    /* 排序取中位數 */
    for (int i = 0; i < NUM_SAMPLES - 1; i++)
        for (int j = i + 1; j < NUM_SAMPLES; j++)
            if (samples[i] > samples[j]) {
                uint16_t tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }

    return samples[NUM_SAMPLES / 2];
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

void XPT2046_Init(SPI_HandleTypeDef *hspi)
{
    _hspi = hspi;
    T_CS_HIGH();
}

/* 只讀 GPIO，不碰 SPI → 可以高頻輪詢，也不會跟 ILI9341 搶匯流排 */
bool XPT2046_IsTouched(void)
{
    return HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN) == GPIO_PIN_RESET;
}

/* 讀取原始 ADC 值（0~4095）。回傳 true 表示有觸碰。
 * 用 SPI bus mutex 保護，避免與 ILI9341 衝突。                            */
bool XPT2046_ReadRaw(uint16_t *x_raw, uint16_t *y_raw)
{
    if (HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN) == GPIO_PIN_SET)
        return false;   /* T_IRQ 高電位 = 沒有觸碰 */

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    /* XPT2046 最高 2 MHz，降速到 84/64 = 1.3 MHz */
    __HAL_SPI_DISABLE(_hspi);
    MODIFY_REG(_hspi->Instance->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_64);
    __HAL_SPI_ENABLE(_hspi);

    T_CS_LOW();
    *x_raw = ReadChannel(CMD_X);
    *y_raw = ReadChannel(CMD_Y);
    T_CS_HIGH();

    /* 恢復 ILI9341 的高速 84/2 = 42 MHz */
    __HAL_SPI_DISABLE(_hspi);
    MODIFY_REG(_hspi->Instance->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_2);
    __HAL_SPI_ENABLE(_hspi);

    xSemaphoreGive(spi_bus_mutex);

    /* 再次確認還在觸碰（避免讀到手離開瞬間的雜訊）*/
    return HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN) == GPIO_PIN_RESET;
}

/* 將原始 ADC 值轉換成螢幕像素座標。
 * 螢幕方向 MADCTL=0xE8（landscape，MY+MX+MV+BGR），(0,0) 在左上。
 * 觸控 xr→螢幕 y，yr→螢幕 x；X 軸不反轉、Y 軸不反轉。                    */
bool XPT2046_ReadPixel(uint16_t *x_px, uint16_t *y_px)
{
    uint16_t xr, yr;
    if (!XPT2046_ReadRaw(&xr, &yr)) return false;

    /* MADCTL=0xE8：觸控 xr→螢幕 y，yr→螢幕 x */
    int32_t x = ((int32_t)(yr - XPT2046_Y_MIN) * XPT2046_SCREEN_W)
                / (XPT2046_Y_MAX - XPT2046_Y_MIN);
    int32_t y = ((int32_t)(xr - XPT2046_X_MIN) * XPT2046_SCREEN_H)
                / (XPT2046_X_MAX - XPT2046_X_MIN);

    if (x < 0) x = 0;
    if (x >= XPT2046_SCREEN_W) x = XPT2046_SCREEN_W - 1;
    if (y < 0) y = 0;
    if (y >= XPT2046_SCREEN_H) y = XPT2046_SCREEN_H - 1;

    *x_px = (uint16_t)x;
    *y_px = (uint16_t)y;
    return true;
}
