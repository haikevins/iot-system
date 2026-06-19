require('dotenv').config();

const express = require('express');
const cors = require('cors');
const mqtt = require('mqtt');
const { InfluxDB, Point } = require('@influxdata/influxdb-client');

const config = {
  host: process.env.HOST || '0.0.0.0',
  port: Number.parseInt(process.env.PORT || '3000', 10),
  mqttUrl: process.env.MQTT_URL || 'mqtt://127.0.0.1:1883',
  mqttUser: process.env.MQTT_USER || '',
  mqttPassword: process.env.MQTT_PASSWORD || '',
  mqttClientId:
    process.env.MQTT_CLIENT_ID || `backend-influx-${Math.random().toString(16).slice(2, 8)}`,
  corsOrigin: process.env.CORS_ORIGIN || '*',
  influxUrl: process.env.INFLUX_URL,
  influxToken: process.env.INFLUX_TOKEN,
  influxOrg: process.env.INFLUX_ORG,
  influxBucket: process.env.INFLUX_BUCKET
};

const requiredVars = ['INFLUX_URL', 'INFLUX_TOKEN', 'INFLUX_ORG', 'INFLUX_BUCKET'];
const missing = requiredVars.filter((name) => !process.env[name]);
if (missing.length > 0) {
  console.error(`Missing env vars: ${missing.join(', ')}`);
  process.exit(1);
}

const app = express();
const latestByNode = {};
let lastMqttMessageAt = null;

app.use(
  cors({
    origin:
      config.corsOrigin === '*'
        ? true
        : config.corsOrigin
            .split(',')
            .map((origin) => origin.trim())
            .filter(Boolean)
  })
);

const influxDB = new InfluxDB({ url: config.influxUrl, token: config.influxToken });
const writeApi = influxDB.getWriteApi(config.influxOrg, config.influxBucket, 'ms');
writeApi.useDefaultTags({ source: 'mqtt-backend' });

const mqttClient = mqtt.connect(config.mqttUrl, {
  username: config.mqttUser,
  password: config.mqttPassword,
  clientId: config.mqttClientId,
  clean: true,
  reconnectPeriod: 2000,
  connectTimeout: 5000
});

const topicRegex = /^iot\/(node\d{2})\/(temp_avg|status)$/i;
const topics = ['iot/+/temp_avg', 'iot/+/status'];

function updateLatest(node, metric, value) {
  if (!latestByNode[node]) {
    latestByNode[node] = {};
  }
  latestByNode[node][metric] = value;
  latestByNode[node].updatedAt = new Date().toISOString();
}

function handleMessage(topic, payloadBuffer) {
  const match = topicRegex.exec(topic);
  if (!match) return;

  const node = match[1].toLowerCase();
  const metric = match[2];
  const payload = payloadBuffer.toString().trim();
  const point = new Point('node_metrics').tag('node', node);

  if (metric === 'temp_avg') {
    const value = Number(payload);
    if (!Number.isFinite(value)) {
      console.error(`Invalid temp_avg payload for ${topic}: "${payload}"`);
      return;
    }
    point.floatField('temp_avg', value);
    updateLatest(node, 'temp_avg', value);
  } else {
    const value = Number.parseInt(payload, 10);
    if (!Number.isInteger(value)) {
      console.error(`Invalid status payload for ${topic}: "${payload}"`);
      return;
    }
    point.intField('status', value);
    updateLatest(node, 'status', value);
  }

  lastMqttMessageAt = new Date().toISOString();
  writeApi.writePoint(point);
}

mqttClient.on('connect', () => {
  console.log(`Connected to MQTT broker: ${config.mqttUrl}`);
  mqttClient.subscribe(topics, { qos: 1 }, (err) => {
    if (err) {
      console.error('MQTT subscribe failed:', err.message);
      return;
    }
    console.log(`Subscribed topics: ${topics.join(', ')}`);
  });
});

mqttClient.on('message', handleMessage);
mqttClient.on('reconnect', () => console.log('MQTT reconnecting...'));
mqttClient.on('offline', () => console.log('MQTT offline'));
mqttClient.on('error', (err) => console.error('MQTT error:', err.message));

app.get('/health', (_req, res) => {
  res.json({
    status: 'ok',
    mqttConnected: mqttClient.connected,
    lastMessageAt: lastMqttMessageAt,
    influxBucket: config.influxBucket
  });
});

app.get('/latest', (_req, res) => {
  res.json(latestByNode);
});

const server = app.listen(config.port, config.host, () => {
  const displayHost = config.host === '0.0.0.0' ? 'localhost' : config.host;
  console.log(`Backend listening on http://${displayHost}:${config.port}`);
});

async function shutdown() {
  console.log('Shutting down...');
  server.close();
  mqttClient.end(true);
  try {
    await writeApi.close();
  } catch (err) {
    console.error('Influx flush failed:', err.message);
  }
  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
