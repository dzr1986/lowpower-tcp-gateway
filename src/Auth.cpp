#include "Auth.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cmath>

std::string hmacSha256Hex(const std::string& secret, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(data.data()),
        data.size(),
        digest,
        &digest_len
    );

    std::ostringstream oss;
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);
    }

    return oss.str();
}

bool verifyLoginSignature(
    const nlohmann::json& msg,
    const std::string& secret,
    int timestamp_window_sec,
    std::string& error
) {
    std::string device_id = msg.value("device_id", "");
    std::string imei = msg.value("imei", "");
    std::string nonce = msg.value("nonce", "");
    std::string sign = msg.value("sign", "");
    long long ts = msg.value("timestamp", 0LL);

    if (device_id.empty() || imei.empty() || nonce.empty() || sign.empty() || ts <= 0) {
        error = "missing auth fields";
        return false;
    }

    auto now = std::chrono::system_clock::now();
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();

    if (std::llabs(now_sec - ts) > timestamp_window_sec) {
        error = "timestamp expired";
        return false;
    }

    std::string data =
        device_id + "|" +
        imei + "|" +
        std::to_string(ts) + "|" +
        nonce;

    std::string expected = hmacSha256Hex(secret, data);

    if (expected != sign) {
        error = "bad signature";
        return false;
    }

    return true;
}