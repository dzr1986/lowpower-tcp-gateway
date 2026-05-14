#include "TcpGatewayServer.h"
#include "session/DeviceSession.h"
#include "storage/RedisStore.h"

#include <iostream>

TcpGatewayServer::TcpGatewayServer(
    asio::io_context& io,
    GatewayContext& ctx,
    const ServerConfig& server_cfg,
    const AppRuntimeConfig& session_cfg
)
    : io_(io),
      ctx_(ctx),
      server_cfg_(server_cfg),
      session_cfg_(session_cfg),
      acceptor_(io, tcp::endpoint(tcp::v4(), server_cfg.tcp_port)),
      offline_timer_(io) {}

void TcpGatewayServer::start() {
    std::cout << "[TCP] listen " << server_cfg_.tcp_port << std::endl;
    accept();
    startOfflineChecker();
}

void TcpGatewayServer::accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<DeviceSession>(
                std::move(socket),
                ctx_,
                session_cfg_
            )->start();
        }

        accept();
    });
}

void TcpGatewayServer::startOfflineChecker() {
    offline_timer_.expires_after(std::chrono::seconds(10));

    offline_timer_.async_wait([this](std::error_code ec) {
        if (!ec) {
            checkOffline();
            startOfflineChecker();
        }
    });
}

void TcpGatewayServer::checkOffline() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> offline;

    {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        for (auto& kv : ctx_.states) {
            auto& state = kv.second;

            if (!state.online) continue;

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - state.last_heartbeat
            ).count();

            if (elapsed > server_cfg_.idle_timeout_sec) {
                offline.push_back(kv.first);
            }
        }

        for (auto& id : offline) {
            ctx_.states[id].online = false;
            ctx_.sessions.erase(id);
        }
    }

    for (auto& id : offline) {
        std::cout << "[OFFLINE_TIMEOUT] device=" << id << std::endl;

        if (ctx_.redis) {
            ctx_.redis->setOffline(id);
        }
    }
}