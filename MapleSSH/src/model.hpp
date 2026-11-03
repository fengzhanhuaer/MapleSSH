#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace maple {
inline constexpr std::size_t MaxFileBytes = 32 * 1024 * 1024;
enum class AuthMethod { Password, PrivateKey };
struct Credential {
    std::string id;
    std::string name;
    std::string username;
    AuthMethod auth = AuthMethod::Password;
    std::string password;
    std::string private_key;
    std::string passphrase;
};
struct Session {
    std::string id;
    std::string name;
    std::string group;
    std::string host;
    std::uint16_t port = 22;
    std::string credential_id;
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
    std::vector<Credential> credentials;
    WebDavSettings webdav;
};
void ValidateSessions(const std::vector<Session>& sessions);
void ValidateCredentials(const std::vector<Credential>& credentials);
void ValidateCatalog(const Vault& vault);
const Credential& FindCredential(const Vault& vault, std::string_view id);
void RemoveCredential(Vault& vault, std::string_view id);
Vault RenameGroup(const Vault& vault, std::string_view previous, std::string_view updated);
std::string SerializeVault(const Vault& vault);
Vault ParseVault(std::string_view text);
std::string SerializeBackup(const Vault& vault);
Vault ParseBackup(std::string_view text);
}
