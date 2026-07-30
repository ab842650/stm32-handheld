/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32f4xx_hal_msp.c
  * @brief        MSP Initialization（周邊腳位 / 時脈的底層設定）
  *
  * HAL_xxx_Init() 內部會呼叫 HAL_xxx_MspInit()——
  * 這是 STM32 HAL 的「弱定義鉤子」，讓你把硬體相關的 GPIO/DMA/NVIC
  * 設定與上層邏輯分開。
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"

extern DMA_HandleTypeDef hdma_spi1_tx;

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * SPI1 MSP Init
 *
 * SPI1 在 APB2 上，腳位：
 *   PA5 → SPI1_SCK  (AF5)
 *   PA6 → SPI1_MISO (AF5)，MISO 加 pull-up 避免浮接雜訊
 *   PA7 → SPI1_MOSI (AF5)
 *
 * AF（Alternate Function）= 腳位的第二功能，
 * 需要設定 GPIO_MODE_AF_PP 並指定 AF 編號，
 * STM32F407 的 SPI1 對應 AF5（查 datasheet Table 9）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hspi->Instance == SPI1)
    {
        __HAL_RCC_SPI1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        /* SCK (PA5) + MOSI (PA7) */
        GPIO_InitStruct.Pin       = GPIO_PIN_5 | GPIO_PIN_7;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* MISO (PA6) — pull-up，萬一螢幕沒有 MISO 輸出時避免浮接 */
        GPIO_InitStruct.Pin       = GPIO_PIN_6;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_PULLUP;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* ── DMA2 Stream3 Channel3 → SPI1_TX ──────────────────────────────
         * STM32F407 DMA 對應表（Reference Manual Table 43）：
         *   SPI1_TX = DMA2, Stream3, Channel3
         *            或 Stream5, Channel3（備用）
         * ------------------------------------------------------------------ */
        hdma_spi1_tx.Instance                 = DMA2_Stream3;
        hdma_spi1_tx.Init.Channel             = DMA_CHANNEL_3;
        hdma_spi1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_spi1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;  /* SPI DR 位址固定 */
        hdma_spi1_tx.Init.MemInc              = DMA_MINC_ENABLE;   /* buffer 位址遞增 */
        hdma_spi1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_spi1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_spi1_tx.Init.Mode                = DMA_NORMAL;         /* 傳完就停，不循環 */
        hdma_spi1_tx.Init.Priority            = DMA_PRIORITY_HIGH;
        hdma_spi1_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_spi1_tx) != HAL_OK) { Error_Handler(); }

        /* 把 DMA handle 綁到 SPI handle，HAL_SPI_Transmit_DMA 才知道用哪個 DMA */
        __HAL_LINKDMA(hspi, hdmatx, hdma_spi1_tx);
    }else if (hspi->Instance == SPI2){
    	__HAL_RCC_SPI2_CLK_ENABLE();
		__HAL_RCC_GPIOB_CLK_ENABLE();

		/* SCK (PB13) + MOSI (PB15) */
		GPIO_InitStruct.Pin       = GPIO_PIN_13 | GPIO_PIN_15;
		GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Pull      = GPIO_NOPULL;
		GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
		HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

		/* MISO (PB14) — pull-up，SD 卡未選中時避免 MISO 浮接雜訊 */
		GPIO_InitStruct.Pin       = GPIO_PIN_14;
		GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Pull      = GPIO_PULLUP;
		GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
		HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    }
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef* hspi)
{
    if (hspi->Instance == SPI1)
    {
        __HAL_RCC_SPI1_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * UART MSP Init — USART2 on PA2(TX) / PA3(RX)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (huart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        GPIO_InitStruct.Pin       = GPIO_PIN_2 | GPIO_PIN_3;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
    else if (huart->Instance == USART3)
    {
        __HAL_RCC_USART3_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();

        GPIO_InitStruct.Pin       = GPIO_PIN_10 | GPIO_PIN_11;   /* PB10=TX, PB11=RX */
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* 開 USART3 中斷：優先級 5（數字 ≥ FreeRTOS 門檻，ISR 內可安全用 FromISR API）*/
        HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(USART3_IRQn);
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* huart)
{
    if (huart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2 | GPIO_PIN_3);
    }
    else if (huart->Instance == USART3)
    {
        __HAL_RCC_USART3_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOB, GPIO_PIN_10 | GPIO_PIN_11);
    }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
