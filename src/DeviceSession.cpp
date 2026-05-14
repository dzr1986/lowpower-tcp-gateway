#include "DeviceSession.h"
#include "Auth.h"
#include "RedisStore.h"
#include "WakeManager.h"

#include <iostream>

DeviceSession::DeviceSession(tcp::socket socket, GatewayContext& ctx, const AppConfig& cfg)
    : socket_(std::move(socket)), ctx_(ctx), cfg_(cfg) {}

void DeviceSession::start() {
    remote_addr_ = socket_.remote_endpoint().address().to_string();
    std::cout << "[TCP] new connection " << remote_addr_ << std::endl;
    readLine();
}

std::string DeviceSession::deviceId() const {
    return device_id_;
}

bool DeviceSession::loggedIn() const {
    return !device_id_.empty();
}

void DeviceSession::sendJson(const json& j) {
    auto self = shared_from_this();

    std::string data = j.dump();
    data.push_back('\n');

    asio::post(socket_.get_executor(), [this, self, data]() {
        bool writing = !write_queue_.empty();
        write_queue_.push_back(data);

        if (!writing) {
            writeNext();
        }
    });
}

void DeviceSession::close() {
    asio::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

void DeviceSession::readLine() {
    auto self = shared_from_this();

    asio::async_read_until(
        socket_,
        read_buffer_,
        '\n',
        [this, self](std::error_code ec, std::size_t) {
            if (ec) {
                handleDisconnect();
                return;
            }

            std::istream is(&read_buffer_);
            std::string line;
            std::getline(is, line);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                handleMessage(line);
            }

            readLine();
        }
    );
}

void DeviceSession::writeNext() {
    auto self = shared_from_this();

    asio::async_write(
        socket_,
        asio::buffer(write_queue_.front()),
        [this, self](std::error_code ec, std::size_t) {
            if (ec) {
                handleDisconnect();
                return;
            }

            write_queue_.pop_front();

            if (!write_queue_.empty()) {
                writeNext();
            }
        }
    );
}

void DeviceSession::handleMessage(const std::string& line) {
    json msg;

    try {
        msg = json::parse(line);
    } catch (...) {
        sendJson({{"type", "error"}, {"code", "bad_json"}});
        return;
    }

    std::string type = msg.value("type", "");

    if (type == "login") {
        handleLogin(msg);
    } else if (type == "heartbeat") {
        handleHeartbeat(msg);
    } else if (type == "wake_ack") {
        handleWakeAck(msg);
    } else if (type == "task_done") {
        handleTaskDone(msg);
    } else if (type == "host_sleep") {
        handleHostSleep(msg);
    } else if (type == "host_state") {
        handleHostState(msg);
    } else {
        sendJson({{"type", "error"}, {"code", "unknown_type"}, {"message", type}});
    }
}

void DeviceSession::handleLogin(const json& msg) {
    std::string device_id = msg.value("device_id", "");
    std::string imei = msg.value("imei", "");

    if (device_id.empty()) {
        sendJson({{"type", "login_ack"}, {"result", 1}, {"error", "device_id_required"}});
        return;
    }

    if (cfg_.auth.enable) {
        std::string secret = cfg_.auth.dev_default_secret;

        if (ctx_.redis) {
            auto redis_secret = ctx_.redis->getDeviceSecret(device_id);
            if (redis_secret.has_value()) {
                secret = redis_secret.value();
            } else {
                ctx_.redis->setDeviceSecretForDev(device_id, secret);
            }
        }

        std::string error;
        if (!verifyLoginSignature(msg, secret, cfg_.auth.timestamp_window_sec, error)) {
            std::cout << "[AUTH_FAIL] device=" << device_id
                      << " error=" << error
                      << std::endl;

            sendJson({
                {"type", "login_ack"},
                {"seq", msg.value("seq", 0)},
                {"result", 2},
                {"error", error}
            });

            close();
            return;
        }
    }

    device_id_ = device_id;

    DeviceState state;
    state.device_id = device_id;
    state.imei = imei;
    state.fw = msg.value("fw", "");
    state.model = msg.value("model", "");
    state.remote_addr = remote_addr_;
    state.online = true;
    state.dev_state = msg.value("dev", "unknown");
    state.login_time = std::chrono::steady_clock::now();
    state.last_heartbeat = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        ctx_.sessions[device_id] = shared_from_this();
        ctx_.states[device_id] = state;
    }

    if (ctx_.redis) {
        ctx_.redis->setOnline(device_id, {
            {"device_id", device_id},
            {"online", true},
            {"remote_addr", remote_addr_},
            {"dev", state.dev_state}
        }, cfg_.server.idle_timeout_sec);
    }

    std::cout << "[LOGIN] device=" << device_id
              << " imei=" << imei
              << " addr=" << remote_addr_
              << std::endl;

    sendJson({
        {"type", "login_ack"},
        {"seq", msg.value("seq", 0)},
        {"result", 0},
        {"heartbeat", cfg_.server.heartbeat_default_sec},
        {"idle_timeout", cfg_.server.idle_timeout_sec}
    });
}

