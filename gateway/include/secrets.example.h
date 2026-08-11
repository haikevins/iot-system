#pragma once

#include <stdint.h>

/*
 * Local gateway configuration template.
 *
 * Copy this file to gateway/include/secrets.h and replace the placeholder
 * values. secrets.h is intentionally excluded from Git.
 */
static constexpr char WIFI_SSID[] = "YOUR_WIFI_SSID";
static constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

static constexpr char MQTT_BROKER[] = "192.168.1.100";
static constexpr uint16_t MQTT_PORT = 1883U;
static constexpr char MQTT_USER[] = "gateway-01";
static constexpr char MQTT_PASSWORD[] = "YOUR_MQTT_PASSWORD";
static constexpr char MQTT_CLIENT_ID[] = "esp32-gateway-01";
