#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <FS.h>
#include <mqtt_client.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <time.h>
#include <sys/time.h>
#include <stddef.h>
#include <string.h>

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

#define LEGACY_NVS_OUTBOX_NAMESPACE          "tel_outbox"
#define LEGACY_NVS_OUTBOX_BOOT_KEY           "boot"
#define LEGACY_NVS_OUTBOX_CAPACITY           64U
#define LEGACY_PERSISTENT_RECORD_MAGIC        0x544C4D31UL
#define LEGACY_PERSISTENT_RECORD_VERSION_V1  1U
#define LEGACY_PERSISTENT_RECORD_VERSION_V2  2U

#define FILE_OUTBOX_DIRECTORY                "/outbox"
#define FILE_OUTBOX_MAX_RECORDS              2048U
#define FILE_OUTBOX_MIN_FREE_BYTES           (128U * 1024U)
#define FILE_OUTBOX_RECORD_MAGIC             0x544C4D46UL
#define FILE_OUTBOX_RECORD_VERSION           3U
#define FILE_OUTBOX_RECORD_ID_LENGTH         56U

#define WIFI_RECONNECT_INTERVAL_MS           5000UL
#define TIME_SYNC_STATUS_LOG_INTERVAL_MS     10000UL
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

struct LegacyTelemetryMessageV2
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

struct LegacyPersistentTelemetryRecordV2
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t recordId;
    LegacyTelemetryMessageV2 message;
};

struct TelemetryMessage
{
    uint8_t address;
    uint8_t sequence;
    int16_t temperatureCentiCelsius;
    uint8_t status;
    uint8_t sensorFaults[SENSOR_COUNT];
    uint8_t faultDetailValid;
    uint8_t nodeSampleAgeValid;
    uint16_t reserved;
    uint32_t capturedAtMs;
    uint32_t nodeSampleAgeMs;
    uint64_t captureBootNonce;
    uint64_t sampledAtUnixMs;
};

struct PersistentTelemetryRecord
{
    uint32_t magic;
    uint16_t version;
    uint16_t recordSize;
    uint64_t queueSequence;
    char recordId[FILE_OUTBOX_RECORD_ID_LENGTH];
    TelemetryMessage message;
    uint32_t checksum;
};

struct LegacyMigrationRecord
{
    bool used;
    uint16_t slotIndex;
    uint64_t legacyRecordId;
    TelemetryMessage message;
};

esp_mqtt_client_handle_t g_mqttClient = nullptr;
TaskHandle_t g_mqttTaskHandle = nullptr;
SemaphoreHandle_t g_outboxMutex = nullptr;

static uint32_t s_outboxCount = 0U;
static uint64_t s_oldestQueueSequence = 0ULL;
static uint64_t s_newestQueueSequence = 0ULL;
static uint64_t s_nextQueueSequence = 1ULL;
static uint64_t s_bootNonce = 0ULL;
static char s_gatewayDeviceId[13] = {0};
static bool s_mqttInitialized = false;
static bool s_wifiWasConnected = false;
static bool s_timeSyncConfigured = false;
static bool s_timeSyncAnnounced = false;
static unsigned long s_lastWiFiAttemptMs = 0UL;
static unsigned long s_lastTimeStatusLogMs = 0UL;

static volatile bool s_mqttConnected = false;

static uint8_t s_currentNodeIndex = 0U;
static uint8_t s_nextSequence[NUM_NODES] = {0U};
static uint32_t s_crcFailureCount = 0UL;
static uint32_t s_invalidLengthCount = 0UL;
static uint32_t s_sequenceMismatchCount = 0UL;
static uint32_t s_duplicateDataCount = 0UL;
static uint32_t s_faultDetailMismatchCount = 0UL;

/**
 * @brief  Builds the legacy NVS key used by one v5 outbox slot.
 * @param  slotIndex: Legacy outbox slot index.
 * @param  keyBuffer: Output buffer for the NVS key.
 * @param  keyBufferSize: Size of keyBuffer.
 * @retval true when the key was created successfully, otherwise false.
 */
