#include "model.hpp"
#include "crypto.hpp"
#include "command_input.hpp"
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
Json CredentialJson(const Credential& credential) {
    return {{"id", credential.id}, {"name", credential.name}, {"username", credential.username},
        {"auth", credential.auth == AuthMethod::Password ? "password" : "private-key"},
        {"password", credential.password}, {"private_key", credential.private_key}, {"passphrase", credential.passphrase}};
}
Credential ReadCredential(const Json& entry) {
    Credential credential;
    credential.username = entry.at("username").get<std::string>();
    const auto auth = entry.at("auth").get<std::string>();
    if (auth != "password" && auth != "private-key") throw std::runtime_error("不支持的认证方式");
    credential.auth = auth == "password" ? AuthMethod::Password : AuthMethod::PrivateKey;
    credential.password = entry.at("password").get<std::string>();
    credential.private_key = entry.at("private_key").get<std::string>();
    credential.passphrase = entry.at("passphrase").get<std::string>();
    return credential;
}
bool SameCredential(const Credential& first, const Credential& second) {
    return first.username == second.username && first.auth == second.auth && first.password == second.password &&
        first.private_key == second.private_key && first.passphrase == second.passphrase;
}
Json CatalogJson(const Vault& vault, std::string_view type) {
    ValidateCatalog(vault);
    auto sessions = Json::array(); auto credentials = Json::array();
    for (const auto& session : vault.sessions)
        sessions.push_back({{"id", session.id}, {"name", session.name}, {"group", session.group}, {"host", session.host},
            {"port", session.port}, {"credential_id", session.credential_id}, {"timeout_seconds", session.timeout_seconds}});
    for (const auto& credential : vault.credentials) credentials.push_back(CredentialJson(credential));
    auto commands = Json::array();
    for (const auto& command : vault.quick_commands) commands.push_back({{"id", command.id}, {"name", command.name}, {"command", command.command}});
    return {{"version", 3}, {"type", std::string(type)}, {"sessions", std::move(sessions)}, {"credentials", std::move(credentials)}, {"quick_commands", std::move(commands)}};
}
Json Parse(std::string_view text) {
    if (text.size() > MaxFileBytes) throw std::runtime_error("配置超过 32 MiB");
    return Json::parse(text);
}
Vault ReadCatalog(const Json& object, std::string_view type, bool local) {
    if (!object.at("version").is_number_integer() || (object.at("version") != 1 && object.at("version") != 2 && object.at("version") != 3) ||
        object.at("type").get<std::string>() != type) throw std::runtime_error("不支持的配置格式或版本");
    const bool legacy = object.at("version") == 1;
    std::set<std::string> fields{"version", "type", "sessions"};
    if (!legacy) fields.insert("credentials");
    if (object.at("version") == 3) fields.insert("quick_commands");
    if (local) fields.insert("webdav");
    ExactFields(object, fields);
    Vault vault;
    if (object.at("version") == 3) {
        const auto& commands = object.at("quick_commands");
        if (!commands.is_array() || commands.size() > 200) throw std::runtime_error("快捷命令列表无效或超过 200 条");
        for (const auto& entry : commands) {
            ExactFields(entry, {"id", "name", "command"});
            vault.quick_commands.push_back({entry.at("id").get<std::string>(), entry.at("name").get<std::string>(), entry.at("command").get<std::string>()});
        }
    }
    if (!legacy) {
        const auto& credentials = object.at("credentials");
        if (!credentials.is_array() || credentials.size() > 1000) throw std::runtime_error("凭据列表无效或超过 1000 条");
        for (const auto& entry : credentials) {
            ExactFields(entry, {"id", "name", "username", "auth", "password", "private_key", "passphrase"});
            auto credential = ReadCredential(entry);
            credential.id = entry.at("id").get<std::string>(); credential.name = entry.at("name").get<std::string>();
            vault.credentials.push_back(std::move(credential));
        }
    }
    const auto& sessions = object.at("sessions");
    if (!sessions.is_array() || sessions.size() > 1000) throw std::runtime_error("会话列表无效或超过 1000 条");
    for (const auto& entry : sessions) {
        if (legacy) ExactFields(entry, {"id", "name", "group", "host", "port", "username", "auth", "password", "private_key", "passphrase", "timeout_seconds"});
        else ExactFields(entry, {"id", "name", "group", "host", "port", "credential_id", "timeout_seconds"});
        Session session;
        session.id = entry.at("id").get<std::string>(); session.name = entry.at("name").get<std::string>();
        session.group = entry.at("group").get<std::string>(); session.host = entry.at("host").get<std::string>();
        if (!entry.at("port").is_number_integer()) throw std::runtime_error("端口须为 1–65535");
        const auto port = entry.at("port").get<std::int64_t>();
        if (port < 1 || port > 65535) throw std::runtime_error("端口须为 1–65535");
        session.port = static_cast<std::uint16_t>(port);
        if (!entry.at("timeout_seconds").is_number_integer()) throw std::runtime_error("超时值无效");
        const auto timeout = entry.at("timeout_seconds").get<std::int64_t>();
        if (timeout < 1 || timeout > 300) throw std::runtime_error("超时须为 1–300 秒");
        session.timeout_seconds = static_cast<int>(timeout);
        if (legacy) {
            auto credential = ReadCredential(entry);
            const auto found = std::find_if(vault.credentials.begin(), vault.credentials.end(), [&](const auto& existing) { return SameCredential(existing, credential); });
            if (found == vault.credentials.end()) {
                credential.id = RandomId(); credential.name = session.name + " 凭据";
                if (credential.name.size() > 256) credential.name = credential.username + " 凭据";
                if (credential.name.size() > 256) credential.name = "导入凭据 " + std::to_string(vault.credentials.size() + 1);
                session.credential_id = credential.id; vault.credentials.push_back(std::move(credential));
            } else session.credential_id = found->id;
        } else session.credential_id = entry.at("credential_id").get<std::string>();
        vault.sessions.push_back(std::move(session));
    }
    ValidateCatalog(vault);
    return vault;
}
std::string Dump(const Json& object) {
    const auto output = object.dump();
    if (output.size() > MaxFileBytes - 64) throw std::runtime_error("配置超过 32 MiB");
    return output;
}
}
void ValidateSessions(const std::vector<Session>& sessions) {
    if (sessions.size() > 1000) throw std::runtime_error("最多支持 1000 条会话");
    std::set<std::string> identifiers;
    for (const auto& session : sessions) {
        CheckString(session.id, 128, true); CheckString(session.name, 256, true); CheckString(session.group, 256);
        CheckString(session.host, 1024, true); CheckString(session.credential_id, 128, true);
        if (std::any_of(session.host.begin(), session.host.end(), [](unsigned char character) { return character <= 32 || character == 127; }))
            throw std::runtime_error("主机地址不能包含空白或控制字符");
        if (!session.port || session.timeout_seconds < 1 || session.timeout_seconds > 300) throw std::runtime_error("连接参数无效");
        if (!identifiers.insert(session.id).second) throw std::runtime_error("会话标识重复");
    }
}
void ValidateCredentials(const std::vector<Credential>& credentials) {
    if (credentials.size() > 1000) throw std::runtime_error("最多支持 1000 条凭据");
    std::set<std::string> identifiers;
    for (const auto& credential : credentials) {
        CheckString(credential.id, 128, true); CheckString(credential.name, 256, true); CheckString(credential.username, 256, true);
        CheckString(credential.password, 65536); CheckString(credential.private_key, 1024 * 1024, credential.auth == AuthMethod::PrivateKey); CheckString(credential.passphrase, 65536);
        if (credential.auth != AuthMethod::Password && credential.auth != AuthMethod::PrivateKey) throw std::runtime_error("不支持的认证方式");
        if (!identifiers.insert(credential.id).second) throw std::runtime_error("凭据标识重复");
    }
}
const Credential& FindCredential(const Vault& vault, std::string_view id) {
    const auto found = std::find_if(vault.credentials.begin(), vault.credentials.end(), [&](const auto& credential) { return credential.id == id; });
    if (found == vault.credentials.end()) throw std::runtime_error("会话引用的凭据不存在，请重新选择凭据");
    return *found;
}
void ValidateCatalog(const Vault& vault) {
    ValidateSessions(vault.sessions); ValidateCredentials(vault.credentials);
    ValidateQuickCommands(vault.quick_commands);
    for (const auto& session : vault.sessions) FindCredential(vault, session.credential_id);
}
void ValidateQuickCommands(const std::vector<QuickCommand>& commands) {
    if (commands.size() > 200) throw std::runtime_error("最多支持 200 条快捷命令");
    std::set<std::string> identifiers;
    for (const auto& command : commands) {
        CheckString(command.id, 128, true); CheckString(command.name, 128, true);
        if (command.name.find_first_not_of(" \t\r\n") == std::string::npos) throw std::runtime_error("快捷命令名称不能为空");
        if (std::any_of(command.name.begin(), command.name.end(), [](unsigned char character) { return character < 32 || character == 127; }))
            throw std::runtime_error("快捷命令名称不能包含控制字符");
        PrepareCommands(command.command);
        if (!identifiers.insert(command.id).second) throw std::runtime_error("快捷命令标识重复");
    }
}
void RemoveCredential(Vault& vault, std::string_view id) {
    const std::string identifier(id);
    if (std::any_of(vault.sessions.begin(), vault.sessions.end(), [&](const auto& session) { return session.credential_id == identifier; }))
        throw std::runtime_error("该凭据仍被主机使用，请先为相关主机选择其他凭据");
    FindCredential(vault, identifier);
    std::erase_if(vault.credentials, [&](const auto& credential) { return credential.id == identifier; });
}
Vault RenameGroup(const Vault& vault, std::string_view previous, std::string_view updated) {
    CheckString(std::string(updated), 256, true);
    if (updated.find_first_not_of(" \t\r\n") == std::string_view::npos) throw std::runtime_error("分组名称不能为空");
    if (previous != updated && std::any_of(vault.sessions.begin(), vault.sessions.end(), [&](const auto& session) { return session.group == updated; }))
        throw std::runtime_error("已存在同名分组，请使用其他名称");
    auto result = vault; bool found = false;
    for (auto& session : result.sessions) if (session.group == previous) { session.group = updated; found = true; }
    if (!found) throw std::runtime_error("分组不存在");
    ValidateCatalog(result); return result;
}
std::string SerializeVault(const Vault& vault) {
    CheckString(vault.webdav.url, 8192); CheckString(vault.webdav.username, 1024); CheckString(vault.webdav.password, 65536); CheckString(vault.webdav.directory, 1024);
    auto object = CatalogJson(vault, "maplessh-vault");
    object["webdav"] = {{"url", vault.webdav.url}, {"username", vault.webdav.username}, {"password", vault.webdav.password}, {"directory", vault.webdav.directory}};
    return Dump(object);
}
Vault ParseVault(std::string_view text) {
    const auto object = Parse(text); auto vault = ReadCatalog(object, "maplessh-vault", true);
    const auto& settings = object.at("webdav"); ExactFields(settings, {"url", "username", "password", "directory"});
    vault.webdav = {settings.at("url").get<std::string>(), settings.at("username").get<std::string>(), settings.at("password").get<std::string>(), settings.at("directory").get<std::string>()};
    SerializeVault(vault); return vault;
}
std::string SerializeBackup(const Vault& vault) { return Dump(CatalogJson(vault, "maplessh-backup")); }
Vault ParseBackup(std::string_view text) { return ReadCatalog(Parse(text), "maplessh-backup", false); }
}
