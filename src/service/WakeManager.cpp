#include "service/WakeManager.h"

#include "backend/MqttBackend.h"
#include "protocol/JsonLineProtocol.h"
#include "protocol/LowPowerProtocol.h"
#include "storage/RedisStore.h"

#include <iostream>

WakeManager::WakeManager(
    asio::io_context& io,
    GatewayContext& ctx,
    const WakeRuntimeConfig& cfg
)
    : io_(io),
      ctx_(ctx),
      cfg_(cfg),
      timer_(io) {}

void WakeManager::start() {
    scheduleTick();
}

long long WakeManager::nowMs() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
}

std::string WakeManager::makeMsgId() {
    return "W" + std::to_string(nowMs());
}

void WakeManager::publishCommandEvent(
    const std::string& device_id,
    const std::string& msg_id,
    const std::string& status,
    const std::string& cmd,
    const std::optional<nlohmann::json>& extra
) {
    if (!ctx_.mqtt_backend) {
        return;
    }

    nlohmann::json payload = {
        {"device_id", device_id},
        {"msg_id", msg_id},
        {"status", status},
        {"cmd", cmd}
    };

    if (extra.has_value()) {
        for (auto it = extra->begin(); it != extra->end(); ++it) {
            payload[it.key()] = it.value();
        }
    }

    ctx_.mqtt_backend->publishCommandEvent(device_id, payload);
}

std::string WakeManager::sendWake(
    const std::string& device_id,
    const std::string& cmd,
    const nlohmann::json& params
) {
    std::shared_ptr<DeviceSession> session;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        auto it = ctx_.sessions.find(device_id);
        if (it == ctx_.sessions.end()) {
            throw std::runtime_error("device offline");
        }

        session = it->second.lock();

        if (!session) {
            ctx_.sessions.erase(it);
            throw std::runtime_error("device session expired");
        }
    }

    if (session->protocol() == SessionProtocol::LOWPOWER || cmd == "lowpower_wakeup") {
        return sendLowPowerWake(device_id);
    }

    std::string msg_id = makeMsgId();

    auto seq = nowMs();
    auto expire_ms = seq + cfg_.expire_ms;

    auto payload = protocol::jsonline::makeWake(
        seq,
        msg_id,
        cmd,
        params,
        expire_ms
    );

    PendingWake pending;
    pending.device_id = device_id;
    pending.msg_id = msg_id;
    pending.cmd = cmd;
    pending.payload = payload;
    pending.session = session;
    pending.protocol = SessionProtocol::JSON_LINE;
    pending.retry_count = 0;
    pending.next_retry = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(cfg_.ack_timeout_ms);

    pending_[msg_id] = pending;

    if (ctx_.redis) {
        ctx_.redis->savePendingWake(
            device_id,
            msg_id,
            payload,
            cfg_.expire_ms / 1000
        );
    }

    session->sendJson(payload);

    publishCommandEvent(
        device_id,
        msg_id,
        "dispatched",
        cmd,
        nlohmann::json{{"protocol", "json_line"}}
    );

    std::cout << "[WAKE_SEND_JSON] device=" << device_id
              << " msg_id=" << msg_id
              << " cmd=" << cmd
              << std::endl;

    return msg_id;
}

