#include "systick_driver.h"

static volatile uint32_t s_tickCountMs = 0U;

/**
 * @brief  Configures SysTick to generate an interrupt every 1 ms.
 * @retval None
 */
void SysTick_Init(void)
{
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000U);
}

/**
 * @brief  SysTick interrupt handler that increments the millisecond counter.
 * @retval None
 */
void SysTick_Handler(void)
{
    s_tickCountMs++;
}

/**
 * @brief  Returns the current millisecond tick count.
 * @retval Milliseconds elapsed since SysTick initialization.
 */
uint32_t SysTick_GetTick(void)
{
    return s_tickCountMs;
}
