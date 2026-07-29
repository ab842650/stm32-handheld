/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : ILI9341 顯示驅動驗測
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"

/* USER CODE BEGIN Includes */
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
#include "fatfs.h"
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;
SPI_HandleTypeDef  hspi1;
SPI_HandleTypeDef  hspi2;           /* SD 卡（獨立 SPI2）*/
DMA_HandleTypeDef  hdma_spi1_tx;

/* USER CODE BEGIN PV */
/* IPC（放在 USER CODE 區內，CubeMX 重新產生時不會被蓋掉）*/
SemaphoreHandle_t  spi_bus_mutex;   /* 保護 SPI1 匯流排 */
QueueHandle_t      ui_event_queue;  /* InputTask → UITask */
TaskHandle_t       inputTaskHandle;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_SPI2_Init(void);
static void MX_DMA_Init(void);

/* USER CODE BEGIN PFP */
void vUITask(void *pvParameters);
void vInputTask(void *pvParameters);
/* USER CODE END PFP */

/**
  * @brief  The application entry point.
  */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_DMA_Init();      /* DMA 必須在 SPI 之前初始化 */
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_SPI1_Init();
    MX_SPI2_Init();     /* SD 卡 SPI */
    MX_FATFS_Init();    /* 連結 FatFs 磁碟驅動（USER_Driver → user_diskio_spi）*/

    /* USER CODE BEGIN 2 */
    /* DWT Cycle Counter 初始化（168 MHz，量測精度 ~6 ns）*/
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

    xTaskCreate(vUITask,    "UITask",    2560, NULL, 3, NULL);   /* 2560 words(10KB)：module 狀態放堆疊，CHIP-8 記憶體 4KB+顯示緩衝 */
    xTaskCreate(vInputTask, "InputTask", 256, NULL, 4, &inputTaskHandle);

    ScreenHome_Register();
    ScreenCalc_Register();
    ScreenGame_Register();
    ScreenPhoto_Register();
    ScreenClock_Register();
    ScreenNotes_Register();
    ScreenGB_Register();

    vTaskStartScheduler();

    /* 不應該跑到這裡（scheduler 啟動後不返回）*/
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
  * @brief SPI1 — 給 ILI9341 用
  *
  * SPI1 掛在 APB2 上，APB2 = 168/2 = 84 MHz
  * Prescaler 8 → SPI 速度 = 84/8 = 10.5 MHz（保守值，測試穩定後可改 4 → 21 MHz）
  *
  * ILI9341 需要：Mode 0（CPOL=0, CPHA=0），MSB first
  */
static void MX_SPI1_Init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;   /* CPOL = 0 */
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;    /* CPHA = 0 */
    hspi1.Init.NSS               = SPI_NSS_SOFT;       /* CS 由軟體控制 */
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
    hspi2.Init.NSS               = SPI_NSS_SOFT;       /* CS 由軟體控制 */
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi2) != HAL_OK) { Error_Handler(); }
}

/**
  * @brief USART2 — 115200，PA2(TX)/PA3(RX)，給 myprintf debug 用
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
  * @brief GPIO Initialization
  *
  * SPI1 的 SCK/MISO/MOSI 腳位由 HAL_SPI_MspInit 在 HAL_SPI_Init 時自動設定。
  * 這裡只需設定 CS / DC / RST（純 GPIO output）。
  */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* 開啟所有用到的 GPIO 時脈 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();

    /* ILI9341 控制腳：CS(PB0)、DC(PB1)、RST(PB2)
     * 初始狀態：CS=HIGH（未選中）、DC=HIGH、RST=HIGH
     * ILI9341_Init() 會自己處理 RST 的時序 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* T_CS (PC4) — 觸控片選，預設拉高（未選中）*/
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_4;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* SD_CS (PC7) — SD 卡片選，預設拉高（未選中）*/
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_7;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* T_IRQ (PC5) — 觸控中斷，低電位表示觸碰，EXTI falling edge */
    GPIO_InitStruct.Pin  = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    /* PD12 (LD4 綠 LED) — 亮燈表示系統已開機 */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12, GPIO_PIN_SET);
    GPIO_InitStruct.Pin   = GPIO_PIN_12;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */

