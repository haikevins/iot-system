# IoT System – Hướng dẫn cấu hình, build và vận hành toàn hệ thống

Tài liệu này mô tả cách cấu hình và chạy toàn bộ hệ thống:

```text
STM32 Node 01/02/03
        ↓ LoRa
ESP32 Gateway
        ↓ MQTT QoS 1
Mosquitto Broker
        ↓
Node.js Backend
        ↓
InfluxDB
        ↓
React Frontend
```

Hướng dẫn tập trung vào các công việc thực tế khi sử dụng hệ thống:

- Đổi Wi‑Fi hoặc đổi mạng LAN.
- Cập nhật IP của máy Ubuntu.
- Cấu hình MQTT.
- Build và flash ESP32 Gateway.
- Build và flash STM32.
- Chạy Mosquitto.
- Chạy InfluxDB.
- Chạy Backend.
- Chạy Frontend.
- Kiểm tra pipeline end-to-end.
- Xử lý một số lỗi thường gặp.

---

# 1. Kiến trúc hệ thống

Hệ thống gồm 4 tầng chính.

## 1.1. STM32 Sensor Nodes

Có 3 node:

```text
node01
node02
node03
```

Mỗi node thực hiện:

```text
ADC / MCP3208
    ↓
lọc tín hiệu
    ↓
tính nhiệt độ
    ↓
sensor diagnostics
    ↓
timestamp tại thời điểm đo
    ↓
LoRa DATA
```

Các node không tự broadcast liên tục mà hoạt động theo cơ chế:

```text
Gateway POLL
    ↓
STM32 đo dữ liệu
    ↓
STM32 DATA
    ↓
Gateway ACK
```

---

## 1.2. ESP32 Gateway

Gateway thực hiện:

```text
LoRa
 ↓
validate packet
 ↓
persistent LittleFS outbox
 ↓
MQTT QoS 1
 ↓
PUBACK từ broker
```

Gateway có thể tiếp tục nhận dữ liệu LoRa ngay cả khi Wi‑Fi chưa kết nối.

Nếu Wi‑Fi hoặc MQTT mất:

```text
STM32
 ↓
Gateway
 ↓
LittleFS
```

Dữ liệu được giữ trong flash và gửi lại khi mạng phục hồi.

---

## 1.3. Backend

Backend sử dụng:

```text
Node.js
Express
MQTT.js
InfluxDB Client
```

Backend nhận:

```text
iot/node01/telemetry
iot/node02/telemetry
iot/node03/telemetry
```

Sau đó:

```text
MQTT QoS1
 ↓
durable disk ingestion
 ↓
fsync()
 ↓
InfluxDB
 ↓
REST API
```

Các API chính:

```text
GET /health
GET /latest
GET /history
GET /ingest-status
```

---

## 1.4. Frontend

Frontend sử dụng React và lấy dữ liệu từ Backend.

Hiển thị:

- Nhiệt độ hiện tại.
- Online / Offline.
- Sensor OK / Fault / Unknown.
- Detailed fault.
- History từ InfluxDB.
- Biểu đồ nhiệt độ.

---

# 2. Các port mặc định

| Thành phần | Port |
|---|---:|
| Mosquitto MQTT | `1883` |
| Backend | `3000` |
| Frontend Vite | `5173` |
| InfluxDB | `8086` |

Pipeline trên máy Ubuntu:

```text
ESP32
 ↓
Ubuntu:1883
 ↓
Backend:3000
 ↓
InfluxDB:8086

Browser
 ↓
Frontend:5173
 ↓
Backend:3000
```

---

# 3. Kiểm tra IP của máy Ubuntu

Máy Ubuntu đang chạy:

- Mosquitto
- Backend
- InfluxDB
- Frontend

Khi đổi Wi‑Fi, IP LAN của máy này có thể thay đổi.

Kiểm tra:

```bash
hostname -I
```

Hoặc:

```bash
ip route get 1.1.1.1
```

Ví dụ:

```text
192.168.1.100
```

Trong tài liệu dưới đây ta giả sử:

```text
UBUNTU_IP = 192.168.1.100
```

---

# 4. Khi đổi Wi‑Fi cần sửa những gì?

Đây là phần quan trọng nhất khi chuyển hệ thống sang mạng khác.

Giả sử:

```text
Wi-Fi mới:
SSID = HomeWifi
Password = 12345678

IP Ubuntu mới:
192.168.1.100
```

Cần sửa:

```text
gateway/include/secrets.h
frontend/.env
backend/.env        ← chỉ CORS nếu cần
```

Không cần sửa firmware STM32.

---

# 5. Cấu hình Gateway

Đi vào:

```bash
cd ~/Documents/Project/iot-system/gateway
```

Mở:

```bash
nano include/secrets.h
```

Ví dụ:

```cpp
#pragma once

#include <stdint.h>

static constexpr char WIFI_SSID[] = "HomeWifi";
static constexpr char WIFI_PASSWORD[] = "12345678";

static constexpr char MQTT_BROKER[] = "192.168.1.100";
static constexpr uint16_t MQTT_PORT = 1883U;

static constexpr char MQTT_USER[] = "gateway-01";
static constexpr char MQTT_PASSWORD[] = "YOUR_MQTT_PASSWORD";
static constexpr char MQTT_CLIENT_ID[] = "esp32-gateway-01";
```

## Khi chỉ đổi Wi‑Fi

Thông thường chỉ cần sửa:

```text
WIFI_SSID
WIFI_PASSWORD
MQTT_BROKER
```

Ví dụ:

```cpp
static constexpr char WIFI_SSID[] = "WifiMoi";
static constexpr char WIFI_PASSWORD[] = "PasswordMoi";

static constexpr char MQTT_BROKER[] = "192.168.50.20";
```

Sau khi sửa phải build và flash lại Gateway.

---

# 6. Credential và Git

Credential thật nằm trong:

```text
gateway/include/secrets.h
```

File này không được commit lên Git.

Repo chỉ giữ:

```text
gateway/include/secrets.example.h
```

Kiểm tra:

```bash
git check-ignore gateway/include/secrets.h
```

Nếu đúng sẽ trả:

```text
gateway/include/secrets.h
```

Không commit file:

```text
secrets.h
.env
```

---

# 7. Mosquitto MQTT Broker

## 7.1. Start Mosquitto

```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
```

Kiểm tra:

```bash
sudo systemctl status mosquitto --no-pager -l
```

Phải thấy:

```text
Active: active (running)
```

và:

```text
Opening ipv4 listen socket on port 1883
```

---

## 7.2. Kiểm tra port MQTT

```bash
sudo ss -ltnp | grep 1883
```

---

## 7.3. Mosquitto persistence

Máy hiện tại đã cấu hình:

```conf
persistence true
persistence_location /var/lib/mosquitto/
```

Kiểm tra:

```bash
sudo grep -nE 'persistence|persistence_location' \
    /etc/mosquitto/mosquitto.conf
```

Không copy lại file persistence nếu config chính đã có.

Không chạy lại:

```bash
sudo cp broker/mosquitto-persistence.conf \
    /etc/mosquitto/conf.d/iot-persistence.conf
```

nếu `/etc/mosquitto/mosquitto.conf` đã khai báo `persistence`.

Nếu khai báo hai lần Mosquitto sẽ lỗi:

```text
Error: Duplicate persistence configuration
```

---

## 7.4. Kiểm tra persistence database

```bash
sudo ls -lh /var/lib/mosquitto/
```

Có thể thấy:

```text
mosquitto.db
```

Có thể yêu cầu Mosquitto lưu state:

```bash
sudo kill -USR1 $(pidof mosquitto)
```

Sau đó:

```bash
sudo ls -lh /var/lib/mosquitto/
```

---

# 8. MQTT authentication

Kiểm tra cấu hình:

```bash
sudo grep -RniE 'password_file|allow_anonymous' \
    /etc/mosquitto/
```

Nếu muốn đổi password cho:

```text
gateway-01
```

