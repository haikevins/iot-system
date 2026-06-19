#include "systick_driver.h"

static volatile uint32_t tick_ms = 0;

void SysTick_Init(void) {
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000);
}

void SysTick_Handler(void) {
    tick_ms++;
}

uint32_t SysTick_GetTick(void) {
    return tick_ms;
}