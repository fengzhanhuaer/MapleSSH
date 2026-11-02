#include "model.hpp"
#include "ssh_client.hpp"
#include "webdav.hpp"
#include "storage.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <array>
#include <future>
#include <functional>

using namespace maple;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
unsigned long AuthenticationCount(int port, const wchar_t* path = L"/metrics") {
    struct Close { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
    using Handle = std::unique_ptr<void, Close>;
    Handle session(WinHttpOpen(L"MapleSSH-test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    Require(session != nullptr, "metrics session failed");
    WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 3000);
    Handle connection(WinHttpConnect(session.get(), L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0));
    Require(connection != nullptr, "metrics connection failed");
    Handle request(WinHttpOpenRequest(connection.get(), L"GET", path, nullptr, nullptr, nullptr, 0));
    Require(request && WinHttpSendRequest(request.get(), nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request.get(), nullptr), "metrics request failed");
    std::array<char, 64> buffer{}; DWORD count = 0;
    Require(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count) && count, "metrics read failed");
    return std::stoul(std::string(buffer.data(), count));
}
void RejectWith(const std::function<void()>& operation, std::string_view expected) {
    try { operation(); } catch (const std::exception& error) { Require(std::string_view(error.what()).find(expected) != std::string_view::npos, "unexpected error category"); return; }
    throw std::runtime_error("expected operation rejection");
}
void CheckSshClient(SshClient& client, bool trust, bool expect_authentication = true) {
    client.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool host_seen = false;
    bool ready = false;
    bool sent = false;
    std::string received;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : client.Drain()) {
            if (event.kind == SshEventKind::HostKey) {
                Require(!host_seen && !client.Connected(), "host verification happened after authentication");
                host_seen = true; client.ConfirmHost(trust);
            } else if (event.kind == SshEventKind::Output) received += event.text;
            else if (event.kind == SshEventKind::State && event.text != "正在连接" && event.text != "正在认证" && event.text != "已连接") {
                if (!trust && host_seen) { client.Stop(); return; }
                if (!expect_authentication && host_seen) { Require(event.text.starts_with("SSH 认证失败"), "failure was not an authentication rejection"); client.Stop(); return; }
                throw std::runtime_error(event.text);
            }
        }
        if (received.find("fixture-ready") != std::string::npos) ready = true;
        if (ready && !sent) { client.Send("maplessh-echo-check\n"); client.Resize(30, 100); sent = true; }
        if (received.find("maplessh-echo-check") != std::string::npos && received.find("fixture-resize:100x30") != std::string::npos) {
            Require(trust && expect_authentication, "rejected authentication established a shell"); client.Stop(); return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.Stop(); throw std::runtime_error("SSH integration timeout");
}
void CheckSsh(Session session, bool trust, bool expect_authentication = true) {
    SshClient client(std::move(session));
    CheckSshClient(client, trust, expect_authentication);
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("provide isolated fixture ready.json");
        const auto information = nlohmann::json::parse(ReadBytes(std::filesystem::path(argv[1])));
        Session session;
        session.id = "integration"; session.name = "fixture"; session.host = "127.0.0.1";
        session.port = information.at("ssh_port"); session.username = information.at("username"); session.password = information.at("password"); session.timeout_seconds = 5;
        const auto metrics_port = information.value("metrics_port", information.at("dav_port").get<int>());
        const auto before = AuthenticationCount(metrics_port);
        CheckSsh(session, false);
        Require(AuthenticationCount(metrics_port) == before, "credentials reached an untrusted host");
        std::cout << "SSH host rejection before any authentication PASS\n";
        CheckSsh(session, true);
        std::cout << "SSH password PTY and echo PASS\n";
        SshClient reusable(session);
        CheckSshClient(reusable, true);
        CheckSshClient(reusable, true);
        std::cout << "SSH reconnect without stale events PASS\n";
        auto invalid = session; invalid.password = "wrong-password";
        CheckSsh(invalid, true, false);
        std::cout << "SSH wrong password rejection PASS\n";
        session.auth = AuthMethod::PrivateKey;
        session.private_key = information.at("private_key");
        CheckSsh(session, true);
        std::cout << "SSH OpenSSH Ed25519 private key PASS\n";
        session.private_key = information.at("encrypted_private_key"); session.passphrase = information.at("passphrase");
        CheckSsh(session, true);
        std::cout << "SSH encrypted Ed25519 private key PASS\n";
        session.private_key = information.at("rsa_private_key"); session.passphrase.clear();
        CheckSsh(session, true);
        std::cout << "SSH RSA PEM private key PASS\n";
        WebDavSettings settings{"http://127.0.0.1:" + std::to_string(information.at("dav_port").get<int>()) + "/dav/", information.at("username"), information.at("password"), "MapleSSH"};
        WebDavClient client(settings);
        Require(client.List().empty(), "fixture backup directory not empty");
        const auto payload = SerializeBackup({session});
        const auto encrypted = Encrypt(payload, "integration-master-password", "backup");
        const auto name = WebDavClient::NewBackupName();
        client.Upload(name, encrypted);
        const auto names = client.List();
        Require(names.size() == 1 && names.front() == name, "WebDAV backup not listed");
        const auto restored = ParseBackup(Decrypt(client.Download(name), "integration-master-password", "backup"));
        Require(restored.size() == 1 && restored.front().private_key == session.private_key, "WebDAV credentials round trip failed");
        bool rejected = false;
        const auto different = Encrypt("different content", "integration-master-password", "backup");
        try { client.Upload(name, different); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "existing backup silently overwritten");
        Require(client.Download(name) == encrypted, "collision changed the original backup");
        settings.password = "wrong-password"; rejected = false;
        try { WebDavClient unauthorized(settings); unauthorized.List(); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "WebDAV wrong password accepted");
        std::cout << "WebDAV verified upload, list, restore, collision and 401 PASS\n";
        const auto fixture_url = "http://127.0.0.1:" + std::to_string(metrics_port);
        settings.password = information.at("password");
        settings.url = fixture_url + "/forbidden/"; WebDavClient forbidden(settings);
        RejectWith([&] { forbidden.List(); }, "403");
        settings.url = fixture_url + "/redirect/"; WebDavClient redirect(settings);
        RejectWith([&] { redirect.List(); }, "重定向");
        Require(AuthenticationCount(metrics_port, L"/redirect-metrics") == 0, "redirect followed with credentials");
        settings.url = fixture_url + "/corrupt/"; WebDavClient corrupt(settings);
        RejectWith([&] { corrupt.Upload(WebDavClient::NewBackupName(), encrypted); }, "回读校验失败");
        settings.url = fixture_url + "/slow/"; WebDavClient cancelled(settings);
        auto pending = std::async(std::launch::async, [&] { RejectWith([&] { cancelled.List(); }, "取消"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancelled.Cancel();
        Require(pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "cancel did not finish within fixture bound"); pending.get();
        settings.url = fixture_url + "/timeout/"; WebDavClient timeout(settings);
        const auto started = std::chrono::steady_clock::now();
        RejectWith([&] { timeout.List(); }, "网络请求失败");
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(14), "network timeout exceeded bound");
        std::cout << "WebDAV 403, redirect, corrupted readback, cancellation and timeout PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
