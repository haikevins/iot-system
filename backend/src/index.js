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
  mqttClientId: process.env.MQTT_CLIENT_ID || 'backend-influx-01',
  corsOrigin: process.env.CORS_ORIGIN || '*',
  influxUrl: process.env.INFLUX_URL,
  influxToken: process.env.INFLUX_TOKEN,
  influxOrg: process.env.INFLUX_ORG,
  influxBucket: process.env.INFLUX_BUCKET
};

const requiredVars = ['INFLUX_URL', 'INFLUX_TOKEN', 'INFLUX_ORG', 'INFLUX_BUCKET'];
const missingVars = requiredVars.filter((name) => !process.env[name]);

if (missingVars.length > 0) {
  console.error(`Missing env vars: ${missingVars.join(', ')}`);
  process.exit(1);
}

const app = express();
const latestByNode = {};
const recentRecordIds = new Map();
const MQTT_RECORD_DEDUPE_TTL_MS = 24 * 60 * 60 * 1000;
const MQTT_RECORD_DEDUPE_MAX_ENTRIES = 4096;
let lastMqttMessageAt = null;
let mqttSessionPresent = false;
let duplicateMqttMessageCount = 0;

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

const influxDB = new InfluxDB({
  url: config.influxUrl,
  token: config.influxToken
});

const writeApi = influxDB.getWriteApi(config.influxOrg, config.influxBucket, 'ms');
const queryApi = influxDB.getQueryApi(config.influxOrg);

writeApi.useDefaultTags({ source: 'mqtt-backend' });

const mqttClient = mqtt.connect(config.mqttUrl, {
  username: config.mqttUser,
  password: config.mqttPassword,
  clientId: config.mqttClientId,
  clean: false,
  protocolVersion: 4,
  reconnectPeriod: 2000,
  connectTimeout: 5000
});

const telemetryTopicRegex = /^iot\/(node\d{2})\/telemetry$/i;
const telemetryTopic = 'iot/+/telemetry';
const SENSOR_COUNT = 6;

function clampInteger(value, fallback, minimum, maximum) {
  const parsed = Number.parseInt(value, 10);

  if (!Number.isInteger(parsed)) {
    return fallback;
  }

  return Math.min(Math.max(parsed, minimum), maximum);
}

