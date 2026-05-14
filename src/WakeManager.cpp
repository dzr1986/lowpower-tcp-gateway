#include "WakeManager.h"
#include "GatewayContext.h"
#include "DeviceSession.h"
#include "RedisStore.h"

#include <iostream>
#include <chrono>

WakeManager::WakeManager(asio::io_context& io, GatewayContext& ctx, const WakeConfig& cfg)
    : io_(io), ctx_(ctx), cfg_(cfg), timer_(io) {}

void WakeManager::start() {
    scheduleTick();
}

std::string WakeManager::makeMsgId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    return "W" + std::to_string(ms);
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

    std::string msg_id = makeMsgId();

    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    nlohmann::json payload = {
        {"type", "wake"},
        {"seq", now_ms},
        {"msg_id", msg_id},
        {"cmd", cmd},
        {"reason", "admin_api"},
        {"expire_ms", now_ms + cfg_.expire_ms},
        {"params", params}
    };

    PendingWake pending;
    pending.device_id = device_id;
    pending.msg_id = msg_id;
    pending.payload = payload;
    pending.session = session;
    pending.retry_count = 0;
    pending.next_retry = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(cfg_.ack_timeout_ms);

    pending_[msg_id] = pending;

    if (ctx_.redis) {
        ctx_.redis->savePendingWake(device_id, msg_id, payload, cfg_.expire_ms / 1000);
    }

    session->sendJson(payload);

    std::cout << "[WAKE_SEND] device=" << device_id
              << " msg_id=" << msg_id
              << " cmd=" << cmd
              << std::endl;

    return msg_id;
}

void WakeManager::markAck(const std::string& msg_id) {
    auto it = pending_.find(msg_id);
    if (it == pending_.end()) return;

    std::cout << "[WAKE_ACK_OK] msg_id=" << msg_id << std::endl;

    pending_.erase(it);
}

void WakeManager::markDone(const std::string& msg_id) {
    auto it = pending_.find(msg_id);
    if (it != pending_.end()) {
        if (ctx_.redis) {
            ctx_.redis->removePendingWake(it->second.device_id, msg_id);
        }
        pending_.erase(it);
    }
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
        auto& pending = kv.second;

        if (now >= pending.next_retry) {
            if (pending.retry_count >= cfg_.max_retry) {
                std::cout << "[WAKE_FAILED] device=" << pending.device_id
                          << " msg_id=" << pending.msg_id
                          << std::endl;
                remove_list.push_back(pending.msg_id);
                continue;
            }

            retryWake(pending);
        }
    }

    for (auto& msg_id : remove_list) {
        pending_.erase(msg_id);
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

    session->sendJson(pending.payload);
}