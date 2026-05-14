#pragma once

#include <string>
#include <nlohmann/json.hpp>

struct ServerConfig {
    int tcp_port = 9000;
    int http_port = 8080;
    int heartbeat_default_sec = 180;
    int idle_timeout_sec = 600;
};

struct RedisConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    std::string prefix = "lp:";
};

struct AuthConfig {
    bool enable = true;
    int timestamp_window_sec = 300;
    std::string dev_default_secret = "DEV_ONLY_CHANGE_ME";
};

struct WakeConfig {
    int ack_timeout_ms = 5000;
    int max_retry = 3;
    int expire_ms = 300000;
};

struct AppConfig {
    ServerConfig server;
    RedisConfig redis;
    AuthConfig auth;
    WakeConfig wake;

    static AppConfig load(const std::string& path);
};