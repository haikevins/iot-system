/**
 * @file    spi_driver.h
 * @brief  SPI1 low-level driver for STM32F103.
 */

#ifndef __SPI_H__
#define __SPI_H__

#include <stm32f10x.h>

/**
 * @brief  Configures GPIO and SPI1 for full-duplex master communication.
 * @note   SPI1 is mapped to PA5 (SCK), PA6 (MISO) and PA7 (MOSI).
 * @retval None
 */
void SPI_GPIO_Init(void);

/**
 * @brief  Transfers one byte over SPI1 and returns the received byte.
 * @param  transmitData: Byte to transmit.
 * @retval Received byte, or 0xFF if the transfer times out.
 */
uint8_t SPI1_Transfer(uint8_t transmitData);

#endif