void DeviceSession::handleHeartbeat(const json& msg) {
    if (!loggedIn()) {
        sendJson({{"type", "error"}, {"code", "not_logged_in"}});
        return;
    }

    DeviceState state_copy;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        auto& state = ctx_.states[device_id_];

        state.last_heartbeat = std::chrono::steady_clock::now();
        state.online = true;
        state.battery = msg.value("battery", state.battery);
        state.rssi = msg.value("rssi", state.rssi);
        state.rsrp = msg.value("rsrp", state.rsrp);
        state.dev_state = msg.value("dev", state.dev_state);
        state.charging = msg.value("charging", state.charging);

        state_copy = state;
    }

    int next_heartbeat = cfg_.server.heartbeat_default_sec;

    if (state_copy.battery >= 0 && state_copy.battery < 20) {
        next_heartbeat = 300;
    }

    if (state_copy.battery >= 0 && state_copy.battery < 10) {
        next_heartbeat = 600;
    }

    if (ctx_.redis) {
        ctx_.redis->setOnline(device_id_, {
            {"device_id", device_id_},
            {"online", true},
            {"battery", state_copy.battery},
            {"rssi", state_copy.rssi},
            {"rsrp", state_copy.rsrp},
            {"dev", state_copy.dev_state}
        }, cfg_.server.idle_timeout_sec);
    }

    sendJson({
        {"type", "heartbeat_ack"},
        {"seq", msg.value("seq", 0)},
        {"result", 0},
        {"next_heartbeat", next_heartbeat}
    });
}

void DeviceSession::handleWakeAck(const json& msg) {
    std::string msg_id = msg.value("msg_id", "");

    std::cout << "[WAKE_ACK] device=" << device_id_
              << " msg_id=" << msg_id
              << " result=" << msg.value("result", -1)
              << std::endl;

    if (ctx_.wake_manager && !msg_id.empty()) {
        ctx_.wake_manager->markAck(msg_id);
    }
}

void DeviceSession::handleTaskDone(const json& msg) {
    std::string msg_id = msg.value("msg_id", "");

    std::cout << "[TASK_DONE] device=" << device_id_
              << " msg_id=" << msg_id
              << " result=" << msg.value("result", -1)
              << " url=" << msg.value("url", "")
              << std::endl;

    if (ctx_.wake_manager && !msg_id.empty()) {
        ctx_.wake_manager->markDone(msg_id);
    }

    sendJson({
        {"type", "task_done_ack"},
        {"seq", msg.value("seq", 0)},
        {"msg_id", msg_id},
        {"result", 0}
    });
}

void DeviceSession::handleHostSleep(const json& msg) {
    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        auto& state = ctx_.states[device_id_];
        state.dev_state = "sleep";
    }

    sendJson({
        {"type", "host_sleep_ack"},
        {"seq", msg.value("seq", 0)},
        {"result", 0}
    });
}

void DeviceSession::handleHostState(const json& msg) {
    std::string dev = msg.value("dev", "unknown");

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        auto& state = ctx_.states[device_id_];
        state.dev_state = dev;
    }

    std::cout << "[HOST_STATE] device=" << device_id_
              << " dev=" << dev
              << std::endl;
}

void DeviceSession::handleDisconnect() {
    if (!device_id_.empty()) {
        std::cout << "[DISCONNECT] device=" << device_id_ << std::endl;

        {
            std::lock_guard<std::mutex> lock(ctx_.mutex);
            ctx_.sessions.erase(device_id_);
            if (ctx_.states.count(device_id_)) {
                ctx_.states[device_id_].online = false;
            }
        }

        if (ctx_.redis) {
            ctx_.redis->setOffline(device_id_);
        }
    }

    close();
}