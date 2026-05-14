#include <asio.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "AppConfig.h"
#include "backend/MqttBackend.h"
#include "GatewayContext.h"
#include "HttpAdminServer.h"
#include "TcpGatewayServer.h"
#include "service/WakeManager.h"
#include "storage/RedisStore.h"

namespace {

AppRuntimeConfig makeSessionConfig(const AppConfig& cfg) {
    AppRuntimeConfig runtime_cfg;
    runtime_cfg.heartbeat_default_sec = cfg.server.heartbeat_default_sec;
    runtime_cfg.idle_timeout_sec = cfg.server.idle_timeout_sec;
    runtime_cfg.dev_default_secret = cfg.auth.dev_default_secret;
    runtime_cfg.json_auth_enable = cfg.auth.enable;
    return runtime_cfg;
}

WakeRuntimeConfig makeWakeConfig(const AppConfig& cfg) {
    WakeRuntimeConfig runtime_cfg;
    runtime_cfg.ack_timeout_ms = cfg.wake.ack_timeout_ms;
    runtime_cfg.max_retry = cfg.wake.max_retry;
    runtime_cfg.expire_ms = cfg.wake.expire_ms;
    return runtime_cfg;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::string config_path = argc > 1 ? argv[1] : "config.json";
        AppConfig cfg = AppConfig::load(config_path);

        asio::io_context io;

        RedisStore redis(cfg.redis);
        if (!redis.connect()) {
            return 1;
        }

        GatewayContext ctx;
        ctx.redis = &redis;

        auto session_cfg = makeSessionConfig(cfg);
        auto wake_cfg = makeWakeConfig(cfg);

        WakeManager wake(io, ctx, wake_cfg);
        ctx.wake_manager = &wake;

        MqttBackend mqtt(io, ctx, cfg.mqtt);
        if (cfg.mqtt.enable) {
            if (!mqtt.start()) {
                std::cerr << "[MQTT] failed to start backend bridge" << std::endl;
                return 1;
            }
            ctx.mqtt_backend = &mqtt;
        }

        TcpGatewayServer tcp_server(io, ctx, cfg.server, session_cfg);
        HttpAdminServer http_server(io, ctx, cfg.server);

        wake.start();
        tcp_server.start();
        http_server.start();

        unsigned int worker_threads = cfg.server.worker_threads > 0
            ? static_cast<unsigned int>(cfg.server.worker_threads)
            : std::max(1u, std::thread::hardware_concurrency());

        std::cout << "[RUNTIME] io workers=" << worker_threads << std::endl;

        std::vector<std::thread> workers;
        workers.reserve(worker_threads > 0 ? worker_threads - 1 : 0);

        for (unsigned int i = 1; i < worker_threads; ++i) {
            workers.emplace_back([&io]() {
                io.run();
            });
        }

        io.run();

        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FATAL] " << ex.what() << std::endl;
        return 1;
    }
}