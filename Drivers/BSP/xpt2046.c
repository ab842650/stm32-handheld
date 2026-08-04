#include "xpt2046.h"
#include "myprintf.h"

static SPI_HandleTypeDef *_hspi;

extern SemaphoreHandle_t spi_bus_mutex;   /* created in main.c */

#define T_CS_LOW()   HAL_GPIO_WritePin(XPT2046_CS_PORT, XPT2046_CS_PIN, GPIO_PIN_RESET)
#define T_CS_HIGH()  HAL_GPIO_WritePin(XPT2046_CS_PORT, XPT2046_CS_PIN, GPIO_PIN_SET)

/* Control byte: start | A2:A0 channel | mode | SER/DFR | PD.
 *   0xD0 = X, 12-bit, differential
 *   0x90 = Y, 12-bit, differential */
#define CMD_X  0xD0
#define CMD_Y  0x90

#define NUM_SAMPLES 5

static int cmp_u16(const void *a, const void *b) {
    return (int)(*(uint16_t*)a) - (int)(*(uint16_t*)b);
}

/* Median of NUM_SAMPLES to reject spikes. */
static uint16_t ReadChannel(uint8_t cmd)
{
    uint16_t samples[NUM_SAMPLES];
    uint8_t tx[3], rx[3];

    for (int i = 0; i < NUM_SAMPLES; i++) {
        tx[0] = cmd; tx[1] = 0; tx[2] = 0;
        HAL_SPI_TransmitReceive(_hspi, tx, rx, 3, HAL_MAX_DELAY);
        /* result is 12 bits: rx[1] upper 7 + rx[2] upper 5 */
        samples[i] = ((rx[1] << 8) | rx[2]) >> 3;
    }

    for (int i = 0; i < NUM_SAMPLES - 1; i++)
        for (int j = i + 1; j < NUM_SAMPLES; j++)
            if (samples[i] > samples[j]) {
                uint16_t tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }

    return samples[NUM_SAMPLES / 2];
}

void XPT2046_Init(SPI_HandleTypeDef *hspi)
{
    _hspi = hspi;
    T_CS_HIGH();
}

bool XPT2046_IsTouched(void)
{
    return HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN) == GPIO_PIN_RESET;
}

/* Raw ADC values, 0..4095. Returns false if not currently touched. */
bool XPT2046_ReadRaw(uint16_t *x_raw, uint16_t *y_raw)
{
    if (HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN) == GPIO_PIN_SET)
        return false;

    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    /* XPT2046 tops out at 2 MHz, so drop SPI1 to 84/64 = 1.3 MHz for the
     * conversion and put it back afterwards — the display shares this bus and
     * runs at 42 MHz. Reading at display speed returns all zeros. */
    __HAL_SPI_DISABLE(_hspi);
    MODIFY_REG(_hspi->Instance->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_64);
    __HAL_SPI_ENABLE(_hspi);

    T_CS_LOW();
    *x_raw = ReadChannel(CMD_X);
    *y_raw = ReadChannel(CMD_Y);
    T_CS_HIGH();

    __HAL_SPI_DISABLE(_hspi);
    MODIFY_REG(_hspi->Instance->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_2);
    __HAL_SPI_ENABLE(_hspi);

    xSemaphoreGive(spi_bus_mutex);

    /* Re-check: the finger may have lifted mid-conversion. */
    return HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN) == GPIO_PIN_RESET;
}

/* Raw ADC -> screen pixels. This mapping is tied to the display's MADCTL
 * (0xE8, landscape): the panel's x axis feeds screen y and vice versa, with no
 * inversion on either. Changing MADCTL means re-deriving this. */
bool XPT2046_ReadPixel(uint16_t *x_px, uint16_t *y_px)
{
    uint16_t xr, yr;
    if (!XPT2046_ReadRaw(&xr, &yr)) return false;

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
