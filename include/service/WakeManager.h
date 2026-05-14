#pragma once

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <memory>
#include <string>
#include <chrono>

#include "session/DeviceSession.h"

struct WakeRuntimeConfig {
    int ack_timeout_ms = 5000;
    int max_retry = 3;
    int expire_ms = 300000;
};

struct PendingWake {
    std::string device_id;
    std::string msg_id;
    std::string cmd;
    nlohmann::json payload;
    std::weak_ptr<DeviceSession> session;
    SessionProtocol protocol = SessionProtocol::UNKNOWN;
    int retry_count = 0;
    std::chrono::steady_clock::time_point next_retry;
};

class WakeManager {
public:
    WakeManager(
        asio::io_context& io,
        GatewayContext& ctx,
        const WakeRuntimeConfig& cfg
    );

    void start();

    std::string sendWake(
        const std::string& device_id,
        const std::string& cmd,
        const nlohmann::json& params
    );

    std::string sendLowPowerWake(
        const std::string& device_id
    );

    void markAck(const std::string& msg_id);
    void markDone(const std::string& msg_id);

private:
    std::string makeMsgId();
    long long nowMs();

    void scheduleTick();
    void onTick();
    void retryWake(PendingWake& pending);

private:
    asio::io_context& io_;
    GatewayContext& ctx_;
    WakeRuntimeConfig cfg_;
    asio::steady_timer timer_;

    std::unordered_map<std::string, PendingWake> pending_;
};