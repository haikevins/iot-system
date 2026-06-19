#include "sx1278.h"
#include "mcp3208.h"
#include "systick_utils.h"
#include <string.h>

// ================= MAIN =================
int main(void)
{
    SysTick_Init();
    SX1278_Init();
    MCP3208_Init();

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
        float temp = MCP3208_GetStableAverageTemp();
        uint8_t status = MCP3208_GetStatusBitmask();

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