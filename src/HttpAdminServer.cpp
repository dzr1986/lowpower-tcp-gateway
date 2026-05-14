#include "HttpAdminServer.h"
#include "service/WakeManager.h"

#include <iostream>
#include <sstream>

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, GatewayContext& ctx)
        : socket_(std::move(socket)), ctx_(ctx) {}

    void start() {
        auto self = shared_from_this();

        asio::async_read_until(
            socket_,
            buffer_,
            "\r\n\r\n",
            [this, self](std::error_code ec, std::size_t) {
                if (ec) return;

                std::istream is(&buffer_);
                std::string line;
                std::getline(is, line);

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                handle(line);
            }
        );
    }

private:
    void handle(const std::string& request_line) {
        std::stringstream ss(request_line);

        std::string method;
        std::string url;
        std::string version;

        ss >> method >> url >> version;

        if (method == "GET" && url.find("/api/devices/") == 0 && url.find("/online") != std::string::npos) {
            auto device_id = extractDeviceId(url);
            handleOnline(device_id);
            return;
        }

        if (method == "POST" && url.find("/api/devices/") == 0 && url.find("/wake") != std::string::npos) {
            auto device_id = extractDeviceId(url);
            auto cmd = extractQuery(url, "cmd");
            if (cmd.empty()) cmd = "take_photo";
            handleWake(device_id, cmd);
            return;
        }

        response(404, {{"error", "not_found"}});
    }

    std::string extractDeviceId(const std::string& url) {
        std::string prefix = "/api/devices/";
        auto p = url.find(prefix);
        if (p == std::string::npos) return "";

        auto start = p + prefix.size();
        auto end = url.find("/", start);

        if (end == std::string::npos) return "";
        return url.substr(start, end - start);
    }

    std::string extractQuery(const std::string& url, const std::string& key) {
        auto q = url.find('?');
        if (q == std::string::npos) return "";

        std::string query = url.substr(q + 1);
        std::stringstream ss(query);
        std::string pair;

        while (std::getline(ss, pair, '&')) {
            auto eq = pair.find('=');
            if (eq == std::string::npos) continue;

            auto k = pair.substr(0, eq);
            auto v = pair.substr(eq + 1);

            if (k == key) return v;
        }

        return "";
    }

    void handleOnline(const std::string& device_id) {
        std::lock_guard<std::mutex> lock(ctx_.mutex);

        if (!ctx_.states.count(device_id)) {
            response(404, {{"error", "device_not_found"}});
            return;
        }

        auto& s = ctx_.states[device_id];

        response(200, {
            {"dev_id", s.device_id},
            {"online", s.online},
            {"imei", s.imei},
            {"fw", s.fw},
            {"model", s.model},
            {"remote_addr", s.remote_addr},
            {"battery", s.battery},
            {"rssi", s.rssi},
            {"rsrp", s.rsrp},
            {"dev_state", s.dev_state}
        });
    }

    void handleWake(const std::string& device_id, const std::string& cmd) {
        try {
            if (!ctx_.wake_manager) {
                response(500, {{"error", "wake_manager_not_ready"}});
                return;
            }

            std::string msg_id = ctx_.wake_manager->sendWake(
                device_id,
                cmd,
                {
                    {"resolution", "640x480"},
                    {"quality", 80}
                }
            );

            response(200, {
                {"result", "sent"},
                {"device_id", device_id},
                {"cmd", cmd},
                {"msg_id", msg_id}
            });
        } catch (const std::exception& e) {
            response(409, {
                {"error", e.what()},
                {"device_id", device_id}
            });
        }
    }

    void response(int code, const nlohmann::json& body) {
        auto self = shared_from_this();

        std::string status = "OK";
        if (code == 404) status = "Not Found";
        if (code == 409) status = "Conflict";
        if (code >= 500) status = "Server Error";

        std::string content = body.dump(2);

        std::ostringstream oss;
        oss << "HTTP/1.1 " << code << " " << status << "\r\n";
        oss << "Content-Type: application/json\r\n";
        oss << "Content-Length: " << content.size() << "\r\n";
        oss << "Connection: close\r\n\r\n";
        oss << content;

        response_ = oss.str();

        asio::async_write(socket_, asio::buffer(response_), [this, self](std::error_code, std::size_t) {
            asio::error_code ec;
            socket_.close(ec);
        });
    }

private:
    tcp::socket socket_;
    asio::streambuf buffer_;
    GatewayContext& ctx_;
    std::string response_;
};

HttpAdminServer::HttpAdminServer(
    asio::io_context& io,
    GatewayContext& ctx,
    const ServerConfig& server_cfg
)
    : io_(io),
      ctx_(ctx),
      server_cfg_(server_cfg),
      acceptor_(io, tcp::endpoint(tcp::v4(), server_cfg.http_port)) {}

void HttpAdminServer::start() {
    std::cout << "[HTTP] listen " << server_cfg_.http_port << std::endl;
    accept();
}

void HttpAdminServer::accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<HttpSession>(std::move(socket), ctx_)->start();
        }

        accept();
    });
}