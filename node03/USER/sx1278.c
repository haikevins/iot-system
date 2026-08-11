#include "sx1278.h"
#include "spi_driver.h"
#include "systick_utils.h"

#define SX1278_RF_FREQUENCY_HZ      433000000UL
#define SX1278_FSTEP_HZ             61.03515625

/**
 * @brief  Configures NSS, RESET and SPI GPIO used by the SX1278.
 * @retval None
 */
static void SX1278_GPIO_Init(void)
{
    GPIO_InitTypeDef gpioInit = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    gpioInit.GPIO_Pin = SX1278_NSS_PIN;
    gpioInit.GPIO_Mode = GPIO_Mode_Out_PP;
    gpioInit.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SX1278_NSS_PORT, &gpioInit);
    GPIO_SetBits(SX1278_NSS_PORT, SX1278_NSS_PIN);

    gpioInit.GPIO_Pin = SX1278_RESET_PIN;
    GPIO_Init(SX1278_RESET_PORT, &gpioInit);
    GPIO_SetBits(SX1278_RESET_PORT, SX1278_RESET_PIN);

    SPI_GPIO_Init();
}

/**
 * @brief  Drives the SX1278 NSS pin low.
 * @retval None
 */
static void SX1278_Select(void)
{
    GPIO_ResetBits(SX1278_NSS_PORT, SX1278_NSS_PIN);
}

/**
 * @brief  Drives the SX1278 NSS pin high.
 * @retval None
 */
static void SX1278_Unselect(void)
{
    GPIO_SetBits(SX1278_NSS_PORT, SX1278_NSS_PIN);
}

/**
 * @brief  Performs a hardware reset of the SX1278.
 * @retval None
 */
static void SX1278_Reset(void)
{
    GPIO_ResetBits(SX1278_RESET_PORT, SX1278_RESET_PIN);
    Delay_Ms(50U);
    GPIO_SetBits(SX1278_RESET_PORT, SX1278_RESET_PIN);
    Delay_Ms(50U);
}

/**
 * @brief  Writes one SX1278 register.
 * @param  registerAddress: SX1278 register address.
 * @param  value: Register value to write.
 * @retval None
 */
void SX1278_WriteReg(uint8_t registerAddress, uint8_t value)
{
    SX1278_Select();
    SPI1_Transfer((uint8_t)(registerAddress | 0x80U));
    SPI1_Transfer(value);
    SX1278_Unselect();
}

/**
 * @brief  Reads one SX1278 register.
 * @param  registerAddress: SX1278 register address.
 * @retval Register value.
 */
static uint8_t SX1278_ReadReg(uint8_t registerAddress)
{
    uint8_t registerValue;

    SX1278_Select();
    SPI1_Transfer((uint8_t)(registerAddress & 0x7FU));
    registerValue = SPI1_Transfer(0x00U);
    SX1278_Unselect();

    return registerValue;
}

/**
 * @brief  Calculates CRC-8 with initial value 0x00 and polynomial 0x07.
 * @param  data: Input byte array.
 * @param  length: Number of bytes to process.
 * @retval Calculated CRC-8 value.
 */
