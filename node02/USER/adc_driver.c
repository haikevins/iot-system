#include "adc_driver.h"
#include "systick_utils.h"
#include <math.h>

/**
 * * * @brief  DMA buffer containing the newest ADC conversion for each sensor channel.
 */
volatile uint16_t g_adcRawBuffer[NUMBER_CHANNEL];

/**
 * * * @brief  ADC channel mapping for the six NTC sensor inputs.
 */
static const uint8_t s_adcChannelList[NUMBER_CHANNEL] = {0U, 1U, 2U, 3U, 8U, 9U};

/**
 * * * @brief  Per-channel temperature calibration scale factors.
 */
static const float s_temperatureScale[NUMBER_CHANNEL] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f
};

/**
 * * * @brief  Per-channel temperature calibration offsets in degrees Celsius.
 */
static const float s_temperatureOffset[NUMBER_CHANNEL] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

#define FAULT_KIND_COUNT 8U

/**
 * * * @brief  Fault bits processed independently by persistence logic.
 */
static const SensorFaultCode s_faultBitList[FAULT_KIND_COUNT] = {
    SENSOR_FAULT_SHORT,
    SENSOR_FAULT_OPEN,
    SENSOR_FAULT_SIGNAL_NOISY,
    SENSOR_FAULT_RESISTANCE,
    SENSOR_FAULT_TEMP_RANGE,
    SENSOR_FAULT_RATE,
    SENSOR_FAULT_CROSS_SENSOR,
    SENSOR_FAULT_MODEL
};

typedef struct
{
    float filteredTemperature;
    float resistanceOhm;
    float lastCandidateTemperature;

    uint16_t filteredRaw;
    uint16_t rawSpread;

    uint32_t lastCandidateTimeMs;
    uint32_t lastGoodTimeMs;

    SensorFaultCode observedFaults;
    SensorFaultCode latchedFaults;

    uint8_t faultAssertCount[FAULT_KIND_COUNT];
    uint8_t faultClearCount[FAULT_KIND_COUNT];

    uint8_t isInitialized;
    uint8_t hasLastCandidate;
} SensorState_t;

static SensorState_t s_sensorState[NUMBER_CHANNEL];

static uint32_t s_lastUpdateTimeMs;
static uint8_t s_isFrameValid;

/**
 * @brief  Returns the absolute value of a floating-point number.
 * @param  value: Input value.
 * @retval Absolute value of the input.
 */
static float GetAbsoluteValue(float value)
{
    return (value < 0.0f) ? -value : value;
}

/**
 * @brief  Sorts an unsigned 16-bit array in ascending order.
 * @param  values: Array to sort.
 * @param  length: Number of elements.
 * @retval None
 */
static void SortUint16Ascending(uint16_t *values, uint8_t length)
{
    uint8_t index;
    uint8_t sortIndex;

    for (index = 1U; index < length; index++)
    {
        uint16_t currentValue = values[index];
        sortIndex = index;

        while ((sortIndex > 0U) && (values[sortIndex - 1U] > currentValue))
        {
            values[sortIndex] = values[sortIndex - 1U];
            sortIndex--;
        }

        values[sortIndex] = currentValue;
    }
}

/**
 * @brief  Sorts a floating-point array in ascending order.
 * @param  values: Array to sort.
 * @param  length: Number of elements.
 * @retval None
 */
static void SortFloatAscending(float *values, uint8_t length)
{
    uint8_t index;
    uint8_t sortIndex;

    for (index = 1U; index < length; index++)
    {
        float currentValue = values[index];
        sortIndex = index;

        while ((sortIndex > 0U) && (values[sortIndex - 1U] > currentValue))
        {
            values[sortIndex] = values[sortIndex - 1U];
            sortIndex--;
        }

        values[sortIndex] = currentValue;
    }
}

/**
 * @brief  Calculates the median of a floating-point array.
 * @param  values: Array containing samples.
 * @param  length: Number of samples.
 * @retval Median value.
 */
