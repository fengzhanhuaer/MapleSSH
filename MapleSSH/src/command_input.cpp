#include "command_input.hpp"
#include <stdexcept>

namespace maple {
PreparedCommands PrepareCommands(std::string_view text) {
    constexpr std::size_t limit = 1024 * 1024;
    if (text.size() > limit) throw std::runtime_error("命令内容超过 1 MiB");
    PreparedCommands result;
    result.input.reserve(text.size() + 1);
    bool has_content = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        if (character == '\r' || character == '\n') {
            if (character == '\r' && index + 1 < text.size() && text[index + 1] == '\n') ++index;
            result.input += '\r'; ++result.lines;
        } else {
            if ((character < 32 && character != '\t') || character == 127) throw std::runtime_error("命令包含不支持的控制字符");
            result.input += static_cast<char>(character);
            if (character != ' ' && character != '\t') has_content = true;
        }
    }
    if (!has_content) throw std::runtime_error("请输入命令");
    if (result.input.back() != '\r') { result.input += '\r'; ++result.lines; }
    if (result.input.size() > limit) throw std::runtime_error("命令内容超过 1 MiB");
    return result;
}
}