ví dụ password file:

```text
/etc/mosquitto/passwd
```

thì chạy:

```bash
sudo mosquitto_passwd /etc/mosquitto/passwd gateway-01
```

Nhập password mới.

Restart:

```bash
sudo systemctl restart mosquitto
```

Nếu đổi MQTT password, phải sửa tương ứng:

```text
gateway/include/secrets.h
backend/.env
```

---

# 9. Chạy InfluxDB

Start:

```bash
sudo systemctl enable influxdb
sudo systemctl restart influxdb
```

Kiểm tra:

```bash
sudo systemctl status influxdb --no-pager -l
```

Kiểm tra health:

```bash
curl -s http://127.0.0.1:8086/health
```

Nếu ổn sẽ có:

```json
{
  "status": "pass"
}
```

Kiểm tra port:

```bash
sudo ss -ltnp | grep 8086
```

---

# 10. Backend configuration

Đi vào:

```bash
cd ~/Documents/Project/iot-system/backend
```

Mở:

```bash
nano .env
```

Ví dụ:

```env
PORT=3000

MQTT_URL=mqtt://127.0.0.1:1883
MQTT_CLIENT_ID=backend-influx-01
MQTT_USERNAME=gateway-01
MQTT_PASSWORD=YOUR_MQTT_PASSWORD

INFLUX_URL=http://127.0.0.1:8086
INFLUX_TOKEN=YOUR_INFLUX_TOKEN
INFLUX_ORG=iot-org
INFLUX_BUCKET=iot

CORS_ORIGIN=http://localhost:5173,http://192.168.1.100:5173
```

## Khi đổi Wi‑Fi

Backend và Mosquitto chạy cùng máy nên giữ:

```env
MQTT_URL=mqtt://127.0.0.1:1883
INFLUX_URL=http://127.0.0.1:8086
```

Chỉ cần cập nhật `CORS_ORIGIN` nếu IP LAN Ubuntu đổi:

```env
CORS_ORIGIN=http://localhost:5173,http://192.168.50.20:5173
```

---

# 11. Chạy Backend

Lần đầu:

```bash
cd ~/Documents/Project/iot-system/backend
npm ci
```

Chạy:

```bash
npm start
```

Backend chạy:

```text
http://localhost:3000
```

---

# 12. Kiểm tra Backend

Terminal khác:

```bash
curl -s http://127.0.0.1:3000/health
```

Nếu có `jq`:

```bash
curl -s http://127.0.0.1:3000/health | jq
```

Kiểm tra durable ingestion:

```bash
curl -s http://127.0.0.1:3000/ingest-status | jq
```

Kiểm tra dữ liệu hiện tại:

```bash
curl -s http://127.0.0.1:3000/latest | jq
```

Kiểm tra history:

```bash
curl -s \
'http://127.0.0.1:3000/history?minutes=15&window=5' | jq
```

---

# 13. Durable backend ingestion

Backend không ghi trực tiếp MQTT → InfluxDB.

Luồng:

```text
MQTT
 ↓
Backend local disk
 ↓
fsync
 ↓
PUBACK
 ↓
InfluxDB
 ↓
flush
 ↓
done
```

Runtime data nằm trong:

```text
backend/data/influx-outbox/
```

Ví dụ:

```text
pending/
done/
rejected/
```

Kiểm tra:

```bash
find backend/data/influx-outbox -maxdepth 2 -type f | head
```

---

# 14. Frontend configuration

Đi vào:

```bash
cd ~/Documents/Project/iot-system/frontend
```

Mở:

```bash
nano .env
```

Ví dụ:

```env
VITE_API_BASE_URL=http://192.168.1.100:3000
VITE_NODE_OFFLINE_MS=12000
```

Nếu browser chạy ngay trên Ubuntu cũng có thể dùng:

```env
VITE_API_BASE_URL=http://127.0.0.1:3000
```

Nhưng nếu muốn điện thoại/laptop khác trong LAN truy cập frontend thì nên dùng IP LAN:

