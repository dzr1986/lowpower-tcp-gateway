std::optional<std::string> RedisStore::getLocalKey(const std::string& device_id) {
    auto k = key("device:local_key:" + device_id);

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", k.c_str())
    );

    if (!reply) return std::nullopt;

    std::optional<std::string> result;

    if (reply->type == REDIS_REPLY_STRING) {
        result = std::string(reply->str, reply->len);
    }

    freeReplyObject(reply);
    return result;
}

bool RedisStore::setLocalKeyForDev(const std::string& device_id, const std::string& local_key) {
    auto k = key("device:local_key:" + device_id);

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SET %s %s", k.c_str(), local_key.c_str())
    );

    bool ok = commandOK(reply);
    if (reply) freeReplyObject(reply);
    return ok;
}