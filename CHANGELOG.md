# Changelog

All notable project-level changes are documented in this file.

The project follows [Semantic Versioning](https://semver.org/) for stable releases.

## [1.0.0] - 2026-10-18

First stable portfolio release of the end-to-end IoT monitoring system.

### Highlights

- Three STM32F103 sensor nodes with six temperature channels per node.
- SX1278 LoRa transport with CRC, sequence numbers, retry, acknowledgement, and duplicate handling.
- ESP32 gateway with persist-before-ACK LittleFS buffering and MQTT QoS 1 delivery.
- Mosquitto broker configuration with authentication and persistence.
- Node.js ingestion service with a durable filesystem spool and InfluxDB storage.
- React dashboard for live state, history, RSSI, freshness, and sensor diagnostics.
- Project-level documentation, contribution/security guidance, and automated CI verification.

### Verification

The repository CI verifies four gates:

- Repository Hygiene
- Backend
- Frontend
- Gateway

STM32 firmware remains verified with the Keil MDK projects included in each node directory.

### Known limitations

- STM32 builds are not automated in GitHub Actions because the current projects target Keil MDK.
- Deployment credentials are intentionally local-only and are not included in the repository or release assets.
- The project does not provide OTA firmware updates or production user authentication.

[1.0.0]: https://github.com/haikevins/iot-system/releases/tag/v1.0.0
