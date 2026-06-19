#include "sx1278.h"
#include "adc_driver.h"
#include "systick_utils.h"
#include <string.h>

volatile uint32_t okCount = 0;
volatile uint32_t failCount = 0;
volatile uint32_t txFailCount = 0;
volatile uint32_t ackTimeoutCount = 0;

int main(void)
{
    SysTick_Init();
    SX1278_Init();

    ADC_GPIO_Init();
    ADC_DMA_Init();

    while (1)
    {
        LoraPacket_t packet;
			
				SX1278_WriteReg(REG_OPMODE, 0x8D);

        // ===== WAIT POLL =====
        if (!SX1278_ReceivePacket(&packet, 2000))
            continue;

        if (packet.addr != SX1278_ADDR)
            continue;

        if (packet.type != SX1278_TYPE_POLL)
            continue;

        // ===== READ SENSOR =====
        float temp = ADC_GetStableAverageTemp();
        uint8_t status = ADC_GetStatusBitmask();

        uint8_t txData[5];
        memcpy(txData, &temp, 4);
        txData[4] = status;

        // ===== SEND DATA =====
        SX1278_SendPacket(
            SX1278_ADDR,
            SX1278_TYPE_TEMPERATURE,
            txData,
            5,
            1000
        );
				
				SX1278_WriteReg(REG_OPMODE, 0x8D);
    }
}