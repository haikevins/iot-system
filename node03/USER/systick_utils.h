/**
 * @file    systick_utils.h
 * @brief  Millisecond timing utility functions.
 */

#ifndef __SYSTICK_UTILS_H__
#define __SYSTICK_UTILS_H__

#include <stdint.h>

/**
 * @brief  Returns the current system time in milliseconds.
 * @retval Current millisecond tick count.
 */
uint32_t millis(void);

/**
 * @brief  Blocks execution for the requested number of milliseconds.
 * @param  delayMs: Delay duration in milliseconds.
 * @retval None
 */
void Delay_Ms(uint32_t delayMs);

#endif
