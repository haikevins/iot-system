#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ===== PIN =====
#define SS    27
#define RST   14
#define DIO0  26

// ===== CONFIG =====
#define LORA_START_BYTE  0xAA

#define NODE1_ID  0x01
#define NODE2_ID  0x02
#define NODE3_ID  0x03

#define TYPE_DATA  0x01
#define TYPE_ACK   0x10
#define TYPE_POLL  0x23
#define ENABLE_DIAG_LOG 1

#if ENABLE_DIAG_LOG
  #define LOGI(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGI(...)
#endif

uint8_t nodes[] = {NODE1_ID, NODE2_ID, NODE3_ID};
#define NUM_NODES (sizeof(nodes) / sizeof(nodes[0]))

// ===== WIFI + MQTT CONFIG =====
const char* WIFI_SSID = "Tung Tung Tung";
const char* WIFI_PASSWORD = "04062004";

const char* MQTT_BROKER = "172.20.10.4";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "gateway-01";
const char* MQTT_PASSWORD = "ngochai2004";
const char* MQTT_CLIENT_ID = "esp32-gateway-01";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

struct TelemetryMessage {
  uint8_t addr;
  float temp;
  uint8_t status;
};

QueueHandle_t telemetryQueue = nullptr;
uint8_t nodeIndex = 0;
uint32_t crcFailCount = 0;
uint32_t invalidLenCount = 0;

