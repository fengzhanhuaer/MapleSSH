#include "mcp_server.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <thread>

using namespace maple;
using Json = nlohmann::json;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Close { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using Handle = std::unique_ptr<void, Close>;
struct Response { DWORD status; std::string body; };
Response Post(unsigned short port, const Json& object, const wchar_t* extra = L"") {
    Handle session(WinHttpOpen(L"MapleSSH-MCP-test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    WinHttpSetTimeouts(session.get(), 2000, 2000, 2000, 3000);
    Handle connection(WinHttpConnect(session.get(), L"127.0.0.1", port, 0));
    Handle request(WinHttpOpenRequest(connection.get(), L"POST", L"/mcp", nullptr, nullptr, nullptr, 0));
    auto body = object.dump();
    const auto headers = std::wstring(L"Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n") + extra;
    Require(WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()), body.data(), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) && WinHttpReceiveResponse(request.get(), nullptr), "MCP POST failed");
    Response response{}; DWORD size = sizeof(response.status);
    Require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &response.status, &size, nullptr), "HTTP status missing");
    std::array<char, 8192> buffer{}; DWORD count = 0;
    while (WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count) && count) response.body.append(buffer.data(), count);
    return response;
}
int main() {
    try {
        std::atomic<int> file_calls{0};
        McpServer::Callbacks callbacks;
        callbacks.list_sessions = [] { return std::string("{\"sessions\":[]}"); };
        callbacks.files = [&](std::string_view tool, std::string_view input) {
            ++file_calls;
            const auto arguments = Json::parse(input);
            if (arguments.value("job_id", "") == "large") return Json{{"state", "succeeded"}, {"payload", std::string(400000, 'x')}}.dump();
            return Json{{"tool", tool}, {"arguments", arguments}, {"job_id", "test-job"}, {"state", tool == "upload_file" || tool == "download_file" ? "awaiting_approval" : "running"}}.dump();
        };
        McpServer server(callbacks); server.Start(0);
        auto response = Post(server.Port(), {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-11-25"}}}});
        Require(response.status == 200 && Json::parse(response.body).at("result").contains("serverInfo"), "MCP initialize failed");
        response = Post(server.Port(), {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        Require(response.status == 202 && response.body.empty(), "MCP notification failed");
        response = Post(server.Port(), {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
        const auto tools = Json::parse(response.body).at("result").at("tools");
        for (const auto* name : {"list_directory", "upload_file", "download_file", "get_file_job", "cancel_file_job"}) {
            bool present = false; for (const auto& tool : tools) if (tool.at("name") == name) present = true;
            Require(present, "MCP file tool missing");
        }
        response = Post(server.Port(), {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", {{"name", "list_sessions"}, {"arguments", Json::object()}}}});
        Require(!Json::parse(response.body).at("result").at("isError").get<bool>(), "MCP list tool call failed");
        auto call = [&](std::string_view tool, const Json& arguments) {
            const auto reply = Post(server.Port(), {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", arguments}}}});
            Require(reply.status == 200, "MCP file call HTTP failure"); return Json::parse(reply.body).at("result");
        };
        auto result = call("list_directory", {{"session_id", "fixture"}, {"path", "/目录"}});
        Require(!result.at("isError").get<bool>() && result.at("structuredContent").at("arguments").at("path") == "/目录", "MCP file dispatch lost Unicode");
        for (const auto* tool : {"upload_file", "download_file"}) {
            result = call(tool, {{"session_id", "fixture"}, {"local_path", "C:/test/file"}, {"remote_path", "/file"}, {"overwrite", false}});
            Require(result.at("structuredContent").at("state") == "awaiting_approval", "MCP transfer dispatch failed");
        }
        const auto before = file_calls.load();
        for (const auto& arguments : {Json{{"session_id", "fixture"}}, Json{{"session_id", "fixture"}, {"local_path", "C:/a"}, {"remote_path", "/a"}, {"overwrite", "yes"}}, Json{{"session_id", "fixture"}, {"local_path", "C:/a"}, {"remote_path", "/a"}, {"password", "must-not-dispatch"}}})
            Require(call("upload_file", arguments).at("isError").get<bool>(), "MCP accepted invalid file arguments");
        Require(file_calls == before, "invalid file arguments reached backend");
        result = call("get_file_job", {{"job_id", "large"}});
        Require(result.at("structuredContent").at("payload").get<std::string>().size() == 400000, "MCP large response truncated");
        Require(!call("cancel_file_job", {{"job_id", "test-job"}}).at("isError").get<bool>(), "MCP cancel dispatch failed");
        response = Post(server.Port(), {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}}, L"Origin: https://evil.example\r\n");
        Require(response.status == 403, "MCP allowed invalid origin");
        const SOCKET idle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(server.Port()); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(connect(idle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "idle MCP connection failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto started = std::chrono::steady_clock::now(); server.Stop(); closesocket(idle);
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "MCP stop blocked on idle client");
        server.Start(0); server.Stop();
        std::cout << "MCP file tool discovery and HTTP lifecycle PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what(); return 1; }
}