static float CalculateMedian(float *values, uint8_t length)
{
    SortFloatAscending(values, length);

    if ((length & 1U) != 0U)
    {
        return values[length / 2U];
    }

    return 0.5f * (values[(length / 2U) - 1U] + values[length / 2U]);
}

/**
 * @brief  Calculates a trimmed mean and reports the raw sample spread.
 * @param  samples: Raw ADC sample array.
 * @param  spreadOut: Output pointer for max-min spread.
 * @retval Trimmed mean ADC value.
 */
static uint16_t CalculateTrimmedMean(uint16_t *samples,
    uint16_t *spreadOut)
{
    uint8_t index;
    uint32_t sum = 0U;

    const uint8_t firstIncludedIndex = RAW_TRIM_COUNT;
    const uint8_t lastExcludedIndex = RAW_SAMPLE_COUNT - RAW_TRIM_COUNT;
    const uint8_t count = RAW_SAMPLE_COUNT - (2U * RAW_TRIM_COUNT);

    SortUint16Ascending(samples, RAW_SAMPLE_COUNT);

    if (spreadOut != 0)
    {
        *spreadOut = (uint16_t)(samples[RAW_SAMPLE_COUNT - 1U] - samples[0U]);
    }

    for (index = firstIncludedIndex; index < lastExcludedIndex; index++)
    {
        sum += samples[index];
    }

    return (uint16_t)((sum + (count / 2U)) / count);
}

/**
 * @brief  Acquires a short ADC burst from all sensor channels and filters each channel.
 * @param  filteredRawOut: Output filtered ADC values.
 * @param  rawSpreadOut: Output max-min sample spread values.
 * @retval None
 */
static void AcquireFilteredRawSamples(uint16_t filteredRawOut[NUMBER_CHANNEL],
    uint16_t rawSpreadOut[NUMBER_CHANNEL])
{
    uint16_t samples[NUMBER_CHANNEL][RAW_SAMPLE_COUNT];
    uint8_t index;
    uint8_t channelIndex;

    for (index = 0U; index < RAW_SAMPLE_COUNT; index++)
    {
        for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
        {
            samples[channelIndex][index] = g_adcRawBuffer[channelIndex];
        }

        if ((index + 1U) < RAW_SAMPLE_COUNT)
        {
            Delay_Ms(RAW_SAMPLE_PERIOD_MS);
        }
    }

    for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
    {
        filteredRawOut[channelIndex] = CalculateTrimmedMean(samples[channelIndex], &rawSpreadOut[channelIndex]);
    }
}

/**
 * @brief  Detects open, short and excessive-noise conditions from raw ADC data.
 * @param  rawAdc: Filtered ADC value.
 * @param  rawSpreadValue: Maximum minus minimum value in the raw sample burst.
 * @retval Detected raw-signal fault bitmask.
 */
static SensorFaultCode DiagnoseRawSignal(uint16_t rawAdc,
    uint16_t rawSpreadValue)
{
    SensorFaultCode faultFlags = SENSOR_FAULT_NONE;

    if (rawAdc <= ADC_SHORT_THRESHOLD)
    {
        faultFlags |= SENSOR_FAULT_SHORT;
    }
    else if (rawAdc >= ADC_OPEN_THRESHOLD)
    {
        faultFlags |= SENSOR_FAULT_OPEN;
    }

    if (rawSpreadValue > RAW_MAX_SPREAD_COUNTS)
    {
        faultFlags |= SENSOR_FAULT_SIGNAL_NOISY;
    }

    return faultFlags;
}

/**
 * @brief  Converts ADC data to NTC resistance and temperature using the Beta model.
 * @param  rawAdc: Filtered ADC value.
 * @param  resistanceOut: Output pointer for NTC resistance.
 * @param  temperatureOut: Output pointer for temperature.
 * @retval Model, resistance and temperature-range fault bitmask.
 */
