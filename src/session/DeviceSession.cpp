#include "session/DeviceSession.h"

#include "backend/MqttBackend.h"
#include "protocol/JsonLineProtocol.h"
#include "protocol/LowPowerProtocol.h"
#include "storage/RedisStore.h"
#include "service/WakeManager.h"

#include <iostream>

DeviceSession::DeviceSession(
    tcp::socket socket,
    GatewayContext& ctx,
    const AppRuntimeConfig& cfg
)
    : socket_(std::move(socket)),
    write_strand_(asio::make_strand(socket_.get_executor())),
      ctx_(ctx),
      cfg_(cfg) {}

void DeviceSession::start() {
    remote_addr_ = socket_.remote_endpoint().address().to_string();

    std::cout << "[SESSION] new conn remote=" << remote_addr_ << std::endl;

    readAutoDetect();
}

std::string DeviceSession::deviceId() const {
    return device_id_;
}

SessionProtocol DeviceSession::protocol() const {
    return protocol_;
}

void DeviceSession::close() {
    asio::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

void DeviceSession::sendJson(const nlohmann::json& j) {
    auto line = protocol::jsonline::encodeLine(j);
    enqueueWrite(std::vector<uint8_t>(line.begin(), line.end()));
}

void DeviceSession::sendRaw(const std::vector<uint8_t>& data) {
    enqueueWrite(data);
}

void DeviceSession::enqueueWrite(const std::vector<uint8_t>& data) {
    auto self = shared_from_this();
    auto buffer = std::make_shared<std::vector<uint8_t>>(data);

    asio::post(write_strand_, [this, self, buffer]() {
        write_queue_.push_back(buffer);

        if (!write_in_progress_) {
            flushWrites();
        }
    });
}

void DeviceSession::flushWrites() {
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }

    write_in_progress_ = true;

    auto self = shared_from_this();
    auto buffer = write_queue_.front();

    asio::async_write(
        socket_,
        asio::buffer(*buffer),
        asio::bind_executor(
            write_strand_,
            [this, self, buffer](std::error_code ec, std::size_t) {
                if (ec) {
                    write_in_progress_ = false;
                    handleDisconnect();
                    return;
                }

                write_queue_.pop_front();
                flushWrites();
            }
        )
    );
}

void DeviceSession::readAutoDetect() {
    auto self = shared_from_this();

    socket_.async_receive(
        asio::buffer(lowpower_header_),
        asio::socket_base::message_peek,
        [this, self](std::error_code ec, std::size_t n) {
            if (ec || n == 0) {
                handleDisconnect();
                return;
            }

            if (lowpower_header_[0] == '{') {
                protocol_ = SessionProtocol::JSON_LINE;
                readJsonLine();
                return;
            }

            if (lowpower_header_[0] == protocol::lowpower::VERSION_1) {
                protocol_ = SessionProtocol::LOWPOWER;
                readLowPowerHeader();
                return;
            }

            std::cout << "[PROTO] unknown first byte="
                      << static_cast<int>(lowpower_header_[0])
                      << std::endl;

            close();
        }
    );
}

void DeviceSession::readJsonLine() {
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
                handleJsonMessage(line);
            }

            readJsonLine();
        }
    );
}

void DeviceSession::readLowPowerHeader() {
    auto self = shared_from_this();

    asio::async_read(
        socket_,
        asio::buffer(lowpower_header_),
        [this, self](std::error_code ec, std::size_t) {
            if (ec) {
                handleDisconnect();
                return;
            }

            uint8_t version = lowpower_header_[0];
            uint8_t type = lowpower_header_[1];
            uint8_t flag = lowpower_header_[2];
            uint16_t size = protocol::lowpower::readU16BE(&lowpower_header_[3]);

            if (version != protocol::lowpower::VERSION_1) {
                std::cout << "[LOWPOWER] bad version" << std::endl;
                close();
                return;
            }

            readLowPowerPayload(version, type, flag, size);
        }
    );
}

void DeviceSession::readLowPowerPayload(
    uint8_t version,
    uint8_t type,
    uint8_t flag,
    uint16_t size
) {
    auto self = shared_from_this();

    lowpower_payload_.assign(size, 0);

    if (size == 0) {
        protocol::lowpower::Frame f;
        f.version = version;
        f.type = static_cast<protocol::lowpower::MsgType>(type);
        f.flag = flag;
        f.size = 0;

        handleLowPowerFrame(f);
        readLowPowerHeader();
        return;
    }

    asio::async_read(
        socket_,
        asio::buffer(lowpower_payload_),
        [this, self, version, type, flag, size](std::error_code ec, std::size_t) {
            if (ec) {
                handleDisconnect();
                return;
            }

            protocol::lowpower::Frame f;
            f.version = version;
            f.type = static_cast<protocol::lowpower::MsgType>(type);
            f.flag = flag;
            f.size = size;
            f.payload = lowpower_payload_;

            handleLowPowerFrame(f);
            readLowPowerHeader();
        }
    );
}

