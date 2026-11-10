#include "ssh_transport.hpp"
#include <openssl/evp.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
namespace maple::ssh {
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
Connection::~Connection() {
    if (socket != INVALID_SOCKET) { shutdown(socket, SD_BOTH); closesocket(socket); }
    if (channel) libssh2_channel_free(channel);
    if (session) libssh2_session_free(session);
}
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

void Authenticate(Connection& connection, const Credential& credential, int timeout, std::stop_token stop) {
        int authentication = 0;
        if (credential.auth == AuthMethod::Password) {
            authentication = Retry(connection, stop, timeout, [&] {
                return libssh2_userauth_password_ex(connection.session, credential.username.data(), static_cast<unsigned int>(credential.username.size()),
                    credential.password.data(), static_cast<unsigned int>(credential.password.size()), nullptr);
            });
        } else {
            authentication = Retry(connection, stop, timeout, [&] {
                return libssh2_userauth_publickey_frommemory(connection.session, credential.username.data(), credential.username.size(), nullptr, 0,
                    credential.private_key.data(), credential.private_key.size(), credential.passphrase.c_str());
            });
        }
        if (authentication) throw std::runtime_error("SSH 认证失败，请检查用户名、密码或私钥格式");
}
}
