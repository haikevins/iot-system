# Contributing

Thank you for taking an interest in this project. Changes should keep the repository reproducible and preserve the separation between embedded nodes, gateway, broker, backend, and frontend.

## Development Principles

- Keep each change focused on one subsystem or one repository-level concern.
- Do not commit credentials, local `.env` files, generated build output, or IDE user-state files.
- Preserve protocol compatibility unless a change explicitly updates both ends of the affected interface and its documentation.
- Update documentation when behavior, configuration, protocol, or verification steps change.
- Prefer small commits that can be reviewed and verified independently.

## Local Configuration

Create local configuration from the committed templates:

```bash
cp backend/.env.example backend/.env
cp frontend/.env.example frontend/.env
cp gateway/include/secrets.example.h gateway/include/secrets.h
```

The generated local files are intentionally ignored by Git.

## Verification

Run the checks that apply to the subsystem you changed.

### Frontend

```bash
cd frontend
npm ci
npm run lint
npm run build
```

### Backend

```bash
cd backend
npm ci
node --check src/index.js
```

Runtime verification that depends on MQTT or InfluxDB should follow the backend and system documentation.

### ESP32 gateway

```bash
cd gateway
pio run
```

Hardware upload and serial-monitor checks are documented in `gateway/README.md`.

### STM32 nodes

Build the relevant Keil µVision project and perform the node-specific hardware verification described in its README. Do not commit generated Keil output or per-user GUI state.

## Pull Requests

Before opening a pull request:

1. Confirm `git diff --check` is clean.
2. Run the applicable build, lint, or syntax checks.
3. Verify that no secrets or local configuration are staged.
4. Update documentation for externally visible behavior changes.
5. Summarize both the change and how it was verified.

The pull request template provides a compact checklist for these items.
