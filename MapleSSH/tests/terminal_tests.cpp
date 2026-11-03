#include <vterm.h>
#include "../src/terminal_cells.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

int main() {
    VTerm* terminal = vterm_new(24, 80);
    try {
        if (!terminal) throw std::runtime_error("libvterm allocation failed");
        vterm_set_utf8(terminal, 1);
        auto* screen = vterm_obtain_screen(terminal);
        vterm_screen_enable_altscreen(screen, 1);
        vterm_screen_reset(screen, 1);
        const char* input = "\x1b[2J\x1b[H\x1b[31m红色\x1b[0m";
        vterm_input_write(terminal, input, std::strlen(input));
        vterm_screen_flush_damage(screen);
        VTermScreenCell cell{};
        if (!vterm_screen_get_cell(screen, {0, 0}, &cell) || cell.chars[0] != 0x7ea2 || cell.width != 2)
            throw std::runtime_error("UTF-8 wide character or cursor sequence failed");
        if (!VTERM_COLOR_IS_INDEXED(&cell.fg) || cell.fg.indexed.idx != 1)
            throw std::runtime_error("ANSI foreground color failed");
        VTermScreenCell continuation{};
        if (!vterm_screen_get_cell(screen, {0, 1}, &continuation) || !maple::IsContinuation(continuation))
            throw std::runtime_error("wide character continuation would be drawn over Chinese text");
        if (maple::IsContinuation(cell)) throw std::runtime_error("Chinese leading cell hidden");
        vterm_set_size(terminal, 30, 100);
        int rows = 0;
        int columns = 0;
        vterm_get_size(terminal, &rows, &columns);
        if (rows != 30 || columns != 100) throw std::runtime_error("terminal resize failed");
        const char* alternate = "\x1b[?1049hALT\x1b[?1049l";
        vterm_input_write(terminal, alternate, std::strlen(alternate));
        vterm_screen_flush_damage(screen);
        if (!vterm_screen_get_cell(screen, {0, 0}, &cell) || cell.chars[0] != 0x7ea2)
            throw std::runtime_error("alternate screen restoration failed");
        vterm_screen_reset(screen, 1);
        for (const auto* state : {"正在连接", "正在认证", "已连接"}) {
            VTermPos cursor{};
            vterm_state_get_cursorpos(vterm_obtain_state(terminal), &cursor);
            const auto status = maple::FormatTerminalStatus(state, cursor.col == 0);
            vterm_input_write(terminal, status.data(), status.size());
        }
        vterm_screen_flush_damage(screen);
        for (int row = 0; row < 3; ++row) {
            if (!vterm_screen_get_cell(screen, {row, 0}, &cell) || cell.chars[0] != '[')
                throw std::runtime_error("connection statuses contain extra blank lines");
        }
        VTermPos cursor{};
        vterm_state_get_cursorpos(vterm_obtain_state(terminal), &cursor);
        if (cursor.row != 3 || cursor.col != 0) throw std::runtime_error("status trailing newline failed");
        const char* partial = "prompt>";
        vterm_input_write(terminal, partial, std::strlen(partial));
        vterm_state_get_cursorpos(vterm_obtain_state(terminal), &cursor);
        const auto disconnected = maple::FormatTerminalStatus("已断开", cursor.col == 0);
        vterm_input_write(terminal, disconnected.data(), disconnected.size());
        vterm_screen_flush_damage(screen);
        if (!vterm_screen_get_cell(screen, {3, 0}, &cell) || cell.chars[0] != 'p')
            throw std::runtime_error("status overwrote remote prompt");
        if (!vterm_screen_get_cell(screen, {4, 0}, &cell) || cell.chars[0] != '[')
            throw std::runtime_error("status did not start a new line after remote prompt");
        const char* remote = "remote\r\n\r\nnext";
        vterm_input_write(terminal, remote, std::strlen(remote));
        vterm_state_get_cursorpos(vterm_obtain_state(terminal), &cursor);
        if (cursor.row != 7 || cursor.col != 4) throw std::runtime_error("remote blank lines changed");
        vterm_free(terminal);
        std::cout << "UTF-8, colors, cursor, resize and alternate screen PASS\n";
        return 0;
    } catch (const std::exception& error) {
        if (terminal) vterm_free(terminal);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
