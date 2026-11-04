#include "command_input.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Callable> void Rejects(Callable action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "unsafe or empty commands accepted");
}
}
int main() {
    try {
        const auto single = maple::PrepareCommands("pwd");
        Require(single.input == "pwd\r" && single.lines == 1, "single command did not execute exactly once");
        for (const std::string ending : {"\n", "\r", "\r\n"}) {
            const auto command = maple::PrepareCommands("pwd" + ending);
            Require(command.input == "pwd\r" && command.lines == 1, "trailing newline caused duplicate execution");
            const auto multiple = maple::PrepareCommands("pwd" + ending + "whoami" + ending);
            Require(multiple.input == "pwd\rwhoami\r" && multiple.lines == 2, "pasted line endings not normalized");
        }
        const auto script = maple::PrepareCommands("cat <<'EOF'\r\n中文\r\n\r\nEOF\r\nprintf '结束'\n");
        Require(script.input == "cat <<'EOF'\r中文\r\rEOF\rprintf '结束'\r" && script.lines == 5, "script structure or UTF-8 changed");
        Require(maple::PrepareCommands("  printf 'a  b'\n\tprintf c  ").input == "  printf 'a  b'\r\tprintf c  \r", "command indentation or trailing spaces changed");
        for (const std::string blank : {"", " ", "\t\r\n  "}) Rejects([&] { maple::PrepareCommands(blank); });
        for (const char control : {'\0', '\x03', '\x1b', '\x7f'}) {
            Rejects([&] { maple::PrepareCommands(std::string("echo test") + control); });
        }
        Require(maple::PrepareCommands(std::string(1024 * 1024 - 1, 'x')).input.size() == 1024 * 1024, "maximum command size rejected");
        Rejects([] { maple::PrepareCommands(std::string(1024 * 1024, 'x')); });
        Rejects([] { maple::PrepareCommands(std::string(1024 * 1024 + 1, 'x')); });
        std::cout << "Command execution framing, multiline scripts, UTF-8, whitespace and limits PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
