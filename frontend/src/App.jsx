import { useEffect, useMemo, useRef, useState } from 'react';
import PropTypes from 'prop-types';
import {
  Brush,
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis
} from 'recharts';
import {
  Activity,
  AlertTriangle,
  BarChart3,
  CircleHelp,
  Clock,
  Cpu,
  Database,
  LayoutDashboard,
  LogOut,
  Menu,
  Monitor,
  Moon,
  Radio,
  RefreshCw,
  RotateCcw,
  Search,
  Server,
  Settings,
  SlidersHorizontal,
  Sun,
  Thermometer,
  Type,
  Wifi,
  WifiOff,
  X
} from 'lucide-react';
import './App.css';

const API_BASE_URL =
  import.meta.env.VITE_API_BASE_URL ||
  `${window.location.protocol}//${window.location.hostname}:3000`;

const NODE_IDS = ['node01', 'node02', 'node03'];
const SENSOR_IDS = [0, 1, 2, 3, 4, 5];
const DEFAULT_POLL_INTERVAL_MS = 2000;
const DEFAULT_HISTORY_REFRESH_MS = 5000;
const OFFLINE_TIMEOUT_MS_RAW = Number(import.meta.env.VITE_NODE_OFFLINE_MS || 12000);
const DEFAULT_OFFLINE_TIMEOUT_MS = Number.isFinite(OFFLINE_TIMEOUT_MS_RAW)
  ? OFFLINE_TIMEOUT_MS_RAW
  : 12000;
const SETTINGS_STORAGE_KEY = 'iot-system-ui-settings-v2';

const DEFAULT_UI_SETTINGS = {
  theme: 'system',
  fontScale: 100,
  autoRefresh: true,
  pollIntervalMs: DEFAULT_POLL_INTERVAL_MS,
  historyRefreshMs: DEFAULT_HISTORY_REFRESH_MS,
  offlineTimeoutMs: DEFAULT_OFFLINE_TIMEOUT_MS,
  defaultHistoryRangeMinutes: 60,
  temperatureDecimals: 2,
  chartGrid: true
};

const FONT_SCALE_OPTIONS = [90, 100, 110, 120];
const POLL_INTERVAL_OPTIONS = [1000, 2000, 5000, 10000];
const HISTORY_REFRESH_OPTIONS = [5000, 10000, 30000, 60000];
const OFFLINE_THRESHOLD_OPTIONS = [10000, 12000, 15000, 30000, 60000];

function loadUiSettings() {
  try {
    const stored = JSON.parse(window.localStorage.getItem(SETTINGS_STORAGE_KEY) || '{}');
    const settings = { ...DEFAULT_UI_SETTINGS, ...stored };

    if (!['light', 'dark', 'system'].includes(settings.theme)) {
      settings.theme = DEFAULT_UI_SETTINGS.theme;
    }

    if (!FONT_SCALE_OPTIONS.includes(settings.fontScale)) {
      settings.fontScale = DEFAULT_UI_SETTINGS.fontScale;
    }

    if (!POLL_INTERVAL_OPTIONS.includes(settings.pollIntervalMs)) {
      settings.pollIntervalMs = DEFAULT_UI_SETTINGS.pollIntervalMs;
    }

    if (!HISTORY_REFRESH_OPTIONS.includes(settings.historyRefreshMs)) {
      settings.historyRefreshMs = DEFAULT_UI_SETTINGS.historyRefreshMs;
    }

    if (!OFFLINE_THRESHOLD_OPTIONS.includes(settings.offlineTimeoutMs)) {
      settings.offlineTimeoutMs = DEFAULT_UI_SETTINGS.offlineTimeoutMs;
    }

    if (!HISTORY_RANGES.some((range) => range.minutes === settings.defaultHistoryRangeMinutes)) {
      settings.defaultHistoryRangeMinutes = DEFAULT_UI_SETTINGS.defaultHistoryRangeMinutes;
    }

    settings.temperatureDecimals = settings.temperatureDecimals === 1 ? 1 : 2;
    settings.autoRefresh = settings.autoRefresh !== false;
    settings.chartGrid = settings.chartGrid !== false;

    return settings;
  } catch {
    return { ...DEFAULT_UI_SETTINGS };
  }
}

const HISTORY_RANGES = [
  { label: '15m', minutes: 15, windowSeconds: 5 },
  { label: '1h', minutes: 60, windowSeconds: 5 },
  { label: '6h', minutes: 360, windowSeconds: 30 },
  { label: '24h', minutes: 1440, windowSeconds: 120 }
];

const NODE_COLORS = {
  node01: '#0a8f60',
  node02: '#35b37e',
  node03: '#1e6a4c'
};

const MAIN_MENU_ITEMS = [{ label: 'Dashboard', icon: LayoutDashboard }];
const GENERAL_MENU_ITEMS = [
  { label: 'Settings', icon: Settings },
  { label: 'Help', icon: CircleHelp },
  { label: 'Logout', icon: LogOut }
];

const FILTER_OPTIONS = [
  { id: 'all', label: 'All' },
  { id: 'online', label: 'Online' },
  { id: 'offline', label: 'Offline' },
  { id: 'fault', label: 'Fault' }
];

const COMMON_SENSOR_FAULT_LABELS = [
  [0x01, 'Short'],
  [0x04, 'Noisy'],
  [0x08, 'Resistance'],
  [0x10, 'Temp range'],
  [0x20, 'Rate'],
  [0x40, 'Cross sensor'],
  [0x80, 'Model']
];

function decodeSensorBits(statusByte) {
  if (!Number.isInteger(statusByte)) {
    return SENSOR_IDS.map(() => null);
  }

  return SENSOR_IDS.map((bit) => ((statusByte >> bit) & 0x01) === 1);
}

function decodeSensorFaultLabels(nodeId, faultCode) {
  if (!Number.isInteger(faultCode)) {
    return null;
  }

  if (faultCode === 0) {
    return [];
  }

  const labels = [];

  if ((faultCode & 0x02) !== 0) {
    labels.push(nodeId === 'node03' ? 'High saturation' : 'Open');
  }

  COMMON_SENSOR_FAULT_LABELS.forEach(([flag, label]) => {
    if ((faultCode & flag) !== 0) {
      labels.push(label);
    }
  });

  return labels.length > 0 ? labels : ['Fault'];
}

function formatSensorFaultSummary(labels) {
  if (!Array.isArray(labels)) {
    return 'Fault';
  }

  if (labels.length === 0) {
    return 'OK';
  }

  return labels.length === 1 ? labels[0] : `${labels[0]} +${labels.length - 1}`;
}

function formatNodeName(nodeId) {
  return nodeId.replace('node', 'Node ');
}

function isNodeOnline(lastSeenAt, nowMs, offlineTimeoutMs) {
  if (!lastSeenAt) {
    return false;
  }

  const timestamp = Date.parse(lastSeenAt);

  if (Number.isNaN(timestamp)) {
    return false;
  }

  return nowMs - timestamp <= offlineTimeoutMs;
}

