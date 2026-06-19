#include "systick_driver.h"

uint32_t millis(void) {
    return SysTick_GetTick();
}

void Delay_Ms(uint32_t ms) {
    uint32_t start = millis();
    while ((millis() - start) < ms);
}