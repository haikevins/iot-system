#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <mqtt_client.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <time.h>
#include <sys/time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing gateway/include/secrets.h. Copy secrets.example.h to secrets.h and configure local credentials."
#endif

#define LORA_SS_PIN                        27
#define LORA_RESET_PIN                     14
#define LORA_DIO0_PIN                      26

#define LORA_START_BYTE                    0xAAU
#define MAX_LORA_PAYLOAD                   64U
#define LORA_FRAME_OVERHEAD                6U
#define MIN_LORA_FRAME_LENGTH              LORA_FRAME_OVERHEAD
#define MAX_LORA_FRAME_LENGTH              (MAX_LORA_PAYLOAD + LORA_FRAME_OVERHEAD)

#define NODE1_ID                           0x01U
#define NODE2_ID                           0x02U
#define NODE3_ID                           0x03U

#define TYPE_DATA                          0x01U
#define TYPE_ACK                           0x10U
#define TYPE_POLL                          0x23U

#define SENSOR_COUNT                        6U
#define LEGACY_TEMPERATURE_PAYLOAD_LENGTH  3U
#define DETAILED_TEMPERATURE_PAYLOAD_LENGTH (3U + SENSOR_COUNT)
#define TIMESTAMPED_TEMPERATURE_PAYLOAD_LENGTH (DETAILED_TEMPERATURE_PAYLOAD_LENGTH + 4U)
#define TEMPERATURE_PAYLOAD_LENGTH         TIMESTAMPED_TEMPERATURE_PAYLOAD_LENGTH
#define PAYLOAD_STATUS_INDEX               2U
#define PAYLOAD_FAULT_BASE_INDEX           3U
#define PAYLOAD_SAMPLE_AGE_INDEX           (PAYLOAD_FAULT_BASE_INDEX + SENSOR_COUNT)

/*
 * SF7, BW125 kHz, CR4/5, explicit header, CRC on, preamble 8 and a 19-byte
 * application frame (13-byte DATA payload + six protocol bytes) gives about
 * 51.5 ms time-on-air.  Round up so the reconstructed timestamp does not make
 * a sample appear newer than it actually is.
 */
#define TIMESTAMPED_DATA_AIRTIME_MS         52UL
#define TEMPERATURE_INVALID_CENTI_C        ((int16_t)-32768)

#define DATA_WAIT_TIMEOUT_MS               1000UL
#define DUPLICATE_ACK_WINDOW_MS            700UL
#define NODE_CYCLE_DELAY_MS                1000UL
#define LORA_RECEIVE_POLL_DELAY_MS         5UL
#define MQTT_ENQUEUE_RETRY_DELAY_MS         1000UL
#define MQTT_ACK_CHECK_PERIOD_MS             250UL
#define MQTT_ACK_WATCHDOG_MS                 60000UL
#define MQTT_RETRANSMIT_TIMEOUT_MS           3000

#define PERSISTENT_OUTBOX_NAMESPACE          "tel_outbox"
#define PERSISTENT_OUTBOX_BOOT_KEY           "boot"
#define PERSISTENT_OUTBOX_CAPACITY           64U
#define PERSISTENT_RECORD_MAGIC              0x544C4D31UL
#define PERSISTENT_RECORD_VERSION            2U
#define LEGACY_PERSISTENT_RECORD_VERSION     1U

#define TIME_SYNC_TIMEOUT_MS                 8000UL
#define MIN_VALID_UNIX_TIME_SECONDS          1704067200LL
#define NTP_SERVER_PRIMARY                   "pool.ntp.org"
#define NTP_SERVER_SECONDARY                 "time.google.com"

#define ENABLE_DIAG_LOG                    1

#if ENABLE_DIAG_LOG
#define LOGI(...)                          Serial.printf(__VA_ARGS__)
#else
#define LOGI(...)
#endif

static const uint8_t s_nodeAddresses[] =
{
    NODE1_ID,
    NODE2_ID,
    NODE3_ID
};

#define NUM_NODES (sizeof(s_nodeAddresses) / sizeof(s_nodeAddresses[0]))

struct ProtocolPacket
{
    uint8_t address;
    uint8_t type;
    uint8_t sequence;
    uint8_t length;
    uint8_t data[MAX_LORA_PAYLOAD];
};

struct LegacyTelemetryMessageV1
{
    uint8_t address;
    uint8_t sequence;
    int16_t temperatureCentiCelsius;
    uint8_t status;
    uint32_t capturedAtMs;
    uint32_t bootId;
    uint64_t sampledAtUnixMs;
};

struct LegacyPersistentTelemetryRecordV1
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t recordId;
    LegacyTelemetryMessageV1 message;
};

struct TelemetryMessage
{
    uint8_t address;
    uint8_t sequence;
    int16_t temperatureCentiCelsius;
    uint8_t status;
    uint8_t sensorFaults[SENSOR_COUNT];
    uint8_t faultDetailValid;
    uint32_t capturedAtMs;
    uint32_t bootId;
    uint64_t sampledAtUnixMs;
};

struct PersistentTelemetryRecord
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t recordId;
    TelemetryMessage message;
};

esp_mqtt_client_handle_t g_mqttClient = nullptr;
TaskHandle_t g_mqttTaskHandle = nullptr;
SemaphoreHandle_t g_outboxMutex = nullptr;

static nvs_handle_t s_outboxNvsHandle = 0;
static bool s_outboxSlotUsed[PERSISTENT_OUTBOX_CAPACITY] = {false};
static uint64_t s_outboxSlotRecordId[PERSISTENT_OUTBOX_CAPACITY] = {0ULL};
static uint16_t s_outboxCount = 0U;
static uint32_t s_currentBootId = 0UL;
static uint32_t s_nextRecordCounter = 1UL;

static volatile bool s_mqttConnected = false;

static uint8_t s_currentNodeIndex = 0U;
static uint8_t s_nextSequence[NUM_NODES] = {0U};
static uint32_t s_crcFailureCount = 0UL;
static uint32_t s_invalidLengthCount = 0UL;
static uint32_t s_sequenceMismatchCount = 0UL;
static uint32_t s_duplicateDataCount = 0UL;
static uint32_t s_faultDetailMismatchCount = 0UL;

