# IoT System Documentation

This directory contains project-level documentation and media. Detailed implementation documentation stays close to the subsystem that owns it.

## Start Here

| Resource | Purpose |
|---|---|
| [Project README](../README.md) | Portfolio-oriented overview, architecture, reliability, quick start, and demo |
| [System Technical Reference](system-reference.md) | Detailed end-to-end protocol, reliability, configuration, runtime, recovery, verification, and troubleshooting |
| [Node 01](../node01/README.md) | STM32 internal-ADC sensing node |
| [Node 02](../node02/README.md) | STM32 internal-ADC sensing node |
| [Node 03](../node03/README.md) | STM32 MCP3208 sensing node |
| [ESP32 Gateway](../gateway/README.md) | LoRa polling, durable LittleFS queue, and MQTT QoS 1 |
| [MQTT Broker](../broker/README.md) | Mosquitto authentication and persistence |
| [Backend](../backend/README.md) | Durable ingestion, InfluxDB, and REST API |
| [Frontend](../frontend/README.md) | React dashboard and visualization behavior |

## Media

- `images/` — hardware photos and README/dashboard imagery.
- `videos/` — desktop and mobile dashboard demonstrations.

## Documentation Principle

The root README is intentionally short enough to explain the project quickly. Deep implementation details live in the system reference or the README of the component responsible for that behavior.
