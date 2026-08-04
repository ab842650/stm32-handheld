/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Application entry point and RTOS tasks
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"

/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include "myprintf.h"
#include "ili9341.h"
#include "xpt2046.h"
#include "ui_event.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "screen.h"
#include "screen_home.h"
#include "screen_calc.h"
#include "screen_game.h"
#include "screen_photo.h"
#include "screen_clock.h"
#include "screen_notes.h"
#include "screen_gb.h"
#include "screen_msg.h"
#include "screen_kb.h"
#include "fatfs.h"
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;          /* ESP32-S3 link, PB10/PB11 */
SPI_HandleTypeDef  hspi1;
SPI_HandleTypeDef  hspi2;           /* SD card, on its own bus */
DMA_HandleTypeDef  hdma_spi1_tx;

/* USER CODE BEGIN PV */
/* Inside a USER CODE block so a CubeMX regen would not wipe it. */
SemaphoreHandle_t  spi_bus_mutex;   /* display and touch share SPI1 */
QueueHandle_t      ui_event_queue;  /* InputTask → UITask */
TaskHandle_t       inputTaskHandle;
TaskHandle_t       uiTaskHandle;
TaskHandle_t       netTaskHandle;

/* Single-producer/single-consumer ring: the ISR owns head, the task owns
 * tail, so neither needs a lock. Both must stay volatile. */
#define ESP_RX_SZ 1024
static volatile uint8_t  esp_rx_buf[ESP_RX_SZ];
static volatile uint16_t esp_rx_head;   /* written by the ISR */
static volatile uint16_t esp_rx_tail;   /* written by the task */
static uint8_t           esp_rx_byte;   /* HAL lands one byte here */

/* Seconds-of-day offset from NTP; screens add it to the tick count. */
volatile uint32_t g_clock_offset = 0;
volatile uint8_t  g_time_valid   = 0;
char              g_weather[48]  = "";

/* Last MSG_MAX Discord messages, read by screen_msg.c. */
#define MSG_MAX 8
#define MSG_LEN 40      /* one screen line is exactly 320/8 chars */
char              g_msgs[MSG_MAX][MSG_LEN];
volatile uint32_t g_msg_n;        /* total ever received; message k is at k % MSG_MAX */
volatile uint8_t  g_msg_unread;
volatile uint8_t  g_send_result;  /* 0 idle, 1 sending, 2 sent, 3 failed */
static char       g_send_text[64];/* picked up by NetTask */
static volatile uint8_t g_send_req;

/* SD concurrency stress test: two tasks hammer the card and byte-compare
 * what they read back, proving the FatFs volume lock works. Passed; left at 0.
 * Set to 1 to re-run — results are printed from vNetTask. */
#define SD_STRESS_TEST 0
#if SD_STRESS_TEST
extern volatile unsigned long ff_lock_n, ff_contend_n;   /* counters in syscall.c */
#define SDST_BUF 128
static const int sdst_id[2] = { 0, 1 };
static FIL       sdst_fil[2];                    /* FIL is large; keep it off the stack */
static uint8_t   sdst_wbuf[2][SDST_BUF];
static uint8_t   sdst_rbuf[2][SDST_BUF];
static volatile uint32_t sdst_iter[2], sdst_fail[2], sdst_lastbad[2];
static volatile uint32_t sdst_mism[2], sdst_tmo[2], sdst_lastfr[2];
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_SPI2_Init(void);
static void MX_DMA_Init(void);

/* USER CODE BEGIN PFP */
void vUITask(void *pvParameters);
void vInputTask(void *pvParameters);
void vNetTask(void *pvParameters);
static int esp_rx_pop(uint8_t *out);
static void esp_parse_line(const char *s);
/* net_download() and its esp_read_line/esp_read_bytes helpers were removed:
 * the protocol worked but was never wired to a UI action. The ESP32 still
 * handles "DL <url>"; restore this half with
 *   git show c04cb1b -- Core/Src/main.c */
#if SD_STRESS_TEST
void vSdStressTask(void *pvParameters);
#endif
/* USER CODE END PFP */

