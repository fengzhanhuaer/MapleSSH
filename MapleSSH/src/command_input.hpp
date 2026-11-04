#pragma once
#include <cstddef>
#include <string>
#include <string_view>
namespace maple {
struct PreparedCommands {
    std::string input;
    std::size_t lines = 0;
};
PreparedCommands PrepareCommands(std::string_view text);
}