/* ── UI Task ────────────────────────────────────────────────────────────────
 * 負責螢幕渲染。用 vTaskDelay 等待而非 HAL_Delay，
 * 等待期間 CPU 可以去跑其他 task（例如之後的 InputTask、SensorTask）。
 * FillScreen 內部也會透過 DMA semaphore yield，不會 busy-wait。
 * ──────────────────────────────────────────────────────────────────────────*/
#define UI_TICK_MS  200   /* 閒置時的重繪間隔（5 Hz）；有觸控會立刻醒來 */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * SD 卡掛載自測（暫時性，驗證 SPI2 + FatFs + 接線）
 *
 * 掛載 → 讀 test.txt（不存在就自己建，順便測寫入）→ 印序列埠。
 * FatFs 回傳碼 fr：0 = FR_OK，其他見 ff.h 的 FRESULT（3=NOT_READY 卡沒反應，
 * 1=DISK_ERR 通訊錯，13=NO_FILESYSTEM 沒 FAT32）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void SD_SelfTest(void)
{
    FRESULT fr;
    FIL     fil;
    UINT    br;
    char    buf[64];

    myprintf("SD: mounting...\r\n");
    fr = f_mount(&USERFatFS, USERPath, 1);          /* 1 = 立即掛載並存取卡 */
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

    SD_SelfTest();   /* 暫時：驗證 SD，之後移到 Photo 畫面 */

    ui_event_t evt;
    for (;;) {
        /* 等觸控事件，但最多等 UI_TICK_MS 就醒來 —— 讓需要動態更新的畫面
         * （時鐘、之後的遊戲/動畫）即使沒人碰也會定時 on_render。
         * 有觸控 → 立刻返回處理；逾時 → evt 沒填，只做 render。 */
        if (xQueueReceive(ui_event_queue, &evt, pdMS_TO_TICKS(UI_TICK_MS)) == pdTRUE) {
            Screen_OnTouch(evt.x, evt.y);
        }
        Screen_OnRender();   /* 每輪都呼叫；靜態畫面的 on_render 留空即可 */
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * vInputTask — 一次按壓只送一個事件
 *
 * 為什麼需要「等放開 + 清通知」？
 *
 *   1. 手指按住不放期間，任何一個下降緣都會再送一次事件。
 *   2. 更麻煩的是 XPT2046 的 datasheet 行為：
 *      PENIRQ 在 ADC 轉換期間會被停用（拉高），轉換完才回到低電位
 *      → 我們「讀座標」這個動作本身就製造出一個新的下降緣
 *      → EXTI 是 FALLING 觸發 → 讀取自己觸發下一次讀取，自己餵自己。
 *
 * 所以處理完一次觸碰後，必須：等手指離開 → 等彈跳結束 →
 * 把期間累積的假通知清掉，才能回去等下一次真正的按下。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void vInputTask(void *pvParameters)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* 等按下 */
        vTaskDelay(pdMS_TO_TICKS(10));             /* 按下的彈跳 */

        uint16_t x, y;
        if (XPT2046_ReadPixel(&x, &y)) {
            ui_event_t evt = { .type = UI_EVT_TOUCH_DOWN, .x = x, .y = y };
            xQueueSend(ui_event_queue, &evt, 0);
            myprintf("touch: x=%3d y=%3d\r\n", x, y);
        }

        /* ① 等手指真的放開（只讀 GPIO，不佔 SPI）*/
        while (XPT2046_IsTouched()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* ② 離開瞬間的彈跳 */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* ③ 清掉這期間累積的假通知（PENIRQ 抖動造成的），
         *    timeout = 0 → 不等待，純粹把計數歸零 */
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

/* GPIO EXTI callback — T_IRQ (PC5) 觸控中斷 → 通知 InputTask
 * 放在 main.c（USER CODE 保護區）而非 it.c，避免 CubeMX regen 弄丟 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_5) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(inputTaskHandle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* USER CODE END 4 */

/**
  * @brief DMA2 初始化 — 只開時脈 + NVIC，實際 stream 設定在 HAL_SPI_MspInit 裡
  *
  * DMA 必須在使用它的周邊（SPI1）之前初始化，否則 HAL_DMA_Init 會失敗。
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