function formatHistoryTime(value) {
  const timestamp = Date.parse(value);

  if (Number.isNaN(timestamp)) {
    return value || '';
  }

  return new Date(timestamp).toLocaleTimeString('en-GB', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false
  });
}

function formatDuration(milliseconds) {
  if (!Number.isFinite(milliseconds) || milliseconds < 0) {
    return '--';
  }

  const seconds = Math.floor(milliseconds / 1000);

  if (seconds < 5) {
    return 'just now';
  }

  if (seconds < 60) {
    return `${seconds}s`;
  }

  const minutes = Math.floor(seconds / 60);

  if (minutes < 60) {
    return `${minutes}m`;
  }

  const hours = Math.floor(minutes / 60);
  const remainingMinutes = minutes % 60;

  if (hours < 24) {
    return remainingMinutes > 0 ? `${hours}h ${remainingMinutes}m` : `${hours}h`;
  }

  const days = Math.floor(hours / 24);
  return `${days}d`;
}

function formatRelativeTimestamp(timestampValue, nowMs) {
  if (!timestampValue) {
    return 'No data';
  }

  const timestamp = Date.parse(timestampValue);

  if (Number.isNaN(timestamp)) {
    return 'No data';
  }

  const age = Math.max(0, nowMs - timestamp);
  const formatted = formatDuration(age);
  return formatted === 'just now' ? 'just now' : `${formatted} ago`;
}

function getFiniteNumber(...values) {
  for (const value of values) {
    if (typeof value === 'number' && Number.isFinite(value)) {
      return value;
    }
  }

  return null;
}

function getNodeTone(node) {
  if (!node.online) {
    return 'offline';
  }

  if (node.faults > 0) {
    return 'fault';
  }

  if (node.unknownSensors > 0) {
    return 'unknown';
  }

  return 'ok';
}

function AnimatedNumber({
  value,
  decimals = 0,
  suffix = '',
  fallback = '--',
  className = '',
  duration = 500
}) {
  const [displayValue, setDisplayValue] = useState(() =>
    Number.isFinite(value) ? Number(value) : null
  );
  const previousValueRef = useRef(Number.isFinite(value) ? Number(value) : 0);

  useEffect(() => {
    if (!Number.isFinite(value)) {
      setDisplayValue(null);
      previousValueRef.current = 0;
      return;
    }

    const target = Number(value);
    const from = previousValueRef.current;
    previousValueRef.current = target;

    let frameId = 0;
    const startedAt = performance.now();

    const tick = (now) => {
      const progress = Math.min((now - startedAt) / duration, 1);
      const eased = 1 - Math.pow(1 - progress, 3);
      setDisplayValue(from + (target - from) * eased);

      if (progress < 1) {
        frameId = window.requestAnimationFrame(tick);
      }
    };

    frameId = window.requestAnimationFrame(tick);

    return () => window.cancelAnimationFrame(frameId);
  }, [value, duration]);

  if (displayValue === null) {
    return <span className={className}>{fallback}</span>;
  }

  return (
    <span className={className}>
      {displayValue.toFixed(decimals)}
      {suffix}
    </span>
  );
}

AnimatedNumber.propTypes = {
  value: PropTypes.number,
  decimals: PropTypes.number,
  suffix: PropTypes.string,
  fallback: PropTypes.string,
  className: PropTypes.string,
  duration: PropTypes.number
};

function HealthItem({ icon: Icon, label, state, detail, tone }) {
  return (
    <div className={`health-item ${tone}`}>
      <span className="health-item-icon" aria-hidden="true">
        <Icon size={17} strokeWidth={2} />
      </span>
      <span className="health-item-copy">
        <span>{label}</span>
        <strong>{state}</strong>
      </span>
      {detail && <small>{detail}</small>}
    </div>
  );
}

HealthItem.propTypes = {
  icon: PropTypes.elementType.isRequired,
  label: PropTypes.string.isRequired,
  state: PropTypes.string.isRequired,
  detail: PropTypes.string,
  tone: PropTypes.oneOf(['good', 'warn', 'bad', 'unknown']).isRequired
};