```env
VITE_API_BASE_URL=http://192.168.1.100:3000
```

---

# 15. Chạy Frontend

Lần đầu:

```bash
cd ~/Documents/Project/iot-system/frontend
npm ci
```

Chạy:

```bash
npm run dev -- --host 0.0.0.0
```

Ví dụ Vite hiển thị:

```text
Local:   http://localhost:5173/
Network: http://192.168.1.100:5173/
```

Trên Ubuntu:

```text
http://localhost:5173
```

Từ thiết bị khác:

```text
http://192.168.1.100:5173
```

Nếu sửa `.env`, phải restart Vite:

```text
Ctrl+C
```

sau đó:

```bash
npm run dev -- --host 0.0.0.0
```

---

# 16. Build ESP32 Gateway

Đi vào:

```bash
cd ~/Documents/Project/iot-system/gateway
```

Kiểm tra PlatformIO:

```bash
pio --version
```

Build:

```bash
pio run
```

Nếu thành công:

```text
SUCCESS
```

Output nằm trong:

```text
.pio/build/nodemcu-32s/
```

---

# 17. Flash ESP32 Gateway

Cắm Gateway vào USB.

Kiểm tra port:

```bash
pio device list
```

Ví dụ:

```text
/dev/ttyUSB0
```

Upload:

```bash
pio run -t upload
```

Nếu cần chỉ định port:

```bash
pio run -t upload --upload-port /dev/ttyUSB0
```

---

# 18. Serial Monitor Gateway

```bash
pio device monitor -b 115200
```

Hoặc:

```bash
pio device monitor \
    --port /dev/ttyUSB0 \
    --baud 115200
```

Thoát:

```text
Ctrl+C
```

---

# 19. Quy trình sau khi đổi Wi‑Fi

Sau khi sửa:

```text
gateway/include/secrets.h
```

chạy:

```bash
cd ~/Documents/Project/iot-system/gateway

pio run

pio run -t upload

pio device monitor -b 115200
```

---

# 20. STM32 Node 01 / 02 / 03

Ba project STM32 hiện dùng Keil MDK:

```text
node01/MDK/Node_1.uvprojx
node02/MDK/Node_2.uvprojx
node03/MDK/Node_3.uvprojx
```

MCU:

```text
STM32F103C8
```

Hiện không sử dụng:

```bash
make
```

hoặc:

```bash
pio run
```

cho STM32.

---

# 21. Build STM32 bằng Keil

Mở:

```text
node01/MDK/Node_1.uvprojx
```

Build:

```text
F7
```

Download bằng ST-Link:

```text
F8
```

Lặp lại với:

```text
Node_2.uvprojx
Node_3.uvprojx
```

---

# 22. Tạo HEX cho STM32

Project hiện có thể chưa bật Create HEX.

Trong Keil:

```text
Options for Target
    ↓
Output
    ↓
Create HEX File
```

Bật tùy chọn này rồi build lại.

---

# 23. Test MQTT trực tiếp

Cài tool:

```bash
sudo apt install mosquitto-clients
```

Nhập password không lưu trực tiếp trong shell history:

```bash
read -s -p "MQTT password: " MQTT_PASS
echo
```

Subscribe:

```bash
mosquitto_sub \
    -h 127.0.0.1 \
    -p 1883 \
    -u gateway-01 \
    -P "$MQTT_PASS" \
    -t 'iot/+/telemetry' \
    -v
```

Nếu gateway hoạt động:

```text
iot/node01/telemetry {...}
iot/node02/telemetry {...}
iot/node03/telemetry {...}
```

Sau khi test:

```bash
unset MQTT_PASS
```

---

# 24. Ví dụ telemetry

Gateway publish dạng:

```json
{
  "id": "gw-a1b2c3d4e5f6-7a31b248be492acd-0000000000000025",
  "seq": 25,
  "temp": 27.35,
  "tempValid": true,
  "status": 0,
  "faultDetailValid": true,
  "faults": [0, 0, 0, 0, 0, 0],
  "sampledAtMs": 1786430000000,
  "timestampValid": true
}
```

