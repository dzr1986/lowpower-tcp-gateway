#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <unordered_map>
#include <deque>
#include <memory>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <map>

using asio::ip::tcp;
using json = nlohmann::json;

static constexpr int TCP_DEVICE_PORT = 9000;
static constexpr int HTTP_ADMIN_PORT = 8080;
static constexpr int DEFAULT_HEARTBEAT_SECONDS = 180;
static constexpr int DEFAULT_IDLE_TIMEOUT_SECONDS = 600;

class DeviceSession;

struct DeviceState {
    std::string device_id;
    std::string imei;
    std::string fw;
    std::string model;
    std::string remote_addr;
    std::string dev_state = "unknown";
    int battery = -1;
    int rssi = 0;
    int rsrp = 0;
    bool charging = false;
    bool online = false;
    std::chrono::steady_clock::time_point login_time;
    std::chrono::steady_clock::time_point last_heartbeat;
};

struct GatewayState {
    std::unordered_map<std::string, std::weak_ptr<DeviceSession>> sessions;
    std::unordered_map<std::string, DeviceState> states;
};

static std::string now_ms_string() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    return std::to_string(ms);
}

static std::string make_msg_id() {
    return "W" + now_ms_string();
}

static std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;

    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            parts.push_back(item);
        }
    }

    return parts;
}

static std::string get_query_param(const std::string& url, const std::string& key) {
    auto qpos = url.find('?');
    if (qpos == std::string::npos) {
        return "";
    }

    std::string query = url.substr(qpos + 1);
    std::stringstream ss(query);
    std::string pair;

    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string k = pair.substr(0, eq);
        std::string v = pair.substr(eq + 1);

        if (k == key) {
            return v;
        }
    }

    return "";
}

static std::string strip_query(const std::string& url) {
    auto qpos = url.find('?');
    if (qpos == std::string::npos) {
        return url;
    }
    return url.substr(0, qpos);
}

class DeviceSession : public std::enable_shared_from_this<DeviceSession> {
public:
    DeviceSession(tcp::socket socket, GatewayState& gateway)
        : socket_(std::move(socket)), gateway_(gateway) {}

    void start() {
        remote_addr_ = socket_.remote_endpoint().address().to_string();
        std::cout << "[TCP] new connection from " << remote_addr_ << std::endl;
        do_read_line();
    }

    std::string device_id() const {
        return device_id_;
    }

    bool is_logged_in() const {
        return !device_id_.empty();
    }

    void send_json(const json& j) {
        auto self = shared_from_this();

        std::string data = j.dump();
        data.push_back('\n');

        asio::post(socket_.get_executor(), [this, self, data]() {
            bool writing = !write_queue_.empty();
            write_queue_.push_back(data);

            if (!writing) {
                do_write();
            }
        });
    }

