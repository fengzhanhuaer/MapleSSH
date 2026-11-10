#include "mcp_server.hpp"
#include <winsock2.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <set>
#include <chrono>

namespace maple {
namespace {
using Json = nlohmann::json;
constexpr auto InvalidSocket = static_cast<std::uintptr_t>(-1);
constexpr std::size_t MaxRequestBytes = 4 * 1024 * 1024;
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool AllowedOrigin(const std::string& origin) {
    if (origin.empty()) return true;
    for (const auto prefix : {std::string("http://localhost"), std::string("http://127.0.0.1")}) {
        if (origin == prefix) return true;
        if (origin.starts_with(prefix + ":")) {
            const auto port = origin.substr(prefix.size() + 1);
            if (!port.empty() && std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return true;
        }
    }
    return false;
}
std::string HttpResponse(int status, std::string_view reason, std::string_view body, std::string_view content_type = "application/json") {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\nConnection: close\r\n\r\n" << body;
    return response.str();
}
Json ErrorResponse(const Json& id, int code, std::string_view message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}
Json TextResult(std::string text, bool error = false) {
    return {{"content", Json::array({{{"type", "text"}, {"text", std::move(text)}}})}, {"isError", error}};
}
Json Tool(std::string name, std::string description, Json schema) {
    return {{"name", std::move(name)}, {"description", std::move(description)}, {"inputSchema", std::move(schema)}};
}
int WaitIo(SOCKET client, bool write, const std::atomic<bool>& stopping, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        if (stopping || std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("MCP 服务关闭或客户端超时");
        fd_set ready; FD_ZERO(&ready); FD_SET(client, &ready);
        timeval timeout{0, 100000};
        const auto selected = select(0, write ? nullptr : &ready, write ? &ready : nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) throw std::runtime_error("MCP 客户端连接失效");
        if (selected > 0) return selected;
    }
}
int Receive(SOCKET client, char* buffer, int capacity, const std::atomic<bool>& stopping) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        WaitIo(client, false, stopping, deadline);
        const auto count = recv(client, buffer, capacity, 0);
        if (count != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK) return count;
    }
}
void SendAll(SOCKET client, std::string_view response, const std::atomic<bool>& stopping) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (offset < response.size()) {
        WaitIo(client, true, stopping, deadline);
        const auto count = send(client, response.data() + offset, static_cast<int>(response.size() - offset), 0);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (count <= 0) throw std::runtime_error("MCP 响应发送失败");
        offset += count;
    }
}
bool FileTool(std::string_view name) {
    return name == "list_directory" || name == "upload_file" || name == "download_file" || name == "get_file_job" || name == "cancel_file_job";
}
void ValidateFileArguments(std::string_view name, const Json& arguments) {
    if (!arguments.is_object()) throw std::runtime_error("文件工具参数必须是对象");
    std::set<std::string> fields;
    if (name == "get_file_job" || name == "cancel_file_job") fields = {"job_id"};
    else if (name == "list_directory") fields = {"session_id", "path"};
    else fields = {"session_id", "local_path", "remote_path", "overwrite"};
    for (const auto& [field, value] : arguments.items()) {
        if (!fields.contains(field)) throw std::runtime_error("不支持的文件工具参数");
        if (field == "overwrite") { if (!value.is_boolean()) throw std::runtime_error("overwrite 必须是布尔值"); }
        else if (!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > 32768 || value.get_ref<const std::string&>().find('\0') != std::string::npos)
            throw std::runtime_error("文件工具字符串参数无效");
    }
    for (const auto& field : fields) if (field != "path" && field != "overwrite" && !arguments.contains(field)) throw std::runtime_error("文件工具缺少必需参数: " + field);
}
std::string RequireString(const Json& object, std::string_view name) {
    if (!object.is_object() || !object.contains(name) || !object.at(name).is_string()) throw std::runtime_error("参数必须包含字符串字段: " + std::string(name));
    return object.at(name).get<std::string>();
}
}
McpServer::McpServer(Callbacks callbacks) : callbacks_(std::move(callbacks)) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("无法初始化 MCP 网络服务");
    });
}
McpServer::~McpServer() { Stop(); }
void McpServer::Start(std::uint16_t port) {
    if (running_) return;
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) throw std::runtime_error("无法创建 MCP 监听套接字");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener); throw std::runtime_error("MCP 端口已被占用，无法启动服务");
    }
    sockaddr_in actual{}; int actual_size = sizeof(actual); getsockname(listener, reinterpret_cast<sockaddr*>(&actual), &actual_size);
    listen_socket_ = static_cast<std::uintptr_t>(listener); port_ = ntohs(actual.sin_port); running_ = true; stopping_ = false;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
