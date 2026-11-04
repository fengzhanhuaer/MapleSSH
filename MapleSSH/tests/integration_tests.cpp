#include "model.hpp"
#include "ssh_client.hpp"
#include "webdav.hpp"
#include "storage.hpp"
#include "command_input.hpp"
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
    Require(!client.Send("not-connected-command\r"), "disconnected client queued a command");
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
        if (ready && !sent) {
            const auto commands = PrepareCommands("maplessh-echo-check\r\n中文命令检查\n\nmaplessh-shortcut-check");
            Require(client.Send(commands.input), "connected client rejected commands"); client.Resize(30, 100); sent = true;
        }
        if (received.find("maplessh-echo-check\r中文命令检查\r\rmaplessh-shortcut-check\r") != std::string::npos && received.find("fixture-resize:100x30") != std::string::npos) {
            Require(received.find("中文终端测试：正在连接，正在认证，已连接。") != std::string::npos, "SSH UTF-8 output changed");
            Require(trust && expect_authentication, "rejected authentication established a shell");
            client.Stop(); Require(!client.Send("after-stop-command\r"), "stopped client queued a command"); return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.Stop(); throw std::runtime_error("SSH integration timeout");
}
void CheckSsh(Session session, Credential credential, bool trust, bool expect_authentication = true) {
    SshClient client(std::move(session), std::move(credential));
    CheckSshClient(client, trust, expect_authentication);
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("provide isolated fixture ready.json");
        const auto information = nlohmann::json::parse(ReadBytes(std::filesystem::path(argv[1])));
        Session session;
        Credential credential; credential.id = "shared-integration"; credential.name = "fixture credential"; session.credential_id = credential.id;
        session.id = "integration"; session.name = "fixture"; session.host = "127.0.0.1";
        session.port = information.at("ssh_port"); credential.username = information.at("username"); credential.password = information.at("password"); session.timeout_seconds = 5;
        const auto metrics_port = information.value("metrics_port", information.at("dav_port").get<int>());
        const auto before = AuthenticationCount(metrics_port);
        CheckSsh(session, credential, false);
        Require(AuthenticationCount(metrics_port) == before, "credentials reached an untrusted host");
        std::cout << "SSH host rejection before any authentication PASS\n";
        CheckSsh(session, credential, true);
        std::cout << "SSH password PTY, multiline command bytes, UTF-8 and disconnected input rejection PASS\n";
        auto shared_host = session; shared_host.id = "integration-shared"; shared_host.name = "second host";
        CheckSsh(shared_host, credential, true);
        std::cout << "SSH shared credential reused by another host PASS\n";
        SshClient reusable(session, credential);
        CheckSshClient(reusable, true);
        CheckSshClient(reusable, true);
        std::cout << "SSH reconnect without stale events PASS\n";
        auto invalid = credential; invalid.password = "wrong-password";
        CheckSsh(session, invalid, true, false);
        std::cout << "SSH wrong password rejection PASS\n";
        credential.auth = AuthMethod::PrivateKey;
        credential.private_key = information.at("private_key");
        CheckSsh(session, credential, true);
        std::cout << "SSH OpenSSH Ed25519 private key PASS\n";
        credential.private_key = information.at("encrypted_private_key"); credential.passphrase = information.at("passphrase");
        CheckSsh(session, credential, true);
        std::cout << "SSH encrypted Ed25519 private key PASS\n";
        credential.private_key = information.at("rsa_private_key"); credential.passphrase.clear();
        CheckSsh(session, credential, true);
        std::cout << "SSH RSA PEM private key PASS\n";
        WebDavSettings settings{"http://127.0.0.1:" + std::to_string(information.at("dav_port").get<int>()) + "/dav/", information.at("username"), information.at("password"), "MapleSSH"};
        WebDavClient client(settings);
        const auto writes_before_login = AuthenticationCount(metrics_port, L"/dav-write-metrics");
        client.TestLogin();
        auto missing_directory = settings; missing_directory.directory = "not-created-yet";
        WebDavClient(missing_directory).TestLogin();
        Require(AuthenticationCount(metrics_port, L"/dav-write-metrics") == writes_before_login, "login test changed remote files");
        Require(client.List().empty(), "fixture backup directory not empty");
        Vault backup_vault; backup_vault.sessions = {session}; backup_vault.credentials = {credential};
        backup_vault.quick_commands = {{"integration-shortcut", "目录", "pwd\r\nls -la"}};
        const auto payload = SerializeBackup(backup_vault);
        const Bytes backup(payload.begin(), payload.end());
        const auto name = WebDavClient::NewBackupName();
        client.Upload(name, backup);
        const auto names = client.List();
        Require(names.size() == 1 && names.front() == name, "WebDAV backup not listed");
        const auto downloaded = client.Download(name);
        const auto restored = ParseBackup(std::string(downloaded.begin(), downloaded.end()));
        Require(restored.sessions.size() == 1 && FindCredential(restored, restored.sessions.front().credential_id).private_key == credential.private_key, "WebDAV credentials round trip failed");
        Require(restored.quick_commands.size() == 1 && restored.quick_commands.front().command == backup_vault.quick_commands.front().command && restored.quick_commands.front().name == "目录", "WebDAV backup lost quick commands");
        bool rejected = false;
        const Bytes different{123, 125};
        try { client.Upload(name, different); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "existing backup silently overwritten");
        Require(client.Download(name) == backup, "collision changed the original backup");
        settings.password = "wrong-password"; rejected = false;
        RejectWith([&] { WebDavClient(settings).TestLogin(); }, "401");
        try { WebDavClient unauthorized(settings); unauthorized.List(); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "WebDAV wrong password accepted");
        std::cout << "WebDAV passwordless backup upload, list, restore, collision and 401 PASS\n";
        const auto fixture_url = "http://127.0.0.1:" + std::to_string(metrics_port);
        settings.password = information.at("password");
        settings.url = fixture_url + "/webpage/";
        RejectWith([&] { WebDavClient(settings).TestLogin(); }, "WebDAV 地址");
        settings.url = fixture_url + "/fake-dav/";
        RejectWith([&] { WebDavClient(settings).TestLogin(); }, "WebDAV");
        settings.url = fixture_url + "/dav-denied/";
        RejectWith([&] { WebDavClient(settings).TestLogin(); }, "访问");
        settings.url = fixture_url + "/forbidden/"; WebDavClient forbidden(settings);
        RejectWith([&] { forbidden.TestLogin(); }, "403");
        RejectWith([&] { forbidden.List(); }, "403");
        settings.url = fixture_url + "/redirect/"; WebDavClient redirect(settings);
        RejectWith([&] { redirect.TestLogin(); }, "重定向");
        RejectWith([&] { redirect.List(); }, "重定向");
        Require(AuthenticationCount(metrics_port, L"/redirect-metrics") == 0, "redirect followed with credentials");
        settings.url = fixture_url + "/corrupt/"; WebDavClient corrupt(settings);
        RejectWith([&] { corrupt.Upload(WebDavClient::NewBackupName(), backup); }, "回读校验失败");
        settings.url = fixture_url + "/slow/"; WebDavClient cancelled(settings);
        auto pending = std::async(std::launch::async, [&] { RejectWith([&] { cancelled.List(); }, "取消"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancelled.Cancel();
        Require(pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "cancel did not finish within fixture bound"); pending.get();
        settings.url = fixture_url + "/timeout/"; WebDavClient timeout(settings);
        const auto started = std::chrono::steady_clock::now();
        RejectWith([&] { timeout.List(); }, "网络请求失败");
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(14), "network timeout exceeded bound");
        std::cout << "WebDAV 403, redirect, corrupted readback, cancellation and timeout PASS\n";
        std::cout << "WebDAV read-only login, missing backup directory, invalid endpoint and rejected credentials PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
