#include "sx1278.h"
#include "spi_driver.h"
#include "systick_utils.h"

static void SX1278_GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    // NSS -> PA4
    GPIO_InitStructure.GPIO_Pin = SX1278_NSS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SX1278_NSS_PORT, &GPIO_InitStructure);
    GPIO_SetBits(SX1278_NSS_PORT, SX1278_NSS_PIN);

    // RESET -> PB10
    GPIO_InitStructure.GPIO_Pin = SX1278_RESET_PIN;
    GPIO_Init(SX1278_RESET_PORT, &GPIO_InitStructure);
    GPIO_SetBits(SX1278_RESET_PORT, SX1278_RESET_PIN);

    SPI_GPIO_Init();
}

static void SX1278_Select(void) {
    GPIO_ResetBits(SX1278_NSS_PORT, SX1278_NSS_PIN);
}

void SX1278_Unselect(void) {
    GPIO_SetBits(SX1278_NSS_PORT, SX1278_NSS_PIN);
}

static void SX1278_Reset(void) {
    GPIO_ResetBits(SX1278_RESET_PORT, SX1278_RESET_PIN);
    Delay_Ms(50);
    GPIO_SetBits(SX1278_RESET_PORT, SX1278_RESET_PIN);
    Delay_Ms(50);
}

void SX1278_WriteReg(uint8_t addr, uint8_t data) {
    SX1278_Select();
    SPI1_Transfer(addr | 0x80);
    SPI1_Transfer(data);
    SX1278_Unselect();
}

static uint8_t SX1278_ReadReg(uint8_t addr) {
    uint8_t val;
    SX1278_Select();
    SPI1_Transfer(addr & 0x7F);
    val = SPI1_Transfer(0x00);
    SX1278_Unselect();
    return val;
}

static uint8_t CRC8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void SX1278_Init(void) {
    SX1278_GPIO_Init();
    SX1278_Reset();

    SX1278_WriteReg(REG_OPMODE, MODE_SLEEP);
    SX1278_WriteReg(REG_OPMODE, MODE_STDBY);

    uint32_t frf = (uint32_t) (433000000 / 61.03515625);
    SX1278_WriteReg(REG_FRF_MSB, (frf >> 16) & 0xFF);
    SX1278_WriteReg(REG_FRF_MID, (frf >> 8) & 0xFF);
    SX1278_WriteReg(REG_FRF_LSB, frf & 0xFF);

    SX1278_WriteReg(REG_PA_CONFIG, 0x8F);

    SX1278_WriteReg(REG_MODEM_CONFIG1, 0x72);
    SX1278_WriteReg(REG_MODEM_CONFIG2, 0x74 | 0x04); // CRC ON

    SX1278_WriteReg(REG_SYMB_TIMEOUT_LSB, 0x00);
    SX1278_WriteReg(REG_PREAMBLE_MSB, 0x08);

    SX1278_WriteReg(REG_PAYLOAD_LENGTH, MAX_LORA_PAYLOAD);

    SX1278_WriteReg(REG_FIFO_TX_BASE_ADDR, 0x00);
    SX1278_WriteReg(REG_FIFO_RX_BASE_ADDR, 0x00);

    SX1278_WriteReg(REG_IRQ_FLAGS, 0xFF); // clear IRQ
}

_Bool SX1278_SendPacket(uint8_t addr, uint8_t type, uint8_t *data, uint8_t len,
    uint32_t timeout_ms) {
    if (len > MAX_LORA_PAYLOAD) {
        return 0;
    }

    uint8_t buffer[4 + MAX_LORA_PAYLOAD];
    buffer[0] = addr;
    buffer[1] = type;
    buffer[2] = len;

    for (uint8_t i = 0; i < len; i++) {
        buffer[3 + i] = data[i];
    }

    uint8_t crc = CRC8(buffer, len + 3);

    // load FIFO
    SX1278_WriteReg(REG_FIFO_ADDR_PTR, 0x00);
    SX1278_WriteReg(REG_FIFO, LORA_START_BYTE);

    for (uint8_t i = 0; i < 3 + len; i++) {
        SX1278_WriteReg(REG_FIFO, buffer[i]);
    }

    SX1278_WriteReg(REG_FIFO, crc);

    SX1278_WriteReg(REG_PREAMBLE_LSB, 3 + len + 2);

    SX1278_WriteReg(REG_IRQ_FLAGS, 0xFF); // clear IRQ

    SX1278_WriteReg(REG_OPMODE, 0x83); // TX mode

    uint32_t start = millis();

    while (1) {
        uint8_t irq = SX1278_ReadReg(REG_IRQ_FLAGS);

        if (irq & IRQ_TX_DONE_MASK) // TxDone
        {
            SX1278_WriteReg(REG_IRQ_FLAGS, 0x08);
            break;
        }

        if (millis() - start > timeout_ms) {
            SX1278_WriteReg(REG_OPMODE, MODE_STDBY);
            return 0;
        }
    }

    SX1278_WriteReg(REG_OPMODE, MODE_RXCONTINUOUS);

    return 1;
}

_Bool SX1278_ReceivePacket(LoraPacket_t *packet, uint32_t timeout_ms) {
    uint32_t start = millis();

    while (1) {
        uint8_t irq = SX1278_ReadReg(REG_IRQ_FLAGS);

        if (irq & IRQ_RX_DONE_MASK) // RxDone
        {
            if (irq & IRQ_PAYLOAD_CRC_ERROR_MASK) {
                SX1278_WriteReg(REG_IRQ_FLAGS, IRQ_PAYLOAD_CRC_ERROR_MASK);
                return 0;
            }

            SX1278_WriteReg(REG_IRQ_FLAGS, IRQ_RX_DONE_MASK);

            uint8_t fifoAddr = SX1278_ReadReg(REG_FIFO_RX_CURRENT_ADDR);
            SX1278_WriteReg(REG_FIFO_ADDR_PTR, fifoAddr);

            uint8_t startByte = SX1278_ReadReg(REG_FIFO);
            if (startByte != LORA_START_BYTE) {
                return 0;
            }

            packet -> addr = SX1278_ReadReg(REG_FIFO);
            packet -> type = SX1278_ReadReg(REG_FIFO);
            packet -> len = SX1278_ReadReg(REG_FIFO);

            for (uint8_t i = 0; i < packet -> len; i++) {
                packet -> data[i] = SX1278_ReadReg(REG_FIFO);
            }

            uint8_t crc = SX1278_ReadReg(REG_FIFO);

            uint8_t tmp[MAX_LORA_PAYLOAD + 3];
            tmp[0] = packet -> addr;
            tmp[1] = packet -> type;
            tmp[2] = packet -> len;

            for (uint8_t i = 0; i < packet -> len; i++) {
                tmp[3 + i] = packet -> data[i];
            }

            if (CRC8(tmp, packet -> len + 3) != crc) {
                return 0;
            }

            return 1;
        }

        if (millis() - start > timeout_ms) {
            return 0;
        }
    }
}