function escapeFluxString(value) {
  return String(value).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function parseTelemetryPayload(topic, payloadBuffer) {
  let payload;

  try {
    payload = JSON.parse(payloadBuffer.toString());
  } catch (error) {
    throw new Error(`Invalid JSON for ${topic}: ${error.message}`);
  }

  const recordId = payload?.id;
  const sequence = payload?.seq;
  const status = payload?.status;
  const temperatureValid = payload?.tempValid;
  const temperature = payload?.temp;
  const sensorFaults = payload?.faults ?? null;
  const faultDetailValid =
    payload?.faultDetailValid ?? Array.isArray(sensorFaults);
  const sampledAtMs = payload?.sampledAtMs ?? null;
  const sampleAgeMs = payload?.ageMs ?? null;
  const timestampValid = payload?.timestampValid ?? true;
  const recovered = payload?.recovered ?? false;

  if (!Number.isSafeInteger(recordId) || recordId < 0) {
    throw new Error(`Invalid id for ${topic}`);
  }

  if (!Number.isInteger(sequence) || sequence < 0 || sequence > 255) {
    throw new Error(`Invalid seq for ${topic}`);
  }

  if (!Number.isInteger(status) || status < 0 || status > 0x3f) {
    throw new Error(`Invalid status for ${topic}`);
  }

  if (typeof faultDetailValid !== 'boolean') {
    throw new Error(`Invalid faultDetailValid for ${topic}`);
  }

  if (faultDetailValid) {
    if (!Array.isArray(sensorFaults) || sensorFaults.length !== SENSOR_COUNT) {
      throw new Error(`faultDetailValid=true requires ${SENSOR_COUNT} fault codes for ${topic}`);
    }

    for (let sensorIndex = 0; sensorIndex < SENSOR_COUNT; sensorIndex += 1) {
      const faultCode = sensorFaults[sensorIndex];

      if (!Number.isInteger(faultCode) || faultCode < 0 || faultCode > 0xff) {
        throw new Error(`Invalid fault code S${sensorIndex + 1} for ${topic}`);
      }

      const summaryFault = ((status >> sensorIndex) & 0x01) === 1;
      const detailedFault = faultCode !== 0;

      if (summaryFault !== detailedFault) {
        throw new Error(`Status/fault detail mismatch at S${sensorIndex + 1} for ${topic}`);
      }
    }
  } else if (sensorFaults !== null) {
    throw new Error(`faultDetailValid=false requires faults=null for ${topic}`);
  }

  if (typeof temperatureValid !== 'boolean') {
    throw new Error(`Invalid tempValid for ${topic}`);
  }

  if (temperatureValid && !Number.isFinite(temperature)) {
    throw new Error(`tempValid=true but temp is invalid for ${topic}`);
  }

  if (!temperatureValid && temperature !== null && temperature !== undefined) {
    throw new Error(`tempValid=false requires temp=null for ${topic}`);
  }

  if (sampledAtMs !== null &&
      (!Number.isSafeInteger(sampledAtMs) || sampledAtMs < 0)) {
    throw new Error(`Invalid sampledAtMs for ${topic}`);
  }

  if (sampleAgeMs !== null &&
      (!Number.isInteger(sampleAgeMs) || sampleAgeMs < 0 || sampleAgeMs > 0xffffffff)) {
    throw new Error(`Invalid ageMs for ${topic}`);
  }

  if (typeof timestampValid !== 'boolean') {
    throw new Error(`Invalid timestampValid for ${topic}`);
  }

  if (typeof recovered !== 'boolean') {
    throw new Error(`Invalid recovered for ${topic}`);
  }

  if (timestampValid && sampledAtMs === null && sampleAgeMs === null) {
    throw new Error(`timestampValid=true requires sampledAtMs or ageMs for ${topic}`);
  }

  return {
    recordId,
    sequence,
    status,
    faultDetailValid,
    sensorFaults: faultDetailValid ? sensorFaults.map(Number) : null,
    temperatureValid,
    temperature: temperatureValid ? Number(temperature) : null,
    sampledAtMs,
    sampleAgeMs,
    timestampValid,
    recovered
  };
}


function isDuplicateTelemetry(node, telemetry, receivedAt) {
  const key = `${node}:${telemetry.recordId}`;
  const receivedAtMs = receivedAt.getTime();
  const previousSeenAtMs = recentRecordIds.get(key);

  if (previousSeenAtMs !== undefined) {
    recentRecordIds.set(key, receivedAtMs);
    return true;
  }

  recentRecordIds.set(key, receivedAtMs);

  if (recentRecordIds.size > MQTT_RECORD_DEDUPE_MAX_ENTRIES) {
    const cutoff = receivedAtMs - MQTT_RECORD_DEDUPE_TTL_MS;

    for (const [recordKey, seenAtMs] of recentRecordIds.entries()) {
      if (seenAtMs < cutoff || recentRecordIds.size > MQTT_RECORD_DEDUPE_MAX_ENTRIES) {
        recentRecordIds.delete(recordKey);
      }
    }
  }

  return false;
}

function calculateSampledAt(receivedAt, telemetry) {
  if (!telemetry.timestampValid) {
    return null;
  }

  if (telemetry.sampledAtMs !== null) {
    return new Date(telemetry.sampledAtMs);
  }

  if (telemetry.sampleAgeMs !== null) {
    return new Date(receivedAt.getTime() - telemetry.sampleAgeMs);
  }

  return null;
}

function updateLatest(node, telemetry, sampledAt, receivedAt) {
  const previousState = latestByNode[node] || {};
  const previousSampledAtMs = previousState.sampledAt
    ? Date.parse(previousState.sampledAt)
    : Number.NEGATIVE_INFINITY;

  if (sampledAt.getTime() < previousSampledAtMs) {
    return false;
  }

  latestByNode[node] = {
    ...previousState,
    id: telemetry.recordId,
    seq: telemetry.sequence,
    temp_avg: telemetry.temperatureValid ? telemetry.temperature : null,
    tempValid: telemetry.temperatureValid,
    status: telemetry.status,
    faultDetailValid: telemetry.faultDetailValid,
    faults: telemetry.sensorFaults,
    sampleAgeMs: telemetry.sampleAgeMs,
    sampledAt: sampledAt.toISOString(),
    lastSeenAt: sampledAt.toISOString(),
    lastReceivedAt: receivedAt.toISOString(),
    statusUpdatedAt: sampledAt.toISOString(),
    tempUpdatedAt: telemetry.temperatureValid
      ? sampledAt.toISOString()
      : previousState.tempUpdatedAt || null
  };

  return true;
}

function writeTelemetryPoint(node, telemetry, sampledAt) {
  const point = new Point('node_metrics')
    .tag('node', node)
    .intField('record_id', telemetry.recordId)
    .intField('seq', telemetry.sequence)
    .intField('status', telemetry.status)
    .booleanField('fault_detail_valid', telemetry.faultDetailValid)
    .booleanField('temp_valid', telemetry.temperatureValid)
    .booleanField('recovered', telemetry.recovered)
    .booleanField('timestamp_valid', true)
    .timestamp(sampledAt);

  if (telemetry.sampleAgeMs !== null) {
    point.intField('age_ms', telemetry.sampleAgeMs);
  }

  if (telemetry.faultDetailValid) {
    telemetry.sensorFaults.forEach((faultCode, sensorIndex) => {
      point.intField(`fault_s${sensorIndex + 1}`, faultCode);
    });
  }

  if (telemetry.temperatureValid) {
    point.floatField('temp_avg', telemetry.temperature);
  }

  writeApi.writePoint(point);
}

function writeRecoveredUnstampedPoint(node, telemetry, receivedAt) {
  const point = new Point('node_recovered_unstamped')
    .tag('node', node)
    .intField('record_id', telemetry.recordId)
    .intField('seq', telemetry.sequence)
    .intField('status', telemetry.status)
    .booleanField('fault_detail_valid', telemetry.faultDetailValid)
    .booleanField('temp_valid', telemetry.temperatureValid)
    .booleanField('recovered', true)
    .booleanField('timestamp_valid', false)
    .timestamp(receivedAt);

  if (telemetry.faultDetailValid) {
    telemetry.sensorFaults.forEach((faultCode, sensorIndex) => {
      point.intField(`fault_s${sensorIndex + 1}`, faultCode);
    });
  }

  if (telemetry.temperatureValid) {
    point.floatField('temp_avg', telemetry.temperature);
  }

  writeApi.writePoint(point);
}

function handleMessage(topic, payloadBuffer) {
  const match = telemetryTopicRegex.exec(topic);

  if (!match) {
    return;
  }

  const node = match[1].toLowerCase();

  try {
    const telemetry = parseTelemetryPayload(topic, payloadBuffer);
    const receivedAt = new Date();
    const sampledAt = calculateSampledAt(receivedAt, telemetry);

    if (isDuplicateTelemetry(node, telemetry, receivedAt)) {
      duplicateMqttMessageCount += 1;
      lastMqttMessageAt = receivedAt.toISOString();
      return;
    }

    if (sampledAt === null) {
      writeRecoveredUnstampedPoint(node, telemetry, receivedAt);
      lastMqttMessageAt = receivedAt.toISOString();
      return;
    }

    updateLatest(node, telemetry, sampledAt, receivedAt);
    writeTelemetryPoint(node, telemetry, sampledAt);

    lastMqttMessageAt = receivedAt.toISOString();
  } catch (error) {
    console.error(error.message);
  }
}

mqttClient.on('connect', (connack) => {
  mqttSessionPresent = Boolean(connack?.sessionPresent);

  console.log(
    `Connected to MQTT broker: ${config.mqttUrl} ` +
      `(persistent session, sessionPresent=${mqttSessionPresent})`
  );

  mqttClient.subscribe(telemetryTopic, { qos: 1 }, (error) => {
    if (error) {
      console.error('MQTT subscribe failed:', error.message);
      return;
    }

    console.log(`Subscribed topic: ${telemetryTopic}`);
  });
});

mqttClient.on('message', handleMessage);
mqttClient.on('reconnect', () => console.log('MQTT reconnecting...'));
mqttClient.on('offline', () => console.log('MQTT offline'));
mqttClient.on('error', (error) => console.error('MQTT error:', error.message));

app.get('/health', (_request, response) => {
  response.json({
    status: 'ok',
    mqttConnected: mqttClient.connected,
    mqttPersistentSession: true,
    mqttSessionPresent,
    mqttClientId: config.mqttClientId,
    lastMessageAt: lastMqttMessageAt,
    influxBucket: config.influxBucket,
    duplicateMqttMessageCount
  });
});

app.get('/latest', (_request, response) => {
  response.json(latestByNode);
});

app.get('/history', async (request, response) => {
  const rangeMinutes = clampInteger(request.query.minutes, 15, 1, 1440);
  const windowSeconds = clampInteger(request.query.window, 5, 1, 300);
  const bucket = escapeFluxString(config.influxBucket);

  const temperatureQuery = `
from(bucket: "${bucket}")
  |> range(start: -${rangeMinutes}m)
  |> filter(fn: (row) => row._measurement == "node_metrics")
  |> filter(fn: (row) => row._field == "temp_avg")
  |> aggregateWindow(every: ${windowSeconds}s, fn: last, createEmpty: false)
  |> keep(columns: ["_time", "node", "_value"])
  |> sort(columns: ["_time"])
`;

  const validityQuery = `
from(bucket: "${bucket}")
  |> range(start: -${rangeMinutes}m)
  |> filter(fn: (row) => row._measurement == "node_metrics")
  |> filter(fn: (row) => row._field == "temp_valid")
  |> aggregateWindow(every: ${windowSeconds}s, fn: last, createEmpty: false)
  |> keep(columns: ["_time", "node", "_value"])
  |> sort(columns: ["_time"])
`;

  try {
    const [temperatureRows, validityRows] = await Promise.all([
      queryApi.collectRows(temperatureQuery),
      queryApi.collectRows(validityQuery)
    ]);

    const valuesByTimeAndNode = new Map();

    function getEntry(time, node) {
      const key = `${time}|${node}`;

      if (!valuesByTimeAndNode.has(key)) {
        valuesByTimeAndNode.set(key, {
          time,
          node,
          temperature: null,
          hasTemperature: false,
          temperatureValid: null
        });
      }

      return valuesByTimeAndNode.get(key);
    }

    for (const row of temperatureRows) {
      const node = String(row.node || '').toLowerCase();
      const time = row._time;
      const value = Number(row._value);

      if (!/^node\d{2}$/.test(node) || !time || !Number.isFinite(value)) {
        continue;
      }

      const entry = getEntry(time, node);
      entry.temperature = value;
      entry.hasTemperature = true;
    }

    for (const row of validityRows) {
      const node = String(row.node || '').toLowerCase();
      const time = row._time;

      if (!/^node\d{2}$/.test(node) || !time) {
        continue;
      }

      const entry = getEntry(time, node);
      entry.temperatureValid = row._value === true;
    }

    const pointsByTime = new Map();

    for (const entry of valuesByTimeAndNode.values()) {
      if (!pointsByTime.has(entry.time)) {
        pointsByTime.set(entry.time, { time: entry.time });
      }

      const point = pointsByTime.get(entry.time);

      /*
       * Legacy points may not contain temp_valid. In that case, a real
       * temp_avg value is treated as valid. New invalid telemetry always
       * writes temp_valid=false, which creates an explicit null/gap.
       */
      const isValid =
        entry.temperatureValid === false
          ? false
          : entry.hasTemperature;

      point[entry.node] = isValid ? entry.temperature : null;
    }

    const points = Array.from(pointsByTime.values()).sort((left, right) =>
      left.time.localeCompare(right.time)
    );

    response.json({
      rangeMinutes,
      windowSeconds,
      points
    });
  } catch (error) {
    console.error('Influx history query failed:', error.message);
    response.status(500).json({ error: 'Unable to query temperature history' });
  }
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
  } catch (error) {
    console.error('Influx flush failed:', error.message);
  }

  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
