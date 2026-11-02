#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
namespace maple {
enum class Trust { Unknown, Match, Mismatch };
class HostKeys {
public:
    explicit HostKeys(std::filesystem::path path) : path_(std::move(path)) {}
    Trust Check(const std::string& host, std::uint16_t port, const std::string& algorithm, const std::string& key);
    void Accept(const std::string& host, std::uint16_t port, const std::string& algorithm, const std::string& key);
private:
    std::filesystem::path path_;
    std::mutex mutex_;
};
}
