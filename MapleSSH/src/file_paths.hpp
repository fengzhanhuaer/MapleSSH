#pragma once
#include <string>
#include <string_view>
#include <filesystem>

namespace maple {
void ValidateRemotePath(std::string_view path);
void ValidateLocalFilePath(const std::filesystem::path& path);
std::string JoinRemotePath(std::string_view directory, std::string_view name);
std::string RemoteParent(std::string path);
std::string RemoteBrowsePath(std::string_view directory, std::string_view requested);
std::string TerminalCdCommand(std::string_view directory);
}
