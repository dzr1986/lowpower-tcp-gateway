#include "backend/MqttBackend.h"

#include "service/WakeManager.h"

#include <iostream>

#if defined(LP_HAS_MQTT)
#include <mosquitto.h>
#endif

MqttBackend::MqttBackend(
    asio::io_context& io,
    GatewayContext& ctx,
    const MqttConfig& cfg
)
    : io_(io),
      ctx_(ctx),
      cfg_(cfg) {}

MqttBackend::~MqttBackend() {
    stop();
}

bool MqttBackend::start() {
    if (!cfg_.enable) {
        return true;
    }

#if !defined(LP_HAS_MQTT)
    std::cerr << "[MQTT] support not compiled, install libmosquitto-dev to enable it" << std::endl;
    return false;
#else
    if (started_) {
        return true;
    }

    mosquitto_lib_init();

    client_ = mosquitto_new(cfg_.client_id.c_str(), true, this);
    if (!client_) {
        std::cerr << "[MQTT] failed to create client" << std::endl;
        mosquitto_lib_cleanup();
        return false;
    }

    if (!cfg_.username.empty()) {
        int rc = mosquitto_username_pw_set(
            client_,
            cfg_.username.c_str(),
            cfg_.password.empty() ? nullptr : cfg_.password.c_str()
        );

        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "[MQTT] set auth failed rc=" << rc << std::endl;
            mosquitto_destroy(client_);
            client_ = nullptr;
            mosquitto_lib_cleanup();
            return false;
        }
    }

    mosquitto_connect_callback_set(client_, &MqttBackend::handleConnectStatic);
    mosquitto_message_callback_set(client_, &MqttBackend::handleMessageStatic);

    int rc = mosquitto_connect_async(
        client_,
        cfg_.host.c_str(),
        cfg_.port,
        cfg_.keepalive_sec
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT] connect failed rc=" << rc << std::endl;
        mosquitto_destroy(client_);
        client_ = nullptr;
        mosquitto_lib_cleanup();
        return false;
    }

    rc = mosquitto_loop_start(client_);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT] loop start failed rc=" << rc << std::endl;
        mosquitto_disconnect(client_);
        mosquitto_destroy(client_);
        client_ = nullptr;
        mosquitto_lib_cleanup();
        return false;
    }

    started_ = true;

    std::cout << "[MQTT] backend bridge enabled host="
              << cfg_.host << ":" << cfg_.port
              << " topic_prefix=" << cfg_.topic_prefix
              << std::endl;

    return true;
#endif
}

void MqttBackend::stop() {
#if defined(LP_HAS_MQTT)
    std::lock_guard<std::mutex> lock(client_mutex_);

    if (!client_) {
        return;
    }

    mosquitto_disconnect(client_);
    mosquitto_loop_stop(client_, true);
    mosquitto_destroy(client_);
    client_ = nullptr;
    started_ = false;
    mosquitto_lib_cleanup();
#endif
}

std::string MqttBackend::makeTopic(const std::string& suffix) const {
    return cfg_.topic_prefix + "/" + suffix;
}

std::string MqttBackend::makeDeviceTopic(const std::string& device_id, const std::string& suffix) const {
    return makeTopic("devices/" + device_id + "/" + suffix);
}

std::string MqttBackend::extractDeviceId(const std::string& topic) const {
    std::string marker = "/devices/";
    auto start = topic.find(marker);
    if (start == std::string::npos) {
        return "";
    }

    start += marker.size();

    auto end = topic.find('/', start);
    if (end == std::string::npos || end <= start) {
        return "";
    }

    return topic.substr(start, end - start);
}

void MqttBackend::publishJson(const std::string& topic, const nlohmann::json& payload, bool retain) {
#if defined(LP_HAS_MQTT)
    std::lock_guard<std::mutex> lock(client_mutex_);

    if (!client_) {
        return;
    }

    auto content = payload.dump();
    int rc = mosquitto_publish(
        client_,
        nullptr,
        topic.c_str(),
        static_cast<int>(content.size()),
        content.data(),
        cfg_.qos,
        retain
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT] publish failed topic=" << topic
                  << " rc=" << rc
                  << std::endl;
    }