function App() {
  const [uiSettings, setUiSettings] = useState(loadUiSettings);
  const [latestByNode, setLatestByNode] = useState({});
  const [latestLoading, setLatestLoading] = useState(true);
  const [history, setHistory] = useState([]);
  const [historyLoading, setHistoryLoading] = useState(true);
  const [historyError, setHistoryError] = useState('');
  const [historyReloadKey, setHistoryReloadKey] = useState(0);
  const [historyRangeMinutes, setHistoryRangeMinutes] = useState(
    () => loadUiSettings().defaultHistoryRangeMinutes
  );
  const [chartResetKey, setChartResetKey] = useState(0);
  const [visibleSeries, setVisibleSeries] = useState(() =>
    Object.fromEntries(NODE_IDS.map((nodeId) => [nodeId, true]))
  );
  const [lastError, setLastError] = useState('');
  const [systemHealth, setSystemHealth] = useState({
    mqttConnected: false,
    lastMessageAt: null,
    influxWorkerRunning: null,
    lastInfluxWriteAt: null,
    lastInfluxError: null,
    outboxPendingCount: null
  });
  const [search, setSearch] = useState('');
  const [statusFilter, setStatusFilter] = useState('all');
  const [activeMenu, setActiveMenu] = useState('Dashboard');
  const [nowTick, setNowTick] = useState(Date.now());
  const [sidebarOpen, setSidebarOpen] = useState(false);

  const updateUiSetting = (key, value) => {
    setUiSettings((current) => ({ ...current, [key]: value }));
  };

  const updateDefaultHistoryRange = (minutes) => {
    setUiSettings((current) => ({
      ...current,
      defaultHistoryRangeMinutes: minutes
    }));
    setHistoryRangeMinutes(minutes);
    setChartResetKey((value) => value + 1);
  };

  const resetUiSettings = () => {
    const defaults = { ...DEFAULT_UI_SETTINGS };
    setUiSettings(defaults);
    setHistoryRangeMinutes(defaults.defaultHistoryRangeMinutes);
    setChartResetKey((value) => value + 1);
  };

  useEffect(() => {
    window.localStorage.setItem(SETTINGS_STORAGE_KEY, JSON.stringify(uiSettings));
    document.documentElement.style.setProperty(
      '--app-font-size',
      `${16 * (uiSettings.fontScale / 100)}px`
    );
  }, [uiSettings]);

  useEffect(() => {
    const media = window.matchMedia('(prefers-color-scheme: dark)');

    const applyTheme = () => {
      const resolvedTheme =
        uiSettings.theme === 'system'
          ? media.matches
            ? 'dark'
            : 'light'
          : uiSettings.theme;

      document.documentElement.dataset.iotTheme = resolvedTheme;
      document.documentElement.style.colorScheme = resolvedTheme;
    };

    applyTheme();

    if (uiSettings.theme === 'system') {
      media.addEventListener('change', applyTheme);
      return () => media.removeEventListener('change', applyTheme);
    }

    return undefined;
  }, [uiSettings.theme]);

  const selectedHistoryRange = useMemo(
    () =>
      HISTORY_RANGES.find((range) => range.minutes === historyRangeMinutes) ||
      HISTORY_RANGES[1],
    [historyRangeMinutes]
  );

  useEffect(() => {
    let active = true;

    const pullLatest = async () => {
      try {
        const [latestResponse, healthResponse] = await Promise.all([
          fetch(`${API_BASE_URL}/latest`),
          fetch(`${API_BASE_URL}/health`)
        ]);

        if (!latestResponse.ok) {
          throw new Error(`Latest HTTP ${latestResponse.status}`);
        }

        if (!healthResponse.ok) {
          throw new Error(`Health HTTP ${healthResponse.status}`);
        }

        const [latestPayload, healthPayload] = await Promise.all([
          latestResponse.json(),
          healthResponse.json()
        ]);

        if (!active) {
          return;
        }

        setLatestByNode(latestPayload || {});
        setSystemHealth({
          mqttConnected: healthPayload?.mqttConnected === true,
          lastMessageAt: healthPayload?.lastMessageAt || null,
          influxWorkerRunning:
            typeof healthPayload?.influxWorkerRunning === 'boolean'
              ? healthPayload.influxWorkerRunning
              : null,
          lastInfluxWriteAt: healthPayload?.lastInfluxWriteAt || null,
          lastInfluxError: healthPayload?.lastInfluxError || null,
          outboxPendingCount: Number.isInteger(healthPayload?.outboxPendingCount)
            ? healthPayload.outboxPendingCount
            : null
        });
        setLastError('');
      } catch (error) {
        if (active) {
          setLastError(error instanceof Error ? error.message : 'Unable to fetch data');
        }
      } finally {
        if (active) {
          setLatestLoading(false);
        }
      }
    };

    pullLatest();

    const interval = uiSettings.autoRefresh
      ? window.setInterval(pullLatest, uiSettings.pollIntervalMs)
      : null;

    return () => {
      active = false;
      if (interval !== null) {
        window.clearInterval(interval);
      }
    };
  }, [uiSettings.autoRefresh, uiSettings.pollIntervalMs]);

  useEffect(() => {
    let active = true;
    setHistoryLoading(true);
    setHistoryError('');
    setHistory([]);

    const pullHistory = async (showLoading = false) => {
      if (showLoading && active) {
        setHistoryLoading(true);
      }

      try {
        const response = await fetch(
          `${API_BASE_URL}/history?minutes=${selectedHistoryRange.minutes}&window=${selectedHistoryRange.windowSeconds}`
        );

        if (!response.ok) {
          throw new Error(`History HTTP ${response.status}`);
        }

        const payload = await response.json();

        if (!active) {
          return;
        }

        setHistory(Array.isArray(payload?.points) ? payload.points : []);
        setHistoryError('');
      } catch (error) {
        if (active) {
          setHistoryError(
            error instanceof Error ? error.message : 'Unable to fetch temperature history'
          );
        }
      } finally {
        if (active) {
          setHistoryLoading(false);
        }
      }
    };

    pullHistory(true);

    const interval = uiSettings.autoRefresh
      ? window.setInterval(() => pullHistory(false), uiSettings.historyRefreshMs)
      : null;

    return () => {
      active = false;
      if (interval !== null) {
        window.clearInterval(interval);
      }
    };
  }, [
    selectedHistoryRange,
    historyReloadKey,
    uiSettings.autoRefresh,
    uiSettings.historyRefreshMs
  ]);

  useEffect(() => {
    const timer = window.setInterval(() => {
      setNowTick(Date.now());
    }, 1000);

    return () => window.clearInterval(timer);
  }, []);

  const nodeCards = useMemo(
    () =>
      NODE_IDS.map((nodeId) => {
        const nodeData = latestByNode?.[nodeId] || {};
        const online = isNodeOnline(
          nodeData.lastSeenAt,
          nowTick,
          uiSettings.offlineTimeoutMs
        );
        const temperatureValid = nodeData.tempValid === true;
        const temp =
          online && temperatureValid && Number.isFinite(nodeData.temp_avg)
            ? nodeData.temp_avg
            : null;
        const statusKnown = online && Number.isInteger(nodeData.status);
        const status = statusKnown ? nodeData.status : null;
        const sensorFaultBits = statusKnown
          ? decodeSensorBits(status)
          : SENSOR_IDS.map(() => null);
        const faultDetailKnown =
          online &&
          nodeData.faultDetailValid === true &&
          Array.isArray(nodeData.faults) &&
          nodeData.faults.length === SENSOR_IDS.length;
        const sensorFaultCodes = faultDetailKnown
          ? nodeData.faults.map((faultCode) =>
              Number.isInteger(faultCode) ? faultCode : null
            )
          : SENSOR_IDS.map(() => null);
        const sensorFaultLabels = sensorFaultCodes.map((faultCode) =>
          decodeSensorFaultLabels(nodeId, faultCode)
        );
        const faults = sensorFaultBits.filter((value) => value === true).length;
        const unknownSensors = sensorFaultBits.filter((value) => value === null).length;
        const healthySensors = Math.max(0, SENSOR_IDS.length - faults - unknownSensors);
        const rssi = getFiniteNumber(nodeData.rssi, nodeData.rssiDbm, nodeData.rssi_dbm);
        const lastSeenTimestamp = Date.parse(nodeData.lastSeenAt || '');
        const sampleAgeMs = Number.isFinite(lastSeenTimestamp)
          ? Math.max(0, nowTick - lastSeenTimestamp)
          : null;

        return {
          id: nodeId,
          seq: Number.isInteger(nodeData.seq) ? nodeData.seq : null,
          temp,
          temperatureValid,
          status,
          online,
          lastSeenAt: nodeData.lastSeenAt || null,
          lastReceivedAt: nodeData.lastReceivedAt || null,
          tempUpdatedAt: nodeData.tempUpdatedAt || null,
          rssi,
          sampleAgeMs,
          sensorFaultBits,
          sensorFaultCodes,
          sensorFaultLabels,
          faultDetailKnown,
          faults,
          unknownSensors,
          healthySensors
        };
      }),
    [latestByNode, nowTick, uiSettings.offlineTimeoutMs]
  );

  const normalizedSearch = search.trim().toLowerCase();

  const filteredNodes = useMemo(() => {
    return nodeCards.filter((node) => {
      const statusMatches =
        statusFilter === 'all' ||
        (statusFilter === 'online' && node.online) ||
        (statusFilter === 'offline' && !node.online) ||
        (statusFilter === 'fault' && node.online && node.faults > 0);

      if (!statusMatches) {
        return false;
      }

      if (!normalizedSearch) {
        return true;
      }

      const numericId = Number(node.id.replace('node', ''));
      const tone = getNodeTone(node);
      const temperatureLabel =
        node.temp === null ? '' : `${node.temp.toFixed(uiSettings.temperatureDecimals)} ${Math.round(node.temp)}`;
      const faultLabels = node.sensorFaultLabels
        .flatMap((labels) => (Array.isArray(labels) ? labels : []))
        .join(' ');
      const searchableTerms = [
        node.id,
        formatNodeName(node.id),
        Number.isFinite(numericId) ? `node ${numericId}` : '',
        Number.isFinite(numericId) ? String(numericId) : '',
        tone,
        node.online ? 'online' : 'offline',
        node.faults > 0 ? 'fault' : '',
        temperatureLabel,
        faultLabels
      ]
        .join(' ')
        .toLowerCase();

      return searchableTerms.includes(normalizedSearch);
    });
  }, [nodeCards, normalizedSearch, statusFilter, uiSettings.temperatureDecimals]);

  const totalFaults = useMemo(
    () => nodeCards.reduce((sum, node) => sum + node.faults, 0),
    [nodeCards]
  );

  const avgTemp = useMemo(() => {
    const temperatures = nodeCards
      .filter((node) => node.online)
      .map((node) => node.temp)
      .filter((temperature) => temperature !== null);

    if (!temperatures.length) {
      return null;
    }

    return temperatures.reduce((sum, temperature) => sum + temperature, 0) /
      temperatures.length;
  }, [nodeCards]);

  const offlineNodes = nodeCards.filter((node) => !node.online).length;
  const activeNodes = nodeCards.filter((node) => node.online).length;
  const lastMessageTimestamp = Date.parse(systemHealth.lastMessageAt || '');
  const hasFreshGatewayMessage =
    Number.isFinite(lastMessageTimestamp) &&
    nowTick - lastMessageTimestamp <= uiSettings.offlineTimeoutMs;
  const gatewayOnline =
    !lastError &&
    systemHealth.mqttConnected &&
    (hasFreshGatewayMessage || activeNodes > 0);
  const backendHealthy = !lastError;
  const influxKnown = typeof systemHealth.influxWorkerRunning === 'boolean';
  const influxHealthy =
    influxKnown &&
    systemHealth.influxWorkerRunning === true &&
    !systemHealth.lastInfluxError;

  const freshestSampleAt = useMemo(() => {
    const timestamps = [systemHealth.lastMessageAt, ...nodeCards.map((node) => node.lastSeenAt)]
      .map((value) => Date.parse(value || ''))
      .filter((value) => Number.isFinite(value));

    if (!timestamps.length) {
      return null;
    }

    return new Date(Math.max(...timestamps)).toISOString();
  }, [nodeCards, systemHealth.lastMessageAt]);

  const alerts = useMemo(() => {
    const entries = [];

    nodeCards.forEach((node) => {
      if (!node.online) {
        if (node.lastSeenAt && Number.isFinite(node.sampleAgeMs)) {
          entries.push(`${formatNodeName(node.id)} offline for ${formatDuration(node.sampleAgeMs)}`);
        } else {
          entries.push(`${formatNodeName(node.id)} offline — no telemetry yet`);
        }
        return;
      }

      if (node.faults > 0) {
        entries.push(
          `${formatNodeName(node.id)} has ${node.faults} sensor fault${node.faults > 1 ? 's' : ''}`
        );
      }
    });

    return entries;
  }, [nodeCards]);

  const chartNodes = filteredNodes.filter((node) => visibleSeries[node.id]);
  const ActiveMenuIcon =
    [...MAIN_MENU_ITEMS, ...GENERAL_MENU_ITEMS].find((item) => item.label === activeMenu)?.icon ??
    LayoutDashboard;

  const selectHistoryRange = (minutes) => {
    setHistoryRangeMinutes(minutes);
    setChartResetKey((value) => value + 1);
  };

  const toggleSeries = (nodeId) => {
    setVisibleSeries((current) => ({
      ...current,
      [nodeId]: !current[nodeId]
    }));
  };

  const handleMenuClick = (label) => {
    setActiveMenu(label);
    setSidebarOpen(false);
  };

  return (
    <div className="layout-shell">
      {sidebarOpen && (
        <button
          type="button"
          className="sidebar-backdrop"
          aria-label="Close navigation"
          onClick={() => setSidebarOpen(false)}
        />
      )}

      <aside className={`sidebar${sidebarOpen ? ' mobile-open' : ''}`}>
        <div className="brand">
          <span className="brand-icon" aria-hidden="true">
            <Cpu size={22} strokeWidth={2.1} />
          </span>
          <div className="brand-copy">
            <h2>IoT System</h2>
            <p>Temperature Monitoring</p>
          </div>
          <button
            type="button"
            className="sidebar-close"
            onClick={() => setSidebarOpen(false)}
            aria-label="Close sidebar"
          >
            <X size={19} />
          </button>
        </div>

        <div className="menu-section">
          <p className="menu-section-title">MENU</p>
          <div className="menu-group">
            {MAIN_MENU_ITEMS.map((item) => {
              const Icon = item.icon;
              return (
                <button
                  key={item.label}
                  type="button"
                  className={`menu-item${activeMenu === item.label ? ' active' : ''}`}
                  onClick={() => handleMenuClick(item.label)}
                >
                  <span className="menu-item-content">
                    <span className="menu-icon">
                      <Icon size={17} strokeWidth={2} />
                    </span>
                    <span>{item.label}</span>
                  </span>
                </button>
              );
            })}
          </div>
        </div>

        <div className="menu-section">
          <p className="menu-section-title">GENERAL</p>
          <div className="menu-group">
            {GENERAL_MENU_ITEMS.map((item) => {
              const Icon = item.icon;
              return (
                <button
                  key={item.label}
                  type="button"
                  className={`menu-item${activeMenu === item.label ? ' active' : ''}`}
                  onClick={() => handleMenuClick(item.label)}
                >
                  <span className="menu-item-content">
                    <span className="menu-icon">
                      <Icon size={17} strokeWidth={2} />
                    </span>
                    <span>{item.label}</span>
                  </span>
                </button>
              );
            })}
          </div>
        </div>
      </aside>

      <main className="dashboard">
        <header className="topbar">
          <div className="topbar-main">
            <button
              type="button"
              className="mobile-menu-btn"
              onClick={() => setSidebarOpen(true)}
              aria-label="Open navigation"
            >
              <Menu size={21} />
            </button>

            <div className="search-box">
              <Search className="search-icon" size={19} strokeWidth={2} aria-hidden="true" />
              <input
                value={search}
                onChange={(event) => setSearch(event.target.value)}
                placeholder="Search nodes, status, faults..."
                aria-label="Search nodes"
              />
              {normalizedSearch && (
                <span className="search-result-count" aria-live="polite">
                  {filteredNodes.length}/{nodeCards.length}
                </span>
              )}
              {search && (
                <button
                  type="button"
                  className="search-clear"
                  onClick={() => setSearch('')}
                  aria-label="Clear search"
                  title="Clear search"
                >
                  <X size={17} strokeWidth={2.2} />
                </button>
              )}
            </div>
          </div>

          <div className="freshness-chip" title={freshestSampleAt || undefined}>
            <Clock size={16} strokeWidth={2.1} aria-hidden="true" />
            <span>Last update:</span>
            <strong>{formatRelativeTimestamp(freshestSampleAt, nowTick)}</strong>
          </div>
        </header>

        <div className="dashboard-body">
          <section className="heading-row">
            <div className="heading-copy">
              <h1 className="title-with-icon">
                <span className="page-title-icon">
                  <ActiveMenuIcon size={22} strokeWidth={2.2} />
                </span>
                <span>{activeMenu}</span>
              </h1>
              {activeMenu === 'Dashboard' && (
                <p>Real-time node telemetry and sensor health</p>
              )}
            </div>
            {activeMenu === 'Dashboard' && (
              <button className="refresh-btn" onClick={() => window.location.reload()}>
                <RefreshCw size={16} strokeWidth={2.2} aria-hidden="true" />
                <span>Refresh</span>
              </button>
            )}
          </section>

          {activeMenu === 'Dashboard' ? (
            <>
              {alerts.length > 0 && (
                <section className="alert-banner" aria-live="polite">
                  <AlertTriangle size={19} strokeWidth={2.2} />
                  <div>
                    <strong>Attention required</strong>
                    <span>{alerts.slice(0, 3).join(' · ')}</span>
                    {alerts.length > 3 && <small>+{alerts.length - 3} more</small>}
                  </div>
                </section>
              )}

              {lastError && (
                <section className="error-banner">
                  Backend connection error: {lastError}
                </section>
              )}

              <section className="filter-toolbar">
                <div className="filter-heading">
                  <SlidersHorizontal size={16} strokeWidth={2} />
                  <span>Filter</span>
                </div>
                <div className="filter-pills" role="group" aria-label="Filter nodes by status">
                  {FILTER_OPTIONS.map((option) => (
                    <button
                      key={option.id}
                      type="button"
                      className={`filter-pill${statusFilter === option.id ? ' active' : ''}`}
                      onClick={() => setStatusFilter(option.id)}
                    >
                      {option.label}
                    </button>
                  ))}
                </div>
                <span className="filter-result">
                  {filteredNodes.length} of {nodeCards.length} nodes
                </span>
              </section>

              <section className="stat-grid">
                <article className="stat-card highlight">
                  <h3 className="metric-title">
                    <Server size={17} strokeWidth={2} />
                    <span>Total Nodes</span>
                  </h3>
                  <p>
                    <AnimatedNumber value={NODE_IDS.length} />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <Wifi size={17} strokeWidth={2} />
                    <span>Online Nodes</span>
                  </h3>
                  <p>
                    <AnimatedNumber value={activeNodes} />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <WifiOff size={17} strokeWidth={2} />
                    <span>Offline Nodes</span>
                  </h3>
                  <p>
                    <AnimatedNumber value={offlineNodes} />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <Thermometer size={17} strokeWidth={2} />
                    <span>Average Temp</span>
                  </h3>
                  <p>
                    <AnimatedNumber
                      value={avgTemp}
                      decimals={uiSettings.temperatureDecimals}
                      suffix="°C"
                    />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <AlertTriangle size={17} strokeWidth={2} />
                    <span>Fault Sensors</span>
                  </h3>
                  <p>
                    <AnimatedNumber value={totalFaults} />
                  </p>
                </article>
              </section>

              <section className="system-health-strip" aria-label="System health">
                <HealthItem
                  icon={Wifi}
                  label="Gateway"
                  state={gatewayOnline ? 'Online' : 'Offline'}
                  tone={gatewayOnline ? 'good' : 'bad'}
                />
                <HealthItem
                  icon={Radio}
                  label="MQTT"
                  state={systemHealth.mqttConnected ? 'Connected' : 'Disconnected'}
                  tone={systemHealth.mqttConnected ? 'good' : 'bad'}
                />
                <HealthItem
                  icon={Server}
                  label="Backend"
                  state={backendHealthy ? 'Healthy' : 'Unavailable'}
                  tone={backendHealthy ? 'good' : 'bad'}
                />
                <HealthItem
                  icon={Database}
                  label="InfluxDB"
                  state={
                    !influxKnown ? 'Unknown' : influxHealthy ? 'Healthy' : 'Degraded'
                  }
                  detail={
                    Number.isInteger(systemHealth.outboxPendingCount) &&
                    systemHealth.outboxPendingCount > 0
                      ? `${systemHealth.outboxPendingCount} pending`
                      : undefined
                  }
                  tone={!influxKnown ? 'unknown' : influxHealthy ? 'good' : 'warn'}
                />
              </section>

              <section className="content-grid">
                <article className="panel chart-panel">
                  <div className="panel-head chart-panel-head">
                    <div>
                      <h2 className="panel-title-with-icon">
                        <BarChart3 size={18} strokeWidth={2} />
                        <span>Temperature</span>
                      </h2>
                      <p className="panel-subtitle">Historical temperature by node</p>
                    </div>
                    <div className="chart-actions">
                      <div className="range-switch" role="group" aria-label="Temperature time range">
                        {HISTORY_RANGES.map((range) => (
                          <button
                            key={range.minutes}
                            type="button"
                            className={historyRangeMinutes === range.minutes ? 'active' : ''}
                            onClick={() => selectHistoryRange(range.minutes)}
                          >
                            {range.label}
                          </button>
                        ))}
                      </div>
                      <button
                        type="button"
                        className="chart-reset-btn"
                        onClick={() => setChartResetKey((value) => value + 1)}
                        title="Reset chart zoom"
                      >
                        <RotateCcw size={16} />
                        <span>Reset zoom</span>
                      </button>
                    </div>
                  </div>

                  <div className="series-legend" aria-label="Toggle chart nodes">
                    {filteredNodes.map((node) => (
                      <button
                        key={node.id}
                        type="button"
                        className={`series-toggle${visibleSeries[node.id] ? ' active' : ''}`}
                        style={{ '--series-color': NODE_COLORS[node.id] }}
                        onClick={() => toggleSeries(node.id)}
                        aria-pressed={visibleSeries[node.id]}
                      >
                        <span className="series-swatch" />
                        {formatNodeName(node.id)}
                      </button>
                    ))}
                  </div>

                  <div className="chart-frame">
                    {historyLoading ? (
                      <div className="chart-skeleton" aria-label="Loading temperature history">
                        <span />
                        <span />
                        <span />
                        <span />
                      </div>
                    ) : historyError && history.length === 0 ? (
                      <div className="chart-state error">
                        <AlertTriangle size={26} />
                        <strong>History unavailable</strong>
                        <span>{historyError}</span>
                        <button
                          type="button"
                          onClick={() => setHistoryReloadKey((value) => value + 1)}
                        >
                          Retry
                        </button>
                      </div>
                    ) : history.length === 0 ? (
                      <div className="chart-state">
                        <BarChart3 size={26} />
                        <strong>No history data</strong>
                        <span>No valid samples in the selected range.</span>
                      </div>
                    ) : chartNodes.length === 0 ? (
                      <div className="chart-state">
                        <Search size={26} />
                        <strong>No visible node series</strong>
                        <span>Clear the search/filter or enable a node from the legend.</span>
                      </div>
                    ) : (
                      <ResponsiveContainer width="100%" height={340}>
                        <LineChart
                          data={history}
                          margin={{ top: 16, right: 18, left: 0, bottom: 8 }}
                        >
                          {uiSettings.chartGrid && (
                            <CartesianGrid strokeDasharray="4 4" stroke="var(--chart-grid)" />
                          )}
                          <XAxis
                            dataKey="time"
                            minTickGap={34}
                            stroke="var(--chart-axis)"
                            tickFormatter={formatHistoryTime}
                          />
                          <YAxis stroke="var(--chart-axis)" unit="°C" width={48} />
                          <Tooltip
                            contentStyle={{
                              background: 'var(--surface-strong)',
                              border: '1px solid var(--border)',
                              color: 'var(--slate-800)',
                              borderRadius: '14px',
                              boxShadow: '0 14px 30px rgba(15, 23, 42, 0.12)'
                            }}
                            formatter={(value, name) => [
                              value === null || value === undefined
                                ? 'N/A'
                                : `${Number(value).toFixed(uiSettings.temperatureDecimals)} °C`,
                              name
                            ]}
                            labelFormatter={(value) => `Time ${formatHistoryTime(value)}`}
                          />
                          {chartNodes.map((node) => (
                            <Line
                              key={node.id}
                              type="monotone"
                              dataKey={node.id}
                              name={formatNodeName(node.id)}
                              stroke={NODE_COLORS[node.id]}
                              strokeWidth={2.8}
                              dot={false}
                              activeDot={{ r: 4, strokeWidth: 2, stroke: 'var(--chart-active-dot-stroke)' }}
                              connectNulls={false}
                              isAnimationActive={false}
                            />
                          ))}
                          <Brush
                            key={`${chartResetKey}-${historyRangeMinutes}`}
                            dataKey="time"
                            height={26}
                            travellerWidth={8}
                            stroke="var(--chart-brush-stroke)"
                            fill="var(--chart-brush-fill)"
                            tickFormatter={formatHistoryTime}
                          />
                        </LineChart>
                      </ResponsiveContainer>
                    )}
                  </div>
                  {historyError && history.length > 0 && (
                    <div className="chart-inline-warning">
                      <AlertTriangle size={15} /> Showing cached history — refresh failed.
                    </div>
                  )}
                </article>

                <article className="panel node-panel">
                  <div className="panel-head">
                    <div>
                      <h2 className="panel-title-with-icon">
                        <Cpu size={18} strokeWidth={2} />
                        <span>Node Status</span>
                      </h2>
                      <p className="panel-subtitle">Live state, freshness and sensor summary</p>
                    </div>
                  </div>

                  {latestLoading ? (
                    <div className="node-card-grid">
                      {NODE_IDS.map((nodeId) => (
                        <div className="node-card skeleton-card" key={nodeId}>
                          <span />
                          <span />
                          <span />
                        </div>
                      ))}
                    </div>
                  ) : filteredNodes.length === 0 ? (
                    <div className="empty-search-state">
                      No nodes match the current search and status filter.
                    </div>
                  ) : (
                    <div className="node-card-grid">
                      {filteredNodes.map((node) => {
                        const tone = getNodeTone(node);
                        const statusLabel =
                          tone === 'offline'
                            ? 'OFFLINE'
                            : tone === 'fault'
                              ? 'FAULT'
                              : tone === 'unknown'
                                ? 'UNKNOWN'
                                : 'ONLINE';

                        return (
                          <article className={`node-card ${tone}`} key={node.id}>
                            <div className="node-card-head">
                              <div>
                                <span className="node-card-kicker">Telemetry node</span>
                                <h3>{formatNodeName(node.id)}</h3>
                              </div>
                              <span className={`node-status-pill ${tone}`}>{statusLabel}</span>
                            </div>

                            <div className="node-temperature">
                              <span>Temperature</span>
                              <strong>
                                {node.online && node.temp !== null
                                  ? `${node.temp.toFixed(uiSettings.temperatureDecimals)} °C`
                                  : '-- °C'}
                              </strong>
                            </div>

                            <div className="node-meta-grid">
                              <div>
                                <span className="node-meta-label">
                                  <Radio size={15} /> RSSI
                                </span>
                                <strong title={node.rssi === null ? 'RSSI is not exposed by the current backend payload.' : undefined}>
                                  {node.rssi === null ? '-- dBm' : `${Math.round(node.rssi)} dBm`}
                                </strong>
                              </div>
                              <div>
                                <span className="node-meta-label">
                                  <Clock size={15} /> Last sample
                                </span>
                                <strong>
                                  {node.sampleAgeMs === null
                                    ? '--'
                                    : formatRelativeTimestamp(node.lastSeenAt, nowTick)}
                                </strong>
                              </div>
                            </div>

                            <div className="node-sensor-summary">
                              <span>Sensors</span>
                              <strong>
                                {node.online
                                  ? `${node.healthySensors}/${SENSOR_IDS.length} OK`
                                  : '--'}
                              </strong>
                            </div>
                          </article>
                        );
                      })}
                    </div>
                  )}
                </article>

                <article className="panel sensor-panel">
                  <div className="panel-head">
                    <div>
                      <h2 className="panel-title-with-icon">
                        <Thermometer size={18} strokeWidth={2} />
                        <span>Sensor Status</span>
                      </h2>
                      <p className="panel-subtitle">Detailed fault state for six sensors per node</p>
                    </div>
                  </div>

                  {latestLoading ? (
                    <div className="sensor-skeleton-grid">
                      {NODE_IDS.map((nodeId) => (
                        <div className="sensor-skeleton" key={nodeId} />
                      ))}
                    </div>
                  ) : filteredNodes.length === 0 ? (
                    <div className="empty-search-state">
                      No sensor groups match the current search and status filter.
                    </div>
                  ) : (
                    <div className="sensor-scroll">
                      {filteredNodes.map((node) => (
                        <div className="sensor-node" key={node.id}>
                          <div className="sensor-node-head">
                            <strong>{formatNodeName(node.id)}</strong>
                            <span>
                              {node.online
                                ? `${node.healthySensors}/${SENSOR_IDS.length} OK`
                                : 'Offline'}
                            </span>
                          </div>
                          <div className="sensor-grid">
                            {SENSOR_IDS.map((sensorId) => {
                              const isFault = node.sensorFaultBits[sensorId];
                              const unknown = isFault === null;
                              const detailedLabels = node.sensorFaultLabels[sensorId];
                              const faultSummary =
                                isFault && Array.isArray(detailedLabels)
                                  ? formatSensorFaultSummary(detailedLabels)
                                  : isFault
                                    ? 'Fault'
                                    : 'OK';
                              const faultTitle =
                                isFault &&
                                Array.isArray(detailedLabels) &&
                                detailedLabels.length > 0
                                  ? detailedLabels.join(', ')
                                  : undefined;

                              return (
                                <div
                                  className={`sensor-chip ${
                                    unknown ? 'unknown' : isFault ? 'fault' : 'ok'
                                  }`}
                                  key={sensorId}
                                  title={faultTitle}
                                >
                                  <span>S{sensorId + 1}</span>
                                  <b>{unknown ? 'Unknown' : faultSummary}</b>
                                </div>
                              );
                            })}
                          </div>
                        </div>
                      ))}
                    </div>
                  )}
                </article>
              </section>
            </>
          ) : activeMenu === 'Settings' ? (
            <section className="utility-page settings-page interactive-settings-page">
              <div className="utility-intro settings-hero">
                <div className="utility-intro-icon">
                  <Settings size={24} strokeWidth={2.1} />
                </div>
                <div className="settings-hero-copy">
                  <span className="utility-eyebrow">Dashboard preferences</span>
                  <h2>Settings</h2>
                  <p>
                    Customize appearance, refresh cadence and data freshness behavior.
                    Changes apply immediately and are saved in this browser.
                  </p>
                </div>
                <button type="button" className="settings-reset-btn" onClick={resetUiSettings}>
                  <RotateCcw size={16} />
                  Reset defaults
                </button>
              </div>

              <div className="settings-live-note">
                <span className="settings-save-dot" />
                Saved automatically on this device
              </div>

              <div className="utility-grid settings-control-grid">
                <article className="utility-card settings-control-card">
                  <div className="utility-card-head">
                    <SlidersHorizontal size={19} />
                    <div>
                      <h3>Appearance</h3>
                      <p>Theme and typography for the entire dashboard.</p>
                    </div>
                  </div>

                  <div className="setting-field">
                    <div className="setting-field-copy">
                      <strong>Theme</strong>
                      <span>Choose a light, dark or OS-matched background.</span>
                    </div>
                    <div className="segmented-setting theme-setting" role="group" aria-label="Theme">
                      {[
                        { id: 'light', label: 'Light', icon: Sun },
                        { id: 'dark', label: 'Dark', icon: Moon },
                        { id: 'system', label: 'System', icon: Monitor }
                      ].map(({ id, label, icon: Icon }) => (
                        <button
                          type="button"
                          key={id}
                          className={uiSettings.theme === id ? 'active' : ''}
                          onClick={() => updateUiSetting('theme', id)}
                        >
                          <Icon size={15} />
                          {label}
                        </button>
                      ))}
                    </div>
                  </div>

                  <div className="setting-field">
                    <div className="setting-field-copy">
                      <strong className="setting-label-with-icon">
                        <Type size={15} /> Font size
                      </strong>
                      <span>Scale text while keeping the dashboard proportions balanced.</span>
                    </div>
                    <div className="segmented-setting font-setting" role="group" aria-label="Font size">
                      {FONT_SCALE_OPTIONS.map((scale) => (
                        <button
                          type="button"
                          key={scale}
                          className={uiSettings.fontScale === scale ? 'active' : ''}
                          onClick={() => updateUiSetting('fontScale', scale)}
                        >
                          {scale}%
                        </button>
                      ))}
                    </div>
                  </div>
                </article>

                <article className="utility-card settings-control-card">
                  <div className="utility-card-head">
                    <Clock size={19} />
                    <div>
                      <h3>Refresh & freshness</h3>
                      <p>Control network polling and when a node is considered offline.</p>
                    </div>
                  </div>

                  <div className="setting-field setting-field-inline">
                    <div className="setting-field-copy">
                      <strong>Auto refresh</strong>
                      <span>Automatically fetch new telemetry and chart history.</span>
                    </div>
                    <button
                      type="button"
                      className={`switch-control ${uiSettings.autoRefresh ? 'on' : ''}`}
                      onClick={() => updateUiSetting('autoRefresh', !uiSettings.autoRefresh)}
                      aria-pressed={uiSettings.autoRefresh}
                    >
                      <span />
                      <b>{uiSettings.autoRefresh ? 'On' : 'Off'}</b>
                    </button>
                  </div>

                  <label className="setting-select-row">
                    <span>
                      <strong>Latest telemetry</strong>
                      <small>Polling interval for /latest and /health</small>
                    </span>
                    <select
                      value={uiSettings.pollIntervalMs}
                      disabled={!uiSettings.autoRefresh}
                      onChange={(event) =>
                        updateUiSetting('pollIntervalMs', Number(event.target.value))
                      }
                    >
                      {POLL_INTERVAL_OPTIONS.map((value) => (
                        <option value={value} key={value}>{value / 1000}s</option>
                      ))}
                    </select>
                  </label>

                  <label className="setting-select-row">
                    <span>
                      <strong>History refresh</strong>
                      <small>How often the selected chart range is refreshed</small>
                    </span>
                    <select
                      value={uiSettings.historyRefreshMs}
                      disabled={!uiSettings.autoRefresh}
                      onChange={(event) =>
                        updateUiSetting('historyRefreshMs', Number(event.target.value))
                      }
                    >
                      {HISTORY_REFRESH_OPTIONS.map((value) => (
                        <option value={value} key={value}>{value / 1000}s</option>
                      ))}
                    </select>
                  </label>

                  <label className="setting-select-row">
                    <span>
                      <strong>Offline threshold</strong>
                      <small>No fresh packet within this period means Offline</small>
                    </span>
                    <select
                      value={uiSettings.offlineTimeoutMs}
                      onChange={(event) =>
                        updateUiSetting('offlineTimeoutMs', Number(event.target.value))
                      }
                    >
                      {OFFLINE_THRESHOLD_OPTIONS.map((value) => (
                        <option value={value} key={value}>{value / 1000}s</option>
                      ))}
                    </select>
                  </label>
                </article>

                <article className="utility-card settings-control-card">
                  <div className="utility-card-head">
                    <BarChart3 size={19} />
                    <div>
                      <h3>Chart preferences</h3>
                      <p>Default time range and temperature presentation.</p>
                    </div>
                  </div>

                  <div className="setting-field">
                    <div className="setting-field-copy">
                      <strong>Default time range</strong>
                      <span>Also switches the current chart to the selected range.</span>
                    </div>
                    <div className="segmented-setting range-setting" role="group" aria-label="Default chart range">
                      {HISTORY_RANGES.map((range) => (
                        <button
                          type="button"
                          key={range.minutes}
                          className={
                            uiSettings.defaultHistoryRangeMinutes === range.minutes
                              ? 'active'
                              : ''
                          }
                          onClick={() => updateDefaultHistoryRange(range.minutes)}
                        >
                          {range.label}
                        </button>
                      ))}
                    </div>
                  </div>

                  <div className="setting-field">
                    <div className="setting-field-copy">
                      <strong>Temperature precision</strong>
                      <span>Number of decimal places shown in cards and tooltips.</span>
                    </div>
                    <div className="segmented-setting compact-setting" role="group" aria-label="Temperature precision">
                      {[1, 2].map((decimals) => (
                        <button
                          type="button"
                          key={decimals}
                          className={uiSettings.temperatureDecimals === decimals ? 'active' : ''}
                          onClick={() => updateUiSetting('temperatureDecimals', decimals)}
                        >
                          {decimals} decimal{decimals > 1 ? 's' : ''}
                        </button>
                      ))}
                    </div>
                  </div>

                  <div className="setting-field setting-field-inline">
                    <div className="setting-field-copy">
                      <strong>Chart grid</strong>
                      <span>Show horizontal and vertical guide lines.</span>
                    </div>
                    <button
                      type="button"
                      className={`switch-control ${uiSettings.chartGrid ? 'on' : ''}`}
                      onClick={() => updateUiSetting('chartGrid', !uiSettings.chartGrid)}
                      aria-pressed={uiSettings.chartGrid}
                    >
                      <span />
                      <b>{uiSettings.chartGrid ? 'On' : 'Off'}</b>
                    </button>
                  </div>
                </article>

                <article className="utility-card settings-summary-card">
                  <div className="utility-card-head">
                    <Activity size={19} />
                    <div>
                      <h3>Current behavior</h3>
                      <p>A quick summary of the settings active in this browser.</p>
                    </div>
                  </div>

                  <dl className="settings-summary-list">
                    <div><dt>Theme</dt><dd>{uiSettings.theme}</dd></div>
                    <div><dt>Font</dt><dd>{uiSettings.fontScale}%</dd></div>
                    <div>
                      <dt>Live polling</dt>
                      <dd>{uiSettings.autoRefresh ? `${uiSettings.pollIntervalMs / 1000}s` : 'Manual'}</dd>
                    </div>
                    <div>
                      <dt>History refresh</dt>
                      <dd>{uiSettings.autoRefresh ? `${uiSettings.historyRefreshMs / 1000}s` : 'Manual'}</dd>
                    </div>
                    <div><dt>Offline after</dt><dd>{uiSettings.offlineTimeoutMs / 1000}s</dd></div>
                    <div>
                      <dt>Default range</dt>
                      <dd>{HISTORY_RANGES.find((range) => range.minutes === uiSettings.defaultHistoryRangeMinutes)?.label}</dd>
                    </div>
                  </dl>
                </article>
              </div>
            </section>
          ) : activeMenu === 'Help' ? (
            <section className="utility-page help-page">
              <div className="utility-intro">
                <div className="utility-intro-icon">
                  <CircleHelp size={24} strokeWidth={2.1} />
                </div>
                <div>
                  <span className="utility-eyebrow">Operations guide</span>
                  <h2>Help & Troubleshooting</h2>
                  <p>
                    Quick reference for understanding node states and locating failures
                    across the STM32 → Gateway → Backend → Frontend pipeline.
                  </p>
                </div>
              </div>

              <div className="utility-grid help-grid">
                <article className="utility-card help-wide-card">
                  <div className="utility-card-head">
                    <Activity size={19} />
                    <div>
                      <h3>Data flow</h3>
                      <p>Normal path from sensing to visualization.</p>
                    </div>
                  </div>
                  <div className="help-flow">
                    <span><Cpu size={16} /> STM32 Nodes</span>
                    <b>→</b>
                    <span><Radio size={16} /> LoRa Gateway</span>
                    <b>→</b>
                    <span><Wifi size={16} /> MQTT</span>
                    <b>→</b>
                    <span><Server size={16} /> Backend</span>
                    <b>→</b>
                    <span><Database size={16} /> InfluxDB</span>
                    <b>→</b>
                    <span><BarChart3 size={16} /> Dashboard</span>
                  </div>
                </article>

                <article className="utility-card">
                  <div className="utility-card-head">
                    <Thermometer size={19} />
                    <div>
                      <h3>Status meanings</h3>
                      <p>How to interpret node and sensor states.</p>
                    </div>
                  </div>
                  <div className="status-guide">
                    <div>
                      <span className="guide-dot good" />
                      <strong>Online</strong>
                      <p>Fresh telemetry is arriving within the offline threshold.</p>
                    </div>
                    <div>
                      <span className="guide-dot bad" />
                      <strong>Offline</strong>
                      <p>No recent telemetry has been received from the node.</p>
                    </div>
                    <div>
                      <span className="guide-dot warn" />
                      <strong>Fault</strong>
                      <p>At least one sensor reports a diagnostic fault.</p>
                    </div>
                    <div>
                      <span className="guide-dot unknown" />
                      <strong>Unknown</strong>
                      <p>Status data is incomplete or has not arrived yet.</p>
                    </div>
                  </div>
                </article>

                <article className="utility-card">
                  <div className="utility-card-head">
                    <AlertTriangle size={19} />
                    <div>
                      <h3>Fast troubleshooting</h3>
                      <p>Check the failing layer before changing firmware.</p>
                    </div>
                  </div>
                  <ol className="troubleshooting-list">
                    <li>
                      <strong>No node telemetry</strong>
                      <span>Check Gateway serial log for POLL, DATA, ACK and LoRa RSSI.</span>
                    </li>
                    <li>
                      <strong>Dashboard is stale</strong>
                      <span>Check <code>/latest</code> and the Last update indicator.</span>
                    </li>
                    <li>
                      <strong>Chart unavailable</strong>
                      <span>Check <code>/history</code> and confirm InfluxDB query access.</span>
                    </li>
                    <li>
                      <strong>InfluxDB outage</strong>
                      <span>Check <code>/ingest-status</code>; pending should drain after recovery.</span>
                    </li>
                  </ol>
                </article>

                <article className="utility-card help-wide-card">
                  <div className="utility-card-head">
                    <Server size={19} />
                    <div>
                      <h3>Useful checks</h3>
                      <p>Quick commands to verify the main services on the Ubuntu host.</p>
                    </div>
                  </div>
                  <div className="command-grid">
                    <code>systemctl is-active mosquitto</code>
                    <code>systemctl is-active influxdb</code>
                    <code>curl http://127.0.0.1:3000/health</code>
                    <code>curl http://127.0.0.1:3000/latest</code>
                    <code>curl http://127.0.0.1:3000/ingest-status</code>
                    <code>curl &apos;http://127.0.0.1:3000/history?minutes=60&amp;window=5&apos;</code>
                  </div>
                </article>
              </div>
            </section>
          ) : (
            <section className="placeholder-panel">
              <div>
                <Activity size={28} />
                <h2>Session</h2>
                <p>No additional session actions are configured.</p>
              </div>
            </section>
          )}
        </div>
      </main>
    </div>
  );
}

export default App;
