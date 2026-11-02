#include "model.hpp"
#include "crypto.hpp"
#include "storage.hpp"
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace maple;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Callable> void Rejects(Callable action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "invalid operation accepted");
}

Session Example() {
    Session session;
    session.id = "test-session";
    session.name = "开发板";
    session.group = "实验室";
    session.host = "172.11.110.100";
    session.port = 35520;
    session.username = "ies";
    session.password = "ssh-password-sentinel";
    return session;
}

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> tests{
        {"model_roundtrip", [] {
            Vault vault;
            vault.sessions.push_back(Example());
            vault.webdav = {"https://example.test/dav/", "webdav-user", "webdav-secret", "MapleSSH"};
            const auto restored = ParseVault(SerializeVault(vault));
            Require(restored.sessions.at(0).name == "开发板", "unicode session name lost");
            Require(restored.sessions.at(0).port == 35520, "port lost");
            Require(restored.webdav.password == "webdav-secret", "local credential lost");
        }},
        {"backup_whitelist", [] {
            Vault vault;
            auto session = Example();
            session.auth = AuthMethod::PrivateKey;
            session.private_key = "private-key-content-sentinel";
            session.passphrase = "key-passphrase-sentinel";
            vault.sessions.push_back(session);
            vault.webdav = {"https://example.test/dav/", "webdav-user-sentinel", "webdav-secret-sentinel", "MapleSSH"};
            const auto payload = SerializeBackup(vault.sessions);
            Require(payload.find(session.private_key) != std::string::npos, "private key content omitted");
            Require(payload.find(session.passphrase) != std::string::npos, "key passphrase omitted");
            Require(payload.find("webdav") == std::string::npos, "backup includes WebDAV settings");
            Require(payload.find("fingerprint") == std::string::npos, "backup includes fingerprint");
            Require(payload.find("known_hosts") == std::string::npos, "backup includes host trust");
            Require(ParseBackup(payload).at(0).private_key == session.private_key, "backup not restorable");
        }},
        {"invalid_models", [] {
            auto session = Example();
            auto invalid = session;
            invalid.host = "";
            Rejects([&] { SerializeBackup({invalid}); });
            invalid = session;
            invalid.port = 0;
            Rejects([&] { SerializeBackup({invalid}); });
            invalid = session;
            invalid.timeout_seconds = 0;
            Rejects([&] { SerializeBackup({invalid}); });
            Rejects([&] { SerializeBackup({session, session}); });
            Rejects([] { ParseBackup("{not-json}"); });
            Rejects([] { ParseBackup("{\"version\":99,\"sessions\":[]}"); });
            Rejects([] { ParseBackup("{\"version\":1,\"type\":\"maplessh-backup\",\"sessions\":[],\"known_hosts\":[]}"); });
        }},
        {"crypto_roundtrip", [] {
            const auto encrypted = Encrypt("sensitive-private-key", "master-password", "backup");
            Require(Decrypt(encrypted, "master-password", "backup") == "sensitive-private-key", "decrypt failed");
            Require(Encrypt("sensitive-private-key", "master-password", "backup") != encrypted, "encryption reused randomness");
            Rejects([&] { Decrypt(encrypted, "master-password", "vault"); });
        }},
        {"crypto_rejects_damage", [] {
            const auto encrypted = Encrypt("secret", "master-password", "vault");
            Rejects([&] { Decrypt(encrypted, "wrong-password", "vault"); });
            auto damaged = encrypted;
            damaged.at(10) ^= 1;
            Rejects([&] { Decrypt(damaged, "master-password", "vault"); });
            damaged = encrypted;
            damaged.back() ^= 1;
            Rejects([&] { Decrypt(damaged, "master-password", "vault"); });
            damaged.resize(20);
            Rejects([&] { Decrypt(damaged, "master-password", "vault"); });
            Rejects([] { Encrypt("secret", "", "vault"); });
        }},
        {"atomic_storage", [] {
            const auto directory = std::filesystem::temp_directory_path() / ("maplessh-test-" + RandomId());
            std::filesystem::create_directories(directory);
            struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
            Storage storage(directory);
            Vault vault;
            vault.sessions.push_back(Example());
            storage.Save(vault);
            Require(storage.Load().sessions.size() == 1, "saved vault unreadable");
            const auto original = ReadBytes(directory / "vault.json");
            storage.SaveRecoveryCopy();
            bool recovery_found = false;
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (entry.path().filename().string().starts_with("vault.before-restore.")) {
                    Require(ReadBytes(entry.path()) == original, "recovery copy changed vault");
                    recovery_found = true;
                }
            }
            Require(recovery_found, "recovery copy missing");
            auto invalid = vault;
            invalid.sessions.push_back(Example());
            Rejects([&] { storage.Save(invalid); });
            Require(ReadBytes(directory / "vault.json") == original, "failed save changed vault");
        }},
        {"passwordless_portability", [] {
            const auto directory = std::filesystem::temp_directory_path() / ("maplessh-test-" + RandomId());
            struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
            Storage source(directory / "source");
            Require(!source.Exists(), "new storage is not empty");
            Vault vault;
            vault.sessions.push_back(Example());
            vault.webdav = {"https://example.test/dav/", "webdav-user", "webdav-secret", "MapleSSH"};
            source.Save(vault);
            Require(Storage(source.Directory()).Load().sessions.at(0).password == vault.sessions.at(0).password, "passwordless restart failed");
            Storage destination(directory / "destination");
            std::filesystem::copy_file(source.Directory() / "vault.json", destination.Directory() / "vault.json");
            const auto restored = destination.Load();
            Require(restored.sessions.at(0).name == vault.sessions.at(0).name, "portable session lost");
            Require(restored.webdav.password == vault.webdav.password, "portable WebDAV credentials lost");
            const Bytes damaged{123};
            WriteAtomic(destination.Directory() / "vault.json", damaged);
            Rejects([&] { destination.Load(); });
            Require(ReadBytes(destination.Directory() / "vault.json") == damaged, "failed load replaced damaged configuration");
        }},
        {"legacy_storage_preserved", [] {
            const auto directory = std::filesystem::temp_directory_path() / ("maplessh-test-" + RandomId());
            Storage storage(directory);
            struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
            const auto original = Encrypt(SerializeVault(Vault{}), "legacy-password", "vault");
            WriteAtomic(directory / "vault.msv", original);
            Require(storage.Exists(), "legacy configuration ignored");
            Rejects([&] { storage.Load(); });
            Rejects([&] { storage.Save(Vault{}); });
            Require(!std::filesystem::exists(directory / "vault.json"), "legacy configuration shadowed");
            Require(ReadBytes(directory / "vault.msv") == original, "legacy configuration modified");
        }}
    };
    try {
        if (argc != 2 || !tests.contains(argv[1])) throw std::runtime_error("select a test case");
        tests.at(argv[1])();
        std::cout << argv[1] << " PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
