#ifndef __SYSTICK_DRIVER_H__
#define __SYSTICK_DRIVER_H__

#include <stm32f10x.h>
#include <stdint.h>

void SysTick_Init(void);
void SysTick_Handler(void);
uint32_t SysTick_GetTick(void);

#endif