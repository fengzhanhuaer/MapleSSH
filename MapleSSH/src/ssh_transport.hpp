#pragma once
#include "model.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#include <functional>
#include <stop_token>
#include <string>
namespace maple::ssh {
struct Connection {
    SOCKET socket = INVALID_SOCKET;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_CHANNEL* channel = nullptr;
    ~Connection();
};
void Initialize();
void Cancelled(std::stop_token stop);
std::string Base64(const unsigned char* bytes, std::size_t size);
void WaitSocket(SOCKET socket, bool read, bool write);
void Connect(Connection& connection, const Session& settings, std::stop_token stop);
int Retry(Connection& connection, std::stop_token stop, int timeout, const std::function<int()>& operation);
void Authenticate(Connection& connection, const Credential& credential, int timeout, std::stop_token stop);
}
