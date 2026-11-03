#pragma once
#include <vterm.h>
#include <cstdint>
#include <string>
#include <string_view>
namespace maple {
inline bool IsContinuation(const VTermScreenCell& cell) {
    return cell.chars[0] == static_cast<std::uint32_t>(-1);
}
inline std::string FormatTerminalStatus(std::string_view text, bool at_line_start) {
    return std::string(at_line_start ? "" : "\r\n") + "\x1b[90m[MapleSSH] " + std::string(text) + "\x1b[0m\r\n";
}
}
