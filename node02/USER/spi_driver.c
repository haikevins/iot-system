#include "spi_driver.h"

#define SPI_TRANSFER_TIMEOUT_COUNT  100000U

/**
 * @brief  Configures GPIO and SPI1 for full-duplex master communication.
 * @note   SPI1 is mapped to PA5 (SCK), PA6 (MISO) and PA7 (MOSI).
 * @retval None
 */
void SPI_GPIO_Init(void)
{
    GPIO_InitTypeDef gpioInit = {0};
    SPI_InitTypeDef spiInit = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_SPI1, ENABLE);

    gpioInit.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpioInit.GPIO_Mode = GPIO_Mode_AF_PP;
    gpioInit.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpioInit);

    gpioInit.GPIO_Pin = GPIO_Pin_6;
    gpioInit.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &gpioInit);

    spiInit.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spiInit.SPI_Mode = SPI_Mode_Master;
    spiInit.SPI_DataSize = SPI_DataSize_8b;
    spiInit.SPI_CPOL = SPI_CPOL_Low;
    spiInit.SPI_CPHA = SPI_CPHA_1Edge;
    spiInit.SPI_NSS = SPI_NSS_Soft;
    spiInit.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_32;
    spiInit.SPI_FirstBit = SPI_FirstBit_MSB;
    spiInit.SPI_CRCPolynomial = 7U;

    SPI_Init(SPI1, &spiInit);
    SPI_Cmd(SPI1, ENABLE);
}

/**
 * @brief  Transfers one byte over SPI1 and returns the received byte.
 * @param  transmitData: Byte to transmit.
 * @retval Received byte, or 0xFF if the transfer times out.
 */
uint8_t SPI1_Transfer(uint8_t transmitData)
{
    uint32_t timeoutCounter = SPI_TRANSFER_TIMEOUT_COUNT;

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (--timeoutCounter == 0U)
        {
            return 0xFFU;
        }
    }

    SPI_I2S_SendData(SPI1, transmitData);

    timeoutCounter = SPI_TRANSFER_TIMEOUT_COUNT;

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET)
    {
        if (--timeoutCounter == 0U)
        {
            return 0xFFU;
        }
    }

    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}