---

# 25. Detailed sensor fault

Các node có thể gửi fault chi tiết.

Ví dụ node01/node02:

```text
0x01 SHORT
0x02 OPEN
0x04 SIGNAL_NOISY
0x08 RESISTANCE
0x10 TEMP_RANGE
0x20 RATE
0x40 CROSS_SENSOR
0x80 MODEL
```

Node03:

```text
0x02 HIGH_SAT
```

Frontend hiển thị:

```text
OK
Open
Noisy
High saturation
Fault
Unknown
```

---

# 26. Thứ tự chạy toàn hệ thống

Khuyến nghị:

```text
1. InfluxDB
2. Mosquitto
3. Backend
4. Frontend
5. ESP32 Gateway
6. STM32 Nodes
```

---

# 27. Start nhanh toàn hệ thống

## Terminal 1 – services

```bash
sudo systemctl restart influxdb
sudo systemctl restart mosquitto

systemctl is-active influxdb
systemctl is-active mosquitto
```

Mong đợi:

```text
active
active
```

---

## Terminal 2 – Backend

```bash
cd ~/Documents/Project/iot-system/backend
npm start
```

---

## Terminal 3 – Frontend

```bash
cd ~/Documents/Project/iot-system/frontend
npm run dev -- --host 0.0.0.0
```

---

## Terminal 4 – Gateway debug

```bash
cd ~/Documents/Project/iot-system/gateway
pio device monitor -b 115200
```

---

# 28. Kiểm tra toàn bộ port

```bash
sudo ss -ltnp | grep -E '1883|3000|5173|8086'
```

Mong đợi:

```text
1883 → Mosquitto
3000 → Backend
5173 → Frontend
8086 → InfluxDB
```

---

# 29. Kiểm tra pipeline từng tầng

## Tầng 1 – InfluxDB

```bash
curl -s http://127.0.0.1:8086/health | jq
```

---

## Tầng 2 – Mosquitto

```bash
systemctl is-active mosquitto
```

---

## Tầng 3 – MQTT telemetry

```bash
mosquitto_sub \
    -h 127.0.0.1 \
    -p 1883 \
    -u gateway-01 \
    -P "$MQTT_PASS" \
    -t 'iot/+/telemetry' \
    -v
```

---

## Tầng 4 – Backend health

```bash
curl -s http://127.0.0.1:3000/health | jq
```

---

## Tầng 5 – Latest

```bash
curl -s http://127.0.0.1:3000/latest | jq
```

---

## Tầng 6 – History

```bash
curl -s \
'http://127.0.0.1:3000/history?minutes=15&window=5' | jq
```

---

## Tầng 7 – Frontend

Mở:

```text
http://192.168.1.100:5173
```

---

# 30. Daily workflow

Mỗi lần bật hệ thống:

```bash
sudo systemctl restart influxdb
sudo systemctl restart mosquitto
```

Terminal Backend:

```bash
cd ~/Documents/Project/iot-system/backend
npm start
```

Terminal Frontend:

```bash
cd ~/Documents/Project/iot-system/frontend
npm run dev -- --host 0.0.0.0
```

Gateway và STM32 chỉ cần cấp nguồn nếu firmware đã flash.

---

# 31. Khi sửa Gateway firmware

```bash
cd ~/Documents/Project/iot-system/gateway

pio run

pio run -t upload

pio device monitor -b 115200
```

---

# 32. Khi sửa Backend

Không cần build firmware.

Dừng:

```text
Ctrl+C
```

Sau đó:

```bash
cd ~/Documents/Project/iot-system/backend
npm start
```

---

# 33. Khi sửa Frontend

Dừng Vite:

```text
Ctrl+C
```

Sau đó:

```bash
cd ~/Documents/Project/iot-system/frontend
npm run dev -- --host 0.0.0.0
```

---

# 34. Khi đổi MQTT password

Ví dụ:

```bash
sudo mosquitto_passwd /etc/mosquitto/passwd gateway-01
```

Sau đó sửa:

```text
gateway/include/secrets.h
backend/.env
```

Restart:

```bash
sudo systemctl restart mosquitto
```

Build + flash gateway:

```bash
cd ~/Documents/Project/iot-system/gateway

pio run
pio run -t upload
```

Restart backend:

```bash
cd ~/Documents/Project/iot-system/backend
npm start
```

---

# 35. Khi đổi IP Ubuntu

Ví dụ IP cũ:

```text
172.20.10.4
```

IP mới:

```text
192.168.1.100
```

Sửa Gateway:

```cpp
MQTT_BROKER = "192.168.1.100";
```

Sửa Frontend:

```env
VITE_API_BASE_URL=http://192.168.1.100:3000
```

Sửa Backend CORS:

```env
CORS_ORIGIN=http://localhost:5173,http://192.168.1.100:5173
```

Không sửa:

```env
MQTT_URL=mqtt://127.0.0.1:1883
INFLUX_URL=http://127.0.0.1:8086
```

vì Backend, Mosquitto và InfluxDB đang cùng chạy trên Ubuntu.

---

# 36. Troubleshooting

## Mosquitto không start

Kiểm tra:

```bash
sudo systemctl status mosquitto --no-pager -l
```

Log:

```bash
sudo journalctl -u mosquitto -n 50 --no-pager
```

Nếu gặp:

```text
Duplicate persistence configuration
```

tìm:

```bash
sudo grep -RniE \
'^[[:space:]]*(persistence|persistence_location|persistence_file)' \
/etc/mosquitto/
```

Không khai báo persistence hai lần.

---

## Gateway không kết nối MQTT

Kiểm tra:

```text
WIFI_SSID
WIFI_PASSWORD
MQTT_BROKER
MQTT_USER
MQTT_PASSWORD
```

trong:

```text
gateway/include/secrets.h
```

Kiểm tra Ubuntu IP:

```bash
hostname -I
```

Kiểm tra MQTT port:

```bash
sudo ss -ltnp | grep 1883
```

---

## Backend không nhận MQTT

Kiểm tra Broker:

```bash
mosquitto_sub \
    -h 127.0.0.1 \
    -p 1883 \
    -u gateway-01 \
    -P "$MQTT_PASS" \
    -t 'iot/+/telemetry' \
    -v
```

Nếu `mosquitto_sub` nhận được mà Backend không nhận:

```text
→ kiểm tra backend/.env
→ kiểm tra MQTT username/password
→ kiểm tra log npm start
```

---

## Backend chạy nhưng Frontend không có data

Kiểm tra:

```bash
curl -s http://127.0.0.1:3000/latest | jq
```

Nếu API có data nhưng UI không có:

```text
→ kiểm tra frontend/.env
→ kiểm tra VITE_API_BASE_URL
→ restart Vite
→ kiểm tra browser console
```

---

## Frontend mở được nhưng báo CORS

Sửa:

```text
backend/.env
```

Ví dụ:

```env
CORS_ORIGIN=http://localhost:5173,http://192.168.1.100:5173
```

Restart backend.

---

## InfluxDB không ghi dữ liệu

Kiểm tra:

```bash
curl -s http://127.0.0.1:8086/health | jq
```

Kiểm tra ingestion:

```bash
curl -s http://127.0.0.1:3000/ingest-status | jq
```

Nếu:

```text
pending tăng liên tục
```

thì Backend nhận MQTT nhưng chưa ghi được InfluxDB.

---

# 37. Kiểm tra reliability

## Test mất Wi‑Fi

1. Cho hệ thống chạy.
2. Tắt Wi‑Fi/router.
3. STM32 vẫn được Gateway poll.
4. Gateway lưu LittleFS.
5. Bật Wi‑Fi lại.
6. Gateway gửi backlog lên MQTT.
7. Kiểm tra `/history`.

---

## Test Backend crash

Dừng Backend:

```text
Ctrl+C
```

Giữ Gateway chạy.

Sau vài phút chạy lại:

```bash
cd ~/Documents/Project/iot-system/backend
npm start
```