/**
 * @brief  Builds the NVS key used by one persistent outbox slot.
 * @param  slotIndex: Outbox slot index.
 * @param  keyBuffer: Output buffer for the NVS key.
 * @param  keyBufferSize: Size of keyBuffer.
 * @retval true when the key was created successfully, otherwise false.
 */
static bool BuildOutboxSlotKey(uint16_t slotIndex,
    char *keyBuffer,
    size_t keyBufferSize)
{
    if ((keyBuffer == nullptr) ||
        (keyBufferSize < 5U) ||
        (slotIndex >= PERSISTENT_OUTBOX_CAPACITY))
    {
        return false;
    }

    snprintf(keyBuffer, keyBufferSize, "q%02u", (unsigned int)slotIndex);
    return true;
}

/**
 * @brief  Returns Unix time in milliseconds when the system clock is synchronized.
 * @retval Unix timestamp in milliseconds, or 0 when the clock is not valid yet.
 */
static uint64_t GetUnixTimeMs(void)
{
    struct timeval currentTime = {};

    if (gettimeofday(&currentTime, nullptr) != 0)
    {
        return 0ULL;
    }

    if ((int64_t)currentTime.tv_sec < MIN_VALID_UNIX_TIME_SECONDS)
    {
        return 0ULL;
    }

    return ((uint64_t)currentTime.tv_sec * 1000ULL) +
        ((uint64_t)currentTime.tv_usec / 1000ULL);
}

/**
 * @brief  Starts SNTP synchronization and waits briefly for a valid wall clock.
 * @note   Failure to synchronize does not stop telemetry. The gateway falls back
 *         to ageMs while the record remains in the current boot session.
 * @retval true when a valid Unix clock is available, otherwise false.
 */
