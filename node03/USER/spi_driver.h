#ifndef __SPI_H__
#define __SPI_H__

#include <stm32f10x.h>

void SPI_GPIO_Init(void);
uint8_t SPI1_Transfer(uint8_t data);

#endif