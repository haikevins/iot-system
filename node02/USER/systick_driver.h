/**
 * @file    systick_driver.h
 * @brief  Millisecond SysTick time-base driver.
 */

#ifndef __SYSTICK_DRIVER_H__
#define __SYSTICK_DRIVER_H__

#include <stm32f10x.h>
#include <stdint.h>

/**
 * @brief  Configures SysTick to generate an interrupt every 1 ms.
 * @retval None
 */
void SysTick_Init(void);

/**
 * @brief  SysTick interrupt handler that increments the millisecond counter.
 * @retval None
 */
void SysTick_Handler(void);

/**
 * @brief  Returns the current millisecond tick count.
 * @retval Milliseconds elapsed since SysTick initialization.
 */
uint32_t SysTick_GetTick(void);

#endif
