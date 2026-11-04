#include "model.hpp"
#include "storage.hpp"
#include "crypto.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace maple;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Callable> void Rejects(Callable action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "invalid shortcut accepted");
}
}
int main() {
    try {
        Vault vault;
        vault.quick_commands = {{"shortcut-1", "查看目录", "pwd\r\nls -la"}, {"shortcut-2", "中文输出", "printf '中文'"}};
        vault.webdav = {"https://example.test/dav/", "local-account", "local-secret", "MapleSSH"};
        const auto local = SerializeVault(vault);
        Require(nlohmann::json::parse(local).at("version") == 3, "shortcut schema version missing");
        Require(ParseVault(local).quick_commands.at(0).command == vault.quick_commands.at(0).command, "saved shortcut changed");
        Require(SerializeVault(ParseVault(local)) == local, "shortcut roundtrip unstable");
        const auto backup = SerializeBackup(vault);
        const auto restored = ParseBackup(backup);
        Require(restored.quick_commands.size() == 2 && restored.quick_commands[1].name == "中文输出", "backup lost shortcuts or order");
        Require(restored.quick_commands[0].id == "shortcut-1" && restored.quick_commands[0].command == "pwd\r\nls -la", "backup shortcut content changed");
        Require(backup.find("local-secret") == std::string::npos && backup.find("known_hosts") == std::string::npos && backup.find("webdav") == std::string::npos, "backup boundary expanded");
        auto invalid = vault; invalid.quick_commands.push_back(invalid.quick_commands.front());
        Rejects([&] { SerializeVault(invalid); });
        invalid = vault; invalid.quick_commands.front().name = " \t";
        Rejects([&] { SerializeVault(invalid); });
        invalid = vault; invalid.quick_commands.front().command = "\r\n";
        Rejects([&] { SerializeVault(invalid); });
        invalid = vault; invalid.quick_commands.front().command = "echo\x1b[A";
        Rejects([&] { SerializeVault(invalid); });
        invalid = vault; invalid.quick_commands.front().command.assign(1024 * 1024, 'x');
        Rejects([&] { SerializeVault(invalid); });
        invalid = vault; invalid.quick_commands.resize(201, invalid.quick_commands.front());
        Rejects([&] { SerializeVault(invalid); });
        auto object = nlohmann::json::parse(backup); object["quick_commands"][0]["extra"] = true;
        Rejects([&] { ParseBackup(object.dump()); });
        object = nlohmann::json::parse(backup); object.erase("quick_commands");
        Rejects([&] { ParseBackup(object.dump()); });
        object["version"] = 2;
        Require(ParseBackup(object.dump()).quick_commands.empty(), "v2 backup failed migration");
        const auto legacy = nlohmann::json{{"version", 1}, {"type", "maplessh-backup"}, {"sessions", nlohmann::json::array()}};
        Require(ParseBackup(legacy.dump()).quick_commands.empty(), "v1 backup failed migration");
        auto oldVault = nlohmann::json::parse(local); oldVault["version"] = 2; oldVault.erase("quick_commands");
        const auto original = oldVault.dump();
        const auto directory = std::filesystem::temp_directory_path() / ("maplessh-shortcut-test-" + RandomId());
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
        WriteAtomic(directory / "vault.json", Bytes(original.begin(), original.end()));
        Storage storage(directory);
        auto migrated = storage.Load();
        Require(migrated.quick_commands.empty(), "v2 local vault migration failed");
        migrated.quick_commands = vault.quick_commands; storage.Save(migrated);
        Require(storage.Load().quick_commands.size() == 2, "shortcut storage reload failed");
        std::size_t copies = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().filename().string().starts_with("vault.before-quick-commands.")) {
                const auto content = ReadBytes(entry.path());
                Require(std::string(content.begin(), content.end()) == original, "migration did not preserve original bytes"); ++copies;
            }
        }
        Require(copies == 1, "v2 migration snapshot missing");
        storage.Save(migrated);
        std::size_t after = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.path().filename().string().starts_with("vault.before-quick-commands.")) ++after;
        Require(after == 1, "unchanged v3 save repeated migration snapshot");
        std::cout << "Shortcut persistence, backup boundaries, validation and legacy migration PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
