# IoT System — Node 03

> STM32F103 LoRa temperature node at address `0x03`, responsible for acquiring six NTC channels through MCP3208 External ADC, applying filtering and diagnostics, and responding reliably to gateway POLL transactions.

***

## Table of Contents

1. [Overview](#overview)
2. [Hardware](#hardware)
3. [Source Structure](#source-structure)
4. [Build Configuration](#build-configuration)
5. [Main Runtime Flow](#main-runtime-flow)
6. [LoRa Protocol](#lora-protocol)
7. [DATA Payload](#data-payload)
8. [Sensor Acquisition](#sensor-acquisition)
9. [Temperature Conversion](#temperature-conversion)
10. [Filtering](#filtering)
11. [Sensor Diagnostics](#sensor-diagnostics)
12. [Fault Persistence](#fault-persistence)
13. [Cross-Sensor Validation](#cross-sensor-validation)
14. [Timestamp and Sample Age](#timestamp-and-sample-age)
15. [Retry and ACK Logic](#retry-and-ack-logic)
16. [Debug Counters](#debug-counters)
17. [Build and Flash](#build-and-flash)
18. [Verification](#verification)
19. [Troubleshooting](#troubleshooting)
20. [Known Limitations](#known-limitations)
21. [Future Improvements](#future-improvements)

***

## Overview

Node 03 is one of three remote acquisition nodes.

```text
6 × NTC
   ↓
MCP3208 External ADC
   ↓
filter + diagnostics
   ↓
average healthy temperature
   ↓
13-byte DATA payload
   ↓
SX1278 433 MHz
   ↓
ESP32 Gateway
```

Node identity:

```text
Repository directory : node03/
LoRa address          : 0x03
Packet DATA type      : 0x01
Packet ACK type       : 0x10
Packet POLL type      : 0x23
```

The node does not transmit continuously.

It waits for a gateway POLL:

```text
Gateway POLL
    ↓
Node acquisition
    ↓
Node DATA
    ↓
Gateway ACK
```

***

## Hardware

### MCU

```text
STM32F103
```

Firmware uses STM32 Standard Peripheral Library-style APIs rather than HAL.


### MCP3208

Node 03 reads six channels:

```text
CH0
CH1
CH2
CH3
CH4
CH5
```

Chip select:

```text
PB0 → MCP3208 CS
```

The ADC and SX1278 share SPI1 with separate chip-select lines.

Analog front-end software compensation:

```text
OPAMP_GAIN = 1.15
```

The firmware divides the measured ADC ratio by this gain before reconstructing NTC resistance.


### SX1278

```text
Frequency          433 MHz
Spreading factor   SF7
Bandwidth          125 kHz
Coding rate        4/5
Preamble           8
Payload CRC        enabled
```


### SPI1

| Signal | STM32 pin |
|---|---|
| SCK | PA5 |
| MISO | PA6 |
| MOSI | PA7 |

SPI mode:

```text
Master
8-bit
CPOL low
CPHA first edge
MSB first
Prescaler 32
```

### Chip selects

| Peripheral | CS |
|---|---|
| SX1278 | PA4 |
| MCP3208 | PB0 |

SX1278 RESET:

```text
PB10
```


### NTC model

```text
Nominal resistance : 10 kΩ @ 25 °C
Beta                : 3950 K
Fixed resistor      : 10 kΩ
```

***

## Source Structure

```text
node03/
├── README.md
├── MDK/
│   ├── Node_3.uvprojx
│   └── ...
└── USER/
    ├── main.c
    ├── mcp3208.c/.h
    ├── spi_driver.c/.h
    ├── sx1278.c/.h
    ├── systick_driver.c/.h
    └── systick_utils.c/.h
```

Responsibilities:

| File | Responsibility |
|---|---|
| `main.c` | POLL/DATA/ACK transaction and payload encoding |
| `mcp3208.c/.h` | sensor acquisition, NTC conversion, diagnostics |
| `spi_driver.c/.h` | SPI1 byte transfer |
| `sx1278.c/.h` | SX1278 setup and application packet framing |
| `systick_driver.c/.h` | SysTick initialization |
| `systick_utils.c/.h` | millisecond timing utilities |

***

## Build Configuration

Keil project:

```text
node03/MDK/Node_3.uvprojx
```

### Important optimization requirement

Use:

```text
Options for Target
→ C/C++
→ Optimization
→ O1
```

A practical timing issue has been observed in this project:

```text
O0 → LoRa transaction timeout/failure observed
O1 → known-good operation
```

Therefore O1 should be treated as part of the verified build configuration, not merely a performance preference.

### HEX output

If a HEX file is required:

```text
Options for Target
→ Output
→ Create HEX File
```

***

## Main Runtime Flow

`main()` initializes:

```text
SysTick
SX1278
MCP3208
```

Then loops forever:

```text
wait for LoRa frame
    ↓
is ADDR == 0x03?
    ↓ yes
is TYPE == POLL?
    ↓ yes
is LEN == 0?
    ↓ yes
acquire stable temperature
    ↓
capture measurement midpoint
    ↓
read status mask
    ↓
read six detailed faults
    ↓
encode temperature ×100
    ↓
build 13-byte payload
    ↓
send DATA
    ↓
wait matching ACK
    ↓
retry if necessary
```

Unexpected packets are ignored.

***

## LoRa Protocol

Application frame:

```text
START | ADDR | TYPE | SEQ | LEN | DATA | CRC8
```

Node address:

```text
ADDR = 0x03
```

Start byte:

```text
0xAA
```

CRC-8:

```text
polynomial = 0x07
initial    = 0x00
```

CRC coverage:

```text
ADDR | TYPE | SEQ | LEN | DATA
```

### SX1278 receive validation

The driver verifies:

- RX done state,
- radio payload CRC status,
- frame length,
- start byte,
- LEN consistency,
- application CRC-8.

The radio payload CRC and application CRC-8 are separate integrity layers.

***

## DATA Payload

Current payload:

```text
TEMP_L | TEMP_H | STATUS | FAULT_S1..FAULT_S6 | SAMPLE_AGE_MS_LE32
```

Size:

```text
2 + 1 + 6 + 4 = 13 bytes
```

Offsets:

| Byte | Field |
|---:|---|
| 0 | Temperature LSB |
| 1 | Temperature MSB |
| 2 | Sensor summary status |
| 3 | Sensor 1 fault |
| 4 | Sensor 2 fault |
| 5 | Sensor 3 fault |
| 6 | Sensor 4 fault |
| 7 | Sensor 5 fault |
| 8 | Sensor 6 fault |
| 9 | Sample age byte 0 |
| 10 | Sample age byte 1 |
| 11 | Sample age byte 2 |
| 12 | Sample age byte 3 |

Temperature encoding:

```text
centiCelsius = round(temperatureC × 100)
```

Invalid marker:

```text
-32768
```

Example:

```text
31.82 °C
→ 3182
→ int16 little-endian
```

`STATUS` bits:

```text
bit 0 sensor 1 has persistent fault
...
bit 5 sensor 6 has persistent fault
```

***

## Sensor Acquisition


Node 03 does not use STM32 ADC1 for these six temperature channels.

For each MCP3208 channel:

```text
read 16 samples
   ↓
sort
   ↓
discard 4 lowest
discard 4 highest
   ↓
average middle 8
```

Constants:

```text
RAW_SAMPLE_COUNT      = 16
RAW_TRIM_COUNT        = 4
RAW_SAMPLE_PERIOD_MS  = 1
ADC_FULL_SCALE        = 4095
```

The acquisition midpoint is stored and later used for sample-age calculation.


### Sensor update period

Diagnostic/filter state uses:

```text
SENSOR_UPDATE_MS = 1000 ms
```

***

## Temperature Conversion


The external analog stage has gain, so raw ADC ratio is compensated:

```text
dividerRatio = (raw / 4095) / 1.15
```

Then:

```text
R_NTC = R_FIXED × dividerRatio / (1 - dividerRatio)
```

with:

```text
R_FIXED = 10000 Ω
```

Temperature uses the Beta model:

```text
1/T = 1/298.15 + ln(R_NTC/10000) / 3950
T_C = 1/(...) - 273.15
```

Model constants:

```text
NTC_R25   = 10000 Ω
NTC_BETA  = 3950
NTC_T25_K = 298.15 K
```


### Per-channel calibration

Firmware contains scale and offset arrays.

Current defaults:

```text
scale  = 1.0
offset = 0.0 °C
```

Conceptually:

```text
T_calibrated = T_model × scale + offset
```

These values should be calibrated against real reference measurements for production accuracy.

***

## Filtering

The node uses multiple filtering layers for different purposes.

### 1. Trimmed mean

Protects raw ADC acquisition against transient outliers.

```text
16 → trim 8 extremes → average 8
```

### 2. EMA

Valid temperature is smoothed with:

```text
TEMP_EMA_ALPHA = 0.25
```

Concept:

```text
filtered = alpha × new + (1-alpha) × previous
```

### 3. Fault persistence

Diagnostic decisions are debounced over multiple update cycles rather than being exposed instantly.

This is a state filter for fault status rather than a signal filter.

***

## Sensor Diagnostics


| Bit | Name | Trigger category |
|---:|---|---|
| `0x01` | SHORT | ADC near ground |
| `0x02` | HIGH_SAT | ADC near full scale / analog saturation |
| `0x04` | SIGNAL_NOISY | raw spread too large |
| `0x08` | RESISTANCE | calculated NTC resistance out of range |
| `0x10` | TEMP_RANGE | calculated temperature out of range |
| `0x20` | RATE | temperature changes too fast |
| `0x40` | CROSS_SENSOR | differs too much from peer sensors |
| `0x80` | MODEL | conversion/model invalid |

Thresholds:

```text
SHORT              <= 20 counts
HIGH_SAT           >= 4075 counts
MAX RAW SPREAD     > 100 counts
R_NTC range        250 Ω .. 60 kΩ
Temperature        -10 .. 125 °C
Max rate           10 °C/s
Rate max gap       3000 ms
Cross delta        12 °C
Cross min valid    4 sensors
```


### Why multiple diagnostics?

A plausible temperature number can still be wrong.

Examples:

- an open wire can approach ADC full-scale,
- noise can produce a plausible average but large spread,
- a model calculation can become invalid,
- one sensor can disagree strongly with five nearby sensors,
- a sensor can jump faster than physically expected.

The driver therefore validates electrical, mathematical and system-level consistency.

***

## Fault Persistence

Raw observed fault:

```text
current acquisition
```

Persistent/latched fault:

```text
state used by telemetry
```

Rules:

```text
3 consecutive bad observations
    → assert fault

5 consecutive good observations
    → clear fault
```

Constants:

```text
FAULT_ASSERT_COUNT = 3
FAULT_CLEAR_COUNT  = 5
```

This hysteresis reduces UI/status flicker.

### Driver API concept

Relevant functions include names in the `MCP3208_...` family for:

- stable average temperature,
- status bitmask,
- persistent sensor fault code,
- latest observed fault code,
- resistance/raw diagnostics,
- last measurement timestamp.

***

## Cross-Sensor Validation

Enabled:

```text
CROSS_SENSOR_ENABLE = 1
```

Requirements:

```text
minimum valid peers = 4
maximum delta       = 12 °C
```

The algorithm compares sensors against a peer/central reference so one channel that drifts far away from the group can be marked suspicious even if its electrical readings remain individually plausible.

Cross-sensor diagnosis should be used only when the sensors are expected to measure comparable thermal conditions.

If future deployment places sensors in intentionally different temperature zones, this rule must be redesigned or grouped by location.

***

## Timestamp and Sample Age

The acquisition driver stores the midpoint time of the measurement burst:

```text
measurementTimeMs
```

Before **every DATA retry**, `main.c` computes:

```text
sampleAgeMs = millis() - measurementTimeMs
```

Then encodes it as little-endian 32-bit data.

Why recompute on each retry?

```text
measurement occurs
DATA attempt 1
ACK timeout
DATA attempt 2
```

Attempt 2 is older than attempt 1. Sending the original age would under-report sample age.

The gateway uses this field to reconstruct the original sample timestamp even if the record spends additional time:

- in radio retry,
- in LittleFS,
- waiting for Wi-Fi,
- waiting for MQTT.

***

## Retry and ACK Logic

Timing:

```text
NODE_RECEIVE_TIMEOUT_MS      = 2000
DATA_TRANSMIT_TIMEOUT_MS     = 1000
ACK_WAIT_TIMEOUT_MS          = 500
DATA_TRANSMIT_MAX_ATTEMPTS   = 3
```

Transaction:

```text
POLL seq=42
   ↓
DATA seq=42 attempt 1
   ↓
wait ACK addr=0x03, seq=42
   ↓ timeout
DATA seq=42 attempt 2
   ↓
ACK seq=42
   ↓
success
```

ACK acceptance requires:

```text
packet.addr == 0x03
packet.type == ACK
packet.seq  == current sequence
packet.len  == 0
```

An ACK for another node or sequence is ignored.

***

## Debug Counters

`main.c` exposes volatile counters:

```text
g_successfulTransactionCount
g_transmitFailureCount
g_ackTimeoutCount
g_retryExhaustedCount
```

These are useful in a debugger/watch window.

Interpretation:

| Counter | Meaning |
|---|---|
| successful transaction | DATA received matching ACK |
| transmit failure | SX1278 TX itself did not complete successfully |
| ACK timeout | DATA sent but matching ACK was not received |
| retry exhausted | all configured attempts failed |

A high ACK-timeout count with successful radio TX can indicate:

- gateway not listening,
- weak/downlink path,
- mismatched protocol,
- lost ACK,
- gateway persistence failure preventing ACK.

***

## Build and Flash

Open Keil project:

```text
node03/MDK/Node_3.uvprojx
```

Confirm:

```text
Optimization = O1
```

Build:

```text
F7
```

Flash/download with ST-Link:

```text
F8
```

If Keil cannot connect:

- check ST-Link wiring,
- target power,
- SWDIO,
- SWCLK,
- NRST if used,
- target selection.

***

## Verification

### 1. Build cleanly

Keil should complete without errors.

### 2. Gateway sees node

Monitor gateway serial output and verify node `03` responds.

### 3. MQTT receives node

On server:

```bash
mosquitto_sub \
  -h 127.0.0.1 \
  -p 1883 \
  -u <MQTT_USER> \
  -P <MQTT_PASSWORD> \
  -t 'iot/node03/telemetry' \
  -v
```

### 4. Backend latest

```bash
curl -s http://127.0.0.1:3000/latest | jq
```

Verify the `node03` entry.

### 5. Fault test

Safely simulate an electrical fault condition appropriate to the test fixture and verify:

```text
observed fault
→ persistent after 3 bad updates
→ backend detailed fault
→ frontend sensor card
```

Do not short a source or circuit in a way that can damage hardware; use controlled test fixtures.

### 6. Recovery

Restore a valid sensor condition and verify the latched fault clears after five consecutive good updates.

***

## Troubleshooting

### Node never answers POLL

Check:

- address `0x03`,
- SX1278 433 MHz module,
- antenna,
- SPI pins,
- NSS/RESET,
- radio settings,
- Keil optimization O1.

### O0 works strangely / LoRa timeouts

Use O1.

This is a known project-specific practical requirement.

### Temperature is `-1000`

`TEMP_ERROR_VALUE` means there is no valid temperature for the requested path.

Inspect:

- fault code,
- raw ADC,
- resistance,
- model range.

### Node average invalid although some sensors look valid

Check status/fault persistence and whether the stable-average routine has enough healthy channels/state initialized.

### Frequent SIGNAL_NOISY

Inspect:

- analog wiring,
- grounding,
- power supply,
- RC filtering,
- ADC source impedance,
- SPI/LoRa interference coupling,
- sensor cable routing.

### Frequent CROSS_SENSOR

Confirm all six sensors are actually expected to be near the same temperature.

### ACK timeout high

Inspect gateway logs for:

- DATA received,
- persist success,
- ACK transmit.

The gateway intentionally will not ACK if it cannot safely persist the record.

***

## Known Limitations

- Sensor calibration defaults are generic.
- Beta model is less accurate than full calibrated Steinhart-Hart over a wide range.
- Cross-sensor checking assumes comparable thermal conditions.
- Poll/response timing is fixed.
- No local non-volatile storage on the STM32 node; durability begins at the gateway.
- Sequence is 8-bit and relies on timing/address context and gateway logic.
- No remote configuration protocol.
- No firmware update over LoRa.
- Fault thresholds are compile-time constants.
- `float` math is used for NTC conversion on STM32F103.

***

## Future Improvements

1. Two- or three-point sensor calibration.
2. Store calibration in MCU flash.
3. Per-sensor calibration table generated from production test.
4. Steinhart-Hart coefficients.
5. Configurable cross-sensor groups.
6. Adaptive fault thresholds.
7. Add supply/reference-voltage diagnostics.
8. Add sensor serial/position metadata.
9. Add local event/error ring buffer.
10. Add protocol command for diagnostics dump.
11. Add remote configuration with authentication.
12. Add watchdog/reset-reason reporting.
13. Add unit tests for temperature conversion.
14. Add hardware-in-the-loop radio retry tests.
15. Add static-analysis/format checks to CI.