void DeviceSession::handleJsonMessage(const std::string& line) {
    std::string error;
    auto msg = protocol::jsonline::parseLine(line, error);

    if (!msg.has_value()) {
        sendJson(protocol::jsonline::makeError("bad_json", error));
        return;
    }

    std::string type = msg->value("type", "");

    if (type == "login") {
        handleJsonLogin(*msg);
    } else if (type == "heartbeat") {
        handleJsonHeartbeat(*msg);
    } else if (type == "wake_ack") {
        handleJsonWakeAck(*msg);
    } else if (type == "task_done") {
        handleJsonTaskDone(*msg);
    } else if (type == "host_sleep") {
        handleJsonHostSleep(*msg);
    } else if (type == "host_state") {
        handleJsonHostState(*msg);
    } else {
        sendJson(protocol::jsonline::makeError("unknown_type", type));
    }
}

void DeviceSession::handleJsonLogin(const nlohmann::json& msg) {
    std::string device_id = msg.value("device_id", "");

    if (device_id.empty()) {
        sendJson(protocol::jsonline::makeLoginAck(
            msg.value("seq", 0),
            1,
            cfg_.heartbeat_default_sec,
            cfg_.idle_timeout_sec,
            "device_id_required"
        ));
        return;
    }

    device_id_ = device_id;

    DeviceState state;
    state.device_id = device_id_;
    state.imei = msg.value("imei", "");
    state.fw = msg.value("fw", "");
    state.model = msg.value("model", "");
    state.remote_addr = remote_addr_;
    state.protocol = "json_line";
    state.online = true;
    state.dev_state = msg.value("dev", "unknown");
    state.login_time = std::chrono::steady_clock::now();
    state.last_heartbeat = std::chrono::steady_clock::now();

    registerOnlineState(state);

    sendJson(protocol::jsonline::makeLoginAck(
        msg.value("seq", 0),
        0,
        cfg_.heartbeat_default_sec,
        cfg_.idle_timeout_sec
    ));

    std::cout << "[JSON_LOGIN] device=" << device_id_ << std::endl;
}

void DeviceSession::handleJsonHeartbeat(const nlohmann::json& msg) {
    if (device_id_.empty()) {
        sendJson(protocol::jsonline::makeError("not_logged_in", ""));
        return;
    }

    DeviceState state_copy;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        auto& s = ctx_.states[device_id_];
        s.online = true;
        s.last_heartbeat = std::chrono::steady_clock::now();
        s.battery = msg.value("battery", s.battery);
        s.rssi = msg.value("rssi", s.rssi);
        s.rsrp = msg.value("rsrp", s.rsrp);
        s.charging = msg.value("charging", s.charging);
        s.dev_state = msg.value("dev", s.dev_state);

        state_copy = s;
    }

    updateRedisOnline(state_copy);

    int next = cfg_.heartbeat_default_sec;

    if (state_copy.battery >= 0 && state_copy.battery < 20) {
        next = 300;
    }

    if (state_copy.battery >= 0 && state_copy.battery < 10) {
        next = 600;
    }

    sendJson(protocol::jsonline::makeHeartbeatAck(
        msg.value("seq", 0),
        0,
        next
    ));
}

void DeviceSession::handleJsonWakeAck(const nlohmann::json& msg) {
    std::string msg_id = msg.value("msg_id", "");

    if (ctx_.wake_manager && !msg_id.empty()) {
        ctx_.wake_manager->markAck(msg_id);
    }

    std::cout << "[JSON_WAKE_ACK] device=" << device_id_
              << " msg_id=" << msg_id
              << std::endl;
}

void DeviceSession::handleJsonTaskDone(const nlohmann::json& msg) {
    std::string msg_id = msg.value("msg_id", "");

    if (ctx_.wake_manager && !msg_id.empty()) {
        ctx_.wake_manager->markDone(msg_id);
    }

    sendJson(protocol::jsonline::makeTaskDoneAck(
        msg.value("seq", 0),
        msg_id,
        0
    ));

    std::cout << "[JSON_TASK_DONE] device=" << device_id_
              << " msg_id=" << msg_id
              << " url=" << msg.value("url", "")
              << std::endl;
}

void DeviceSession::handleJsonHostSleep(const nlohmann::json& msg) {
    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        ctx_.states[device_id_].dev_state = "sleep";
    }

    sendJson({
        {"type", "host_sleep_ack"},
        {"seq", msg.value("seq", 0)},
        {"result", 0}
    });
}

void DeviceSession::handleJsonHostState(const nlohmann::json& msg) {
    std::string dev = msg.value("dev", "unknown");

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        ctx_.states[device_id_].dev_state = dev;
    }
}

