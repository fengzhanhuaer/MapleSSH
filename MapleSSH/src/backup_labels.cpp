#include "backup_labels.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string_view>

namespace maple {
namespace {
std::string LocalBackupTime(std::string_view name) {
    constexpr std::string_view prefix = "maplessh-";
    constexpr std::string_view extension = ".msb";
    if (name.size() != prefix.size() + 16 + 1 + 32 + extension.size() || !name.starts_with(prefix) || !name.ends_with(extension)) return {};
    const auto timestamp = name.substr(prefix.size(), 16);
    const auto identifier = name.substr(prefix.size() + 17, 32);
    if (timestamp[8] != 'T' || timestamp[15] != 'Z' || name[prefix.size() + 16] != '-') return {};
    for (std::size_t index = 0; index < timestamp.size(); ++index) {
        if (index != 8 && index != 15 && (timestamp[index] < '0' || timestamp[index] > '9')) return {};
    }
    if (!std::all_of(identifier.begin(), identifier.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
    })) return {};
    const auto number = [&](std::size_t offset, std::size_t count) {
        unsigned int value = 0;
        for (std::size_t index = offset; index < offset + count; ++index) value = value * 10 + timestamp[index] - '0';
        return static_cast<WORD>(value);
    };
    SYSTEMTIME utc{};
    utc.wYear = number(0, 4); utc.wMonth = number(4, 2); utc.wDay = number(6, 2);
    utc.wHour = number(9, 2); utc.wMinute = number(11, 2); utc.wSecond = number(13, 2);
    FILETIME validated{};
    SYSTEMTIME local{};
    if (!SystemTimeToFileTime(&utc, &validated) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    char label[32]{};
    std::snprintf(label, sizeof(label), "%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned int>(local.wYear),
        static_cast<unsigned int>(local.wMonth), static_cast<unsigned int>(local.wDay), static_cast<unsigned int>(local.wHour),
        static_cast<unsigned int>(local.wMinute), static_cast<unsigned int>(local.wSecond));
    return label;
}
}
std::vector<std::string> BackupLabels(const std::vector<std::string>& names) {
    std::vector<std::string> labels;
    std::map<std::string, std::size_t> totals;
    for (const auto& name : names) {
        auto label = LocalBackupTime(name);
        if (label.empty()) label = name;
        ++totals[label]; labels.push_back(std::move(label));
    }
    std::map<std::string, std::size_t> occurrences;
    for (auto& label : labels) {
        if (totals.at(label) > 1) {
            const auto occurrence = ++occurrences[label];
            label += "（备份 " + std::to_string(occurrence) + "）";
        }
    }
    return labels;
}
}
