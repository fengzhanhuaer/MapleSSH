#include "model.hpp"
#include "ssh_client.hpp"
#include "webdav.hpp"
#include "storage.hpp"
#include "command_input.hpp"
#include "sftp_client.hpp"
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
FileSnapshot WaitFile(SftpClient& files, const std::string& id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        auto snapshot = files.Snapshot(id);
        if (snapshot.state != "running") return snapshot;
        if (std::chrono::steady_clock::now() >= deadline) { files.Cancel(id); throw std::runtime_error("SFTP fixture timeout"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
void CheckSftp(const Session& session, const Credential& credential, int metrics_port, const std::filesystem::path& temporary) {
    SshClient terminal(session, credential);
    terminal.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!terminal.Connected()) {
        for (const auto& event : terminal.Drain()) if (event.kind == SshEventKind::HostKey) terminal.ConfirmHost(true);
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("SFTP terminal fixture login failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto before = AuthenticationCount(metrics_port);
    SftpClient wrong_key(session, credential, "wrong-host-key");
    auto result = WaitFile(wrong_key, wrong_key.Submit(FileOperation::List, "/"));
    Require(result.state == "failed" && AuthenticationCount(metrics_port) == before, "SFTP sent credentials before checking pinned key");
    auto files = terminal.Files();
    result = WaitFile(*files, files->Submit(FileOperation::List, "."));
    Require(result.state == "succeeded" && result.remote_path == "/" && !result.entries.empty(), "SFTP root listing failed");
    const auto remote = std::string("/目录 'space/中文 'binary;.bin");
    const auto source = temporary / L"上传 'binary.bin";
    const auto destination = temporary / L"下载 'binary.bin";
    Bytes content(512 * 1024); for (std::size_t index = 0; index < content.size(); ++index) content[index] = static_cast<unsigned char>(index % 251);
    WriteAtomic(source, content);
    auto upload = files->Submit(FileOperation::Upload, remote, source);
    Require(terminal.Send("sftp-terminal-alive\r"), "terminal stopped during SFTP upload");
    result = WaitFile(*files, upload);
    if (result.state != "succeeded") throw std::runtime_error("SFTP upload failed: " + result.error);
    Require(result.bytes == content.size(), "SFTP upload progress wrong");
    result = WaitFile(*files, files->Submit(FileOperation::List, "/目录 'space"));
    Require(result.state == "succeeded" && result.entries.size() == 1 && result.entries.front().regular && result.entries.front().size == content.size(), "SFTP uploaded file not listed");
    result = WaitFile(*files, files->Submit(FileOperation::Download, remote, destination));
    if (result.state != "succeeded") throw std::runtime_error("SFTP download failed: " + result.error);
    Require(ReadBytes(destination) == content, "SFTP binary round trip corrupted");
    result = WaitFile(*files, files->Submit(FileOperation::Upload, remote, source));
    Require(result.state == "failed", "SFTP silently overwrote remote file");
    result = WaitFile(*files, files->Submit(FileOperation::Download, remote, destination));
    Require(result.state == "failed" && ReadBytes(destination) == content, "SFTP silently overwrote local file");
    Bytes replacement{1, 0, 2, 255, 3}; WriteAtomic(source, replacement);
    result = WaitFile(*files, files->Submit(FileOperation::Upload, remote, source, true));
    if (result.state != "succeeded") throw std::runtime_error("SFTP explicit overwrite failed: " + result.error);
    result = WaitFile(*files, files->Submit(FileOperation::Download, remote, destination, true));
    Require(result.state == "succeeded" && ReadBytes(destination) == replacement, "SFTP explicit replacement round trip failed");
    const auto cancel = files->Submit(FileOperation::Download, remote, temporary / "cancelled.bin"); files->Cancel(cancel);
    result = WaitFile(*files, cancel);
    Require(result.state == "cancelled" && !std::filesystem::exists(temporary / "cancelled.bin"), "SFTP cancelled download was committed");
    WriteAtomic(source, Bytes(8 * 1024 * 1024, 42));
    const auto active = files->Submit(FileOperation::Upload, remote, source, true);
    const auto active_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (files->Snapshot(active).bytes == 0 && files->Snapshot(active).state == "running" && std::chrono::steady_clock::now() < active_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(files->Snapshot(active).bytes > 0 && files->Snapshot(active).state == "running", "fixture did not reach active transfer");
    Require(terminal.Send("terminal-during-active-upload\r"), "terminal rejected input during transfer");
    const auto echo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (terminal.OutputSnapshot().find("terminal-during-active-upload") == std::string::npos && std::chrono::steady_clock::now() < echo_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(terminal.OutputSnapshot().find("terminal-during-active-upload") != std::string::npos, "terminal output stalled during active SFTP upload");
    files->Cancel(active); result = WaitFile(*files, active);
    Require(result.state == "cancelled" && result.bytes < result.total, "active upload cancellation failed");
    result = WaitFile(*files, files->Submit(FileOperation::Download, remote, destination, true));
    Require(result.state == "succeeded" && ReadBytes(destination) == replacement, "cancelled upload changed original target");
    result = WaitFile(*files, files->Submit(FileOperation::List, "/missing-directory"));
    Require(result.state == "failed" && terminal.Connected(), "missing directory killed terminal");
    result = WaitFile(*files, files->Submit(FileOperation::Download, "/目录 'space", temporary / "directory.bin"));
    Require(result.state == "failed" && !std::filesystem::exists(temporary / "directory.bin"), "directory downloaded as a regular file");
    for (int attempt = 0; attempt < 100 && terminal.OutputSnapshot().find("sftp-terminal-alive") == std::string::npos; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(terminal.OutputSnapshot().find("sftp-terminal-alive") != std::string::npos, "terminal I/O was blocked by SFTP");
    auto pending = files->Submit(FileOperation::Upload, "/disconnected.bin", source);
    const auto disconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (files->Snapshot(pending).bytes == 0 && files->Snapshot(pending).state == "running" && std::chrono::steady_clock::now() < disconnect_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto disconnect_at = std::chrono::steady_clock::now(); terminal.Stop();
    Require(files->Snapshot(pending).state != "running", "disconnect left SFTP task running");
    Require(std::chrono::steady_clock::now() - disconnect_at < std::chrono::seconds(3), "disconnect blocked on SFTP transfer");
    RejectWith([&] { terminal.Files(); }, "连接");
    std::cout << "SFTP pinned host before auth, list, binary/Unicode round trip, overwrite protection, cancel and independent terminal PASS\n";
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
        CheckSftp(session, credential, metrics_port, std::filesystem::path(argv[1]).parent_path());
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
