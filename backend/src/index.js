require('dotenv').config();

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
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
  influxBucket: process.env.INFLUX_BUCKET,
  ingestOutboxDir: path.resolve(
    process.env.INGEST_OUTBOX_DIR || path.join(__dirname, '..', 'data', 'influx-outbox')
  ),
  influxRetryMs: Number.parseInt(process.env.INFLUX_RETRY_MS || '5000', 10),
  doneRetentionHours: Number.parseInt(process.env.INGEST_DONE_RETENTION_HOURS || '168', 10),
  doneMaxEntries: Number.parseInt(process.env.INGEST_DONE_MAX_ENTRIES || '20000', 10)
};

const requiredVars = ['INFLUX_URL', 'INFLUX_TOKEN', 'INFLUX_ORG', 'INFLUX_BUCKET'];
const missingVars = requiredVars.filter((name) => !process.env[name]);

if (missingVars.length > 0) {
  console.error(`Missing env vars: ${missingVars.join(', ')}`);
  process.exit(1);
}

const app = express();
const latestByNode = {};
const outboxPaths = {
  pending: path.join(config.ingestOutboxDir, 'pending'),
  done: path.join(config.ingestOutboxDir, 'done'),
  rejected: path.join(config.ingestOutboxDir, 'rejected')
};

for (const directoryPath of Object.values(outboxPaths)) {
  fs.mkdirSync(directoryPath, { recursive: true });
}

let lastMqttMessageAt = null;
let mqttSessionPresent = false;
let duplicateMqttMessageCount = 0;
let outboxPendingCount = 0;
let outboxDoneCount = 0;
let outboxRejectedCount = 0;
let lastInfluxWriteAt = null;
let lastInfluxError = null;
let influxWorkerRunning = false;
let shuttingDown = false;
let workerWakeResolver = null;
let workerPromise = null;

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

/*
 * Writes are flushed explicitly by the durable outbox worker.  Automatic
 * batching/retries are disabled so a pending disk record remains the single
 * source of truth until InfluxDB confirms the HTTP write.
 */
const writeApi = influxDB.getWriteApi(
  config.influxOrg,
  config.influxBucket,
  'ms',
  {
    batchSize: 2,
    flushInterval: 0,
    maxRetries: 0,
    maxBufferLines: 128
  }
);
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

  const rawRecordId = payload?.id;
  let recordId;
  let legacyNumericRecordId = null;

  if (typeof rawRecordId === 'string') {
    const normalizedRecordId = rawRecordId.trim();

    if (!/^[A-Za-z0-9][A-Za-z0-9._:-]{7,95}$/.test(normalizedRecordId)) {
      throw new Error(`Invalid string id for ${topic}`);
    }

    recordId = normalizedRecordId;
  } else if (Number.isSafeInteger(rawRecordId) && rawRecordId >= 0) {
    /* Rolling-update support for pre-v10 gateway payloads. */
    legacyNumericRecordId = rawRecordId;
    recordId = String(rawRecordId);
  } else {
    throw new Error(`Invalid id for ${topic}`);
  }

  const sequence = payload?.seq;
  const status = payload?.status;
  const temperatureValid = payload?.tempValid;
  const temperature = payload?.temp;
  const sensorFaults = payload?.faults ?? null;
  const faultDetailValid =
    payload?.faultDetailValid ?? Array.isArray(sensorFaults);
  const sampledAtMs = payload?.sampledAtMs ?? null;
  const sampleAgeMs = payload?.ageMs ?? null;
  const rssiDbm = payload?.rssiDbm ?? payload?.rssi ?? null;
  const timestampValid = payload?.timestampValid ?? true;
  const recovered = payload?.recovered ?? false;

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

  if (rssiDbm !== null &&
      (!Number.isInteger(rssiDbm) || rssiDbm < -200 || rssiDbm > 50)) {
    throw new Error(`Invalid rssiDbm for ${topic}`);
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
    legacyNumericRecordId,
    sequence,
    status,
    faultDetailValid,
    sensorFaults: faultDetailValid ? sensorFaults.map(Number) : null,
    temperatureValid,
    temperature: temperatureValid ? Number(temperature) : null,
    sampledAtMs,
    sampleAgeMs,
    rssiDbm,
    timestampValid,
    recovered
  };
}