static SensorFaultCode ConvertRawToTemperature(uint16_t rawAdc,
    float *resistanceOut,
    float *temperatureOut)
{
    float ntcResistance;
    float inverseTemperature;
    float temperatureCelsius;
    SensorFaultCode faultFlags = SENSOR_FAULT_NONE;

    if ((rawAdc == 0U) || ((float)rawAdc >= ADC_FULL_SCALE))
    {
        return SENSOR_FAULT_MODEL;
    }

    ntcResistance = R_FIXED *
    ((float)rawAdc / (ADC_FULL_SCALE - (float)rawAdc));

    if (!(ntcResistance > 0.0f))
    {
        return SENSOR_FAULT_MODEL;
    }

    if ((ntcResistance < R_NTC_MIN_OHM) || (ntcResistance > R_NTC_MAX_OHM))
    {
        faultFlags |= SENSOR_FAULT_RESISTANCE;
    }

    inverseTemperature = (1.0f / NTC_T25_K)
    + (logf(ntcResistance / NTC_R25) / NTC_BETA);

    if (!(inverseTemperature > 0.0f))
    {
        faultFlags |= SENSOR_FAULT_MODEL;
    }
    else
    {
        temperatureCelsius = (1.0f / inverseTemperature) - 273.15f;

        if (!((temperatureCelsius >= TEMP_MIN_C) && (temperatureCelsius <= TEMP_MAX_C)))
        {
            faultFlags |= SENSOR_FAULT_TEMP_RANGE;
        }

        if (temperatureOut != 0)
        {
            *temperatureOut = temperatureCelsius;
        }
    }

    if (resistanceOut != 0)
    {
        *resistanceOut = ntcResistance;
    }

    return faultFlags;
}

/**
 * @brief  Updates fault assertion and recovery counters for one sensor.
 * @param  channelIndex: Sensor channel index.
 * @param  observedFaultFlags: Faults detected in the current update.
 * @retval None
 */
static void UpdateFaultPersistenceState(uint8_t channelIndex,
    SensorFaultCode observedFaultFlags)
{
    uint8_t index;
    SensorState_t *sensor = &s_sensorState[channelIndex];

    sensor->observedFaults = observedFaultFlags;

    for (index = 0U; index < FAULT_KIND_COUNT; index++)
    {
        SensorFaultCode faultBit = s_faultBitList[index];

        if ((observedFaultFlags & faultBit) != 0U)
        {
            sensor->faultClearCount[index] = 0U;

            if (sensor->faultAssertCount[index] < FAULT_ASSERT_COUNT)
            {
                sensor->faultAssertCount[index]++;
            }

            if (sensor->faultAssertCount[index] >= FAULT_ASSERT_COUNT)
            {
                sensor->latchedFaults |= faultBit;
            }
        }
        else
        {
            sensor->faultAssertCount[index] = 0U;

            if ((sensor->latchedFaults & faultBit) != 0U)
            {
                if (sensor->faultClearCount[index] < FAULT_CLEAR_COUNT)
                {
                    sensor->faultClearCount[index]++;
                }

                if (sensor->faultClearCount[index] >= FAULT_CLEAR_COUNT)
                {
                    sensor->latchedFaults &= (SensorFaultCode)(~faultBit);
                    sensor->faultClearCount[index] = 0U;
                }
            }
            else
            {
                sensor->faultClearCount[index] = 0U;
            }
        }
    }
}

/**
 * @brief  Runs the complete ADC acquisition, conversion, filtering and diagnostics pipeline.
 * @retval None
 */
