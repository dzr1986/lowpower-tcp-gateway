#include "DeviceSession.h"
#include "RedisStore.h"
#include "WakeManager.h"
#include "LowPowerProtocol.h"

#include <iostream>

void DeviceSession::readAutoDetect() {
    auto self = shared_from_this();

    socket_.async_receive(
        asio::buffer(lowpower_header_),
        asio::socket_base::message_peek,
        [this, self](std::error_code ec, std::size_t n) {
            if (ec) {
                handleDisconnect();
                return;
            }

            if (n == 0) {
                handleDisconnect();
                return;
            }

            // JSON Line 一般以 '{' 开头
            if (lowpower_header_[0] == '{') {
                protocol_ = SessionProtocol::JSON_LINE;
                readJsonLine();
                return;
            }

            // LowPower frame: version 1, type 0/2 常见起始
            if (lowpower_header_[0] == 0x01) {
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
            uint16_t size =
                static_cast<uint16_t>((lowpower_header_[3] << 8) | lowpower_header_[4]);

            if (version != 0x01) {
                std::cout << "[LOWPOWER] unsupported version="
                          << static_cast<int>(version)
                          << std::endl;
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
        lowpower_lp::Frame f;
        f.version = version;
        f.type = static_cast<lowpower_lp::MsgType>(type);
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

            lowpower_lp::Frame f;
            f.version = version;
            f.type = static_cast<lowpower_lp::MsgType>(type);
            f.flag = flag;
            f.size = size;
            f.payload = lowpower_payload_;

            handleLowPowerFrame(f);
            readLowPowerHeader();
        }
    );
}

void DeviceSession::handleLowPowerFrame(const lowpower_lp::Frame& frame) {
    using lowpower_lp::MsgType;

    switch (frame.type) {
        case MsgType::AUTH_REQ:
            handleLowPowerAuthReq(frame);
            break;

        case MsgType::HEARTBEAT:
            handleLowPowerHeartbeat(frame);
            break;

        default:
            std::cout << "[LOWPOWER] unsupported frame type="
                      << static_cast<int>(frame.type)
                      << std::endl;
            break;
    }
}

void DeviceSession::handleLowPowerAuthReq(const lowpower_lp::Frame& frame) {
    auto auth_payload = lowpower_lp::parseAuthPayloadLV(frame.payload);

    if (!auth_payload.has_value()) {
        std::cout << "[LOWPOWER_AUTH] bad LV payload" << std::endl;
        close();
        return;
    }

    // 注意：这里的 encrypted_devid_base64 需要用 local_key 解密。
    // 但服务器要先知道 device_id 才能查 local_key。
    // 生产设计建议：
    // 1. Cat.1 自研固件可在外层加 imei/device_id 明文字段；
    // 2. 或者通过 Redis 建立 encrypted_devid -> device_id 索引；
    // 3. 或者按 IMEI/TLS/client-cert 预定位 local_key。
    //
    // 这里给一个工程化落地方式：Redis 中提前保存 lp:lowpower:device_id_index:{encrypted_devid_b64} = D001

    std::string encrypted_devid_key =
        "lowpower:device_id_index:" + auth_payload->encrypted_devid_base64;

    std::string device_id;

    if (ctx_.redis) {
        auto maybe_device = ctx_.redis->getRaw(encrypted_devid_key);
        if (maybe_device.has_value()) {
            device_id = maybe_device.value();
        }
    }

    if (device_id.empty()) {
        std::cout << "[LOWPOWER_AUTH] cannot map encrypted devid to device_id" << std::endl;
        close();
        return;
    }

    std::string local_key = resolveLocalKeyOrDefault(device_id);

    std::vector<uint8_t> plain_data;

    try {
        plain_data = lowpower_lp::aes128CbcPkcs7Decrypt(
            local_key,
            auth_payload->iv,
            auth_payload->encrypted_data
        );
    } catch (const std::exception& e) {
        std::cout << "[LOWPOWER_AUTH] decrypt failed: " << e.what() << std::endl;
        close();
        return;
    }

    auto req = lowpower_lp::parseAuthRequestJson(plain_data);
    if (!req.has_value()) {
        std::cout << "[LOWPOWER_AUTH] bad auth json" << std::endl;
        close();
        return;
    }

    if (!lowpower_lp::verifyAuthRequest(req.value(), device_id, local_key)) {
        std::cout << "[LOWPOWER_AUTH] signature verify failed device="
                  << device_id
                  << std::endl;
        close();
        return;
    }

    device_id_ = device_id;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        DeviceState state;
        state.device_id = device_id_;
        state.remote_addr = remote_addr_;
        state.online = true;
        state.dev_state = "sleep";
        state.login_time = std::chrono::steady_clock::now();
        state.last_heartbeat = std::chrono::steady_clock::now();

        ctx_.sessions[device_id_] = shared_from_this();
        ctx_.states[device_id_] = state;
    }

    auto resp = lowpower_lp::buildAuthResponse(
        device_id_,
        local_key,
        req->random,
        cfg_.server.heartbeat_default_sec
    );

    sendRaw(lowpower_lp::encodeFrame(resp.frame));

    std::cout << "[LOWPOWER_AUTH_OK] device=" << device_id_
              << " heartbeat=" << cfg_.server.heartbeat_default_sec
              << std::endl;
}

void DeviceSession::handleLowPowerHeartbeat(const lowpower_lp::Frame& frame) {
    if (!loggedIn()) {
        std::cout << "[LOWPOWER_HEARTBEAT] not logged in" << std::endl;
        close();
        return;
    }

    if (frame.flag != 0 || frame.size != 0) {
        std::cout << "[LOWPOWER_HEARTBEAT] invalid heartbeat frame" << std::endl;
        close();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        auto& state = ctx_.states[device_id_];
        state.online = true;
        state.last_heartbeat = std::chrono::steady_clock::now();
        state.dev_state = "sleep";
    }

    if (ctx_.redis) {
        ctx_.redis->setOnline(device_id_, {
            {"device_id", device_id_},
            {"online", true},
            {"protocol", "lowpower"},
            {"dev", "sleep"}
        }, cfg_.server.idle_timeout_sec);
    }

    // 协议 FAQ：心跳回复内容与设备发送一致。
    sendRaw(lowpower_lp::makeHeartbeatFrame());

    std::cout << "[LOWPOWER_HEARTBEAT] device=" << device_id_ << std::endl;
}

void DeviceSession::sendRaw(const std::vector<uint8_t>& data) {
    auto self = shared_from_this();

    asio::post(socket_.get_executor(), [this, self, data]() {
        auto buf = std::make_shared<std::vector<uint8_t>>(data);

        asio::async_write(
            socket_,
            asio::buffer(*buf),
            [this, self, buf](std::error_code ec, std::size_t) {
                if (ec) {
                    handleDisconnect();
                }
            }
        );
    });
}

std::string DeviceSession::resolveLocalKeyOrDefault(const std::string& device_id) {
    if (ctx_.redis) {
        auto k = ctx_.redis->getLocalKey(device_id);
        if (k.has_value()) {
            return k.value();
        }
    }

    return cfg_.auth.dev_default_secret;
}