#else
    (void) topic;
    (void) payload;
    (void) retain;
#endif
}

void MqttBackend::publishDeviceState(const DeviceState& state) {
    publishJson(
        makeDeviceTopic(state.device_id, "state"),
        {
            {"device_id", state.device_id},
            {"online", state.online},
            {"protocol", state.protocol},
            {"dev_state", state.dev_state},
            {"remote_addr", state.remote_addr},
            {"battery", state.battery},
            {"rssi", state.rssi},
            {"rsrp", state.rsrp},
            {"charging", state.charging},
            {"imei", state.imei},
            {"fw", state.fw},
            {"model", state.model}
        },
        true
    );
}

void MqttBackend::publishDeviceOffline(const std::string& device_id) {
    publishJson(
        makeDeviceTopic(device_id, "state"),
        {
            {"device_id", device_id},
            {"online", false}
        },
        true
    );
}

void MqttBackend::publishCommandEvent(const std::string& device_id, const nlohmann::json& payload) {
    publishJson(makeDeviceTopic(device_id, "commands/events"), payload, false);
}

void MqttBackend::handleCommandRequest(const std::string& device_id, const nlohmann::json& payload) {
    auto cmd = payload.value("cmd", "");
    auto params = payload.value("params", nlohmann::json::object());

    if (cmd.empty()) {
        publishCommandEvent(
            device_id,
            {
                {"device_id", device_id},
                {"status", "rejected"},
                {"error", "cmd_required"}
            }
        );
        return;
    }

    asio::post(io_, [this, device_id, cmd, params]() {
        if (!ctx_.wake_manager) {
            publishCommandEvent(
                device_id,
                {
                    {"device_id", device_id},
                    {"cmd", cmd},
                    {"status", "rejected"},
                    {"error", "command_dispatcher_not_ready"}
                }
            );
            return;
        }

        try {
            ctx_.wake_manager->sendWake(device_id, cmd, params);
        } catch (const std::exception& ex) {
            publishCommandEvent(
                device_id,
                {
                    {"device_id", device_id},
                    {"cmd", cmd},
                    {"status", "rejected"},
                    {"error", ex.what()}
                }
            );
        }
    });
}

void MqttBackend::onConnect(int rc) {
#if defined(LP_HAS_MQTT)
    if (rc != 0) {
        std::cerr << "[MQTT] connected with rc=" << rc << std::endl;
        return;
    }

    auto topic = makeTopic(cfg_.command_request_topic);
    int sub_rc = mosquitto_subscribe(client_, nullptr, topic.c_str(), cfg_.qos);

    if (sub_rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT] subscribe failed topic=" << topic
                  << " rc=" << sub_rc
                  << std::endl;
        return;
    }

    std::cout << "[MQTT] subscribed topic=" << topic << std::endl;
#else
    (void) rc;
#endif
}

void MqttBackend::onMessage(const struct mosquitto_message* message) {
#if defined(LP_HAS_MQTT)
    if (!message || !message->topic || !message->payload) {
        return;
    }

    std::string topic = message->topic;
    std::string device_id = extractDeviceId(topic);
    if (device_id.empty()) {
        return;
    }

    try {
        auto payload = nlohmann::json::parse(
            std::string(
                static_cast<const char*>(message->payload),
                static_cast<std::size_t>(message->payloadlen)
            )
        );

        handleCommandRequest(device_id, payload);
    } catch (const std::exception& ex) {
        publishCommandEvent(
            device_id,
            {
                {"device_id", device_id},
                {"status", "rejected"},
                {"error", std::string("bad_request_json: ") + ex.what()}
            }
        );
    }
#else
    (void) message;
#endif
}

void MqttBackend::handleConnectStatic(struct mosquitto* mosq, void* obj, int rc) {
    (void) mosq;

    auto* self = static_cast<MqttBackend*>(obj);
    if (!self) {
        return;
    }

    self->onConnect(rc);
}

void MqttBackend::handleMessageStatic(
    struct mosquitto* mosq,
    void* obj,
    const struct mosquitto_message* message
) {
    (void) mosq;

    auto* self = static_cast<MqttBackend*>(obj);
    if (!self) {
        return;
    }

    self->onMessage(message);
}