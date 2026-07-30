/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "stm32f4xx_it.h"
#include "FreeRTOS.h"
#include "task.h"

extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_spi1_tx;
extern TaskHandle_t      inputTaskHandle;
extern UART_HandleTypeDef huart3;

/* Cortex-M4 Exception Handlers -----------------------------------------------*/

void NMI_Handler(void)          { while (1) {} }
void HardFault_Handler(void)    { while (1) {} }
void MemManage_Handler(void)    { while (1) {} }
void BusFault_Handler(void)     { while (1) {} }
void UsageFault_Handler(void)   { while (1) {} }
void DebugMon_Handler(void)     {}

/* Peripheral Interrupt Handlers ----------------------------------------------*/

/**
  * @brief EXTI line0 — User Button (PA0)
  */
void EXTI0_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}

/**
  * @brief DMA2 Stream3 — SPI1_TX 完成中斷
  */
void DMA2_Stream3_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_spi1_tx);
}

/**
  * @brief TIM2 — HAL timebase
  */
void TIM2_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim2);
}

/* USER CODE BEGIN 1 */

/**
  * @brief EXTI9_5 — T_IRQ (PC5) 觸控中斷，falling edge
  * @note  對應的 HAL_GPIO_EXTI_Callback 放在 main.c（USER CODE 區）以防 regen
  */
void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_5);
}

/**
  * @brief USART3 — ESP32 UART。HAL 處理旗標並在收滿時呼叫 RxCpltCallback
  */
void USART3_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart3);
}

/* USER CODE END 1 */