function getOutboxFileName(node, recordId, legacyNumericRecordId = null) {
  if (Number.isSafeInteger(legacyNumericRecordId) && legacyNumericRecordId >= 0) {
    return `${node}-${String(legacyNumericRecordId).padStart(16, '0')}.json`;
  }

  const digest = crypto
    .createHash('sha256')
    .update(node)
    .update('\0')
    .update(String(recordId))
    .digest('hex');

  return `${node}-${digest}.json`;
}

async function fileExists(filePath) {
  try {
    await fs.promises.access(filePath, fs.constants.F_OK);
    return true;
  } catch (error) {
    if (error.code === 'ENOENT') {
      return false;
    }

    throw error;
  }
}

async function syncDirectory(directoryPath) {
  let directoryHandle = null;

  try {
    directoryHandle = await fs.promises.open(directoryPath, 'r');
    await directoryHandle.sync();
  } catch (error) {
    /* Some filesystems do not support fsync on directory descriptors. */
    if (!['EINVAL', 'ENOTSUP', 'EBADF', 'EPERM'].includes(error.code)) {
      throw error;
    }
  } finally {
    if (directoryHandle !== null) {
      await directoryHandle.close();
    }
  }
}

async function writeJsonAtomically(finalPath, value) {
  const directoryPath = path.dirname(finalPath);
  const temporaryPath = `${finalPath}.tmp-${process.pid}-${Date.now()}-${Math.random()
    .toString(16)
    .slice(2)}`;
  let fileHandle = null;

  try {
    fileHandle = await fs.promises.open(temporaryPath, 'wx', 0o600);
    await fileHandle.writeFile(`${JSON.stringify(value)}\n`, 'utf8');
    await fileHandle.sync();
    await fileHandle.close();
    fileHandle = null;

    await fs.promises.rename(temporaryPath, finalPath);
    await syncDirectory(directoryPath);
  } catch (error) {
    if (fileHandle !== null) {
      await fileHandle.close().catch(() => {});
    }

    await fs.promises.unlink(temporaryPath).catch(() => {});
    throw error;
  }
}

async function persistRejectedMessage(topic, payloadBuffer, errorMessage) {
  const digest = crypto
    .createHash('sha256')
    .update(topic)
    .update('\0')
    .update(payloadBuffer)
    .digest('hex');
  const rejectedPath = path.join(outboxPaths.rejected, `${digest}.json`);

  if (await fileExists(rejectedPath)) {
    return;
  }

  await writeJsonAtomically(rejectedPath, {
    version: 1,
    topic,
    payload: payloadBuffer.toString('utf8'),
    error: errorMessage,
    rejectedAt: new Date().toISOString()
  });

  outboxRejectedCount += 1;
}

function wakeInfluxWorker() {
  if (workerWakeResolver !== null) {
    const resolve = workerWakeResolver;
    workerWakeResolver = null;
    resolve();
  }
}

function waitForWorkerWake(timeoutMs) {
  return new Promise((resolve) => {
    const timeout = setTimeout(() => {
      if (workerWakeResolver === onWake) {
        workerWakeResolver = null;
      }

      resolve();
    }, timeoutMs);

    function onWake() {
      clearTimeout(timeout);
      resolve();
    }

    workerWakeResolver = onWake;
  });
}

async function refreshOutboxCounts() {
  const [pending, done, rejected] = await Promise.all([
    fs.promises.readdir(outboxPaths.pending),
    fs.promises.readdir(outboxPaths.done),
    fs.promises.readdir(outboxPaths.rejected)
  ]);

  outboxPendingCount = pending.filter((name) => name.endsWith('.json')).length;
  outboxDoneCount = done.filter((name) => name.endsWith('.json')).length;
  outboxRejectedCount = rejected.filter((name) => name.endsWith('.json')).length;
}

