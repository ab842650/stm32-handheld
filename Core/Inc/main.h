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

/* SD 卡 SPI 介面（user_diskio_spi.c 依賴這幾個 define）
 * 獨立 SPI2（PB13/14/15），與 ILI9341/XPT2046 的 SPI1 分開；SD_CS 接 PC7 */
#define SD_SPI_HANDLE       hspi2
#define SD_CS_Pin           GPIO_PIN_7
#define SD_CS_GPIO_Port     GPIOC

/* USER CODE END Private defines */

/* BSP include path: Drivers/BSP/ 需加入 STM32CubeIDE Project → Properties
 * → C/C++ Build → Settings → Include Paths：../Drivers/BSP             */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
