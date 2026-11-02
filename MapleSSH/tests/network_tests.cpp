#include "webdav.hpp"
#include "host_keys.hpp"
#include "crypto.hpp"
#include "storage.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace maple;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Callable> void Rejects(Callable action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "unsafe input accepted");
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("maplessh-host-test-" + RandomId());
    try {
        WebDavSettings settings{"https://example.test/dav/", "user", "password", "MapleSSH"};
        auto endpoint = ParseEndpoint(settings);
        Require(endpoint.secure && endpoint.port == 443, "HTTPS endpoint invalid");
        Require(endpoint.path == L"/dav/MapleSSH/", "backup directory invalid");
        settings.url = "http://192.168.1.1/dav/";
        Rejects([&] { ParseEndpoint(settings); });
        settings.url = "http://127.0.0.1:18080/dav/";
        Require(!ParseEndpoint(settings).secure, "loopback HTTP rejected");
        settings.url = "https://example.test/dav/?query=1";
        Rejects([&] { ParseEndpoint(settings); });
        settings.url = "https://user:password@example.test/dav/";
        Rejects([&] { ParseEndpoint(settings); });
        settings.url = "https://example.test/dav/";
        settings.directory = "../outside";
        Rejects([&] { ParseEndpoint(settings); });
        settings.directory = "MapleSSH";
        endpoint = ParseEndpoint(settings);
        Require(BackupNameFromHref(endpoint, L"/dav/MapleSSH/maplessh-20261102-test.msb") == "maplessh-20261102-test.msb", "valid href rejected");
        Require(BackupNameFromHref(endpoint, L"https://foreign.test/dav/MapleSSH/maplessh-x.msb").empty(), "cross-origin href accepted");
        Require(BackupNameFromHref(endpoint, L"/dav/MapleSSH/%2e%2e/maplessh-x.msb").empty(), "traversal href accepted");
        Require(BackupNameFromHref(endpoint, L"/dav/MapleSSH/nested/maplessh-x.msb").empty(), "nested href accepted");
        std::filesystem::create_directories(directory);
        HostKeys keys(directory / "known_hosts");
        Require(keys.Check("host", 22, "ssh-ed25519", "key-one") == Trust::Unknown, "new host unexpectedly trusted");
        keys.Accept("host", 22, "ssh-ed25519", "key-one");
        Require(keys.Check("host", 22, "ssh-ed25519", "key-one") == Trust::Match, "accepted key not trusted");
        Require(keys.Check("host", 22, "ssh-ed25519", "key-two") == Trust::Mismatch, "changed key accepted");
        Require(keys.Check("host", 2222, "ssh-ed25519", "key-one") == Trust::Unknown, "port ignored");
        HostKeys reopened(directory / "known_hosts");
        Require(reopened.Check("host", 22, "ssh-ed25519", "key-one") == Trust::Match, "trust record not persisted");
        const auto before = ReadBytes(directory / "known_hosts");
        Rejects([&] { reopened.Accept("host", 22, "ssh-ed25519", "key-two"); });
        Require(ReadBytes(directory / "known_hosts") == before, "changed key overwrote trust record");
        std::filesystem::remove_all(directory);
        std::cout << "URL policy, href constraints and local host trust PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