    void close() {
        asio::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    void do_read_line() {
        auto self = shared_from_this();

        asio::async_read_until(
            socket_,
            read_buffer_,
            '\n',
            [this, self](std::error_code ec, std::size_t length) {
                if (ec) {
                    handle_disconnect();
                    return;
                }

                std::istream is(&read_buffer_);
                std::string line;
                std::getline(is, line);

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (!line.empty()) {
                    handle_message(line);
                }

                do_read_line();
            }
        );
    }

    void do_write() {
        auto self = shared_from_this();

        asio::async_write(
            socket_,
            asio::buffer(write_queue_.front()),
            [this, self](std::error_code ec, std::size_t) {
                if (ec) {
                    handle_disconnect();
                    return;
                }

                write_queue_.pop_front();

                if (!write_queue_.empty()) {
                    do_write();
                }
            }
        );
    }

    void handle_message(const std::string& line) {
        json msg;

        try {
            msg = json::parse(line);
        } catch (const std::exception& e) {
            std::cout << "[TCP] invalid json: " << line << std::endl;
            send_json({
                {"type", "error"},
                {"code", "invalid_json"},
                {"message", e.what()}
            });
            return;
        }

        std::string type = msg.value("type", "");

        if (type == "login") {
            handle_login(msg);
        } else if (type == "heartbeat") {
            handle_heartbeat(msg);
        } else if (type == "wake_ack") {
            handle_wake_ack(msg);
        } else if (type == "task_done") {
            handle_task_done(msg);
        } else if (type == "host_sleep") {
            handle_host_sleep(msg);
        } else if (type == "host_state") {
            handle_host_state(msg);
        } else {
            send_json({
                {"type", "error"},
                {"code", "unknown_type"},
                {"message", type}
            });
        }
    }

    void handle_login(const json& msg) {
        std::string device_id = msg.value("device_id", "");
        std::string imei = msg.value("imei", "");
        std::string fw = msg.value("fw", "");
        std::string model = msg.value("model", "");

        if (device_id.empty()) {
            send_json({
                {"type", "login_ack"},
                {"result", 1},
                {"error", "device_id_required"}
            });
            return;
        }

        device_id_ = device_id;

        DeviceState state;
        state.device_id = device_id;
        state.imei = imei;
        state.fw = fw;
        state.model = model;
        state.remote_addr = remote_addr_;
        state.online = true;
        state.dev_state = msg.value("dev", "unknown");
        state.login_time = std::chrono::steady_clock::now();
        state.last_heartbeat = std::chrono::steady_clock::now();

        gateway_.sessions[device_id] = shared_from_this();
        gateway_.states[device_id] = state;

        int seq = msg.value("seq", 0);

        std::cout << "[LOGIN] device=" << device_id
                  << " imei=" << imei
                  << " fw=" << fw
                  << " model=" << model
                  << " addr=" << remote_addr_
                  << std::endl;

        send_json({
            {"type", "login_ack"},
            {"seq", seq},
            {"result", 0},
            {"heartbeat", DEFAULT_HEARTBEAT_SECONDS},
            {"idle_timeout", DEFAULT_IDLE_TIMEOUT_SECONDS},
            {"server_time_ms", now_ms_string()}
        });
    }

    void handle_heartbeat(const json& msg) {
        if (!is_logged_in()) {
            send_json({
                {"type", "error"},
                {"code", "not_logged_in"}
            });
            return;
        }

        auto& state = gateway_.states[device_id_];

        state.last_heartbeat = std::chrono::steady_clock::now();
        state.online = true;
        state.battery = msg.value("battery", state.battery);
        state.rssi = msg.value("rssi", state.rssi);
        state.rsrp = msg.value("rsrp", state.rsrp);
        state.dev_state = msg.value("dev", state.dev_state);
        state.charging = msg.value("charging", state.charging);

        int seq = msg.value("seq", 0);

        std::cout << "[HEARTBEAT] device=" << device_id_
                  << " battery=" << state.battery
                  << " rssi=" << state.rssi
                  << " dev=" << state.dev_state
                  << std::endl;

        int next_heartbeat = DEFAULT_HEARTBEAT_SECONDS;

        if (state.battery >= 0 && state.battery < 20) {
            next_heartbeat = 300;
        }

        if (state.battery >= 0 && state.battery < 10) {
            next_heartbeat = 600;
        }

        send_json({
            {"type", "heartbeat_ack"},
            {"seq", seq},
            {"result", 0},
            {"next_heartbeat", next_heartbeat}
        });
    }

    void handle_wake_ack(const json& msg) {
        std::string msg_id = msg.value("msg_id", "");
        int result = msg.value("result", -1);
        std::string status = msg.value("status", "");

        std::cout << "[WAKE_ACK] device=" << device_id_
                  << " msg_id=" << msg_id
                  << " result=" << result
                  << " status=" << status
                  << std::endl;
    }

    void handle_task_done(const json& msg) {
        std::string msg_id = msg.value("msg_id", "");
        int result = msg.value("result", -1);
        std::string file_type = msg.value("file_type", "");
        std::string url = msg.value("url", "");
        int size = msg.value("size", 0);

        std::cout << "[TASK_DONE] device=" << device_id_
                  << " msg_id=" << msg_id
                  << " result=" << result
                  << " file_type=" << file_type
                  << " size=" << size
                  << " url=" << url
                  << std::endl;

        int seq = msg.value("seq", 0);

        send_json({
            {"type", "task_done_ack"},
            {"seq", seq},
            {"msg_id", msg_id},
            {"result", 0}
        });
    }

    void handle_host_sleep(const json& msg) {
        if (!is_logged_in()) {
            return;
        }

        auto& state = gateway_.states[device_id_];
        state.dev_state = "sleep";

        std::cout << "[HOST_SLEEP] device=" << device_id_ << std::endl;

        int seq = msg.value("seq", 0);

        send_json({
            {"type", "host_sleep_ack"},
            {"seq", seq},
            {"result", 0}
        });
    }

    void handle_host_state(const json& msg) {
        if (!is_logged_in()) {
            return;
        }

        auto& state = gateway_.states[device_id_];
        state.dev_state = msg.value("dev", state.dev_state);

        std::cout << "[HOST_STATE] device=" << device_id_
                  << " dev=" << state.dev_state
                  << std::endl;
    }

    void handle_disconnect() {
        if (!device_id_.empty()) {
            std::cout << "[DISCONNECT] device=" << device_id_ << std::endl;

            auto it = gateway_.states.find(device_id_);
            if (it != gateway_.states.end()) {
                it->second.online = false;
            }

            gateway_.sessions.erase(device_id_);
        } else {
            std::cout << "[DISCONNECT] unknown device from "
                      << remote_addr_
                      << std::endl;
        }

        close();
    }

private:
    tcp::socket socket_;
    asio::streambuf read_buffer_;
    std::deque<std::string> write_queue_;

    GatewayState& gateway_;

    std::string device_id_;
    std::string remote_addr_;
};

class TcpGatewayServer {
public:
    TcpGatewayServer(asio::io_context& io, GatewayState& gateway, int port)
        : io_(io),
          gateway_(gateway),
          acceptor_(io, tcp::endpoint(tcp::v4(), port)),
          offline_timer_(io) {}

