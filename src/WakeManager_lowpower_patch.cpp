#include "LowPowerProtocol.h"
#include "RedisStore.h"
#include "DeviceSession.h"

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

    std::string local_key;

    if (ctx_.redis) {
        auto k = ctx_.redis->getLocalKey(device_id);
        if (!k.has_value()) {
            throw std::runtime_error("local_key not found");
        }
        local_key = k.value();
    } else {
        throw std::runtime_error("redis not configured");
    }

    std::string msg_id = makeMsgId();
    auto raw = lowpower_lp::makeWakeupFrame(local_key);

    // 这里需要 DeviceSession 暴露 sendRawPublic()
    session->sendRawPublic(raw);

    PendingWake pending;
    pending.device_id = device_id;
    pending.msg_id = msg_id;
    pending.payload = {
        {"protocol", "lowpower"},
        {"type", "wakeup"},
        {"msg_id", msg_id}
    };
    pending.session = session;
    pending.retry_count = 0;
    pending.next_retry = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(cfg_.ack_timeout_ms);

    pending_[msg_id] = pending;

    std::cout << "[WAKE_SEND_LOWPOWER] device=" << device_id
              << " msg_id=" << msg_id
              << std::endl;

    return msg_id;
}