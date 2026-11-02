#include "model.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace maple {
using Json = nlohmann::json;
namespace {
void CheckString(const std::string& value, std::size_t limit, bool required = false) {
    if (value.size() > limit || (required && value.empty()) || value.find('\0') != std::string::npos)
        throw std::runtime_error("配置字段为空、过长或包含无效字符");
}
void ExactFields(const Json& object, const std::set<std::string>& expected) {
    if (!object.is_object() || object.size() != expected.size()) throw std::runtime_error("配置字段不匹配");
    for (const auto& [name, value] : object.items())
        if (!expected.contains(name)) throw std::runtime_error("不支持的配置字段");
}
Json SessionJson(const Session& session) {
    return {{"id", session.id}, {"name", session.name}, {"group", session.group}, {"host", session.host},
        {"port", session.port}, {"username", session.username},
        {"auth", session.auth == AuthMethod::Password ? "password" : "private-key"},
        {"password", session.password}, {"private_key", session.private_key},
        {"passphrase", session.passphrase}, {"timeout_seconds", session.timeout_seconds}};
}
Json SessionsJson(const std::vector<Session>& sessions) {
    ValidateSessions(sessions);
    auto output = Json::array();
    for (const auto& session : sessions) output.push_back(SessionJson(session));
    return output;
}
std::vector<Session> SessionsFromJson(const Json& array) {
    if (!array.is_array() || array.size() > 1000) throw std::runtime_error("会话列表无效或超过 1000 条");
    std::vector<Session> sessions;
    for (const auto& entry : array) {
        ExactFields(entry, {"id", "name", "group", "host", "port", "username", "auth", "password", "private_key", "passphrase", "timeout_seconds"});
        Session session;
        session.id = entry.at("id").get<std::string>();
        session.name = entry.at("name").get<std::string>();
        session.group = entry.at("group").get<std::string>();
        session.host = entry.at("host").get<std::string>();
        const auto port = entry.at("port").get<std::int64_t>();
        if (!entry.at("port").is_number_integer() || port < 1 || port > 65535) throw std::runtime_error("端口须为 1–65535");
        session.port = static_cast<std::uint16_t>(port);
        session.username = entry.at("username").get<std::string>();
        const auto auth = entry.at("auth").get<std::string>();
        if (auth != "password" && auth != "private-key") throw std::runtime_error("不支持的认证方式");
        session.auth = auth == "password" ? AuthMethod::Password : AuthMethod::PrivateKey;
        session.password = entry.at("password").get<std::string>();
        session.private_key = entry.at("private_key").get<std::string>();
        session.passphrase = entry.at("passphrase").get<std::string>();
        if (!entry.at("timeout_seconds").is_number_integer()) throw std::runtime_error("超时值无效");
        const auto timeout = entry.at("timeout_seconds").get<std::int64_t>();
        if (timeout < 1 || timeout > 300) throw std::runtime_error("超时须为 1–300 秒");
        session.timeout_seconds = static_cast<int>(timeout);
        sessions.push_back(std::move(session));
    }
    ValidateSessions(sessions);
    return sessions;
}
Json Parse(std::string_view text) {
    if (text.size() > MaxFileBytes) throw std::runtime_error("配置超过 32 MiB");
    return Json::parse(text);
}
void CheckEnvelope(const Json& object, std::string_view type) {
    if (object.at("version") != 1 || object.at("type").get<std::string>() != type) throw std::runtime_error("不支持的配置格式或版本");
}
}
void ValidateSessions(const std::vector<Session>& sessions) {
    if (sessions.size() > 1000) throw std::runtime_error("最多支持 1000 条会话");
    std::set<std::string> identifiers;
    for (const auto& session : sessions) {
        CheckString(session.id, 128, true);
        CheckString(session.name, 256, true);
        CheckString(session.group, 256);
        CheckString(session.host, 1024, true);
        CheckString(session.username, 256, true);
        CheckString(session.password, 65536);
        CheckString(session.private_key, 1024 * 1024, session.auth == AuthMethod::PrivateKey);
        CheckString(session.passphrase, 65536);
        if (std::any_of(session.host.begin(), session.host.end(), [](unsigned char character) { return character <= 32 || character == 127; }))
            throw std::runtime_error("主机地址不能包含空白或控制字符");
        if (!session.port || session.timeout_seconds < 1 || session.timeout_seconds > 300 ||
            (session.auth != AuthMethod::Password && session.auth != AuthMethod::PrivateKey))
            throw std::runtime_error("连接参数无效");
        if (!identifiers.insert(session.id).second) throw std::runtime_error("会话标识重复");
    }
}
std::string SerializeVault(const Vault& vault) {
    CheckString(vault.webdav.url, 8192);
    CheckString(vault.webdav.username, 1024);
    CheckString(vault.webdav.password, 65536);
    CheckString(vault.webdav.directory, 1024);
    const auto output = Json{{"version", 1}, {"type", "maplessh-vault"}, {"sessions", SessionsJson(vault.sessions)},
        {"webdav", {{"url", vault.webdav.url}, {"username", vault.webdav.username}, {"password", vault.webdav.password}, {"directory", vault.webdav.directory}}}}.dump();
    if (output.size() > MaxFileBytes - 64) throw std::runtime_error("配置超过大小限制");
    return output;
}
Vault ParseVault(std::string_view text) {
    const auto object = Parse(text);
    ExactFields(object, {"version", "type", "sessions", "webdav"});
    CheckEnvelope(object, "maplessh-vault");
    ExactFields(object.at("webdav"), {"url", "username", "password", "directory"});
    Vault vault;
    vault.sessions = SessionsFromJson(object.at("sessions"));
    const auto& settings = object.at("webdav");
    vault.webdav = {settings.at("url").get<std::string>(), settings.at("username").get<std::string>(),
        settings.at("password").get<std::string>(), settings.at("directory").get<std::string>()};
    SerializeVault(vault);
    return vault;
}
std::string SerializeBackup(const std::vector<Session>& sessions) {
    const auto output = Json{{"version", 1}, {"type", "maplessh-backup"}, {"sessions", SessionsJson(sessions)}}.dump();
    if (output.size() > MaxFileBytes - 64) throw std::runtime_error("备份超过大小限制");
    return output;
}
std::vector<Session> ParseBackup(std::string_view text) {
    const auto object = Parse(text);
    ExactFields(object, {"version", "type", "sessions"});
    CheckEnvelope(object, "maplessh-backup");
    return SessionsFromJson(object.at("sessions"));
}
}