    void start() {
        std::cout << "[TCP] listening on port " << TCP_DEVICE_PORT << std::endl;
        do_accept();
        start_offline_checker();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<DeviceSession>(
                    std::move(socket),
                    gateway_
                );
                session->start();
            }

            do_accept();
        });
    }

    void start_offline_checker() {
        offline_timer_.expires_after(std::chrono::seconds(10));

        offline_timer_.async_wait([this](std::error_code ec) {
            if (ec) {
                return;
            }

            check_offline_devices();
            start_offline_checker();
        });
    }

    void check_offline_devices() {
        auto now = std::chrono::steady_clock::now();

        std::vector<std::string> offline_devices;

        for (auto& kv : gateway_.states) {
            const std::string& device_id = kv.first;
            DeviceState& state = kv.second;

            if (!state.online) {
                continue;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - state.last_heartbeat
            ).count();

            if (elapsed > DEFAULT_IDLE_TIMEOUT_SECONDS) {
                offline_devices.push_back(device_id);
            }
        }

        for (const auto& device_id : offline_devices) {
            std::cout << "[OFFLINE_TIMEOUT] device=" << device_id << std::endl;

            gateway_.states[device_id].online = false;

            auto it = gateway_.sessions.find(device_id);
            if (it != gateway_.sessions.end()) {
                if (auto session = it->second.lock()) {
                    session->close();
                }
                gateway_.sessions.erase(it);
            }
        }
    }

private:
    asio::io_context& io_;
    GatewayState& gateway_;
    tcp::acceptor acceptor_;
    asio::steady_timer offline_timer_;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, GatewayState& gateway)
        : socket_(std::move(socket)), gateway_(gateway) {}

    void start() {
        do_read();
    }

