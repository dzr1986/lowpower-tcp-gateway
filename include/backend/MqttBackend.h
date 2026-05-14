#pragma once

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <mutex>
#include <string>

#include "AppConfig.h"
#include "GatewayContext.h"

struct mosquitto;

class MqttBackend {
public:
    MqttBackend(
        asio::io_context& io,
        GatewayContext& ctx,
        const MqttConfig& cfg
    );

    ~MqttBackend();

    bool start();
    void stop();

    void publishDeviceState(const DeviceState& state);
    void publishDeviceOffline(const std::string& device_id);
    void publishCommandEvent(const std::string& device_id, const nlohmann::json& payload);

private:
    std::string makeTopic(const std::string& suffix) const;
    std::string makeDeviceTopic(const std::string& device_id, const std::string& suffix) const;
    std::string extractDeviceId(const std::string& topic) const;

    void publishJson(const std::string& topic, const nlohmann::json& payload, bool retain);
    void handleCommandRequest(const std::string& device_id, const nlohmann::json& payload);

    void onConnect(int rc);
    void onMessage(const struct mosquitto_message* message);

    static void handleConnectStatic(struct mosquitto* mosq, void* obj, int rc);
    static void handleMessageStatic(struct mosquitto* mosq, void* obj, const struct mosquitto_message* message);

private:
    asio::io_context& io_;
    GatewayContext& ctx_;
    MqttConfig cfg_;
    struct mosquitto* client_ = nullptr;
    std::mutex client_mutex_;
    bool started_ = false;
};