#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace maple {
inline constexpr std::size_t MaxFileBytes = 32 * 1024 * 1024;
enum class AuthMethod { Password, PrivateKey };
struct Session {
    std::string id;
    std::string name;
    std::string group;
    std::string host;
    std::uint16_t port = 22;
    std::string username;
    AuthMethod auth = AuthMethod::Password;
    std::string password;
    std::string private_key;
    std::string passphrase;
    int timeout_seconds = 15;
};
struct WebDavSettings {
    std::string url;
    std::string username;
    std::string password;
    std::string directory = "MapleSSH";
};
struct Vault {
    std::vector<Session> sessions;
    WebDavSettings webdav;
};
void ValidateSessions(const std::vector<Session>& sessions);
std::string SerializeVault(const Vault& vault);
Vault ParseVault(std::string_view text);
std::string SerializeBackup(const std::vector<Session>& sessions);
std::vector<Session> ParseBackup(std::string_view text);
}
