/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32f4xx_hal_msp.c
  * @brief        MSP init — board-level GPIO / clock / DMA / NVIC setup.
  *
  * HAL_xxx_Init() calls HAL_xxx_MspInit(), a weak hook, which is where the
  * hardware-specific wiring lives so it stays out of the driver logic.
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

/* SPI1 (APB2): PA5 SCK, PA6 MISO, PA7 MOSI — all AF5 per datasheet Table 9. */
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

        /* MISO (PA6) — pulled up; the display may never drive it */
        GPIO_InitStruct.Pin       = GPIO_PIN_6;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_PULLUP;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* SPI1_TX maps to DMA2 Stream3 Channel3 (RM Table 43). */
        hdma_spi1_tx.Instance                 = DMA2_Stream3;
        hdma_spi1_tx.Init.Channel             = DMA_CHANNEL_3;
        hdma_spi1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_spi1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;  /* SPI DR address is fixed */
        hdma_spi1_tx.Init.MemInc              = DMA_MINC_ENABLE;   /* walk through the buffer  */
        hdma_spi1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_spi1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_spi1_tx.Init.Mode                = DMA_NORMAL;         /* one-shot, not circular   */
        hdma_spi1_tx.Init.Priority            = DMA_PRIORITY_HIGH;
        hdma_spi1_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_spi1_tx) != HAL_OK) { Error_Handler(); }

        /* Bind the DMA to the SPI handle so Transmit_DMA can find it. */
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

		/* MISO (PB14) — pulled up; floats while the card is deselected */
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

/* USART2 = debug console on PA2/PA3; USART3 = ESP32 link on PB10/PB11. */
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

        /* Priority 5 is at/below configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
         * which is what makes the FromISR API legal from this ISR. */
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
