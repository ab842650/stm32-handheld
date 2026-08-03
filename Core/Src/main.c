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
#include "fatfs.h"
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;          /* ESP32-S3 WiFi 連線（PB10/PB11）*/
SPI_HandleTypeDef  hspi1;
SPI_HandleTypeDef  hspi2;           /* SD 卡（獨立 SPI2）*/
DMA_HandleTypeDef  hdma_spi1_tx;

/* USER CODE BEGIN PV */
/* IPC（放在 USER CODE 區內，CubeMX 重新產生時不會被蓋掉）*/
SemaphoreHandle_t  spi_bus_mutex;   /* 保護 SPI1 匯流排 */
QueueHandle_t      ui_event_queue;  /* InputTask → UITask */
TaskHandle_t       inputTaskHandle;
TaskHandle_t       uiTaskHandle;
TaskHandle_t       netTaskHandle;

/* ── ESP32 UART 接收：中斷 ISR 寫入、task 讀出的環形緩衝區 ── */
#define ESP_RX_SZ 1024
static volatile uint8_t  esp_rx_buf[ESP_RX_SZ];
static volatile uint16_t esp_rx_head;   /* ISR 寫到哪 */
static volatile uint16_t esp_rx_tail;   /* task 讀到哪 */
static uint8_t           esp_rx_byte;   /* HAL 每次收 1 byte 暫存這 */

/* 從 NTP 對時得到的「一天中的秒數」offset，時鐘顯示時加上去（screen_clock.c 讀）*/
volatile uint32_t g_clock_offset = 0;
volatile uint8_t  g_time_valid   = 0;   /* 已成功對到時 = 1 */
char              g_weather[48]  = "";  /* 最近一次抓到的天氣字串 */
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
static int esp_read_line(char *buf, int max, uint32_t timeout_ms);
static int esp_read_bytes(uint8_t *buf, int count, uint32_t timeout_ms);
static int net_download(const char *url, const char *path);
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
    MX_USART3_UART_Init();      /* ESP32-S3 UART */
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

    xTaskCreate(vUITask,    "UITask",    2560, NULL, 3, &uiTaskHandle);   /* 2560 words(10KB)：module 狀態放堆疊，CHIP-8 記憶體 4KB+顯示緩衝 */
    xTaskCreate(vInputTask, "InputTask", 256, NULL, 4, &inputTaskHandle);
    xTaskCreate(vNetTask,   "NetTask",   1024, NULL, 2, &netTaskHandle);  /* ESP32 UART：對時/天氣/下載（下載用到 FatFs，堆疊要夠）*/

    ScreenHome_Register();
    ScreenCalc_Register();
    ScreenGame_Register();
    ScreenPhoto_Register();
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
  * @brief USART3 — 115200，PB10(TX)/PB11(RX)，接 ESP32-S3 做 WiFi
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

    /* 武裝中斷接收：收到 1 byte 就中斷 → HAL_UART_RxCpltCallback */
    HAL_UART_Receive_IT(&huart3, &esp_rx_byte, 1);
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

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * vNetTask — 管 ESP32 UART：對時 + 收指令回話
 *
 * 為什麼獨立一個 task：網路操作（尤其之後下載檔案）可能傳很久，
 * 放在 UI 迴圈會拖畫面。獨立 task + 低優先級，不跟渲染/觸控搶。
 *
 * 目前工作：定期問 TIME? 校準時鐘（對到前 3s 重試、對到後 60s 校準），
 * 順便撈 ring buffer 把 ESP32 回話組成整行交給 esp_parse_line。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void vNetTask(void *pvParameters)
{
    char     line[64];
    uint8_t  len     = 0;
    uint32_t last_q  = 0;
    uint32_t last_wx = 0;

    for (;;) {

        /* 定期送 TIME?：還沒對到時 3 秒重試，對到後 60 秒校準一次 */
        uint32_t interval = g_time_valid ? 60000 : 3000;
        if (last_q == 0 || HAL_GetTick() - last_q >= interval) {
            last_q = HAL_GetTick();
            const char msg[] = "TIME?\r\n";
            HAL_UART_Transmit(&huart3, (uint8_t *)msg, sizeof(msg) - 1, 100);
        }

        /* 定期送 WX?：每 30 秒抓一次天氣（測試用，正式可放慢到數分鐘）*/
        if (last_wx == 0 || HAL_GetTick() - last_wx >= 30000) {
            last_wx = HAL_GetTick();
            const char m2[] = "WX?\r\n";
            HAL_UART_Transmit(&huart3, (uint8_t *)m2, sizeof(m2) - 1, 100);
        }

        /* 撈 ring buffer，組行，遇換行就解析 */
        uint8_t rx;
        while (esp_rx_pop(&rx)) {
            if (rx == '\n' || rx == '\r') {
                if (len > 0) { line[len] = '\0'; esp_parse_line(line); len = 0; }
            } else if (len < sizeof(line) - 1) {
                line[len++] = rx;
            }
        }

        /* 每 15 秒印一次各 task 的堆疊「歷來最少剩餘」（word 數），用來校準堆疊大小 */
        static uint32_t last_hw = 0;
        if (HAL_GetTick() - last_hw >= 15000) {
            last_hw = HAL_GetTick();
            myprintf("stack free (words) — UI:%lu Input:%lu Net:%lu\r\n",
                     (unsigned long)uxTaskGetStackHighWaterMark(uiTaskHandle),
                     (unsigned long)uxTaskGetStackHighWaterMark(inputTaskHandle),
                     (unsigned long)uxTaskGetStackHighWaterMark(netTaskHandle));
        }

        vTaskDelay(pdMS_TO_TICKS(20));   /* 20ms 輪詢一次，回話延遲可忽略 */
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

/* FreeRTOS 堆疊溢位 hook — 任何 task 堆疊爆掉時被呼叫（configCHECK_FOR_STACK_OVERFLOW=2）
 * 立刻印出是哪個 task 並停住，接除錯器即可定位，不會再默默壞掉。 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    myprintf("\r\n!!! STACK OVERFLOW: %s !!!\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* UART 接收完成回呼 — HAL 每收到 1 byte 自動呼叫一次
 * 把 byte 塞進 ring buffer，然後重新武裝下一次接收 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        uint16_t next = (esp_rx_head + 1) % ESP_RX_SZ;
        if (next != esp_rx_tail) {          /* buffer 沒滿才寫，滿了就丟掉這 byte */
            esp_rx_buf[esp_rx_head] = esp_rx_byte;
            esp_rx_head = next;
        }
        HAL_UART_Receive_IT(&huart3, &esp_rx_byte, 1);   /* 重新武裝，繼續收下一個 */
    }
}

