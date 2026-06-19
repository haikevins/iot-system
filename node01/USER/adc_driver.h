#ifndef __ADC_DRIVER_H__
#define __ADC_DRIVER_H__

#include <stm32f10x.h>
#include <math.h>

// ===== CONFIG =====
#define NUMBER_CHANNEL 6

#define VREF               3.3f
#define ADC_MAX            4095.0f

#define R_SERIES           10000.0f
#define R0                 10000.0f
#define BETA               3950.0f
#define T0                 298.15f

// ===== FILTER =====
#define SAMPLE_COUNT       7
#define MOVING_AVG_SIZE    5

// ===== VALID RANGE =====
#define ADC_MIN_VALID      10
#define ADC_MAX_VALID      4090

// ===== CALIB =====
#define TEMP_OFFSET        0.5f
#define TEMP_SCALE         1.0f

#define TEMP_ERROR_VALUE   -1000.0f

// ===== STATUS =====
#define SENSOR_OK          0
#define SENSOR_JUMP        1
#define SENSOR_STUCK       2
#define SENSOR_ERROR       255

// ===== VALIDATION =====
#define TEMP_JUMP_THRESHOLD     5.0f
#define STUCK_TIME_MS           300000
#define MIN_UPDATE_INTERVAL_MS  1000

// ===== GLOBAL =====
extern volatile uint16_t adc_buffer[NUMBER_CHANNEL];

// ===== API =====
void ADC_GPIO_Init(void);
void ADC_DMA_Init(void);

float ADC_ReadTempFiltered(uint8_t ch);
float ADC_GetStableAverageTemp(void);
uint8_t ADC_GetStatusBitmask(void);

#endif