Persistent MQTT session phải nhận backlog.

---

## Test Mosquitto restart

```bash
sudo systemctl restart mosquitto
```

Sau đó kiểm tra:

```bash
sudo systemctl status mosquitto --no-pager -l
```

Gateway và Backend phải reconnect tự động.

---

## Test InfluxDB down

```bash
sudo systemctl stop influxdb
```

Kiểm tra:

```bash
curl -s http://127.0.0.1:3000/ingest-status | jq
```

Pending phải tăng.

Bật lại:

```bash
sudo systemctl start influxdb
```

Backend phải retry và pending giảm.

---

## Test Gateway reboot

```text
STM32 DATA
 ↓
LittleFS persisted
 ↓
reboot Gateway
 ↓
Gateway mount LittleFS
 ↓
resend pending record
```

Sau reboot kiểm tra history không mất sample đã persist.

---

# 38. Build production Frontend

Nếu muốn test production build:

```bash
cd ~/Documents/Project/iot-system/frontend

npm run build
```

Output:

```text
dist/
```

Có thể preview:

```bash
npm run preview -- --host 0.0.0.0
```

---

# 39. Git workflow

Kiểm tra:

```bash
git status
```

Commit:

```bash
git add .
git commit -m "update: system configuration and reliability"
```

Push:

```bash
git push
```

Không commit:

```text
gateway/include/secrets.h
backend/.env
frontend/.env
backend/data/influx-outbox/
```

---

# 40. Tóm tắt khi đổi Wi‑Fi

Giả sử IP Ubuntu mới:

```text
192.168.1.100
```

Sửa:

## Gateway

```text
gateway/include/secrets.h
```

```cpp
WIFI_SSID
WIFI_PASSWORD
MQTT_BROKER
```

## Frontend

```text
frontend/.env
```

```env
VITE_API_BASE_URL=http://192.168.1.100:3000
```

## Backend

```text
backend/.env
```

```env
CORS_ORIGIN=http://localhost:5173,http://192.168.1.100:5173
```

Giữ nguyên:

```env
MQTT_URL=mqtt://127.0.0.1:1883
INFLUX_URL=http://127.0.0.1:8086
```

Sau đó:

```bash
cd gateway
pio run
pio run -t upload
```

Restart Backend:

```bash
cd backend
npm start
```

Restart Frontend:

```bash
cd frontend
npm run dev -- --host 0.0.0.0
```

---

# 41. Quick Start

Nếu tất cả đã được cấu hình và firmware đã flash:

```bash
sudo systemctl restart influxdb
sudo systemctl restart mosquitto
```

Backend:

```bash
cd ~/Documents/Project/iot-system/backend
npm start
```

Frontend:

```bash
cd ~/Documents/Project/iot-system/frontend
npm run dev -- --host 0.0.0.0
```

Kiểm tra:

```bash
curl -s http://127.0.0.1:3000/health | jq
```

Mở:

```text
http://<UBUNTU_IP>:5173
```

Ví dụ:

```text
http://192.168.1.100:5173
```

---

# 42. Pipeline cuối cùng

```text
STM32 ADC
    ↓
Temperature Processing
    ↓
Detailed Diagnostics
    ↓
ADC Timestamp
    ↓
LoRa DATA
    ↓
CRC / SEQ / Retry
    ↓
ESP32 Gateway
    ↓
LittleFS Durable Outbox
    ↓
MQTT QoS 1
    ↓
Mosquitto Persistent Broker
    ↓
Backend Durable Disk Ingestion
    ↓
InfluxDB Confirmed Write
    ↓
REST API
    ↓
React Frontend
```

Mục tiêu của kiến trúc này là dữ liệu không chỉ "gửi được", mà còn có khả năng phục hồi khi:

```text
Wi‑Fi mất
MQTT mất
Gateway reboot
Broker restart
Backend crash
InfluxDB down
LoRa packet loss
Sensor fault
```

mà vẫn giữ được khả năng theo dõi và phục hồi dữ liệu.
