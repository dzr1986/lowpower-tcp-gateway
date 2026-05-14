#pragma once

#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

class DeviceSession;
class RedisStore;
class WakeManager;

struct DeviceState {
    std::string device_id;
    std::string imei;
    std::string fw;
    std::string model;
    std::string remote_addr;
    std::string protocol;
    std::string dev_state = "unknown";

    int battery = -1;
    int rssi = 0;
    int rsrp = 0;
    bool charging = false;
    bool online = false;

    std::chrono::steady_clock::time_point login_time;
    std::chrono::steady_clock::time_point last_heartbeat;
};

struct GatewayContext {
    std::mutex mutex;

    std::unordered_map<std::string, std::weak_ptr<DeviceSession>> sessions;
    std::unordered_map<std::string, DeviceState> states;

    RedisStore* redis = nullptr;
    WakeManager* wake_manager = nullptr;
};