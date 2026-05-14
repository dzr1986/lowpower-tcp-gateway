#include "protocol/JsonLineProtocol.h"

namespace protocol::jsonline {

std::optional<nlohmann::json> parseLine(const std::string& line, std::string& error) {
    try {
        return nlohmann::json::parse(line);
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::string encodeLine(const nlohmann::json& j) {
    std::string s = j.dump();
    s.push_back('\n');
    return s;
}

nlohmann::json makeError(
    const std::string& code,
    const std::string& message
) {
    return {
        {"type", "error"},
        {"code", code},
        {"message", message}
    };
}

nlohmann::json makeLoginAck(
    int seq,
    int result,
    int heartbeat,
    int idle_timeout,
    const std::string& error
) {
    nlohmann::json j = {
        {"type", "login_ack"},
        {"seq", seq},
        {"result", result},
        {"heartbeat", heartbeat},
        {"idle_timeout", idle_timeout}
    };

    if (!error.empty()) {
        j["error"] = error;
    }

    return j;
}

nlohmann::json makeHeartbeatAck(
    int seq,
    int result,
    int next_heartbeat
) {
    return {
        {"type", "heartbeat_ack"},
        {"seq", seq},
        {"result", result},
        {"next_heartbeat", next_heartbeat}
    };
}

nlohmann::json makeWake(
    long long seq,
    const std::string& msg_id,
    const std::string& cmd,
    const nlohmann::json& params,
    long long expire_ms
) {
    return {
        {"type", "wake"},
        {"seq", seq},
        {"msg_id", msg_id},
        {"cmd", cmd},
        {"reason", "admin_api"},
        {"expire_ms", expire_ms},
        {"params", params}
    };
}

nlohmann::json makeTaskDoneAck(
    int seq,
    const std::string& msg_id,
    int result
) {
    return {
        {"type", "task_done_ack"},
        {"seq", seq},
        {"msg_id", msg_id},
        {"result", result}
    };
}

}