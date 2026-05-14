#pragma once

#include <asio.hpp>
#include "GatewayContext.h"
#include "AppConfig.h"

using asio::ip::tcp;

class HttpAdminServer {
public:
    HttpAdminServer(
        asio::io_context& io,
        GatewayContext& ctx,
        const ServerConfig& server_cfg
    );

    void start();

private:
    void accept();

private:
    asio::io_context& io_;
    GatewayContext& ctx_;
    ServerConfig server_cfg_;
    tcp::acceptor acceptor_;
};