/**
  * @brief  The application entry point.
  */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_DMA_Init();      /* must precede SPI init */
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();      /* ESP32-S3 UART */
    MX_SPI1_Init();
    MX_SPI2_Init();     /* SD card */
    MX_FATFS_Init();    /* links USER_Driver -> user_diskio_spi */

    /* USER CODE BEGIN 2 */
    /* DWT cycle counter: ~6 ns resolution at 168 MHz */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    myprintf("System start\r\n");

    spi_bus_mutex  = xSemaphoreCreateMutex();
    ui_event_queue = xQueueCreate(10, sizeof(ui_event_t));

    ILI9341_Init(&hspi1);
    myprintf("ILI9341 init done\r\n");

    XPT2046_Init(&hspi1);
    myprintf("XPT2046 init done\r\n");

    xTaskCreate(vUITask,    "UITask",    2560, NULL, 3, &uiTaskHandle);   /* 10 KB: modules keep state on this stack, CHIP-8 needs 4 KB plus a framebuffer */
    xTaskCreate(vInputTask, "InputTask", 256, NULL, 4, &inputTaskHandle);
    xTaskCreate(vNetTask,   "NetTask",   1024, NULL, 2, &netTaskHandle);  /* time/weather/messages; measured peak ~260 words, rest is headroom */
#if SD_STRESS_TEST
    /* Equal priority so the scheduler preempts them mid-f_*, which is what
     * actually forces concurrent FatFs entry. */
    xTaskCreate(vSdStressTask, "SDStress1", 1024, (void *)&sdst_id[0], 1, NULL);
    xTaskCreate(vSdStressTask, "SDStress2", 1024, (void *)&sdst_id[1], 1, NULL);
#endif

    ScreenHome_Register();
    ScreenCalc_Register();
    ScreenGame_Register();
    ScreenPhoto_Register();
    ScreenNotes_Register();
    ScreenGB_Register();
    ScreenMsg_Register();
    ScreenKb_Register();

    vTaskStartScheduler();

    /* not reached */
    while (1) {}
    /* USER CODE END 2 */
}

/**
  * @brief System Clock Configuration — 168 MHz via HSE + PLL
  */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 8;
    RCC_OscInitStruct.PLL.PLLN       = 336;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) { Error_Handler(); }
}

/**
  * @brief SPI1 — ILI9341 display and XPT2046 touch
  *
  * APB2 runs at 84 MHz. Prescaler 8 gives 10.5 MHz; the display tolerates
  * prescaler 2 (42 MHz), which is what the driver switches to at run time.
  * ILI9341 wants mode 0 (CPOL=0, CPHA=0), MSB first.
  */
static void MX_SPI1_Init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;   /* CPOL = 0 */
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;    /* CPHA = 0 */
    hspi1.Init.NSS               = SPI_NSS_SOFT;       /* CS driven by software */
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2; /* 84/2 = 42 MHz */
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) { Error_Handler(); }
}

static void MX_SPI2_Init(void)
{
    hspi2.Instance               = SPI2;
    hspi2.Init.Mode              = SPI_MODE_MASTER;
    hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;   /* CPOL = 0 */
    hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;    /* CPHA = 0 */
    hspi2.Init.NSS               = SPI_NSS_SOFT;       /* CS driven by software */
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi2) != HAL_OK) { Error_Handler(); }
}

/**
  * @brief USART2 — 115200 on PA2/PA3, debug console for myprintf
  */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) { Error_Handler(); }
}

/**
  * @brief USART3 — 115200 on PB10/PB11, link to the ESP32-S3
  */
static void MX_USART3_UART_Init(void)
{
    huart3.Instance          = USART3;
    huart3.Init.BaudRate     = 115200;
    huart3.Init.WordLength   = UART_WORDLENGTH_8B;
    huart3.Init.StopBits     = UART_STOPBITS_1;
    huart3.Init.Parity       = UART_PARITY_NONE;
    huart3.Init.Mode         = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK) { Error_Handler(); }

    /* Arm byte-at-a-time RX; each byte lands in HAL_UART_RxCpltCallback. */
    HAL_UART_Receive_IT(&huart3, &esp_rx_byte, 1);
}

/**
  * @brief GPIO Initialization
  *
  * HAL_SPI_MspInit already configures SCK/MISO/MOSI; only the plain GPIO
  * lines (CS / DC / RST) are set up here.
  */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* clocks for every port used below */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();

    /* ILI9341 CS(PB0) / DC(PB1) / RST(PB2), all idle high.
     * ILI9341_Init() drives the reset sequence itself. */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* T_CS (PC4) — touch chip select, idle high */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* SD_CS (PC7) — card chip select, idle high */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_7;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* T_IRQ (PC5) — low while touched; EXTI on the falling edge */
    GPIO_InitStruct.Pin  = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    /* PD12 (LD4) — lit once the system is up */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_12;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */

/* ── UI Task ────────────────────────────────────────────────────────────────
 * Owns all rendering. Every wait here blocks rather than spins: vTaskDelay
 * instead of HAL_Delay, and the DMA semaphore inside the display driver.
 * ──────────────────────────────────────────────────────────────────────────*/