const char* nodeNameFromAddr(uint8_t addr) {
  switch (addr) {
    case NODE1_ID: return "node01";
    case NODE2_ID: return "node02";
    case NODE3_ID: return "node03";
    default: return nullptr;
  }
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

void ensureMQTTConnected() {
  while (!mqttClient.connected()) {
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("MQTT connected");
      return;
    }
    Serial.print("MQTT connect failed, rc=");
    Serial.println(mqttClient.state());
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void publishNodeData(uint8_t addr, float temp, uint8_t status) {
  const char* nodeName = nodeNameFromAddr(addr);
  if (!nodeName) return;

  char topicTemp[32];
  char topicStatus[32];
  char tempPayload[16];
  char statusPayload[8];

  snprintf(topicTemp, sizeof(topicTemp), "iot/%s/temp_avg", nodeName);
  snprintf(topicStatus, sizeof(topicStatus), "iot/%s/status", nodeName);
  dtostrf(temp, 0, 2, tempPayload);
  snprintf(statusPayload, sizeof(statusPayload), "%u", status);

  char* tempValue = tempPayload;
  while (*tempValue == ' ') tempValue++;

  bool tempOk = mqttClient.publish(topicTemp, tempValue, true);
  bool statusOk = mqttClient.publish(topicStatus, statusPayload, true);

  if (!tempOk || !statusOk) {
    Serial.println("MQTT publish failed");
    return;
  }
  LOGI("[MQTT] %s temp=%.2f status=%u\n", nodeName, temp, status);
}

bool enqueueTelemetry(uint8_t addr, float temp, uint8_t status) {
  TelemetryMessage msg = {addr, temp, status};
  if (xQueueSend(telemetryQueue, &msg, 0) != pdPASS) {
    Serial.println("Telemetry queue full");
    return false;
  }
  return true;
}

// ================= CRC8 =================
uint8_t CRC8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  for(uint8_t i=0;i<len;i++){
    crc ^= data[i];
    for(uint8_t j=0;j<8;j++){
      if(crc & 0x80) crc = (crc << 1) ^ 0x07;
      else crc <<= 1;
    }
  }
  return crc;
}

// ================= SEND POLL =================
void sendPoll(uint8_t addr) {
  uint8_t tmp[3] = {addr, TYPE_POLL, 0};
  uint8_t crc = CRC8(tmp, 3);

  LoRa.beginPacket();
  LoRa.write(LORA_START_BYTE);
  LoRa.write(addr);
  LoRa.write(TYPE_POLL);
  LoRa.write((uint8_t)0);
  LoRa.write(crc);
  LoRa.endPacket();
  LOGI("[POLL] node=%02X\n", addr);
}

// ================= SEND ACK =================
void sendACK(uint8_t addr) {
  uint8_t tmp[3] = {addr, TYPE_ACK, 0};
  uint8_t crc = CRC8(tmp, 3);

  LoRa.beginPacket();
  LoRa.write(LORA_START_BYTE);
  LoRa.write(addr);
  LoRa.write(TYPE_ACK);
  LoRa.write((uint8_t)0);
  LoRa.write(crc);
  LoRa.endPacket();
  LOGI("[ACK] node=%02X\n", addr);
}

void mqttTask(void *pvParameters) {
  (void)pvParameters;
  TelemetryMessage msg;

  for (;;) {
    ensureWiFiConnected();
    if (!mqttClient.connected()) ensureMQTTConnected();
    mqttClient.loop();

    while (xQueueReceive(telemetryQueue, &msg, 0) == pdPASS) {
      publishNodeData(msg.addr, msg.temp, msg.status);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void loraTask(void *pvParameters) {
  (void)pvParameters;

  for (;;) {
    uint8_t currentNode = nodes[nodeIndex];

    // ===== SEND POLL =====
    sendPoll(currentNode);

    unsigned long start = millis();
    bool gotData = false;

    while (millis() - start < 1000) {
      int packetSize = LoRa.parsePacket();
      if (!packetSize) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      uint8_t startByte = LoRa.read();
      if (startByte != LORA_START_BYTE) {
        continue;
      }

      uint8_t addr = LoRa.read();
      uint8_t type = LoRa.read();
      uint8_t len  = LoRa.read();

      if (len > 64) {
        invalidLenCount++;
        if (invalidLenCount % 20 == 1) {
          LOGI("[WARN] Invalid LEN count=%lu\n", (unsigned long)invalidLenCount);
        }
        while (LoRa.available()) {
          LoRa.read();
        }
        continue;
      }

      uint8_t data[64];
      for (uint8_t i = 0; i < len; i++) {
        if (!LoRa.available()) break;
        data[i] = LoRa.read();
      }

      uint8_t rx_crc = LoRa.read();

      // ===== CRC CHECK =====
      uint8_t tmp[67];
      tmp[0] = addr;
      tmp[1] = type;
      tmp[2] = len;
      memcpy(&tmp[3], data, len);

      if (CRC8(tmp, len + 3) != rx_crc) {
        crcFailCount++;
        if (crcFailCount % 20 == 1) {
          LOGI("[WARN] CRC fail count=%lu\n", (unsigned long)crcFailCount);
        }
        continue;
      }

      if (addr != currentNode) {
        continue;
      }

      if (len == 5 && type == TYPE_DATA) {
        float temp;
        memcpy(&temp, data, 4);
        uint8_t status = data[4] & 0x3F;
        LOGI("[DATA] node=%02X temp=%.2f status=%u rssi=%d\n",
             addr, temp, status, LoRa.packetRssi());

        enqueueTelemetry(addr, temp, status);
      }

      // ===== SEND ACK =====
      sendACK(addr);

      gotData = true;
      break;
    }

    if (!gotData) {
      Serial.print("TIMEOUT node ");
      Serial.println(currentNode, HEX);
    }

    // ===== NEXT NODE =====
    nodeIndex++;
    if (nodeIndex >= NUM_NODES) nodeIndex = 0;

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  SPI.begin(18, 19, 23, SS);
  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed");
    while (1);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  WiFi.mode(WIFI_STA);

  telemetryQueue = xQueueCreate(16, sizeof(TelemetryMessage));
  if (telemetryQueue == nullptr) {
    Serial.println("Queue create failed");
    while (1) {
      delay(1000);
    }
  }

  xTaskCreatePinnedToCore(mqttTask, "mqttTask", 6144, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(loraTask, "loraTask", 6144, nullptr, 1, nullptr, 0);

  Serial.println("Gateway ready (Node1 + Node2 + Node3)");
}

// ================= LOOP =================
void loop() {
  vTaskDelay(portMAX_DELAY);
}
