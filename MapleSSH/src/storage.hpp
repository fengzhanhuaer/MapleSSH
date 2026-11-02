#pragma once
#include "model.hpp"
#include "crypto.hpp"
#include <filesystem>
namespace maple {
Bytes ReadBytes(const std::filesystem::path& path);
void WriteAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> content);
std::filesystem::path ExecutableDirectory();
class Storage {
public:
    explicit Storage(std::filesystem::path directory);
    bool Exists() const;
    Vault Load(std::string_view password) const;
    void Save(const Vault& vault, std::string_view password) const;
    void SaveRecoveryCopy() const;
    const std::filesystem::path& Directory() const { return directory_; }
private:
    std::filesystem::path directory_;
};
}