static void ADC_UpdateSensorData(void)
{
    uint16_t rawAdc[NUMBER_CHANNEL];
    uint16_t rawSpreadValue[NUMBER_CHANNEL];

    float candidateTemperature[NUMBER_CHANNEL];
    float candidateResistance[NUMBER_CHANNEL];
    uint8_t isCandidateValid[NUMBER_CHANNEL];

    SensorFaultCode observedFaultFlags[NUMBER_CHANNEL];

    float peerTemperatures[NUMBER_CHANNEL];
    uint8_t peerCount = 0U;
    float peerMedian = 0.0f;

    uint32_t currentTimeMs = millis();
    uint8_t channelIndex;

    if ((s_isFrameValid != 0U) &&
        ((uint32_t)(currentTimeMs - s_lastUpdateTimeMs) < SENSOR_UPDATE_MS))
    {
        return;
    }

    AcquireFilteredRawSamples(rawAdc, rawSpreadValue);

    for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
    {
        SensorState_t *sensor = &s_sensorState[channelIndex];
        float modelTemperature = 0.0f;
        float calibratedTemperature = 0.0f;
        float calculatedResistance = 0.0f;
        SensorFaultCode faultFlags;

        isCandidateValid[channelIndex] = 0U;
        candidateTemperature[channelIndex] = 0.0f;
        candidateResistance[channelIndex] = 0.0f;

        sensor->filteredRaw = rawAdc[channelIndex];
        sensor->rawSpread = rawSpreadValue[channelIndex];

        faultFlags = DiagnoseRawSignal(rawAdc[channelIndex], rawSpreadValue[channelIndex]);

        if ((faultFlags & (SENSOR_FAULT_SHORT | SENSOR_FAULT_OPEN)) == 0U)
        {
            faultFlags |= ConvertRawToTemperature(rawAdc[channelIndex], &calculatedResistance, &modelTemperature);
            candidateResistance[channelIndex] = calculatedResistance;
            sensor->resistanceOhm = calculatedResistance;

            if ((faultFlags &
                (SENSOR_FAULT_RESISTANCE |
                SENSOR_FAULT_TEMP_RANGE |
                SENSOR_FAULT_MODEL)) == 0U)
            {

                calibratedTemperature =
                    modelTemperature * s_temperatureScale[channelIndex] + s_temperatureOffset[channelIndex];

                if (!((calibratedTemperature >= TEMP_MIN_C) &&
                    (calibratedTemperature <= TEMP_MAX_C)))
                {
                    faultFlags |= SENSOR_FAULT_TEMP_RANGE;
                }
                else
                {
                    if ((sensor->hasLastCandidate != 0U) &&
                        ((uint32_t)(currentTimeMs - sensor->lastCandidateTimeMs) > 0U) &&
                        ((uint32_t)(currentTimeMs - sensor->lastCandidateTimeMs)
                        <= RATE_CHECK_MAX_GAP_MS))
                    {

                        float elapsedSeconds =
                            (float)(currentTimeMs - sensor->lastCandidateTimeMs) / 1000.0f;

                        float temperatureRate =
                            GetAbsoluteValue(calibratedTemperature -
                            sensor->lastCandidateTemperature) / elapsedSeconds;

                        if (temperatureRate > MAX_TEMP_RATE_C_PER_SEC)
                        {
                            faultFlags |= SENSOR_FAULT_RATE;
                        }
                    }

                    candidateTemperature[channelIndex] = calibratedTemperature;
                }
            }
        }
        else
        {
            sensor->hasLastCandidate = 0U;
        }

        observedFaultFlags[channelIndex] = faultFlags;

        if (faultFlags == SENSOR_FAULT_NONE)
        {
            isCandidateValid[channelIndex] = 1U;
            peerTemperatures[peerCount++] = candidateTemperature[channelIndex];
        }
    }

#if CROSS_SENSOR_ENABLE
    if (peerCount >= CROSS_MIN_VALID_SENSORS)
    {
        float sortedPeerTemperatures[NUMBER_CHANNEL];
        uint8_t index;

        for (index = 0U; index < peerCount; index++)
        {
            sortedPeerTemperatures[index] = peerTemperatures[index];
        }

        peerMedian = CalculateMedian(sortedPeerTemperatures, peerCount);

        for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
        {
            if (isCandidateValid[channelIndex] != 0U)
            {
                if (GetAbsoluteValue(candidateTemperature[channelIndex] - peerMedian)
                    > CROSS_MAX_DELTA_C)
                {
                    observedFaultFlags[channelIndex] |= SENSOR_FAULT_CROSS_SENSOR;
                }
            }
        }
    }
#endif

    for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
    {
        SensorState_t *sensor = &s_sensorState[channelIndex];

        UpdateFaultPersistenceState(channelIndex, observedFaultFlags[channelIndex]);

        if (observedFaultFlags[channelIndex] == SENSOR_FAULT_NONE)
        {
            float temperatureValue = candidateTemperature[channelIndex];

            sensor->lastCandidateTemperature = temperatureValue;
            sensor->lastCandidateTimeMs = currentTimeMs;
            sensor->hasLastCandidate = 1U;
            sensor->lastGoodTimeMs = currentTimeMs;

            if (sensor->isInitialized == 0U)
            {
                sensor->filteredTemperature = temperatureValue;
                sensor->isInitialized = 1U;
            }
            else
            {
                sensor->filteredTemperature +=
                    TEMP_EMA_ALPHA * (temperatureValue - sensor->filteredTemperature);
            }
        }
    }

    s_lastUpdateTimeMs = millis();
    s_isFrameValid = 1U;
}

