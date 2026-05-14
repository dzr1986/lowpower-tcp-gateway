#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include <hiredis/hiredis.h>

#include "AppConfig.h"

class RedisStore {
public:
    explicit RedisStore(const RedisConfig& cfg);
    ~RedisStore();

    bool connect();

    std::optional<std::string> getRaw(const std::string& raw_key_without_prefix);
    bool setRaw(const std::string& raw_key_without_prefix, const std::string& value);
    bool delRaw(const std::string& raw_key_without_prefix);

    std::optional<std::string> getDeviceSecret(const std::string& device_id);
    bool setDeviceSecretForDev(const std::string& device_id, const std::string& secret);

    std::optional<std::string> getLocalKey(const std::string& device_id);
    bool setLocalKeyForDev(const std::string& device_id, const std::string& local_key);

    std::optional<std::string> getDeviceIdByEncryptedId(const std::string& encrypted_device_id_b64);
    bool setEncryptedIdIndex(const std::string& encrypted_device_id_b64, const std::string& device_id);

    bool setOnline(const std::string& device_id, const nlohmann::json& state, int ttl_sec);
    bool setOffline(const std::string& device_id);

    bool savePendingWake(
        const std::string& device_id,
        const std::string& msg_id,
        const nlohmann::json& payload,
        int ttl_sec
    );

    bool removePendingWake(
        const std::string& device_id,
        const std::string& msg_id
    );

private:
    std::string key(const std::string& k) const;
    bool commandOK(redisReply* reply) const;

private:
    RedisConfig cfg_;
    redisContext* ctx_ = nullptr;
};