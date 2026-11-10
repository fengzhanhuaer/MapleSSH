#include "file_browser.hpp"
#include "file_paths.hpp"
#include <wx/filedlg.h>
#include <wx/wrapsizer.h>
#include <algorithm>

namespace maple {
FileBrowserPanel::FileBrowserPanel(wxWindow* parent, std::function<std::shared_ptr<SftpClient>()> files,
    std::function<bool(std::string)> sender, std::function<void()> hide)
    : wxPanel(parent), files_(std::move(files)), sender_(std::move(sender)) {
    const wxColour background(17, 22, 31), foreground(221, 228, 238);
    SetBackgroundColour(background); SetForegroundColour(foreground);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* heading = new wxBoxSizer(wxHORIZONTAL);
    heading->Add(new wxStaticText(this, wxID_ANY, wxS("远程文件")), 1, wxALIGN_CENTER_VERTICAL);
    auto* collapse = new wxButton(this, wxID_ANY, wxS("收起"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    collapse->Bind(wxEVT_BUTTON, [hide = std::move(hide)](wxCommandEvent&) { hide(); });
    heading->Add(collapse, 0); layout->Add(heading, 0, wxEXPAND | wxALL, FromDIP(4));
    path_ = new wxTextCtrl(this, wxID_ANY, ".", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    path_->SetBackgroundColour(background); path_->SetForegroundColour(foreground);
    path_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Browse(path_->GetValue().ToStdString(wxConvUTF8)); });
    layout->Add(path_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4)); actions_.push_back(path_);
    auto* buttons = new wxWrapSizer(wxHORIZONTAL);
    auto add = [&](const wxString& label, auto action) {
        auto* button = new wxButton(this, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        button->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) { try { action(); } catch (const std::exception& error) { Error(error); } });
        buttons->Add(button, 0, wxRIGHT | wxBOTTOM, FromDIP(3)); actions_.push_back(button);
    };
    add(wxS("上一级"), [this] { Browse(RemoteParent(directory_)); });
    add(wxS("刷新"), [this] { Browse(directory_); });
    add(wxS("上传…"), [this] { Upload(); });
    add(wxS("下载…"), [this] { Download(); });
    add(wxS("cd 跳转"), [this] { EnterDirectory(directory_); });
    layout->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    list_->SetBackgroundColour(background); list_->SetTextColour(foreground);
    list_->InsertColumn(0, wxS("名称"), wxLIST_FORMAT_LEFT, FromDIP(155));
    list_->InsertColumn(1, wxS("类型"), wxLIST_FORMAT_LEFT, FromDIP(48));
    list_->InsertColumn(2, wxS("字节"), wxLIST_FORMAT_RIGHT, FromDIP(85));
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
        if (const auto* entry = Selected()) { if (entry->directory) Browse(entry->name == ".." ? RemoteParent(directory_) : JoinRemotePath(directory_, entry->name)); else if (entry->regular) Download(); }
    });
    list_->Bind(wxEVT_CONTEXT_MENU, &FileBrowserPanel::ContextMenu, this);
    layout->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4)); actions_.push_back(list_);
    auto* progress = new wxBoxSizer(wxHORIZONTAL);
    progress_ = new wxGauge(this, wxID_ANY, 100); progress->Add(progress_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    cancel_ = new wxButton(this, wxID_ANY, wxS("取消"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    cancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { try { if (client_ && !watching_.empty()) client_->Cancel(watching_); } catch (const std::exception& error) { Error(error); } });
    progress->Add(cancel_, 0); layout->Add(progress, 0, wxEXPAND | wxALL, FromDIP(4));
    status_ = new wxStaticText(this, wxID_ANY, wxS("SSH 连接后加载文件"), wxDefaultPosition, FromDIP(wxSize(0, 42)), wxST_ELLIPSIZE_END);
    layout->Add(status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));
    SetSizer(layout); cancel_->Disable(); for (auto* action : actions_) action->Disable();
}
void FileBrowserPanel::Error(const std::exception& error) {
    status_->SetLabel(wxString::FromUTF8(error.what())); status_->SetToolTip(status_->GetLabel());
}
void FileBrowserPanel::Watch(const std::string& id) { watching_ = id; status_->SetLabel(wxS("正在处理…")); progress_->SetValue(0); }
std::shared_ptr<SftpClient> FileBrowserPanel::ActiveFiles() const {
    auto current = files_();
    if (current != client_) throw std::runtime_error("SSH 会话已重连，请等待文件列表刷新后再操作");
    return current;
}
void FileBrowserPanel::Browse(std::string path) {
    try { if (!connected_) throw std::runtime_error("请先连接 SSH"); Watch(ActiveFiles()->Submit(FileOperation::List, RemoteBrowsePath(directory_, path))); }
    catch (const std::exception& error) { path_->ChangeValue(wxString::FromUTF8(directory_)); Error(error); }
}
const RemoteFile* FileBrowserPanel::Selected() const {
    const auto selected = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return selected >= 0 && static_cast<std::size_t>(selected) < entries_.size() ? &entries_[selected] : nullptr;
}
void FileBrowserPanel::Upload() {
    if (!connected_) return;
    wxFileDialog choose(this, wxS("选择上传文件"), {}, {}, wxS("所有文件 (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (choose.ShowModal() != wxID_OK) return;
    try {
        const auto name = choose.GetFilename().ToStdString(wxConvUTF8);
        const auto target = JoinRemotePath(directory_, name);
        const auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.name == name; });
        bool overwrite = false;
        if (existing != entries_.end()) {
            if (!existing->regular) throw std::runtime_error("目标不是普通文件，不能覆盖");
            if (wxMessageBox(wxS("覆盖远程同名文件？\n") + wxString::FromUTF8(target), wxS("确认上传覆盖"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
            overwrite = true;
        }
        Watch(ActiveFiles()->Submit(FileOperation::Upload, target, std::filesystem::path(choose.GetPath().ToStdWstring()), overwrite));
    } catch (const std::exception& error) { Error(error); }
}
void FileBrowserPanel::Download() {
    const auto* entry = Selected(); if (!connected_ || !entry) return;
    try {
        if (!entry->regular) throw std::runtime_error("第一版仅下载普通文件，不递归下载目录或符号链接");
        const auto remote = JoinRemotePath(directory_, entry->name);
        auto name = wxString::FromUTF8(entry->name);
        if (name.find_first_of(wxS("<>:\"/\\|?*")) != wxString::npos) name = wxS("download.bin");
        wxFileDialog choose(this, wxS("保存下载文件"), {}, name, wxS("所有文件 (*.*)|*.*"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (choose.ShowModal() != wxID_OK) return;
        const auto local = std::filesystem::path(choose.GetPath().ToStdWstring());
        Watch(ActiveFiles()->Submit(FileOperation::Download, remote, local, std::filesystem::exists(local)));
    } catch (const std::exception& error) { Error(error); }
}
void FileBrowserPanel::ContextMenu(wxContextMenuEvent& event) {
    try {
        if (event.GetPosition() != wxDefaultPosition) {
            int flags = 0;
            const auto row = list_->HitTest(list_->ScreenToClient(event.GetPosition()), flags);
            const auto previous = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (previous >= 0) list_->SetItemState(previous, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            if (row >= 0) list_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        }
        const auto* entry = Selected();
        const auto directory = entry && entry->directory ? (entry->name == ".." ? RemoteParent(directory_) : JoinRemotePath(directory_, entry->name)) : directory_;
        wxMenu menu;
        menu.Append(wxID_OPEN, wxS("上传到当前目录…"));
        menu.Append(wxID_SAVEAS, wxS("下载所选文件…"));
        menu.AppendSeparator();
        menu.Append(wxID_FORWARD, entry && entry->directory ? wxS("cd 到所选目录") : wxS("cd 到当前目录"));
        menu.Enable(wxID_OPEN, connected_ && watching_.empty());
        menu.Enable(wxID_SAVEAS, connected_ && watching_.empty() && entry && entry->regular);
        menu.Enable(wxID_FORWARD, connected_ && directory.starts_with('/'));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { Upload(); }, wxID_OPEN);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { Download(); }, wxID_SAVEAS);
        menu.Bind(wxEVT_MENU, [this, directory](wxCommandEvent&) { EnterDirectory(directory); }, wxID_FORWARD);
        list_->PopupMenu(&menu);
    } catch (const std::exception& error) { Error(error); }
}
void FileBrowserPanel::EnterDirectory(std::string directory) {
    if (!connected_) return;
    try {
        const auto command = TerminalCdCommand(directory);
        ActiveFiles();
        if (!sender_(command)) throw std::runtime_error("SSH 已断开，目录命令未发送");
        status_->SetLabel(wxS("cd 已发送，请以终端输出为准"));
    } catch (const std::exception& error) { Error(error); }
}
void FileBrowserPanel::ShowFiles(const FileSnapshot& snapshot) {
    directory_ = snapshot.remote_path; entries_ = snapshot.entries; path_->ChangeValue(wxString::FromUTF8(directory_));
    if (directory_ != "/") entries_.insert(entries_.begin(), RemoteFile{"..", true});
    list_->Freeze(); list_->DeleteAllItems();
    for (const auto& entry : entries_) {
        const auto row = list_->InsertItem(list_->GetItemCount(), wxString::FromUTF8(entry.name));
        list_->SetItem(row, 1, entry.directory ? wxS("目录") : entry.symlink ? wxS("链接") : entry.regular ? wxS("文件") : wxS("其他"));
        if (entry.regular) list_->SetItem(row, 2, wxString::Format("%llu", static_cast<unsigned long long>(entry.size)));
    }
    list_->Thaw();
    status_->SetLabel(wxString::Format(wxS("%zu 项%s"), snapshot.entries.size(), snapshot.truncated ? wxS("（列表已截断或存在不支持的文件名）") : wxS("")));
}
void FileBrowserPanel::Poll(bool connected) {
    connected_ = connected;
    try {
        if (connected && IsShownOnScreen()) {
            auto current = files_();
            if (current != client_) { client_ = std::move(current); watching_.clear(); directory_ = "."; entries_.clear(); list_->DeleteAllItems(); Browse("."); }
        }
        if (client_ && !watching_.empty()) {
            const auto snapshot = client_->Snapshot(watching_);
            if (snapshot.state != "running") {
                watching_.clear(); progress_->SetValue(snapshot.state == "succeeded" ? 100 : 0);
                if (snapshot.state == "succeeded" && snapshot.operation == FileOperation::List) ShowFiles(snapshot);
                else if (snapshot.state == "succeeded") { status_->SetLabel(wxS("传输完成")); if (connected && snapshot.operation == FileOperation::Upload) Browse(directory_); }
                else { status_->SetLabel(wxString::FromUTF8(snapshot.state == "cancelled" ? "文件任务已取消" : snapshot.error)); status_->SetToolTip(status_->GetLabel()); }
            } else {
                if (snapshot.total) progress_->SetValue(static_cast<int>(std::min(100.0, snapshot.bytes * 100.0 / snapshot.total)));
                status_->SetLabel(wxString::Format(wxS("处理中：%llu / %llu 字节"), static_cast<unsigned long long>(snapshot.bytes), static_cast<unsigned long long>(snapshot.total)));
            }
        }
    } catch (const std::exception& error) { Error(error); }
    const bool busy = !watching_.empty();
    for (auto* action : actions_) action->Enable(connected && !busy);
    cancel_->Enable(busy); if (!connected && !busy) status_->SetLabel(wxS("SSH 未连接"));
}
}
