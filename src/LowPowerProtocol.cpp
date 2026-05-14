#include "LowPowerProtocol.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <stdexcept>

namespace lowpower_lp {

static uint16_t readU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static void writeU16BE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

std::optional<Frame> parseFrame(const std::vector<uint8_t>& data) {
    if (data.size() < 5) return std::nullopt;

    Frame f;
    f.version = data[0];
    f.type = static_cast<MsgType>(data[1]);
    f.flag = data[2];
    f.size = readU16BE(&data[3]);

    if (data.size() != static_cast<size_t>(5 + f.size)) {
        return std::nullopt;
    }

    if (f.size > 0) {
        f.payload.assign(data.begin() + 5, data.end());
    }

    return f;
}

std::vector<uint8_t> encodeFrame(const Frame& frame) {
    std::vector<uint8_t> out;
    out.reserve(5 + frame.payload.size());

    out.push_back(frame.version);
    out.push_back(static_cast<uint8_t>(frame.type));
    out.push_back(frame.flag);
    writeU16BE(out, static_cast<uint16_t>(frame.payload.size()));

    out.insert(out.end(), frame.payload.begin(), frame.payload.end());

    return out;
}

std::optional<AuthPayload> parseAuthPayloadLV(const std::vector<uint8_t>& payload) {
    if (payload.size() < 6) return std::nullopt;

    uint16_t iv_len = readU16BE(&payload[0]);
    uint16_t devid_len = readU16BE(&payload[2]);
    uint16_t data_len = readU16BE(&payload[4]);

    size_t expect = 6 + iv_len + devid_len + data_len;
    if (payload.size() != expect) return std::nullopt;

    size_t pos = 6;

    AuthPayload p;

    p.iv.assign(payload.begin() + pos, payload.begin() + pos + iv_len);
    pos += iv_len;

    p.encrypted_devid_base64.assign(
        reinterpret_cast<const char*>(&payload[pos]),
        devid_len
    );
    pos += devid_len;

    p.encrypted_data.assign(payload.begin() + pos, payload.begin() + pos + data_len);

    return p;
}

std::vector<uint8_t> buildAuthPayloadLV(
    const std::vector<uint8_t>& iv,
    const std::string& encrypted_devid_base64,
    const std::vector<uint8_t>& encrypted_data
) {
    std::vector<uint8_t> out;

    writeU16BE(out, static_cast<uint16_t>(iv.size()));
    writeU16BE(out, static_cast<uint16_t>(encrypted_devid_base64.size()));
    writeU16BE(out, static_cast<uint16_t>(encrypted_data.size()));

    out.insert(out.end(), iv.begin(), iv.end());
    out.insert(out.end(), encrypted_devid_base64.begin(), encrypted_devid_base64.end());
    out.insert(out.end(), encrypted_data.begin(), encrypted_data.end());

    return out;
}

std::vector<uint8_t> makeHeartbeatFrame() {
    Frame f;
    f.version = VERSION_1;
    f.type = MsgType::HEARTBEAT;
    f.flag = 0;
    return encodeFrame(f);
}

std::vector<uint8_t> makeWakeupFrame(const std::string& local_key) {
    uint32_t c = crc32String(local_key);

    Frame f;
    f.version = VERSION_1;
    f.type = MsgType::WAKEUP;
    f.flag = 0;
    writeU32BE(f.payload, c);

    return encodeFrame(f);
}

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];

        for (int j = 0; j < 8; ++j) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }

    return ~crc;
}

uint32_t crc32String(const std::string& s) {
    return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static std::vector<uint8_t> normalizeAes128Key(const std::string& key) {
    std::vector<uint8_t> k(16, 0);
    std::memcpy(k.data(), key.data(), std::min<size_t>(16, key.size()));
    return k;
}

std::vector<uint8_t> aes128CbcPkcs7Encrypt(
    const std::string& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& plain
) {
    if (iv.size() != 16) throw std::runtime_error("AES CBC IV must be 16 bytes");

    auto k = normalizeAes128Key(key);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<uint8_t> out(plain.size() + 16);
    int out_len1 = 0;
    int out_len2 = 0;

    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, k.data(), iv.data());
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    EVP_EncryptUpdate(ctx, out.data(), &out_len1, plain.data(), plain.size());
    EVP_EncryptFinal_ex(ctx, out.data() + out_len1, &out_len2);

    EVP_CIPHER_CTX_free(ctx);

    out.resize(out_len1 + out_len2);
    return out;
}

