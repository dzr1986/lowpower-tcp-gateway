#include "storage/RedisStore.h"

#include <iostream>

RedisStore::RedisStore(const RedisConfig& cfg)
    : cfg_(cfg) {}

RedisStore::~RedisStore() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisStore::connect() {
    ctx_ = redisConnect(cfg_.host.c_str(), cfg_.port);

    if (!ctx_ || ctx_->err) {
        std::cerr << "[REDIS] connect failed: "
                  << (ctx_ ? ctx_->errstr : "null")
                  << std::endl;
        return false;
    }

    if (!cfg_.password.empty()) {
        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx_, "AUTH %s", cfg_.password.c_str())
        );

        bool ok = commandOK(reply);
        if (reply) freeReplyObject(reply);

        if (!ok) {
            std::cerr << "[REDIS] auth failed" << std::endl;
            return false;
        }
    }

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SELECT %d", cfg_.db)
    );

    bool ok = commandOK(reply);
    if (reply) freeReplyObject(reply);

    if (!ok) {
        std::cerr << "[REDIS] select db failed" << std::endl;
        return false;
    }

    std::cout << "[REDIS] connected "
              << cfg_.host << ":" << cfg_.port
              << " db=" << cfg_.db
              << std::endl;

    return true;
}

std::string RedisStore::key(const std::string& k) const {
    return cfg_.prefix + k;
}

bool RedisStore::commandOK(redisReply* reply) const {
    if (!reply) return false;
    return reply->type == REDIS_REPLY_STATUS &&
           std::string(reply->str) == "OK";
}

std::optional<std::string> RedisStore::getRaw(const std::string& raw_key_without_prefix) {
    auto k = key(raw_key_without_prefix);

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", k.c_str())
    );

    if (!reply) return std::nullopt;

    std::optional<std::string> value;

    if (reply->type == REDIS_REPLY_STRING) {
        value = std::string(reply->str, reply->len);
    }

    freeReplyObject(reply);
    return value;
}

bool RedisStore::setRaw(const std::string& raw_key_without_prefix, const std::string& value) {
    auto k = key(raw_key_without_prefix);

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SET %s %s", k.c_str(), value.c_str())
    );

    bool ok = commandOK(reply);
    if (reply) freeReplyObject(reply);
    return ok;
}

bool RedisStore::delRaw(const std::string& raw_key_without_prefix) {
    auto k = key(raw_key_without_prefix);

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "DEL %s", k.c_str())
    );

    bool ok = reply != nullptr;
    if (reply) freeReplyObject(reply);
    return ok;
}

std::optional<std::string> RedisStore::getDeviceSecret(const std::string& device_id) {
    return getRaw("device:secret:" + device_id);
}

bool RedisStore::setDeviceSecretForDev(const std::string& device_id, const std::string& secret) {
    return setRaw("device:secret:" + device_id, secret);
}

std::optional<std::string> RedisStore::getLocalKey(const std::string& device_id) {
    return getRaw("device:local_key:" + device_id);
}

bool RedisStore::setLocalKeyForDev(const std::string& device_id, const std::string& local_key) {
    return setRaw("device:local_key:" + device_id, local_key);
}

std::optional<std::string> RedisStore::getDeviceIdByEncryptedId(
    const std::string& encrypted_device_id_b64
) {
    return getRaw("lowpower:device_id_index:" + encrypted_device_id_b64);
}

bool RedisStore::setEncryptedIdIndex(
    const std::string& encrypted_device_id_b64,
    const std::string& device_id
) {
    return setRaw("lowpower:device_id_index:" + encrypted_device_id_b64, device_id);
}

bool RedisStore::setOnline(
    const std::string& device_id,
    const nlohmann::json& state,
    int ttl_sec
) {
    auto k = key("device:online:" + device_id);
    auto v = state.dump();

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SETEX %s %d %s", k.c_str(), ttl_sec, v.c_str())
    );

    bool ok = commandOK(reply);
    if (reply) freeReplyObject(reply);
    return ok;
}

bool RedisStore::setOffline(const std::string& device_id) {
    return delRaw("device:online:" + device_id);
}

bool RedisStore::savePendingWake(
    const std::string& device_id,
    const std::string& msg_id,
    const nlohmann::json& payload,
    int ttl_sec
) {
    auto k = key("device:pending_wake:" + device_id + ":" + msg_id);
    auto v = payload.dump();

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SETEX %s %d %s", k.c_str(), ttl_sec, v.c_str())
    );

    bool ok = commandOK(reply);
    if (reply) freeReplyObject(reply);
    return ok;
}

bool RedisStore::removePendingWake(
    const std::string& device_id,
    const std::string& msg_id
) {
    return delRaw("device:pending_wake:" + device_id + ":" + msg_id);
}