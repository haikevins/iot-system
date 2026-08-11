#include "sx1278.h"
#include "adc_driver.h"
#include "systick_driver.h"
#include "systick_utils.h"

#define NODE_RECEIVE_TIMEOUT_MS             2000U
#define DATA_TRANSMIT_TIMEOUT_MS            1000U
#define ACK_WAIT_TIMEOUT_MS                 500U
#define DATA_TRANSMIT_MAX_ATTEMPTS          3U

#define SENSOR_COUNT                        6U
#define TEMPERATURE_PAYLOAD_LENGTH          (3U + SENSOR_COUNT)
#define PAYLOAD_TEMPERATURE_LSB_INDEX       0U
#define PAYLOAD_TEMPERATURE_MSB_INDEX       1U
#define PAYLOAD_STATUS_INDEX                2U
#define PAYLOAD_FAULT_BASE_INDEX            3U

#define TEMPERATURE_SCALE                   100.0f
#define TEMPERATURE_INVALID_CENTI_C         ((int16_t)-32768)

volatile uint32_t g_successfulTransactionCount = 0U;
volatile uint32_t g_transmitFailureCount = 0U;
volatile uint32_t g_ackTimeoutCount = 0U;
volatile uint32_t g_retryExhaustedCount = 0U;

/**
 * @brief  Converts temperature in degrees Celsius to signed centi-degrees.
 * @note   The value 0x8000 is reserved as an invalid-temperature marker.
 * @param  temperatureCelsius: Temperature in degrees Celsius.
 * @retval Signed temperature multiplied by 100, or 0x8000 when out of range.
 */
static int16_t EncodeTemperatureCentiCelsius(float temperatureCelsius)
{
    float scaledTemperature;

    if ((temperatureCelsius < -327.67f) ||
        (temperatureCelsius > 327.67f))
    {
        return TEMPERATURE_INVALID_CENTI_C;
    }

    scaledTemperature = temperatureCelsius * TEMPERATURE_SCALE;

    if (scaledTemperature >= 0.0f)
    {
        scaledTemperature += 0.5f;
    }
    else
    {
        scaledTemperature -= 0.5f;
    }

    return (int16_t)scaledTemperature;
}

/**
 * @brief  Waits for an ACK that matches the current node and sequence number.
 * @param  sequence: Sequence number of the DATA packet being acknowledged.
 * @retval 1 when a matching ACK is received before timeout, otherwise 0.
 */
static _Bool WaitForAcknowledgement(uint8_t sequence)
{
    uint32_t startTimeMs = millis();

    while ((millis() - startTimeMs) < ACK_WAIT_TIMEOUT_MS)
    {
        LoraPacket_t packet;
        uint32_t elapsedTimeMs = millis() - startTimeMs;
        uint32_t remainingTimeMs = ACK_WAIT_TIMEOUT_MS - elapsedTimeMs;

        if (!SX1278_ReceivePacket(&packet, remainingTimeMs))
        {
            return 0;
        }

        if ((packet.addr == SX1278_ADDR) &&
            (packet.type == SX1278_TYPE_ACK) &&
            (packet.seq == sequence) &&
            (packet.len == 0U))
        {
            return 1;
        }
    }

    return 0;
}

/**
 * @brief  Sends one DATA packet and retries when the matching ACK is not received.
 * @param  sequence: Sequence number copied from the gateway POLL packet.
 * @param  payload: Temperature, status and six detailed-fault bytes.
 * @retval 1 when the transaction is acknowledged, otherwise 0.
 */
static _Bool SendTemperatureWithRetry(uint8_t sequence,
    const uint8_t payload[TEMPERATURE_PAYLOAD_LENGTH])
{
    uint8_t attemptIndex;

    for (attemptIndex = 0U;
         attemptIndex < DATA_TRANSMIT_MAX_ATTEMPTS;
         attemptIndex++)
    {
        if (!SX1278_SendPacket(SX1278_ADDR,
            SX1278_TYPE_TEMPERATURE,
            sequence,
            payload,
            TEMPERATURE_PAYLOAD_LENGTH,
            DATA_TRANSMIT_TIMEOUT_MS))
        {
            g_transmitFailureCount++;
            continue;
        }

        if (WaitForAcknowledgement(sequence))
        {
            g_successfulTransactionCount++;
            return 1;
        }

        g_ackTimeoutCount++;
    }

    g_retryExhaustedCount++;
    return 0;
}

/**
 * @brief  Waits for gateway POLL packets and returns temperature and diagnostics.
 * @note   DATA payload is TEMP_L | TEMP_H | STATUS | FAULT_S1..FAULT_S6.
 * @note   Each detailed fault byte contains the low 8 bits of SensorFaultCode.
 * @retval Never returns.
 */
int main(void)
{
    SysTick_Init();
    SX1278_Init();
    ADC_GPIO_Init();
    ADC_DMA_Init();

    while (1)
    {
        LoraPacket_t receivedPacket;
        float averageTemperatureCelsius;
        int16_t encodedTemperature;
        uint8_t sensorStatusMask;
        uint8_t sensorIndex;
        uint8_t transmitPayload[TEMPERATURE_PAYLOAD_LENGTH];

        if (!SX1278_ReceivePacket(&receivedPacket, NODE_RECEIVE_TIMEOUT_MS))
        {
            continue;
        }

        if ((receivedPacket.addr != SX1278_ADDR) ||
            (receivedPacket.type != SX1278_TYPE_POLL) ||
            (receivedPacket.len != 0U))
        {
            continue;
        }

        averageTemperatureCelsius = ADC_GetStableAverageTemp();
        sensorStatusMask = (uint8_t)(ADC_GetStatusBitmask() & 0x3FU);
        encodedTemperature = EncodeTemperatureCentiCelsius(averageTemperatureCelsius);

        transmitPayload[PAYLOAD_TEMPERATURE_LSB_INDEX] =
            (uint8_t)((uint16_t)encodedTemperature & 0x00FFU);
        transmitPayload[PAYLOAD_TEMPERATURE_MSB_INDEX] =
            (uint8_t)(((uint16_t)encodedTemperature >> 8U) & 0x00FFU);
        transmitPayload[PAYLOAD_STATUS_INDEX] = sensorStatusMask;

        for (sensorIndex = 0U; sensorIndex < SENSOR_COUNT; sensorIndex++)
        {
            transmitPayload[PAYLOAD_FAULT_BASE_INDEX + sensorIndex] =
                (uint8_t)(ADC_GetSensorFaultCode(sensorIndex) & 0x00FFU);
        }

        SendTemperatureWithRetry(receivedPacket.seq, transmitPayload);
    }
}
