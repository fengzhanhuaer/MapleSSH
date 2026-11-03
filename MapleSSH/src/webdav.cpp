#include "webdav.hpp"
#include <windows.h>
#include <winhttp.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <sstream>

namespace maple {
namespace {
std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("无效 UTF-8 字符串");
    std::wstring output(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), count);
    return output;
}
std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("无效 Unicode 字符串");
    std::string output(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), count, nullptr, nullptr);
    return output;
}
std::wstring Segment(std::string_view value) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring output;
    for (unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.') output += character;
        else { output += L'%'; output += digits[character >> 4]; output += digits[character & 15]; }
    }
    return output;
}
bool SafeName(std::string_view value) {
    return value.size() < 200 && value.starts_with("maplessh-") && value.ends_with(".msb") &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' || character == '.';
        });
}
struct InternetDeleter { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using InternetHandle = std::unique_ptr<void, InternetDeleter>;
void CheckHttp(BOOL result) { if (!result) throw std::runtime_error("WebDAV 网络请求失败，请检查地址、证书或连接"); }
void CheckCom(HRESULT result) { if (FAILED(result)) throw std::runtime_error("WebDAV 返回的 XML 无效"); }
void ValidateLoginXml(const Bytes& content) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IStream> stream(SHCreateMemStream(content.data(), static_cast<UINT>(content.size())));
    if (!stream) throw std::runtime_error("无法分配 XML 缓冲区");
    ComPtr<IXmlReader> reader;
    CheckCom(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr));
    CheckCom(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    CheckCom(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32));
    CheckCom(reader->SetInput(stream.Get()));
    std::vector<std::wstring> elements;
    bool multistatus = false, collection = false, accessible = false;
    unsigned responses = 0;
    std::wstring status;
    XmlNodeType kind{};
    HRESULT result = S_OK;
    while ((result = reader->Read(&kind)) == S_OK) {
        UINT depth = 0; CheckCom(reader->GetDepth(&depth));
        if (kind == XmlNodeType_Element) {
            const wchar_t* local = nullptr; const wchar_t* space = nullptr;
            CheckCom(reader->GetLocalName(&local, nullptr)); CheckCom(reader->GetNamespaceUri(&space, nullptr));
            elements.resize(depth); elements.emplace_back(std::wstring_view(space) == L"DAV:" ? local : L"");
            if (depth == 0) multistatus = elements.back() == L"multistatus";
            if (depth == 1 && elements.back() == L"response") ++responses;
            if (elements == std::vector<std::wstring>{L"multistatus", L"response", L"propstat"}) { collection = false; status.clear(); }
            if (elements == std::vector<std::wstring>{L"multistatus", L"response", L"propstat", L"prop", L"resourcetype", L"collection"}) collection = true;
        } else if ((kind == XmlNodeType_Text || kind == XmlNodeType_CDATA) &&
            elements == std::vector<std::wstring>{L"multistatus", L"response", L"propstat", L"status"}) {
            const wchar_t* text = nullptr; UINT count = 0; CheckCom(reader->GetValue(&text, &count));
            if (status.size() + count > 1024) throw std::runtime_error("WebDAV 返回状态过长");
            status.append(text, count);
        } else if (kind == XmlNodeType_EndElement) {
            if (depth == 2 && elements.size() >= 3 && elements[0] == L"multistatus" && elements[1] == L"response" && elements[2] == L"propstat") {
                std::wistringstream fields(status); std::wstring version; unsigned code = 0;
                if (fields >> version >> code && version.starts_with(L"HTTP/") && code == 200 && collection) accessible = true;
            }
            elements.resize(depth);
        }
    }
    CheckCom(result);
    if (!multistatus || responses != 1) throw std::runtime_error("服务器未返回有效 WebDAV 目录，请检查 WebDAV 地址是否包含 /dav/");
    if (!accessible) throw std::runtime_error("已连接 WebDAV，但无法访问该目录，请检查账号权限和地址");
}
}
WebEndpoint ParseEndpoint(const WebDavSettings& settings) {
    if (settings.url.empty() || settings.url.size() > 8192 || settings.url.find_first_of("\r\n\t?#") != std::string::npos ||
        settings.url.find('\0') != std::string::npos) throw std::runtime_error("请输入不含查询参数的 WebDAV 地址");
    if (settings.directory.empty() || settings.directory.size() > 256 || settings.directory == "." || settings.directory == ".." ||
        settings.directory.find_first_of("/\\%\r\n\t") != std::string::npos || settings.directory.find('\0') != std::string::npos)
        throw std::runtime_error("备份目录须为单层目录名称");
    const auto url = Wide(settings.url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength =
        components.dwUserNameLength = components.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) ||
        components.dwUserNameLength || components.dwPasswordLength || components.dwExtraInfoLength || !components.dwHostNameLength ||
        (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS))
        throw std::runtime_error("WebDAV 地址无效，不要在 URL 中填写账号或密码");
    WebEndpoint endpoint;
    endpoint.host.assign(components.lpszHostName, components.dwHostNameLength);
    endpoint.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    endpoint.port = components.nPort;
    endpoint.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (!endpoint.secure && _wcsicmp(endpoint.host.c_str(), L"localhost") && endpoint.host != L"127.0.0.1" && endpoint.host != L"::1" && endpoint.host != L"[::1]")
        throw std::runtime_error("远程 WebDAV 必须使用 HTTPS，本机调试地址除外");
    if (endpoint.path.empty()) endpoint.path = L"/";
    if (!endpoint.path.ends_with(L"/")) endpoint.path += L"/";
    if (endpoint.path.find(L"..") != std::wstring::npos || endpoint.path.find(L"%") != std::wstring::npos || endpoint.path.find(L"\\") != std::wstring::npos)
        throw std::runtime_error("WebDAV 基础路径不支持相对段或预编码字符");
    endpoint.base_path = endpoint.path;
    endpoint.path += Segment(settings.directory) + L"/";
    return endpoint;
}
std::string BackupNameFromHref(const WebEndpoint& endpoint, std::wstring_view href) {
    std::wstring path(href);
    if (path.starts_with(L"https://") || path.starts_with(L"http://")) {
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength =
            components.dwUserNameLength = components.dwPasswordLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(path.c_str(), static_cast<DWORD>(path.size()), 0, &components) || components.dwExtraInfoLength ||
            components.dwUserNameLength || components.dwPasswordLength || components.nPort != endpoint.port ||
            (components.nScheme == INTERNET_SCHEME_HTTPS) != endpoint.secure ||
            _wcsicmp(std::wstring(components.lpszHostName, components.dwHostNameLength).c_str(), endpoint.host.c_str())) return {};
        path = std::wstring(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (!path.starts_with(endpoint.path)) return {};
    const auto name = Utf8(std::wstring_view(path).substr(endpoint.path.size()));
    return SafeName(name) ? name : std::string{};
}
WebDavClient::WebDavClient(WebDavSettings settings) : settings_(std::move(settings)), endpoint_(ParseEndpoint(settings_)) {}
WebDavClient::Response WebDavClient::Request(const wchar_t* method, std::wstring_view suffix, const Bytes& body,
    const wchar_t* headers, std::size_t limit, bool use_base_path) {
    if (cancelled_) throw std::runtime_error("操作已取消");
    InternetHandle session(WinHttpOpen(L"MapleSSH/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    if (!session) throw std::runtime_error("无法初始化 WinHTTP");
    CheckHttp(WinHttpSetTimeouts(session.get(), 10000, 10000, 10000, 10000));
    InternetHandle connection(WinHttpConnect(session.get(), endpoint_.host.c_str(), endpoint_.port, 0));
    if (!connection) throw std::runtime_error("无法建立 WebDAV 连接");
    const auto path = (use_base_path ? endpoint_.base_path : endpoint_.path) + std::wstring(suffix);
    InternetHandle request(WinHttpOpenRequest(connection.get(), method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint_.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) throw std::runtime_error("无法创建 WebDAV 请求");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    CheckHttp(WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)));
    const auto username = Wide(settings_.username);
    const auto password = Wide(settings_.password);
    CheckHttp(WinHttpSetCredentials(request.get(), WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC,
        username.c_str(), password.c_str(), nullptr));
    CheckHttp(WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<std::uint8_t*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0));
    CheckHttp(WinHttpReceiveResponse(request.get(), nullptr));
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    CheckHttp(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX));
    Response response{status, {}};
    std::array<std::uint8_t, 16384> buffer{};
    const auto started = GetTickCount64();
    for (;;) {
        if (cancelled_) throw std::runtime_error("操作已取消");
        if (GetTickCount64() - started > 120000) throw std::runtime_error("WebDAV 下载超时");
        DWORD count = 0;
        CheckHttp(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count));
        if (!count) break;
        if (response.body.size() + count > limit) throw std::runtime_error("WebDAV 响应超过大小限制");
        response.body.insert(response.body.end(), buffer.begin(), buffer.begin() + count);
    }
    if (cancelled_) throw std::runtime_error("操作已取消");
    if (status == 401) throw std::runtime_error("WebDAV 账号或密码错误（401）");
    if (status == 403) throw std::runtime_error("WebDAV 拒绝访问，请检查账号权限（403）");
    if (status >= 300 && status < 400) throw std::runtime_error("WebDAV 返回重定向，请直接填写最终 HTTPS 地址");
    return response;
}
void WebDavClient::TestLogin() {
    const std::string query = R"(<?xml version="1.0"?><d:propfind xmlns:d="DAV:"><d:prop><d:resourcetype/></d:prop></d:propfind>)";
    const auto response = Request(L"PROPFIND", L"", Bytes(query.begin(), query.end()),
        L"Depth: 0\r\nContent-Type: application/xml; charset=utf-8\r\n", 2 * 1024 * 1024, true);
    if (response.status != 207)
        throw std::runtime_error("登录测试失败，HTTP " + std::to_string(response.status) + "。请检查 WebDAV 地址，例如 https://nas.example/dav/，不要填写网页登录地址。");
    ValidateLoginXml(response.body);
}
std::vector<std::string> WebDavClient::List() {
    const std::string query = "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>";
    Bytes body(query.begin(), query.end());
    const auto response = Request(L"PROPFIND", L"", body, L"Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n", 2 * 1024 * 1024);
    if (response.status == 404) return {};
    if (response.status != 207) throw std::runtime_error("WebDAV 列表失败，HTTP " + std::to_string(response.status));
    using Microsoft::WRL::ComPtr;
    ComPtr<IStream> stream(SHCreateMemStream(response.body.data(), static_cast<UINT>(response.body.size())));
    if (!stream) throw std::runtime_error("无法分配 XML 缓冲区");
    ComPtr<IXmlReader> reader;
    CheckCom(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr));
    CheckCom(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    CheckCom(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32));
    CheckCom(reader->SetInput(stream.Get()));
    std::vector<std::string> names;
    XmlNodeType kind{};
    bool in_href = false;
    std::wstring href;
    HRESULT result = S_OK;
    while ((result = reader->Read(&kind)) == S_OK) {
        const wchar_t* local = nullptr;
        const wchar_t* space = nullptr;
        if (kind == XmlNodeType_Element || kind == XmlNodeType_EndElement) {
            CheckCom(reader->GetLocalName(&local, nullptr));
            CheckCom(reader->GetNamespaceUri(&space, nullptr));
        }
        if (kind == XmlNodeType_Element && std::wstring_view(local) == L"href" && std::wstring_view(space) == L"DAV:") {
            in_href = true;
            href.clear();
        } else if (in_href && (kind == XmlNodeType_Text || kind == XmlNodeType_CDATA)) {
            const wchar_t* value = nullptr;
            UINT count = 0;
            CheckCom(reader->GetValue(&value, &count));
            if (href.size() + count > 8192) throw std::runtime_error("WebDAV 返回路径过长");
            href.append(value, count);
        } else if (in_href && kind == XmlNodeType_EndElement && std::wstring_view(local) == L"href") {
            const auto name = BackupNameFromHref(endpoint_, href);
            if (!name.empty()) names.push_back(name);
            if (names.size() > 10000) throw std::runtime_error("备份列表过大");
            in_href = false;
        }
    }
    CheckCom(result);
    std::sort(names.rbegin(), names.rend());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}
