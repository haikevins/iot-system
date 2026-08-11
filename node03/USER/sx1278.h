/**
 * @file    sx1278.h
 * @brief   SX1278 LoRa packet driver for node address 0x03.
 */

#ifndef __SX1278_H__
#define __SX1278_H__

#include <stm32f10x.h>

#define SX1278_ADDR                         0x03U
#define SX1278_TYPE_TEMPERATURE             0x01U
#define SX1278_TYPE_ACK                     0x10U
#define SX1278_TYPE_POLL                    0x23U

#define SX1278_NSS_PIN                      GPIO_Pin_4
#define SX1278_NSS_PORT                     GPIOA
#define SX1278_RESET_PIN                    GPIO_Pin_10
#define SX1278_RESET_PORT                   GPIOB

#define LORA_START_BYTE                     0xAAU
#define MAX_LORA_PAYLOAD                    64U
#define LORA_FRAME_OVERHEAD                 6U
#define MAX_LORA_FRAME_LENGTH               (MAX_LORA_PAYLOAD + LORA_FRAME_OVERHEAD)

#define REG_FIFO                            0x00U
#define REG_OPMODE                          0x01U
#define REG_FRF_MSB                         0x06U
#define REG_FRF_MID                         0x07U
#define REG_FRF_LSB                         0x08U
#define REG_PA_CONFIG                       0x09U
#define REG_FIFO_ADDR_PTR                   0x0DU
#define REG_FIFO_TX_BASE_ADDR               0x0EU
#define REG_FIFO_RX_BASE_ADDR               0x0FU
#define REG_FIFO_RX_CURRENT_ADDR            0x10U
#define REG_IRQ_FLAGS                       0x12U
#define REG_RX_NB_BYTES                     0x13U
#define REG_MODEM_CONFIG1                   0x1DU
#define REG_MODEM_CONFIG2                   0x1EU
#define REG_SYMB_TIMEOUT_LSB                0x1FU
#define REG_PREAMBLE_MSB                    0x20U
#define REG_PREAMBLE_LSB                    0x21U
#define REG_PAYLOAD_LENGTH                  0x22U
#define REG_MAX_PAYLOAD_LENGTH              0x23U
#define REG_DIO_MAPPING1                    0x40U

#define IRQ_TX_DONE_MASK                    0x08U
#define IRQ_RX_DONE_MASK                    0x40U
#define IRQ_PAYLOAD_CRC_ERROR_MASK          0x20U

#define MODE_SLEEP_LOW_FREQ                 0x88U
#define MODE_STDBY_LOW_FREQ                 0x89U
#define MODE_TX_LOW_FREQ                    0x8BU
#define MODE_RXCONTINUOUS_LOW_FREQ          0x8DU

typedef struct
{
    uint8_t addr;
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t data[MAX_LORA_PAYLOAD];
} LoraPacket_t;

/**
 * @brief  Initializes the SX1278 GPIO, SPI interface and LoRa modem registers.
 * @retval None
 */
void SX1278_Init(void);

/**
 * @brief  Sends one application packet through the SX1278 FIFO.
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
    uint32_t timeoutMs);

/**
 * @brief  Receives and validates one protocol packet from the SX1278.
 * @param  packet: Output packet structure.
 * @param  timeoutMs: Maximum receive wait time in milliseconds.
 * @retval 1 when a complete packet with valid length and CRC is received, otherwise 0.
 */
_Bool SX1278_ReceivePacket(LoraPacket_t *packet, uint32_t timeoutMs);

/**
 * @brief  Writes one SX1278 register.
 * @param  registerAddress: SX1278 register address.
 * @param  value: Register value to write.
 * @retval None
 */
void SX1278_WriteReg(uint8_t registerAddress, uint8_t value);

#endif
