/**
 * @file    adc_driver.h
 * @brief  ADC acquisition, NTC temperature conversion and sensor diagnostics.
 */

#ifndef __ADC_DRIVER_H__
#define __ADC_DRIVER_H__

#include <stm32f10x.h>

#define NUMBER_CHANNEL              6U
#define ADC_FULL_SCALE              4095.0f

#define R_FIXED                     10000.0f
#define NTC_R25                     10000.0f
#define NTC_BETA                    3950.0f
#define NTC_T25_K                   298.15f

#define RAW_SAMPLE_COUNT            16U
#define RAW_TRIM_COUNT              4U
#define RAW_SAMPLE_PERIOD_MS        1U

#define ADC_SHORT_THRESHOLD         20U
#define ADC_OPEN_THRESHOLD          4075U
#define RAW_MAX_SPREAD_COUNTS       100U

#define R_NTC_MIN_OHM               250.0f
#define R_NTC_MAX_OHM               500000.0f

#define TEMP_MIN_C                  (-40.0f)
#define TEMP_MAX_C                  125.0f
#define MAX_TEMP_RATE_C_PER_SEC     10.0f
#define RATE_CHECK_MAX_GAP_MS       3000U

#define CROSS_SENSOR_ENABLE         1U
#define CROSS_MIN_VALID_SENSORS     4U
#define CROSS_MAX_DELTA_C           12.0f

#define TEMP_EMA_ALPHA              0.25f
#define SENSOR_UPDATE_MS            1000U
#define FAULT_ASSERT_COUNT          3U
#define FAULT_CLEAR_COUNT           5U
#define TEMP_ERROR_VALUE            (-1000.0f)

typedef uint16_t SensorFaultCode;

#define SENSOR_FAULT_NONE           ((SensorFaultCode)0x0000U)
#define SENSOR_FAULT_SHORT          ((SensorFaultCode)0x0001U)
#define SENSOR_FAULT_OPEN           ((SensorFaultCode)0x0002U)
#define SENSOR_FAULT_SIGNAL_NOISY   ((SensorFaultCode)0x0004U)
#define SENSOR_FAULT_RESISTANCE     ((SensorFaultCode)0x0008U)
#define SENSOR_FAULT_TEMP_RANGE     ((SensorFaultCode)0x0010U)
#define SENSOR_FAULT_RATE           ((SensorFaultCode)0x0020U)
#define SENSOR_FAULT_CROSS_SENSOR   ((SensorFaultCode)0x0040U)
#define SENSOR_FAULT_MODEL          ((SensorFaultCode)0x0080U)

extern volatile uint16_t g_adcRawBuffer[NUMBER_CHANNEL];

/**
 * @brief  Configures the GPIO pins used by ADC1 sensor channels.
 * @note   Analog inputs are mapped to PA0, PA1, PA2, PA3, PB0 and PB1.
 * @retval None
 */
void ADC_GPIO_Init(void);

/**
 * @brief  Configures ADC1 scan conversion and DMA circular mode.
 * @note   DMA continuously updates g_adcRawBuffer with six ADC channels.
 * @retval None
 */
void ADC_DMA_Init(void);

/**
 * @brief  Returns the filtered temperature of one sensor channel.
 * @param  channel: Sensor channel index from 0 to NUMBER_CHANNEL - 1.
 * @retval Temperature in degrees Celsius, or TEMP_ERROR_VALUE on fault.
 */
float ADC_ReadTempFiltered(uint8_t channel);

/**
 * @brief  Calculates the average temperature from all healthy sensors.
 * @retval Average temperature in degrees Celsius, or TEMP_ERROR_VALUE if no sensor is valid.
 */
float ADC_GetStableAverageTemp(void);

/**
 * @brief  Returns the latched sensor-fault summary.
 * @note   Bit n corresponds to sensor channel n.
 * @retval Six-bit sensor fault mask.
 */
uint8_t ADC_GetStatusBitmask(void);

/**
 * @brief  Returns persistent fault flags for one sensor channel.
 * @param  channel: Sensor channel index.
 * @retval Latched SensorFaultCode bitmask.
 */
SensorFaultCode ADC_GetSensorFaultCode(uint8_t channel);

/**
 * @brief  Returns faults observed in the latest sample for one sensor channel.
 * @param  channel: Sensor channel index.
 * @retval Observed SensorFaultCode bitmask.
 */
SensorFaultCode ADC_GetObservedFaultCode(uint8_t channel);

/**
 * @brief  Returns the latest filtered ADC value for one sensor channel.
 * @param  channel: Sensor channel index.
 * @retval Filtered ADC value, or 0 for an invalid channel.
 */
uint16_t ADC_GetSensorRaw(uint8_t channel);

/**
 * @brief  Returns the latest calculated NTC resistance for one sensor channel.
 * @param  channel: Sensor channel index.
 * @retval NTC resistance in ohms, or 0.0 for an invalid channel.
 */
float ADC_GetSensorResistance(uint8_t channel);

#endif
