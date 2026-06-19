#include "mcp3208.h"
#include "spi_driver.h"
#include <math.h>

// ===== MOVING AVERAGE =====
static float ma_buffer[NUMBER_CHANNEL][MOVING_AVG_SIZE];
static float ma_sum[NUMBER_CHANNEL];
static uint8_t ma_index[NUMBER_CHANNEL];
static uint8_t ma_count[NUMBER_CHANNEL];

// ===== SENSOR STATE =====
typedef struct {
    float last_temp;
    uint32_t last_update_time;
    uint32_t last_change_time;
    uint8_t error_flag;
} SensorState;

static SensorState sensor_state[NUMBER_CHANNEL];

// ===== SORT =====
static void sort(uint16_t *arr, uint8_t n) {
    for (uint8_t i = 0; i < n - 1; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if (arr[i] > arr[j]) {
                uint16_t tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
    }
}

/*
    Tinh trung binh dong cua cac gia tri nhiet do
*/
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

// ===== VALIDATE =====
static float ValidateTemp(uint8_t channel, float temp) {

    uint32_t now = millis();
    SensorState *s = &sensor_state[channel];

    if (s->last_update_time == 0) {
        s->last_temp = temp;
        s->last_update_time = now;
        s->last_change_time = now;
        s->error_flag = SENSOR_OK;
        return temp;
    }

    // jump detect
    if (fabs(temp - s->last_temp) > TEMP_JUMP_THRESHOLD) {
        s->error_flag = SENSOR_JUMP;
        return s->last_temp;
    }

    // stuck detect
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

/*
    Select MCP3208
*/
static void MCP3208_Select(void) {
    GPIO_ResetBits(MCP3208_CS_PORT, MCP3208_CS_PIN);
}

/*
    De-select MCP3208
*/
static void MCP3208_Unselect(void) {
    GPIO_SetBits(MCP3208_CS_PORT, MCP3208_CS_PIN);
}

/*
    Khoi tao MCP3208
*/
void MCP3208_Init(void) {

    SPI_GPIO_Init();

    GPIO_InitTypeDef GPIO_InitStructure = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin = MCP3208_CS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

    GPIO_Init(MCP3208_CS_PORT, &GPIO_InitStructure);

    MCP3208_Unselect();
}

/*
    Doc kenh ADC tu MCP3208
*/
static uint16_t MCP3208_ReadChannel(uint8_t channel) {

    if (channel >= NUMBER_CHANNEL) return 0;

    uint8_t high, low;

    MCP3208_Select();

    SPI1_Transfer(0x06 | ((channel & 0x04) >> 2)); /* chon kenh ADC */
    high = SPI1_Transfer((channel & 0x03) << 6); /* doc 4 bit cao */
    low  = SPI1_Transfer(0x00); /* doc 8 bit thap */

    MCP3208_Unselect();

    return ((high & 0x0F) << 8) | low; /* ket hop 12 bit */
}

/*
    Chuyen doi gia tri ADC sang nhiet do
     - adc: gia tri doc tu MCP3208 (0-4095)
     - tra ve: nhiet do tinh duoc, neu co loi tra ve TEMP_ERROR_VALUE
*/
static float MCP3208_ReadTemp(uint16_t adc) {

    if (adc == 0) return TEMP_ERROR_VALUE;

    float vout = ((float)adc / ADC_MAX) * VREF;
    float vtemp = vout / OPAMP_GAIN;

    if (vtemp <= 0.001f || vtemp >= (VREF - 0.001f))
        return TEMP_ERROR_VALUE;

    float r_ntc = R_FIXED * (vtemp / (VREF - vtemp));

    if (r_ntc <= 0.0f)
        return TEMP_ERROR_VALUE;

    float tempK = 1.0f / ((1.0f / T0) + (1.0f / BETA) * log(r_ntc / R0));
    float tempC = tempK - 273.15f;

    if (!isfinite(tempC))
        return TEMP_ERROR_VALUE;

    return tempC;
}

/*
    Doc SAMPLE_COUNT mau tu kenh ADC va tra ve gia tri trung vi
*/
uint16_t MCP3208_ReadFilteredADC(uint8_t channel) {

    uint16_t samples[SAMPLE_COUNT];
    uint8_t valid_count = 0;

    for (uint8_t i = 0; i < SAMPLE_COUNT; i++) {

        uint16_t val = MCP3208_ReadChannel(channel);

        /*
            Chi nhan mau hop le
        */
        if (val > ADC_MIN_VALID && val < ADC_MAX_VALID) {
            samples[valid_count++] = val;
        }
    }

    /*
        Neu nho hon 3 mau hop le thi coi nhu khong co du lieu, tra ve 0 de bao loi
    */
    if (valid_count < 3) {
        return 0;
    }

    /*
        Sap xep va tra ve gia tri trung vi
    */
    sort(samples, valid_count);

    /*
        Tra ve gia tri trung vi
    */
    return samples[valid_count / 2];
}

/*
    Ham doc nhiet do da loc tu kenh ADC
*/
float MCP3208_ReadTempFiltered(uint8_t channel) {

    if (channel >= NUMBER_CHANNEL)
        return TEMP_ERROR_VALUE;

    uint32_t now = millis();

    if ((now - sensor_state[channel].last_update_time) < MIN_UPDATE_INTERVAL_MS) {
        return sensor_state[channel].last_temp;
    }

    uint16_t adc = MCP3208_ReadFilteredADC(channel);

    if (adc == 0) {
        sensor_state[channel].error_flag = SENSOR_ERROR;
        return TEMP_ERROR_VALUE;
    }

    float temp = MCP3208_ReadTemp(adc);

    if (temp == TEMP_ERROR_VALUE) {
        sensor_state[channel].error_flag = SENSOR_ERROR;
        return TEMP_ERROR_VALUE;
    }

    temp = MovingAverage(channel, temp);

    temp = temp * TEMP_SCALE + TEMP_OFFSET;

    temp = ValidateTemp(channel, temp);

    return temp;
}

// ===== STATUS =====
uint8_t MCP3208_GetSensorStatus(uint8_t channel) {

    if (channel >= NUMBER_CHANNEL)
        return SENSOR_ERROR;

    return sensor_state[channel].error_flag;
}

float MCP3208_GetStableAverageTemp(void)
{
    float sum = 0.0f;
    uint8_t count = 0;

    for (uint8_t ch = 0; ch < NUMBER_CHANNEL; ch++) {
				
        float temp = MCP3208_ReadTempFiltered(ch);
        uint8_t status = MCP3208_GetSensorStatus(ch);

        if (status == SENSOR_OK && temp != TEMP_ERROR_VALUE) {
            sum += temp;
            count++;
        }
    }

    if (count == 0) {
        return TEMP_ERROR_VALUE;
    }

    return sum / count;
}

uint8_t MCP3208_GetStatusBitmask(void)
{
    uint8_t mask = 0;

    for (uint8_t ch = 0; ch < NUMBER_CHANNEL; ch++)
    {
        uint8_t st = MCP3208_GetSensorStatus(ch);

        if (st != SENSOR_OK)
        {
            mask |= (1 << ch);
        }
    }

    return mask;
}