void McpServer::Stop() {
    stopping_ = true;
    if (worker_.joinable()) {
        worker_.request_stop();
        { const std::lock_guard lock(client_mutex_); if (client_socket_ != InvalidSocket) shutdown(static_cast<SOCKET>(client_socket_), SD_BOTH); }
        worker_.join();
        const auto listener = listen_socket_.exchange(InvalidSocket);
        if (listener != InvalidSocket) closesocket(static_cast<SOCKET>(listener));
    }
    running_ = false; port_ = 0;
}
void McpServer::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        const auto listener = listen_socket_.load(); if (listener == InvalidSocket) break;
        fd_set readable; FD_ZERO(&readable); FD_SET(static_cast<SOCKET>(listener), &readable);
        timeval timeout{0, 200000}; if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
        const SOCKET client = accept(static_cast<SOCKET>(listener), nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            { const std::lock_guard lock(client_mutex_); client_socket_ = static_cast<std::uintptr_t>(client); }
            if (!stop.stop_requested()) HandleClient(static_cast<std::uintptr_t>(client));
            { const std::lock_guard lock(client_mutex_); closesocket(client); client_socket_ = InvalidSocket; }
        }
    }
}
void McpServer::HandleClient(std::uintptr_t raw_client) {
    const SOCKET client = static_cast<SOCKET>(raw_client);
    u_long nonblocking = 1;
    if (ioctlsocket(client, FIONBIO, &nonblocking) != 0) return;
    std::string request; std::array<char, 8192> buffer{}; std::size_t header_end = std::string::npos; std::size_t content_length = 0;
    try {
        while (request.size() <= MaxRequestBytes && (header_end = request.find("\r\n\r\n")) == std::string::npos) {
            const auto count = Receive(client, buffer.data(), static_cast<int>(buffer.size()), stopping_); if (count <= 0) throw std::runtime_error("MCP 请求读取失败");
            request.append(buffer.data(), static_cast<std::size_t>(count));
        }
        if (header_end == std::string::npos) throw std::runtime_error("MCP 请求头过大");
        std::istringstream lines(request.substr(0, header_end)); std::string line;
        if (!std::getline(lines, line)) throw std::runtime_error("MCP 请求行无效");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream request_line(line); std::string method, path, version; request_line >> method >> path >> version;
        std::map<std::string, std::string> headers;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back(); const auto separator = line.find(':'); if (separator == std::string::npos) continue;
            auto value = line.substr(separator + 1); value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
            headers[Lower(line.substr(0, separator))] = std::move(value);
        }
        if (method != "POST" || path != "/mcp") {
            const auto response = HttpResponse(method == "GET" ? 405 : 404, method == "GET" ? "Method Not Allowed" : "Not Found", "{\"error\":\"MCP endpoint requires POST /mcp\"}");
            SendAll(client, response, stopping_); return;
        }
        if (!AllowedOrigin(headers["origin"])) {
            const auto response = HttpResponse(403, "Forbidden", "{\"error\":\"invalid Origin\"}"); SendAll(client, response, stopping_); return;
        }
        if (headers.contains("content-length")) {
            const auto value = headers["content-length"];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), content_length);
            if (parsed.ec != std::errc{} || content_length > MaxRequestBytes) throw std::runtime_error("MCP 请求体过大");
        }
        while (request.size() - (header_end + 4) < content_length) {
            const auto count = Receive(client, buffer.data(), static_cast<int>(buffer.size()), stopping_); if (count <= 0) throw std::runtime_error("MCP 请求体读取失败");
            request.append(buffer.data(), static_cast<std::size_t>(count));
        }
        const auto response_body = HandleMessage(request.substr(header_end + 4, content_length));
        const auto response = response_body.empty() ? HttpResponse(202, "Accepted", "", "text/plain") : HttpResponse(200, "OK", response_body);
        SendAll(client, response, stopping_);
    } catch (const std::exception& error) {
        const auto response = HttpResponse(400, "Bad Request", ErrorResponse(nullptr, -32600, error.what()).dump());
        try { SendAll(client, response, stopping_); } catch (...) { }
    }
}
std::string McpServer::HandleMessage(std::string_view body) {
    const auto request = Json::parse(body);
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method")) throw std::runtime_error("无效的 JSON-RPC 请求");
    const auto id = request.contains("id") ? request.at("id") : Json(nullptr); const auto method = request.at("method").get<std::string>();
    if (!request.contains("id")) return {};
    if (method == "initialize") return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", "2025-11-25"}, {"capabilities", {{"tools", {{"listChanged", false}}}}}, {"serverInfo", {{"name", "MapleSSH"}, {"version", "0.1.0"}}}, {"instructions", "仅允许访问用户已在 MapleSSH 中打开的 SSH 会话；密码、私钥和主机指纹不会通过 MCP 返回。"}}}}.dump();
    if (method == "ping") return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}}.dump();
    if (method == "tools/list") {
        const auto empty = Json{{"type", "object"}, {"additionalProperties", false}};
        const auto session_id = Json{{"type", "object"}, {"properties", Json{{"session_id", Json{{"type", "string"}}}}}, {"required", Json::array({"session_id"})}, {"additionalProperties", false}};
        const auto send_input = Json{{"type", "object"}, {"properties", Json{{"session_id", Json{{"type", "string"}}}, {"text", Json{{"type", "string"}}}}}, {"required", Json::array({"session_id", "text"})}, {"additionalProperties", false}};
        const auto read_output = Json{{"type", "object"}, {"properties", Json{{"session_id", Json{{"type", "string"}}}, {"max_bytes", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1048576}}}}}, {"required", Json::array({"session_id"})}, {"additionalProperties", false}};
        auto tools = Json::array({
            Tool("list_sessions", "列出 MapleSSH 中已配置的 SSH 会话，不包含密码、私钥和主机指纹。", empty),
            Tool("get_session_status", "读取一个已配置会话的打开和连接状态。", session_id),
            Tool("send_terminal_input", "向一个已打开且已连接的 SSH 终端发送输入，不会自动创建连接。", send_input),
            Tool("read_terminal_output", "读取一个已打开 SSH 终端最近的输出。", read_output),
            Tool("disconnect_session", "断开一个已打开的 SSH 终端。", session_id),
            Tool("connect_session", "使用 MapleSSH 已保存的凭据开始连接指定会话；可能需要在界面确认主机指纹。", session_id)
        });
        const auto string = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 32768}};
        auto directory = session_id;
        directory["properties"]["path"] = string;
        auto transfer = session_id;
        transfer["properties"]["local_path"] = string; transfer["properties"]["remote_path"] = string;
        transfer["properties"]["overwrite"] = Json{{"type", "boolean"}, {"default", false}};
        transfer["required"] = Json::array({"session_id", "local_path", "remote_path"});
        const auto job = Json{{"type", "object"}, {"properties", {{"job_id", string}}}, {"required", Json::array({"job_id"})}, {"additionalProperties", false}};
        tools.push_back(Tool("list_directory", "异步列出远程目录，返回 job_id；使用 get_file_job 获取结果。", directory));
        tools.push_back(Tool("upload_file", "异步上传本机普通文件。需用户在 MapleSSH 界面批准本机绝对路径；默认不覆盖，返回 job_id。", transfer));
        tools.push_back(Tool("download_file", "异步下载远程普通文件。需用户在 MapleSSH 界面批准本机绝对路径；默认不覆盖，返回 job_id。", transfer));
        tools.push_back(Tool("get_file_job", "查询文件任务：awaiting_approval、running、succeeded、failed 或 cancelled，含进度及目录结果。", job));
        tools.push_back(Tool("cancel_file_job", "取消等待批准或进行中的文件任务，不重试操作。", job));
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", tools}}}}.dump();
    }
    if (method != "tools/call") return ErrorResponse(id, -32601, "未知的 MCP 方法").dump();
    const auto& params = request.at("params"); const auto name = RequireString(params, "name"); const auto arguments = params.contains("arguments") ? params.at("arguments") : Json::object();
    try {
        std::string output;
        if (name == "list_sessions") output = callbacks_.list_sessions();
        else if (name == "get_session_status") output = callbacks_.session_status(RequireString(arguments, "session_id"));
        else if (name == "send_terminal_input") output = callbacks_.send_input(RequireString(arguments, "session_id"), RequireString(arguments, "text"));
        else if (name == "read_terminal_output") {
            std::size_t max_bytes = 64 * 1024;
            if (arguments.contains("max_bytes")) {
                if (!arguments.at("max_bytes").is_number_integer()) throw std::runtime_error("max_bytes 必须是整数");
                max_bytes = std::clamp<std::size_t>(arguments.at("max_bytes").get<std::size_t>(), 1, 1024 * 1024);
            }
            output = callbacks_.read_output(RequireString(arguments, "session_id"), max_bytes);
        } else if (name == "disconnect_session") output = callbacks_.disconnect(RequireString(arguments, "session_id"));
        else if (name == "connect_session") output = callbacks_.connect(RequireString(arguments, "session_id"));
        else if (FileTool(name)) {
            ValidateFileArguments(name, arguments);
            if (!callbacks_.files) throw std::runtime_error("文件工具尚未接入");
            output = callbacks_.files(name, arguments.dump());
        }
        else return ErrorResponse(id, -32602, "未知的工具: " + name).dump();
        auto result = TextResult(output); try { result["structuredContent"] = Json::parse(output); } catch (...) { }
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump();
    } catch (const std::exception& error) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", TextResult(error.what(), true)}}.dump();
    }
}
}