/**
 * @brief  Configures the six ADC sensor pins as analog inputs.
 * @note   Uses PA0, PA1, PA2, PA3, PB0 and PB1.
 * @retval None
 */
void ADC_GPIO_Init(void)
{
    GPIO_InitTypeDef gpioInit = {0};

    RCC_APB2PeriphClockCmd(
        RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB,
        ENABLE
    );

    gpioInit.GPIO_Mode = GPIO_Mode_AIN;

    gpioInit.GPIO_Pin =
        GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_Init(GPIOA, &gpioInit);

    gpioInit.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
    GPIO_Init(GPIOB, &gpioInit);
}

/**
 * @brief  Configures ADC1 scan conversion and DMA circular mode.
 * @note   DMA continuously refreshes g_adcRawBuffer.
 * @retval None
 */
void ADC_DMA_Init(void)
{
    ADC_InitTypeDef adcInit = {0};
    DMA_InitTypeDef dmaInit = {0};
    uint8_t index;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    RCC_ADCCLKConfig(RCC_PCLK2_Div6);

    DMA_DeInit(DMA1_Channel1);

    dmaInit.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->DR;
    dmaInit.DMA_MemoryBaseAddr = (uint32_t)g_adcRawBuffer;
    dmaInit.DMA_DIR = DMA_DIR_PeripheralSRC;
    dmaInit.DMA_BufferSize = NUMBER_CHANNEL;
    dmaInit.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dmaInit.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dmaInit.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dmaInit.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dmaInit.DMA_Mode = DMA_Mode_Circular;
    dmaInit.DMA_Priority = DMA_Priority_High;

    DMA_Init(DMA1_Channel1, &dmaInit);
    DMA_Cmd(DMA1_Channel1, ENABLE);

    ADC_DeInit(ADC1);

    adcInit.ADC_Mode = ADC_Mode_Independent;
    adcInit.ADC_ScanConvMode = ENABLE;
    adcInit.ADC_ContinuousConvMode = ENABLE;
    adcInit.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    adcInit.ADC_DataAlign = ADC_DataAlign_Right;
    adcInit.ADC_NbrOfChannel = NUMBER_CHANNEL;

    ADC_Init(ADC1, &adcInit);

    for (index = 0U; index < NUMBER_CHANNEL; index++)
    {
        ADC_RegularChannelConfig(
            ADC1,
            s_adcChannelList[index],
            index + 1U,
            ADC_SampleTime_55Cycles5
        );
    }

    ADC_DMACmd(ADC1, ENABLE);
    ADC_Cmd(ADC1, ENABLE);

    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1))
    {
    }

    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1))
    {
    }

    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

/**
 * @brief  Returns the filtered temperature of one sensor channel.
 * @param  channelIndex: Sensor channel index.
 * @retval Temperature in degrees Celsius, or TEMP_ERROR_VALUE on fault.
 */
