#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

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

#define TEMPERATURE_PAYLOAD_LENGTH         3U
#define TEMPERATURE_INVALID_CENTI_C        ((int16_t)-32768)

#define DATA_WAIT_TIMEOUT_MS               1000UL
#define DUPLICATE_ACK_WINDOW_MS            700UL
#define NODE_CYCLE_DELAY_MS                1000UL
#define LORA_RECEIVE_POLL_DELAY_MS         5UL

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

const char *WIFI_SSID = "Tung Tung Tung";
const char *WIFI_PASSWORD = "04062004";
const char *MQTT_BROKER = "172.20.10.4";
const uint16_t MQTT_PORT = 1883U;
const char *MQTT_USER = "gateway-01";
const char *MQTT_PASSWORD = "ngochai2004";
const char *MQTT_CLIENT_ID = "esp32-gateway-01";

struct ProtocolPacket
{
    uint8_t address;
    uint8_t type;
    uint8_t sequence;
    uint8_t length;
    uint8_t data[MAX_LORA_PAYLOAD];
};

struct TelemetryMessage
{
    uint8_t address;
    int16_t temperatureCentiCelsius;
    uint8_t status;
};

WiFiClient g_wifiClient;
PubSubClient g_mqttClient(g_wifiClient);
QueueHandle_t g_telemetryQueue = nullptr;

static uint8_t s_currentNodeIndex = 0U;
static uint8_t s_nextSequence[NUM_NODES] = {0U};
static uint32_t s_crcFailureCount = 0UL;
static uint32_t s_invalidLengthCount = 0UL;
static uint32_t s_sequenceMismatchCount = 0UL;
static uint32_t s_duplicateDataCount = 0UL;

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
 * @brief  Ensures the MQTT client is connected to the configured broker.
 * @retval None
 */
static void EnsureMqttConnected(void)
{
    while (!g_mqttClient.connected())
    {
        if (g_mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD))
        {
            Serial.println("MQTT connected");
            return;
        }

        Serial.print("MQTT connect failed, rc=");
        Serial.println(g_mqttClient.state());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/**
 * @brief  Publishes node temperature and sensor status to MQTT.
 * @note   Invalid temperature marker 0x8000 is not published to temp_avg;
 *         status is still published so the backend retains the fault state.
 * @param  message: Telemetry message to publish.
 * @retval None
 */
static void PublishNodeData(const TelemetryMessage &message)
{
    const char *nodeName = GetNodeName(message.address);

    if (nodeName == nullptr)
    {
        return;
    }

    char temperatureTopic[32];
    char statusTopic[32];
    char temperaturePayload[20];
    char statusPayload[8];

    snprintf(temperatureTopic,
        sizeof(temperatureTopic),
        "iot/%s/temp_avg",
        nodeName);
    snprintf(statusTopic,
        sizeof(statusTopic),
        "iot/%s/status",
        nodeName);
    snprintf(statusPayload,
        sizeof(statusPayload),
        "%u",
        message.status);

    bool temperaturePublished = true;

    if (message.temperatureCentiCelsius != TEMPERATURE_INVALID_CENTI_C)
    {
        int32_t temperatureValue = message.temperatureCentiCelsius;
        const bool isNegative = temperatureValue < 0;

        if (isNegative)
        {
            temperatureValue = -temperatureValue;
        }

        snprintf(temperaturePayload,
            sizeof(temperaturePayload),
            "%s%ld.%02ld",
            isNegative ? "-" : "",
            (long)(temperatureValue / 100L),
            (long)(temperatureValue % 100L));

        temperaturePublished = g_mqttClient.publish(temperatureTopic,
            temperaturePayload,
            true);
    }

    const bool statusPublished = g_mqttClient.publish(statusTopic,
        statusPayload,
        true);

    if (!temperaturePublished || !statusPublished)
    {
        Serial.println("MQTT publish failed");
        return;
    }

    if (message.temperatureCentiCelsius == TEMPERATURE_INVALID_CENTI_C)
    {
        LOGI("[MQTT] %s temp=INVALID status=%u\n",
            nodeName,
            message.status);
    }
    else
    {
        LOGI("[MQTT] %s temp=%s status=%u\n",
            nodeName,
            temperaturePayload,
            message.status);
    }
}

/**
 * @brief  Pushes one decoded DATA packet to the telemetry queue.
 * @param  address: Node address.
 * @param  temperatureCentiCelsius: Signed temperature multiplied by 100.
 * @param  status: Six-bit sensor fault mask.
 * @retval 1 when queued successfully, otherwise 0.
 */
static bool EnqueueTelemetry(uint8_t address,
    int16_t temperatureCentiCelsius,
    uint8_t status)
{
    TelemetryMessage message =
    {
        address,
        temperatureCentiCelsius,
        status
    };

    if (xQueueSend(g_telemetryQueue, &message, 0) != pdPASS)
    {
        Serial.println("Telemetry queue full");
        return false;
    }

    return true;
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
            (packet.length == TEMPERATURE_PAYLOAD_LENGTH))
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
 * @brief  Maintains Wi-Fi/MQTT and publishes queued telemetry.
 * @param  parameter: FreeRTOS task parameter, unused.
 * @retval Never returns.
 */
static void MqttTask(void *parameter)
{
    (void)parameter;
    TelemetryMessage message;

    for (;;)
    {
        EnsureWiFiConnected();

        if (!g_mqttClient.connected())
        {
            EnsureMqttConnected();
        }

        g_mqttClient.loop();

        while (xQueueReceive(g_telemetryQueue, &message, 0) == pdPASS)
        {
            PublishNodeData(message);
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
                    (packet.length != TEMPERATURE_PAYLOAD_LENGTH))
                {
                    continue;
                }

                const int16_t temperatureCentiCelsius =
                    DecodeTemperatureCentiCelsius(packet.data);
                const uint8_t status = (uint8_t)(packet.data[2] & 0x3FU);

                EnqueueTelemetry(currentNodeAddress,
                    temperatureCentiCelsius,
                    status);

                SendAcknowledgement(currentNodeAddress, currentSequence);

                if (temperatureCentiCelsius == TEMPERATURE_INVALID_CENTI_C)
                {
                    LOGI("[DATA] node=%02X seq=%u temp=INVALID status=%u rssi=%d\n",
                        currentNodeAddress,
                        currentSequence,
                        status,
                        LoRa.packetRssi());
                }
                else
                {
                    LOGI("[DATA] node=%02X seq=%u temp_raw=%d status=%u rssi=%d\n",
                        currentNodeAddress,
                        currentSequence,
                        temperatureCentiCelsius,
                        status,
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
 * @brief  Initializes LoRa, MQTT queue and FreeRTOS tasks.
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

    g_mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    WiFi.mode(WIFI_STA);

    g_telemetryQueue = xQueueCreate(16, sizeof(TelemetryMessage));

    if (g_telemetryQueue == nullptr)
    {
        Serial.println("Queue create failed");

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
        nullptr,
        1);

    xTaskCreatePinnedToCore(LoraTask,
        "loraTask",
        6144,
        nullptr,
        1,
        nullptr,
        0);

    Serial.println("Gateway ready (protocol v2: SEQ + ACK/retry + fixed-point temperature)");
}

/**
 * @brief  Suspends the Arduino loop task because application work runs in FreeRTOS tasks.
 * @retval None
 */
void loop(void)
{
    vTaskDelay(portMAX_DELAY);
}
