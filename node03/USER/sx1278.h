#ifndef __SX1278_H__
#define __SX1278_H__

#include <stm32f10x.h>

// Format packet: START || ADDR || TYPE || LEN || DATA || CRC

#define SX1278_ADDR 0x03 // Node ID
#define SX1278_TYPE_TEMPERATURE 0x01 // Data type
#define SX1278_TYPE_ACK 0x10 // ACK from gateway
#define SX1278_TYPE_POLL 0x23

#define SX1278_NSS_PIN GPIO_Pin_4
#define SX1278_NSS_PORT GPIOA

#define SX1278_RESET_PIN GPIO_Pin_10
#define SX1278_RESET_PORT GPIOB

// #define SX1278_DIO0_PIN GPIO_Pin_11
// #define SX1278_DIO0_PORT GPIOB

#define LORA_START_BYTE 0xAA
#define MAX_LORA_PAYLOAD 64

// SX1278 Registers
#define REG_FIFO 0x00
#define REG_OPMODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_FIFO_RX_BASE_ADDR 0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_MODEM_CONFIG1 0x1D
#define REG_MODEM_CONFIG2 0x1E
#define REG_SYMB_TIMEOUT_LSB 0x20
#define REG_PREAMBLE_MSB 0x21
#define REG_PREAMBLE_LSB 0x22
#define REG_PAYLOAD_LENGTH 0x23
#define REG_DIO_MAPPING1 0x40

// IRQ Flags bits
#define IRQ_TX_DONE_MASK 0x08
#define IRQ_RX_DONE_MASK 0x40
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20

// Operation modes
#define MODE_SLEEP 0x80
#define MODE_STDBY 0x81
#define MODE_TX 0x83
#define MODE_RXCONTINUOUS 0x85

typedef struct {
    uint8_t addr;
    uint8_t type;
    uint8_t len;
    uint8_t data[MAX_LORA_PAYLOAD];
}
LoraPacket_t;

void SX1278_Init(void);
_Bool SX1278_SendPacket(uint8_t addr, uint8_t type, uint8_t *data, uint8_t len,
    uint32_t timeout_ms);
_Bool SX1278_ReceivePacket(LoraPacket_t *packet, uint32_t timeout_ms);

void SX1278_Unselect(void);

void SX1278_WriteReg(uint8_t addr, uint8_t data);

#endif