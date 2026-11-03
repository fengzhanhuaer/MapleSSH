#pragma once
#include "model.hpp"
#include "crypto.hpp"
#include <atomic>
#include <string>
#include <vector>
namespace maple {
struct WebEndpoint {
    std::wstring host;
    std::wstring path;
    std::wstring base_path;
    std::uint16_t port = 443;
    bool secure = true;
};
WebEndpoint ParseEndpoint(const WebDavSettings& settings);
std::string BackupNameFromHref(const WebEndpoint& endpoint, std::wstring_view href);
class WebDavClient {
public:
    explicit WebDavClient(WebDavSettings settings);
    void Cancel() { cancelled_ = true; }
    void TestLogin();
    std::vector<std::string> List();
    void Upload(const std::string& name, const Bytes& content);
    Bytes Download(const std::string& name);
    static std::string NewBackupName();
private:
    struct Response { unsigned long status; Bytes body; };
    Response Request(const wchar_t* method, std::wstring_view suffix, const Bytes& body,
        const wchar_t* headers = L"", std::size_t limit = MaxFileBytes, bool use_base_path = false);
    WebDavSettings settings_;
    WebEndpoint endpoint_;
    std::atomic<bool> cancelled_{false};
};
}
