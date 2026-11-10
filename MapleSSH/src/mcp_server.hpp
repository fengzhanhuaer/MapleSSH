#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>

namespace maple {
class McpServer final {
public:
    struct Callbacks {
        std::function<std::string()> list_sessions;
        std::function<std::string(std::string_view)> session_status;
        std::function<std::string(std::string_view, std::string_view)> send_input;
        std::function<std::string(std::string_view, std::size_t)> read_output;
        std::function<std::string(std::string_view)> disconnect;
        std::function<std::string(std::string_view)> connect;
        std::function<std::string(std::string_view, std::string_view)> files;
    };
    explicit McpServer(Callbacks callbacks);
    ~McpServer();
    void Start(std::uint16_t port = 18542);
    void Stop();
    bool Running() const { return running_; }
    std::uint16_t Port() const { return port_; }
private:
    void Run(std::stop_token stop);
    void HandleClient(std::uintptr_t client);
    std::string HandleMessage(std::string_view body);
    Callbacks callbacks_;
    std::jthread worker_;
    std::atomic<std::uintptr_t> listen_socket_{static_cast<std::uintptr_t>(-1)};
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{true};
    std::mutex client_mutex_;
    std::uintptr_t client_socket_ = static_cast<std::uintptr_t>(-1);
};
}

