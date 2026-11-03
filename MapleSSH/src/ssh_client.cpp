#include "ssh_client.hpp"
#include "crypto.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#include <openssl/evp.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>

namespace maple {
namespace {
using Clock = std::chrono::steady_clock;
void Initialize() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) || libssh2_init(0)) throw std::runtime_error("无法初始化 SSH 网络库");
    });
}
void Cancelled(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("连接已取消"); }
std::string Base64(const unsigned char* bytes, std::size_t size) {
    std::string output(4 * ((size + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), bytes, static_cast<int>(size));
    return output;
}
std::wstring WideHost(const std::string& host) {
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), static_cast<int>(host.size()), nullptr, 0);
    if (!count) throw std::runtime_error("主机地址编码无效");
    std::wstring output(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), static_cast<int>(host.size()), output.data(), count);
    return output;
}
void WaitSocket(SOCKET socket, bool read, bool write) {
    if (!read && !write) read = true;
    fd_set readers;
    fd_set writers;
    FD_ZERO(&readers);
    FD_ZERO(&writers);
    if (read) FD_SET(socket, &readers);
    if (write) FD_SET(socket, &writers);
    timeval timeout{0, 100000};
    if (select(0, &readers, &writers, nullptr, &timeout) == SOCKET_ERROR) throw std::runtime_error("SSH 套接字失效");
}
struct Connection {
    SOCKET socket = INVALID_SOCKET;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_CHANNEL* channel = nullptr;
    ~Connection() {
        if (socket != INVALID_SOCKET) { shutdown(socket, SD_BOTH); closesocket(socket); }
        if (channel) libssh2_channel_free(channel);
        if (session) libssh2_session_free(session);
    }
};
void Connect(Connection& connection, const Session& settings, std::stop_token stop) {
    const auto host = WideHost(settings.host);
    const auto service = std::to_wstring(settings.port);
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    PADDRINFOEXW addresses = nullptr;
    OVERLAPPED pending{};
    pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pending.hEvent) throw std::runtime_error("无法初始化 DNS 查询");
    HANDLE cancellation = nullptr;
    const auto deadline = Clock::now() + std::chrono::seconds(settings.timeout_seconds);
    auto result = GetAddrInfoExW(host.c_str(), service.c_str(), NS_DNS, nullptr, &hints, &addresses,
        nullptr, &pending, nullptr, &cancellation);
    if (result == WSA_IO_PENDING) {
        while (WaitForSingleObject(pending.hEvent, 100) == WAIT_TIMEOUT) {
            if (stop.stop_requested() || Clock::now() >= deadline) {
                GetAddrInfoExCancel(&cancellation);
                WaitForSingleObject(pending.hEvent, INFINITE);
                break;
            }
        }
        result = GetAddrInfoExOverlappedResult(&pending);
    }
    CloseHandle(pending.hEvent);
    std::unique_ptr<ADDRINFOEXW, decltype(&FreeAddrInfoExW)> list(addresses, FreeAddrInfoExW);
    Cancelled(stop);
    if (result || !addresses) throw std::runtime_error("主机解析失败或超时");
    for (auto* address = addresses; address; address = address->ai_next) {
        Cancelled(stop);
        SOCKET candidate = socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        ioctlsocket(candidate, FIONBIO, &nonblocking);
        const auto started = connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        bool ready = started == 0;
        if (!ready && WSAGetLastError() == WSAEWOULDBLOCK) {
            while (!stop.stop_requested() && Clock::now() < deadline) {
                fd_set writable;
                fd_set errors;
                FD_ZERO(&writable); FD_ZERO(&errors);
                FD_SET(candidate, &writable); FD_SET(candidate, &errors);
                timeval timeout{0, 100000};
                const auto selected = select(0, nullptr, &writable, &errors, &timeout);
                if (selected == SOCKET_ERROR) break;
                if (selected > 0) {
                    int error = 0;
                    int length = sizeof(error);
                    ready = getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
                    break;
                }
            }
        }
        if (ready) { connection.socket = candidate; return; }
        closesocket(candidate);
    }
    Cancelled(stop);
    throw std::runtime_error("连接失败或超时，请检查地址和端口");
}
int Retry(Connection& connection, std::stop_token stop, int timeout, const std::function<int()>& operation) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout);
    for (;;) {
        Cancelled(stop);
        const auto result = operation();
        if (result != LIBSSH2_ERROR_EAGAIN) return result;
        if (Clock::now() >= deadline) throw std::runtime_error("SSH 操作超时");
        const auto directions = libssh2_session_block_directions(connection.session);
        WaitSocket(connection.socket, (directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0,
            (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0);
    }
}
}
SshClient::SshClient(Session session, Credential credential) : session_(std::move(session)), credential_(std::move(credential)) { Initialize(); }
SshClient::~SshClient() { Stop(); }
void SshClient::Start(int rows, int columns) {
    Stop();
    { const std::lock_guard lock(mutex_); host_decision_.reset(); input_.clear(); events_.clear(); queued_bytes_ = 0; rows_ = std::clamp(rows, 2, 300); columns_ = std::clamp(columns, 2, 500); resized_ = true; }
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
void SshClient::Stop() {
    if (worker_.joinable()) { worker_.request_stop(); condition_.notify_all(); worker_.join(); }
    connected_ = false;
}
void SshClient::Send(std::string text) {
    if (!connected_) return;
    const std::lock_guard lock(mutex_);
    if (input_.size() + text.size() > 1024 * 1024) throw std::runtime_error("待发送输入过多，请稍后重试");
    input_ += text;
}
void SshClient::Resize(int rows, int columns) {
    const std::lock_guard lock(mutex_);
    rows_ = std::clamp(rows, 2, 300);
    columns_ = std::clamp(columns, 2, 500);
    resized_ = true;
}
void SshClient::Post(SshEvent event) {
    const std::lock_guard lock(mutex_);
    queued_bytes_ += event.text.size();
    events_.push_back(std::move(event));
}
std::vector<SshEvent> SshClient::Drain() {
    const std::lock_guard lock(mutex_);
    std::vector<SshEvent> output;
    output.reserve(events_.size());
    while (!events_.empty()) { output.push_back(std::move(events_.front())); events_.pop_front(); }
    queued_bytes_ = 0;
    condition_.notify_all();
    return output;
}
void SshClient::ConfirmHost(bool accepted) {
    { const std::lock_guard lock(mutex_); host_decision_ = accepted; }
    condition_.notify_all();
}
void SshClient::Run(std::stop_token stop) {
    try {
        Post({SshEventKind::State, "正在连接"});
        Connection connection;
        Connect(connection, session_, stop);
        connection.session = libssh2_session_init();
        if (!connection.session) throw std::runtime_error("无法初始化 SSH 会话");
        libssh2_session_set_blocking(connection.session, 0);
        if (Retry(connection, stop, session_.timeout_seconds, [&] { return libssh2_session_handshake(connection.session, connection.socket); }))
            throw std::runtime_error("SSH 握手失败");
        std::size_t key_size = 0;
        int key_type = 0;
        const auto* key = libssh2_session_hostkey(connection.session, &key_size, &key_type);
        const auto* fingerprint = reinterpret_cast<const unsigned char*>(libssh2_hostkey_hash(connection.session, LIBSSH2_HOSTKEY_HASH_SHA256));
        if (!key || !fingerprint || key_size > 65536) throw std::runtime_error("无法读取服务器主机密钥");
        std::string algorithm = "unknown";
        switch (key_type) {
            case LIBSSH2_HOSTKEY_TYPE_RSA: algorithm = "ssh-rsa"; break;
            case LIBSSH2_HOSTKEY_TYPE_DSS: algorithm = "ssh-dss"; break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: algorithm = "ecdsa-sha2-nistp256"; break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: algorithm = "ecdsa-sha2-nistp384"; break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: algorithm = "ecdsa-sha2-nistp521"; break;
            case LIBSSH2_HOSTKEY_TYPE_ED25519: algorithm = "ssh-ed25519"; break;
        }
        auto display = Base64(fingerprint, 32);
        while (display.ends_with('=')) display.pop_back();
        Post({SshEventKind::HostKey, "SHA256:" + display, algorithm, Base64(reinterpret_cast<const unsigned char*>(key), key_size)});
        {
            std::unique_lock lock(mutex_);
            while (!host_decision_ && !stop.stop_requested()) condition_.wait_for(lock, std::chrono::milliseconds(100));
            Cancelled(stop);
            if (!host_decision_.value()) throw std::runtime_error("主机身份未获信任，连接已取消");
        }
        Post({SshEventKind::State, "正在认证"});
        int authentication = 0;
        if (credential_.auth == AuthMethod::Password) {
            authentication = Retry(connection, stop, session_.timeout_seconds, [&] {
                return libssh2_userauth_password_ex(connection.session, credential_.username.data(), static_cast<unsigned int>(credential_.username.size()),
                    credential_.password.data(), static_cast<unsigned int>(credential_.password.size()), nullptr);
            });
        } else {
            authentication = Retry(connection, stop, session_.timeout_seconds, [&] {
                return libssh2_userauth_publickey_frommemory(connection.session, credential_.username.data(), credential_.username.size(), nullptr, 0,
                    credential_.private_key.data(), credential_.private_key.size(), credential_.passphrase.c_str());
            });
        }
        if (authentication) throw std::runtime_error("SSH 认证失败，请检查用户名、密码或私钥格式");
        Retry(connection, stop, session_.timeout_seconds, [&] {
            connection.channel = libssh2_channel_open_session(connection.session);
            return connection.channel ? 0 : libssh2_session_last_errno(connection.session);
        });
        if (!connection.channel) throw std::runtime_error("无法创建 SSH 通道");
        int rows = 24;
        int columns = 80;
        { const std::lock_guard lock(mutex_); rows = rows_; columns = columns_; }
        if (Retry(connection, stop, session_.timeout_seconds, [&] {
            return libssh2_channel_request_pty_ex(connection.channel, "xterm-256color", 14, nullptr, 0, columns, rows, 0, 0);
        })) throw std::runtime_error("服务器拒绝分配交互终端");
        if (Retry(connection, stop, session_.timeout_seconds, [&] { return libssh2_channel_shell(connection.channel); }))
            throw std::runtime_error("服务器拒绝启动 shell");
        libssh2_keepalive_config(connection.session, 1, 15);
        connected_ = true;
        Post({SshEventKind::State, "已连接"});
        std::array<char, 16384> buffer{};
        std::string pending;
        std::size_t sent = 0;
        auto keepalive_at = Clock::now() + std::chrono::seconds(15);
        while (!stop.stop_requested()) {
            bool resize = false;
            bool allow_read = true;
            { const std::lock_guard lock(mutex_);
                if (pending.empty()) { pending.swap(input_); sent = 0; }
                resize = resized_; resized_ = false; rows = rows_; columns = columns_;
                allow_read = queued_bytes_ < 2 * 1024 * 1024;
            }
            if (resize && Retry(connection, stop, session_.timeout_seconds, [&] {
                return libssh2_channel_request_pty_size(connection.channel, columns, rows);
            })) throw std::runtime_error("终端尺寸更新失败");
            if (!pending.empty()) {
                const auto count = libssh2_channel_write(connection.channel, pending.data() + sent, pending.size() - sent);
                if (count > 0) { sent += static_cast<std::size_t>(count); if (sent == pending.size()) pending.clear(); }
                else if (count != LIBSSH2_ERROR_EAGAIN && count < 0) throw std::runtime_error("SSH 输入发送失败");
            }
            bool received = false;
            if (allow_read) for (int stream = 0; stream < 2; ++stream) {
                const auto count = libssh2_channel_read_ex(connection.channel, stream, buffer.data(), buffer.size());
                if (count > 0) { Post({SshEventKind::Output, std::string(buffer.data(), static_cast<std::size_t>(count))}); received = true; }
                else if (count < 0 && count != LIBSSH2_ERROR_EAGAIN) throw std::runtime_error("SSH 连接读取失败");
            }
            if (libssh2_channel_eof(connection.channel)) break;
            if (Clock::now() >= keepalive_at) {
                int interval = 0;
                const auto result = libssh2_keepalive_send(connection.session, &interval);
                if (result && result != LIBSSH2_ERROR_EAGAIN) throw std::runtime_error("SSH 保活失败");
                keepalive_at = Clock::now() + std::chrono::seconds(15);
            }
            if (!allow_read && pending.empty()) {
                std::unique_lock lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stop.stop_requested() || queued_bytes_ < 2 * 1024 * 1024 || !input_.empty() || resized_; });
            } else if (!received) WaitSocket(connection.socket, true, !pending.empty());
        }
        connected_ = false;
        Post({SshEventKind::State, "已断开"});
    } catch (const std::exception& error) {
        connected_ = false;
        Post({SshEventKind::State, stop.stop_requested() ? "已断开" : error.what()});
    }
}
}