std::string WakeManager::sendLowPowerWake(const std::string& device_id) {
    std::shared_ptr<DeviceSession> session;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        auto it = ctx_.sessions.find(device_id);
        if (it == ctx_.sessions.end()) {
            throw std::runtime_error("device offline");
        }

        session = it->second.lock();

        if (!session) {
            ctx_.sessions.erase(it);
            throw std::runtime_error("device session expired");
        }
    }

    if (!ctx_.redis) {
        throw std::runtime_error("redis required for local_key");
    }

    auto local_key = ctx_.redis->getLocalKey(device_id);
    if (!local_key.has_value()) {
        throw std::runtime_error("local_key not found");
    }

    std::string msg_id = makeMsgId();

    auto raw = protocol::lowpower::makeWakeupFrame(local_key.value());

    nlohmann::json pending_payload = {
        {"protocol", "lowpower"},
        {"type", "wakeup"},
        {"msg_id", msg_id}
    };

    PendingWake pending;
    pending.device_id = device_id;
    pending.msg_id = msg_id;
    pending.cmd = "lowpower_wakeup";
    pending.payload = pending_payload;
    pending.session = session;
    pending.protocol = SessionProtocol::LOWPOWER;
    pending.retry_count = 0;
    pending.next_retry = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(cfg_.ack_timeout_ms);

    pending_[msg_id] = pending;

    if (ctx_.redis) {
        ctx_.redis->savePendingWake(
            device_id,
            msg_id,
            pending_payload,
            cfg_.expire_ms / 1000
        );
    }

    session->sendRaw(raw);

    publishCommandEvent(
        device_id,
        msg_id,
        "dispatched",
        pending.cmd,
        nlohmann::json{{"protocol", "lowpower"}}
    );

    std::cout << "[WAKE_SEND_LOWPOWER] device=" << device_id
              << " msg_id=" << msg_id
              << std::endl;

    return msg_id;
}

void WakeManager::markAck(const std::string& msg_id) {
    auto it = pending_.find(msg_id);

    if (it == pending_.end()) {
        return;
    }

    std::cout << "[WAKE_ACK_OK] msg_id=" << msg_id << std::endl;

    publishCommandEvent(
        it->second.device_id,
        msg_id,
        "acked",
        it->second.cmd
    );

    pending_.erase(it);
}

void WakeManager::markDone(const std::string& msg_id) {
    auto it = pending_.find(msg_id);

    if (it == pending_.end()) {
        return;
    }

    if (ctx_.redis) {
        ctx_.redis->removePendingWake(it->second.device_id, msg_id);
    }

    publishCommandEvent(
        it->second.device_id,
        msg_id,
        "done",
        it->second.cmd
    );

    pending_.erase(it);
}

void WakeManager::scheduleTick() {
    timer_.expires_after(std::chrono::milliseconds(1000));

    timer_.async_wait([this](std::error_code ec) {
        if (ec) return;

        onTick();
        scheduleTick();
    });
}

void WakeManager::onTick() {
    auto now = std::chrono::steady_clock::now();

    std::vector<std::string> remove_list;

    for (auto& kv : pending_) {
        auto& p = kv.second;

        if (now < p.next_retry) {
            continue;
        }

        if (p.retry_count >= cfg_.max_retry) {
            std::cout << "[WAKE_FAILED] device=" << p.device_id
                      << " msg_id=" << p.msg_id
                      << std::endl;

            publishCommandEvent(
                p.device_id,
                p.msg_id,
                "failed",
                p.cmd,
                nlohmann::json{{"retry_count", p.retry_count}}
            );

            remove_list.push_back(p.msg_id);
            continue;
        }

        retryWake(p);
    }

    for (auto& id : remove_list) {
        pending_.erase(id);
    }
}

void WakeManager::retryWake(PendingWake& pending) {
    auto session = pending.session.lock();

    if (!session) {
        std::cout << "[WAKE_RETRY_SKIP] session expired msg_id="
                  << pending.msg_id
                  << std::endl;
        return;
    }

    pending.retry_count++;
    pending.next_retry = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(cfg_.ack_timeout_ms);

    std::cout << "[WAKE_RETRY] device=" << pending.device_id
              << " msg_id=" << pending.msg_id
              << " retry=" << pending.retry_count
              << std::endl;

    publishCommandEvent(
        pending.device_id,
        pending.msg_id,
        "retrying",
        pending.cmd,
        nlohmann::json{{"retry_count", pending.retry_count}}
    );

    if (pending.protocol == SessionProtocol::JSON_LINE) {
        session->sendJson(pending.payload);
        return;
    }

    if (pending.protocol == SessionProtocol::LOWPOWER) {
        if (!ctx_.redis) return;

        auto local_key = ctx_.redis->getLocalKey(pending.device_id);
        if (!local_key.has_value()) return;

        session->sendRaw(protocol::lowpower::makeWakeupFrame(local_key.value()));
        return;
    }
}