async function acceptTelemetryDurably(topic, payloadBuffer) {
  const match = telemetryTopicRegex.exec(topic);

  if (!match) {
    return;
  }

  const node = match[1].toLowerCase();
  let telemetry;

  try {
    telemetry = parseTelemetryPayload(topic, payloadBuffer);
  } catch (error) {
    /*
     * A malformed telemetry packet is a poison message: persisting it in a
     * rejected directory gives us evidence for debugging, then MQTT may ACK it
     * so the broker does not redeliver the same invalid packet forever.
     */
    await persistRejectedMessage(topic, payloadBuffer, error.message);
    console.error(error.message);
    return;
  }

  const receivedAt = new Date();
  const sampledAt = calculateSampledAt(receivedAt, telemetry);
  const fileName = getOutboxFileName(
    node,
    telemetry.recordId,
    telemetry.legacyNumericRecordId
  );
  const pendingPath = path.join(outboxPaths.pending, fileName);
  const donePath = path.join(outboxPaths.done, fileName);

  if (await fileExists(donePath)) {
    duplicateMqttMessageCount += 1;
    lastMqttMessageAt = receivedAt.toISOString();
    return;
  }

  if (await fileExists(pendingPath)) {
    duplicateMqttMessageCount += 1;
    lastMqttMessageAt = receivedAt.toISOString();
    wakeInfluxWorker();
    return;
  }

  const spoolEntry = {
    version: 1,
    node,
    topic,
    telemetry,
    receivedAt: receivedAt.toISOString(),
    sampledAt: sampledAt === null ? null : sampledAt.toISOString(),
    enqueuedAt: new Date().toISOString()
  };

  /*
   * write -> fsync(file) -> rename -> fsync(directory)
   * MQTT PUBACK is not allowed until this promise resolves.
   */
  await writeJsonAtomically(pendingPath, spoolEntry);
  outboxPendingCount += 1;
  lastMqttMessageAt = receivedAt.toISOString();

  if (sampledAt !== null) {
    updateLatest(node, telemetry, sampledAt, receivedAt);
  }

  wakeInfluxWorker();
}

async function movePendingToDone(pendingPath) {
  const fileName = path.basename(pendingPath);
  const donePath = path.join(outboxPaths.done, fileName);
  const doneAlreadyExists = await fileExists(donePath);

  if (doneAlreadyExists) {
    await fs.promises.unlink(pendingPath).catch((error) => {
      if (error.code !== 'ENOENT') {
        throw error;
      }
    });
  } else {
    await fs.promises.rename(pendingPath, donePath);
  }

  await Promise.all([
    syncDirectory(outboxPaths.pending),
    syncDirectory(outboxPaths.done)
  ]);

  outboxPendingCount = Math.max(0, outboxPendingCount - 1);

  if (!doneAlreadyExists) {
    outboxDoneCount += 1;
  }
}

async function listPendingFiles() {
  const names = await fs.promises.readdir(outboxPaths.pending);
  return names
    .filter((name) => name.endsWith('.json'))
    .sort()
    .map((name) => path.join(outboxPaths.pending, name));
}

async function readSpoolEntry(filePath) {
  const content = await fs.promises.readFile(filePath, 'utf8');
  const entry = JSON.parse(content);

  if (entry?.version !== 1 || typeof entry?.node !== 'string' || !entry?.telemetry) {
    throw new Error(`Invalid durable outbox entry: ${filePath}`);
  }

  return entry;
}

async function writeSpoolEntryToInflux(entry) {
  const telemetry = entry.telemetry;
  const sampledAt = entry.sampledAt === null ? null : new Date(entry.sampledAt);
  const receivedAt = new Date(entry.receivedAt);

  if (sampledAt === null) {
    writeRecoveredUnstampedPoint(entry.node, telemetry, receivedAt);
  } else {
    writeTelemetryPoint(entry.node, telemetry, sampledAt);
  }

  /*
   * InfluxDB client writePoint() is buffered. flush() is explicitly awaited so
   * the disk record is never removed before the HTTP write succeeds.
   */
  await writeApi.flush();
}

async function cleanupDoneReceipts() {
  const entries = await fs.promises.readdir(outboxPaths.done, { withFileTypes: true });
  const files = [];
  const nowMs = Date.now();
  const retentionMs = Math.max(1, config.doneRetentionHours) * 60 * 60 * 1000;

  for (const entry of entries) {
    if (!entry.isFile() || !entry.name.endsWith('.json')) {
      continue;
    }

    const filePath = path.join(outboxPaths.done, entry.name);
    const stats = await fs.promises.stat(filePath);
    files.push({ filePath, mtimeMs: stats.mtimeMs });
  }

  files.sort((left, right) => left.mtimeMs - right.mtimeMs);
  let removeCount = Math.max(0, files.length - Math.max(100, config.doneMaxEntries));

  for (const file of files) {
    const expired = nowMs - file.mtimeMs > retentionMs;

    if (!expired && removeCount <= 0) {
      continue;
    }

    await fs.promises.unlink(file.filePath).catch((error) => {
      if (error.code !== 'ENOENT') {
        throw error;
      }
    });

    outboxDoneCount = Math.max(0, outboxDoneCount - 1);

    if (removeCount > 0) {
      removeCount -= 1;
    }
  }

  await syncDirectory(outboxPaths.done);
}

