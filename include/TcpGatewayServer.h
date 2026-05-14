#pragma once

#include <asio.hpp>
#include "AppConfig.h"
#include "session/DeviceSession.h"

using asio::ip::tcp;

class TcpGatewayServer {
public:
    TcpGatewayServer(
        asio::io_context& io,
        GatewayContext& ctx,
        const ServerConfig& server_cfg,
        const AppRuntimeConfig& session_cfg
    );

    void start();

private:
    void accept();
    void startOfflineChecker();
    void checkOffline();

private:
    asio::io_context& io_;
    GatewayContext& ctx_;
    ServerConfig server_cfg_;
    AppRuntimeConfig session_cfg_;
    tcp::acceptor acceptor_;
    asio::steady_timer offline_timer_;
};