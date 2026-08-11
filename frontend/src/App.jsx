import { useEffect, useMemo, useRef, useState } from 'react';
import PropTypes from 'prop-types';
import {
  CartesianGrid,
  Legend,
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
  CalendarDays,
  CircleHelp,
  Cpu,
  LayoutDashboard,
  LogOut,
  SlidersHorizontal,
  Server,
  Settings,
  Thermometer,
  Wifi,
  WifiOff
} from 'lucide-react';
import './App.css';

const API_BASE_URL =
  import.meta.env.VITE_API_BASE_URL ||
  `${window.location.protocol}//${window.location.hostname}:3000`;
const NODE_IDS = ['node01', 'node02', 'node03'];
const SENSOR_IDS = [0, 1, 2, 3, 4, 5];
const POLL_INTERVAL_MS = 2000;
const HISTORY_REFRESH_MS = 5000;
const HISTORY_RANGE_MINUTES = 15;
const HISTORY_WINDOW_SECONDS = 5;
const OFFLINE_TIMEOUT_MS_RAW = Number(import.meta.env.VITE_NODE_OFFLINE_MS || 12000);
const OFFLINE_TIMEOUT_MS = Number.isFinite(OFFLINE_TIMEOUT_MS_RAW)
  ? OFFLINE_TIMEOUT_MS_RAW
  : 12000;

const NODE_COLORS = {
  node01: '#0a8f60',
  node02: '#35b37e',
  node03: '#1e6a4c'
};
const MAIN_MENU_ITEMS = [
  { label: 'Dashboard', icon: LayoutDashboard },
  { label: 'Control', icon: SlidersHorizontal },
  { label: 'Calendar', icon: CalendarDays }
];
const GENERAL_MENU_ITEMS = [
  { label: 'Settings', icon: Settings },
  { label: 'Help', icon: CircleHelp },
  { label: 'Logout', icon: LogOut }
];

const WEEK_DAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
const MAINTENANCE_TEMPLATE = [
  { node: 'node01', day: 5, note: 'Ve sinh dieu hoa' },
  { node: 'node02', day: 12, note: 'Kiem tra gas' },
  { node: 'node03', day: 19, note: 'Hieu chuan cam bien' },
  { node: 'node01', day: 26, note: 'Kiem tra cam bien' }
];

function decodeSensorBits(statusByte) {
  if (!Number.isInteger(statusByte)) {
    return SENSOR_IDS.map(() => null);
  }

  return SENSOR_IDS.map((bit) => ((statusByte >> bit) & 0x01) === 1);
}

const COMMON_SENSOR_FAULT_LABELS = [
  [0x01, 'Short'],
  [0x04, 'Noisy'],
  [0x08, 'Resistance'],
  [0x10, 'Temp range'],
  [0x20, 'Rate'],
  [0x40, 'Cross sensor'],
  [0x80, 'Model']
];

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

function formatDateKey(date) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}

function isNodeOnline(lastSeenAt, nowMs) {
  if (!lastSeenAt) return false;

  const timestamp = Date.parse(lastSeenAt);

  if (Number.isNaN(timestamp)) return false;

  return nowMs - timestamp <= OFFLINE_TIMEOUT_MS;
}

function formatHistoryTime(value) {
  const timestamp = Date.parse(value);

  if (Number.isNaN(timestamp)) {
    return value || '';
  }

  return new Date(timestamp).toLocaleTimeString('en-GB', { hour12: false });
}

