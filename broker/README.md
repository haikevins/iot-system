# Mosquitto persistence for the IoT pipeline

The backend uses a persistent MQTT session (`clean: false`). To preserve that
session and queued QoS 1 messages across a Mosquitto restart, enable broker disk
persistence as well.

## Ubuntu / Debian system service

```bash
sudo cp broker/mosquitto-persistence.conf /etc/mosquitto/conf.d/iot-persistence.conf
sudo install -d -o mosquitto -g mosquitto /var/lib/mosquitto
sudo systemctl restart mosquitto
sudo systemctl status mosquitto --no-pager
```

After MQTT traffic has occurred, confirm the persistence database exists:

```bash
sudo ls -lh /var/lib/mosquitto/mosquitto.db
```

This snippet only changes persistence. Keep your existing listener,
authentication, password file and TLS configuration unchanged.
