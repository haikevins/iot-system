# Security Policy

## Supported Version

The repository currently tracks the latest code on `main`. Security fixes should target the current maintained version unless a release note states otherwise.

## Reporting a Vulnerability

Please do not open a public issue for a vulnerability that could expose credentials, permit unauthorized MQTT/API access, or disclose sensitive deployment information.

Use GitHub's private vulnerability reporting feature when it is available for this repository. If private reporting is not available, contact the repository owner privately through the contact method listed on the GitHub profile before publishing technical details.

A useful report includes:

- the affected component (`node01`, `node02`, `node03`, `gateway`, `broker`, `backend`, or `frontend`);
- the conditions required to reproduce the issue;
- the security impact;
- minimal reproduction steps or a proof of concept;
- any suggested mitigation, if known.

## Deployment Security Notes

This repository is an engineering project and its example configuration is not a production security baseline. Deployments should, at minimum:

- replace all placeholder MQTT and InfluxDB credentials;
- keep `backend/.env`, `frontend/.env`, and `gateway/include/secrets.h` out of version control;
- restrict broker and API exposure with host/network firewall rules;
- use least-privilege MQTT and InfluxDB credentials;
- configure CORS for the intended frontend origin rather than unrestricted access;
- use TLS or a trusted private network when MQTT, HTTP, or InfluxDB traffic crosses an untrusted network;
- rotate credentials if a local configuration file or token is accidentally exposed.

Known production-hardening boundaries are also documented in the root README and system technical reference.
