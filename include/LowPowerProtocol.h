#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace lowpower_lp {

static constexpr uint8_t VERSION_1 = 0x01;

enum class MsgType : uint8_t {
    AUTH_REQ  = 0x00,
    AUTH_RESP = 0x01,
    HEARTBEAT = 0x02,
    WAKEUP    = 0x03
};

enum class EncryptFlag : uint8_t {
    NONE = 0x00,
    AES_CBC_128_PKCS7 = 0x01
};

struct Frame {
    uint8_t version = VERSION_1;
    MsgType type = MsgType::HEARTBEAT;
    uint8_t flag = 0;
    uint16_t size = 0;
    std::vector<uint8_t> payload;
};

struct AuthPayload {
    std::vector<uint8_t> iv;
    std::string encrypted_devid_base64;
    std::vector<uint8_t> encrypted_data;
};

struct AuthRequestData {
    int type = 1;
    int method = 1;
    std::string authorization;
    std::string signature;
    std::string time;
    std::string random;
};

struct AuthResponseBuildResult {
    Frame frame;
    std::string server_random;
};

std::optional<Frame> parseFrame(const std::vector<uint8_t>& data);
std::vector<uint8_t> encodeFrame(const Frame& frame);

std::optional<AuthPayload> parseAuthPayloadLV(const std::vector<uint8_t>& payload);
std::vector<uint8_t> buildAuthPayloadLV(
    const std::vector<uint8_t>& iv,
    const std::string& encrypted_devid_base64,
    const std::vector<uint8_t>& encrypted_data
);

std::vector<uint8_t> makeHeartbeatFrame();
std::vector<uint8_t> makeWakeupFrame(const std::string& local_key);

uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32String(const std::string& s);

std::vector<uint8_t> aes128CbcPkcs7Encrypt(
    const std::string& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& plain
);

std::vector<uint8_t> aes128CbcPkcs7Decrypt(
    const std::string& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& cipher
);

std::string base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64Decode(const std::string& s);

std::string hmacSha256Base64(const std::string& key, const std::string& data);

std::string getAuthField(const std::string& authorization, const std::string& key);

std::optional<AuthRequestData> parseAuthRequestJson(const std::vector<uint8_t>& plain_json);

bool verifyAuthRequest(
    const AuthRequestData& req,
    const std::string& devid,
    const std::string& local_key
);

AuthResponseBuildResult buildAuthResponse(
    const std::string& devid,
    const std::string& local_key,
    const std::string& device_random,
    int heartbeat_interval
);

}