static bool BuildLegacyNvsSlotKey(uint16_t slotIndex,
    char *keyBuffer,
    size_t keyBufferSize)
{
    if ((keyBuffer == nullptr) ||
        (keyBufferSize < 5U) ||
        (slotIndex >= LEGACY_NVS_OUTBOX_CAPACITY))
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
 * @brief  Starts SNTP without blocking LoRa acquisition.
 * @note   SNTP is configured only after Wi-Fi becomes available.
 * @retval None
 */
static void ConfigureSystemTimeIfNeeded(void)
{
    if (s_timeSyncConfigured)
    {
        return;
    }

    configTime(0, 0, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
    s_timeSyncConfigured = true;
    s_lastTimeStatusLogMs = millis();
    Serial.println("SNTP configured in background");
}

/**
 * @brief  Logs when a valid wall clock becomes available.
 * @retval None
 */
static void MonitorSystemTime(void)
{
    if (!s_timeSyncConfigured || s_timeSyncAnnounced)
    {
        return;
    }

    if (GetUnixTimeMs() != 0ULL)
    {
        s_timeSyncAnnounced = true;
        Serial.println("System time synchronized");
        return;
    }

    if ((millis() - s_lastTimeStatusLogMs) >= TIME_SYNC_STATUS_LOG_INTERVAL_MS)
    {
        s_lastTimeStatusLogMs = millis();
        LOGI("System time still unsynchronized; sample age is preserved durably\n");
    }
}

/**
 * @brief  Calculates CRC32 for one persistent record.
 * @param  data: Input bytes.
 * @param  length: Number of bytes to process.
 * @retval IEEE CRC32 value.
 */
static uint32_t CalculateCrc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t byteIndex = 0U; byteIndex < length; byteIndex++)
    {
        crc ^= (uint32_t)data[byteIndex];

        for (uint8_t bitIndex = 0U; bitIndex < 8U; bitIndex++)
        {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

/**
 * @brief  Creates a stable gateway device identifier and a per-boot random nonce.
 * @retval true when the factory MAC was read successfully, otherwise false.
 */
static bool InitializeGatewayIdentity(void)
{
    uint8_t baseMac[6] = {0U};

    if (esp_efuse_mac_get_default(baseMac) != ESP_OK)
    {
        Serial.println("Unable to read ESP32 eFuse MAC");
        return false;
    }

    snprintf(s_gatewayDeviceId,
        sizeof(s_gatewayDeviceId),
        "%02x%02x%02x%02x%02x%02x",
        baseMac[0],
        baseMac[1],
        baseMac[2],
        baseMac[3],
        baseMac[4],
        baseMac[5]);

    s_bootNonce = ((uint64_t)esp_random() << 32U) | (uint64_t)esp_random();

    if (s_bootNonce == 0ULL)
    {
        s_bootNonce = 1ULL;
    }

    LOGI("Gateway identity=%s bootNonce=%016llx\n",
        s_gatewayDeviceId,
        (unsigned long long)s_bootNonce);

    return true;
}

/**
 * @brief  Checks whether the default filesystem partition is still erased.
 * @note   This is used to distinguish first-time initialization from a damaged
 *         existing filesystem. Existing non-blank data is never auto-formatted.
 * @retval true when every byte is 0xFF, otherwise false.
 */
static bool IsLittleFsPartitionBlank(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        "spiffs");

    if (partition == nullptr)
    {
        return false;
    }

    uint8_t buffer[256];

    for (size_t offset = 0U; offset < partition->size; offset += sizeof(buffer))
    {
        const size_t bytesToRead =
            ((partition->size - offset) < sizeof(buffer))
                ? (partition->size - offset)
                : sizeof(buffer);

        if (esp_partition_read(partition, offset, buffer, bytesToRead) != ESP_OK)
        {
            return false;
        }

        for (size_t byteIndex = 0U; byteIndex < bytesToRead; byteIndex++)
        {
            if (buffer[byteIndex] != 0xFFU)
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief  Mounts the LittleFS telemetry partition without silently erasing data.
 * @retval true when LittleFS is ready, otherwise false.
 */
static bool MountTelemetryFilesystem(void)
{
    if (!LittleFS.begin(false))
    {
        if (!IsLittleFsPartitionBlank())
        {
            Serial.println("LittleFS mount failed on non-blank partition; refusing auto-format");
            return false;
        }

        Serial.println("LittleFS partition is blank; formatting first-use filesystem");

        if (!LittleFS.begin(true))
        {
            Serial.println("LittleFS first-use format/mount failed");
            return false;
        }
    }

    if (!LittleFS.exists(FILE_OUTBOX_DIRECTORY) &&
        !LittleFS.mkdir(FILE_OUTBOX_DIRECTORY))
    {
        Serial.println("Unable to create LittleFS outbox directory");
        return false;
    }

    return true;
}

/**
 * @brief  Builds one LittleFS path from a queue sequence.
 * @param  queueSequence: Monotonic sequence of the durable record.
 * @param  temporary: true for the pre-commit temporary file.
 * @param  pathBuffer: Output path buffer.
 * @param  pathBufferSize: Size of pathBuffer.
 * @retval true when the path fits the output buffer, otherwise false.
 */
static bool BuildOutboxRecordPath(uint64_t queueSequence,
    bool temporary,
    char *pathBuffer,
    size_t pathBufferSize)
{
    if ((pathBuffer == nullptr) || (pathBufferSize < 40U))
    {
        return false;
    }

    const int written = snprintf(pathBuffer,
        pathBufferSize,
        "%s/q%020llu.%s",
        FILE_OUTBOX_DIRECTORY,
        (unsigned long long)queueSequence,
        temporary ? "tmp" : "rec");

    return (written > 0) && ((size_t)written < pathBufferSize);
}

/**
 * @brief  Verifies the integrity and schema of one file-backed record.
 * @param  record: Record to validate.
 * @retval true when the record is valid, otherwise false.
 */
static bool ValidatePersistentRecord(const PersistentTelemetryRecord &record)
{
    if ((record.magic != FILE_OUTBOX_RECORD_MAGIC) ||
        (record.version != FILE_OUTBOX_RECORD_VERSION) ||
        (record.recordSize != sizeof(PersistentTelemetryRecord)) ||
        (record.queueSequence == 0ULL) ||
        (record.recordId[0] == '\0'))
    {
        return false;
    }

    const uint32_t expectedChecksum = CalculateCrc32(
        (const uint8_t *)&record,
        offsetof(PersistentTelemetryRecord, checksum));

    return expectedChecksum == record.checksum;
}

/**
 * @brief  Reads and validates one LittleFS persistent record.
 * @param  path: Record file path.
 * @param  record: Output record.
 * @retval true when the file contains a valid record, otherwise false.
 */
static bool ReadPersistentRecordFile(const char *path,
    PersistentTelemetryRecord &record)
{
    File file = LittleFS.open(path, FILE_READ);

    if (!file || file.isDirectory() ||
        (file.size() != sizeof(PersistentTelemetryRecord)))
    {
        if (file)
        {
            file.close();
        }

        return false;
    }

    record = {};
    const size_t bytesRead = file.read(
        (uint8_t *)&record,
        sizeof(PersistentTelemetryRecord));
    file.close();

    return (bytesRead == sizeof(PersistentTelemetryRecord)) &&
        ValidatePersistentRecord(record);
}

/**
 * @brief  Writes one durable LittleFS record using temp-file then rename commit.
 * @param  record: Fully populated record with checksum.
 * @retval true only after the final file can be read and validated.
 */
static bool WritePersistentRecordFile(const PersistentTelemetryRecord &record)
{
    char temporaryPath[64];
    char finalPath[64];

    if (!BuildOutboxRecordPath(record.queueSequence,
            true,
            temporaryPath,
            sizeof(temporaryPath)) ||
        !BuildOutboxRecordPath(record.queueSequence,
            false,
            finalPath,
            sizeof(finalPath)))
    {
        return false;
    }

    LittleFS.remove(temporaryPath);

    if (LittleFS.exists(finalPath))
    {
        return false;
    }

    File file = LittleFS.open(temporaryPath, FILE_WRITE);

    if (!file)
    {
        return false;
    }

    const size_t bytesWritten = file.write(
        (const uint8_t *)&record,
        sizeof(PersistentTelemetryRecord));
    file.flush();
    file.close();

    if (bytesWritten != sizeof(PersistentTelemetryRecord))
    {
        LittleFS.remove(temporaryPath);
        return false;
    }

    if (!LittleFS.rename(temporaryPath, finalPath))
    {
        LittleFS.remove(temporaryPath);
        return false;
    }

    PersistentTelemetryRecord verificationRecord = {};

    if (!ReadPersistentRecordFile(finalPath, verificationRecord) ||
        (strncmp(verificationRecord.recordId,
            record.recordId,
            FILE_OUTBOX_RECORD_ID_LENGTH) != 0))
    {
        Serial.println("LittleFS outbox verify-after-commit failed");
        return false;
    }

    return true;
}

/**
 * @brief  Scans the LittleFS outbox and restores FIFO boundaries after reboot.
 * @retval true when every committed record is valid, otherwise false.
 */
static bool ScanPersistentOutbox(void)
{
    File directory = LittleFS.open(FILE_OUTBOX_DIRECTORY);

    if (!directory || !directory.isDirectory())
    {
        return false;
    }

    uint32_t count = 0U;
    uint64_t oldest = UINT64_MAX;
    uint64_t newest = 0ULL;
    File file = directory.openNextFile();

    while (file)
    {
        char entryPath[96];
        snprintf(entryPath, sizeof(entryPath), "%s", file.path());
        const bool isDirectory = file.isDirectory();
        const size_t fileSize = file.size();
        file.close();

        const size_t pathLength = strlen(entryPath);

        if (!isDirectory &&
            (pathLength >= 4U) &&
            (strcmp(entryPath + pathLength - 4U, ".tmp") == 0))
        {
            LittleFS.remove(entryPath);
        }
        else if (!isDirectory &&
            (pathLength >= 4U) &&
            (strcmp(entryPath + pathLength - 4U, ".rec") == 0))
        {
            if (fileSize != sizeof(PersistentTelemetryRecord))
            {
                Serial.printf("Invalid outbox file size: %s\n", entryPath);
                directory.close();
                return false;
            }

            PersistentTelemetryRecord record = {};

            if (!ReadPersistentRecordFile(entryPath, record))
            {
                Serial.printf("Corrupt outbox record: %s\n", entryPath);
                directory.close();
                return false;
            }

            count++;

            if (record.queueSequence < oldest)
            {
                oldest = record.queueSequence;
            }

            if (record.queueSequence > newest)
            {
                newest = record.queueSequence;
            }
        }

        file = directory.openNextFile();
    }

    directory.close();

    s_outboxCount = count;
    s_oldestQueueSequence = (count > 0U) ? oldest : 0ULL;
    s_newestQueueSequence = (count > 0U) ? newest : 0ULL;
    s_nextQueueSequence = (count > 0U) ? (newest + 1ULL) : 1ULL;

    return true;
}

/**
 * @brief  Checks whether a record ID already exists in the LittleFS outbox.
 * @note   Used only by one-time NVS migration, so directory scanning is acceptable.
 * @param  recordId: Globally unique record ID.
 * @retval true when the record already exists, otherwise false.
 */
static bool PersistentOutboxContainsRecordId(const char *recordId)
{
    File directory = LittleFS.open(FILE_OUTBOX_DIRECTORY);

    if (!directory || !directory.isDirectory())
    {
        return false;
    }

    bool found = false;
    File file = directory.openNextFile();

    while (file)
    {
        char entryPath[96];
        snprintf(entryPath, sizeof(entryPath), "%s", file.path());
        const bool isDirectory = file.isDirectory();
        file.close();

        const size_t pathLength = strlen(entryPath);

        if (!isDirectory &&
            (pathLength >= 4U) &&
            (strcmp(entryPath + pathLength - 4U, ".rec") == 0))
        {
            PersistentTelemetryRecord record = {};

            if (ReadPersistentRecordFile(entryPath, record) &&
                (strncmp(record.recordId,
                    recordId,
                    FILE_OUTBOX_RECORD_ID_LENGTH) == 0))
            {
                found = true;
                break;
            }
        }

        file = directory.openNextFile();
    }

    directory.close();
    return found;
}

/**
 * @brief  Generates the globally unique string ID used end-to-end.
 * @param  queueSequence: Local FIFO sequence number.
 * @param  recordId: Output ID buffer.
 * @param  recordIdSize: Size of recordId.
 * @retval true when the ID fits the supplied buffer.
 */
static bool GenerateRecordId(uint64_t queueSequence,
    char *recordId,
    size_t recordIdSize)
{
    const int written = snprintf(recordId,
        recordIdSize,
        "gw-%s-%016llx-%016llx",
        s_gatewayDeviceId,
        (unsigned long long)s_bootNonce,
        (unsigned long long)queueSequence);

    return (written > 0) && ((size_t)written < recordIdSize);
}

/**
 * @brief  Generates a deterministic ID for a record migrated from the legacy NVS outbox.
 * @param  legacyRecordId: Previous numeric record ID.
 * @param  recordId: Output ID buffer.
 * @param  recordIdSize: Size of recordId.
 * @retval true when the ID fits the supplied buffer.
 */
static bool GenerateLegacyMigrationRecordId(uint64_t legacyRecordId,
    char *recordId,
    size_t recordIdSize)
{
    const int written = snprintf(recordId,
        recordIdSize,
        "gw-%s-legacy-%016llx",
        s_gatewayDeviceId,
        (unsigned long long)legacyRecordId);

    return (written > 0) && ((size_t)written < recordIdSize);
}

/**
 * @brief  Adds one record to LittleFS before the STM32 receives an ACK.
 * @param  message: Telemetry message to persist.
 * @param  forcedRecordId: Optional deterministic ID used during migration.
 * @retval true only when the final file has been committed and verified.
 */
static bool PersistentOutboxPushInternal(const TelemetryMessage &message,
    const char *forcedRecordId)
{
    if (xSemaphoreTake(g_outboxMutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    const size_t totalBytes = LittleFS.totalBytes();
    const size_t usedBytes = LittleFS.usedBytes();
    const size_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0U;

    if ((s_outboxCount >= FILE_OUTBOX_MAX_RECORDS) ||
        (freeBytes < (FILE_OUTBOX_MIN_FREE_BYTES + sizeof(PersistentTelemetryRecord))))
    {
        xSemaphoreGive(g_outboxMutex);
        Serial.printf("LittleFS outbox full: count=%u free=%u\n",
            (unsigned int)s_outboxCount,
            (unsigned int)freeBytes);
        return false;
    }

    PersistentTelemetryRecord record = {};
    record.magic = FILE_OUTBOX_RECORD_MAGIC;
    record.version = FILE_OUTBOX_RECORD_VERSION;
    record.recordSize = sizeof(PersistentTelemetryRecord);
    record.queueSequence = s_nextQueueSequence;
    record.message = message;

    bool idReady = false;

    if (forcedRecordId != nullptr)
    {
        const int written = snprintf(record.recordId,
            sizeof(record.recordId),
            "%s",
            forcedRecordId);
        idReady = (written >= 0) && ((size_t)written < sizeof(record.recordId));
    }
    else
    {
        idReady = GenerateRecordId(record.queueSequence,
            record.recordId,
            sizeof(record.recordId));
    }

    if (!idReady)
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    record.checksum = CalculateCrc32(
        (const uint8_t *)&record,
        offsetof(PersistentTelemetryRecord, checksum));

    if (!WritePersistentRecordFile(record))
    {
        xSemaphoreGive(g_outboxMutex);
        Serial.println("LittleFS outbox commit failed");
        return false;
    }

    if (s_outboxCount == 0U)
    {
        s_oldestQueueSequence = record.queueSequence;
    }

    s_newestQueueSequence = record.queueSequence;
    s_nextQueueSequence = record.queueSequence + 1ULL;
    s_outboxCount++;

    LOGI("[OUTBOX+] id=%s q=%llu count=%u free=%u\n",
        record.recordId,
        (unsigned long long)record.queueSequence,
        (unsigned int)s_outboxCount,
        (unsigned int)(LittleFS.totalBytes() - LittleFS.usedBytes()));

    xSemaphoreGive(g_outboxMutex);
    return true;
}

/**
 * @brief  Persists one normal telemetry record before acknowledging the node.
 * @param  message: Telemetry message to store.
 * @retval true after durable LittleFS commit, otherwise false.
 */
static bool PersistentOutboxPush(const TelemetryMessage &message)
{
    return PersistentOutboxPushInternal(message, nullptr);
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

    if ((s_outboxCount == 0U) || (s_oldestQueueSequence == 0ULL))
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    char path[64];
    const bool pathReady = BuildOutboxRecordPath(
        s_oldestQueueSequence,
        false,
        path,
        sizeof(path));
    const bool success = pathReady && ReadPersistentRecordFile(path, record);

    if (!success)
    {
        Serial.println("LittleFS outbox head read failed");
    }

    xSemaphoreGive(g_outboxMutex);
    return success;
}

/**
 * @brief  Removes the oldest record only after broker PUBACK.
 * @param  expectedRecordId: String ID of the acknowledged record.
 * @retval true when the committed file has been removed, otherwise false.
 */
static bool PersistentOutboxPop(const char *expectedRecordId)
{
    if ((expectedRecordId == nullptr) ||
        (xSemaphoreTake(g_outboxMutex, portMAX_DELAY) != pdTRUE))
    {
        return false;
    }

    if ((s_outboxCount == 0U) || (s_oldestQueueSequence == 0ULL))
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    char path[64];
    PersistentTelemetryRecord record = {};

    if (!BuildOutboxRecordPath(s_oldestQueueSequence,
            false,
            path,
            sizeof(path)) ||
        !ReadPersistentRecordFile(path, record) ||
        (strncmp(record.recordId,
            expectedRecordId,
            FILE_OUTBOX_RECORD_ID_LENGTH) != 0))
    {
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    if (!LittleFS.remove(path))
    {
        Serial.println("LittleFS outbox remove failed");
        xSemaphoreGive(g_outboxMutex);
        return false;
    }

    const uint64_t removedSequence = s_oldestQueueSequence;

    if (s_outboxCount > 0U)
    {
        s_outboxCount--;
    }

    if (s_outboxCount == 0U)
    {
        s_oldestQueueSequence = 0ULL;
        s_newestQueueSequence = 0ULL;
    }
    else
    {
        uint64_t nextSequence = removedSequence + 1ULL;
        bool nextFound = false;

        while (nextSequence <= s_newestQueueSequence)
        {
            char nextPath[64];

            if (BuildOutboxRecordPath(nextSequence,
                    false,
                    nextPath,
                    sizeof(nextPath)) &&
                LittleFS.exists(nextPath))
            {
                s_oldestQueueSequence = nextSequence;
                nextFound = true;
                break;
            }

            nextSequence++;
        }

        if (!nextFound)
        {
            Serial.println("LittleFS outbox FIFO index inconsistent");
            xSemaphoreGive(g_outboxMutex);
            return false;
        }
    }

    LOGI("[OUTBOX-] id=%s q=%llu count=%u\n",
        expectedRecordId,
        (unsigned long long)removedSequence,
        (unsigned int)s_outboxCount);

    xSemaphoreGive(g_outboxMutex);
    return true;
}

/**
 * @brief  Reads one v1/v2 record from the former NVS outbox for one-time migration.
 * @param  nvsHandle: Open legacy namespace handle.
 * @param  slotIndex: Legacy slot index.
 * @param  migrationRecord: Normalized migration result.
 * @retval true when a valid supported record was read, otherwise false.
 */
static bool ReadLegacyNvsMigrationRecord(nvs_handle_t nvsHandle,
    uint16_t slotIndex,
    LegacyMigrationRecord &migrationRecord)
{
    char slotKey[8];
    size_t storedSize = 0U;

    if (!BuildLegacyNvsSlotKey(slotIndex, slotKey, sizeof(slotKey)))
    {
        return false;
    }

    esp_err_t result = nvs_get_blob(nvsHandle, slotKey, nullptr, &storedSize);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        migrationRecord = {};
        return true;
    }

    if (result != ESP_OK)
    {
        return false;
    }

    migrationRecord = {};
    migrationRecord.used = true;
    migrationRecord.slotIndex = slotIndex;

    if (storedSize == sizeof(LegacyPersistentTelemetryRecordV2))
    {
        LegacyPersistentTelemetryRecordV2 legacy = {};
        size_t readSize = sizeof(legacy);
        result = nvs_get_blob(nvsHandle, slotKey, &legacy, &readSize);

        if ((result != ESP_OK) ||
            (readSize != sizeof(legacy)) ||
            (legacy.magic != LEGACY_PERSISTENT_RECORD_MAGIC) ||
            (legacy.version != LEGACY_PERSISTENT_RECORD_VERSION_V2))
        {
            return false;
        }

        migrationRecord.legacyRecordId = legacy.recordId;
        migrationRecord.message.address = legacy.message.address;
        migrationRecord.message.sequence = legacy.message.sequence;
        migrationRecord.message.temperatureCentiCelsius =
            legacy.message.temperatureCentiCelsius;
        migrationRecord.message.status = legacy.message.status;
        memcpy(migrationRecord.message.sensorFaults,
            legacy.message.sensorFaults,
            SENSOR_COUNT);
        migrationRecord.message.faultDetailValid = legacy.message.faultDetailValid;
        migrationRecord.message.sampledAtUnixMs = legacy.message.sampledAtUnixMs;
        migrationRecord.message.captureBootNonce = 0ULL;
        migrationRecord.message.nodeSampleAgeValid = 0U;
        return true;
    }

    if (storedSize == sizeof(LegacyPersistentTelemetryRecordV1))
    {
        LegacyPersistentTelemetryRecordV1 legacy = {};
        size_t readSize = sizeof(legacy);
        result = nvs_get_blob(nvsHandle, slotKey, &legacy, &readSize);

        if ((result != ESP_OK) ||
            (readSize != sizeof(legacy)) ||
            (legacy.magic != LEGACY_PERSISTENT_RECORD_MAGIC) ||
            (legacy.version != LEGACY_PERSISTENT_RECORD_VERSION_V1))
        {
            return false;
        }

        migrationRecord.legacyRecordId = legacy.recordId;
        migrationRecord.message.address = legacy.message.address;
        migrationRecord.message.sequence = legacy.message.sequence;
        migrationRecord.message.temperatureCentiCelsius =
            legacy.message.temperatureCentiCelsius;
        migrationRecord.message.status = legacy.message.status;
        migrationRecord.message.faultDetailValid = 0U;
        migrationRecord.message.sampledAtUnixMs = legacy.message.sampledAtUnixMs;
        migrationRecord.message.captureBootNonce = 0ULL;
        migrationRecord.message.nodeSampleAgeValid = 0U;
        return true;
    }

    return false;
}

/**
 * @brief  Migrates queued v5 NVS records into LittleFS without duplicate IDs.
 * @note   NVS is used only for this one-time migration and not for new telemetry.
 * @retval true when no legacy data remains un-migrated, otherwise false.
 */
static bool MigrateLegacyNvsOutbox(void)
{
    esp_err_t result = nvs_flash_init();

    if ((result != ESP_OK) &&
        (result != ESP_ERR_NVS_NO_FREE_PAGES) &&
        (result != ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        Serial.printf("Legacy NVS init failed: %d\n", (int)result);
        return false;
    }

    if ((result == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (result == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        Serial.println("Legacy NVS requires erase; refusing because queued records may exist");
        return false;
    }

    nvs_handle_t nvsHandle = 0;
    result = nvs_open(LEGACY_NVS_OUTBOX_NAMESPACE, NVS_READWRITE, &nvsHandle);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        return true;
    }

    if (result != ESP_OK)
    {
        Serial.printf("Legacy outbox namespace open failed: %d\n", (int)result);
        return false;
    }

    LegacyMigrationRecord records[LEGACY_NVS_OUTBOX_CAPACITY] = {};
    uint16_t recordCount = 0U;

    for (uint16_t slotIndex = 0U;
         slotIndex < LEGACY_NVS_OUTBOX_CAPACITY;
         slotIndex++)
    {
        LegacyMigrationRecord record = {};

        if (!ReadLegacyNvsMigrationRecord(nvsHandle, slotIndex, record))
        {
            Serial.printf("Legacy NVS slot %u is unreadable; migration stopped\n",
                (unsigned int)slotIndex);
            nvs_close(nvsHandle);
            return false;
        }

        if (record.used)
        {
            records[recordCount++] = record;
        }
    }

    for (uint16_t left = 0U; left < recordCount; left++)
    {
        for (uint16_t right = (uint16_t)(left + 1U); right < recordCount; right++)
        {
            if (records[right].legacyRecordId < records[left].legacyRecordId)
            {
                const LegacyMigrationRecord temporary = records[left];
                records[left] = records[right];
                records[right] = temporary;
            }
        }
    }

    for (uint16_t recordIndex = 0U; recordIndex < recordCount; recordIndex++)
    {
        LegacyMigrationRecord &legacy = records[recordIndex];
        char migratedRecordId[FILE_OUTBOX_RECORD_ID_LENGTH];

        if (!GenerateLegacyMigrationRecordId(legacy.legacyRecordId,
                migratedRecordId,
                sizeof(migratedRecordId)))
        {
            nvs_close(nvsHandle);
            return false;
        }

        if (!PersistentOutboxContainsRecordId(migratedRecordId) &&
            !PersistentOutboxPushInternal(legacy.message, migratedRecordId))
        {
            Serial.println("Legacy NVS -> LittleFS migration failed");
            nvs_close(nvsHandle);
            return false;
        }

        char slotKey[8];
        BuildLegacyNvsSlotKey(legacy.slotIndex, slotKey, sizeof(slotKey));
        result = nvs_erase_key(nvsHandle, slotKey);

        if (result == ESP_OK)
        {
            result = nvs_commit(nvsHandle);
        }

        if (result != ESP_OK)
        {
            Serial.printf("Unable to clear migrated legacy slot %u: %d\n",
                (unsigned int)legacy.slotIndex,
                (int)result);
            nvs_close(nvsHandle);
            return false;
        }
    }

    if (recordCount > 0U)
    {
        nvs_erase_key(nvsHandle, LEGACY_NVS_OUTBOX_BOOT_KEY);
        nvs_commit(nvsHandle);
        Serial.printf("Migrated %u legacy NVS telemetry records to LittleFS\n",
            (unsigned int)recordCount);
    }

    nvs_close(nvsHandle);
    return true;
}

/**
 * @brief  Initializes the LittleFS durable outbox and migrates legacy NVS records.
 * @retval true when the outbox is safe to use, otherwise false.
 */
static bool InitializePersistentOutbox(void)
{
    if (!InitializeGatewayIdentity() || !MountTelemetryFilesystem())
    {
        return false;
    }

    g_outboxMutex = xSemaphoreCreateMutex();

    if (g_outboxMutex == nullptr)
    {
        return false;
    }

    if (!ScanPersistentOutbox())
    {
        return false;
    }

    if (!MigrateLegacyNvsOutbox())
    {
        return false;
    }

    Serial.printf(
        "LittleFS outbox ready: restored=%u max=%u used=%u/%u bytes device=%s\n",
        (unsigned int)s_outboxCount,
        (unsigned int)FILE_OUTBOX_MAX_RECORDS,
        (unsigned int)LittleFS.usedBytes(),
        (unsigned int)LittleFS.totalBytes(),
        s_gatewayDeviceId);

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
 * @brief  Advances Wi-Fi connection state without blocking the LoRa task.
 * @note   Connection attempts are rate-limited; this function always returns quickly.
 * @retval true when Wi-Fi is connected, otherwise false.
 */
static bool MaintainWiFiConnection(void)
{
    const bool connected = WiFi.status() == WL_CONNECTED;

    if (connected)
    {
        if (!s_wifiWasConnected)
        {
            s_wifiWasConnected = true;
            Serial.print("WiFi connected. IP: ");
            Serial.println(WiFi.localIP());
        }

        ConfigureSystemTimeIfNeeded();
        MonitorSystemTime();
        return true;
    }

    if (s_wifiWasConnected)
    {
        s_wifiWasConnected = false;
        s_mqttConnected = false;
        Serial.println("WiFi disconnected; LoRa acquisition continues offline");
    }

    const unsigned long nowMs = millis();

    if ((s_lastWiFiAttemptMs == 0UL) ||
        ((nowMs - s_lastWiFiAttemptMs) >= WIFI_RECONNECT_INTERVAL_MS))
    {
        s_lastWiFiAttemptMs = nowMs;
        Serial.println("WiFi reconnect attempt...");

        if (WiFi.getMode() != WIFI_STA)
        {
            WiFi.mode(WIFI_STA);
        }

        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    return false;
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
        esp_mqtt_client_destroy(g_mqttClient);
        g_mqttClient = nullptr;
        return false;
    }

    if (esp_mqtt_client_start(g_mqttClient) != ESP_OK)
    {
        Serial.println("MQTT client start failed");
        esp_mqtt_client_destroy(g_mqttClient);
        g_mqttClient = nullptr;
        return false;
    }

    return true;
}

/**
 * @brief  Enqueues one durable telemetry record for MQTT QoS 1 delivery.
 * @note   Record IDs are JSON strings so JavaScript never loses uint64 precision.
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
    char telemetryPayload[512];
    char timestampText[32];
    char ageText[24];
    char faultArrayText[64];
    const bool recovered = message.captureBootNonce != s_bootNonce;
    const uint64_t currentUnixMs = GetUnixTimeMs();
    uint64_t effectiveSampledAtUnixMs = message.sampledAtUnixMs;
    uint64_t effectiveAgeMs = 0ULL;
    bool ageValid = false;
    bool timestampValid = false;

    if (effectiveSampledAtUnixMs != 0ULL)
    {
        timestampValid = true;

        if (currentUnixMs >= effectiveSampledAtUnixMs)
        {
            effectiveAgeMs = currentUnixMs - effectiveSampledAtUnixMs;
            ageValid = effectiveAgeMs <= 0xFFFFFFFFULL;
        }
    }
    else if (!recovered)
    {
        effectiveAgeMs = (uint64_t)((uint32_t)(millis() - message.capturedAtMs));

        if (message.nodeSampleAgeValid != 0U)
        {
            effectiveAgeMs += (uint64_t)message.nodeSampleAgeMs +
                (uint64_t)TIMESTAMPED_DATA_AIRTIME_MS;
        }

        ageValid = effectiveAgeMs <= 0xFFFFFFFFULL;

        if (ageValid &&
            (currentUnixMs != 0ULL) &&
            (currentUnixMs >= effectiveAgeMs))
        {
            effectiveSampledAtUnixMs = currentUnixMs - effectiveAgeMs;
            timestampValid = true;
        }
        else if (ageValid)
        {
            /* Backend wall clock can still reconstruct sampledAt from ageMs. */
            timestampValid = true;
        }
    }

    if (effectiveSampledAtUnixMs != 0ULL)
    {
        snprintf(timestampText,
            sizeof(timestampText),
            "%llu",
            (unsigned long long)effectiveSampledAtUnixMs);
    }
    else
    {
        snprintf(timestampText, sizeof(timestampText), "null");
    }

    if (ageValid)
    {
        snprintf(ageText,
            sizeof(ageText),
            "%lu",
            (unsigned long)effectiveAgeMs);
    }
    else
    {
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
        "{\"id\":\"%s\",\"seq\":%u,\"temp\":%s,\"tempValid\":%s,"
        "\"status\":%u,\"faultDetailValid\":%s,\"faults\":%s,"
        "\"sampledAtMs\":%s,\"ageMs\":%s,"
        "\"timestampValid\":%s,\"recovered\":%s}",
        record.recordId,
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
        LOGI("[MQTT-QOS1-TX] %s id=%s seq=%u msg_id=%d payload=%s\n",
            nodeName,
            record.recordId,
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
 * @retval true when the record is durably committed to LittleFS, otherwise false.
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
    message.nodeSampleAgeValid = nodeSampleAgeValid ? 1U : 0U;
    message.nodeSampleAgeMs = nodeSampleAgeMs;
    message.capturedAtMs = capturedAtMs;
    message.captureBootNonce = s_bootNonce;

    if (faultDetailValid)
    {
        for (uint8_t sensorIndex = 0U;
             sensorIndex < SENSOR_COUNT;
             sensorIndex++)
        {
            message.sensorFaults[sensorIndex] = sensorFaults[sensorIndex];
        }
    }

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
    else if (!nodeSampleAgeValid)
    {
        /* Legacy payloads can only be timestamped at gateway reception. */
        message.sampledAtUnixMs = receivedAtUnixMs;
    }
    else
    {
        /* Keep nodeSampleAgeMs in flash; reconstruct when NTP becomes valid. */
        message.sampledAtUnixMs = 0ULL;
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
 * @brief  Maintains Wi-Fi and delivers LittleFS records with MQTT QoS 1.
 * @note   Wi-Fi/MQTT failures never block the independent LoRa acquisition task.
 * @note   A telemetry record is removed only after broker PUBACK is received.
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
        const bool wifiConnected = MaintainWiFiConnection();

        if (wifiConnected && !s_mqttInitialized)
        {
            if (InitializeMqttClient())
            {
                s_mqttInitialized = true;
            }
            else
            {
                Serial.println("MQTT initialization failed; retrying without stopping LoRa");
                vTaskDelay(pdMS_TO_TICKS(MQTT_ENQUEUE_RETRY_DELAY_MS));
                continue;
            }
        }

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
            wifiConnected &&
            s_mqttInitialized &&
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
                        Serial.println("PUBACK received but LittleFS outbox removal failed");
                        vTaskDelay(pdMS_TO_TICKS(MQTT_ENQUEUE_RETRY_DELAY_MS));
                        continue;
                    }

                    pubAckCount++;

                    LOGI("[MQTT-PUBACK] id=%s seq=%u msg_id=%d count=%lu\n",
                        pendingRecord.recordId,
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
                (g_mqttClient != nullptr) &&
                (esp_mqtt_client_get_outbox_size(g_mqttClient) == 0))
            {
                Serial.printf("MQTT PUBACK watchdog expired for msg_id=%d; retrying durable record\n",
                    pendingMqttMessageId);

                waitingForPubAck = false;
                pendingMqttMessageId = -1;
            }

            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
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
 * @brief  Initializes LoRa, LittleFS durable outbox and independent FreeRTOS tasks.
 * @note   Wi-Fi is intentionally not awaited here; LoRa acquisition starts offline.
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

    if (!InitializePersistentOutbox())
    {
        Serial.println("Persistent outbox initialization failed");

        while (1)
        {
            delay(1000);
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    xTaskCreatePinnedToCore(LoraTask,
        "loraTask",
        6144,
        nullptr,
        1,
        nullptr,
        0);

    xTaskCreatePinnedToCore(MqttTask,
        "mqttTask",
        7168,
        nullptr,
        1,
        &g_mqttTaskHandle,
        1);

    Serial.println(
        "Gateway ready (protocol v10: offline-first LittleFS + MQTT QoS1 + string IDs)");
}

/**
 * @brief  Suspends the Arduino loop task because application work runs in FreeRTOS tasks.
 * @retval None
 */
void loop(void)
{
    vTaskDelay(portMAX_DELAY);
}