void WebDavClient::Upload(const std::string& name, const Bytes& content) {
    if (!SafeName(name) || content.size() > MaxFileBytes) throw std::runtime_error("备份文件名或大小无效");
    const auto directory = Request(L"MKCOL", L"", {});
    if (directory.status != 201 && directory.status != 405) throw std::runtime_error("创建备份目录失败，HTTP " + std::to_string(directory.status));
    const auto temporary = Wide("maplessh-upload-" + RandomId() + ".tmp");
    try {
        const auto response = Request(L"PUT", temporary, content, L"If-None-Match: *\r\nContent-Type: application/octet-stream\r\n");
        if (response.status != 201 && response.status != 204) throw std::runtime_error("备份上传失败，HTTP " + std::to_string(response.status));
        auto host = endpoint_.host;
        if (host.find(L':') != std::wstring::npos && !host.starts_with(L"[")) host = L"[" + host + L"]";
        const auto origin = std::wstring(endpoint_.secure ? L"https://" : L"http://") + host +
            (endpoint_.port == (endpoint_.secure ? 443 : 80) ? L"" : L":" + std::to_wstring(endpoint_.port));
        const auto headers = L"Destination: " + origin + endpoint_.path + Wide(name) + L"\r\nOverwrite: F\r\n";
        const auto published = Request(L"MOVE", temporary, {}, headers.c_str());
        if (published.status != 201) throw std::runtime_error("备份发布失败（不会覆盖同名文件），HTTP " + std::to_string(published.status));
        const auto returned = Download(name);
        if (returned.size() != content.size() || Sha256(returned) != Sha256(content)) throw std::runtime_error("备份回读校验失败，不能确认上传成功");
    } catch (...) {
        if (!cancelled_) { try { Request(L"DELETE", temporary, {}); } catch (...) {} }
        throw;
    }
}
Bytes WebDavClient::Download(const std::string& name) {
    if (!SafeName(name)) throw std::runtime_error("备份文件名无效");
    auto response = Request(L"GET", Wide(name), {});
    if (response.status != 200) throw std::runtime_error("备份下载失败，HTTP " + std::to_string(response.status));
    return std::move(response.body);
}
std::string WebDavClient::NewBackupName() {
    const auto now = std::time(nullptr);
    std::tm calendar{};
    gmtime_s(&calendar, &now);
    std::array<char, 32> timestamp{};
    std::strftime(timestamp.data(), timestamp.size(), "%Y%m%dT%H%M%SZ", &calendar);
    return "maplessh-" + std::string(timestamp.data()) + "-" + RandomId() + ".msb";
}
}
