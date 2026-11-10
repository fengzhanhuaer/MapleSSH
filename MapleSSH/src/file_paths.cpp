#include "file_paths.hpp"
#include <windows.h>
#include <algorithm>
#include <stdexcept>

namespace maple {
void ValidateLocalFilePath(const std::filesystem::path& path) {
    const auto native = path.native();
    if (native.empty() || !path.is_absolute() || !path.has_filename() || native.find(L'\0') != std::wstring::npos || native.starts_with(L"\\\\.\\") || native.starts_with(L"\\\\?\\"))
        throw std::runtime_error("本机文件必须是普通 Win32 绝对文件路径");
    for (const auto& component : path.relative_path()) {
        const auto name = component.native();
        if (name == L"." || name == L"..") continue;
        if (name.empty() || name.ends_with(L'.') || name.ends_with(L' ') || name.find_first_of(L"<>:\"|?*") != std::wstring::npos ||
            std::any_of(name.begin(), name.end(), [](wchar_t character) { return character < 32; })) throw std::runtime_error("本机文件路径含无效名称或数据流");
        auto base = name.substr(0, name.find(L'.'));
        std::transform(base.begin(), base.end(), base.begin(), [](wchar_t character) { return character >= L'a' && character <= L'z' ? character - L'a' + L'A' : character; });
        if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
            (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) && base[3] >= L'1' && base[3] <= L'9')) throw std::runtime_error("本机文件不能使用设备保留名称");
    }
}
void ValidateRemotePath(std::string_view path) {
    if (path.empty() || path.size() > 4096 || std::any_of(path.begin(), path.end(), [](unsigned char character) { return character < 32 || character == 127; }) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0))
        throw std::runtime_error("路径为空、过长、不是 UTF-8 或含控制字符");
}
std::string JoinRemotePath(std::string_view directory, std::string_view name) {
    ValidateRemotePath(directory); ValidateRemotePath(name);
    if (name == "." || name == ".." || name.find('/') != std::string_view::npos) throw std::runtime_error("文件名无效");
    auto path = std::string(directory);
    if (!path.ends_with('/')) path += '/';
    path += name; ValidateRemotePath(path); return path;
}
std::string RemoteParent(std::string path) {
    ValidateRemotePath(path);
    while (path.size() > 1 && path.ends_with('/')) path.pop_back();
    const auto separator = path.find_last_of('/');
    return separator == std::string::npos ? "." : separator == 0 ? "/" : path.substr(0, separator);
}
std::string TerminalCdCommand(std::string_view directory) {
    ValidateRemotePath(directory);
    if (!directory.starts_with('/')) throw std::runtime_error("终端目录必须是绝对路径");
    std::string command = "cd -- '";
    for (const auto character : directory) command += character == '\'' ? "'\"'\"'" : std::string(1, character);
    return command + "'\r";
}
std::string RemoteBrowsePath(std::string_view directory, std::string_view requested) {
    ValidateRemotePath(directory); ValidateRemotePath(requested);
    if (requested.starts_with('/') || directory == ".") return std::string(requested);
    auto path = std::string(directory);
    if (!path.ends_with('/')) path += '/';
    path += requested; ValidateRemotePath(path); return path;
}
}