static uint8_t SX1278_CalculateCrc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0x00U;
    uint8_t byteIndex;
    uint8_t bitIndex;

    for (byteIndex = 0U; byteIndex < length; byteIndex++)
    {
        crc ^= data[byteIndex];

        for (bitIndex = 0U; bitIndex < 8U; bitIndex++)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1U) ^ 0x07U);
            }
            else
            {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

/**
 * @brief  Initializes the SX1278 for 433 MHz LoRa communication.
 * @note   Modem configuration is SF7, BW125 kHz, CR4/5, explicit header,
 *         preamble length 8 and LoRa payload CRC enabled.
 * @retval None
 */
void SX1278_Init(void)
{
    uint32_t frequencyRegister;

    SX1278_GPIO_Init();
    SX1278_Reset();

    SX1278_WriteReg(REG_OPMODE, MODE_SLEEP_LOW_FREQ);
    SX1278_WriteReg(REG_OPMODE, MODE_STDBY_LOW_FREQ);

    frequencyRegister = (uint32_t)((float)SX1278_RF_FREQUENCY_HZ / SX1278_FSTEP_HZ);
    SX1278_WriteReg(REG_FRF_MSB, (uint8_t)((frequencyRegister >> 16U) & 0xFFU));
    SX1278_WriteReg(REG_FRF_MID, (uint8_t)((frequencyRegister >> 8U) & 0xFFU));
    SX1278_WriteReg(REG_FRF_LSB, (uint8_t)(frequencyRegister & 0xFFU));

    SX1278_WriteReg(REG_PA_CONFIG, 0x8FU);
    SX1278_WriteReg(REG_MODEM_CONFIG1, 0x72U);
    SX1278_WriteReg(REG_MODEM_CONFIG2, 0x74U);
    SX1278_WriteReg(REG_SYMB_TIMEOUT_LSB, 0x00U);
    SX1278_WriteReg(REG_PREAMBLE_MSB, 0x00U);
    SX1278_WriteReg(REG_PREAMBLE_LSB, 0x08U);
    SX1278_WriteReg(REG_PAYLOAD_LENGTH, 0x00U);
    SX1278_WriteReg(REG_MAX_PAYLOAD_LENGTH, (uint8_t)MAX_LORA_FRAME_LENGTH);
    SX1278_WriteReg(REG_FIFO_TX_BASE_ADDR, 0x00U);
    SX1278_WriteReg(REG_FIFO_RX_BASE_ADDR, 0x00U);
    SX1278_WriteReg(REG_IRQ_FLAGS, 0xFFU);
    SX1278_WriteReg(REG_OPMODE, MODE_RXCONTINUOUS_LOW_FREQ);
}

/**
 * @brief  Sends one packet using START | ADDR | TYPE | SEQ | LEN | DATA | CRC8.
 * @note   CRC8 covers ADDR | TYPE | SEQ | LEN | DATA and does not include START.
 * @param  address: Node address stored in the packet.
 * @param  packetType: Application packet type.
 * @param  sequence: Transaction sequence number.
 * @param  payload: Pointer to payload bytes. May be NULL when payloadLength is 0.
 * @param  payloadLength: Number of payload bytes.
 * @param  timeoutMs: Maximum transmit wait time in milliseconds.
 * @retval 1 on successful radio transmission, otherwise 0.
 */
_Bool SX1278_SendPacket(uint8_t address,
    uint8_t packetType,
    uint8_t sequence,
    const uint8_t *payload,
    uint8_t payloadLength,
    uint32_t timeoutMs)
{
    uint8_t crcBuffer[MAX_LORA_PAYLOAD + 4U];
    uint8_t crc;
    uint8_t payloadIndex;
    uint8_t frameLength;
    uint32_t startTimeMs;

    if (payloadLength > MAX_LORA_PAYLOAD)
    {
        return 0;
    }

    if ((payloadLength > 0U) && (payload == 0))
    {
        return 0;
    }

    crcBuffer[0] = address;
    crcBuffer[1] = packetType;
    crcBuffer[2] = sequence;
    crcBuffer[3] = payloadLength;

    for (payloadIndex = 0U; payloadIndex < payloadLength; payloadIndex++)
    {
        crcBuffer[4U + payloadIndex] = payload[payloadIndex];
    }

    crc = SX1278_CalculateCrc8(crcBuffer, (uint8_t)(payloadLength + 4U));
    frameLength = (uint8_t)(payloadLength + LORA_FRAME_OVERHEAD);

    SX1278_WriteReg(REG_OPMODE, MODE_STDBY_LOW_FREQ);
    SX1278_WriteReg(REG_FIFO_ADDR_PTR, 0x00U);
    SX1278_WriteReg(REG_FIFO, LORA_START_BYTE);
    SX1278_WriteReg(REG_FIFO, address);
    SX1278_WriteReg(REG_FIFO, packetType);
    SX1278_WriteReg(REG_FIFO, sequence);
    SX1278_WriteReg(REG_FIFO, payloadLength);

    for (payloadIndex = 0U; payloadIndex < payloadLength; payloadIndex++)
    {
        SX1278_WriteReg(REG_FIFO, payload[payloadIndex]);
    }

    SX1278_WriteReg(REG_FIFO, crc);
    SX1278_WriteReg(REG_PAYLOAD_LENGTH, frameLength);
    SX1278_WriteReg(REG_IRQ_FLAGS, 0xFFU);
    SX1278_WriteReg(REG_OPMODE, MODE_TX_LOW_FREQ);

    startTimeMs = millis();

    while (1)
    {
        uint8_t irqFlags = SX1278_ReadReg(REG_IRQ_FLAGS);

        if ((irqFlags & IRQ_TX_DONE_MASK) != 0U)
        {
            SX1278_WriteReg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
            SX1278_WriteReg(REG_OPMODE, MODE_RXCONTINUOUS_LOW_FREQ);
            return 1;
        }

        if ((millis() - startTimeMs) > timeoutMs)
        {
            SX1278_WriteReg(REG_OPMODE, MODE_RXCONTINUOUS_LOW_FREQ);
            return 0;
        }
    }
}

/**
 * @brief  Receives and validates one packet using the application protocol.
 * @note   The function validates radio payload size before copying DATA into
 *         LoraPacket_t, preventing LEN from overflowing the payload buffer.
 * @param  packet: Output packet structure.
 * @param  timeoutMs: Maximum receive wait time in milliseconds.
 * @retval 1 when a complete packet with valid length and CRC is received, otherwise 0.
 */
_Bool SX1278_ReceivePacket(LoraPacket_t *packet, uint32_t timeoutMs)
{
    uint32_t startTimeMs;

    if (packet == 0)
    {
        return 0;
    }

    SX1278_WriteReg(REG_OPMODE, MODE_RXCONTINUOUS_LOW_FREQ);
    startTimeMs = millis();

    while (1)
    {
        uint8_t irqFlags = SX1278_ReadReg(REG_IRQ_FLAGS);

        if ((irqFlags & IRQ_RX_DONE_MASK) != 0U)
        {
            uint8_t receivedFrameLength;
            uint8_t fifoAddress;
            uint8_t startByte;
            uint8_t receivedCrc;
            uint8_t expectedFrameLength;
            uint8_t crcBuffer[MAX_LORA_PAYLOAD + 4U];
            uint8_t payloadIndex;

            if ((irqFlags & IRQ_PAYLOAD_CRC_ERROR_MASK) != 0U)
            {
                SX1278_WriteReg(REG_IRQ_FLAGS,
                    (uint8_t)(IRQ_RX_DONE_MASK | IRQ_PAYLOAD_CRC_ERROR_MASK));
                return 0;
            }

            receivedFrameLength = SX1278_ReadReg(REG_RX_NB_BYTES);
            SX1278_WriteReg(REG_IRQ_FLAGS, IRQ_RX_DONE_MASK);

            if ((receivedFrameLength < LORA_FRAME_OVERHEAD) ||
                (receivedFrameLength > MAX_LORA_FRAME_LENGTH))
            {
                return 0;
            }

            fifoAddress = SX1278_ReadReg(REG_FIFO_RX_CURRENT_ADDR);
            SX1278_WriteReg(REG_FIFO_ADDR_PTR, fifoAddress);

            startByte = SX1278_ReadReg(REG_FIFO);

            if (startByte != LORA_START_BYTE)
            {
                return 0;
            }

            packet->addr = SX1278_ReadReg(REG_FIFO);
            packet->type = SX1278_ReadReg(REG_FIFO);
            packet->seq = SX1278_ReadReg(REG_FIFO);
            packet->len = SX1278_ReadReg(REG_FIFO);

            if (packet->len > MAX_LORA_PAYLOAD)
            {
                return 0;
            }

            expectedFrameLength = (uint8_t)(packet->len + LORA_FRAME_OVERHEAD);

            if (receivedFrameLength != expectedFrameLength)
            {
                return 0;
            }

            for (payloadIndex = 0U; payloadIndex < packet->len; payloadIndex++)
            {
                packet->data[payloadIndex] = SX1278_ReadReg(REG_FIFO);
            }

            receivedCrc = SX1278_ReadReg(REG_FIFO);

            crcBuffer[0] = packet->addr;
            crcBuffer[1] = packet->type;
            crcBuffer[2] = packet->seq;
            crcBuffer[3] = packet->len;

            for (payloadIndex = 0U; payloadIndex < packet->len; payloadIndex++)
            {
                crcBuffer[4U + payloadIndex] = packet->data[payloadIndex];
            }

            if (SX1278_CalculateCrc8(crcBuffer,
                (uint8_t)(packet->len + 4U)) != receivedCrc)
            {
                return 0;
            }

            return 1;
        }

        if ((millis() - startTimeMs) > timeoutMs)
        {
            return 0;
        }
    }
}