static bool InitializeSystemTime(void)
{
    configTime(0, 0, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);

    const unsigned long startTimeMs = millis();

    while ((millis() - startTimeMs) < TIME_SYNC_TIMEOUT_MS)
    {
        if (GetUnixTimeMs() != 0ULL)
        {
            Serial.println("System time synchronized");
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    Serial.println("System time not synchronized; using uptime fallback");
    return false;
}

/**
 * @brief  Reads one current or legacy persistent telemetry record from NVS.
 * @note   Version-1 records are converted in RAM with detailed faults marked unknown.
 * @param  slotIndex: Outbox slot index.
 * @param  record: Output version-2 record.
 * @retval true when a valid supported record exists, otherwise false.
 */
static bool ReadPersistentRecord(uint16_t slotIndex,
    PersistentTelemetryRecord &record)
{
    char slotKey[8];
    size_t storedSize = 0U;

    if (!BuildOutboxSlotKey(slotIndex, slotKey, sizeof(slotKey)))
    {
        return false;
    }

    esp_err_t result = nvs_get_blob(s_outboxNvsHandle,
        slotKey,
        nullptr,
        &storedSize);

    if (result != ESP_OK)
    {
        return false;
    }

    if (storedSize == sizeof(PersistentTelemetryRecord))
    {
        PersistentTelemetryRecord currentRecord = {};
        size_t readSize = sizeof(currentRecord);

        result = nvs_get_blob(s_outboxNvsHandle,
            slotKey,
            &currentRecord,
            &readSize);

        if ((result != ESP_OK) ||
            (readSize != sizeof(currentRecord)) ||
            (currentRecord.magic != PERSISTENT_RECORD_MAGIC) ||
            (currentRecord.version != PERSISTENT_RECORD_VERSION))
        {
            return false;
        }

        record = currentRecord;
        return true;
    }

    if (storedSize == sizeof(LegacyPersistentTelemetryRecordV1))
    {
        LegacyPersistentTelemetryRecordV1 legacyRecord = {};
        size_t readSize = sizeof(legacyRecord);

        result = nvs_get_blob(s_outboxNvsHandle,
            slotKey,
            &legacyRecord,
            &readSize);

        if ((result != ESP_OK) ||
            (readSize != sizeof(legacyRecord)) ||
            (legacyRecord.magic != PERSISTENT_RECORD_MAGIC) ||
            (legacyRecord.version != LEGACY_PERSISTENT_RECORD_VERSION))
        {
            return false;
        }

        record = {};
        record.magic = legacyRecord.magic;
        record.version = PERSISTENT_RECORD_VERSION;
        record.recordId = legacyRecord.recordId;
        record.message.address = legacyRecord.message.address;
        record.message.sequence = legacyRecord.message.sequence;
        record.message.temperatureCentiCelsius =
            legacyRecord.message.temperatureCentiCelsius;
        record.message.status = legacyRecord.message.status;
        record.message.faultDetailValid = 0U;
        record.message.capturedAtMs = legacyRecord.message.capturedAtMs;
        record.message.bootId = legacyRecord.message.bootId;
        record.message.sampledAtUnixMs = legacyRecord.message.sampledAtUnixMs;

        return true;
    }

    return false;
}

/**
 * @brief  Initializes the flash-backed telemetry outbox and restores queued records.
 * @note   NVS records are individual atomic entries. A DATA frame is ACKed only
 *         after its record has been committed successfully.
 * @retval true when the outbox is ready, otherwise false.
 */
static bool InitializePersistentOutbox(void)
{
    esp_err_t result = nvs_flash_init();

    if (result != ESP_OK)
    {
        Serial.printf("NVS init failed: %d\n", (int)result);
        return false;
    }

    result = nvs_open(PERSISTENT_OUTBOX_NAMESPACE,
        NVS_READWRITE,
        &s_outboxNvsHandle);

    if (result != ESP_OK)
    {
        Serial.printf("Outbox NVS open failed: %d\n", (int)result);
        return false;
    }

    g_outboxMutex = xSemaphoreCreateMutex();

    if (g_outboxMutex == nullptr)
    {
        Serial.println("Outbox mutex create failed");
        return false;
    }

    uint32_t previousBootId = 0UL;
    result = nvs_get_u32(s_outboxNvsHandle,
        PERSISTENT_OUTBOX_BOOT_KEY,
        &previousBootId);

    if ((result != ESP_OK) && (result != ESP_ERR_NVS_NOT_FOUND))
    {
        Serial.printf("Outbox boot counter read failed: %d\n", (int)result);
        return false;
    }

    s_currentBootId = previousBootId + 1UL;

    if (s_currentBootId == 0UL)
    {
        s_currentBootId = 1UL;
    }

    result = nvs_set_u32(s_outboxNvsHandle,
        PERSISTENT_OUTBOX_BOOT_KEY,
        s_currentBootId);

    if (result == ESP_OK)
    {
        result = nvs_commit(s_outboxNvsHandle);
    }

    if (result != ESP_OK)
    {
        Serial.printf("Outbox boot counter commit failed: %d\n", (int)result);
        return false;
    }

    s_outboxCount = 0U;

    for (uint16_t slotIndex = 0U;
         slotIndex < PERSISTENT_OUTBOX_CAPACITY;
         slotIndex++)
    {
        PersistentTelemetryRecord record = {};
        char slotKey[8];
        size_t storedSize = 0U;

        if (!BuildOutboxSlotKey(slotIndex, slotKey, sizeof(slotKey)))
        {
            return false;
        }

        result = nvs_get_blob(s_outboxNvsHandle,
            slotKey,
            nullptr,
            &storedSize);

        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            continue;
        }

        if (result != ESP_OK)
        {
            Serial.printf("Outbox slot %u size read failed: %d\n",
                (unsigned int)slotIndex,
                (int)result);
            return false;
        }

        if (!ReadPersistentRecord(slotIndex, record))
        {
            Serial.printf("Outbox slot %u is invalid; refusing silent data loss\n",
                (unsigned int)slotIndex);
            return false;
        }

        s_outboxSlotUsed[slotIndex] = true;
        s_outboxSlotRecordId[slotIndex] = record.recordId;
        s_outboxCount++;
    }

    Serial.printf("Persistent outbox ready: restored=%u capacity=%u boot=%lu\n",
        (unsigned int)s_outboxCount,
        (unsigned int)PERSISTENT_OUTBOX_CAPACITY,
        (unsigned long)s_currentBootId);

    return true;
}

/**
 * @brief  Persists one telemetry record before acknowledging the STM32 node.
 * @param  message: Telemetry message to store.
 * @retval true when the NVS commit completed successfully, otherwise false.
 */
static bool PersistentOutboxPush(const TelemetryMessage &message)
{
    if (xSemaphoreTake(g_outboxMutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    if (s_outboxCount >= PERSISTENT_OUTBOX_CAPACITY)
    {
        xSemaphoreGive(g_outboxMutex);
        Serial.println("Persistent outbox full");
        return false;
    }

    uint16_t freeSlot = PERSISTENT_OUTBOX_CAPACITY;

    for (uint16_t slotIndex = 0U;
         slotIndex < PERSISTENT_OUTBOX_CAPACITY;
         slotIndex++)
    {
        if (!s_outboxSlotUsed[slotIndex])
        {
            freeSlot = slotIndex;
            break;
        }
    }

    if (freeSlot >= PERSISTENT_OUTBOX_CAPACITY)
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    PersistentTelemetryRecord record = {};
    record.magic = PERSISTENT_RECORD_MAGIC;
    record.version = PERSISTENT_RECORD_VERSION;
    record.recordId = ((uint64_t)s_currentBootId << 32U) |
        (uint64_t)s_nextRecordCounter++;
    record.message = message;

    char slotKey[8];
    BuildOutboxSlotKey(freeSlot, slotKey, sizeof(slotKey));

    esp_err_t result = nvs_set_blob(s_outboxNvsHandle,
        slotKey,
        &record,
        sizeof(record));

    if (result == ESP_OK)
    {
        result = nvs_commit(s_outboxNvsHandle);
    }

    if (result != ESP_OK)
    {
        Serial.printf("Outbox persist failed: %d\n", (int)result);
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    s_outboxSlotUsed[freeSlot] = true;
    s_outboxSlotRecordId[freeSlot] = record.recordId;
    s_outboxCount++;

    LOGI("[OUTBOX+] id=%llu slot=%u count=%u\n",
        (unsigned long long)record.recordId,
        (unsigned int)freeSlot,
        (unsigned int)s_outboxCount);

    xSemaphoreGive(g_outboxMutex);
    return true;
}

/**
 * @brief  Returns the oldest durable telemetry record without removing it.
 * @param  record: Output record.
 * @retval true when a record is available, otherwise false.
 */
static bool PersistentOutboxPeek(PersistentTelemetryRecord &record)
{
    if (xSemaphoreTake(g_outboxMutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    if (s_outboxCount == 0U)
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    uint16_t oldestSlot = PERSISTENT_OUTBOX_CAPACITY;
    uint64_t oldestRecordId = UINT64_MAX;

    for (uint16_t slotIndex = 0U;
         slotIndex < PERSISTENT_OUTBOX_CAPACITY;
         slotIndex++)
    {
        if (s_outboxSlotUsed[slotIndex] &&
            (s_outboxSlotRecordId[slotIndex] < oldestRecordId))
        {
            oldestRecordId = s_outboxSlotRecordId[slotIndex];
            oldestSlot = slotIndex;
        }
    }

    const bool success = (oldestSlot < PERSISTENT_OUTBOX_CAPACITY) &&
        ReadPersistentRecord(oldestSlot, record);

    if (!success)
    {
        Serial.println("Persistent outbox head read failed");
    }

    xSemaphoreGive(g_outboxMutex);
    return success;
}

/**
 * @brief  Removes the oldest durable telemetry record after MQTT broker PUBACK.
 * @param  expectedRecordId: Record ID that was acknowledged by the broker.
 * @retval true when the record was durably removed, otherwise false.
 */
static bool PersistentOutboxPop(uint64_t expectedRecordId)
{
    if (xSemaphoreTake(g_outboxMutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    uint16_t targetSlot = PERSISTENT_OUTBOX_CAPACITY;

    for (uint16_t slotIndex = 0U;
         slotIndex < PERSISTENT_OUTBOX_CAPACITY;
         slotIndex++)
    {
        if (s_outboxSlotUsed[slotIndex] &&
            (s_outboxSlotRecordId[slotIndex] == expectedRecordId))
        {
            targetSlot = slotIndex;
            break;
        }
    }

    if (targetSlot >= PERSISTENT_OUTBOX_CAPACITY)
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    char slotKey[8];
    BuildOutboxSlotKey(targetSlot, slotKey, sizeof(slotKey));

    esp_err_t result = nvs_erase_key(s_outboxNvsHandle, slotKey);

    if (result == ESP_OK)
    {
        result = nvs_commit(s_outboxNvsHandle);
    }

    if (result != ESP_OK)
    {
        Serial.printf("Outbox remove failed: %d\n", (int)result);
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    s_outboxSlotUsed[targetSlot] = false;
    s_outboxSlotRecordId[targetSlot] = 0ULL;

    if (s_outboxCount > 0U)
    {
        s_outboxCount--;
    }

    LOGI("[OUTBOX-] id=%llu slot=%u count=%u\n",
        (unsigned long long)expectedRecordId,
        (unsigned int)targetSlot,
        (unsigned int)s_outboxCount);

    xSemaphoreGive(g_outboxMutex);
    return true;
}

/**
 * @brief  Returns the MQTT node name associated with a LoRa node address.
 * @param  address: LoRa node address.
 * @retval Node name string, or nullptr for an unknown address.
 */
static const char *GetNodeName(uint8_t address)
{
    switch (address)
    {
        case NODE1_ID:
        {
            return "node01";
        }

        case NODE2_ID:
        {
            return "node02";
        }

        case NODE3_ID:
        {
            return "node03";
        }

        default:
        {
            return nullptr;
        }
    }
}

/**
 * @brief  Calculates CRC-8 with initial value 0x00 and polynomial 0x07.
 * @param  data: Input byte array.
 * @param  length: Number of bytes to process.
 * @retval Calculated CRC-8 value.
 */
static uint8_t CalculateCrc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0x00U;

    for (uint8_t byteIndex = 0U; byteIndex < length; byteIndex++)
    {
        crc ^= data[byteIndex];

        for (uint8_t bitIndex = 0U; bitIndex < 8U; bitIndex++)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1U) ^ 0x07U);
            }
            else
            {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

/**
 * @brief  Discards every unread byte from the current LoRa packet.
 * @retval None
 */
static void DrainLoRaPacket(void)
{
    while (LoRa.available() > 0)
    {
        LoRa.read();
    }
}

/**
 * @brief  Sends one protocol frame through the LoRa library.
 * @note   Frame format is START | ADDR | TYPE | SEQ | LEN | DATA | CRC8.
 *         CRC8 covers ADDR | TYPE | SEQ | LEN | DATA.
 * @param  address: Node address stored in the frame.
 * @param  packetType: Application packet type.
 * @param  sequence: Transaction sequence number.
 * @param  payload: Pointer to payload bytes, or nullptr when payloadLength is 0.
 * @param  payloadLength: Number of payload bytes.
 * @retval 1 when LoRa.endPacket() reports success, otherwise 0.
 */
static bool SendProtocolPacket(uint8_t address,
    uint8_t packetType,
    uint8_t sequence,
    const uint8_t *payload,
    uint8_t payloadLength)
{
    uint8_t crcBuffer[MAX_LORA_PAYLOAD + 4U];

    if (payloadLength > MAX_LORA_PAYLOAD)
    {
        return false;
    }

    if ((payloadLength > 0U) && (payload == nullptr))
    {
        return false;
    }

    crcBuffer[0] = address;
    crcBuffer[1] = packetType;
    crcBuffer[2] = sequence;
    crcBuffer[3] = payloadLength;

    for (uint8_t payloadIndex = 0U; payloadIndex < payloadLength; payloadIndex++)
    {
        crcBuffer[4U + payloadIndex] = payload[payloadIndex];
    }

    const uint8_t crc = CalculateCrc8(crcBuffer, (uint8_t)(payloadLength + 4U));

    LoRa.beginPacket();
    LoRa.write(LORA_START_BYTE);
    LoRa.write(address);
    LoRa.write(packetType);
    LoRa.write(sequence);
    LoRa.write(payloadLength);

    for (uint8_t payloadIndex = 0U; payloadIndex < payloadLength; payloadIndex++)
    {
        LoRa.write(payload[payloadIndex]);
    }

    LoRa.write(crc);
    return LoRa.endPacket() == 1;
}

/**
 * @brief  Receives and validates one complete application packet.
 * @param  packet: Output packet structure.
 * @retval 1 when a complete frame with valid length and CRC is available, otherwise 0.
 */
static bool ReceiveProtocolPacket(ProtocolPacket &packet)
{
    const int packetSize = LoRa.parsePacket();

    if (packetSize <= 0)
    {
        return false;
    }

    if ((packetSize < (int)MIN_LORA_FRAME_LENGTH) ||
        (packetSize > (int)MAX_LORA_FRAME_LENGTH))
    {
        s_invalidLengthCount++;
        DrainLoRaPacket();
        return false;
    }

    if (LoRa.available() < 5)
    {
        s_invalidLengthCount++;
        DrainLoRaPacket();
        return false;
    }

    const uint8_t startByte = (uint8_t)LoRa.read();

    if (startByte != LORA_START_BYTE)
    {
        DrainLoRaPacket();
        return false;
    }

    packet.address = (uint8_t)LoRa.read();
    packet.type = (uint8_t)LoRa.read();
    packet.sequence = (uint8_t)LoRa.read();
    packet.length = (uint8_t)LoRa.read();

    if (packet.length > MAX_LORA_PAYLOAD)
    {
        s_invalidLengthCount++;
        DrainLoRaPacket();
        return false;
    }

    const uint8_t expectedFrameLength =
        (uint8_t)(packet.length + LORA_FRAME_OVERHEAD);

    if (packetSize != expectedFrameLength)
    {
        s_invalidLengthCount++;
        DrainLoRaPacket();
        return false;
    }

    if (LoRa.available() < (int)(packet.length + 1U))
    {
        s_invalidLengthCount++;
        DrainLoRaPacket();
        return false;
    }

    for (uint8_t payloadIndex = 0U;
         payloadIndex < packet.length;
         payloadIndex++)
    {
        packet.data[payloadIndex] = (uint8_t)LoRa.read();
    }

    const uint8_t receivedCrc = (uint8_t)LoRa.read();
    uint8_t crcBuffer[MAX_LORA_PAYLOAD + 4U];

    crcBuffer[0] = packet.address;
    crcBuffer[1] = packet.type;
    crcBuffer[2] = packet.sequence;
    crcBuffer[3] = packet.length;

    for (uint8_t payloadIndex = 0U;
         payloadIndex < packet.length;
         payloadIndex++)
    {
        crcBuffer[4U + payloadIndex] = packet.data[payloadIndex];
    }

    if (CalculateCrc8(crcBuffer,
        (uint8_t)(packet.length + 4U)) != receivedCrc)
    {
        s_crcFailureCount++;
        return false;
    }

    return true;
}

/**
 * @brief  Sends a POLL packet for one node and one transaction sequence.
 * @param  address: Target node address.
 * @param  sequence: New transaction sequence number.
 * @retval 1 on successful radio transmission, otherwise 0.
 */
static bool SendPoll(uint8_t address, uint8_t sequence)
{
    const bool result = SendProtocolPacket(address,
        TYPE_POLL,
        sequence,
        nullptr,
        0U);

    if (result)
    {
        LOGI("[POLL] node=%02X seq=%u\n", address, sequence);
    }

    return result;
}

/**
 * @brief  Sends an ACK for a received DATA packet.
 * @param  address: Source node address of the DATA packet.
 * @param  sequence: Sequence number copied from the DATA packet.
 * @retval 1 on successful radio transmission, otherwise 0.
 */
static bool SendAcknowledgement(uint8_t address, uint8_t sequence)
{
    const bool result = SendProtocolPacket(address,
        TYPE_ACK,
        sequence,
        nullptr,
        0U);

    if (result)
    {
        LOGI("[ACK] node=%02X seq=%u\n", address, sequence);
    }

    return result;
}

/**
 * @brief  Decodes signed little-endian centi-degrees from DATA payload bytes.
 * @param  payload: DATA payload with temperature in bytes 0 and 1.
 * @retval Signed temperature in centi-degrees Celsius.
 */
static int16_t DecodeTemperatureCentiCelsius(const uint8_t *payload)
{
    const uint16_t rawValue = (uint16_t)payload[0] |
        ((uint16_t)payload[1] << 8U);

    return (int16_t)rawValue;
}

/**
 * @brief  Decodes a little-endian 32-bit unsigned integer.
 * @param  data: Pointer to four bytes.
 * @retval Decoded value.
 */
static uint32_t DecodeUint32LittleEndian(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
        ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) |
        ((uint32_t)data[3] << 24U);
}

/**
 * @brief  Checks whether a DATA payload length is supported by the gateway.
 * @param  payloadLength: DATA payload length.
 * @retval true for legacy 3-byte, detailed 9-byte or timestamped 13-byte payloads.
 */
static bool IsSupportedTemperaturePayloadLength(uint8_t payloadLength)
{
    return (payloadLength == LEGACY_TEMPERATURE_PAYLOAD_LENGTH) ||
        (payloadLength == DETAILED_TEMPERATURE_PAYLOAD_LENGTH) ||
        (payloadLength == TIMESTAMPED_TEMPERATURE_PAYLOAD_LENGTH);
}

/**
 * @brief  Verifies that the summary status mask matches detailed sensor faults.
 * @param  status: Six-bit summary fault mask.
 * @param  sensorFaults: Six detailed sensor fault bytes.
 * @retval true when each status bit matches whether its detailed code is non-zero.
 */
static bool ValidateFaultDetailConsistency(uint8_t status,
    const uint8_t sensorFaults[SENSOR_COUNT])
{
    for (uint8_t sensorIndex = 0U;
         sensorIndex < SENSOR_COUNT;
         sensorIndex++)
    {
        const bool summaryFault =
            ((status >> sensorIndex) & 0x01U) != 0U;
        const bool detailedFault = sensorFaults[sensorIndex] != 0U;

        if (summaryFault != detailedFault)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief  Ensures the ESP32 is connected to Wi-Fi.
 * @retval None
 */
static void EnsureWiFiConnected(void)
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return;
    }

    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
}

/**
 * @brief  Receives ESP-MQTT connection and publish acknowledgement events.
 * @note   MQTT_EVENT_PUBLISHED is emitted only after the broker acknowledges a QoS 1/2 publish.
 * @param  handlerArgs: Event handler context, unused.
 * @param  eventBase: ESP event base, unused.
 * @param  eventId: MQTT event identifier.
 * @param  eventData: Pointer to esp_mqtt_event_t.
 * @retval None
 */
static void MqttEventHandler(void *handlerArgs,
    esp_event_base_t eventBase,
    int32_t eventId,
    void *eventData)
{
    (void)handlerArgs;
    (void)eventBase;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

    switch ((esp_mqtt_event_id_t)eventId)
    {
        case MQTT_EVENT_CONNECTED:
        {
            s_mqttConnected = true;
            Serial.println("MQTT connected (QoS 1 enabled)");
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
        {
            s_mqttConnected = false;
            Serial.println("MQTT disconnected");
            break;
        }

        case MQTT_EVENT_PUBLISHED:
        {
            if (g_mqttTaskHandle != nullptr)
            {
                xTaskNotify(g_mqttTaskHandle,
                    (uint32_t)event->msg_id,
                    eSetValueWithOverwrite);
            }

            break;
        }

        case MQTT_EVENT_ERROR:
        {
            Serial.println("MQTT transport/protocol error");
            break;
        }

        default:
        {
            break;
        }
    }
}

/**
 * @brief  Initializes the native ESP-MQTT client with QoS 1 retransmission support.
 * @note   Clean Session is disabled so the broker can preserve the MQTT session across reconnects.
 * @note   ESP-MQTT keeps unacknowledged QoS 1 messages in its outbox and retransmits them.
 * @retval true when the client is created and started successfully, otherwise false.
 */
static bool InitializeMqttClient(void)
{
    esp_mqtt_client_config_t mqttConfig = {};

    mqttConfig.broker.address.hostname = MQTT_BROKER;
    mqttConfig.broker.address.port = MQTT_PORT;
    mqttConfig.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;

    mqttConfig.credentials.username = MQTT_USER;
    mqttConfig.credentials.client_id = MQTT_CLIENT_ID;
    mqttConfig.credentials.authentication.password = MQTT_PASSWORD;

    mqttConfig.session.disable_clean_session = true;
    mqttConfig.session.keepalive = 30;
    mqttConfig.session.message_retransmit_timeout =
        MQTT_RETRANSMIT_TIMEOUT_MS;

    mqttConfig.network.reconnect_timeout_ms = 2000;

    g_mqttClient = esp_mqtt_client_init(&mqttConfig);

    if (g_mqttClient == nullptr)
    {
        Serial.println("MQTT client init failed");
        return false;
    }

    if (esp_mqtt_client_register_event(
        g_mqttClient,
        MQTT_EVENT_ANY,
        MqttEventHandler,
        nullptr) != ESP_OK)
    {
        Serial.println("MQTT event registration failed");
        return false;
    }

    if (esp_mqtt_client_start(g_mqttClient) != ESP_OK)
    {
        Serial.println("MQTT client start failed");
        return false;
    }

    return true;
}

/**
 * @brief  Enqueues one durable telemetry record for MQTT QoS 1 delivery.
 * @note   Detailed faults are sent as six raw fault-code bytes when available.
 * @param  record: Durable outbox record to publish.
 * @retval Positive MQTT message ID on success, or -1 on failure.
 */
static int EnqueueTelemetryQos1(const PersistentTelemetryRecord &record)
{
    const TelemetryMessage &message = record.message;
    const char *nodeName = GetNodeName(message.address);

    if ((nodeName == nullptr) || (g_mqttClient == nullptr))
    {
        return -1;
    }

    char telemetryTopic[40];
    char telemetryPayload[448];
    char timestampText[32];
    char ageText[24];
    char faultArrayText[64];

    const bool recovered = message.bootId != s_currentBootId;
    const uint64_t currentUnixMs = GetUnixTimeMs();
    bool timestampValid = false;

    if (message.sampledAtUnixMs != 0ULL)
    {
        snprintf(timestampText,
            sizeof(timestampText),
            "%llu",
            (unsigned long long)message.sampledAtUnixMs);
        timestampValid = true;

        if ((currentUnixMs >= message.sampledAtUnixMs) &&
            ((currentUnixMs - message.sampledAtUnixMs) <= 0xFFFFFFFFULL))
        {
            snprintf(ageText,
                sizeof(ageText),
                "%lu",
                (unsigned long)(currentUnixMs - message.sampledAtUnixMs));
        }
        else
        {
            snprintf(ageText, sizeof(ageText), "null");
        }
    }
    else if (!recovered)
    {
        snprintf(timestampText, sizeof(timestampText), "null");
        snprintf(ageText,
            sizeof(ageText),
            "%lu",
            (unsigned long)((uint32_t)(millis() - message.capturedAtMs)));
        timestampValid = true;
    }
    else
    {
        snprintf(timestampText, sizeof(timestampText), "null");
        snprintf(ageText, sizeof(ageText), "null");
    }

    if (message.faultDetailValid != 0U)
    {
        snprintf(faultArrayText,
            sizeof(faultArrayText),
            "[%u,%u,%u,%u,%u,%u]",
            message.sensorFaults[0],
            message.sensorFaults[1],
            message.sensorFaults[2],
            message.sensorFaults[3],
            message.sensorFaults[4],
            message.sensorFaults[5]);
    }
    else
    {
        snprintf(faultArrayText, sizeof(faultArrayText), "null");
    }

    snprintf(telemetryTopic,
        sizeof(telemetryTopic),
        "iot/%s/telemetry",
        nodeName);

    char temperatureText[20];

    if (message.temperatureCentiCelsius == TEMPERATURE_INVALID_CENTI_C)
    {
        snprintf(temperatureText, sizeof(temperatureText), "null");
    }
    else
    {
        int32_t absoluteTemperature = message.temperatureCentiCelsius;
        const bool isNegative = absoluteTemperature < 0;

        if (isNegative)
        {
            absoluteTemperature = -absoluteTemperature;
        }

        snprintf(temperatureText,
            sizeof(temperatureText),
            "%s%ld.%02ld",
            isNegative ? "-" : "",
            (long)(absoluteTemperature / 100L),
            (long)(absoluteTemperature % 100L));
    }

    snprintf(telemetryPayload,
        sizeof(telemetryPayload),
        "{\"id\":%llu,\"seq\":%u,\"temp\":%s,\"tempValid\":%s,"
        "\"status\":%u,\"faultDetailValid\":%s,\"faults\":%s,"
        "\"sampledAtMs\":%s,\"ageMs\":%s,"
        "\"timestampValid\":%s,\"recovered\":%s}",
        (unsigned long long)record.recordId,
        message.sequence,
        temperatureText,
        message.temperatureCentiCelsius == TEMPERATURE_INVALID_CENTI_C
            ? "false"
            : "true",
        message.status,
        message.faultDetailValid != 0U ? "true" : "false",
        faultArrayText,
        timestampText,
        ageText,
        timestampValid ? "true" : "false",
        recovered ? "true" : "false");

    const int mqttMessageId = esp_mqtt_client_enqueue(g_mqttClient,
        telemetryTopic,
        telemetryPayload,
        0,
        1,
        0,
        true);

    if (mqttMessageId >= 0)
    {
        LOGI("[MQTT-QOS1-TX] %s id=%llu seq=%u msg_id=%d payload=%s\n",
            nodeName,
            (unsigned long long)record.recordId,
            message.sequence,
            mqttMessageId,
            telemetryPayload);
    }

    return mqttMessageId;
}

/**
 * @brief  Persists one decoded DATA packet before sending the LoRa ACK.
 * @param  address: Node address.
 * @param  sequence: LoRa transaction sequence number.
 * @param  temperatureCentiCelsius: Signed temperature multiplied by 100.
 * @param  status: Six-bit sensor fault summary mask.
 * @param  sensorFaults: Six detailed fault-code bytes.
 * @param  faultDetailValid: true when detailed fault bytes are available.
 * @param  capturedAtMs: Gateway uptime timestamp when the DATA frame was accepted.
 * @param  nodeSampleAgeMs: Age reported by STM32 from ADC-burst midpoint to TX.
 * @param  nodeSampleAgeValid: true when the DATA payload contains sample age.
 * @retval true when the record is durably committed to NVS, otherwise false.
 */
static bool PersistTelemetry(uint8_t address,
    uint8_t sequence,
    int16_t temperatureCentiCelsius,
    uint8_t status,
    const uint8_t sensorFaults[SENSOR_COUNT],
    bool faultDetailValid,
    uint32_t capturedAtMs,
    uint32_t nodeSampleAgeMs,
    bool nodeSampleAgeValid)
{
    TelemetryMessage message = {};
    message.address = address;
    message.sequence = sequence;
    message.temperatureCentiCelsius = temperatureCentiCelsius;
    message.status = status;
    message.faultDetailValid = faultDetailValid ? 1U : 0U;

    if (faultDetailValid)
    {
        for (uint8_t sensorIndex = 0U;
             sensorIndex < SENSOR_COUNT;
             sensorIndex++)
        {
            message.sensorFaults[sensorIndex] = sensorFaults[sensorIndex];
        }
    }

    message.capturedAtMs = capturedAtMs;
    message.bootId = s_currentBootId;

    const uint64_t receivedAtUnixMs = GetUnixTimeMs();

    if (nodeSampleAgeValid &&
        (receivedAtUnixMs != 0ULL) &&
        (receivedAtUnixMs >=
            ((uint64_t)nodeSampleAgeMs + TIMESTAMPED_DATA_AIRTIME_MS)))
    {
        message.sampledAtUnixMs =
            receivedAtUnixMs -
            (uint64_t)nodeSampleAgeMs -
            (uint64_t)TIMESTAMPED_DATA_AIRTIME_MS;
    }
    else
    {
        /* Rolling-update fallback for legacy 3-byte / 9-byte node payloads. */
        message.sampledAtUnixMs = receivedAtUnixMs;
    }

    return PersistentOutboxPush(message);
}

/**
 * @brief  Re-ACKs duplicate DATA retries without enqueueing telemetry twice.
 * @param  address: Current polled node address.
 * @param  sequence: Current transaction sequence number.
 * @retval None
 */
static void AcknowledgeDuplicateRetries(uint8_t address, uint8_t sequence)
{
    const unsigned long startTimeMs = millis();

    while ((millis() - startTimeMs) < DUPLICATE_ACK_WINDOW_MS)
    {
        ProtocolPacket packet;

        if (!ReceiveProtocolPacket(packet))
        {
            vTaskDelay(pdMS_TO_TICKS(LORA_RECEIVE_POLL_DELAY_MS));
            continue;
        }

        if ((packet.address == address) &&
            (packet.type == TYPE_DATA) &&
            (packet.sequence == sequence) &&
            IsSupportedTemperaturePayloadLength(packet.length))
        {
            s_duplicateDataCount++;
            SendAcknowledgement(address, sequence);
            LOGI("[DUP] node=%02X seq=%u count=%lu\n",
                address,
                sequence,
                (unsigned long)s_duplicateDataCount);
        }
    }
}

/**
 * @brief  Maintains Wi-Fi and delivers queued telemetry with MQTT QoS 1.
 * @note   A telemetry item is removed only after broker PUBACK is received.
 * @note   ESP-MQTT handles retransmission of an unacknowledged QoS 1 message after reconnect.
 * @param  parameter: FreeRTOS task parameter, unused.
 * @retval Never returns.
 */
static void MqttTask(void *parameter)
{
    (void)parameter;

    PersistentTelemetryRecord pendingRecord = {};
    bool hasPendingRecord = false;
    bool waitingForPubAck = false;
    int pendingMqttMessageId = -1;
    uint32_t enqueueFailureCount = 0UL;
    uint32_t pubAckCount = 0UL;
    unsigned long mqttEnqueueTimeMs = 0UL;

    for (;;)
    {
        EnsureWiFiConnected();

        if (!hasPendingRecord)
        {
            if (PersistentOutboxPeek(pendingRecord))
            {
                hasPendingRecord = true;
                waitingForPubAck = false;
                pendingMqttMessageId = -1;
            }
        }

        if (hasPendingRecord &&
            !waitingForPubAck &&
            s_mqttConnected)
        {
            pendingMqttMessageId = EnqueueTelemetryQos1(pendingRecord);

            if (pendingMqttMessageId >= 0)
            {
                waitingForPubAck = true;
                mqttEnqueueTimeMs = millis();
            }
            else
            {
                enqueueFailureCount++;
                Serial.printf("MQTT QoS1 enqueue failed, retry=%lu\n",
                    (unsigned long)enqueueFailureCount);

                vTaskDelay(pdMS_TO_TICKS(MQTT_ENQUEUE_RETRY_DELAY_MS));
                continue;
            }
        }

        if (waitingForPubAck)
        {
            uint32_t acknowledgedMessageId = 0U;

            if (xTaskNotifyWait(0U,
                UINT32_MAX,
                &acknowledgedMessageId,
                pdMS_TO_TICKS(MQTT_ACK_CHECK_PERIOD_MS)) == pdTRUE)
            {
                if ((int)acknowledgedMessageId == pendingMqttMessageId)
                {
                    if (!PersistentOutboxPop(pendingRecord.recordId))
                    {
                        Serial.println("PUBACK received but durable outbox removal failed");
                        vTaskDelay(pdMS_TO_TICKS(MQTT_ENQUEUE_RETRY_DELAY_MS));
                        continue;
                    }

                    pubAckCount++;

                    LOGI("[MQTT-PUBACK] id=%llu seq=%u msg_id=%d count=%lu\n",
                        (unsigned long long)pendingRecord.recordId,
                        pendingRecord.message.sequence,
                        pendingMqttMessageId,
                        (unsigned long)pubAckCount);

                    hasPendingRecord = false;
                    waitingForPubAck = false;
                    pendingMqttMessageId = -1;
                    continue;
                }

                LOGI("[MQTT] Ignored unexpected PUBACK msg_id=%lu expected=%d\n",
                    (unsigned long)acknowledgedMessageId,
                    pendingMqttMessageId);
            }

            if (((millis() - mqttEnqueueTimeMs) >= MQTT_ACK_WATCHDOG_MS) &&
                s_mqttConnected &&
                (esp_mqtt_client_get_outbox_size(g_mqttClient) == 0))
            {
                Serial.printf("MQTT PUBACK watchdog expired for msg_id=%d; retrying durable record\n",
                    pendingMqttMessageId);

                waitingForPubAck = false;
                pendingMqttMessageId = -1;
            }

            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief  Polls nodes round-robin and performs SEQ + ACK transaction handling.
 * @param  parameter: FreeRTOS task parameter, unused.
 * @retval Never returns.
 */
static void LoraTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        const uint8_t currentNodeAddress = s_nodeAddresses[s_currentNodeIndex];
        const uint8_t currentSequence = s_nextSequence[s_currentNodeIndex]++;
        bool dataReceived = false;

        if (SendPoll(currentNodeAddress, currentSequence))
        {
            const unsigned long startTimeMs = millis();

            while ((millis() - startTimeMs) < DATA_WAIT_TIMEOUT_MS)
            {
                ProtocolPacket packet;

                if (!ReceiveProtocolPacket(packet))
                {
                    vTaskDelay(pdMS_TO_TICKS(LORA_RECEIVE_POLL_DELAY_MS));
                    continue;
                }

                if (packet.address != currentNodeAddress)
                {
                    continue;
                }

                if (packet.sequence != currentSequence)
                {
                    s_sequenceMismatchCount++;
                    LOGI("[SEQ] node=%02X expected=%u received=%u\n",
                        currentNodeAddress,
                        currentSequence,
                        packet.sequence);
                    continue;
                }

                if ((packet.type != TYPE_DATA) ||
                    !IsSupportedTemperaturePayloadLength(packet.length))
                {
                    continue;
                }

                const int16_t temperatureCentiCelsius =
                    DecodeTemperatureCentiCelsius(packet.data);
                const uint8_t status =
                    (uint8_t)(packet.data[PAYLOAD_STATUS_INDEX] & 0x3FU);
                uint8_t sensorFaults[SENSOR_COUNT] = {0U};
                const bool faultDetailValid =
                    (packet.length == DETAILED_TEMPERATURE_PAYLOAD_LENGTH) ||
                    (packet.length == TIMESTAMPED_TEMPERATURE_PAYLOAD_LENGTH);
                const bool nodeSampleAgeValid =
                    packet.length == TIMESTAMPED_TEMPERATURE_PAYLOAD_LENGTH;
                uint32_t nodeSampleAgeMs = 0UL;

                if (nodeSampleAgeValid)
                {
                    nodeSampleAgeMs = DecodeUint32LittleEndian(
                        &packet.data[PAYLOAD_SAMPLE_AGE_INDEX]);
                }

                if (faultDetailValid)
                {
                    for (uint8_t sensorIndex = 0U;
                         sensorIndex < SENSOR_COUNT;
                         sensorIndex++)
                    {
                        sensorFaults[sensorIndex] =
                            packet.data[PAYLOAD_FAULT_BASE_INDEX + sensorIndex];
                    }

                    if (!ValidateFaultDetailConsistency(status, sensorFaults))
                    {
                        s_faultDetailMismatchCount++;

                        LOGI("[FAULT-DETAIL] node=%02X seq=%u summary/detail mismatch count=%lu\n",
                            currentNodeAddress,
                            currentSequence,
                            (unsigned long)s_faultDetailMismatchCount);

                        continue;
                    }
                }

                const uint32_t capturedAtMs = millis();

                const bool persisted = PersistTelemetry(currentNodeAddress,
                    currentSequence,
                    temperatureCentiCelsius,
                    status,
                    sensorFaults,
                    faultDetailValid,
                    capturedAtMs,
                    nodeSampleAgeMs,
                    nodeSampleAgeValid);

                if (!persisted)
                {
                    LOGI("[OUTBOX] node=%02X seq=%u persist failed/full, DATA not ACKed\n",
                        currentNodeAddress,
                        currentSequence);
                    continue;
                }

                SendAcknowledgement(currentNodeAddress, currentSequence);

                if (temperatureCentiCelsius == TEMPERATURE_INVALID_CENTI_C)
                {
                    LOGI("[DATA] node=%02X seq=%u temp=INVALID status=%u detail=%s adc_age=%s%lu rssi=%d\n",
                        currentNodeAddress,
                        currentSequence,
                        status,
                        faultDetailValid ? "yes" : "no",
                        nodeSampleAgeValid ? "" : "n/a:",
                        (unsigned long)nodeSampleAgeMs,
                        LoRa.packetRssi());
                }
                else
                {
                    LOGI("[DATA] node=%02X seq=%u temp_raw=%d status=%u detail=%s adc_age=%s%lu rssi=%d\n",
                        currentNodeAddress,
                        currentSequence,
                        temperatureCentiCelsius,
                        status,
                        faultDetailValid ? "yes" : "no",
                        nodeSampleAgeValid ? "" : "n/a:",
                        (unsigned long)nodeSampleAgeMs,
                        LoRa.packetRssi());
                }

                dataReceived = true;
                AcknowledgeDuplicateRetries(currentNodeAddress, currentSequence);
                break;
            }
        }

        if (!dataReceived)
        {
            LOGI("[TIMEOUT] node=%02X seq=%u\n",
                currentNodeAddress,
                currentSequence);
        }

        s_currentNodeIndex++;

        if (s_currentNodeIndex >= NUM_NODES)
        {
            s_currentNodeIndex = 0U;
        }

        vTaskDelay(pdMS_TO_TICKS(NODE_CYCLE_DELAY_MS));
    }
}

/**
 * @brief  Initializes LoRa, persistent telemetry outbox and FreeRTOS tasks.
 * @retval None
 */
void setup(void)
{
    Serial.begin(115200);
    delay(1000);

    SPI.begin(18, 19, 23, LORA_SS_PIN);
    LoRa.setPins(LORA_SS_PIN, LORA_RESET_PIN, LORA_DIO0_PIN);

    if (!LoRa.begin(433E6))
    {
        Serial.println("LoRa init failed");

        while (1)
        {
            delay(1000);
        }
    }

    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setPreambleLength(8);
    LoRa.enableCrc();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    EnsureWiFiConnected();
    InitializeSystemTime();

    if (!InitializePersistentOutbox())
    {
        Serial.println("Persistent outbox initialization failed");

        while (1)
        {
            delay(1000);
        }
    }

    xTaskCreatePinnedToCore(MqttTask,
        "mqttTask",
        6144,
        nullptr,
        1,
        &g_mqttTaskHandle,
        1);

    if (!InitializeMqttClient())
    {
        Serial.println("MQTT initialization failed");

        while (1)
        {
            delay(1000);
        }
    }

    xTaskCreatePinnedToCore(LoraTask,
        "loraTask",
        6144,
        nullptr,
        1,
        nullptr,
        0);

    Serial.println("Gateway ready (protocol v5: durable NVS outbox + MQTT QoS 1)");
}

/**
 * @brief  Suspends the Arduino loop task because application work runs in FreeRTOS tasks.
 * @retval None
 */
void loop(void)
{
    vTaskDelay(portMAX_DELAY);
}
