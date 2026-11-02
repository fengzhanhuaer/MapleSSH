#include <vterm.h>
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
        vterm_free(terminal);
        std::cout << "UTF-8, colors, cursor, resize and alternate screen PASS\n";
        return 0;
    } catch (const std::exception& error) {
        if (terminal) vterm_free(terminal);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