float ADC_ReadTempFiltered(uint8_t channelIndex)
{
    if (channelIndex >= NUMBER_CHANNEL)
    {
        return TEMP_ERROR_VALUE;
    }

    ADC_UpdateSensorData();

    if ((s_sensorState[channelIndex].isInitialized == 0U) ||
        (s_sensorState[channelIndex].latchedFaults != SENSOR_FAULT_NONE))
    {
        return TEMP_ERROR_VALUE;
    }

    return s_sensorState[channelIndex].filteredTemperature;
}

/**
 * @brief  Calculates the average temperature from all healthy sensors.
 * @retval Average temperature in degrees Celsius, or TEMP_ERROR_VALUE if no sensor is valid.
 */
float ADC_GetStableAverageTemp(void)
{
    float sum = 0.0f;
    uint8_t count = 0U;
    uint8_t channelIndex;

    ADC_UpdateSensorData();

    for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
    {
        if ((s_sensorState[channelIndex].isInitialized != 0U) &&
            (s_sensorState[channelIndex].latchedFaults == SENSOR_FAULT_NONE))
        {
            sum += s_sensorState[channelIndex].filteredTemperature;
            count++;
        }
    }

    if (count == 0U)
    {
        return TEMP_ERROR_VALUE;
    }

    return sum / (float)count;
}

/**
 * @brief  Returns one latched-fault status faultBit per sensor channel.
 * @retval Sensor fault bitmask.
 */
uint8_t ADC_GetStatusBitmask(void)
{
    uint8_t statusMask = 0U;
    uint8_t channelIndex;

    ADC_UpdateSensorData();

    for (channelIndex = 0U; channelIndex < NUMBER_CHANNEL; channelIndex++)
    {
        if ((s_sensorState[channelIndex].isInitialized == 0U) ||
            (s_sensorState[channelIndex].latchedFaults != SENSOR_FAULT_NONE))
        {
            statusMask |= (uint8_t)(1U << channelIndex);
        }
    }

    return statusMask;
}

/**
 * @brief  Returns persistent fault flags for one sensor channel.
 * @param  channelIndex: Sensor channel index.
 * @retval Latched SensorFaultCode bitmask.
 */
SensorFaultCode ADC_GetSensorFaultCode(uint8_t channelIndex)
{
    if (channelIndex >= NUMBER_CHANNEL)
    {
        return SENSOR_FAULT_MODEL;
    }

    ADC_UpdateSensorData();
    return s_sensorState[channelIndex].latchedFaults;
}

/**
 * @brief  Returns fault flags detected in the latest sample.
 * @param  channelIndex: Sensor channel index.
 * @retval Observed SensorFaultCode bitmask.
 */
SensorFaultCode ADC_GetObservedFaultCode(uint8_t channelIndex)
{
    if (channelIndex >= NUMBER_CHANNEL)
    {
        return SENSOR_FAULT_MODEL;
    }

    ADC_UpdateSensorData();
    return s_sensorState[channelIndex].observedFaults;
}

/**
 * @brief  Returns the latest filtered raw ADC value for one sensor channel.
 * @param  channelIndex: Sensor channel index.
 * @retval Filtered ADC value, or 0 for an invalid channel.
 */
uint16_t ADC_GetSensorRaw(uint8_t channelIndex)
{
    if (channelIndex >= NUMBER_CHANNEL)
    {
        return 0U;
    }

    ADC_UpdateSensorData();
    return s_sensorState[channelIndex].filteredRaw;
}

/**
 * @brief  Returns the latest calculated NTC resistance.
 * @param  channelIndex: Sensor channel index.
 * @retval Resistance in ohms, or 0.0 for an invalid channel.
 */
float ADC_GetSensorResistance(uint8_t channelIndex)
{
    if (channelIndex >= NUMBER_CHANNEL)
    {
        return 0.0f;
    }

    ADC_UpdateSensorData();
    return s_sensorState[channelIndex].resistanceOhm;
}
