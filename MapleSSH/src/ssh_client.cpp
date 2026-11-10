#include "ssh_client.hpp"
#include "ssh_transport.hpp"
#include "sftp_client.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace maple {
using namespace ssh;
using Clock = std::chrono::steady_clock;
SshClient::SshClient(Session session, Credential credential) : session_(std::move(session)), credential_(std::move(credential)) { Initialize(); }
SshClient::~SshClient() { Stop(); }
void SshClient::Start(int rows, int columns) {
    Stop();
    { const std::lock_guard lock(mutex_); host_decision_.reset(); input_.clear(); events_.clear(); output_history_.clear(); queued_bytes_ = 0; rows_ = std::clamp(rows, 2, 300); columns_ = std::clamp(columns, 2, 500); resized_ = true; }
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
void SshClient::Stop() {
    connected_ = false;
    if (worker_.joinable()) { worker_.request_stop(); condition_.notify_all(); worker_.join(); }
    StopFiles();
    { const std::lock_guard lock(mutex_); files_.reset(); pinned_key_.clear(); }
}
void SshClient::StopFiles() {
    std::shared_ptr<SftpClient> files;
    { const std::lock_guard lock(mutex_); files = files_; }
    if (files) files->Stop();
}
std::shared_ptr<SftpClient> SshClient::Files() {
    const std::lock_guard lock(mutex_);
    if (!connected_ || pinned_key_.empty()) throw std::runtime_error("请先连接 SSH 并完成主机身份验证");
    if (!files_) files_ = std::make_shared<SftpClient>(session_, credential_, pinned_key_);
    return files_;
}
bool SshClient::Send(std::string text) {
    const std::lock_guard lock(mutex_);
    if (!connected_) return false;
    if (input_.size() + text.size() > 1024 * 1024) throw std::runtime_error("待发送输入过多，请稍后重试");
    input_ += text;
    return true;
}
std::string SshClient::OutputSnapshot(std::size_t max_bytes) const {
    const std::lock_guard lock(mutex_);
    const auto start = output_history_.size() > max_bytes ? output_history_.size() - max_bytes : 0;
    return output_history_.substr(start);
}
void SshClient::Resize(int rows, int columns) {
    const std::lock_guard lock(mutex_);
    rows_ = std::clamp(rows, 2, 300);
    columns_ = std::clamp(columns, 2, 500);
    resized_ = true;
}
void SshClient::Post(SshEvent event) {
    const std::lock_guard lock(mutex_);
    if (event.kind == SshEventKind::Output) {
        output_history_ += event.text;
        if (output_history_.size() > 4 * 1024 * 1024) output_history_.erase(0, output_history_.size() - 4 * 1024 * 1024);
    }
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
        Authenticate(connection, credential_, session_.timeout_seconds, stop);
        { const std::lock_guard lock(mutex_); pinned_key_.assign(key, key_size); }
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
    StopFiles();
}
}
