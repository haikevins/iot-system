#include "adc_driver.h"
#include "systick_utils.h"
#include <math.h>

// ================= BUFFER =================
volatile uint16_t adc_buffer[NUMBER_CHANNEL];

// ================= CHANNEL MAP =================
static uint8_t adc_channels[NUMBER_CHANNEL] = {0,1,2,3,8,9};

// ================= MEDIAN BUFFER =================
static uint16_t sample_buffer[NUMBER_CHANNEL][SAMPLE_COUNT];
static uint8_t sample_index[NUMBER_CHANNEL];

// ================= MOVING AVG =================
static float ma_buffer[NUMBER_CHANNEL][MOVING_AVG_SIZE];
static float ma_sum[NUMBER_CHANNEL];
static uint8_t ma_index[NUMBER_CHANNEL];
static uint8_t ma_count[NUMBER_CHANNEL];

// ================= SENSOR STATE =================
typedef struct {
    float last_temp;
    uint32_t last_update_time;
    uint32_t last_change_time;
    uint8_t error_flag;
} SensorState;

static SensorState sensor_state[NUMBER_CHANNEL];

// ================= SORT =================
static void sort(uint16_t *arr, uint8_t n) {
    for (uint8_t i = 0; i < n - 1; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if (arr[i] > arr[j]) {
                uint16_t t = arr[i];
                arr[i] = arr[j];
                arr[j] = t;
            }
        }
    }
}

// ================= GPIO INIT =================
void ADC_GPIO_Init(void) {

    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;

    GPIO_InitStructure.GPIO_Pin =
        GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
}

// ================= DMA + ADC INIT =================
void ADC_DMA_Init(void) {

    ADC_InitTypeDef ADC_InitStructure = {0};
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    // ===== DMA =====
    DMA_DeInit(DMA1_Channel1);
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->DR;
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)adc_buffer;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize = NUMBER_CHANNEL;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;

    DMA_Init(DMA1_Channel1, &DMA_InitStructure);
    DMA_Cmd(DMA1_Channel1, ENABLE);

    // ===== ADC =====
    ADC_DeInit(ADC1);

    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = ENABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = ENABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel = NUMBER_CHANNEL;

    ADC_Init(ADC1, &ADC_InitStructure);

    for (int i = 0; i < NUMBER_CHANNEL; i++) {
        ADC_RegularChannelConfig(
            ADC1,
            adc_channels[i],
            i + 1,
            ADC_SampleTime_55Cycles5
        );
    }

    ADC_DMACmd(ADC1, ENABLE);
    ADC_Cmd(ADC1, ENABLE);

    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1));

    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1));

    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

// ================= MOVING AVG =================
static float MovingAverage(uint8_t channel, float new_val) {

    if (channel >= NUMBER_CHANNEL) return new_val;

    ma_sum[channel] -= ma_buffer[channel][ma_index[channel]];
    ma_buffer[channel][ma_index[channel]] = new_val;
    ma_sum[channel] += new_val;

    if (++ma_index[channel] >= MOVING_AVG_SIZE)
        ma_index[channel] = 0;

    if (ma_count[channel] < MOVING_AVG_SIZE)
        ma_count[channel]++;

    return ma_sum[channel] / ma_count[channel];
}

// ================= VALIDATE =================
static float ValidateTemp(uint8_t ch, float temp) {

    uint32_t now = millis();
    SensorState *s = &sensor_state[ch];

    if (s->last_update_time == 0) {
        s->last_temp = temp;
        s->last_update_time = now;
        s->last_change_time = now;
        s->error_flag = SENSOR_OK;
        return temp;
    }

    if (fabs(temp - s->last_temp) > TEMP_JUMP_THRESHOLD) {
        s->error_flag = SENSOR_JUMP;
        return s->last_temp;
    }

    if (fabs(temp - s->last_temp) < 0.05f) {
        if ((now - s->last_change_time) > STUCK_TIME_MS) {
            s->error_flag = SENSOR_STUCK;
            return TEMP_ERROR_VALUE;
        }
    } else {
        s->last_change_time = now;
    }

    s->last_temp = temp;
    s->last_update_time = now;
    s->error_flag = SENSOR_OK;

    return temp;
}

// ================= CONVERT =================
static float ADC_Convert(uint16_t adc) {

    if (adc == 0) return TEMP_ERROR_VALUE;

    float v = ((float)adc / ADC_MAX) * VREF;

    if (v <= 0.001f || v >= (VREF - 0.001f))
        return TEMP_ERROR_VALUE;

    float r = R_SERIES * v / (VREF - v);

    float tempK = 1.0f / ((1.0f / T0) + (1.0f / BETA) * log(r / R0));

    return tempK - 273.15f;
}

// ================= MEDIAN =================
static uint16_t GetMedian(uint8_t ch) {

    sample_buffer[ch][sample_index[ch]] = adc_buffer[ch];

    if (++sample_index[ch] >= SAMPLE_COUNT)
        sample_index[ch] = 0;

    uint16_t tmp[SAMPLE_COUNT];
    for (int i = 0; i < SAMPLE_COUNT; i++)
        tmp[i] = sample_buffer[ch][i];

    sort(tmp, SAMPLE_COUNT);

    return tmp[SAMPLE_COUNT / 2];
}

// ================= MAIN FILTER =================
float ADC_ReadTempFiltered(uint8_t ch) {

    if (ch >= NUMBER_CHANNEL)
        return TEMP_ERROR_VALUE;

    uint32_t now = millis();

    if ((now - sensor_state[ch].last_update_time) < MIN_UPDATE_INTERVAL_MS)
        return sensor_state[ch].last_temp;

    uint16_t adc = GetMedian(ch);

    if (adc < ADC_MIN_VALID || adc > ADC_MAX_VALID) {
        sensor_state[ch].error_flag = SENSOR_ERROR;
        return TEMP_ERROR_VALUE;
    }

    float temp = ADC_Convert(adc);

    temp = MovingAverage(ch, temp);
    temp = temp * TEMP_SCALE + TEMP_OFFSET;
    temp = ValidateTemp(ch, temp);

    return temp;
}

// ================= AVERAGE =================
float ADC_GetStableAverageTemp(void) {

    float sum = 0;
    uint8_t cnt = 0;

    for (uint8_t i = 0; i < NUMBER_CHANNEL; i++) {

        float t = ADC_ReadTempFiltered(i);

        if (sensor_state[i].error_flag == SENSOR_OK &&
            t != TEMP_ERROR_VALUE) {
            sum += t;
            cnt++;
        }
    }

    if (cnt == 0) return TEMP_ERROR_VALUE;

    return sum / cnt;
}

// ================= STATUS =================
uint8_t ADC_GetStatusBitmask(void) {

    uint8_t mask = 0;

    for (uint8_t i = 0; i < NUMBER_CHANNEL; i++) {
        if (sensor_state[i].error_flag != SENSOR_OK)
            mask |= (1 << i);
    }

    return mask;
}