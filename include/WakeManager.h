#pragma once

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <memory>
#include <string>
#include <chrono>

class DeviceSession;
struct GatewayContext;
struct WakeConfig;

struct PendingWake {
    std::string device_id;
    std::string msg_id;
    nlohmann::json payload;
    std::weak_ptr<DeviceSession> session;
    int retry_count = 0;
    std::chrono::steady_clock::time_point next_retry;
};

class WakeManager {
public:
    WakeManager(asio::io_context& io, GatewayContext& ctx, const WakeConfig& cfg);

    void start();

    std::string sendWake(
        const std::string& device_id,
        const std::string& cmd,
        const nlohmann::json& params
    );

    void markAck(const std::string& msg_id);
    void markDone(const std::string& msg_id);

private:
    void scheduleTick();
    void onTick();
    void retryWake(PendingWake& pending);
    std::string makeMsgId();

private:
    asio::io_context& io_;
    GatewayContext& ctx_;
    WakeConfig cfg_;
    asio::steady_timer timer_;

    std::unordered_map<std::string, PendingWake> pending_;
};