std::vector<uint8_t> aes128CbcPkcs7Decrypt(
    const std::string& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& cipher
) {
    if (iv.size() != 16) throw std::runtime_error("AES CBC IV must be 16 bytes");

    auto k = normalizeAes128Key(key);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<uint8_t> out(cipher.size());
    int out_len1 = 0;
    int out_len2 = 0;

    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, k.data(), iv.data());
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    if (EVP_DecryptUpdate(ctx, out.data(), &out_len1, cipher.data(), cipher.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES decrypt update failed");
    }

    if (EVP_DecryptFinal_ex(ctx, out.data() + out_len1, &out_len2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES decrypt final failed");
    }

    EVP_CIPHER_CTX_free(ctx);

    out.resize(out_len1 + out_len2);
    return out;
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.resize(4 * ((data.size() + 2) / 3));

    int len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&out[0]),
        data.data(),
        static_cast<int>(data.size())
    );

    out.resize(len);
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.resize(3 * s.size() / 4 + 3);

    int len = EVP_DecodeBlock(
        out.data(),
        reinterpret_cast<const unsigned char*>(s.data()),
        static_cast<int>(s.size())
    );

    if (len < 0) {
        throw std::runtime_error("base64 decode failed");
    }

    size_t padding = 0;
    if (!s.empty() && s[s.size() - 1] == '=') padding++;
    if (s.size() > 1 && s[s.size() - 2] == '=') padding++;

    out.resize(len - padding);
    return out;
}

std::string hmacSha256Base64(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(
        EVP_sha256(),
        key.data(),
        static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(data.data()),
        data.size(),
        digest,
        &digest_len
    );

    std::vector<uint8_t> bytes(digest, digest + digest_len);
    return base64Encode(bytes);
}

std::string getAuthField(const std::string& authorization, const std::string& key) {
    std::stringstream ss(authorization);
    std::string item;

    while (std::getline(ss, item, ',')) {
        auto pos = item.find('=');
        if (pos == std::string::npos) continue;

        std::string k = item.substr(0, pos);
        std::string v = item.substr(pos + 1);

        if (k == key) return v;
    }

    return "";
}

std::optional<AuthRequestData> parseAuthRequestJson(const std::vector<uint8_t>& plain_json) {
    try {
        std::string s(plain_json.begin(), plain_json.end());
        auto j = nlohmann::json::parse(s);

        AuthRequestData req;
        req.type = j.value("type", 1);
        req.method = j.value("method", 1);
        req.authorization = j.value("authorization", "");
        req.signature = j.value("signature", "");
        req.time = getAuthField(req.authorization, "time");
        req.random = getAuthField(req.authorization, "random");

        if (req.authorization.empty() || req.signature.empty() || req.random.empty()) {
            return std::nullopt;
        }

        return req;
    } catch (...) {
        return std::nullopt;
    }
}

bool verifyAuthRequest(
    const AuthRequestData& req,
    const std::string& devid,
    const std::string& local_key
) {
    std::string sign_src = devid + ":" + req.time + ":" + req.random;
    std::string expected = hmacSha256Base64(local_key, sign_src);
    return expected == req.signature;
}

AuthResponseBuildResult buildAuthResponse(
    const std::string& devid,
    const std::string& local_key,
    const std::string& device_random,
    int heartbeat_interval
) {
    std::vector<uint8_t> iv(16);
    RAND_bytes(iv.data(), iv.size());

    auto now = std::chrono::system_clock::now();
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();

    std::vector<uint8_t> rnd(16);
    RAND_bytes(rnd.data(), rnd.size());
    std::string server_random = base64Encode(rnd);

    std::string authorization =
        "time=" + std::to_string(now_sec) +
        ",random=" + server_random;

    std::string sign_src =
        devid + ":" +
        std::to_string(now_sec) + ":" +
        server_random;

    std::string signature = hmacSha256Base64(local_key, sign_src);

    nlohmann::json resp = {
        {"err", 0},
        {"interval", heartbeat_interval},
        {"random", device_random},
        {"authorization", authorization},
        {"signature", signature}
    };

    std::string resp_str = resp.dump();
    std::vector<uint8_t> plain(resp_str.begin(), resp_str.end());
    auto encrypted_data = aes128CbcPkcs7Encrypt(local_key, iv, plain);

    std::vector<uint8_t> devid_plain(devid.begin(), devid.end());
    auto encrypted_devid = aes128CbcPkcs7Encrypt(local_key, iv, devid_plain);
    auto encrypted_devid_b64 = base64Encode(encrypted_devid);

    Frame f;
    f.version = VERSION_1;
    f.type = MsgType::AUTH_RESP;
    f.flag = static_cast<uint8_t>(EncryptFlag::AES_CBC_128_PKCS7);
    f.payload = buildAuthPayloadLV(iv, encrypted_devid_b64, encrypted_data);

    return {
        .frame = f,
        .server_random = server_random
    };
}

}