#pragma once
#include <wx/wx.h>
#pragma push_macro("small")
#undef small
#include <vterm.h>
#pragma pop_macro("small")
#include <deque>
#include <functional>
#include <vector>
namespace maple {
class TerminalPanel : public wxPanel {
public:
    explicit TerminalPanel(wxWindow* parent);
    ~TerminalPanel() override;
    void Feed(std::string_view text);
    void SetSender(std::function<void(std::string)> sender) { sender_ = std::move(sender); }
    void SetResize(std::function<void(int, int)> resize) { resize_ = std::move(resize); }
    void Copy();
    void Paste();
    int Rows() const { return rows_; }
    int Columns() const { return columns_; }
private:
    static int PushLine(int columns, const VTermScreenCell* cells, void* user);
    static int PopLine(int columns, VTermScreenCell* cells, void* user);
    static int Property(VTermProp property, VTermValue* value, void* user);
    static void Output(const char* bytes, std::size_t size, void* user);
    VTermScreenCell Cell(int absolute_row, int column) const;
    wxPoint Position(const wxPoint& point) const;
    void Paint(wxPaintEvent& event);
    void Key(wxKeyEvent& event);
    void Character(wxKeyEvent& event);
    void Recalculate();
    void SendKey(VTermKey key, VTermModifier modifiers);
    bool Selected(int row, int column) const;
    VTerm* terminal_ = nullptr;
    VTermScreen* screen_ = nullptr;
    std::deque<std::vector<VTermScreenCell>> history_;
    std::size_t history_cells_ = 0;
    int rows_ = 24;
    int columns_ = 80;
    int font_size_ = 11;
    int cell_width_ = 9;
    int cell_height_ = 18;
    int scroll_offset_ = 0;
    bool alternate_ = false;
    bool cursor_visible_ = true;
    bool dragging_ = false;
    bool has_selection_ = false;
    wxPoint anchor_;
    wxPoint selection_end_;
    std::function<void(std::string)> sender_;
    std::function<void(int, int)> resize_;
};
}
