#include "AppConfig.h"

#include <fstream>
#include <stdexcept>

AppConfig AppConfig::load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("failed to open config: " + path);
    }

    nlohmann::json j;
    ifs >> j;

    AppConfig cfg;

    if (j.contains("server")) {
        auto s = j["server"];
        cfg.server.tcp_port = s.value("tcp_port", cfg.server.tcp_port);
        cfg.server.http_port = s.value("http_port", cfg.server.http_port);
        cfg.server.heartbeat_default_sec = s.value("heartbeat_default_sec", cfg.server.heartbeat_default_sec);
        cfg.server.idle_timeout_sec = s.value("idle_timeout_sec", cfg.server.idle_timeout_sec);
    }

    if (j.contains("redis")) {
        auto r = j["redis"];
        cfg.redis.host = r.value("host", cfg.redis.host);
        cfg.redis.port = r.value("port", cfg.redis.port);
        cfg.redis.password = r.value("password", cfg.redis.password);
        cfg.redis.db = r.value("db", cfg.redis.db);
        cfg.redis.prefix = r.value("prefix", cfg.redis.prefix);
    }

    if (j.contains("auth")) {
        auto a = j["auth"];
        cfg.auth.enable = a.value("enable", cfg.auth.enable);
        cfg.auth.timestamp_window_sec = a.value("timestamp_window_sec", cfg.auth.timestamp_window_sec);
        cfg.auth.dev_default_secret = a.value("dev_default_secret", cfg.auth.dev_default_secret);
    }

    if (j.contains("wake")) {
        auto w = j["wake"];
        cfg.wake.ack_timeout_ms = w.value("ack_timeout_ms", cfg.wake.ack_timeout_ms);
        cfg.wake.max_retry = w.value("max_retry", cfg.wake.max_retry);
        cfg.wake.expire_ms = w.value("expire_ms", cfg.wake.expire_ms);
    }

    return cfg;
}