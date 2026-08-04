/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* Exported functions --------------------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN Private defines */

/* SD card SPI, required by user_diskio_spi.c. On its own bus (SPI2,
 * PB13/14/15) so it never contends with the display and touch on SPI1. */
#define SD_SPI_HANDLE       hspi2
#define SD_CS_Pin           GPIO_PIN_7
#define SD_CS_GPIO_Port     GPIOC

/* USER CODE END Private defines */

/* ../Drivers/BSP must be on the include path: STM32CubeIDE Project ->
 * Properties -> C/C++ Build -> Settings -> Include Paths */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
