#pragma once

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "GatewayContext.h"
#include "protocol/LowPowerProtocol.h"

using asio::ip::tcp;

class RedisStore;
class WakeManager;

struct AppRuntimeConfig {
    int heartbeat_default_sec = 180;
    int idle_timeout_sec = 600;
    int auth_wait_timeout_sec = 10;
    std::string dev_default_secret = "DEV_ONLY_CHANGE_ME";
    bool json_auth_enable = true;
};

enum class SessionProtocol {
    UNKNOWN,
    JSON_LINE,
    LOWPOWER
};

class DeviceSession : public std::enable_shared_from_this<DeviceSession> {
public:
    DeviceSession(
        tcp::socket socket,
        GatewayContext& ctx,
        const AppRuntimeConfig& cfg
    );

    void start();

    void close();

    void sendJson(const nlohmann::json& j);
    void sendRaw(const std::vector<uint8_t>& data);

    std::string deviceId() const;
    SessionProtocol protocol() const;

private:
    void readAutoDetect();
    void readJsonLine();

    void readLowPowerHeader();
    void readLowPowerPayload(uint8_t version, uint8_t type, uint8_t flag, uint16_t size);

    void handleJsonMessage(const std::string& line);
    void handleJsonLogin(const nlohmann::json& msg);
    void handleJsonHeartbeat(const nlohmann::json& msg);
    void handleJsonWakeAck(const nlohmann::json& msg);
    void handleJsonTaskDone(const nlohmann::json& msg);
    void handleJsonHostSleep(const nlohmann::json& msg);
    void handleJsonHostState(const nlohmann::json& msg);

    void handleLowPowerFrame(const protocol::lowpower::Frame& frame);
    void handleLowPowerAuthReq(const protocol::lowpower::Frame& frame);
    void handleLowPowerHeartbeat(const protocol::lowpower::Frame& frame);

    std::string resolveLocalKey(const std::string& device_id);
    void registerOnlineState(const DeviceState& state);
    void updateRedisOnline(const DeviceState& state);
    void handleDisconnect();

private:
    tcp::socket socket_;
    asio::streambuf read_buffer_;
    std::deque<std::string> write_queue_;

    GatewayContext& ctx_;
    AppRuntimeConfig cfg_;

    SessionProtocol protocol_ = SessionProtocol::UNKNOWN;

    std::string device_id_;
    std::string remote_addr_;

    std::array<uint8_t, protocol::lowpower::HEADER_SIZE> lowpower_header_{};
    std::vector<uint8_t> lowpower_payload_;
};