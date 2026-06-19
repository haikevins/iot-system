#ifndef __MCP3208_H__
#define __MCP3208_H__

#include <stm32f10x.h>

// ===== SYSTEM CONFIG =====
#define NUMBER_CHANNEL     6

// ===== ADC CONFIG =====
#define VREF               3.3f
#define ADC_MAX            4095.0f

// ===== NTC CONFIG =====
#define R_FIXED            10000.0f
#define R0                 10000.0f
#define BETA               3950.0f
#define T0                 298.15f   // Kelvin

#define OPAMP_GAIN         1.15f

// ===== FILTER CONFIG =====
#define SAMPLE_COUNT       7
#define MOVING_AVG_SIZE    5

#define ADC_MIN_VALID      10
#define ADC_MAX_VALID      4090

// ===== CALIBRATION =====
#define TEMP_OFFSET        0.5f
#define TEMP_SCALE         1.0f

#define TEMP_ERROR_VALUE   -1000.0f

// ===== SENSOR STATUS =====
#define SENSOR_OK          0
#define SENSOR_JUMP        1
#define SENSOR_STUCK       2
#define SENSOR_ERROR       255

// ===== VALIDATION CONFIG =====
#define TEMP_JUMP_THRESHOLD     5.0f
#define STUCK_TIME_MS           300000   // 5 phút
#define MIN_UPDATE_INTERVAL_MS  1000     // 1s

// ===== GPIO =====
#define MCP3208_CS_PORT    GPIOB
#define MCP3208_CS_PIN     GPIO_Pin_0

// ===== API =====
void MCP3208_Init(void);
float MCP3208_ReadTempFiltered(uint8_t channel);
float MCP3208_GetStableAverageTemp(void);
uint8_t MCP3208_GetStatusBitmask(void);
uint16_t MCP3208_ReadFilteredADC(uint8_t channel);

#endif