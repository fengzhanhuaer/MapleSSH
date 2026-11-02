#include "host_keys.hpp"
#include "storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace maple {
namespace {
using Json = nlohmann::json;
std::string Canonical(std::string host) {
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return host;
}
Json ReadHosts(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {{"version", 1}, {"hosts", Json::array()}};
    const auto bytes = ReadBytes(path);
    auto object = Json::parse(bytes);
    if (!object.is_object() || object.size() != 2 || object.at("version") != 1 ||
        !object.at("hosts").is_array() || object.at("hosts").size() > 4096)
        throw std::runtime_error("本机主机信任文件损坏，不能继续连接");
    for (const auto& entry : object.at("hosts")) {
        entry.at("host").get<std::string>();
        entry.at("port").get<std::uint16_t>();
        entry.at("algorithm").get<std::string>();
        entry.at("key").get<std::string>();
    }
    return object;
}
Trust Find(const Json& object, const std::string& host, std::uint16_t port, const std::string& algorithm, const std::string& key) {
    for (const auto& entry : object.at("hosts"))
        if (entry.at("host") == Canonical(host) && entry.at("port") == port)
            return entry.at("algorithm") == algorithm && entry.at("key") == key ? Trust::Match : Trust::Mismatch;
    return Trust::Unknown;
}
}
Trust HostKeys::Check(const std::string& host, std::uint16_t port, const std::string& algorithm, const std::string& key) {
    const std::lock_guard lock(mutex_);
    return Find(ReadHosts(path_), host, port, algorithm, key);
}
void HostKeys::Accept(const std::string& host, std::uint16_t port, const std::string& algorithm, const std::string& key) {
    const std::lock_guard lock(mutex_);
    auto object = ReadHosts(path_);
    const auto trust = Find(object, host, port, algorithm, key);
    if (trust == Trust::Mismatch) throw std::runtime_error("主机密钥已变化，拒绝自动替换信任记录");
    if (trust == Trust::Match) return;
    if (object.at("hosts").size() >= 4096) throw std::runtime_error("本机主机信任记录过多");
    object["hosts"].push_back({{"host", Canonical(host)}, {"port", port}, {"algorithm", algorithm}, {"key", key}});
    const auto text = object.dump();
    WriteAtomic(path_, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
}
