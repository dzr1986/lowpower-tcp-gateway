#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace protocol::jsonline {

std::optional<nlohmann::json> parseLine(const std::string& line, std::string& error);

std::string encodeLine(const nlohmann::json& j);

nlohmann::json makeError(
    const std::string& code,
    const std::string& message
);

nlohmann::json makeLoginAck(
    int seq,
    int result,
    int heartbeat,
    int idle_timeout,
    const std::string& error = ""
);

nlohmann::json makeHeartbeatAck(
    int seq,
    int result,
    int next_heartbeat
);

nlohmann::json makeWake(
    long long seq,
    const std::string& msg_id,
    const std::string& cmd,
    const nlohmann::json& params,
    long long expire_ms
);

nlohmann::json makeTaskDoneAck(
    int seq,
    const std::string& msg_id,
    int result
);

}