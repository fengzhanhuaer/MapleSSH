#include "main_frame.hpp"
#include "dialogs.hpp"
#include "backup_labels.hpp"
#include "command_input.hpp"
#include <wx/artprov.h>
#include <wx/choicdlg.h>
#include <wx/splitter.h>
#include <wx/aboutdlg.h>
#include <wx/textdlg.h>
#include <algorithm>
#include <map>

namespace maple {
namespace {
enum { IdNew = wxID_HIGHEST + 1, IdEdit, IdDelete, IdConnect, IdDisconnect, IdReconnect, IdSettings, IdBackup, IdRestore, IdCancel, IdLicenses, IdCredentials, IdRenameGroup };
class SessionItem final : public wxTreeItemData {
public:
    explicit SessionItem(std::string value) : id(std::move(value)) {}
    std::string id;
};
class GroupItem final : public wxTreeItemData {
public:
    explicit GroupItem(std::string value) : name(std::move(value)) {}
    std::string name;
};
}
MainFrame::MainFrame(Storage storage, Vault vault)
    : wxFrame(nullptr, wxID_ANY, "MapleSSH", wxDefaultPosition, wxSize(1180, 760)),
      storage_(std::move(storage)), vault_(std::move(vault)),
      keys_(storage_.Directory() / "known_hosts"), timer_(this) {
    SetMinSize(wxSize(850, 540));
    auto* menus = new wxMenuBar;
    auto* sessions = new wxMenu;
    sessions->Append(IdNew, wxS("新增会话\tCtrl+N")); sessions->Append(IdEdit, wxS("编辑会话")); sessions->Append(IdDelete, wxS("删除会话"));
    sessions->AppendSeparator(); sessions->Append(IdConnect, wxS("连接\tCtrl+Enter")); sessions->Append(IdDisconnect, wxS("断开当前连接")); sessions->Append(IdReconnect, wxS("重新连接"));
    sessions->AppendSeparator(); sessions->Append(IdCredentials, wxS("凭据管理…"));
    sessions->AppendSeparator(); sessions->Append(wxID_EXIT, wxS("退出"));
    auto* backup = new wxMenu;
    backup->Append(IdSettings, wxS("WebDAV 设置…")); backup->AppendSeparator();
    backup->Append(IdBackup, wxS("手动备份…")); backup->Append(IdRestore, wxS("手动恢复…")); backup->Append(IdCancel, wxS("取消当前操作"));
    auto* help = new wxMenu; help->Append(wxID_ABOUT, wxS("关于 MapleSSH")); help->Append(IdLicenses, wxS("第三方许可证…"));
    menus->Append(sessions, wxS("会话")); menus->Append(backup, wxS("备份")); menus->Append(help, wxS("帮助")); SetMenuBar(menus);
    auto* toolbar = CreateToolBar(wxTB_FLAT | wxTB_TEXT);
    const wxSize iconSize(16, 16);
    toolbar->SetToolBitmapSize(toolbar->FromDIP(iconSize));
    toolbar->AddTool(IdNew, wxS("新增"), wxArtProvider::GetBitmapBundle(wxART_NEW, wxART_TOOLBAR, iconSize));
    toolbar->AddTool(IdEdit, wxS("编辑"), wxArtProvider::GetBitmapBundle(wxART_EDIT, wxART_TOOLBAR, iconSize));
    toolbar->AddTool(IdConnect, wxS("连接"), wxArtProvider::GetBitmapBundle(wxART_GO_FORWARD, wxART_TOOLBAR, iconSize));
    toolbar->AddTool(IdDisconnect, wxS("断开"), wxArtProvider::GetBitmapBundle(wxART_CROSS_MARK, wxART_TOOLBAR, iconSize));
    toolbar->AddTool(IdCredentials, wxS("凭据"), wxArtProvider::GetBitmapBundle(wxART_LIST_VIEW, wxART_TOOLBAR, iconSize));
    toolbar->AddSeparator();
    toolbar->AddTool(IdBackup, wxS("备份"), wxArtProvider::GetBitmapBundle(wxART_FILE_SAVE, wxART_TOOLBAR, iconSize));
    toolbar->AddTool(IdRestore, wxS("恢复"), wxArtProvider::GetBitmapBundle(wxART_UNDO, wxART_TOOLBAR, iconSize));
    toolbar->AddTool(IdSettings, wxS("设置"), wxArtProvider::GetBitmapBundle(wxART_EXECUTABLE_FILE, wxART_TOOLBAR, iconSize));
    toolbar->Realize();
    auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
    tree_ = new wxTreeCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_HIDE_ROOT | wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxBORDER_NONE);
    notebook_ = new wxAuiNotebook(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_DEFAULT_STYLE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB | wxAUI_NB_TAB_MOVE);
    auto* welcome = new wxPanel(notebook_);
    auto* welcome_layout = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(welcome, wxID_ANY, "MapleSSH"); title->SetFont(wxFont(wxFontInfo(28).Bold()));
    welcome_layout->AddStretchSpacer(); welcome_layout->Add(title, 0, wxALIGN_CENTER | wxBOTTOM, 20);
    auto* description = new wxStaticText(welcome, wxID_ANY,
        wxS("Windows 原生 SSH 工作台\n\n新增会话，然后双击连接。\nCtrl+Shift+C / V 复制粘贴，Ctrl+滚轮调整终端字体。\n\n配置在本机保存，无需主密码；WebDAV 只在手动操作时备份。\n主机指纹仅保存在本机，不进入备份。"));
    description->SetFont(wxFont(wxFontInfo(11))); welcome_layout->Add(description, 0, wxALIGN_CENTER);
    welcome_layout->AddStretchSpacer(); welcome->SetSizer(welcome_layout); notebook_->AddPage(welcome, wxS("开始"), true);
    splitter->SetMinimumPaneSize(180); splitter->SplitVertically(tree_, notebook_, 245);
    auto* layout = new wxBoxSizer(wxVERTICAL); layout->Add(splitter, 1, wxEXPAND); SetSizer(layout);
    CreateStatusBar(2); const int widths[]{-1, 220}; SetStatusWidths(2, widths);
    SetStatusText(wxS("就绪 · 配置保存在程序同目录"), 0); SetStatusText(wxS("手动 WebDAV 备份"), 1);
    BuildTree();
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, L"THIRD_PARTY_NOTICES", RT_RCDATA);
        const auto loaded = resource ? LoadResource(module, resource) : nullptr;
        const auto* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
        if (!bytes) { wxMessageBox(wxS("无法读取内嵌许可证"), "MapleSSH", wxOK | wxICON_ERROR, this); return; }
        wxDialog dialog(this, wxID_ANY, wxS("第三方许可证"), wxDefaultPosition, wxSize(850, 650), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* text = new wxTextCtrl(&dialog, wxID_ANY, wxString::FromUTF8(bytes, SizeofResource(module, resource)), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
        layout->Add(text, 1, wxEXPAND | wxALL, 10); layout->Add(dialog.CreateButtonSizer(wxOK), 0, wxEXPAND | wxALL, 10);
        dialog.SetSizer(layout); dialog.ShowModal();
    }, IdLicenses);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Edit(true); }, IdNew);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Edit(false); }, IdEdit);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Delete(); }, IdDelete);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ConnectSelected(); }, IdConnect);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (auto* tab = ActiveTab()) tab->client->Stop(); }, IdDisconnect);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (job_) return;
        if (auto* tab = ActiveTab()) {
            try {
                const auto current = std::find_if(vault_.sessions.begin(), vault_.sessions.end(), [&](const auto& session) { return session.id == tab->session.id; });
                ConnectTab(*tab, current == vault_.sessions.end() ? tab->session : *current);
            } catch (const std::exception& error) { ShowError(error); }
        }
    }, IdReconnect);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ManageCredentials(); }, IdCredentials);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { RenameSelectedGroup(); }, IdRenameGroup);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Settings(); }, IdSettings);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Backup(); }, IdBackup);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Restore(); }, IdRestore);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (job_ && job_->client) job_->client->Cancel(); }, IdCancel);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { wxFrame::Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxMessageBox(wxS("MapleSSH 0.1.0\nwxWidgets · libssh2 · libvterm · OpenSSL\n\n单文件绿色 SSH 客户端\n第三方许可证已内嵌在程序资源 THIRD_PARTY_NOTICES 中。"), wxS("关于 MapleSSH"), wxOK | wxICON_INFORMATION, this);
    }, wxID_ABOUT);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
        if (event.GetId() == IdCancel) event.Enable(job_ && job_->client);
        else if ((event.GetId() >= IdNew && event.GetId() <= IdRestore) || event.GetId() == IdCredentials || event.GetId() == IdRenameGroup) event.Enable(!job_);
    });
    tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent&) { ConnectSelected(); });
    tree_->Bind(wxEVT_TREE_ITEM_MENU, &MainFrame::GroupMenu, this);
    notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& event) {
        auto* page = notebook_->GetPage(event.GetSelection());
        const auto found = std::find_if(tabs_.begin(), tabs_.end(), [page](const auto& tab) { return tab->page == page; });
        if (found == tabs_.end()) { event.Veto(); return; }
        (*found)->terminal->SetSender({}); (*found)->terminal->SetResize({}); (*found)->client->Stop();
        tabs_.erase(found); event.Skip();
    });
    Bind(wxEVT_TIMER, &MainFrame::Poll, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::Close, this);
    timer_.Start(30); Centre();
}
MainFrame::~MainFrame() {
    timer_.Stop();
    if (job_ && job_->client) job_->client->Cancel();
    if (background_.joinable()) background_.join();
    for (auto& tab : tabs_) { tab->terminal->SetSender({}); tab->terminal->SetResize({}); tab->client->Stop(); }
}
void MainFrame::ShowError(const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), "MapleSSH", wxOK | wxICON_ERROR, this); }
void MainFrame::BuildTree() {
    const auto selected = SelectedId();
    tree_->DeleteAllItems(); const auto root = tree_->AddRoot(wxS("会话"));
    std::map<std::string, wxTreeItemId> groups;
    for (const auto& session : vault_.sessions) {
        const auto& group = session.group;
        if (!groups.contains(group)) groups[group] = tree_->AppendItem(root, group.empty() ? wxString(wxS("未分组")) : wxString::FromUTF8(group), -1, -1, new GroupItem(group));
        const auto item = tree_->AppendItem(groups[group], wxString::FromUTF8(session.name), -1, -1, new SessionItem(session.id));
        if (session.id == selected) tree_->SelectItem(item);
    }
    tree_->ExpandAll();
}
std::string MainFrame::SelectedId() const {
    const auto item = tree_->GetSelection(); if (!item.IsOk()) return {};
    const auto* data = dynamic_cast<SessionItem*>(tree_->GetItemData(item)); return data ? data->id : std::string{};
}
void MainFrame::Edit(bool create) {
    if (job_) return;
    Session initial;
    if (create) {
        const auto selected = tree_->GetSelection();
        if (selected.IsOk()) if (const auto* group = dynamic_cast<GroupItem*>(tree_->GetItemData(selected))) initial.group = group->name;
    }
    if (!create) {
        const auto id = SelectedId(); const auto found = std::find_if(vault_.sessions.begin(), vault_.sessions.end(), [&id](const auto& session) { return session.id == id; });
        if (found == vault_.sessions.end()) return; initial = *found;
    }
    SessionDialog dialog(this, initial, vault_); if (dialog.ShowModal() != wxID_OK) return;
    auto updated = dialog.ResultVault(); const auto session = dialog.Result();
    if (create) updated.sessions.push_back(session);
    else for (auto& entry : updated.sessions) if (entry.id == session.id) entry = session;
    Save(std::move(updated));
}
void MainFrame::GroupMenu(wxTreeEvent& event) {
    if (job_ || !event.GetItem().IsOk()) return;
    const auto* group = dynamic_cast<GroupItem*>(tree_->GetItemData(event.GetItem())); if (!group) return;
    tree_->SelectItem(event.GetItem());
    wxMenu menu; menu.Append(IdRenameGroup, wxS("重命名分组…")); tree_->PopupMenu(&menu);
}
void MainFrame::RenameSelectedGroup() {
    if (job_) return;
    const auto selected = tree_->GetSelection(); if (!selected.IsOk()) return;
    const auto* group = dynamic_cast<GroupItem*>(tree_->GetItemData(selected)); if (!group) return;
    const auto previous = group->name;
    wxTextEntryDialog dialog(this, wxS("输入新的分组名称，该组中的主机会一起更新。"), wxS("重命名分组"), wxString::FromUTF8(previous));
    if (dialog.ShowModal() != wxID_OK) return;
    try {
        auto name = dialog.GetValue(); name.Trim(true).Trim(false);
        if (name.ToStdString(wxConvUTF8) == previous) return;
        Save(RenameGroup(vault_, previous, name.ToStdString(wxConvUTF8)));
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::ManageCredentials() {
    if (job_) return; CredentialManagerDialog dialog(this, vault_);
    if (dialog.ShowModal() == wxID_OK) Save(dialog.Result());
}
void MainFrame::Delete() {
    if (job_) return;
    const auto id = SelectedId(); if (id.empty()) return;
    if (wxMessageBox(wxS("删除所选会话？不会影响已经打开的连接。"), wxS("删除会话"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
    auto updated = vault_; std::erase_if(updated.sessions, [&id](const auto& session) { return session.id == id; }); Save(std::move(updated));
}
MainFrame::Tab* MainFrame::ActiveTab() {
    const auto selected = notebook_->GetSelection(); if (selected == wxNOT_FOUND) return nullptr;
    auto* page = notebook_->GetPage(selected);
    for (auto& tab : tabs_) if (tab->page == page) return tab.get();
    return nullptr;
}
void MainFrame::ConnectSelected() {
    if (job_) return;
    try {
        const auto id = SelectedId();
        const auto found = std::find_if(vault_.sessions.begin(), vault_.sessions.end(), [&id](const auto& session) { return session.id == id; });
        if (found == vault_.sessions.end()) return;
        if (tabs_.size() >= 16) throw std::runtime_error("首版最多同时打开 16 个终端标签");
        FindCredential(vault_, found->credential_id);
        auto tab = std::make_unique<Tab>();
        auto page = std::make_unique<wxPanel>(notebook_); tab->page = page.get();
        tab->terminal = new TerminalPanel(tab->page);
        BuildCommandInput(*tab); ConnectTab(*tab, *found);
        notebook_->AddPage(tab->page, wxString::FromUTF8(found->name), true);
        page.release(); tabs_.push_back(std::move(tab)); RefreshQuickCommands(); tabs_.back()->terminal->SetFocus();
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::ConnectTab(Tab& tab, const Session& session) {
    auto client = std::make_unique<SshClient>(session, FindCredential(vault_, session.credential_id));
    if (tab.client) tab.client->Stop();
    tab.session = session; tab.client = std::move(client);
    auto* connection = tab.client.get();
    tab.terminal->SetSender([this, connection](std::string text) { try { connection->Send(std::move(text)); } catch (const std::exception& error) { SetStatusText(wxString::FromUTF8(error.what())); } });
    tab.terminal->SetResize([connection](int rows, int columns) { connection->Resize(rows, columns); });
    tab.client->Start(tab.terminal->Rows(), tab.terminal->Columns()); tab.terminal->SetFocus();
}
void MainFrame::BuildCommandInput(Tab& tab) {
    auto* target = &tab;
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(tab.terminal, 1, wxEXPAND);
    auto* shortcut_row = new wxBoxSizer(wxHORIZONTAL);
    shortcut_row->Add(new wxStaticText(tab.page, wxID_ANY, wxS("快捷命令")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    tab.shortcuts = new wxScrolledWindow(tab.page, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 42)), wxHSCROLL | wxBORDER_NONE);
    tab.shortcuts->SetScrollRate(FromDIP(8), 0); tab.shortcuts->SetSizer(new wxBoxSizer(wxHORIZONTAL));
    shortcut_row->Add(tab.shortcuts, 1, wxEXPAND);
    auto* add = new wxButton(tab.page, wxID_ANY, wxS("添加…"));
    add->Bind(wxEVT_BUTTON, [this, target](wxCommandEvent&) { EditQuickCommand({}, target->command->GetValue().ToStdString(wxConvUTF8)); });
    add->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Enable(!job_); });
    shortcut_row->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    layout->Add(shortcut_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    layout->Add(new wxStaticText(tab.page, wxID_ANY, wxS("命令输入：Enter 换行，Ctrl+Enter 执行；仅发送到本会话")), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    auto* input_row = new wxBoxSizer(wxHORIZONTAL);
    tab.command = new wxTextCtrl(tab.page, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 80)), wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_RICH2);
    tab.command->SetHint(wxS("输入或粘贴命令，点击执行后发送"));
    tab.command->SetFont(wxFont(wxFontInfo(10).FaceName("Cascadia Mono").Family(wxFONTFAMILY_TELETYPE)));
    tab.command->Bind(wxEVT_CHAR_HOOK, [this, target](wxKeyEvent& event) {
        if (event.ControlDown() && !event.AltDown() && !event.ShiftDown() && (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER)) {
            ExecuteCommands(*target, target->command->GetValue().ToStdString(wxConvUTF8), true);
            return;
        }
        event.Skip();
    });
    input_row->Add(tab.command, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    auto* actions = new wxBoxSizer(wxVERTICAL);
    auto* execute = new wxButton(tab.page, wxID_ANY, wxS("执行"));
    execute->Bind(wxEVT_BUTTON, [this, target](wxCommandEvent&) { ExecuteCommands(*target, target->command->GetValue().ToStdString(wxConvUTF8), true); });
    execute->Bind(wxEVT_UPDATE_UI, [target](wxUpdateUIEvent& event) { event.Enable(target->client && target->client->Connected() && target->command->GetLastPosition() > 0); });
    actions->Add(execute, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* clear = new wxButton(tab.page, wxID_ANY, wxS("清空"));
    clear->Bind(wxEVT_BUTTON, [target](wxCommandEvent&) { target->command->Clear(); target->command->SetFocus(); });
    actions->Add(clear, 0, wxEXPAND); input_row->Add(actions, 0, wxEXPAND);
    layout->Add(input_row, 0, wxEXPAND | wxALL, FromDIP(8)); tab.page->SetSizer(layout);
}
void MainFrame::RefreshQuickCommands() {
    for (auto& tab : tabs_) {
        auto* target = tab.get(); auto* layout = tab->shortcuts->GetSizer();
        layout->Clear(true);
        for (const auto& command : vault_.quick_commands) {
            auto* button = new wxButton(tab->shortcuts, wxID_ANY, wxString::FromUTF8(command.name), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            button->SetToolTip(wxString::FromUTF8(command.command) + wxS("\n点击直接发送；右键编辑或删除"));
            button->Bind(wxEVT_BUTTON, [this, target, id = command.id](wxCommandEvent&) {
                const auto found = std::find_if(vault_.quick_commands.begin(), vault_.quick_commands.end(), [&](const auto& entry) { return entry.id == id; });
                if (found != vault_.quick_commands.end()) ExecuteCommands(*target, found->command, false);
            });
            button->Bind(wxEVT_CONTEXT_MENU, [this, button, id = command.id](wxContextMenuEvent&) {
                wxMenu menu; menu.Append(wxID_EDIT, wxS("编辑快捷命令…")); menu.Append(wxID_DELETE, wxS("删除快捷命令…"));
                menu.Enable(wxID_EDIT, !job_); menu.Enable(wxID_DELETE, !job_);
                menu.Bind(wxEVT_MENU, [this, id](wxCommandEvent&) { EditQuickCommand(id); }, wxID_EDIT);
                menu.Bind(wxEVT_MENU, [this, id](wxCommandEvent&) { DeleteQuickCommand(id); }, wxID_DELETE);
                button->PopupMenu(&menu);
            });
            layout->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        }
        tab->shortcuts->FitInside(); tab->shortcuts->Layout(); tab->page->Layout();
    }
}
void MainFrame::EditQuickCommand(const std::string& id, const std::string& initial) {
    if (job_) return;
    QuickCommand command;
    if (id.empty()) command.command = initial;
    else {
        const auto found = std::find_if(vault_.quick_commands.begin(), vault_.quick_commands.end(), [&](const auto& entry) { return entry.id == id; });
        if (found == vault_.quick_commands.end()) return; command = *found;
    }
    QuickCommandDialog dialog(this, command); if (dialog.ShowModal() != wxID_OK) return;
    auto updated = vault_; command = dialog.Result();
    if (id.empty()) updated.quick_commands.push_back(command);
    else for (auto& entry : updated.quick_commands) if (entry.id == id) entry = command;
    try { ValidateCatalog(updated); Save(std::move(updated)); } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::DeleteQuickCommand(const std::string& id) {
    if (job_) return;
    const auto found = std::find_if(vault_.quick_commands.begin(), vault_.quick_commands.end(), [&](const auto& command) { return command.id == id; });
    if (found == vault_.quick_commands.end()) return;
    if (wxMessageBox(wxS("删除快捷命令“") + wxString::FromUTF8(found->name) + wxS("”？"), wxS("删除快捷命令"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
    auto updated = vault_; std::erase_if(updated.quick_commands, [&](const auto& command) { return command.id == id; });
    Save(std::move(updated));
}
void MainFrame::ExecuteCommands(Tab& tab, std::string_view text, bool confirm_multiline) {
    try {
        if (!tab.client || !tab.client->Connected()) throw std::runtime_error("当前会话尚未连接，命令未发送");
        const auto commands = PrepareCommands(text);
        if (confirm_multiline && commands.lines > 1) {
            const auto message = wxString::Format(wxS("将向 %s（%s:%u）发送 %zu 行命令。\n命令可能立即执行，确认继续？"),
                wxString::FromUTF8(tab.session.name), wxString::FromUTF8(tab.session.host), static_cast<unsigned int>(tab.session.port), commands.lines);
            if (wxMessageBox(message, wxS("执行多行命令"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
        }
        if (!tab.client->Send(commands.input)) throw std::runtime_error("当前会话已断开，命令未发送");
        SetStatusText(wxString::FromUTF8(tab.session.name) + wxS("：命令已发送")); tab.terminal->SetFocus();
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::Settings() {
    if (job_) return; WebDavDialog dialog(this, vault_.webdav); if (dialog.ShowModal() != wxID_OK) return;
    auto updated = vault_; updated.webdav = dialog.Result(); Save(std::move(updated));
}
void MainFrame::Launch(const std::shared_ptr<Job>& job, std::function<void(Job&)> work) {
    if (job_) return;
    if (background_.joinable()) background_.join();
    job_ = job; SetStatusText(wxS("正在处理，可在备份菜单取消网络操作…"));
    background_ = std::jthread([job, work = std::move(work)] {
        try { work(*job); } catch (const std::exception& error) { job->error = error.what(); }
        job->done.store(true, std::memory_order_release);
    });
}
void MainFrame::Save(Vault updated, bool recovery_copy) {
    auto job = std::make_shared<Job>(); job->kind = JobKind::Save; job->vault = std::move(updated);
    Launch(job, [this, recovery_copy](Job& state) { if (recovery_copy) storage_.SaveRecoveryCopy(); storage_.Save(state.vault); });
}
void MainFrame::Backup() {
    if (job_) return;
    try {
        auto job = std::make_shared<Job>(); job->kind = JobKind::Backup;
        job->client = std::make_shared<WebDavClient>(vault_.webdav); job->name = WebDavClient::NewBackupName();
        if (wxMessageBox(wxS("将会话、SSH 密码、私钥和快捷命令上传到所设置的 WebDAV。\n不包含本机指纹或 WebDAV 登录资料。确认备份？"), wxS("手动备份"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        auto catalog = vault_; catalog.webdav = {};
        Launch(job, [catalog = std::move(catalog)](Job& state) { const auto content = SerializeBackup(catalog); state.client->Upload(state.name, Bytes(content.begin(), content.end())); });
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::Restore() {
    if (job_) return;
    try {
        auto job = std::make_shared<Job>(); job->kind = JobKind::List; job->client = std::make_shared<WebDavClient>(vault_.webdav);
        Launch(job, [](Job& state) { state.names = state.client->List(); });
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::StartDownload(const std::string& name) {
    try {
        auto job = std::make_shared<Job>(); job->kind = JobKind::Download; job->name = name;
        job->client = std::make_shared<WebDavClient>(vault_.webdav);
        Launch(job, [](Job& state) { const auto content = state.client->Download(state.name); state.restored = ParseBackup(std::string(content.begin(), content.end())); });
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::CompleteJob() {
    if (!job_ || !job_->done.load(std::memory_order_acquire)) return;
    if (background_.joinable()) background_.join();
    auto completed = std::move(job_); SetStatusText(wxS("就绪"));
    if (!completed->error.empty()) { std::runtime_error error(completed->error); ShowError(error); return; }
    switch (completed->kind) {
        case JobKind::Save: vault_ = std::move(completed->vault); BuildTree(); RefreshQuickCommands(); SetStatusText(wxS("配置已保存")); break;
        case JobKind::Backup: wxMessageBox(wxS("备份已上传并通过回读校验：\n") + wxString::FromUTF8(completed->name), wxS("备份成功"), wxOK | wxICON_INFORMATION, this); break;
        case JobKind::List: {
            if (completed->names.empty()) { wxMessageBox(wxS("该目录没有 MapleSSH 备份"), wxS("手动恢复"), wxOK | wxICON_INFORMATION, this); break; }
            wxArrayString choices; for (const auto& label : BackupLabels(completed->names)) choices.Add(wxString::FromUTF8(label));
            wxSingleChoiceDialog choice(this, wxS("选择要恢复的备份（本机时间，最新在前）"), wxS("手动恢复"), choices);
            choice.SetSelection(0);
            if (choice.ShowModal() == wxID_OK) StartDownload(completed->names.at(choice.GetSelection()));
            break;
        }
        case JobKind::Download: {
            const auto message = wxString::Format(wxS("备份含 %zu 条会话、%zu 条凭据、%zu 条快捷命令。\n将替换当前会话、凭据和快捷命令列表，并先保留本机副本。\n本机指纹、WebDAV 设置和已打开连接保持不变。继续？"), completed->restored.sessions.size(), completed->restored.credentials.size(), completed->restored.quick_commands.size());
            if (wxMessageBox(message, wxS("确认恢复"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) break;
            auto updated = vault_; updated.sessions = std::move(completed->restored.sessions); updated.credentials = std::move(completed->restored.credentials); updated.quick_commands = std::move(completed->restored.quick_commands); Save(std::move(updated), true); break;
        }
    }
}
void MainFrame::Poll(wxTimerEvent&) {
    if (polling_) return;
    polling_ = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{polling_};
    for (auto& tab : tabs_) {
        for (const auto& event : tab->client->Drain()) {
            if (event.kind == SshEventKind::Output) tab->terminal->Feed(event.text);
            else if (event.kind == SshEventKind::State) {
                const auto page = notebook_->GetPageIndex(tab->page);
                if (page != wxNOT_FOUND) notebook_->SetPageText(page, wxString::FromUTF8(tab->session.name + " · " + event.text));
                tab->terminal->FeedStatus(event.text);
                SetStatusText(wxString::FromUTF8(tab->session.name + "：" + event.text));
            } else {
                bool accepted = false;
                try {
                    const auto trust = keys_.Check(tab->session.host, tab->session.port, event.algorithm, event.key);
                    if (trust == Trust::Match) accepted = true;
                    else if (trust == Trust::Mismatch) wxMessageBox(wxS("服务器主机密钥与本机记录不一致，已阻止认证。\n请通过可信通道核实服务器身份，不要直接忽略。"), wxS("主机密钥变化"), wxOK | wxICON_ERROR, this);
                    else {
                        const auto prompt = wxString::FromUTF8(tab->session.host + ":" + std::to_string(tab->session.port) + "\n" + event.algorithm + "\n" + event.text + "\n\n请通过可信通道核实以上指纹。是否信任并仅在本机保存？");
                        accepted = wxMessageBox(prompt, wxS("首次连接：确认服务器身份"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) == wxYES;
                        if (accepted) keys_.Accept(tab->session.host, tab->session.port, event.algorithm, event.key);
                    }
                } catch (const std::exception& error) { accepted = false; ShowError(error); }
                tab->client->ConfirmHost(accepted);
            }
        }
    }
    CompleteJob();
}
void MainFrame::Close(wxCloseEvent& event) {
    if (event.CanVeto() && (!tabs_.empty() || job_) && wxMessageBox(wxS("退出将断开所有 SSH 连接并取消进行中的网络操作。确认退出？"), wxS("退出 MapleSSH"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) { event.Veto(); return; }
    timer_.Stop(); if (job_ && job_->client) job_->client->Cancel(); event.Skip();
}
}