private:
    void do_read() {
        auto self = shared_from_this();

        asio::async_read_until(
            socket_,
            buffer_,
            "\r\n\r\n",
            [this, self](std::error_code ec, std::size_t) {
                if (ec) {
                    return;
                }

                std::istream is(&buffer_);
                std::string request_line;
                std::getline(is, request_line);

                if (!request_line.empty() && request_line.back() == '\r') {
                    request_line.pop_back();
                }

                handle_request(request_line);
            }
        );
    }

    void handle_request(const std::string& request_line) {
        std::stringstream ss(request_line);

        std::string method;
        std::string url;
        std::string version;

        ss >> method >> url >> version;

        std::string path = strip_query(url);
        auto parts = split_path(path);

        if (method == "GET" &&
            parts.size() == 4 &&
            parts[0] == "api" &&
            parts[1] == "devices" &&
            parts[3] == "online") {
            handle_get_online(parts[2]);
            return;
        }

        if (method == "POST" &&
            parts.size() == 4 &&
            parts[0] == "api" &&
            parts[1] == "devices" &&
            parts[3] == "wake") {
            std::string cmd = get_query_param(url, "cmd");
            if (cmd.empty()) {
                cmd = "take_photo";
            }

            handle_post_wake(parts[2], cmd);
            return;
        }

        send_response(404, {
            {"error", "not_found"},
            {"request", request_line}
        });
    }

    void handle_get_online(const std::string& device_id) {
        auto it = gateway_.states.find(device_id);

        if (it == gateway_.states.end()) {
            send_response(404, {
                {"error", "device_not_found"},
                {"device_id", device_id}
            });
            return;
        }

        const auto& s = it->second;

        auto now = std::chrono::steady_clock::now();

        long last_heartbeat_age = std::chrono::duration_cast<std::chrono::seconds>(
            now - s.last_heartbeat
        ).count();

        send_response(200, {
            {"device_id", s.device_id},
            {"online", s.online},
            {"imei", s.imei},
            {"fw", s.fw},
            {"model", s.model},
            {"remote_addr", s.remote_addr},
            {"battery", s.battery},
            {"rssi", s.rssi},
            {"rsrp", s.rsrp},
            {"charging", s.charging},
            {"dev", s.dev_state},
            {"last_heartbeat_age_sec", last_heartbeat_age}
        });
    }

    void handle_post_wake(const std::string& device_id, const std::string& cmd) {
        auto it = gateway_.sessions.find(device_id);

        if (it == gateway_.sessions.end()) {
            send_response(409, {
                {"error", "device_offline"},
                {"device_id", device_id}
            });
            return;
        }

        auto session = it->second.lock();

        if (!session) {
            gateway_.sessions.erase(it);
            send_response(409, {
                {"error", "device_session_expired"},
                {"device_id", device_id}
            });
            return;
        }

        std::string msg_id = make_msg_id();

        json wake = {
            {"type", "wake"},
            {"seq", std::stoll(now_ms_string())},
            {"msg_id", msg_id},
            {"cmd", cmd},
            {"reason", "http_api"},
            {"expire_ms", std::stoll(now_ms_string()) + 300000},
            {"params", {
                {"resolution", "640x480"},
                {"quality", 80}
            }}
        };

        session->send_json(wake);

        std::cout << "[HTTP_WAKE] device=" << device_id
                  << " cmd=" << cmd
                  << " msg_id=" << msg_id
                  << std::endl;

        send_response(200, {
            {"result", "sent"},
            {"device_id", device_id},
            {"cmd", cmd},
            {"msg_id", msg_id}
        });
    }

    void send_response(int status, const json& body) {
        auto self = shared_from_this();

        std::string status_text = "OK";

        if (status == 404) {
            status_text = "Not Found";
        } else if (status == 409) {
            status_text = "Conflict";
        } else if (status >= 500) {
            status_text = "Internal Server Error";
        }

        std::string content = body.dump(2);

        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << " " << status_text << "\r\n";
        oss << "Content-Type: application/json\r\n";
        oss << "Content-Length: " << content.size() << "\r\n";
        oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << content;

        response_ = oss.str();

        asio::async_write(
            socket_,
            asio::buffer(response_),
            [this, self](std::error_code, std::size_t) {
                asio::error_code ec;
                socket_.shutdown(tcp::socket::shutdown_both, ec);
                socket_.close(ec);
            }
        );
    }

private:
    tcp::socket socket_;
    asio::streambuf buffer_;
    GatewayState& gateway_;
    std::string response_;
};

class HttpAdminServer {
public:
    HttpAdminServer(asio::io_context& io, GatewayState& gateway, int port)
        : io_(io),
          gateway_(gateway),
          acceptor_(io, tcp::endpoint(tcp::v4(), port)) {}

    void start() {
        std::cout << "[HTTP] admin listening on port "
                  << HTTP_ADMIN_PORT
                  << std::endl;
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<HttpSession>(
                    std::move(socket),
                    gateway_
                );
                session->start();
            }

            do_accept();
        });
    }

private:
    asio::io_context& io_;
    GatewayState& gateway_;
    tcp::acceptor acceptor_;
};

int main() {
    try {
        asio::io_context io;

        GatewayState gateway;

        TcpGatewayServer tcp_server(
            io,
            gateway,
            TCP_DEVICE_PORT
        );

        HttpAdminServer http_server(
            io,
            gateway,
            HTTP_ADMIN_PORT
        );

        tcp_server.start();
        http_server.start();

        io.run();
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}