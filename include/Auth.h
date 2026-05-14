#pragma once

#include <string>
#include <nlohmann/json.hpp>

std::string hmacSha256Hex(const std::string& secret, const std::string& data);

bool verifyLoginSignature(
    const nlohmann::json& msg,
    const std::string& secret,
    int timestamp_window_sec,
    std::string& error
);