#define UI_TICK_MS  200   /* idle redraw period; touch wakes the task sooner */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * SD bring-up check: mount, then read test.txt (creating it if absent, which
 * also exercises writing). Common FRESULTs: 1 DISK_ERR, 3 NOT_READY (card not
 * responding), 13 NO_FILESYSTEM (not FAT32).
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void SD_SelfTest(void)
{
    FRESULT fr;
    FIL     fil;
    UINT    br;
    char    buf[64];

    myprintf("SD: mounting...\r\n");
    fr = f_mount(&USERFatFS, USERPath, 1);          /* 1 = mount now rather than lazily */
    if (fr != FR_OK) {
        myprintf("SD: mount FAILED (fr=%d)\r\n", fr);
        return;
    }
    myprintf("SD: mounted OK\r\n");

    fr = f_open(&fil, "test.txt", FA_READ);
    if (fr != FR_OK) {
        myprintf("SD: test.txt not found (fr=%d), creating...\r\n", fr);
        fr = f_open(&fil, "test.txt", FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) { myprintf("SD: create FAILED (fr=%d)\r\n", fr); return; }
        f_write(&fil, "Hello SD!", 9, &br);
        f_close(&fil);
        myprintf("SD: wrote test.txt (%u bytes)\r\n", br);
        fr = f_open(&fil, "test.txt", FA_READ);
        if (fr != FR_OK) { myprintf("SD: reopen FAILED (fr=%d)\r\n", fr); return; }
    }

    fr = f_read(&fil, buf, sizeof(buf) - 1, &br);
    f_close(&fil);
    if (fr != FR_OK) { myprintf("SD: read FAILED (fr=%d)\r\n", fr); return; }
    buf[br] = '\0';
    myprintf("SD: read OK (%u bytes) -> \"%s\"\r\n", br, buf);
}

