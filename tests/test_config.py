"""Unit tests for gateway.config."""

import os
import tempfile
import pytest

from gateway.config import GatewayConfig, MQTTConfig, HTTPConfig


class TestGatewayConfigDefaults:
    def test_default_host(self):
        cfg = GatewayConfig()
        assert cfg.host == "0.0.0.0"

    def test_default_port(self):
        cfg = GatewayConfig()
        assert cfg.port == 9000

    def test_default_mqtt_disabled(self):
        cfg = GatewayConfig()
        assert cfg.mqtt.enabled is False

    def test_default_http_disabled(self):
        cfg = GatewayConfig()
        assert cfg.http.enabled is False


class TestGatewayConfigFromFile:
    def _write_yaml(self, content: str) -> str:
        f = tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", delete=False
        )
        f.write(content)
        f.close()
        return f.name

    def test_load_basic_settings(self):
        path = self._write_yaml(
            """
server:
  host: "127.0.0.1"
  port: 9999
log_level: DEBUG
"""
        )
        cfg = GatewayConfig.from_file(path)
        assert cfg.host == "127.0.0.1"
        assert cfg.port == 9999
        assert cfg.log_level == "DEBUG"
        os.unlink(path)

    def test_load_mqtt_settings(self):
        path = self._write_yaml(
            """
backends:
  mqtt:
    enabled: true
    host: "mqtt.example.com"
    port: 8883
    username: "user"
    topic_prefix: "iot"
"""
        )
        cfg = GatewayConfig.from_file(path)
        assert cfg.mqtt.enabled is True
        assert cfg.mqtt.host == "mqtt.example.com"
        assert cfg.mqtt.port == 8883
        assert cfg.mqtt.username == "user"
        assert cfg.mqtt.topic_prefix == "iot"
        os.unlink(path)

    def test_load_http_settings(self):
        path = self._write_yaml(
            """
backends:
  http:
    enabled: true
    url: "https://api.example.com/data"
    auth_token: "secret"
"""
        )
        cfg = GatewayConfig.from_file(path)
        assert cfg.http.enabled is True
        assert cfg.http.url == "https://api.example.com/data"
        assert cfg.http.auth_token == "secret"
        os.unlink(path)

    def test_missing_file_returns_defaults(self):
        cfg = GatewayConfig.from_file("/nonexistent/config.yaml")
        assert cfg.port == 9000

    def test_load_timeouts(self):
        path = self._write_yaml(
            """
timeouts:
  heartbeat: 120
  session: 600
"""
        )
        cfg = GatewayConfig.from_file(path)
        assert cfg.heartbeat_timeout == 120
        assert cfg.session_timeout == 600
        os.unlink(path)


class TestGatewayConfigEnvOverrides:
    def test_host_override(self, monkeypatch):
        monkeypatch.setenv("GATEWAY_HOST", "192.168.1.1")
        cfg = GatewayConfig()
        cfg.apply_env_overrides()
        assert cfg.host == "192.168.1.1"

    def test_port_override(self, monkeypatch):
        monkeypatch.setenv("GATEWAY_PORT", "8080")
        cfg = GatewayConfig()
        cfg.apply_env_overrides()
        assert cfg.port == 8080

    def test_mqtt_enabled_override(self, monkeypatch):
        monkeypatch.setenv("GATEWAY_MQTT_ENABLED", "true")
        cfg = GatewayConfig()
        cfg.apply_env_overrides()
        assert cfg.mqtt.enabled is True

    def test_mqtt_host_override(self, monkeypatch):
        monkeypatch.setenv("GATEWAY_MQTT_HOST", "broker.example.com")
        cfg = GatewayConfig()
        cfg.apply_env_overrides()
        assert cfg.mqtt.host == "broker.example.com"

    def test_http_url_override(self, monkeypatch):
        monkeypatch.setenv("GATEWAY_HTTP_URL", "http://newhost/events")
        cfg = GatewayConfig()
        cfg.apply_env_overrides()
        assert cfg.http.url == "http://newhost/events"

    def test_invalid_port_is_ignored(self, monkeypatch):
        monkeypatch.setenv("GATEWAY_PORT", "not_a_number")
        cfg = GatewayConfig()
        original_port = cfg.port
        cfg.apply_env_overrides()
        assert cfg.port == original_port

    def test_no_env_vars_no_change(self):
        cfg = GatewayConfig()
        original_port = cfg.port
        cfg.apply_env_overrides()
        assert cfg.port == original_port
