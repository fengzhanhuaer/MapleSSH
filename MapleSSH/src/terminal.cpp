#include "terminal.hpp"
#include "terminal_cells.hpp"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <algorithm>
#include <tuple>

namespace maple {
namespace {
wxString CellText(const VTermScreenCell& cell) {
    wxString output;
    for (const auto character : cell.chars) { if (!character) break; output += wxUniChar(character); }
    if (output.empty()) output = " ";
    return output;
}
VTermModifier Modifiers(const wxKeyEvent& event) {
    int modifiers = VTERM_MOD_NONE;
    if (event.ShiftDown()) modifiers |= VTERM_MOD_SHIFT;
    if (event.AltDown()) modifiers |= VTERM_MOD_ALT;
    if (event.ControlDown()) modifiers |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(modifiers);
}
}
TerminalPanel::TerminalPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(17, 22, 31));
    terminal_ = vterm_new(rows_, columns_);
    if (!terminal_) throw std::runtime_error("无法分配终端");
    vterm_set_utf8(terminal_, 1);
    screen_ = vterm_obtain_screen(terminal_);
    static const VTermScreenCallbacks callbacks{nullptr, nullptr, nullptr, Property, nullptr, nullptr, PushLine, PopLine, nullptr};
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_output_set_callback(terminal_, Output, this);
    vterm_screen_enable_altscreen(screen_, 1);
    VTermColor foreground;
    VTermColor background;
    vterm_color_rgb(&foreground, 221, 228, 238);
    vterm_color_rgb(&background, 17, 22, 31);
    vterm_screen_set_default_colors(screen_, &foreground, &background);
    vterm_screen_reset(screen_, 1);
    Bind(wxEVT_PAINT, &TerminalPanel::Paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Recalculate(); event.Skip(); });
    Bind(wxEVT_KEY_DOWN, &TerminalPanel::Key, this);
    Bind(wxEVT_CHAR, &TerminalPanel::Character, this);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        SetFocus(); anchor_ = selection_end_ = Position(event.GetPosition());
        dragging_ = true; has_selection_ = false; CaptureMouse(); Refresh(false);
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        if (dragging_) { selection_end_ = Position(event.GetPosition()); has_selection_ = selection_end_ != anchor_; Refresh(false); }
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { dragging_ = false; if (HasCapture()) ReleaseMouse(); });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragging_ = false; });
    Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { if (has_selection_) Copy(); else Paste(); });
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
        if (event.ControlDown()) { font_size_ = std::clamp(font_size_ + (event.GetWheelRotation() > 0 ? 1 : -1), 7, 30); Recalculate(); }
        else if (!alternate_) { scroll_offset_ = std::clamp(scroll_offset_ + (event.GetWheelRotation() > 0 ? 3 : -3), 0, static_cast<int>(history_.size())); Refresh(false); }
    });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { vterm_state_focus_in(vterm_obtain_state(terminal_)); Refresh(false); event.Skip(); });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { vterm_state_focus_out(vterm_obtain_state(terminal_)); Refresh(false); event.Skip(); });
    Recalculate();
}
TerminalPanel::~TerminalPanel() { if (HasCapture()) ReleaseMouse(); if (terminal_) vterm_free(terminal_); }
void TerminalPanel::Output(const char* bytes, std::size_t size, void* user) {
    auto* panel = static_cast<TerminalPanel*>(user);
    if (panel->sender_) panel->sender_(std::string(bytes, size));
}
int TerminalPanel::Property(VTermProp property, VTermValue* value, void* user) {
    auto* panel = static_cast<TerminalPanel*>(user);
    if (property == VTERM_PROP_ALTSCREEN) { panel->alternate_ = value->boolean != 0; panel->scroll_offset_ = 0; panel->has_selection_ = false; }
    if (property == VTERM_PROP_CURSORVISIBLE) panel->cursor_visible_ = value->boolean != 0;
    return 1;
}
int TerminalPanel::PushLine(int columns, const VTermScreenCell* cells, void* user) {
    auto* panel = static_cast<TerminalPanel*>(user);
    panel->history_.emplace_back(cells, cells + columns);
    panel->history_cells_ += columns;
    if (panel->scroll_offset_) ++panel->scroll_offset_;
    while (panel->history_.size() > 2000 || panel->history_cells_ > 100000) {
        panel->history_cells_ -= panel->history_.front().size();
        panel->history_.pop_front();
        if (panel->anchor_.y > 0) --panel->anchor_.y;
        if (panel->selection_end_.y > 0) --panel->selection_end_.y;
    }
    panel->scroll_offset_ = std::min(panel->scroll_offset_, static_cast<int>(panel->history_.size()));
    return 1;
}
int TerminalPanel::PopLine(int columns, VTermScreenCell* cells, void* user) {
    auto* panel = static_cast<TerminalPanel*>(user);
    if (panel->history_.empty()) return 0;
    const auto& line = panel->history_.back();
    std::fill(cells, cells + columns, VTermScreenCell{});
    std::copy_n(line.begin(), std::min<std::size_t>(line.size(), columns), cells);
    panel->history_cells_ -= line.size();
    panel->history_.pop_back();
    panel->scroll_offset_ = std::min(panel->scroll_offset_, static_cast<int>(panel->history_.size()));
    return 1;
}
void TerminalPanel::Feed(std::string_view text) {
    vterm_input_write(terminal_, text.data(), text.size());
    vterm_screen_flush_damage(screen_);
    Refresh(false);
}
void TerminalPanel::FeedStatus(std::string_view text) {
    VTermPos cursor{};
    vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &cursor);
    Feed(FormatTerminalStatus(text, cursor.col == 0));
}
void TerminalPanel::Recalculate() {
    SetFont(wxFont(wxFontInfo(font_size_).FaceName("Cascadia Mono").Family(wxFONTFAMILY_TELETYPE)));
    wxClientDC measure(this);
    measure.SetFont(GetFont());
    const auto extent = measure.GetTextExtent("M");
    cell_width_ = std::max(1, extent.x);
    cell_height_ = std::max(1, extent.y);
    const auto size = GetClientSize();
    const auto new_rows = std::clamp(size.y / cell_height_, 2, 300);
    const auto new_columns = std::clamp(size.x / cell_width_, 2, 500);
    if (rows_ != new_rows || columns_ != new_columns) {
        rows_ = new_rows; columns_ = new_columns;
        vterm_set_size(terminal_, rows_, columns_);
        if (resize_) resize_(rows_, columns_);
        has_selection_ = false;
    }
    Refresh(false);
}
VTermScreenCell TerminalPanel::Cell(int absolute_row, int column) const {
    VTermScreenCell cell{};
    if (absolute_row >= 0 && absolute_row < static_cast<int>(history_.size())) {
        const auto& line = history_[absolute_row];
        if (column >= 0 && column < static_cast<int>(line.size())) cell = line[column];
    } else {
        const auto row = absolute_row - static_cast<int>(history_.size());
        if (row >= 0 && row < rows_ && column >= 0 && column < columns_) vterm_screen_get_cell(screen_, {row, column}, &cell);
    }
    return cell;
}
wxPoint TerminalPanel::Position(const wxPoint& point) const {
    return {std::clamp(point.x / cell_width_, 0, columns_ - 1),
        static_cast<int>(history_.size()) - scroll_offset_ + std::clamp(point.y / cell_height_, 0, rows_ - 1)};
}
bool TerminalPanel::Selected(int row, int column) const {
    if (!has_selection_) return false;
    auto first = std::pair(anchor_.y, anchor_.x);
    auto last = std::pair(selection_end_.y, selection_end_.x);
    if (last < first) std::swap(first, last);
    return std::pair(row, column) >= first && std::pair(row, column) <= last;
}
void TerminalPanel::Paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC canvas(this);
    canvas.SetBackground(wxBrush(wxColour(17, 22, 31)));
    canvas.Clear();
    const auto normal_font = GetFont();
    for (int row = 0; row < rows_; ++row) {
        const int absolute_row = static_cast<int>(history_.size()) - scroll_offset_ + row;
        for (int column = 0; column < columns_; ++column) {
            auto cell = Cell(absolute_row, column);
            if (IsContinuation(cell)) continue;
            vterm_screen_convert_color_to_rgb(screen_, &cell.fg);
            vterm_screen_convert_color_to_rgb(screen_, &cell.bg);
            wxColour foreground(cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue);
            wxColour background(cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue);
            if (cell.attrs.reverse) std::swap(foreground, background);
            if (Selected(absolute_row, column)) { foreground = wxColour(255,255,255); background = wxColour(45,83,125); }
            const auto width = std::max(1, static_cast<int>(cell.width));
            canvas.SetPen(*wxTRANSPARENT_PEN);
            canvas.SetBrush(wxBrush(background));
            canvas.DrawRectangle(column * cell_width_, row * cell_height_, width * cell_width_, cell_height_);
            auto font = normal_font;
            if (cell.attrs.bold) font.MakeBold();
            if (cell.attrs.italic) font.MakeItalic();
            if (cell.attrs.underline) font.MakeUnderlined();
            canvas.SetFont(font);
            canvas.SetTextForeground(foreground);
            if (!cell.attrs.conceal) canvas.DrawText(CellText(cell), column * cell_width_, row * cell_height_);
        }
    }
    if (!scroll_offset_ && HasFocus() && cursor_visible_) {
        VTermPos cursor{};
        vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &cursor);
        canvas.SetPen(wxPen(wxColour(150, 205, 240)));
        canvas.SetBrush(*wxTRANSPARENT_BRUSH);
        canvas.DrawRectangle(cursor.col * cell_width_, cursor.row * cell_height_, cell_width_, cell_height_);
    }
}
void TerminalPanel::SendKey(VTermKey key, VTermModifier modifiers) {
    scroll_offset_ = 0; has_selection_ = false;
    vterm_keyboard_key(terminal_, key, modifiers);
    Refresh(false);
}
void TerminalPanel::Key(wxKeyEvent& event) {
    const auto code = event.GetKeyCode();
    if (event.ControlDown() && event.ShiftDown() && code == 'C') { Copy(); return; }
    if ((event.ControlDown() && event.ShiftDown() && code == 'V') || (event.ShiftDown() && code == WXK_INSERT)) { Paste(); return; }
    VTermKey key = VTERM_KEY_NONE;
    switch (code) {
        case WXK_RETURN: case WXK_NUMPAD_ENTER: key = VTERM_KEY_ENTER; break;
        case WXK_TAB: key = VTERM_KEY_TAB; break;
        case WXK_BACK: key = VTERM_KEY_BACKSPACE; break;
        case WXK_ESCAPE: key = VTERM_KEY_ESCAPE; break;
        case WXK_UP: key = VTERM_KEY_UP; break;
        case WXK_DOWN: key = VTERM_KEY_DOWN; break;
        case WXK_LEFT: key = VTERM_KEY_LEFT; break;
        case WXK_RIGHT: key = VTERM_KEY_RIGHT; break;
        case WXK_HOME: key = VTERM_KEY_HOME; break;
        case WXK_END: key = VTERM_KEY_END; break;
        case WXK_PAGEUP: key = VTERM_KEY_PAGEUP; break;
        case WXK_PAGEDOWN: key = VTERM_KEY_PAGEDOWN; break;
        case WXK_INSERT: key = VTERM_KEY_INS; break;
        case WXK_DELETE: key = VTERM_KEY_DEL; break;
        default: if (code >= WXK_F1 && code <= WXK_F12) key = static_cast<VTermKey>(VTERM_KEY_FUNCTION(code - WXK_F1 + 1));
    }
    if (key != VTERM_KEY_NONE) { SendKey(key, Modifiers(event)); return; }
    event.Skip();
}
void TerminalPanel::Character(wxKeyEvent& event) {
    const auto code = event.GetUnicodeKey();
    if (code == WXK_NONE || code == 0 || code > 0x10ffff) { event.Skip(); return; }
    scroll_offset_ = 0; has_selection_ = false;
    vterm_keyboard_unichar(terminal_, static_cast<std::uint32_t>(code), Modifiers(event));
    Refresh(false);
}
void TerminalPanel::Copy() {
    if (!has_selection_) return;
    auto first = std::pair(anchor_.y, anchor_.x);
    auto last = std::pair(selection_end_.y, selection_end_.x);
    if (last < first) std::swap(first, last);
    wxString output;
    for (int row = first.first; row <= last.first; ++row) {
        wxString line;
        const auto left = row == first.first ? first.second : 0;
        const auto right = row == last.first ? last.second : columns_ - 1;
        for (int column = left; column <= right; ++column) {
            const auto cell = Cell(row, column);
            if (IsContinuation(cell)) continue;
            line += CellText(cell);
        }
        line.Trim();
        output += line;
        if (row != last.first) output += '\n';
    }
    if (wxTheClipboard->Open()) { wxTheClipboard->SetData(new wxTextDataObject(output)); wxTheClipboard->Close(); }
}
void TerminalPanel::Paste() {
    wxTextDataObject data;
    if (!wxTheClipboard->Open()) return;
    const auto received = wxTheClipboard->GetData(data);
    wxTheClipboard->Close();
    if (!received) return;
    auto text = data.GetText();
    if (text.length() > 1024 * 1024) { wxMessageBox(wxS("粘贴内容过大"), "MapleSSH", wxOK | wxICON_WARNING, this); return; }
    if ((text.Contains("\n") || text.Contains("\r")) && wxMessageBox(wxS("内容包含多行，可能立即执行命令。确认粘贴？"), wxS("确认粘贴"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
    text.Replace("\r\n", "\n");
    vterm_keyboard_start_paste(terminal_);
    for (const auto character : text) vterm_keyboard_unichar(terminal_, character.GetValue(), VTERM_MOD_NONE);
    vterm_keyboard_end_paste(terminal_);
    scroll_offset_ = 0; Refresh(false);
}
}