async function runInfluxWorker() {
  influxWorkerRunning = true;
  let lastCleanupAtMs = 0;

  try {
    while (!shuttingDown) {
      const pendingFiles = await listPendingFiles();

      if (pendingFiles.length === 0) {
        if (Date.now() - lastCleanupAtMs > 60 * 60 * 1000) {
          await cleanupDoneReceipts().catch((error) => {
            console.error('Done receipt cleanup failed:', error.message);
          });
          lastCleanupAtMs = Date.now();
        }

        await waitForWorkerWake(1000);
        continue;
      }

      let writeFailed = false;

      for (const pendingPath of pendingFiles) {
        if (shuttingDown) {
          break;
        }

        try {
          const entry = await readSpoolEntry(pendingPath);
          await writeSpoolEntryToInflux(entry);
          await movePendingToDone(pendingPath);

          lastInfluxWriteAt = new Date().toISOString();
          lastInfluxError = null;
        } catch (error) {
          lastInfluxError = error.message;
          console.error('Influx durable write failed:', error.message);
          writeFailed = true;
          break;
        }
      }

      if (writeFailed) {
        await waitForWorkerWake(Math.max(1000, config.influxRetryMs));
      }
    }
  } finally {
    influxWorkerRunning = false;
  }
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
    rssiDbm: telemetry.rssiDbm,
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
    .stringField('record_uid', String(telemetry.recordId))
    .intField('seq', telemetry.sequence)
    .intField('status', telemetry.status)
    .booleanField('fault_detail_valid', telemetry.faultDetailValid)
    .booleanField('temp_valid', telemetry.temperatureValid)
    .booleanField('recovered', telemetry.recovered)
    .booleanField('timestamp_valid', true)
    .timestamp(sampledAt);

  const legacyRecordId = Number.isSafeInteger(telemetry.legacyNumericRecordId)
    ? telemetry.legacyNumericRecordId
    : Number.isSafeInteger(telemetry.recordId)
      ? telemetry.recordId
      : null;

  if (legacyRecordId !== null) {
    point.intField('record_id', legacyRecordId);
  }

  if (telemetry.sampleAgeMs !== null) {
    point.intField('age_ms', telemetry.sampleAgeMs);
  }

  if (telemetry.rssiDbm !== null) {
    point.intField('rssi_dbm', telemetry.rssiDbm);
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
    .stringField('record_uid', String(telemetry.recordId))
    .intField('seq', telemetry.sequence)
    .intField('status', telemetry.status)
    .booleanField('fault_detail_valid', telemetry.faultDetailValid)
    .booleanField('temp_valid', telemetry.temperatureValid)
    .booleanField('recovered', true)
    .booleanField('timestamp_valid', false)
    .timestamp(receivedAt);

  const legacyRecordId = Number.isSafeInteger(telemetry.legacyNumericRecordId)
    ? telemetry.legacyNumericRecordId
    : Number.isSafeInteger(telemetry.recordId)
      ? telemetry.recordId
      : null;

  if (legacyRecordId !== null) {
    point.intField('record_id', legacyRecordId);
  }

  if (telemetry.rssiDbm !== null) {
    point.intField('rssi_dbm', telemetry.rssiDbm);
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

/*
 * MQTT.js sends PUBACK for QoS 1 only after handleMessage() invokes callback.
 * We override it so the callback is delayed until telemetry is durably fsynced.
 */
mqttClient.handleMessage = (packet, callback) => {
  const topic = Buffer.isBuffer(packet.topic)
    ? packet.topic.toString()
    : String(packet.topic || '');
  const payloadBuffer = Buffer.isBuffer(packet.payload)
    ? packet.payload
    : Buffer.from(packet.payload || '');

  acceptTelemetryDurably(topic, payloadBuffer)
    .then(() => callback())
    .catch((error) => {
      lastInfluxError = `Durable MQTT ingest failed: ${error.message}`;
      console.error(lastInfluxError);

      /* No callback success => no PUBACK. Broker will redeliver QoS 1. */
      callback(error);
    });
};

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
    recordIdMode: 'global-string',
    lastMessageAt: lastMqttMessageAt,
    influxBucket: config.influxBucket,
    influxReliabilityMode: 'durable-disk-outbox',
    influxWorkerRunning,
    lastInfluxWriteAt,
    lastInfluxError,
    outboxPendingCount,
    outboxDoneCount,
    outboxRejectedCount,
    duplicateMqttMessageCount
  });
});

