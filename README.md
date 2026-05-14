# lowpower-tcp-gateway

A lightweight, asyncio-based TCP gateway designed for low-power IoT devices
(NB-IoT, GPRS, LoRa, etc.).  Devices connect over TCP, speak a compact binary
protocol, and the gateway forwards their data to configurable backends (MQTT,
HTTP/REST).

---

## Features

| Feature | Details |
|---|---|
| **Async I/O** | Built on Python `asyncio` – handles thousands of concurrent connections efficiently |
| **Binary protocol** | 10-byte fixed header; supports REGISTER, HEARTBEAT, DATA, ACK, CMD, CMD_RESP |
| **Session management** | Per-device session registry; idle/heartbeat timeouts; automatic reconnect handling |
| **MQTT backend** | Publishes device data to configurable topics via `aiomqtt` |
| **HTTP backend** | POSTs device data as JSON via `aiohttp` |
| **Config file** | YAML-based configuration with environment variable overrides |
| **Docker support** | Includes `Dockerfile` and `docker-compose.yml` |

---

## Protocol

Each frame has the following structure:

```
+--------+--------+---------+---------+---------+------------------+
| Magic  |  Type  | Length  | Dev ID  |   Seq   |     Payload      |
| 1 byte | 1 byte | 2 bytes | 4 bytes | 2 bytes |    0–65535 B     |
+--------+--------+---------+---------+---------+------------------+
```

| Field | Value |
|---|---|
| Magic | `0xAB` |
| Type | `0x01` REGISTER, `0x02` HEARTBEAT, `0x03` DATA, `0x04` ACK, `0x05` CMD, `0x06` CMD_RESP |
| Length | Payload length (big-endian) |
| Dev ID | 4-byte device identifier (big-endian) |
| Seq | 2-byte sequence number (big-endian) |

**Typical device lifecycle:**

1. TCP connect  
2. Send **REGISTER** frame → gateway replies **ACK**  
3. Send **DATA** frames periodically → gateway replies **ACK** and forwards data  
4. Send **HEARTBEAT** frames to maintain the session → gateway replies **ACK**  
5. TCP disconnect (device sleeps)

---

## Quick Start

### With Python

```bash
# Install dependencies
pip install -r requirements.txt

# Run with defaults (binds to 0.0.0.0:9000)
python main.py

# Custom config
python main.py --config /etc/gateway/config.yaml

# Override host/port on the command line
python main.py --host 0.0.0.0 --port 9000 --log-level DEBUG
```

### With Docker

```bash
docker build -t lowpower-tcp-gateway .
docker run -p 9000:9000 lowpower-tcp-gateway
```

### With Docker Compose

```bash
docker-compose up -d
```

---

## Configuration

Copy `config.yaml` and edit as needed:

```yaml
log_level: INFO

server:
  host: "0.0.0.0"
  port: 9000
  max_connections: 1000

timeouts:
  heartbeat: 60    # seconds before closing an idle connection
  session: 300     # seconds before reaping an inactive session

backends:
  mqtt:
    enabled: true
    host: "mqtt.example.com"
    port: 1883
    username: "user"
    password: "pass"
    topic_prefix: "gateway"

  http:
    enabled: true
    url: "https://api.example.com/data"
    auth_token: "your-token"
```

### Environment variable overrides

Every config key has a corresponding `GATEWAY_*` environment variable:

| Variable | Default |
|---|---|
| `GATEWAY_HOST` | `0.0.0.0` |
| `GATEWAY_PORT` | `9000` |
| `GATEWAY_LOG_LEVEL` | `INFO` |
| `GATEWAY_MQTT_ENABLED` | `false` |
| `GATEWAY_MQTT_HOST` | `localhost` |
| `GATEWAY_MQTT_PORT` | `1883` |
| `GATEWAY_MQTT_USERNAME` | _(empty)_ |
| `GATEWAY_MQTT_PASSWORD` | _(empty)_ |
| `GATEWAY_MQTT_TOPIC_PREFIX` | `gateway` |
| `GATEWAY_HTTP_ENABLED` | `false` |
| `GATEWAY_HTTP_URL` | `http://localhost:8080/data` |
| `GATEWAY_HTTP_AUTH_TOKEN` | _(empty)_ |

---

## MQTT Topics

When the MQTT backend is enabled, each device event is published as JSON:

| Event | Topic |
|---|---|
| Data | `{prefix}/devices/{device_id_hex}/data` |
| Registration | `{prefix}/devices/{device_id_hex}/register` |

Example payload:
```json
{
  "device_id": 305441741,
  "timestamp": 1700000000.123,
  "seq": 42,
  "payload": "7b2276223a34327d",
  "payload_size": 8
}
```

---

## Running Tests

```bash
pip install -r requirements-dev.txt
pytest
```

64 tests cover protocol encoding/decoding, session management, configuration
loading, and end-to-end server integration.

---

## Project Structure

```
lowpower-tcp-gateway/
├── gateway/
│   ├── __init__.py
│   ├── server.py          # Async TCP server
│   ├── protocol.py        # Binary protocol parser / encoder
│   ├── session.py         # Device session management
│   ├── router.py          # Message routing to backends
│   ├── config.py          # Configuration management
│   └── backends/
│       ├── mqtt.py        # MQTT publisher backend
│       └── http.py        # HTTP POST backend
├── tests/
│   ├── test_protocol.py
│   ├── test_session.py
│   ├── test_config.py
│   └── test_server.py
├── main.py                # Entry point
├── config.yaml            # Default configuration
├── requirements.txt
├── requirements-dev.txt
├── Dockerfile
└── docker-compose.yml
```
