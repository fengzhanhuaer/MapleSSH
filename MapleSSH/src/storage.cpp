#include "storage.hpp"
#include <windows.h>
#include <fstream>
#include <stdexcept>

namespace maple {
Bytes ReadBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("无法读取配置文件");
    const auto size = stream.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(MaxFileBytes)) throw std::runtime_error("文件超过 32 MiB 或无法读取");
    Bytes content(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!content.empty()) stream.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof()) throw std::runtime_error("文件读取不完整或读取期间变化");
    return content;
}
void WriteAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> content) {
    if (content.size() > MaxFileBytes) throw std::runtime_error("文件超过 32 MiB");
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp." + std::filesystem::path(RandomId()).wstring();
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建配置临时文件，请检查程序目录写入权限");
    DWORD written = 0;
    const bool saved = WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size() && FlushFileBuffers(handle);
    const bool closed = CloseHandle(handle) != 0;
    if (!saved || !closed || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("保存失败，原配置未被替换");
    }
}
std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw std::runtime_error("无法获取程序路径");
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}
Storage::Storage(std::filesystem::path directory) : directory_(std::move(directory)) {
    std::filesystem::create_directories(directory_);
}
bool Storage::Exists() const { return std::filesystem::exists(directory_ / "vault.msv"); }
Vault Storage::Load(std::string_view password) const {
    return ParseVault(Decrypt(ReadBytes(directory_ / "vault.msv"), password, "vault"));
}
void Storage::Save(const Vault& vault, std::string_view password) const {
    WriteAtomic(directory_ / "vault.msv", Encrypt(SerializeVault(vault), password, "vault"));
}
void Storage::SaveRecoveryCopy() const {
    if (Exists()) WriteAtomic(directory_ / ("vault.before-restore." + RandomId() + ".msv"), ReadBytes(directory_ / "vault.msv"));
}
}