app.get('/ingest-status', (_request, response) => {
  response.json({
    mode: 'durable-disk-outbox',
    directory: config.ingestOutboxDir,
    pending: outboxPendingCount,
    doneReceipts: outboxDoneCount,
    rejected: outboxRejectedCount,
    workerRunning: influxWorkerRunning,
    lastInfluxWriteAt,
    lastInfluxError
  });
});

app.get('/latest', (_request, response) => {
  response.json(latestByNode);
});

app.get('/history', async (request, response) =>
{
    const rangeMinutes = clampInteger(
        request.query.minutes,
        15,
        1,
        1440
    );

    const windowSeconds = clampInteger(
        request.query.window,
        5,
        1,
        300
    );

    const bucket = escapeFluxString(config.influxBucket);

const query = `
from(bucket: "${bucket}")
    |> range(start: -${rangeMinutes}m)
    |> filter(fn: (r) =>
        r._measurement == "node_metrics"
    )
    |> filter(fn: (r) =>
        r._field == "temp_avg" or
        r._field == "temp_valid"
    )
    |> aggregateWindow(
        every: ${windowSeconds}s,
        fn: last,
        createEmpty: false
    )
    |> pivot(
        rowKey: ["_time", "node"],
        columnKey: ["_field"],
        valueColumn: "_value"
    )
    |> keep(
        columns: [
            "_time",
            "node",
            "temp_avg",
            "temp_valid"
        ]
    )
    |> sort(columns: ["_time"])
`;

    try
    {
        const rows = await queryApi.collectRows(query);

        const pointsByTime = new Map();

        for (const row of rows)
        {
            const node = String(row.node || '').toLowerCase();
            const time = row._time;

            if (!/^node0[1-3]$/.test(node) || !time)
            {
                continue;
            }

            if (!pointsByTime.has(time))
            {
                pointsByTime.set(
                    time,
                    {
                        time,
                        node01: null,
                        node02: null,
                        node03: null
                    }
                );
            }

            const point = pointsByTime.get(time);

            const temperature = Number(row.temp_avg);

            /*
             * Legacy data có thể chưa có temp_valid.
             * Nếu có temp_avg hợp lệ thì vẫn hiển thị.
             */
            const temperatureValid =
                row.temp_valid === false
                    ? false
                    : Number.isFinite(temperature);

            point[node] =
                temperatureValid &&
                Number.isFinite(temperature)
                    ? temperature
                    : null;
        }

        const points = Array
            .from(pointsByTime.values())
            .sort((left, right) =>
            {
                return left.time.localeCompare(right.time);
            });

        response.json(
            {
                rangeMinutes,
                windowSeconds,
                points
            }
        );
    }
    catch (error)
    {
        console.error(
            'Influx history query failed:',
            error
        );

        response.status(500).json(
            {
                error: 'Unable to query temperature history',
                detail: error.message
            }
        );
    }
});

const server = app.listen(config.port, config.host, () => {
  const displayHost = config.host === '0.0.0.0' ? 'localhost' : config.host;
  console.log(`Backend listening on http://${displayHost}:${config.port}`);
});

async function initializeDurableOutbox() {
  await refreshOutboxCounts();
  console.log(
    `Influx durable outbox: pending=${outboxPendingCount}, done=${outboxDoneCount}, ` +
      `rejected=${outboxRejectedCount}, dir=${config.ingestOutboxDir}`
  );

  workerPromise = runInfluxWorker().catch((error) => {
    lastInfluxError = error.message;
    console.error('Influx worker stopped unexpectedly:', error.message);
  });
  wakeInfluxWorker();
}

initializeDurableOutbox().catch((error) => {
  console.error('Unable to initialize durable Influx outbox:', error.message);
  process.exit(1);
});

async function shutdown() {
  if (shuttingDown) {
    return;
  }

  shuttingDown = true;
  console.log('Shutting down...');
  server.close();
  wakeInfluxWorker();

  await new Promise((resolve) => {
    mqttClient.end(false, {}, resolve);
  }).catch(() => {});

  if (workerPromise !== null) {
    await workerPromise.catch(() => {});
  }

  try {
    await writeApi.close();
  } catch (error) {
    console.error('Influx close failed:', error.message);
  }

  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
