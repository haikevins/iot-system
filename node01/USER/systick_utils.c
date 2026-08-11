#include "systick_driver.h"
#include "systick_utils.h"

/**
 * @brief  Returns the current system time in milliseconds.
 * @retval Current millisecond tick count.
 */
uint32_t millis(void)
{
    return SysTick_GetTick();
}

/**
 * @brief  Blocks execution for the requested number of milliseconds.
 * @param  delayMs: Delay duration in milliseconds.
 * @retval None
 */
void Delay_Ms(uint32_t delayMs)
{
    uint32_t startTimeMs = millis();

    while ((millis() - startTimeMs) < delayMs)
    {
    }
}