function AnimatedNumber({
  value,
  decimals = 0,
  suffix = '',
  fallback = '--',
  className = '',
  duration = 700
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

    if (!Number.isFinite(from)) {
      setDisplayValue(target);
      return;
    }

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

function App() {
  const [latestByNode, setLatestByNode] = useState({});
  const [history, setHistory] = useState([]);
  const [lastError, setLastError] = useState('');
  const [gatewayHealth, setGatewayHealth] = useState({ mqttConnected: false, lastMessageAt: null });
  const [search, setSearch] = useState('');
  const [activeMenu, setActiveMenu] = useState('Dashboard');
  const [nowTick, setNowTick] = useState(Date.now());
  const [acStates, setAcStates] = useState([false, false, false]);
  const [controlMode, setControlMode] = useState('auto');

  const acControls = useMemo(
    () => [
      { id: 'ac-1', label: 'Điều hòa 1' },
      { id: 'ac-2', label: 'Điều hòa 2' },
      { id: 'ac-3', label: 'Điều hòa 3' }
    ],
    []
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
          throw new Error(`HTTP ${latestResponse.status}`);
        }

        if (!healthResponse.ok) {
          throw new Error(`HTTP ${healthResponse.status}`);
        }

        const [latestPayload, healthPayload] = await Promise.all([
          latestResponse.json(),
          healthResponse.json()
        ]);

        if (!active) return;

        setLatestByNode(latestPayload);
        setGatewayHealth({
          mqttConnected: healthPayload?.mqttConnected === true,
          lastMessageAt: healthPayload?.lastMessageAt || null
        });
        setLastError('');
      } catch (error) {
        if (active) {
          setLastError(error instanceof Error ? error.message : 'Unable to fetch data');
        }
      }
    };

    pullLatest();
    const interval = window.setInterval(pullLatest, POLL_INTERVAL_MS);

    return () => {
      active = false;
      window.clearInterval(interval);
    };
  }, []);

  useEffect(() => {
    let active = true;

    const pullHistory = async () => {
      try {
        const response = await fetch(
          `${API_BASE_URL}/history?minutes=${HISTORY_RANGE_MINUTES}&window=${HISTORY_WINDOW_SECONDS}`
        );

        if (!response.ok) {
          throw new Error(`History HTTP ${response.status}`);
        }

        const payload = await response.json();

        if (!active) return;

        setHistory(Array.isArray(payload?.points) ? payload.points : []);
      } catch (error) {
        if (active) {
          console.error('Unable to fetch history:', error);
        }
      }
    };

    pullHistory();
    const interval = window.setInterval(pullHistory, HISTORY_REFRESH_MS);

    return () => {
      active = false;
      window.clearInterval(interval);
    };
  }, []);

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
        const online = isNodeOnline(nodeData.lastSeenAt, nowTick);
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

        return {
          id: nodeId,
          seq: Number.isInteger(nodeData.seq) ? nodeData.seq : null,
          temp,
          temperatureValid,
          status,
          online,
          lastSeenAt: nodeData.lastSeenAt || null,
          tempUpdatedAt: nodeData.tempUpdatedAt || null,
          sensorFaultBits,
          sensorFaultCodes,
          sensorFaultLabels,
          faultDetailKnown,
          faults,
          unknownSensors
        };
      }),
    [latestByNode, nowTick]
  );
  const filteredNodes = useMemo(
    () =>
      nodeCards.filter((node) =>
        node.id.toLowerCase().includes(search.trim().toLowerCase())
      ),
    [nodeCards, search]
  );

  const totalFaults = useMemo(
    () => nodeCards.reduce((sum, node) => sum + node.faults, 0),
    [nodeCards]
  );

  const avgTemp = useMemo(() => {
    const temps = nodeCards
      .filter((node) => node.online)
      .map((node) => node.temp)
      .filter((temp) => temp !== null);
    if (!temps.length) return null;
    return temps.reduce((sum, temp) => sum + temp, 0) / temps.length;
  }, [nodeCards]);

  const totalSensors = NODE_IDS.length * SENSOR_IDS.length;
  const offlineNodes = nodeCards.filter((node) => !node.online).length;
  const activeNodes = nodeCards.filter((node) => node.online).length;
  const lastMessageTimestamp = Date.parse(gatewayHealth.lastMessageAt || '');
  const hasFreshGatewayMessage =
    Number.isFinite(lastMessageTimestamp) && nowTick - lastMessageTimestamp <= OFFLINE_TIMEOUT_MS;
  const gatewayOnline =
    !lastError && gatewayHealth.mqttConnected && (hasFreshGatewayMessage || activeNodes > 0);
  const unknownSensors = nodeCards.reduce((sum, node) => sum + node.unknownSensors, 0);
  const healthySensors = Math.max(0, totalSensors - totalFaults - unknownSensors);
  const healthPercent = Math.round((healthySensors / totalSensors) * 100);
  const healthTone = healthPercent >= 85 ? 'good' : healthPercent >= 60 ? 'warn' : 'bad';
  const healthColor = healthTone === 'good' ? '#13865c' : healthTone === 'warn' ? '#d97706' : '#dc2626';
  const ActiveMenuIcon =
    [...MAIN_MENU_ITEMS, ...GENERAL_MENU_ITEMS].find((item) => item.label === activeMenu)?.icon ??
    LayoutDashboard;
  const now = useMemo(() => new Date(nowTick), [nowTick]);
  const currentYear = now.getFullYear();
  const currentMonth = now.getMonth();
  const monthLabel = now.toLocaleDateString('en-GB', { month: 'long', year: 'numeric' });
  const todayLabel = now.toLocaleDateString('en-GB', {
    weekday: 'long',
    day: '2-digit',
    month: 'short',
    year: 'numeric'
  });
  const todayKey = formatDateKey(now);
  const daysInMonth = new Date(currentYear, currentMonth + 1, 0).getDate();
  const firstDayIndex = (new Date(currentYear, currentMonth, 1).getDay() + 6) % 7;
  const calendarCells = useMemo(() => {
    const cells = Array.from({ length: firstDayIndex }, () => null);
    for (let day = 1; day <= daysInMonth; day += 1) {
      cells.push(day);
    }
    return cells;
  }, [firstDayIndex, daysInMonth]);
  const maintenanceSchedule = useMemo(() => {
    return MAINTENANCE_TEMPLATE.map((item) => {
      const date = new Date(currentYear, currentMonth, item.day);
      return {
        ...item,
        date: formatDateKey(date)
      };
    }).sort((a, b) => a.date.localeCompare(b.date));
  }, [currentYear, currentMonth]);
  const maintenanceByDate = useMemo(() => {
    return maintenanceSchedule.reduce((acc, item) => {
      if (!acc[item.date]) {
        acc[item.date] = [];
      }
      acc[item.date].push(item);
      return acc;
    }, {});
  }, [maintenanceSchedule]);

  const toggleAc = (index) => {
    setAcStates((prev) => prev.map((value, idx) => (idx === index ? !value : value)));
  };

  return (
    <div className="layout-shell">
      <aside className="sidebar">
        <div className="brand">
          <span className="brand-icon">◎</span>
          <div>
            <h2>Monitor</h2>
          </div>
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
                  onClick={() => setActiveMenu(item.label)}
                >
                  <span className="menu-item-content">
                    <span className="menu-icon">
                      <Icon size={16} strokeWidth={2} />
                    </span>
                    <span>{item.label}</span>
                  </span>
                  {item.badge && <span className="menu-badge">{item.badge}</span>}
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
                  onClick={() => setActiveMenu(item.label)}
                >
                  <span className="menu-item-content">
                    <span className="menu-icon">
                      <Icon size={16} strokeWidth={2} />
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
          <div className="search-box">
            <span className="search-icon">⌕</span>
            <input
              value={search}
              onChange={(event) => setSearch(event.target.value)}
              placeholder="Search"
            />
          </div>

          <div className="topbar-right">
            <div className={`connection-pill ${gatewayOnline ? 'good' : 'bad'}`}>
              <span className="pill-dot" />
              {gatewayOnline ? 'Online' : 'Offline'}
            </div>
            <div className="profile-chip">
              <div className="avatar">NNH</div>
              <div>
                <strong>Nguyen Ngoc Hai</strong>
              </div>
            </div>
          </div>
        </header>
        <div className="search-dashboard-divider" />
        <div className="dashboard-body">
          <section className="heading-row">
            <h1 className="title-with-icon">
              <ActiveMenuIcon size={24} strokeWidth={2.2} />
              <span>{activeMenu}</span>
            </h1>
            <button className="refresh-btn" onClick={() => window.location.reload()}>
              Refresh
            </button>
          </section>

          {activeMenu === 'Dashboard' ? (
            <>
              {lastError && <div className="error-banner">Backend connection error: {lastError}</div>}

              <section className="stat-grid">
                <article className="stat-card highlight">
                  <h3 className="metric-title">
                    <Server size={16} strokeWidth={2} />
                    <span>Total Nodes</span>
                  </h3>
                  <p>
                    <AnimatedNumber className="value-pop" value={NODE_IDS.length} />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <Wifi size={16} strokeWidth={2} />
                    <span>Online Nodes</span>
                  </h3>
                  <p>
                    <AnimatedNumber className="value-pop" value={activeNodes} />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <WifiOff size={16} strokeWidth={2} />
                    <span>Offline Nodes</span>
                  </h3>
                  <p>
                    <AnimatedNumber className="value-pop" value={offlineNodes} />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <Thermometer size={16} strokeWidth={2} />
                    <span>Average Temp</span>
                  </h3>
                  <p>
                    <AnimatedNumber className="value-pop" value={avgTemp} decimals={2} suffix="°C" />
                  </p>
                </article>
                <article className="stat-card">
                  <h3 className="metric-title">
                    <AlertTriangle size={16} strokeWidth={2} />
                    <span>Fault Sensors</span>
                  </h3>
                  <p>
                    <AnimatedNumber className="value-pop" value={totalFaults} />
                  </p>
                </article>
              </section>

              <section className="content-grid">
                <article className="panel chart-panel live-panel">
                  <div className="panel-head">
                    <h2 className="panel-title-with-icon">
                      <BarChart3 size={17} strokeWidth={2} />
                      <span>Temperature</span>
                    </h2>
                  </div>
                  <ResponsiveContainer width="100%" height={210}>
                    <LineChart data={history} margin={{ top: 10, right: 12, left: 0, bottom: 0 }}>
                      <defs>
                        {NODE_IDS.map((nodeId) => (
                          <linearGradient id={`tempLine-${nodeId}`} key={nodeId} x1="0" y1="0" x2="1" y2="0">
                            <stop offset="0%" stopColor={NODE_COLORS[nodeId]} stopOpacity="0.65" />
                            <stop offset="100%" stopColor={NODE_COLORS[nodeId]} stopOpacity="1" />
                          </linearGradient>
                        ))}
                      </defs>
                      <CartesianGrid strokeDasharray="4 4" stroke="#e6ebea" />
                      <XAxis dataKey="time" minTickGap={26} stroke="#8aa19a" tickFormatter={formatHistoryTime} />
                      <YAxis stroke="#8aa19a" unit="°C" />
                      <Tooltip
                        contentStyle={{
                          background: '#ffffff',
                          border: '1px solid #dde5e2',
                          borderRadius: '12px'
                        }}
                        formatter={(value) =>
                          value === null || value === undefined ? 'N/A' : `${Number(value).toFixed(2)} °C`
                        }
                      
                        labelFormatter={formatHistoryTime}
                      />
                      <Legend />
                      {NODE_IDS.map((nodeId) => (
                      <Line
                        key={nodeId}
                        type="monotone"
                        dataKey={nodeId}
                        name={formatNodeName(nodeId)}
                        stroke={`url(#tempLine-${nodeId})`}
                        strokeWidth={3}
                        dot={false}
                        activeDot={{ r: 4, strokeWidth: 2, stroke: '#ffffff' }}
                        connectNulls={false}
                        isAnimationActive={true}
                        animationDuration={700}
                        animationEasing="ease-out"
                      />
                      ))}
                    </LineChart>
                  </ResponsiveContainer>
                </article>

                <article className="panel node-panel">
                  <div className="panel-head">
                    <h2 className="panel-title-with-icon">
                      <Cpu size={17} strokeWidth={2} />
                      <span>Node Status</span>
                    </h2>
                  </div>
                  <div className="node-list">
                    {filteredNodes.map((node, index) => {
                      const tone = !node.online
                        ? 'offline'
                        : node.unknownSensors > 0
                          ? 'unknown'
                          : node.faults > 0
                            ? 'fault'
                            : 'ok';
                      return (
                      <div
                        className={`node-row ${tone}`}
                        key={node.id}
                        style={{ '--row-delay': `${index * 45}ms` }}
                      >
                        <div className="row-left">
                          <span className={`node-dot ${tone}`} />
                          <strong>{formatNodeName(node.id)}</strong>
                        </div>
                        <div className="row-right">
                          <span>{node.online && node.temp !== null ? `${node.temp.toFixed(2)}°C` : '--'}</span>
                          <em className={tone}>
                            {!node.online
                              ? 'Offline'
                              : node.unknownSensors > 0
                                ? 'Unknown'
                                : node.faults > 0
                                  ? `${node.faults} faults`
                                  : 'All good'}
                          </em>
                        </div>
                      </div>
                      );
                    })}
                  </div>
                </article>

                <article className="panel sensor-panel">
                  <div className="panel-head">
                    <h2 className="panel-title-with-icon">
                      <Thermometer size={17} strokeWidth={2} />
                      <span>Sensor Status</span>
                    </h2>
                  </div>
                  <div className="sensor-scroll">
                    {filteredNodes.map((node, nodeIndex) => (
                      <div className="sensor-node" key={node.id} style={{ '--node-delay': `${nodeIndex * 70}ms` }}>
                        <div className="sensor-node-head">
                          <strong>{formatNodeName(node.id)}</strong>
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
                              isFault && Array.isArray(detailedLabels) && detailedLabels.length > 0
                                ? detailedLabels.join(', ')
                                : undefined;

                            return (
                              <div
                                className={`sensor-chip ${unknown ? 'unknown' : isFault ? 'fault' : 'ok'}`}
                                key={sensorId}
                                style={{ '--chip-delay': `${sensorId * 45}ms` }}
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
                </article>

                <article className={`panel progress-panel ${healthTone}`}>
                  <div className="panel-head">
                    <h2 className="panel-title-with-icon">
                      <Activity size={17} strokeWidth={2} />
                      <span>System Health</span>
                    </h2>
                  </div>
                  <div
                    className={`gauge ${healthTone}`}
                    style={{
                      background: `conic-gradient(${healthColor} ${healthPercent * 3.6}deg, #e1ebe7 0deg)`
                    }}
                  >
                    <div className="gauge-inner">
                      <strong>
                        <AnimatedNumber className="value-pop" value={healthPercent} suffix="%" />
                      </strong>
                    </div>
                  </div>
                </article>
              </section>
            </>
          ) : activeMenu === 'Control' ? (
            <section className="control-grid">
              <article className="panel control-panel">
                <div className="panel-head">
                  <h2 className="panel-title-with-icon">
                    <SlidersHorizontal size={17} strokeWidth={2} />
                    <span>Control</span>
                  </h2>
                </div>
                <div className="control-list">
                  {acControls.map((item, index) => {
                    const isOn = acStates[index];
                    return (
                      <div className={`control-row ${isOn ? 'on' : 'off'}`} key={item.id}>
                        <div className="row-left">
                          <span className={`control-dot ${isOn ? 'on' : 'off'}`} />
                          <strong>{item.label}</strong>
                        </div>
                        <button
                          type="button"
                          className={`toggle-btn ${isOn ? 'on' : 'off'}`}
                          onClick={() => toggleAc(index)}
                        >
                          {isOn ? 'ON' : 'OFF'}
                        </button>
                      </div>
                    );
                  })}
                  <div className="control-row mode-row">
                    <div className="row-left">
                      <span className={`control-dot ${controlMode === 'auto' ? 'on' : 'off'}`} />
                      <strong>Mode</strong>
                    </div>
                    <div className="mode-switch">
                      <button
                        type="button"
                        className={`mode-btn ${controlMode === 'auto' ? 'active' : ''}`}
                        onClick={() => setControlMode('auto')}
                      >
                        Auto
                      </button>
                      <button
                        type="button"
                        className={`mode-btn ${controlMode === 'manual' ? 'active' : ''}`}
                        onClick={() => setControlMode('manual')}
                      >
                        Manual
                      </button>
                    </div>
                  </div>
                </div>
              </article>
            </section>
          ) : activeMenu === 'Calendar' ? (
            <section className="calendar-layout">
              <article className="panel calendar-panel">
                <div className="panel-head">
                  <h2 className="panel-title-with-icon">
                    <CalendarDays size={17} strokeWidth={2} />
                    <span>Maintenance Calendar</span>
                  </h2>
                  <div className="calendar-now">
                    <span className="calendar-month">{monthLabel}</span>
                    <span className="calendar-time">{todayLabel}</span>
                    <span className="calendar-time">{now.toLocaleTimeString('en-GB', { hour12: false })}</span>
                  </div>
                </div>
                <div className="calendar-grid">
                  {WEEK_DAYS.map((day) => (
                    <div key={day} className="calendar-weekday">
                      {day}
                    </div>
                  ))}
                  {calendarCells.map((day, index) => {
                    if (!day) {
                      return <div key={`empty-${index}`} className="calendar-cell empty" />;
                    }
                    const date = new Date(currentYear, currentMonth, day);
                    const dateKey = formatDateKey(date);
                    const events = maintenanceByDate[dateKey] || [];
                    const isToday = dateKey === todayKey;
                    return (
                      <div
                        key={dateKey}
                        className={`calendar-cell${isToday ? ' today' : ''}${events.length ? ' has-event' : ''}`}
                      >
                        <span className="calendar-day">{day}</span>
                        <div className="calendar-dots">
                          {events.map((event, idx) => (
                            <span
                              key={`${event.node}-${idx}`}
                              className="calendar-dot"
                              style={{ backgroundColor: NODE_COLORS[event.node] || '#0f8b5f' }}
                            />
                          ))}
                        </div>
                      </div>
                    );
                  })}
                </div>
              </article>
              <article className="panel calendar-notes">
                <div className="panel-head">
                  <h2 className="panel-title-with-icon">
                    <Thermometer size={17} strokeWidth={2} />
                    <span>Maintenance Notes</span>
                  </h2>
                </div>
                <div className="maintenance-list">
                  {maintenanceSchedule.map((item) => {
                    const isToday = item.date === todayKey;
                    const nodeColor = NODE_COLORS[item.node] || '#0f8b5f';
                    return (
                      <div
                        key={`${item.node}-${item.date}`}
                        className={`maintenance-item${isToday ? ' today' : ''}`}
                      >
                        <div>
                          <div className="maintenance-note">{item.note}</div>
                          <div className="maintenance-date">{item.date}</div>
                        </div>
                        <span
                          className="node-pill"
                          style={{ backgroundColor: `${nodeColor}22`, color: nodeColor }}
                        >
                          {formatNodeName(item.node)}
                        </span>
                      </div>
                    );
                  })}
                </div>
              </article>
            </section>
          ) : (
            <section className="placeholder-panel">
              <div>
                <h2>Coming soon</h2>
                <p>Section is under construction.</p>
              </div>
            </section>
          )}
        </div>
      </main>
    </div>
  );
}

export default App;