void DeviceSession::handleLowPowerFrame(const protocol::lowpower::Frame& frame) {
    using protocol::lowpower::MsgType;

    switch (frame.type) {
        case MsgType::AUTH_REQ:
            handleLowPowerAuthReq(frame);
            break;

        case MsgType::HEARTBEAT:
            handleLowPowerHeartbeat(frame);
            break;

        default:
            std::cout << "[LOWPOWER] unsupported type="
                      << static_cast<int>(frame.type)
                      << std::endl;
            break;
    }
}

void DeviceSession::handleLowPowerAuthReq(const protocol::lowpower::Frame& frame) {
    auto payload = protocol::lowpower::parseAuthPayloadLV(frame.payload);

    if (!payload.has_value()) {
        std::cout << "[LOWPOWER_AUTH] bad payload" << std::endl;
        close();
        return;
    }

    std::string device_id;

    if (ctx_.redis) {
        auto v = ctx_.redis->getDeviceIdByEncryptedId(payload->encrypted_devid_base64);
        if (v.has_value()) {
            device_id = v.value();
        }
    }

    if (device_id.empty()) {
        std::cout << "[LOWPOWER_AUTH] cannot map encrypted devid" << std::endl;
        close();
        return;
    }

    std::string local_key = resolveLocalKey(device_id);

    std::vector<uint8_t> plain;

    try {
        plain = protocol::lowpower::aes128CbcPkcs7Decrypt(
            local_key,
            payload->iv,
            payload->encrypted_data
        );
    } catch (const std::exception& e) {
        std::cout << "[LOWPOWER_AUTH] decrypt failed " << e.what() << std::endl;
        close();
        return;
    }

    auto req = protocol::lowpower::parseAuthRequestJson(plain);

    if (!req.has_value()) {
        std::cout << "[LOWPOWER_AUTH] bad auth json" << std::endl;
        close();
        return;
    }

    if (!protocol::lowpower::verifyAuthRequest(*req, device_id, local_key)) {
        std::cout << "[LOWPOWER_AUTH] signature failed device="
                  << device_id
                  << std::endl;
        close();
        return;
    }

    device_id_ = device_id;

    DeviceState state;
    state.device_id = device_id_;
    state.remote_addr = remote_addr_;
    state.protocol = "lowpower";
    state.online = true;
    state.dev_state = "sleep";
    state.login_time = std::chrono::steady_clock::now();
    state.last_heartbeat = std::chrono::steady_clock::now();

    registerOnlineState(state);

    auto resp = protocol::lowpower::buildAuthResponse(
        device_id_,
        local_key,
        req->random,
        cfg_.heartbeat_default_sec
    );

    sendRaw(protocol::lowpower::encodeFrame(resp.frame));

    std::cout << "[LOWPOWER_AUTH_OK] device=" << device_id_ << std::endl;
}

void DeviceSession::handleLowPowerHeartbeat(const protocol::lowpower::Frame& frame) {
    if (device_id_.empty()) {
        close();
        return;
    }

    if (frame.flag != 0 || frame.size != 0) {
        close();
        return;
    }

    DeviceState state_copy;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        auto& s = ctx_.states[device_id_];
        s.online = true;
        s.last_heartbeat = std::chrono::steady_clock::now();
        s.dev_state = "sleep";

        state_copy = s;
    }

    updateRedisOnline(state_copy);

    sendRaw(protocol::lowpower::makeHeartbeatFrame());

    std::cout << "[LOWPOWER_HEARTBEAT] device=" << device_id_ << std::endl;
}

std::string DeviceSession::resolveLocalKey(const std::string& device_id) {
    if (ctx_.redis) {
        auto key = ctx_.redis->getLocalKey(device_id);
        if (key.has_value()) {
            return key.value();
        }
    }

    return cfg_.dev_default_secret;
}

void DeviceSession::registerOnlineState(const DeviceState& state) {
    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);
        ctx_.sessions[state.device_id] = shared_from_this();
        ctx_.states[state.device_id] = state;
    }

    updateRedisOnline(state);
}

void DeviceSession::updateRedisOnline(const DeviceState& state) {
    if (!ctx_.redis) return;

    ctx_.redis->setOnline(
        state.device_id,
        {
            {"device_id", state.device_id},
            {"online", state.online},
            {"protocol", state.protocol},
            {"remote_addr", state.remote_addr},
            {"battery", state.battery},
            {"rssi", state.rssi},
            {"rsrp", state.rsrp},
            {"dev", state.dev_state}
        },
        cfg_.idle_timeout_sec
    );

    if (ctx_.mqtt_backend) {
        ctx_.mqtt_backend->publishDeviceState(state);
    }
}

void DeviceSession::handleDisconnect() {
    if (closing_.exchange(true)) {
        return;
    }

    if (!device_id_.empty()) {
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

        if (ctx_.mqtt_backend) {
            ctx_.mqtt_backend->publishDeviceOffline(device_id_);
        }

        std::cout << "[DISCONNECT] device=" << device_id_ << std::endl;
    }

    close();
}