void vUITask(void *pvParameters)
{
	Screen_Push(SCREEN_HOME);

    SD_SelfTest();

    ui_event_t evt;
    for (;;) {
        /* Wake on a touch, or every UI_TICK_MS regardless, so screens that
         * animate still get rendered when nobody is touching anything. */
        if (xQueueReceive(ui_event_queue, &evt, pdMS_TO_TICKS(UI_TICK_MS)) == pdTRUE) {
            Screen_OnTouch(evt.x, evt.y);
        }
        Screen_OnRender();   /* static screens simply leave on_render empty */
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * vInputTask — exactly one event per press.
 *
 * The wait-for-release-then-drain dance is not just debouncing. Per the
 * XPT2046 datasheet PENIRQ goes inactive during a conversion and returns low
 * afterwards, so reading the coordinates manufactures a fresh falling edge on
 * an EXTI configured for falling edges — the read retriggers itself and the
 * task feeds itself forever. Draining the notifications is what breaks it.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void vInputTask(void *pvParameters)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(10));             /* press bounce */

        uint16_t x, y;
        if (XPT2046_ReadPixel(&x, &y)) {
            ui_event_t evt = { .type = UI_EVT_TOUCH_DOWN, .x = x, .y = y };
            xQueueSend(ui_event_queue, &evt, 0);
            myprintf("touch: x=%3d y=%3d\r\n", x, y);
        }

        /* Wait for release — GPIO only, so it never takes the SPI bus. */
        while (XPT2046_IsTouched()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* release bounce */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Drop the spurious notifications PENIRQ generated meanwhile;
         * timeout 0 just zeroes the count without waiting. */
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

#if SD_STRESS_TEST
/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * vSdStressTask — proves the FatFs volume lock actually works.
 *
 * Two equal-priority tasks each write a known pattern, read it back and
 * byte-compare. Equal priority plus time slicing means they get preempted
 * inside f_*, which is what produces genuine concurrent entry.
 * Unlocked: the shared FATFS window gets clobbered and mismatches appear.
 * Locked:  mismatches stay 0 while ff_contend_n keeps climbing — that rising
 * counter is what distinguishes "the lock works" from "we never raced".
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void vSdStressTask(void *pvParameters)
{
    int  id = *(const int *)pvParameters;        /* 0 or 1 */
    char path[16];
    snprintf(path, sizeof(path), "/STRESS%d.TXT", id + 1);

    /* f_mount happens later, in SD_SelfTest on vUITask. Wait for it before
     * counting, or the first few dozen not-yet-mounted failures pollute the
     * statistics. */
    for (;;) {
        FRESULT fr = f_open(&sdst_fil[id], path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) { f_close(&sdst_fil[id]); break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    for (;;) {
        uint32_t n   = sdst_iter[id];
        int      bad = 0;
        FRESULT  fr  = FR_OK;
        UINT     bw = 0, br = 0;

        /* pattern derived from (id, iteration) so it is self-checking */
        for (int i = 0; i < SDST_BUF; i++)
            sdst_wbuf[id][i] = (uint8_t)(id * 71 + n * 13 + i);

        /* write */
        fr = f_open(&sdst_fil[id], path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            bad = 1;
        } else {
            fr = f_write(&sdst_fil[id], sdst_wbuf[id], SDST_BUF, &bw);
            if (fr != FR_OK || bw != SDST_BUF) bad = 2;
            f_close(&sdst_fil[id]);
        }
        /* read back */
        if (!bad) {
            memset(sdst_rbuf[id], 0, SDST_BUF);
            fr = f_open(&sdst_fil[id], path, FA_READ);
            if (fr != FR_OK) {
                bad = 3;
            } else {
                fr = f_read(&sdst_fil[id], sdst_rbuf[id], SDST_BUF, &br);
                if (fr != FR_OK || br != SDST_BUF) bad = 4;
                f_close(&sdst_fil[id]);
            }
        }
        /* the only check that means corruption */
        if (!bad && memcmp(sdst_wbuf[id], sdst_rbuf[id], SDST_BUF) != 0) {
            bad = 5;
            sdst_mism[id]++;                    /* must stay 0 */
        }

        if (bad) {
            sdst_fail[id]++;
            sdst_lastbad[id] = bad;
            sdst_lastfr[id]  = (uint32_t)fr;
            if (fr == FR_TIMEOUT) sdst_tmo[id]++;   /* queued too long, not corruption */
        }
        sdst_iter[id]++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * vNetTask — owns the ESP32 link.
 *
 * Network round trips take hundreds of ms, so they get their own low-priority
 * task rather than stalling rendering or touch. Polls TIME?, WX? and MSG?,
 * sends queued messages, and reassembles replies from the ring buffer into
 * lines for esp_parse_line.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void vNetTask(void *pvParameters)
{
    char     line[64];
    uint8_t  len     = 0;
    uint32_t last_q   = 0;
    uint32_t last_wx  = 0;
    uint32_t last_msg = 0;

    for (;;) {

        /* retry every 3 s until synced, then re-sync every 60 s */
        uint32_t interval = g_time_valid ? 60000 : 3000;
        if (last_q == 0 || HAL_GetTick() - last_q >= interval) {
            last_q = HAL_GetTick();
            const char msg[] = "TIME?\r\n";
            HAL_UART_Transmit(&huart3, (uint8_t *)msg, sizeof(msg) - 1, 100);
        }

        /* weather every 30 s */
        if (last_wx == 0 || HAL_GetTick() - last_wx >= 30000) {
            last_wx = HAL_GetTick();
            const char m2[] = "WX?\r\n";
            HAL_UART_Transmit(&huart3, (uint8_t *)m2, sizeof(m2) - 1, 100);
        }

        /* queued by Msg_Send() from the UI */
        if (g_send_req) {
            g_send_req = 0;
            char cmd[80];
            int n = snprintf(cmd, sizeof(cmd), "SEND %s\r\n", g_send_text);
            HAL_UART_Transmit(&huart3, (uint8_t *)cmd, n, 200);
        }

        /* The ESP32 polls Discord itself; this is just a cheap collection. */
        if (last_msg == 0 || HAL_GetTick() - last_msg >= 1000) {
            last_msg = HAL_GetTick();
            const char m3[] = "MSG?\r\n";
            HAL_UART_Transmit(&huart3, (uint8_t *)m3, sizeof(m3) - 1, 100);
        }

        /* drain the ring buffer, assemble lines, parse on newline */
        uint8_t rx;
        while (esp_rx_pop(&rx)) {
            if (rx == '\n' || rx == '\r') {
                if (len > 0) { line[len] = '\0'; esp_parse_line(line); len = 0; }
            } else if (len < sizeof(line) - 1) {
                line[len++] = rx;
            }
        }

#if SD_STRESS_TEST
        /* fail must stay 0; contend must be > 0 or nothing actually raced */
        static uint32_t last_st = 0;
        if (HAL_GetTick() - last_st >= 2000) {
            last_st = HAL_GetTick();
            myprintf("SD stress: iter=%lu/%lu MISMATCH=%lu/%lu fail=%lu/%lu tmo=%lu/%lu "
                     "lastfr=%lu/%lu  lock=%lu contend=%lu\r\n",
                     (unsigned long)sdst_iter[0], (unsigned long)sdst_iter[1],
                     (unsigned long)sdst_mism[0], (unsigned long)sdst_mism[1],
                     (unsigned long)sdst_fail[0], (unsigned long)sdst_fail[1],
                     (unsigned long)sdst_tmo[0],  (unsigned long)sdst_tmo[1],
                     (unsigned long)sdst_lastfr[0], (unsigned long)sdst_lastfr[1],
                     ff_lock_n, ff_contend_n);
        }
#endif

        /* Lowest free stack ever seen, in words. Only reflects code paths
         * actually executed, so exercise GB/CHIP-8 before trusting it. */
        static uint32_t last_hw = 0;
        if (HAL_GetTick() - last_hw >= 15000) {
            last_hw = HAL_GetTick();
            myprintf("stack free (words) — UI:%lu Input:%lu Net:%lu\r\n",
                     (unsigned long)uxTaskGetStackHighWaterMark(uiTaskHandle),
                     (unsigned long)uxTaskGetStackHighWaterMark(inputTaskHandle),
                     (unsigned long)uxTaskGetStackHighWaterMark(netTaskHandle));
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* T_IRQ (PC5) touch interrupt -> wake InputTask. Lives here rather than in
 * stm32f4xx_it.c so a CubeMX regen cannot lose it. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_5) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(inputTaskHandle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* Called by FreeRTOS when any task overflows its stack. Naming the task and
 * halting beats the previous failure mode: silent memory corruption. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    myprintf("\r\n!!! STACK OVERFLOW: %s !!!\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* HAL calls this per received byte. Push and re-arm — the receive is not
 * automatically restarted. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        uint16_t next = (esp_rx_head + 1) % ESP_RX_SZ;
        if (next != esp_rx_tail) {          /* full: drop the byte */
            esp_rx_buf[esp_rx_head] = esp_rx_byte;
            esp_rx_head = next;
        }
        HAL_UART_Receive_IT(&huart3, &esp_rx_byte, 1);
    }
}

/* One byte out of the ring; 1 if there was data, 0 if empty. */
static int esp_rx_pop(uint8_t *out)
{
    if (esp_rx_head == esp_rx_tail) return 0;
    *out = esp_rx_buf[esp_rx_tail];
    esp_rx_tail = (esp_rx_tail + 1) % ESP_RX_SZ;
    return 1;
}

/* Parse one complete reply line from the ESP32. */
static void esp_parse_line(const char *s)
{
    if (strcmp(s, "MSGNONE") == 0) return;      /* arrives every second; would flood the log */
    myprintf("ESP: %s\r\n", s);

    unsigned h, m, sec;
    if (sscanf(s, "TIME %u:%u:%u", &h, &m, &sec) == 3) {
        uint32_t uptime = xTaskGetTickCount() / configTICK_RATE_HZ;
        uint32_t target = h * 3600 + m * 60 + sec;                    /* wall-clock seconds of day */
        /* pick offset so (uptime + offset) mod 86400 == target */
        g_clock_offset = (target + 86400u - (uptime % 86400u)) % 86400u;
        g_time_valid = 1;
        return;
    }

    /* "WX <text>" */
    if (strncmp(s, "WX ", 3) == 0) {
        strncpy(g_weather, s + 3, sizeof(g_weather) - 1);
        g_weather[sizeof(g_weather) - 1] = '\0';
        return;
    }

    /* "MSG <user>: <text>" */
    if (strncmp(s, "MSG ", 4) == 0) {
        char *slot = g_msgs[g_msg_n % MSG_MAX];
        strncpy(slot, s + 4, MSG_LEN - 1);
        slot[MSG_LEN - 1] = '\0';
        g_msg_n++;
        if (g_msg_unread < 99) g_msg_unread++;
        return;
    }

    /* send result */
    if (strcmp(s, "SENDOK")  == 0) { g_send_result = 2; return; }
    if (strcmp(s, "SENDERR") == 0) { g_send_result = 3; return; }
}

/* Non-blocking: queues the text for NetTask so the UI never touches UART. */
void Msg_Send(const char *text)
{
    if (g_send_req) return;                  /* previous send still pending */
    strncpy(g_send_text, text, sizeof(g_send_text) - 1);
    g_send_text[sizeof(g_send_text) - 1] = '\0';
    g_send_result = 1;
    g_send_req    = 1;
}

/* USER CODE END 4 */

/**
  * @brief DMA2 — clock and NVIC only; the stream itself is set up in
  *        HAL_SPI_MspInit. Must run before SPI1 init or HAL_DMA_Init fails.
  */
static void MX_DMA_Init(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* DMA2_Stream3 → SPI1_TX */
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) { HAL_IncTick(); }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
