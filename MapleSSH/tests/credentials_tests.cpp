#include "model.hpp"
#include "storage.hpp"
#include "crypto.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

using namespace maple;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Callable> void Rejects(Callable action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid catalog accepted");
}
int main() {
    try {
        Credential credential;
        credential.id = "shared"; credential.name = "服务器管理员"; credential.username = "root"; credential.password = "shared-secret";
        Session first; first.id = "one"; first.name = "服务器一"; first.group = "云主机"; first.host = "server-one.test"; first.credential_id = credential.id;
        auto second = first; second.id = "two"; second.name = "服务器二"; second.host = "server-two.test";
        Vault vault; vault.credentials = {credential}; vault.sessions = {first, second};
        const auto saved = SerializeVault(vault);
        const auto object = nlohmann::json::parse(saved);
        Require(object.at("version") == 2 && object.at("credentials").size() == 1, "credentials not stored independently");
        Require(!object.at("sessions").at(0).contains("password"), "session duplicated credential contents");
        auto reopened = ParseVault(saved);
        reopened.credentials.at(0).password = "updated-secret";
        Require(FindCredential(reopened, reopened.sessions.at(0).credential_id).password == "updated-secret", "first host missed credential change");
        Require(FindCredential(reopened, reopened.sessions.at(1).credential_id).password == "updated-secret", "second host missed credential change");
        Require(SerializeVault(ParseVault(SerializeVault(reopened))) == SerializeVault(reopened), "catalog round trip changed data");
        auto missing = vault; missing.credentials.clear(); Rejects([&] { SerializeVault(missing); });
        auto duplicate = vault; duplicate.credentials.push_back(credential); Rejects([&] { SerializeVault(duplicate); });
        Rejects([&] { RemoveCredential(vault, credential.id); });
        Require(vault.credentials.size() == 1, "in-use credential was deleted");
        auto unused = credential; unused.id = "unused"; vault.credentials.push_back(unused); RemoveCredential(vault, unused.id);
        Vault deletion; deletion.credentials = {unused, credential};
        RemoveCredential(deletion, deletion.credentials.front().id);
        Require(deletion.credentials.size() == 1 && deletion.credentials.front().id == credential.id, "deleting by catalog reference corrupted the remaining credential");
        const auto renamed = RenameGroup(vault, "云主机", "生产环境");
        Require(renamed.sessions.at(0).group == "生产环境" && renamed.sessions.at(1).group == "生产环境", "group rename did not update all hosts");
        Require(renamed.sessions.at(0).credential_id == credential.id && renamed.credentials.at(0).password == credential.password, "rename changed credentials");
        Rejects([&] { RenameGroup(vault, "云主机", ""); });
        auto conflict = vault; conflict.sessions.at(1).group = "现有分组";
        Rejects([&] { RenameGroup(conflict, "云主机", "现有分组"); });
        auto ungrouped = vault; for (auto& session : ungrouped.sessions) session.group.clear();
        Require(RenameGroup(ungrouped, "", "已分组").sessions.at(1).group == "已分组", "default group cannot be renamed");
        vault.webdav = {"https://dav.test/dav/", "dav-user", "dav-secret", "MapleSSH"};
        const auto backup = SerializeBackup(vault);
        Require(backup.find("dav-secret") == std::string::npos, "WebDAV secret entered backup");
        const auto restored = ParseBackup(backup);
        Require(restored.credentials.size() == 1 && restored.sessions.size() == 2, "backup lost shared catalog");
        Require(restored.sessions.at(0).credential_id == restored.sessions.at(1).credential_id, "backup broke credential sharing");
        const auto old_session = nlohmann::json{{"id", "one"}, {"name", "旧主机"}, {"group", "旧组"}, {"host", "old.test"}, {"port", 22}, {"username", "root"}, {"auth", "private-key"}, {"password", ""}, {"private_key", "old-private-key"}, {"passphrase", "old-key-passphrase"}, {"timeout_seconds", 15}};
        auto old_second = old_session; old_second["id"] = "two"; old_second["host"] = "other.test";
        const auto legacy = nlohmann::json{{"version", 1}, {"type", "maplessh-backup"}, {"sessions", nlohmann::json::array({old_session, old_second})}};
        const auto migrated = ParseBackup(legacy.dump());
        Require(migrated.credentials.size() == 1 && migrated.sessions.size() == 2, "identical legacy credentials were not reused");
        Require(migrated.credentials.at(0).private_key == "old-private-key" && migrated.credentials.at(0).passphrase == "old-key-passphrase", "legacy migration lost credentials");
        Require(migrated.sessions.at(0).credential_id == migrated.sessions.at(1).credential_id, "legacy hosts do not share migrated credential");
        auto local = legacy; local["type"] = "maplessh-vault"; local["webdav"] = {{"url", vault.webdav.url}, {"username", vault.webdav.username}, {"password", vault.webdav.password}, {"directory", vault.webdav.directory}};
        Require(ParseVault(local.dump()).webdav.password == "dav-secret", "legacy migration lost WebDAV settings");
        auto different = legacy; different["sessions"][1]["passphrase"] = "different-passphrase";
        Require(ParseBackup(different.dump()).credentials.size() == 2, "different legacy credentials were incorrectly combined");
        const auto directory = std::filesystem::temp_directory_path() / ("maplessh-migration-" + RandomId());
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
        Storage storage(directory); const auto legacy_text = local.dump(); const Bytes legacy_bytes(legacy_text.begin(), legacy_text.end());
        WriteAtomic(directory / "vault.json", legacy_bytes); const auto loaded = storage.Load(); storage.Save(loaded);
        bool migration_copy = false;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().filename().string().starts_with("vault.before-credentials.")) {
                Require(ReadBytes(entry.path()) == legacy_bytes, "migration backup changed legacy bytes"); migration_copy = true;
            }
        }
        Require(migration_copy, "credential migration did not preserve the old configuration");
        Require(storage.Load().credentials.size() == 1, "migrated configuration cannot be reopened");
        std::cout << "Shared credentials, migration, backups, deletion guard and group rename PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
