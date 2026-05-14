"""
Configuration management for the low-power TCP gateway.

Settings are loaded (in priority order):
1. Environment variables (prefixed with ``GATEWAY_``)
2. A YAML file (path given via ``--config`` or the ``GATEWAY_CONFIG`` env var)
3. Hard-coded defaults (below)
"""

import logging
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional

try:
    import yaml  # type: ignore
    HAS_YAML = True
except ImportError:
    HAS_YAML = False

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 9000
DEFAULT_MAX_CONNECTIONS = 1000
DEFAULT_HEARTBEAT_TIMEOUT = 60   # seconds
DEFAULT_SESSION_TIMEOUT = 300    # seconds
DEFAULT_READ_TIMEOUT = 30        # seconds
DEFAULT_MAX_FRAME_SIZE = 65535   # bytes


@dataclass
class MQTTConfig:
    """MQTT backend settings."""

    enabled: bool = False
    host: str = "localhost"
    port: int = 1883
    username: str = ""
    password: str = ""
    client_id: str = "lowpower-gateway"
    topic_prefix: str = "gateway"
    keepalive: int = 60
    qos: int = 1
    tls: bool = False


@dataclass
class HTTPConfig:
    """HTTP backend settings."""

    enabled: bool = False
    url: str = "http://localhost:8080/data"
    timeout: float = 5.0
    headers: Dict[str, str] = field(default_factory=dict)
    auth_token: str = ""


@dataclass
class GatewayConfig:
    """Top-level gateway configuration."""

    # TCP server
    host: str = DEFAULT_HOST
    port: int = DEFAULT_PORT
    max_connections: int = DEFAULT_MAX_CONNECTIONS

    # Timeouts
    heartbeat_timeout: int = DEFAULT_HEARTBEAT_TIMEOUT
    session_timeout: int = DEFAULT_SESSION_TIMEOUT
    read_timeout: int = DEFAULT_READ_TIMEOUT

    # Protocol
    max_frame_size: int = DEFAULT_MAX_FRAME_SIZE

    # Backends
    mqtt: MQTTConfig = field(default_factory=MQTTConfig)
    http: HTTPConfig = field(default_factory=HTTPConfig)

    # Logging
    log_level: str = "INFO"

    @classmethod
    def from_file(cls, path: str) -> "GatewayConfig":
        """Load configuration from a YAML file."""
        if not HAS_YAML:
            logger.warning("PyYAML not installed; ignoring config file %s", path)
            return cls()

        if not os.path.isfile(path):
            logger.warning("Config file not found: %s; using defaults", path)
            return cls()

        with open(path, "r") as fh:
            raw = yaml.safe_load(fh) or {}

        return cls._from_dict(raw)

    @classmethod
    def _from_dict(cls, raw: dict) -> "GatewayConfig":
        server = raw.get("server", {})
        timeouts = raw.get("timeouts", {})
        backends = raw.get("backends", {})
        mqtt_raw = backends.get("mqtt", {})
        http_raw = backends.get("http", {})

        mqtt = MQTTConfig(
            enabled=mqtt_raw.get("enabled", False),
            host=mqtt_raw.get("host", "localhost"),
            port=int(mqtt_raw.get("port", 1883)),
            username=mqtt_raw.get("username", ""),
            password=mqtt_raw.get("password", ""),
            client_id=mqtt_raw.get("client_id", "lowpower-gateway"),
            topic_prefix=mqtt_raw.get("topic_prefix", "gateway"),
            keepalive=int(mqtt_raw.get("keepalive", 60)),
            qos=int(mqtt_raw.get("qos", 1)),
            tls=mqtt_raw.get("tls", False),
        )

        http = HTTPConfig(
            enabled=http_raw.get("enabled", False),
            url=http_raw.get("url", "http://localhost:8080/data"),
            timeout=float(http_raw.get("timeout", 5.0)),
            headers=http_raw.get("headers", {}),
            auth_token=http_raw.get("auth_token", ""),
        )

        return cls(
            host=server.get("host", DEFAULT_HOST),
            port=int(server.get("port", DEFAULT_PORT)),
            max_connections=int(
                server.get("max_connections", DEFAULT_MAX_CONNECTIONS)
            ),
            heartbeat_timeout=int(
                timeouts.get("heartbeat", DEFAULT_HEARTBEAT_TIMEOUT)
            ),
            session_timeout=int(
                timeouts.get("session", DEFAULT_SESSION_TIMEOUT)
            ),
            read_timeout=int(timeouts.get("read", DEFAULT_READ_TIMEOUT)),
            max_frame_size=int(
                server.get("max_frame_size", DEFAULT_MAX_FRAME_SIZE)
            ),
            mqtt=mqtt,
            http=http,
            log_level=raw.get("log_level", "INFO"),
        )

    def apply_env_overrides(self) -> None:
        """Apply ``GATEWAY_*`` environment variable overrides."""
        overrides = {
            "GATEWAY_HOST": ("host", str),
            "GATEWAY_PORT": ("port", int),
            "GATEWAY_MAX_CONNECTIONS": ("max_connections", int),
            "GATEWAY_HEARTBEAT_TIMEOUT": ("heartbeat_timeout", int),
            "GATEWAY_SESSION_TIMEOUT": ("session_timeout", int),
            "GATEWAY_READ_TIMEOUT": ("read_timeout", int),
            "GATEWAY_LOG_LEVEL": ("log_level", str),
            "GATEWAY_MQTT_ENABLED": ("mqtt.enabled", lambda v: v.lower() == "true"),
            "GATEWAY_MQTT_HOST": ("mqtt.host", str),
            "GATEWAY_MQTT_PORT": ("mqtt.port", int),
            "GATEWAY_MQTT_USERNAME": ("mqtt.username", str),
            "GATEWAY_MQTT_PASSWORD": ("mqtt.password", str),
            "GATEWAY_MQTT_TOPIC_PREFIX": ("mqtt.topic_prefix", str),
            "GATEWAY_HTTP_ENABLED": ("http.enabled", lambda v: v.lower() == "true"),
            "GATEWAY_HTTP_URL": ("http.url", str),
            "GATEWAY_HTTP_AUTH_TOKEN": ("http.auth_token", str),
        }

        for env_key, (attr_path, cast) in overrides.items():
            val = os.environ.get(env_key)
            if val is None:
                continue
            try:
                parts = attr_path.split(".", 1)
                if len(parts) == 1:
                    setattr(self, parts[0], cast(val))
                else:
                    obj = getattr(self, parts[0])
                    setattr(obj, parts[1], cast(val))
            except (ValueError, AttributeError) as exc:
                logger.warning("Invalid env override %s=%r: %s", env_key, val, exc)