/* 從 ring buffer 撈一個 byte；有資料回 1，空的回 0 */
static int esp_rx_pop(uint8_t *out)
{
    if (esp_rx_head == esp_rx_tail) return 0;   /* 空 */
    *out = esp_rx_buf[esp_rx_tail];
    esp_rx_tail = (esp_rx_tail + 1) % ESP_RX_SZ;
    return 1;
}

/* 解析一整行 ESP32 回話。目前認得 "TIME HH:MM:SS" → 更新時鐘 offset */
static void esp_parse_line(const char *s)
{
    myprintf("ESP: %s\r\n", s);                 /* debug：印出收到的整行 */

    unsigned h, m, sec;
    if (sscanf(s, "TIME %u:%u:%u", &h, &m, &sec) == 3) {
        uint32_t uptime = xTaskGetTickCount() / configTICK_RATE_HZ;   /* 開機至今秒數 */
        uint32_t target = h * 3600 + m * 60 + sec;                    /* 真實的一天秒數 */
        /* offset：讓 (uptime + offset) 的一天秒數 == target */
        g_clock_offset = (target + 86400u - (uptime % 86400u)) % 86400u;
        g_time_valid = 1;
        return;
    }

    /* "WX <內容>" → 存起來（之後放上畫面）*/
    if (strncmp(s, "WX ", 3) == 0) {
        strncpy(g_weather, s + 3, sizeof(g_weather) - 1);
        g_weather[sizeof(g_weather) - 1] = '\0';
    }
}

/* 阻塞讀一整行（到 \n）進 buf。逾時回 0，成功回 1。 */
static int esp_read_line(char *buf, int max, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    int len = 0;
    for (;;) {
        uint8_t c;
        if (esp_rx_pop(&c)) {
            if (c == '\n' || c == '\r') {
                if (len > 0) { buf[len] = '\0'; return 1; }   /* 一行結束 */
            } else if (len < max - 1) {
                buf[len++] = c;
            }
        } else {
            if (HAL_GetTick() - start > timeout_ms) return 0;  /* 沒資料太久 → 放棄 */
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

/* 阻塞讀剛好 count 個裸 byte 進 buf。逾時回 0，成功回 1。 */
static int esp_read_bytes(uint8_t *buf, int count, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    int got = 0;
    while (got < count) {
        uint8_t c;
        if (esp_rx_pop(&c)) {
            buf[got++] = c;
            start = HAL_GetTick();     /* 有進資料 → 重置逾時 */
        } else if (HAL_GetTick() - start > timeout_ms) {
            return 0;
        } else {
            vTaskDelay(1);             /* 沒資料 → 讓出 CPU，避免忙等害觸控頓 */
        }
    }
    return 1;
}

/* 下載 url 到 SD 的 path。回傳寫入 byte 數，失敗回負數。
 * 協定：送 "DL url" → 收 "BEGIN" → 迴圈收 "C <len>"+資料、寫檔、回 "A"，
 *       直到 "C 0"。每塊自帶長度，不需事先知道總大小。 */
static int net_download(const char *url, const char *path)
{
    char cmd[128];
    int n = snprintf(cmd, sizeof(cmd), "DL %s\r\n", url);
    HAL_UART_Transmit(&huart3, (uint8_t *)cmd, n, 200);

    /* 等 BEGIN（跳過殘留的 WX/TIME 舊行）；收到 ERR 代表 ESP32 端失敗 */
    char line[80];
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        if (!esp_read_line(line, sizeof(line), 10000)) return -1;
        if (strcmp(line, "BEGIN") == 0) break;
        if (strcmp(line, "ERR")   == 0) return -2;
        if (HAL_GetTick() - t0 > 12000) return -1;
    }

    /* FIL(~550B) 和 buf(512B) 放 static，不佔 NetTask 堆疊（否則會溢位）*/
    static FIL     f;
    static uint8_t buf[512];
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -3;

    int done = 0;
    for (;;) {
        /* 讀塊標頭 "C <len>" */
        if (!esp_read_line(line, sizeof(line), 5000)) { f_close(&f); return -4; }
        int clen;
        if (sscanf(line, "C %d", &clen) != 1)         { f_close(&f); return -5; }
        if (clen == 0) break;                          /* C 0 = 傳完 */
        if (clen > (int)sizeof(buf))                  { f_close(&f); return -6; }

        /* 讀這塊的裸資料，寫檔，回 ack */
        if (!esp_read_bytes(buf, clen, 3000))         { f_close(&f); return -7; }
        UINT bw;
        f_write(&f, buf, clen, &bw);
        HAL_UART_Transmit(&huart3, (uint8_t *)"A", 1, 200);   /* ack：單一 byte，不留殘餘換行 */
        done += clen;
    }
    f_close(&f);
    return done;
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
