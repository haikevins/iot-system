# IoT System — Frontend

> Responsive React monitoring dashboard for live STM32 node telemetry, temperature history, LoRa RSSI, sensor diagnostics, freshness, alerts and system health.

***

## Table of Contents

1. [Overview](#overview)
2. [Technology Stack](#technology-stack)
3. [Application Structure](#application-structure)
4. [Configuration](#configuration)
5. [Dashboard](#dashboard)
6. [Search and Filters](#search-and-filters)
7. [Temperature Chart](#temperature-chart)
8. [Node Cards](#node-cards)
9. [Sensor Diagnostics](#sensor-diagnostics)
10. [System Health and Alerts](#system-health-and-alerts)
11. [Settings](#settings)
12. [Help Page](#help-page)
13. [Light and Dark Themes](#light-and-dark-themes)
14. [Responsive Design](#responsive-design)
15. [API Integration](#api-integration)
16. [Loading and Error States](#loading-and-error-states)
17. [Development](#development)
18. [Production Build](#production-build)
19. [Troubleshooting](#troubleshooting)
20. [Known Limitations](#known-limitations)
21. [Future Improvements](#future-improvements)

***

## Overview

The frontend is the operator-facing layer of the IoT System.

It does not communicate directly with:

- STM32 nodes,
- ESP32 gateway,
- Mosquitto,
- InfluxDB.

Instead:

```text
Browser
   ↓ HTTP
Backend REST API
   ↓
latest + history + health
```

This separation keeps database credentials and MQTT credentials out of browser code.

Main navigation:

```text
Dashboard
Settings
Help
Logout
```

`Dashboard`, `Settings` and `Help` contain real application content. The project does not currently implement a complete user-authentication/logout backend.

***

## Technology Stack

| Technology | Purpose |
|---|---|
| React 18 | UI component model |
| React DOM | Browser rendering |
| Vite 5 | Development server and production build |
| Recharts | Temperature time-series chart |
| Lucide React | Consistent icon system |
| CSS | Responsive layout and theming |
| localStorage | Persistent UI preferences |

Current package scripts:

```json
{
  "scripts": {
    "dev": "vite",
    "build": "vite build",
    "lint": "eslint .",
    "preview": "vite preview"
  }
}
```

***

## Application Structure

```text
frontend/
├── README.md
├── .env.example
├── .env
├── package.json
├── vite.config.js
├── eslint.config.js
├── index.html
├── public/
│   ├── dashboard-background.png
│   └── iot-system.svg
└── src/
    ├── App.jsx
    ├── App.css
    ├── index.css
    └── main.jsx
```

The current UI is intentionally compact enough to remain in a small number of React/CSS files while the feature set is still evolving.

For a larger application, future refactoring should split:

- API client,
- settings context,
- chart components,
- node cards,
- sensor cards,
- layout,
- pages,

into dedicated modules.

***

## Configuration

Create:

```bash
cd frontend
cp .env.example .env
```

Typical LAN configuration:

```env
VITE_API_BASE_URL=http://<UBUNTU_LAN_IP>:3000
VITE_NODE_OFFLINE_MS=12000
```

### API address

If the browser runs on the same Ubuntu machine:

```env
VITE_API_BASE_URL=http://127.0.0.1:3000
```

If a phone/laptop accesses the Vite server over LAN:

```env
VITE_API_BASE_URL=http://192.168.x.x:3000
```

Do **not** use `127.0.0.1` in that case because it refers to the phone/laptop itself.

### Restart after environment changes

Vite reads environment variables at startup.

After changing `.env`:

```text
Ctrl+C
```

then:

```bash
npm run dev -- --host 0.0.0.0
```

***

## Dashboard

The Dashboard is the main operational page.

Top-level areas:

```text
Topbar
├── Search
└── Last update

Dashboard header
└── Refresh

Quick filter
├── All
├── Online
├── Offline
└── Fault

Summary cards
├── Total Nodes
├── Online Nodes
├── Offline Nodes
├── Average Temp
└── Fault Sensors

Temperature chart

Node Status
├── Node 01
├── Node 02
└── Node 03

Sensor Status

System Health
```

### Summary cards

The cards aggregate current backend state:

- total known nodes,
- currently online nodes,
- currently offline nodes,
- average valid temperature,
- total faulted sensors.

Invalid temperature samples are excluded instead of being treated as `0 °C`.

***

## Search and Filters

Search is a real state filter rather than a decorative input.

Supported concepts include:

```text
node01
Node 01
node 1
01
online
offline
fault
temperature text/value
```

Quick status filters:

```text
All | Online | Offline | Fault
```

Search and filter can operate together.

The filtered node list drives:

- node cards,
- sensor cards,
- chart series visibility/availability.

A result counter communicates how many nodes match.

***

## Temperature Chart

Historical data comes from:

```text
GET /history
```

The chart uses Recharts.

### Time ranges

| UI | Minutes | Window |
|---|---:|---:|
| 15m | 15 | 5 s |
| 1h | 60 | 5 s |
| 6h | 360 | 30 s |
| 24h | 1440 | 120 s |

Example request:

```text
/history?minutes=60&window=5
```

### Node toggles

Each node has a chart visibility toggle.

This makes it possible to:

- compare all nodes,
- isolate one sensor node,
- reduce visual clutter.

### Zoom

The chart uses a Recharts `Brush` for horizontal range selection.

UI also provides:

```text
Reset zoom
```

### Missing/invalid points

The chart is configured with:

```text
connectNulls = false
```

Therefore:

```text
valid ─── valid   null   valid ─── valid
```

renders a gap instead of a fake straight line through an invalid sample.

This is important for diagnostic integrity.

### Tooltip

The tooltip shows:

- time,
- Node 01 temperature,
- Node 02 temperature,
- Node 03 temperature,

for visible/available series.

Temperature decimal precision is configurable in Settings.

***

## Node Cards

Each node card combines radio, freshness and sensor information.

Conceptual layout:

```text
TELEMETRY NODE                     ONLINE
Node 01

Temperature
32.14 °C

RSSI                 Last sample
-51 dBm              4s ago

Sensors
6/6 OK
```

### Temperature

Displayed only when backend data is valid.

### RSSI

Source:

```text
SX1278 receive at ESP32 Gateway
      ↓
LoRa.packetRssi()
      ↓
backend rssiDbm
```

If RSSI is unavailable for an older record:

```text
-- dBm
```

is preferable to inventing a value.

### Last sample age

Freshness is calculated relative to backend timestamps.

The UI uses human-friendly labels:

```text
just now
4s ago
1m ago
```

### Online / Offline

A node is offline when its latest telemetry age exceeds the configured offline threshold.

Default:

```text
12 seconds
```

The threshold can be changed in Settings.

***

## Sensor Diagnostics

Each node has six sensors.

The frontend maps backend fault bytes into human-readable state.

Common labels include:

```text
OK
Short
Open
High saturation
Noisy
Resistance
Temperature range
Rate
Cross sensor
Model
Unknown
```

Node 01/02 bit `0x02` means:

```text
OPEN
```

Node 03 bit `0x02` means:

```text
HIGH_SAT
```

This difference is preserved because Node 03 uses the MCP3208/analog-gain path.

### Fault summary

The UI distinguishes:

- node online/offline,
- temperature validity,
- per-sensor health.

A radio-online node can still contain a bad sensor. These are separate concepts.

***

## System Health and Alerts

### Alert banner

The dashboard shows an alert only when action is useful.

Examples:

```text
Node 02 offline for 37s
```

or:

```text
Node 01 has sensor faults
```

When all nodes are healthy, no warning banner is displayed.

### System Health

The health row is intentionally compact:

```text
Gateway ● Online | MQTT ● Connected | Backend ● Healthy | InfluxDB ● Healthy
```

The frontend derives these indicators from data freshness and backend health/ingestion endpoints.

They are operational indicators, not a replacement for server logs.

### Last update

The topbar shows:

```text
Last update: just now
```

This is more useful than merely showing “Gateway Online”, because it tells the operator whether the browser is receiving fresh backend state.

***

## Settings

Settings are interactive and browser-persistent.

Storage:

```text
localStorage
key = iot-system-ui-settings-v2
```

### Appearance

Theme:

```text
Light
Dark
System
```

Font scale:

```text
90%
100%
110%
120%
```

### Refresh and freshness

Auto refresh:

```text
On / Off
```

Latest telemetry polling:

```text
1s
2s
5s
10s
```

History refresh:

```text
5s
10s
30s
60s
```

Offline threshold:

```text
10s
12s
15s
30s
60s
```

These values are functional.

For example:

```text
Telemetry polling 2s → 5s
```

changes the actual fetch interval.

### Chart preferences

Default range:

```text
15m
1h
6h
24h
```

Temperature precision:

```text
1 decimal
2 decimals
```

Grid:

```text
On
Off
```

### Reset defaults

The page provides a reset action that restores the default preference object.

Representative defaults:

```json
{
  "theme": "system",
  "fontScale": 100,
  "autoRefresh": true,
  "pollIntervalMs": 2000,
  "historyRefreshMs": 5000,
  "offlineTimeoutMs": 12000,
  "defaultHistoryRangeMinutes": 60,
  "temperatureDecimals": 2,
  "chartGrid": true
}
```

The environment-based offline value can be used as the initial default.

***

## Help Page

The Help page contains project-specific operational information rather than placeholder text.

Sections include:

- system data flow,
- status meanings,
- troubleshooting workflow,
- useful backend/service commands.

Data-flow presentation:

```text
STM32 Nodes
   →
LoRa Gateway
   →
MQTT
   →
Backend
   →
InfluxDB
   →
Dashboard
```

The page is styled for both light and dark themes.

***

## Light and Dark Themes

The application supports:

```text
light
dark
system
```

`system` follows the browser/OS color preference.

### Dark-mode design goals

Dark mode uses a green-black palette rather than isolated white panels.

Dark-theme fixes include:

- brighter section headings,
- readable node names,
- readable temperature labels,
- dark filter toolbar,
- dark chart-range controls,
- dark RSSI/Last Sample cards,
- tinted status badges,
- dark sensor surfaces,
- themed chart brush,
- improved chart axis contrast,
- themed Settings inputs/selects.

### Font stack

The UI uses a Linux/Ubuntu-friendly system stack conceptually based on:

```css
"Ubuntu",
"Inter",
"Segoe UI",
"Roboto",
system-ui,
sans-serif
```

The application does not require shipping proprietary font files.

***

## Responsive Design

Desktop:

```text
sidebar + main dashboard
```

Mobile:

- sidebar collapses behind hamburger menu,
- summary cards become two columns,
- chart uses full available width,
- node cards become one column,
- controls wrap,
- large flow diagrams stack vertically.

The goal is to keep the same monitoring features available on a phone rather than building a separate mobile application.

***

## API Integration

Frontend endpoint base:

```js
VITE_API_BASE_URL
```

Main requests:

```text
GET /latest
GET /history
GET /health
GET /ingest-status
```

### `/latest`

Used for:

- current temperature,
- online/offline,
- RSSI,
- faults,
- freshness.

### `/history`

Used for chart data.

### `/health`

Used for backend system state.

### `/ingest-status`

Used to infer database ingestion health and expose server-side reliability state.

***

## Loading and Error States

### Initial loading

Skeleton components are shown instead of a blank dashboard.

### History error

The chart area reports a useful error such as:

```text
History unavailable
```

with a retry action.

### Empty state

If no history exists for the selected period, the UI explains the empty state rather than rendering a misleading chart.

### Search empty state

If no nodes match:

```text
No nodes match the current search/filter.
```

***

## Development

Install:

```bash
cd frontend
npm ci
```

Run local-only:

```bash
npm run dev
```

Run for LAN access:

```bash
npm run dev -- --host 0.0.0.0
```

Vite default:

```text
http://localhost:5173
```

LAN:

```text
http://<UBUNTU_LAN_IP>:5173
```

Lint:

```bash
npm run lint
```

***

## Production Build

Build:

```bash
npm run build
```

Output:

```text
dist/
```

Preview:

```bash
npm run preview -- --host 0.0.0.0
```

A Vite warning about a JavaScript chunk above the default size threshold is not itself a build failure. Future code splitting can reduce bundle size.

***

## Troubleshooting

### Browser still shows old UI

Hard refresh:

```text
Ctrl + Shift + R
```

Restart Vite if source/environment changed.

### Tab still says `Vite + React`

Ensure `index.html` title is:

```html
<title>IoT System</title>
```

and the custom IoT favicon is referenced.

Then hard refresh/browser-cache refresh.

### Phone cannot open frontend

Check Vite:

```bash
npm run dev -- --host 0.0.0.0
```

Check:

```bash
sudo ss -ltnp | grep 5173
```

Firewall:

```bash
sudo ufw allow 5173/tcp
sudo ufw allow 3000/tcp
```

### Dashboard opens but has no data

From Ubuntu:

```bash
curl -s http://127.0.0.1:3000/latest | jq
```

If that works, verify `VITE_API_BASE_URL`.

### Chart missing

Test:

```bash
curl -s \
'http://127.0.0.1:3000/history?minutes=60&window=5' \
| jq '.points | length'
```

If count is positive, inspect browser console/network.

### Dark mode contains white boxes

Look for fixed CSS values such as:

```css
background: #fff;
```

inside components that should use theme variables or dark overrides.

### RSSI displays `--`

Check:

```bash
curl -s http://127.0.0.1:3000/latest | jq
```

New telemetry should include `rssiDbm`.

Old records created before RSSI support can legitimately have no RSSI.

***

## Known Limitations

- Settings are per-browser via `localStorage`, not synchronized across devices.
- No user login/role model is implemented yet.
- Logout is not backed by an authentication service.
- Theme is CSS-driven rather than a full design-token framework.
- Frontend health indicators depend on the information exposed by backend APIs.
- The current application is optimized for a small fixed set of three nodes.
- Long historical ranges rely on server-side aggregation rather than frontend virtualized datasets.
- Dashboard background imagery is aesthetic and may be removed for industrial deployments requiring maximum contrast.

***

## Future Improvements

1. Split `App.jsx` into page/component modules.
2. Add a typed API client layer.
3. Add React Query/SWR-style request caching.
4. Add URL-based routing.
5. Add user authentication.
6. Add role-based access.
7. Add persistent alert history.
8. Add per-sensor threshold configuration.
9. Add CSV export.
10. Add historical RSSI chart.
11. Add SNR/link-quality chart.
12. Add gateway queue depth to System Health.
13. Add live MQTT/server event stream using WebSocket/SSE.
14. Add accessibility audit.
15. Add component tests.
16. Add E2E browser tests.
17. Add code splitting.
18. Add production PWA/offline shell if useful.
19. Add configurable dashboard layouts.
20. Add localization.
